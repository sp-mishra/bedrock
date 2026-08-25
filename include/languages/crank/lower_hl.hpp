#pragma once

// crank/lower_hl.hpp — HL MIR lowering + portable CFG/control/safety/cleanup/tx emission.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// lower_hl_result: output of crank→HL MIR lowering.
//   hl_fn:       the hl_mir_function (arena-owned nodes)
//   diagnostics: any structural issues found during lowering
//
// lower_to_hl(lower_input) → lower_hl_result
//   Phase A (schema 1.1.0): control flow + comparisons
//     - if/while/match → branch/branch_cond + icmp/fcmp
//     - return → ret (variadic operands; zero for Unit)
//     - break/continue → branch to loop exit/header
//     - select for ternary / simple if-expr
//   Phase B (schema 1.2.0): integer operators
//     - sdiv/udiv, srem/urem, bit_and/or/xor/not, shl/lshr/ashr
//   Phase C (schema 1.3.0): safety obligations → guard/trap
//     - proven obligations: no emission
//     - unknown obligations: icmp + guard with kind/policy/diag_code
//     - refuted obligations: compile-time diagnostic (unchanged)
//     - trap/terminate policy: trap terminator in guard-failure block
//   Phase D (schema 1.4.0): defer → cleanup_region
//     - crank_defer_list with LIFO order → cleanup_region + cleanup_yield
//     - controlled exits route through cleanup; trap/terminate bypass
//     - declares defer_scopes capability
//   Phase E (schema 1.5.0): transaction → tx.region
//     - resource[key] read → tx.read; write → tx.write
//     - tx yield → tx.yield; abort → tx.abort
//     - tx_attr carries isolation/retry/replay/conflict/partial/durability/distrib/coord
//     - declares transactions capability
//   Structured loops: crank named loops → structured_for (is_parallel if @parallel)
//   Slices/tensors → memref_type with strided views
//
// crank_defer_list — per-block defer cleanup record (LIFO, no unwinder).
// crank_exit_edge  — annotated exit edge (controlled vs trap).
// hl_lowering_stats — timing + structural counters.
//
// Design §3.6 / §4.5. Consumes typed, obligation-checked Vakya tree from modules 1–3.

#include "lithe/lithe_codegen.hpp"
#include "lithe/lithe_ir/frontend/lowering_contract.hpp"
#include "lithe/lithe_exec/exec_hint.hpp"
#include "languages/crank/safety.hpp"
#include "languages/crank/source_span.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace crank {
    // ============================================================================
    // hl_lowering_stats — timing + structural metrics
    // ============================================================================

    struct hl_lowering_stats {
        std::uint32_t structured_for_count = 0; // total structured_for ops emitted
        std::uint32_t parallel_loop_count = 0; // structured_for with is_parallel
        std::uint32_t memref_count = 0; // memref_load/memref_store ops
        std::uint32_t defer_site_count = 0; // defer statements encountered
        std::uint32_t exit_edge_count = 0; // controlled exit edges (return/break/continue/guard)
        std::uint32_t trap_edge_count = 0; // trap/terminate exit edges (no defer run)
        std::uint32_t block_count = 0; // HL blocks emitted
        std::uint32_t max_loop_nest = 0; // deepest loop nesting level
        // Phase A–E emission counters
        std::uint32_t branch_count = 0; // unconditional branch ops emitted
        std::uint32_t branch_cond_count = 0; // conditional branch ops emitted
        std::uint32_t ret_count = 0; // return ops emitted
        std::uint32_t icmp_count = 0; // integer compare ops
        std::uint32_t fcmp_count = 0; // float compare ops
        std::uint32_t int_op_count = 0; // sdiv/udiv/srem/urem/bit/shift ops
        std::uint32_t guard_count = 0; // guard ops emitted (unknown obligations)
        std::uint32_t trap_count = 0; // trap terminator ops emitted
        std::uint32_t cleanup_region_count = 0; // cleanup_region ops emitted
        std::uint32_t tx_region_count = 0; // tx.region ops emitted
        std::int64_t lower_ns = 0; // wall time nanoseconds
    };

    // ============================================================================
    // crank_defer_entry — one deferred cleanup action
    //
    // Arguments evaluated at defer statement (snapshot semantics).
    // ============================================================================

    struct crank_defer_entry {
        std::string call_name;
        std::vector<std::int64_t> captured_args; // evaluated at defer site
        source_span at;
    };

    // ============================================================================
    // crank_defer_list — per-block ordered list of deferred cleanups
    //
    // Entries pushed as defer statements encountered (program order).
    // On controlled exit edge: walked REVERSE (LIFO).
    // On trap/terminate exit: NOT walked.
    // ============================================================================

    struct crank_defer_list {
        std::vector<crank_defer_entry> entries;

        void push(crank_defer_entry e) {
            entries.push_back(std::move(e));
        }

        // LIFO-ordered copy for emission on a controlled exit edge.
        [[nodiscard]] std::vector<crank_defer_entry> lifo_order() const {
            return {entries.rbegin(), entries.rend()};
        }

        [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    };

    // ============================================================================
    // crank_exit_edge — annotated exit edge for a block
    //
    // kind: what triggered the exit; determines whether defers run.
    //   controlled → return / break / continue / guard-return_result (defers run)
    //   trap       → trap / terminate (defers do NOT run)
    // ============================================================================

    enum class exit_edge_kind : std::uint8_t {
        controlled, // return / break / continue / guard-return_result
        trap, // trap / terminate — no defer run
    };

    struct crank_exit_edge {
        exit_edge_kind kind = exit_edge_kind::controlled;
        std::string target; // block/label name (empty = fall-off)
        std::vector<crank_defer_entry> defers_to_run; // LIFO-ordered (empty for trap)
        source_span at;
    };

    // ============================================================================
    // Obligation descriptor — maps an unknown safety obligation to a portable guard.
    //
    // The lowering pass emits a guard op for each obligation with status == unknown.
    // Proven obligations emit nothing. Refuted obligations produce a diagnostic.
    // ============================================================================

    enum class obligation_status : std::uint8_t {
        proven, // discharged at compile time; no guard emitted
        unknown, // emit guard op with kind/policy
        refuted, // compile-time error (diagnostic emitted, no guard)
    };

    enum class obligation_kind : std::uint8_t {
        bounds, // array/slice bounds check
        div_by_zero, // integer division denominator check
        range_cast, // narrowing integer cast range check
        assertion, // user-written assertion
        overflow, // checked arithmetic overflow
        transaction, // transaction precondition
        parallel_safety, // data-race safety assertion
    };

    struct obligation_info {
        obligation_kind kind = obligation_kind::assertion;
        obligation_status status = obligation_status::unknown;
        safety_failure policy = safety_failure::trap; // effective failure policy
        std::string label; // human-readable / diag code
        source_span at;
    };

    // ============================================================================
    // CFG node descriptors — describe if/while/match/return/break/continue to lower.
    //
    // These are declarative descriptors: the caller builds them from the AST;
    // lower_to_hl emits the corresponding portable HL MIR ops.
    // ============================================================================

    enum class cfg_node_kind : std::uint8_t {
        if_else, // if (cond) { then } else { else }
        while_loop, // while (cond) { body }; break/continue supported
        match_chain, // match expr { arm => ... } — lowered as icmp+branch_cond chain
        ret, // return expr (or Unit return)
        break_, // break out of innermost while
        continue_, // continue to header of innermost while
        ternary, // cond ? a : b → select op
    };

    // Arithmetic operand type — for choosing icmp vs fcmp and signed vs unsigned ops.
    enum class arith_type : std::uint8_t {
        signed_int,
        unsigned_int,
        floating,
        boolean,
    };

    // Comparison predicate for a single binary comparison expression.
    enum class cmp_op : std::uint8_t {
        eq, ne, lt, le, gt, ge
    };

    struct cmp_info {
        cmp_op op = cmp_op::eq;
        arith_type type = arith_type::signed_int;
    };

    // Integer arithmetic op (Phase B).
    enum class int_op_kind : std::uint8_t {
        sdiv, udiv,
        srem, urem,
        bit_and, bit_or, bit_xor, bit_not,
        shl, lshr, ashr,
    };

    struct int_op_info {
        int_op_kind kind = int_op_kind::sdiv;
        source_span at;
    };

    // Arm for a match chain.
    struct match_arm {
        std::int64_t value; // constant integer pattern
        std::string label; // arm block label hint
    };

    // Descriptor for one CFG node to lower.
    struct cfg_node {
        cfg_node_kind kind = cfg_node_kind::ret;
        source_span at;
        std::string label; // optional block name hint

        // if_else / ternary: cond comparison
        cmp_info cmp{};

        // while_loop: has_break / has_continue flags; labels for break/continue targets
        bool has_break = false;
        bool has_continue = false;

        // match: arm list (default arm = last)
        std::vector<match_arm> match_arms;

        // ret: whether the function returns a non-Unit value
        bool returns_value = false;
    };

    // ============================================================================
    // Transaction descriptor — describes a transaction block to lower to tx.region.
    // ============================================================================

    struct tx_read_op_info {
        std::string resource;
        std::string key;
        bool snapshot = false; // old(resource[key]) → snapshot bit
        source_span at;
    };

    struct tx_write_op_info {
        std::string resource;
        std::string key;
        source_span at;
    };

    // Maps Crank transaction config enums to tx_attr fields.
    struct tx_config_info {
        lithe::codegen::hl::tx_isolation iso = lithe::codegen::hl::tx_isolation::serializable;
        std::uint16_t retry = 0;
        lithe::codegen::hl::tx_replay replay = lithe::codegen::hl::tx_replay::none;
        lithe::codegen::hl::tx_conflict conflict = lithe::codegen::hl::tx_conflict::abort;
        lithe::codegen::hl::tx_partial partial = lithe::codegen::hl::tx_partial::disallow;
        lithe::codegen::hl::tx_durability durability = lithe::codegen::hl::tx_durability::durable;
        std::string distribution; // empty = local
        std::string coordinator; // empty = none
        bool has_abort = false; // body contains abort(err)
        std::vector<tx_read_op_info> reads;
        std::vector<tx_write_op_info> writes;
        source_span at;
    };

    // ============================================================================
    // lower_input — what the lowering pass receives
    //
    // Represents one Crank function already through modules 1–3.
    // ============================================================================

    struct loop_bounds_info {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 1;
        bool is_parallel = false;
        bool lower_known = true;
        bool upper_known = true;
        bool step_known = true;
        std::uint64_t trip_count_hint = 0;
        std::string name; // loop variable name from source
    };

    struct tensor_info {
        std::string name;
        std::uint8_t rank = 1;
        // Element type as a Crank source type name (e.g. "Float64", "Int32").
        // Resolved through lowering contract to canonical Lithe IR type string.
        std::string elem_crank_type{"Float64"};
        std::vector<std::int64_t> shape; // per-dimension; -1 = dynamic

        tensor_info() = default;

        tensor_info(std::string n, std::uint8_t r, std::string et, std::vector<std::int64_t> s)
            : name(std::move(n)), rank(r), elem_crank_type(std::move(et)), shape(std::move(s)) {}
    };

    // ============================================================================
    // scalar_program — typed scalar SSA supplied by the Crank semantic lowering.
    //
    // This deliberately models only values that have a direct, portable physical
    // MIR representation today.  It is independent of source syntax, so semantic
    // lowering can use it for expressions from any Crank surface construct.
    // ============================================================================

    using scalar_value_id = std::uint32_t;

    struct scalar_constant {
        scalar_value_id result = 0;
        std::int64_t value = 0;
    };

    enum class scalar_op_kind : std::uint8_t {
        add,
        sub,
        mul,
        div,
    };

    struct scalar_operation {
        scalar_op_kind kind = scalar_op_kind::add;
        scalar_value_id result = 0;
        scalar_value_id lhs = 0;
        scalar_value_id rhs = 0;
    };

    struct scalar_program {
        std::vector<scalar_constant> constants;
        std::vector<scalar_operation> operations;
        std::optional<scalar_value_id> return_value;

        [[nodiscard]] bool empty() const noexcept {
            return constants.empty() && operations.empty() && !return_value.has_value();
        }
    };

    // A reduction binds one scalar accumulator to one structured loop.  Its
    // initial value must be defined by scalar_program; the result becomes
    // available after the loop.  The current portable contract is deliberately
    // integer-only and uses the loop induction value as the right-hand operand.
    struct loop_reduction_info {
        scalar_value_id initial_value = 0;
        scalar_value_id result_value = 0;
        scalar_op_kind kind = scalar_op_kind::add;
    };

    struct lower_input {
        std::string fn_name;
        std::vector<loop_bounds_info> loops; // one entry per nested loop level
        std::vector<tensor_info> tensors; // slices/tensors in scope
        std::vector<crank_defer_entry> defers; // defer sites (source order)
        std::vector<crank_exit_edge> exit_edges; // annotated exit edges
        safety_failure safety_policy = safety_failure::trap;

        // Phase A: CFG nodes to lower (if/while/match/return/break/continue/ternary).
        std::vector<cfg_node> cfg_nodes;

        // Phase B: integer arithmetic ops to lower.
        std::vector<int_op_info> int_ops;

        // Executable scalar SSA from semantic expression lowering.  The legacy
        // int_ops list remains structural metadata; scalar_program is the value-
        // carrying representation consumed by physical MIR execution.
        scalar_program scalar;

        // One optional reduction descriptor per loop, in source nesting order.
        // An absent list preserves the structural-loop lowering contract.
        std::vector<loop_reduction_info> loop_reductions;

        // Phase C: safety obligations to lower → guard/trap.
        std::vector<obligation_info> obligations;

        // Phase D: defers → cleanup_region (uses existing defers + exit_edges).
        bool emit_cleanup_region = false; // true if any defer present

        // Phase E: transactions to lower → tx.region.
        std::vector<tx_config_info> transactions;

        std::size_t arena_capacity = 1u << 20;
    };

    // ============================================================================
    // lower_hl_result — output of lower_to_hl
    // ============================================================================

    struct lower_hl_result {
        lithe::codegen::hl::hl_mir_function hl_fn;
        std::vector<crank_exit_edge> exit_edges; // with resolved defer lists
        std::vector<std::string> diagnostics;
        hl_lowering_stats stats;

        // Execution hint from @parallel/@simd/@gpu attributes on the function.
        // Set by the caller after merge_exec_hints(); consumed by execute_physical_native
        // and compile_and_cache via compile_request.
        lithe::exec::execution_hint exec_hint;

        // Lazily populated by execute_via_interpreter on first call; reused on
        // subsequent calls so coordinate_lowering_pass runs only once per result.
        mutable std::optional<lithe::codegen::mir::physical_mir_function> cached_phys;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // ============================================================================
    // Detail helpers for lower_to_hl
    // ============================================================================

    namespace detail {
        // Map obligation_kind → lithe guard_kind enum.
        [[nodiscard]] inline lithe::codegen::hl::guard_kind
        obligation_to_guard_kind(obligation_kind k) noexcept {
            switch (k) {
            case obligation_kind::bounds: return lithe::codegen::hl::guard_kind::bounds;
            case obligation_kind::div_by_zero: return lithe::codegen::hl::guard_kind::div_by_zero;
            case obligation_kind::range_cast: return lithe::codegen::hl::guard_kind::range_cast;
            case obligation_kind::assertion: return lithe::codegen::hl::guard_kind::assertion;
            case obligation_kind::overflow: return lithe::codegen::hl::guard_kind::overflow;
            case obligation_kind::transaction: return lithe::codegen::hl::guard_kind::transaction;
            case obligation_kind::parallel_safety: return lithe::codegen::hl::guard_kind::parallel_safety;
            }
            return lithe::codegen::hl::guard_kind::assertion;
        }

        // Map safety_failure → lithe failure_policy enum.
        [[nodiscard]] inline lithe::codegen::hl::failure_policy
        safety_to_failure_policy(safety_failure p) noexcept {
            switch (p) {
            case safety_failure::return_result: return lithe::codegen::hl::failure_policy::return_result;
            case safety_failure::trap: return lithe::codegen::hl::failure_policy::trap;
            case safety_failure::terminate: return lithe::codegen::hl::failure_policy::terminate;
            case safety_failure::host_handler: return lithe::codegen::hl::failure_policy::host_handler;
            }
            return lithe::codegen::hl::failure_policy::trap;
        }

        // Map cmp_op × arith_type → compare_predicate.
        [[nodiscard]] inline lithe::codegen::hl::compare_predicate
        cmp_to_predicate(cmp_op op, arith_type t) noexcept {
            using cp = lithe::codegen::hl::compare_predicate;
            if (t == arith_type::floating) {
                switch (op) {
                case cmp_op::eq: return cp::oeq;
                case cmp_op::ne: return cp::one;
                case cmp_op::lt: return cp::olt;
                case cmp_op::le: return cp::ole;
                case cmp_op::gt: return cp::ogt;
                case cmp_op::ge: return cp::oge;
                }
                return cp::oeq;
            }
            bool uns = (t == arith_type::unsigned_int);
            switch (op) {
            case cmp_op::eq: return cp::eq;
            case cmp_op::ne: return cp::ne;
            case cmp_op::lt: return uns ? cp::ult : cp::slt;
            case cmp_op::le: return uns ? cp::ule : cp::sle;
            case cmp_op::gt: return uns ? cp::ugt : cp::sgt;
            case cmp_op::ge: return uns ? cp::uge : cp::sge;
            }
            return cp::eq;
        }

        // Map int_op_kind → hl_opcode.
        [[nodiscard]] inline lithe::codegen::hl::hl_opcode
        int_op_to_opcode(int_op_kind k) noexcept {
            using op = lithe::codegen::hl::hl_opcode;
            switch (k) {
            case int_op_kind::sdiv: return op::sdiv;
            case int_op_kind::udiv: return op::udiv;
            case int_op_kind::srem: return op::srem;
            case int_op_kind::urem: return op::urem;
            case int_op_kind::bit_and: return op::bit_and;
            case int_op_kind::bit_or: return op::bit_or;
            case int_op_kind::bit_xor: return op::bit_xor;
            case int_op_kind::bit_not: return op::bit_not;
            case int_op_kind::shl: return op::shl;
            case int_op_kind::lshr: return op::lshr;
            case int_op_kind::ashr: return op::ashr;
            }
            return op::sdiv;
        }

        [[nodiscard]] inline lithe::codegen::hl::hl_opcode
        scalar_op_to_opcode(scalar_op_kind k) noexcept {
            using op = lithe::codegen::hl::hl_opcode;
            switch (k) {
            case scalar_op_kind::add: return op::add;
            case scalar_op_kind::sub: return op::sub;
            case scalar_op_kind::mul: return op::mul;
            case scalar_op_kind::div: return op::div;
            }
            return op::add;
        }

        // Map obligation_kind → trap_kind for the guard-failure trap block.
        [[nodiscard]] inline lithe::codegen::hl::trap_kind
        obligation_to_trap_kind(obligation_kind k) noexcept {
            using tk = lithe::codegen::hl::trap_kind;
            switch (k) {
            case obligation_kind::bounds: return tk::bounds_violation;
            case obligation_kind::div_by_zero: return tk::div_by_zero;
            case obligation_kind::range_cast: return tk::range_conversion;
            case obligation_kind::assertion: return tk::assert_failed;
            case obligation_kind::overflow: return tk::overflow_checked;
            case obligation_kind::transaction: return tk::tx_failed;
            case obligation_kind::parallel_safety: return tk::unreachable;
            }
            return tk::unreachable;
        }
    } // namespace detail

    // ============================================================================
    // lower_to_hl — main lowering entry point
    //
    // Builds an hl_mir_function from the lower_input descriptor:
    //   1. Emits entry block.
    //   2. Phase A: emits CFG nodes (branch/branch_cond/ret/icmp/fcmp/select).
    //   3. Phase B: emits integer arithmetic ops (sdiv/udiv/srem/urem/bit_*/shift).
    //   4. Phase C: emits guard/trap for unknown obligations; records refuted diagnostics.
    //   5. Phase D: if any defers present, wraps function body in cleanup_region;
    //      declares defer_scopes capability via emit_cleanup_region flag + stats.
    //   6. Phase E: emits tx.region with tx.read/tx.write/tx.yield/tx.abort per transaction.
    //   7. Emits structured_for per loop (existing Phase 0).
    //   8. Emits memref_load stubs per tensor.
    //   9. Resolves defer lists on exit edges (existing).
    //  10. Records stats.
    //
    // Capabilities required (for module declaration):
    //   - transactions:   when any tx_config_info is present
    //   - defer_scopes:   when emit_cleanup_region is true or defers non-empty
    //   - external_calls: caller declares via crank_capability_required separately
    // ============================================================================

    [[nodiscard]] inline lower_hl_result
    lower_to_hl(lower_input inp) {
        using namespace lithe::codegen::hl;
        using lithe::codegen::ssa_value_id;

        lower_hl_result res{
            lower_hl_result{
                .hl_fn = hl_mir_function{inp.arena_capacity}
            }
        };
        res.hl_fn.name = inp.fn_name;

        auto t0 = std::chrono::steady_clock::now();

        hl_region& body = res.hl_fn.body_region;

        // ── Entry block ──────────────────────────────────────────────────────────
        hl_block* entry = res.hl_fn.make_block();
        if (!entry) {
            res.diagnostics.push_back("lower_to_hl: arena exhausted at entry block");
            return res;
        }
        body.blocks.push_back(entry);
        ++res.stats.block_count;

        hl_block* current = entry;

        // A scalar_program supplies its own value-carrying return.  Rejecting a
        // simultaneous structural return prevents an earlier zero-operand ret
        // from making the scalar result unreachable in physical MIR.
        if (inp.scalar.return_value) {
            for (const auto& node : inp.cfg_nodes) {
                if (node.kind == cfg_node_kind::ret) {
                    res.diagnostics.push_back(
                        "lower_to_hl: scalar return cannot be combined with a structural return");
                    return res;
                }
            }
        }

        // ── Phase A: CFG nodes (branch/branch_cond/ret/icmp/fcmp/select) ─────────
        //
        // Each cfg_node describes one source-level construct. The lowering below
        // emits the corresponding portable HL MIR ops into the body region.
        // Block ids are assigned by make_block() (next_id sequence); branch targets
        // reference those ids via branch_attr / branch_cond_attr.
        //
        // Pattern for if_else:
        //   current block → icmp → branch_cond(then_blk, else_blk)
        //   then_blk → branch(join_blk)
        //   else_blk → branch(join_blk)
        //   join_blk becomes new current
        //
        // Pattern for while_loop:
        //   header_blk → icmp → branch_cond(body_blk, exit_blk)
        //   body_blk   → ... → branch(header_blk)
        //   exit_blk   becomes new current
        //
        // Pattern for match:
        //   chain of icmp + branch_cond per arm; last arm is default → branch(join)
        //
        // Pattern for ret:
        //   current block → ret (zero operands for Unit; ≥1 operand for value return)
        //
        // Pattern for break_/continue_:
        //   current block → branch(loop_exit / loop_header)

        for (const auto& node : inp.cfg_nodes) {
            switch (node.kind) {
            case cfg_node_kind::if_else: {
                // Emit: icmp/fcmp into current block; branch_cond to then/else;
                // then and else blocks each end in branch to join.
                hl_block* then_blk = res.hl_fn.make_block();
                hl_block* else_blk = res.hl_fn.make_block();
                hl_block* join_blk = res.hl_fn.make_block();
                if (!then_blk || !else_blk || !join_blk) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at if_else blocks");
                    break;
                }
                then_blk->parent_region = &body;
                else_blk->parent_region = &body;
                join_blk->parent_region = &body;
                body.blocks.push_back(then_blk);
                body.blocks.push_back(else_blk);
                body.blocks.push_back(join_blk);
                res.stats.block_count += 3;

                // Comparison op (icmp or fcmp) → i1 result
                bool is_float = (node.cmp.type == arith_type::floating);
                hl_operation* cmp_op_node = res.hl_fn.make_op(
                    is_float ? hl_opcode::fcmp : hl_opcode::icmp);
                if (!cmp_op_node) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at cmp");
                    break;
                }
                cmp_op_node->attr = compare_attr{
                    detail::cmp_to_predicate(node.cmp.op, node.cmp.type),
                    !is_float // ordered = true for int/bool; true (NaN-safe) for float
                };
                current->ops.push_back(cmp_op_node);
                if (is_float) ++res.stats.fcmp_count;
                else ++res.stats.icmp_count;

                // branch_cond into current → then / else
                hl_operation* bc = res.hl_fn.make_op(hl_opcode::branch_cond);
                if (!bc) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at branch_cond");
                    break;
                }
                bc->attr = branch_cond_attr{then_blk->id, else_blk->id};
                current->ops.push_back(bc);
                ++res.stats.branch_cond_count;

                // then_blk → branch(join)
                hl_operation* then_br = res.hl_fn.make_op(hl_opcode::branch);
                if (!then_br) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at then_br");
                    break;
                }
                then_br->attr = branch_attr{join_blk->id};
                then_blk->ops.push_back(then_br);
                ++res.stats.branch_count;

                // else_blk → branch(join)
                hl_operation* else_br = res.hl_fn.make_op(hl_opcode::branch);
                if (!else_br) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at else_br");
                    break;
                }
                else_br->attr = branch_attr{join_blk->id};
                else_blk->ops.push_back(else_br);
                ++res.stats.branch_count;

                current = join_blk;
                break;
            }

            case cfg_node_kind::while_loop: {
                // header → branch_cond(body, exit); body → branch(header); exit = new current.
                hl_block* header_blk = res.hl_fn.make_block();
                hl_block* body_blk = res.hl_fn.make_block();
                hl_block* exit_blk = res.hl_fn.make_block();
                if (!header_blk || !body_blk || !exit_blk) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at while_loop blocks");
                    break;
                }
                header_blk->parent_region = &body;
                body_blk->parent_region = &body;
                exit_blk->parent_region = &body;
                body.blocks.push_back(header_blk);
                body.blocks.push_back(body_blk);
                body.blocks.push_back(exit_blk);
                res.stats.block_count += 3;

                // current → branch(header) — fall into loop header
                hl_operation* entry_br = res.hl_fn.make_op(hl_opcode::branch);
                if (!entry_br) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at while entry_br");
                    break;
                }
                entry_br->attr = branch_attr{header_blk->id};
                current->ops.push_back(entry_br);
                ++res.stats.branch_count;

                // Comparison in header
                bool is_float = (node.cmp.type == arith_type::floating);
                hl_operation* cmp_op_node = res.hl_fn.make_op(
                    is_float ? hl_opcode::fcmp : hl_opcode::icmp);
                if (!cmp_op_node) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at while cmp");
                    break;
                }
                cmp_op_node->attr = compare_attr{
                    detail::cmp_to_predicate(node.cmp.op, node.cmp.type), !is_float
                };
                header_blk->ops.push_back(cmp_op_node);
                if (is_float) ++res.stats.fcmp_count;
                else ++res.stats.icmp_count;

                // header → branch_cond(body, exit)
                hl_operation* hbc = res.hl_fn.make_op(hl_opcode::branch_cond);
                if (!hbc) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at while branch_cond");
                    break;
                }
                hbc->attr = branch_cond_attr{body_blk->id, exit_blk->id};
                header_blk->ops.push_back(hbc);
                ++res.stats.branch_cond_count;

                // body → branch(header) — back edge
                hl_operation* back_br = res.hl_fn.make_op(hl_opcode::branch);
                if (!back_br) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at while back_br");
                    break;
                }
                back_br->attr = branch_attr{header_blk->id};
                body_blk->ops.push_back(back_br);
                ++res.stats.branch_count;

                current = exit_blk;
                break;
            }

            case cfg_node_kind::match_chain: {
                // Sequential icmp + branch_cond per arm; default arm = final else.
                if (node.match_arms.empty()) break;
                hl_block* join_blk = res.hl_fn.make_block();
                if (!join_blk) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at match join");
                    break;
                }
                join_blk->parent_region = &body;
                body.blocks.push_back(join_blk);
                ++res.stats.block_count;

                hl_block* scan_blk = current;
                for (std::size_t i = 0; i + 1 < node.match_arms.size(); ++i) {
                    hl_block* arm_blk = res.hl_fn.make_block();
                    hl_block* next_blk = res.hl_fn.make_block();
                    if (!arm_blk || !next_blk) {
                        res.diagnostics.push_back("lower_to_hl: arena exhausted at match arm blocks");
                        break;
                    }
                    arm_blk->parent_region = &body;
                    next_blk->parent_region = &body;
                    body.blocks.push_back(arm_blk);
                    body.blocks.push_back(next_blk);
                    res.stats.block_count += 2;

                    // icmp eq (signed) for pattern match
                    hl_operation* cmp_op_node = res.hl_fn.make_op(hl_opcode::icmp);
                    if (!cmp_op_node) {
                        res.diagnostics.push_back("lower_to_hl: arena exhausted at match cmp");
                        break;
                    }
                    cmp_op_node->attr = compare_attr{compare_predicate::eq, true};
                    scan_blk->ops.push_back(cmp_op_node);
                    ++res.stats.icmp_count;

                    hl_operation* bc = res.hl_fn.make_op(hl_opcode::branch_cond);
                    if (!bc) {
                        res.diagnostics.push_back("lower_to_hl: arena exhausted at match branch_cond");
                        break;
                    }
                    bc->attr = branch_cond_attr{arm_blk->id, next_blk->id};
                    scan_blk->ops.push_back(bc);
                    ++res.stats.branch_cond_count;

                    // arm_blk → branch(join)
                    hl_operation* arm_br = res.hl_fn.make_op(hl_opcode::branch);
                    if (!arm_br) {
                        res.diagnostics.push_back("lower_to_hl: arena exhausted at match arm_br");
                        break;
                    }
                    arm_br->attr = branch_attr{join_blk->id};
                    arm_blk->ops.push_back(arm_br);
                    ++res.stats.branch_count;

                    scan_blk = next_blk;
                }
                // Default arm (last): scan_blk → branch(join)
                hl_operation* def_br = res.hl_fn.make_op(hl_opcode::branch);
                if (!def_br) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at match def_br");
                    break;
                }
                def_br->attr = branch_attr{join_blk->id};
                scan_blk->ops.push_back(def_br);
                ++res.stats.branch_count;

                current = join_blk;
                break;
            }

            case cfg_node_kind::ret: {
                // Emit ret terminator in current block.
                hl_operation* ret_op = res.hl_fn.make_op(hl_opcode::ret);
                if (!ret_op) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at ret");
                    break;
                }
                // Value operands are not SSA-wired at this structural level;
                // the number of expected operands is encoded in returns_value.
                // The portable verifier checks operand count via T-check post-freeze.
                current->ops.push_back(ret_op);
                ++res.stats.ret_count;

                // Allocate a fresh continuation block so subsequent nodes land there.
                hl_block* cont_blk = res.hl_fn.make_block();
                if (!cont_blk) break;
                cont_blk->parent_region = &body;
                body.blocks.push_back(cont_blk);
                ++res.stats.block_count;
                current = cont_blk;
                break;
            }

            case cfg_node_kind::break_: {
                // break → branch to the exit block of the enclosing while loop.
                // The exit block id is the most recently allocated exit_blk.
                // Since we emit loops in order, the last body.blocks.tail that carries
                // the while's exit_blk id is the correct target. For the structural
                // skeleton we emit a branch whose target is 0 (the entry block id)
                // and annotate via label so a post-pass can wire it correctly.
                // In a real AST-driven lowering the while-loop descriptor provides
                // the exit_blk pointer directly; here we use label = "loop_exit".
                hl_operation* br = res.hl_fn.make_op(hl_opcode::branch);
                if (!br) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at break");
                    break;
                }
                // Target block id: 0 = entry (structural placeholder; real lowering
                // resolves this from the enclosing loop's exit_blk->id).
                br->attr = branch_attr{entry->id};
                current->ops.push_back(br);
                ++res.stats.branch_count;

                hl_block* cont_blk = res.hl_fn.make_block();
                if (!cont_blk) break;
                cont_blk->parent_region = &body;
                body.blocks.push_back(cont_blk);
                ++res.stats.block_count;
                current = cont_blk;
                break;
            }

            case cfg_node_kind::continue_: {
                // continue → branch to the header block of the enclosing while loop.
                hl_operation* br = res.hl_fn.make_op(hl_opcode::branch);
                if (!br) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at continue");
                    break;
                }
                br->attr = branch_attr{entry->id};
                current->ops.push_back(br);
                ++res.stats.branch_count;

                hl_block* cont_blk = res.hl_fn.make_block();
                if (!cont_blk) break;
                cont_blk->parent_region = &body;
                body.blocks.push_back(cont_blk);
                ++res.stats.block_count;
                current = cont_blk;
                break;
            }

            case cfg_node_kind::ternary: {
                // cond ? a : b → select op (3 operands → 1 result).
                // Comparison emitted first (icmp/fcmp → i1 cond).
                bool is_float = (node.cmp.type == arith_type::floating);
                hl_operation* cmp_op_node = res.hl_fn.make_op(
                    is_float ? hl_opcode::fcmp : hl_opcode::icmp);
                if (!cmp_op_node) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at ternary cmp");
                    break;
                }
                cmp_op_node->attr = compare_attr{
                    detail::cmp_to_predicate(node.cmp.op, node.cmp.type), !is_float
                };
                current->ops.push_back(cmp_op_node);
                if (is_float) ++res.stats.fcmp_count;
                else ++res.stats.icmp_count;

                hl_operation* sel = res.hl_fn.make_op(hl_opcode::select);
                if (!sel) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at select");
                    break;
                }
                current->ops.push_back(sel);
                break;
            }
            } // switch cfg_node_kind
        }

        // ── Phase B: integer arithmetic ops ──────────────────────────────────────
        for (const auto& iop : inp.int_ops) {
            hl_operation* op = res.hl_fn.make_op(detail::int_op_to_opcode(iop.kind));
            if (!op) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at int_op");
                break;
            }
            current->ops.push_back(op);
            ++res.stats.int_op_count;
        }

        // ── Phase B.1: executable scalar SSA ───────────────────────────────────
        // Values are deliberately kept separate from the structural integer-op
        // inventory above.  This lets existing syntax/analysis clients keep their
        // metadata while semantic expression lowering progressively adopts a real
        // use-def graph.
        std::unordered_map<scalar_value_id, ssa_value_id> scalar_values;
        const auto define_scalar = [&](const scalar_value_id source_id) -> std::optional<ssa_value_id> {
            if (source_id == 0) {
                res.diagnostics.push_back("lower_to_hl: scalar value id 0 is reserved");
                return std::nullopt;
            }
            if (scalar_values.contains(source_id)) {
                res.diagnostics.push_back(
                    "lower_to_hl: scalar value defined more than once: " +
                    std::to_string(source_id));
                return std::nullopt;
            }
            const ssa_value_id value{res.hl_fn.next_id++};
            scalar_values.emplace(source_id, value);
            return value;
        };
        const auto use_scalar = [&](const scalar_value_id source_id) -> std::optional<ssa_value_id> {
            if (const auto it = scalar_values.find(source_id); it != scalar_values.end())
                return it->second;
            res.diagnostics.push_back(
                "lower_to_hl: scalar value used before definition: " +
                std::to_string(source_id));
            return std::nullopt;
        };

        for (const auto& constant : inp.scalar.constants) {
            const auto result_id = define_scalar(constant.result);
            if (!result_id) continue;

            hl_operation* op = res.hl_fn.make_op(hl_opcode::constant);
            if (!op) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at scalar constant");
                break;
            }
            const auto results = res.hl_fn.alloc_span<ssa_value_id>(1);
            if (results.empty()) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at scalar constant result");
                break;
            }
            results[0] = *result_id;
            op->results = results;
            op->attr = constant_attr::integer_value(constant.value);
            current->ops.push_back(op);
        }

        for (const auto& operation : inp.scalar.operations) {
            const auto lhs = use_scalar(operation.lhs);
            const auto rhs = use_scalar(operation.rhs);
            const auto result_id = define_scalar(operation.result);
            if (!lhs || !rhs || !result_id) continue;

            hl_operation* op = res.hl_fn.make_op(detail::scalar_op_to_opcode(operation.kind));
            if (!op) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at scalar operation");
                break;
            }
            const auto operands = res.hl_fn.alloc_span<ssa_value_id>(2);
            const auto results = res.hl_fn.alloc_span<ssa_value_id>(1);
            if (operands.empty() || results.empty()) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at scalar operation values");
                break;
            }
            operands[0] = *lhs;
            operands[1] = *rhs;
            results[0] = *result_id;
            op->operands = operands;
            op->results = results;
            current->ops.push_back(op);
            ++res.stats.int_op_count;
        }

        std::optional<scalar_value_id> deferred_scalar_return;
        if (inp.scalar.return_value) {
            if (!inp.loop_reductions.empty()) {
                deferred_scalar_return = *inp.scalar.return_value;
            }
            else {
                const auto value = use_scalar(*inp.scalar.return_value);
                if (value) {
                    hl_operation* ret = res.hl_fn.make_op(hl_opcode::ret);
                    if (!ret) {
                        res.diagnostics.push_back("lower_to_hl: arena exhausted at scalar return");
                    }
                    else {
                        const auto operands = res.hl_fn.alloc_span<ssa_value_id>(1);
                        if (operands.empty()) {
                            res.diagnostics.push_back("lower_to_hl: arena exhausted at scalar return value");
                        }
                        else {
                            operands[0] = *value;
                            ret->operands = operands;
                            current->ops.push_back(ret);
                            ++res.stats.ret_count;
                        }
                    }
                }
            }
        }

        // ── Phase C: obligations → guard / trap ───────────────────────────────────
        for (const auto& ob : inp.obligations) {
            if (ob.status == obligation_status::proven) {
                continue; // proven: no emission
            }
            if (ob.status == obligation_status::refuted) {
                // Compile-time error — emit diagnostic, no runtime op.
                res.diagnostics.push_back(
                    "lower_to_hl: refuted obligation [" + ob.label + "] at fn " + inp.fn_name);
                continue;
            }
            // unknown → emit icmp (predicate: ne, signed_int by default for guard cond)
            // then guard op with kind/policy.
            hl_operation* cmp_op_node = res.hl_fn.make_op(hl_opcode::icmp);
            if (!cmp_op_node) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at guard icmp");
                break;
            }
            cmp_op_node->attr = compare_attr{compare_predicate::ne, true};
            current->ops.push_back(cmp_op_node);
            ++res.stats.icmp_count;

            hl_operation* guard_op = res.hl_fn.make_op(hl_opcode::guard);
            if (!guard_op) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at guard");
                break;
            }
            guard_op->attr = guard_attr{
                detail::obligation_to_guard_kind(ob.kind),
                detail::safety_to_failure_policy(ob.policy),
                0, // diag_code_idx: 0 = none (string table not yet built)
                0, // source_span_idx: 0 = none
            };
            current->ops.push_back(guard_op);
            ++res.stats.guard_count;

            // For trap/terminate policy: emit trap terminator in guard-failure block.
            if (ob.policy == safety_failure::trap || ob.policy == safety_failure::terminate) {
                hl_block* fail_blk = res.hl_fn.make_block();
                if (!fail_blk) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at trap fail_blk");
                    break;
                }
                fail_blk->parent_region = &body;
                body.blocks.push_back(fail_blk);
                ++res.stats.block_count;

                hl_operation* trap_op = res.hl_fn.make_op(hl_opcode::trap);
                if (!trap_op) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at trap");
                    break;
                }
                trap_op->attr = trap_attr{
                    detail::obligation_to_trap_kind(ob.kind), 0
                };
                fail_blk->ops.push_back(trap_op);
                ++res.stats.trap_count;
            }
        }

        // ── Phase D: defer → cleanup_region ──────────────────────────────────────
        //
        // When the function has defer sites, emit a cleanup_region op that owns a
        // cleanup block. Each deferred call (LIFO order) is represented as a call op
        // inside the cleanup block; the cleanup block ends in cleanup_yield.
        // Controlled exit edges route through the cleanup region; trap/terminate
        // exits bypass it entirely (existing crank_exit_edge classification).
        //
        // Declares defer_scopes capability (noted in stats.cleanup_region_count).
        if (!inp.defers.empty() || inp.emit_cleanup_region) {
            hl_operation* cr_op = res.hl_fn.make_op(hl_opcode::cleanup_region);
            if (!cr_op) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at cleanup_region");
            }
            else {
                // Cleanup body region
                hl_region* cr_region = res.hl_fn.make_region();
                hl_block* cr_block = res.hl_fn.make_block();
                if (!cr_region || !cr_block) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at cleanup region/block");
                }
                else {
                    cr_block->parent_region = cr_region;
                    cr_region->parent_op = cr_op;
                    cr_region->blocks.push_back(cr_block);

                    // Emit deferred call stubs in LIFO order (call_name as label; no physical call here).
                    crank_defer_list dlist_for_region;
                    for (const auto& de : inp.defers)
                        dlist_for_region.push(de);
                    for (const auto& de : dlist_for_region.lifo_order()) {
                        hl_operation* call_op = res.hl_fn.make_op(hl_opcode::call);
                        if (!call_op) break;
                        cr_block->ops.push_back(call_op);
                        (void)de; // call_name attached via string table in freeze path
                    }

                    // cleanup_yield terminates the cleanup block
                    hl_operation* cy = res.hl_fn.make_op(hl_opcode::cleanup_yield);
                    if (cy) cr_block->ops.push_back(cy);

                    auto regions_span = res.hl_fn.alloc_span<hl_region*>(1);
                    if (!regions_span.empty()) {
                        regions_span[0] = cr_region;
                        cr_op->regions = regions_span;
                    }
                    current->ops.push_back(cr_op);
                    ++res.stats.cleanup_region_count;
                }
            }
        }

        // ── Phase E: transactions → tx.region ────────────────────────────────────
        for (const auto& txcfg : inp.transactions) {
            hl_operation* tx_op = res.hl_fn.make_op(hl_opcode::tx_region);
            if (!tx_op) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at tx_region");
                break;
            }
            tx_op->attr = tx_attr{
                txcfg.iso,
                txcfg.retry,
                txcfg.replay,
                txcfg.conflict,
                txcfg.partial,
                txcfg.durability,
                0, // distribution_idx (string table idx; 0 = local)
                0, // coordinator_idx  (string table idx; 0 = none)
            };

            // Transaction body region
            hl_region* tx_region = res.hl_fn.make_region();
            hl_block* tx_blk = res.hl_fn.make_block();
            if (!tx_region || !tx_blk) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at tx region/block");
                break;
            }
            tx_blk->parent_region = tx_region;
            tx_region->parent_op = tx_op;
            tx_region->blocks.push_back(tx_blk);

            // tx.read ops
            for (const auto& r : txcfg.reads) {
                hl_operation* rd = res.hl_fn.make_op(hl_opcode::tx_read);
                if (!rd) break;
                tx_blk->ops.push_back(rd);
            }

            // tx.write ops
            for (const auto& w : txcfg.writes) {
                hl_operation* wr = res.hl_fn.make_op(hl_opcode::tx_write);
                if (!wr) break;
                tx_blk->ops.push_back(wr);
            }

            // tx.abort or tx.yield terminator
            if (txcfg.has_abort) {
                hl_operation* ab = res.hl_fn.make_op(hl_opcode::tx_abort);
                if (ab) tx_blk->ops.push_back(ab);
            }
            else {
                hl_operation* yi = res.hl_fn.make_op(hl_opcode::tx_yield);
                if (yi) tx_blk->ops.push_back(yi);
            }

            auto regions_span = res.hl_fn.alloc_span<hl_region*>(1);
            if (!regions_span.empty()) {
                regions_span[0] = tx_region;
                tx_op->regions = regions_span;
            }
            current->ops.push_back(tx_op);
            ++res.stats.tx_region_count;
        }

        // ── Phase 0: structured_for per loop ──────────────────────────────────────
        // The first value-carrying loop contract is intentionally narrow: one
        // rank-1 loop with one integer accumulator.  Nested reductions require
        // an explicit multi-region yield contract rather than ad-hoc register
        // sharing, so they are rejected until that generalization is added.
        if (!inp.loop_reductions.empty()
            && (inp.loop_reductions.size() != inp.loops.size() || inp.loops.size() != 1)) {
            res.diagnostics.push_back(
                "lower_to_hl: value-carrying reductions currently require exactly one loop");
            return res;
        }

        for (std::size_t loop_index = 0; loop_index < inp.loops.size(); ++loop_index) {
            const auto& li = inp.loops[loop_index];
            const loop_reduction_info* reduction = inp.loop_reductions.empty()
                ? nullptr : std::addressof(inp.loop_reductions[loop_index]);
            hl_operation* sf_op = res.hl_fn.make_op(hl_opcode::structured_for);
            if (!sf_op) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at structured_for");
                break;
            }

            structured_for_attr attr;
            attr.rank = 1;
            attr.is_parallel = li.is_parallel;
            attr.bounds_known = li.lower_known && li.upper_known && li.step_known;
            attr.stride_regular = li.step_known && (li.step != 0);
            attr.trip_count_hint = li.trip_count_hint;
            attr.bounds[0] = {
                .lower = static_cast<int>(li.lower),
                .upper = static_cast<int>(li.upper),
                .step = static_cast<int>(li.step),
                .lower_known = li.lower_known,
                .upper_known = li.upper_known,
                .step_known = li.step_known,
            };

            if (attr.trip_count_hint == 0 && attr.bounds_known && li.step > 0 && li.upper > li.lower) {
                const auto span = static_cast<std::uint64_t>(li.upper - li.lower);
                const auto step = static_cast<std::uint64_t>(li.step);
                attr.trip_count_hint = (span + step - 1u) / step;
            }
            sf_op->attr = attr;

            std::optional<ssa_value_id> initial_accumulator;
            std::optional<ssa_value_id> final_accumulator;
            if (reduction) {
                initial_accumulator = use_scalar(reduction->initial_value);
                final_accumulator = define_scalar(reduction->result_value);
                if (!initial_accumulator || !final_accumulator) return res;

                const auto operands = res.hl_fn.alloc_span<ssa_value_id>(1);
                const auto results = res.hl_fn.alloc_span<ssa_value_id>(1);
                if (operands.empty() || results.empty()) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at loop-carried values");
                    return res;
                }
                operands[0] = *initial_accumulator;
                results[0] = *final_accumulator;
                sf_op->operands = operands;
                sf_op->results = results;
            }

            hl_region* loop_body = res.hl_fn.make_region();
            if (!loop_body) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at loop region");
                break;
            }
            loop_body->parent_op = sf_op;

            hl_block* loop_block = res.hl_fn.make_block();
            if (!loop_block) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at loop block");
                break;
            }
            loop_block->parent_region = loop_body;
            loop_body->blocks.push_back(loop_block);

            if (reduction) {
                // Block argument 0 is the induction variable; argument 1 is the
                // incoming accumulator.  region_yield supplies its next value.
                const auto block_args = res.hl_fn.alloc_span<ssa_value_id>(2);
                if (block_args.empty()) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at loop block arguments");
                    return res;
                }
                const ssa_value_id induction_value{res.hl_fn.next_id++};
                const ssa_value_id carried_value{res.hl_fn.next_id++};
                block_args[0] = induction_value;
                block_args[1] = carried_value;
                loop_block->block_args = block_args;

                hl_operation* index_op = res.hl_fn.make_op(hl_opcode::loop_index);
                hl_operation* update_op = res.hl_fn.make_op(
                    detail::scalar_op_to_opcode(reduction->kind));
                hl_operation* yield_op = res.hl_fn.make_op(hl_opcode::region_yield);
                if (!index_op || !update_op || !yield_op) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at loop reduction body");
                    return res;
                }
                const auto index_results = res.hl_fn.alloc_span<ssa_value_id>(1);
                const auto update_operands = res.hl_fn.alloc_span<ssa_value_id>(2);
                const auto update_results = res.hl_fn.alloc_span<ssa_value_id>(1);
                const auto yield_operands = res.hl_fn.alloc_span<ssa_value_id>(1);
                if (index_results.empty() || update_operands.empty()
                    || update_results.empty() || yield_operands.empty()) {
                    res.diagnostics.push_back("lower_to_hl: arena exhausted at loop reduction values");
                    return res;
                }
                const ssa_value_id updated_value{res.hl_fn.next_id++};
                index_results[0] = induction_value;
                update_operands[0] = carried_value;
                update_operands[1] = induction_value;
                update_results[0] = updated_value;
                yield_operands[0] = updated_value;
                index_op->results = index_results;
                update_op->operands = update_operands;
                update_op->results = update_results;
                yield_op->operands = yield_operands;
                loop_block->ops.push_back(index_op);
                loop_block->ops.push_back(update_op);
                loop_block->ops.push_back(yield_op);
                ++res.stats.int_op_count;
            }

            auto regions_span = res.hl_fn.alloc_span<hl_region*>(1);
            if (!regions_span.empty()) {
                regions_span[0] = loop_body;
                sf_op->regions = regions_span;
            }

            current->ops.push_back(sf_op);
            if (!reduction) current = loop_block;

            ++res.stats.structured_for_count;
            ++res.stats.block_count;
            ++res.stats.max_loop_nest;
            if (li.is_parallel) ++res.stats.parallel_loop_count;
        }

        if (deferred_scalar_return) {
            const auto value = use_scalar(*deferred_scalar_return);
            if (!value) return res;
            hl_operation* ret = res.hl_fn.make_op(hl_opcode::ret);
            const auto operands = ret ? res.hl_fn.alloc_span<ssa_value_id>(1)
                                      : std::span<ssa_value_id>{};
            if (!ret || operands.empty()) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at loop reduction return");
                return res;
            }
            operands[0] = *value;
            ret->operands = operands;
            current->ops.push_back(ret);
            ++res.stats.ret_count;
        }

        // ── Memref stubs per tensor ───────────────────────────────────────────────
        for (const auto& ti : inp.tensors) {
            hl_operation* mr_op = res.hl_fn.make_op(hl_opcode::memref_load);
            if (!mr_op) {
                res.diagnostics.push_back("lower_to_hl: arena exhausted at memref_load");
                break;
            }

            std::vector<std::int64_t> dims(ti.shape.begin(), ti.shape.end());
            auto contract_result = lithe::ir::frontend::lower_tensor_type(
                ti.elem_crank_type, ti.rank, dims);
            if (!contract_result.ok()) {
                for (const auto& v : contract_result.violations)
                    res.diagnostics.push_back("lower_to_hl[tensor:" + ti.name + "]: " + v.message);
                break;
            }

            const std::string& ir_str = *contract_result.ir_type_str;
            lithe::codegen::abstract_value_kind elem_kind =
                (!ir_str.empty() && ir_str[0] == 'f')
                    ? lithe::codegen::abstract_value_kind::floating
                    : lithe::codegen::abstract_value_kind::integer;
            std::uint32_t elem_bits = 64;
            if (ir_str.size() > 1) {
                std::uint32_t w = 0;
                for (std::size_t i = 1; i < ir_str.size() && ir_str[i] >= '0' && ir_str[i] <= '9'; ++i)
                    w = w * 10 + static_cast<std::uint32_t>(ir_str[i] - '0');
                if (w > 0) elem_bits = w;
            }

            std::array<std::int64_t, 8> shape_arr{};
            for (std::size_t i = 0; i < dims.size() && i < 8; ++i)
                shape_arr[i] = dims[i];

            memref_attr mattr;
            mattr.base_operand_index = 0;
            mattr.view = memref_type::row_major(elem_kind, elem_bits, ti.rank, shape_arr);
            mr_op->attr = mattr;
            current->ops.push_back(mr_op);
            ++res.stats.memref_count;
        }

        // ── Resolve defer lists on exit edges ─────────────────────────────────────
        crank_defer_list dlist;
        for (const auto& de : inp.defers) {
            dlist.push(de);
            ++res.stats.defer_site_count;
        }

        for (auto edge : inp.exit_edges) {
            if (edge.kind == exit_edge_kind::controlled) {
                edge.defers_to_run = dlist.lifo_order();
                ++res.stats.exit_edge_count;
            }
            else {
                edge.defers_to_run.clear();
                ++res.stats.trap_edge_count;
            }
            res.exit_edges.push_back(std::move(edge));
        }

        auto t1 = std::chrono::steady_clock::now();
        res.stats.lower_ns = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

        return res;
    }
} // namespace crank
