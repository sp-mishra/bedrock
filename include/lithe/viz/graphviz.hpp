#pragma once

// lithe/viz/graphviz.hpp — DOT output provider for graph_document (opt-in)
// Depends on: lithe/lithe_visualization.hpp
// Provides:   lithe::viz::to_dot(const graph_document&) → std::string

#include "../lithe_visualization.hpp"

#include <sstream>
#include <string>

namespace lithe::viz {
    inline std::string to_dot(const graph_document& doc) {
        std::ostringstream ss;
        ss << "digraph G {\n";
        ss << "  rankdir=TB;\n";
        ss << "  node [shape=box, fontname=\"Helvetica\"];\n";

        // Nodes: nodes() yields pair<size_t index, const Node&>
        for (const auto& [idx, node] : doc.graph.nodes()) {
            const auto& attr = node.data;
            ss << "  n" << idx
                << " [label=\"" << attr.label << '"';
            if (!attr.category.empty())
                ss << ", tooltip=\"" << attr.category << '"';
            if (attr.use_count > 1)
                ss << ", style=filled, fillcolor=lightblue";
            ss << "];\n";
        }

        // Edges: edges() yields pair<size_t index, const Edge&>; Edge has .from .to .data
        for (const auto& [idx, edge] : doc.graph.edges()) {
            ss << "  n" << edge.from.value << " -> n" << edge.to.value;
            if (!edge.data.kind.empty())
                ss << " [label=\"" << edge.data.kind << "\"]";
            ss << ";\n";
        }

        ss << "}\n";
        return ss.str();
    }
} // namespace lithe::viz
