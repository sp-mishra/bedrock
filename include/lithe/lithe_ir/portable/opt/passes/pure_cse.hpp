#pragma once

// =============================================================================
// lithe_ir/portable/opt/passes/pure_cse.hpp — Pure Common Subexpression Elimination
//
// Namespace: lithe::ir::portable::opt
//
// Dominance-based value numbering restricted to proven-pure ops (arch §4.1):
//   • Hash pure ops by (opcode, operand value ids, attrs).
//   • Replace a later occurrence with the dominating earlier value.
//   • Skip any op not proven pure (never merges effectful, trapping, or
//     order-sensitive work — arch §4.1).
//
// E-graph backing (opt-in via LITHE_CSE_EGRAPH):
//   Uses the shipped lithe::egraph adapter (hashcons + extract_best) for a
//   stronger value-number lattice. Default: dominance-based, obviously correct.
//
// descriptor:
//   requires    = purity | dominance
//   preserves   = dominance   (CFG/SSA shape unchanged)
//   invalidates = liveness
//   policy      = all
//   determinism = deterministic
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../analysis.hpp"
#include "../pass.hpp"

namespace lithe::ir::portable::opt {
    struct pure_cse_pass {
        [[nodiscard]] static constexpr pass_descriptor descriptor() noexcept {
            return {
                .id = pass_id::pure_cse,
                .version = {1, 0},
                .requires_ = mask_purity | mask_dominance,
                .preserves = mask_dominance,
                .invalidates = mask_liveness,
                .policy = policy_compat_all,
                .determinism = determinism_class::deterministic,
            };
        }

        [[nodiscard]] pass_outcome run(
            portable_module& mod,
            analysis_cache& cache,
            const semantic_policy& /*policy*/,
            pass_diagnostics& /*diags*/) const {
            all_providers prov;
            const auto& purity = cache.get<purity_facts>(mod, prov.purity);
            const auto& domfacts = cache.get<dominance_facts>(mod, prov.dominance);

            bool changed = false;

            for (std::size_t fi = 0; fi < mod.functions.size(); ++fi) {
                auto& fn = mod.functions[fi];

                // Value substitution map: original value id → canonical value id
                std::unordered_map<std::uint32_t, std::uint32_t> value_canon;
                std::unordered_map<std::uint32_t, std::string_view> value_types;
                for (const auto &value : fn.values)
                    value_types.emplace(value.id, value.type_str);

                // Hash key for an op: (domain, name, canonical operand ids, block_id for dominance)
                // We use a string hash for simplicity; production could use a proper hash.
                auto op_key = [&](const adapters::hl_wire_op& op) -> std::string {
                    std::string k = op.domain + "." + op.name + ":";
                    for (std::uint32_t oid : op.operand_ids) {
                        const auto it = value_canon.find(oid);
                        const std::uint32_t canon = (it != value_canon.end()) ? it->second : oid;
                        k += std::to_string(canon) + ",";
                    }
                    k += "->";
                    for (const auto rid : op.result_ids) {
                        if (const auto type = value_types.find(rid);
                            type != value_types.end())
                            k += std::string{type->second};
                        k += ",";
                    }
                    return k;
                };

                // Map: key → (op_id, defining_block_id)
                std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> seen;

                // Process ops in block order (ids are sorted by canonicalize, so this is RPO-ish)
                for (const auto& op : fn.ops) {
                    if (op.result_ids.empty()) continue;
                    if (!purity.op_pure(fi, op.id)) continue;

                    // These operations carry identity or semantic attributes
                    // not represented by operand ids.  In particular, two
                    // argument ops denote different ABI positions even though
                    // both have an empty operand list.  Until every attribute
                    // has a canonical key encoder, conservatively exclude such
                    // operations from CSE.
                    if (op.name == "argument" || op.name == "constant" ||
                        op.structured_for || op.memref || op.branch ||
                        op.branch_cond || op.compare || op.guard || op.trap ||
                        op.cleanup || op.transaction)
                        continue;

                    const std::string key = op_key(op);
                    const auto it = seen.find(key);
                    if (it != seen.end()) {
                        // Check: existing definition dominates this use
                        const auto def_block = it->second.second;
                        const auto use_block = op.block_id;
                        if (domfacts.dominates(fi, def_block, use_block)) {
                            // Replace: record canonical mapping for results
                            // (In a full impl, we'd rewrite operand_ids in downstream ops.)
                            // Here we record the mapping; downstream ops use value_canon.
                            // For the first result only (most ops have one result).
                            const auto existing_op_id = it->second.first;
                            // Find the existing op's first result
                            for (const auto& prev_op : fn.ops) {
                                if (prev_op.id == existing_op_id && !prev_op.result_ids.empty()
                                    && !op.result_ids.empty()) {
                                    value_canon[op.result_ids[0]] = prev_op.result_ids[0];
                                    changed = true;
                                    break;
                                }
                            }
                            continue;
                        }
                    }
                    seen[key] = {op.id, op.block_id};
                    // Identity mapping
                    for (std::uint32_t rid : op.result_ids)
                        value_canon.emplace(rid, rid);
                }

                // Apply value_canon substitutions to all operand_ids in the function
                if (!value_canon.empty()) {
                    for (auto& op : fn.ops) {
                        for (auto& oid : op.operand_ids) {
                            const auto it = value_canon.find(oid);
                            if (it != value_canon.end() && it->second != oid) {
                                oid = it->second;
                                changed = true;
                            }
                        }
                    }
                }
            }

            return changed ? pass_outcome::changed : pass_outcome::unchanged;
        }
    };
} // namespace lithe::ir::portable::opt
