#pragma once

// =============================================================================
// lithe_diff_verify.hpp — Cross-backend equivalence harness  (opt-in, tooling)
//
// Namespace:  lithe::verify
// NOT included by lithe.hpp — never on the hot path.  Tooling / CI only.
//
// Depends on: lithe/lithe_diagnostics.hpp   (diag::diagnostic, severity::error)
//             lithe/lithe_core.hpp          (Expression concept, evaluate)
//
// Provides:
//   verify_result       — pass/fail + per-mismatch diagnostics
//   differential_verifier
//       .run<IR, Sig, Args...>(ir, oracle_fn, backends..., args...)
//         Calls oracle(ir, args...) to get the reference output, then calls
//         each backend_fn(ir, args...) and compares.  Any deviation emits a
//         diag::diagnostic per mismatch.  Comparison uses operator== on the
//         result type; floating-point types use the tolerance member.
//
//   verify_equivalent(before, after, test_vectors)
//       Expression-level semantic equivalence check.  Evaluates both
//       expressions over each test vector and compares results numerically.
//       Returns verify_result; any numeric difference > tolerance is a mismatch.
//       Designed for validating rewrite rules, e-graph rewrites, and
//       optimization passes (EGraphs, Rule Packs, Tensor Rewrites, Domain Rules).
//
// Design:
//   • Never in the hot path — tooling/CI use only.
//   • Zero cost unless included.
//   • No virtual, no macros.  C++23.
// =============================================================================

#include "lithe_core.hpp"
#include "lithe_diagnostics.hpp"

#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace lithe::verify {
    // =============================================================================
    // verify_result
    // =============================================================================

    struct mismatch_record {
        std::size_t backend_index; // 0-based index into the backends list
        std::string description; // human-readable summary
        diag::diagnostic diag; // structured diagnostic (severity::error)
    };

    struct verify_result {
        bool passed = true;
        std::vector<mismatch_record> mismatches;

        [[nodiscard]] bool ok() const noexcept { return passed; }

        [[nodiscard]] std::size_t mismatch_count() const noexcept {
            return mismatches.size();
        }
    };

    // =============================================================================
    // detail: comparison helpers
    // =============================================================================

    namespace detail {
        // Exact equality for non-floating-point types.
        template <class T>
            requires (!std::floating_point<T>)
        bool values_equal(const T& a, const T& b, double /*tol*/) noexcept(noexcept(a == b)) {
            return a == b;
        }

        // Approximate equality for floating-point types.
        template <std::floating_point T>
        bool values_equal(const T& a, const T& b, double tol) noexcept {
            if (a == b) return true;
            const double diff = std::abs(static_cast<double>(a) - static_cast<double>(b));
            const double mag = std::max(std::abs(static_cast<double>(a)),
                                        std::abs(static_cast<double>(b)));
            return diff <= tol * (mag > 1.0 ? mag : 1.0);
        }

        // Void result: always matches (nothing to compare).
        inline bool values_equal(const std::monostate&, const std::monostate&, double) noexcept {
            return true;
        }

        template <class T>
        diag::diagnostic make_mismatch_diag(std::size_t backend_idx, std::string_view desc) {
            diag::diagnostic d;
            d.level = diag::severity::error;
            d.stage = diag::stage::backend;
            d.code = diag::codes::backend_capability_mismatch;
            d.message = "backend[" + std::to_string(backend_idx) + "] mismatch: "
                + std::string{desc};
            return d;
        }

        // Wrap void callables to return monostate so the comparison path is uniform.
        template <class F, class IR, class... Args>
        auto invoke_wrapped(F&& fn, const IR& ir, Args&&... args) {
            using R = std::invoke_result_t<F, const IR&, Args...>;
            if constexpr (std::is_void_v<R>) {
                std::invoke(std::forward<F>(fn), ir, std::forward<Args>(args)...);
                return std::monostate{};
            }
            else {
                return std::invoke(std::forward<F>(fn), ir, std::forward<Args>(args)...);
            }
        }
    } // namespace detail

    // =============================================================================
    // differential_verifier
    // =============================================================================

    struct differential_verifier {
        double tolerance = 1e-6; // relative tolerance for floating-point comparisons

        // run(ir, oracle, backends_tuple, args...)
        //   oracle:   callable(const IR&, Args...) → Result  (reference implementation)
        //   backends: std::tuple of callables with the same signature
        //   args:     forwarded to both oracle and each backend
        template <class IR, class Oracle, class BackendsTuple, class... Args>
        [[nodiscard]] verify_result
        run_tuple(const IR& ir, Oracle&& oracle, BackendsTuple&& backends,
                  Args&&... args) const {
            verify_result result;

            // Get oracle reference result.
            auto ref = detail::invoke_wrapped(oracle, ir, args...);

            // Compare each backend against the oracle.
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                ([&] {
                    constexpr std::size_t idx = Is;
                    auto got = detail::invoke_wrapped(std::get < idx > (backends), ir, args...);
                    using R = decltype(got);
                    if (!detail::values_equal(ref, got, tolerance)) {
                        result.passed = false;
                        std::string desc = "output differs from oracle";
                        auto d = detail::make_mismatch_diag<R>(idx, desc);
                        result.mismatches.push_back(mismatch_record{idx, std::move(desc), std::move(d)});
                    }
                }(), ...);
            }(std::make_index_sequence<std::tuple_size_v<std::decay_t<BackendsTuple>>>{});

            return result;
        }

        // Convenience: run(ir, oracle, backend_fns..., args...) — variadic helper.
        // Last variadic pack is split: first N are backends, rest are call-args.
        // Use run_tuple directly for unambiguous separation.
        template <class IR, class Oracle, class... Backends>
        [[nodiscard]] verify_result
        run(const IR& ir, Oracle&& oracle, Backends&&... backends) const {
            return run_tuple(ir, std::forward<Oracle>(oracle),
                             std::forward_as_tuple(std::forward<Backends>(backends)...));
        }
    };

    // =============================================================================
    // expression_equivalence_result
    //
    // Extended verify_result for expression-level equivalence checks.
    // Carries the test-vector index where a mismatch was first detected.
    // =============================================================================

    struct expression_mismatch {
        std::size_t vector_index; // which test vector triggered the mismatch
        double before_value; // evaluated value of 'before' expression
        double after_value; // evaluated value of 'after' expression
        diag::diagnostic diag;
    };

    struct expression_equivalence_result {
        bool passed = true;
        std::vector<expression_mismatch> mismatches;

        [[nodiscard]] bool ok() const noexcept { return passed; }
        [[nodiscard]] std::size_t count() const noexcept { return mismatches.size(); }
    };

    // =============================================================================
    // expression_verifier
    //
    // Evaluates two expressions over a set of test vectors and checks numerical
    // equivalence.  Designed for validating rewrite rules, e-graph rewrites, and
    // optimization pass correctness.
    //
    // Eval concept: callable(expr, test_vector) → double
    //   The caller supplies the evaluator because expression types are statically
    //   diverse; the verifier is agnostic to the concrete Expression type.
    //
    // Usage:
    //   expression_verifier ev;
    //   ev.tolerance = 1e-9;
    //   auto r = ev.verify(before_expr, after_expr, test_vecs, my_eval);
    //   if (!r.ok()) { /* r.mismatches[0].vector_index */ }
    // =============================================================================

    struct expression_verifier {
        double tolerance = 1e-6;

        // verify(before, after, test_vectors, eval)
        //
        //   test_vectors: any range of items V such that eval(expr, v) is valid.
        //   eval:         callable(Expr const&, V const&) → double
        //
        template <class ExprBefore, class ExprAfter,
                  class TestVectors, class Eval>
        [[nodiscard]] expression_equivalence_result
        verify(const ExprBefore& before, const ExprAfter& after,
               TestVectors&& test_vectors, Eval&& eval) const {
            expression_equivalence_result result;
            std::size_t idx = 0;
            for (const auto& vec : test_vectors) {
                const double bv = static_cast<double>(eval(before, vec));
                const double av = static_cast<double>(eval(after, vec));
                if (!detail::values_equal(bv, av, tolerance)) {
                    result.passed = false;
                    diag::diagnostic d;
                    d.level = diag::severity::error;
                    d.stage = diag::stage::optimization;
                    d.code = "lithe.verify.expression_mismatch";
                    d.message = "expressions differ at test vector " +
                        std::to_string(idx) +
                        ": before=" + std::to_string(bv) +
                        " after=" + std::to_string(av);
                    result.mismatches.push_back(
                        expression_mismatch{idx, bv, av, std::move(d)});
                }
                ++idx;
            }
            return result;
        }
    };

    // =============================================================================
    // verify_equivalent — free-function shorthand
    //
    // Convenience wrapper over expression_verifier::verify.
    //
    //   verify_equivalent(before, after, test_vectors, eval)
    //     → expression_equivalence_result
    //
    // Typical usage:
    //   auto r = lithe::verify::verify_equivalent(
    //       before_expr, after_expr,
    //       std::vector<double>{0.0, 1.0, -1.0, 2.5, 1e6},
    //       [](const auto& e, double x) {
    //           return lithe::evaluate(e, my_eval_t{x});
    //       });
    //
    // For e-graph rewrites, rule packs, tensor rewrites, and domain rules:
    //   auto r = lithe::verify::verify_equivalent(
    //       original, rewritten, test_inputs, domain_eval);
    // =============================================================================

    template <class ExprBefore, class ExprAfter,
              class TestVectors, class Eval>
    [[nodiscard]] expression_equivalence_result
    verify_equivalent(const ExprBefore& before, const ExprAfter& after,
                      TestVectors&& test_vectors, Eval&& eval,
                      double tolerance = 1e-6) {
        expression_verifier ev;
        ev.tolerance = tolerance;
        return ev.verify(before, after,
                         std::forward<TestVectors>(test_vectors),
                         std::forward<Eval>(eval));
    }
} // namespace lithe::verify
