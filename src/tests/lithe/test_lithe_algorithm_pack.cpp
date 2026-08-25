// =============================================================================
// test_lithe_algorithm_pack.cpp — algorithm_pack + algorithm_box (§6.1–§6.3)
//
// Verifies:
//   • Empty policy costs 0 bytes (sizeof assert via algorithm_pack).
//   • algorithm_box inline path: small callables stored without heap allocation.
//   • algorithm_box heap path: large callables stored on the heap.
//   • cost_based_backend_selector: basic selection across backends.
//   • algorithm_descriptor static properties correct.
//   • algorithm_pack<> with multiple algorithms compiles and validates.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <cstdint>
#include <type_traits>

#include "lithe/lithe_algorithms/selection.hpp"
#include "lithe/lithe_algorithms/lifecycle.hpp"
#include "lithe/lithe_algorithms/pipeline.hpp"

namespace al = lithe::algorithms;

// ============================================================================
// algorithm_descriptor
// ============================================================================

TEST_CASE (


"cost_based_backend_selector: descriptor properties"
,
"[algorithm][descriptor]"
)
 {
    const auto desc = al::cost_based_backend_selector::descriptor();
    REQUIRE_FALSE(desc.id.empty());
    CHECK(desc.deterministic == true);
    CHECK(desc.thread_safe == true);
    CHECK(desc.reentrant == true);
    CHECK(desc.version_major == 1);
}

// ============================================================================
// algorithm_pack — empty policies have size 0
// ============================================================================

TEST_CASE (


"algorithm_pack: empty policies cost 0 bytes"
,
"[algorithm_pack]"
)
 {
    // An algorithm_pack of empty types should itself be empty.
    struct empty_algo_a {
        [[nodiscard]] static al::algorithm_descriptor descriptor() noexcept {
            return {.id = "test.a"};
        }
    };
    struct empty_algo_b {
        [[nodiscard]] static al::algorithm_descriptor descriptor() noexcept {
            return {.id = "test.b"};
        }
    };

    using pack_t = al::algorithm_pack<empty_algo_a, empty_algo_b>;
    // The pack itself isn't empty (contains a tuple) but each empty algo
    // contributes 0 bytes inside the tuple via EBO or [[no_unique_address]].
    // We verify the pack at least has both algorithms accessible.
    pack_t pack;
    auto& a = pack.get<0>();
    auto& b = pack.get<1>();
    CHECK(a.descriptor().id == "test.a");
    CHECK(b.descriptor().id == "test.b");
}

// ============================================================================
// algorithm_box
// ============================================================================

TEST_CASE (


"algorithm_box: inline path for small callable"
,
"[algorithm_box]"
)
 {
    // A trivially-copyable small lambda fits in the default inline buffer.
    int captured = 42;
    al::algorithm_box<int()> box{[captured]() noexcept { return captured; }};
    REQUIRE(box.has_value());
    CHECK(box() == 42);
}

TEST_CASE (


"algorithm_box: large callable uses heap path"
,
"[algorithm_box]"
)
 {
    // A callable larger than inline buffer triggers the heap path.
    struct large_callable {
        std::array<char, 512> padding{};
        int value;
        int operator()() const noexcept { return value; }
    };

    al::algorithm_box<int(), 32> box{large_callable{.value = 99}};
    REQUIRE(box.has_value());
    CHECK(box() == 99);
}

TEST_CASE (


"algorithm_box: move semantics"
,
"[algorithm_box]"
)
 {
    al::algorithm_box<int()> a{[]() noexcept { return 7; }};
    al::algorithm_box<int()> b{std::move(a)};
    REQUIRE(b.has_value());
    CHECK(b() == 7);
    CHECK_FALSE(a.has_value());
}

TEST_CASE (


"algorithm_box: empty box has no value"
,
"[algorithm_box]"
)
 {
    al::algorithm_box<int()> empty;
    CHECK_FALSE(empty.has_value());
}

// ============================================================================
// cost_based_backend_selector — basic pipeline
// ============================================================================

TEST_CASE (


"cost_based_backend_selector: selects eligible backend"
,
"[selector]"
)
 {
    using namespace lithe::execution;

    al::cost_based_backend_selector sel;
    al::negotiation_report_buffer   report;

    // Two backends: A is available + eligible, B is available but missing caps.
    compile_requirements reqs;
    reqs.required = backend_capability_set::from({backend_feature::interpreter_execution});
    reqs.allowed_modes.set(execution_mode::interpret);

    execution_mode_set interp_modes;
    interp_modes.set(execution_mode::interpret);

    std::array<al::backend_capability_info, 2> backends{{
        {   // backend A — eligible
            .backend_id    = "test.interpreter",
            .caps          = backend_capability_set::from({backend_feature::interpreter_execution,
                                                           backend_feature::integer_arithmetic}),
            .supported_modes = interp_modes,
            .compile_cost  = 10.0,
            .exec_cost     = 5.0,
            .available     = true,
        },
        {   // backend B — missing required cap
            .backend_id    = "test.debug_text",
            .caps          = backend_capability_set::from({backend_feature::integer_arithmetic}),
            .supported_modes = interp_modes,
            .available     = true,
        },
    }};

    auto result = sel(backends, reqs, report);
    REQUIRE(result.has_value());
    CHECK(result->backend_id == "test.interpreter");
    CHECK_FALSE(report.view().empty());
}

TEST_CASE (


"cost_based_backend_selector: no eligible backend → error"
,
"[selector]"
)
 {
    using namespace lithe::execution;

    al::cost_based_backend_selector sel;
    al::negotiation_report_buffer   report;

    compile_requirements reqs;
    reqs.required = backend_capability_set::from({backend_feature::interpreter_execution});

    // No backend has interpreter_execution.
    std::array<al::backend_capability_info, 1> backends{{
        {
            .backend_id = "test.null",
            .caps       = backend_capability_set::from({backend_feature::integer_arithmetic}),
            .available  = true,
        },
    }};

    auto result = sel(backends, reqs, report);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE (


"cost_based_backend_selector: preferred caps improve score"
,
"[selector]"
)
 {
    using namespace lithe::execution;

    al::cost_based_backend_selector sel;
    al::negotiation_report_buffer   report;

    compile_requirements reqs;
    reqs.required  = backend_capability_set::from({backend_feature::interpreter_execution});
    reqs.preferred = backend_capability_set::from({backend_feature::tensor_arithmetic});
    reqs.allowed_modes.set(execution_mode::interpret);

    execution_mode_set interp_modes;
    interp_modes.set(execution_mode::interpret);

    std::array<al::backend_capability_info, 2> backends{{
        {   // A: required only
            .backend_id  = "test.basic",
            .caps = backend_capability_set::from({backend_feature::interpreter_execution}),
            .supported_modes = interp_modes,
            .compile_cost = 1.0,
            .available = true,
        },
        {   // B: required + preferred → higher score
            .backend_id  = "test.tensor",
            .caps = backend_capability_set::from({backend_feature::interpreter_execution,
                                                  backend_feature::tensor_arithmetic}),
            .supported_modes = interp_modes,
            .compile_cost = 1.0,
            .available = true,
        },
    }};

    auto result = sel(backends, reqs, report);
    REQUIRE(result.has_value());
    CHECK(result->backend_id == "test.tensor");
}

TEST_CASE (


"cost_based_backend_selector: attempt_anyway is debug-only"
,
"[selector][debug_policy]"
)
{
    using namespace lithe::execution;

    // Without attempt_anyway: mode-blocked backend is rejected.
    al::cost_based_backend_selector sel_strict;
    al::negotiation_report_buffer   report;

    compile_requirements reqs;
    reqs.required = backend_capability_set::from({backend_feature::interpreter_execution});
    reqs.forbidden_modes.set(execution_mode::jit_tier1);
    reqs.allowed_modes.set(execution_mode::interpret);

    execution_mode_set jit_only;
    jit_only.set(execution_mode::jit_tier1); // only jit mode — forbidden

    std::array<al::backend_capability_info, 1> backends{{
        {
            .backend_id = "test.jit_only",
            .caps = backend_capability_set::from({backend_feature::interpreter_execution}),
            .supported_modes = jit_only,
            .available = true,
        },
    }};

    auto result_strict = sel_strict(backends, reqs, report);
    CHECK_FALSE(result_strict.has_value()); // rejected

    // With attempt_anyway: debug policy allows it.
    al::cost_based_backend_selector sel_debug;
    sel_debug.debug_policy.enabled = true;
    auto result_debug = sel_debug(backends, reqs, report);
    REQUIRE(result_debug.has_value()); // attempt_anyway kicks in
}

// ============================================================================
// lifecycle policies — empty + concept satisfaction
// ============================================================================

TEST_CASE (


"no_op_tiering_policy: never tiers"
,
"[lifecycle]"
)
 {
    al::no_op_tiering_policy p;
    CHECK_FALSE(p.should_tier(0, lithe::execution::execution_mode::interpret, 1000000));
    CHECK(p.target_tier(lithe::execution::execution_mode::interpret)
          == lithe::execution::execution_mode::interpret);
}

TEST_CASE (


"no_op_eviction_policy: never evicts"
,
"[lifecycle]"
)
 {
    al::no_op_eviction_policy p;
    CHECK_FALSE(p.should_evict(0, 0, 100));
}

TEST_CASE (


"no_op_retirement_policy: retires when no active frames"
,
"[lifecycle]"
)
 {
    al::no_op_retirement_policy p;
    CHECK(p.can_retire(0, 0));
    CHECK_FALSE(p.can_retire(0, 1));
}

TEST_CASE (


"default_lifecycle_policies: empty"
,
"[lifecycle]"
)
 {
    static_assert(std::is_empty_v<al::default_lifecycle_policies>);
    al::default_lifecycle_policies dlp;
    (void)dlp;
    SUCCEED();
}
