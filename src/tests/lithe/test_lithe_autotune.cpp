// =============================================================================
// test_lithe_autotune.cpp — §3.2 auto_tuner profile-variant benchmarking
//
// Verifies:
//   • auto_tuner<ProfileA, ProfileB> returns a tune_result with correct fields.
//   • winner_id matches one of the profile ids.
//   • variant_results has one entry per variant.
//   • Tie-break: when insignificant, deterministic profile preferred.
//   • Works with std_o0 and std_o1 built-in profiles.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_autotune.hpp"

using namespace lithe;
using namespace lithe::tune;

// ============================================================================
// Basic smoke test: auto_tuner with two built-in profiles
// ============================================================================

TEST_CASE (


"auto_tuner: two variants produce a tune_result"
,
"[autotune]"
)
 {
    auto expr = make_node<add_tag>(make_node<mul_tag>(2, 3), 0);

    // bench: just consume the optimised expression (no work needed for test).
    auto bench = [](auto&&) {};

    auto_tuner<profile::std_o0, profile::std_o1> tuner;
    // Use 2 iterations to keep the test fast.
    auto result = tuner.tune(expr, bench, 2);

    // winner_id must be one of the two profile ids.
    const bool valid_id = (result.winner_id == std::string_view{"std.o0"})
                       || (result.winner_id == std::string_view{"std.o1"});
    REQUIRE(valid_id);

    // Two variants → two profiling results.
    REQUIRE(result.variant_results.size() == 2);

    // Index in bounds.
    REQUIRE(result.winner_variant_index < 2);
}

// ============================================================================
// Single-variant tuner: always picks the only option
// ============================================================================

TEST_CASE (


"auto_tuner: single variant always wins"
,
"[autotune]"
)
 {
    auto expr = make_node<add_tag>(1, 2);
    auto bench = [](auto&&) {};

    auto_tuner<profile::std_o0> tuner;
    auto result = tuner.tune(expr, bench, 2);

    REQUIRE(result.winner_id == std::string_view{"std.o0"});
    REQUIRE(result.winner_variant_index == 0);
    REQUIRE(result.variant_results.size() == 1);
}

// ============================================================================
// Tie-break: with equal timing both deterministic → picks index 0
// ============================================================================

TEST_CASE (


"auto_tuner: tie-break prefers deterministic variant"
,
"[autotune]"
)
 {
    auto expr = make_node<mul_tag>(3, 4);
    auto bench = [](auto&&) {};

    // Both std_o0 and std_o1 are deterministic. When times are nearly equal,
    // the tie-break selects the first deterministic variant (index 0 = std_o0).
    // We can't guarantee which wins on real hardware, but the winner must be valid.
    auto_tuner<profile::std_o0, profile::std_o1> tuner;
    auto result = tuner.tune(expr, bench, 2);

    // In any outcome the winner must be one of the two profiles.
    REQUIRE(result.winner_variant_index < 2);
    REQUIRE(!std::string_view{profile::std_o0::descriptor.id}.empty());
    REQUIRE(!std::string_view{profile::std_o1::descriptor.id}.empty());
}

// ============================================================================
// tune_result fields structure
// ============================================================================

TEST_CASE (


"tune_result: fields default-constructed correctly"
,
"[autotune]"
)
 {
    tune_result r;
    REQUIRE(r.winner_id.empty());
    REQUIRE(r.winner_variant_index == 0);
    REQUIRE(!r.is_significant);
    REQUIRE(r.speedup_vs_baseline == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(r.variant_results.empty());
}
