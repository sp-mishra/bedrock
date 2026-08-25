#pragma once

// =============================================================================
// lithe_schedule_bridge.hpp — Schedule-policy selection bridge
//
// Namespace:  lithe::intelligence
// Depends on: lithe/lithe_decision_engine.hpp (decision_engine, cost_ranking,
//                                              candidate, ranked)
//             lithe/lithe_cost_model.hpp      (cost_vector, cost_context)
//             lithe/lithe_feature_extractor.hpp (mir_features, feature_vector)
//             <array>, <expected>, <string_view>
//
// Provides:
//   schedule_policy_id       — stable enum mapping to pravaha scheduler policies
//   choose_schedule(...)     → schedule_policy_id  (heuristic pipeline)
//
//   Strengthened scheduling interfaces (Gap 3 — shared cost/feature infra):
//
//   scheduler_features       — feature_vector specialized for scheduling decisions;
//                              extracted from mir_features without pravaha includes
//   scheduler_cost_model     — adapts lithe::cost::cost_vector to pravaha scheduling;
//                              satisfies cost_estimator for schedule_policy_id nodes
//   scheduler_strategy<S>    — concept: S can select a schedule_policy_id from
//                              scheduler_features + cost_context; mirrors
//                              selector_strategy so compilation and execution
//                              decisions share the same concept vocabulary
//
// Design:
//   • id-based: Lithe never includes pravaha headers.  The caller maps id →
//     concrete pravaha policy.  Both libraries consume cost_vector/mir_features
//     and are therefore comparable without a circular dependency.
//   • Built on decision_engine<cost_ranking<schedule_policy_id>> so the
//     policy selection participates in the same unified ranking pipeline.
//   • scheduler_strategy<S> + scheduler_cost_model mean Pravaha can eventually
//     consume the same Feature Extraction / Cost Framework / Decision Engine
//     used by Lithe — sharing infrastructure across compile and execute decisions.
//   • No virtual, no macros. C++23. Header-only.
// =============================================================================

#include "lithe_cost_model.hpp"
#include "lithe_decision_engine.hpp"
#include "lithe_feature_extractor.hpp"
#include "lithe_algorithms/selection.hpp"
#include "lithe_execution/foundation.hpp"

#include <array>
#include <expected>
#include <string_view>

namespace lithe::intelligence {
    // =============================================================================
    // schedule_policy_id
    //
    // Stable ids that mirror pravaha's scheduler policy taxonomy:
    //   fifo           → pravaha::fifo_scheduler_policy
    //   priority       → pravaha::priority_scheduler_policy
    //   critical_path  → pravaha::critical_path_scheduler_policy
    //   work_stealing  → pravaha::work_stealing_scheduler_policy
    //   locality       → pravaha::locality_scheduler_policy
    //   gpu            → pravaha::gpu_scheduler_policy
    // =============================================================================

    enum class schedule_policy_id : std::uint8_t {
        fifo = 0,
        priority = 1,
        critical_path = 2,
        work_stealing = 3,
        locality = 4,
        gpu = 5,
    };

    [[nodiscard]] inline constexpr std::string_view to_string(schedule_policy_id id) noexcept {
        switch (id) {
        case schedule_policy_id::fifo: return "fifo";
        case schedule_policy_id::priority: return "priority";
        case schedule_policy_id::critical_path: return "critical_path";
        case schedule_policy_id::work_stealing: return "work_stealing";
        case schedule_policy_id::locality: return "locality";
        case schedule_policy_id::gpu: return "gpu";
        }
        return "unknown";
    }

    // =============================================================================
    // Mapping thresholds (constexpr — can be overridden at compile time)
    // =============================================================================

    // critical_path_len as fraction of instruction_count; 0 if instruction_count==0
    inline constexpr float kCritPathRatio = 0.5f; // ratio > this → critical_path
    // block_count / instruction_count as parallelism proxy
    inline constexpr float kParallelRatio = 0.15f; // ratio > this → work_stealing
    inline constexpr std::size_t kGpuLoopDepth = 2; // min loop_depth for gpu hint

    // =============================================================================
    // choose_schedule
    //
    // Maps mir_features + cost_context → schedule_policy_id via a small
    // decision_engine<cost_ranking> run.
    //
    // The cost stage assigns a heuristic cost_vector to each policy candidate:
    //   critical_path:  low latency for deep DAGs; high cost otherwise
    //   work_stealing:  low latency for high-parallelism graphs
    //   gpu:            zero latency when GPU backend; high cost otherwise
    //   priority:       moderate baseline always viable
    //   fifo:           high latency (last resort)
    //   locality:       medium latency (NUMA-affine workloads)
    //
    // The engine returns the best-first ranking; choose_schedule returns [0].
    // =============================================================================

    [[nodiscard]] inline schedule_policy_id
    choose_schedule(const features::mir_features& mf,
                    const cost::cost_context& ctx) noexcept {
        using T = schedule_policy_id;

        constexpr std::array<T, 6> all_policies = {
            T::fifo, T::priority, T::critical_path,
            T::work_stealing, T::locality, T::gpu,
        };

        // Determine GPU backend from ctx.backend_id
        const bool is_gpu_backend =
            ctx.backend_id.find("gpu") != std::string_view::npos ||
            ctx.backend_id.find("vulkan") != std::string_view::npos ||
            ctx.backend_id.find("metal") != std::string_view::npos;

        // Derived ratios from mir_features (integer fields)
        const float crit_ratio = (mf.instruction_count > 0)
                                     ? static_cast<float>(mf.critical_path_len) /
                                     static_cast<float>(mf.instruction_count)
                                     : 0.0f;

        const float parallel_ratio = (mf.instruction_count > 0)
                                         ? static_cast<float>(mf.block_count) /
                                         static_cast<float>(mf.instruction_count)
                                         : 0.0f;

        auto gen = [&]() -> const std::array<T, 6>& { return all_policies; };

        auto feat = [](T) -> features::feature_vector { return {}; };

        auto cost_fn = [&](T policy, const features::feature_vector&,
                           const cost::cost_context&) -> cost::cost_vector {
            cost::cost_vector cv;
            switch (policy) {
            case T::critical_path:
                // Preferred for deep DAGs: low latency if critical path ratio is large
                cv.latency = (crit_ratio > kCritPathRatio)
                                 ? 0.5f
                                 : 5.0f;
                break;
            case T::work_stealing:
                // Preferred for high block/instruction ratio (many parallel tasks)
                cv.latency = (parallel_ratio > kParallelRatio)
                                 ? 0.5f
                                 : 4.0f;
                break;
            case T::gpu:
                // Preferred when GPU backend is selected and there is loop depth
                cv.latency = (is_gpu_backend && mf.loop_depth >= kGpuLoopDepth)
                                 ? 0.25f
                                 : 10.0f;
                break;
            case T::locality:
                // Moderate: useful for NUMA-sensitive graphs, but not preferred over priority
                cv.latency = 3.5f;
                break;
            case T::priority:
                // Safe default: lowest unconditional cost — wins when no specific signal fires
                cv.latency = 2.5f;
                break;
            case T::fifo:
                // Last resort
                cv.latency = 8.0f;
                break;
            }
            cv.memory = 0.0f;
            cv.power = 0.0f;
            cv.throughput = 0.0f;
            return cv;
        };

        decision_engine<cost_ranking<T>> engine;
        const ranked<T> result = engine.decide<T>(gen, feat, cost_fn, ctx);

        if (result.empty()) return schedule_policy_id::priority;
        return result.ordered.front().value;
    }

    // =========================================================================
    // scheduler_features — feature_vector specialized for scheduling decisions
    //
    // Extracted from mir_features without including any pravaha headers.
    // Encodes the scheduling-relevant aspects of a MIR function into a compact
    // float vector so that scheduler_strategy implementations can consume the
    // same feature extraction infrastructure used by compilation decisions.
    //
    // Dimensions (6 total):
    //   [0] critical_path_ratio  = critical_path_len / instruction_count
    //   [1] parallel_ratio       = block_count / instruction_count
    //   [2] loop_depth_norm      = loop_depth / 8.0  (clamped to 1.0)
    //   [3] memory_op_ratio      = memory_op_count / instruction_count
    //   [4] branch_ratio         = branch_count / instruction_count
    //   [5] spill_ratio          = spill_hint_count / vreg_count
    // =========================================================================

    struct scheduler_features {
        static constexpr std::size_t kDims = 6;

        float critical_path_ratio = 0.0f;
        float parallel_ratio = 0.0f;
        float loop_depth_norm = 0.0f;
        float memory_op_ratio = 0.0f;
        float branch_ratio = 0.0f;
        float spill_ratio = 0.0f;

        // Build from mir_features.
        [[nodiscard]] static scheduler_features from_mir(
            const features::mir_features& mf) noexcept {
            const auto ic = static_cast<float>(mf.instruction_count);
            const auto vc = static_cast<float>(mf.vreg_count);
            scheduler_features sf;
            if (ic > 0.0f) {
                sf.critical_path_ratio =
                    static_cast<float>(mf.critical_path_len) / ic;
                sf.parallel_ratio =
                    static_cast<float>(mf.block_count) / ic;
                sf.memory_op_ratio =
                    static_cast<float>(mf.memory_op_count) / ic;
                sf.branch_ratio =
                    static_cast<float>(mf.branch_count) / ic;
            }
            sf.loop_depth_norm =
                std::min(1.0f, static_cast<float>(mf.loop_depth) / 8.0f);
            if (vc > 0.0f)
                sf.spill_ratio = static_cast<float>(mf.spill_hint_count) / vc;
            return sf;
        }

        // Encode as a feature_vector for use in decision_engine and ML pipelines.
        [[nodiscard]] features::feature_vector to_feature_vector() const {
            features::feature_vector fv;
            fv.append(critical_path_ratio);
            fv.append(parallel_ratio);
            fv.append(loop_depth_norm);
            fv.append(memory_op_ratio);
            fv.append(branch_ratio);
            fv.append(spill_ratio);
            return fv;
        }
    };

    // =========================================================================
    // scheduler_cost_model
    //
    // Assigns a cost_vector to each schedule_policy_id candidate given a
    // scheduler_features context.  Satisfies the cost_estimator concept pattern
    // (but uses schedule_policy_id as the "node" type — a deliberate deviation
    // from the lithe_enode_t norm since scheduling operates on policy ids, not
    // IR nodes).
    //
    // Heuristics:
    //   critical_path: low latency if critical_path_ratio > 0.5, else high
    //   work_stealing: low latency if parallel_ratio > 0.15, else high
    //   gpu:           zero latency if loop_depth_norm > 0.25, else very high
    //   locality:      moderate latency, memory benefit from cache affinity
    //   priority:      safe balanced default
    //   fifo:          last resort (highest latency)
    // =========================================================================

    struct scheduler_cost_model {
        const scheduler_features* sf = nullptr; // non-owning; set before use

        [[nodiscard]] cost::cost_vector estimate(
            schedule_policy_id policy,
            const scheduler_features& features,
            const cost::cost_context& /*ctx*/) const noexcept {
            cost::cost_vector cv;
            switch (policy) {
            case schedule_policy_id::critical_path:
                cv.latency = (features.critical_path_ratio > kCritPathRatio)
                                 ? 0.5f
                                 : 5.0f;
                cv.memory = 0.5f;
                break;
            case schedule_policy_id::work_stealing:
                cv.latency = (features.parallel_ratio > kParallelRatio)
                                 ? 0.5f
                                 : 4.0f;
                cv.memory = 1.0f;
                break;
            case schedule_policy_id::gpu:
                cv.latency = (features.loop_depth_norm > 0.25f) ? 0.25f : 10.0f;
                cv.memory = 2.0f;
                cv.power = 3.0f;
                break;
            case schedule_policy_id::locality:
                cv.latency = 3.5f;
                cv.memory = 0.25f; // cache-affine = low memory cost
                break;
            case schedule_policy_id::priority:
                cv.latency = 2.5f; // lowest unconditional cost — safe default
                cv.memory = 1.5f;
                break;
            case schedule_policy_id::fifo:
                cv.latency = 8.0f;
                cv.memory = 2.0f;
                break;
            }
            cv.throughput = 0.0f;
            // cv.power set per-case above; zero for policies that don't model power.
            return cv;
        }
    };

    // =========================================================================
    // scheduler_strategy<S> — concept
    //
    // A type S satisfies scheduler_strategy iff it can select a schedule_policy_id
    // from a scheduler_features + cost_context pair.
    //
    // Mirrors selector_strategy (lithe_selector_strategy.hpp) so that
    // compilation decisions (backend selector) and execution decisions
    // (scheduler) use the same concept vocabulary and can share documentation,
    // wrappers, and tooling.
    //
    // Minimum requirements:
    //   S::descriptor() → algorithm_descriptor  (static metadata)
    //   s.schedule(sf, ctx) → expected<schedule_policy_id, selection_error>
    //
    // All built-in scheduler strategies are static_assert-checked below.
    // =========================================================================

    template <class S>
    concept scheduler_strategy =
        requires {
            {
                S::descriptor()
            } ->
            std::convertible_to<algorithms::algorithm_descriptor>;
        } &&
        requires(S& s,
                 const scheduler_features& sf,
                 const cost::cost_context& ctx) {
            {
                s.schedule(sf, ctx)
            } ->
            std::same_as<std::expected<schedule_policy_id,
                                       execution::selection_error>>;
        };

    // =========================================================================
    // heuristic_scheduler_strategy
    //
    // Default implementation: wraps choose_schedule() behind the
    // scheduler_strategy concept interface.  Stateless; always succeeds.
    // =========================================================================

    struct heuristic_scheduler_strategy {
        [[nodiscard]] static algorithms::algorithm_descriptor descriptor() noexcept {
            return algorithms::algorithm_descriptor{
                .id = "lithe.scheduler.heuristic",
                .version_major = 1,
                .version_minor = 0,
                .deterministic = true,
                .thread_safe = true,
                .reentrant = true,
                .supports_cancellation = false,
                .safe_for_runtime_replacement = true,
                .required_analyses = {},
            };
        }

        [[nodiscard]] std::expected<schedule_policy_id, execution::selection_error>
        schedule(const scheduler_features& sf,
                 const cost::cost_context& ctx) const noexcept {
            // Re-derive mir_features from scheduler_features for choose_schedule.
            // This is a lightweight reconstruction; scheduler_features is already
            // extracted and does not require a full mir_feature_extractor run.
            features::mir_features mf;
            // Use a synthetic instruction_count of 100 so ratios are recoverable.
            constexpr std::size_t kSyntheticInstrCount = 100;
            mf.instruction_count = kSyntheticInstrCount;
            mf.critical_path_len = static_cast<std::size_t>(
                sf.critical_path_ratio * kSyntheticInstrCount);
            mf.block_count = static_cast<std::size_t>(
                sf.parallel_ratio * kSyntheticInstrCount);
            mf.loop_depth = static_cast<std::size_t>(
                sf.loop_depth_norm * 8.0f);
            return choose_schedule(mf, ctx);
        }
    };

    static_assert(scheduler_strategy<heuristic_scheduler_strategy>);
} // namespace lithe::intelligence
