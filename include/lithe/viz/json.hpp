#pragma once

// lithe/viz/json.hpp — JSON output provider for graph_document (opt-in)
// Depends on: lithe/lithe_visualization.hpp
// Provides:   lithe::viz::to_json(const graph_document&) → std::string
// Note: uses hand-rolled JSON (no external dependency).

#include "../lithe_visualization.hpp"

#include <sstream>
#include <string>

namespace lithe::viz {
    // Escape a string for JSON.
    namespace detail::json {
        inline std::string escape(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 4);
            for (const char c : s) {
                switch (c) {
                case '"': out += "\\\"";
                    break;
                case '\\': out += "\\\\";
                    break;
                case '\n': out += "\\n";
                    break;
                case '\r': out += "\\r";
                    break;
                case '\t': out += "\\t";
                    break;
                default: out += c;
                    break;
                }
            }
            return out;
        }
    } // namespace detail::json

    inline std::string to_json(const graph_document& doc) {
        std::ostringstream ss;
        ss << "{\n  \"nodes\": [\n";

        bool first = true;
        for (const auto& [idx, node] : doc.graph.nodes()) {
            const auto& attr = node.data;
            if (!first) ss << ",\n";
            first = false;
            ss << "    {\"id\":" << idx
                << ",\"label\":\"" << detail::json::escape(attr.label) << '"'
                << ",\"category\":\"" << detail::json::escape(attr.category) << '"'
                << ",\"use_count\":" << attr.use_count
                << '}';
        }
        ss << "\n  ],\n  \"edges\": [\n";

        first = true;
        for (const auto& [idx, edge] : doc.graph.edges()) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    {\"from\":" << edge.from.value
                << ",\"to\":" << edge.to.value
                << ",\"kind\":\"" << detail::json::escape(edge.data.kind) << '"'
                << '}';
        }
        ss << "\n  ]\n}\n";
        return ss.str();
    }
} // namespace lithe::viz
