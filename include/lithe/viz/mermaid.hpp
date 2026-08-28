#pragma once

// lithe/viz/mermaid.hpp — Mermaid output provider for graph_document (opt-in)
// Depends on: lithe/lithe_visualization.hpp
// Provides:   lithe::viz::to_mermaid(const graph_document&) → std::string

#include "../lithe_visualization.hpp"

#include <sstream>
#include <string>

namespace lithe::viz {
    inline std::string to_mermaid(const graph_document& doc) {
        std::ostringstream ss;
        ss << "graph TD\n";

        // Nodes
        for (const auto& [idx, node] : doc.graph.nodes()) {
            const auto& attr = node.data;
            ss << "  n" << idx << "[\"" << attr.label;
            if (attr.use_count > 1) ss << " (x" << attr.use_count << ")";
            ss << "\"]\n";
        }

        // Edges
        for (const auto& [idx, edge] : doc.graph.edges()) {
            ss << "  n" << edge.from.value << " --> ";
            if (!edge.data.kind.empty())
                ss << "|" << edge.data.kind << "| ";
            ss << "n" << edge.to.value << "\n";
        }

        return ss.str();
    }
} // namespace lithe::viz
