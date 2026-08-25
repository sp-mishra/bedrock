#pragma once

// =============================================================================
// lithe_feature_extractor.hpp — Feature Extraction Framework
//
// Namespace:  lithe::features
// Depends on: lithe/lithe_core.hpp       (Expression, Terminal concepts, tree::*)
//             lithe/lithe_cost_model.hpp  (lithe_enode_t, stable_id constants)
//             <cstddef>, <cstdint>, <span>, <array>, <concepts>, <vector>
//
// Provides:
//   feature_vector             — dense float array (inline SBO ≤ 32, heap beyond)
//
//   Feature bundle aggregates (plain POD):
//     graph_features           — structural graph statistics (node/edge counts, depth…)
//     expression_features      — op frequencies, tree shape, loop nesting
//     mir_features             — physical MIR instruction/block/vreg counts, CFG metrics
//     runtime_features         — execution-time observations (latency, memory, power…)
//
//   Concept:
//     feature_extractor<F,In>  — F can extract feature_vector from In
//
//   Built-in extractors:
//     graph_feature_extractor        — Expression → graph_features → feature_vector
//     expression_feature_extractor   — Expression → expression_features → feature_vector
//     mir_feature_extractor          — physical_mir_function → mir_features → feature_vector
//     runtime_feature_extractor      — accumulates runtime samples; encodes feature_vector
//     combined_feature_extractor<A,B>— concatenates two extractors' outputs
//
// Design:
//   • No virtual, no macros.  C++23.  Header-only.
//   • feature_vector is plain aggregate; extraction is zero-allocation for ≤32 dims.
//   • All feature bundles are plain aggregates; no inheritance hierarchy.
//   • feature_extractor<F,In> is purely structural — no base class, no registration.
//   • mir_feature_extractor includes "lithe_codegen.hpp" — include that header before
//     instantiating it.  The concept and all other types are codegen-independent.
// =============================================================================

#include "lithe_core.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace lithe::features {
    // =============================================================================
    // feature_vector
    //
    // Dense float array.  Up to kInlineCapacity elements stored inline; overflow
    // spills to a heap vector.  normalize() performs in-place L2 normalization.
    // =============================================================================

    inline constexpr std::size_t kInlineFeatureCapacity = 32;

    struct feature_vector {
        std::array<float, kInlineFeatureCapacity> buf_{};
        std::size_t size_ = 0;
        std::vector<float> overflow_;

        [[nodiscard]] std::size_t size() const noexcept {
            return overflow_.empty() ? size_ : overflow_.size();
        }

        [[nodiscard]] float operator[](std::size_t i) const noexcept {
            return overflow_.empty() ? buf_[i] : overflow_[i];
        }

        [[nodiscard]] std::span<const float> as_span() const noexcept {
            if (overflow_.empty()) return {buf_.data(), size_};
            return overflow_;
        }

        void append(float v) {
            if (overflow_.empty() && size_ < kInlineFeatureCapacity) {
                buf_[size_++] = v;
            }
            else {
                if (overflow_.empty())
                    overflow_.assign(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(size_));
                overflow_.push_back(v);
            }
        }

        void append(std::span<const float> vs) { for (float v : vs) append(v); }

        void clear() noexcept {
            size_ = 0;
            overflow_.clear();
        }

        // In-place L2 normalization; no-op when norm == 0.
        feature_vector& normalize() noexcept {
            float sq = 0.0f;
            for (float v : as_span()) sq += v * v;
            if (sq <= 0.0f) return *this;
            const float inv = 1.0f / __builtin_sqrtf(sq);
            if (overflow_.empty()) {
                for (std::size_t i = 0; i < size_; ++i) buf_[i] *= inv;
            }
            else {
                for (float& v : overflow_) v *= inv;
            }
            return *this;
        }
    };

    // =============================================================================
    // feature_extractor<F, In> concept
    //
    //   f.extract(input) → feature_vector
    // =============================================================================

    template <class F, class In>
    concept feature_extractor =
        requires(F& f, const In& input) {
            { f.extract(input) } -> std::same_as<feature_vector>;
        };

    // =============================================================================
    // graph_features
    //
    // Structural statistics of an expression tree or DAG.
    //   op_frequencies[8] — normalized per-op bucket: add, sub, mul, div, neg,
    //                        compare, logic, other.
    // =============================================================================

    struct graph_features {
        std::size_t node_count = 0;
        std::size_t edge_count = 0;
        std::size_t depth = 0;
        std::size_t leaf_count = 0;
        std::size_t internal_count = 0;
        std::size_t max_fanout = 0;
        std::size_t sharing_count = 0;
        std::array<float, 8> op_frequencies{};

        [[nodiscard]] feature_vector to_feature_vector() const noexcept {
            feature_vector fv;
            fv.append(static_cast<float>(node_count));
            fv.append(static_cast<float>(edge_count));
            fv.append(static_cast<float>(depth));
            fv.append(static_cast<float>(leaf_count));
            fv.append(static_cast<float>(internal_count));
            fv.append(static_cast<float>(max_fanout));
            fv.append(static_cast<float>(sharing_count));
            fv.append(op_frequencies);
            return fv;
        }
    };

    // =============================================================================
    // expression_features
    //
    // Fine-grained expression statistics.
    //   arity_histogram[5] — {leaf, unary, binary, ternary, n-ary} counts.
    // =============================================================================

    struct expression_features {
        std::size_t tree_size = 0;
        std::size_t unique_node_count = 0;
        std::size_t tree_depth = 0;
        std::array<std::size_t, 5> arity_histogram{};
        std::size_t div_count = 0;
        std::size_t mul_count = 0;
        std::size_t add_count = 0;
        std::size_t constant_count = 0;
        std::size_t variable_count = 0;
        std::size_t loop_nesting = 0;

        [[nodiscard]] feature_vector to_feature_vector() const noexcept {
            feature_vector fv;
            fv.append(static_cast<float>(tree_size));
            fv.append(static_cast<float>(unique_node_count));
            fv.append(static_cast<float>(tree_depth));
            for (auto c : arity_histogram) fv.append(static_cast<float>(c));
            fv.append(static_cast<float>(div_count));
            fv.append(static_cast<float>(mul_count));
            fv.append(static_cast<float>(add_count));
            fv.append(static_cast<float>(constant_count));
            fv.append(static_cast<float>(variable_count));
            fv.append(static_cast<float>(loop_nesting));
            return fv;
        }
    };

    // =============================================================================
    // mir_features
    //
    // Physical MIR statistics for scheduling and backend selection.
    //   critical_path_len  — estimated longest dependency chain (block count proxy)
    // =============================================================================

    struct mir_features {
        std::size_t instruction_count = 0;
        std::size_t vreg_count = 0;
        std::size_t block_count = 0;
        std::size_t critical_path_len = 0;
        std::size_t loop_depth = 0;
        std::size_t memory_op_count = 0;
        std::size_t branch_count = 0;
        std::size_t call_count = 0;
        std::size_t spill_hint_count = 0;

        [[nodiscard]] feature_vector to_feature_vector() const noexcept {
            feature_vector fv;
            fv.append(static_cast<float>(instruction_count));
            fv.append(static_cast<float>(vreg_count));
            fv.append(static_cast<float>(block_count));
            fv.append(static_cast<float>(critical_path_len));
            fv.append(static_cast<float>(loop_depth));
            fv.append(static_cast<float>(memory_op_count));
            fv.append(static_cast<float>(branch_count));
            fv.append(static_cast<float>(call_count));
            fv.append(static_cast<float>(spill_hint_count));
            return fv;
        }
    };

    // =============================================================================
    // runtime_features
    //
    // Accumulated execution-time observations.  Updated via record().
    // Welford online algorithm for mean/variance; zero allocation.
    // =============================================================================

    struct runtime_features {
        std::uint64_t call_count = 0;
        std::uint64_t min_latency_ns = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t max_latency_ns = 0;
        double mean_latency_ns = 0.0;
        double m2_latency = 0.0; // Welford M2 accumulator
        std::uint64_t memory_bytes_peak = 0;
        double throughput_ops_s = 0.0;
        double power_mw_mean = 0.0;

        void record(std::uint64_t latency_ns) noexcept {
            ++call_count;
            if (latency_ns < min_latency_ns) min_latency_ns = latency_ns;
            if (latency_ns > max_latency_ns) max_latency_ns = latency_ns;
            const double delta = static_cast<double>(latency_ns) - mean_latency_ns;
            mean_latency_ns += delta / static_cast<double>(call_count);
            m2_latency += delta * (static_cast<double>(latency_ns) - mean_latency_ns);
        }

        [[nodiscard]] double sample_variance() const noexcept {
            return call_count < 2 ? 0.0 : m2_latency / static_cast<double>(call_count - 1);
        }

        [[nodiscard]] feature_vector to_feature_vector() const noexcept {
            feature_vector fv;
            fv.append(static_cast<float>(call_count));
            fv.append(min_latency_ns == std::numeric_limits<std::uint64_t>::max()
                          ? 0.0f
                          : static_cast<float>(min_latency_ns));
            fv.append(static_cast<float>(max_latency_ns));
            fv.append(static_cast<float>(mean_latency_ns));
            fv.append(static_cast<float>(sample_variance()));
            fv.append(static_cast<float>(memory_bytes_peak));
            fv.append(static_cast<float>(throughput_ops_s));
            fv.append(static_cast<float>(power_mw_mean));
            return fv;
        }
    };

    // =============================================================================
    // detail: visitor helpers for expression traversal
    // =============================================================================

    namespace detail {
        // op_counter: visitor that counts op occurrences and builds expression_features.
        // Used by graph/expression extractors via lithe::evaluate.
        struct op_counter {
            expression_features ef;

            // on_node: called for each internal expression node.
            template <class Tag, class... Children>
            std::size_t on_node(Tag, Children... child_sizes) noexcept {
                const std::size_t op = emit::tag_descriptor<Tag>::stable_id;
                const std::size_t arity = sizeof...(Children);

                if (op == emit::tag_descriptor<div_tag>::stable_id) ++ef.div_count;
                else if (op == emit::tag_descriptor<mul_tag>::stable_id) ++ef.mul_count;
                else if (op == emit::tag_descriptor<add_tag>::stable_id) ++ef.add_count;
                else if (op == emit::tag_descriptor<for_tag>::stable_id ||
                    op == emit::tag_descriptor<while_tag>::stable_id)
                    ++ef.loop_nesting;

                if (arity == 0) ++ef.arity_histogram[0];
                else if (arity == 1) ++ef.arity_histogram[1];
                else if (arity == 2) ++ef.arity_histogram[2];
                else if (arity == 3) ++ef.arity_histogram[3];
                else ++ef.arity_histogram[4];

                ++ef.tree_size;
                std::size_t max_child = 0;
                ((max_child = child_sizes > max_child ? child_sizes : max_child), ...);
                return max_child + 1;
            }

            // on_terminal: called for each leaf (arithmetic value, symbolic var, etc.)
            template <class T>
            std::size_t on_terminal(const T&) noexcept {
                ++ef.constant_count;
                ++ef.arity_histogram[0];
                ++ef.tree_size;
                return 1;
            }
        };
    } // namespace detail

    // =============================================================================
    // graph_feature_extractor
    //
    // Extracts graph_features from any lithe Expression using tree::* utilities.
    // Satisfies feature_extractor<graph_feature_extractor, E> for any Expression E.
    // =============================================================================

    struct graph_feature_extractor {
        template <Expression E>
        [[nodiscard]] graph_features extract_graph(const E& expr) const noexcept {
            graph_features gf;
            gf.node_count = tree::size(expr);
            gf.depth = tree::depth(expr);
            gf.leaf_count = tree::leaf_nodes(expr);
            gf.internal_count = tree::internal_nodes(expr);
            // edges = nodes - 1 (tree); for DAGs this is approximate
            gf.edge_count = gf.node_count > 0 ? gf.node_count - 1 : 0;

            // op_frequencies: use evaluate to count ops bottom-up
            detail::op_counter counter;
            lithe::evaluate(expr, counter);
            const float total = static_cast<float>(gf.internal_count > 0 ? gf.internal_count : 1);
            gf.op_frequencies[0] = static_cast<float>(counter.ef.add_count) / total;
            gf.op_frequencies[2] = static_cast<float>(counter.ef.mul_count) / total;
            gf.op_frequencies[3] = static_cast<float>(counter.ef.div_count) / total;

            return gf;
        }

        template <Expression E>
        [[nodiscard]] feature_vector extract(const E& expr) const noexcept {
            return extract_graph(expr).to_feature_vector();
        }
    };

    // =============================================================================
    // expression_feature_extractor
    //
    // Extracts expression_features from any lithe Expression.
    // Satisfies feature_extractor<expression_feature_extractor, E> for any Expression E.
    // =============================================================================

    struct expression_feature_extractor {
        template <Expression E>
        [[nodiscard]] expression_features extract_features(const E& expr) const noexcept {
            detail::op_counter counter;
            lithe::evaluate(expr, counter);
            auto& ef = counter.ef;
            ef.tree_depth = tree::depth(expr);
            return ef;
        }

        template <Expression E>
        [[nodiscard]] feature_vector extract(const E& expr) const noexcept {
            return extract_features(expr).to_feature_vector();
        }
    };

    // =============================================================================
    // mir_feature_extractor
    //
    // Extracts mir_features from a physical MIR function.
    // Templated on the function type so this header does not hard-depend on
    // lithe_codegen.hpp.  Include lithe_codegen.hpp before instantiating.
    //
    // op_classifier: optional callable(op) → uint8_t bucket
    //   bucket 0 = memory, 1 = branch, 2 = call, 3 = other
    // When not supplied, a default predicate for lithe::codegen::opcode is used
    // (requires lithe_codegen.hpp to be visible at the instantiation site).
    // =============================================================================

    namespace detail {
        // Default classifier for lithe::codegen::opcode — used inside a template so
        // the header never hard-includes lithe_codegen.hpp at parse time.
        struct default_opcode_classifier {
            template <class Opcode>
            [[nodiscard]] std::uint8_t operator()(Opcode op) const noexcept {
                // We spell out the numeric values corresponding to the codegen opcode enum:
                //   load=5, store=6, store_spill=7, load_spill=8  → memory (0)
                //   branch=34, branch_cond=35                     → branch (1)
                //   call=33                                       → call (2)
                const auto v = static_cast<std::uint8_t>(
                    static_cast<std::underlying_type_t<Opcode>>(op));
                if (v == 5 || v == 6 || v == 7 || v == 8) return 0; // memory
                if (v == 34 || v == 35) return 1; // branch
                if (v == 33) return 2; // call
                return 3; // other
            }
        };
    } // namespace detail

    struct mir_feature_extractor {
        template <class PhysicalMirFn,
                  class Classifier = detail::default_opcode_classifier>
        [[nodiscard]] mir_features
        extract_mir(const PhysicalMirFn& fn,
                    Classifier classify = {}) const noexcept {
            mir_features mf;
            mf.block_count = fn.function.blocks.size();
            mf.spill_hint_count = fn.function.spill_slots.size();
            // next_vreg_id - 1 approximates the number of allocated virtual registers
            mf.vreg_count = fn.function.original_vreg_ir.next_vreg_id > 0
                                ? fn.function.original_vreg_ir.next_vreg_id - 1
                                : 0;

            for (const auto& blk : fn.function.blocks) {
                mf.instruction_count += blk.instructions.size();
                for (const auto& instr : blk.instructions) {
                    const std::uint8_t bucket = classify(instr.op);
                    if (bucket == 0) ++mf.memory_op_count;
                    else if (bucket == 1) ++mf.branch_count;
                    else if (bucket == 2) ++mf.call_count;
                }
            }
            mf.critical_path_len = mf.block_count;
            return mf;
        }

        template <class PhysicalMirFn>
        [[nodiscard]] feature_vector extract(const PhysicalMirFn& fn) const noexcept {
            return extract_mir(fn).to_feature_vector();
        }
    };

    // =============================================================================
    // runtime_feature_extractor
    //
    // Accumulates runtime_features from successive record() calls.
    // Satisfies feature_extractor<runtime_feature_extractor, runtime_features>.
    // =============================================================================

    struct runtime_feature_extractor {
        runtime_features accumulated;

        void record(std::uint64_t latency_ns) noexcept {
            accumulated.record(latency_ns);
        }

        void record_memory(std::uint64_t bytes) noexcept {
            if (bytes > accumulated.memory_bytes_peak)
                accumulated.memory_bytes_peak = bytes;
        }

        void record_throughput(double ops_per_second) noexcept {
            accumulated.throughput_ops_s = ops_per_second;
        }

        void record_power(double milliwatts) noexcept {
            if (accumulated.call_count == 0) {
                accumulated.power_mw_mean = milliwatts;
                return;
            }
            // Running mean over call_count observations
            accumulated.power_mw_mean +=
                (milliwatts - accumulated.power_mw_mean)
                / static_cast<double>(accumulated.call_count);
        }

        [[nodiscard]] feature_vector extract(const runtime_features& rf) const noexcept {
            return rf.to_feature_vector();
        }

        [[nodiscard]] feature_vector current() const noexcept {
            return accumulated.to_feature_vector();
        }

        void reset() noexcept { accumulated = {}; }
    };

    static_assert(feature_extractor<runtime_feature_extractor, runtime_features>);

    // =============================================================================
    // combined_feature_extractor<A, B>
    //
    // Concatenates the feature vectors of two extractors over the same input.
    // Designed for learned cost models that need both static structure and
    // dynamic execution history in one dense vector.
    //
    // Usage:
    //   combined_feature_extractor<expression_feature_extractor,
    //                               runtime_feature_extractor> cfe;
    //   auto fv = cfe.extract(expr);   // expression dims + runtime dims
    // =============================================================================

    template <class A, class B>
    struct combined_feature_extractor {
        A first;
        B second;

        template <class In>
            requires feature_extractor<A, In>
        [[nodiscard]] feature_vector extract(const In& input) const noexcept {
            feature_vector fv = first.extract(input);
            if constexpr (requires { second.current(); }) {
                fv.append(second.current().as_span());
            }
            else {
                fv.append(second.extract(input).as_span());
            }
            return fv;
        }
    };
} // namespace lithe::features
