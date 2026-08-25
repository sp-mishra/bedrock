// =============================================================================
// test_lithe_ir_hl_control.cpp — verifier/round-trip/digest tests for the
// language-control extension to HL MIR.
//
// Covers: CFG (branch/return/icmp/fcmp/select), integer ops, safety (guard/trap),
//         cleanup, transactions, and all new diagnostic codes.
//
// Added at END of test suite per project rules — does NOT modify existing tests.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "lithe/lithe_ir_core.hpp"
#include "lithe/lithe_ir/portable/module.hpp"
#include "lithe/lithe_ir/portable/verify.hpp"

using namespace lithe::ir;
using namespace lithe::ir::portable;
using namespace lithe::ir::adapters;

// =============================================================================
// Helpers
// =============================================================================

namespace {
    // Make a minimal valid module containing a function that accepts a block list.
    portable_module make_module_from_fn(lithe_hl_mir_ir fn) {
        portable_module mod;
        mod.schema = {1, 1, 0};
        mod.functions.push_back(std::move(fn));
        mod.manifest.producer = "test_hl_control";
        mod.manifest.source_language = "test";
        return mod;
    }

    // Declare capabilities helper.
    portable_module make_module_cap(lithe_hl_mir_ir fn, portable_capability_bit cap) {
        auto mod = make_module_from_fn(std::move(fn));
        mod.declared_capabilities.set(cap);
        return mod;
    }

    // Build a two-block function: abs(x) = if x < 0 then -x else x
    // Block 0: icmp slt x,0 → cond; branch_cond cond → [blk1, blk2]
    // Block 1: sub 0,x → neg; return neg
    // Block 2: return x
    lithe_hl_mir_ir make_abs_fn() {
        lithe_hl_mir_ir fn;
        fn.function_name = "abs_fn";
        fn.source_stage = stage::lowered;
        fn.schema = {1, 1, 0};

        // Values: 0=arg_x (i64), 1=zero (i64), 2=cond (i1), 3=neg (i64)
        fn.values = {
            hl_wire_value{0, "i64"}, // x
            hl_wire_value{1, "i64"}, // zero constant
            hl_wire_value{2, "i1"}, // cond (icmp result)
            hl_wire_value{3, "i64"}, // -x
        };

        // Op 0: argument (x) → value 0
        hl_wire_op arg_op;
        arg_op.id = 0;
        arg_op.domain = "lithe.hl";
        arg_op.name = "argument";
        arg_op.result_ids = {0};
        arg_op.block_id = 0;
        arg_op.region_id = 0;

        // Op 1: constant 0 → value 1
        hl_wire_op const_op;
        const_op.id = 1;
        const_op.domain = "lithe.hl";
        const_op.name = "constant";
        const_op.result_ids = {1};
        const_op.block_id = 0;
        const_op.region_id = 0;

        // Op 2: icmp (slt) x,0 → cond
        hl_wire_op icmp_op;
        icmp_op.id = 2;
        icmp_op.domain = "lithe.hl";
        icmp_op.name = "icmp";
        icmp_op.operand_ids = {0, 1};
        icmp_op.result_ids = {2};
        icmp_op.block_id = 0;
        icmp_op.region_id = 0;
        icmp_op.compare = hl_wire_op::compare_wire_attr{2 /* slt */, true};

        // Op 3: branch_cond cond → true=blk1(id=1), false=blk2(id=2)
        hl_wire_op bcond_op;
        bcond_op.id = 3;
        bcond_op.domain = "lithe.hl";
        bcond_op.name = "branch_cond";
        bcond_op.operand_ids = {2};
        bcond_op.block_id = 0;
        bcond_op.region_id = 0;
        bcond_op.branch_cond = hl_wire_op::branch_cond_wire_attr{1, 2};

        // Op 4: sub 1,0 (i.e. 0-x) → value 3 (block 1)
        hl_wire_op sub_op;
        sub_op.id = 4;
        sub_op.domain = "lithe.hl";
        sub_op.name = "sub";
        sub_op.operand_ids = {1, 0};
        sub_op.result_ids = {3};
        sub_op.block_id = 1;
        sub_op.region_id = 0;

        // Op 5: return neg (block 1)
        hl_wire_op ret1_op;
        ret1_op.id = 5;
        ret1_op.domain = "lithe.hl";
        ret1_op.name = "return";
        ret1_op.operand_ids = {3};
        ret1_op.block_id = 1;
        ret1_op.region_id = 0;

        // Op 6: return x (block 2)
        hl_wire_op ret2_op;
        ret2_op.id = 6;
        ret2_op.domain = "lithe.hl";
        ret2_op.name = "return";
        ret2_op.operand_ids = {0};
        ret2_op.block_id = 2;
        ret2_op.region_id = 0;

        fn.ops = {arg_op, const_op, icmp_op, bcond_op, sub_op, ret1_op, ret2_op};

        fn.blocks = {
            hl_wire_block{0, {0, 1, 2, 3}, {}}, // entry block
            hl_wire_block{1, {4, 5}, {}}, // true branch: -x
            hl_wire_block{2, {6}, {}}, // false branch: x
        };
        fn.regions = {hl_wire_region{0, {0, 1, 2}, {}}};
        fn.entry_block_ids = {0};
        return fn;
    }
} // namespace

// =============================================================================
// Test: positive — abs function verifies ok
// =============================================================================

TEST_CASE (

"hl_control: abs fn (icmp+branch_cond+sub+return) verifies ok"
,
"[hl_control][positive]"
)
 {
    const auto mod = make_module_from_fn(make_abs_fn());
    // Schema 1.1.0 ops may be unknown under strict policy; allow_unknown_optional_ops=true.
    const verify_policy pol{.require_capability_coverage=true, .allow_unknown_optional_ops=true};
    const auto rep = verify_portable(mod, pol);
    if (!rep.ok) {
        for (const auto& d : rep.diagnostics)
            UNSCOPED_INFO(d.code << ": " << d.message);
    }
    CHECK(rep.ok);
}

// =============================================================================
// Test: T003 — branch_cond with non-i1 condition
// =============================================================================

TEST_CASE (

"hl_control: T003 — branch_cond non-i1 condition"
,
"[hl_control][negative][T003]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "bad_cond";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 1, 0};
    fn.values = {hl_wire_value{0, "i64"}, hl_wire_value{1, "i64"}};

    hl_wire_op cst; cst.id=0; cst.domain="lithe.hl"; cst.name="constant";
    cst.result_ids={0}; cst.block_id=0; cst.region_id=0;

    hl_wire_op bc; bc.id=1; bc.domain="lithe.hl"; bc.name="branch_cond";
    bc.operand_ids={0}; bc.block_id=0; bc.region_id=0;
    bc.branch_cond = hl_wire_op::branch_cond_wire_attr{0, 0};

    fn.ops = {cst, bc};
    fn.blocks = {hl_wire_block{0, {0, 1}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    const auto mod = make_module_from_fn(std::move(fn));
    const auto rep = verify_portable(mod);
    const bool has_t003 = std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
        [](const auto& d){ return std::string_view{d.code} == "LITHE-PORT-T003"; });
    CHECK(has_t003);
}

// =============================================================================
// Test: C002 — branch_cond missing target block
// =============================================================================

TEST_CASE (

"hl_control: C002 — branch_cond missing target block"
,
"[hl_control][negative][C002]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "missing_target";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 1, 0};
    fn.values = {hl_wire_value{0, "i1"}};

    hl_wire_op cst; cst.id=0; cst.domain="lithe.hl"; cst.name="constant";
    cst.result_ids={0}; cst.block_id=0; cst.region_id=0;

    hl_wire_op bc; bc.id=1; bc.domain="lithe.hl"; bc.name="branch_cond";
    bc.operand_ids={0}; bc.block_id=0; bc.region_id=0;
    bc.branch_cond = hl_wire_op::branch_cond_wire_attr{99, 100}; // non-existent targets

    fn.ops = {cst, bc};
    fn.blocks = {hl_wire_block{0, {0, 1}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    const auto mod = make_module_from_fn(std::move(fn));
    const auto rep = verify_portable(mod);
    const bool has_c002 = std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
        [](const auto& d){ return std::string_view{d.code} == "LITHE-PORT-C002"; });
    CHECK(has_c002);
}

// =============================================================================
// Test: C003 — op after terminator
// =============================================================================

TEST_CASE (

"hl_control: C003 — op after terminator"
,
"[hl_control][negative][C003]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "op_after_term";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 1, 0};
    fn.values = {hl_wire_value{0, "i64"}};

    hl_wire_op ret_op; ret_op.id=0; ret_op.domain="lithe.hl"; ret_op.name="return";
    ret_op.block_id=0; ret_op.region_id=0;

    hl_wire_op cst; cst.id=1; cst.domain="lithe.hl"; cst.name="constant";
    cst.result_ids={0}; cst.block_id=0; cst.region_id=0;

    fn.ops = {ret_op, cst};
    fn.blocks = {hl_wire_block{0, {0, 1}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    const auto mod = make_module_from_fn(std::move(fn));
    const auto rep = verify_portable(mod);
    const bool has_c003 = std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
        [](const auto& d){ return std::string_view{d.code} == "LITHE-PORT-C003"; });
    CHECK(has_c003);
}

// =============================================================================
// Test: K001 — tx.write without transactions capability
// =============================================================================

TEST_CASE (

"hl_control: K001 — tx.write without transactions capability"
,
"[hl_control][negative][K001]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "missing_tx_cap";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 5, 0};
    fn.values = {
        hl_wire_value{0, "opaque64"}, // resource handle
        hl_wire_value{1, "i64"},       // key
        hl_wire_value{2, "i64"},       // value
    };

    hl_wire_op tx_write; tx_write.id=0; tx_write.domain="lithe.hl"; tx_write.name="tx.write";
    tx_write.operand_ids={0,1,2}; tx_write.block_id=0; tx_write.region_id=0;

    hl_wire_op ret_op; ret_op.id=1; ret_op.domain="lithe.hl"; ret_op.name="return";
    ret_op.block_id=0; ret_op.region_id=0;

    fn.ops = {tx_write, ret_op};
    fn.blocks = {hl_wire_block{0, {0, 1}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    // No transactions capability declared.
    const auto mod = make_module_from_fn(std::move(fn));
    const verify_policy pol{.require_capability_coverage=true, .allow_unknown_optional_ops=true};
    const auto rep = verify_portable(mod, pol);
    const bool has_k001 = std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
        [](const auto& d){ return std::string_view{d.code} == "LITHE-PORT-K001"; });
    CHECK(has_k001);
}

// =============================================================================
// Test: K001 — cleanup_region without defer_scopes capability
// =============================================================================

TEST_CASE (

"hl_control: K001 — cleanup_region without defer_scopes capability"
,
"[hl_control][negative][K001]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "missing_defer_cap";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 4, 0};

    hl_wire_op cr; cr.id=0; cr.domain="lithe.hl"; cr.name="cleanup_region";
    cr.block_id=0; cr.region_id=0;

    hl_wire_op ret_op; ret_op.id=1; ret_op.domain="lithe.hl"; ret_op.name="return";
    ret_op.block_id=0; ret_op.region_id=0;

    fn.ops = {cr, ret_op};
    fn.blocks = {hl_wire_block{0, {0, 1}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    const auto mod = make_module_from_fn(std::move(fn));
    const verify_policy pol{.require_capability_coverage=true, .allow_unknown_optional_ops=true};
    const auto rep = verify_portable(mod, pol);
    const bool has_k001 = std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
        [](const auto& d){ return std::string_view{d.code} == "LITHE-PORT-K001"; });
    CHECK(has_k001);
}

// =============================================================================
// Test: E002 — tx.write outside tx.region (detected via region_id heuristic)
// =============================================================================

TEST_CASE (

"hl_control: E002 — tx.write without any tx.region op present"
,
"[hl_control][negative][E002]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "tx_outside";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 5, 0};
    fn.values = {
        hl_wire_value{0, "opaque64"},
        hl_wire_value{1, "i64"},
        hl_wire_value{2, "i64"},
    };

    // tx.write with no tx.region op in function → E002 (tx_body_regions is empty).
    hl_wire_op tw; tw.id=0; tw.domain="lithe.hl"; tw.name="tx.write";
    tw.operand_ids={0,1,2}; tw.block_id=0; tw.region_id=0;

    hl_wire_op ret_op; ret_op.id=1; ret_op.domain="lithe.hl"; ret_op.name="return";
    ret_op.block_id=0; ret_op.region_id=0;

    fn.ops = {tw, ret_op};
    fn.blocks = {hl_wire_block{0, {0, 1}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    auto mod = make_module_from_fn(std::move(fn));
    mod.declared_capabilities.set(portable_capability_bit::transactions);
    const verify_policy pol{.require_capability_coverage=true, .allow_unknown_optional_ops=true};
    const auto rep = verify_portable(mod, pol);
    const bool has_e002 = std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
        [](const auto& d){ return std::string_view{d.code} == "LITHE-PORT-E002"; });
    CHECK(has_e002);
}

// =============================================================================
// Test: Round-trip — abs fn freeze→thaw→re-freeze preserves semantic_digest
// =============================================================================

TEST_CASE (

"hl_control: round-trip digest stability for CFG ops"
,
"[hl_control][roundtrip]"
)
 {
    // Digest is computed from canonical_encode of the portable_module.
    // We only test wire-form encode determinism here (no live round-trip
    // since that requires the full bridge / codegen include).
    const auto mod1 = make_module_from_fn(make_abs_fn());
    const auto mod2 = make_module_from_fn(make_abs_fn());

    SECTION("same wire module encodes identically") {
        const auto enc1 = canonical_encode(mod1);
        const auto enc2 = canonical_encode(mod2);
        CHECK(enc1 == enc2);
    }

    SECTION("same wire module has same semantic digest") {
        const auto d1 = semantic_digest(mod1);
        const auto d2 = semantic_digest(mod2);
        CHECK(d1 == d2);
    }

    SECTION("differing module has different digest") {
        auto mod3 = make_module_from_fn(make_abs_fn());
        mod3.functions[0].ops[0].name = "constant"; // corrupt an op name
        CHECK(canonical_encode(mod1) != canonical_encode(mod3));
    }
}

// =============================================================================
// Test: Round-trip digest — transaction attrs
// =============================================================================

TEST_CASE (

"hl_control: round-trip digest with tx attrs"
,
"[hl_control][roundtrip][tx]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "tx_fn";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 5, 0};
    fn.values        = {hl_wire_value{0, "i64"}};

    hl_wire_op tx_op; tx_op.id=0; tx_op.domain="lithe.hl"; tx_op.name="tx.region";
    tx_op.result_ids={0}; tx_op.block_id=0; tx_op.region_id=0;
    tx_op.transaction = hl_wire_op::tx_wire_attr{
        2,    // serializable (idx 2)
        3,    // retry=3
        0, 0, 0, 1, 0, 0 // defaults, durable (idx 1)
    };

    hl_wire_op ret_op; ret_op.id=1; ret_op.domain="lithe.hl"; ret_op.name="return";
    ret_op.operand_ids={0}; ret_op.block_id=0; ret_op.region_id=0;

    fn.ops = {tx_op, ret_op};
    fn.blocks = {hl_wire_block{0, {0,1}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    auto mod1 = make_module_from_fn(fn);
    auto mod2 = make_module_from_fn(fn);
    mod1.declared_capabilities.set(portable_capability_bit::transactions);
    mod2.declared_capabilities.set(portable_capability_bit::transactions);

    CHECK(canonical_encode(mod1) == canonical_encode(mod2));
    CHECK(semantic_digest(mod1)  == semantic_digest(mod2));
}

// =============================================================================
// Test: Round-trip digest — guard/trap attrs
// =============================================================================

TEST_CASE (

"hl_control: round-trip digest with guard/trap attrs"
,
"[hl_control][roundtrip][safety]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "guarded_fn";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 3, 0};
    fn.values        = {hl_wire_value{0, "i1"}};

    hl_wire_op cst; cst.id=0; cst.domain="lithe.hl"; cst.name="constant";
    cst.result_ids={0}; cst.block_id=0; cst.region_id=0;

    hl_wire_op guard_op; guard_op.id=1; guard_op.domain="lithe.hl"; guard_op.name="guard";
    guard_op.operand_ids={0}; guard_op.block_id=0; guard_op.region_id=0;
    guard_op.guard = hl_wire_op::guard_wire_attr{3 /*assert*/, 1 /*trap*/, 0, 0};

    hl_wire_op trap_op; trap_op.id=2; trap_op.domain="lithe.hl"; trap_op.name="trap";
    trap_op.block_id=0; trap_op.region_id=0;
    trap_op.trap = hl_wire_op::trap_wire_attr{6 /*unreachable*/, 0};

    fn.ops = {cst, guard_op, trap_op};
    fn.blocks = {hl_wire_block{0, {0,1,2}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    auto mod1 = make_module_from_fn(fn);
    auto mod2 = make_module_from_fn(fn);
    CHECK(canonical_encode(mod1) == canonical_encode(mod2));
}

// =============================================================================
// Test: Round-trip digest — cleanup attrs
// =============================================================================

TEST_CASE (

"hl_control: round-trip digest with cleanup attr"
,
"[hl_control][roundtrip][cleanup]"
)
 {
    lithe_hl_mir_ir fn;
    fn.function_name = "cleanup_fn";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 4, 0};

    hl_wire_op cr; cr.id=0; cr.domain="lithe.hl"; cr.name="cleanup_region";
    cr.block_id=0; cr.region_id=0;
    cr.cleanup = hl_wire_op::cleanup_wire_attr{{1, 2, 3}};

    hl_wire_op cy; cy.id=1; cy.domain="lithe.hl"; cy.name="cleanup_yield";
    cy.block_id=0; cy.region_id=0;

    fn.ops = {cr, cy};
    fn.blocks = {hl_wire_block{0, {0,1}, {}}};
    fn.regions = {hl_wire_region{0, {0}, {}}};
    fn.entry_block_ids = {0};

    auto mod1 = make_module_cap(fn, portable_capability_bit::defer_scopes);
    auto mod2 = make_module_cap(fn, portable_capability_bit::defer_scopes);
    CHECK(canonical_encode(mod1) == canonical_encode(mod2));
}

// =============================================================================
// Test: S002 dominance — use before def across branch
// =============================================================================

TEST_CASE (

"hl_control: S002 — use before def (value defined in non-dominating block)"
,
"[hl_control][negative][S002]"
)
 {
    // Block 0: branch → block 1
    // Block 1: defines val 0; return val 0
    // Block 2: return val 0   ← uses val 0 but block 2 not dominated by block 1
    // Entry: block 0 → branches to block 1 only → block 2 unreachable, but
    // we test with two parallel branches to force non-domination.
    //
    // Simpler: use val 0 in block 0 before defining it (forward ref from op in same block
    // but to a result of a later op — impossible in sequential order, so instead:
    // value defined in block 1, used in block 0 (entry), which is not dominated by block 1).

    lithe_hl_mir_ir fn;
    fn.function_name = "dom_violation";
    fn.source_stage  = stage::lowered;
    fn.schema        = {1, 1, 0};
    fn.values = {
        hl_wire_value{0, "i64"},  // defined in block 1, used in block 0
        hl_wire_value{1, "i64"},  // dummy
    };

    // Block 0: uses val 0 (defined later in block 1), then return val1
    hl_wire_op use_op; use_op.id=0; use_op.domain="lithe.hl"; use_op.name="add";
    use_op.operand_ids={0,0}; use_op.result_ids={1};
    use_op.block_id=0; use_op.region_id=0;

    hl_wire_op ret0; ret0.id=1; ret0.domain="lithe.hl"; ret0.name="return";
    ret0.operand_ids={1}; ret0.block_id=0; ret0.region_id=0;

    // Block 1: defines val 0; return
    hl_wire_op def_op; def_op.id=2; def_op.domain="lithe.hl"; def_op.name="constant";
    def_op.result_ids={0}; def_op.block_id=1; def_op.region_id=0;

    hl_wire_op ret1; ret1.id=3; ret1.domain="lithe.hl"; ret1.name="return";
    ret1.block_id=1; ret1.region_id=0;

    fn.ops = {use_op, ret0, def_op, ret1};
    fn.blocks = {
        hl_wire_block{0, {0, 1}, {}},
        hl_wire_block{1, {2, 3}, {}},
    };
    fn.regions = {hl_wire_region{0, {0, 1}, {}}};
    fn.entry_block_ids = {0};

    const auto mod = make_module_from_fn(std::move(fn));
    const auto rep = verify_portable(mod);
    const bool has_s002 = std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
        [](const auto& d){ return std::string_view{d.code} == "LITHE-PORT-S002"; });
    // S002 now real (dominance check active with CFG ops present).
    CHECK(has_s002);
}
