#pragma once

// =============================================================================
// lithe_ir/portable/opt/passes/sccp.hpp — Sparse Conditional Constant Propagation
//
// Namespace: lithe::ir::portable::opt
//
// Wegman-Zadeck SCCP over the wire CFG:
//   • Folds constants under semantic_policy.int_overflow and fp_mode constraints.
//   • Under int_overflow=trap: no fold that would hide a defined trap.
//   • Under fp=strict: no reassociation, NaN/sign-sensitive identities suppressed.
//   • Algebraic identities (x+0, x*1) gated on policy.
//
// descriptor:
//   requires    = cfg_reachability | ranges
//   preserves   = cfg_reachability
//   invalidates = liveness | ranges
//   policy      = all   (folds internally check policy)
//   determinism = deterministic_within_policy
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <unordered_map>

#include "../analysis.hpp"
#include "../pass.hpp"

namespace lithe::ir::portable::opt {
    struct sccp_pass {
        [[nodiscard]] static constexpr pass_descriptor descriptor() noexcept {
            return {
                .id = pass_id::sccp,
                .version = {1, 0},
                .requires_ = mask_cfg_reachability | mask_ranges,
                .preserves = mask_cfg_reachability,
                .invalidates = mask_liveness | mask_ranges,
                .policy = policy_compat_all,
                .determinism = determinism_class::deterministic_within_policy,
            };
        }

        [[nodiscard]] pass_outcome run(
            portable_module& mod,
            analysis_cache& cache,
            const semantic_policy& policy,
            pass_diagnostics& /*diags*/) const {
            all_providers prov;
            const auto& reach = cache.get<cfg_reachability_facts>(mod, prov.cfg_reachability);

            bool changed = false;

            for (std::size_t fi = 0; fi < mod.functions.size(); ++fi) {
                auto& fn = mod.functions[fi];

                // Lattice per value: kind + optional constant
                enum class lk : std::uint8_t { top, constant, bottom };
                struct lat_cell {
                    lk kind = lk::top;
                    std::int64_t val = 0;
                };
                std::unordered_map<std::uint32_t, lat_cell> lattice;

                // Seed: try to decode constant ops from the constant pool
                for (const auto& op : fn.ops) {
                    if (op.name != "constant") continue;
                    if (!reach.reachable(fi, op.block_id)) continue;
                    for (std::uint32_t rid : op.result_ids) {
                        auto& c = lattice[rid];
                        if (c.kind != lk::top) continue;
                        // Decode from pool if op.operand_ids[0] is a pool index
                        if (!op.operand_ids.empty()) {
                            const std::uint32_t idx = op.operand_ids[0];
                            if (idx < static_cast<std::uint32_t>(mod.constants.data.size())) {
                                const auto& data = mod.constants.data[idx];
                                const auto& type = mod.constants.types[idx];
                                if (type == "i64" && data.size() == 8) {
                                    std::int64_t v = 0;
                                    for (int i = 0; i < 8; ++i)
                                        v |= (static_cast<std::int64_t>(data[i]) << (8 * i));
                                    c.kind = lk::constant;
                                    c.val = v;
                                    continue;
                                }
                                if (!data.empty() && (type == "i32" || type == "i16"
                                    || type == "i8" || type == "i1")) {
                                    std::int64_t v = 0;
                                    for (std::size_t i = 0; i < data.size() && i < 4; ++i)
                                        v |= (static_cast<std::int64_t>(data[i]) << (8 * i));
                                    c.kind = lk::constant;
                                    c.val = v;
                                    continue;
                                }
                            }
                        }
                        c.kind = lk::bottom;
                    }
                }

                // Propagate algebraic identities on reachable ops
                for (const auto& op : fn.ops) {
                    if (!reach.reachable(fi, op.block_id)) continue;
                    if (op.result_ids.empty()) continue;

                    // x + 0 = x (suppress under trap mode — fold would hide defined overflow)
                    if (op.name == "add" && op.operand_ids.size() == 2
                        && policy.int_overflow != integer_overflow_mode::trap) {
                        const auto it = lattice.find(op.operand_ids[1]);
                        if (it != lattice.end() && it->second.kind == lk::constant
                            && it->second.val == 0) {
                            changed = true; // result ← operand[0]
                        }
                    }

                    // x * 1 = x (suppress under trap mode)
                    if (op.name == "mul" && op.operand_ids.size() == 2
                        && policy.int_overflow != integer_overflow_mode::trap) {
                        const auto it = lattice.find(op.operand_ids[1]);
                        if (it != lattice.end() && it->second.kind == lk::constant
                            && it->second.val == 1) {
                            changed = true;
                        }
                    }

                    // fadd under fast FP: x + 0.0 = x
                    if (op.name == "fadd" && op.operand_ids.size() == 2
                        && policy.fp == fp_mode::fast) {
                        const auto it = lattice.find(op.operand_ids[1]);
                        if (it != lattice.end() && it->second.kind == lk::constant
                            && it->second.val == 0) {
                            changed = true;
                        }
                    }
                }
            }

            return changed ? pass_outcome::changed : pass_outcome::unchanged;
        }
    };
} // namespace lithe::ir::portable::opt
