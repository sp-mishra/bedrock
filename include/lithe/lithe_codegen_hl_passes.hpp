#pragma once
#include "lithe_codegen_pipeline.hpp"  // cfg_analysis_result, hl types, poly types

#include <limits>

// ============================================================================
// HL MIR Passes — namespace lithe::codegen::hl
// ============================================================================
// All passes are structs with a run() method; no std::function, no virtuals.
// Compose via: apply_passes(fn, pass_a{}, pass_b{}, ...);
// ============================================================================

namespace lithe::codegen::hl {
    // -------------------------------------------------------------------------
    // Helpers: convert structured_for_attr::iv_bounds ↔ poly::loop_bounds
    // -------------------------------------------------------------------------

    [[nodiscard]] inline poly::loop_bounds to_poly_bounds(
        const structured_for_attr::iv_bounds& b, std::uint32_t preg_id = 0) noexcept {
        poly::loop_bounds lb;
        lb.induction_preg_id = preg_id;
        lb.lower = b.lower;
        lb.upper = b.upper;
        lb.step = b.step;
        lb.lower_known = b.lower_known;
        lb.upper_known = b.upper_known;
        lb.step_known = b.step_known;
        return lb;
    }

    // -------------------------------------------------------------------------
    // extract_polyhedral_from_hl — forward (top-down) polyhedral extraction
    //   Builds polyhedral_loop directly from structured_for attrs.
    //   Precision: exact (no recovery heuristics needed).
    //   Complement to lithe::poly::extract_polyhedral_pass (bottom-up fallback).
    // -------------------------------------------------------------------------

    struct hl_polyhedral_extraction_result {
        std::vector<poly::polyhedral_loop> loops;
        std::vector<std::string> diagnostics;
        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    struct extract_polyhedral_from_hl {
        [[nodiscard]] hl_polyhedral_extraction_result run(
            const hl_mir_function& fn) const {
            hl_polyhedral_extraction_result result;

            // Walk all operations in the body looking for structured_for.
            const auto visit_region = [&](auto& self, const hl_region& region) -> void {
                for (const hl_block* blk = region.blocks.head; blk; blk = blk->list_node.next) {
                    for (const hl_operation* op = blk->ops.head; op; op = op->list_node.next) {
                        if (op->op != hl_opcode::structured_for) continue;
                        if (!std::holds_alternative<structured_for_attr>(op->attr)) {
                            result.diagnostics.push_back(
                                "hl_poly: structured_for op#" + std::to_string(op->id) +
                                " missing structured_for_attr");
                            continue;
                        }
                        const auto& sf = std::get<structured_for_attr>(op->attr);

                        poly::polyhedral_loop pl;
                        // Use op->id as a synthetic loop id (no flat CFG block exists yet).
                        pl.base.header = op->id;
                        pl.is_affine = true;

                        const std::uint8_t rank = sf.rank;
                        pl.iteration = poly::affine_matrix::zero(2 * rank, rank + 1);
                        pl.schedule = poly::affine_matrix::identity(rank);

                        for (std::uint8_t d = 0; d < rank; ++d) {
                            const auto& b = sf.bounds[d];
                            if (!b.lower_known || !b.upper_known || !b.step_known) {
                                pl.is_affine = false;
                                pl.diagnostics.push_back(
                                    "dim " + std::to_string(d) + ": bounds not fully static");
                            }
                            // row 2d  : 0…1(col d)…0 | -lower  (v_d ≥ lower)
                            pl.iteration.at(2 * d, d) = 1;
                            pl.iteration.at(2 * d, rank) = -b.lower;
                            // row 2d+1: 0…-1(col d)…0 | upper-1 (v_d < upper)
                            pl.iteration.at(2 * d + 1, d) = -1;
                            pl.iteration.at(2 * d + 1, rank) = b.upper - 1;

                            poly::loop_induction_var iv;
                            iv.preg_id = static_cast<std::uint32_t>(d);
                            iv.def_instr_id = op->id;
                            iv.bounds = to_poly_bounds(b, static_cast<std::uint32_t>(d));
                            pl.ivars.push_back(iv);
                        }
                        result.loops.push_back(std::move(pl));

                        // Recurse into nested regions.
                        for (std::size_t ri = 0; ri < op->regions.size(); ++ri)
                            if (op->regions[ri]) self(self, *op->regions[ri]);
                    }
                }
            };

            visit_region(visit_region, fn.body_region);
            return result;
        }
    };

    // -------------------------------------------------------------------------
    // pre_header_isolation — split blocks so every structured_for is the sole op
    //   in its enclosing block before region fusion is attempted.
    //
    //   For each block in a region: if a block mixes non-structured_for ops with
    //   a structured_for op, split the block at each structured_for boundary so
    //   that structured_for ends up alone in a dedicated block.  Ops before the
    //   structured_for stay in the predecessor block; ops after move into a fresh
    //   successor block.  All new blocks are arena-allocated.
    //
    //   Returns the count of blocks created (0 = no split needed).
    // -------------------------------------------------------------------------

    struct pre_header_isolation_result {
        std::size_t blocks_created = 0;
        std::vector<std::string> diagnostics;
        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    struct pre_header_isolation {
        [[nodiscard]] pre_header_isolation_result run(hl_mir_function& fn) const {
            pre_header_isolation_result out;

            const auto process_region = [&](auto& self, hl_region& region) -> void {
                // Collect blocks first — we'll splice during iteration.
                // Walk a snapshot because we insert new blocks mid-traversal.
                std::vector<hl_block*> blk_snapshot;
                for (hl_block* b = region.blocks.head; b; b = b->list_node.next)
                    blk_snapshot.push_back(b);

                for (hl_block* blk : blk_snapshot) {
                    // Scan for structured_for ops that have neighbours.
                    hl_operation* op = blk->ops.head;
                    while (op) {
                        hl_operation* next_op = op->list_node.next;

                        if (op->op == hl_opcode::structured_for) {
                            // If there are ops before `op` in this block, split them off
                            // into the existing block; `op` starts a fresh successor.
                            if (op->list_node.prev != nullptr) {
                                // Allocate a new block for `op` and everything after it.
                                hl_block* succ = fn.make_block();
                                if (!succ) {
                                    out.diagnostics.push_back("pre_header_isolation: OOM");
                                    return;
                                }
                                succ->parent_region = &region;

                                // Move op..tail from blk into succ.
                                // Detach the chain [op..blk->ops.tail] from blk.
                                hl_operation* split_head = op;
                                hl_operation* split_tail = blk->ops.tail;

                                // Unlink split_head from its predecessor.
                                if (split_head->list_node.prev)
                                    split_head->list_node.prev->list_node.next = nullptr;
                                blk->ops.tail = split_head->list_node.prev;
                                split_head->list_node.prev = nullptr;

                                // Recount blk.
                                std::size_t count_blk = 0;
                                for (hl_operation* x = blk->ops.head; x; x = x->list_node.next)
                                    ++count_blk;
                                blk->ops.size_ = count_blk;

                                // Attach to succ.
                                succ->ops.head = split_head;
                                succ->ops.tail = split_tail;
                                std::size_t count_succ = 0;
                                for (hl_operation* x = succ->ops.head; x; x = x->list_node.next)
                                    ++count_succ;
                                succ->ops.size_ = count_succ;

                                // Insert succ after blk in the region's block list.
                                hl_block* after = blk->list_node.next;
                                succ->list_node.prev = blk;
                                succ->list_node.next = after;
                                blk->list_node.next = succ;
                                if (after) after->list_node.prev = succ;
                                else region.blocks.tail = succ;
                                ++region.blocks.size_;
                                ++out.blocks_created;

                                // Continue inside succ, not blk.
                                blk = succ;
                                op = split_head;
                                next_op = op->list_node.next;
                            }

                            // If there are ops after `op` in this block, split them into
                            // a fresh successor.
                            if (op->list_node.next != nullptr) {
                                hl_block* succ = fn.make_block();
                                if (!succ) {
                                    out.diagnostics.push_back("pre_header_isolation: OOM");
                                    return;
                                }
                                succ->parent_region = &region;

                                // Move [op->next .. blk->ops.tail] into succ.
                                hl_operation* split_head2 = op->list_node.next;
                                hl_operation* split_tail2 = blk->ops.tail;

                                op->list_node.next = nullptr;
                                split_head2->list_node.prev = nullptr;
                                blk->ops.tail = op;
                                blk->ops.size_ = static_cast<std::size_t>(
                                    blk->ops.size_) - (succ->ops.size_); // adjusted below

                                succ->ops.head = split_head2;
                                succ->ops.tail = split_tail2;
                                std::size_t count_succ2 = 0;
                                for (hl_operation* x = succ->ops.head; x; x = x->list_node.next)
                                    ++count_succ2;
                                succ->ops.size_ = count_succ2;

                                std::size_t count_blk2 = 0;
                                for (hl_operation* x = blk->ops.head; x; x = x->list_node.next)
                                    ++count_blk2;
                                blk->ops.size_ = count_blk2;

                                // Insert succ after blk.
                                hl_block* after2 = blk->list_node.next;
                                succ->list_node.prev = blk;
                                succ->list_node.next = after2;
                                blk->list_node.next = succ;
                                if (after2) after2->list_node.prev = succ;
                                else region.blocks.tail = succ;
                                ++region.blocks.size_;
                                ++out.blocks_created;

                                // `op` is now alone in blk; advance to succ.
                                blk = succ;
                                op = succ->ops.head;
                                next_op = op ? op->list_node.next : nullptr;
                                continue;
                            }
                        }

                        // Recurse into nested regions.
                        for (std::size_t ri = 0; ri < op->regions.size(); ++ri)
                            if (op->regions[ri]) self(self, *op->regions[ri]);

                        op = next_op;
                    }
                }
            };

            process_region(process_region, fn.body_region);
            return out;
        }
    };

    // -------------------------------------------------------------------------
    // region_fusion_pass — fuse two adjacent structured_for ops with equal bounds
    //   O(1) splice via intrusive_list::splice_back.
    //   On failed legality: no mutation (arena_checkpoint_guard rolls back).
    // -------------------------------------------------------------------------

    struct region_fusion_result {
        bool fused = false;
        std::string diagnostic;
    };

    struct region_fusion_pass {
        // Fuse `second` into `first` inside `blk`.
        // Precondition: both ops are in blk->ops, second immediately follows first.
        [[nodiscard]] region_fusion_result run(
            hl_mir_function& fn,
            hl_block& blk,
            hl_operation* first,
            hl_operation* second) const {
            region_fusion_result out;

            if (!first || !second) {
                out.diagnostic = "fusion: null operand";
                return out;
            }
            if (first->op != hl_opcode::structured_for ||
                second->op != hl_opcode::structured_for) {
                out.diagnostic = "fusion: both ops must be structured_for";
                return out;
            }
            if (!std::holds_alternative<structured_for_attr>(first->attr) ||
                !std::holds_alternative<structured_for_attr>(second->attr)) {
                out.diagnostic = "fusion: missing structured_for_attr";
                return out;
            }

            const auto& fa = std::get<structured_for_attr>(first->attr);
            const auto& sa = std::get<structured_for_attr>(second->attr);
            const auto first_legality = summarize_loop_legality(*first);
            const auto second_legality = summarize_loop_legality(*second);

            if (!first_legality.canonical_counted || !second_legality.canonical_counted) {
                out.diagnostic = "fusion: both loops require canonical counted bounds";
                return out;
            }
            if (first_legality.is_parallel != second_legality.is_parallel) {
                out.diagnostic = "fusion: parallel execution mode mismatch";
                return out;
            }
            if (first_legality.has_loop_carried_values || second_legality.has_loop_carried_values
                || first_legality.has_reduction || second_legality.has_reduction) {
                out.diagnostic = "fusion: loop-carried values and reductions require dependence lowering";
                return out;
            }
            if (first_legality.has_control_flow || second_legality.has_control_flow) {
                out.diagnostic = "fusion: control flow requires region normalization";
                return out;
            }
            if (first_legality.possible_in_place_dependency
                || second_legality.possible_in_place_dependency) {
                out.diagnostic = "fusion: possible in-place memory dependency";
                return out;
            }

            // Legality: ranks and all bounds must match.
            if (fa.rank != sa.rank) {
                out.diagnostic = "fusion: rank mismatch";
                return out;
            }
            for (std::uint8_t d = 0; d < fa.rank; ++d) {
                const auto& fb = fa.bounds[d];
                const auto& sb = sa.bounds[d];
                if (fb.lower != sb.lower || fb.upper != sb.upper || fb.step != sb.step) {
                    out.diagnostic = "fusion: bounds mismatch on dim " + std::to_string(d);
                    return out;
                }
            }

            if (first->regions.empty() || second->regions.empty() ||
                !first->regions[0] || !second->regions[0]) {
                out.diagnostic = "fusion: missing body region";
                return out;
            }

            // Speculative: checkpoint before structural mutation.
            arena_checkpoint_guard guard{fn};

            // Splice second's body blocks onto first's body.
            first->regions[0]->blocks.splice_back(second->regions[0]->blocks);

            // Remove second from the block's op list.
            blk.ops.erase(second);

            guard.commit();
            out.fused = true;
            return out;
        }
    };

    // -------------------------------------------------------------------------
    // loop_tiling_pass — tile a structured_for using tile sizes in its attr
    //   Produces a new rank*2 nest (outer tiles × inner points) from a rank nest.
    //   tile[d]==0 means skip tiling for that dimension.
    // -------------------------------------------------------------------------

    struct loop_tiling_result {
        bool tiled = false;
        std::string diagnostic;
    };

    struct loop_tiling_pass {
        [[nodiscard]] loop_tiling_result run(
            hl_mir_function& fn,
            hl_block& blk,
            hl_operation* for_op) const {
            loop_tiling_result out;
            if (!for_op || for_op->op != hl_opcode::structured_for) {
                out.diagnostic = "tiling: not a structured_for op";
                return out;
            }
            if (!std::holds_alternative<structured_for_attr>(for_op->attr)) {
                out.diagnostic = "tiling: missing attr";
                return out;
            }

            auto& sf = std::get<structured_for_attr>(for_op->attr);

            // Determine which dimensions are tiled.
            bool any_tiled = false;
            for (std::uint8_t d = 0; d < sf.rank; ++d)
                if (sf.tile[d] > 0) {
                    any_tiled = true;
                    break;
                }

            if (!any_tiled) {
                out.diagnostic = "tiling: no tile sizes set";
                return out;
            }

            arena_checkpoint_guard guard{fn};

            // Build new (outer) structured_for wrapping a new (inner) structured_for.
            // Outer loops: for each tiled dim d, outer IV = (lower..upper step tile[d]).
            // Inner loops: for each tiled dim d, inner IV = (0..tile[d] step 1).
            // Untiled dims stay at their original place in either outer or inner depending
            // on whether they precede or follow tiled dims (simple: keep order).

            const std::uint8_t outer_rank = sf.rank;

            auto* outer_op = fn.make_op(hl_opcode::structured_for);
            if (!outer_op) {
                out.diagnostic = "tiling: OOM";
                return out;
            }

            structured_for_attr outer_attr;
            outer_attr.rank = outer_rank;
            outer_attr.is_parallel = sf.is_parallel;

            auto* inner_op = fn.make_op(hl_opcode::structured_for);
            if (!inner_op) {
                out.diagnostic = "tiling: OOM";
                return out;
            }

            structured_for_attr inner_attr;
            inner_attr.rank = outer_rank;
            inner_attr.is_parallel = false;

            for (std::uint8_t d = 0; d < outer_rank; ++d) {
                const auto& b = sf.bounds[d];
                const std::uint32_t ts = sf.tile[d];
                if (ts > 0) {
                    // Outer: step = tile_size
                    outer_attr.bounds[d] = {
                        b.lower, b.upper, static_cast<int>(ts),
                        b.lower_known, b.upper_known, true
                    };
                    // Inner: 0..tile_size (clipped at boundary by coordinate lowering)
                    inner_attr.bounds[d] = {
                        0, static_cast<int>(ts), b.step,
                        true, true, true
                    };
                }
                else {
                    outer_attr.bounds[d] = b;
                    inner_attr.bounds[d] = {0, 0, 1, true, false, true}; // single-point
                }
            }

            outer_attr.tile = {}; // outer loop is no longer tiled
            inner_attr.tile = {}; // inner loop carries no tile sizes

            // Build regions.
            auto* inner_region = fn.make_region();
            auto* outer_region = fn.make_region();
            if (!inner_region || !outer_region) {
                out.diagnostic = "tiling: OOM";
                return out;
            }

            // Inner body: move original body region contents.
            if (!for_op->regions.empty() && for_op->regions[0])
                inner_region->blocks.splice_back(for_op->regions[0]->blocks);

            inner_op->attr = inner_attr;
            auto inner_rspan = fn.alloc_span<hl_region*>(1);
            if (inner_rspan.empty()) {
                out.diagnostic = "tiling: OOM";
                return out;
            }
            inner_rspan[0] = inner_region;
            inner_op->regions = inner_rspan;
            inner_region->parent_op = inner_op;

            // Outer body: one block containing the inner loop.
            auto* outer_body_blk = fn.make_block();
            if (!outer_body_blk) {
                out.diagnostic = "tiling: OOM";
                return out;
            }
            outer_body_blk->ops.push_back(inner_op);
            inner_op->list_node = {};
            outer_region->blocks.push_back(outer_body_blk);
            outer_body_blk->parent_region = outer_region;

            outer_op->attr = outer_attr;
            auto outer_rspan = fn.alloc_span<hl_region*>(1);
            if (outer_rspan.empty()) {
                out.diagnostic = "tiling: OOM";
                return out;
            }
            outer_rspan[0] = outer_region;
            outer_op->regions = outer_rspan;
            outer_region->parent_op = outer_op;

            // Replace for_op with outer_op in blk.
            outer_op->list_node = for_op->list_node;
            if (for_op->list_node.prev)
                for_op->list_node.prev->list_node.next = outer_op;
            else
                blk.ops.head = outer_op;
            if (for_op->list_node.next)
                for_op->list_node.next->list_node.prev = outer_op;
            else
                blk.ops.tail = outer_op;

            guard.commit();
            out.tiled = true;
            return out;
        }
    };

    // -------------------------------------------------------------------------
    // vectorization_pass — identify conservatively vectorizable parallel loops.
    //   It never upgrades a memref's declared alignment: that value is a layout
    //   guarantee, not an optimization hint. Actual SIMD emission remains a
    //   backend decision and can consume summarize_loop_legality() directly.
    // -------------------------------------------------------------------------

    struct vectorization_result {
        std::size_t dims_annotated = 0;
        std::size_t loops_rejected = 0;
        std::string diagnostic;
    };

    struct vectorization_pass {
        std::uint32_t preferred_vector_width = 256; // bits; 256 = AVX2, 512 = AVX-512

        [[nodiscard]] vectorization_result run(hl_mir_function& fn) const {
            vectorization_result out;
            const auto visit = [&](auto& self, hl_region& region) -> void {
                for (hl_block* blk = region.blocks.head; blk; blk = blk->list_node.next) {
                    for (hl_operation* op = blk->ops.head; op; op = op->list_node.next) {
                        if (op->op == hl_opcode::structured_for &&
                            std::holds_alternative<structured_for_attr>(op->attr)) {
                            auto& sf = std::get<structured_for_attr>(op->attr);
                            if (sf.is_parallel) {
                                const auto legality = summarize_loop_legality(*op);
                                const auto requested_alignment = preferred_vector_width / 8;
                                const bool eligible = legality.rank_one
                                    && legality.canonical_counted
                                    && legality.regular_stride
                                    && legality.all_memrefs_contiguous
                                    && legality.uniform_memory_element_type
                                    && !legality.has_loop_carried_values
                                    && !legality.has_reduction
                                    && !legality.has_control_flow
                                    && !legality.possible_in_place_dependency
                                    && legality.minimum_alignment_bytes >= requested_alignment;
                                if (eligible) {
                                    // Preserve the observed regular-stride fact for consumers
                                    // that only inspect the structured loop attribute.
                                    sf.stride_regular = true;
                                    ++out.dims_annotated;
                                }
                                else {
                                    ++out.loops_rejected;
                                }
                            }
                        }
                        for (std::size_t ri = 0; ri < op->regions.size(); ++ri)
                            if (op->regions[ri]) self(self, *op->regions[ri]);
                    }
                }
            };
            visit(visit, fn.body_region);
            if (out.dims_annotated == 0 && out.loops_rejected != 0)
                out.diagnostic = "vectorization: no parallel loop met the conservative SIMD legality requirements";
            return out;
        }
    };

    // -------------------------------------------------------------------------
    // coordinate_lowering_pass — hl::mir_function → physical_mir_function
    //   The single, explicit HL→LL boundary.  After this pass, all existing
    //   flat passes and backends work unchanged.
    //
    //   For each structured_for op:
    //     • Emits flat header / body / latch blocks.
    //     • Emits IV load_imm (init) + add/cmp_lt/branch_cond (header) + add (latch).
    //   For each memref_load / memref_store:
    //     • Emits address arithmetic (base + Σ idx·stride·sizeof(elem)).
    //     • Emits flat load / store.
    //   Scalar arithmetic ops map 1:1 to flat opcodes.
    // -------------------------------------------------------------------------

    struct coordinate_lowering_result {
        mir::physical_mir_function fn;
        std::vector<std::string> diagnostics;
        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    struct coordinate_lowering_pass {
        [[nodiscard]] coordinate_lowering_result run(
            const hl_mir_function& hl_fn) const {
            coordinate_lowering_result out;
            auto& flat = out.fn;
            flat.function.name = hl_fn.name;
            flat.metadata.current_phase = mir::phase::physical_mir;

            std::uint32_t next_block_id = 1;
            std::uint32_t next_instr_id = 1;
            // Physical register ids are dense and zero-based.  Several backend
            // allocation/ABI tables size storage by the number of registers, so
            // starting at one makes the highest id fall one past that storage.
            std::uint32_t next_preg_id = 0;
            std::uint32_t next_argument_index = 0;

            // Map ssa_value_id → flat preg for scalar values.
            std::unordered_map<std::uint64_t, preg> ssa_to_preg;
            // Active only while lowering one value-carrying structured loop body.
            // A region_yield copies its operands into these loop-carried registers
            // before branching to the loop latch.
            std::vector<preg> active_yield_targets;
            struct lowering_loop_context {
                std::uint32_t preheader = 0;
                std::uint32_t header = 0;
                std::uint32_t latch = 0;
                std::uint32_t exit = 0;
                preg induction{};
                std::int64_t lower = 0;
                std::int64_t upper = 0;
                std::int64_t step = 1;
            };
            std::vector<lowering_loop_context> active_loops;

            auto fresh_preg = [&]() -> preg {
                const auto id = static_cast<std::uint16_t>(next_preg_id++);
                return preg{id, "r" + std::to_string(id)};
            };

            auto fresh_block = [&]() -> allocated_basic_block {
                allocated_basic_block b;
                b.id = next_block_id++;
                return b;
            };

            auto emit_load_imm = [&](allocated_basic_block& blk, preg dst,
                                     std::int64_t val) {
                allocated_instruction i;
                i.id = next_instr_id++;
                i.op = opcode::load_imm;
                i.defs = {allocated_operand::as_preg(dst)};
                i.uses = {allocated_operand::as_i64(val)};
                blk.instructions.push_back(std::move(i));
            };

            auto emit_add = [&](allocated_basic_block& blk, preg dst, preg src,
                                std::int64_t imm) {
                allocated_instruction i;
                i.id = next_instr_id++;
                i.op = opcode::add;
                i.defs = {allocated_operand::as_preg(dst)};
                i.uses = {allocated_operand::as_preg(src), allocated_operand::as_i64(imm)};
                blk.instructions.push_back(std::move(i));
            };

            auto emit_cmp_lt = [&](allocated_basic_block& blk, preg dst,
                                   preg lhs, std::int64_t rhs) {
                allocated_instruction i;
                i.id = next_instr_id++;
                i.op = opcode::cmp_lt;
                i.defs = {allocated_operand::as_preg(dst)};
                i.uses = {allocated_operand::as_preg(lhs), allocated_operand::as_i64(rhs)};
                blk.instructions.push_back(std::move(i));
            };

            auto emit_branch_cond = [&](allocated_basic_block& blk, preg cond,
                                        std::uint32_t true_id, std::uint32_t false_id) {
                allocated_instruction i;
                i.id = next_instr_id++;
                i.op = opcode::branch_cond;
                i.uses = {
                    allocated_operand::as_preg(cond),
                    allocated_operand::as_block(true_id),
                    allocated_operand::as_block(false_id)
                };
                blk.instructions.push_back(std::move(i));
            };

            auto emit_branch = [&](allocated_basic_block& blk, std::uint32_t target_id) {
                allocated_instruction i;
                i.id = next_instr_id++;
                i.op = opcode::branch;
                i.uses = {allocated_operand::as_block(target_id)};
                blk.instructions.push_back(std::move(i));
            };

            // Map hl_opcode scalar ops to flat opcodes.
            auto flat_opcode = [](hl_opcode hlo) -> std::optional<opcode> {
                switch (hlo) {
                case hl_opcode::fadd: return opcode::fadd;
                case hl_opcode::fsub: return opcode::fsub;
                case hl_opcode::fmul: return opcode::fmul;
                case hl_opcode::fdiv: return opcode::fdiv;
                case hl_opcode::fneg: return opcode::fneg;
                case hl_opcode::add: return opcode::add;
                case hl_opcode::sub: return opcode::sub;
                case hl_opcode::mul: return opcode::mul;
                case hl_opcode::div: return opcode::div;
                default: return std::nullopt;
                }
            };

            // Lower one hl_region into a sequence of flat blocks appended to flat.function.blocks.
            // Returns the id of the first block emitted.
            const auto lower_region = [&](auto& self,
                                          const hl_region& region,
                                          std::uint32_t next_sibling_id) -> std::uint32_t {
                std::uint32_t first_id = 0;
                for (const hl_block* hblk = region.blocks.head;
                     hblk; hblk = hblk->list_node.next) {
                    allocated_basic_block flat_blk = fresh_block();
                    if (first_id == 0) first_id = flat_blk.id;

                    // Lower MLIR-style block arguments into flat pregs.
                    // Each block_arg SSA id maps to a dedicated physical register.
                    // The corresponding values arrive via branch operands at call sites;
                    // here we simply allocate the receiving registers so downstream
                    // ssa_to_preg lookups resolve correctly.
                    for (const ssa_value_id& arg_id : hblk->block_args) {
                        if (!ssa_to_preg.contains(arg_id.id)) {
                            preg arg_preg = fresh_preg();
                            ssa_to_preg[arg_id.id] = arg_preg;
                        }
                    }

                    for (const hl_operation* op = hblk->ops.head;
                         op; op = op->list_node.next) {
                        if (op->op == hl_opcode::structured_for) {
                            if (!std::holds_alternative<structured_for_attr>(op->attr)) {
                                out.diagnostics.push_back("coord_lower: structured_for missing attr");
                                continue;
                            }
                            const auto& sf = std::get<structured_for_attr>(op->attr);

                            if (op->regions.empty() || !op->regions[0]) {
                                out.diagnostics.push_back("coord_lower: structured_for missing body");
                                continue;
                            }

                            // Single-dimension lowering (innermost first recursion handles nesting).
                            // For rank > 1 the caller should have tiled / fully lowered by this point,
                            // but we handle rank==1 here and warn for rank>1.
                            if (sf.rank > 1) {
                                out.diagnostics.push_back(
                                    "coord_lower: rank-" + std::to_string(sf.rank) +
                                    " structured_for: lower outer dims first via loop_tiling_pass");
                            }

                            const auto& b = sf.bounds[0];

                            if (sf.trip_count_hint != 0) {
                                const auto body_blocks = op->regions[0]->blocks.size();
                                const auto visits_per_trip = body_blocks + 2u; // header + latch
                                const auto trips = static_cast<std::size_t>(sf.trip_count_hint);
                                const auto max_size = std::numeric_limits<std::size_t>::max();
                                // Account for the prolog, the final failed header
                                // check, and the exit continuation in addition to
                                // the per-iteration header/body/latch visits.
                                const auto fixed_visits = 4u;
                                const auto budget = trips > (max_size - fixed_visits) / visits_per_trip
                                    ? max_size : trips * visits_per_trip + fixed_visits;
                                if (!flat.execution_block_visit_budget
                                    || budget > *flat.execution_block_visit_budget) {
                                    flat.execution_block_visit_budget = budget;
                                }
                            }

                            // Reserve block ids for header, latch, exit.
                            auto header_blk = fresh_block();
                            auto latch_blk = fresh_block();
                            auto exit_blk = fresh_block();

                            const std::uint32_t header_id = header_blk.id;
                            const std::uint32_t latch_id = latch_blk.id;
                            const std::uint32_t exit_id = exit_blk.id;

                            // Prolog: initialize the IV, then enter the header.
                            preg iv = fresh_preg();
                            emit_load_imm(flat_blk, iv, b.lower);
                            emit_branch(flat_blk, header_id);
                            flat.function.blocks.push_back(std::move(flat_blk));

                            // Header tests the initial IV before entering the body;
                            // the latch performs the increment.  This preserves the
                            // declared half-open [lower, upper) iteration space.
                            preg cond = fresh_preg();

                            std::vector<preg> carried;
                            if (!op->operands.empty() || !op->results.empty()) {
                                if (op->operands.size() != op->results.size()) {
                                    out.diagnostics.push_back(
                                        "coord_lower: structured_for loop-carried operand/result count mismatch");
                                    continue;
                                }
                                const hl_block* body_block = op->regions[0]->blocks.head;
                                if (!body_block || body_block->block_args.size() != op->operands.size() + 1) {
                                    out.diagnostics.push_back(
                                        "coord_lower: structured_for loop body must have IV plus carried block arguments");
                                    continue;
                                }
                                ssa_to_preg[body_block->block_args[0].id] = iv;
                                carried.reserve(op->operands.size());
                                for (std::size_t i = 0; i < op->operands.size(); ++i) {
                                    const auto source = ssa_to_preg.find(op->operands[i].id);
                                    if (source == ssa_to_preg.end()) {
                                        out.diagnostics.push_back(
                                            "coord_lower: loop-carried initial value is not defined");
                                        continue;
                                    }
                                    carried.push_back(source->second);
                                    ssa_to_preg[body_block->block_args[i + 1].id] = source->second;
                                    ssa_to_preg[op->results[i].id] = source->second;
                                }
                                if (carried.size() != op->operands.size()) continue;
                                active_yield_targets = carried;
                            }
                            // Lower body recursively; its exit falls to latch.
                            // Keep a local context so affine memory operations can
                            // retain optional physical-MIR descriptors.
                            active_loops.push_back({
                                .preheader = flat_blk.id,
                                .header = header_id,
                                .latch = latch_id,
                                .exit = exit_id,
                                .induction = iv,
                                .lower = b.lower,
                                .upper = b.upper,
                                .step = b.step,
                            });
                            std::uint32_t body_first_id =
                                self(self, *op->regions[0], latch_id);
                            const auto loop_context = active_loops.back();
                            active_loops.pop_back();
                            active_yield_targets.clear();

                            // Empty body region lowers to zero blocks: cycle the loop
                            // directly through the latch so the header→latch→header
                            // back-edge stays well-formed (no dangling bb0 target).
                            if (body_first_id == 0) body_first_id = latch_id;

                            emit_cmp_lt(header_blk, cond, iv, b.upper);
                            emit_branch_cond(header_blk, cond, body_first_id, exit_id);
                            flat.function.blocks.push_back(std::move(header_blk));

                            // Latch advances IV after the body and loops back.
                            emit_add(latch_blk, iv, iv, b.step);
                            emit_branch(latch_blk, header_id);
                            flat.function.blocks.push_back(std::move(latch_blk));

                            flat.canonical_loops.push_back({
                                .preheader_block = loop_context.preheader,
                                .header_block = loop_context.header,
                                .latch_block = loop_context.latch,
                                .exit_block = loop_context.exit,
                                .induction = loop_context.induction,
                                .lower = loop_context.lower,
                                .upper = loop_context.upper,
                                .step = loop_context.step,
                            });

                            // Exit block becomes the continuation.
                            flat_blk = std::move(exit_blk);
                            // (next iteration of outer loop appends to flat_blk)
                            continue;
                        }
                        else if (op->op == hl_opcode::memref_load) {
                            // Emit address arithmetic then flat load.
                            if (!std::holds_alternative<memref_attr>(op->attr)) {
                                out.diagnostics.push_back("coord_lower: memref_load missing attr");
                                continue;
                            }
                            const auto& ma = std::get<memref_attr>(op->attr);
                            const auto& mrt = ma.view;

                            // Base pointer operand.
                            preg base_ptr = fresh_preg();
                            {
                                allocated_instruction li;
                                li.id = next_instr_id++;
                                li.op = opcode::load;
                                li.defs = {allocated_operand::as_preg(base_ptr)};
                                if (!op->operands.empty()) {
                                    // operand[base_operand_index] holds the base pointer SSA id
                                    const auto key = op->operands[
                                        static_cast<std::size_t>(ma.base_operand_index)];
                                    auto it = ssa_to_preg.find(key.id);
                                    if (it != ssa_to_preg.end())
                                        li.uses = {allocated_operand::as_preg(it->second)};
                                }
                                flat_blk.instructions.push_back(std::move(li));
                            }

                            // Compute linear address.
                            // Sub-byte (elem_bits < 8): address arithmetic in bit offsets.
                            //   byte_addr    = base + (Σ idx[i]*stride[i]*elem_bits) / 8
                            //   bit_in_byte  = (Σ idx[i]*stride[i]*elem_bits) % 8
                            //   bitmask      = (1 << elem_bits) - 1
                            //   value        = (load[byte_addr] >> bit_in_byte) & bitmask
                            // Byte-aligned (elem_bits >= 8): byte offset arithmetic.
                            preg addr = base_ptr;
                            const bool sub_byte = (mrt.elem_bits > 0 && mrt.elem_bits < 8);
                            // For sub-byte we accumulate total bit offset; for byte-aligned
                            // we accumulate byte offset directly.
                            preg bit_offset_reg = fresh_preg();
                            if (sub_byte) {
                                emit_load_imm(flat_blk, bit_offset_reg, 0);
                            }
                            const std::int64_t elem_bits = static_cast<std::int64_t>(mrt.elem_bits);
                            const std::int64_t elem_bytes = sub_byte ? 1 : (elem_bits / 8);
                            for (std::uint8_t d = 0; d < mrt.rank; ++d) {
                                // idx SSA comes after base_operand_index in operands
                                const std::size_t idx_pos =
                                    static_cast<std::size_t>(ma.base_operand_index) + 1 + d;
                                if (idx_pos >= op->operands.size()) continue;

                                const auto key = op->operands[idx_pos];
                                auto it = ssa_to_preg.find(key.id);
                                preg idx_preg = (it != ssa_to_preg.end()) ? it->second : fresh_preg();

                                if (sub_byte) {
                                    // stride_bits = stride[d] * elem_bits
                                    const std::int64_t stride_bits = mrt.strides[d] * elem_bits;
                                    // tmp = idx * stride_bits
                                    preg tmp = fresh_preg();
                                    {
                                        allocated_instruction mi;
                                        mi.id = next_instr_id++;
                                        mi.op = opcode::mul;
                                        mi.defs = {allocated_operand::as_preg(tmp)};
                                        mi.uses = {
                                            allocated_operand::as_preg(idx_preg),
                                            allocated_operand::as_i64(stride_bits)
                                        };
                                        flat_blk.instructions.push_back(std::move(mi));
                                    }
                                    // bit_offset_reg += tmp
                                    preg new_bits = fresh_preg();
                                    {
                                        allocated_instruction ai;
                                        ai.id = next_instr_id++;
                                        ai.op = opcode::add;
                                        ai.defs = {allocated_operand::as_preg(new_bits)};
                                        ai.uses = {
                                            allocated_operand::as_preg(bit_offset_reg),
                                            allocated_operand::as_preg(tmp)
                                        };
                                        flat_blk.instructions.push_back(std::move(ai));
                                    }
                                    bit_offset_reg = new_bits;
                                }
                                else {
                                    // stride_bytes = stride[d] * elem_bytes
                                    const std::int64_t stride_bytes = mrt.strides[d] * elem_bytes;
                                    // tmp = idx * stride_bytes
                                    preg tmp = fresh_preg();
                                    {
                                        allocated_instruction mi;
                                        mi.id = next_instr_id++;
                                        mi.op = opcode::mul;
                                        mi.defs = {allocated_operand::as_preg(tmp)};
                                        mi.uses = {
                                            allocated_operand::as_preg(idx_preg),
                                            allocated_operand::as_i64(stride_bytes)
                                        };
                                        flat_blk.instructions.push_back(std::move(mi));
                                    }
                                    // addr = addr + tmp
                                    preg new_addr = fresh_preg();
                                    {
                                        allocated_instruction ai;
                                        ai.id = next_instr_id++;
                                        ai.op = opcode::add;
                                        ai.defs = {allocated_operand::as_preg(new_addr)};
                                        ai.uses = {
                                            allocated_operand::as_preg(addr),
                                            allocated_operand::as_preg(tmp)
                                        };
                                        flat_blk.instructions.push_back(std::move(ai));
                                    }
                                    if (mrt.rank == 1 && !active_loops.empty()
                                        && idx_preg.id == active_loops.back().induction.id) {
                                        flat.affine_addresses.push_back({
                                            .loop_header_block = active_loops.back().header,
                                            .multiply_instruction = next_instr_id - 2,
                                            .address_instruction = next_instr_id - 1,
                                            .induction = idx_preg,
                                            .base = addr,
                                            .scaled_index = tmp,
                                            .address = new_addr,
                                            .stride_bytes = stride_bytes,
                                        });
                                    }
                                    addr = new_addr;
                                }
                            }

                            if (sub_byte) {
                                // byte_addr = base + bit_offset / 8
                                preg byte_off = fresh_preg();
                                {
                                    allocated_instruction di;
                                    di.id = next_instr_id++;
                                    di.op = opcode::div;
                                    di.defs = {allocated_operand::as_preg(byte_off)};
                                    di.uses = {
                                        allocated_operand::as_preg(bit_offset_reg),
                                        allocated_operand::as_i64(8)
                                    };
                                    flat_blk.instructions.push_back(std::move(di));
                                }
                                preg byte_addr = fresh_preg();
                                {
                                    allocated_instruction ai;
                                    ai.id = next_instr_id++;
                                    ai.op = opcode::add;
                                    ai.defs = {allocated_operand::as_preg(byte_addr)};
                                    ai.uses = {
                                        allocated_operand::as_preg(addr),
                                        allocated_operand::as_preg(byte_off)
                                    };
                                    flat_blk.instructions.push_back(std::move(ai));
                                }
                                addr = byte_addr;

                                // bit_in_byte = bit_offset % 8
                                preg bit_in_byte = fresh_preg();
                                {
                                    allocated_instruction mi;
                                    mi.id = next_instr_id++;
                                    mi.op = opcode::mod;
                                    mi.defs = {allocated_operand::as_preg(bit_in_byte)};
                                    mi.uses = {
                                        allocated_operand::as_preg(bit_offset_reg),
                                        allocated_operand::as_i64(8)
                                    };
                                    flat_blk.instructions.push_back(std::move(mi));
                                }

                                // Flat load from computed address.
                                if (!op->results.empty()) {
                                    // raw_byte = load[byte_addr]
                                    preg raw_byte = fresh_preg();
                                    {
                                        allocated_instruction fl;
                                        fl.id = next_instr_id++;
                                        fl.op = opcode::load;
                                        fl.defs = {allocated_operand::as_preg(raw_byte)};
                                        fl.uses = {
                                            allocated_operand::as_preg(addr),
                                            allocated_operand::as_i64(0)
                                        };
                                        flat_blk.instructions.push_back(std::move(fl));
                                    }
                                    // shifted = raw_byte >> bit_in_byte
                                    preg shifted = fresh_preg();
                                    {
                                        allocated_instruction si;
                                        si.id = next_instr_id++;
                                        si.op = opcode::shr;
                                        si.defs = {allocated_operand::as_preg(shifted)};
                                        si.uses = {
                                            allocated_operand::as_preg(raw_byte),
                                            allocated_operand::as_preg(bit_in_byte)
                                        };
                                        flat_blk.instructions.push_back(std::move(si));
                                    }
                                    // bitmask = (1 << elem_bits) - 1
                                    const std::int64_t bitmask = (std::int64_t{1} << elem_bits) - 1;
                                    preg dst = fresh_preg();
                                    {
                                        allocated_instruction bi;
                                        bi.id = next_instr_id++;
                                        bi.op = opcode::bit_and;
                                        bi.defs = {allocated_operand::as_preg(dst)};
                                        bi.uses = {
                                            allocated_operand::as_preg(shifted),
                                            allocated_operand::as_i64(bitmask)
                                        };
                                        flat_blk.instructions.push_back(std::move(bi));
                                    }
                                    ssa_to_preg[op->results[0].id] = dst;
                                }
                            }
                            else {
                                // Flat load from computed address.
                                if (!op->results.empty()) {
                                    preg dst = fresh_preg();
                                    ssa_to_preg[op->results[0].id] = dst;
                                    allocated_instruction fl;
                                    fl.id = next_instr_id++;
                                    fl.op = opcode::fload;
                                    fl.defs = {allocated_operand::as_preg(dst)};
                                    fl.uses = {
                                        allocated_operand::as_preg(addr),
                                        allocated_operand::as_i64(0)
                                    };
                                    flat_blk.instructions.push_back(std::move(fl));
                                }
                            }
                            continue;
                        }
                        else if (op->op == hl_opcode::memref_store) {
                            if (!std::holds_alternative<memref_attr>(op->attr)) {
                                out.diagnostics.push_back("coord_lower: memref_store missing attr");
                                continue;
                            }
                            const auto& ma = std::get<memref_attr>(op->attr);
                            const auto& mrt = ma.view;

                            preg base_ptr = fresh_preg();
                            {
                                allocated_instruction li;
                                li.id = next_instr_id++;
                                li.op = opcode::load;
                                li.defs = {allocated_operand::as_preg(base_ptr)};
                                if (!op->operands.empty()) {
                                    const auto key = op->operands[
                                        static_cast<std::size_t>(ma.base_operand_index)];
                                    auto it = ssa_to_preg.find(key.id);
                                    if (it != ssa_to_preg.end())
                                        li.uses = {allocated_operand::as_preg(it->second)};
                                }
                                flat_blk.instructions.push_back(std::move(li));
                            }

                            preg addr = base_ptr;
                            const bool sub_byte_s = (mrt.elem_bits > 0 && mrt.elem_bits < 8);
                            preg bit_offset_reg_s = fresh_preg();
                            if (sub_byte_s) {
                                emit_load_imm(flat_blk, bit_offset_reg_s, 0);
                            }
                            const std::int64_t elem_bits_s = static_cast<std::int64_t>(mrt.elem_bits);
                            const std::int64_t elem_bytes_s = sub_byte_s ? 1 : (elem_bits_s / 8);
                            for (std::uint8_t d = 0; d < mrt.rank; ++d) {
                                const std::size_t idx_pos =
                                    static_cast<std::size_t>(ma.base_operand_index) + 2 + d; // +2: skip val
                                if (idx_pos >= op->operands.size()) continue;

                                const auto key = op->operands[idx_pos];
                                auto it = ssa_to_preg.find(key.id);
                                preg idx_preg = (it != ssa_to_preg.end()) ? it->second : fresh_preg();

                                if (sub_byte_s) {
                                    const std::int64_t stride_bits = mrt.strides[d] * elem_bits_s;
                                    preg tmp = fresh_preg();
                                    {
                                        allocated_instruction mi;
                                        mi.id = next_instr_id++;
                                        mi.op = opcode::mul;
                                        mi.defs = {allocated_operand::as_preg(tmp)};
                                        mi.uses = {
                                            allocated_operand::as_preg(idx_preg),
                                            allocated_operand::as_i64(stride_bits)
                                        };
                                        flat_blk.instructions.push_back(std::move(mi));
                                    }
                                    preg new_bits = fresh_preg();
                                    {
                                        allocated_instruction ai;
                                        ai.id = next_instr_id++;
                                        ai.op = opcode::add;
                                        ai.defs = {allocated_operand::as_preg(new_bits)};
                                        ai.uses = {
                                            allocated_operand::as_preg(bit_offset_reg_s),
                                            allocated_operand::as_preg(tmp)
                                        };
                                        flat_blk.instructions.push_back(std::move(ai));
                                    }
                                    bit_offset_reg_s = new_bits;
                                }
                                else {
                                    const std::int64_t stride_bytes = mrt.strides[d] * elem_bytes_s;
                                    preg tmp = fresh_preg();
                                    {
                                        allocated_instruction mi;
                                        mi.id = next_instr_id++;
                                        mi.op = opcode::mul;
                                        mi.defs = {allocated_operand::as_preg(tmp)};
                                        mi.uses = {
                                            allocated_operand::as_preg(idx_preg),
                                            allocated_operand::as_i64(stride_bytes)
                                        };
                                        flat_blk.instructions.push_back(std::move(mi));
                                    }
                                    preg new_addr = fresh_preg();
                                    {
                                        allocated_instruction ai;
                                        ai.id = next_instr_id++;
                                        ai.op = opcode::add;
                                        ai.defs = {allocated_operand::as_preg(new_addr)};
                                        ai.uses = {
                                            allocated_operand::as_preg(addr),
                                            allocated_operand::as_preg(tmp)
                                        };
                                        flat_blk.instructions.push_back(std::move(ai));
                                    }
                                    addr = new_addr;
                                }
                            }

                            if (sub_byte_s) {
                                // byte_addr = base + bit_offset / 8
                                preg byte_off = fresh_preg();
                                {
                                    allocated_instruction di;
                                    di.id = next_instr_id++;
                                    di.op = opcode::div;
                                    di.defs = {allocated_operand::as_preg(byte_off)};
                                    di.uses = {
                                        allocated_operand::as_preg(bit_offset_reg_s),
                                        allocated_operand::as_i64(8)
                                    };
                                    flat_blk.instructions.push_back(std::move(di));
                                }
                                preg byte_addr = fresh_preg();
                                {
                                    allocated_instruction ai;
                                    ai.id = next_instr_id++;
                                    ai.op = opcode::add;
                                    ai.defs = {allocated_operand::as_preg(byte_addr)};
                                    ai.uses = {
                                        allocated_operand::as_preg(addr),
                                        allocated_operand::as_preg(byte_off)
                                    };
                                    flat_blk.instructions.push_back(std::move(ai));
                                }
                                addr = byte_addr;

                                // bit_in_byte = bit_offset % 8
                                preg bit_in_byte = fresh_preg();
                                {
                                    allocated_instruction mi;
                                    mi.id = next_instr_id++;
                                    mi.op = opcode::mod;
                                    mi.defs = {allocated_operand::as_preg(bit_in_byte)};
                                    mi.uses = {
                                        allocated_operand::as_preg(bit_offset_reg_s),
                                        allocated_operand::as_i64(8)
                                    };
                                    flat_blk.instructions.push_back(std::move(mi));
                                }

                                // Value to store is operand[base_operand_index+1].
                                preg val_preg = fresh_preg();
                                const std::size_t val_idx_s =
                                    static_cast<std::size_t>(ma.base_operand_index) + 1;
                                if (val_idx_s < op->operands.size()) {
                                    auto it = ssa_to_preg.find(op->operands[val_idx_s].id);
                                    if (it != ssa_to_preg.end()) val_preg = it->second;
                                }

                                // Read-modify-write: load byte, clear bits, insert value.
                                const std::int64_t bitmask_s = (std::int64_t{1} << elem_bits_s) - 1;
                                // raw_byte = load[byte_addr]
                                preg raw_byte = fresh_preg();
                                {
                                    allocated_instruction fl;
                                    fl.id = next_instr_id++;
                                    fl.op = opcode::load;
                                    fl.defs = {allocated_operand::as_preg(raw_byte)};
                                    fl.uses = {
                                        allocated_operand::as_preg(addr),
                                        allocated_operand::as_i64(0)
                                    };
                                    flat_blk.instructions.push_back(std::move(fl));
                                }
                                // shifted_mask = bitmask << bit_in_byte
                                preg shifted_mask = fresh_preg();
                                {
                                    allocated_instruction si;
                                    si.id = next_instr_id++;
                                    si.op = opcode::shl;
                                    si.defs = {allocated_operand::as_preg(shifted_mask)};
                                    si.uses = {
                                        allocated_operand::as_i64(bitmask_s),
                                        allocated_operand::as_preg(bit_in_byte)
                                    };
                                    flat_blk.instructions.push_back(std::move(si));
                                }
                                // clear_mask = ~shifted_mask  (bit_not)
                                preg clear_mask = fresh_preg();
                                {
                                    allocated_instruction ni;
                                    ni.id = next_instr_id++;
                                    ni.op = opcode::bit_not;
                                    ni.defs = {allocated_operand::as_preg(clear_mask)};
                                    ni.uses = {allocated_operand::as_preg(shifted_mask)};
                                    flat_blk.instructions.push_back(std::move(ni));
                                }
                                // cleared = raw_byte & clear_mask
                                preg cleared = fresh_preg();
                                {
                                    allocated_instruction ai;
                                    ai.id = next_instr_id++;
                                    ai.op = opcode::bit_and;
                                    ai.defs = {allocated_operand::as_preg(cleared)};
                                    ai.uses = {
                                        allocated_operand::as_preg(raw_byte),
                                        allocated_operand::as_preg(clear_mask)
                                    };
                                    flat_blk.instructions.push_back(std::move(ai));
                                }
                                // val_shifted = val & bitmask  (clamp), then << bit_in_byte
                                preg val_masked = fresh_preg();
                                {
                                    allocated_instruction bi;
                                    bi.id = next_instr_id++;
                                    bi.op = opcode::bit_and;
                                    bi.defs = {allocated_operand::as_preg(val_masked)};
                                    bi.uses = {
                                        allocated_operand::as_preg(val_preg),
                                        allocated_operand::as_i64(bitmask_s)
                                    };
                                    flat_blk.instructions.push_back(std::move(bi));
                                }
                                preg val_shifted = fresh_preg();
                                {
                                    allocated_instruction si;
                                    si.id = next_instr_id++;
                                    si.op = opcode::shl;
                                    si.defs = {allocated_operand::as_preg(val_shifted)};
                                    si.uses = {
                                        allocated_operand::as_preg(val_masked),
                                        allocated_operand::as_preg(bit_in_byte)
                                    };
                                    flat_blk.instructions.push_back(std::move(si));
                                }
                                // merged = cleared | val_shifted
                                preg merged = fresh_preg();
                                {
                                    allocated_instruction oi;
                                    oi.id = next_instr_id++;
                                    oi.op = opcode::bit_or;
                                    oi.defs = {allocated_operand::as_preg(merged)};
                                    oi.uses = {
                                        allocated_operand::as_preg(cleared),
                                        allocated_operand::as_preg(val_shifted)
                                    };
                                    flat_blk.instructions.push_back(std::move(oi));
                                }
                                // store merged → [byte_addr]
                                {
                                    allocated_instruction fs;
                                    fs.id = next_instr_id++;
                                    fs.op = opcode::store;
                                    fs.uses = {
                                        allocated_operand::as_preg(merged),
                                        allocated_operand::as_preg(addr),
                                        allocated_operand::as_i64(0)
                                    };
                                    flat_blk.instructions.push_back(std::move(fs));
                                }
                            }
                            else {
                                // Value to store is operand[base_operand_index+1].
                                preg val_preg = fresh_preg();
                                const std::size_t val_idx =
                                    static_cast<std::size_t>(ma.base_operand_index) + 1;
                                if (val_idx < op->operands.size()) {
                                    auto it = ssa_to_preg.find(op->operands[val_idx].id);
                                    if (it != ssa_to_preg.end()) val_preg = it->second;
                                }

                                {
                                    allocated_instruction fs;
                                    fs.id = next_instr_id++;
                                    fs.op = opcode::fstore;
                                    fs.uses = {
                                        allocated_operand::as_preg(val_preg),
                                        allocated_operand::as_preg(addr),
                                        allocated_operand::as_i64(0)
                                    };
                                    flat_blk.instructions.push_back(std::move(fs));
                                }
                            }
                            continue;
                        }
                        else if (op->op == hl_opcode::argument) {
                            if (!op->results.empty()) {
                                preg dst = fresh_preg();
                                ssa_to_preg[op->results[0].id] = dst;
                                allocated_instruction load;
                                load.id = next_instr_id++;
                                load.op = opcode::load_arg;
                                load.defs = {allocated_operand::as_preg(dst)};
                                load.uses = {allocated_operand::as_argument_index(
                                    next_argument_index++)};
                                flat_blk.instructions.push_back(std::move(load));
                            }
                            continue;
                        }
                        else if (op->op == hl_opcode::region_yield ||
                                 op->op == hl_opcode::ret) {
                            if (op->op == hl_opcode::region_yield && !active_yield_targets.empty()) {
                                if (op->operands.size() != active_yield_targets.size()) {
                                    out.diagnostics.push_back(
                                        "coord_lower: region_yield loop-carried value count mismatch");
                                }
                                else {
                                    for (std::size_t i = 0; i < op->operands.size(); ++i) {
                                        const auto value = ssa_to_preg.find(op->operands[i].id);
                                        if (value == ssa_to_preg.end()) {
                                            out.diagnostics.push_back(
                                                "coord_lower: region_yield references an undefined value");
                                            continue;
                                        }
                                        emit_add(flat_blk, active_yield_targets[i], value->second, 0);
                                    }
                                }
                            }
                            // Nested yields continue in their parent. A root
                            // yield/ret becomes a physical return and carries
                            // its SSA result when one is present.
                            if (next_sibling_id != 0) {
                                emit_branch(flat_blk, next_sibling_id);
                            } else {
                                allocated_instruction ret;
                                ret.id = next_instr_id++;
                                ret.op = opcode::ret;
                                if (!op->operands.empty()) {
                                    const auto value =
                                        ssa_to_preg.find(op->operands[0].id);
                                    if (value != ssa_to_preg.end())
                                        ret.uses = {
                                            allocated_operand::as_preg(value->second)};
                                }
                                flat_blk.instructions.push_back(std::move(ret));
                            }
                            continue;
                        }
                        else if (op->op == hl_opcode::constant) {
                            if (!op->results.empty() &&
                                std::holds_alternative<constant_attr>(op->attr)) {
                                const auto& constant = std::get<constant_attr>(op->attr);
                                if (constant.kind == constant_kind::floating_point) {
                                    out.diagnostics.push_back(
                                        "coord_lower: floating constants require a floating immediate backend");
                                    continue;
                                }
                                preg dst = fresh_preg();
                                ssa_to_preg[op->results[0].id] = dst;
                                const auto value = constant.kind == constant_kind::boolean
                                    ? static_cast<std::int64_t>(constant.boolean)
                                    : constant.integer;
                                emit_load_imm(flat_blk, dst, value);
                            } else if (!op->results.empty()) {
                                out.diagnostics.push_back(
                                    "coord_lower: constant missing constant_attr");
                            }
                            continue;
                        }
                        else if (op->op == hl_opcode::loop_index) {
                            // loop_index is represented by the first body block
                            // argument, pre-bound to the physical IV above.
                            if (op->results.size() != 1
                                || !ssa_to_preg.contains(op->results[0].id)) {
                                out.diagnostics.push_back(
                                    "coord_lower: loop_index is missing its induction-value binding");
                            }
                            continue;
                        }
                        else {
                            // Try flat opcode mapping.
                            if (auto fop = flat_opcode(op->op)) {
                                allocated_instruction fi;
                                fi.id = next_instr_id++;
                                fi.op = *fop;
                                for (const auto& ssav : op->operands) {
                                    auto it = ssa_to_preg.find(ssav.id);
                                    if (it != ssa_to_preg.end())
                                        fi.uses.push_back(allocated_operand::as_preg(it->second));
                                }
                                if (!op->results.empty()) {
                                    preg dst = fresh_preg();
                                    ssa_to_preg[op->results[0].id] = dst;
                                    fi.defs = {allocated_operand::as_preg(dst)};
                                }
                                flat_blk.instructions.push_back(std::move(fi));
                            }
                            else {
                                out.diagnostics.push_back(
                                    "coord_lower: unhandled hl_opcode " +
                                    std::to_string(static_cast<int>(op->op)));
                            }
                        }
                    }
                    // Implicit fallthrough: if the block has no terminator, wire
                    // it to the next HL block (id = next_block_id, since fresh_block
                    // will consume it) or to next_sibling_id for the last block.
                    auto is_phys_terminator = [](lithe::codegen::opcode o) noexcept {
                        using op_t = lithe::codegen::opcode;
                        return o == op_t::ret || o == op_t::branch || o == op_t::branch_cond;
                    };
                    if (flat_blk.instructions.empty() ||
                        !is_phys_terminator(flat_blk.instructions.back().op)) {
                        const std::uint32_t fall_target =
                            hblk->list_node.next ? next_block_id : next_sibling_id;
                        if (fall_target != 0)
                            emit_branch(flat_blk, fall_target);
                    }
                    flat.function.blocks.push_back(std::move(flat_blk));
                }
                return first_id;
            };

            // Lower the top-level body region.
            lower_region(lower_region, hl_fn.body_region, 0);

            // Wire up entry block.
            if (!flat.function.blocks.empty()) {
                flat.function.cfg.entry_block = flat.function.blocks.front().id;
                // Emit ret in last block if not already terminated.
                auto& last_blk = flat.function.blocks.back();
                if (last_blk.instructions.empty() ||
                    last_blk.instructions.back().op != opcode::ret) {
                    allocated_instruction ret_i;
                    ret_i.id = next_instr_id++;
                    ret_i.op = opcode::ret;
                    last_blk.instructions.push_back(std::move(ret_i));
                }
            }

            return out;
        }
    };

    // -------------------------------------------------------------------------
    // task_plan_extraction_pass — extract task_decomposition_plan from a parallel
    //   structured_for after coordinate lowering is done conceptually.
    //   Operates on hl_mir_function pre-lowering: finds is_parallel structured_for
    //   and fills task_decomposition_plan.  The caller wraps the kernel pointer.
    // -------------------------------------------------------------------------

    struct task_plan_extraction_result {
        std::vector<task_decomposition_plan> plans;
        std::vector<std::string> diagnostics;
    };

    struct task_plan_extraction_pass {
        void (*default_kernel)(void*, std::size_t, std::size_t) = nullptr;

        [[nodiscard]] task_plan_extraction_result run(const hl_mir_function& fn) const {
            task_plan_extraction_result out;

            const auto visit = [&](auto& self, const hl_region& region) -> void {
                for (const hl_block* blk = region.blocks.head; blk; blk = blk->list_node.next) {
                    for (const hl_operation* op = blk->ops.head; op; op = op->list_node.next) {
                        if (op->op == hl_opcode::structured_for &&
                            std::holds_alternative<structured_for_attr>(op->attr)) {
                            const auto& sf = std::get<structured_for_attr>(op->attr);
                            if (sf.is_parallel) {
                                task_decomposition_plan plan;
                                plan.rank = sf.rank;
                                plan.chunk = 1;
                                for (std::uint8_t d = 0; d < sf.rank; ++d) {
                                    plan.bounds[d] = {
                                        sf.bounds[d].lower,
                                        sf.bounds[d].upper,
                                        sf.bounds[d].step
                                    };
                                }
                                plan.kernel = default_kernel;
                                plan.user_data = nullptr;
                                out.plans.push_back(plan);
                            }
                        }
                        for (std::size_t ri = 0; ri < op->regions.size(); ++ri)
                            if (op->regions[ri]) self(self, *op->regions[ri]);
                    }
                }
            };

            visit(visit, fn.body_region);
            return out;
        }
    };

    // -------------------------------------------------------------------------
    // apply_passes — variadic pass composition (struct-with-run pattern)
    //   apply_passes(fn, pass_a{}, pass_b{}) runs passes left-to-right.
    //   Each pass's run() is called with fn; return value is discarded.
    //   For passes that transform the function, chain them manually.
    // -------------------------------------------------------------------------

    template <class Fn, class... Passes>
    void apply_passes(Fn& fn, Passes&&... passes) {
        (passes.run(fn), ...);
    }
} // namespace lithe::codegen::hl
