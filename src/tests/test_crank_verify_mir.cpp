// =============================================================================
// test_crank_verify_mir.cpp — crank physical-MIR verifier gate (design §4).
//
// Covers:
//   1. A well-formed straight-line fn (block ends in ret with a value) verifies.
//   2. A block that ends without a terminator → verification_failed
//      (path_without_return_or_trap message).
//   3. expects_value + a ret with no value operand → missing_return_value.
//   4. verified_mir is only mintable by the verifier; default is invalid.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/verify_mir.hpp"

using namespace crank;

namespace {
    // Build a physical MIR function with a single block whose instructions are given.
    lithe::codegen::mir::physical_mir_function
    make_fn(std::string name, std::vector<lithe::codegen::allocated_instruction> insts) {
        lithe::codegen::mir::physical_mir_function f;
        f.function.name = std::move(name);
        // verify_physical_mir requires phase::physical_mir and a valid entry block (id != 0).
        f.metadata.current_phase = lithe::codegen::mir::phase::physical_mir;
        lithe::codegen::allocated_basic_block bb;
        bb.id = 1;
        bb.instructions = std::move(insts);
        f.function.blocks.push_back(std::move(bb));
        f.function.cfg.entry_block = 1;
        return f;
    }

    lithe::codegen::allocated_instruction ret_with_value() {
        lithe::codegen::allocated_instruction i;
        i.op = lithe::codegen::opcode::ret;
        i.uses.push_back(lithe::codegen::allocated_operand::as_i64(1));
        return i;
    }

    lithe::codegen::allocated_instruction ret_no_value() {
        lithe::codegen::allocated_instruction i;
        i.op = lithe::codegen::opcode::ret;
        return i;
    }

    lithe::codegen::allocated_instruction nop() {
        lithe::codegen::allocated_instruction i;
        i.op = lithe::codegen::opcode::nop;
        return i;
    }
} // namespace

TEST_CASE (

"well-formed value-returning fn verifies"
,
"[crank][verify_mir]"
)
 {
    auto f = make_fn("ok", {ret_with_value()});
    auto r = verify_crank_mir(f, /*expects_value=*/true);
    REQUIRE(r.completed());
    REQUIRE(r.unwrap().valid());
    REQUIRE(r.unwrap().function() == &f);
}

TEST_CASE (

"block without a terminator is rejected"
,
"[crank][verify_mir]"
)
 {
    auto f = make_fn("falloff", {nop()});  // no ret/branch
    auto r = verify_crank_mir(f, /*expects_value=*/false);
    REQUIRE_FALSE(r.completed());
    REQUIRE(r.error->kind == execution_error_kind::verification_failed);
    REQUIRE(r.error->ir_op == "bb1");
}

TEST_CASE (

"value-returning ret with no operand is missing_return_value"
,
"[crank][verify_mir]"
)
 {
    auto f = make_fn("noval", {ret_no_value()});
    auto r = verify_crank_mir(f, /*expects_value=*/true);
    REQUIRE_FALSE(r.completed());
    REQUIRE(r.error->kind == execution_error_kind::missing_return_value);
    REQUIRE(r.error->code() == "CRANK-E-EXEC-019");
}

TEST_CASE (

"void fn with a valueless ret verifies"
,
"[crank][verify_mir]"
)
 {
    auto f = make_fn("voidfn", {ret_no_value()});
    auto r = verify_crank_mir(f, /*expects_value=*/false);
    REQUIRE(r.completed());
}

TEST_CASE (

"default-constructed verified_mir is invalid"
,
"[crank][verify_mir]"
)
 {
    verified_mir m;
    REQUIRE_FALSE(m.valid());
    REQUIRE(m.function() == nullptr);
}
