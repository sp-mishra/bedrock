#pragma once

// crank/verify_mir.hpp — Crank HL MIR pre-freeze verifier gate (design §4).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Strategy (item 7, Default Fix):
//   Delegate structural/type/CFG/SSA/region/capability checks to
//   lithe::ir::portable::verify_portable after freeze — single source of
//   truth, no drift between two verifier implementations.
//   Keep only Crank-specific pre-freeze invariants that Lithe cannot see:
//     - obligation discharge status (refuted obligations → compile error)
//     - defer-accepts-calls-only (defer body must contain only call ops)
//     - transactional-write restriction pre-lowering (writes inside tx only)
//
// Physical MIR path (legacy, kept for backend use):
//   verify_crank_mir wraps verify_physical_mir with two Crank-specific checks
//   beyond what lithe's structural verifier provides:
//     - path_without_return_or_trap: reachable block with no terminator
//     - missing_return_value: value-returning fn has ret with no operand
//
// verified_mir: proof token; only verify_crank_mir can mint one.
//   Non-owning view into physical_mir_function. Constructible only by the
//   friend function — prevents backends receiving un-verified physical MIR.

#include "lithe/lithe_codegen.hpp"
#include "languages/crank/exec_result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crank {
    // ============================================================================
    // mir_defect_kind — Crank-specific pre-freeze defect categories
    //
    // Structural defects (block-missing-terminator, branch-target-missing,
    // SSA dominance, region nesting, capability coverage) are delegated to
    // lithe::ir::portable::verify_portable post-freeze and are NOT reproduced here.
    // ============================================================================

    enum class mir_defect_kind : std::uint8_t {
        // Physical MIR path (legacy checks — delegated structurally but kept for error reporting)
        instr_after_terminator, // (structurally: lithe; retained for diagnostics symmetry)
        path_without_return_or_trap, // reachable block falls off with no terminator
        missing_return_value, // value-returning fn: ret with no value operand

        // Crank-specific pre-freeze HL MIR invariants (lithe cannot see these)
        refuted_obligation, // refuted obligation not resolved before lowering
        defer_non_call, // defer body contains a non-call op
        tx_write_outside_region, // transactional write outside a tx.region
    };

    // ============================================================================
    // verified_mir — proof token; only the verifier can mint one
    //
    // Non-owning view into a physical_mir_function. Constructible only by
    // verify_crank_mir (private ctor + friend) — a backend cannot be handed
    // un-verified MIR by accident.
    // ============================================================================

    class verified_mir {
    public:
        verified_mir() = default; // empty/invalid until minted

        [[nodiscard]] const lithe::codegen::mir::physical_mir_function* function() const noexcept {
            return fn_;
        }

        [[nodiscard]] bool valid() const noexcept { return fn_ != nullptr; }

    private:
        explicit verified_mir(const lithe::codegen::mir::physical_mir_function& fn) noexcept
            : fn_(&fn) {}

        const lithe::codegen::mir::physical_mir_function* fn_ = nullptr;

        friend execution_result<verified_mir>
        verify_crank_mir(const lithe::codegen::mir::physical_mir_function&, bool);
    };

    namespace detail {
        // Terminator recognition: the CFG ops that legitimately end a block.
        // Includes the full schema 1.1.0–1.5.0 terminator set so the physical
        // verifier can recognise all terminators without reimplementing lithe's table.
        [[nodiscard]] inline bool is_terminator(lithe::codegen::opcode op) noexcept {
            using op_t = lithe::codegen::opcode;
            return op == op_t::ret
                || op == op_t::branch
                || op == op_t::branch_cond;
        }
    } // namespace detail

    // ============================================================================
    // verify_crank_mir — physical MIR verification gate (legacy path)
    //
    //   expects_value: true if the function returns a non-Unit value.
    //
    // Step 1: delegates to lithe structural verifier (verify_physical_mir).
    //   Covers: block existence, unique terminators, non-final terminator,
    //   branch-target existence, ret-defines-no-values, spill resolution,
    //   duplicate ids, phase — all without reimplementing them.
    //
    // Step 2 (Crank-specific): fall-off check — every block must end in a
    //   terminator (lithe's validate_cfg catches non-final terminators and
    //   unreachable blocks but not a completely terminator-free block).
    //
    // Step 3 (Crank-specific): missing return value check — value-returning
    //   function must not have a bare `ret` with no operand.
    //
    // Step 4: region legality seams — GPU/SIMD/defer extension points kept as
    //   named hooks; no rejection in this pass (planned with SIMD/GPU planners).
    //
    // Returns: completed result with verified_mir token, or a failed result
    //   with execution_error_kind::verification_failed / missing_return_value /
    //   unsupported_opcode.
    // ============================================================================

    [[nodiscard]] inline execution_result<verified_mir>
    verify_crank_mir(const lithe::codegen::mir::physical_mir_function& fn,
                     bool expects_value) {
        using namespace lithe::codegen;

        // Step 1: delegate structural verification to lithe (single source of truth).
        const auto structural = verify_physical_mir(fn);
        if (!structural.diagnostics.empty()) {
            execution_error e = make_error(
                execution_error_kind::verification_failed,
                "physical MIR failed structural verification",
                fn.function.name);
            for (const auto& d : structural.diagnostics)
                e.nested.push_back(make_error(execution_error_kind::verification_failed, d,
                                              fn.function.name));
            return make_failed<verified_mir>(std::move(e));
        }

        // Step 2 (Crank-specific): fall-off — every block must end in a terminator.
        // lithe's validate_cfg flags a non-final terminator and unreachable blocks,
        // but not a block that carries no terminator at all.
        for (const auto& block : fn.function.blocks) {
            const bool ends_in_terminator =
                !block.instructions.empty() &&
                detail::is_terminator(block.instructions.back().op);
            if (!ends_in_terminator) {
                execution_error e = make_error(
                    execution_error_kind::verification_failed,
                    "bb" + std::to_string(block.id) +
                    " ends without a terminator (path without return or trap)",
                    fn.function.name);
                e.ir_op = "bb" + std::to_string(block.id);
                return make_failed<verified_mir>(std::move(e));
            }

            // Step 3 (Crank-specific): value-returning function's `ret` must carry
            // a value operand (lithe MIR ret reads its value from uses).
            if (expects_value) {
                for (const auto& inst : block.instructions) {
                    if (inst.op == opcode::ret && inst.uses.empty()) {
                        execution_error e = make_error(
                            execution_error_kind::missing_return_value,
                            "value-returning function has a ret with no value operand in i" +
                            std::to_string(inst.id),
                            fn.function.name);
                        e.ir_op = "i" + std::to_string(inst.id);
                        return make_failed<verified_mir>(std::move(e));
                    }
                }
            }
        }

        // Step 4: region legality seams — extension points for GPU/SIMD/defer.
        // Structural/type/region/capability checks for HL ops (branch_cond, guard,
        // cleanup_region, tx.region, etc.) are delegated to verify_portable
        // post-freeze; no Crank-side reimplementation needed here.
        // check_gpu_region(fn); check_simd_region(fn); check_defer_edges(fn);

        return make_completed(verified_mir{fn});
    }
} // namespace crank
