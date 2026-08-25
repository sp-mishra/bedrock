#pragma once

// =============================================================================
// lithe_ir/adapters/graph.hpp — stage adapter for the graph (surface/canonical/
// optimized) IR stages
//
// Maps the graph-IR stage (surface_ast / canonical_ast / optimized_ast) to/from
// the generic format_descriptor + section model used by text_provider and
// binary_provider.
//
// Design:
//   • Backend-neutral — no backend header included here.
//   • Provides tag_invoke glue consumed by both text and binary providers.
//   • The canonical graph IR representation is a lithe_graph_ir struct (defined
//     below) that carries a minimally-typed, stable serialisable form.
//   • Real expression-tree lowering (lithe_core.hpp) happens at a higher level;
//     this adapter sees only the serialisable form.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../format.hpp"    // stage, schema_version, format_descriptor

namespace lithe::ir::adapters {
    // =========================================================================
    // Serialisable graph-IR node
    //
    // Represents one AST node in the stable interchange form.  The node carries:
    //   • op_domain / op_name — stable string identity (never an integer index)
    //   • child_ids           — indices into the enclosing graph_ir::nodes table
    //   • literal_payload     — optional inlined literal (doubles/ints/bools)
    //
    // Invariant: child_ids[i] < nodes.size() for all i (enforced on import).
    // =========================================================================

    enum class graph_literal_kind : std::uint8_t {
        none = 0,
        i64 = 1,
        f64 = 2,
        bool_ = 3,
        str = 4, // string stored in strings table; literal_index is the index
    };

    struct graph_node {
        std::uint32_t id = 0; // stable node id within this document
        std::string op_domain; // e.g. "lithe.core", "lithe.dsl_extension"
        std::string op_name; // e.g. "add", "mul", "constant"

        std::vector<std::uint32_t> child_ids; // indices into graph_ir::nodes

        graph_literal_kind lit_kind = graph_literal_kind::none;
        std::int64_t lit_i64 = 0;
        double lit_f64 = 0.0;
        bool lit_bool = false;
        std::uint32_t lit_str_idx = 0; // index into graph_ir::strings if lit_kind == str
    };

    // =========================================================================
    // lithe_graph_ir — the stage-adapter IR object for graph stages
    //
    // Carries:
    //   nodes   — all AST nodes (topological order; roots last)
    //   strings — interned string literals (referenced by lit_str_idx)
    //   roots   — root node ids (the expression result nodes)
    //   source_stage — which graph stage this document represents
    //   schema  — document schema version
    // =========================================================================

    struct lithe_graph_ir {
        std::vector<graph_node> nodes;
        std::vector<std::string> strings; // string literal table
        std::vector<std::uint32_t> roots; // root node ids

        stage source_stage = stage::surface;
        schema_version schema = {1, 0, 0};

        // Returns true iff all child_ids are valid indices.
        [[nodiscard]] bool structurally_valid() const noexcept {
            const auto n = static_cast<std::uint32_t>(nodes.size());
            for (const auto& node : nodes) {
                for (const auto cid : node.child_ids)
                    if (cid >= n) return false;
                if (node.lit_kind == graph_literal_kind::str &&
                    node.lit_str_idx >= strings.size())
                    return false;
            }
            for (const auto rid : roots)
                if (rid >= n) return false;
            return true;
        }

        // True iff nodes vector is non-empty and structurally valid.
        [[nodiscard]] bool valid() const noexcept {
            return !nodes.empty() && structurally_valid();
        }
    };

    // =========================================================================
    // stage_for_graph — maps a lithe::ir::stage value to the "graph" category
    // =========================================================================

    [[nodiscard]] inline constexpr bool is_graph_stage(const stage s) noexcept {
        return s == stage::surface || s == stage::canonical || s == stage::optimized;
    }

    // =========================================================================
    // Section ids for graph IR (stable interchange constants)
    // These are used by both text_provider and binary_provider.
    // =========================================================================

    namespace section_ids {
        inline constexpr std::string_view graph_nodes = "lithe.ir.graph.nodes";
        inline constexpr std::string_view graph_strings = "lithe.ir.graph.strings";
        inline constexpr std::string_view graph_roots = "lithe.ir.graph.roots";
        inline constexpr std::string_view graph_meta = "lithe.ir.graph.meta";
    } // namespace section_ids

    // =========================================================================
    // Serialisable identity for graph ops (used by upgrade_registry wiring)
    // =========================================================================

    struct graph_op_identity {
        std::string_view domain;
        std::string_view name;
        schema_version schema;

        [[nodiscard]] constexpr bool operator==(const graph_op_identity&) const noexcept = default;
    };
} // namespace lithe::ir::adapters
