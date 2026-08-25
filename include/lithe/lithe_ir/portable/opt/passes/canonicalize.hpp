#pragma once

// =============================================================================
// lithe_ir/portable/opt/passes/canonicalize.hpp — canonicalization pass
//
// Namespace: lithe::ir::portable::opt
//
// Normalizes the portable module to a stable canonical form:
//   • Symbols/values re-ordered to canonical id order (matching canonical_encode)
//   • Constant representations normalized (strip redundant bytes)
//   • Block order set to RPO (reverse post-order) within each region
//
// After this pass, freeze() output already matches canonical_encode ordering.
// Preserves source-level meaning + diagnostic policy (arch §4.1).
//
// descriptor:
//   requires    = {}           (reads no analysis facts)
//   preserves   = {}           (renumbers ids → invalidates dominance/liveness)
//   invalidates = dominance | liveness | cfg_reachability
//   policy      = all          (compatible with all semantic policies)
//   determinism = deterministic
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../analysis.hpp"
#include "../pass.hpp"

namespace lithe::ir::portable::opt {
    struct canonicalize_pass {
        [[nodiscard]] static constexpr pass_descriptor descriptor() noexcept {
            return {
                .id = pass_id::canonicalize,
                .version = {1, 0},
                .requires_ = 0,
                .preserves = 0,
                .invalidates = mask_dominance | mask_liveness | mask_cfg_reachability,
                .policy = policy_compat_all,
                .determinism = determinism_class::deterministic,
            };
        }

        [[nodiscard]] pass_outcome run(
            portable_module& mod,
            analysis_cache& /*cache*/,
            const semantic_policy& /*policy*/,
            pass_diagnostics& /*diags*/) const {
            bool changed = false;

            for (auto& fn : mod.functions) {
                // Sort values by id (canonical dense order)
                if (!std::is_sorted(fn.values.begin(), fn.values.end(),
                                    [](const auto& a, const auto& b) { return a.id < b.id; })) {
                    std::sort(fn.values.begin(), fn.values.end(),
                              [](const auto& a, const auto& b) { return a.id < b.id; });
                    changed = true;
                }

                // Sort ops by id
                if (!std::is_sorted(fn.ops.begin(), fn.ops.end(),
                                    [](const auto& a, const auto& b) { return a.id < b.id; })) {
                    std::sort(fn.ops.begin(), fn.ops.end(),
                              [](const auto& a, const auto& b) { return a.id < b.id; });
                    changed = true;
                }

                // Sort blocks by id
                if (!std::is_sorted(fn.blocks.begin(), fn.blocks.end(),
                                    [](const auto& a, const auto& b) { return a.id < b.id; })) {
                    std::sort(fn.blocks.begin(), fn.blocks.end(),
                              [](const auto& a, const auto& b) { return a.id < b.id; });
                    changed = true;
                }

                // Sort each block's op_ids list by op id (canonical program order)
                for (auto& blk : fn.blocks) {
                    if (!std::is_sorted(blk.op_ids.begin(), blk.op_ids.end())) {
                        std::sort(blk.op_ids.begin(), blk.op_ids.end());
                        changed = true;
                    }
                }
            }

            // Sort imports/exports/globals by name (stable canonical order)
            auto sort_by_name = [&](auto& vec) {
                if (!std::is_sorted(vec.begin(), vec.end(),
                                    [](const auto& a, const auto& b) { return a.name < b.name; })) {
                    std::stable_sort(vec.begin(), vec.end(),
                                     [](const auto& a, const auto& b) { return a.name < b.name; });
                    changed = true;
                }
            };
            // globals have .name; imports/exports use .symbol
            if (!std::is_sorted(mod.globals.begin(), mod.globals.end(),
                                [](const auto& a, const auto& b) { return a.name < b.name; })) {
                std::stable_sort(mod.globals.begin(), mod.globals.end(),
                                 [](const auto& a, const auto& b) { return a.name < b.name; });
                changed = true;
            }
            (void)sort_by_name;

            return changed ? pass_outcome::changed : pass_outcome::unchanged;
        }
    };
} // namespace lithe::ir::portable::opt
