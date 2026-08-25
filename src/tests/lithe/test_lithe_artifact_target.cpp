#include "catch_amalgamated.hpp"

#include "lithe/lithe.hpp"
#include "lithe/backends/lithe_codegen_debug_text_backend.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/backends/lithe_codegen_assembler.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"

// Helpers shared across test cases in this file.
namespace {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // Build a minimal physical MIR function with one block containing a ret.
    mir::physical_mir_function make_trivial_physical(std::string name = "f") {
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = 1;

        allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";

        allocated_instruction ret_inst;
        ret_inst.id = 1;
        ret_inst.op = opcode::ret;
        bb.instructions.push_back(ret_inst);
        fn.blocks.push_back(std::move(bb));

        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }

    // Wrap a physical MIR function in a codegen_result (simulates a successful compile).
    codegen_result wrap_physical(mir::physical_mir_function fn) {
        codegen_result r;
        r.physical_mir = std::move(fn);
        return r;
    }
} // namespace

// ---------------------------------------------------------------------------
// Prompt 14 test 1 — debug_text_backend emits artifact_kind::debug_text
// ---------------------------------------------------------------------------
TEST_CASE (



"debug_text_backend emit() returns debug_text artifact"
,
"[lithe][artifact][debug_text]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    debug_text_backend backend;
    const auto fn  = make_trivial_physical("debug_fn");
    const auto art = backend.emit(fn);

    REQUIRE(art.kind == artifact_kind::debug_text);
    REQUIRE(art.name == "debug_fn");
    REQUIRE_FALSE(art.text_payload.empty());
    CHECK(art.diagnostics.empty());
    CHECK(art.text_payload.find("debug_fn") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Prompt 14 test 2 — interpreter_backend emits artifact_kind::interpreter_result
// ---------------------------------------------------------------------------
TEST_CASE (



"interpreter_backend emit() returns interpreter_result artifact"
,
"[lithe][artifact][interpreter]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    interpreter_backend backend;
    const auto fn  = make_trivial_physical("interp_fn");
    const auto art = backend.emit(fn);

    REQUIRE(art.kind == artifact_kind::interpreter_result);
    REQUIRE(art.name == "interp_fn");
    CHECK(art.diagnostics.empty());
}

// ---------------------------------------------------------------------------
// Prompt 14 test 3 — text_assembly_target emits artifact_kind::assembly_text
// ---------------------------------------------------------------------------
TEST_CASE (



"text_assembly_target emit() returns assembly_text artifact"
,
"[lithe][artifact][assembly]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    text_assembly_target target;
    const auto fn  = make_trivial_physical("asm_fn");
    const auto art = target.emit(fn);

    REQUIRE(art.kind == artifact_kind::assembly_text);
    REQUIRE(art.name == "asm_fn");
    REQUIRE_FALSE(art.text_payload.empty());
    CHECK(art.diagnostics.empty());
    CHECK(art.text_payload.find("asm_fn") != std::string::npos);
    CHECK(art.text_payload.find("ret") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Prompt 14 test 4 — compile_to_artifact works with debug_text_backend
// ---------------------------------------------------------------------------
TEST_CASE (



"compile_to_artifact produces debug_text artifact from expression"
,
"[lithe][artifact][compile_to_artifact]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    debug_text_backend backend;
    const auto expr = make_node<add_tag>(1, 2);
    const auto art  = compile_to_artifact(expr, backend);

    CHECK(art.kind == artifact_kind::debug_text);
    CHECK(art.diagnostics.empty());
    CHECK_FALSE(art.text_payload.empty());
}

// ---------------------------------------------------------------------------
// Prompt 14 test 5 — old compile_and_emit still works (backward compat)
// ---------------------------------------------------------------------------
TEST_CASE (



"compile_and_emit still works after artifact layer addition"
,
"[lithe][artifact][compile_and_emit]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    debug_text_backend backend;
    const auto expr   = make_node<add_tag>(1, 2);
    const auto result = compile_and_emit(expr, backend);

    CHECK(result.ok());
    CHECK(result.artifact_text.has_value());
    CHECK_FALSE(result.artifact_text->empty());
}

// ---------------------------------------------------------------------------
// emit_artifact: failed codegen_result propagates diagnostics, kind == none
// ---------------------------------------------------------------------------
TEST_CASE (



"emit_artifact on failed codegen_result returns diagnostic artifact"
,
"[lithe][artifact][emit_artifact]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    codegen_result bad;
    bad.diagnostics.push_back("synthetic compile failure");

    debug_text_backend backend;
    const auto art = emit_artifact(backend, bad);

    REQUIRE(art.kind == artifact_kind::none);
    REQUIRE_FALSE(art.diagnostics.empty());
    CHECK(art.diagnostics[0].find("compile failure") != std::string::npos);
}

// ---------------------------------------------------------------------------
// emit_artifact: successful codegen_result routes through target emit()
// ---------------------------------------------------------------------------
TEST_CASE (



"emit_artifact on successful codegen_result delegates to target emit"
,
"[lithe][artifact][emit_artifact]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    text_assembly_target target;
    const auto result = wrap_physical(make_trivial_physical("wrapped_fn"));
    const auto art    = emit_artifact(target, result);

    REQUIRE(art.kind == artifact_kind::assembly_text);
    CHECK(art.name == "wrapped_fn");
    CHECK(art.diagnostics.empty());
}

