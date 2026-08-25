#pragma once

// crank/const_dim.hpp — Compile-time dimension expression evaluator (v2, §v2.4).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Lifts the CRANK-GEN-V1 gate on const-generic arithmetic expressions.
// Handles: +, -, *, /, % over usize/isize parameters and literals.
//
// Surfaces:
//   dim_op          — arithmetic operator kinds
//   dim_expr        — recursive const-generic dimension expression tree
//   dim_eval_result — evaluated integer value or diagnostic
//   dim_diag_kind   — CRANK-GEN-DIM-001/002/003 diagnostic codes
//   evaluate_dim    — evaluate a dim_expr against a parameter binding map
//
// Design refs: crank.md §v2.4; grammar.md §5.3.

#include "languages/crank/source_span.hpp"

#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace crank {
    // ============================================================================
    // dim_op — arithmetic operator on const-generic dimensions
    // ============================================================================

    enum class dim_op : std::uint8_t { add, sub, mul, div, mod };

    [[nodiscard]] constexpr std::string_view to_string(dim_op op) noexcept {
        switch (op) {
        case dim_op::add: return "+";
        case dim_op::sub: return "-";
        case dim_op::mul: return "*";
        case dim_op::div: return "/";
        case dim_op::mod: return "%";
        }
        return "?";
    }

    // ============================================================================
    // dim_expr — recursive dimension expression (literal, param ref, or binary op)
    //
    // Stored as a variant to avoid heap allocation for the common 1–2 node cases.
    // Deep trees (rare in practice) use a shared_ptr child pair.
    // ============================================================================

    struct dim_literal {
        std::int64_t value;
    };

    struct dim_param {
        std::string name;
    };

    struct dim_binary;

    using dim_expr = std::variant<dim_literal, dim_param, dim_binary>;

    struct dim_binary {
        dim_op op;
        // Children on the heap to keep dim_expr a fixed-size variant.
        std::shared_ptr<dim_expr> lhs;
        std::shared_ptr<dim_expr> rhs;
    };

    // Convenience constructors
    [[nodiscard]] inline dim_expr dim_lit(std::int64_t v) noexcept {
        return dim_literal{v};
    }

    [[nodiscard]] inline dim_expr dim_ref(std::string name) {
        return dim_param{std::move(name)};
    }

    [[nodiscard]] inline dim_expr dim_binop(dim_op op, dim_expr lhs, dim_expr rhs) {
        return dim_binary{
            op,
            std::make_shared<dim_expr>(std::move(lhs)),
            std::make_shared<dim_expr>(std::move(rhs))
        };
    }

    // ============================================================================
    // dim_diag_kind + dim_diagnostic
    // ============================================================================

    enum class dim_diag_kind : std::uint8_t {
        non_integer_result, // CRANK-GEN-DIM-001 — non-integer result in usize context
        dimension_overflow, // CRANK-GEN-DIM-002 — result overflows usize/isize range
        division_by_zero, // CRANK-GEN-DIM-003 — divisor is zero constant
        unbound_param, // parameter referenced but not in binding map
    };

    [[nodiscard]] constexpr std::string_view to_string(dim_diag_kind k) noexcept {
        switch (k) {
        case dim_diag_kind::non_integer_result: return "CRANK-GEN-DIM-001";
        case dim_diag_kind::dimension_overflow: return "CRANK-GEN-DIM-002";
        case dim_diag_kind::division_by_zero: return "CRANK-GEN-DIM-003";
        case dim_diag_kind::unbound_param: return "CRANK-GEN-DIM-004";
        }
        return "CRANK-GEN-DIM-???";
    }

    struct dim_diagnostic {
        dim_diag_kind kind;
        source_span at;
        std::string message;
        bool is_error = true;
    };

    // ============================================================================
    // dim_bindings — map from parameter name → concrete int64 value at instantiation
    // ============================================================================

    using dim_bindings = std::unordered_map<std::string, std::int64_t>;

    // ============================================================================
    // evaluate_dim — evaluate a dim_expr against bindings
    //
    // Returns the integer result or a dim_diagnostic on error.
    // usize semantics: result must be >= 0 and fit in int64_t.
    // ============================================================================

    [[nodiscard]] inline std::expected<std::int64_t, dim_diagnostic>
    evaluate_dim(const dim_expr& expr,
                 const dim_bindings& bindings,
                 source_span at,
                 bool require_nonneg = true) {
        return std::visit([&](const auto& node) -> std::expected<std::int64_t, dim_diagnostic> {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, dim_literal>) {
                if (require_nonneg && node.value < 0) {
                    return std::unexpected(dim_diagnostic{
                        dim_diag_kind::dimension_overflow, at,
                        std::string("CRANK-GEN-DIM-002: negative dimension literal ")
                        + std::to_string(node.value)
                    });
                }
                return node.value;
            }
            else if constexpr (std::is_same_v<T, dim_param>) {
                auto it = bindings.find(node.name);
                if (it == bindings.end()) {
                    return std::unexpected(dim_diagnostic{
                        dim_diag_kind::unbound_param, at,
                        std::string("CRANK-GEN-DIM-004: const parameter '")
                        + node.name + "' has no binding at this instantiation"
                    });
                }
                return it->second;
            }
            else if constexpr (std::is_same_v<T, dim_binary>) {
                auto lval = evaluate_dim(*node.lhs, bindings, at, require_nonneg);
                if (!lval) return lval;
                auto rval = evaluate_dim(*node.rhs, bindings, at, require_nonneg);
                if (!rval) return rval;

                const std::int64_t l = *lval;
                const std::int64_t r = *rval;

                switch (node.op) {
                case dim_op::div:
                case dim_op::mod:
                    if (r == 0) {
                        return std::unexpected(dim_diagnostic{
                            dim_diag_kind::division_by_zero, at,
                            std::string("CRANK-GEN-DIM-003: division by zero in dimension expression")
                        });
                    }
                    break;
                default: break;
                }

                std::int64_t result{};
                bool overflow = false;

                switch (node.op) {
                case dim_op::add:
                    // Overflow: both positive and sum overflows, or both negative
                    if (r > 0 && l > (std::numeric_limits<std::int64_t>::max() - r)) overflow = true;
                    if (r < 0 && l < (std::numeric_limits<std::int64_t>::min() - r)) overflow = true;
                    result = l + r;
                    break;
                case dim_op::sub:
                    if (r < 0 && l > (std::numeric_limits<std::int64_t>::max() + r)) overflow = true;
                    if (r > 0 && l < (std::numeric_limits<std::int64_t>::min() + r)) overflow = true;
                    result = l - r;
                    break;
                case dim_op::mul:
                    if (l != 0 && r != 0) {
                        if (l > 0 && r > 0 && l > (std::numeric_limits<std::int64_t>::max() / r)) overflow = true;
                        if (l < 0 && r < 0 && l < (std::numeric_limits<std::int64_t>::max() / r)) overflow = true;
                        if ((l > 0) != (r > 0) && std::abs(l) > (std::numeric_limits<std::int64_t>::max() /
                            std::abs(r))) overflow = true;
                    }
                    result = l * r;
                    break;
                case dim_op::div: result = l / r;
                    break;
                case dim_op::mod: result = l % r;
                    break;
                }

                if (overflow) {
                    return std::unexpected(dim_diagnostic{
                        dim_diag_kind::dimension_overflow, at,
                        std::string("CRANK-GEN-DIM-002: dimension arithmetic overflow in expression")
                    });
                }

                if (require_nonneg && result < 0) {
                    return std::unexpected(dim_diagnostic{
                        dim_diag_kind::dimension_overflow, at,
                        std::string("CRANK-GEN-DIM-002: dimension result ")
                        + std::to_string(result) + " is negative (usize underflow)"
                    });
                }

                return result;
            }
            return std::unexpected(dim_diagnostic{
                dim_diag_kind::non_integer_result, at,
                "CRANK-GEN-DIM-001: non-integer dimension expression"
            });
        }, expr);
    }

    // ============================================================================
    // dim_to_string — render a dim_expr as a human-readable string (for diagnostics)
    // ============================================================================

    [[nodiscard]] inline std::string dim_to_string(const dim_expr& expr) {
        return std::visit([](const auto& node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, dim_literal>) {
                return std::to_string(node.value);
            }
            else if constexpr (std::is_same_v<T, dim_param>) {
                return node.name;
            }
            else if constexpr (std::is_same_v<T, dim_binary>) {
                return std::string("(") + dim_to_string(*node.lhs)
                    + " " + std::string(to_string(node.op))
                    + " " + dim_to_string(*node.rhs) + ")";
            }
            return "?";
        }, expr);
    }
} // namespace crank
