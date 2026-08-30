#pragma once

// taranga/opcode_map.hpp — Wasm opcode → Lithe HL target, as data.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// The single table that says, for every Wasm instruction taranga lowers, WHAT it
// becomes in Lithe HL MIR. Keeping this as data (not a switch buried in the
// lowerer) is deliberate: it is the contract surface between the SSA builder and
// lower_hl, it is what the docs enumerate, and it is what a reader audits to see
// coverage. lower_hl.hpp NEVER hardcodes an opcode string — it asks this table.
//
// Each Wasm instruction resolves to one of three lowering strategies:
//   - direct:    emits exactly one HL op (wire name given). e.g. i32.add → "add".
//   - synthesize: no single HL op exists; lower_hl expands a fixed HL sequence
//                 (e.g. i32.clz, numeric conversions). The table records the
//                 *kind* of synthesis so the lowerer dispatches without string
//                 sniffing.
//   - structured: control constructs (block/loop/if/br/return/call) that the SSA
//                 builder turns into CFG edges + region ops, not a value op.
//
// The HL wire names here MUST exist in lithe's opcode_wire_table (freeze.hpp).
// We assert coverage indirectly: lower_hl resolves each name via
// lithe::ir::portable::wire_name_opcode and fails closed if a name is unknown, so
// a typo here surfaces as a lowering violation rather than a silent miscompile.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace taranga {

    // How a Wasm instruction becomes HL.
    enum class lower_strategy : std::uint8_t {
        direct,      // one HL value op, wire name in hl_name
        synthesize,  // multi-op expansion; see synth_kind
        structured,  // CFG/region construct handled by ssa_build
        constant,    // *.const — materialises an HL constant
        memory,      // *.load / *.store — memref op + address synthesis
        variable,    // local/global get/set — SSA read/write or memref-backed global
    };

    // For synthesize strategy: which fixed expansion the lowerer performs.
    // (Named so lower_hl dispatches on an enum, never on the opcode text.)
    enum class synth_kind : std::uint8_t {
        none = 0,
        clz, ctz, popcnt,            // bit counting — loop/table expansion
        rotl, rotr,                  // rotate — shl/lshr/or compose
        eqz,                         // x == 0 → icmp eq with zero constant
        wrap,                        // i64→i32 truncate
        extend_s, extend_u,          // i32→i64 sign/zero extend
        trunc_f2i, convert_i2f,      // float/int conversions
        reinterpret,                 // bit-cast between same-width int/float
        promote, demote,             // f32<->f64
        copysign, nearest, ceil, floor, trunc_f, // float rounding/sign
        min_f, max_f,                // float min/max (NaN-propagating)
        // integer compare families that map to icmp with a specific predicate:
        cmp_eq, cmp_ne,
        cmp_lt_s, cmp_lt_u, cmp_gt_s, cmp_gt_u,
        cmp_le_s, cmp_le_u, cmp_ge_s, cmp_ge_u,
        // float compare families → fcmp:
        fcmp_eq, fcmp_ne, fcmp_lt, fcmp_gt, fcmp_le, fcmp_ge,
    };

    // A single row: the Wasm mnemonic (keyword after the type prefix is folded in),
    // the strategy, and the HL wire name (direct only) or synth kind (synthesize).
    struct opcode_entry {
        std::string_view wasm;       // full WAT mnemonic, e.g. "i32.add", "f64.sqrt"
        lower_strategy   strategy;
        std::string_view hl_name;    // valid iff strategy == direct
        synth_kind       synth;      // valid iff strategy == synthesize
    };

    namespace detail {

        // Compact constructors for readability.
        [[nodiscard]] constexpr opcode_entry D(std::string_view w, std::string_view hl) noexcept {
            return {w, lower_strategy::direct, hl, synth_kind::none};
        }
        [[nodiscard]] constexpr opcode_entry S(std::string_view w, synth_kind k) noexcept {
            return {w, lower_strategy::synthesize, {}, k};
        }
        [[nodiscard]] constexpr opcode_entry C(std::string_view w) noexcept {
            return {w, lower_strategy::constant, {}, synth_kind::none};
        }
        [[nodiscard]] constexpr opcode_entry M(std::string_view w) noexcept {
            return {w, lower_strategy::memory, {}, synth_kind::none};
        }
        [[nodiscard]] constexpr opcode_entry V(std::string_view w) noexcept {
            return {w, lower_strategy::variable, {}, synth_kind::none};
        }
        [[nodiscard]] constexpr opcode_entry X(std::string_view w) noexcept {
            return {w, lower_strategy::structured, {}, synth_kind::none};
        }

        // The opcode table. Direct rows reuse HL wire names verified against
        // freeze.hpp's opcode_wire_table. Integer arithmetic that Wasm defines as
        // signedness-agnostic (add/sub/mul) maps to HL add/sub/mul; div/rem, which
        // Wasm splits by signedness, map to sdiv/udiv/srem/urem directly.
        inline constexpr std::array opcode_table = std::to_array<opcode_entry>({
            // ---- constants ------------------------------------------------------
            C("i32.const"), C("i64.const"), C("f32.const"), C("f64.const"),

            // ---- integer arithmetic (i32) --------------------------------------
            D("i32.add", "add"), D("i32.sub", "sub"), D("i32.mul", "mul"),
            D("i32.div_s", "sdiv"), D("i32.div_u", "udiv"),
            D("i32.rem_s", "srem"), D("i32.rem_u", "urem"),
            D("i32.and", "bit_and"), D("i32.or", "bit_or"), D("i32.xor", "bit_xor"),
            D("i32.shl", "shl"), D("i32.shr_s", "ashr"), D("i32.shr_u", "lshr"),
            S("i32.clz", synth_kind::clz), S("i32.ctz", synth_kind::ctz),
            S("i32.popcnt", synth_kind::popcnt),
            S("i32.rotl", synth_kind::rotl), S("i32.rotr", synth_kind::rotr),
            S("i32.eqz", synth_kind::eqz),

            // ---- integer arithmetic (i64) --------------------------------------
            D("i64.add", "add"), D("i64.sub", "sub"), D("i64.mul", "mul"),
            D("i64.div_s", "sdiv"), D("i64.div_u", "udiv"),
            D("i64.rem_s", "srem"), D("i64.rem_u", "urem"),
            D("i64.and", "bit_and"), D("i64.or", "bit_or"), D("i64.xor", "bit_xor"),
            D("i64.shl", "shl"), D("i64.shr_s", "ashr"), D("i64.shr_u", "lshr"),
            S("i64.clz", synth_kind::clz), S("i64.ctz", synth_kind::ctz),
            S("i64.popcnt", synth_kind::popcnt),
            S("i64.rotl", synth_kind::rotl), S("i64.rotr", synth_kind::rotr),
            S("i64.eqz", synth_kind::eqz),

            // ---- integer comparisons (i32) → icmp predicates -------------------
            S("i32.eq", synth_kind::cmp_eq), S("i32.ne", synth_kind::cmp_ne),
            S("i32.lt_s", synth_kind::cmp_lt_s), S("i32.lt_u", synth_kind::cmp_lt_u),
            S("i32.gt_s", synth_kind::cmp_gt_s), S("i32.gt_u", synth_kind::cmp_gt_u),
            S("i32.le_s", synth_kind::cmp_le_s), S("i32.le_u", synth_kind::cmp_le_u),
            S("i32.ge_s", synth_kind::cmp_ge_s), S("i32.ge_u", synth_kind::cmp_ge_u),

            // ---- integer comparisons (i64) -------------------------------------
            S("i64.eq", synth_kind::cmp_eq), S("i64.ne", synth_kind::cmp_ne),
            S("i64.lt_s", synth_kind::cmp_lt_s), S("i64.lt_u", synth_kind::cmp_lt_u),
            S("i64.gt_s", synth_kind::cmp_gt_s), S("i64.gt_u", synth_kind::cmp_gt_u),
            S("i64.le_s", synth_kind::cmp_le_s), S("i64.le_u", synth_kind::cmp_le_u),
            S("i64.ge_s", synth_kind::cmp_ge_s), S("i64.ge_u", synth_kind::cmp_ge_u),

            // ---- float arithmetic (f32) ----------------------------------------
            D("f32.add", "fadd"), D("f32.sub", "fsub"), D("f32.mul", "fmul"),
            D("f32.div", "fdiv"), D("f32.neg", "fneg"),
            D("f32.abs", "abs"), D("f32.sqrt", "sqrt"),
            S("f32.min", synth_kind::min_f), S("f32.max", synth_kind::max_f),
            S("f32.ceil", synth_kind::ceil), S("f32.floor", synth_kind::floor),
            S("f32.trunc", synth_kind::trunc_f), S("f32.nearest", synth_kind::nearest),
            S("f32.copysign", synth_kind::copysign),

            // ---- float arithmetic (f64) ----------------------------------------
            D("f64.add", "fadd"), D("f64.sub", "fsub"), D("f64.mul", "fmul"),
            D("f64.div", "fdiv"), D("f64.neg", "fneg"),
            D("f64.abs", "abs"), D("f64.sqrt", "sqrt"),
            S("f64.min", synth_kind::min_f), S("f64.max", synth_kind::max_f),
            S("f64.ceil", synth_kind::ceil), S("f64.floor", synth_kind::floor),
            S("f64.trunc", synth_kind::trunc_f), S("f64.nearest", synth_kind::nearest),
            S("f64.copysign", synth_kind::copysign),

            // ---- float comparisons → fcmp --------------------------------------
            S("f32.eq", synth_kind::fcmp_eq), S("f32.ne", synth_kind::fcmp_ne),
            S("f32.lt", synth_kind::fcmp_lt), S("f32.gt", synth_kind::fcmp_gt),
            S("f32.le", synth_kind::fcmp_le), S("f32.ge", synth_kind::fcmp_ge),
            S("f64.eq", synth_kind::fcmp_eq), S("f64.ne", synth_kind::fcmp_ne),
            S("f64.lt", synth_kind::fcmp_lt), S("f64.gt", synth_kind::fcmp_gt),
            S("f64.le", synth_kind::fcmp_le), S("f64.ge", synth_kind::fcmp_ge),

            // ---- conversions (all synthesized — no HL cast op) -----------------
            S("i32.wrap_i64", synth_kind::wrap),
            S("i64.extend_i32_s", synth_kind::extend_s),
            S("i64.extend_i32_u", synth_kind::extend_u),
            S("f32.demote_f64", synth_kind::demote),
            S("f64.promote_f32", synth_kind::promote),
            S("i32.trunc_f32_s", synth_kind::trunc_f2i),
            S("i32.trunc_f64_s", synth_kind::trunc_f2i),
            S("i64.trunc_f32_s", synth_kind::trunc_f2i),
            S("i64.trunc_f64_s", synth_kind::trunc_f2i),
            S("f32.convert_i32_s", synth_kind::convert_i2f),
            S("f64.convert_i32_s", synth_kind::convert_i2f),
            S("f32.convert_i64_s", synth_kind::convert_i2f),
            S("f64.convert_i64_s", synth_kind::convert_i2f),
            S("i32.reinterpret_f32", synth_kind::reinterpret),
            S("i64.reinterpret_f64", synth_kind::reinterpret),
            S("f32.reinterpret_i32", synth_kind::reinterpret),
            S("f64.reinterpret_i64", synth_kind::reinterpret),

            // ---- memory --------------------------------------------------------
            M("i32.load"), M("i64.load"), M("f32.load"), M("f64.load"),
            M("i32.store"), M("i64.store"), M("f32.store"), M("f64.store"),

            // ---- variables -----------------------------------------------------
            V("local.get"), V("local.set"), V("local.tee"),
            V("global.get"), V("global.set"),

            // ---- structured control -------------------------------------------
            X("block"), X("loop"), X("if"), X("else"), X("end"),
            X("br"), X("br_if"), X("return"), X("call"), X("call_indirect"),
            X("select"), X("drop"), X("nop"), X("unreachable"),
        });

    } // namespace detail

    // Look up an entry by full WAT mnemonic. nullopt = unsupported opcode (v1).
    [[nodiscard]] inline std::optional<opcode_entry>
    lookup_opcode(std::string_view wasm) noexcept {
        for (const auto& e : detail::opcode_table)
            if (e.wasm == wasm) return e;
        return std::nullopt;
    }

    // Convenience: is this mnemonic something taranga lowers at all?
    [[nodiscard]] inline bool is_supported_opcode(std::string_view wasm) noexcept {
        return lookup_opcode(wasm).has_value();
    }

} // namespace taranga
