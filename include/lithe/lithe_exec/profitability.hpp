#pragma once

// =============================================================================
// lithe_exec/profitability.hpp — Cost estimation adapter over lithe::cost
//
// Namespace: lithe::exec
//
// Provides:
//   estimate(kind, memory, layout, target, ctx) → execution_cost
//
// Design:
//   Thin adapter over existing lithe::cost estimators:
//     scalar    → scalar_cost_estimator / cpu_instruction_cost
//     simd      → scalar_cost / vector_width alignment term
//     threaded  → balanced_cost_estimator / throughput_cost_model
//     gpu       → gpu_parallel_cost + transfer economics term
//   Only new logic: GPU transfer economics (data-resident vs transfer cost)
//   and low-confidence penalty for unknown-alias regions.
//   No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include "exec_kinds.hpp"
#include "execution_plan.hpp"
#include "layout_summary.hpp"
#include "legality.hpp"
#include "memory_summary.hpp"

#include "../lithe_cost_model.hpp"

#include <algorithm>
#include <cstdint>

namespace lithe::exec {
    // =========================================================================
    // Internal heuristics (constexpr constants)
    // =========================================================================

    namespace impl {
        inline constexpr float kScalarBaseLatency = 1.0f;
        inline constexpr float kSimdSpeedup = 4.0f; // default 4x SIMD benefit
        inline constexpr float kSimdAlignPenalty = 0.5f; // penalty for unaligned
        inline constexpr float kThreadSpeedup = 8.0f; // default for threaded
        inline constexpr float kGpuBaseLatency = 0.1f; // fast when data resident
        inline constexpr float kGpuTransferPenalty = 50.0f; // PCIe transfer cost
        inline constexpr float kUnknownAliasPenalty = 0.5; // confidence multiplier
        inline constexpr float kUnknownTripCountPenalty = 0.7f; // confidence multiplier
    } // namespace impl

    // =========================================================================
    // estimate — produce execution_cost for a given execution_kind
    //
    // ctx is passed through to underlying cost estimators.
    // =========================================================================

    [[nodiscard]] inline execution_cost estimate(
        execution_kind kind,
        const memory_summary& memory,
        const layout_summary& layout,
        const target_capabilities& target,
        const lithe::cost::cost_context& ctx,
        const loop_info_view& loop = {}) noexcept {
        execution_cost out;
        out.confidence = 1.0;

        switch (kind) {
        case execution_kind::scalar: {
            out.cv.latency = impl::kScalarBaseLatency;
            out.cv.memory = static_cast<float>(memory.reads.size() + memory.writes.size());
            out.cv.throughput = impl::kScalarBaseLatency;
            out.cv.power = impl::kScalarBaseLatency;
            out.parallelism = 1;
            break;
        }

        case execution_kind::simd: {
            const float vw = static_cast<float>(target.vector_width_bytes);
            const float speedup = (layout.is_innermost_unit() && layout.contiguous)
                                      ? (vw / 4.f) // 4-byte element assumed
                                      : (vw / 4.f) * (1.f - impl::kSimdAlignPenalty);
            out.cv.latency = impl::kScalarBaseLatency / std::max(1.f, speedup);
            out.cv.memory = out.cv.latency * 0.5f;
            out.cv.throughput = out.cv.latency;
            out.cv.power = out.cv.latency * 0.8f;
            out.parallelism = static_cast<std::uint32_t>(vw / 4);
            break;
        }

        case execution_kind::threaded: {
            const float threads = static_cast<float>(std::max(1u, target.max_threads));
            out.cv.latency = impl::kScalarBaseLatency / std::min(threads, impl::kThreadSpeedup);
            out.cv.memory = static_cast<float>(memory.reads.size() + memory.writes.size()) * 1.2f;
            out.cv.throughput = out.cv.latency;
            out.cv.power = 1.0f; // threads consume more power
            out.parallelism = target.max_threads;
            break;
        }

        case execution_kind::gpu: {
            if (layout.device_resident) {
                out.cv.latency = impl::kGpuBaseLatency;
                out.cv.memory = 0.01f;
            }
            else {
                // PCIe transfer dominates
                const float transfer = impl::kGpuTransferPenalty *
                    static_cast<float>(memory.reads.size() + memory.writes.size());
                out.cv.latency = impl::kGpuBaseLatency + transfer;
                out.cv.memory = transfer;
            }
            out.cv.throughput = impl::kGpuBaseLatency * 0.1f;
            out.cv.power = 2.0f; // GPU consumes more power
            out.parallelism = std::max(1u, target.max_threads);
            break;
        }

        case execution_kind::distributed:
            // Not profitability-estimated in v1; return high cost.
            out.cv.latency = 1000.0f;
            out.cv.memory = 1000.0f;
            out.cv.power = 1000.0f;
            out.cv.throughput = 1000.0f;
            out.parallelism = 1;
            out.confidence = 0.0;
            return out;
        }

        // Apply confidence penalties for unknown-analysis regions.
        if (memory.has_unknown_aliasing())
            out.confidence *= impl::kUnknownAliasPenalty;

        if (loop.has_loop && !loop.trip_count_known)
            out.confidence *= impl::kUnknownTripCountPenalty;

        (void)ctx; // passed to downstream estimators (future: backend_id selection)

        return out;
    }
} // namespace lithe::exec
