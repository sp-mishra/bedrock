#pragma once

// =============================================================================
// lithe_adaptive.hpp — Adaptive cost model (observe→blend→update loop)
//
// Namespace:  lithe::intelligence
// Depends on: lithe/lithe_cost_model.hpp    (cost_vector, cost_context,
//                                            cost_estimator concept)
//             lithe/lithe_feedback.hpp       (feedback_store, performance_profile,
//                                            hardware_signature, feedback_key)
//             <algorithm>, <cstddef>, <cstdint>
//
// Provides:
//   adaptive_cost_model<Base>  — wraps any cost_estimator<Base,Node>;
//     blends its static estimate with observed performance_profile data from a
//     feedback_store.  Satisfies cost_estimator itself, so it drops in wherever
//     a cost estimator is expected (decision_engine, e-graph extraction, etc.)
//
//     blend factor (0..1):
//       0.0 = pure static estimate (ignores feedback)
//       1.0 = pure observed (ignores static estimate)
//     The effective blend is confidence-weighted:
//       effective_blend = blend * min(1.0, sample_count / confidence_threshold)
//     so a single sample barely shifts the estimate; many samples trust observations.
//
// Design:
//   • No virtual, no macros. C++23. Header-only.
//   • Satisfies cost_estimator<adaptive_cost_model<Base>, Node> statically.
//   • Non-owning pointer to feedback_store (zero-cost when store == nullptr).
//   • Thread-safe for concurrent reads if Base::estimate is const-safe and
//     feedback_store::query is const-safe (it is — uses shared read lock).
// =============================================================================

#include "lithe_cost_model.hpp"
#include "lithe_feedback.hpp"

#include <algorithm>
#include <cstddef>

namespace lithe::intelligence {
    // =============================================================================
    // adaptive_cost_model<Base>
    //
    // Template parameter:
    //   Base — any type satisfying cost_estimator<Base, Node> for the desired Node.
    //
    // Wraps Base and optionally blends its estimate with observed data from a
    // feedback_store.  The store is accessed via a non-owning pointer; passing
    // nullptr disables feedback (equivalent to blend = 0).
    // =============================================================================

    template <class Base>
    struct adaptive_cost_model {
        Base base{};
        feedback::feedback_store* store = &feedback::feedback_store::global();
        float blend = 0.5f;
        std::size_t confidence_threshold = 5; // samples needed for full blend

        // -------------------------------------------------------------------------
        // estimate — the cost_estimator entry point
        //
        // Node type is deduced from the call; same Node type must satisfy
        // cost_estimator<Base, Node>.
        // -------------------------------------------------------------------------

        template <class Node>
            requires cost::cost_estimator<Base, Node>
        [[nodiscard]] cost::cost_vector
        estimate(const Node& node,
                 std::span<const cost::cost_vector> child_costs,
                 const cost::cost_context& ctx) const noexcept(
            noexcept(base.estimate(node, child_costs, ctx))) {
            const cost::cost_vector base_est = base.estimate(node, child_costs, ctx);

            if (!store || blend <= 0.0f) return base_est;

            // Query the feedback store using backend_id as a proxy for expr_hash.
            // The real hash is not available here without the full expression; we
            // use hw_signature + backend_id hash as a lightweight key.
            const auto hw = feedback::hardware_signature::current();
            // Approximate expr identity: hash backend_id string into a size_t.
            std::size_t id_hash = std::hash<std::string_view>{}(ctx.backend_id);
            id_hash ^= ctx.hw_signature + std::size_t{0x9e37'79b9} + (id_hash << 6);

            const auto profile = store->query(id_hash, hw);
            if (!profile.has_value() || profile->sample_count == 0) return base_est;

            // Confidence-weighted blend: ramp from 0 → blend over confidence_threshold
            const float conf = std::min(1.0f,
                                        static_cast<float>(profile->sample_count) /
                                        static_cast<float>(confidence_threshold));
            const float alpha = blend * conf; // effective blend

            // Build observed cost_vector from performance_profile fields.
            cost::cost_vector observed;
            observed.latency = static_cast<float>(profile->avg_latency_ms);
            // throughput stored inverted (lower = more costly)
            observed.throughput = (profile->avg_throughput_gops > 0.0)
                                      ? static_cast<float>(1.0 / profile->avg_throughput_gops)
                                      : 0.0f;
            observed.memory = static_cast<float>(profile->avg_memory_mb);
            observed.power = static_cast<float>(profile->avg_power_w);

            // Linear blend: result = (1-alpha)*base + alpha*observed
            cost::cost_vector result;
            result.latency = (1.0f - alpha) * base_est.latency + alpha * observed.latency;
            result.memory = (1.0f - alpha) * base_est.memory + alpha * observed.memory;
            result.power = (1.0f - alpha) * base_est.power + alpha * observed.power;
            result.throughput = (1.0f - alpha) * base_est.throughput + alpha * observed.throughput;
            return result;
        }
    };

    // Verify the concept satisfaction with a concrete estimator type.
    // (balanced_cost_estimator is defined in lithe_cost_model.hpp)
    static_assert(
        cost::cost_estimator<
            adaptive_cost_model<cost::balanced_cost_estimator>,
            cost::lithe_enode_t>,
        "adaptive_cost_model must satisfy cost_estimator");
} // namespace lithe::intelligence
