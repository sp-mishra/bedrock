#pragma once

// =============================================================================
// lithe_ir/portable/freeze.hpp — live hl_mir_function → wire (freeze)
//
// Namespace: lithe::ir::portable
//
// freeze_function:  codegen::hl::hl_mir_function  →  adapters::lithe_hl_mir_ir
// freeze_module:    span<hl_mir_function*>          →  portable_module
//
// Algorithm (canonical walk):
//   1. Assign canonical dense value/block/region ids (structural order, not
//      allocation order) — mandatory for digest determinism.
//   2. Map hl_opcode → stable (domain, name) via opcode_wire_table.
//   3. Translate hl_op_attr variants → hl_wire_op optional payloads.
//   4. Build wire ops, blocks, regions in canonical order.
//   5. Set source_stage = stage::lowered.
//
// Opt-in overlay: this header pulls lithe_codegen.hpp.  For the light IR core
// (no codegen), use lithe_ir_core.hpp alone; include bridge.hpp or this header
// explicitly when the live MIR bridge is required.
//
// Include DAG direction: lithe_ir/portable → codegen (never the reverse).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../../lithe_codegen.hpp"       // codegen::*, codegen::hl::*
#include "../adapters/hl_mir.hpp"        // lithe_hl_mir_ir, hl_wire_*
#include "../format.hpp"                 // stage, schema_version
#include "module.hpp"                    // portable_module

namespace lithe::ir::portable {
    // =============================================================================
    // freeze_error
    // =============================================================================

    struct freeze_error {
        enum class code : std::uint8_t {
            unmapped_opcode,
            dangling_operand,
            missing_terminator,
            attr_translation_failed,
            arena_walk_failed,
        };

        code err_code = code::arena_walk_failed;
        std::uint32_t op_id = 0;
        std::string detail;
    };

    // =============================================================================
    // freeze_options
    // =============================================================================

    struct freeze_options {
        bool intern_strings = true;
        schema_version schema_override{};
        bool schema_override_set = false;
    };

    // =============================================================================
    // module_freeze_options — for freeze_module
    // =============================================================================

    struct module_freeze_options {
        freeze_options fn_opts;
        std::vector<portable_import> imports;
        std::vector<portable_export> exports;
        std::vector<portable_global> globals;
        portable_constant_pool constants;
        capability_set declared_capabilities;
        portable_manifest manifest;
    };

    // =============================================================================
    // opcode_wire_table — single source-of-truth for live↔wire opcode mapping
    // =============================================================================

    namespace detail {
        struct opcode_wire_entry {
            codegen::hl::hl_opcode op;
            std::string_view domain;
            std::string_view name;
        };

        // All hl_opcode variants mapped — additions here automatically propagate to
        // both opcode_wire_name() and wire_name_opcode().
        inline constexpr std::array<opcode_wire_entry, 48> opcode_wire_table = {
            {
                {codegen::hl::hl_opcode::structured_for, "lithe.hl", "structured_for"},
                {codegen::hl::hl_opcode::structured_reduce, "lithe.hl", "structured_reduce"},
                {codegen::hl::hl_opcode::region_yield, "lithe.hl", "region_yield"},
                {codegen::hl::hl_opcode::loop_index, "lithe.hl", "loop_index"},
                {codegen::hl::hl_opcode::memref_load, "lithe.hl", "memref_load"},
                {codegen::hl::hl_opcode::memref_store, "lithe.hl", "memref_store"},
                {codegen::hl::hl_opcode::fadd, "lithe.hl", "fadd"},
                {codegen::hl::hl_opcode::fsub, "lithe.hl", "fsub"},
                {codegen::hl::hl_opcode::fmul, "lithe.hl", "fmul"},
                {codegen::hl::hl_opcode::fdiv, "lithe.hl", "fdiv"},
                {codegen::hl::hl_opcode::fneg, "lithe.hl", "fneg"},
                {codegen::hl::hl_opcode::add, "lithe.hl", "add"},
                {codegen::hl::hl_opcode::sub, "lithe.hl", "sub"},
                {codegen::hl::hl_opcode::mul, "lithe.hl", "mul"},
                {codegen::hl::hl_opcode::div, "lithe.hl", "div"},
                {codegen::hl::hl_opcode::exp, "lithe.hl", "exp"},
                {codegen::hl::hl_opcode::log, "lithe.hl", "log"},
                {codegen::hl::hl_opcode::sqrt, "lithe.hl", "sqrt"},
                {codegen::hl::hl_opcode::abs, "lithe.hl", "abs"},
                {codegen::hl::hl_opcode::call, "lithe.hl", "call"},
                {codegen::hl::hl_opcode::constant, "lithe.hl", "constant"},
                {codegen::hl::hl_opcode::argument, "lithe.hl", "argument"},
                // Schema 1.1.0 — CFG + compare/select
                {codegen::hl::hl_opcode::branch, "lithe.hl", "branch"},
                {codegen::hl::hl_opcode::branch_cond, "lithe.hl", "branch_cond"},
                {codegen::hl::hl_opcode::ret, "lithe.hl", "return"},
                {codegen::hl::hl_opcode::icmp, "lithe.hl", "icmp"},
                {codegen::hl::hl_opcode::fcmp, "lithe.hl", "fcmp"},
                {codegen::hl::hl_opcode::select, "lithe.hl", "select"},
                // Schema 1.2.0 — Integer ops
                {codegen::hl::hl_opcode::sdiv, "lithe.hl", "sdiv"},
                {codegen::hl::hl_opcode::udiv, "lithe.hl", "udiv"},
                {codegen::hl::hl_opcode::srem, "lithe.hl", "srem"},
                {codegen::hl::hl_opcode::urem, "lithe.hl", "urem"},
                {codegen::hl::hl_opcode::bit_and, "lithe.hl", "bit_and"},
                {codegen::hl::hl_opcode::bit_or, "lithe.hl", "bit_or"},
                {codegen::hl::hl_opcode::bit_xor, "lithe.hl", "bit_xor"},
                {codegen::hl::hl_opcode::bit_not, "lithe.hl", "bit_not"},
                {codegen::hl::hl_opcode::shl, "lithe.hl", "shl"},
                {codegen::hl::hl_opcode::lshr, "lithe.hl", "lshr"},
                {codegen::hl::hl_opcode::ashr, "lithe.hl", "ashr"},
                // Schema 1.3.0 — Safety
                {codegen::hl::hl_opcode::guard, "lithe.hl", "guard"},
                {codegen::hl::hl_opcode::trap, "lithe.hl", "trap"},
                // Schema 1.4.0 — Cleanup / defer
                {codegen::hl::hl_opcode::cleanup_region, "lithe.hl", "cleanup_region"},
                {codegen::hl::hl_opcode::cleanup_yield, "lithe.hl", "cleanup_yield"},
                // Schema 1.5.0 — Transactions
                {codegen::hl::hl_opcode::tx_region, "lithe.hl", "tx.region"},
                {codegen::hl::hl_opcode::tx_read, "lithe.hl", "tx.read"},
                {codegen::hl::hl_opcode::tx_write, "lithe.hl", "tx.write"},
                {codegen::hl::hl_opcode::tx_abort, "lithe.hl", "tx.abort"},
                {codegen::hl::hl_opcode::tx_yield, "lithe.hl", "tx.yield"},
            }
        };
    } // namespace detail

    // opcode_wire_name: live opcode → (domain, name). Returns nullopt if unmapped.
    [[nodiscard]] inline std::optional<std::pair<std::string_view, std::string_view>>
    opcode_wire_name(codegen::hl::hl_opcode op) noexcept {
        for (const auto& e : detail::opcode_wire_table)
            if (e.op == op) return std::pair{e.domain, e.name};
        return std::nullopt;
    }

    // wire_name_opcode: (domain, name) → live opcode. Returns nullopt if unmapped.
    [[nodiscard]] inline std::optional<codegen::hl::hl_opcode>
    wire_name_opcode(std::string_view domain, std::string_view name) noexcept {
        for (const auto& e : detail::opcode_wire_table)
            if (e.domain == domain && e.name == name) return e.op;
        return std::nullopt;
    }

    // =============================================================================
    // value_type_string — live type → stable canonical type_str
    //
    // Scalar grammar:  i8 | i16 | i32 | i64 | i1 | f16 | f32 | f64
    // Memref grammar:  memref<d0xd1x...xelem[,strides=[s0,s1,...]]>
    //   • Dynamic dimension: '?' (shape[i] == 0)
    //   • Stride suffix omitted when contiguous
    // =============================================================================

    [[nodiscard]] inline std::string
    value_type_string(codegen::abstract_value_kind kind, std::uint32_t bits) {
        using K = codegen::abstract_value_kind;
        switch (kind) {
        case K::predicate: return "i1";
        case K::integer:
            switch (bits) {
            case 8: return "i8";
            case 16: return "i16";
            case 32: return "i32";
            case 64: return "i64";
            default: return "i" + std::to_string(bits);
            }
        case K::floating:
            switch (bits) {
            case 16: return "f16";
            case 32: return "f32";
            case 64: return "f64";
            default: return "f" + std::to_string(bits);
            }
        default:
            return "opaque" + std::to_string(bits);
        }
    }

    [[nodiscard]] inline std::string
    memref_type_string(const codegen::hl::memref_type& m) {
        std::string s = "memref<";
        for (std::uint8_t i = 0; i < m.rank; ++i) {
            if (i > 0) s += 'x';
            if (m.shape[i] == 0) s += '?';
            else s += std::to_string(m.shape[i]);
        }
        s += 'x';
        s += value_type_string(m.elem_kind, m.elem_bits);
        if (!m.contiguous) {
            s += ",strides=[";
            for (std::uint8_t i = 0; i < m.rank; ++i) {
                if (i > 0) s += ',';
                s += std::to_string(m.strides[i]);
            }
            s += ']';
        }
        s += '>';
        return s;
    }

    // =============================================================================
    // freeze_function — live hl_mir_function → adapters::lithe_hl_mir_ir
    // =============================================================================

    [[nodiscard]] inline std::expected<adapters::lithe_hl_mir_ir, freeze_error>
    freeze_function(const codegen::hl::hl_mir_function& fn,
                    const freeze_options& opts = {}) {
        using namespace codegen::hl;
        using namespace adapters;

        // ssa_value_id.id is uint64_t (live); canonical wire ids are uint32_t.
        using live_val_id = std::uint64_t;
        using live_node_id = std::uint32_t; // block/region/op ids are uint32_t

        lithe_hl_mir_ir wire;
        wire.function_name = fn.name;
        wire.source_stage = stage::lowered;
        wire.schema = opts.schema_override_set
                          ? opts.schema_override
                          : schema_version{1, 0, 0};

        // -------------------------------------------------------------------------
        // Pass 1: canonical id assignment via structural walk
        // -------------------------------------------------------------------------

        std::unordered_map<live_val_id, std::uint32_t> value_id_map;
        std::unordered_map<live_node_id, std::uint32_t> block_id_map;
        std::unordered_map<live_node_id, std::uint32_t> region_id_map;

        std::uint32_t next_val_id = 0;
        std::uint32_t next_blk_id = 0;
        std::uint32_t next_reg_id = 0;

        struct walk_op {
            const hl_operation* op;
            std::uint32_t can_id;
        };
        struct walk_block {
            const hl_block* blk;
            std::uint32_t can_id;
        };
        struct walk_region {
            const hl_region* reg;
            std::uint32_t can_id;
        };

        std::vector<walk_region> ordered_regions;
        std::vector<walk_block> ordered_blocks;
        std::vector<walk_op> ordered_ops;

        // Iterative DFS: push body_region first
        std::vector<const hl_region*> region_stack;
        region_stack.push_back(&fn.body_region);

        while (!region_stack.empty()) {
            const hl_region* reg = region_stack.back();
            region_stack.pop_back();

            if (region_id_map.count(reg->id)) continue;
            const std::uint32_t can_reg = next_reg_id++;
            region_id_map[reg->id] = can_reg;
            ordered_regions.push_back({reg, can_reg});

            for (const hl_block* blk = reg->blocks.head; blk; blk = blk->list_node.next) {
                const std::uint32_t can_blk = next_blk_id++;
                block_id_map[blk->id] = can_blk;
                ordered_blocks.push_back({blk, can_blk});

                // Assign canonical ids to block arguments
                for (const codegen::ssa_value_id arg : blk->block_args) {
                    if (!value_id_map.count(arg.id))
                        value_id_map[arg.id] = next_val_id++;
                }

                for (const hl_operation* op = blk->ops.head; op; op = op->list_node.next) {
                    ordered_ops.push_back({op, static_cast<std::uint32_t>(ordered_ops.size())});

                    // Assign canonical ids to results
                    for (const codegen::ssa_value_id rid : op->results) {
                        if (!value_id_map.count(rid.id))
                            value_id_map[rid.id] = next_val_id++;
                    }

                    // Push child regions
                    for (const hl_region* child : op->regions)
                        if (child && !region_id_map.count(child->id))
                            region_stack.push_back(child);
                }
            }
        }

        // -------------------------------------------------------------------------
        // Pass 2: build wire values table
        // -------------------------------------------------------------------------
        wire.values.resize(next_val_id);
        for (const auto& [live_id, can_id] : value_id_map) {
            wire.values[can_id].id = can_id;
            wire.values[can_id].type_str = "i64"; // default; overridden below
        }

        // -------------------------------------------------------------------------
        // Pass 3: build wire ops
        // -------------------------------------------------------------------------
        wire.ops.reserve(ordered_ops.size());
        for (const auto& [op, can_op_id] : ordered_ops) {
            auto maybe_name = opcode_wire_name(op->op);
            if (!maybe_name) {
                return std::unexpected(freeze_error{
                    freeze_error::code::unmapped_opcode, op->id,
                    "opcode has no wire name"
                });
            }

            hl_wire_op wop;
            wop.id = can_op_id;
            wop.domain = std::string(maybe_name->first);
            wop.name = std::string(maybe_name->second);
            wop.block_id = 0; // back-filled in pass 4
            wop.region_id = 0; // back-filled in pass 4

            for (const codegen::ssa_value_id oid : op->operands) {
                const auto it = value_id_map.find(oid.id);
                if (it == value_id_map.end())
                    return std::unexpected(freeze_error{
                        freeze_error::code::dangling_operand, op->id,
                        "operand id not found in value map"
                    });
                wop.operand_ids.push_back(it->second);
            }
            for (const codegen::ssa_value_id rid : op->results) {
                const auto it = value_id_map.find(rid.id);
                if (it == value_id_map.end())
                    return std::unexpected(freeze_error{
                        freeze_error::code::dangling_operand, op->id,
                        "result id not found in value map"
                    });
                wop.result_ids.push_back(it->second);
            }

            // Translate attributes
            if (std::holds_alternative<structured_for_attr>(op->attr)) {
                const auto& fa = std::get<structured_for_attr>(op->attr);
                hl_wire_op::for_attr wfa;
                wfa.rank = fa.rank;
                wfa.is_parallel = fa.is_parallel;
                for (std::uint8_t i = 0; i < fa.rank; ++i) {
                    wfa.lower_bounds.push_back(fa.bounds[i].lower);
                    wfa.upper_bounds.push_back(fa.bounds[i].upper);
                    wfa.steps.push_back(fa.bounds[i].step);
                    wfa.tile_sizes.push_back(fa.tile[i]);
                }
                wop.structured_for = std::move(wfa);
            }
            else if (std::holds_alternative<memref_attr>(op->attr)) {
                const auto& ma = std::get<memref_attr>(op->attr);
                const auto& mv = ma.view;
                hl_wire_op::memref_desc wmd;
                wmd.rank = mv.rank;
                wmd.element_kind = value_type_string(mv.elem_kind, mv.elem_bits);
                wmd.elem_bits = static_cast<std::uint8_t>(
                    mv.elem_bits > 255u ? 255u : mv.elem_bits);
                for (std::uint8_t i = 0; i < mv.rank; ++i)
                    wmd.shape.push_back(static_cast<std::uint64_t>(mv.shape[i]));
                for (std::uint8_t i = 0; i < mv.rank; ++i)
                    wmd.strides.push_back(mv.strides[i]);
                wop.memref = std::move(wmd);
                // Override value type for load results
                if (!wop.result_ids.empty())
                    wire.values[wop.result_ids[0]].type_str =
                        value_type_string(mv.elem_kind, mv.elem_bits);
            }
            else if (std::holds_alternative<constant_attr>(op->attr)) {
                const auto& ca = std::get<constant_attr>(op->attr);
                wop.constant = hl_wire_op::constant_wire_attr{
                    .kind = static_cast<std::uint8_t>(ca.kind),
                    .integer = ca.integer,
                    .floating_point = ca.floating_point,
                    .boolean = ca.boolean};
            }
            else if (std::holds_alternative<branch_attr>(op->attr)) {
                const auto& ba = std::get<branch_attr>(op->attr);
                hl_wire_op::branch_wire_attr wba;
                const auto it = block_id_map.find(ba.target_block);
                wba.target_block_id = (it != block_id_map.end()) ? it->second : ba.target_block;
                wop.branch = wba;
            }
            else if (std::holds_alternative<branch_cond_attr>(op->attr)) {
                const auto& bca = std::get<branch_cond_attr>(op->attr);
                hl_wire_op::branch_cond_wire_attr wbca;
                const auto it1 = block_id_map.find(bca.true_block);
                const auto it2 = block_id_map.find(bca.false_block);
                wbca.true_block_id = (it1 != block_id_map.end()) ? it1->second : bca.true_block;
                wbca.false_block_id = (it2 != block_id_map.end()) ? it2->second : bca.false_block;
                wop.branch_cond = wbca;
            }
            else if (std::holds_alternative<compare_attr>(op->attr)) {
                const auto& ca = std::get<compare_attr>(op->attr);
                hl_wire_op::compare_wire_attr wca;
                wca.predicate_idx = static_cast<std::uint32_t>(ca.pred);
                wca.ordered = ca.ordered;
                wop.compare = wca;
            }
            else if (std::holds_alternative<guard_attr>(op->attr)) {
                const auto& ga = std::get<guard_attr>(op->attr);
                hl_wire_op::guard_wire_attr wga;
                wga.guard_kind_idx = static_cast<std::uint32_t>(ga.kind);
                wga.policy_idx = static_cast<std::uint32_t>(ga.policy);
                wga.diag_code_idx = ga.diag_code_idx;
                wga.source_span_idx = ga.source_span_idx;
                wop.guard = wga;
            }
            else if (std::holds_alternative<trap_attr>(op->attr)) {
                const auto& ta = std::get<trap_attr>(op->attr);
                hl_wire_op::trap_wire_attr wta;
                wta.trap_kind_idx = static_cast<std::uint32_t>(ta.kind);
                wta.diag_code_idx = ta.diag_code_idx;
                wop.trap = wta;
            }
            else if (std::holds_alternative<cleanup_attr>(op->attr)) {
                const auto& cla = std::get<cleanup_attr>(op->attr);
                hl_wire_op::cleanup_wire_attr wcla;
                wcla.cleanup_ids = cla.cleanup_ids;
                wop.cleanup = wcla;
            }
            else if (std::holds_alternative<tx_attr>(op->attr)) {
                const auto& txa = std::get<tx_attr>(op->attr);
                hl_wire_op::tx_wire_attr wtxa;
                wtxa.isolation_idx = static_cast<std::uint32_t>(txa.iso);
                wtxa.retry = txa.retry;
                wtxa.replay_idx = static_cast<std::uint32_t>(txa.replay);
                wtxa.conflict_idx = static_cast<std::uint32_t>(txa.conflict);
                wtxa.partial_idx = static_cast<std::uint32_t>(txa.partial);
                wtxa.durability_idx = static_cast<std::uint32_t>(txa.durability);
                wtxa.distribution_idx = txa.distribution_idx;
                wtxa.coordinator_idx = txa.coordinator_idx;
                wop.transaction = wtxa;
            }

            wire.ops.push_back(std::move(wop));
        }

        // -------------------------------------------------------------------------
        // Pass 4: build wire blocks (back-fill op block_id / region_id)
        // -------------------------------------------------------------------------
        wire.blocks.reserve(ordered_blocks.size());
        for (const auto& [blk, can_blk_id] : ordered_blocks) {
            hl_wire_block wb;
            wb.id = can_blk_id;

            for (const codegen::ssa_value_id arg : blk->block_args) {
                const auto it = value_id_map.find(arg.id);
                if (it != value_id_map.end()) wb.arg_ids.push_back(it->second);
            }

            const std::uint32_t can_reg_id =
                (blk->parent_region && region_id_map.count(blk->parent_region->id))
                    ? region_id_map.at(blk->parent_region->id)
                    : 0u;

            for (const hl_operation* op = blk->ops.head; op; op = op->list_node.next) {
                // Find canonical op id by scanning ordered_ops for matching pointer
                for (const auto& [wop_ptr, can_op_id] : ordered_ops) {
                    if (wop_ptr == op) {
                        wb.op_ids.push_back(can_op_id);
                        wire.ops[can_op_id].block_id = can_blk_id;
                        wire.ops[can_op_id].region_id = can_reg_id;
                        break;
                    }
                }
            }
            wire.blocks.push_back(std::move(wb));
        }

        // -------------------------------------------------------------------------
        // Pass 5: build wire regions
        // -------------------------------------------------------------------------
        wire.regions.reserve(ordered_regions.size());
        for (const auto& [reg, can_reg_id] : ordered_regions) {
            hl_wire_region wr;
            wr.id = can_reg_id;
            for (const hl_block* blk = reg->blocks.head; blk; blk = blk->list_node.next) {
                const auto it = block_id_map.find(blk->id);
                if (it != block_id_map.end()) wr.block_ids.push_back(it->second);
            }
            wire.regions.push_back(std::move(wr));
        }

        // Entry block: canonical id of body_region's first block
        wire.entry_block_ids.clear();
        if (fn.body_region.blocks.head) {
            const auto it = block_id_map.find(fn.body_region.blocks.head->id);
            if (it != block_id_map.end())
                wire.entry_block_ids.push_back(it->second);
        }

        return wire;
    }

    // =============================================================================
    // freeze_module — span<const hl_mir_function*> → portable_module
    // =============================================================================

    [[nodiscard]] inline std::expected<portable_module, freeze_error>
    freeze_module(std::span<const codegen::hl::hl_mir_function* const> fns,
                  const module_freeze_options& opts = {}) {
        portable_module mod;
        mod.imports = opts.imports;
        mod.exports = opts.exports;
        mod.globals = opts.globals;
        mod.constants = opts.constants;
        mod.declared_capabilities = opts.declared_capabilities;
        mod.manifest = opts.manifest;
        mod.schema = schema_version{1, 0, 0};

        for (const auto* fn : fns) {
            if (!fn)
                return std::unexpected(freeze_error{
                    freeze_error::code::arena_walk_failed, 0, "null function pointer"
                });
            auto result = freeze_function(*fn, opts.fn_opts);
            if (!result) return std::unexpected(result.error());
            mod.functions.push_back(std::move(*result));
        }
        return mod;
    }
} // namespace lithe::ir::portable
