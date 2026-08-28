#pragma once

// =============================================================================
// lithe_cost_model.hpp — Lithe cost models for e-graph extraction
//
// Namespace:  lithe::cost
// Depends on: containers/graph/egraph.hpp  (cost_model concept, e_node,
//                                           node_count_cost)
//             lithe/lithe_passes.hpp        (emit::tag_descriptor, add_tag, …)
//             <span>, <cstddef>, <algorithm>, <string_view>, <cstdint>
//
// Provides:
//   cost_model<C,Node>     — re-exported concept alias (egraph::cost_model)
//
//   Unified cost framework (namespace lithe::cost — platform capability):
//     metric_id              — named enum for the four canonical cost axes;
//                              extension ids >= kExtensionMetricIdBase (1000)
//     cost_vector            — multi-dimensional cost (latency, memory, power, throughput)
//     cost_context           — evaluation context (backend_id, hw_signature, profile_id)
//     cost_estimator<C,Node> — concept: produces cost_vector from a node + context
//
//   Built-in cost models (cost_t = std::size_t; all noexcept):
//     ast_size_cost          — min node count (alias: egraph::node_count_cost)
//     cpu_instruction_cost   — heuristic: div→4, neg→2, add/mul→1 + child_sum
//     gpu_parallel_cost      — heuristic: div→8, neg→1 + child_sum
//     tensor_fusion_cost     — heuristic: mul→0, div→6 + child_sum
//     memory_cost_model      — penalise deep pointer chains; depth-based
//     power_cost_model       — minimise energy: div→8, mul→2, add→1 + child_sum
//     throughput_cost_model  — maximise throughput: add/mul→1, div→4,
//                              others→2 + max(child_costs)
//
//   Built-in cost estimators (produce cost_vector):
//     scalar_cost_estimator<CM> — wraps any cost_model; maps scalar cost to
//                                 one dimension of cost_vector
//     balanced_cost_estimator   — heuristic: estimates all four dimensions
//
// NOTE: extract_best is NOT defined here to avoid a circular include with
//       lithe_egraph.hpp (which defines lithe_egraph_t).  Use the free
//       function egraph::extract_best from containers/graph/egraph.hpp
//       directly, or via lithe_egraph.hpp.
// =============================================================================

#include "containers/graph/egraph.hpp"
#include "lithe_passes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lithe::cost {
    // =============================================================================
    // metric_id — stable identifier for a named cost metric
    //
    // Provides named enum constants for the four canonical cost axes so that
    // code consuming cost_vector can reference axes by name rather than by
    // struct member.  The four built-in ids map 1:1 to cost_vector fields;
    // extension metrics use ids >= kExtensionMetricIdBase.
    //
    // The id band [0, kExtensionMetricIdBase) is reserved for built-in axes.
    // Downstream subsystems (Pravaha, Sutra, plugin backends) assign extension
    // metric ids starting at kExtensionMetricIdBase.
    // =============================================================================

    inline constexpr std::size_t kExtensionMetricIdBase = 1000;

    enum class metric_id : std::uint32_t {
        latency = 0, // end-to-end execution time proxy
        memory = 1, // peak working-set size
        power = 2, // energy proxy
        throughput = 3, // inverted: lower = higher actual throughput
    };

    [[nodiscard]] inline constexpr std::string_view to_string(metric_id id) noexcept {
        switch (id) {
        case metric_id::latency: return "latency";
        case metric_id::memory: return "memory";
        case metric_id::power: return "power";
        case metric_id::throughput: return "throughput";
        }
        return "extension";
    }

    // =============================================================================
    // cost_vector — multi-dimensional cost tuple
    //
    // Captures four independent cost axes used by backend selection, auto-tuning,
    // profile selection, and e-graph extraction:
    //
    //   latency    — end-to-end execution time (lower = faster wall-clock)
    //   memory     — peak working-set size (lower = more cache-friendly)
    //   power      — energy consumption proxy (lower = less power draw)
    //   throughput — ops per time unit (higher = better; stored inverted as cost)
    //
    // All fields are float so the vector can be used as weights in a linear
    // scoring function without precision loss for typical heuristic values.
    //
    // dominates(other): true iff this is Pareto-dominant (≤ on all axes, < on one).
    // weighted_sum(w):  scalar projection used by selection policies.
    // =============================================================================

    struct cost_vector {
        float latency = 0.0f;
        float memory = 0.0f;
        float power = 0.0f;
        float throughput = 0.0f; // inverted: lower stored cost = higher throughput

        [[nodiscard]] constexpr bool dominates(const cost_vector& o) const noexcept {
            return latency <= o.latency &&
                memory <= o.memory &&
                power <= o.power &&
                throughput <= o.throughput &&
                (latency < o.latency ||
                    memory < o.memory ||
                    power < o.power ||
                    throughput < o.throughput);
        }

        // Weighted scalar projection.  w_* are relative axis weights (need not sum to 1).
        [[nodiscard]] constexpr float weighted_sum(
            float w_latency = 1.0f, float w_memory = 1.0f,
            float w_power = 1.0f, float w_throughput = 1.0f) const noexcept {
            return w_latency * latency +
                w_memory * memory +
                w_power * power +
                w_throughput * throughput;
        }

        [[nodiscard]] constexpr bool operator==(const cost_vector&) const noexcept = default;
    };

    // =============================================================================
    // cost_context — evaluation context for cost estimators
    //
    // Carries the three pieces of context that a cost estimator may need to
    // specialize its estimates:
    //
    //   backend_id      — target backend (e.g. "lithe.interp", "lithe.jit.asmjit")
    //   hw_signature    — 64-bit hardware fingerprint (CPU family, GPU device id, …)
    //   profile_id      — active optimization profile (e.g. "std.o3", "tensor.o3")
    //
    // All fields are optional sentinels; estimators that do not need them see
    // empty strings / zero and apply their default heuristic.
    // =============================================================================

    struct cost_context {
        std::string_view backend_id = {}; // stable view; lifetime managed by caller
        std::uint64_t hw_signature = 0;
        std::string_view profile_id = {};

        [[nodiscard]] constexpr bool operator==(const cost_context&) const noexcept = default;
    };

    // =============================================================================
    // cost_estimator<C, Node> — concept
    //
    // Generalization of cost_model that produces a cost_vector instead of a scalar.
    // Allows backend selection, auto-tuning, and e-graph extraction to share a
    // single decision framework.
    //
    // A type C satisfies cost_estimator<C,Node> iff:
    //   c.estimate(node, child_costs, ctx) → cost_vector
    // where child_costs is span<const cost_vector>.
    //
    // Existing cost_model types are NOT automatically estimators; use
    // scalar_cost_estimator<CM> to wrap them.
    // =============================================================================

    template <class C, class Node>
    concept cost_estimator =
        requires(C& c, const Node& n,
                 std::span<const cost_vector> cv,
                 const cost_context& ctx) {
            { c.estimate(n, cv, ctx) } -> std::same_as<cost_vector>;
        };

    // =============================================================================
    // cost_model — concept alias
    //
    // Re-exports egraph::cost_model so code that only includes lithe_cost_model.hpp
    // can write lithe::cost::cost_model<C,Node> without depending on egraph.hpp
    // for its namespace.
    // =============================================================================

    template <class C, class Node>
    concept cost_model = ::egraph::cost_model<C, Node>;

    // Convenience: the concrete node type used by all Lithe cost models.
    using lithe_enode_t = ::egraph::e_node<std::size_t, std::size_t>;

    // =============================================================================
    // ast_size_cost — domain-neutral minimum-node-count default
    //
    // Identical to egraph::node_count_cost; provided under the lithe::cost
    // namespace so callers do not need to spell out the egraph:: prefix.
    // =============================================================================

    using ast_size_cost = ::egraph::node_count_cost;

    // =============================================================================
    // cpu_instruction_cost
    //
    // Heuristic for CPU scalar register machines:
    //   div  → 4  (latency-bound; forced serialisation)
    //   neg  → 2  (two-operand but single-uop on most µarches)
    //   add  → 1  (cheap; often fused with memory)
    //   mul  → 1  (fused-multiply: same throughput as add on modern CPUs)
    //   else → 1  (unknown op: conservative unit cost)
    //
    // Total cost = op_cost + sum(child_costs).
    // =============================================================================

    struct cpu_instruction_cost {
        using cost_t = std::size_t;

        [[nodiscard]] cost_t cost(
            const lithe_enode_t& n,
            std::span<const cost_t> child_costs) const noexcept {
            cost_t child_sum = 0;
            for (auto c : child_costs) child_sum += c;

            cost_t op_cost = 1;
            if (n.op == emit::tag_descriptor<div_tag>::stable_id) op_cost = 4;
            else if (n.op == emit::tag_descriptor<neg_tag>::stable_id) op_cost = 2;
            // add_op and mul_op both map to 1 (the initialised value); no branch needed.

            return op_cost + child_sum;
        }
    };

    static_assert(cost_model<cpu_instruction_cost, lithe_enode_t>);

    // =============================================================================
    // gpu_parallel_cost
    //
    // Heuristic for GPU vector pipelines:
    //   div  → 8  (non-vectorizable; forced scalar fallback path)
    //   else → 1  (add/mul/neg are natively vectorizable)
    //
    // Total cost = op_cost + sum(child_costs).
    // =============================================================================

    struct gpu_parallel_cost {
        using cost_t = std::size_t;

        [[nodiscard]] cost_t cost(
            const lithe_enode_t& n,
            std::span<const cost_t> child_costs) const noexcept {
            cost_t child_sum = 0;
            for (auto c : child_costs) child_sum += c;

            const cost_t op_cost =
                (n.op == emit::tag_descriptor<div_tag>::stable_id) ? 8u : 1u;

            return op_cost + child_sum;
        }
    };

    static_assert(cost_model<gpu_parallel_cost, lithe_enode_t>);

    // =============================================================================
    // tensor_fusion_cost
    //
    // Heuristic for fused multiply-add (FMA) kernel generators:
    //   mul  → 0  (absorbed into FMA fusion; no extra instruction emitted)
    //   div  → 6  (reciprocal approximation + Newton–Raphson: expensive on tensors)
    //   else → 1
    //
    // Total cost = op_cost + sum(child_costs).
    // =============================================================================

    struct tensor_fusion_cost {
        using cost_t = std::size_t;

        [[nodiscard]] cost_t cost(
            const lithe_enode_t& n,
            std::span<const cost_t> child_costs) const noexcept {
            cost_t child_sum = 0;
            for (auto c : child_costs) child_sum += c;

            cost_t op_cost = 1;
            if (n.op == emit::tag_descriptor<mul_tag>::stable_id) op_cost = 0;
            else if (n.op == emit::tag_descriptor<div_tag>::stable_id) op_cost = 6;

            return op_cost + child_sum;
        }
    };

    static_assert(cost_model<tensor_fusion_cost, lithe_enode_t>);

    // =============================================================================
    // memory_cost_model
    //
    // Penalises deep pointer chains (e.g., repeated dereference / address-of
    // sequences that cause cache misses in pointer-chasing workloads).
    //
    // Model:
    //   depth = max(child_costs) + 1     (tracks the longest pointer chain depth)
    //   deref/addr ops double the depth  (pointer indirection doubles miss penalty)
    //
    // Since Lithe's built-in surface ops do not include explicit deref/addr tags,
    // leaf nodes (no children) cost 1.  Any op whose stable_id matches an
    // unknown/extension op and whose arity is 1 is treated as a potential pointer
    // op and penalised by ×2.
    //
    // Tune by sub-classing and overriding is_indirect_op().
    // =============================================================================

    struct memory_cost_model {
        using cost_t = std::size_t;

        // Returns true for ops that represent an indirection (deref, load, addr-of).
        // Override in a derived struct for domain-specific pointer ops.
        [[nodiscard]] static bool is_indirect_op(std::size_t /*op*/) noexcept {
            // No standard Lithe surface op is an explicit deref; always returns false
            // for built-in ops.  Extension code should subclass and override.
            return false;
        }

        [[nodiscard]] cost_t cost(
            const lithe_enode_t& n,
            std::span<const cost_t> child_costs) const noexcept {
            // Leaf: depth 1 (the node itself).
            if (child_costs.empty()) return 1u;

            // Depth = deepest child + 1.
            cost_t max_child = 0;
            for (auto c : child_costs) {
                if (c > max_child) max_child = c;
            }
            const cost_t depth = max_child + 1u;

            // Indirect ops (pointer-chasing) double the effective depth.
            return is_indirect_op(n.op) ? depth * 2u : depth;
        }
    };

    static_assert(cost_model<memory_cost_model, lithe_enode_t>);

    // =============================================================================
    // power_cost_model
    //
    // Heuristic for energy-efficient code generation (battery-constrained targets,
    // embedded processors, or power-capped HPC nodes):
    //   div  → 8  (hardware divider: high switching activity, multi-cycle)
    //   mul  → 2  (multiplier array: moderate switching activity)
    //   add  → 1  (adder: lowest switching activity)
    //   else → 1
    //
    // Total cost = op_cost + sum(child_costs).
    // =============================================================================

    struct power_cost_model {
        using cost_t = std::size_t;

        [[nodiscard]] cost_t cost(
            const lithe_enode_t& n,
            std::span<const cost_t> child_costs) const noexcept {
            cost_t child_sum = 0;
            for (auto c : child_costs) child_sum += c;

            cost_t op_cost = 1;
            if (n.op == emit::tag_descriptor<div_tag>::stable_id) op_cost = 8;
            else if (n.op == emit::tag_descriptor<mul_tag>::stable_id) op_cost = 2;
            // add_tag, neg_tag, sub_tag: default 1

            return op_cost + child_sum;
        }
    };

    static_assert(cost_model<power_cost_model, lithe_enode_t>);

    // =============================================================================
    // throughput_cost_model
    //
    // Heuristic for instruction-throughput-limited pipelines (vectorised loops,
    // SIMD-heavy kernels where latency is hidden by out-of-order execution):
    //
    //   add/mul → 1  (fully pipelined; throughput = 1 per cycle)
    //   div     → 4  (unpipelined; stalls the issue queue)
    //   others  → 2  (conservative mid-tier throughput)
    //
    // Key difference from cpu_instruction_cost: cost = op_cost + max(child_costs)
    // rather than sum, reflecting that a superscalar/OOO machine can overlap
    // sibling sub-expressions in separate execution ports.
    // =============================================================================

    struct throughput_cost_model {
        using cost_t = std::size_t;

        [[nodiscard]] cost_t cost(
            const lithe_enode_t& n,
            std::span<const cost_t> child_costs) const noexcept {
            // Critical-path depth model: use max of children, not sum.
            cost_t max_child = 0;
            for (auto c : child_costs) {
                if (c > max_child) max_child = c;
            }

            cost_t op_cost = 2; // default: unrecognised op
            if (n.op == emit::tag_descriptor<add_tag>::stable_id ||
                n.op == emit::tag_descriptor<mul_tag>::stable_id) {
                op_cost = 1;
            }
            else if (n.op == emit::tag_descriptor<div_tag>::stable_id) {
                op_cost = 4;
            }

            return op_cost + max_child;
        }
    };

    static_assert(cost_model<throughput_cost_model, lithe_enode_t>);

    // =============================================================================
    // scalar_cost_estimator<CM>
    //
    // Adapter: wraps any cost_model CM and maps its scalar cost_t into one axis
    // of cost_vector.  The mapping is:
    //   latency    ← static_cast<float>(scalar)
    //   memory     ← 0.0f  (not estimated by a scalar model)
    //   power      ← 0.0f
    //   throughput ← 0.0f
    //
    // cost_context is accepted but ignored by scalar models; override estimate()
    // in a derived type to add context-sensitivity.
    // =============================================================================

    template <class CM>
        requires cost_model<CM, lithe_enode_t>
    struct scalar_cost_estimator {
        CM inner{};

        [[nodiscard]] cost_vector estimate(
            const lithe_enode_t& n,
            std::span<const cost_vector> child_cvs,
            const cost_context& /*ctx*/) const noexcept {
            // Reconstruct scalar child_costs from the latency axis of child vectors.
            // Stack-allocate up to 16 children inline; spill to stack array for larger.
            using scalar_t = typename CM::cost_t;
            scalar_t buf[16]{};
            const std::size_t nc = std::min(child_cvs.size(), std::size_t{16});
            for (std::size_t i = 0; i < nc; ++i)
                buf[i] = static_cast<scalar_t>(child_cvs[i].latency);

            const scalar_t sc = inner.cost(n, std::span<const scalar_t>{buf, nc});
            return cost_vector{.latency = static_cast<float>(sc)};
        }
    };

    static_assert(cost_estimator<scalar_cost_estimator<cpu_instruction_cost>, lithe_enode_t>);

    // =============================================================================
    // balanced_cost_estimator
    //
    // Heuristic estimator producing all four cost_vector axes simultaneously.
    // Used by cost_based_backend_selector and auto_tuner when a richer signal
    // is needed than a single-axis scalar model.
    //
    // Axis heuristics (built-in surface ops):
    //   latency    — cpu_instruction_cost-like (div→4, neg→2, else→1)
    //   memory     — pointer-chain depth (deref/addr ops double depth)
    //   power      — power_cost_model-like (div→8, mul→2, else→1)
    //   throughput — throughput_cost_model-like (critical path, max(children))
    //
    // context sensitivity:
    //   If ctx.backend_id == "lithe.jit.asmjit" or "lithe.hetero.*":
    //     gpu_parallel_cost applied to latency axis.
    // =============================================================================

    struct balanced_cost_estimator {
        [[nodiscard]] cost_vector estimate(
            const lithe_enode_t& n,
            std::span<const cost_vector> child_cvs,
            const cost_context& ctx) const noexcept {
            // ── Accumulate child axis sums and max for critical-path axis ─────────
            float lat_sum = 0.0f, mem_max = 0.0f, pwr_sum = 0.0f, thr_max = 0.0f;
            for (const auto& cv : child_cvs) {
                lat_sum += cv.latency;
                if (cv.memory > mem_max) mem_max = cv.memory;
                pwr_sum += cv.power;
                if (cv.throughput > thr_max) thr_max = cv.throughput;
            }

            // ── Per-op costs ──────────────────────────────────────────────────────
            const std::size_t op = n.op;
            const bool is_div = (op == emit::tag_descriptor<div_tag>::stable_id);
            const bool is_mul = (op == emit::tag_descriptor<mul_tag>::stable_id);
            const bool is_neg = (op == emit::tag_descriptor<neg_tag>::stable_id);

            // Latency axis — GPU backend uses gpu_parallel_cost heuristic
            float lat_op;
            const bool is_gpu_backend =
                !ctx.backend_id.empty() &&
                (ctx.backend_id.starts_with("lithe.hetero") ||
                    ctx.backend_id == "lithe.jit.vulkan" ||
                    ctx.backend_id == "lithe.jit.metal");
            if (is_gpu_backend) {
                lat_op = is_div ? 8.0f : 1.0f;
            }
            else {
                lat_op = is_div ? 4.0f : is_neg ? 2.0f : 1.0f;
            }

            // Power axis
            const float pwr_op = is_div ? 8.0f : is_mul ? 2.0f : 1.0f;

            // Throughput axis (critical path)
            const float thr_op = is_div
                                     ? 4.0f
                                     : (is_mul ? 1.0f : (op == emit::tag_descriptor<add_tag>::stable_id ? 1.0f : 2.0f));

            // Memory axis — leaf nodes cost 1; non-leaf nodes inherit deepest child
            const float mem_op = child_cvs.empty() ? 1.0f : 0.0f;

            return cost_vector{
                .latency = lat_op + lat_sum,
                .memory = mem_op + mem_max,
                .power = pwr_op + pwr_sum,
                .throughput = thr_op + thr_max,
            };
        }
    };

    static_assert(cost_estimator<balanced_cost_estimator, lithe_enode_t>);
} // namespace lithe::cost
