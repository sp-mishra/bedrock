// =============================================================================
// test_lithe_impl2_compat.cpp — guard the additive contract (§11 P2)
//
// Asserts that the old API — emit() and execute_with_fallback — compiles and
// behaves identically after the impl-2 additions.  The facet adapters are
// additive; they must not modify any existing API surface.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/backends/lithe_codegen_backend_registry.hpp"
#include "lithe/backends/lithe_codegen_debug_text_backend.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/backends/lithe_execution_backends.hpp"
#include "lithe/lithe.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/lithe_engine.hpp"
#include "lithe/lithe_ir_core.hpp"
#include "lithe/lithe_rt.hpp"

namespace cg = lithe::codegen;

// ============================================================================
// Helper: build a trivial "ret 0" MIR function
// ============================================================================

namespace {
void public_header_noop() noexcept {}

cg::mir::physical_mir_function make_ret0_fn() {
  using namespace cg;

  allocated_function_ir fn;
  fn.name = "ret0";
  fn.cfg.entry_block = 1;

  allocated_basic_block bb;
  bb.id = 1;
  bb.name = "entry";

  allocated_instruction ret;
  ret.id = 1;
  ret.op = opcode::ret;
  ret.uses = {allocated_operand::as_i64(0)};
  bb.instructions.push_back(ret);
  fn.blocks.push_back(std::move(bb));

  cg::mir::physical_mir_function phys;
  phys.function = std::move(fn);
  phys.metadata.current_phase = cg::mir::phase::physical_mir;
  return phys;
}
} // anonymous namespace

TEST_CASE(

    "public Lithe aggregate headers compose in one translation unit",
    "[lithe][headers][smoke]") {
  STATIC_REQUIRE(
      lithe::folding::DomainFolder<lithe::folding::arithmetic_folder>);
  STATIC_REQUIRE(
      cg::backends::interpreter_backend::supports_opcode(cg::opcode::ret));

  constexpr auto proxy =
      lithe::runtime::ffi::binding::bind_native_function<&public_header_noop>();
  STATIC_REQUIRE(proxy.has_value());
  STATIC_REQUIRE(proxy->arity == 0);
  STATIC_REQUIRE(proxy->ret_type == lithe::runtime::ffi::type_hint_void);

  lithe::ir::portable::portable_module module;
  const auto encoded = lithe::ir::portable::encode_portable(module);
  CHECK_FALSE(encoded.has_value()); // empty modules fail structural validation
}

// ============================================================================
// Old API: interpreter_backend::emit() is unchanged
// ============================================================================

TEST_CASE(

    "compat: interpreter_backend::emit() works unchanged after impl-2",
    "[compat][interpreter]") {
  cg::backends::interpreter_backend backend;
  auto fn = make_ret0_fn();

  // Old path — must still compile and return a valid artifact.
  auto art = backend.emit(fn);
  // The null return instruction sets return_value = 0.
  CHECK(art.kind != cg::artifact_kind::none);
}

TEST_CASE(

    "compat: debug_text_backend::emit() works unchanged after impl-2",
    "[compat][debug_text]") {
  cg::backends::debug_text_backend backend;
  auto fn = make_ret0_fn();
  auto art = backend.emit(fn);
  CHECK(art.kind != cg::artifact_kind::none);
  CHECK(!art.text_payload.empty());
}

// ============================================================================
// Old API: execute_with_fallback is unchanged
// ============================================================================

TEST_CASE(

    "compat: execute_with_fallback unchanged", "[compat][fallback]") {
  auto fn = make_ret0_fn();

  auto primary = cg::backends::make_backend("debug_text");
  auto fallback = cg::backends::make_backend("interpreter");
  REQUIRE(primary.has_value());
  REQUIRE(fallback.has_value());

  auto art = cg::backends::execute_with_fallback(fn, *primary, *fallback);
  // debug_text handles all features, so no fallback should trigger.
  CHECK(art.kind != cg::artifact_kind::none);
  bool has_fallback_diagnostic = false;
  for (const auto &d : art.diagnostics) {
    if (d.find("execute_with_fallback") != std::string::npos) {
      has_fallback_diagnostic = true;
    }
  }
  CHECK_FALSE(has_fallback_diagnostic);
}

// ============================================================================
// backend_variant still exists and is usable
// ============================================================================

TEST_CASE(

    "compat: backend_variant type exists and can hold all backends",
    "[compat][variant]") {
  using BV = cg::backends::backend_variant;

  // Can still construct each variant member.
  BV v1{cg::backends::debug_text_backend{}};
  BV v2{cg::backends::interpreter_backend{}};
  BV v3{cg::backends::null_backend{}};

  CHECK(v1.index() != v2.index());
  CHECK(v2.index() != v3.index());
}

// ============================================================================
// list_available_backends() is unchanged
// ============================================================================

TEST_CASE(

    "compat: list_available_backends returns expected names",
    "[compat][registry]") {
  auto names = cg::backends::list_available_backends();
  REQUIRE_FALSE(names.empty());

  bool has_interpreter = false;
  bool has_debug_text = false;
  for (auto n : names) {
    if (n == "interpreter")
      has_interpreter = true;
    if (n == "debug_text")
      has_debug_text = true;
  }
  CHECK(has_interpreter);
  CHECK(has_debug_text);
}
