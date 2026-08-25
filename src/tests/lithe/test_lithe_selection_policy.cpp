// =============================================================================
// test_lithe_selection_policy.cpp — §5.1 selection_policy + selection_explanation
//
// Verifies:
//   • lowest_latency vs highest_throughput pick different backends on a rigged
//     capability set.
//   • selection_explanation lists a reject reason for a dropped backend.
//   • Default (balanced) policy preserves prior behavior (back-compat).
//   • selection_explanation.winner_id populated on success.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_algorithms/selection.hpp"
#include "lithe/lithe_execution/foundation.hpp"
#include "lithe/lithe_execution/capability.hpp"

namespace alg = lithe::algorithms;
namespace ex = lithe::execution;

// ============================================================================
// Helper: build two rigged backend_capability_info entries
//   "fast"   — high exec_cost (paradoxically, lower latency via lower cost val),
//               does NOT have preferred caps
//   "strong" — has preferred caps, higher cost
// ============================================================================

static ex::compile_requirements make_reqs_with_preferred() {
    ex::compile_requirements r;
    r.required = ex::backend_capability_set{};
    r.preferred = ex::backend_capability_set::from(
        {ex::backend_feature::integer_arithmetic});
    return r;
}

static std::vector<alg::backend_capability_info> make_two_backends() {
    alg::backend_capability_info fast;
    fast.backend_id = "fast";
    fast.caps = ex::backend_capability_set{};
    fast.supported_modes.set(ex::execution_mode::interpret);
    fast.compile_cost = 1.0;
    fast.exec_cost = 1.0; // lowest cost → wins on lowest_latency
    fast.transfer_cost = 1.0;
    fast.ir_compatible = true;
    fast.services_ok = true;
    fast.security_ok = true;
    fast.artifact_ok = true;
    fast.available = true;

    alg::backend_capability_info strong;
    strong.backend_id = "strong";
    strong.caps = ex::backend_capability_set::from(
        {ex::backend_feature::integer_arithmetic}); // has preferred cap
    strong.supported_modes.set(ex::execution_mode::interpret);
    strong.compile_cost = 100.0;
    strong.exec_cost = 100.0; // high cost
    strong.transfer_cost = 100.0;
    strong.ir_compatible = true;
    strong.services_ok = true;
    strong.security_ok = true;
    strong.artifact_ok = true;
    strong.available = true;

    return {fast, strong};
}

// ============================================================================
// Test: balanced (default) → strong wins (preferred cap bonus)
// ============================================================================

TEST_CASE (


"selection_policy: balanced selects backend with preferred caps"
,
"[selection_policy]"
)
{
    const auto backends = make_two_backends();
    const auto reqs     = make_reqs_with_preferred();

    alg::negotiation_report_buffer buf;
    alg::cost_based_backend_selector sel;
    sel.policy = alg::selection_policy::balanced;

    auto result = sel(std::span<const alg::backend_capability_info>{backends}, reqs, buf);
    REQUIRE(result.has_value());
    // "strong" has preferred cap → bonus 1000 > penalty from cost difference.
    REQUIRE(result->backend_id == "strong");
}

// ============================================================================
// Test: lowest_latency → "fast" wins (cost dominates)
// ============================================================================

TEST_CASE (


"selection_policy: lowest_latency picks lowest-exec-cost backend"
,
"[selection_policy]"
)
{
    const auto backends = make_two_backends();
    ex::compile_requirements reqs;   // no preferred caps set
    reqs.required = ex::backend_capability_set{};
    reqs.preferred = ex::backend_capability_set{};

    alg::negotiation_report_buffer buf;
    alg::cost_based_backend_selector sel;
    sel.policy = alg::selection_policy::lowest_latency;

    auto result = sel(std::span<const alg::backend_capability_info>{backends}, reqs, buf);
    REQUIRE(result.has_value());
    // "fast" has exec_cost=1 vs "strong" exec_cost=100 → lowest_latency picks "fast".
    REQUIRE(result->backend_id == "fast");
}

// ============================================================================
// Test: highest_throughput → "strong" wins (preferred cap bonus × 2000)
// ============================================================================

TEST_CASE (


"selection_policy: highest_throughput boosts preferred-cap backend"
,
"[selection_policy]"
)
{
    const auto backends = make_two_backends();
    const auto reqs     = make_reqs_with_preferred();

    alg::negotiation_report_buffer buf;
    alg::cost_based_backend_selector sel;
    sel.policy = alg::selection_policy::highest_throughput;

    auto result = sel(std::span<const alg::backend_capability_info>{backends}, reqs, buf);
    REQUIRE(result.has_value());
    // preferred-cap bonus is 2000 (highest_throughput) >> cost penalty ~1 → "strong" wins.
    REQUIRE(result->backend_id == "strong");
}

// ============================================================================
// Test: selection_explanation lists reject and accept
// ============================================================================

TEST_CASE (


"selection_explanation: unavailable backend is rejected"
,
"[selection_policy]"
)
{
    alg::backend_capability_info good;
    good.backend_id    = "good";
    good.supported_modes.set(ex::execution_mode::interpret);
    good.ir_compatible = true;
    good.services_ok   = true;
    good.security_ok   = true;
    good.artifact_ok   = true;
    good.available     = true;

    alg::backend_capability_info gone;
    gone.backend_id    = "gone";
    gone.available     = false;  // unavailable → should be rejected

    std::vector<alg::backend_capability_info> bks{good, gone};
    ex::compile_requirements reqs;
    reqs.required  = ex::backend_capability_set{};
    reqs.preferred = ex::backend_capability_set{};

    alg::negotiation_report_buffer buf;
    alg::selection_explanation expl;
    alg::cost_based_backend_selector sel;

    auto result = sel(std::span<const alg::backend_capability_info>{bks}, reqs, buf, &expl);
    REQUIRE(result.has_value());
    REQUIRE(result->backend_id == "good");

    // explanation: one accept (good), one reject (gone).
    const auto& decisions = expl.decisions;
    REQUIRE(decisions.size() >= 2);

    bool found_reject = false;
    for (const auto& d : decisions) {
        if (d.backend_id == "gone") {
            REQUIRE(!d.accepted);
            REQUIRE(!d.reason_code.empty());
            found_reject = true;
        }
    }
    REQUIRE(found_reject);
    REQUIRE(expl.winner_id == "good");
}

// ============================================================================
// Test: selection_explanation reject carries diag::diagnostic
// ============================================================================

TEST_CASE (


"selection_explanation: reject decision carries a diagnostic"
,
"[selection_policy]"
)
{
    alg::selection_explanation expl;
    expl.add_reject("mybackend", "caps");

    REQUIRE(expl.decisions.size() == 1);
    const auto& d = expl.decisions.front();
    REQUIRE(!d.accepted);
    REQUIRE(d.backend_id == "mybackend");
    REQUIRE(d.reason_code == "caps");
    REQUIRE(d.diag.level == lithe::diag::severity::info);
    REQUIRE(!d.diag.message.empty());
}
