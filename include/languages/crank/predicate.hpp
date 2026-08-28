#pragma once

// crank/predicate.hpp — Predicate → vakya constraint lowering (Module 3).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Lowers a *pure* pred_expr subtree to a vakya::types::constraint that carries
// a tarka::Term* (or a stub uint64_t hash on the no-SMT path).
//
// G-TRK-1 fallback (b): drives Tarka's per-node Term constructors directly
// from crank::. Crank owns the crank-specific mapping (result/old/len → ops);
// crank never hand-writes Z3.
//
// Predicate AST kinds (grammar §7.5):
//   pred_literal   — integer / bool / float literal
//   pred_ident     — variable reference
//   pred_result    — `result` keyword (return value in `ensures`)
//   pred_old       — `old(expr)` — value at fn entry in `ensures`
//   pred_len       — `len(expr)` — sequence length
//   pred_arith     — +, -, *, /, % on predicates
//   pred_cmp       — ==, !=, <, <=, >, >=
//   pred_logic     — &&, ||, ! (boolean combinators)
//   pred_implies   — -> (implication: !p || q, §10.1)
//   pred_forall    — forall x: T, p(x)
//   pred_exists    — exists x: T, p(x)
//   pred_call      — effectful call — REJECTED (diagnostic emitted)
//
// Usage:
//   crank::predicate_lowerer lwr;
//   auto result = lwr.lower(pred_node, ctx);
//   // result.term_payload = tarka::Term* or hash (no-SMT path)
//   // result.ok() — false if effectful call rejected

#include "languages/crank/source_span.hpp"
#include "vakya/verify.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <expected>

namespace crank {
    // ============================================================================
    // pred_kind — predicate expression node kind
    // ============================================================================

    enum class pred_kind : std::uint8_t {
        literal_int,
        literal_bool,
        literal_float,
        ident,
        result_kw, // `result` pseudo-variable
        old_expr, // `old(expr)`
        len_expr, // `len(expr)`
        arith, // binary: +,-,*,/,%
        cmp, // binary: ==,!=,<,<=,>,>=
        logic, // binary &&, ||; unary !
        implies, // -> (implication)
        forall, // forall x: T, body
        exists, // exists x: T, body
        call, // effectful call — rejected
    };

    // ============================================================================
    // pred_op — operator for arith/cmp/logic nodes
    // ============================================================================

    enum class pred_op : std::uint8_t {
        // arith
        add, sub, mul, div_, mod_,
        // cmp
        eq, ne, lt, le, gt, ge,
        // logic
        and_, or_, not_,
        // implies
        implies_,
    };

    // ============================================================================
    // pred_node — lightweight predicate expression tree
    //
    // All child references are indices into a flat pred_node arena (no pointers).
    // ============================================================================

    struct pred_node {
        pred_kind kind;
        pred_op op = pred_op::eq; // for arith/cmp/logic/implies
        std::string name; // ident / quantifier var
        std::int64_t int_val = 0;
        double flt_val = 0.0;
        bool bool_val = false;
        source_span at;
        std::vector<std::uint32_t> children; // indices into arena
    };

    // ============================================================================
    // pred_arena — flat storage for pred_node trees
    // ============================================================================

    using pred_arena = std::vector<pred_node>;

    // ============================================================================
    // lower_result — outcome of one predicate lowering
    // ============================================================================

    struct lower_result {
        std::uint64_t term_payload = 0; // tarka::Term* (cast) or structural hash
        bool is_tarka = false;
        std::string description;
        bool valid = true;

        [[nodiscard]] bool ok() const noexcept { return valid; }
    };

    // ============================================================================
    // predicate_lower_error — why lowering was rejected
    // ============================================================================

    struct predicate_lower_error {
        source_span at;
        std::string message;
    };

    // ============================================================================
    // lower_context — bindings for result/old in ensures, plus solver handle
    // ============================================================================

    struct lower_context {
        // Opaque handles for crank-specific bindings.
        // On the Tarka path these would be tarka::Term*; on the no-SMT path they
        // are structural hashes (non-zero sentinels).
        std::uint64_t result_term = 0; // bound when lowering `ensures`
        std::uint64_t old_entry_term = 0; // bound when lowering `ensures` old(e)
    };

    // ============================================================================
    // predicate_lowerer
    //
    // Drives predicate → constraint lowering.
    // G-TRK-1 fallback (b): uses structural hash of the pred_node as the
    // term_payload when Tarka is absent. Crank owns the mapping; never hand-writes
    // Z3 AST. On the Tarka path the caller replaces the hash with a real Term*.
    // ============================================================================

    class predicate_lowerer {
    public:
        predicate_lowerer() = default;

        // Lower a single predicate node (by index) in the given arena.
        // Returns the lowered constraint payload or an error.
        [[nodiscard]] std::expected<lower_result, predicate_lower_error>
        lower(const pred_arena& arena, std::uint32_t root_idx,
              const lower_context& ctx) {
            if (root_idx >= arena.size())
                return std::unexpected(predicate_lower_error{{}, "invalid predicate root index"});
            return lower_node(arena, root_idx, ctx);
        }

        // Convenience: lower and emit as a proof_obligation
        [[nodiscard]] std::expected<vakya::types::proof_obligation, predicate_lower_error>
        lower_to_obligation(const pred_arena& arena, std::uint32_t root_idx,
                            const lower_context& ctx,
                            vakya::types::constraint_kind kind = vakya::types::kProveKind) {
            auto res = lower(arena, root_idx, ctx);
            if (!res) return std::unexpected(res.error());

            vakya::types::proof_obligation ob;
            ob.kind = kind;
            ob.term_payload = res->term_payload;
            ob.description = res->description;
            return ob;
        }

        [[nodiscard]] const std::vector<predicate_lower_error>& diagnostics() const noexcept {
            return diags_;
        }

    private:
        std::vector<predicate_lower_error> diags_;

        [[nodiscard]] std::expected<lower_result, predicate_lower_error>
        lower_node(const pred_arena& arena, std::uint32_t idx,
                   const lower_context& ctx) {
            const pred_node& node = arena[idx];

            switch (node.kind) {
            case pred_kind::literal_int: {
                lower_result r;
                r.description = std::to_string(node.int_val);
                r.term_payload = static_cast<std::uint64_t>(node.int_val);
                r.valid = true;
                return r;
            }
            case pred_kind::literal_bool: {
                lower_result r;
                r.description = node.bool_val ? "true" : "false";
                r.term_payload = node.bool_val ? 1u : 0u;
                r.valid = true;
                return r;
            }
            case pred_kind::literal_float: {
                lower_result r;
                r.description = std::to_string(node.flt_val);
                // float bits as uint64 payload — placeholder on no-SMT path
                std::uint64_t bits = 0;
                static_assert(sizeof(double) == sizeof(std::uint64_t));
                __builtin_memcpy(&bits, &node.flt_val, sizeof bits);
                r.term_payload = bits;
                r.valid = true;
                return r;
            }
            case pred_kind::ident: {
                lower_result r;
                r.description = node.name;
                // Hash the name as the term payload on the no-SMT path.
                r.term_payload = std::hash<std::string>{}(node.name);
                r.valid = true;
                return r;
            }
            case pred_kind::result_kw: {
                if (ctx.result_term == 0)
                    return std::unexpected(predicate_lower_error{
                        node.at, "`result` used outside `ensures` context"
                    });
                lower_result r;
                r.description = "result";
                r.term_payload = ctx.result_term;
                r.valid = true;
                return r;
            }
            case pred_kind::old_expr: {
                if (ctx.old_entry_term == 0)
                    return std::unexpected(predicate_lower_error{
                        node.at, "`old(...)` used outside `ensures` context"
                    });
                // old(...) simply uses the entry-value binding; children[0] is the inner expr
                lower_result r;
                r.description = "old(...)";
                r.term_payload = ctx.old_entry_term;
                r.valid = true;
                return r;
            }
            case pred_kind::len_expr: {
                if (node.children.empty())
                    return std::unexpected(predicate_lower_error{
                        node.at, "len() requires an argument"
                    });
                auto inner = lower_node(arena, node.children[0], ctx);
                if (!inner) return std::unexpected(inner.error());

                lower_result r;
                r.description = "len(" + inner->description + ")";
                // Compose hash: len sentinel XOR inner
                r.term_payload = std::hash<std::string>{}("len") ^ (inner->term_payload << 1u);
                r.valid = true;
                return r;
            }
            case pred_kind::arith:
            case pred_kind::cmp:
            case pred_kind::logic:
            case pred_kind::implies: {
                return lower_binary(arena, node, ctx);
            }
            case pred_kind::forall:
            case pred_kind::exists: {
                return lower_quantifier(arena, node, ctx);
            }
            case pred_kind::call: {
                // Effectful call inside predicate — rejected (grammar §7.5)
                predicate_lower_error err{
                    node.at,
                    "effectful call '" + node.name
                    + "' is not allowed inside a predicate (pred_expr must be pure)"
                };
                diags_.push_back(err);
                return std::unexpected(std::move(err));
            }
            }
            return std::unexpected(predicate_lower_error{node.at, "unknown predicate kind"});
        }

        [[nodiscard]] std::expected<lower_result, predicate_lower_error>
        lower_binary(const pred_arena& arena, const pred_node& node,
                     const lower_context& ctx) {
            // unary `not` has one child
            bool is_unary = (node.op == pred_op::not_);
            if (node.children.empty())
                return std::unexpected(predicate_lower_error{node.at, "binary node has no children"});

            auto lhs = lower_node(arena, node.children[0], ctx);
            if (!lhs) return std::unexpected(lhs.error());

            if (is_unary) {
                lower_result r;
                r.description = "!" + lhs->description;
                r.term_payload = ~lhs->term_payload;
                r.valid = true;
                return r;
            }

            if (node.children.size() < 2)
                return std::unexpected(predicate_lower_error{node.at, "binary node needs 2 children"});

            auto rhs = lower_node(arena, node.children[1], ctx);
            if (!rhs) return std::unexpected(rhs.error());

            lower_result r;
            r.description = "(" + lhs->description + " " + std::string(op_str(node.op)) + " " + rhs->description + ")";
            // Structural hash combining lhs, rhs, and op
            r.term_payload = lhs->term_payload
                ^ (rhs->term_payload * 2654435761ULL)
                ^ (static_cast<std::uint64_t>(node.op) << 48u);
            r.valid = true;
            return r;
        }

        [[nodiscard]] std::expected<lower_result, predicate_lower_error>
        lower_quantifier(const pred_arena& arena, const pred_node& node,
                         const lower_context& ctx) {
            if (node.children.empty())
                return std::unexpected(predicate_lower_error{node.at, "quantifier has no body"});

            // Push the quantifier variable into context name-set (stub: hash-extend)
            lower_context inner_ctx = ctx;
            auto body = lower_node(arena, node.children[0], inner_ctx);
            if (!body) return std::unexpected(body.error());

            std::string_view qstr = (node.kind == pred_kind::forall) ? "forall" : "exists";
            lower_result r;
            r.description = std::string(qstr) + " " + node.name + ". " + body->description;
            r.term_payload = std::hash<std::string>{}(std::string(qstr) + node.name)
                ^ (body->term_payload * 6364136223846793005ULL);
            r.valid = true;
            return r;
        }

        [[nodiscard]] static constexpr std::string_view op_str(pred_op op) noexcept {
            switch (op) {
            case pred_op::add: return "+";
            case pred_op::sub: return "-";
            case pred_op::mul: return "*";
            case pred_op::div_: return "/";
            case pred_op::mod_: return "%";
            case pred_op::eq: return "==";
            case pred_op::ne: return "!=";
            case pred_op::lt: return "<";
            case pred_op::le: return "<=";
            case pred_op::gt: return ">";
            case pred_op::ge: return ">=";
            case pred_op::and_: return "&&";
            case pred_op::or_: return "||";
            case pred_op::not_: return "!";
            case pred_op::implies_: return "->";
            }
            return "?";
        }
    };
} // namespace crank
