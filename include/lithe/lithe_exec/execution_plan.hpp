#pragma once

// =============================================================================
// lithe_exec/execution_plan.hpp — Engine-neutral execution plan aggregate
//
// Namespace: lithe::exec
//
// Provides:
//   dependency_summary — loop-carried / cross-iteration dependence summary
//   execution_cost     — cost_vector + parallelism + confidence companion
//   execution_plan     — analysis-level plan for one region; rich, may own vectors
//
//   to_task_decomposition_plan(plan, bounds, rank, chunk) — lower a
//     threaded/gpu execution_plan to the ABI-level task_decomposition_plan POD
//     consumed by Pravaha. Scalar plans produce no task plan.
//
// Design:
//   - execution_plan = analysis descriptor (rich, with vectors).
//   - task_decomposition_plan = Pravaha ABI handoff (trivially copyable, POD).
//   - execution_cost reuses lithe::cost::cost_vector + two fields not in it.
//   - No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
//
// Dependencies:
//   lithe_exec/exec_kinds.hpp, lithe_exec/effect_summary.hpp,
//   lithe_exec/memory_summary.hpp, lithe_exec/layout_summary.hpp,
//   lithe_exec/runtime_guard.hpp
//   lithe_cost_model.hpp  (cost_vector)
//   lithe_codegen_hl.hpp  (task_decomposition_plan, loop_range)
// =============================================================================

#include "exec_kinds.hpp"
#include "effect_summary.hpp"
#include "layout_summary.hpp"
#include "memory_summary.hpp"
#include "runtime_guard.hpp"

// lithe_codegen_hl.hpp is an internal fragment; must include lithe_codegen.hpp first.
#include "../lithe_codegen.hpp"
#include "../lithe_codegen_hl.hpp"
#include "../lithe_cost_model.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace lithe::exec {
    // =========================================================================
    // execution_cost — cost_vector + parallelism + confidence
    //
    // Reuses lithe::cost::cost_vector for the four canonical axes.
    // Adds parallelism degree and model confidence.
    // =========================================================================

    struct execution_cost {
        lithe::cost::cost_vector cv; // latency/memory/power/throughput
        std::uint32_t parallelism = 1; // degree of parallelism (1=scalar)
        double confidence = 1.0; // model confidence [0,1]

        [[nodiscard]] constexpr bool dominates(const execution_cost& o) const noexcept {
            return cv.dominates(o.cv);
        }

        [[nodiscard]] constexpr float weighted_sum(
            float wl = 1.f, float wm = 1.f, float wp = 1.f, float wt = 1.f) const noexcept {
            return cv.weighted_sum(wl, wm, wp, wt);
        }
    };

    // =========================================================================
    // execution_plan — analysis-level descriptor for one HL MIR region
    // =========================================================================

    struct execution_plan {
        // Identity
        std::uint32_t region_id = 0; // HL MIR region / loop id
        execution_kind kind = execution_kind::scalar;
        analysis_outcome legality = analysis_outcome::unknown;
        region_class classification = region_class::unknown;

        // Analysis summaries
        dependency_summary dependencies;
        memory_summary memory;
        effect_mask effects;
        layout_summary layout;

        // Cost
        execution_cost cost;

        // Runtime versioning
        std::vector<runtime_guard> guards;
        std::optional<execution_plan_id> fallback; // valid iff outcome==unknown or guarded

        [[nodiscard]] bool is_legal() const noexcept {
            return legality == analysis_outcome::proven_legal;
        }

        [[nodiscard]] bool needs_guard() const noexcept {
            return !guards.empty() || legality == analysis_outcome::unknown;
        }
    };

    // =========================================================================
    // to_task_decomposition_plan — lower execution_plan → task_decomposition_plan
    //
    // Only call for threaded or gpu execution_plan.
    // Scalar plans produce no task plan (caller must check kind).
    //
    // Parameters:
    //   plan      — the execution_plan to lower (must be threaded or gpu)
    //   bounds    — loop_range array (caller extracts from HL MIR)
    //   rank      — number of valid bounds entries (≤ task_decomposition_plan::max_rank)
    //   chunk     — preferred chunk size for the task scheduler
    // =========================================================================

    [[nodiscard]] inline lithe::codegen::hl::task_decomposition_plan
    to_task_decomposition_plan(
        const execution_plan& plan,
        const std::array<lithe::codegen::hl::loop_range,
                         lithe::codegen::hl::task_decomposition_plan::max_rank>& bounds,
        std::uint8_t rank,
        std::size_t chunk = 1) noexcept {
        lithe::codegen::hl::task_decomposition_plan tdp;
        tdp.bounds = bounds;
        tdp.rank = rank;
        tdp.chunk = chunk;
        // kernel and user_data filled by the caller (Lithe does not own the fn ptr).
        (void)plan; // plan metadata consumed by caller to set kernel/user_data
        return tdp;
    }
} // namespace lithe::exec
