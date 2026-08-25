// =============================================================================
// test_crank_verify.cpp — Crank verify driver unit tests (Module 3).
//
// Verifies: include/languages/crank/predicate.hpp
//           include/languages/crank/assumptions.hpp
//           include/languages/crank/verify.hpp
//           include/languages/crank/dump.hpp (dump_assumptions)
//
//  1. Predicate: lower integer literal → non-zero term payload.
//  2. Predicate: lower boolean literal.
//  3. Predicate: lower ident → hashed payload.
//  4. Predicate: lower len(xs) → len-composed payload.
//  5. Predicate: lower (a == b) binary comparison.
//  6. Predicate: lower forall x, p.
//  7. Predicate: effectful call inside predicate → error (diagnostic).
//  8. Predicate: result_kw without context → error.
//  9. Assumption context: push_requires visible, scope guard pops on exit.
// 10. Assumption context: push_for_range adds lower+upper entries.
// 11. Assumption context: push_if_cond negate=true prepends !.
// 12. assumption_stats counts correctly.
// 13. verify_driver/no_smt_backend: all obligations deferred → guards.
// 14. verify_driver/no_smt_backend: proof unknown → refuted outcome.
// 15. verify_driver/no_smt_backend: assert unknown → guard inserted.
// 16. verify_driver off policy: assert always guard; proof explicit error.
// 17. verify_driver assume policy: all proven without discharge.
// 18. discharge with refuted constant obligation → any_refuted().
// 19. fn_verify_result has_live_guard / any_refuted helpers.
// 20. dump_assumptions round-trips kind and description.
// 21. verify_policy to_string coverage.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/predicate.hpp"
#include "languages/crank/assumptions.hpp"
#include "languages/crank/verify.hpp"
#include "languages/crank/dump.hpp"

using namespace crank;
using namespace vakya::types;

// ============================================================================
// Test 1 — lower integer literal
// ============================================================================

TEST_CASE (

"predicate lower: integer literal has non-zero payload"
,
"[crank][verify][predicate]"
)
 {
    pred_arena arena;
    pred_node n;
    n.kind    = pred_kind::literal_int;
    n.int_val = 42;
    arena.push_back(n);

    predicate_lowerer lwr;
    lower_context ctx{};
    auto res = lwr.lower(arena, 0u, ctx);

    REQUIRE(res.has_value());
    CHECK(res->ok());
    CHECK(res->term_payload == 42u);
    CHECK(res->description == "42");
}

// ============================================================================
// Test 2 — lower boolean literal
// ============================================================================

TEST_CASE (

"predicate lower: bool literal payload"
,
"[crank][verify][predicate]"
)
 {
    pred_arena arena;
    pred_node n;
    n.kind     = pred_kind::literal_bool;
    n.bool_val = true;
    arena.push_back(n);

    predicate_lowerer lwr;
    auto res = lwr.lower(arena, 0u, {});
    REQUIRE(res.has_value());
    CHECK(res->term_payload == 1u);
    CHECK(res->description == "true");
}

// ============================================================================
// Test 3 — lower ident
// ============================================================================

TEST_CASE (

"predicate lower: ident hashed payload"
,
"[crank][verify][predicate]"
)
 {
    pred_arena arena;
    pred_node n;
    n.kind = pred_kind::ident;
    n.name = "myVar";
    arena.push_back(n);

    predicate_lowerer lwr;
    auto res = lwr.lower(arena, 0u, {});
    REQUIRE(res.has_value());
    CHECK(res->term_payload == std::hash<std::string>{}("myVar"));
    CHECK(res->description == "myVar");
}

// ============================================================================
// Test 4 — lower len(xs)
// ============================================================================

TEST_CASE (

"predicate lower: len(xs) composition"
,
"[crank][verify][predicate]"
)
 {
    // arena: [0]=ident xs, [1]=len(0)
    pred_arena arena;
    pred_node xs_node;
    xs_node.kind = pred_kind::ident;
    xs_node.name = "xs";
    arena.push_back(xs_node);

    pred_node len_node;
    len_node.kind = pred_kind::len_expr;
    len_node.children.push_back(0u);
    arena.push_back(len_node);

    predicate_lowerer lwr;
    auto res = lwr.lower(arena, 1u, {});
    REQUIRE(res.has_value());
    CHECK(res->description.find("len(xs)") != std::string::npos);
    CHECK(res->term_payload != 0u);
}

// ============================================================================
// Test 5 — lower binary comparison a == b
// ============================================================================

TEST_CASE (

"predicate lower: binary cmp (a == b)"
,
"[crank][verify][predicate]"
)
 {
    pred_arena arena;
    // [0]=ident a, [1]=ident b, [2]=cmp(0, 1, eq)
    pred_node a, b, cmp_node;
    a.kind = pred_kind::ident; a.name = "a"; arena.push_back(a);
    b.kind = pred_kind::ident; b.name = "b"; arena.push_back(b);
    cmp_node.kind = pred_kind::cmp;
    cmp_node.op   = pred_op::eq;
    cmp_node.children = {0u, 1u};
    arena.push_back(cmp_node);

    predicate_lowerer lwr;
    auto res = lwr.lower(arena, 2u, {});
    REQUIRE(res.has_value());
    CHECK(res->description.find("==") != std::string::npos);
}

// ============================================================================
// Test 6 — lower forall x, p
// ============================================================================

TEST_CASE (

"predicate lower: forall quantifier"
,
"[crank][verify][predicate]"
)
 {
    pred_arena arena;
    // [0]=ident x, [1]=forall x. (body=0)
    pred_node x; x.kind = pred_kind::ident; x.name = "x"; arena.push_back(x);
    pred_node fa;
    fa.kind = pred_kind::forall;
    fa.name = "x";
    fa.children.push_back(0u);
    arena.push_back(fa);

    predicate_lowerer lwr;
    auto res = lwr.lower(arena, 1u, {});
    REQUIRE(res.has_value());
    CHECK(res->description.find("forall") != std::string::npos);
    CHECK(res->term_payload != 0u);
}

// ============================================================================
// Test 7 — effectful call inside predicate → error
// ============================================================================

TEST_CASE (

"predicate lower: effectful call rejected with diagnostic"
,
"[crank][verify][predicate]"
)
 {
    pred_arena arena;
    pred_node call;
    call.kind = pred_kind::call;
    call.name = "sideEffectFn";
    arena.push_back(call);

    predicate_lowerer lwr;
    auto res = lwr.lower(arena, 0u, {});
    CHECK(!res.has_value());
    CHECK(!lwr.diagnostics().empty());
    CHECK(lwr.diagnostics()[0].message.find("sideEffectFn") != std::string::npos);
}

// ============================================================================
// Test 8 — result_kw without ensures context → error
// ============================================================================

TEST_CASE (

"predicate lower: result used outside ensures is error"
,
"[crank][verify][predicate]"
)
 {
    pred_arena arena;
    pred_node r; r.kind = pred_kind::result_kw; arena.push_back(r);

    predicate_lowerer lwr;
    lower_context ctx{}; // result_term = 0 → error
    auto res = lwr.lower(arena, 0u, ctx);
    CHECK(!res.has_value());
}

// ============================================================================
// Test 9 — assumption context push/pop via scope guard
// ============================================================================

TEST_CASE (

"assumption_context: push_requires visible; scope guard pops on exit"
,
"[crank][verify][assumptions]"
)
 {
    assumption_context actx;
    CHECK(actx.empty());

    actx.push_requires(0x1234u, "len(xs)==len(out)");
    CHECK(actx.size() == 1u);

    {
        auto scope = actx.enter_scope();
        actx.push_for_range(0u, 0xFFu, "i");
        CHECK(actx.size() == 3u);  // requires + lo + hi
    }
    // guard exited — for_range entries popped
    CHECK(actx.size() == 1u);
    CHECK(actx.active_assumptions()[0].kind == assumption_kind::requires_clause);
}

// ============================================================================
// Test 10 — push_for_range adds lower+upper entries
// ============================================================================

TEST_CASE (

"assumption_context: push_for_range emits lo/hi bounds"
,
"[crank][verify][assumptions]"
)
 {
    assumption_context actx;
    actx.push_for_range(0u, 100u, "i");
    REQUIRE(actx.size() == 2u);
    CHECK(actx.active_assumptions()[0].kind == assumption_kind::for_range_lower);
    CHECK(actx.active_assumptions()[1].kind == assumption_kind::for_range_upper);
}

// ============================================================================
// Test 11 — push_if_cond negate prepends !
// ============================================================================

TEST_CASE (

"assumption_context: push_if_cond negated description starts with !"
,
"[crank][verify][assumptions]"
)
 {
    assumption_context actx;
    actx.push_if_cond(0xABu, /*negate=*/true, "cond");
    REQUIRE(actx.size() == 1u);
    CHECK(actx.active_assumptions()[0].description[0] == '!');
}

// ============================================================================
// Test 12 — assumption_stats counts
// ============================================================================

TEST_CASE (

"assumption_stats: per-kind counts correct"
,
"[crank][verify][assumptions]"
)
 {
    assumption_context actx;
    actx.push_requires(0u, "r1");
    actx.push_requires(0u, "r2");
    actx.push_proven_assertion(0u, "a1");
    actx.push_refinement(0u, "ref1");
    actx.push_if_cond(0u, false, "c");
    actx.push_for_range(0u, 0u, "i");  // 2 entries

    auto stats = collect_assumption_stats(actx.active_assumptions());
    CHECK(stats.total            == 7u);
    CHECK(stats.requires_count   == 2u);
    CHECK(stats.assertion_count  == 1u);
    CHECK(stats.refinement_count == 1u);
    CHECK(stats.if_cond_count    == 1u);
    CHECK(stats.range_count      == 2u);
}

// ============================================================================
// Test 13 — verify_driver no_smt: all obligations deferred → guards
// ============================================================================

TEST_CASE (

"verify_driver/no_smt: all obligations become guards (deferred)"
,
"[crank][verify][driver]"
)
 {
    obligation_builder bld;
    bld.add_index({}, "xs", "i");
    bld.add_div({}, "b");
    auto obs = bld.take();

    assumption_context actx;
    safety_policy_record sfail{safety_failure::trap, policy_source::context_default, "fn"};

    verify_driver<> drv;
    auto result = drv.discharge("Scale", obs, actx, false, sfail);

    CHECK(result.has_live_guard());
    for (const auto& o : result.obligations) {
        CHECK(o.guard_inserted);
        // no_smt → deferred→unknown
        CHECK(o.status == proof_status::unknown);
    }
}

// ============================================================================
// Test 14 — proof unknown under no_smt → unknown (driver maps deferred→unknown)
// ============================================================================

TEST_CASE (

"verify_driver/no_smt: proof unknown → obligation status unknown"
,
"[crank][verify][driver]"
)
 {
    vakya::types::proof_obligation ob;
    ob.kind        = kProveKind;
    ob.description = "len(xs) >= 0";

    assumption_context actx;
    verify_driver<> drv;
    auto out = drv.discharge_explicit(ob, proof_construct_kind::proof, actx);

    // proof unknown → refuted (§3.3)
    CHECK(out.status == proof_status::refuted);
    CHECK(!out.guard_inserted);
}

// ============================================================================
// Test 15 — assert unknown under no_smt → guard
// ============================================================================

TEST_CASE (

"verify_driver/no_smt: assert unknown → guard inserted"
,
"[crank][verify][driver]"
)
 {
    vakya::types::proof_obligation ob;
    ob.kind        = kArithKind;
    ob.description = "b != 0";

    assumption_context actx;
    verify_driver<> drv;
    auto out = drv.discharge_explicit(ob, proof_construct_kind::assert_, actx);

    // assert unknown → guard
    CHECK(out.status == proof_status::unknown);
    CHECK(out.guard_inserted);
}

// ============================================================================
// Test 16 — off policy: assert always guard; proof → error-mapped (refuted)
// ============================================================================

TEST_CASE (

"verify_driver off policy: assert→guard, proof→refuted"
,
"[crank][verify][driver]"
)
 {
    verify_options opts;
    opts.policy = verify_policy::off;
    verify_driver<> drv(opts);

    assumption_context actx;

    vakya::types::proof_obligation ob_assert;
    ob_assert.kind = kArithKind;
    ob_assert.description = "off_assert";

    vakya::types::proof_obligation ob_proof;
    ob_proof.kind = kProveKind;
    ob_proof.description = "off_proof";

    auto out_a = drv.discharge_explicit(ob_assert, proof_construct_kind::assert_, actx);
    CHECK(out_a.guard_inserted);

    auto out_p = drv.discharge_explicit(ob_proof, proof_construct_kind::proof, actx);
    CHECK(out_p.status == proof_status::refuted);
}

// ============================================================================
// Test 17 — assume policy: all proven without discharge
// ============================================================================

TEST_CASE (

"verify_driver assume policy: all obligations marked proven"
,
"[crank][verify][driver]"
)
 {
    verify_options opts;
    opts.policy = verify_policy::assume;
    verify_driver<> drv(opts);

    obligation_builder bld;
    bld.add_index({}, "xs", "i");
    auto obs = bld.take();

    assumption_context actx;
    safety_policy_record sfail{safety_failure::trap, policy_source::context_default, ""};
    auto result = drv.discharge("fn", obs, actx, false, sfail);

    CHECK(!result.has_live_guard());
    for (const auto& o : result.obligations)
        CHECK(o.status == proof_status::proven);
}

// ============================================================================
// Test 18 — refuted constant obligation propagates through discharge
// ============================================================================

TEST_CASE (

"discharge propagates refuted constant obligation"
,
"[crank][verify][driver]"
)
 {
    obligation_builder bld;
    bld.add_div_constant_zero({});
    auto obs = bld.take();

    assumption_context actx;
    verify_options opts; opts.policy = verify_policy::check;
    verify_driver<> drv(opts);
    safety_policy_record sfail{safety_failure::trap, policy_source::context_default, ""};
    auto result = drv.discharge("bad_fn", obs, actx, false, sfail);

    CHECK(result.any_refuted());
}

// ============================================================================
// Test 19 — fn_verify_result helpers
// ============================================================================

TEST_CASE (

"fn_verify_result: has_live_guard and any_refuted helpers"
,
"[crank][verify][driver]"
)
 {
    fn_verify_result r;
    r.fn_name = "test";

    discharge_outcome o1;
    o1.status = proof_status::proven;
    o1.guard_inserted = false;

    discharge_outcome o2;
    o2.status = proof_status::unknown;
    o2.guard_inserted = true;

    r.obligations.push_back(o1);
    CHECK(!r.has_live_guard());
    CHECK(!r.any_refuted());

    r.obligations.push_back(o2);
    CHECK(r.has_live_guard());
    CHECK(!r.any_refuted());

    discharge_outcome o3;
    o3.status = proof_status::refuted;
    r.obligations.push_back(o3);
    CHECK(r.any_refuted());
}

// ============================================================================
// Test 20 — dump_assumptions round-trips kind and description
// ============================================================================

TEST_CASE (

"dump_assumptions emits kind and description"
,
"[crank][verify][dump]"
)
 {
    assumption_context actx;
    actx.push_requires(0u, "len(xs)==len(out)");
    actx.push_for_range(0u, 100u, "i");

    std::string json = dump_assumptions(actx);
    CHECK(!json.empty());
    CHECK(json.find("requires") != std::string::npos);
    CHECK(json.find("len(xs)==len(out)") != std::string::npos);
    CHECK(json.find("for_range") != std::string::npos);
}

// ============================================================================
// Test 21 — verify_policy to_string coverage
// ============================================================================

TEST_CASE (

"verify_policy to_string covers all variants"
,
"[crank][verify]"
)
 {
    CHECK(to_string(verify_policy::off)      == "off");
    CHECK(to_string(verify_policy::assume)   == "assume");
    CHECK(to_string(verify_policy::check)    == "check");
    CHECK(to_string(verify_policy::paranoid) == "paranoid");
}
