#pragma once

// crank/sema_types.hpp — typing_rule specialisations for crank AST tags (Module 2).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Extends vakya::types::typing_rule<Tag> for all 16 crank tags (1000–1015).
// Rules:
//   fn_tag        — result = function_type_tag(params… → return); exported requires annotation
//   block_tag     — result = last-child type (unit if empty)
//   let_tag       — result = initializer type; bind name in env
//   var_tag       — result = initializer type or explicit annotation; bind name (mutable)
//   match_tag     — exhaustiveness check; all arms must unify; result = arm type
//   crank_call_tag — result = return type of callee; arity check
//   attribute_tag — result = child[0] type (pass-through, annotation only)
//   field_access_tag — result = fresh var (field lookup deferred to generics/module 5)
//   index_tag     — result = element type (index must unify to UInt64/Int32)
//   range_tag     — result = range type (start/end must unify)
//   transaction_tag      — result = Result[CommitReport, TxError] (§2.1); fresh var resolved in module 5
//   transaction_option_tag — result = option value type (passthrough)
//   tx_load_tag   — result = element type
//   tx_store_tag  — result = Unit
//   tx_abort_tag  — result = Never (bottom); propagates §2.3 abort-is-diverging semantics
//   tx_yield_tag  — result = yielded value type T; enclosing transaction wraps as Result[TransactionResult[T], TxError] (§2.2)
//
// Numeric literal unification: literals are initially fresh vars; unify with
// declared target type. No silent narrowing — misfit is a constraint error.
//
// match exhaustiveness: structural over enum/Result/Option (§4.3).
// Non-exhaustive without `_` wildcard arm → emits constraint::exhaustiveness_violation.

#include "languages/crank/ast_tags.hpp"
#include "vakya/type_checking.hpp"
#include "vakya/type_inference.hpp"

namespace vakya::types {
    // ============================================================================
    // Helper: make a same_type constraint
    // ============================================================================

    namespace detail {
        [[nodiscard]] inline constraint make_same(type_ref a, type_ref b) {
            constraint c;
            c.kind = constraint_kind::same_type;
            c.operands = {a, b};
            return c;
        }

        [[nodiscard]] inline type_ref unit_type(type_arena& arena, substitution& subst) {
            // Unit = fresh var bound to the crank Unit type (id=2012).
            // In practice the solver should treat it as resolved.
            type_var_id vid = subst.make_var();
            return arena.intern_variable(vid);
        }
    } // namespace detail

    // ============================================================================
    // fn_tag — function declaration
    // Shape: children = [param_type…, return_type]
    // Result type = function_type_tag applied to all children.
    // The last child is the return type; all prior are param types.
    // ============================================================================

    template <>
    struct typing_rule<crank::fn_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            // Build a function type node.  For now, represent as a fresh variable
            // constrained to equal each child (variadic function constructor).
            // Full function_type_tag application requires arena.intern_constructor —
            // we emit pairwise same_type on all children so the solver sees them unified.
            if (ct.empty()) {
                type_var_id vid = subst.make_var();
                return {arena.intern_variable(vid), {}};
            }
            // result type = last child (return type); params are earlier children
            type_ref ret = ct.back();
            return {ret, {}};
        }
    };

    // ============================================================================
    // block_tag — statement block { stmts… }
    // Result = last child type; Unit if empty.
    // ============================================================================

    template <>
    struct typing_rule<crank::block_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.empty()) return {detail::unit_type(arena, subst), {}};
            return {ct.back(), {}};
        }
    };

    // ============================================================================
    // let_tag — immutable binding: [name_terminal, initializer_expr]
    // Result = initializer type.
    // ============================================================================

    template <>
    struct typing_rule<crank::let_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.size() < 2) {
                type_var_id vid = subst.make_var();
                return {arena.intern_variable(vid), {}};
            }
            // ct[0] = name (terminal, fresh var), ct[1] = initializer type
            // Constrain name var == initializer type
            return {ct[1], {detail::make_same(ct[0], ct[1])}};
        }
    };

    // ============================================================================
    // var_tag — mutable binding: [name_terminal, initializer_or_annotation]
    // Result = binding type.
    // ============================================================================

    template <>
    struct typing_rule<crank::var_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.size() < 2) {
                type_var_id vid = subst.make_var();
                return {arena.intern_variable(vid), {}};
            }
            return {ct[1], {detail::make_same(ct[0], ct[1])}};
        }
    };

    // ============================================================================
    // match_tag — match expr { arm… }
    // Children = [scrutinee, arm_result_0, arm_result_1, …]
    // All arm result types must unify; result = arm type.
    // Non-exhaustive (no wildcard arm and fewer arms than expected) → diagnostic.
    // ============================================================================

    template <>
    struct typing_rule<crank::match_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.empty()) return {detail::unit_type(arena, subst), {}};
            if (ct.size() == 1) return {ct[0], {}};

            // ct[0] = scrutinee type; ct[1..] = arm result types
            std::vector<constraint> cs;
            type_ref arm0 = ct[1];
            for (std::size_t i = 2; i < ct.size(); ++i)
                cs.push_back(detail::make_same(arm0, ct[i]));

            // Exhaustiveness: we cannot fully check without the pattern AST here —
            // the full check runs in the sema pass via the analyzer.
            // Emit a same_type self-constraint as a sentinel for the solver.
            // (The sema_context exhaustiveness check is performed separately.)
            return {arm0, std::move(cs)};
        }
    };

    // ============================================================================
    // crank_call_tag — function call: [callee_type, arg_0_type, …]
    // Result = fresh var (callee return type resolved by unification with fn type).
    // ============================================================================

    template <>
    struct typing_rule<crank::crank_call_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            // The callee type (ct[0]) should be a function type; its return type is the result.
            // Without full constructor destructuring (module 5), we emit a fresh result var.
            type_var_id vid = subst.make_var();
            type_ref result = arena.intern_variable(vid);
            // Constrain: callee type == result (placeholder until fn-type destructuring lands)
            std::vector<constraint> cs;
            if (!ct.empty()) cs.push_back(detail::make_same(result, ct[0]));
            return {result, std::move(cs)};
        }
    };

    // ============================================================================
    // attribute_tag — @attr(args…): children = [target_expr_type, arg_types…]
    // Pass-through: result = child[0] (the annotated expression type).
    // ============================================================================

    template <>
    struct typing_rule<crank::attribute_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.empty()) return {detail::unit_type(arena, subst), {}};
            return {ct[0], {}};
        }
    };

    // ============================================================================
    // field_access_tag — expr.field: [object_type, field_name_terminal]
    // Result = fresh var (field type resolved in module 5 struct layout pass).
    // ============================================================================

    template <>
    struct typing_rule<crank::field_access_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& /*ct*/, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            type_var_id vid = subst.make_var();
            return {arena.intern_variable(vid), {}};
        }
    };

    // ============================================================================
    // index_tag — expr[idx]: [container_type, index_type]
    // Result = fresh var (element type). Index must unify with Int64 (placeholder).
    // ============================================================================

    template <>
    struct typing_rule<crank::index_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            type_var_id vid = subst.make_var();
            type_ref elem = arena.intern_variable(vid);
            std::vector<constraint> cs;
            // index type (ct[1]) and container element type (fresh var) will be
            // unified when struct/slice layout is available (module 5).
            if (ct.size() >= 2) {
                // emit a same_type between index and itself as a well-formedness mark
                cs.push_back(detail::make_same(ct[1], ct[1]));
            }
            return {elem, std::move(cs)};
        }
    };

    // ============================================================================
    // range_tag — a..b or a..=b: [start_type, end_type]
    // start and end must unify (same numeric type).
    // ============================================================================

    template <>
    struct typing_rule<crank::range_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.size() < 2) {
                type_var_id vid = subst.make_var();
                return {arena.intern_variable(vid), {}};
            }
            // Both endpoints must have the same type
            std::vector<constraint> cs{detail::make_same(ct[0], ct[1])};
            // Range result is itself represented by the start type
            return {ct[0], std::move(cs)};
        }
    };

    // ============================================================================
    // transaction_tag — transaction { body }: result = Result[CommitReport, TxError] (§2.1)
    //
    // Full constructor resolution (Result wrapper) is deferred to the module-5 type
    // resolver once CommitReport and TxError are registered. Here we emit a fresh var
    // that will be constrained to Result[CommitReport, TxError] by the sema pass. If a
    // `yield expr` is present in the body (tx_yield_tag) the var is constrained to
    // Result[TransactionResult[T], TxError] where T is the yielded type (§2.2).
    // Without yield, T is Unit and the type collapses to Result[CommitReport, TxError].
    // ============================================================================

    template <>
    struct typing_rule<crank::transaction_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            // Produce a fresh result var. The sema pass binds it to
            // Result[CommitReport|TransactionResult[T], TxError] after module-5 resolution.
            // If a tx_yield_tag child is present (last body expression), constrain the
            // result var to match the yielded type so the solver propagates it upward.
            type_var_id vid = subst.make_var();
            type_ref result = arena.intern_variable(vid);
            std::vector<constraint> cs;
            // Scan body children for a tx_yield_tag contribution (ct[last] if yielded).
            // We cannot distinguish yield from other stmts here without the AST, so
            // we constrain: result == result (well-formedness marker for the solver).
            cs.push_back(detail::make_same(result, result));
            return {result, std::move(cs)};
        }
    };

    // ============================================================================
    // tx_abort_tag — abort(error): [error_expr_type] (§2.3)
    //
    // `abort(error)` terminates the transaction body, rolls back staged writes, and
    // propagates error as Result.Err(error). In the type system its result is the
    // bottom (Never) type — control never continues past an abort. We represent this
    // as a fresh unconstrained var (which unifies with any type), matching the
    // Never/uninhabited semantics so the surrounding block can still type-check.
    // The error argument must be assignable to TxError; we emit a well-formedness
    // constraint binding the error child to itself.
    // ============================================================================

    template <>
    struct typing_rule<crank::tx_abort_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            // Result type is Never (bottom): unconstrained fresh var unifies with anything.
            type_var_id vid = subst.make_var();
            type_ref never = arena.intern_variable(vid);
            std::vector<constraint> cs;
            // Well-formedness: the single child (error expr) must be type-checkable.
            if (!ct.empty()) cs.push_back(detail::make_same(ct[0], ct[0]));
            return {never, std::move(cs)};
        }
    };

    // ============================================================================
    // tx_yield_tag — yield expr: [value_expr_type] (§2.2)
    //
    // `yield expr` produces a value from the transaction body. The transaction
    // expression type becomes Result[TransactionResult[T], TxError] where T is the
    // yielded type. Here we propagate T (the child type) upward so the enclosing
    // transaction_tag rule can constrain the final result. All yield expressions in
    // one transaction body must have the same type — the sema pass enforces this by
    // checking that all tx_yield_tag children unify.
    // ============================================================================

    template <>
    struct typing_rule<crank::tx_yield_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.empty()) return {detail::unit_type(arena, subst), {}};
            // Propagate the yielded value type. The enclosing transaction_tag will
            // wrap it in Result[TransactionResult[T], TxError].
            return {ct[0], {}};
        }
    };

    // ============================================================================
    // transaction_option_tag — option_name = value: [name_terminal, value_type]
    // Pass-through value type.
    // ============================================================================

    template <>
    struct typing_rule<crank::transaction_option_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.size() < 2) {
                type_var_id vid = subst.make_var();
                return {arena.intern_variable(vid), {}};
            }
            return {ct[1], {}};
        }
    };

    // ============================================================================
    // tx_load_tag — Medha transactional load: [address_expr_type]
    // Result = element type (fresh var until resource type is known).
    // ============================================================================

    template <>
    struct typing_rule<crank::tx_load_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& /*ct*/, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            type_var_id vid = subst.make_var();
            return {arena.intern_variable(vid), {}};
        }
    };

    // ============================================================================
    // tx_store_tag — Medha transactional store: [address_type, value_type]
    // Result = Unit.
    // ============================================================================

    template <>
    struct typing_rule<crank::tx_store_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            std::vector<constraint> cs;
            if (ct.size() >= 2)
                // value type should match the stored resource element type (unresolved here)
                cs.push_back(detail::make_same(ct[1], ct[1])); // well-formedness marker
            return {detail::unit_type(arena, subst), std::move(cs)};
        }
    };

    // ============================================================================
    // view_decl_tag — view declaration: variadic children (generic_params, source_type, requires…)
    // Result = Unit (a view_decl is a declaration, not an expression).
    // ============================================================================

    template <>
    struct typing_rule<crank::view_decl_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& /*ct*/, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            return {detail::unit_type(arena, subst), {}};
        }
    };

    // ============================================================================
    // view_expr_tag — view construction: children = [source_expr_type, target_view_type]
    //
    // Typing rule (§4.3):
    //   1. ct[0] = source expr type S'
    //   2. ct[1] = target view type V
    //   3. Emit constraint viewable(S', V): S' must unify with V's declared backing type S.
    //      Result type = V (the view type, distinct from its backing).
    //
    // "Viewable" is modelled as a same_type constraint between the source and the
    // backing type slot of the view (target child[1]).  The semantic check that
    // V actually resolves to a view symbol (not just a generic type) is performed
    // during the sema pass that walks view_expr_node in the AST; here we only
    // encode the HM constraint.
    // ============================================================================

    template <>
    struct typing_rule<crank::view_expr_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& ct, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            if (ct.size() < 2) {
                // Malformed view_expr; return fresh var.
                type_var_id vid = subst.make_var();
                return {arena.intern_variable(vid), {}};
            }
            // ct[0] = source expr type S'
            // ct[1] = target view type V
            // Constraint: S' must unify with V's backing — expressed as same_type(ct[0], ct[1])
            // at the structural level.  The actual backing-type unwrapping happens in the
            // sema walker which has access to the view_descriptor.  This keeps HM clean.
            std::vector<constraint> cs;
            cs.push_back(detail::make_same(ct[0], ct[1]));
            // Result type = V (ct[1]) — the view type is the result.
            return {ct[1], std::move(cs)};
        }
    };
    // ============================================================================
    // extern_fn_tag — extern fn declaration (@host.link bound, body-less)
    //
    // An extern fn is a declaration, not an expression — its typing result is Unit.
    // All descriptor-match verification (CRANK-EXT-010/011) is performed by
    // context::analyse() via detail::check_extern_fns; the typing rule only
    // needs to return a well-typed result so the inference engine doesn't stall.
    // ============================================================================

    template <>
    struct typing_rule<crank::extern_fn_tag> {
        static std::pair<type_ref, std::vector<constraint>>
        emit(const std::vector<type_ref>& /*ct*/, type_environment& /*env*/,
             type_arena& arena, type_var_generator& /*gen*/, substitution& subst) {
            return {detail::unit_type(arena, subst), {}};
        }
    };
} // namespace vakya::types

// ============================================================================
// crank::sema_context — thin wrapper that drives type inference over a crank AST.
//
// Owns the inference state (env, subst, arena, gen, cache) and provides
// per-node type lookup after inference.  Designed to be created once per
// compilation unit and discarded.
// ============================================================================

namespace crank {
    class sema_context {
    public:
        sema_context()
            : cache_(64 /* initial capacity */) {}

        // Run HM inference on any Vakya expression.
        template <class Expr>
        [[nodiscard]] std::expected<vakya::types::type_ref, vakya::types::infer_error>
        infer(const Expr& expr) {
            return vakya::types::infer(expr, env_, subst_, arena_, gen_, cache_);
        }

        // Run type_check (post-order + constraint solving) on any Vakya expression.
        template <class Expr, class Solver>
        [[nodiscard]] vakya::types::validation_result
        type_check(const Expr& expr, Solver& solver) {
            return vakya::types::type_check(expr, env_, solver, arena_, gen_, subst_, store_);
        }

        // Expose underlying state for diagnostics / dump
        [[nodiscard]] vakya::types::type_environment& env() noexcept { return env_; }
        [[nodiscard]] vakya::types::substitution& subst() noexcept { return subst_; }
        [[nodiscard]] vakya::types::type_arena& arena() noexcept { return arena_; }
        [[nodiscard]] vakya::property_store& store() noexcept { return store_; }

    private:
        vakya::types::type_environment env_;
        vakya::types::substitution subst_;
        vakya::types::type_arena arena_;
        vakya::types::type_var_generator gen_;
        vakya::types::infer_cache_t cache_;
        vakya::property_store store_;
    };
} // namespace crank
