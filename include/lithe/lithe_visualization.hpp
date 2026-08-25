#pragma once

// =============================================================================
// lithe_visualization.hpp — Neutral graph document + IR adapters  (opt-in)
//
// Namespace:  lithe::viz
// NOT included by lithe.hpp — zero cost unless included.
//
// Depends on: lithe/lithe_core.hpp          (dag_view, Expression, build_dag)
//             lithe/lithe_passes.hpp         (compiler::observability::compile_trace)
//             lithe/lithe_pdg.hpp            (program_dependence_graph — opt-in)
//             lithe/lithe_codegen_pipeline.hpp (cfg_analysis_result — opt-in)
//             containers/graph/LiteGraph.hpp  (Graph<NodeT,EdgeT,Directed>)
//
// Provides:
//   graph_document   — neutral model backed by LiteGraph (Directed).
//                      Core (Lithe) emits data only; providers consume it.
//
//   Adapters (free functions → graph_document):
//     to_document(const dag_view<E>&)           — AST/DAG adapter
//     to_document(const Expression auto& e)     — direct expression adapter
//     to_document(const program_dependence_graph&) — PDG adapter (needs lithe_pdg.hpp)
//     to_document(const cfg_analysis_result&)   — CFG adapter (needs codegen_pipeline)
//     trace_to_document(const compile_trace&)   — pass-timeline
//
//   Provider headers (each opt-in, each a separate sub-header):
//     lithe/viz/graphviz.hpp  — to_dot(const graph_document&) → std::string
//     lithe/viz/mermaid.hpp   — to_mermaid(const graph_document&) → std::string
//     lithe/viz/json.hpp      — to_json(const graph_document&) → std::string
//
// Design:
//   • graph_document = thin wrapper over LiteGraph<node_attrs, edge_attrs, Directed>
//   • No virtual, no macros.  C++23.
// =============================================================================

#include "lithe_core.hpp"
#include "lithe_passes.hpp"

#if __has_include(<containers/graph/LiteGraph.hpp>)
#  include <containers/graph/LiteGraph.hpp>
#else
#  error "lithe_visualization.hpp requires containers/graph/LiteGraph.hpp"
#endif

#if __has_include("lithe_codegen_pipeline.hpp")
#  include "lithe_codegen_pipeline.hpp"
#  define LITHE_VIZ_HAS_CFG 1
#endif

#if __has_include("lithe_pdg.hpp")
#  include "lithe_pdg.hpp"
#  define LITHE_VIZ_HAS_PDG 1
#endif

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>

namespace lithe::viz {
    // =============================================================================
    // node_attrs / edge_attrs — must satisfy litegraph::Hashable
    // =============================================================================

    struct node_attrs {
        std::size_t doc_id = 0;
        std::string label;
        std::string category; // "ast" | "pdg" | "cfg" | "pass" | …
        std::size_t use_count = 0;

        bool operator==(const node_attrs& o) const noexcept {
            return doc_id == o.doc_id && label == o.label
                && category == o.category && use_count == o.use_count;
        }
    };

    struct edge_attrs {
        std::string kind; // "data" | "control" | "cfg_branch" | …
        std::size_t child_index = 0; // position among ordered children

        bool operator==(const edge_attrs& o) const noexcept {
            return kind == o.kind && child_index == o.child_index;
        }
    };
} // namespace lithe::viz

// Inject std::hash specializations before first use of litegraph::Graph<node_attrs,…>
namespace std {
    template <>
    struct hash<lithe::viz::node_attrs> {
        std::size_t operator()(const lithe::viz::node_attrs& a) const noexcept {
            std::size_t h = std::hash<std::size_t>{}(a.doc_id);
            h ^= std::hash<std::string>{}(a.label) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    template <>
    struct hash<lithe::viz::edge_attrs> {
        std::size_t operator()(const lithe::viz::edge_attrs& a) const noexcept {
            std::size_t h = std::hash<std::string>{}(a.kind);
            h ^= std::hash<std::size_t>{}(a.child_index) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };
} // namespace std

namespace lithe::viz {
    // =============================================================================
    // graph_document
    // =============================================================================

    using backing_graph_t = litegraph::Graph<node_attrs, edge_attrs, litegraph::Directed>;

    struct graph_document {
        backing_graph_t graph;

        litegraph::NodeId add_node(node_attrs attrs) {
            return graph.add_node(std::move(attrs));
        }

        litegraph::EdgeId add_edge(litegraph::NodeId from, litegraph::NodeId to,
                                   edge_attrs attrs = {}) {
            return graph.add_edge(from, to, std::move(attrs));
        }

        [[nodiscard]] std::size_t node_count() const { return graph.node_count(); }
        [[nodiscard]] std::size_t edge_count() const { return graph.edge_count(); }
    };

    // =============================================================================
    // Adapter: dag_view<E> → graph_document
    // =============================================================================

    namespace detail {
        template <class E>
        void dag_to_doc_impl(const graph::dag_view<E>& dag,
                             graph_document& doc,
                             std::unordered_map<graph::node_id, litegraph::NodeId>& id_map,
                             graph::node_id nid) {
            if (id_map.count(nid)) return;

            const auto* dn = dag.get_node(nid);
            if (!dn) return;

            std::string lbl{dn->operation_name()};
            if (lbl.empty()) lbl = "node_" + std::to_string(nid);

            const auto lg_id = doc.add_node(node_attrs{nid, std::move(lbl), "ast", dn->use_count});
            id_map[nid] = lg_id;

            std::size_t ci = 0;
            for (const auto child_nid : dn->children) {
                dag_to_doc_impl(dag, doc, id_map, child_nid);
                doc.add_edge(lg_id, id_map.at(child_nid), edge_attrs{"data", ci++});
            }
        }
    } // namespace detail

    template <class E>
    graph_document to_document(const graph::dag_view<E>& dag) {
        graph_document doc;
        std::unordered_map<graph::node_id, litegraph::NodeId> id_map;
        id_map.reserve(dag.node_count());
        detail::dag_to_doc_impl(dag, doc, id_map, dag.root);
        return doc;
    }

    // =============================================================================
    // Adapter: Expression → graph_document
    // =============================================================================

    template <class E>
        requires Expression<std::decay_t<E>>
    graph_document to_document(E&& expr) {
        // build_dag lives in lithe::graph namespace (lithe_core.hpp).
        auto shared = graph::build_dag(expr);
        return to_document(shared.dag);
    }

    // =============================================================================
    // Adapter: program_dependence_graph → graph_document  (needs lithe_pdg.hpp)
    // =============================================================================

#if defined(LITHE_VIZ_HAS_PDG)

    inline graph_document to_document(const pdg::program_dependence_graph& pdg_in) {
        graph_document doc;
        std::unordered_map<std::uint32_t, litegraph::NodeId> id_map;

        auto ensure_node = [&](std::uint32_t id) -> litegraph::NodeId {
            auto it = id_map.find(id);
            if (it != id_map.end()) return it->second;
            auto lg_id = doc.add_node(
                node_attrs{id, "n" + std::to_string(id), "pdg", 0});
            id_map[id] = lg_id;
            return lg_id;
        };

        for (const auto& node : pdg_in.nodes()) {
            const auto from_lg = ensure_node(node.instr_id);
            for (const auto& e : pdg_in.out_edges(node.instr_id)) {
                const auto to_lg = ensure_node(e.to_instr);
                std::string kind = e.is_data() ? "data" : "control";
                doc.add_edge(from_lg, to_lg, edge_attrs{std::move(kind)});
            }
        }
        return doc;
    }

#endif // LITHE_VIZ_HAS_PDG

    // =============================================================================
    // Adapter: cfg_analysis_result → graph_document  (needs lithe_codegen_pipeline.hpp)
    // =============================================================================

#if defined(LITHE_VIZ_HAS_CFG)

    inline graph_document to_document(const codegen::cfg_analysis_result& cfg) {
        graph_document doc;
        std::unordered_map<std::uint32_t, litegraph::NodeId> id_map;

        auto ensure_node = [&](std::uint32_t id) -> litegraph::NodeId {
            auto it = id_map.find(id);
            if (it != id_map.end()) return it->second;
            auto lg_id = doc.add_node(
                node_attrs{id, "bb" + std::to_string(id), "cfg", 0});
            id_map[id] = lg_id;
            return lg_id;
        };

        for (const auto& edge : cfg.typed_edges) {
            const auto from = ensure_node(edge.from);
            const auto to = ensure_node(edge.to);
            std::string kind;
            switch (edge.kind) {
            case codegen::edge_kind::sync_branch: kind = "sync_branch";
                break;
            case codegen::edge_kind::fallthrough: kind = "fallthrough";
                break;
            case codegen::edge_kind::async_fork: kind = "async_fork";
                break;
            case codegen::edge_kind::sync_join: kind = "sync_join";
                break;
            case codegen::edge_kind::rpc_boundary: kind = "rpc_boundary";
                break;
            case codegen::edge_kind::entanglement: kind = "entanglement";
                break;
            default: kind = "unknown";
                break;
            }
            doc.add_edge(from, to, edge_attrs{std::move(kind)});
        }
        return doc;
    }

#endif // LITHE_VIZ_HAS_CFG

    // =============================================================================
    // trace_to_document — pass-timeline from §3.3 compile_trace
    // =============================================================================

    inline graph_document trace_to_document(
        const compiler::observability::compile_trace& trace) {
        graph_document doc;
        litegraph::NodeId prev = litegraph::INVALID_NODE_ID;

        for (const auto& ev : trace.pass_events) {
            const auto cur = doc.add_node(
                node_attrs{ev.pass_index, ev.pass_name, "pass", 0});
            if (prev != litegraph::INVALID_NODE_ID)
                doc.add_edge(prev, cur, edge_attrs{"pass_seq"});
            prev = cur;
        }
        return doc;
    }
} // namespace lithe::viz
