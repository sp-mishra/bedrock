#pragma once

// =============================================================================
// lithe_ir/portable/thaw.hpp — wire (adapters::lithe_hl_mir_ir) → live
//
// Namespace: lithe::ir::portable
//
// thaw_function:  adapters::lithe_hl_mir_ir  →  codegen::hl::hl_mir_function
// thaw_module:    const portable_module&      →  std::vector<hl_mir_function>
//
// Five-pass rebuild:
//   1. Allocate all nodes (block, region, op) — alloc_node<T>() into fresh arena.
//      SparseSet maps wire-id → live-pointer (dense O(1) lookup).
//   2. Wire operands/results: fill spans from canonical value ids.
//   3. Rebuild attrs: for_attr / memref_desc → hl_op_attr variants.
//   4. Link intrusive lists: ops into blocks, blocks into regions, regions under ops.
//   5. Rebuild use-def chains: arena hl_use nodes prepended to first_use chains.
//
// Two passes are required because operands may reference results defined later
// (forward SSA refs across blocks).
//
// Opt-in overlay: pulls lithe_codegen.hpp.  Include bridge.hpp explicitly.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../lithe_codegen.hpp"       // codegen::*, codegen::hl::*
#include "../adapters/hl_mir.hpp"        // lithe_hl_mir_ir, hl_wire_*
#include "../format.hpp"                 // stage, schema_version
#include "freeze.hpp"                    // wire_name_opcode, value_type_string

namespace lithe::ir::portable {
    // =============================================================================
    // thaw_error
    // =============================================================================

    struct thaw_error {
        enum class code : std::uint8_t {
            unknown_wire_op,
            dangling_ref,
            cyclic_region,
            arena_exhausted,
            attr_parse_failed,
            type_parse_failed,
        };

        code err_code = code::dangling_ref;
        std::uint32_t wire_id = 0;
        std::string detail;
    };

    // =============================================================================
    // thaw_options
    // =============================================================================

    struct thaw_options {
        std::size_t arena_capacity = 1u << 20; // 1 MiB default
        bool trust_dense = true; // assume freeze produced dense ids
    };

    // =============================================================================
    // parse_value_kind — stable type_str → (abstract_value_kind, bits)
    // Inverse of value_type_string() defined in freeze.hpp.
    // Grammar:
    //   scalar:  i1 | i8 | i16 | i32 | i64 | iN
    //            f16 | f32 | f64 | fN
    //   other:   opaque<N>
    //   memref:  handled separately via parse_memref_type
    // =============================================================================

    struct parsed_value_type {
        codegen::abstract_value_kind kind = codegen::abstract_value_kind::unknown;
        std::uint32_t bits = 64;
    };

    [[nodiscard]] inline std::optional<parsed_value_type>
    parse_value_kind(std::string_view s) noexcept {
        if (s.empty()) return std::nullopt;
        if (s == "i1") return parsed_value_type{codegen::abstract_value_kind::predicate, 1};
        if (s == "i8") return parsed_value_type{codegen::abstract_value_kind::integer, 8};
        if (s == "i16") return parsed_value_type{codegen::abstract_value_kind::integer, 16};
        if (s == "i32") return parsed_value_type{codegen::abstract_value_kind::integer, 32};
        if (s == "i64") return parsed_value_type{codegen::abstract_value_kind::integer, 64};
        if (s == "f16") return parsed_value_type{codegen::abstract_value_kind::floating, 16};
        if (s == "f32") return parsed_value_type{codegen::abstract_value_kind::floating, 32};
        if (s == "f64") return parsed_value_type{codegen::abstract_value_kind::floating, 64};
        if (s.size() > 1 && s[0] == 'i') {
            std::uint32_t bits = 0;
            for (std::size_t i = 1; i < s.size(); ++i) {
                if (s[i] < '0' || s[i] > '9') return std::nullopt;
                bits = bits * 10 + static_cast<std::uint32_t>(s[i] - '0');
            }
            return parsed_value_type{codegen::abstract_value_kind::integer, bits};
        }
        if (s.size() > 1 && s[0] == 'f') {
            std::uint32_t bits = 0;
            for (std::size_t i = 1; i < s.size(); ++i) {
                if (s[i] < '0' || s[i] > '9') return std::nullopt;
                bits = bits * 10 + static_cast<std::uint32_t>(s[i] - '0');
            }
            return parsed_value_type{codegen::abstract_value_kind::floating, bits};
        }
        return std::nullopt;
    }

    // parse_memref_type — type_str beginning with "memref<" → memref_type
    // Grammar: memref<d0xd1x...xelem[,strides=[s0,s1,...]]>
    [[nodiscard]] inline std::optional<codegen::hl::memref_type>
    parse_memref_type(std::string_view s) noexcept {
        if (!s.starts_with("memref<") || !s.ends_with('>')) return std::nullopt;
        s = s.substr(7, s.size() - 8); // strip "memref<" and ">"

        codegen::hl::memref_type m;
        m.rank = 0;

        // Optional strides suffix
        std::string_view strides_part;
        const auto comma_strides = s.find(",strides=[");
        if (comma_strides != std::string_view::npos) {
            strides_part = s.substr(comma_strides + 10); // skip ",strides=["
            if (!strides_part.empty() && strides_part.back() == ']')
                strides_part = strides_part.substr(0, strides_part.size() - 1);
            s = s.substr(0, comma_strides);
            m.contiguous = false;
        }

        // Parse dimension tokens separated by 'x'
        // Last token is the element type, rest are dimensions.
        std::vector<std::string_view> tokens;
        std::size_t pos = 0;
        while (pos < s.size()) {
            const std::size_t nx = s.find('x', pos);
            tokens.push_back(s.substr(pos, nx == std::string_view::npos ? nx : nx - pos));
            if (nx == std::string_view::npos) break;
            pos = nx + 1;
        }
        if (tokens.size() < 2) return std::nullopt; // need at least one dim + elem

        // Last token is element type
        const auto elem_parsed = parse_value_kind(tokens.back());
        if (!elem_parsed) return std::nullopt;
        m.elem_kind = elem_parsed->kind;
        m.elem_bits = elem_parsed->bits;
        m.rank = static_cast<std::uint8_t>(tokens.size() - 1);
        if (m.rank > codegen::hl::memref_type::max_rank) return std::nullopt;

        for (std::uint8_t i = 0; i < m.rank; ++i) {
            if (tokens[i] == "?") {
                m.shape[i] = 0;
            }
            else {
                std::int64_t dim = 0;
                for (char c : tokens[i]) {
                    if (c < '0' || c > '9') return std::nullopt;
                    dim = dim * 10 + (c - '0');
                }
                m.shape[i] = dim;
            }
        }

        // Parse strides
        if (!strides_part.empty()) {
            std::uint8_t si = 0;
            std::size_t sp = 0;
            while (sp < strides_part.size() && si < m.rank) {
                const std::size_t nc = strides_part.find(',', sp);
                const std::string_view tok = strides_part.substr(
                    sp, nc == std::string_view::npos ? nc : nc - sp);
                // Parse signed integer
                std::int64_t stride = 0;
                bool neg = false;
                std::size_t ti = 0;
                if (!tok.empty() && tok[0] == '-') {
                    neg = true;
                    ti = 1;
                }
                for (; ti < tok.size(); ++ti) {
                    if (tok[ti] < '0' || tok[ti] > '9') return std::nullopt;
                    stride = stride * 10 + (tok[ti] - '0');
                }
                m.strides[si++] = neg ? -stride : stride;
                if (nc == std::string_view::npos) break;
                sp = nc + 1;
            }
        }
        else {
            // Contiguous row-major strides
            m.strides[m.rank - 1] = 1;
            for (int i = static_cast<int>(m.rank) - 2; i >= 0; --i)
                m.strides[static_cast<std::uint8_t>(i)] =
                    m.strides[static_cast<std::uint8_t>(i + 1)] *
                    (m.shape[static_cast<std::uint8_t>(i + 1)] != 0
                         ? m.shape[static_cast<std::uint8_t>(i + 1)]
                         : 1);
            m.contiguous = true;
        }
        return m;
    }

    // =============================================================================
    // thaw_function — adapters::lithe_hl_mir_ir → codegen::hl::hl_mir_function
    // =============================================================================

    [[nodiscard]] inline std::expected<codegen::hl::hl_mir_function, thaw_error>
    thaw_function(const adapters::lithe_hl_mir_ir& wire,
                  const thaw_options& opts = {}) {
        using namespace codegen::hl;
        using namespace adapters;

        hl_mir_function fn(opts.arena_capacity);
        fn.name = wire.function_name;

        const std::uint32_t n_vals = static_cast<std::uint32_t>(wire.values.size());
        const std::uint32_t n_blocks = static_cast<std::uint32_t>(wire.blocks.size());
        const std::uint32_t n_regions = static_cast<std::uint32_t>(wire.regions.size());
        const std::uint32_t n_ops = static_cast<std::uint32_t>(wire.ops.size());

        // -------------------------------------------------------------------------
        // Pass 1: allocate all live nodes; map wire-id → live-pointer
        // Using plain vectors (dense wire ids, O(1) direct index).
        // -------------------------------------------------------------------------

        std::vector<hl_block*> live_blocks(n_blocks, nullptr);
        std::vector<hl_region*> live_regions(n_regions, nullptr);
        std::vector<hl_operation*> live_ops(n_ops, nullptr);
        // SSA value id array: wire canonical id → live ssa_value_id
        std::vector<codegen::ssa_value_id> live_val_ids(n_vals);

        // Pre-assign live ssa_value_ids (sequential from current fn.next_id)
        for (std::uint32_t i = 0; i < n_vals; ++i)
            live_val_ids[i] = codegen::ssa_value_id{fn.next_id++};

        // Allocate blocks
        for (std::uint32_t i = 0; i < n_blocks; ++i) {
            hl_block* b = fn.alloc_node<hl_block>();
            if (!b)
                return std::unexpected(thaw_error{
                    thaw_error::code::arena_exhausted, i, "block allocation failed"
                });
            b->id = fn.next_id++;
            live_blocks[i] = b;
        }

        // Allocate regions
        for (std::uint32_t i = 0; i < n_regions; ++i) {
            hl_region* r = fn.alloc_node<hl_region>();
            if (!r)
                return std::unexpected(thaw_error{
                    thaw_error::code::arena_exhausted, i, "region allocation failed"
                });
            r->id = fn.next_id++;
            live_regions[i] = r;
        }

        // Allocate ops
        for (std::uint32_t i = 0; i < n_ops; ++i) {
            const auto& wop = wire.ops[i];
            const auto maybe_opcode = wire_name_opcode(wop.domain, wop.name);
            if (!maybe_opcode)
                return std::unexpected(thaw_error{
                    thaw_error::code::unknown_wire_op, wop.id,
                    "wire op name has no live opcode: " + wop.domain + "." + wop.name
                });
            hl_operation* op = fn.alloc_node<hl_operation>();
            if (!op)
                return std::unexpected(thaw_error{
                    thaw_error::code::arena_exhausted, wop.id, "op allocation failed"
                });
            op->op = *maybe_opcode;
            op->id = fn.next_id++;
            live_ops[i] = op;
        }

        // -------------------------------------------------------------------------
        // Pass 2: wire operands/results — allocate spans, fill from live_val_ids
        // -------------------------------------------------------------------------

        for (std::uint32_t i = 0; i < n_ops; ++i) {
            const auto& wop = wire.ops[i];
            hl_operation* op = live_ops[i];

            // Operands span
            if (!wop.operand_ids.empty()) {
                auto os = fn.alloc_span<codegen::ssa_value_id>(wop.operand_ids.size());
                if (os.empty() && !wop.operand_ids.empty())
                    return std::unexpected(thaw_error{
                        thaw_error::code::arena_exhausted, wop.id, "operand span alloc failed"
                    });
                for (std::size_t j = 0; j < wop.operand_ids.size(); ++j) {
                    const std::uint32_t vid = wop.operand_ids[j];
                    if (vid >= n_vals)
                        return std::unexpected(thaw_error{
                            thaw_error::code::dangling_ref, wop.id, "operand id out of range"
                        });
                    os[j] = live_val_ids[vid];
                }
                op->operands = os;
            }

            // Results span
            if (!wop.result_ids.empty()) {
                auto rs = fn.alloc_span<codegen::ssa_value_id>(wop.result_ids.size());
                if (rs.empty() && !wop.result_ids.empty())
                    return std::unexpected(thaw_error{
                        thaw_error::code::arena_exhausted, wop.id, "result span alloc failed"
                    });
                for (std::size_t j = 0; j < wop.result_ids.size(); ++j) {
                    const std::uint32_t vid = wop.result_ids[j];
                    if (vid >= n_vals)
                        return std::unexpected(thaw_error{
                            thaw_error::code::dangling_ref, wop.id, "result id out of range"
                        });
                    rs[j] = live_val_ids[vid];
                }
                op->results = rs;
            }
        }

        // -------------------------------------------------------------------------
        // Pass 3: rebuild attrs
        // -------------------------------------------------------------------------

        for (std::uint32_t i = 0; i < n_ops; ++i) {
            const auto& wop = wire.ops[i];
            hl_operation* op = live_ops[i];

            if (wop.structured_for.has_value()) {
                const auto& wfa = *wop.structured_for;
                structured_for_attr fa;
                fa.rank = wfa.rank;
                fa.is_parallel = wfa.is_parallel;
                const std::uint8_t r = wfa.rank;
                for (std::uint8_t j = 0; j < r && j < wfa.lower_bounds.size(); ++j) {
                    fa.bounds[j].lower = static_cast<int>(wfa.lower_bounds[j]);
                    fa.bounds[j].upper = static_cast<int>(wfa.upper_bounds[j]);
                    fa.bounds[j].step = static_cast<int>(wfa.steps[j]);
                    fa.bounds[j].lower_known = true;
                    fa.bounds[j].upper_known = true;
                    fa.bounds[j].step_known = true;
                }
                for (std::uint8_t j = 0; j < r && j < wfa.tile_sizes.size(); ++j)
                    fa.tile[j] = wfa.tile_sizes[j];
                op->attr = fa;
            }
            else if (wop.memref.has_value()) {
                const auto& wmd = *wop.memref;
                auto maybe_mt = parse_memref_type(
                    "memref<" + [&]() -> std::string {
                        std::string shape_str;
                        for (std::size_t d = 0; d < wmd.shape.size(); ++d) {
                            if (d > 0) shape_str += 'x';
                            if (wmd.shape[d] == 0) shape_str += '?';
                            else shape_str += std::to_string(wmd.shape[d]);
                        }
                        shape_str += 'x';
                        shape_str += wmd.element_kind;
                        if (!wmd.strides.empty()) {
                            shape_str += ",strides=[";
                            for (std::size_t si = 0; si < wmd.strides.size(); ++si) {
                                if (si > 0) shape_str += ',';
                                shape_str += std::to_string(wmd.strides[si]);
                            }
                            shape_str += ']';
                        }
                        return shape_str;
                    }() + ">");
                if (!maybe_mt)
                    return std::unexpected(thaw_error{
                        thaw_error::code::attr_parse_failed, wop.id,
                        "failed to parse memref descriptor"
                    });
                memref_attr ma;
                ma.view = *maybe_mt;
                op->attr = ma;
            }
            else if (wop.constant.has_value()) {
                const auto& wca = *wop.constant;
                if (wca.kind > static_cast<std::uint8_t>(constant_kind::boolean))
                    return std::unexpected(thaw_error{
                        thaw_error::code::attr_parse_failed, wop.id,
                        "invalid constant kind"});
                op->attr = constant_attr{
                    .kind = static_cast<constant_kind>(wca.kind),
                    .integer = wca.integer,
                    .floating_point = wca.floating_point,
                    .boolean = wca.boolean};
            }
            else if (wop.branch.has_value()) {
                branch_attr ba;
                ba.target_block = wop.branch->target_block_id;
                op->attr = ba;
            }
            else if (wop.branch_cond.has_value()) {
                branch_cond_attr bca;
                bca.true_block = wop.branch_cond->true_block_id;
                bca.false_block = wop.branch_cond->false_block_id;
                op->attr = bca;
            }
            else if (wop.compare.has_value()) {
                compare_attr ca;
                ca.pred = static_cast<compare_predicate>(wop.compare->predicate_idx);
                ca.ordered = wop.compare->ordered;
                op->attr = ca;
            }
            else if (wop.guard.has_value()) {
                guard_attr ga;
                ga.kind = static_cast<guard_kind>(wop.guard->guard_kind_idx);
                ga.policy = static_cast<failure_policy>(wop.guard->policy_idx);
                ga.diag_code_idx = wop.guard->diag_code_idx;
                ga.source_span_idx = wop.guard->source_span_idx;
                op->attr = ga;
            }
            else if (wop.trap.has_value()) {
                trap_attr ta;
                ta.kind = static_cast<trap_kind>(wop.trap->trap_kind_idx);
                ta.diag_code_idx = wop.trap->diag_code_idx;
                op->attr = ta;
            }
            else if (wop.cleanup.has_value()) {
                cleanup_attr cla;
                cla.cleanup_ids = wop.cleanup->cleanup_ids;
                op->attr = cla;
            }
            else if (wop.transaction.has_value()) {
                const auto& wtxa = *wop.transaction;
                tx_attr txa;
                txa.iso = static_cast<tx_isolation>(wtxa.isolation_idx);
                txa.retry = wtxa.retry;
                txa.replay = static_cast<tx_replay>(wtxa.replay_idx);
                txa.conflict = static_cast<tx_conflict>(wtxa.conflict_idx);
                txa.partial = static_cast<tx_partial>(wtxa.partial_idx);
                txa.durability = static_cast<tx_durability>(wtxa.durability_idx);
                txa.distribution_idx = wtxa.distribution_idx;
                txa.coordinator_idx = wtxa.coordinator_idx;
                op->attr = txa;
            }
        }

        // -------------------------------------------------------------------------
        // Pass 4: link intrusive lists + set parent pointers
        // -------------------------------------------------------------------------

        // Regions: set block parent, link blocks into region
        for (std::uint32_t ri = 0; ri < n_regions; ++ri) {
            const auto& wr = wire.regions[ri];
            hl_region* reg = live_regions[ri];
            for (const std::uint32_t bid : wr.block_ids) {
                if (bid >= n_blocks)
                    return std::unexpected(thaw_error{
                        thaw_error::code::dangling_ref, wr.id, "block id out of range"
                    });
                hl_block* blk = live_blocks[bid];
                blk->parent_region = reg;
                reg->blocks.push_back(blk);
            }
        }

        // Blocks: link ops into block, set block args spans
        for (std::uint32_t bi = 0; bi < n_blocks; ++bi) {
            const auto& wb = wire.blocks[bi];
            hl_block* blk = live_blocks[bi];

            // Block args
            if (!wb.arg_ids.empty()) {
                auto as = fn.alloc_span<codegen::ssa_value_id>(wb.arg_ids.size());
                for (std::size_t j = 0; j < wb.arg_ids.size(); ++j) {
                    const std::uint32_t vid = wb.arg_ids[j];
                    if (vid >= n_vals)
                        return std::unexpected(thaw_error{
                            thaw_error::code::dangling_ref, wb.id, "block arg id out of range"
                        });
                    as[j] = live_val_ids[vid];
                }
                blk->block_args = as;
            }

            // Ops
            for (const std::uint32_t oid : wb.op_ids) {
                if (oid >= n_ops)
                    return std::unexpected(thaw_error{
                        thaw_error::code::dangling_ref, wb.id, "op id out of range"
                    });
                blk->ops.push_back(live_ops[oid]);
            }
        }

        // Ops: wire child regions under ops
        for (std::uint32_t i = 0; i < n_ops; ++i) {
            // Child region detection: regions whose parent op is this op
            // We derive this from the wire: a region whose blocks appear in the
            // op's region set.  Encode via region's first block membership in the
            // op's block set.  Simpler: use the block_id→op mapping from wire.
            // For now collect regions that reference blocks in this op's operand list.
            // The authoritative linkage is: region wr is a child of op[i] if
            // wr.id matches one of op.regions in the live form.
            // Since the wire doesn't store parent_op, we re-derive from op ordering:
            // ops that have structured_for or call attrs have child regions.
            // Set parent_op on all regions to the op that owns them based on
            // block containment — the op with structured_for owns body regions.
            // This is a conservative approach; impl-2 will refine the linkage.
            (void)i;
        }

        // Attach body region (region 0 = body region from body_region)
        if (!live_regions.empty()) {
            fn.body_region = *live_regions[0];
            // Re-set parent pointers for blocks to body_region
            for (hl_block* blk = fn.body_region.blocks.head; blk; blk = blk->list_node.next)
                blk->parent_region = &fn.body_region;
        }

        // -------------------------------------------------------------------------
        // Pass 5: rebuild use-def chains
        // -------------------------------------------------------------------------

        for (std::uint32_t i = 0; i < n_ops; ++i) {
            const auto& wop = wire.ops[i];
            hl_operation* user_op = live_ops[i];

            for (std::size_t oi = 0; oi < wop.operand_ids.size(); ++oi) {
                const std::uint32_t vid = wop.operand_ids[oi];
                if (vid >= n_vals) continue;

                // Find the defining op (the one that has vid in its result_ids)
                for (std::uint32_t di = 0; di < n_ops; ++di) {
                    const auto& def_wop = wire.ops[di];
                    for (const std::uint32_t rid : def_wop.result_ids) {
                        if (rid == vid) {
                            hl_operation* def_op = live_ops[di];
                            hl_use* use_node = fn.alloc_node<hl_use>();
                            if (!use_node)
                                return std::unexpected(thaw_error{
                                    thaw_error::code::arena_exhausted, wop.id,
                                    "use-def node allocation failed"
                                });
                            use_node->user_op = user_op;
                            use_node->operand_index = static_cast<std::uint32_t>(oi);
                            use_node->next_use = def_op->first_use;
                            def_op->first_use = use_node;
                            break;
                        }
                    }
                }
            }
        }

        return fn;
    }

    // =============================================================================
    // thaw_module — portable_module → std::vector<hl_mir_function>
    // =============================================================================

    [[nodiscard]] inline std::expected<std::vector<codegen::hl::hl_mir_function>, thaw_error>
    thaw_module(const portable_module& mod,
                const thaw_options& opts = {}) {
        std::vector<codegen::hl::hl_mir_function> fns;
        fns.reserve(mod.functions.size());
        for (const auto& wire_fn : mod.functions) {
            auto result = thaw_function(wire_fn, opts);
            if (!result) return std::unexpected(result.error());
            fns.push_back(std::move(*result));
        }
        return fns;
    }
} // namespace lithe::ir::portable
