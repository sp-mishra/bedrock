// =============================================================================
// test_lithe_interpreter_vertical.cpp — interpreter compile→install→invoke (§11 P3)
//
// Full vertical on the interpreter backend:
//   1. Compile a small MIR function (a + b = return).
//   2. Install the artifact into an interpreter_resource.
//   3. get_entry with a typed signature.
//   4. Call via typed_entry; assert result.
//   5. Assert compile and invoke are separate steps (program artifact exists
//      before any invoke).
//   6. Assert entry_lease keeps storage but frame count == 0 outside calls.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <cstdint>

#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/lithe_execution/facet.hpp"
#include "lithe/lithe_execution/entry.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/backends/lithe_codegen_interpreter_facet.hpp"

namespace ex = lithe::execution;
namespace cg = lithe::codegen;
namespace mir = lithe::codegen::mir;

// ============================================================================
// Helper: build a minimal "load_arg 0 + load_arg 1; ret" MIR function
// ============================================================================

namespace {
    mir::physical_mir_function make_add_fn() {
        using namespace cg;

        allocated_function_ir fn;
        fn.name = "add_test";
        fn.cfg.entry_block = 1;

        allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";

        // load_arg 0 → r0
        allocated_instruction load0;
        load0.id = 1;
        load0.op = opcode::load_arg;
        load0.defs = {allocated_operand::as_preg({0, "r0"})};
        load0.uses = {allocated_operand::as_argument_index(0)};
        bb.instructions.push_back(load0);

        // load_arg 1 → r1
        allocated_instruction load1;
        load1.id = 2;
        load1.op = opcode::load_arg;
        load1.defs = {allocated_operand::as_preg({1, "r1"})};
        load1.uses = {allocated_operand::as_argument_index(1)};
        bb.instructions.push_back(load1);

        // add r0, r1 → r2
        allocated_instruction add;
        add.id = 3;
        add.op = opcode::add;
        add.defs = {allocated_operand::as_preg({2, "r2"})};
        add.uses = {
            allocated_operand::as_preg({0, "r0"}),
            allocated_operand::as_preg({1, "r1"})
        };
        bb.instructions.push_back(add);

        // ret r2
        allocated_instruction ret;
        ret.id = 4;
        ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({2, "r2"})};
        bb.instructions.push_back(ret);

        fn.blocks.push_back(std::move(bb));

        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }
} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE (


"interpreter vertical: compile produces an artifact before any invoke"
,
"[interpreter][vertical][compile]"
)
{
    cg::backends::interpreter_backend backend;
    auto fn = make_add_fn();

    // compile step only — no invoke yet
    auto art_result = ex::cpo::compile(backend, std::move(fn));
    REQUIRE(art_result.has_value());
    CHECK(art_result->valid());
    CHECK(art_result->manifest.role == ex::artifact_class::interpreter_plan);

    // The program exists before any call.
    CHECK(art_result->payload.valid());
}

TEST_CASE (


"interpreter vertical: install produces a valid resource"
,
"[interpreter][vertical][install]"
)
{
    cg::backends::interpreter_backend backend;

    auto art = ex::cpo::compile(backend, make_add_fn());
    REQUIRE(art.has_value());

    auto res = ex::cpo::install(backend, std::move(*art));
    REQUIRE(res.has_value());
    CHECK(res->valid());
}

TEST_CASE (


"interpreter vertical: compile and invoke are separate steps"
,
"[interpreter][vertical][separation]"
)
{
    cg::backends::interpreter_backend backend;

    // compile
    auto art = ex::cpo::compile(backend, make_add_fn());
    REQUIRE(art.has_value());
    // install
    auto res = ex::cpo::install(backend, std::move(*art));
    REQUIRE(res.has_value());

    // The resource is valid BEFORE any invoke.
    REQUIRE(res->valid());

    // get_entry
    auto entry_result = ex::cpo::get_entry(backend, *res,
        ex::type_tag<std::int64_t(std::int64_t, std::int64_t)>{});
    REQUIRE(entry_result.has_value());
    CHECK(entry_result->valid());

    // invoke via typed entry
    auto r = (*entry_result)(3, 4);
    CHECK(r == 7);
}

TEST_CASE (


"interpreter vertical: entry_lease frame count is 0 outside call"
,
"[interpreter][vertical][entry]"
)
{
    cg::backends::interpreter_backend backend;

    auto art = ex::cpo::compile(backend, make_add_fn());
    REQUIRE(art.has_value());
    auto res = ex::cpo::install(backend, std::move(*art));
    REQUIRE(res.has_value());

    auto entry_result = ex::cpo::get_entry(backend, *res,
        ex::type_tag<std::int64_t(std::int64_t, std::int64_t)>{});
    REQUIRE(entry_result.has_value());

    // Frame count is 0 before call.
    CHECK(entry_result->active_frames() == 0);

    std::uint64_t frames_during = 0;
    // We can't observe inside the typed_entry call directly, but we can verify
    // that after the call the counter returns to 0.
    auto r = (*entry_result)(10, 20);
    CHECK(r == 30);
    CHECK(entry_result->active_frames() == 0);
    (void)frames_during;
}

TEST_CASE (


"interpreter vertical: repeated invocations via same entry"
,
"[interpreter][vertical][repeated]"
)
{
    cg::backends::interpreter_backend backend;

    auto art = ex::cpo::compile(backend, make_add_fn());
    REQUIRE(art.has_value());
    auto res = ex::cpo::install(backend, std::move(*art));
    REQUIRE(res.has_value());

    auto entry_result = ex::cpo::get_entry(backend, *res,
        ex::type_tag<std::int64_t(std::int64_t, std::int64_t)>{});
    REQUIRE(entry_result.has_value());

    for (std::int64_t i = 0; i < 5; ++i) {
        auto r = (*entry_result)(i, i * 2);
        CHECK(r == i + i * 2);
    }
}
