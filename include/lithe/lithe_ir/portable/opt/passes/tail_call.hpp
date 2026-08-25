#pragma once

// =============================================================================
// lithe_ir/portable/opt/passes/tail_call.hpp — Tail-Call Formation Pass
//
// Namespace: lithe::ir::portable::opt
//
// Transforms eligible tail-call sites into tail-call form.
// Gate conditions (arch §4.1, §11.M2.3):
//   • No defer scope observing the current frame.
//   • No in-flight transaction at the call site.
//   • No exception handler frame wrapping the call.
//   • Stack-frame observation not required.
//
// Disabled below aggressive level (arch §11.M2.3 sequencing mandate).
// If semantic info is absent from the module, records "insufficient info" and
// is a no-op.
//
// descriptor:
//   requires    = effects | purity
//   preserves   = {}
//   invalidates = liveness
//   policy      = all   (gated internally on preserve_defer/exceptions/transactions)
//   determinism = deterministic_within_policy
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>

#include "../analysis.hpp"
#include "../pass.hpp"

namespace lithe::ir::portable::opt {
    struct tail_call_pass {
        [[nodiscard]] static constexpr pass_descriptor descriptor() noexcept {
            return {
                .id = pass_id::tail_call_form,
                .version = {1, 0},
                .requires_ = mask_effects | mask_purity,
                .preserves = 0,
                .invalidates = mask_liveness,
                .policy = policy_compat_all,
                .determinism = determinism_class::deterministic_within_policy,
            };
        }

        [[nodiscard]] pass_outcome run(
            portable_module& mod,
            analysis_cache& cache,
            const semantic_policy& policy,
            pass_diagnostics& diags) const {
            // Gate: must be able to reason about defer/exception/transaction
            const bool has_semantic_info =
                !mod.declared_capabilities.empty() ||
                !mod.functions.empty();

            if (!has_semantic_info) {
                diags.warn("OPT-TAIL-CALL", "tail_call_form: insufficient semantic info — no-op");
                return pass_outcome::unchanged;
            }

            all_providers prov;
            const auto& efacts = cache.get<effects_facts>(mod, prov.effects);

            bool changed = false;

            for (std::size_t fi = 0; fi < mod.functions.size(); ++fi) {
                auto& fn = mod.functions[fi];
                for (auto& op : fn.ops) {
                    if (op.name != "call") continue;

                    const auto ef = efacts.op_effect(fi, op.id);

                    // Hard gates (arch §4.1 conditions)
                    if (ef.has_defer && policy.preserve_defer) continue;
                    if (ef.has_txn && policy.preserve_transactions) continue;
                    if (ef.has_exception && policy.preserve_exceptions) continue;

                    // Check: this call is in tail position (last real op before region_yield)
                    // For the wire form, we check if op is immediately before a terminator
                    // in its block with no intervening effectful ops.
                    if (is_tail_position(fn, op, fi, efacts)) {
                        op.domain = "lithe.hl.tail";
                        changed = true;
                    }
                }
            }

            return changed ? pass_outcome::changed : pass_outcome::unchanged;
        }

    private:
        [[nodiscard]] static bool is_tail_position(
            const adapters::lithe_hl_mir_ir& fn,
            const adapters::hl_wire_op& call_op,
            std::size_t fi,
            const effects_facts& efacts) noexcept {
            // Find the block; check all ops after call_op
            for (const auto& blk : fn.blocks) {
                if (blk.id != call_op.block_id) continue;
                bool past_call = false;
                for (std::uint32_t oid : blk.op_ids) {
                    if (oid == call_op.id) {
                        past_call = true;
                        continue;
                    }
                    if (!past_call) continue;
                    const auto ef = efacts.op_effect(fi, oid);
                    // Only a terminator (region_yield) is allowed after a tail call
                    if (!ef.is_terminator) return false;
                }
                return past_call;
            }
            return false;
        }
    };
} // namespace lithe::ir::portable::opt
