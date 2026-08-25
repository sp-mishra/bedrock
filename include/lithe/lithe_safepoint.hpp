#pragma once
#include "lithe_codegen_pipeline.hpp"  // cfg_analysis_result, edge_kind, mir_pass_context

// =============================================================================
// Phase 5: Safepoint & Stack-Map Injection
// Namespace: lithe::safepoint
//
// Traverses the PDG/CFG for async_fork edges and yield abstract operations,
// intersects live-out vreg sets at each asynchronous boundary, and emits a
// stack_map_artifact attached to the physical_mir_function.  The artifact
// tells an external GC or coroutine runtime exactly which virtual registers
// hold live pointers at every potential context-switch point.
//
//  • safepoint_injection_pass — analysis + annotation pass
//  • inject_safepoints()      — convenience free function
// =============================================================================

namespace lithe::safepoint {
    // -------------------------------------------------------------------------
    // 1. Result type
    // -------------------------------------------------------------------------

    struct safepoint_result {
        codegen::mir::physical_mir_function function;
        std::size_t safepoints_injected = 0;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // -------------------------------------------------------------------------
    // 2. Pass implementation
    //
    // Strategy:
    //   a) Run liveness on the original virtual MIR (function.original_vreg_ir).
    //   b) Walk typed CFG edges; for each async_fork edge record block_id of the
    //      source block and the ID of the terminator instruction.  The live set
    //      at that point is live_out of the source block intersected with
    //      live_in of the target block (i.e., the values that must survive the
    //      fork handoff).
    //   c) Walk every block's instruction list looking for abstract operations
    //      whose name is "yield" (domain-agnostic match).  For each, record the
    //      per-instruction live_out set computed by liveness.
    //   d) Collect results into stack_map_artifact and attach it to the function.
    // -------------------------------------------------------------------------

    struct safepoint_injection_pass {
        [[nodiscard]] safepoint_result run(
            const codegen::mir::physical_mir_function& fn,
            const codegen::cfg_analysis_result& cfg) const {
            safepoint_result result;
            result.function = fn;

            // Liveness is computed over the virtual-register IR preserved inside
            // allocated_function_ir.  If the original IR has no blocks we have
            // nothing to annotate.
            const codegen::function_ir& vir = fn.function.original_vreg_ir;
            if (vir.blocks.empty()) {
                result.function.stack_map = codegen::mir::stack_map_artifact{};
                return result;
            }

            // Run standard backward liveness analysis.
            const codegen::liveness_analysis liveness = codegen::analyze_liveness(vir);

            // Build block-id → block reference map for the virtual IR so we can
            // look up per-instruction liveness cheaply.
            std::unordered_map<std::uint32_t, const codegen::basic_block*> vir_block_map;
            vir_block_map.reserve(vir.blocks.size());
            for (const auto& b : vir.blocks)
                vir_block_map.emplace(b.id, &b);

            codegen::mir::stack_map_artifact artifact;

            // ----------------------------------------------------------------
            // (a) async_fork edges → safepoint at the terminator of the source
            //     block.  Live set = live_out(source) ∩ live_in(target).
            // ----------------------------------------------------------------
            for (const auto& edge : cfg.typed_edges) {
                if (edge.kind != codegen::edge_kind::async_fork) continue;

                const auto src_it = liveness.per_block.find(edge.from);
                const auto dst_it = liveness.per_block.find(edge.to);
                if (src_it == liveness.per_block.end() ||
                    dst_it == liveness.per_block.end())
                    continue;

                // Intersect live_out(src) with live_in(dst) using flat vectors.
                const auto& src_out = src_it->second.live_out;
                const auto& dst_in = dst_it->second.live_in;

                std::vector<std::uint32_t> live_at_fork;
                live_at_fork.reserve(std::min(src_out.size(), dst_in.size()));
                for (const auto reg : src_out) {
                    if (dst_in.contains(reg))
                        live_at_fork.push_back(reg);
                }
                std::ranges::sort(live_at_fork);

                // Find the terminator instruction ID for the source block.
                std::uint32_t term_id = 0;
                const auto blk_it = vir_block_map.find(edge.from);
                if (blk_it != vir_block_map.end() &&
                    !blk_it->second->instructions.empty())
                    term_id = blk_it->second->instructions.back().id;

                codegen::mir::stack_map_entry entry;
                entry.block_id = edge.from;
                entry.instruction_id = term_id;
                entry.live_vregs = std::move(live_at_fork);
                artifact.entries.push_back(std::move(entry));
                ++result.safepoints_injected;
            }

            // ----------------------------------------------------------------
            // (b) yield abstract operations → safepoint at that instruction.
            //     Live set = live_out at that instruction position.
            // ----------------------------------------------------------------
            for (const auto& block : vir.blocks) {
                const auto inst_liveness_it = liveness.per_instruction.find(block.id);
                if (inst_liveness_it == liveness.per_instruction.end()) continue;
                const auto& inst_live = inst_liveness_it->second;

                for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
                    const auto& instr = block.instructions[idx];
                    if (!instr.abstract_operation.has_value()) continue;
                    if (instr.abstract_operation->name != "yield") continue;

                    // Per-instruction liveness is indexed parallel to instructions.
                    if (idx >= inst_live.size()) continue;
                    const auto& live_out = inst_live[idx].live_out;

                    std::vector<std::uint32_t> live_at_yield;
                    live_at_yield.reserve(live_out.size());
                    for (const auto reg : live_out)
                        live_at_yield.push_back(reg);
                    std::ranges::sort(live_at_yield);

                    codegen::mir::stack_map_entry entry;
                    entry.block_id = block.id;
                    entry.instruction_id = instr.id;
                    entry.live_vregs = std::move(live_at_yield);
                    artifact.entries.push_back(std::move(entry));
                    ++result.safepoints_injected;
                }
            }

            result.function.stack_map = std::move(artifact);
            return result;
        }
    };

    // -------------------------------------------------------------------------
    // 3. Convenience free function
    // -------------------------------------------------------------------------

    [[nodiscard]] inline safepoint_result inject_safepoints(
        const codegen::mir::physical_mir_function& fn,
        codegen::mir_pass_context& ctx) {
        const auto& cfg = codegen::get_or_compute_cfg(ctx, fn);
        return safepoint_injection_pass{}.run(fn, cfg);
    }
} // namespace lithe::safepoint

// Reopen lithe::codegen so the file ends in the same namespace it started in.
