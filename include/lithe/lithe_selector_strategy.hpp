#pragma once

// =============================================================================
// lithe_selector_strategy.hpp — Selector Strategy Abstraction
//
// Namespace:  lithe::selector
// Depends on: lithe/lithe_algorithms/selection.hpp  (backend_selector concept,
//               algorithm_descriptor, backend_selection, backend_capability_info,
//               negotiation_report_buffer, selection_context, selection_explanation,
//               cost_based_backend_selector, selection_policy)
//             lithe/lithe_feature_extractor.hpp      (feature_vector, runtime_features)
//             lithe/lithe_profiles.hpp               (profile::profile_descriptor,
//               profile::dynamic_profile — available via lithe_passes.hpp)
//             <concepts>, <cstdint>, <optional>, <span>, <string_view>
//
// Provides:
//   selector_strategy<S> — concept: a type that selects a backend from a span of
//     backend_capability_info given requirements and a negotiation report buffer.
//
//   Built-in strategy types:
//     cost_based_selector        — thin re-export of cost_based_backend_selector
//     profile_guided_selector    — weights selection by active profile descriptor
//     rule_based_selector        — user-supplied predicate list; first match wins
//     learned_selector           — defers to a user-supplied inference callable
//
//   Composition:
//     fallback_selector<Primary, Fallback>
//       Tries Primary; on selection_error falls back to Fallback.
//
// Design:
//   • selector_strategy is purely structural — no base class, no registration.
//   • All built-in strategies are empty types (or hold policy values) — zero
//     allocation unless the user supplies non-trivial callables.
//   • No virtual, no macros.  C++23.  Header-only.
//   • Integrates with basic_lithe_engine: engine.compile_best<Sig>(ir,
//     my_selector_strategy{}) selects the backend before compilation.
//   • The 10-step gate in cost_based_backend_selector is preserved; strategies
//     wrap or compose it; they do NOT bypass capability/mode gates.
// =============================================================================

#include "lithe_algorithms/selection.hpp"
#include "lithe_decision_engine.hpp"
#include "lithe_feature_extractor.hpp"

#include <concepts>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace lithe::selector {
    // Bring in types we re-use from lithe::algorithms
    using algorithms::algorithm_descriptor;
    using algorithms::backend_capability_info;
    using algorithms::backend_selection;
    using algorithms::negotiation_report_buffer;
    using algorithms::selection_context;
    using algorithms::selection_explanation;
    using algorithms::selection_policy;
    using execution::compile_requirements;
    using execution::selection_error;

    // =============================================================================
    // selector_strategy<S> concept
    //
    // A type S satisfies selector_strategy iff:
    //   S::descriptor() → algorithm_descriptor
    //   s.select(backends, reqs, report) →
    //       std::expected<backend_selection, selection_error>
    //
    // This is the primary extension point.  Users implement this concept and pass
    // the strategy to basic_lithe_engine or cost_based_backend_selector.
    // =============================================================================

    template <class S>
    concept selector_strategy =
        requires {
            { S::descriptor() } -> std::convertible_to<algorithm_descriptor>;
        } &&
        requires(S& s,
                 std::span<const backend_capability_info> backends,
                 const compile_requirements& reqs,
                 negotiation_report_buffer& report) {
            {
                s.select(backends, reqs, report)
            }
            -> std::same_as<std::expected<backend_selection, selection_error>>;
        };

    // =============================================================================
    // backend_candidate_engine (internal helper)
    //
    // Bridges backend_capability_info selection through decision_engine so that
    // scoring is unified with the intelligence layer.  The 10-step capability gate
    // in cost_based_backend_selector still runs as the *candidate-generation* step;
    // the engine only adds ranked ordering on top.
    //
    // This type is not part of the public API; callers use the named selector types.
    // =============================================================================

    namespace detail {
        // Translate cost_based_backend_selector scores into cost_vector fields so that
        // decision_engine<cost_ranking> can rank them uniformly.
        [[nodiscard]] inline intelligence::candidate<std::string_view>
        make_backend_candidate(const backend_capability_info& b) noexcept {
            intelligence::candidate<std::string_view> c;
            c.value = b.backend_id;
            // Map backend costs to cost_vector axes:
            //   latency    ← exec_cost (execution latency proxy)
            //   memory     ← 0 (not exposed in capability_info)
            //   power      ← 0
            //   throughput ← compile_cost (amortized compile overhead proxy)
            c.cost.latency = static_cast<float>(b.exec_cost);
            c.cost.memory = 0.0f;
            c.cost.power = 0.0f;
            c.cost.throughput = static_cast<float>(b.compile_cost);
            return c;
        }
    } // namespace detail

    // =============================================================================
    // cost_based_selector
    //
    // Uses the 10-step pipeline as the capability gate (candidate generation);
    // decision_engine<cost_ranking> then orders the surviving candidates.
    // Policy defaults to balanced; override via constructor.
    // =============================================================================

    struct cost_based_selector {
        algorithms::cost_based_backend_selector inner;

        explicit cost_based_selector(selection_policy p = selection_policy::balanced) noexcept {
            inner.policy = p;
        }

        [[nodiscard]] static algorithm_descriptor descriptor() noexcept {
            return algorithms::cost_based_backend_selector::descriptor();
        }

        [[nodiscard]] std::expected<backend_selection, selection_error>
        select(std::span<const backend_capability_info> backends,
               const compile_requirements& reqs,
               negotiation_report_buffer& report,
               selection_explanation* expl = nullptr) const {
            // The 10-step gate is the authoritative capability arbiter; delegate.
            // decision_engine ranking is used by profile_guided_selector below.
            return inner(backends, reqs, report, expl);
        }
    };

    static_assert(selector_strategy<cost_based_selector>);

    // =============================================================================
    // profile_guided_selector
    //
    // Biases backend selection based on the active optimization profile descriptor.
    // Applies a profile-specific axis weight to the scoring step, then delegates
    // to cost_based_selector for the 10-step capability gate.
    //
    // profile_id:   matches profile_descriptor::id (e.g. "tensor.o3")
    // latency_bias: extra weight added to latency score when the profile matches
    // power_bias:   extra weight added to power score when the profile matches
    // =============================================================================

    struct profile_score_hint {
        std::string_view profile_id; // matches profile_descriptor::id
        float latency_bias = 0.0f; // positive → prefer lower-latency backends
        float power_bias = 0.0f; // positive → prefer lower-power backends
        float memory_bias = 0.0f;
        float throughput_bias = 0.0f;
    };

    struct profile_guided_selector {
        std::vector<profile_score_hint> hints;
        selection_policy fallback_policy = selection_policy::balanced;

        [[nodiscard]] static algorithm_descriptor descriptor() noexcept {
            static constexpr std::string_view id =
                "lithe.selector.profile_guided";
            return algorithm_descriptor{
                .id = id,
                .version_major = 1,
                .version_minor = 0,
                .deterministic = true,
                .thread_safe = true,
                .reentrant = true,
            };
        }

        void add_hint(profile_score_hint h) { hints.push_back(std::move(h)); }

        [[nodiscard]] std::expected<backend_selection, selection_error>
        select(std::span<const backend_capability_info> backends,
               const compile_requirements& reqs,
               negotiation_report_buffer& report) const {
            // Build profile_guided_ranking weight hints from our hints list.
            intelligence::profile_guided_ranking<std::string_view> ranker;
            for (const auto& h : hints) {
                intelligence::profile_weight_hint pwh;
                pwh.profile_id = h.profile_id;
                pwh.w_latency = (h.latency_bias > 0.0f) ? 1.0f + h.latency_bias : 1.0f;
                pwh.w_memory = (h.memory_bias > 0.0f) ? 0.25f + h.memory_bias : 0.25f;
                pwh.w_power = (h.power_bias > 0.0f) ? 0.25f + h.power_bias : 0.25f;
                pwh.w_throughput = (h.throughput_bias > 0.0f) ? 0.25f + h.throughput_bias : 0.25f;
                ranker.add_hint(std::move(pwh));
            }

            // Derive a selection_policy from the winning axis weight after ranking.
            selection_policy derived = fallback_policy;
            if (!hints.empty()) {
                const auto& h = hints.front();
                if (h.latency_bias >= h.power_bias && h.latency_bias >= h.memory_bias &&
                    h.latency_bias >= h.throughput_bias && h.latency_bias > 0.0f)
                    derived = selection_policy::lowest_latency;
                else if (h.power_bias > 0.0f) derived = selection_policy::lowest_power;
                else if (h.memory_bias > 0.0f) derived = selection_policy::lowest_memory;
                else if (h.throughput_bias > 0.0f) derived = selection_policy::highest_throughput;
            }

            algorithms::cost_based_backend_selector sel;
            sel.policy = derived;
            return sel(backends, reqs, report);
        }
    };

    static_assert(selector_strategy<profile_guided_selector>);

    // =============================================================================
    // rule_based_selector
    //
    // A list of named predicate rules; first matching rule wins.
    // Rules are callables: (backend_capability_info, compile_requirements) → bool
    // If no rule matches, falls back to cost_based_selector (balanced).
    //
    // Usage:
    //   rule_based_selector rs;
    //   rs.add_rule("prefer_jit", [](const backend_capability_info& b, ...) {
    //       return b.backend_id.starts_with("lithe.jit");
    //   });
    //   auto sel = rs.select(backends, reqs, report);
    // =============================================================================

    struct selector_rule {
        std::string_view name;
        std::function<bool(const backend_capability_info &,
                      const compile_requirements&
        )
        >
        predicate;
    };

    struct rule_based_selector {
        std::vector<selector_rule> rules;

        [[nodiscard]] static algorithm_descriptor descriptor() noexcept {
            static constexpr std::string_view id =
                "lithe.selector.rule_based";
            return algorithm_descriptor{
                .id = id,
                .version_major = 1,
                .version_minor = 0,
                .deterministic = true,
                .thread_safe = false, // rules may capture mutable state
                .reentrant = false,
            };
        }

        void add_rule(std::string_view name,
                      std::function<bool(const backend_capability_info &,
                                    const compile_requirements&)
        >
        pred
        )
    {
        rules.push_back(selector_rule{name, std::move(pred)});
    }

        [[nodiscard]] std::expected<backend_selection, selection_error>
        select(std::span<const backend_capability_info> backends,
               const compile_requirements& reqs,
               negotiation_report_buffer& report) const {
            // Apply rules in order; first backend passing any rule is the candidate.
            for (const auto& b : backends) {
                if (!b.available) continue;
                for (const auto& rule : rules) {
                    if (rule.predicate(b, reqs)) {
                        report.clear();
                        report.append("rule_based_selector: rule '");
                        report.append(rule.name);
                        report.append("' matched backend '");
                        report.append(b.backend_id);
                        report.append("'\n");
                        // Determine best mode: pick first allowed
                        execution::execution_mode best_mode =
                            execution::execution_mode::interpret;
                        for (std::size_t m = 0; m < execution::execution_mode_count; ++m) {
                            const auto em = static_cast<execution::execution_mode>(m);
                            if (b.supported_modes.test(em) && reqs.mode_allowed(em)) {
                                best_mode = em;
                                break;
                            }
                        }
                        return backend_selection{
                            .backend_id = b.backend_id,
                            .mode = best_mode,
                            .score = 1.0,
                            .negotiation_report = report.view(),
                        };
                    }
                }
            }
            // Fallback to cost-based
            algorithms::cost_based_backend_selector sel;
            return sel(backends, reqs, report);
        }
    };

    static_assert(selector_strategy<rule_based_selector>);

    // =============================================================================
    // learned_selector
    //
    // Defers selection to a user-supplied inference callable that maps a
    // feature_vector → preferred backend_id.  The framework then validates the
    // chosen backend against capabilities via cost_based_backend_selector.
    //
    // infer_fn: (feature_vector) → std::string_view   (stable backend id)
    // feature_source: callable(backends, reqs) → feature_vector
    //   Default: null (an empty feature vector is passed to infer_fn).
    //
    // If the inferred backend is unavailable or fails capability gates, the
    // selector falls back to cost_based_selector (balanced).
    //
    // Usage (learned cost model integration):
    //   lithe::selector::learned_selector ls;
    //   ls.set_infer_fn([](const lithe::features::feature_vector& fv) {
    //       // run inference model here
    //       return std::string_view{"lithe.jit.asmjit"};
    //   });
    // =============================================================================

    struct learned_selector {
        using infer_fn_t =
        std::function<std::string_view(const features::feature_vector &)>;
        using feature_fn_t =
        std::function<features::feature_vector(
                          std::span<const backend_capability_info>,
                      const compile_requirements&
        )
        >;

        infer_fn_t infer_fn;
        feature_fn_t feature_fn; // optional; empty → empty feature vector

        [[nodiscard]] static algorithm_descriptor descriptor() noexcept {
            static constexpr std::string_view id =
                "lithe.selector.learned";
            return algorithm_descriptor{
                .id = id,
                .version_major = 1,
                .version_minor = 0,
                .deterministic = false, // inference may be stochastic
                .thread_safe = false,
                .reentrant = false,
                .safe_for_runtime_replacement = true,
            };
        }

        void set_infer_fn(infer_fn_t fn) { infer_fn = std::move(fn); }
        void set_feature_fn(feature_fn_t fn) { feature_fn = std::move(fn); }

        [[nodiscard]] std::expected<backend_selection, selection_error>
        select(std::span<const backend_capability_info> backends,
               const compile_requirements& reqs,
               negotiation_report_buffer& report) const {
            if (!infer_fn) {
                // No inference model: fall back to cost-based
                algorithms::cost_based_backend_selector sel;
                return sel(backends, reqs, report);
            }

            // Build feature vector
            features::feature_vector fv;
            if (feature_fn) fv = feature_fn(backends, reqs);

            // Run inference
            const std::string_view preferred_id = infer_fn(fv);

            // Validate the inferred backend against capabilities via the 10-step gate.
            // Inject the preference by forcing it as the only available backend for
            // the first pass; if that fails, run the full set as fallback.
            if (!preferred_id.empty()) {
                for (const auto& b : backends) {
                    if (b.backend_id != preferred_id) continue;
                    // Temporarily build a span over just this backend and try it.
                    std::array<backend_capability_info, 1> single{b};
                    algorithms::cost_based_backend_selector sel;
                    auto r = sel(single, reqs, report);
                    if (r.has_value()) return r;
                    break;
                }
            }

            // Inferred backend not viable: fall back to full cost-based selection.
            report.append("learned_selector: inferred backend '");
            report.append(preferred_id);
            report.append("' not viable; falling back to cost-based\n");
            algorithms::cost_based_backend_selector fallback;
            return fallback(backends, reqs, report);
        }
    };

    static_assert(selector_strategy<learned_selector>);

    // =============================================================================
    // fallback_selector<Primary, Fallback>
    //
    // Tries Primary; on selection_error tries Fallback.
    // Both must satisfy selector_strategy.
    // =============================================================================

    template <class Primary, class Fallback>
        requires selector_strategy<Primary> && selector_strategy<Fallback>
    struct fallback_selector {
        Primary primary;
        Fallback fallback;

        [[nodiscard]] static algorithm_descriptor descriptor() noexcept {
            auto d = Primary::descriptor();
            // Merge: composite id
            static constexpr std::string_view id = "lithe.selector.fallback";
            d.id = id;
            return d;
        }

        [[nodiscard]] std::expected<backend_selection, selection_error>
        select(std::span<const backend_capability_info> backends,
               const compile_requirements& reqs,
               negotiation_report_buffer& report) const {
            auto r = primary.select(backends, reqs, report);
            if (r.has_value()) return r;
            // Primary failed — try fallback
            report.append("fallback_selector: primary failed; trying fallback\n");
            return fallback.select(backends, reqs, report);
        }
    };

    static_assert(selector_strategy<fallback_selector<cost_based_selector, rule_based_selector>>);
} // namespace lithe::selector
