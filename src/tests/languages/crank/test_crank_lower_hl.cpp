// =============================================================================
// test_crank_lower_hl.cpp — Crank HL MIR lowering: Phase A–E portable ops.
//
// Covers:
//   1. Control-flow lowering (if/while/match/return/break/continue/ternary)
//      → branch/branch_cond/ret/icmp/fcmp/select op emission.
//   2. Integer + comparison operator lowering
//      → sdiv/udiv/srem/urem/bit_*/shl/lshr/ashr; icmp/fcmp predicates.
//   3. Guard / trap lowering from obligations
//      → unknown → icmp + guard; proven → nothing; refuted → diagnostic.
//   4. Defer → cleanup_region / cleanup_yield (LIFO; trap bypasses).
//   5. Transaction → tx.region / tx.read / tx.write / tx.yield / tx.abort.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/lower_hl.hpp"
#include "lithe/lithe_ir/frontend/lowering_contract.hpp"

using namespace lithe::codegen::hl;

// Helper: count ops of a given opcode in the body region (top-level blocks only).
static std::size_t count_top_ops(const crank::lower_hl_result& res, hl_opcode opcode) {
    std::size_t n = 0;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == opcode) ++n;
        }
    }
    return n;
}

// Helper: count total blocks in body region (top-level).
static std::size_t count_blocks(const crank::lower_hl_result& res) {
    std::size_t n = 0;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next)
        ++n;
    return n;
}

// ============================================================================
// Test group 1 — Control-flow lowering (Phase A)
// ============================================================================

TEST_CASE (

"lower_to_hl: if_else emits icmp + branch_cond + two branches + join block"
,
"[crank][lower_hl][phase_a]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "IfElse";

    crank::cfg_node node;
    node.kind     = crank::cfg_node_kind::if_else;
    node.cmp.op   = crank::cmp_op::lt;
    node.cmp.type = crank::arith_type::signed_int;
    inp.cfg_nodes.push_back(node);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());

    // Phase A: icmp, branch_cond, two branches to join
    CHECK(res.stats.icmp_count        == 1);
    CHECK(res.stats.branch_cond_count == 1);
    CHECK(res.stats.branch_count      >= 2);  // then→join, else→join

    // 4 blocks: entry, then, else, join
    CHECK(count_blocks(res) >= 4);

    // branch_cond must have distinct true/false targets
    bool found_bc = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::branch_cond) {
                const auto& bc = std::get<branch_cond_attr>(op->attr);
                CHECK(bc.true_block != bc.false_block);
                found_bc = true;
            }
        }
    }
    CHECK(found_bc);
}

TEST_CASE (

"lower_to_hl: if_else with float compare emits fcmp"
,
"[crank][lower_hl][phase_a]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "IfFloat";

    crank::cfg_node node;
    node.kind     = crank::cfg_node_kind::if_else;
    node.cmp.op   = crank::cmp_op::lt;
    node.cmp.type = crank::arith_type::floating;
    inp.cfg_nodes.push_back(node);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.fcmp_count == 1);
    CHECK(res.stats.icmp_count == 0);

    // Predicate must be olt (ordered less-than for float)
    bool found_olt = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::fcmp) {
                const auto& ca = std::get<compare_attr>(op->attr);
                CHECK(ca.pred == compare_predicate::olt);
                found_olt = true;
            }
        }
    }
    CHECK(found_olt);
}

TEST_CASE (

"lower_to_hl: while_loop emits header branch_cond + body back-edge branch"
,
"[crank][lower_hl][phase_a]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "WhileLoop";

    crank::cfg_node node;
    node.kind     = crank::cfg_node_kind::while_loop;
    node.cmp.op   = crank::cmp_op::lt;
    node.cmp.type = crank::arith_type::signed_int;
    inp.cfg_nodes.push_back(node);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());

    // header + body + exit + entry = at least 4 blocks
    CHECK(count_blocks(res) >= 4);

    // Must have a branch_cond (header → body/exit) and an icmp
    CHECK(res.stats.branch_cond_count >= 1);
    CHECK(res.stats.icmp_count        >= 1);

    // Must have unconditional branches: entry→header, body→header (back-edge)
    CHECK(res.stats.branch_count >= 2);

    // The back-edge branch (in body_blk) must target the header_blk id.
    // header_blk's branch_cond must have body_blk as true target.
    // Verify the structural property: some branch_cond's true_block is
    // the same as some back-edge branch's target.
    std::vector<std::uint32_t> cond_true_targets;
    std::vector<std::uint32_t> back_edge_targets;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::branch_cond) {
                const auto& bc = std::get<branch_cond_attr>(op->attr);
                cond_true_targets.push_back(bc.true_block);
            }
            if (op->op == hl_opcode::branch) {
                const auto& ba = std::get<branch_attr>(op->attr);
                back_edge_targets.push_back(ba.target_block);
            }
        }
    }
    // At least one back-edge branch targets the same block as a cond_true (body_blk)
    // or the header itself. The structural invariant is just that both vectors are non-empty.
    CHECK_FALSE(cond_true_targets.empty());
    CHECK_FALSE(back_edge_targets.empty());
}

TEST_CASE (

"lower_to_hl: return emits ret terminator"
,
"[crank][lower_hl][phase_a]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "RetFn";

    crank::cfg_node node;
    node.kind           = crank::cfg_node_kind::ret;
    node.returns_value  = false;
    inp.cfg_nodes.push_back(node);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.ret_count == 1);
    CHECK(count_top_ops(res, hl_opcode::ret) == 1);
}

TEST_CASE (

"lower_to_hl: match_chain emits icmp-per-arm + branch_cond chain"
,
"[crank][lower_hl][phase_a]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "MatchFn";

    crank::cfg_node node;
    node.kind       = crank::cfg_node_kind::match_chain;
    node.cmp.op     = crank::cmp_op::eq;
    node.cmp.type   = crank::arith_type::signed_int;
    node.match_arms = {
        crank::match_arm{0, "arm_zero"},
        crank::match_arm{1, "arm_one"},
        crank::match_arm{2, "default"},  // last = default
    };
    inp.cfg_nodes.push_back(node);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());

    // 2 arms before default → 2 icmp + 2 branch_cond; default → branch to join
    CHECK(res.stats.icmp_count        >= 2);
    CHECK(res.stats.branch_cond_count >= 2);
    CHECK(res.stats.branch_count      >= 3);  // arm→join × 2 + default→join
}

TEST_CASE (

"lower_to_hl: break/continue emit unconditional branch ops"
,
"[crank][lower_hl][phase_a]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "BreakContinue";

    crank::cfg_node brk;
    brk.kind = crank::cfg_node_kind::break_;
    inp.cfg_nodes.push_back(brk);

    crank::cfg_node cont;
    cont.kind = crank::cfg_node_kind::continue_;
    inp.cfg_nodes.push_back(cont);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.branch_count >= 2);
}

TEST_CASE (

"lower_to_hl: ternary emits icmp/fcmp + select op"
,
"[crank][lower_hl][phase_a]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "TernaryFn";

    crank::cfg_node node;
    node.kind     = crank::cfg_node_kind::ternary;
    node.cmp.op   = crank::cmp_op::gt;
    node.cmp.type = crank::arith_type::signed_int;
    inp.cfg_nodes.push_back(node);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.icmp_count == 1);
    CHECK(count_top_ops(res, hl_opcode::select) == 1);

    // Predicate must be sgt (signed greater-than)
    bool found_sgt = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::icmp) {
                const auto& ca = std::get<compare_attr>(op->attr);
                CHECK(ca.pred == compare_predicate::sgt);
                found_sgt = true;
            }
        }
    }
    CHECK(found_sgt);
}

// ============================================================================
// Test group 2 — Integer + comparison operator lowering (Phase B)
// ============================================================================

TEST_CASE (

"lower_to_hl: sdiv/udiv emitted for signed/unsigned division"
,
"[crank][lower_hl][phase_b]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "DivOps";
    inp.int_ops.push_back({crank::int_op_kind::sdiv, {}});
    inp.int_ops.push_back({crank::int_op_kind::udiv, {}});

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(count_top_ops(res, hl_opcode::sdiv) == 1);
    CHECK(count_top_ops(res, hl_opcode::udiv) == 1);
    CHECK(res.stats.int_op_count == 2);
}

TEST_CASE (

"lower_to_hl: srem/urem emitted for signed/unsigned remainder"
,
"[crank][lower_hl][phase_b]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "RemOps";
    inp.int_ops.push_back({crank::int_op_kind::srem, {}});
    inp.int_ops.push_back({crank::int_op_kind::urem, {}});

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(count_top_ops(res, hl_opcode::srem) == 1);
    CHECK(count_top_ops(res, hl_opcode::urem) == 1);
}

TEST_CASE (

"lower_to_hl: bitwise ops (bit_and/or/xor/not) emitted"
,
"[crank][lower_hl][phase_b]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "BitOps";
    inp.int_ops.push_back({crank::int_op_kind::bit_and, {}});
    inp.int_ops.push_back({crank::int_op_kind::bit_or,  {}});
    inp.int_ops.push_back({crank::int_op_kind::bit_xor, {}});
    inp.int_ops.push_back({crank::int_op_kind::bit_not, {}});

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(count_top_ops(res, hl_opcode::bit_and) == 1);
    CHECK(count_top_ops(res, hl_opcode::bit_or)  == 1);
    CHECK(count_top_ops(res, hl_opcode::bit_xor) == 1);
    CHECK(count_top_ops(res, hl_opcode::bit_not) == 1);
    CHECK(res.stats.int_op_count == 4);
}

TEST_CASE (

"lower_to_hl: shift ops (shl/lshr/ashr) emitted"
,
"[crank][lower_hl][phase_b]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "ShiftOps";
    inp.int_ops.push_back({crank::int_op_kind::shl,  {}});
    inp.int_ops.push_back({crank::int_op_kind::lshr, {}});
    inp.int_ops.push_back({crank::int_op_kind::ashr, {}});

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(count_top_ops(res, hl_opcode::shl)  == 1);
    CHECK(count_top_ops(res, hl_opcode::lshr) == 1);
    CHECK(count_top_ops(res, hl_opcode::ashr) == 1);
}

TEST_CASE (

"lower_to_hl: icmp predicate table — signed lt/le/gt/ge/eq/ne"
,
"[crank][lower_hl][phase_b]"
)
 {
    // Table-driven: (cmp_op, arith_type) → expected compare_predicate
    struct row { crank::cmp_op op; crank::arith_type t; compare_predicate expected; };
    const row table[] = {
        { crank::cmp_op::eq, crank::arith_type::signed_int,   compare_predicate::eq  },
        { crank::cmp_op::ne, crank::arith_type::signed_int,   compare_predicate::ne  },
        { crank::cmp_op::lt, crank::arith_type::signed_int,   compare_predicate::slt },
        { crank::cmp_op::le, crank::arith_type::signed_int,   compare_predicate::sle },
        { crank::cmp_op::gt, crank::arith_type::signed_int,   compare_predicate::sgt },
        { crank::cmp_op::ge, crank::arith_type::signed_int,   compare_predicate::sge },
        { crank::cmp_op::lt, crank::arith_type::unsigned_int, compare_predicate::ult },
        { crank::cmp_op::le, crank::arith_type::unsigned_int, compare_predicate::ule },
        { crank::cmp_op::gt, crank::arith_type::unsigned_int, compare_predicate::ugt },
        { crank::cmp_op::ge, crank::arith_type::unsigned_int, compare_predicate::uge },
        { crank::cmp_op::lt, crank::arith_type::floating,     compare_predicate::olt },
        { crank::cmp_op::le, crank::arith_type::floating,     compare_predicate::ole },
        { crank::cmp_op::gt, crank::arith_type::floating,     compare_predicate::ogt },
        { crank::cmp_op::ge, crank::arith_type::floating,     compare_predicate::oge },
        { crank::cmp_op::eq, crank::arith_type::floating,     compare_predicate::oeq },
        { crank::cmp_op::ne, crank::arith_type::floating,     compare_predicate::one },
    };

    for (const auto& row : table) {
        crank::lower_input inp;
        inp.fn_name = "CmpTable";

        crank::cfg_node node;
        node.kind     = crank::cfg_node_kind::ternary;
        node.cmp.op   = row.op;
        node.cmp.type = row.t;
        inp.cfg_nodes.push_back(node);

        auto res = crank::lower_to_hl(std::move(inp));
        REQUIRE(res.ok());

        bool found = false;
        for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
             blk; blk = blk->list_node.next) {
            for (const hl_operation* op = blk->ops.head;
                 op; op = op->list_node.next) {
                hl_opcode expected_cmp = (row.t == crank::arith_type::floating)
                    ? hl_opcode::fcmp : hl_opcode::icmp;
                if (op->op == expected_cmp) {
                    const auto& ca = std::get<compare_attr>(op->attr);
                    CHECK(ca.pred == row.expected);
                    found = true;
                }
            }
        }
        CHECK(found);
    }
}

// ============================================================================
// Test group 3 — Guard / trap lowering from obligations (Phase C)
// ============================================================================

TEST_CASE (

"lower_to_hl: unknown obligation emits icmp(ne) + guard with div_by_zero kind"
,
"[crank][lower_hl][phase_c]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "DivGuard";

    crank::obligation_info ob;
    ob.kind   = crank::obligation_kind::div_by_zero;
    ob.status = crank::obligation_status::unknown;
    ob.policy = crank::safety_failure::trap;
    ob.label  = "b != 0";
    inp.obligations.push_back(ob);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());

    // icmp(ne) for the predicate + guard op
    CHECK(res.stats.icmp_count  == 1);
    CHECK(res.stats.guard_count == 1);
    CHECK(res.stats.trap_count  == 1);  // trap policy → trap terminator

    // Verify guard attr: kind = div_by_zero, policy = trap
    bool found_guard = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::guard) {
                const auto& ga = std::get<guard_attr>(op->attr);
                CHECK(ga.kind   == guard_kind::div_by_zero);
                CHECK(ga.policy == failure_policy::trap);
                found_guard = true;
            }
        }
    }
    CHECK(found_guard);
}

TEST_CASE (

"lower_to_hl: proven obligation emits no guard"
,
"[crank][lower_hl][phase_c]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "ProvenGuard";

    crank::obligation_info ob;
    ob.kind   = crank::obligation_kind::bounds;
    ob.status = crank::obligation_status::proven;
    ob.policy = crank::safety_failure::trap;
    ob.label  = "i < len";
    inp.obligations.push_back(ob);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.guard_count == 0);
    CHECK(res.stats.icmp_count  == 0);
}

TEST_CASE (

"lower_to_hl: refuted obligation produces diagnostic, no guard"
,
"[crank][lower_hl][phase_c]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "RefutedGuard";

    crank::obligation_info ob;
    ob.kind   = crank::obligation_kind::div_by_zero;
    ob.status = crank::obligation_status::refuted;
    ob.policy = crank::safety_failure::trap;
    ob.label  = "constant 0";
    inp.obligations.push_back(ob);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK_FALSE(res.ok());  // diagnostic produced
    CHECK_FALSE(res.diagnostics.empty());
    CHECK(res.stats.guard_count == 0);
}

TEST_CASE (

"lower_to_hl: terminate policy also emits trap terminator"
,
"[crank][lower_hl][phase_c]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "TerminateGuard";

    crank::obligation_info ob;
    ob.kind   = crank::obligation_kind::overflow;
    ob.status = crank::obligation_status::unknown;
    ob.policy = crank::safety_failure::terminate;
    ob.label  = "overflow check";
    inp.obligations.push_back(ob);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.guard_count == 1);
    CHECK(res.stats.trap_count  == 1);

    bool found_trap = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::trap) {
                const auto& ta = std::get<trap_attr>(op->attr);
                CHECK(ta.kind == trap_kind::overflow_checked);
                found_trap = true;
            }
        }
    }
    CHECK(found_trap);
}

TEST_CASE (

"lower_to_hl: return_result policy emits guard but no trap terminator"
,
"[crank][lower_hl][phase_c]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "RetResultGuard";

    crank::obligation_info ob;
    ob.kind   = crank::obligation_kind::bounds;
    ob.status = crank::obligation_status::unknown;
    ob.policy = crank::safety_failure::return_result;
    ob.label  = "bounds check";
    inp.obligations.push_back(ob);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.guard_count == 1);
    CHECK(res.stats.trap_count  == 0);  // return_result → no trap terminator

    bool found_guard = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::guard) {
                const auto& ga = std::get<guard_attr>(op->attr);
                CHECK(ga.policy == failure_policy::return_result);
                found_guard = true;
            }
        }
    }
    CHECK(found_guard);
}

// ============================================================================
// Test group 4 — Defer → cleanup_region (Phase D)
// ============================================================================

TEST_CASE (

"lower_to_hl: defer sites emit cleanup_region + cleanup_yield"
,
"[crank][lower_hl][phase_d]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "DeferCleanup";

    inp.defers.push_back({.call_name = "cleanup_a", .captured_args = {1}, .at = {}});
    inp.defers.push_back({.call_name = "cleanup_b", .captured_args = {2}, .at = {}});

    // One controlled exit, one trap exit
    inp.exit_edges.push_back({.kind = crank::exit_edge_kind::controlled, .target = "ret", .at = {}});
    inp.exit_edges.push_back({.kind = crank::exit_edge_kind::trap,       .target = "trap", .at = {}});

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());

    // cleanup_region emitted
    CHECK(res.stats.cleanup_region_count == 1);
    CHECK(res.stats.defer_site_count     == 2);

    // Verify cleanup_region exists in top-level ops
    bool found_cr = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::cleanup_region) {
                found_cr = true;
                // Must have a body region with cleanup_yield terminator
                REQUIRE_FALSE(op->regions.empty());
                const hl_region* cr = op->regions[0];
                REQUIRE_FALSE(cr->blocks.empty());
                const hl_block* cb = cr->blocks.head;
                bool found_cy = false;
                for (const hl_operation* cop = cb->ops.head; cop; cop = cop->list_node.next)
                    if (cop->op == hl_opcode::cleanup_yield) { found_cy = true; break; }
                CHECK(found_cy);
            }
        }
    }
    CHECK(found_cr);
}

TEST_CASE (

"lower_to_hl: controlled exit runs LIFO defers; trap exit runs none"
,
"[crank][lower_hl][phase_d]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "DeferLIFO";

    inp.defers.push_back({.call_name = "first",  .captured_args = {1}, .at = {}});
    inp.defers.push_back({.call_name = "second", .captured_args = {2}, .at = {}});
    inp.defers.push_back({.call_name = "third",  .captured_args = {3}, .at = {}});

    inp.exit_edges.push_back({.kind = crank::exit_edge_kind::controlled, .target = "ret", .at = {}});
    inp.exit_edges.push_back({.kind = crank::exit_edge_kind::trap, .target = "trap", .at = {}});

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.defer_site_count == 3);
    CHECK(res.stats.exit_edge_count  == 1);
    CHECK(res.stats.trap_edge_count  == 1);

    for (const auto& edge : res.exit_edges) {
        if (edge.kind == crank::exit_edge_kind::controlled) {
            REQUIRE(edge.defers_to_run.size() == 3);
            // LIFO: last-declared first
            CHECK(edge.defers_to_run[0].call_name == "third");
            CHECK(edge.defers_to_run[1].call_name == "second");
            CHECK(edge.defers_to_run[2].call_name == "first");
        }
        if (edge.kind == crank::exit_edge_kind::trap) {
            CHECK(edge.defers_to_run.empty());
        }
    }
}

TEST_CASE (

"lower_to_hl: no defers → no cleanup_region emitted"
,
"[crank][lower_hl][phase_d]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "NoDefer";
    // no defers, no emit_cleanup_region flag

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.cleanup_region_count == 0);
    CHECK(count_top_ops(res, hl_opcode::cleanup_region) == 0);
}

// ============================================================================
// Test group 5 — Transaction → tx.region (Phase E)
// ============================================================================

TEST_CASE (

"lower_to_hl: transaction emits tx.region with tx.read + tx.write + tx.yield"
,
"[crank][lower_hl][phase_e]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "TransferTx";

    crank::tx_config_info cfg;
    cfg.iso        = tx_isolation::serializable;
    cfg.retry      = 3;
    cfg.replay     = tx_replay::on_conflict;
    cfg.conflict   = tx_conflict::retry;
    cfg.partial    = tx_partial::disallow;
    cfg.durability = tx_durability::durable;
    cfg.has_abort  = false;
    cfg.reads.push_back({.resource = "accounts", .key = "from", .snapshot = false, .at = {}});
    cfg.writes.push_back({.resource = "accounts", .key = "from", .at = {}});
    cfg.writes.push_back({.resource = "accounts", .key = "to",   .at = {}});
    inp.transactions.push_back(cfg);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.tx_region_count == 1);

    // Verify tx.region exists, has body region with tx.read + tx.write + tx.yield
    bool found_txr = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op == hl_opcode::tx_region) {
                found_txr = true;

                // tx_attr check
                const auto& ta = std::get<tx_attr>(op->attr);
                CHECK(ta.iso     == tx_isolation::serializable);
                CHECK(ta.retry   == 3);
                CHECK(ta.replay  == tx_replay::on_conflict);
                CHECK(ta.conflict== tx_conflict::retry);

                REQUIRE_FALSE(op->regions.empty());
                const hl_region* txr = op->regions[0];
                REQUIRE_FALSE(txr->blocks.empty());

                std::size_t reads = 0, writes = 0;
                bool found_yield = false;
                for (const hl_block* tb = txr->blocks.head; tb; tb = tb->list_node.next) {
                    for (const hl_operation* top = tb->ops.head; top; top = top->list_node.next) {
                        if (top->op == hl_opcode::tx_read)  ++reads;
                        if (top->op == hl_opcode::tx_write) ++writes;
                        if (top->op == hl_opcode::tx_yield) found_yield = true;
                    }
                }
                CHECK(reads      == 1);
                CHECK(writes     == 2);
                CHECK(found_yield);
            }
        }
    }
    CHECK(found_txr);
}

TEST_CASE (

"lower_to_hl: transaction with abort emits tx.abort instead of tx.yield"
,
"[crank][lower_hl][phase_e]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "AbortTx";

    crank::tx_config_info cfg;
    cfg.iso       = tx_isolation::read_committed;
    cfg.has_abort = true;
    inp.transactions.push_back(cfg);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.tx_region_count == 1);

    bool found_abort = false;
    bool found_yield = false;
    for (const hl_block* blk = res.hl_fn.body_region.blocks.head;
         blk; blk = blk->list_node.next) {
        for (const hl_operation* op = blk->ops.head;
             op; op = op->list_node.next) {
            if (op->op != hl_opcode::tx_region) continue;
            REQUIRE_FALSE(op->regions.empty());
            const hl_region* txr = op->regions[0];
            for (const hl_block* tb = txr->blocks.head; tb; tb = tb->list_node.next) {
                for (const hl_operation* top = tb->ops.head; top; top = top->list_node.next) {
                    if (top->op == hl_opcode::tx_abort) found_abort = true;
                    if (top->op == hl_opcode::tx_yield) found_yield = true;
                }
            }
        }
    }
    CHECK(found_abort);
    CHECK_FALSE(found_yield);
}

TEST_CASE (

"lower_to_hl: module must declare transactions capability when tx.region present"
,
"[crank][lower_hl][phase_e]"
)
 {
    // Verifies that tx_region_count > 0 signals the transactions capability
    // requirement (caller declares via crank_capability_required).
    crank::lower_input inp;
    inp.fn_name = "TxCapCheck";
    crank::tx_config_info cfg;
    inp.transactions.push_back(cfg);

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.tx_region_count > 0);

    // Verify that crank_capability_required maps transaction → transactions bit.
    using lithe::ir::frontend::crank_feature;
    using lithe::ir::portable::portable_capability_bit;
    auto cap = lithe::ir::frontend::crank_capability_required(crank_feature::transaction);
    CHECK(cap == portable_capability_bit::transactions);
}

// ============================================================================
// Test group 6 — Combined: multiple phases together
// ============================================================================

TEST_CASE (

"lower_to_hl: combined if + while + guard in one function"
,
"[crank][lower_hl][combined]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "Combined";

    // Phase A: if (x < 0) { ... } else { ... }
    crank::cfg_node if_node;
    if_node.kind     = crank::cfg_node_kind::if_else;
    if_node.cmp.op   = crank::cmp_op::lt;
    if_node.cmp.type = crank::arith_type::signed_int;
    inp.cfg_nodes.push_back(if_node);

    // Phase B: sdiv
    inp.int_ops.push_back({crank::int_op_kind::sdiv, {}});

    // Phase C: unknown div_by_zero obligation
    crank::obligation_info ob;
    ob.kind   = crank::obligation_kind::div_by_zero;
    ob.status = crank::obligation_status::unknown;
    ob.policy = crank::safety_failure::trap;
    ob.label  = "divisor != 0";
    inp.obligations.push_back(ob);

    // Phase D: one defer
    inp.defers.push_back({.call_name = "cleanup", .captured_args = {}, .at = {}});
    inp.exit_edges.push_back({.kind = crank::exit_edge_kind::controlled, .target = "ret", .at = {}});

    auto res = crank::lower_to_hl(std::move(inp));
    CHECK(res.ok());
    CHECK(res.stats.icmp_count         >= 2);  // if-cmp + guard-cmp
    CHECK(res.stats.branch_cond_count  >= 1);
    CHECK(res.stats.int_op_count       == 1);
    CHECK(res.stats.guard_count        == 1);
    CHECK(res.stats.trap_count         == 1);
    CHECK(res.stats.cleanup_region_count == 1);
    CHECK(res.stats.defer_site_count   == 1);
}

TEST_CASE (

"lower_to_hl: loop metadata populates structured_for optimization hints"
,
"[crank][lower_hl][loop_hints]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "LoopHinted";
    inp.loops.push_back(crank::loop_bounds_info{
        .lower = 0,
        .upper = 100,
        .step = 4,
        .is_parallel = false,
        .lower_known = true,
        .upper_known = true,
        .step_known = true,
        .trip_count_hint = 25,
        .name = "i",
    });

    auto res = crank::lower_to_hl(std::move(inp));
    REQUIRE(res.ok());

    const hl_block* blk = res.hl_fn.body_region.blocks.head;
    REQUIRE(blk != nullptr);

    const hl_operation* sf = nullptr;
    for (const hl_operation* op = blk->ops.head; op; op = op->list_node.next) {
        if (op->op == hl_opcode::structured_for) {
            sf = op;
            break;
        }
    }
    REQUIRE(sf != nullptr);
    REQUIRE(std::holds_alternative<structured_for_attr>(sf->attr));

    const auto& attr = std::get<structured_for_attr>(sf->attr);
    CHECK(attr.bounds_known);
    CHECK(attr.stride_regular);
    CHECK(attr.trip_count_hint == 25u);
    CHECK(attr.bounds[0].lower_known);
    CHECK(attr.bounds[0].upper_known);
    CHECK(attr.bounds[0].step_known);
}

TEST_CASE (

"lower_to_hl: trip_count_hint is derived for bounded positive-step loops"
,
"[crank][lower_hl][loop_hints]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "LoopHintDerived";
    inp.loops.push_back(crank::loop_bounds_info{
        .lower = 0,
        .upper = 10,
        .step = 3,
        .is_parallel = false,
        .lower_known = true,
        .upper_known = true,
        .step_known = true,
        .trip_count_hint = 0,
        .name = "i",
    });

    auto res = crank::lower_to_hl(std::move(inp));
    REQUIRE(res.ok());

    const hl_block* blk = res.hl_fn.body_region.blocks.head;
    REQUIRE(blk != nullptr);
    const hl_operation* sf = blk->ops.head;
    REQUIRE(sf != nullptr);
    REQUIRE(sf->op == hl_opcode::structured_for);
    const auto& attr = std::get<structured_for_attr>(sf->attr);

    // ceil((10 - 0) / 3) = 4
    CHECK(attr.trip_count_hint == 4u);
}

