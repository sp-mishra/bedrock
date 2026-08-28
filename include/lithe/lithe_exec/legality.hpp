#pragma once

// =============================================================================
// lithe_exec/legality.hpp — Per-kind legality checks over a region
//
// Namespace: lithe::exec
//
// Provides:
//   target_capabilities — backend_capability_set + numeric limits
//                         (extends lithe_execution::target_constraints)
//   loop_info_view      — lightweight view over a polyhedral_loop for legality
//   check_legality(kind, region, effect, memory, loop, layout, target)
//                       → analysis_outcome
//
// Design:
//   Implements the 10-step reject ladder (design §4.2):
//     1. Policy allows this kind?
//     2. Target capability for this kind present?
//     3. Effect legality (effect_summary predicates)
//     4. Transaction region → force scalar
//     5. SIMD: unit stride + alignment check
//     6. GPU: device_available + memory address space
//     7. Threaded: no unsynchronized write conflicts (memory_summary)
//     8. Threaded/GPU: loop-carried deps (dep summary)
//     9. Reductions: associativity check
//     10. Unknown alias / stride / trip count → unknown (may need guard)
//
//   Returns proven_legal / proven_illegal / unknown.
//   No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include "exec_kinds.hpp"
#include "exec_hint.hpp"
#include "effect_summary.hpp"
#include "memory_summary.hpp"
#include "layout_summary.hpp"
#include "reduction.hpp"

#include "../lithe_execution/foundation.hpp"

#include <cstdint>
#include <span>

namespace lithe::exec {
    // =========================================================================
    // target_capabilities — backend capability set + numeric resource limits
    //
    // Extends lithe::execution::backend_capability_set with the numeric fields
    // that legality/profitability need. Does NOT duplicate backend_capability_set.
    // =========================================================================

    struct target_capabilities {
        lithe::execution::backend_capability_set caps;

        // Physical numeric limits
        std::uint32_t vector_width_bytes = 16; // SIMD width in bytes (e.g. 16=SSE, 32=AVX2)
        std::uint64_t shared_memory_bytes = 0; // GPU shared memory per block (0 = unknown)
        std::uint64_t device_memory_bytes = 0; // Total GPU device memory (0 = unknown)
        std::uint32_t max_threads = 1; // Max concurrent HW threads
        bool gpu_device_present = false;

        [[nodiscard]] constexpr bool has(lithe::execution::backend_feature f) const noexcept {
            return caps.has(f);
        }

        [[nodiscard]] constexpr bool simd_capable() const noexcept {
            return vector_width_bytes >= 16;
        }

        [[nodiscard]] constexpr bool threaded_capable() const noexcept {
            return max_threads > 1;
        }

        [[nodiscard]] constexpr bool gpu_capable() const noexcept {
            return gpu_device_present;
        }
    };

    // =========================================================================
    // loop_info_view — lightweight loop info for legality checks
    //
    // Decoupled from polyhedral_loop to avoid including lithe_poly.hpp in this
    // narrow legality header. Caller fills it from polyhedral_analysis_result.
    // =========================================================================

    struct loop_info_view {
        bool has_loop = false;
        bool trip_count_known = false;
        std::int64_t trip_count = 0; // 0 = unknown
        bool is_affine = false;
        std::uint32_t depth = 0; // nesting depth
    };

    // =========================================================================
    // region_context — all info about a region passed to check_legality
    // =========================================================================

    struct region_context {
        std::uint32_t region_id = 0;
        region_class cls = region_class::unknown;
        bool in_transaction = false;
        dependency_summary* deps = nullptr; // may be null (treated as unknown)
        std::span<const reduction_info> reductions = {};
    };

    // =========================================================================
    // check_legality — 10-step reject ladder
    // =========================================================================

    [[nodiscard]] inline analysis_outcome check_legality(
        execution_kind candidate,
        const region_context& region,
        const effect_summary& effects,
        const memory_summary& memory,
        const loop_info_view& loop,
        const layout_summary& layout,
        const target_capabilities& target,
        const auto_execution_policy& policy) noexcept {
        // Step 1: Policy allows this kind?
        if (!policy.allows(candidate)) return analysis_outcome::proven_illegal;

        // Step 2: Target capability for this kind?
        switch (candidate) {
        case execution_kind::simd:
            if (!target.simd_capable()) return analysis_outcome::proven_illegal;
            break;
        case execution_kind::threaded:
            if (!target.threaded_capable()) return analysis_outcome::proven_illegal;
            break;
        case execution_kind::gpu:
            if (!target.gpu_capable()) return analysis_outcome::proven_illegal;
            break;
        case execution_kind::distributed:
            return analysis_outcome::proven_illegal; // not yet supported
        case execution_kind::scalar:
            break;
        }

        // Step 3: Effect legality
        switch (candidate) {
        case execution_kind::gpu:
            if (!gpu_legal(effects)) return analysis_outcome::proven_illegal;
            break;
        case execution_kind::simd:
            if (!simd_legal(effects)) return analysis_outcome::proven_illegal;
            break;
        case execution_kind::threaded:
            if (!threaded_legal(effects)) return analysis_outcome::proven_illegal;
            break;
        default:
            break;
        }

        // Step 4: Transaction region → force scalar
        if (region.in_transaction || region.cls == region_class::transaction_region) {
            return (candidate == execution_kind::scalar)
                       ? analysis_outcome::proven_legal
                       : analysis_outcome::proven_illegal;
        }

        // Step 5: SIMD — requires unit stride + sufficient alignment
        if (candidate == execution_kind::simd) {
            if (!loop.is_affine) return analysis_outcome::proven_illegal;
            if (!layout.contiguous) return analysis_outcome::proven_illegal;
            if (!layout.simd_eligible(target.vector_width_bytes))
                return analysis_outcome::proven_illegal;
        }

        // Step 6: GPU — check address space / device residency
        if (candidate == execution_kind::gpu) {
            if (layout.space == address_space::host && !target.gpu_device_present)
                return analysis_outcome::proven_illegal;
            if (!target.gpu_device_present)
                return analysis_outcome::proven_illegal;
            // Unknown residency → may need runtime guard
            if (layout.space == address_space::unknown)
                return analysis_outcome::unknown;
        }

        // Step 7: Threaded — no unsynchronized conflicting writes
        if (candidate == execution_kind::threaded || candidate == execution_kind::gpu) {
            if (memory.has_unknown_aliasing())
                return analysis_outcome::unknown; // guard needed
        }

        // Step 8: Loop-carried dependence check
        if (candidate == execution_kind::threaded || candidate == execution_kind::simd ||
            candidate == execution_kind::gpu) {
            if (region.deps) {
                if (region.deps->has_unknown_dep)
                    return analysis_outcome::unknown;
                if (region.deps->has_loop_carried && !region.deps->has_reduction_only)
                    return analysis_outcome::proven_illegal;
                if (region.deps->has_cross_iter_raw && candidate == execution_kind::simd)
                    return analysis_outcome::proven_illegal;
            }
        }

        // Step 9: Reduction associativity
        if (region.cls == region_class::reduction_loop) {
            for (const auto& red : region.reductions) {
                if (!red.associative) return analysis_outcome::proven_illegal;
            }
        }

        // Step 10: Unknown trip count → unknown for threaded/GPU (min_trip_count guard)
        if (candidate == execution_kind::threaded || candidate == execution_kind::gpu) {
            if (loop.has_loop && !loop.trip_count_known)
                return analysis_outcome::unknown; // guard: min_trip_count
        }

        return analysis_outcome::proven_legal;
    }
} // namespace lithe::exec
