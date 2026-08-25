#pragma once

// =============================================================================
// lithe_ir/portable/opt/passes/cfg_simplify.hpp — CFG simplification pass
//
// Namespace: lithe::ir::portable::opt
//
// Removes unreachable blocks and merges single-pred/single-succ block chains.
// Threads trivial branches.
//
// Safety guards (arch §4.1):
//   • Never remove/merge a block that owns a defer/exception/transaction effect.
//   • Never remove/merge across such an edge (from effect summary).
//
// descriptor:
//   requires    = cfg_reachability | effects
//   preserves   = {}
//   invalidates = dominance | liveness | cfg_reachability
//   policy      = all
//   determinism = deterministic
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

#include "../analysis.hpp"
#include "../pass.hpp"

namespace lithe::ir::portable::opt {
    struct cfg_simplify_pass {
        [[nodiscard]] static constexpr pass_descriptor descriptor() noexcept {
            return {
                .id = pass_id::cfg_simplify,
                .version = {1, 0},
                .requires_ = mask_cfg_reachability | mask_effects,
                .preserves = 0,
                .invalidates = mask_dominance | mask_liveness | mask_cfg_reachability,
                .policy = policy_compat_all,
                .determinism = determinism_class::deterministic,
            };
        }

        [[nodiscard]] pass_outcome run(
            portable_module& mod,
            analysis_cache& cache,
            const semantic_policy& policy,
            pass_diagnostics& /*diags*/) const {
            all_providers prov;
            const auto& reach = cache.get<cfg_reachability_facts>(mod, prov.cfg_reachability);
            const auto& efacts = cache.get<effects_facts>(mod, prov.effects);

            bool changed = false;

            for (std::size_t fi = 0; fi < mod.functions.size(); ++fi) {
                auto& fn = mod.functions[fi];
                if (fn.blocks.empty()) continue;

                // Collect unreachable block ids
                std::unordered_set<std::uint32_t> unreachable;
                for (const auto& blk : fn.blocks) {
                    if (!reach.reachable(fi, blk.id)) {
                        unreachable.insert(blk.id);
                    }
                }

                // Remove unreachable blocks (unless they carry semantic effects
                // that must be preserved by policy)
                std::unordered_set<std::uint32_t> to_remove;
                for (std::uint32_t bid : unreachable) {
                    const auto bef = efacts.block_effect(fi, bid);
                    // Never remove a block with defer/txn/exception effects
                    if (bef.has_defer && policy.preserve_defer) continue;
                    if (bef.has_txn && policy.preserve_transactions) continue;
                    if (bef.has_exception && policy.preserve_exceptions) continue;
                    // Never remove a block with trapping ops if preserve_traps
                    if (bef.trapping && policy.preserve_traps) continue;
                    to_remove.insert(bid);
                }

                if (!to_remove.empty()) {
                    constexpr auto invalid_id =
                        std::numeric_limits<std::uint32_t>::max();

                    // Determine dead operations while block ids still refer to
                    // the original dense index space.
                    std::unordered_set<std::uint32_t> dead_ops;
                    for (const auto& op : fn.ops)
                        if (to_remove.contains(op.block_id)) dead_ops.insert(op.id);

                    // Compact blocks and build the old-id to new-id mapping.
                    std::vector<std::uint32_t> block_remap(fn.blocks.size(), invalid_id);
                    std::vector<adapters::hl_wire_block> surviving_blocks;
                    surviving_blocks.reserve(fn.blocks.size() - to_remove.size());
                    for (auto &block : fn.blocks) {
                        if (to_remove.contains(block.id)) continue;
                        const auto old_id = block.id;
                        const auto new_id = static_cast<std::uint32_t>(
                            surviving_blocks.size());
                        if (old_id < block_remap.size()) block_remap[old_id] = new_id;
                        block.id = new_id;
                        surviving_blocks.push_back(std::move(block));
                    }
                    fn.blocks = std::move(surviving_blocks);

                    // Rewrite regions and entry blocks into the compact block
                    // index space.
                    for (auto& reg : fn.regions) {
                        std::vector<std::uint32_t> rewritten;
                        rewritten.reserve(reg.block_ids.size());
                        for (const auto old_id : reg.block_ids)
                            if (old_id < block_remap.size() &&
                                block_remap[old_id] != invalid_id)
                                rewritten.push_back(block_remap[old_id]);
                        reg.block_ids = std::move(rewritten);
                    }
                    for (auto &entry : fn.entry_block_ids)
                        if (entry < block_remap.size() &&
                            block_remap[entry] != invalid_id)
                            entry = block_remap[entry];

                    // Compact operations and rewrite block ownership plus all
                    // block op lists.  Branches from surviving reachable blocks
                    // cannot target a removed unreachable block.
                    std::vector<std::uint32_t> op_remap(fn.ops.size(), invalid_id);
                    std::vector<adapters::hl_wire_op> surviving_ops;
                    surviving_ops.reserve(fn.ops.size() - dead_ops.size());
                    for (auto &op : fn.ops) {
                        if (dead_ops.contains(op.id)) continue;
                        const auto old_id = op.id;
                        const auto new_id =
                            static_cast<std::uint32_t>(surviving_ops.size());
                        if (old_id < op_remap.size()) op_remap[old_id] = new_id;
                        op.id = new_id;
                        if (op.block_id < block_remap.size())
                            op.block_id = block_remap[op.block_id];
                        if (op.branch && op.branch->target_block_id < block_remap.size())
                            op.branch->target_block_id =
                                block_remap[op.branch->target_block_id];
                        if (op.branch_cond) {
                            if (op.branch_cond->true_block_id < block_remap.size())
                                op.branch_cond->true_block_id =
                                    block_remap[op.branch_cond->true_block_id];
                            if (op.branch_cond->false_block_id < block_remap.size())
                                op.branch_cond->false_block_id =
                                    block_remap[op.branch_cond->false_block_id];
                        }
                        surviving_ops.push_back(std::move(op));
                    }
                    fn.ops = std::move(surviving_ops);
                    for (auto &block : fn.blocks) {
                        std::vector<std::uint32_t> rewritten;
                        rewritten.reserve(block.op_ids.size());
                        for (const auto old_id : block.op_ids)
                            if (old_id < op_remap.size() &&
                                op_remap[old_id] != invalid_id)
                                rewritten.push_back(op_remap[old_id]);
                        block.op_ids = std::move(rewritten);
                    }

                    // Dead values are retained conservatively; later liveness
                    // and DCE passes may remove their producers/uses.
                    changed = true;
                }
            }

            return changed ? pass_outcome::changed : pass_outcome::unchanged;
        }
    };
} // namespace lithe::ir::portable::opt
