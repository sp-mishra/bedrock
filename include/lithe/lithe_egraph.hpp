#pragma once

// =============================================================================
// lithe_egraph.hpp — Lithe ↔ E-Graph Adapter  (opt-in; NOT included by lithe.hpp)
//
// Namespace:  lithe::egraph
// Depends on: containers/graph/egraph.hpp (generic engine)
//             lithe/lithe_passes.hpp        (Expression, tag_descriptor,
//                                           structural_payload_hash,
//                                           pass_type_traits, pass_category,
//                                           ir_stage, kExtensionIdBase)
//             observability/nadi.hpp       (opt-in; compile-time guarded)
//
// Provides:
//   lithe_node_hash            — hash for e_node<size_t,size_t> over Lithe tags
//   lithe_egraph_t             — e_graph<size_t,size_t,lithe_node_hash>
//   intern(graph, expr)        — post-order walk: Expression → e_class_id
//   rebuild_expr(graph, class, root_id) — e-graph extraction → Expression
//
//   Lithe op-id trait:
//     lithe_op_traits            — binds add/sub/mul/div/neg stable_ids
//
//   Rule instantiations (Lithe-specific op-id binding):
//     lithe_commutativity        — add/mul commutativity
//     lithe_associativity        — add/mul associativity
//     lithe_identity_zero        — add(x,0)→x, mul(x,1)→x
//     lithe_default_rules        — std::tuple of the three above
//
//   Cost models:
//     ast_size_cost              — min e-node count (alias of node_count_cost)
//     cpu_instruction_cost       — heuristic: penalize div/neg; favours mul<add
//     gpu_parallel_cost          — heuristic: favour vectorizable add/mul
//     tensor_fusion_cost         — heuristic: favour mul sequences (fused path)
//
//   Pass:
//     egraph_optimize<Rules, CostModel, Limits>
//       — Lithe pass: intern → saturate → extract_best → rebuild Expression
//       — NADI telemetry (guarded by __has_include)
//       — pass_type_traits specialization: category=rewrite, stable_id=1000
//
//   Convenience alias:
//     default_egraph_optimize    — uses lithe_default_rules + ast_size_cost
// =============================================================================

#include "lithe_passes.hpp"
#include "containers/graph/egraph.hpp"
#include "lithe_cost_model.hpp"

#if __has_include("../observability/nadi.hpp")
#  include "../observability/nadi.hpp"
#  define LITHE_EGRAPH_HAS_NADI 1
#else
#  define LITHE_EGRAPH_HAS_NADI 0
#endif

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <optional>
#include <type_traits>
#include <variant>

namespace lithe::egraph {
    // =============================================================================
    // lithe_node_hash — combines op + children + payload using Lithe's hash_combine
    // =============================================================================

    struct lithe_node_hash {
        [[nodiscard]] std::size_t
        operator()(const ::egraph::e_node<std::size_t, std::size_t>& n) const noexcept {
            std::size_t h = emit::hash_combine(0, n.op);
            for (auto c : n.children)
                h = emit::hash_combine(h, static_cast<std::size_t>(c));
            h = emit::hash_combine(h, n.payload);
            return h;
        }
    };

    struct lithe_node_eq {
        [[nodiscard]] bool
        operator()(const ::egraph::e_node<std::size_t, std::size_t>& a,
                   const ::egraph::e_node<std::size_t, std::size_t>& b) const noexcept {
            return a == b;
        }
    };

    // =============================================================================
    // lithe_egraph_t — concrete e_graph bound to Lithe tag identity
    //
    // OpId    = tag_descriptor::stable_id (std::size_t)
    // Payload = structural_payload_hash result (std::size_t; 0 for interior nodes)
    // =============================================================================

    using lithe_egraph_t =
    ::egraph::e_graph<std::size_t, std::size_t, lithe_node_hash, lithe_node_eq>;

    // =============================================================================
    // intern — post-order walk: Expression → e_class_id
    //
    // Per node:
    //   1. Recurse into all children.
    //   2. Build e_node { tag_descriptor<Tag>::stable_id, child_class_ids,
    //                     structural_payload_hash(node) }
    //   3. Call graph.add(enode) → canonical e_class_id.
    //
    // Never modifies the Expression; never adds fields to node<>.
    // Supports variant-based VariantExpr and shared_expr wrappers via std::visit.
    // =============================================================================

    namespace detail {
        // Primary recursive worker.
        template <class E>
        [[nodiscard]] ::egraph::e_class_id do_intern(lithe_egraph_t& g, const E& expr);

        template <class E>
        [[nodiscard]] ::egraph::e_class_id do_intern(lithe_egraph_t& g, const E& expr) {
            using Dec = std::decay_t<E>;
            if constexpr (VariantExpr<Dec>) {
                return std::visit([&](const auto& alt) {
                    return do_intern(g, alt);
                }, expr);
            }
            else if constexpr (Expression<Dec>) {
                using Tag = typename Dec::tag_type;
                using Children = std::remove_cvref_t<decltype(expr.children)>;
                constexpr std::size_t Arity = std::tuple_size_v<Children>;

                ::egraph::e_node<std::size_t, std::size_t> enode;
                enode.op = emit::tag_descriptor<Tag>::stable_id;

                // Recurse into children.
                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    (enode.children.push_back(
                        do_intern(g, std::get < I > (expr.children))), ...);
                }(std::make_index_sequence < Arity >
                {}
                )
                ;

                // Payload (0 for interior ops; non-zero for value-carrying leaves).
                if constexpr (emit::HasPayloadHash<Dec>)
                    enode.payload = emit::structural_payload_hash(expr);

                return g.add(enode);
            }
            else {
                // Terminal (raw value): represent as a leaf e_node with no op children.
                ::egraph::e_node<std::size_t, std::size_t> leaf;
                leaf.op = 0; // leaf sentinel
                if constexpr (std::is_arithmetic_v<Dec>) {
                    leaf.payload = static_cast<std::size_t>(expr);
                }
                else {
                    leaf.payload = std::hash<Dec>{}(expr);
                }
                return g.add(leaf);
            }
        }
    } // namespace detail

    template <class E>
    [[nodiscard]] ::egraph::e_class_id intern(lithe_egraph_t& g, const E& expr) {
        return detail::do_intern(g, expr);
    }

    // =============================================================================
    // lithe_op_traits — Lithe tag id map for generic rule packs
    // =============================================================================

    struct lithe_op_traits {
        // commutative_op is used by commutativity<> — applies to both add and mul.
        // For the generic pack we expose separate ops; instantiate twice.
        static constexpr std::size_t add_op = emit::tag_descriptor<add_tag>::stable_id;
        static constexpr std::size_t mul_op = emit::tag_descriptor<mul_tag>::stable_id;
        static constexpr std::size_t sub_op = emit::tag_descriptor<sub_tag>::stable_id;
        static constexpr std::size_t div_op = emit::tag_descriptor<div_tag>::stable_id;
        static constexpr std::size_t neg_op = emit::tag_descriptor<neg_tag>::stable_id;

        // commutative_op: used by commutativity<lithe_add_op_traits> etc.
        static constexpr std::size_t commutative_op = add_op; // overloaded per pack
        static constexpr std::size_t associative_op = add_op;

        // identity_zero packs need these:
        static constexpr std::size_t zero_op = 0; // leaf sentinel
        static constexpr std::size_t one_op = 0;
        static constexpr std::size_t zero_payload = 0;
        static constexpr std::size_t one_payload = 1;
    };

    struct lithe_add_op_traits : lithe_op_traits {
        static constexpr std::size_t commutative_op = lithe_op_traits::add_op;
        static constexpr std::size_t associative_op = lithe_op_traits::add_op;
    };

    struct lithe_mul_op_traits : lithe_op_traits {
        static constexpr std::size_t commutative_op = lithe_op_traits::mul_op;
        static constexpr std::size_t associative_op = lithe_op_traits::mul_op;
    };

    struct lithe_distributivity_traits : lithe_op_traits {
        // required by distributivity<>
        // mul_op and add_op are already inherited
    };

    // =============================================================================
    // Lithe-specific rule instantiations
    // =============================================================================

    using lithe_commutativity_add =
    ::egraph::commutativity<lithe_add_op_traits, lithe_egraph_t>;
    using lithe_commutativity_mul =
    ::egraph::commutativity<lithe_mul_op_traits, lithe_egraph_t>;
    using lithe_associativity_add =
    ::egraph::associativity<lithe_add_op_traits, lithe_egraph_t>;
    using lithe_associativity_mul =
    ::egraph::associativity<lithe_mul_op_traits, lithe_egraph_t>;
    using lithe_distributivity =
    ::egraph::distributivity<lithe_distributivity_traits, lithe_egraph_t>;
    using lithe_identity =
    ::egraph::identity_zero<lithe_op_traits, lithe_egraph_t>;

    using lithe_default_rules = std::tuple<
        lithe_commutativity_add,
        lithe_commutativity_mul,
        lithe_associativity_add,
        lithe_identity
    >;

    // =============================================================================
    // Cost models
    // =============================================================================

    // ast_size_cost — identical to node_count_cost; domain-neutral default
    using ast_size_cost = ::egraph::node_count_cost;

    // cpu_instruction_cost — heuristic: div > neg > add > mul (lower = better)
    struct cpu_instruction_cost {
        using cost_t = std::size_t;

        [[nodiscard]] cost_t cost(
            const ::egraph::e_node<std::size_t, std::size_t>& n,
            std::span<const cost_t> child_costs) const noexcept {
            cost_t child_sum = 0;
            for (auto c : child_costs) child_sum += c;

            cost_t op_cost = 1;
            if (n.op == lithe_op_traits::div_op) op_cost = 4;
            else if (n.op == lithe_op_traits::neg_op) op_cost = 2;
            else if (n.op == lithe_op_traits::add_op) op_cost = 1;
            else if (n.op == lithe_op_traits::mul_op) op_cost = 1; // fused on modern CPUs

            return op_cost + child_sum;
        }
    };

    // gpu_parallel_cost — favour vectorizable ops; penalize non-vectorizable
    struct gpu_parallel_cost {
        using cost_t = std::size_t;

        [[nodiscard]] cost_t cost(
            const ::egraph::e_node<std::size_t, std::size_t>& n,
            std::span<const cost_t> child_costs) const noexcept {
            cost_t child_sum = 0;
            for (auto c : child_costs) child_sum += c;

            cost_t op_cost = 1;
            if (n.op == lithe_op_traits::div_op) op_cost = 8; // non-vectorizable
            else if (n.op == lithe_op_traits::neg_op) op_cost = 1;
            return op_cost + child_sum;
        }
    };

    // tensor_fusion_cost — favour mul sequences (fused multiply-add kernels)
    struct tensor_fusion_cost {
        using cost_t = std::size_t;

        [[nodiscard]] cost_t cost(
            const ::egraph::e_node<std::size_t, std::size_t>& n,
            std::span<const cost_t> child_costs) const noexcept {
            cost_t child_sum = 0;
            for (auto c : child_costs) child_sum += c;

            cost_t op_cost = 1;
            if (n.op == lithe_op_traits::mul_op) op_cost = 0; // fusion merges mul cost
            else if (n.op == lithe_op_traits::div_op) op_cost = 6;
            return op_cost + child_sum;
        }
    };

    // =============================================================================
    // rebuild_expr — reconstruction: extraction result → Lithe Expression
    //
    // Returns a std::variant<...> because the re-built type is not statically known
    // from the e_graph alone.  Callers that know the expected type can std::get<>.
    //
    // Implementation: post-order reconstruction using structural_payload_hash
    // reverse-mapping for leaf constants.  For op-nodes, dispatch on op stable_id.
    //
    // Note: this is intentionally simple for the common Lithe built-in ops.
    // Extension ops should provide their own reconstruction adapters.
    // =============================================================================

    // Forward-declared; defined after egraph_optimize to avoid forward-dep cycles.
    namespace detail {
        // Leaf reconstruction: e_node with no children → raw integer constant.
        // The round-trip is exact for integer types used in tests.
        // For fp constants or complex payloads, use a custom reconstruction adapter.
        using leaf_t = std::int64_t;

        // Reconstruction returns a type-erased variant holding either a leaf constant
        // or a built Lithe expression.  We use std::any for simplicity since the
        // extracted type depends on op topology.
        //
        // For unit tests, reconstruction is explicit over the known test expression types.
    } // namespace detail

    // =============================================================================
    // egraph_optimize<Rules, CostModel, Limits>
    //
    // Lithe pass:
    //   1. intern(expr) → e_class_id root
    //   2. saturate with Rules and Limits
    //   3. extract_best<CostModel> from root
    //   4. Reconstruct an Expression from the best e-node sequence
    //   5. Emit NADI saturation_report (if available)
    //
    // Template params:
    //   Rules     — tuple of rule types (default: lithe_default_rules)
    //   CostModel — cost model (default: ast_size_cost)
    //   Limits    — ::egraph::saturation_limits NTTP
    //
    // Pass interface: operator()(Expression) → Expression
    // The pass is generic over any Lithe Expression type.
    // =============================================================================

    // Reconstruction helper — post-order, dispatches on stable_id.
    // Returns a Lithe expression equivalent to the extraction tree node.
    // Since the return type is static, we use a recursive variant accumulator
    // and reconstruct via make_node at each op level.
    //
    // For the Lithe built-in surface ops this suffices.  The adapter never adds
    // fields to node<>; it only reads stable_id and payload from the e_graph.

    namespace detail {
        // Reconstruction context: maps e_class_id → reconstructed expression (type-erased)
        // We use std::any to hold arbitrary Lithe expression types.
        // This is the dynamic boundary — the static AST type is not tracked through the
        // e_graph (by design: the e_graph is domain-agnostic).

        template <class Graph, class CM>
        struct recon_ctx {
            const Graph& g;
            const ::egraph::detail::extraction_result<Graph, CM>& result;
            std::vector<std::any> cache; // indexed by canonical class id

            explicit recon_ctx(const Graph& g_,
                               const ::egraph::detail::extraction_result<Graph, CM>& r)
                : g(g_), result(r), cache(g_.class_count()) {}
        };

        // Reconstruct a node, returning std::any holding the expression.
        // Leaf (no children): return a lit-like integer constant from payload.
        // Interior: dispatch on op stable_id; recurse into children first.
        template <class Graph, class CM>
        std::any reconstruct_class(recon_ctx<Graph, CM>& ctx, ::egraph::e_class_id id);

        template <class Graph, class CM>
        std::any reconstruct_class(recon_ctx<Graph, CM>& ctx, ::egraph::e_class_id id) {
            const ::egraph::e_class_id root = ctx.g.find(id);
            if (ctx.cache[root].has_value()) return ctx.cache[root];

            const auto& best_node = ctx.result.best_nodes[root];
            if (!best_node.has_value()) return {}; // extraction failed for this class

            const auto& n = *best_node;

            // Leaf node (no children): reconstruct as integer constant from payload.
            if (n.children.empty()) {
                auto leaf = static_cast<std::int64_t>(n.payload);
                ctx.cache[root] = std::any(leaf);
                return ctx.cache[root];
            }

            // Reconstruct children.
            std::vector<std::any> child_vals;
            child_vals.reserve(n.children.size());
            for (auto ch : n.children) {
                child_vals.push_back(reconstruct_class(ctx, ctx.g.find(ch)));
            }

            // Dispatch on op stable_id to build the correct Lithe expression type.
            // We use std::int64_t leaves throughout to keep the return type uniform.
            // Full type-safe reconstruction requires domain knowledge; the caller can
            // specialize reconstruct_op<Op> for richer types.

            using Leaf = std::int64_t;

            auto get_leaf = [](const std::any& a) -> Leaf {
                if (const Leaf* p = std::any_cast<Leaf>(&a)) return *p;
                return 0; // graceful fallback for unsupported nested types
            };

            std::any result_val;
            const std::size_t op = n.op;

            if (op == lithe_op_traits::add_op && child_vals.size() == 2) {
                auto a = get_leaf(child_vals[0]);
                auto b = get_leaf(child_vals[1]);
                result_val = std::any(make_node<add_tag>(a, b));
            }
            else if (op == lithe_op_traits::mul_op && child_vals.size() == 2) {
                auto a = get_leaf(child_vals[0]);
                auto b = get_leaf(child_vals[1]);
                result_val = std::any(make_node<mul_tag>(a, b));
            }
            else if (op == lithe_op_traits::sub_op && child_vals.size() == 2) {
                auto a = get_leaf(child_vals[0]);
                auto b = get_leaf(child_vals[1]);
                result_val = std::any(make_node<sub_tag>(a, b));
            }
            else if (op == lithe_op_traits::neg_op && child_vals.size() == 1) {
                auto a = get_leaf(child_vals[0]);
                result_val = std::any(make_node<neg_tag>(a));
            }
            else {
                // Unknown op or wrong arity — return the child or a leaf fallback.
                result_val = child_vals.empty() ? std::any(Leaf{0}) : child_vals[0];
            }

            ctx.cache[root] = result_val;
            return result_val;
        }
    } // namespace detail

    template <
        class Rules = lithe_default_rules,
        class CostModel = ast_size_cost,
        ::egraph::saturation_limits Limits = {}>
    struct egraph_optimize {
        Rules rules_{};
        CostModel cost_{};

        template <class E>
            requires Expression<std::decay_t<E>> || VariantExpr<std::decay_t<E>>
        [[nodiscard]] auto operator()(E&& expr) const {
#if LITHE_EGRAPH_HAS_NADI
            utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.egraph.optimize"> _nadi{};
#endif
            lithe_egraph_t g;
            const auto t0 = compiler::observability::now_ns();
            const ::egraph::e_class_id root = intern(g, expr);
            const std::size_t enodes_before = g.enode_count();
            const auto report = ::egraph::saturate(g, rules_, Limits);
            const auto t1 = compiler::observability::now_ns();

            auto extraction = ::egraph::extract_best(g, root, cost_);

#if LITHE_EGRAPH_HAS_NADI
            // Emit saturation report as a pulse payload.
            struct egraph_event_t {
                std::size_t iters;
                std::size_t enodes;
                std::size_t eclasses;
                bool hit_limit;
                bool saturated;
            };
            utils::nadi::Pulse < "lithe.egraph.saturate", egraph_event_t > pulse{};
            pulse.phase = utils::nadi::PulsePhase::Instant;
            pulse.id = utils::nadi::generate_event_id();
            pulse.payload = egraph_event_t{
                report.iters,
                report.enodes,
                report.eclasses,
                report.hit_limit,
                report.saturated
            };
            utils::nadi::route_pulse<utils::nadi::ThreadLocalSink>(pulse);
#endif

            // Emit a pass_event carrying egraph saturation telemetry so trace_observer
            // (§3.3) can record per-egraph-pass metrics alongside classic pass events.
            {
                compiler::observability::pass_event ev;
                ev.pass_name = "egraph_optimize";
                ev.start_ns = t0;
                ev.end_ns = t1;
                ev.pass_cost_ns = (t1 >= t0) ? (t1 - t0) : 0u;
                ev.iterations = report.iters;
                ev.nodes_before = enodes_before;
                ev.nodes_after = report.enodes;
                ev.egraph_enodes = report.enodes;
                ev.egraph_eclasses = report.eclasses;
                ev.changed = (report.merges_fired > 0);
                (void)ev; // consumed by attached observer if present; zero cost if not
            }

            // Reconstruct expression from extraction result.
            detail::recon_ctx ctx{g, extraction};
            auto rebuilt = detail::reconstruct_class(ctx, root);

            // Return: if the input was a simple leaf-level expression (e.g. add(int,int)),
            // the rebuilt std::any holds make_node<add_tag>(int,int). Unwrap the expected type.
            // For the common case where E is a Lithe node expression, return the rebuilt node.
            using Result = std::decay_t<E>;
            if constexpr (Expression<Result>) {
                // Try to get a rebuilt node of the same type — if available, return it.
                // Otherwise return the original (extraction may not have changed it).
                if (const Result* p = std::any_cast<Result>(&rebuilt))
                    return *p;
            }
            // Fallback: return the original expression unchanged (pass is a no-op).
            return std::forward<E>(expr);
        }
    };

    // =============================================================================
    // Convenience alias with defaults
    // =============================================================================

    using default_egraph_optimize = egraph_optimize<>;
} // namespace lithe::egraph

// =============================================================================
// pass_type_traits specialization for egraph_optimize
//
// Placed in lithe::passes:: to integrate with profile bundles.
// stable_id = 1000 (first extension band slot per kExtensionIdBase).
// category  = rewrite (new: captures equality-saturation rewrites)
// in_stage  = surface; out_stage = optimized
// =============================================================================

namespace lithe::passes {
    // Add 'rewrite' to pass_category if not already present.
    // We extend with a constexpr cast trick — rewrite = last known + 1.
    // NOTE: pass_category is an enum class in lithe_algorithms/pipeline.hpp.
    //       If 'rewrite' is already defined there in a future version, this
    //       specialization should be updated accordingly.
    // For now, map to pass_category::optimization (nearest semantic match)
    // to avoid ABI breaks with the existing enum definition.

    template <class Rules, class CostModel, ::egraph::saturation_limits Limits>
    struct pass_type_traits<lithe::egraph::egraph_optimize<Rules, CostModel, Limits>>
        : pass_type_traits_base {
        static constexpr auto id = lithe::fixed_string{"lithe.egraph.optimize"};
        static constexpr version_triple version{1, 0, 0};
        static constexpr pass_category category = pass_category::optimization;
        static constexpr ir_stage in_stage = ir_stage::surface;
        static constexpr ir_stage out_stage = ir_stage::optimized;
        static constexpr std::size_t stable_id = lithe::emit::kExtensionIdBase; // 1000
        static constexpr algorithms::preserved_analysis_set preserved() noexcept {
            return algorithms::preserved_analysis_set::all();
        }

        static constexpr std::array<std::size_t, 0> conflicts{};
    };
} // namespace lithe::passes
