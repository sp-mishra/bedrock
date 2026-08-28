#pragma once

// =============================================================================
// lithe_ir/portable/cfg_adapter.hpp — wire-CFG → LiteGraph directed graph
//
// Namespace: lithe::ir::portable
//
// Builds a transient litegraph::Graph (directed) from the wire CFG of a single
// lithe_hl_mir_ir function.  Nodes = canonical block ids (std::uint32_t);
// edges = fall-through / region-successor edges derived from block ordering.
//
// Usage:
//   auto [g, node_ids] = to_litegraph(wire_fn);
//   // Then feed g to litegraph::DominatorTree, Tarjan SCC, BFS, etc.
//
// node_ids[i] = NodeId assigned to wire block canonical id i.
//
// Pure container code — no codegen include.  Safe to include from lithe_ir_core.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <vector>

#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include "containers/graph/DominatorTree.hpp"
#include "../adapters/hl_mir.hpp"   // lithe_hl_mir_ir

namespace lithe::ir::portable {
    // =============================================================================
    // litegraph_cfg — directed Graph with block-id nodes (no edge payload needed)
    // =============================================================================

    using litegraph_cfg = litegraph::Graph<std::uint32_t, std::monostate, litegraph::Directed>;

    // =============================================================================
    // litegraph_cfg_result — graph + mapping from wire block index → NodeId
    // =============================================================================

    struct litegraph_cfg_result {
        litegraph_cfg graph;
        std::vector<litegraph::NodeId> node_ids; // node_ids[block_canonical_id] = NodeId
    };

    // =============================================================================
    // to_litegraph — build a directed CFG from a wire function
    //
    // Nodes: one per block (node data = canonical block id).
    // Edges: intra-region fall-through (block[k] → block[k+1] within a region).
    // =============================================================================

    [[nodiscard]] inline litegraph_cfg_result
    to_litegraph(const adapters::lithe_hl_mir_ir& wire) {
        litegraph_cfg g;

        const std::uint32_t n_blocks = static_cast<std::uint32_t>(wire.blocks.size());

        // Add all block nodes; record NodeId indexed by canonical block id
        std::vector<litegraph::NodeId> nids;
        nids.reserve(n_blocks);
        for (std::uint32_t i = 0; i < n_blocks; ++i)
            nids.push_back(g.add_node(i));

        // Intra-region fall-through edges
        for (const auto& wr : wire.regions) {
            const std::uint32_t nb = static_cast<std::uint32_t>(wr.block_ids.size());
            for (std::uint32_t k = 0; k + 1 < nb; ++k) {
                const std::uint32_t from_bid = wr.block_ids[k];
                const std::uint32_t to_bid = wr.block_ids[k + 1];
                if (from_bid < n_blocks && to_bid < n_blocks)
                    g.add_edge(nids[from_bid], nids[to_bid]);
            }
        }

        return {std::move(g), std::move(nids)};
    }

    // =============================================================================
    // entry_node — canonical id of the function entry block
    // =============================================================================

    [[nodiscard]] inline std::uint32_t
    entry_node(const adapters::lithe_hl_mir_ir& wire) noexcept {
        if (!wire.entry_block_ids.empty()) return wire.entry_block_ids[0];
        if (!wire.blocks.empty()) return wire.blocks[0].id;
        return 0;
    }
} // namespace lithe::ir::portable
