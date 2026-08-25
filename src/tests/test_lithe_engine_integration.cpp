// =============================================================================
// test_lithe_engine_integration.cpp — managed binding closes the invoke gap
// (§8.0)
//
// Verifies:
//   • engine_integration.hpp binds a managed code version to an execution
//   lease. • managed_entry_adapter closes managed_function::invoke() for
//   interpreter entry. • bind_managed_entry returns an error for invalid
//   inputs. • code_version_metadata ownership migrates to rt::code_manager at
//   this boundary. • managed_integration_context::invoke works end-to-end for
//   interpreter. • managed_function::invoke executes a bound typed entry
//   through the managed ABI.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <ranges>
#include <thread>
#include <type_traits>
#include <vector>

#include "lithe/backends/lithe_codegen_backend_registry.hpp"
#include "lithe/backends/lithe_codegen_interpreter_facet.hpp"
#include "lithe/backends/lithe_execution_backends.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/lithe_engine.hpp"
#include "lithe/lithe_local_engine.hpp"
#include "lithe/lithe_rt.hpp" // full managed runtime (runtime_instance, compile, etc.)
#include "lithe/lithe_rt/engine_integration.hpp"

namespace ex = lithe::execution;
namespace cg = lithe::codegen;
namespace mir = lithe::codegen::mir;

// ============================================================================
// Helper: build a small add MIR function
// ============================================================================

namespace {
mir::physical_mir_function make_add_fn_rt() {
  cg::allocated_function_ir fn;
  fn.name = "add_integration_test";
  fn.cfg.entry_block = 1;

  cg::allocated_basic_block bb;
  bb.id = 1;
  bb.name = "entry";

  cg::allocated_instruction load0;
  load0.id = 1;
  load0.op = cg::opcode::load_arg;
  load0.defs = {cg::allocated_operand::as_preg({0, "r0"})};
  load0.uses = {cg::allocated_operand::as_argument_index(0)};
  bb.instructions.push_back(load0);

  cg::allocated_instruction load1;
  load1.id = 2;
  load1.op = cg::opcode::load_arg;
  load1.defs = {cg::allocated_operand::as_preg({1, "r1"})};
  load1.uses = {cg::allocated_operand::as_argument_index(1)};
  bb.instructions.push_back(load1);

  cg::allocated_instruction add;
  add.id = 3;
  add.op = cg::opcode::add;
  add.defs = {cg::allocated_operand::as_preg({2, "r2"})};
  add.uses = {cg::allocated_operand::as_preg({0, "r0"}),
              cg::allocated_operand::as_preg({1, "r1"})};
  bb.instructions.push_back(add);

  cg::allocated_instruction ret;
  ret.id = 4;
  ret.op = cg::opcode::ret;
  ret.uses = {cg::allocated_operand::as_preg({2, "r2"})};
  bb.instructions.push_back(ret);

  fn.blocks.push_back(std::move(bb));

  mir::physical_mir_function phys;
  phys.function = std::move(fn);
  phys.metadata.current_phase = mir::phase::physical_mir;
  return phys;
}

cg::hl::hl_mir_function make_hl_add_fn_rt() {
  cg::hl::hl_mir_function fn{1u << 18};
  fn.name = "hl_add_integration_test";
  auto *region = fn.make_region();
  auto *block = fn.make_block();
  block->parent_region = region;
  region->blocks.push_back(block);
  fn.body_region = *region;

  auto add_argument = [&](const std::uint64_t id) {
    auto *operation = fn.make_op(cg::hl::hl_opcode::argument);
    auto results = fn.alloc_span<cg::ssa_value_id>(1);
    results[0] = cg::ssa_value_id{id};
    operation->results = results;
    block->ops.push_back(operation);
  };
  add_argument(1);
  add_argument(2);

  auto *add = fn.make_op(cg::hl::hl_opcode::add);
  auto operands = fn.alloc_span<cg::ssa_value_id>(2);
  operands[0] = cg::ssa_value_id{1};
  operands[1] = cg::ssa_value_id{2};
  add->operands = operands;
  auto add_result = fn.alloc_span<cg::ssa_value_id>(1);
  add_result[0] = cg::ssa_value_id{3};
  add->results = add_result;
  block->ops.push_back(add);

  auto *yield = fn.make_op(cg::hl::hl_opcode::region_yield);
  auto yield_operands = fn.alloc_span<cg::ssa_value_id>(1);
  yield_operands[0] = cg::ssa_value_id{3};
  yield->operands = yield_operands;
  block->ops.push_back(yield);

  fn.body_region.blocks.head = nullptr;
  fn.body_region.blocks.tail = nullptr;
  fn.body_region.blocks.size_ = 0;
  fn.body_region.blocks.push_back(block);
  return fn;
}
} // anonymous namespace

// ============================================================================
// bind_managed_entry: error on invalid entry
// ============================================================================

TEST_CASE(

    "bind_managed_entry: invalid typed_entry → trap error",
    "[engine_integration][error]") {
  using sig_t = std::int64_t(std::int64_t, std::int64_t);

  // Empty typed_entry is invalid.
  lithe::execution::typed_entry<sig_t> empty_entry;
  auto code = std::make_shared<lithe::rt::code_resource>();

  auto result =
      lithe::rt::bind_managed_entry<sig_t>(std::move(empty_entry), code, 1);

  REQUIRE_FALSE(result.has_value());
  // Error message should mention the problem.
  CHECK_FALSE(result.error().detail.empty());
}

TEST_CASE(

    "bind_managed_entry: null code_resource → trap error",
    "[engine_integration][error]") {
  using sig_t = std::int64_t(std::int64_t, std::int64_t);

  // Build a valid typed_entry first.
  cg::backends::interpreter_backend backend;
  auto fn = make_add_fn_rt();

  auto art = ex::cpo::compile(backend, std::move(fn));
  REQUIRE(art.has_value());
  auto res = ex::cpo::install(backend, std::move(*art));
  REQUIRE(res.has_value());
  auto entry = ex::cpo::get_entry(backend, *res, ex::type_tag<sig_t>{});
  REQUIRE(entry.has_value());

  // Pass a null code_resource → error.
  auto result =
      lithe::rt::bind_managed_entry<sig_t>(std::move(*entry), nullptr, 1);

  REQUIRE_FALSE(result.has_value());
}

// ============================================================================
// managed_entry_adapter: interpreter invoke gap closed
// ============================================================================

TEST_CASE(

    "managed_entry_adapter: closes invoke gap for interpreter entry",
    "[engine_integration][invoke]") {
  using sig_t = std::int64_t(std::int64_t, std::int64_t);

  cg::backends::interpreter_backend backend;
  auto fn = make_add_fn_rt();

  auto art = ex::cpo::compile(backend, std::move(fn));
  REQUIRE(art.has_value());
  auto res = ex::cpo::install(backend, std::move(*art));
  REQUIRE(res.has_value());
  auto entry_result = ex::cpo::get_entry(backend, *res, ex::type_tag<sig_t>{});
  REQUIRE(entry_result.has_value());

  // Build a code_resource with version counter.
  auto code = std::make_shared<lithe::rt::code_resource>();

  auto adapter =
      lithe::rt::bind_managed_entry<sig_t>(std::move(*entry_result), code, 1);
  REQUIRE(adapter.has_value());
  CHECK(adapter->valid());

  // Invoke via the managed adapter.
  std::array<lithe::rt::runtime_value, 2> args{
      lithe::rt::runtime_value{std::int64_t{13}},
      lithe::rt::runtime_value{std::int64_t{29}},
  };
  auto result = adapter->invoke(args);
  REQUIRE(result.has_value());

  // Result should be 13 + 29 = 42.
  const auto *val = std::get_if<std::int64_t>(&*result);
  REQUIRE(val != nullptr);
  CHECK(*val == 42LL);

  CHECK(code->active_frame_count() == 0);
  code->state.store(lithe::rt::code_state::retiring,
                    std::memory_order_release);
  result = adapter->invoke(args);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == lithe::rt::trap_code::deoptimization_requested);
  CHECK(code->active_frame_count() == 0);
  CHECK(code->state.load(std::memory_order_acquire) ==
        lithe::rt::code_state::retiring);
}

// ============================================================================
// code_version_metadata ownership migrates to rt::code_manager at this boundary
// ============================================================================

TEST_CASE(

    "code_version_metadata: ownership migrates to rt::code_manager",
    "[engine_integration][ownership]") {
  // The engine side holds code_version_metadata in resource_store entries.
  // When bind_managed_entry is called, the adapter holds a
  // shared_ptr<code_resource> which is the rt::code_manager-owned object. After
  // the bind, the managed side is authoritative for this version.
  //
  // Structural check: build a runtime_instance with code_manager; install a
  // code_resource; verify the engine_integration adapter shares that resource.

  auto rt_result = lithe::rt::runtime_instance::create(
      {lithe::rt::execution_profile::managed_language});
  REQUIRE(rt_result.has_value());
  auto &rt = **rt_result;

  // Compile a managed_function via rt::compile() (existing path).
  auto fn = make_add_fn_rt();
  auto mfn = lithe::rt::compile(rt, fn);
  REQUIRE(mfn.has_value());
  CHECK(mfn->bound());

  // An unbound managed invoker remains unavailable.
  auto thread_result = rt.attach_current_thread();
  REQUIRE(thread_result.has_value());
  auto &thread = *thread_result;

  std::array<lithe::rt::runtime_value, 2> args{
      lithe::rt::runtime_value{std::int64_t{1}},
      lithe::rt::runtime_value{std::int64_t{2}},
  };
  auto result = mfn->invoke(thread, args);
  CHECK(!result.has_value());

  // Compile a typed entry and bind it into the managed_function. The function
  // now owns the erased managed ABI while the adapter owns the typed lease.
  using sig_t = std::int64_t(std::int64_t, std::int64_t);
  cg::backends::interpreter_backend backend;
  auto typed_fn = make_add_fn_rt();
  auto art = ex::cpo::compile(backend, std::move(typed_fn));
  REQUIRE(art.has_value());
  auto res = ex::cpo::install(backend, std::move(*art));
  REQUIRE(res.has_value());
  auto entry = ex::cpo::get_entry(backend, *res, ex::type_tag<sig_t>{});
  REQUIRE(entry.has_value());

  auto bound = lithe::rt::bind_managed_entry<sig_t>(*mfn, std::move(*entry));
  REQUIRE(bound.has_value());
  CHECK(mfn->invocable());

  result = mfn->invoke(thread, args);
  REQUIRE(result.has_value());
  REQUIRE(std::get_if<std::int64_t>(&*result) != nullptr);
  CHECK(std::get<std::int64_t>(*result) == 3);
  CHECK(mfn->code_resource_handle()->active_frame_count() == 0);

  SECTION("wrong argument type is rejected") {
    std::array<lithe::rt::runtime_value, 2> bad_args{
        lithe::rt::runtime_value{true},
        lithe::rt::runtime_value{std::int64_t{2}},
    };
    const auto bad = mfn->invoke(thread, bad_args);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == lithe::rt::trap_code::invalid_indirect_call);
  }

  SECTION("wrong arity is rejected") {
    std::array<lithe::rt::runtime_value, 1> short_args{
        lithe::rt::runtime_value{std::int64_t{1}},
    };
    const auto bad = mfn->invoke(thread, short_args);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == lithe::rt::trap_code::invalid_indirect_call);
  }
}

// ============================================================================
// managed_integration_context: end-to-end via integration adapter
// ============================================================================

TEST_CASE(

    "managed_integration_context: invoke via interpreter adapter",
    "[engine_integration][context]") {
  using sig_t = std::int64_t(std::int64_t, std::int64_t);

  auto rt_result = lithe::rt::runtime_instance::create(
      {lithe::rt::execution_profile::managed_language});
  REQUIRE(rt_result.has_value());
  auto rt_ptr = *rt_result;

  cg::backends::interpreter_backend backend;
  auto fn = make_add_fn_rt();

  auto art = ex::cpo::compile(backend, std::move(fn));
  REQUIRE(art.has_value());
  auto res = ex::cpo::install(backend, std::move(*art));
  REQUIRE(res.has_value());
  auto entry_result = ex::cpo::get_entry(backend, *res, ex::type_tag<sig_t>{});
  REQUIRE(entry_result.has_value());

  auto code = std::make_shared<lithe::rt::code_resource>();
  auto adapter =
      lithe::rt::bind_managed_entry<sig_t>(std::move(*entry_result), code, 42);
  REQUIRE(adapter.has_value());

  lithe::rt::managed_integration_context<sig_t> ctx{std::move(*adapter),
                                                    rt_ptr};
  CHECK(ctx.valid());

  auto thread_result = rt_ptr->attach_current_thread();
  REQUIRE(thread_result.has_value());
  auto &thread = *thread_result;

  std::array<lithe::rt::runtime_value, 2> args{
      lithe::rt::runtime_value{std::int64_t{7}},
      lithe::rt::runtime_value{std::int64_t{8}},
  };

  auto result = ctx.invoke(thread, args);
  REQUIRE(result.has_value());
  const auto *val = std::get_if<std::int64_t>(&*result);
  REQUIRE(val != nullptr);
  CHECK(*val == 15LL);
}

#if defined(LITHE_HAS_ASMJIT)
TEST_CASE(

    "managed_function owns an AsmJIT entry through the managed ABI",
    "[engine_integration][asmjit][ownership]") {
  using sig_t = std::int64_t(std::int64_t, std::int64_t);

  auto rt_result = lithe::rt::runtime_instance::create(
      {lithe::rt::execution_profile::managed_language});
  REQUIRE(rt_result.has_value());
  auto &runtime = **rt_result;

  auto managed = lithe::rt::compile(runtime, make_add_fn_rt());
  REQUIRE(managed.has_value());

  cg::backends::asmjit_backend backend;
  auto native = ex::cpo::compile_and_install(backend, make_add_fn_rt());
  REQUIRE(native.has_value());
  auto entry = ex::cpo::get_entry(backend, *native, ex::type_tag<sig_t>{});
  REQUIRE(entry.has_value());

  // The typed entry captures the shared native payload. Once bound, the
  // managed function remains callable independently of the local resource.
  auto bound =
      lithe::rt::bind_managed_entry<sig_t>(*managed, std::move(*entry));
  REQUIRE(bound.has_value());
  native = std::unexpected(ex::compile_install_error{"released by test"});

  auto thread = runtime.attach_current_thread();
  REQUIRE(thread.has_value());
  std::array<lithe::rt::runtime_value, 2> args{
      lithe::rt::runtime_value{std::int64_t{20}},
      lithe::rt::runtime_value{std::int64_t{22}},
  };
  const auto result = managed->invoke(*thread, args);
  REQUIRE(result.has_value());
  CHECK(std::get<std::int64_t>(*result) == 42);
  CHECK(managed->code_resource_handle()->active_frame_count() == 0);
}
#endif

TEST_CASE(

    "local_lithe_engine automatically compiles binds and invokes",
    "[lithe][engine_integration][local_engine][automatic]") {
  using sig_t = std::int64_t(std::int64_t, std::int64_t);

  auto runtime_result = lithe::rt::runtime_instance::create(
      {lithe::rt::execution_profile::managed_language});
  REQUIRE(runtime_result.has_value());

  const auto root = std::filesystem::temp_directory_path() /
                    ("lithe_local_engine_" + std::to_string(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } remove_root{root};

  lithe::execution::store::memory_catalog catalog;
  lithe::execution::store::filesystem_blob_store blobs{root};
  lithe::local_lithe_engine engine{
      std::tuple{cg::backends::interpreter_backend{}}, *runtime_result, catalog,
      blobs};

  auto thread = (*runtime_result)->attach_current_thread();
  REQUIRE(thread.has_value());
  std::array<lithe::rt::runtime_value, 2> args{
      lithe::rt::runtime_value{std::int64_t{19}},
      lithe::rt::runtime_value{std::int64_t{23}},
  };
  auto result = engine.compile_and_invoke<sig_t>(make_add_fn_rt(), *thread, args);
  REQUIRE(result.has_value());
  CHECK(std::get<std::int64_t>(*result) == 42);
  CHECK(engine.execution_engine().profiling().total_compiles == 1);
  CHECK(engine.execution_engine().profiling().total_installs == 1);
  CHECK(engine.execution_engine().profiling().total_invocations == 1);

  auto cached =
      engine.compile_and_invoke<sig_t>(make_add_fn_rt(), *thread, args);
  REQUIRE(cached.has_value());
  CHECK(std::get<std::int64_t>(*cached) == 42);
  CHECK(engine.execution_engine().profiling().total_compiles == 1);
  CHECK(engine.execution_engine().profiling().total_installs == 1);
  CHECK(engine.execution_engine().profiling().total_invocations == 2);
  CHECK(engine.managed_cache_size() == 1);
  CHECK(engine.managed_cache_hits() == 1);

  std::array<bool, 6> concurrent_ok{};
  std::vector<std::thread> workers;
  for (std::size_t i = 0; i < concurrent_ok.size(); ++i) {
    workers.emplace_back([&, i] {
      concurrent_ok[i] =
          engine.compile_managed<sig_t>(make_add_fn_rt()).has_value();
    });
  }
  for (auto &worker : workers) worker.join();
  CHECK(std::ranges::all_of(concurrent_ok, std::identity{}));
  CHECK(engine.execution_engine().profiling().total_compiles == 1);
  CHECK(engine.managed_cache_hits() == 7);
  CHECK(engine.managed_cache_misses() == 1);
}

TEST_CASE(

    "local_lithe_engine runs HL MIR through portable cache and lowering",
    "[lithe][engine_integration][local_engine][hl_pipeline]") {
  using sig_t = std::int64_t(std::int64_t, std::int64_t);
  auto runtime_result = lithe::rt::runtime_instance::create(
      {lithe::rt::execution_profile::managed_language});
  REQUIRE(runtime_result.has_value());

  const auto root = std::filesystem::temp_directory_path() /
                    ("lithe_hl_engine_" + std::to_string(
                        std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } remove_root{root};

  ex::store::memory_catalog catalog;
  ex::store::filesystem_blob_store blobs{root};
  lithe::local_lithe_engine engine{
      std::tuple{cg::backends::interpreter_backend{}}, *runtime_result, catalog,
      blobs};
  auto thread = (*runtime_result)->attach_current_thread();
  REQUIRE(thread.has_value());
  std::array<lithe::rt::runtime_value, 2> args{
      lithe::rt::runtime_value{std::int64_t{20}},
      lithe::rt::runtime_value{std::int64_t{22}},
  };

  auto hl_function = make_hl_add_fn_rt();
  const std::array<const cg::hl::hl_mir_function *, 1> functions{&hl_function};
  auto frozen = lithe::ir::portable::freeze_module(functions);
  REQUIRE(frozen.has_value());
  auto optimized = engine.portable_cache().load_or_optimize(std::move(*frozen));
  REQUIRE(optimized.has_value());
  REQUIRE(optimized->module.functions.size() == 1);
  std::string optimized_ops;
  for (const auto &operation : optimized->module.functions.front().ops) {
    optimized_ops += std::to_string(operation.id) + ":" + operation.name + "(";
    for (const auto operand : operation.operand_ids)
      optimized_ops += std::to_string(operand) + ",";
    optimized_ops += ")->";
    for (const auto result_id : operation.result_ids)
      optimized_ops += std::to_string(result_id) + ",";
    optimized_ops += " ";
  }
  INFO(optimized_ops);
  CHECK(std::ranges::count_if(
            optimized->module.functions.front().ops,
            [](const auto &operation) { return operation.name == "argument"; }) ==
        2);
  auto result = engine.compile_and_invoke<sig_t>(
      std::move(hl_function), *thread, args);
  const auto error_detail = result ? std::string{} : result.error().detail;
  INFO(error_detail);
  REQUIRE(result.has_value());
  CHECK(std::get<std::int64_t>(*result) == 42);
  CHECK(engine.execution_engine().profiling().total_compiles == 1);
  CHECK(catalog.size() == 1);
}

TEST_CASE(

    "local_lithe_engine bounds resident code and retires evictions",
    "[lithe][engine_integration][local_engine][eviction]") {
  using sig_t = std::int64_t(std::int64_t, std::int64_t);
  auto runtime_result = lithe::rt::runtime_instance::create(
      {lithe::rt::execution_profile::managed_language});
  REQUIRE(runtime_result.has_value());
  const auto root = std::filesystem::temp_directory_path() /
                    ("lithe_resident_engine_" + std::to_string(
                        std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } remove_root{root};
  ex::store::memory_catalog catalog;
  ex::store::filesystem_blob_store blobs{root};
  lithe::local_lithe_engine engine{
      std::tuple{cg::backends::interpreter_backend{}}, *runtime_result, catalog,
      blobs};
  engine.set_managed_cache_capacity(1);

  auto first_ir = make_add_fn_rt();
  auto first = engine.compile_managed<sig_t>(first_ir);
  REQUIRE(first.has_value());
  auto first_code = first->code_resource_handle();

  auto second_ir = make_add_fn_rt();
  second_ir.function.name = "second_add_integration_test";
  auto second = engine.compile_managed<sig_t>(std::move(second_ir));
  REQUIRE(second.has_value());
  CHECK(engine.managed_cache_size() == 1);
  CHECK(engine.managed_cache_misses() == 2);
  CHECK(engine.managed_cache_evictions() == 1);
  CHECK(first_code->state.load(std::memory_order_acquire) ==
        lithe::rt::code_state::retired);
  CHECK_FALSE(first_code->unwind.live);
  CHECK_FALSE(first_code->roots.live);
}

TEST_CASE(

    "managed ABI supports pointers rooted objects handles functions and void",
    "[lithe][engine_integration][managed_abi]") {
  using object_ref = lithe::runtime::values::object_ref;
  using managed_handle = lithe::runtime::values::managed_handle;
  using function_ref = lithe::runtime::values::native_function_ref;
  using sig_t = void(void *, object_ref, managed_handle, function_ref);

  auto runtime_result = lithe::rt::runtime_instance::create(
      {lithe::rt::execution_profile::managed_language});
  REQUIRE(runtime_result.has_value());
  auto &runtime = **runtime_result;
  auto managed = lithe::rt::compile(runtime, make_add_fn_rt());
  REQUIRE(managed.has_value());

  int raw_storage = 7;
  object_ref object{&raw_storage, 77, 9};
  auto explicit_root = runtime.root(object);
  function_ref function{&raw_storage, 0, lithe::runtime::ffi::type_hint_void,
                        {}};
  bool called = false;
  lithe::execution::typed_entry<sig_t> entry{
      lithe::execution::entry_lease{lithe::execution::make_frame_counter()},
      std::function<sig_t>{[&](void *raw, object_ref received,
                               managed_handle rooted,
                               function_ref received_function) {
        CHECK(raw == &raw_storage);
        CHECK(received.ptr == &raw_storage);
        CHECK(received.layout_id == 77);
        CHECK(rooted.get().ptr == &raw_storage);
        CHECK(received_function.fn_ptr == &raw_storage);
        called = true;
      }}};

  auto bound = lithe::rt::bind_managed_entry<sig_t>(*managed, std::move(entry));
  REQUIRE(bound.has_value());
  REQUIRE(managed->signature().has_value());
  CHECK(managed->signature()->result == lithe::rt::managed_abi_kind::void_result);
  CHECK(managed->signature()->arity == 4);
  CHECK(managed->signature()->arguments[1] ==
        lithe::rt::managed_abi_kind::object_reference);
  CHECK(managed->signature()->arguments[2] ==
        lithe::rt::managed_abi_kind::managed_handle);

  auto thread = runtime.attach_current_thread();
  REQUIRE(thread.has_value());
  std::array<lithe::rt::runtime_value, 4> args{
      lithe::runtime::values::make_ptr(&raw_storage),
      lithe::runtime::values::make_object(object),
      lithe::runtime::values::make_managed_handle(explicit_root.handle()),
      lithe::runtime::values::make_func(function),
  };
  const auto result = managed->invoke(*thread, args);
  REQUIRE(result.has_value());
  CHECK(lithe::runtime::values::is_void(*result));
  CHECK(called);
  CHECK(thread->context().current_frame == nullptr);
  CHECK(thread->context().managed_depth == 0);
}
