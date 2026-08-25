#include "catch_amalgamated.hpp"

#include "lithe/lithe_codegen_pipeline.hpp"

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers (minimal MIR builders matching the patterns in test_lithe_mir_cfg.cpp)
// ─────────────────────────────────────────────────────────────────────────────
namespace {
    using namespace lithe::codegen;
    using namespace lithe::poly;

    // ------------------------------------------------------------------
    // Instruction factories
    // ------------------------------------------------------------------

    allocated_instruction make_load_imm(std::uint32_t id, preg dst, std::int64_t val) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::load_imm;
        i.defs = {allocated_operand::as_preg(dst)};
        i.uses = {allocated_operand::as_i64(val)};
        return i;
    }

    allocated_instruction make_add_iv(std::uint32_t id, preg iv, std::int64_t step) {
        // iv = iv + step  (induction variable increment)
        allocated_instruction i;
        i.id = id;
        i.op = opcode::add;
        i.defs = {allocated_operand::as_preg(iv)};
        i.uses = {allocated_operand::as_preg(iv), allocated_operand::as_i64(step)};
        return i;
    }

    allocated_instruction make_sub_iv(std::uint32_t id, preg iv, std::int64_t step) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::sub;
        i.defs = {allocated_operand::as_preg(iv)};
        i.uses = {allocated_operand::as_preg(iv), allocated_operand::as_i64(step)};
        return i;
    }

    allocated_instruction make_cmp_lt(std::uint32_t id, preg lhs, std::int64_t rhs_imm) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::cmp_lt;
        i.uses = {allocated_operand::as_preg(lhs), allocated_operand::as_i64(rhs_imm)};
        return i;
    }

    allocated_instruction make_cmp_le(std::uint32_t id, preg lhs, std::int64_t rhs_imm) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::cmp_le;
        i.uses = {allocated_operand::as_preg(lhs), allocated_operand::as_i64(rhs_imm)};
        return i;
    }

    allocated_instruction make_branch_cond(std::uint32_t id, preg cond,
                                           std::uint32_t true_bb, std::uint32_t false_bb) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::branch_cond;
        i.uses = {
            allocated_operand::as_preg(cond),
            allocated_operand::as_block(true_bb),
            allocated_operand::as_block(false_bb)
        };
        return i;
    }

    allocated_instruction make_branch(std::uint32_t id, std::uint32_t target) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::branch;
        i.uses = {allocated_operand::as_block(target)};
        return i;
    }

    allocated_instruction make_ret(std::uint32_t id) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::ret;
        return i;
    }

    // ------------------------------------------------------------------
    // Block factory
    // ------------------------------------------------------------------

    allocated_basic_block make_block(
        std::uint32_t id,
        std::vector<allocated_instruction> instructions,
        std::vector<std::uint32_t> successors = {}) {
        allocated_basic_block b;
        b.id = id;
        b.name = "bb" + std::to_string(id);
        b.instructions = std::move(instructions);
        b.successors = std::move(successors);
        return b;
    }

    // ------------------------------------------------------------------
    // Physical IR factory
    // ------------------------------------------------------------------

    mir::physical_mir_function make_physical(
        std::vector<allocated_basic_block> blocks,
        std::uint32_t entry_block) {
        allocated_function_ir fn;
        fn.name = "poly_test";
        fn.blocks = std::move(blocks);
        fn.cfg.entry_block = entry_block;
        for (const auto& b : fn.blocks) {
            fn.cfg.successors[b.id] = b.successors;
            for (const auto s : b.successors)
                fn.cfg.predecessors[s].push_back(b.id);
        }
        mir::physical_mir_function out;
        out.function = std::move(fn);
        out.metadata.current_phase = mir::phase::physical_mir;
        return out;
    }

    // ------------------------------------------------------------------
    // Build a simple counted loop:
    //
    //   bb_pre  : i = load_imm lower
    //             branch -> bb_hdr
    //   bb_hdr  : i = i + 1          (IV increment)
    //             cmp_lt i, upper
    //             branch_cond cond -> bb_body, bb_exit
    //   bb_body : nop
    //             branch -> bb_hdr   (back edge)
    //   bb_exit : ret
    //
    // Blocks have contiguous ids starting at `base_id`.
    // Returns pre_id, hdr_id, body_id, exit_id in that order.
    // ------------------------------------------------------------------

    struct simple_loop_ids {
        std::uint32_t pre, hdr, body, exit;
    };

    std::pair<mir::physical_mir_function, simple_loop_ids>
    make_simple_loop(int lower, int upper,
                     std::uint32_t base_id = 1,
                     std::uint32_t base_instr = 100) {
        const preg iv{1, "i"};
        const preg cond{2, "cond"};

        const std::uint32_t pre_id = base_id;
        const std::uint32_t hdr_id = base_id + 1;
        const std::uint32_t body_id = base_id + 2;
        const std::uint32_t exit_id = base_id + 3;

        std::uint32_t iid = base_instr;
        auto next = [&] { return iid++; };

        auto bb_pre = make_block(pre_id, {
                                     make_load_imm(next(), iv, lower),
                                     make_branch(next(), hdr_id)
                                 },
                                 {hdr_id});
        auto bb_hdr = make_block(hdr_id, {
                                     make_add_iv(next(), iv, 1),
                                     make_cmp_lt(next(), iv, upper),
                                     make_branch_cond(next(), cond, body_id, exit_id)
                                 },
                                 {body_id, exit_id});
        auto bb_body = make_block(body_id, {make_branch(next(), hdr_id)}, {hdr_id});
        auto bb_exit = make_block(exit_id, {make_ret(next())});

        auto fn = make_physical({bb_pre, bb_hdr, bb_body, bb_exit}, pre_id);
        return {fn, {pre_id, hdr_id, body_id, exit_id}};
    }
} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. affine_matrix
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"affine_matrix: zero construction"
,
"[lithe][poly][matrix]"
)
{
    const auto m = affine_matrix::zero(3, 4);
    REQUIRE(m.rows == 3u);
    REQUIRE(m.cols == 4u);
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 4; ++c)
            REQUIRE(m.at(r, c) == 0);
}

TEST_CASE (



"affine_matrix: identity construction"
,
"[lithe][poly][matrix]"
)
{
    const auto m = affine_matrix::identity(3);
    REQUIRE(m.rows == 3u);
    REQUIRE(m.cols == 3u);
    REQUIRE(m.at(0, 0) == 1);
    REQUIRE(m.at(1, 1) == 1);
    REQUIRE(m.at(2, 2) == 1);
    REQUIRE(m.at(0, 1) == 0);
    REQUIRE(m.at(1, 0) == 0);
}

TEST_CASE (



"affine_matrix: at() read/write"
,
"[lithe][poly][matrix]"
)
{
    auto m = affine_matrix::zero(2, 3);
    m.at(0, 2) = 7;
    m.at(1, 0) = -3;
    REQUIRE(m.at(0, 2) ==  7);
    REQUIRE(m.at(1, 0) == -3);
    REQUIRE(m.at(0, 0) ==  0);
}

TEST_CASE (



"affine_matrix: mul produces correct result"
,
"[lithe][poly][matrix]"
)
{
    // P = [[0,1],[1,0]] (swap permutation)
    auto P = affine_matrix::zero(2, 2);
    P.at(0, 1) = 1;
    P.at(1, 0) = 1;

    const auto I = affine_matrix::identity(2);
    const auto PI = affine_matrix::mul(P, I);

    // P * I == P
    REQUIRE(PI.at(0, 0) == 0);
    REQUIRE(PI.at(0, 1) == 1);
    REQUIRE(PI.at(1, 0) == 1);
    REQUIRE(PI.at(1, 1) == 0);
}

TEST_CASE (



"affine_matrix: operator== works"
,
"[lithe][poly][matrix]"
)
{
    const auto a = affine_matrix::identity(2);
    const auto b = affine_matrix::identity(2);
    auto c = affine_matrix::identity(2);
    c.at(0, 1) = 5;

    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. loop_bounds defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"loop_bounds: default values and fully_known()"
,
"[lithe][poly][bounds]"
)
{
    loop_bounds b;
    REQUIRE(b.lower == 0);
    REQUIRE(b.upper == 0);
    REQUIRE(b.step  == 1);
    REQUIRE(b.step_known  == true);
    REQUIRE(b.lower_known == false);
    REQUIRE(b.upper_known == false);
    REQUIRE_FALSE(b.fully_known());

    b.lower_known = true;
    b.upper_known = true;
    REQUIRE(b.fully_known());
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. extract_polyhedral_pass: no loops
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"extract_polyhedral_pass: no loops → empty result"
,
"[lithe][poly][extract]"
)
{
    auto fn = make_physical({make_block(1, {make_ret(1)})}, 1);

    mir_pass_context ctx;
    const auto res = extract_polyhedral(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.loops.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. extract_polyhedral_pass: simple counted loop with known bounds
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"extract_polyhedral_pass: simple loop with known bounds"
,
"[lithe][poly][extract]"
)
{
    auto [fn, ids] = make_simple_loop(0, 10);

    mir_pass_context ctx;
    const auto res = extract_polyhedral(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.loops.size() == 1u);

    const auto &pl = res.loops[0];
    REQUIRE(pl.is_affine);
    REQUIRE(pl.ivars.size() == 1u);

    const auto &iv = pl.ivars[0];
    REQUIRE(iv.bounds.lower_known == true);
    REQUIRE(iv.bounds.upper_known == true);
    REQUIRE(iv.bounds.lower == 0);
    REQUIRE(iv.bounds.upper == 10);
    REQUIRE(iv.bounds.step  == 1);

    // Iteration matrix: 2 rows (lower/upper), 2 cols (1 IV + RHS).
    REQUIRE(pl.iteration.rows == 2u);
    REQUIRE(pl.iteration.cols == 2u);
    // Row 0: +1·i | -lower(0) → coefficient=1, rhs=0
    REQUIRE(pl.iteration.at(0, 0) ==  1);
    REQUIRE(pl.iteration.at(0, 1) ==  0);  // -lower = 0
    // Row 1: -1·i | upper-1(9) → coefficient=-1, rhs=9
    REQUIRE(pl.iteration.at(1, 0) == -1);
    REQUIRE(pl.iteration.at(1, 1) ==  9);

    // Schedule starts as identity.
    REQUIRE(pl.schedule == affine_matrix::identity(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. extract_polyhedral_pass: upper bound is a register (not immediate) → upper_known=false
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"extract_polyhedral_pass: non-constant upper bound → upper_known=false"
,
"[lithe][poly][extract]"
)
{
    const preg iv{1, "i"};
    const preg n{2, "n"};   // runtime bound
    const preg cond{3, "cond"};

    // cmp_lt uses a preg for the bound → upper not known.
    auto cmp = make_cmp_lt(105, iv, 0); // placeholder, will replace use
    cmp.uses = {allocated_operand::as_preg(iv), allocated_operand::as_preg(n)};

    auto bb_pre  = make_block(1, {make_load_imm(100, iv, 0), make_branch(101, 2)}, {2});
    auto bb_hdr  = make_block(2, {make_add_iv(102, iv, 1),
                                   cmp,
                                   make_branch_cond(106, cond, 3, 4)},
                              {3, 4});
    auto bb_body = make_block(3, {make_branch(107, 2)}, {2});
    auto bb_exit = make_block(4, {make_ret(108)});

    auto fn = make_physical({bb_pre, bb_hdr, bb_body, bb_exit}, 1);

    mir_pass_context ctx;
    const auto res = extract_polyhedral(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.loops.size() == 1u);

    const auto &pl = res.loops[0];
    REQUIRE(pl.ivars.size() == 1u);
    REQUIRE(pl.ivars[0].bounds.lower_known == true);
    REQUIRE(pl.ivars[0].bounds.upper_known == false);
    REQUIRE_FALSE(pl.is_affine);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. extract_polyhedral_pass: nested loop (two IVs)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"extract_polyhedral_pass: nested loop yields two IVs"
,
"[lithe][poly][extract]"
)
{
    // Outer loop: i from 0..4 (blocks 1–4)
    // Inner loop: j from 0..4 (blocks 5–8)
    // Structure (simplified — both share the same header block for this test):
    //   bb1 (pre-outer): load_imm i 0; branch bb2
    //   bb2 (hdr-outer): i=i+1; load_imm j 0; j=j+1; cmp_lt i 4; cmp_lt j 4; branch_cond -> bb3, bb4
    //   bb3 (body):      branch bb2 (back-edge)
    //   bb4 (exit):      ret
    //
    // Both IVs are in the header so extract_polyhedral_pass finds them both.

    const preg iv_i{1, "i"};
    const preg iv_j{2, "j"};
    const preg cond{3, "cond"};

    auto bb_pre = make_block(1,
        {make_load_imm(10, iv_i, 0),
         make_load_imm(11, iv_j, 0),
         make_branch(12, 2)},
        {2});

    // Header has both IVs incremented + both bounds tested.
    // We use cond for the outer cmp result and drive branching on it.
    auto bb_hdr = make_block(2,
        {make_add_iv(20, iv_i, 1),
         make_cmp_lt(21, iv_i, 4),
         make_add_iv(22, iv_j, 1),
         make_cmp_lt(23, iv_j, 4),
         make_branch_cond(24, cond, 3, 4)},
        {3, 4});

    auto bb_body = make_block(3, {make_branch(30, 2)}, {2});
    auto bb_exit = make_block(4, {make_ret(31)});

    auto fn = make_physical({bb_pre, bb_hdr, bb_body, bb_exit}, 1);

    mir_pass_context ctx;
    const auto res = extract_polyhedral(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.loops.size() == 1u);

    const auto &pl = res.loops[0];
    REQUIRE(pl.ivars.size() == 2u);
    REQUIRE(pl.ivars[0].bounds.lower_known);
    REQUIRE(pl.ivars[0].bounds.upper == 4);
    REQUIRE(pl.ivars[1].bounds.lower_known);
    REQUIRE(pl.ivars[1].bounds.upper == 4);
    REQUIRE(pl.is_affine);
    // Schedule is 2×2 identity.
    REQUIRE(pl.schedule == affine_matrix::identity(2));
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. loop_fusion_pass: adjacent loops with identical bounds are fused
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"loop_fusion_pass: adjacent identical-bound loops are fused"
,
"[lithe][poly][fusion]"
)
{
    // Two loops, each 0..8:
    //   bb1(pre-A): load_imm i 0; branch bb2
    //   bb2(hdr-A): i=i+1; cmp_lt i 8; branch_cond bb3 bb4
    //   bb3(body-A): branch bb2
    //   bb4(pre-B == exit-A): load_imm j 0; branch bb5
    //   bb5(hdr-B): j=j+1; cmp_lt j 8; branch_cond bb6 bb7
    //   bb6(body-B): branch bb5
    //   bb7(exit-B): ret

    const preg i{1, "i"}, j{2, "j"}, cond{3, "cond"};

    auto bb1 = make_block(1, {make_load_imm(10, i, 0), make_branch(11, 2)}, {2});
    auto bb2 = make_block(2, {make_add_iv(20, i, 1), make_cmp_lt(21, i, 8),
                               make_branch_cond(22, cond, 3, 4)}, {3, 4});
    auto bb3 = make_block(3, {make_branch(30, 2)}, {2});
    // bb4 is both exit of A and pre-entry of B
    auto bb4 = make_block(4, {make_load_imm(40, j, 0), make_branch(41, 5)}, {5});
    auto bb5 = make_block(5, {make_add_iv(50, j, 1), make_cmp_lt(51, j, 8),
                               make_branch_cond(52, cond, 6, 7)}, {6, 7});
    auto bb6 = make_block(6, {make_branch(60, 5)}, {5});
    auto bb7 = make_block(7, {make_ret(70)});

    auto fn = make_physical({bb1, bb2, bb3, bb4, bb5, bb6, bb7}, 1);

    mir_pass_context ctx;
    auto poly = extract_polyhedral(fn, ctx);

    REQUIRE(poly.loops.size() == 2u);
    REQUIRE(poly.loops[0].ivars[0].bounds.lower == 0);
    REQUIRE(poly.loops[0].ivars[0].bounds.upper == 8);
    REQUIRE(poly.loops[1].ivars[0].bounds.lower == 0);
    REQUIRE(poly.loops[1].ivars[0].bounds.upper == 8);

    const auto res = apply_loop_fusion(fn, poly, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.fused_pairs == 1u);
    // After fusion the analysis should have one fewer loop entry.
    REQUIRE(res.analysis.loops.size() == 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. loop_fusion_pass: mismatched bounds → no fusion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"loop_fusion_pass: mismatched upper bounds → loops not fused"
,
"[lithe][poly][fusion]"
)
{
    const preg i{1, "i"}, j{2, "j"}, cond{3, "cond"};

    // Loop A: 0..4, Loop B: 0..8
    auto bb1 = make_block(1, {make_load_imm(10, i, 0), make_branch(11, 2)}, {2});
    auto bb2 = make_block(2, {make_add_iv(20, i, 1), make_cmp_lt(21, i, 4),
                               make_branch_cond(22, cond, 3, 4)}, {3, 4});
    auto bb3 = make_block(3, {make_branch(30, 2)}, {2});
    auto bb4 = make_block(4, {make_load_imm(40, j, 0), make_branch(41, 5)}, {5});
    auto bb5 = make_block(5, {make_add_iv(50, j, 1), make_cmp_lt(51, j, 8),
                               make_branch_cond(52, cond, 6, 7)}, {6, 7});
    auto bb6 = make_block(6, {make_branch(60, 5)}, {5});
    auto bb7 = make_block(7, {make_ret(70)});

    auto fn = make_physical({bb1, bb2, bb3, bb4, bb5, bb6, bb7}, 1);

    mir_pass_context ctx;
    auto poly = extract_polyhedral(fn, ctx);

    const auto res = apply_loop_fusion(fn, poly, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.fused_pairs == 0u);
    REQUIRE(res.analysis.loops.size() == 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. loop_fusion_pass: non-adjacent loops (basic block between them) → no fusion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"loop_fusion_pass: non-adjacent loops (block between) → no fusion"
,
"[lithe][poly][fusion]"
)
{
    const preg i{1, "i"}, j{2, "j"}, cond{3, "cond"};

    // A's exit (bb4) goes to a separator (bb99) that has TWO successors so it
    // cannot be a trivial pre-header.  This breaks the unique-predecessor
    // single-successor requirement and prevents fusion.
    //   bb4 (exit of A): branch bb99
    //   bb99 (separator, two successors): branch_cond -> bb5 or bb100
    //   bb100 (dead exit): ret
    //   bb5 (hdr-B): j=j+1; cmp_lt j 6; branch_cond bb6 bb7
    auto bb1  = make_block(1,  {make_load_imm(10, i, 0), make_branch(11, 2)}, {2});
    auto bb2  = make_block(2,  {make_add_iv(20, i, 1), make_cmp_lt(21, i, 6),
                                 make_branch_cond(22, cond, 3, 4)}, {3, 4});
    auto bb3  = make_block(3,  {make_branch(30, 2)}, {2});
    // bb4 exits to a multi-successor separator, NOT directly to B's header.
    auto bb4  = make_block(4,  {make_branch_cond(40, cond, 99, 100)}, {99, 100});
    auto bb99 = make_block(99, {make_load_imm(990, j, 0), make_branch(991, 5)}, {5});
    auto bb100= make_block(100,{make_ret(1000)});
    auto bb5  = make_block(5,  {make_add_iv(50, j, 1), make_cmp_lt(51, j, 6),
                                 make_branch_cond(52, cond, 6, 7)}, {6, 7});
    auto bb6  = make_block(6,  {make_branch(60, 5)}, {5});
    auto bb7  = make_block(7,  {make_ret(70)});

    auto fn = make_physical({bb1, bb2, bb3, bb4, bb99, bb100, bb5, bb6, bb7}, 1);

    mir_pass_context ctx;
    auto poly = extract_polyhedral(fn, ctx);

    const auto res = apply_loop_fusion(fn, poly, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.fused_pairs == 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. loop_interchange_pass: schedule matrix updated for 2-IV affine loop
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"loop_interchange_pass: schedule matrix swaps outer and inner IV"
,
"[lithe][poly][interchange]"
)
{
    const preg iv_i{1, "i"}, iv_j{2, "j"}, cond{3, "cond"};

    // Same 2-IV header as test case 6.
    auto bb_pre = make_block(1,
        {make_load_imm(10, iv_i, 0), make_load_imm(11, iv_j, 0), make_branch(12, 2)}, {2});
    auto bb_hdr = make_block(2,
        {make_add_iv(20, iv_i, 1), make_cmp_lt(21, iv_i, 4),
         make_add_iv(22, iv_j, 1), make_cmp_lt(23, iv_j, 4),
         make_branch_cond(24, cond, 3, 4)},
        {3, 4});
    auto bb_body = make_block(3, {make_branch(30, 2)}, {2});
    auto bb_exit = make_block(4, {make_ret(31)});

    auto fn = make_physical({bb_pre, bb_hdr, bb_body, bb_exit}, 1);

    mir_pass_context ctx;
    auto poly = extract_polyhedral(fn, ctx);

    REQUIRE(poly.loops.size() == 1u);
    REQUIRE(poly.loops[0].is_affine);

    const auto res = apply_loop_interchange(fn, poly, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.interchanged == 1u);

    // After interchange the schedule should be the permutation matrix [[0,1],[1,0]].
    const auto &sched = res.analysis.loops[0].schedule;
    REQUIRE(sched.rows == 2u);
    REQUIRE(sched.cols == 2u);
    REQUIRE(sched.at(0, 0) == 0);
    REQUIRE(sched.at(0, 1) == 1);
    REQUIRE(sched.at(1, 0) == 1);
    REQUIRE(sched.at(1, 1) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. loop_interchange_pass: non-affine loop is left unchanged
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"loop_interchange_pass: non-affine loop not interchanged"
,
"[lithe][poly][interchange]"
)
{
    // Build a loop where the upper bound is non-constant (upper_known=false).
    const preg iv{1, "i"}, n{2, "n"}, cond{3, "cond"};

    auto cmp = make_cmp_lt(105, iv, 0);
    cmp.uses = {allocated_operand::as_preg(iv), allocated_operand::as_preg(n)};

    auto bb_pre  = make_block(1, {make_load_imm(100, iv, 0), make_branch(101, 2)}, {2});
    auto bb_hdr  = make_block(2, {make_add_iv(102, iv, 1), cmp,
                                   make_branch_cond(106, cond, 3, 4)}, {3, 4});
    auto bb_body = make_block(3, {make_branch(107, 2)}, {2});
    auto bb_exit = make_block(4, {make_ret(108)});

    auto fn = make_physical({bb_pre, bb_hdr, bb_body, bb_exit}, 1);

    mir_pass_context ctx;
    auto poly = extract_polyhedral(fn, ctx);

    // is_affine should be false due to unknown upper bound.
    REQUIRE_FALSE(poly.loops.empty());
    REQUIRE_FALSE(poly.loops[0].is_affine);

    const auto res = apply_loop_interchange(fn, poly, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.interchanged == 0u);
    // Schedule stays identity (1-IV loop, never touched).
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. Convenience API: extract_polyhedral uses cached loop analysis
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"extract_polyhedral convenience API: uses mir_pass_context cache"
,
"[lithe][poly][api]"
)
{
    auto [fn, ids] = make_simple_loop(1, 5);

    mir_pass_context ctx;

    // First call populates the cache.
    const auto res1 = extract_polyhedral(fn, ctx);
    REQUIRE(res1.ok());
    REQUIRE(res1.loops.size() == 1u);
    // Loop cache should now be populated.
    REQUIRE(ctx.analysis_cache.loops.has_value());

    // Second call must return the same result without re-running analyze_loops.
    const auto res2 = extract_polyhedral(fn, ctx);
    REQUIRE(res2.loops.size() == 1u);
    REQUIRE(res2.loops[0].ivars[0].bounds.lower == 1);
    REQUIRE(res2.loops[0].ivars[0].bounds.upper == 5);
}

// ─────────────────────────────────────────────────────────────────────────────
// 13. extract_polyhedral_from_hl: forward path from structured_for attr
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"extract_polyhedral_from_hl: rank-1 structured_for produces exact affine_matrix"
,
"[lithe][poly][hl][forward]"
)
{
    using namespace lithe::codegen::hl;

    hl_mir_function fn{};
    fn.name = "test_hl_poly_rank1";

    // Body region: one block with one structured_for op (lower=0, upper=10, step=1).
    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto* for_op = fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank = 1;
    sf.bounds[0] = { 0, 10, 1, true, true, true };
    for_op->attr = sf;

    // Attach empty body region.
    auto* body_region = fn.make_region();
    auto body_rspan = fn.alloc_span<hl_region*>(1);
    body_rspan[0] = body_region;
    for_op->regions = body_rspan;
    body_region->parent_op = for_op;

    blk->ops.push_back(for_op);

    extract_polyhedral_from_hl pass;
    const auto result = pass.run(fn);

    REQUIRE(result.ok());
    REQUIRE(result.loops.size() == 1u);

    const auto& pl = result.loops[0];
    REQUIRE(pl.is_affine);
    REQUIRE(pl.ivars.size() == 1u);
    REQUIRE(pl.ivars[0].bounds.lower == 0);
    REQUIRE(pl.ivars[0].bounds.upper == 10);
    REQUIRE(pl.ivars[0].bounds.step  == 1);

    // iteration matrix: 2×2 [ [1, 0], [-1, 9] ] for v ≥ 0 and v < 10.
    REQUIRE(pl.iteration.rows == 2u);
    REQUIRE(pl.iteration.cols == 2u);
    REQUIRE(pl.iteration.at(0, 0) ==  1);  // v ≥ 0  → row0 col0
    REQUIRE(pl.iteration.at(0, 1) ==  0);  // RHS = -lower = 0
    REQUIRE(pl.iteration.at(1, 0) == -1);  // v < 10 → row1 col0
    REQUIRE(pl.iteration.at(1, 1) ==  9);  // RHS = upper-1 = 9
}

TEST_CASE (



"extract_polyhedral_from_hl: rank-2 nest matches expected constraint rows"
,
"[lithe][poly][hl][forward]"
)
{
    using namespace lithe::codegen::hl;

    hl_mir_function fn{};
    fn.name = "test_hl_poly_rank2";

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto* for_op = fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank = 2;
    sf.bounds[0] = { 0, 8, 1, true, true, true };  // outer: 0..8
    sf.bounds[1] = { 0, 16, 1, true, true, true }; // inner: 0..16
    for_op->attr = sf;

    auto* body_region = fn.make_region();
    auto body_rspan = fn.alloc_span<hl_region*>(1);
    body_rspan[0] = body_region;
    for_op->regions = body_rspan;
    body_region->parent_op = for_op;

    blk->ops.push_back(for_op);

    extract_polyhedral_from_hl pass;
    const auto result = pass.run(fn);

    REQUIRE(result.ok());
    REQUIRE(result.loops.size() == 1u);

    const auto& pl = result.loops[0];
    REQUIRE(pl.is_affine);
    REQUIRE(pl.ivars.size() == 2u);
    // iteration matrix: 4 rows × 3 cols (2 IVs + RHS)
    REQUIRE(pl.iteration.rows == 4u);
    REQUIRE(pl.iteration.cols == 3u);
    // Outer dim (row 0,1):
    REQUIRE(pl.iteration.at(0, 0) ==  1);  // i ≥ 0
    REQUIRE(pl.iteration.at(1, 0) == -1);  // i < 8  → RHS = 7
    REQUIRE(pl.iteration.at(1, 2) ==  7);
    // Inner dim (row 2,3):
    REQUIRE(pl.iteration.at(2, 1) ==  1);  // j ≥ 0
    REQUIRE(pl.iteration.at(3, 1) == -1);  // j < 16 → RHS = 15
    REQUIRE(pl.iteration.at(3, 2) == 15);
}

TEST_CASE (



"extract_polyhedral_from_hl vs bottom-up: equal matrices for equivalent rank-1 loop"
,
"[lithe][poly][hl][cross-check]"
)
{
    // Build equivalent flat MIR and HL MIR; assert identical affine_matrix.
    using namespace lithe::codegen;
    using namespace lithe::codegen::hl;

    // ── Flat path ──────────────────────────────────────────────────────────
    const preg iv{1, "i"}, cond{2, "c"};
    auto bb_pre  = make_block(1,
        {make_load_imm(100, iv, 0), make_branch(101, 2)}, {2});
    auto bb_hdr  = make_block(2,
        {make_add_iv(102, iv, 1),
         make_cmp_lt(103, iv, 10),
         make_branch_cond(104, cond, 3, 4)}, {3, 4});
    auto bb_body = make_block(3,
        {make_branch(105, 2)}, {2});
    auto bb_exit = make_block(4, {make_ret(106)});

    auto flat_fn = make_physical({bb_pre, bb_hdr, bb_body, bb_exit}, 1);
    mir_pass_context ctx;
    const auto flat_result = extract_polyhedral(flat_fn, ctx);
    REQUIRE_FALSE(flat_result.loops.empty());
    const auto& flat_pl = flat_result.loops[0];

    // ── HL path ────────────────────────────────────────────────────────────
    hl_mir_function hl_fn{};
    auto* blk = hl_fn.make_block();
    hl_fn.body_region.blocks.push_back(blk);
    blk->parent_region = &hl_fn.body_region;

    auto* for_op = hl_fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank = 1;
    sf.bounds[0] = { 0, 10, 1, true, true, true };
    for_op->attr = sf;

    auto* body_region = hl_fn.make_region();
    auto body_rspan = hl_fn.alloc_span<hl_region*>(1);
    body_rspan[0] = body_region;
    for_op->regions = body_rspan;
    blk->ops.push_back(for_op);

    extract_polyhedral_from_hl hl_pass;
    const auto hl_result = hl_pass.run(hl_fn);
    REQUIRE_FALSE(hl_result.loops.empty());
    const auto& hl_pl = hl_result.loops[0];

    // Both paths must agree on iteration matrix and IV bounds.
    REQUIRE(hl_pl.is_affine == flat_pl.is_affine);
    REQUIRE(hl_pl.iteration == flat_pl.iteration);
    REQUIRE(hl_pl.ivars[0].bounds.lower == flat_pl.ivars[0].bounds.lower);
    REQUIRE(hl_pl.ivars[0].bounds.upper == flat_pl.ivars[0].bounds.upper);
    REQUIRE(hl_pl.ivars[0].bounds.step  == flat_pl.ivars[0].bounds.step);
}
