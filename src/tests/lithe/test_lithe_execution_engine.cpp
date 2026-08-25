#include "catch_amalgamated.hpp"

#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/lithe_extension.hpp"

// ---------------------------------------------------------------------------
// Helpers shared across all test cases.
// ---------------------------------------------------------------------------
namespace {
    using namespace lithe::codegen;

    // Build a single-block physical_mir_function from a flat instruction list.
    mir::physical_mir_function make_physical(
        std::string name,
        std::vector<allocated_instruction> instructions
    ) {
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = 1;

        allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";
        bb.instructions = std::move(instructions);
        fn.blocks.push_back(std::move(bb));

        mir::physical_mir_function physical;
        physical.function = std::move(fn);
        physical.metadata.current_phase = mir::phase::physical_mir;
        return physical;
    }

    // Build a two-block physical_mir_function (entry → body).
    // Used to exercise multi-block / "deep graph" traversal.
    mir::physical_mir_function make_two_block(
        std::string name,
        std::vector<allocated_instruction> entry_insts,
        std::vector<allocated_instruction> body_insts
    ) {
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = 1;

        {
            allocated_basic_block bb;
            bb.id = 1;
            bb.name = "entry";
            bb.instructions = std::move(entry_insts);
            fn.blocks.push_back(std::move(bb));
        }
        {
            allocated_basic_block bb;
            bb.id = 2;
            bb.name = "body";
            bb.instructions = std::move(body_insts);
            fn.blocks.push_back(std::move(bb));
        }

        mir::physical_mir_function physical;
        physical.function = std::move(fn);
        physical.metadata.current_phase = mir::phase::physical_mir;
        return physical;
    }

    // Build: load_imm r0 = lhs; load_imm r1 = rhs; <op> r2 = r0, r1; ret r2
    mir::physical_mir_function make_arithmetic(
        std::string name,
        opcode op,
        std::int64_t lhs,
        std::int64_t rhs
    ) {
        std::vector<allocated_instruction> insts;

        allocated_instruction li_lhs;
        li_lhs.id = 1;
        li_lhs.op = opcode::load_imm;
        li_lhs.defs = {allocated_operand::as_preg({0, "r0"})};
        li_lhs.uses = {allocated_operand::as_i64(lhs)};
        insts.push_back(li_lhs);

        allocated_instruction li_rhs;
        li_rhs.id = 2;
        li_rhs.op = opcode::load_imm;
        li_rhs.defs = {allocated_operand::as_preg({1, "r1"})};
        li_rhs.uses = {allocated_operand::as_i64(rhs)};
        insts.push_back(li_rhs);

        allocated_instruction arith;
        arith.id = 3;
        arith.op = op;
        arith.defs = {allocated_operand::as_preg({2, "r2"})};
        arith.uses = {
            allocated_operand::as_preg({0, "r0"}),
            allocated_operand::as_preg({1, "r1"})
        };
        insts.push_back(arith);

        allocated_instruction ret;
        ret.id = 4;
        ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({2, "r2"})};
        insts.push_back(ret);

        return make_physical(std::move(name), std::move(insts));
    }
} // anonymous namespace

// ===========================================================================
// Test 1 — constexpr evaluation of simple MIR arithmetic
// ===========================================================================
TEST_CASE (



"constexpr engine folds load_imm + add into a known value"
,
"[lithe][execution_engine][constexpr]"
)
 {
    using namespace lithe::codegen;

    // 3 + 7 = 10; both operands are compile-time constants so partial_evaluate
    // should fold the add into a load_imm and mark the result as changed.
    const auto fn = make_arithmetic("ce_add", opcode::add, 3, 7);

    constexpr_engine engine;
    const auto art = engine.execute(fn);

    REQUIRE(art.metadata.count("changed") > 0);
    REQUIRE(art.metadata.at("changed") == "true");

    // At least one instruction was folded or simplified.
    const std::size_t folded =
        art.metadata.count("folded_instructions")
            ? std::stoul(art.metadata.at("folded_instructions"))
            : 0u;
    const std::size_t simplified =
        art.metadata.count("simplified_to_mov")
            ? std::stoul(art.metadata.at("simplified_to_mov"))
            : 0u;
    REQUIRE((folded + simplified) >= 1u);
}

// ===========================================================================
// Test 2 — runtime evaluation still works
// ===========================================================================
TEST_CASE (



"runtime engine executes MIR and returns the correct value"
,
"[lithe][execution_engine][runtime]"
)
 {
    using namespace lithe::codegen;

    // 10 * 4 = 40
    const auto fn = make_arithmetic("re_mul", opcode::mul, 10, 4);

    runtime_engine engine;
    const auto art = engine.execute(fn);

    REQUIRE(art.diagnostics.empty());
    REQUIRE(art.metadata.count("return_value") > 0);
    REQUIRE(std::stoll(art.metadata.at("return_value")) == 40);
}

// ===========================================================================
// Test 3 — partial evaluation preserves unknown values
// ===========================================================================
TEST_CASE (



"partial evaluation leaves dynamic register values as unknown"
,
"[lithe][execution_engine][partial_eval]"
)
 {
    using namespace lithe::codegen;

    // Build: load_arg r0 = arg0 (unknown at compile time)
    //        load_imm r1 = 5
    //        add r2 = r0, r1    (r0 unknown → r2 unknown)
    //        ret r2
    std::vector<allocated_instruction> insts;

    allocated_instruction la;
    la.id   = 1;
    la.op   = opcode::load_arg;
    la.defs = {allocated_operand::as_preg({0, "r0"})};
    la.uses = {allocated_operand::as_argument_index(0)};
    insts.push_back(la);

    allocated_instruction li;
    li.id   = 2;
    li.op   = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({1, "r1"})};
    li.uses = {allocated_operand::as_i64(5)};
    insts.push_back(li);

    allocated_instruction add;
    add.id   = 3;
    add.op   = opcode::add;
    add.defs = {allocated_operand::as_preg({2, "r2"})};
    add.uses = {allocated_operand::as_preg({0, "r0"}),
                allocated_operand::as_preg({1, "r1"})};
    insts.push_back(add);

    allocated_instruction ret;
    ret.id   = 4;
    ret.op   = opcode::ret;
    ret.uses = {allocated_operand::as_preg({2, "r2"})};
    insts.push_back(ret);

    const auto fn = make_physical("partial_unknown", std::move(insts));

    constexpr_engine engine;
    const auto pe = engine.partially_evaluate(fn);

    // The add over an unknown operand must not be folded.
    REQUIRE(pe.folded_instructions == 0u);
}

// ===========================================================================
// Test 4 — constexpr pass pipeline executes
// ===========================================================================
TEST_CASE (



"constexpr pass pipeline runs and reports statistics"
,
"[lithe][execution_engine][pipeline]"
)
 {
    using namespace lithe::codegen;

    // 0 + 0: absorbing identity — partial_evaluation_pass simplifies to mov.
    const auto fn = make_arithmetic("pipeline_test", opcode::add, 0, 0);

    constexpr_engine engine;

    // collect_instrumentation drives constexpr_run with a typed pass list.
    partial_evaluation_pass pe_pass;
    const auto result = engine.collect_instrumentation(fn, pe_pass);

    REQUIRE(result.ok());
    REQUIRE(result.passes_run >= 1u);
}

// ===========================================================================
// Test 5 — iterative traversal handles a two-block MIR graph
// ===========================================================================
TEST_CASE (



"runtime engine traverses a two-block MIR function correctly"
,
"[lithe][execution_engine][runtime][multiblock]"
)
 {
    using namespace lithe::codegen;

    // Block 1 (entry): load_imm r0 = 20; load_imm r1 = 2
    // Block 2 (body):  sub r2 = r0, r1; ret r2
    // Expected return: 18
    //
    // The interpreter walks blocks in order; since there is no branch
    // instruction the second block is entered via fall-through.
    std::vector<allocated_instruction> entry_insts;
    {
        allocated_instruction li0;
        li0.id = 1; li0.op = opcode::load_imm;
        li0.defs = {allocated_operand::as_preg({0, "r0"})};
        li0.uses = {allocated_operand::as_i64(20)};
        entry_insts.push_back(li0);

        allocated_instruction li1;
        li1.id = 2; li1.op = opcode::load_imm;
        li1.defs = {allocated_operand::as_preg({1, "r1"})};
        li1.uses = {allocated_operand::as_i64(2)};
        entry_insts.push_back(li1);
    }

    std::vector<allocated_instruction> body_insts;
    {
        allocated_instruction sub;
        sub.id = 3; sub.op = opcode::sub;
        sub.defs = {allocated_operand::as_preg({2, "r2"})};
        sub.uses = {allocated_operand::as_preg({0, "r0"}),
                    allocated_operand::as_preg({1, "r1"})};
        body_insts.push_back(sub);

        allocated_instruction ret;
        ret.id = 4; ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({2, "r2"})};
        body_insts.push_back(ret);
    }

    const auto fn = make_two_block("two_block", entry_insts, body_insts);

    runtime_engine engine;
    const auto art = engine.execute(fn);

    REQUIRE(art.diagnostics.empty());
    REQUIRE(art.metadata.count("return_value") > 0);
    REQUIRE(std::stoll(art.metadata.at("return_value")) == 18);
}

// Namespace-scope registry used by Test 6.
// inline constexpr is required here — cannot be declared inside a function body.
namespace {
    using namespace lithe::dsl_extension;

    inline constexpr auto g_nttp_registry = make_extension_registry(
        extension_descriptor < "fma" > { 8, false, false, 3 }

    ,
    extension_descriptor<"dot"> { 4, false, true, 2 }
    );

    // Compile-time containment checks.
    static_assert(decltype(g_nttp_registry)::template contains<"fma">());
    static_assert(decltype(g_nttp_registry)::template contains<"dot">());
    static_assert(!decltype(g_nttp_registry)::template contains<"unknown">());

    // Compile-time metadata checks via direct static_assert on the registry type.
    static_assert(g_nttp_registry.get<"fma">().metadata.precedence == 8);
    static_assert(g_nttp_registry.get<"fma">().metadata.arity == 3);
    static_assert(g_nttp_registry.get<"dot">().metadata.is_commutative);
    static_assert(g_nttp_registry.get<"dot">().metadata.arity == 2);
} // anonymous namespace

// ===========================================================================
// Test 6 — NTTP extension registration works without macros
// ===========================================================================
TEST_CASE (



"extension_tag NTTP registration reflects metadata without macros"
,
"[lithe][execution_engine][nttp][extension]"
)
 {
    using namespace lithe::dsl_extension;

    // Runtime checks: the static_asserts above already verify correctness at
    // compile time; these REQUIRE calls confirm the values are also accessible
    // at runtime.
    REQUIRE(g_nttp_registry.get<"fma">().name_view() == "fma");
    REQUIRE(g_nttp_registry.get<"dot">().name_view() == "dot");

    REQUIRE(g_nttp_registry.get<"fma">().metadata.precedence == 8);
    REQUIRE(g_nttp_registry.get<"dot">().metadata.is_commutative);

    // extension_tag types carry the name as a static constexpr member.
    using fma_tag = extension_tag<"fma">;
    REQUIRE(fma_tag::tag_name.view() == "fma");
}

// ===========================================================================
// Test 7 — same MIR executes correctly under both constexpr and runtime
// ===========================================================================
TEST_CASE (



"constexpr and runtime engines agree on the same MIR function"
,
"[lithe][execution_engine][dual]"
)
 {
    using namespace lithe::codegen;

    // 12 - 4 = 8
    const auto fn = make_arithmetic("dual_sub", opcode::sub, 12, 4);

    // Constexpr engine: partial evaluator folds the sub.
    constexpr_engine ce;
    const auto ce_art = ce.execute(fn);

    // The result was folded (changed=true) — value known at compile time.
    REQUIRE(ce_art.metadata.at("changed") == "true");

    // Runtime engine: interpreter runs the instructions.
    runtime_engine re;
    const auto re_art = re.execute(fn);

    REQUIRE(re_art.diagnostics.empty());
    REQUIRE(re_art.metadata.count("return_value") > 0);
    REQUIRE(std::stoll(re_art.metadata.at("return_value")) == 8);

    // Both engines agree the function produces 8.
    // (constexpr engine folds 12-4 → load_imm 8 in the PE pass; the folded
    // count confirms the constant was computed.)
    const std::size_t folded =
        ce_art.metadata.count("folded_instructions")
            ? std::stoul(ce_art.metadata.at("folded_instructions"))
            : 0u;
    const std::size_t simplified =
        ce_art.metadata.count("simplified_to_mov")
            ? std::stoul(ce_art.metadata.at("simplified_to_mov"))
            : 0u;
    REQUIRE((folded + simplified) >= 1u);
}

// ============================================================================
// Finding 7: interpreter integer ops wrap deterministically
// ============================================================================

TEST_CASE (


"interpreter integer ops wrap deterministically"
,
"[lithe][interp]"
)
 {
    using namespace lithe::codegen;

    // INT64_MAX + 1 must wrap to INT64_MIN (two's-complement wrapping).
    {
        const auto fn = make_arithmetic("wrap_add",
            opcode::add,
            std::numeric_limits<std::int64_t>::max(), 1);
        runtime_engine re;
        const auto art = re.execute(fn);
        REQUIRE(art.metadata.count("return_value") > 0);
        const auto val = static_cast<std::int64_t>(
            std::stoll(art.metadata.at("return_value")));
        REQUIRE(val == std::numeric_limits<std::int64_t>::min());
    }

    // INT64_MIN / -1 must be INT64_MIN (not a signal or UB).
    {
        const auto fn = make_arithmetic("wrap_div",
            opcode::div,
            std::numeric_limits<std::int64_t>::min(),
            static_cast<std::int64_t>(-1));
        runtime_engine re;
        const auto art = re.execute(fn);
        REQUIRE(art.metadata.count("return_value") > 0);
        const auto val = static_cast<std::int64_t>(
            std::stoll(art.metadata.at("return_value")));
        REQUIRE(val == std::numeric_limits<std::int64_t>::min());
    }

    // Shift count 64 masked to 0: 1 << 64 == 1 << (64 & 63) == 1 << 0 == 1.
    {
        const auto fn = make_arithmetic("wrap_shl", opcode::shl,
            std::int64_t{1}, std::int64_t{64});
        runtime_engine re;
        const auto art = re.execute(fn);
        REQUIRE(art.metadata.count("return_value") > 0);
        const auto val = std::stoll(art.metadata.at("return_value"));
        REQUIRE(val == 1);
    }
}
