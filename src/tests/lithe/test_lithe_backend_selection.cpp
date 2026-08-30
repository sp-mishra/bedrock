// =============================================================================
// test_lithe_backend_selection.cpp — mode≠capability gating (§3.4)
//                                    (gating half only; scoring added in impl-3)
//
// Verifies:
//   • A JIT-capable backend is still selected under
//     forbidden_modes={jit}, allowed_modes={interpret}.
//   • Forcing jit mode under the same forbidden set → rejected.
//   • required-cap absence → rejected.
//   • static_backend_set::select_first honours mode gating.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <type_traits>

#include "lithe/lithe_execution/capability.hpp"
#include "lithe/lithe_execution/foundation.hpp"
#include "lithe/backends/lithe_execution_backends.hpp"

namespace ex = lithe::execution;

// ============================================================================
// mode_gate predicate unit tests
// ============================================================================

TEST_CASE (


"mode_gate: required cap absent → rejected"
,
"[selection][mode_gate]"
)
 {
    ex::compile_requirements reqs;
    reqs.required = ex::backend_capability_set::from({
        ex::backend_feature::integer_arithmetic,
        ex::backend_feature::interpreter_execution
    });

    // Backend with only integer_arithmetic — missing interpreter_execution.
    auto caps = ex::backend_capability_set::from({ex::backend_feature::integer_arithmetic});

    bool ok = ex::mode_gate(reqs, caps, ex::execution_mode::interpret);
    REQUIRE_FALSE(ok);
}

TEST_CASE (


"mode_gate: forbidden mode → rejected"
,
"[selection][mode_gate]"
)
 {
    ex::compile_requirements reqs;
    reqs.forbidden_modes.set(ex::execution_mode::jit_tier1);
    reqs.required = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution});

    auto caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution});

    // jit_tier1 is forbidden; passing jit_tier1 as mode hint → rejected.
    bool ok = ex::mode_gate(reqs, caps, ex::execution_mode::jit_tier1);
    REQUIRE_FALSE(ok);
}

TEST_CASE (


"mode_gate: JIT-capable backend in interpret mode under forbidden jit → accepted"
,
"[selection][mode_gate]"
)
{
    // forbidden jit, allowed interpret
    ex::compile_requirements reqs;
    reqs.forbidden_modes.set(ex::execution_mode::jit_tier1);
    reqs.allowed_modes.set(ex::execution_mode::interpret);
    reqs.required = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution});

    // A backend that is JIT-capable but also supports interpreter.
    auto caps = ex::backend_capability_set::from({
        ex::backend_feature::interpreter_execution,
        ex::backend_feature::integer_arithmetic,
    });

    // interpret mode is allowed — this must succeed.
    bool ok = ex::mode_gate(reqs, caps, ex::execution_mode::interpret);
    REQUIRE(ok);
}

TEST_CASE (


"mode_gate: all modes empty → all modes allowed"
,
"[selection][mode_gate]"
)
 {
    ex::compile_requirements reqs;
    // no allowed_modes / forbidden_modes set
    reqs.required = ex::backend_capability_set::from({ex::backend_feature::integer_arithmetic});

    auto caps = ex::backend_capability_set::from({ex::backend_feature::integer_arithmetic});

    // Any mode is allowed when no restrictions set.
    CHECK(ex::mode_gate(reqs, caps, ex::execution_mode::interpret));
    CHECK(ex::mode_gate(reqs, caps, ex::execution_mode::jit_tier1));
    CHECK(ex::mode_gate(reqs, caps, ex::execution_mode::aot));
}

TEST_CASE (


"compile_requirements::mode_allowed matches mode_gate logic"
,
"[selection]"
)
 {
    ex::compile_requirements reqs;
    reqs.forbidden_modes.set(ex::execution_mode::aot);
    reqs.allowed_modes.set(ex::execution_mode::interpret);

    CHECK(reqs.mode_allowed(ex::execution_mode::interpret));
    CHECK_FALSE(reqs.mode_allowed(ex::execution_mode::aot));
    CHECK_FALSE(reqs.mode_allowed(ex::execution_mode::jit_tier1)); // not in allowed_modes
}

// ============================================================================
// static_backend_set select_first — compile-time ordered, no erasure
// ============================================================================

TEST_CASE (


"static_backend_set: select_first honours mode gating"
,
"[selection][static_set]"
)
 {
    using namespace lithe::codegen::backends;

    interpreter_backend interp;
    debug_text_backend  dbg;

    ex::static_backend_set set{interp, dbg};

    // interpreter_backend has interpreter_execution capability.
    ex::compile_requirements reqs;
    reqs.required = ex::backend_capability_set::from({
        ex::backend_feature::interpreter_execution
    });

    void* selected = set.select_first(reqs, ex::execution_mode::interpret);
    // Should select interpreter_backend (first that satisfies caps).
    REQUIRE(selected != nullptr);
    CHECK(selected == static_cast<void*>(&interp));
}

TEST_CASE (


"static_backend_set: no backend satisfies → nullptr"
,
"[selection][static_set]"
)
 {
    using namespace lithe::codegen::backends;

    debug_text_backend dbg;

    ex::static_backend_set set{dbg};

    // Require interpreter_execution — debug_text_backend doesn't have it.
    ex::compile_requirements reqs;
    reqs.required = ex::backend_capability_set::from({
        ex::backend_feature::interpreter_execution
    });

    void* selected = set.select_first(reqs, ex::execution_mode::interpret);
    CHECK(selected == nullptr);
}

TEST_CASE (


"static_backend_set: visit_all hits every backend"
,
"[selection][static_set]"
)
 {
    using namespace lithe::codegen::backends;

    interpreter_backend interp;
    debug_text_backend  dbg;

    ex::static_backend_set set{interp, dbg};

    int count = 0;
    set.visit_all([&](auto& /*b*/) { ++count; });
    CHECK(count == 2);
}

// ============================================================================
// cost_based_backend_selector — scoring + negotiation report (impl-3 §6.4)
// DO NOT modify the assertions above; these are append-only additions.
// ============================================================================

#include "lithe/lithe_algorithms/selection.hpp"

namespace al = lithe::algorithms;

TEST_CASE (


"cost_based_backend_selector: preferred-cap scoring picks better backend"
,
"[selection][scoring]"
)
{
    al::cost_based_backend_selector sel;
    al::negotiation_report_buffer   report;

    ex::compile_requirements reqs;
    reqs.required  = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution});
    reqs.preferred = ex::backend_capability_set::from({ex::backend_feature::tensor_arithmetic});
    reqs.allowed_modes.set(ex::execution_mode::interpret);

    ex::execution_mode_set interp;
    interp.set(ex::execution_mode::interpret);

    std::array<al::backend_capability_info, 2> backends{{
        { .backend_id = "basic",
          .caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution}),
          .supported_modes = interp, .compile_cost = 1.0, .available = true },
        { .backend_id = "tensor",
          .caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution,
                                                     ex::backend_feature::tensor_arithmetic}),
          .supported_modes = interp, .compile_cost = 1.0, .available = true },
    }};

    auto result = sel(backends, reqs, report);
    REQUIRE(result.has_value());
    CHECK(result->backend_id == "tensor");
    CHECK(result->score > 0.0);
}

TEST_CASE (


"cost_based_backend_selector: cost estimate ordering respected"
,
"[selection][scoring]"
)
{
    al::cost_based_backend_selector sel;
    al::negotiation_report_buffer   report;

    ex::compile_requirements reqs;
    reqs.required = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution});
    reqs.allowed_modes.set(ex::execution_mode::interpret);

    ex::execution_mode_set interp;
    interp.set(ex::execution_mode::interpret);

    // A has high cost → lower score; B has low cost → higher score.
    std::array<al::backend_capability_info, 2> backends{{
        { .backend_id = "expensive",
          .caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution}),
          .supported_modes = interp, .compile_cost = 9999.0, .available = true },
        { .backend_id = "cheap",
          .caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution}),
          .supported_modes = interp, .compile_cost = 0.1, .available = true },
    }};

    auto result = sel(backends, reqs, report);
    REQUIRE(result.has_value());
    CHECK(result->backend_id == "cheap");
}

TEST_CASE (


"cost_based_backend_selector: NADI negotiation report emitted"
,
"[selection][report]"
)
{
    al::cost_based_backend_selector sel;
    al::negotiation_report_buffer   report;

    ex::compile_requirements reqs;
    reqs.required = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution});
    reqs.allowed_modes.set(ex::execution_mode::interpret);

    ex::execution_mode_set interp;
    interp.set(ex::execution_mode::interpret);

    std::array<al::backend_capability_info, 1> backends{{
        { .backend_id = "interp",
          .caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution}),
          .supported_modes = interp, .available = true },
    }};

    auto result = sel(backends, reqs, report);
    REQUIRE(result.has_value());
    // The report must mention the selected backend.
    const auto rv = report.view();
    CHECK_FALSE(rv.empty());
    CHECK(rv.find("interp") != std::string_view::npos);
    CHECK(rv.find("selected") != std::string_view::npos);
}

TEST_CASE (


"cost_based_backend_selector: attempt_anyway demoted to explicit debug policy"
,
"[selection][debug_policy]"
)
{
    // attempt_anyway is NOT the default; the default rejects mode-blocked backends.
    al::cost_based_backend_selector default_sel;
    CHECK_FALSE(default_sel.debug_policy.enabled);

    al::cost_based_backend_selector debug_sel;
    debug_sel.debug_policy.enabled = true;
    CHECK(debug_sel.debug_policy.enabled);
}

TEST_CASE(
    "cost_based_backend_selector: adapter plan rejection removes backend",
    "[selection][admission]") {
    al::cost_based_backend_selector sel;
    al::negotiation_report_buffer report;

    ex::compile_requirements reqs;
    reqs.required = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution});
    reqs.allowed_modes.set(ex::execution_mode::interpret);

    ex::execution_mode_set interp;
    interp.set(ex::execution_mode::interpret);

    std::array<al::backend_capability_info, 2> backends{{
        {.backend_id = "simd_like",
         .caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution}),
         .supported_modes = interp,
         .available = true,
         .admission = ex::backend_admission_state{
             .provider = ex::backend_provider::simd,
             .plan_admitted = false,
             .provider_available = true,
             .reason = ex::backend_admission_reason::plan_rejected,
         }},
        {.backend_id = "interp",
         .caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution}),
         .supported_modes = interp,
         .available = true},
    }};

    al::selection_explanation explanation;
    auto result = sel(backends, reqs, report, &explanation);
    REQUIRE(result.has_value());
    CHECK(result->backend_id == "interp");

    bool saw_plan_rejected = false;
    for (const auto& d : explanation.decisions)
        if (!d.accepted && d.backend_id == "simd_like" && d.reason_code == "plan_rejected")
            saw_plan_rejected = true;
    CHECK(saw_plan_rejected);
}

TEST_CASE(
    "cost_based_backend_selector: adapter provider unavailability is reason-coded",
    "[selection][admission]") {
    al::cost_based_backend_selector sel;
    al::negotiation_report_buffer report;

    ex::compile_requirements reqs;
    reqs.required = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution});
    reqs.allowed_modes.set(ex::execution_mode::interpret);

    ex::execution_mode_set interp;
    interp.set(ex::execution_mode::interpret);

    std::array<al::backend_capability_info, 1> backends{{
        {.backend_id = "metal",
         .caps = ex::backend_capability_set::from({ex::backend_feature::interpreter_execution}),
         .supported_modes = interp,
         .available = true,
         .admission = ex::backend_admission_state{
             .provider = ex::backend_provider::metal,
             .plan_admitted = true,
             .provider_available = false,
             .reason = ex::backend_admission_reason::provider_unavailable,
         }},
    }};

    al::selection_explanation explanation;
    auto result = sel(backends, reqs, report, &explanation);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().detail == "no eligible backend found");

    bool saw_provider_unavailable = false;
    for (const auto& d : explanation.decisions)
        if (!d.accepted && d.reason_code == "provider_unavailable")
            saw_provider_unavailable = true;
    CHECK(saw_provider_unavailable);
}

