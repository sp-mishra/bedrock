#pragma once

// taranga/ssa_build.hpp — Wasm stack machine → taranga SSA value graph.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// Wasm is a stack machine: instructions pop operands from and push results to an
// implicit value stack. Lithe HL MIR is an SSA value graph. This band bridges the
// two by *abstract stack execution*: we walk each function body maintaining a
// compile-time value stack of ssa_value_ids, and every instruction pops its arity
// and pushes its result id. The result is a per-function ssa_function — a flat op
// list in program order with explicit operand/result value-ids — that lower_hl
// turns into HL ops without re-deriving dataflow.
//
// Two body encodings converge here (as everywhere in taranga):
//   - WAT folded form: build_ast already nested operands as child instr nodes, so
//     a post-order walk yields operands before their consumer for free. This is
//     the path exercised in v1 and is fully implemented below.
//   - WAT flat form / binary byte stream: a linear opcode sequence driving the
//     stack directly. build_from_binary stores the raw body bytes; decoding that
//     stream into ops is a byte-walk over opcode_map — recorded as a TODO-scoped
//     diagnostic here (TARANGA-SSA-090) so a binary function body is reported, not
//     silently miscompiled. The folded WAT path is the reference semantics.
//
// SSA form here is pre-CFG: straight-line functions get one block; structured
// control (block/loop/if) is recorded as boundary ops (region_enter/region_exit)
// that lower_hl expands into Lithe regions. Locals are modelled as mutable slots
// whose *current* SSA id is tracked in a side table (local_defs) — a local.get
// reads it, local.set/tee rewrites it. This is the classic "locals are not SSA;
// their reads/writes are" model; lower_hl introduces block args where a slot is
// live across a control edge.

#include "languages/taranga/build_ast.hpp"
#include "languages/taranga/module_view.hpp"
#include "languages/taranga/opcode_map.hpp"
#include "languages/taranga/source_span.hpp"
#include "languages/taranga/wasm_types.hpp"

#include "vakya/diagnostics.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace taranga {

    using ssa_value_id = std::uint32_t;
    inline constexpr ssa_value_id k_null_value = 0xFFFF'FFFFu;

    // One SSA operation — the taranga-side intermediate lower_hl consumes.
    struct ssa_op {
        std::string      mnemonic;                 // full WAT mnemonic ("i32.add")
        lower_strategy   strategy = lower_strategy::direct;
        synth_kind       synth = synth_kind::none;
        std::vector<ssa_value_id> operands;        // popped, in Wasm stack order
        ssa_value_id     result = k_null_value;    // pushed (k_null_value if none)
        value_type       result_type = value_type::i32;
        std::uint64_t    imm_value = 0;            // const payload / index
        std::uint32_t    imm_index = 0;            // local/global/func/type index
        bool             has_imm_value = false;
        byte_span        span{};
    };

    // A basic block: ops in program order + structured successors (filled by
    // lower_hl for control ops; ssa_build leaves straight-line blocks terminated
    // implicitly). v1 emits a single block per function for straight-line bodies.
    struct ssa_block {
        std::vector<ssa_op> ops;
    };

    struct ssa_function {
        std::string name;
        std::vector<value_type> params;   // in order
        std::vector<value_type> locals;   // declared locals (after params)
        std::vector<value_type> results;
        std::vector<ssa_block> blocks;    // v1: one block
        std::uint32_t value_count = 0;    // number of SSA ids minted
    };

    struct ssa_result {
        std::vector<ssa_function> functions;
        vakya::diag::collecting_sink diagnostics;
        [[nodiscard]] bool ok() const noexcept { return !diagnostics.has_errors(); }
    };

    namespace detail {

        // Per-function abstract-execution state.
        struct ssa_state {
            ssa_function fn;
            std::vector<ssa_value_id> local_defs; // current SSA id per local slot
            ssa_value_id next_id = 0;

            ssa_value_id fresh() { return next_id++; }
        };

        // Arity of a value op = number of stack operands it pops. For folded WAT
        // the operands are explicit child nodes, so we derive arity from children
        // count rather than a per-opcode table — the tree already encoded it.
        // (Immediates are folded into ext, not children, so child count is exactly
        // the operand arity.)

        // Result value_type of an instruction, from its mnemonic prefix + kind.
        [[nodiscard]] inline value_type
        result_type_of(std::string_view mnemonic, const opcode_entry& e,
                       value_type fallback) {
            // Comparisons and eqz always yield i32 (Wasm bool).
            switch (e.synth) {
            case synth_kind::eqz:
            case synth_kind::cmp_eq: case synth_kind::cmp_ne:
            case synth_kind::cmp_lt_s: case synth_kind::cmp_lt_u:
            case synth_kind::cmp_gt_s: case synth_kind::cmp_gt_u:
            case synth_kind::cmp_le_s: case synth_kind::cmp_le_u:
            case synth_kind::cmp_ge_s: case synth_kind::cmp_ge_u:
            case synth_kind::fcmp_eq: case synth_kind::fcmp_ne:
            case synth_kind::fcmp_lt: case synth_kind::fcmp_gt:
            case synth_kind::fcmp_le: case synth_kind::fcmp_ge:
                return value_type::i32;
            default: break;
            }
            // Conversions name their result type in the mnemonic prefix.
            auto dot = mnemonic.find('.');
            if (dot != std::string_view::npos)
                if (auto vt = value_type_from_spelling(mnemonic.substr(0, dot)))
                    return *vt;
            return fallback;
        }

        // Recursively lower a folded WAT instruction node, pushing ops in
        // post-order (operands first). Returns the SSA id the node produces, or
        // k_null_value for a void op (store, return, drop).
        inline ssa_value_id
        lower_instr_node(const module_view& view, lang::ir_node_id nid,
                         ssa_state& st, ssa_block& blk,
                         vakya::diag::collecting_sink& diags) {
            const auto& node = view.node(nid);
            const std::string& mnemonic = node.ext.head;

            // Structured control is not a value op — recurse into its children as
            // nested straight-line for v1, recording a boundary op. lower_hl turns
            // the boundary into a region; here we keep dataflow linear.
            auto entry = lookup_opcode(mnemonic);
            if (!entry) {
                diags.on_diagnostic(make_error(
                    "TARANGA-SSA-001", "unsupported instruction '" + mnemonic + "'"));
                return k_null_value;
            }

            // Evaluate operand children first (post-order = operands before op).
            std::vector<ssa_value_id> operand_ids;
            for (auto child : view.children(nid)) {
                // Only instr-bearing children are operands; names/immediates are ext.
                const auto ck = view.kind_of(child);
                if (ck == taranga_kind::name) continue;
                operand_ids.push_back(lower_instr_node(view, child, st, blk, diags));
            }

            ssa_op op;
            op.mnemonic = mnemonic;
            op.strategy = entry->strategy;
            op.synth = entry->synth;
            op.operands = operand_ids;
            op.span = node.span;

            switch (entry->strategy) {
            case lower_strategy::constant: {
                op.has_imm_value = node.ext.has_value;
                op.imm_value = node.ext.value;
                op.result_type = node.ext.has_vtype ? node.ext.vtype : value_type::i32;
                op.result = st.fresh();
                break;
            }
            case lower_strategy::variable: {
                op.imm_index = node.ext.immediate;
                if (mnemonic == "local.get") {
                    // Read the current SSA id of the slot (no new op result needed,
                    // but we still record the op for lower_hl's slot tracking).
                    const std::uint32_t slot = node.ext.immediate;
                    if (slot < st.local_defs.size() &&
                        st.local_defs[slot] != k_null_value) {
                        op.result = st.local_defs[slot];
                    } else {
                        op.result = st.fresh(); // undefined-read → fresh (validator catches)
                    }
                    op.result_type = slot < st.fn.params.size()
                        ? st.fn.params[slot]
                        : (slot - st.fn.params.size() < st.fn.locals.size()
                               ? st.fn.locals[slot - st.fn.params.size()]
                               : value_type::i32);
                } else if (mnemonic == "local.set" || mnemonic == "local.tee") {
                    const std::uint32_t slot = node.ext.immediate;
                    const ssa_value_id v = operand_ids.empty() ? k_null_value
                                                               : operand_ids.back();
                    if (slot < st.local_defs.size()) st.local_defs[slot] = v;
                    op.result = (mnemonic == "local.tee") ? v : k_null_value;
                } else { // global.get / global.set
                    op.result = (mnemonic == "global.get") ? st.fresh() : k_null_value;
                }
                break;
            }
            case lower_strategy::memory: {
                op.imm_index = node.ext.immediate;   // align
                op.imm_value = node.ext.immediate2;  // offset
                if (mnemonic.find(".load") != std::string_view::npos) {
                    auto dot = mnemonic.find('.');
                    op.result_type = (dot != std::string_view::npos)
                        ? value_type_from_spelling(mnemonic.substr(0, dot))
                              .value_or(value_type::i32)
                        : value_type::i32;
                    op.result = st.fresh();
                } else {
                    op.result = k_null_value; // store
                }
                break;
            }
            case lower_strategy::structured: {
                // Boundary op — no value result for v1 straight-line lowering.
                op.result = k_null_value;
                break;
            }
            case lower_strategy::direct:
            case lower_strategy::synthesize: {
                // Value-producing arithmetic/compare. Result type from prefix.
                value_type fallback = value_type::i32;
                if (!operand_ids.empty()) {
                    // Infer fallback from the mnemonic prefix (i32/i64/f32/f64).
                    auto dot = mnemonic.find('.');
                    if (dot != std::string_view::npos)
                        if (auto vt = value_type_from_spelling(mnemonic.substr(0, dot)))
                            fallback = *vt;
                }
                op.result_type = result_type_of(mnemonic, *entry, fallback);
                op.result = st.fresh();
                break;
            }
            }

            blk.ops.push_back(std::move(op));
            return blk.ops.back().result;
        }

    } // namespace detail

    // Build SSA for every defined function in a validated module.
    [[nodiscard]] inline ssa_result build_ssa(const module_view& view) {
        ssa_result out;

        auto funcs = view.functions();
        for (std::uint32_t fi = 0; fi < funcs.size(); ++fi) {
            const auto fnode = funcs[fi];
            detail::ssa_state st;
            st.fn.name = view.node(fnode).ext.text;
            if (st.fn.name.empty()) st.fn.name = "func" + std::to_string(fi);

            // Signature from the recorded type.
            if (const auto* sig = view.signature_of(fnode)) {
                st.fn.params = sig->params;
                st.fn.results = sig->results;
            }
            // Declared locals are the func node's local children.
            for (auto c : view.children(fnode))
                if (view.kind_of(c) == taranga_kind::local)
                    st.fn.locals.push_back(view.node(c).ext.vtype);

            // Local slots = params ++ locals; init to null (undefined).
            st.local_defs.assign(st.fn.params.size() + st.fn.locals.size(),
                                 k_null_value);
            // Params get their own SSA ids up front (argument values).
            for (std::uint32_t p = 0; p < st.fn.params.size(); ++p)
                st.local_defs[p] = st.fresh();

            ssa_block block;
            const auto body = view.body_of(fnode);
            if (body == lang::k_null_ir) {
                // No structured body (e.g. binary raw-byte body): report, skip.
                const auto& fn = view.node(fnode);
                if (fn.ext.has_value && !fn.ext.text.empty()) {
                    out.diagnostics.on_diagnostic(make_warning(
                        "TARANGA-SSA-090",
                        "binary function body byte-decode not yet implemented for '" +
                            st.fn.name + "' (v1 lowers WAT folded form)"));
                }
            } else {
                for (auto instr : view.children(body))
                    detail::lower_instr_node(view, instr, st, block, out.diagnostics);
            }
            st.fn.blocks.push_back(std::move(block));
            st.fn.value_count = st.next_id;
            out.functions.push_back(std::move(st.fn));
        }
        return out;
    }

} // namespace taranga
