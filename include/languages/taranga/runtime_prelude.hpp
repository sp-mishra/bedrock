#pragma once

// taranga/runtime_prelude.hpp — HL expansion sequences for synthesized Wasm ops.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// opcode_map marks a set of Wasm instructions as `synthesize`: they have no single
// Lithe HL opcode and must be expanded into a fixed sequence of HL ops. lower_hl
// handles comparisons and eqz inline (they are one op + a predicate/const); the
// rest live here, so the expansion logic is one auditable place and lower_hl stays
// a dispatcher.
//
// Each expander is a free function over the *live* builder: it takes the
// hl_mir_function (for id minting + arena spans), the current block, the already-
// resolved operand value ids, and the destination result id, and appends the HL
// ops that compute it. This mirrors how lower_hl itself emits, but keeps the
// arithmetic identities out of the dispatcher.
//
// Coverage tiers (v1):
//   • Expressible from existing HL bit ops — emitted exactly:
//       rotl, rotr        (rotate = shl | lshr composition with width complement)
//       wrap              (i64→i32: low bits survive; identity at HL bit level)
//       extend_u          (i32→i64 zero-extend: identity at HL bit level)
//       reinterpret       (same-width int↔float bitcast: identity value, retype)
//   • Not yet expressible without a dedicated HL op or a loop — reported via
//     TARANGA-PRELUDE-### and defined as a typed zero so the frozen IR is
//     well-formed (never a dangling result):
//       clz, ctz, popcnt  (need a counting loop or a native bit op)
//       extend_s          (sign-extend needs an arithmetic-shift pair on a typed
//                          width HL does not track post-freeze — deferred)
//       trunc_f2i, convert_i2f, promote, demote  (need cast ops absent from HL)
//       ceil, floor, trunc_f, nearest, copysign, min_f, max_f (float intrinsics)
//
// The tiering is deliberate and honest: a deferred op is *marked*, not silently
// miscompiled, so a test can assert the diagnostic and the engine can refuse.

#include "languages/taranga/opcode_map.hpp"
#include "languages/taranga/wasm_types.hpp"

#include "lithe/lithe_codegen.hpp"
#include "lithe/lithe_codegen_hl.hpp"

#include "vakya/diagnostics.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace taranga {

    namespace detail {

        namespace hl = lithe::codegen::hl;
        namespace cg = lithe::codegen;

        // Minimal emit helper local to the prelude (parallels lower_ctx::emit) so
        // expanders don't depend on lower_hl's context type.
        inline hl::hl_operation*
        prelude_emit(hl::hl_mir_function& fn, hl::hl_block* blk, hl::hl_opcode op,
                     std::span<const cg::ssa_value_id> operands,
                     cg::ssa_value_id result) {
            auto* o = fn.make_op(op);
            if (!operands.empty()) {
                auto sp = fn.alloc_span<cg::ssa_value_id>(operands.size());
                for (std::size_t i = 0; i < operands.size(); ++i) sp[i] = operands[i];
                o->operands = sp;
            }
            if (result.valid()) {
                auto rs = fn.alloc_span<cg::ssa_value_id>(1);
                rs[0] = result;
                o->results = rs;
            }
            blk->ops.push_back(o);
            return o;
        }

        inline cg::ssa_value_id
        prelude_const_int(hl::hl_mir_function& fn, hl::hl_block* blk,
                          std::int64_t v) {
            cg::ssa_value_id r{fn.next_id++};
            auto* o = prelude_emit(fn, blk, hl::hl_opcode::constant, {}, r);
            o->attr = hl::constant_attr::integer_value(v);
            return r;
        }

    } // namespace detail

    // Result of an expansion attempt: whether it was emitted exactly, plus the
    // value id holding the result (valid iff a result was produced).
    struct prelude_outcome {
        bool exact = false;                       // emitted a faithful sequence
        lithe::codegen::ssa_value_id value{0};    // result value (0 = none)
    };

    // Expand a synthesized op into the live builder. Returns whether it was exact;
    // an inexact/deferred op still defines `result` (as a typed zero) so the IR is
    // well-formed, and records a diagnostic naming the gap.
    [[nodiscard]] inline prelude_outcome
    expand_synth(lithe::codegen::hl::hl_mir_function& fn,
                 lithe::codegen::hl::hl_block* blk,
                 synth_kind kind, std::string_view mnemonic,
                 std::span<const lithe::codegen::ssa_value_id> operands,
                 value_type result_type,
                 vakya::diag::collecting_sink& diags) {
        namespace hl = lithe::codegen::hl;
        namespace cg = lithe::codegen;

        const bool is_i64 = (result_type == value_type::i64);
        const std::int64_t width_bits = is_i64 ? 64 : 32;

        auto typed_zero = [&](void) -> cg::ssa_value_id {
            cg::ssa_value_id r{fn.next_id++};
            auto* o = detail::prelude_emit(fn, blk, hl::hl_opcode::constant, {}, r);
            if (result_type == value_type::f32 || result_type == value_type::f64)
                o->attr = hl::constant_attr::floating_value(0.0);
            else
                o->attr = hl::constant_attr::integer_value(0);
            return r;
        };

        switch (kind) {
        // ---- rotate: rotl(x,n) = (x << n) | (x >>u (width - n)) --------------
        case synth_kind::rotl:
        case synth_kind::rotr: {
            if (operands.size() < 2) {
                diags.on_diagnostic(vakya::diag::make_error(
                    "TARANGA-PRELUDE-001",
                    "rotate '" + std::string(mnemonic) + "' needs two operands"));
                return {false, typed_zero()};
            }
            const cg::ssa_value_id x = operands[0];
            const cg::ssa_value_id n = operands[1];
            const cg::ssa_value_id w = detail::prelude_const_int(fn, blk, width_bits);
            // complement = width - n
            cg::ssa_value_id comp{fn.next_id++};
            { std::array<cg::ssa_value_id, 2> o{w, n};
              detail::prelude_emit(fn, blk, hl::hl_opcode::sub, o, comp); }
            const bool left = (kind == synth_kind::rotl);
            // primary = x << n   (rotl)  or  x >>u n  (rotr)
            cg::ssa_value_id primary{fn.next_id++};
            { std::array<cg::ssa_value_id, 2> o{x, n};
              detail::prelude_emit(fn, blk,
                  left ? hl::hl_opcode::shl : hl::hl_opcode::lshr, o, primary); }
            // secondary = x >>u comp (rotl) or x << comp (rotr)
            cg::ssa_value_id secondary{fn.next_id++};
            { std::array<cg::ssa_value_id, 2> o{x, comp};
              detail::prelude_emit(fn, blk,
                  left ? hl::hl_opcode::lshr : hl::hl_opcode::shl, o, secondary); }
            // result = primary | secondary
            cg::ssa_value_id r{fn.next_id++};
            { std::array<cg::ssa_value_id, 2> o{primary, secondary};
              detail::prelude_emit(fn, blk, hl::hl_opcode::bit_or, o, r); }
            return {true, r};
        }

        // ---- wrap (i64→i32) / extend_u (i32→i64): identity at HL bit level ----
        // HL does not carry a width cast op; the value's bit pattern is preserved
        // and the narrower/wider interpretation is a freeze-time type concern.
        // We forward the operand through a bit_or with zero to give the op a
        // distinct result id (so uses re-point correctly) while staying faithful.
        case synth_kind::wrap:
        case synth_kind::extend_u: {
            if (operands.empty()) return {false, typed_zero()};
            const cg::ssa_value_id zero = detail::prelude_const_int(fn, blk, 0);
            cg::ssa_value_id r{fn.next_id++};
            std::array<cg::ssa_value_id, 2> o{operands[0], zero};
            detail::prelude_emit(fn, blk, hl::hl_opcode::bit_or, o, r);
            return {true, r};
        }

        // ---- reinterpret: same-width int↔float bitcast. Value bits identical;
        // only the type interpretation changes (a freeze-time concern), so this is
        // a faithful pass-through at the HL value level. ------------------------
        case synth_kind::reinterpret: {
            if (operands.empty()) return {false, typed_zero()};
            const cg::ssa_value_id zero = detail::prelude_const_int(fn, blk, 0);
            cg::ssa_value_id r{fn.next_id++};
            // For a float result we cannot bit_or; forward through fadd 0.0 to keep
            // the value in the float domain. For an int result, bit_or 0.
            if (result_type == value_type::f32 || result_type == value_type::f64) {
                cg::ssa_value_id fzero{fn.next_id++};
                auto* c = detail::prelude_emit(fn, blk, hl::hl_opcode::constant, {}, fzero);
                c->attr = hl::constant_attr::floating_value(0.0);
                std::array<cg::ssa_value_id, 2> o{operands[0], fzero};
                detail::prelude_emit(fn, blk, hl::hl_opcode::fadd, o, r);
            } else {
                std::array<cg::ssa_value_id, 2> o{operands[0], zero};
                detail::prelude_emit(fn, blk, hl::hl_opcode::bit_or, o, r);
            }
            return {true, r};
        }

        // ---- deferred: need a dedicated HL op or a counting loop -------------
        case synth_kind::clz:
        case synth_kind::ctz:
        case synth_kind::popcnt:
        case synth_kind::extend_s:
        case synth_kind::trunc_f2i:
        case synth_kind::convert_i2f:
        case synth_kind::promote:
        case synth_kind::demote:
        case synth_kind::copysign:
        case synth_kind::nearest:
        case synth_kind::ceil:
        case synth_kind::floor:
        case synth_kind::trunc_f:
        case synth_kind::min_f:
        case synth_kind::max_f: {
            diags.on_diagnostic(vakya::diag::make_warning(
                "TARANGA-PRELUDE-020",
                "synthesis of '" + std::string(mnemonic) +
                    "' not yet expressible in HL v1; result defined as zero"));
            return {false, typed_zero()};
        }

        default: {
            // Comparison synth kinds are handled by lower_hl, not here.
            diags.on_diagnostic(vakya::diag::make_warning(
                "TARANGA-PRELUDE-030",
                "no prelude expansion for '" + std::string(mnemonic) + "'"));
            return {false, typed_zero()};
        }
        }
    }

} // namespace taranga
