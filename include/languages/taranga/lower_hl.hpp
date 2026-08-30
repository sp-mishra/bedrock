#pragma once

// taranga/lower_hl.hpp — taranga SSA → live Lithe HL MIR (codegen::hl).
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// This is the band where Wasm semantics become Lithe. ssa_build produced a
// taranga-owned SSA op list per function (dataflow explicit, control linear);
// here each op is *emitted into a live hl_mir_function* — the same builder crank
// lowers into — so the result is ready for lithe::ir::portable::freeze_function.
// Freezing itself is deliberately NOT done here (it is the engine's job, mirroring
// crank::lower_to_hl which also stops at the live builder): keeping lowering and
// freezing separate lets a caller inspect / verify the live IR first.
//
// Opcode routing is table-driven end to end. A *direct* Wasm op names an HL wire
// op in opcode_map; we resolve that name to a live hl_opcode via lithe's
// wire_name_opcode("lithe.hl", name) and fail closed (TARANGA-LOWER-010) if the
// name is unknown — so a typo in opcode_map surfaces here, never as a silent
// miscompile. A *synthesize* op has no single HL op: comparisons lower to
// icmp/fcmp + a compare_attr predicate (fully implemented); the remaining
// expansions (clz/ctz/popcnt/rotl/rotr, casts, float rounding) are recorded as
// TARANGA-LOWER-020 "expansion not yet emitted" and materialise a typed zero so
// the frozen IR stays well-formed and verifiable rather than dangling. runtime
// synthesis of those sequences is the runtime_prelude band's remit.
//
// Value mapping: a taranga ssa_value_id indexes into a per-function vector of live
// codegen::ssa_value_id (minted from fn.next_id). Params are seeded as `argument`
// ops up front so a local.get of a parameter resolves to a real HL value.

#include "languages/taranga/opcode_map.hpp"
#include "languages/taranga/runtime_prelude.hpp"
#include "languages/taranga/ssa_build.hpp"
#include "languages/taranga/validate.hpp"
#include "languages/taranga/wasm_types.hpp"

#include "lithe/lithe_codegen.hpp"        // codegen::ssa_value_id, abstract_value_kind
#include "lithe/lithe_codegen_hl.hpp"
#include "lithe/lithe_ir/portable/freeze.hpp"

#include "vakya/diagnostics.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace taranga {

    // A function lowered to live HL MIR, plus the diagnostics its lowering raised.
    struct lowered_function {
        lithe::codegen::hl::hl_mir_function hl_fn;   // move-only; owns its arena
        std::string name;
    };

    struct lower_result {
        std::vector<lowered_function> functions;
        vakya::diag::collecting_sink diagnostics;
        [[nodiscard]] bool ok() const noexcept { return !diagnostics.has_errors(); }
    };

    namespace detail {

        namespace hl = lithe::codegen::hl;
        namespace cg = lithe::codegen;   // ssa_value_id, abstract_value_kind live here

        // Map a Wasm value_type to lithe's (kind, bits) for constant materialisation.
        [[nodiscard]] inline cg::abstract_value_kind
        kind_of_vtype(value_type vt) noexcept {
            using K = cg::abstract_value_kind;
            switch (vt) {
            case value_type::f32:
            case value_type::f64: return K::floating;
            default:              return K::integer;   // i32/i64
            }
        }

        [[nodiscard]] inline bool is_float_vtype(value_type vt) noexcept {
            return vt == value_type::f32 || vt == value_type::f64;
        }

        // Resolve a comparison synth_kind to (hl_opcode, predicate). Integer
        // compares use icmp; float compares use fcmp (ordered). eqz is handled by
        // the caller (compare-with-zero) and never reaches here.
        struct compare_route {
            hl::hl_opcode op;
            hl::compare_predicate pred;
            bool ok = true;
        };

        [[nodiscard]] inline compare_route route_compare(synth_kind k) noexcept {
            using P = hl::compare_predicate;
            using O = hl::hl_opcode;
            switch (k) {
            case synth_kind::cmp_eq:   return {O::icmp, P::eq};
            case synth_kind::cmp_ne:   return {O::icmp, P::ne};
            case synth_kind::cmp_lt_s: return {O::icmp, P::slt};
            case synth_kind::cmp_le_s: return {O::icmp, P::sle};
            case synth_kind::cmp_gt_s: return {O::icmp, P::sgt};
            case synth_kind::cmp_ge_s: return {O::icmp, P::sge};
            case synth_kind::cmp_lt_u: return {O::icmp, P::ult};
            case synth_kind::cmp_le_u: return {O::icmp, P::ule};
            case synth_kind::cmp_gt_u: return {O::icmp, P::ugt};
            case synth_kind::cmp_ge_u: return {O::icmp, P::uge};
            case synth_kind::fcmp_eq:  return {O::fcmp, P::oeq};
            case synth_kind::fcmp_ne:  return {O::fcmp, P::one};
            case synth_kind::fcmp_lt:  return {O::fcmp, P::olt};
            case synth_kind::fcmp_le:  return {O::fcmp, P::ole};
            case synth_kind::fcmp_gt:  return {O::fcmp, P::ogt};
            case synth_kind::fcmp_ge:  return {O::fcmp, P::oge};
            default:                   return {O::icmp, P::eq, false};
            }
        }

        // Per-function lowering state: the live builder, current block, and the
        // taranga-ssa-id → live-value map.
        struct lower_ctx {
            hl::hl_mir_function fn;
            hl::hl_block* current = nullptr;
            std::vector<cg::ssa_value_id> value_map; // indexed by taranga ssa_value_id

            explicit lower_ctx(std::size_t n_values) : value_map(n_values) {}

            // Mint a fresh live value and bind taranga id → it.
            cg::ssa_value_id bind(ssa_value_id tid) {
                cg::ssa_value_id v{fn.next_id++};
                if (tid != k_null_value && tid < value_map.size()) value_map[tid] = v;
                return v;
            }
            // Look up an already-bound live value (0 = unbound / void).
            [[nodiscard]] cg::ssa_value_id live(ssa_value_id tid) const noexcept {
                if (tid == k_null_value || tid >= value_map.size()) return cg::ssa_value_id{0};
                return value_map[tid];
            }

            // Emit an op with the given operand live-ids and (optional) one result.
            hl::hl_operation* emit(hl::hl_opcode op,
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
                current->ops.push_back(o);
                return o;
            }
        };

        // Materialise a typed integer/float constant, returning its live value.
        inline cg::ssa_value_id
        emit_const(lower_ctx& ctx, const ssa_op& sop) {
            cg::ssa_value_id r = ctx.bind(sop.result);
            auto* o = ctx.emit(hl::hl_opcode::constant, {}, r);
            if (is_float_vtype(sop.result_type)) {
                // Reinterpret the stored bit pattern as the matching float width.
                double d = 0.0;
                if (sop.result_type == value_type::f64) {
                    std::uint64_t bits = sop.imm_value;
                    static_assert(sizeof(double) == sizeof(std::uint64_t));
                    std::memcpy(&d, &bits, sizeof(d));
                } else {
                    auto bits = static_cast<std::uint32_t>(sop.imm_value);
                    float f = 0.0f;
                    static_assert(sizeof(float) == sizeof(std::uint32_t));
                    std::memcpy(&f, &bits, sizeof(f));
                    d = static_cast<double>(f);
                }
                o->attr = hl::constant_attr::floating_value(d);
            } else {
                o->attr = hl::constant_attr::integer_value(
                    static_cast<std::int64_t>(sop.imm_value));
            }
            return r;
        }

        // Emit one SSA op into the current block. Returns nothing; side effects on
        // ctx. Diagnostics for unroutable ops go into `diags`.
        inline void lower_op(lower_ctx& ctx, const ssa_op& sop,
                             vakya::diag::collecting_sink& diags) {
            switch (sop.strategy) {
            case lower_strategy::constant: {
                emit_const(ctx, sop);
                return;
            }
            case lower_strategy::variable: {
                // local.get / global.get already resolved to an existing live value
                // in ssa_build (result reuses the slot's current id); a *.set has no
                // value result. The value_map binding is done lazily: if this op has
                // a result id not yet bound (e.g. global.get), mint one.
                if (sop.result != k_null_value && !ctx.live(sop.result).valid())
                    ctx.bind(sop.result);
                return;
            }
            case lower_strategy::memory: {
                // Linear memory is a memref<?xi8> in v1 (see memory.hpp). Address is
                // operand[0]; a load produces a value, a store consumes value+addr.
                const bool is_load = sop.mnemonic.find(".load") != std::string_view::npos;
                std::vector<cg::ssa_value_id> ops;
                for (auto oid : sop.operands) ops.push_back(ctx.live(oid));
                cg::ssa_value_id r = is_load ? ctx.bind(sop.result) : cg::ssa_value_id{0};
                auto* o = ctx.emit(is_load ? hl::hl_opcode::memref_load
                                           : hl::hl_opcode::memref_store,
                                   ops, r);
                // Element type/width from the access mnemonic; rank-1 dynamic byte
                // memref, matching the v1 linear-memory model.
                hl::memref_attr ma;
                ma.view.elem_kind = kind_of_vtype(sop.result_type);
                ma.view.elem_bits = (sop.result_type == value_type::i64 ||
                                     sop.result_type == value_type::f64) ? 64u : 32u;
                ma.view.rank = 1;
                ma.view.shape[0] = 0; // dynamic
                ma.view.strides[0] = 1;
                ma.base_operand_index = 0;
                o->attr = ma;
                return;
            }
            case lower_strategy::direct: {
                // The HL wire name lives on the opcode_map entry, not the mnemonic;
                // resolve it to a live opcode and fail closed on a miss.
                auto entry = lookup_opcode(sop.mnemonic);
                auto opc = entry
                    ? lithe::ir::portable::wire_name_opcode("lithe.hl", entry->hl_name)
                    : std::nullopt;
                if (!opc) {
                    diags.on_diagnostic(make_error(
                        "TARANGA-LOWER-010",
                        "no HL opcode for wire name of '" + sop.mnemonic + "'"));
                    return;
                }
                std::vector<cg::ssa_value_id> ops;
                for (auto oid : sop.operands) ops.push_back(ctx.live(oid));
                cg::ssa_value_id r = ctx.bind(sop.result);
                ctx.emit(*opc, ops, r);
                return;
            }
            case lower_strategy::synthesize: {
                // Comparisons are fully expressible as icmp/fcmp + predicate attr.
                auto route = route_compare(sop.synth);
                if (route.ok) {
                    std::vector<cg::ssa_value_id> ops;
                    for (auto oid : sop.operands) ops.push_back(ctx.live(oid));
                    cg::ssa_value_id r = ctx.bind(sop.result);
                    auto* o = ctx.emit(route.op, ops, r);
                    o->attr = hl::compare_attr{route.pred,
                                               route.op == hl::hl_opcode::fcmp};
                    return;
                }
                if (sop.synth == synth_kind::eqz) {
                    // x == 0 → const 0 ; icmp eq
                    cg::ssa_value_id zero{ctx.fn.next_id++};
                    auto* z = ctx.emit(hl::hl_opcode::constant, {}, zero);
                    z->attr = hl::constant_attr::integer_value(0);
                    std::array<cg::ssa_value_id, 2> ops{
                        sop.operands.empty() ? cg::ssa_value_id{0}
                                             : ctx.live(sop.operands.front()),
                        zero};
                    cg::ssa_value_id r = ctx.bind(sop.result);
                    auto* o = ctx.emit(hl::hl_opcode::icmp, ops, r);
                    o->attr = hl::compare_attr{hl::compare_predicate::eq, false};
                    return;
                }
                // Remaining expansions (bit-count, rotate, casts, rounding) are the
                // runtime_prelude band's job: it appends a fixed HL sequence into the
                // current block and returns the result value (exact) or a typed zero
                // with its own diagnostic (deferred). Either way the result is bound
                // so uses re-point correctly and the frozen IR stays well-formed.
                {
                    std::vector<cg::ssa_value_id> ops;
                    for (auto oid : sop.operands) ops.push_back(ctx.live(oid));
                    prelude_outcome pr = expand_synth(
                        ctx.fn, ctx.current, sop.synth, sop.mnemonic, ops,
                        sop.result_type, diags);
                    if (sop.result != k_null_value && sop.result < ctx.value_map.size())
                        ctx.value_map[sop.result] = pr.value;
                }
                return;
            }
            case lower_strategy::structured: {
                // Control constructs (block/loop/if/br/return/call/select/drop). v1
                // handles the straight-line-relevant ones; nested regions are future
                // work. `return` is the function terminator, emitted by the caller
                // from the function's trailing value, so here we only note the rest.
                if (sop.mnemonic == "drop" || sop.mnemonic == "nop") return;
                if (sop.mnemonic == "return") return; // terminator handled by caller
                if (sop.mnemonic == "unreachable") {
                    auto* t = ctx.emit(hl::hl_opcode::trap, {}, cg::ssa_value_id{0});
                    t->attr = hl::trap_attr{hl::trap_kind::unreachable, 0};
                    return;
                }
                diags.on_diagnostic(make_warning(
                    "TARANGA-LOWER-030",
                    "structured control '" + sop.mnemonic +
                        "' not yet lowered (v1 straight-line)"));
                return;
            }
            }
        }

    } // namespace detail

    // Lower every SSA function of a validated module to live HL MIR.
    [[nodiscard]] inline lower_result
    lower_to_hl(const validated_module& vm, const ssa_result& ssa) {
        namespace hl = lithe::codegen::hl;
        namespace cg = lithe::codegen;
        lower_result out;

        for (const auto& sfn : ssa.functions) {
            detail::lower_ctx ctx(sfn.value_count);
            ctx.fn.name = sfn.name;
            hl::hl_block* entry = ctx.fn.make_block();
            ctx.fn.body_region.blocks.push_back(entry);
            ctx.current = entry;

            // Seed parameters as `argument` values so local.get(param) resolves.
            // ssa_build minted param ids [0 .. n_params) first; bind each to an
            // argument op result carrying that live id.
            for (std::uint32_t p = 0; p < sfn.params.size(); ++p) {
                cg::ssa_value_id v = ctx.bind(static_cast<ssa_value_id>(p));
                auto* a = ctx.fn.make_op(hl::hl_opcode::argument);
                auto rs = ctx.fn.alloc_span<cg::ssa_value_id>(1);
                rs[0] = v;
                a->results = rs;
                ctx.current->ops.push_back(a);
            }

            // Emit the body ops (v1: single block).
            ssa_value_id last_value = k_null_value;
            if (!sfn.blocks.empty()) {
                for (const auto& sop : sfn.blocks.front().ops) {
                    detail::lower_op(ctx, sop, out.diagnostics);
                    if (sop.result != k_null_value) last_value = sop.result;
                }
            }

            // Terminator: a `return` of the last produced value if the signature
            // has a result, else a void return. Every HL function needs one.
            hl::hl_operation* ret = ctx.fn.make_op(hl::hl_opcode::ret);
            if (!sfn.results.empty() && last_value != k_null_value) {
                cg::ssa_value_id rv = ctx.live(last_value);
                if (rv.valid()) {
                    auto ops = ctx.fn.alloc_span<cg::ssa_value_id>(1);
                    ops[0] = rv;
                    ret->operands = ops;
                }
            }
            ctx.current->ops.push_back(ret);

            out.functions.push_back(
                lowered_function{std::move(ctx.fn), sfn.name});
        }
        // Suppress unused-parameter on vm: it is the proof token gating this call.
        (void)vm;
        return out;
    }

} // namespace taranga
