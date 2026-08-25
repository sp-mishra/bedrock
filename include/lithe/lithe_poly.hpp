#pragma once
// Internal fragment — include via lithe_codegen_pipeline.hpp (umbrella).
// Depends on: lithe::codegen + lithe::pdg types.

// =============================================================================
// Phase 4: Polyhedral Loop Analysis
// Namespace: lithe::poly
//
// Extends the natural-loop infrastructure (loop_info / loop_analysis_result)
// with an affine iteration-space model and two classic loop transformations:
//
//  • affine_matrix          — flat row-major constraint matrix; no external BLAS
//  • loop_bounds            — static lower/upper/step per induction variable
//  • loop_induction_var     — links a preg to its bounds and increment instruction
//  • polyhedral_loop        — loop_info + ivars + schedule matrix
//  • polyhedral_analysis_result
//  • extract_polyhedral_pass — identify IVs and build affine bounds matrices
//  • loop_fusion_pass        — merge adjacent loops with identical bounds
//  • loop_interchange_pass   — swap outer/inner IVs; distance-vector legality check
// =============================================================================

namespace lithe::poly {
    // -------------------------------------------------------------------------
    // 1. affine_matrix — flat row-major matrix over int
    //    Rows represent constraints; last column is the RHS constant.
    //    For n induction variables the iteration-domain matrix is (2n × n+1):
    //      row 2k   : 0 … 1(col k) … 0 | -lower  (lower bound: v_k ≥ lower)
    //      row 2k+1 : 0 … -1(col k) … 0 | upper-1 (upper bound: v_k < upper)
    //
    //    Stack-allocated storage: loop nests in tensor/layout domains rarely
    //    exceed depth 8.  MaxIVs=8 gives cols≤9 and rows≤16, so the flat
    //    storage is 144 ints (576 bytes) — well within a cache line cluster.
    // -------------------------------------------------------------------------

    struct affine_matrix {
        // MaxIVs induction variables → cols ≤ MaxIVs+1, rows ≤ 2*MaxIVs.
        static constexpr std::size_t MaxIVs = 8;
        static constexpr std::size_t MaxCols = MaxIVs + 1; // 9
        static constexpr std::size_t MaxRows = 2 * MaxIVs; // 16
        static constexpr std::size_t MaxCells = MaxRows * MaxCols; // 144

        std::size_t rows = 0;
        std::size_t cols = 0; // n_vars + 1  (RHS in last column)
        // Row-major; only [0, rows*cols) is live. Rest is zero-initialised.
        std::array<int, MaxCells> data{};

        [[nodiscard]] constexpr int at(const std::size_t r, const std::size_t c) const noexcept {
            return data[r * cols + c];
        }

        [[nodiscard]] constexpr int& at(const std::size_t r, const std::size_t c) noexcept {
            return data[r * cols + c];
        }

        // n×n identity (square, no RHS column — used for schedule matrices).
        [[nodiscard]] static constexpr affine_matrix identity(const std::size_t n) {
            affine_matrix m;
            m.rows = n;
            m.cols = n;
            for (std::size_t i = 0; i < n; ++i) m.at(i, i) = 1;
            return m;
        }

        [[nodiscard]] static constexpr affine_matrix zero(const std::size_t r, const std::size_t c) {
            affine_matrix m;
            m.rows = r;
            m.cols = c;
            // data{} already zero-initialised; nothing more to do.
            return m;
        }

        // Left-multiply: result = lhs * rhs  (lhs.cols == rhs.rows assumed).
        [[nodiscard]] static constexpr affine_matrix mul(const affine_matrix& lhs,
                                                         const affine_matrix& rhs) {
            assert(lhs.cols == rhs.rows);
            affine_matrix out = zero(lhs.rows, rhs.cols);
            for (std::size_t i = 0; i < lhs.rows; ++i)
                for (std::size_t k = 0; k < lhs.cols; ++k)
                    for (std::size_t j = 0; j < rhs.cols; ++j)
                        out.at(i, j) += lhs.at(i, k) * rhs.at(k, j);
            return out;
        }

        // Compare only the live region, not the entire flat buffer.
        [[nodiscard]] constexpr bool operator==(const affine_matrix& o) const noexcept {
            if (rows != o.rows || cols != o.cols) return false;
            for (std::size_t i = 0; i < rows; ++i)
                for (std::size_t j = 0; j < cols; ++j)
                    if (at(i, j) != o.at(i, j)) return false;
            return true;
        }
    };

    // -------------------------------------------------------------------------
    // 2. loop_bounds — per-IV static bound information
    // -------------------------------------------------------------------------

    struct loop_bounds {
        std::uint32_t induction_preg_id = 0;
        int lower = 0;
        int upper = 0; // exclusive upper; 0 when not statically known
        int step = 1;
        bool lower_known = false;
        bool upper_known = false;
        bool step_known = true;

        [[nodiscard]] bool fully_known() const noexcept {
            return lower_known && upper_known && step_known;
        }
    };

    // -------------------------------------------------------------------------
    // 3. loop_induction_var
    // -------------------------------------------------------------------------

    struct loop_induction_var {
        std::uint32_t preg_id = 0;
        std::uint32_t def_instr_id = 0;
        loop_bounds bounds;
    };

    // -------------------------------------------------------------------------
    // 4. polyhedral_loop
    // -------------------------------------------------------------------------

    struct polyhedral_loop {
        codegen::loop_info base;
        std::vector<loop_induction_var> ivars;
        affine_matrix iteration;
        affine_matrix schedule;
        std::vector<std::string> diagnostics;
        bool is_affine = false;
    };

    // -------------------------------------------------------------------------
    // 5. polyhedral_analysis_result
    // -------------------------------------------------------------------------

    struct polyhedral_analysis_result {
        std::vector<polyhedral_loop> loops;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // -------------------------------------------------------------------------
    // 6. extract_polyhedral_pass
    // -------------------------------------------------------------------------

    struct extract_polyhedral_pass {
        [[nodiscard]] polyhedral_analysis_result run(
            const codegen::mir::physical_mir_function& fn,
            const codegen::loop_analysis_result& loops,
            const codegen::value_flow_analysis_result& vf
        ) const {
            polyhedral_analysis_result result;

            std::unordered_map<std::uint32_t,
                               const codegen::allocated_basic_block*> block_ptr;
            for (const auto& b : fn.function.blocks)
                block_ptr[b.id] = &b;

            for (const auto& li : loops.loops) {
                polyhedral_loop pl;
                pl.base = li;

                const auto* hdr = block_ptr.count(li.header) ? block_ptr.at(li.header) : nullptr;
                if (!hdr) {
                    result.diagnostics.push_back(
                        "poly: header bb" + std::to_string(li.header) + " not found");
                    result.loops.push_back(std::move(pl));
                    continue;
                }

                // (a) Induction variable detection: x = x ± imm in header.
                for (const auto& instr : hdr->instructions) {
                    if (instr.op != codegen::opcode::add &&
                        instr.op != codegen::opcode::sub)
                        continue;
                    if (instr.defs.size() != 1) continue;
                    if (instr.defs[0].type != codegen::allocated_operand::kind::preg) continue;

                    const auto& dst = std::get<codegen::preg>(instr.defs[0].value);
                    const codegen::preg* src_reg = nullptr;
                    std::int64_t step_val = 0;
                    bool has_imm = false;

                    for (const auto& u : instr.uses) {
                        if (u.type == codegen::allocated_operand::kind::preg)
                            src_reg = &std::get<codegen::preg>(u.value);
                        else if (u.type == codegen::allocated_operand::kind::immediate_i64) {
                            step_val = std::get<std::int64_t>(u.value);
                            has_imm = true;
                        }
                    }

                    if (!src_reg || !has_imm || dst.id != src_reg->id) continue;

                    const int step_signed = (instr.op == codegen::opcode::sub)
                                                ? -static_cast<int>(step_val)
                                                : static_cast<int>(step_val);

                    loop_induction_var iv;
                    iv.preg_id = dst.id;
                    iv.def_instr_id = instr.id;
                    iv.bounds.induction_preg_id = dst.id;
                    iv.bounds.step = step_signed;
                    iv.bounds.step_known = true;

                    // (b) Lower bound: scan all blocks for a load_imm defining this
                    // preg outside the loop body (def_use_chains only keeps the last
                    // definition, which is the increment inside the loop itself).
                    for (const auto& blk : fn.function.blocks) {
                        if (li.body.contains(blk.id)) continue;
                        for (const auto& di : blk.instructions) {
                            if (di.op != codegen::opcode::load_imm) continue;
                            bool defines_iv = false;
                            for (const auto& def_op : di.defs) {
                                if (def_op.type == codegen::allocated_operand::kind::preg &&
                                    std::get<codegen::preg>(def_op.value).id == dst.id) {
                                    defines_iv = true;
                                    break;
                                }
                            }
                            if (!defines_iv) continue;
                            for (const auto& u : di.uses) {
                                if (u.type == codegen::allocated_operand::kind::immediate_i64) {
                                    iv.bounds.lower = static_cast<int>(std::get<std::int64_t>(u.value));
                                    iv.bounds.lower_known = true;
                                }
                            }
                            break;
                        }
                        if (iv.bounds.lower_known) break;
                    }

                    // Upper bound: cmp_lt/cmp_le in header with this IV and an imm.
                    for (const auto& ci : hdr->instructions) {
                        if (ci.op != codegen::opcode::cmp_lt &&
                            ci.op != codegen::opcode::cmp_le)
                            continue;
                        if (ci.uses.size() < 2) continue;
                        if (ci.uses[0].type != codegen::allocated_operand::kind::preg) continue;
                        if (std::get<codegen::preg>(ci.uses[0].value).id != dst.id) continue;
                        if (ci.uses[1].type == codegen::allocated_operand::kind::immediate_i64) {
                            int bound = static_cast<int>(std::get<std::int64_t>(ci.uses[1].value));
                            if (ci.op == codegen::opcode::cmp_le) ++bound;
                            iv.bounds.upper = bound;
                            iv.bounds.upper_known = true;
                        }
                        break;
                    }

                    pl.ivars.push_back(iv);
                }

                // (c) Build iteration domain matrix.
                const std::size_t n = pl.ivars.size();
                if (n > 0) {
                    pl.iteration = affine_matrix::zero(2 * n, n + 1);
                    for (std::size_t k = 0; k < n; ++k) {
                        const auto& bnd = pl.ivars[k].bounds;
                        pl.iteration.at(2 * k, k) = 1;
                        pl.iteration.at(2 * k, n) = bnd.lower_known ? -bnd.lower : 0;
                        pl.iteration.at(2 * k + 1, k) = -1;
                        pl.iteration.at(2 * k + 1, n) = bnd.upper_known ? bnd.upper - 1 : 0;
                    }
                    pl.schedule = affine_matrix::identity(n);
                    pl.is_affine = std::ranges::all_of(pl.ivars, [](const auto& iv) {
                        return iv.bounds.fully_known();
                    });
                }

                result.loops.push_back(std::move(pl));
            }

            return result;
        }
    };

    // -------------------------------------------------------------------------
    // 7. loop_fusion_pass
    // -------------------------------------------------------------------------

    struct loop_fusion_result {
        codegen::mir::physical_mir_function function;
        polyhedral_analysis_result analysis;
        std::size_t fused_pairs = 0;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    struct loop_fusion_pass {
        [[nodiscard]] loop_fusion_result run(
            const codegen::mir::physical_mir_function& fn,
            const polyhedral_analysis_result& poly,
            const codegen::value_flow_analysis_result& vf
        ) const {
            loop_fusion_result result;
            result.function = fn;
            result.analysis = poly;

            // Sort loops by header block id to establish program order before
            // attempting pairwise fusion.  analyze_loops uses an unordered_map
            // internally so the order is otherwise non-deterministic.
            std::ranges::sort(result.analysis.loops, [](const polyhedral_loop& a,
                                                        const polyhedral_loop& b) {
                return a.base.header < b.base.header;
            });

            auto rebuild_cfg = [](codegen::mir::physical_mir_function& f) {
                f.function.cfg.successors.clear();
                f.function.cfg.predecessors.clear();
                for (const auto& b : f.function.blocks) {
                    f.function.cfg.successors[b.id] = b.successors;
                    for (const auto s : b.successors)
                        f.function.cfg.predecessors[s].push_back(b.id);
                }
            };

            bool changed = true;
            while (changed) {
                changed = false;
                auto& loops = result.analysis.loops;
                for (std::size_t i = 0; i + 1 < loops.size(); ++i) {
                    auto& A = loops[i];
                    auto& B = loops[i + 1];

                    if (A.ivars.empty() || B.ivars.empty()) continue;
                    const auto& ba = A.ivars[0].bounds;
                    const auto& bb = B.ivars[0].bounds;

                    if (!ba.fully_known() || !bb.fully_known()) continue;
                    if (ba.lower != bb.lower || ba.upper != bb.upper ||
                        ba.step != bb.step)
                        continue;

                    // Adjacency: B.header must be a direct external successor of
                    // some A exit block, or reachable through exactly one trivial
                    // pre-header block (single-predecessor, single-successor).
                    const bool adjacent = std::ranges::any_of(
                        A.base.exit_blocks,
                        [&](const std::uint32_t ex) {
                            const auto it = result.function.function.cfg.successors.find(ex);
                            if (it == result.function.function.cfg.successors.end()) return false;
                            for (const auto s : it->second) {
                                if (A.base.body.contains(s)) continue;
                                if (s == B.base.header) return true;
                                // One-hop through a trivial pre-header (single predecessor).
                                const auto pred_it = result.function.function.cfg.predecessors.find(s);
                                if (pred_it == result.function.function.cfg.predecessors.end()) continue;
                                if (pred_it->second.size() != 1) continue; // not a unique pre-header
                                const auto it2 = result.function.function.cfg.successors.find(s);
                                if (it2 == result.function.function.cfg.successors.end()) continue;
                                if (it2->second.size() == 1 &&
                                    it2->second[0] == B.base.header)
                                    return true;
                            }
                            return false;
                        });
                    if (!adjacent) continue;

                    // Conservative cross-dep check.
                    std::unordered_set<std::uint32_t> a_body_instrs;
                    for (const auto bid : A.base.body) {
                        for (const auto& blk : result.function.function.blocks) {
                            if (blk.id != bid) continue;
                            for (const auto& ins : blk.instructions)
                                a_body_instrs.insert(ins.id);
                        }
                    }
                    std::unordered_set<std::uint32_t> b_hdr_instrs;
                    for (const auto& blk : result.function.function.blocks) {
                        if (blk.id != B.base.header) continue;
                        for (const auto& ins : blk.instructions)
                            b_hdr_instrs.insert(ins.id);
                    }

                    bool cross_dep = false;
                    for (const auto& [vid, chain] : vf.def_use_chains) {
                        if (vid == A.ivars[0].bounds.induction_preg_id) continue;
                        if (!a_body_instrs.contains(chain.definition.instruction_id)) continue;
                        for (const auto& u : chain.uses) {
                            if (b_hdr_instrs.contains(u.instruction_id)) {
                                cross_dep = true;
                                break;
                            }
                        }
                        if (cross_dep) break;
                    }
                    if (cross_dep) continue;

                    // Perform fusion.
                    std::uint32_t a_latch = A.base.back_edges.empty()
                                                ? 0
                                                : A.base.back_edges[0].from;
                    std::uint32_t b_latch = B.base.back_edges.empty()
                                                ? 0
                                                : B.base.back_edges[0].from;

                    std::uint32_t b_body_entry = 0;
                    {
                        const auto sit = result.function.function.cfg.successors.find(B.base.header);
                        if (sit != result.function.function.cfg.successors.end()) {
                            for (const auto s : sit->second) {
                                if (B.base.body.contains(s) && s != B.base.header) {
                                    b_body_entry = s;
                                    break;
                                }
                            }
                        }
                        if (b_body_entry == 0) b_body_entry = B.base.header;
                    }

                    // Redirect A's latch → b_body_entry.
                    if (a_latch) {
                        for (auto& blk : result.function.function.blocks) {
                            if (blk.id != a_latch) continue;
                            std::ranges::replace(blk.successors, A.base.header, b_body_entry);
                            for (auto& ins : blk.instructions)
                                for (auto& op : ins.uses)
                                    if (op.type == codegen::allocated_operand::kind::block &&
                                        std::get<std::uint32_t>(op.value) == A.base.header)
                                        op.value = b_body_entry;
                            break;
                        }
                    }

                    // Redirect B's latch → A.header.
                    if (b_latch) {
                        for (auto& blk : result.function.function.blocks) {
                            if (blk.id != b_latch) continue;
                            std::ranges::replace(blk.successors, B.base.header, A.base.header);
                            for (auto& ins : blk.instructions)
                                for (auto& op : ins.uses)
                                    if (op.type == codegen::allocated_operand::kind::block &&
                                        std::get<std::uint32_t>(op.value) == B.base.header)
                                        op.value = A.base.header;
                            break;
                        }
                    }

                    // Remove B's header if it is distinct from b_body_entry.
                    if (b_body_entry != B.base.header) {
                        auto it = std::ranges::find_if(
                            result.function.function.blocks,
                            [&](const codegen::allocated_basic_block& b) {
                                return b.id == B.base.header;
                            });
                        if (it != result.function.function.blocks.end())
                            result.function.function.blocks.erase(it);
                    }

                    rebuild_cfg(result.function);
                    ++result.fused_pairs;
                    loops.erase(loops.begin() + static_cast<std::ptrdiff_t>(i + 1));
                    changed = true;
                    break;
                }
            }

            return result;
        }
    };

    // -------------------------------------------------------------------------
    // 8. loop_interchange_pass
    // -------------------------------------------------------------------------

    struct loop_interchange_result {
        codegen::mir::physical_mir_function function;
        polyhedral_analysis_result analysis;
        std::size_t interchanged = 0;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    struct loop_interchange_pass {
        [[nodiscard]] loop_interchange_result run(
            const codegen::mir::physical_mir_function& fn,
            const polyhedral_analysis_result& poly,
            const lithe::pdg::pdg_build_result& pdg_result,
            const lithe::semantic::domain_type hint = lithe::semantic::domain_type::unknown
        ) const {
            loop_interchange_result result;
            result.function = fn;
            result.analysis = poly;

            using DT = lithe::semantic::domain_type;
            const bool any_domain = (hint == DT::unknown);
            const bool want_layout = lithe::semantic::has_domain(hint, DT::layout);
            const bool want_tensor = lithe::semantic::has_domain(hint, DT::tensor);

            for (auto& pl : result.analysis.loops) {
                if (!pl.is_affine) continue;
                if (pl.ivars.size() < 2) continue;
                if (!any_domain && !want_layout && !want_tensor) continue;

                // Collect body instruction ids.
                std::unordered_set<std::uint32_t> body_instrs;
                for (const auto bid : pl.base.body)
                    for (const auto& blk : result.function.function.blocks) {
                        if (blk.id != bid) continue;
                        for (const auto& ins : blk.instructions)
                            body_instrs.insert(ins.id);
                    }

                // Permutation matrix P: swap rows 0 and 1.
                const std::size_t n = pl.ivars.size();
                affine_matrix P = affine_matrix::identity(n);
                std::swap(P.at(0, 0), P.at(1, 0));
                std::swap(P.at(0, 1), P.at(1, 1));

                // Distance-vector legality check.
                bool legal = true;
                for (const auto& node : pdg_result.graph.nodes()) {
                    if (!body_instrs.contains(node.instr_id)) continue;
                    for (const auto& edge : pdg_result.graph.out_edges(node.instr_id)) {
                        if (!edge.is_data()) continue;
                        if (!body_instrs.contains(edge.to_instr)) continue;

                        const int d0 = iv_axis_of(node.instr_id, pl, 0, result.function) ? 1 : 0;
                        const int d1 = iv_axis_of(node.instr_id, pl, 1, result.function) ? 1 : 0;
                        const int pd0 = P.at(0, 0) * d0 + P.at(0, 1) * d1;
                        const int pd1 = P.at(1, 0) * d0 + P.at(1, 1) * d1;
                        if (pd0 < 0 || (pd0 == 0 && pd1 < 0)) {
                            legal = false;
                            break;
                        }
                    }
                    if (!legal) break;
                }
                if (!legal) continue;

                pl.schedule = affine_matrix::mul(P, pl.schedule);

                // Physically reorder inner/outer blocks.
                const std::uint32_t inner_iv_def = pl.ivars[1].def_instr_id;
                const std::uint32_t outer_iv_def = pl.ivars[0].def_instr_id;
                std::uint32_t inner_block = 0, outer_latch = 0;
                for (const auto& blk : result.function.function.blocks) {
                    if (!pl.base.body.contains(blk.id)) continue;
                    for (const auto& ins : blk.instructions) {
                        if (ins.id == inner_iv_def) inner_block = blk.id;
                        if (ins.id == outer_iv_def && blk.id != pl.base.header)
                            outer_latch = blk.id;
                    }
                }

                if (inner_block && outer_latch && inner_block != outer_latch) {
                    auto find_blk = [&](std::uint32_t id) {
                        return std::ranges::find_if(
                            result.function.function.blocks,
                            [id](const codegen::allocated_basic_block& b) { return b.id == id; });
                    };
                    auto it_i = find_blk(inner_block);
                    auto it_o = find_blk(outer_latch);
                    if (it_i != result.function.function.blocks.end() &&
                        it_o != result.function.function.blocks.end())
                        std::iter_swap(it_i, it_o);
                }

                ++result.interchanged;
            }

            return result;
        }

    private:
        static bool iv_axis_of(
            const std::uint32_t instr_id,
            const polyhedral_loop& pl,
            const std::size_t axis_idx,
            const codegen::mir::physical_mir_function& fn) noexcept {
            if (axis_idx >= pl.ivars.size()) return false;
            const std::uint32_t iv_def = pl.ivars[axis_idx].def_instr_id;
            std::uint32_t instr_block = 0, iv_block = 0;
            for (const auto& b : fn.function.blocks)
                for (const auto& i : b.instructions) {
                    if (i.id == instr_id) instr_block = b.id;
                    if (i.id == iv_def) iv_block = b.id;
                }
            return instr_block != 0 && instr_block == iv_block;
        }
    };

    // -------------------------------------------------------------------------
    // 9. Convenience free functions
    // -------------------------------------------------------------------------

    [[nodiscard]] inline polyhedral_analysis_result extract_polyhedral(
        const codegen::mir::physical_mir_function& fn,
        codegen::mir_pass_context& ctx) {
        const auto& loops = codegen::get_or_compute_loop(ctx, fn);
        const auto& vf = codegen::get_or_compute_def_use(ctx, fn);
        return extract_polyhedral_pass{}.run(fn, loops, vf);
    }

    [[nodiscard]] inline loop_fusion_result apply_loop_fusion(
        const codegen::mir::physical_mir_function& fn,
        polyhedral_analysis_result& poly,
        codegen::mir_pass_context& ctx) {
        const auto& vf = codegen::get_or_compute_def_use(ctx, fn);
        return loop_fusion_pass{}.run(fn, poly, vf);
    }

    [[nodiscard]] inline loop_interchange_result apply_loop_interchange(
        const codegen::mir::physical_mir_function& fn,
        polyhedral_analysis_result& poly,
        codegen::mir_pass_context& ctx,
        const lithe::semantic::domain_type hint = lithe::semantic::domain_type::unknown) {
        const auto pdg = lithe::pdg::build_pdg(fn, ctx);
        return loop_interchange_pass{}.run(fn, poly, pdg, hint);
    }
} // namespace lithe::poly
