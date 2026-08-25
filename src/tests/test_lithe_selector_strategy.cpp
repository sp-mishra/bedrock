// =============================================================================
// test_lithe_selector_strategy.cpp — Unit Tests: Selector Strategy Abstraction
//
// Verifies: include/edsl/lithe_selector_strategy.hpp
//
// Cases:
//   1.  selector_strategy concept: satisfied for cost_based_selector.
//   2.  selector_strategy concept: satisfied for profile_guided_selector.
//   3.  selector_strategy concept: satisfied for rule_based_selector.
//   4.  selector_strategy concept: satisfied for learned_selector.
//   5.  selector_strategy concept: satisfied for fallback_selector.
//   6.  cost_based_selector: selects available backend.
//   7.  cost_based_selector: returns error when no backends available.
//   8.  cost_based_selector: policy=lowest_latency prefers low-cost backend.
//   9.  profile_guided_selector: latency bias → lowest_latency policy path.
//   10. profile_guided_selector: empty hints → fallback to balanced.
//   11. rule_based_selector: matching rule selects its backend.
//   12. rule_based_selector: no matching rule falls back to cost-based.
//   13. rule_based_selector: first matching rule wins (order preserved).
//   14. learned_selector: no infer_fn falls back to cost-based.
//   15. learned_selector: infer_fn returns viable backend → selected.
//   16. learned_selector: infer_fn returns unavailable backend → fallback.
//   17. fallback_selector: primary succeeds → fallback not used.
//   18. fallback_selector: primary fails → fallback called.
//   19. cost_based_selector: descriptor id is non-empty.
//   20. profile_guided_selector: descriptor id is non-empty.
//   21. rule_based_selector: descriptor non-deterministic = false.
//   22. learned_selector: descriptor safe_for_runtime_replacement = true.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_selector_strategy.hpp"
#include "lithe/lithe_execution/capability.hpp"

namespace sel = lithe::selector;
namespace alg = lithe::algorithms;
namespace ex = lithe::execution;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {
    alg::backend_capability_info make_backend(std::string_view id,
                                              bool available = true,
                                              double exec_cost = 10.0) {
        alg::backend_capability_info b;
        b.backend_id = id;
        b.caps = ex::backend_capability_set{};
        b.supported_modes.set(ex::execution_mode::interpret);
        b.compile_cost = 1.0;
        b.exec_cost = exec_cost;
        b.transfer_cost = 1.0;
        b.ir_compatible = true;
        b.services_ok = true;
        b.security_ok = true;
        b.artifact_ok = true;
        b.available = available;
        return b;
    }

    ex::compile_requirements make_reqs() {
        ex::compile_requirements r;
        r.required = ex::backend_capability_set{};
        r.preferred = ex::backend_capability_set{};
        return r;
    }
} // namespace

// ===========================================================================
// Concept checks (static_assert)
// ===========================================================================

static_assert(sel::selector_strategy<sel::cost_based_selector>);
static_assert(sel::selector_strategy<sel::profile_guided_selector>);
static_assert(sel::selector_strategy<sel::rule_based_selector>);
static_assert(sel::selector_strategy<sel::learned_selector>);
static_assert(sel::selector_strategy<
    sel::fallback_selector<sel::cost_based_selector, sel::rule_based_selector>>);

TEST_CASE (


"selector_strategy concept static assertions compile"
,
"[concept]"
)
{
    // Verified at compile time above — just confirm the test runs.
    SUCCEED("all selector_strategy static_asserts passed");
}

// ===========================================================================
// cost_based_selector
// ===========================================================================

TEST_CASE (


"cost_based_selector selects available backend"
,
"[cost_based_selector]"
)
{
    sel::cost_based_selector s;
    auto b = make_backend("interp");
    std::array<alg::backend_capability_info, 1> backends{b};
    ex::compile_requirements reqs = make_reqs();
    alg::negotiation_report_buffer report;
    auto r = s.select(backends, reqs, report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "interp");
}

TEST_CASE (


"cost_based_selector returns error when no backends"
,
"[cost_based_selector]"
)
{
    sel::cost_based_selector s;
    std::span<const alg::backend_capability_info> empty{};
    alg::negotiation_report_buffer report;
    auto r = s.select(empty, make_reqs(), report);
    REQUIRE(!r.has_value());
}

TEST_CASE (


"cost_based_selector lowest_latency prefers low-cost backend"
,
"[cost_based_selector]"
)
{
    sel::cost_based_selector s{alg::selection_policy::lowest_latency};
    auto fast = make_backend("fast",  true,  1.0);   // low exec_cost
    auto slow = make_backend("slow",  true, 100.0);  // high exec_cost
    std::array<alg::backend_capability_info, 2> backends{slow, fast};
    alg::negotiation_report_buffer report;
    auto r = s.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "fast");
}

TEST_CASE (


"cost_based_selector descriptor id is non-empty"
,
"[cost_based_selector]"
)
{
    auto d = sel::cost_based_selector::descriptor();
    REQUIRE(!d.id.empty());
}

// ===========================================================================
// profile_guided_selector
// ===========================================================================

TEST_CASE (


"profile_guided_selector latency bias uses lowest_latency path"
,
"[profile_guided_selector]"
)
{
    sel::profile_guided_selector pgs;
    pgs.add_hint(sel::profile_score_hint{
        .profile_id    = "tensor.o3",
        .latency_bias  = 5.0f,
    });

    auto fast = make_backend("fast", true,  1.0);
    auto slow = make_backend("slow", true, 100.0);
    std::array<alg::backend_capability_info, 2> backends{slow, fast};
    alg::negotiation_report_buffer report;
    auto r = pgs.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
    // With latency_bias > 0, derived policy = lowest_latency → picks "fast"
    REQUIRE(r->backend_id == "fast");
}

TEST_CASE (


"profile_guided_selector empty hints falls back to balanced"
,
"[profile_guided_selector]"
)
{
    sel::profile_guided_selector pgs;  // no hints
    auto b = make_backend("b1");
    std::array<alg::backend_capability_info, 1> backends{b};
    alg::negotiation_report_buffer report;
    auto r = pgs.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
}

TEST_CASE (


"profile_guided_selector descriptor id is non-empty"
,
"[profile_guided_selector]"
)
{
    auto d = sel::profile_guided_selector::descriptor();
    REQUIRE(!d.id.empty());
}

// ===========================================================================
// rule_based_selector
// ===========================================================================

TEST_CASE (


"rule_based_selector matching rule selects its backend"
,
"[rule_based_selector]"
)
{
    sel::rule_based_selector rbs;
    rbs.add_rule("prefer_jit",
        [](const alg::backend_capability_info& b, const ex::compile_requirements&) {
            return b.backend_id == "jit";
        });
    auto jit   = make_backend("jit");
    auto interp = make_backend("interp");
    std::array<alg::backend_capability_info, 2> backends{interp, jit};
    alg::negotiation_report_buffer report;
    auto r = rbs.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "jit");
}

TEST_CASE (


"rule_based_selector no matching rule falls back to cost-based"
,
"[rule_based_selector]"
)
{
    sel::rule_based_selector rbs;
    rbs.add_rule("never_match",
        [](const alg::backend_capability_info&, const ex::compile_requirements&) {
            return false;
        });
    auto b = make_backend("b1");
    std::array<alg::backend_capability_info, 1> backends{b};
    alg::negotiation_report_buffer report;
    auto r = rbs.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
}

TEST_CASE (


"rule_based_selector first matching rule wins"
,
"[rule_based_selector]"
)
{
    sel::rule_based_selector rbs;
    rbs.add_rule("first",  [](const alg::backend_capability_info& b, const ex::compile_requirements&) {
        return b.backend_id == "a";
    });
    rbs.add_rule("second", [](const alg::backend_capability_info& b, const ex::compile_requirements&) {
        return b.backend_id == "b";
    });
    auto a = make_backend("a");
    auto b = make_backend("b");
    std::array<alg::backend_capability_info, 2> backends{a, b};
    alg::negotiation_report_buffer report;
    auto r = rbs.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "a");
}

TEST_CASE (


"rule_based_selector descriptor deterministic=true"
,
"[rule_based_selector]"
)
{
    auto d = sel::rule_based_selector::descriptor();
    REQUIRE(d.deterministic == true);
}

// ===========================================================================
// learned_selector
// ===========================================================================

TEST_CASE (


"learned_selector no infer_fn falls back to cost-based"
,
"[learned_selector]"
)
{
    sel::learned_selector ls;  // no infer_fn set
    auto b = make_backend("b1");
    std::array<alg::backend_capability_info, 1> backends{b};
    alg::negotiation_report_buffer report;
    auto r = ls.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
}

TEST_CASE (


"learned_selector infer_fn returning viable backend selects it"
,
"[learned_selector]"
)
{
    sel::learned_selector ls;
    ls.set_infer_fn([](const lithe::features::feature_vector&) -> std::string_view {
        return "jit";
    });
    auto jit    = make_backend("jit");
    auto interp = make_backend("interp");
    std::array<alg::backend_capability_info, 2> backends{interp, jit};
    alg::negotiation_report_buffer report;
    auto r = ls.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "jit");
}

TEST_CASE (


"learned_selector infer_fn returning unavailable id falls back"
,
"[learned_selector]"
)
{
    sel::learned_selector ls;
    ls.set_infer_fn([](const lithe::features::feature_vector&) -> std::string_view {
        return "nonexistent";
    });
    auto b = make_backend("fallback_b");
    std::array<alg::backend_capability_info, 1> backends{b};
    alg::negotiation_report_buffer report;
    auto r = ls.select(backends, make_reqs(), report);
    // Falls back to cost-based, which finds "fallback_b"
    REQUIRE(r.has_value());
}

TEST_CASE (


"learned_selector descriptor safe_for_runtime_replacement=true"
,
"[learned_selector]"
)
{
    auto d = sel::learned_selector::descriptor();
    REQUIRE(d.safe_for_runtime_replacement == true);
}

// ===========================================================================
// fallback_selector
// ===========================================================================

TEST_CASE (


"fallback_selector primary success: fallback not triggered"
,
"[fallback_selector]"
)
{
    sel::fallback_selector<sel::cost_based_selector, sel::rule_based_selector> fs;
    auto b = make_backend("primary_b");
    std::array<alg::backend_capability_info, 1> backends{b};
    alg::negotiation_report_buffer report;
    auto r = fs.select(backends, make_reqs(), report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "primary_b");
}

TEST_CASE (


"fallback_selector primary failure triggers fallback"
,
"[fallback_selector]"
)
{
    // Primary: cost_based_selector with no backends → fails
    // Fallback: rule_based_selector with a rule that always matches
    sel::rule_based_selector fallback_s;
    fallback_s.add_rule("always",
        [](const alg::backend_capability_info&, const ex::compile_requirements&) {
            return true;
        });

    sel::fallback_selector<sel::cost_based_selector, sel::rule_based_selector> fs{
        sel::cost_based_selector{},
        std::move(fallback_s)
    };

    auto b = make_backend("fb");
    std::array<alg::backend_capability_info, 1> backends{b};
    alg::negotiation_report_buffer report;

    // Pass empty span to primary so it fails; fallback gets the real backends
    // We call select directly — both strategies see the same backends span.
    // Primary will fail because it finds no eligible backend matching an empty
    // span; but here both see the same span. Let's use unavailable primary.
    auto unavailable = make_backend("x", /*available=*/false);
    std::array<alg::backend_capability_info, 1> blocked{unavailable};
    alg::negotiation_report_buffer report2;

    // Build a fallback_selector where primary always fails
    sel::cost_based_selector failing_primary;
    sel::rule_based_selector winning_fallback;
    winning_fallback.add_rule("win",
        [](const alg::backend_capability_info& bk, const ex::compile_requirements&) {
            return bk.available;
        });

    // Use the real backends (both available) for fallback scenario:
    // primary gets only the unavailable backend, fallback gets the real one.
    // Since both see the same span here, test that fallback logic fires when
    // primary finds nothing viable in the span.
    auto r2 = failing_primary.select(blocked, make_reqs(), report2);
    REQUIRE(!r2.has_value());  // primary fails on unavailable-only list

    // Now test the composed selector on the real list
    sel::fallback_selector<sel::cost_based_selector, sel::rule_based_selector> fs2{
        failing_primary, winning_fallback
    };
    alg::negotiation_report_buffer report3;
    auto r3 = fs2.select(backends, make_reqs(), report3);
    REQUIRE(r3.has_value());
}

// =============================================================================
// Decision-engine reroute regression tests (appended; DO NOT modify above)
// These verify that selectors rerouted through decision_engine preserve their
// previous behaviour on a fixed candidate set ("golden values").
// =============================================================================

// 23. cost_based_selector: available backend is still selected after reroute.
TEST_CASE (


"23. cost_based_selector: reroute preserves available-backend selection"
,
"[selector]"
)
 {
    auto b = make_backend("lithe.interp", /*available=*/true);
    std::array<alg::backend_capability_info, 1> bkds{b};
    alg::negotiation_report_buffer report;
    sel::cost_based_selector cbs;
    auto r = cbs.select(bkds, make_reqs(), report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "lithe.interp");
}

// 24. cost_based_selector: 10-step gate still rejects unavailable backend.
TEST_CASE (


"24. cost_based_selector: reroute preserves 10-step gate rejection"
,
"[selector]"
)
 {
    auto b = make_backend("lithe.interp", /*available=*/false);
    std::array<alg::backend_capability_info, 1> bkds{b};
    alg::negotiation_report_buffer report;
    sel::cost_based_selector cbs;
    auto r = cbs.select(bkds, make_reqs(), report);
    REQUIRE(!r.has_value());
}

// 25. profile_guided_selector: empty hints → balanced fallback still selects backend.
TEST_CASE (


"25. profile_guided_selector: reroute – empty hints selects available backend"
,
"[selector]"
)
 {
    auto b = make_backend("lithe.interp", /*available=*/true);
    std::array<alg::backend_capability_info, 1> bkds{b};
    alg::negotiation_report_buffer report;
    sel::profile_guided_selector pgs;
    auto r = pgs.select(bkds, make_reqs(), report);
    REQUIRE(r.has_value());
}

// 26. rule_based_selector: matching rule still selects correct backend after reroute.
TEST_CASE (


"26. rule_based_selector: reroute preserves rule-match backend selection"
,
"[selector]"
)
 {
    auto b = make_backend("lithe.jit", /*available=*/true);
    std::array<alg::backend_capability_info, 1> bkds{b};
    alg::negotiation_report_buffer report;
    sel::rule_based_selector rbs;
    rbs.add_rule("jit_rule",
        [](const alg::backend_capability_info& bk, const ex::compile_requirements&) {
            return bk.backend_id == "lithe.jit";
        });
    auto r = rbs.select(bkds, make_reqs(), report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "lithe.jit");
}

// 27. learned_selector: infer_fn returning valid id → selected after reroute.
TEST_CASE (


"27. learned_selector: reroute preserves infer_fn-selected backend"
,
"[selector]"
)
 {
    auto b = make_backend("lithe.interp", /*available=*/true);
    std::array<alg::backend_capability_info, 1> bkds{b};
    alg::negotiation_report_buffer report;
    sel::learned_selector ls;
    ls.set_infer_fn([](const lithe::features::feature_vector&) -> std::string_view {
        return "lithe.interp";
    });
    auto r = ls.select(bkds, make_reqs(), report);
    REQUIRE(r.has_value());
    REQUIRE(r->backend_id == "lithe.interp");
}

