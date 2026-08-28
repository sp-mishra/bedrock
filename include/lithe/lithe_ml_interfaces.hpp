#pragma once

// =============================================================================
// lithe_ml_interfaces.hpp — Explicit ML plug-in interface layer
//
// Namespace:  lithe::ml
// Depends on: lithe/lithe_cost_model.hpp          (cost_vector, cost_context,
//                                                  cost_estimator concept)
//             lithe/lithe_feature_extractor.hpp    (feature_vector)
//             lithe/lithe_decision_engine.hpp       (decision_strategy concept,
//                                                  ranked, candidate)
//             lithe/lithe_algorithms/selection.hpp  (algorithm_descriptor)
//             <functional>, <string_view>, <span>
//
// Provides:
//   — Explicit interface concepts:
//     feature_provider<FP,In>      — FP can extract a feature_vector from In
//     ml_cost_estimator<C,Node>    — alias: same as lithe::cost::cost_estimator
//     ml_decision_strategy<S,T>    — alias: same as lithe::intelligence::decision_strategy
//
//   — Four named implementation tags (types that carry static metadata):
//     heuristic_impl        — rule-of-thumb, deterministic, zero-allocation
//     analytical_impl       — closed-form model, deterministic, hardware-signature-aware
//     profile_guided_impl   — driven by measured performance profile data
//     learned_impl          — backed by an inference model (ML/NN/etc.)
//
//   — Corresponding adapters / skeletons:
//     heuristic_feature_provider<Extractor>  — wraps any feature_extractor
//     learned_feature_provider               — delegates to user std::function
//     heuristic_cost_estimator<CM>           — alias for scalar_cost_estimator<CM>
//     learned_cost_estimator                 — type-erased inference hook
//     heuristic_decision_strategy<T>         — wraps cost_ranking
//     learned_decision_strategy<T>           — defers to inference callable
//
//   — Dispatch tag to select implementation tier at compile time:
//     impl_tag<Impl>                         — type constant; e.g. impl_tag<learned_impl>
//
// Design:
//   • Every major decision point (backend selection, cost estimation, scheduling)
//     can evolve through the four implementation tiers by changing a single
//     template argument — no call-site changes required.
//   • All concepts are structural (no base class, no registration).
//   • The four impl tags are empty types (zero overhead); used as template
//     arguments to select a tier, e.g. decision_engine<cost_ranking<T,learned_impl>>.
//   • No virtual, no macros. C++23. Header-only. Opt-in (not pulled by lithe.hpp).
// =============================================================================

#include "lithe_cost_model.hpp"
#include "lithe_decision_engine.hpp"
#include "lithe_feature_extractor.hpp"
#include "lithe_algorithms/selection.hpp"

#include <functional>
#include <span>
#include <string_view>

namespace lithe::ml {
    // =========================================================================
    // Implementation tier tags
    //
    // Each tag is an empty struct used as a template argument to select which
    // implementation tier to instantiate.  They carry no state or behavior;
    // their purpose is entirely at the type level.
    //
    // Tier progression:
    //   heuristic → analytical → profile_guided → learned
    //
    // A system starts at heuristic (cheapest, always available) and can evolve
    // toward learned (most accurate, requires training data) by changing the
    // tag in the template argument.  All tiers satisfy the same concepts.
    // =========================================================================

    struct heuristic_impl {
        static constexpr std::string_view name = "heuristic";
        static constexpr bool deterministic = true;
        static constexpr bool requires_training_data = false;
        static constexpr bool requires_profile_data = false;
    };

    struct analytical_impl {
        static constexpr std::string_view name = "analytical";
        static constexpr bool deterministic = true;
        static constexpr bool requires_training_data = false;
        static constexpr bool requires_profile_data = false;
    };

    struct profile_guided_impl {
        static constexpr std::string_view name = "profile_guided";
        static constexpr bool deterministic = false; // varies with profile
        static constexpr bool requires_training_data = false;
        static constexpr bool requires_profile_data = true;
    };

    struct learned_impl {
        static constexpr std::string_view name = "learned";
        static constexpr bool deterministic = false; // model-dependent
        static constexpr bool requires_training_data = true;
        static constexpr bool requires_profile_data = false;
    };

    // =========================================================================
    // impl_tag<Impl> — type constant carrying an implementation tier
    //
    // Carries a tag as a template value parameter so that code can branch on
    // tier at compile time without specializing entire structures.
    //
    // Example:
    //   template <class Impl = heuristic_impl>
    //   struct my_selector {
    //       using tier = impl_tag<Impl>;
    //       static constexpr bool is_learned = std::is_same_v<Impl, learned_impl>;
    //   };
    // =========================================================================

    template <class Impl>
    struct impl_tag {
        using type = Impl;
        static constexpr std::string_view name = Impl::name;
    };

    using heuristic_tag = impl_tag<heuristic_impl>;
    using analytical_tag = impl_tag<analytical_impl>;
    using profile_tag = impl_tag<profile_guided_impl>;
    using learned_tag = impl_tag<learned_impl>;

    // =========================================================================
    // feature_provider<FP, In> — concept
    //
    // A type FP satisfies feature_provider<FP, In> iff:
    //   fp.provide(input) → features::feature_vector
    //
    // Distinct from lithe::features::feature_extractor<F,In> only in name:
    // "feature_provider" is the ML-layer vocabulary; "feature_extractor" is the
    // infrastructure vocabulary.  Any type satisfying feature_extractor<F,In>
    // also satisfies feature_provider<F,In> (same structural requirements).
    //
    // The separation allows the ML interface layer to be documented and
    // understood independently of the feature extraction infrastructure.
    // =========================================================================

    template <class FP, class In>
    concept feature_provider =
        requires(FP& fp, const In& input) {
            { fp.provide(input) } -> std::same_as<features::feature_vector>;
        };

    // =========================================================================
    // ml_cost_estimator<C, Node> — concept alias
    //
    // Same requirements as lithe::cost::cost_estimator.  Provided under the
    // lithe::ml namespace so ML-facing code does not need to depend on
    // lithe_cost_model.hpp directly.
    // =========================================================================

    template <class C, class Node>
    concept ml_cost_estimator = cost::cost_estimator<C, Node>;

    // =========================================================================
    // ml_decision_strategy<S, T> — concept alias
    //
    // Same requirements as lithe::intelligence::decision_strategy.  Provided
    // under lithe::ml for the same reason as ml_cost_estimator above.
    // =========================================================================

    template <class S, class T>
    concept ml_decision_strategy = intelligence::decision_strategy<S, T>;

    // =========================================================================
    // heuristic_feature_provider<Extractor>
    //
    // Wraps any feature_extractor (lithe::features concept) and bridges it to
    // the feature_provider interface by delegating provide() → extract().
    //
    // Zero overhead: if constexpr eliminates the call when Extractor is empty.
    // =========================================================================

    template <class Extractor>
    struct heuristic_feature_provider {
        [[no_unique_address]] Extractor extractor{};

        using impl_type = heuristic_impl;

        template <class In>
            requires features::feature_extractor<Extractor, In>
        [[nodiscard]] features::feature_vector provide(const In& input) {
            return extractor.extract(input);
        }
    };

    // =========================================================================
    // learned_feature_provider
    //
    // Delegates feature extraction to a user-supplied inference callable.
    // The callable receives a const void* (type-erased input) and returns a
    // feature_vector.  Callers cast the pointer to the concrete input type.
    //
    // Usage:
    //   learned_feature_provider lfp;
    //   lfp.set_fn([](const void* in) {
    //       const auto& expr = *static_cast<const MyExpr*>(in);
    //       return run_embedding_model(expr);
    //   });
    //   auto fv = lfp.provide_erased(&expr);
    // =========================================================================

    struct learned_feature_provider {
        using impl_type = learned_impl;
        using infer_fn_t = std::function<features::feature_vector(const void*)>;

        infer_fn_t infer_fn;

        void set_fn(infer_fn_t fn) { infer_fn = std::move(fn); }

        [[nodiscard]] bool has_fn() const noexcept { return static_cast<bool>(infer_fn); }

        // Type-erased entry point: caller passes a pointer to the concrete input.
        [[nodiscard]] features::feature_vector provide_erased(const void* input) const {
            if (infer_fn) return infer_fn(input);
            return {};
        }
    };

    // =========================================================================
    // learned_cost_estimator
    //
    // Type-erased cost estimator backed by a user-supplied inference callable.
    // Satisfies ml_cost_estimator<learned_cost_estimator, lithe::cost::lithe_enode_t>.
    //
    // When no inference function is set, falls back to balanced_cost_estimator.
    // =========================================================================

    struct learned_cost_estimator {
        using impl_type = learned_impl;
        using infer_fn_t = std::function<
            cost::cost_vector(const cost::lithe_enode_t &,
                              std::span<const cost::cost_vector>,
            const cost::cost_context&
        )
        >;

        infer_fn_t infer_fn;
        cost::balanced_cost_estimator fallback{};

        void set_fn(infer_fn_t fn) { infer_fn = std::move(fn); }

        [[nodiscard]] cost::cost_vector estimate(
            const cost::lithe_enode_t& n,
            std::span<const cost::cost_vector> children,
            const cost::cost_context& ctx) const {
            if (infer_fn) return infer_fn(n, children, ctx);
            return fallback.estimate(n, children, ctx);
        }
    };

    static_assert(ml_cost_estimator<learned_cost_estimator, cost::lithe_enode_t>);

    // =========================================================================
    // heuristic_decision_strategy<T>
    //
    // Thin wrapper around intelligence::cost_ranking<T> that tags the result
    // with heuristic_impl.  Satisfies ml_decision_strategy<S,T>.
    // =========================================================================

    template <class T>
    struct heuristic_decision_strategy {
        using impl_type = heuristic_impl;

        [[nodiscard]] static algorithms::algorithm_descriptor descriptor() noexcept {
            return algorithms::algorithm_descriptor{
                .id = "lithe.ml.strategy.heuristic",
                .version_major = 1,
                .version_minor = 0,
                .deterministic = true,
                .thread_safe = true,
                .reentrant = true,
            };
        }

        [[nodiscard]] intelligence::ranked<T> rank(
            std::span<intelligence::candidate<T>> candidates,
            const cost::cost_context& ctx) const {
            intelligence::cost_ranking<T> inner;
            return inner.rank(candidates, ctx);
        }
    };

    static_assert(ml_decision_strategy<
        heuristic_decision_strategy<int>, int>);

    // =========================================================================
    // learned_decision_strategy<T>
    //
    // Defers candidate ranking to a user-supplied inference callable.
    // Fallback: heuristic_decision_strategy when no fn is set.
    // Satisfies ml_decision_strategy<S,T>.
    // =========================================================================

    template <class T>
    struct learned_decision_strategy {
        using impl_type = learned_impl;
        using rank_fn_t = std::function<
            intelligence::ranked<T>(
                std::span<intelligence::candidate<T>>,
            const cost::cost_context&
        )
        >;

        rank_fn_t rank_fn;
        heuristic_decision_strategy<T> fallback{};

        void set_fn(rank_fn_t fn) { rank_fn = std::move(fn); }

        [[nodiscard]] static algorithms::algorithm_descriptor descriptor() noexcept {
            return algorithms::algorithm_descriptor{
                .id = "lithe.ml.strategy.learned",
                .version_major = 1,
                .version_minor = 0,
                .deterministic = false,
                .thread_safe = false,
                .reentrant = false,
                .safe_for_runtime_replacement = true,
            };
        }

        [[nodiscard]] intelligence::ranked<T> rank(
            std::span<intelligence::candidate<T>> candidates,
            const cost::cost_context& ctx) const {
            if (rank_fn) return rank_fn(candidates, ctx);
            return fallback.rank(candidates, ctx);
        }
    };

    static_assert(ml_decision_strategy<
        learned_decision_strategy<int>, int>);
} // namespace lithe::ml
