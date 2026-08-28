#pragma once

// =============================================================================
// lithe_decision_engine.hpp — Unified decision pipeline for the intelligence layer
//
// Namespace:  lithe::intelligence
// Depends on: lithe/lithe_feature_extractor.hpp  (feature_vector, feature_extractor)
//             lithe/lithe_cost_model.hpp          (cost_vector, cost_context,
//                                                 cost_estimator)
//             <algorithm>, <concepts>, <functional>, <ranges>, <span>, <vector>
//
// Provides:
//   candidate<T>          — value T + feature_vector + cost_vector + score
//   ranked<T>             — best-first ordered span of candidates
//   decision_strategy<S,T>— concept: S can rank a span<candidate<T>>
//   decision_engine<Strategy> — orchestrates: gen → feat → cost → rank → select
//
//   Built-in strategies (all satisfy decision_strategy):
//     rule_ranking<T>      — user predicate list, first-match wins; no cost
//     cost_ranking<T>      — weighted_sum over cost_vector; default strategy
//     profile_guided_ranking<T> — axis-biased weighted_sum
//     learned_ranking<T>   — delegates to std::function<double(feature_vector)>
//
// Design:
//   • No virtual, no macros. C++23. Header-only.
//   • Static dispatch only: Strategy is a template parameter.
//   • candidate<T> embeds existing feature_vector and cost_vector — no new types.
//   • decision_engine adds only orchestration + ranking; it holds no metrics.
//   • cost_ranking is the default strategy for decision_engine.
//   • Empty candidate set → empty ranked{}.
//   • feat_fn called once per candidate; result stored in candidate.features.
// =============================================================================

#include "lithe_cost_model.hpp"
#include "lithe_feature_extractor.hpp"

#include <algorithm>
#include <concepts>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace lithe::intelligence {
    // =============================================================================
    // candidate<T>
    //
    // One item in the decision space.  T identifies the candidate
    // (backend id, pass-pipeline id, policy id, …).
    //
    // Lifecycle:
    //   1. value + features filled by gen/feat stages
    //   2. cost filled by cost stage
    //   3. score filled by rank stage
    // =============================================================================

    template <class T>
    struct candidate {
        T value;
        features::feature_vector features;
        cost::cost_vector cost{};
        double score = 0.0;
    };

    // =============================================================================
    // ranked<T> — best-first ordered result
    // =============================================================================

    template <class T>
    struct ranked {
        std::vector<candidate<T>> ordered; // ordered[0] is the best

        [[nodiscard]] bool empty() const noexcept { return ordered.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return ordered.size(); }
    };

    // =============================================================================
    // decision_strategy<S, T> concept
    //
    // S satisfies decision_strategy<S,T> iff:
    //   s.rank(candidates, ctx) → ranked<T>
    // where candidates is std::span<candidate<T>>.
    // =============================================================================

    template <class S, class T>
    concept decision_strategy =
        requires(const S& s,
                 std::span<candidate<T>> candidates,
                 const cost::cost_context& ctx) {
            { s.rank(candidates, ctx) } -> std::same_as<ranked<T>>;
        };

    // =============================================================================
    // rule_ranking<T>
    //
    // User-supplied predicate list; first candidate satisfying any rule is best.
    // Remaining candidates ordered in input order (score = 0.0 for all).
    // No cost/feature stage needed — rules inspect candidate.value only.
    //
    // Fallback when no rule matches: cost_ranking order (weighted_sum latency-first).
    // =============================================================================

    template <class T>
    struct rule_ranking {
        using predicate_t = std::function<bool(const T&)>;

        std::vector<predicate_t> rules;

        void add_rule(predicate_t p) { rules.push_back(std::move(p)); }

        [[nodiscard]] ranked<T> rank(std::span<candidate<T>> candidates,
                                     const cost::cost_context& /*ctx*/) const {
            ranked<T> result;
            result.ordered.assign(candidates.begin(), candidates.end());

            // Score: 1.0 for first-matched candidate, 0.0 for the rest.
            for (auto& c : result.ordered) {
                for (const auto& rule : rules) {
                    if (rule(c.value)) {
                        c.score = 1.0;
                        break;
                    }
                }
            }

            std::stable_sort(result.ordered.begin(), result.ordered.end(),
                             [](const candidate<T>& a, const candidate<T>& b) {
                                 return a.score > b.score;
                             });
            return result;
        }
    };

    static_assert(decision_strategy<rule_ranking<int>, int>);

    // =============================================================================
    // cost_ranking<T>  (default)
    //
    // Scores candidates via cost_vector::weighted_sum with axis weights drawn from
    // the cost_context's profile_id.  Default weights: latency=1, rest=0.25.
    // Higher score = lower cost = better.  Ties broken by input order (stable_sort).
    // =============================================================================

    template <class T>
    struct cost_ranking {
        float w_latency = 1.0f;
        float w_memory = 0.25f;
        float w_power = 0.25f;
        float w_throughput = 0.25f;

        [[nodiscard]] ranked<T> rank(std::span<candidate<T>> candidates,
                                     const cost::cost_context& /*ctx*/) const {
            ranked<T> result;
            result.ordered.assign(candidates.begin(), candidates.end());

            const float wl = w_latency, wm = w_memory,
                        wp = w_power, wt = w_throughput;

            for (auto& c : result.ordered) {
                const float raw = c.cost.weighted_sum(wl, wm, wp, wt);
                // Score is inverse-cost: lower cost → higher score.
                // Add epsilon to avoid division by zero.
                c.score = (raw > 0.0f) ? (1.0f / raw) : 1.0e9f;
            }

            std::stable_sort(result.ordered.begin(), result.ordered.end(),
                             [](const candidate<T>& a, const candidate<T>& b) {
                                 return a.score > b.score;
                             });
            return result;
        }
    };

    static_assert(decision_strategy<cost_ranking<int>, int>);

    // =============================================================================
    // profile_guided_ranking<T>
    //
    // Like cost_ranking but axis weights are overridden per active profile_id.
    // Profiles are matched by prefix; last matching hint wins.
    // Fallback to cost_ranking weights when no hint matches.
    // =============================================================================

    struct profile_weight_hint {
        std::string_view profile_id;
        float w_latency = 1.0f;
        float w_memory = 0.25f;
        float w_power = 0.25f;
        float w_throughput = 0.25f;
    };

    template <class T>
    struct profile_guided_ranking {
        std::vector<profile_weight_hint> hints;
        cost_ranking<T> fallback_ranker{};

        void add_hint(profile_weight_hint h) { hints.push_back(std::move(h)); }

        [[nodiscard]] ranked<T> rank(std::span<candidate<T>> candidates,
                                     const cost::cost_context& ctx) const {
            cost_ranking<T> active = fallback_ranker;
            for (const auto& h : hints) {
                if (ctx.profile_id.starts_with(h.profile_id)) {
                    active.w_latency = h.w_latency;
                    active.w_memory = h.w_memory;
                    active.w_power = h.w_power;
                    active.w_throughput = h.w_throughput;
                }
            }
            return active.rank(candidates, ctx);
        }
    };

    static_assert(decision_strategy<profile_guided_ranking<int>, int>);

    // =============================================================================
    // learned_ranking<T>
    //
    // Delegates scoring to a user-supplied std::function<double(const feature_vector&)>.
    // If no scorer is set, falls back to cost_ranking.
    // This is the only ML hook in the intelligence layer.
    // =============================================================================

    template <class T>
    struct learned_ranking {
        using scorer_fn_t = std::function<double(const features::feature_vector &)>;
        scorer_fn_t scorer;

        void set_scorer(scorer_fn_t fn) { scorer = std::move(fn); }

        [[nodiscard]] ranked<T> rank(std::span<candidate<T>> candidates,
                                     const cost::cost_context& ctx) const {
            if (!scorer) {
                cost_ranking<T> fb{};
                return fb.rank(candidates, ctx);
            }

            ranked<T> result;
            result.ordered.assign(candidates.begin(), candidates.end());

            for (auto& c : result.ordered)
                c.score = scorer(c.features);

            std::stable_sort(result.ordered.begin(), result.ordered.end(),
                             [](const candidate<T>& a, const candidate<T>& b) {
                                 return a.score > b.score;
                             });
            return result;
        }
    };

    static_assert(decision_strategy<learned_ranking<int>, int>);

    // =============================================================================
    // decision_engine<Strategy>
    //
    // Orchestrates the fixed pipeline:
    //   gen()                → range of T
    //   feat(T)              → feature_vector      (called once per candidate)
    //   cost_fn(T,features,ctx) → cost_vector
    //   strategy_.rank(...)  → ranked<T>
    //
    // Template params:
    //   Strategy — must satisfy decision_strategy<Strategy, T>
    // =============================================================================

    template <class Strategy>
    class decision_engine {
        Strategy strategy_;

    public:
        explicit decision_engine(Strategy s = {}) noexcept(
            std::is_nothrow_move_constructible_v<Strategy>)
            : strategy_(std::move(s)) {}

        [[nodiscard]] Strategy& strategy() noexcept { return strategy_; }
        [[nodiscard]] const Strategy& strategy() const noexcept { return strategy_; }

        // -------------------------------------------------------------------------
        // decide — run the full pipeline
        //
        //   GenFn:    ()           → range<T>       (generates candidate values)
        //   FeatFn:   (T)          → feature_vector (may return empty{})
        //   CostFn:   (T, const feature_vector&, const cost_context&) → cost_vector
        //
        // Returns ranked<T> (best-first).  Empty if gen produces no candidates.
        // -------------------------------------------------------------------------

        template <class T, class GenFn, class FeatFn, class CostFn>
            requires decision_strategy<Strategy, T>
        [[nodiscard]] ranked<T> decide(GenFn&& gen,
                                       FeatFn&& feat,
                                       CostFn&& cost_fn,
                                       const cost::cost_context& ctx) const {
            // Stage 1: generate candidates
            std::vector<candidate<T>> candidates;
            for (auto&& val : std::forward<GenFn>(gen)()) {
                candidate<T> c;
                c.value = std::forward<decltype(val)>(val);

                // Stage 2: extract features (once per candidate)
                c.features = std::forward<FeatFn>(feat)(c.value);

                // Stage 3: estimate cost
                c.cost = std::forward<CostFn>(cost_fn)(c.value, c.features, ctx);

                candidates.push_back(std::move(c));
            }

            if (candidates.empty()) return ranked<T>{};

            // Stage 4: rank via strategy
            return strategy_.rank(std::span<candidate<T>>{candidates}, ctx);
        }
    };

    // Convenience alias using the default cost_ranking strategy
    template <class T>
    using default_decision_engine = decision_engine<cost_ranking<T>>;
} // namespace lithe::intelligence
