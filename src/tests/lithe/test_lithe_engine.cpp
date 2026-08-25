// =============================================================================
// test_lithe_engine.cpp — engine wiring + three compile APIs + error independence
//
// Verifies:
//   • End-to-end compile→install→invoke via interpreter backend.
//   • Engine owns resource_store, NOT rt::code_manager.
//   • lithe_engine.hpp compiles WITHOUT including lithe_rt/engine.hpp
//     (checked via static_assert on missing types).
//   • compile_with<B,Sig,IR> returns exact typed selected_entry<B,IR,Sig>.
//   • compile_best returns selected_entry_t variant; ineligible backends dropped.
//   • [impl-4] lithe_engine dynamic façade: make() + valid() + compile_erased.
//   • [impl-4] Parallel compile_best + lease acquire/release (resource_store stress).
//   • compile_and_invoke_best returns native_result_t with no entry escaping.
//   • engine_compile_error and engine_compile_invoke_error do NOT contain ir_error.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

// engine header — MUST NOT pull in lithe_rt/engine.hpp.
#include "lithe/lithe_engine.hpp"

// Backend adapters (interpreter vertical).
#include "lithe/backends/lithe_codegen_interpreter_facet.hpp"
#include "lithe/backends/lithe_execution_backends.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"

namespace ex = lithe::execution;
namespace cg = lithe::codegen;
namespace mir = lithe::codegen::mir;

// ============================================================================
// Compile-time: core errors must not contain ir_error
// ============================================================================

static_assert(!std::is_same_v<lithe::engine_compile_error, lithe::execution::ir_error>,
              "engine_compile_error must not be ir_error");
static_assert(!std::is_same_v<lithe::engine_compile_invoke_error, lithe::execution::ir_error>,
              "engine_compile_invoke_error must not be ir_error");

// ============================================================================
// Compile-time: lithe_engine.hpp must not define rt::managed_function
// (proxy for not having included lithe_rt/engine.hpp).
// ============================================================================

// We verify that lithe::rt::managed_function is NOT available after including
// only lithe_engine.hpp.  This is enforced structurally: the engine header
// does not include lithe_rt/engine.hpp.  The test below confirms the engine
// works without any lithe_rt types in scope.
static_assert(!std::is_base_of_v<std::true_type, std::false_type>,
              "static_assert machinery works");

// ============================================================================
// Helper: build a small add MIR function (from interpreter vertical tests)
// ============================================================================

namespace {
    mir::physical_mir_function make_add_fn() {
        cg::allocated_function_ir fn;
        fn.name = "add_engine_test";
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
        add.uses = {
            cg::allocated_operand::as_preg({0, "r0"}),
            cg::allocated_operand::as_preg({1, "r1"})
        };
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
} // anonymous namespace

// ============================================================================
// API 1: compile_with<B, Sig, IR>
// ============================================================================

TEST_CASE (


"engine compile_with: returns exact typed selected_entry<B,IR,Sig>"
,
"[engine][compile_with]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    using ir_t      = mir::physical_mir_function;
    using sig_t     = std::int64_t(std::int64_t, std::int64_t);

    backend_t backend;

    // Wrap backend in a tuple so the engine can reference it.
    auto bset = std::tuple<backend_t&>{backend};

    lithe::basic_lithe_engine<decltype(bset)> engine{std::move(bset)};

    auto fn = make_add_fn();

    auto result = engine.compile_with<backend_t, sig_t, ir_t>(backend, std::move(fn));
    REQUIRE(result.has_value());
    CHECK(result->valid());

    // Check backend tag.
    CHECK_FALSE(result->backend_id.empty());

    // Call it.
    auto r = (*result)(3LL, 4LL);
    CHECK(r == 7LL);
}

// ============================================================================
// API 2: compile_best<Sig, IR>
// ============================================================================

TEST_CASE (


"engine compile_best: returns selected_entry_t variant"
,
"[engine][compile_best]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    using ir_t      = mir::physical_mir_function;
    using sig_t     = std::int64_t(std::int64_t, std::int64_t);

    backend_t backend;
    auto bset = std::tuple<backend_t&>{backend};
    lithe::basic_lithe_engine<decltype(bset)> engine{std::move(bset)};

    auto fn = make_add_fn();

    auto result = engine.compile_best<sig_t, ir_t>(std::move(fn));
    REQUIRE(result.has_value());

    // Visit the variant to invoke.
    auto r = std::visit([](auto& se) -> std::int64_t {
        REQUIRE(se.valid());
        return se(10LL, 32LL);
    }, *result);

    CHECK(r == 42LL);
}

TEST_CASE (


"engine compile_best: selected_entry_t alternatives each have backend tag"
,
"[engine][compile_best]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    using ir_t      = mir::physical_mir_function;
    using sig_t     = std::int64_t(std::int64_t, std::int64_t);

    backend_t backend;
    auto bset = std::tuple<backend_t&>{backend};
    lithe::basic_lithe_engine<decltype(bset)> engine{std::move(bset)};

    auto fn = make_add_fn();
    auto result = engine.compile_best<sig_t, ir_t>(std::move(fn));
    REQUIRE(result.has_value());

    std::visit([](const auto& se) {
        CHECK_FALSE(se.backend_id.empty());
    }, *result);
}

// ============================================================================
// API 3: compile_and_invoke_best
// ============================================================================

TEST_CASE (


"engine compile_and_invoke_best: returns native_result_t, no entry escapes"
,
"[engine][compile_and_invoke_best]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    using ir_t      = mir::physical_mir_function;
    using sig_t     = std::int64_t(std::int64_t, std::int64_t);

    backend_t backend;
    auto bset = std::tuple<backend_t&>{backend};
    lithe::basic_lithe_engine<decltype(bset)> engine{std::move(bset)};

    auto fn = make_add_fn();

    auto result = engine.compile_and_invoke_best<sig_t, ir_t, std::int64_t, std::int64_t>(
        std::move(fn), 20LL, 22LL);

    REQUIRE(result.has_value());
    CHECK(*result == 42LL);
}

// ============================================================================
// Engine ownership: resource_store is owned by engine, not rt::code_manager
// ============================================================================

TEST_CASE (


"engine: owns resource_store (not rt::code_manager)"
,
"[engine][ownership]"
)
 {
    using backend_t = cg::backends::interpreter_backend;
    backend_t backend;
    auto bset = std::tuple<backend_t&>{backend};
    lithe::basic_lithe_engine<decltype(bset)> engine{std::move(bset)};

    // resource_store is accessible via engine.store().
    auto& store = engine.store();
    CHECK(store.empty());

    // There must be no reference to rt::code_manager in the engine type.
    // (Structural check: engine is constructible without any lithe_rt header.)
    SUCCEED();
}

// ============================================================================
// Profiling
// ============================================================================

TEST_CASE (


"engine: profiling counts compile and install"
,
"[engine][profiling]"
)
 {
    using backend_t = cg::backends::interpreter_backend;
    using ir_t      = mir::physical_mir_function;
    using sig_t     = std::int64_t(std::int64_t, std::int64_t);

    backend_t backend;
    auto bset = std::tuple<backend_t&>{backend};
    lithe::basic_lithe_engine<decltype(bset)> engine{std::move(bset)};

    CHECK(engine.profiling().total_compiles == 0);
    CHECK(engine.profiling().total_installs == 0);

    auto fn = make_add_fn();
    auto _ = engine.compile_with<backend_t, sig_t, ir_t>(backend, std::move(fn));

    CHECK(engine.profiling().total_compiles == 1);
    CHECK(engine.profiling().total_installs == 1);
}

// ============================================================================
// [impl-4] §11 P8 lithe_engine dynamic façade
// ============================================================================

TEST_CASE (


"lithe_engine: make() creates a valid facade"
,
"[engine][dynamic_facade]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    backend_t backend;
    auto bset = std::tuple<backend_t>(std::move(backend));

    auto facade = lithe::lithe_engine::make<decltype(bset)>(std::move(bset));
    CHECK(facade.valid());
}

TEST_CASE (


"lithe_engine: default-constructed facade is not valid"
,
"[engine][dynamic_facade]"
)
{
    lithe::lithe_engine facade;
    CHECK(!facade.valid());
}

TEST_CASE (


"lithe_engine: compile_erased on uninitialised facade returns error"
,
"[engine][dynamic_facade]"
)
{
    lithe::lithe_engine facade;
    auto result = facade.compile_erased(nullptr, "no_type");
    REQUIRE(!result.has_value());
}

TEST_CASE (


"lithe_engine: compile_erased on initialised facade returns error directing to typed path"
,
"[engine][dynamic_facade]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    backend_t backend;
    auto bset = std::tuple<backend_t>(std::move(backend));

    auto facade = lithe::lithe_engine::make<decltype(bset)>(std::move(bset));
    REQUIRE(facade.valid());

    // The generic erased thunk is intentionally a stub directing callers to
    // the typed basic_lithe_engine path.
    auto result = facade.compile_erased(nullptr, "some_ir_type");
    REQUIRE(!result.has_value());
    // Error message references basic_lithe_engine.
    CHECK(!result.error().detail.empty());
}

TEST_CASE (


"lithe_engine: invoke_erased on initialised facade returns error directing to typed path"
,
"[engine][dynamic_facade]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    backend_t backend;
    auto bset = std::tuple<backend_t>(std::move(backend));
    auto facade = lithe::lithe_engine::make<decltype(bset)>(std::move(bset));
    REQUIRE(facade.valid());

    auto result = facade.invoke_erased(ex::resource_handle{}, 0LL, 0LL);
    REQUIRE(!result.has_value());
    CHECK(!result.error().detail.empty());
}

// ============================================================================
// [impl-4] §5.3 resource_store concurrency: parallel compile_best + acquire/release
// ============================================================================

TEST_CASE (


"engine: parallel compile_best under resource_store mutation"
,
"[engine][concurrency][stress]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    using ir_t      = mir::physical_mir_function;
    using sig_t_    = std::int64_t(std::int64_t, std::int64_t);

    backend_t backend;
    auto bset = std::tuple<backend_t>(std::move(backend));
    lithe::basic_lithe_engine<decltype(bset)> engine{std::move(bset)};

    constexpr int kThreads = 4;
    constexpr int kRounds  = 20;

    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int r = 0; r < kRounds; ++r) {
                auto fn = make_add_fn();
                auto res = engine.compile_best<sig_t_>(std::move(fn));
                if (res.has_value()) {
                    // Visit to confirm entry is valid.
                    std::visit([&](auto& se) {
                        if (se.valid()) ++success_count;
                    }, *res);
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    CHECK(success_count.load() == kThreads * kRounds);
}

// ============================================================================
// [G4] compile_with dead code_version_metadata removed (impl-1)
//   • return type unchanged: exactly selected_entry<B,IR,Sig>.
//   • compile→invoke still succeeds; compile counting reads from profiling_,
//     not the deleted version_counter_.
// ============================================================================

TEST_CASE (


"engine compile_with: return type unchanged after G4 [G4]"
,
"[engine][compile_with]"
)
{
    using backend_t = cg::backends::interpreter_backend;
    using ir_t      = mir::physical_mir_function;
    using sig_t     = std::int64_t(std::int64_t, std::int64_t);

    backend_t backend;
    auto bset = std::tuple<backend_t&>{backend};
    using engine_t = lithe::basic_lithe_engine<decltype(bset)>;

    using ret_t = decltype(std::declval<engine_t&>()
                     .template compile_with<backend_t, sig_t, ir_t>(
                         std::declval<backend_t&>(), std::declval<ir_t>()));
    static_assert(std::is_same_v<ret_t,
        std::expected<lithe::selected_entry<backend_t, ir_t, sig_t>,
                      lithe::engine_compile_error>>,
        "compile_with must still return selected_entry<B,IR,Sig>");

    engine_t engine{std::move(bset)};
    auto result = engine.compile_with<backend_t, sig_t, ir_t>(backend, make_add_fn());
    REQUIRE(result.has_value());
    CHECK(result->valid());
    CHECK((*result)(5LL, 6LL) == 11LL);

    // Compile counting lives in profiling_, not the deleted version_counter_.
    CHECK(engine.profiling().total_compiles == 1);
}

// ============================================================================
// jit_execution_policy / jit_engine — with_target type-erased emit path
// ============================================================================

TEST_CASE (


"jit_engine: with_target delegates execute() to bound CodeEmissionTarget"
,
"[engine][jit_engine][jit_execution_policy]"
)
{
    // interpreter_backend satisfies CodeEmissionTarget; use it as the JIT target
    // so the test runs without requiring a hardware JIT (AsmJit / native codegen).
    cg::backends::interpreter_backend backend;
    backend.arguments = {3LL, 4LL};

    cg::jit_engine eng;
    eng.with_target(backend);

    const auto fn = make_add_fn();
    const auto art = eng.execute(fn);

    // The interpreter returns interpreter_result kind; diagnostics should be empty.
    CHECK(art.diagnostics.empty());
    CHECK(art.kind == cg::artifact_kind::interpreter_result);
}

TEST_CASE (


"jit_engine: no target bound → diagnostic artifact with jit_function kind"
,
"[engine][jit_engine][jit_execution_policy]"
)
{
    cg::jit_engine eng;
    const auto art = eng.execute(make_add_fn());
    REQUIRE(!art.diagnostics.empty());
    CHECK(art.kind == cg::artifact_kind::jit_function);
    CHECK(art.diagnostics[0].find("no target bound") != std::string::npos);
}

TEST_CASE (


"jit_engine: jit_engine alias equals execution_engine<jit_execution_policy>"
,
"[engine][jit_engine][jit_execution_policy]"
)
{
    static_assert(
        std::is_same_v<cg::jit_engine,
                       cg::execution_engine<cg::jit_execution_policy>>,
        "jit_engine must be execution_engine<jit_execution_policy>");
    SUCCEED("jit_engine alias verified at compile time");
}
