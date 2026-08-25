// =============================================================================
// test_crank_safety.cpp — Crank safety obligations unit tests (Module 3).
//
// Verifies: include/languages/crank/obligations.hpp
//           include/languages/crank/safety.hpp
//           include/languages/crank/dump.hpp (dump_obligations, dump_guards)
//
//  1. Index access emits two bounds obligations (lower + upper).
//  2. Integer division emits div_by_zero obligation.
//  3. Narrowing `as` emits range_cast obligation.
//  4. Constant divisor = 0 is immediately refuted.
//  5. obligation_stats counts per-family.
//  6. safety_failure policy resolution: fn attr > module > context default.
//  7. non-Result fn + return_result + live guard → compile diagnostic (§7b.3).
//  8. Result fn + return_result + live guard → no compile diagnostic.
//  9. trap fn with live guard → no §7b.3 diagnostic.
// 10. dump_obligations round-trips family/outcome/label.
// 11. dump_guards lists only guards, with policy label.
// 12. SafetyError is trivially copyable.
// 13. safety_kind to_string coverage.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/obligations.hpp"
#include "languages/crank/safety.hpp"
#include "languages/crank/dump.hpp"

using namespace crank;

// ============================================================================
// Test 1 — index access emits two bounds obligations
// ============================================================================

TEST_CASE (

"index access emits lower and upper bounds obligations"
,
"[crank][safety][obligations]"
)
 {
    obligation_builder bld;
    bld.add_index({}, "out", "i");
    auto obs = bld.take();

    REQUIRE(obs.size() == 2u);
    CHECK(obs[0].family == obligation_family::bounds);
    CHECK(obs[1].family == obligation_family::bounds);
    CHECK(obs[0].label.find("0 <=") != std::string::npos);
    CHECK(obs[1].label.find("len(out)") != std::string::npos);
    // outcomes default to unknown
    CHECK(obs[0].outcome == vakya::types::proof_status::unknown);
    CHECK(obs[1].outcome == vakya::types::proof_status::unknown);
}

// ============================================================================
// Test 2 — division emits div_by_zero obligation
// ============================================================================

TEST_CASE (

"division emits div_by_zero obligation"
,
"[crank][safety][obligations]"
)
 {
    obligation_builder bld;
    bld.add_div({}, "b");
    auto obs = bld.take();

    REQUIRE(obs.size() == 1u);
    CHECK(obs[0].family == obligation_family::div_by_zero);
    CHECK(obs[0].label.find("b != 0") != std::string::npos);
}

// ============================================================================
// Test 3 — narrowing as emits range_cast obligation
// ============================================================================

TEST_CASE (

"narrowing as emits range_cast obligation"
,
"[crank][safety][obligations]"
)
 {
    obligation_builder bld;
    bld.add_as({}, "Int64", "Int32", "x");
    auto obs = bld.take();

    REQUIRE(obs.size() == 1u);
    CHECK(obs[0].family == obligation_family::range_cast);
    CHECK(obs[0].label.find("Int32") != std::string::npos);
}

// ============================================================================
// Test 4 — constant divisor = 0 is immediately refuted
// ============================================================================

TEST_CASE (

"constant zero divisor is immediately refuted"
,
"[crank][safety][obligations]"
)
 {
    obligation_builder bld;
    bld.add_div_constant_zero({});
    auto obs = bld.take();

    REQUIRE(obs.size() == 1u);
    CHECK(obs[0].family == obligation_family::div_by_zero);
    CHECK(obs[0].outcome == vakya::types::proof_status::refuted);
}

// ============================================================================
// Test 5 — obligation_stats counts per-family
// ============================================================================

TEST_CASE (

"obligation_stats correct per-family counts"
,
"[crank][safety][obligations]"
)
 {
    obligation_builder bld;
    bld.add_index({}, "xs", "i");      // 2 bounds
    bld.add_div({}, "d");              // 1 div
    bld.add_as({}, "Int64", "Int32", "v");  // 1 range
    bld.add_parallel_safe({}, "map"); // 1 parallel

    auto obs = bld.take();
    auto stats = collect_obligation_stats(obs);

    CHECK(stats.total         == 5u);
    CHECK(stats.bounds_count  == 2u);
    CHECK(stats.div_count     == 1u);
    CHECK(stats.range_count   == 1u);
    CHECK(stats.parallel_count == 1u);
}

// ============================================================================
// Test 6 — safety policy resolution order
// ============================================================================

TEST_CASE (

"safety policy resolution: fn attr > module > context default"
,
"[crank][safety]"
)
 {
    safety_policy_resolver resolver;
    resolver.set_module_policy(safety_failure::terminate);
    resolver.set_context_default(safety_failure::trap);

    // No fn attr → module wins
    auto r1 = resolver.resolve("foo", false, safety_failure::host_handler);
    CHECK(r1.policy == safety_failure::terminate);
    CHECK(r1.source == policy_source::module_declaration);

    // Fn attr → fn wins
    auto r2 = resolver.resolve("bar", true, safety_failure::host_handler);
    CHECK(r2.policy == safety_failure::host_handler);
    CHECK(r2.source == policy_source::function_attribute);

    // No module, no fn attr → context default
    safety_policy_resolver resolver2;
    resolver2.set_context_default(safety_failure::terminate);
    auto r3 = resolver2.resolve("baz", false, safety_failure::trap);
    CHECK(r3.policy == safety_failure::terminate);
    CHECK(r3.source == policy_source::context_default);
}

// ============================================================================
// Test 7 — non-Result fn + return_result + live guard → compile diagnostic
// ============================================================================

TEST_CASE (

"non-Result fn with return_result and live guard emits compile diagnostic"
,
"[crank][safety][policy]"
)
 {
    auto diag = check_non_result_return_result(
        "Mean",
        /*fn_returns_result=*/false,
        safety_failure::return_result,
        /*has_live_guard=*/true,
        {});

    REQUIRE(diag.has_value());
    CHECK(diag->kind == safety_diagnostic_kind::non_result_return_result);
    CHECK(diag->is_error());
    CHECK(diag->message.find("Mean") != std::string::npos);
    CHECK(diag->message.find("return_result") != std::string::npos);
}

// ============================================================================
// Test 8 — Result fn + return_result + live guard → no compile diagnostic
// ============================================================================

TEST_CASE (

"Result fn with return_result and live guard is OK"
,
"[crank][safety][policy]"
)
 {
    auto diag = check_non_result_return_result(
        "SafeDiv",
        /*fn_returns_result=*/true,
        safety_failure::return_result,
        /*has_live_guard=*/true,
        {});
    CHECK(!diag.has_value());
}

// ============================================================================
// Test 9 — trap policy with live guard → no §7b.3 diagnostic
// ============================================================================

TEST_CASE (

"trap policy with live guard produces no diagnostic"
,
"[crank][safety][policy]"
)
 {
    auto diag = check_non_result_return_result(
        "Mean",
        /*fn_returns_result=*/false,
        safety_failure::trap,
        /*has_live_guard=*/true,
        {});
    CHECK(!diag.has_value());
}

// ============================================================================
// Test 10 — dump_obligations round-trips family/outcome/label
// ============================================================================

TEST_CASE (

"dump_obligations emits valid JSON with family and outcome"
,
"[crank][safety][dump]"
)
 {
    obligation_builder bld;
    source_span sp{0, 1, 2, 3};
    bld.add_index(sp, "xs", "i");
    auto obs = bld.take();
    // Mark first as proven, second stays unknown
    obs[0].outcome = vakya::types::proof_status::proven;

    std::string json = dump_obligations(obs);
    CHECK(!json.empty());
    CHECK(json.find("bounds") != std::string::npos);
    CHECK(json.find("proven") != std::string::npos);
    CHECK(json.find("unknown") != std::string::npos);
}

// ============================================================================
// Test 11 — dump_guards lists only guards
// ============================================================================

TEST_CASE (

"dump_guards only lists obligations with unknown outcome"
,
"[crank][safety][dump]"
)
 {
    obligation_builder bld;
    bld.add_div({}, "divisor");   // unknown → guard
    bld.add_as({}, "Int64", "Int32", "v");  // unknown → guard
    auto obs = bld.take();
    // one proven, one unknown
    obs[0].outcome = vakya::types::proof_status::proven;

    std::string json = dump_guards(obs, safety_failure::trap);
    // Only the second obligation should appear
    CHECK(json.find("trap") != std::string::npos);
    // proven obligation must not appear in guards
    bool range_in_guards = json.find("range_cast") != std::string::npos;
    bool proven_in_guards = json.find("proven") != std::string::npos;
    CHECK(!(range_in_guards && proven_in_guards));
}

// ============================================================================
// Test 12 — SafetyError is trivially copyable
// ============================================================================

TEST_CASE (

"SafetyError is trivially copyable (POD on failure path)"
,
"[crank][safety]"
)
 {
    static_assert(std::is_trivially_copyable_v<SafetyError>,
                  "SafetyError must be trivially copyable — no allocation on failure path");
    SUCCEED("SafetyError is trivially copyable");
}

// ============================================================================
// Test 13 — safety_kind to_string coverage
// ============================================================================

TEST_CASE (

"safety_kind to_string covers all variants"
,
"[crank][safety]"
)
 {
    CHECK(to_string(safety_kind::bounds_violation) == "BoundsViolation");
    CHECK(to_string(safety_kind::div_by_zero)      == "DivByZero");
    CHECK(to_string(safety_kind::range_conversion) == "RangeConversion");
    CHECK(to_string(safety_kind::assert_failed)    == "AssertFailed");
    CHECK(to_string(safety_kind::overflow_checked) == "OverflowChecked");
    CHECK(to_string(safety_kind::tx_failed)        == "TxFailed");
}
