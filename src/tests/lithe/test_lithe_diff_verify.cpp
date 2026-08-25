// =============================================================================
// test_lithe_diff_verify.cpp — §5.2 differential_verifier
//
// Verifies:
//   • Two identical backends → verify_result::passed == true.
//   • One divergent backend → reported mismatch diagnostic (passed == false).
//   • Floating-point comparison uses tolerance.
//   • Void-result callables: always pass.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_diff_verify.hpp"

using namespace lithe::verify;

// ============================================================================
// Integer: oracle == backend → pass
// ============================================================================

TEST_CASE (


"differential_verifier: identical backends pass"
,
"[diff_verify]"
)
 {
    differential_verifier dv;

    auto oracle  = [](const int& ir) { return ir * 2; };
    auto backend = [](const int& ir) { return ir * 2; };

    auto result = dv.run(42, oracle, backend);
    REQUIRE(result.ok());
    REQUIRE(result.mismatch_count() == 0);
}

// ============================================================================
// Integer: divergent backend → mismatch reported
// ============================================================================

TEST_CASE (


"differential_verifier: divergent backend reports mismatch"
,
"[diff_verify]"
)
 {
    differential_verifier dv;

    auto oracle  = [](const int& ir) { return ir * 2; };
    auto broken  = [](const int& ir) { return ir * 3; };  // wrong result

    auto result = dv.run(10, oracle, broken);
    REQUIRE(!result.ok());
    REQUIRE(result.mismatch_count() == 1);

    // The mismatch diagnostic carries an error-level severity.
    REQUIRE(result.mismatches[0].diag.level == lithe::diag::severity::error);
    REQUIRE(!result.mismatches[0].description.empty());
}

// ============================================================================
// Two backends: first ok, second broken
// ============================================================================

TEST_CASE (


"differential_verifier: one of two backends diverges"
,
"[diff_verify]"
)
 {
    differential_verifier dv;

    auto oracle   = [](const int& v) { return v + 1; };
    auto good     = [](const int& v) { return v + 1; };
    auto bad      = [](const int& v) { return v + 2; };

    auto result = dv.run_tuple(5, oracle,
                               std::forward_as_tuple(good, bad));
    REQUIRE(!result.ok());
    REQUIRE(result.mismatch_count() == 1);
    REQUIRE(result.mismatches[0].backend_index == 1);
}

// ============================================================================
// Float: within tolerance → pass
// ============================================================================

TEST_CASE (


"differential_verifier: float within tolerance passes"
,
"[diff_verify]"
)
 {
    differential_verifier dv;
    dv.tolerance = 1e-5;

    auto oracle  = [](const double& x) { return x * 3.14; };
    auto approx  = [](const double& x) { return x * 3.14 + 1e-10; }; // tiny diff

    auto result = dv.run(1.0, oracle, approx);
    REQUIRE(result.ok());
}

// ============================================================================
// Float: beyond tolerance → mismatch
// ============================================================================

TEST_CASE (


"differential_verifier: float beyond tolerance fails"
,
"[diff_verify]"
)
 {
    differential_verifier dv;
    dv.tolerance = 1e-9;

    auto oracle = [](const double& x) { return x; };
    auto bad    = [](const double& x) { return x + 1.0; }; // 1.0 >> tolerance

    auto result = dv.run(1.0, oracle, bad);
    REQUIRE(!result.ok());
    REQUIRE(result.mismatch_count() == 1);
}

// ============================================================================
// Void result: always passes
// ============================================================================

TEST_CASE (


"differential_verifier: void callables always pass"
,
"[diff_verify]"
)
 {
    differential_verifier dv;

    auto oracle  = [](const int&) {};
    auto backend = [](const int&) {};

    auto result = dv.run(99, oracle, backend);
    REQUIRE(result.ok());
    REQUIRE(result.mismatch_count() == 0);
}

// ============================================================================
// verify_result default state
// ============================================================================

TEST_CASE (


"verify_result: default state is pass"
,
"[diff_verify]"
)
 {
    verify_result r;
    REQUIRE(r.ok());
    REQUIRE(r.mismatch_count() == 0);
}
