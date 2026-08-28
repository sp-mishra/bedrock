#pragma once

// =============================================================================
// lithe_exec/exec_kinds.hpp — Core enums for the automatic execution analysis layer
//
// Namespace: lithe::exec
//
// Provides:
//   execution_kind   — what kind of execution a region maps to
//   analysis_outcome — result of a legality check
//   region_class     — structural classification of a loop/task region
//
// Design: POD-backed uint8_t enums + constexpr to_string. No virtual, no macros.
// Header-only C++23. Part of the opt-in lithe_exec layer (NOT in lithe.hpp).
// =============================================================================

#include <cstdint>
#include <string_view>

namespace lithe::exec {
    // =========================================================================
    // execution_kind — maps a region to a target execution mode
    // =========================================================================

    enum class execution_kind : std::uint8_t {
        scalar = 0, // single-threaded, scalar ops
        simd = 1, // SIMD vectorization within one thread
        threaded = 2, // multi-threaded parallel execution (CPU)
        gpu = 3, // GPU device compute
        distributed = 4, // distributed / multi-node execution
        // Reserved tail for future extension
    };

    [[nodiscard]] inline constexpr std::string_view to_string(execution_kind k) noexcept {
        switch (k) {
        case execution_kind::scalar: return "scalar";
        case execution_kind::simd: return "simd";
        case execution_kind::threaded: return "threaded";
        case execution_kind::gpu: return "gpu";
        case execution_kind::distributed: return "distributed";
        }
        return "unknown";
    }

    // =========================================================================
    // analysis_outcome — result of a legality or profitability query
    // =========================================================================

    enum class analysis_outcome : std::uint8_t {
        proven_legal = 0, // statically proven safe for the requested kind
        proven_illegal = 1, // statically proven unsafe (hard reject)
        unknown = 2, // cannot prove either way (may need runtime guard)
    };

    [[nodiscard]] inline constexpr std::string_view to_string(analysis_outcome o) noexcept {
        switch (o) {
        case analysis_outcome::proven_legal: return "proven_legal";
        case analysis_outcome::proven_illegal: return "proven_illegal";
        case analysis_outcome::unknown: return "unknown";
        }
        return "unknown";
    }

    // =========================================================================
    // region_class — structural classification of a loop / task region
    // =========================================================================

    enum class region_class : std::uint8_t {
        scalar_only = 0, // no parallelism applicable
        independent_loop = 1, // iterations have no cross-iteration deps
        reduction_loop = 2, // single accumulation with associative op
        pipelineable_loop = 3, // producer-consumer pipeline structure
        task_region = 4, // explicit task-parallel region
        gpu_candidate = 5, // independent + device-resident data
        transaction_region = 6, // Medha transaction — stay scalar
        unknown = 7,
    };

    [[nodiscard]] inline constexpr std::string_view to_string(region_class c) noexcept {
        switch (c) {
        case region_class::scalar_only: return "scalar_only";
        case region_class::independent_loop: return "independent_loop";
        case region_class::reduction_loop: return "reduction_loop";
        case region_class::pipelineable_loop: return "pipelineable_loop";
        case region_class::task_region: return "task_region";
        case region_class::gpu_candidate: return "gpu_candidate";
        case region_class::transaction_region: return "transaction_region";
        case region_class::unknown: return "unknown";
        }
        return "unknown";
    }

    // =========================================================================
    // dependency_summary — loop-carried / cross-iteration dependence rollup
    //
    // Placed here (not execution_plan.hpp) so legality.hpp can use it
    // without a circular include.
    // =========================================================================

    struct dependency_summary {
        bool has_loop_carried = false; // any loop-carried data dependency
        bool has_reduction_only = false; // loop-carried deps are all reductions
        bool has_control_dep = false; // conditional control dependencies exist
        bool has_cross_iter_raw = false; // RAW across loop iterations (blocks SIMD)
        bool has_unknown_dep = false; // cannot statically classify all deps
    };
} // namespace lithe::exec
