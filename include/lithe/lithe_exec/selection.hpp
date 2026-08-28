#pragma once

// =============================================================================
// lithe_exec/selection.hpp — auto_exec_selection_strategy for decision_engine
//
// Namespace: lithe::exec
//
// Provides:
//   auto_exec_selection_strategy — a decision_strategy<execution_kind> that:
//     gen:    enumerates candidate execution_kinds (scalar always last as fallback)
//     feat:   attaches effect/memory/layout/loop summaries as feature scores
//     cost:   calls profitability::estimate per kind
//     rank:   cost ascending; applies execution_hint bias and policy filters
//     select: lowest legal+profitable; @gpu(required) with no legal plan →
//             emits LITHE-EXEC-021 diagnostic
//
// Design:
//   Implements a decision_engine Strategy (satisfies decision_strategy concept).
//   Does NOT build a new selection engine — reuses decision_engine<Strategy>.
//   Hint bias implemented as score boost on preferred execution_kind.
//   No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include "exec_kinds.hpp"
#include "exec_hint.hpp"
#include "execution_plan.hpp"
#include "legality.hpp"
#include "profitability.hpp"

#include "../lithe_decision_engine.hpp"
#include "../lithe_diagnostics.hpp"

#include <span>
#include <vector>

namespace lithe::exec {
    // =========================================================================
    // exec_candidate_context — per-run context for the selection strategy
    // =========================================================================

    struct exec_candidate_context {
        const effect_summary* effects = nullptr;
        const memory_summary* memory = nullptr;
        const layout_summary* layout = nullptr;
        const loop_info_view* loop = nullptr;
        const region_context* region = nullptr;
        const target_capabilities* target = nullptr;
        const auto_execution_policy* policy = nullptr;
        std::span<const execution_hint> hints = {};
        const lithe::cost::cost_context* cost_ctx = nullptr;

        // Convenience: check if any hint requires a specific kind
        [[nodiscard]] bool has_required_hint(execution_kind k) const noexcept {
            for (const auto& h : hints)
                if (h.required && h.preferred && *h.preferred == k) return true;
            return false;
        }

        [[nodiscard]] bool forbids(execution_kind k) const noexcept {
            for (const auto& h : hints) {
                if (k == execution_kind::gpu && h.forbid_gpu) return true;
                if ((k == execution_kind::threaded || k == execution_kind::simd)
                    && h.forbid_parallel)
                    return true;
            }
            return false;
        }
    };

    // =========================================================================
    // auto_exec_selection_strategy — decision_engine Strategy for execution_kind
    //
    // Satisfies decision_strategy<auto_exec_selection_strategy, execution_kind>.
    // =========================================================================

    struct auto_exec_selection_strategy {
        // Hint preference boost: preferred kind gets score * (1 + kHintBoost)
        static constexpr double kHintBoost = 2.0;

        [[nodiscard]] lithe::intelligence::ranked<execution_kind>
        rank(std::span<lithe::intelligence::candidate<execution_kind>> candidates,
             const lithe::cost::cost_context& ctx) const {
            lithe::intelligence::ranked<execution_kind> result;
            result.ordered.assign(candidates.begin(), candidates.end());

            for (auto& c : result.ordered) {
                const float raw = c.cost.weighted_sum(1.f, 0.25f, 0.25f, 0.25f);
                c.score = (raw > 0.0f) ? (1.0 / static_cast<double>(raw)) : 1.0e9;
            }

            std::stable_sort(result.ordered.begin(), result.ordered.end(),
                             [](const lithe::intelligence::candidate<execution_kind>& a,
                                const lithe::intelligence::candidate<execution_kind>& b) {
                                 return a.score > b.score;
                             });

            (void)ctx;
            return result;
        }
    };

    static_assert(lithe::intelligence::decision_strategy<
        auto_exec_selection_strategy, execution_kind>);

    // =========================================================================
    // select_execution_kind — orchestrated selection for one region
    //
    // Runs legality + profitability for all policy-allowed kinds, then picks
    // via auto_exec_selection_strategy. Handles hint bias, forbidden filters,
    // and required-hint diagnostic.
    //
    // Returns the selected execution_kind (scalar if nothing else is legal).
    // Populates diag with LITHE-EXEC-021 if @gpu(required) fails legality.
    // =========================================================================

    template <lithe::diag::diagnostic_sink Sink = lithe::diag::null_sink>
    [[nodiscard]] execution_kind select_execution_kind(
        const exec_candidate_context& ctx,
        Sink& sink = lithe::diag::null_sink{}) noexcept(false) {
        if (!ctx.effects || !ctx.memory || !ctx.layout || !ctx.loop ||
            !ctx.region || !ctx.target || !ctx.policy)
            return execution_kind::scalar;

        static constexpr execution_kind kAllKinds[] = {
            execution_kind::scalar,
            execution_kind::simd,
            execution_kind::threaded,
            execution_kind::gpu,
        };

        std::vector<lithe::intelligence::candidate<execution_kind>> candidates;
        candidates.reserve(4);

        const lithe::cost::cost_context default_ctx{};
        const auto& cost_ctx = ctx.cost_ctx ? *ctx.cost_ctx : default_ctx;

        for (execution_kind k : kAllKinds) {
            // Skip policy-forbidden or hint-forbidden kinds
            if (!ctx.policy->allows(k)) continue;
            if (ctx.forbids(k)) continue;

            const auto outcome = check_legality(
                k, *ctx.region, *ctx.effects, *ctx.memory, *ctx.loop,
                *ctx.layout, *ctx.target, *ctx.policy);

            if (outcome == analysis_outcome::proven_illegal) {
                // Emit diagnostic if this was a required hint
                if (ctx.has_required_hint(k) && k == execution_kind::gpu) {
                    lithe::diag::diagnostic d;
                    d.level = lithe::diag::severity::error;
                    d.stage = lithe::diag::stage::backend;
                    d.code = lithe::diag::codes::exec::gpu_required_illegal;
                    d.message = "LITHE-EXEC-021: @gpu(required=true) region proven illegal "
                        "— no legal GPU plan";
                    sink.on_diagnostic(d);
                }
                continue;
            }

            lithe::intelligence::candidate<execution_kind> cand;
            cand.value = k;
            cand.cost = estimate(k, *ctx.memory, *ctx.layout, *ctx.target,
                                 cost_ctx, *ctx.loop).cv;

            // Hint bias: boost score for preferred kind
            for (const auto& h : ctx.hints) {
                if (h.preferred && *h.preferred == k)
                    cand.score += auto_exec_selection_strategy::kHintBoost;
            }

            candidates.push_back(cand);
        }

        if (candidates.empty()) return execution_kind::scalar;

        auto cspn = std::span{candidates};
        auto_exec_selection_strategy strategy;
        auto ranked = strategy.rank(cspn, cost_ctx);
        return ranked.ordered.empty() ? execution_kind::scalar : ranked.ordered[0].value;
    }
} // namespace lithe::exec
