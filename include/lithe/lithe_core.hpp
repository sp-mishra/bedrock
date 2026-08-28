#pragma once

// lithe/lithe_core.hpp — Lithe construction-surface compatibility shim.
//
// The EDSL construction layer (tags, node, interface, make_node, emit, tree,
// graph, ir, wrappers, structural hashing/equality, traversal) now lives in the
// standalone Vākya library (include/vakya/vakya.hpp) under namespace vakya.
//
// This header re-exports every moved name into namespace lithe so all existing
// lithe:: call sites, the ~65 test_lithe_*.cpp tests, and downstream consumers
// (Pravaha, Sutra) keep compiling unchanged. It ALSO owns the compiler-phase
// wrappers (surface/canonical/optimized/lowered_expr), which are a compilation
// concept — not pure construction — and therefore stay in Lithe.
//
// Phase wrappers register a lithe::structural_unwrap overload so that Vākya's
// structural_equal / structural_hash transparently see through them via the
// structural_unwrap ADL customization point, without Vākya naming these types.
//
// Design: header-only, C++23, no virtual, no macros, pay-for-what-you-use.

#include "vakya/vakya.hpp"

#include <type_traits>
#include <utility>

namespace lithe {
    // -----------------------------
    // Re-export Vākya construction surface into namespace lithe.
    // -----------------------------

    // Tags
    using vakya::add_tag;
    using vakya::sub_tag;
    using vakya::mul_tag;
    using vakya::div_tag;
    using vakya::mod_tag;
    using vakya::neg_tag;
    using vakya::subscript_tag;
    using vakya::eq_tag;
    using vakya::ne_tag;
    using vakya::lt_tag;
    using vakya::le_tag;
    using vakya::gt_tag;
    using vakya::ge_tag;
    using vakya::and_tag;
    using vakya::or_tag;
    using vakya::not_tag;
    using vakya::bit_and_tag;
    using vakya::bit_or_tag;
    using vakya::bit_xor_tag;
    using vakya::bit_not_tag;
    using vakya::shl_tag;
    using vakya::shr_tag;
    using vakya::if_tag;
    using vakya::while_tag;
    using vakya::for_tag;
    using vakya::let_tag;
    using vakya::seq_tag;
    using vakya::call_tag;
    using vakya::cast_tag;
    using vakya::sizeof_tag;
    using vakya::deref_tag;
    using vakya::addr_tag;
    using vakya::lambda_tag;
    using vakya::return_tag;
    using vakya::box_tag;
    using vakya::unbox_tag;
    using vakya::get_element_ptr_tag;
    using vakya::extract_value_tag;
    using vakya::insert_value_tag;
    using vakya::indirect_call_tag;

    // Core construction types
    using vakya::node;
    using vakya::make_node;
    using vakya::interface;
    using vakya::IRBuilder;
    using vakya::capture_t;

    // Concepts
    using vakya::Expression;
    using vakya::Operand;
    using vakya::Terminal;
    using vakya::VariantExpr;

    // Terminal traits
    using vakya::is_terminal;
    using vakya::is_terminal_v;
    using vakya::is_std_variant;
    using vakya::is_std_variant_v;

    // Wrappers
    using vakya::expr;
    using vakya::expr_ref;
    using vakya::as_expr;
    using vakya::is_expr_wrapper;
    using vakya::is_expr_wrapper_v;
    using vakya::is_expr_ref_wrapper;
    using vakya::is_expr_ref_wrapper_v;

    // Structural-unwrap customization point (default identity from Vākya).
    using vakya::structural_unwrap;

    // Traversal / evaluation
    using vakya::evaluate;
    using vakya::visit;
    using vakya::transform;
    using vakya::rebuild;
    using vakya::rewrite_relay;
    using vakya::rewrite_once;

    // Structural hashing / equality (free-function facade)
    using vakya::structural_hash_t;
    using vakya::structural_hash;
    using vakya::structural_equal;
    using vakya::structural_key;

    // Free operators for terminals participate via ADL on vakya types already;
    // node<>/expr<>/expr_ref<> live in namespace vakya, so vakya's operator+/*
    // are found by ADL. Nothing to re-export for those.

    // -----------------------------
    // Sub-namespace re-exports.
    // -----------------------------
    namespace emit {
        using namespace vakya::emit;
    }

    namespace tree {
        using namespace vakya::tree;
    }

    namespace graph {
        using namespace vakya::graph;
    }

    namespace ir {
        using namespace vakya::ir;
    }

    namespace lang {
        using namespace vakya::lang;
    }

    // -----------------------------
    // Phase-aware IR wrappers (COMPILER concept — stays in Lithe).
    // -----------------------------
    template <class T>
    struct surface_expr {
        using value_type = std::decay_t<T>;
        value_type value;

        constexpr explicit surface_expr(T&& v)
            : value(std::forward<T>(v)) {}

        constexpr explicit surface_expr(const value_type& v)
            : value(v) {}
    };

    template <class T>
    struct canonical_expr {
        using value_type = std::decay_t<T>;
        value_type value;

        constexpr explicit canonical_expr(T&& v)
            : value(std::forward<T>(v)) {}

        constexpr explicit canonical_expr(const value_type& v)
            : value(v) {}
    };

    template <class T>
    struct optimized_expr {
        using value_type = std::decay_t<T>;
        value_type value;

        constexpr explicit optimized_expr(T&& v)
            : value(std::forward<T>(v)) {}

        constexpr explicit optimized_expr(const value_type& v)
            : value(v) {}
    };

    template <class T>
    struct lowered_expr {
        using value_type = std::decay_t<T>;
        value_type value;

        constexpr explicit lowered_expr(T&& v)
            : value(std::forward<T>(v)) {}

        constexpr explicit lowered_expr(const value_type& v)
            : value(v) {}
    };

    template <class T>
    struct is_surface_expr : std::false_type {};

    template <class T>
    struct is_surface_expr<surface_expr<T>> : std::true_type {};

    template <class T>
    struct is_canonical_expr : std::false_type {};

    template <class T>
    struct is_canonical_expr<canonical_expr<T>> : std::true_type {};

    template <class T>
    struct is_optimized_expr : std::false_type {};

    template <class T>
    struct is_optimized_expr<optimized_expr<T>> : std::true_type {};

    template <class T>
    struct is_lowered_expr : std::false_type {};

    template <class T>
    struct is_lowered_expr<lowered_expr<T>> : std::true_type {};

    template <class T>
    inline constexpr bool is_surface_expr_v = is_surface_expr<std::decay_t<T>>::value;

    template <class T>
    inline constexpr bool is_canonical_expr_v = is_canonical_expr<std::decay_t<T>>::value;

    template <class T>
    inline constexpr bool is_optimized_expr_v = is_optimized_expr<std::decay_t<T>>::value;

    template <class T>
    inline constexpr bool is_lowered_expr_v = is_lowered_expr<std::decay_t<T>>::value;

    template <class T>
    inline constexpr bool is_phase_expr_v =
        is_surface_expr_v<T> ||
        is_canonical_expr_v<T> ||
        is_optimized_expr_v<T> ||
        is_lowered_expr_v<T>;

    template <class Expr>
    constexpr decltype(auto) unwrap_expr(Expr&& expr) {
        if constexpr (is_phase_expr_v<Expr>) {
            return std::forward<Expr>(expr).value;
        }
        else {
            return std::forward<Expr>(expr);
        }
    }

    template <class Expr>
    [[nodiscard]] constexpr auto as_surface_expr(Expr&& expr) {
        if constexpr (is_surface_expr_v<Expr>) {
            return std::forward<Expr>(expr);
        }
        else {
            return surface_expr<std::decay_t<Expr>>{std::forward<Expr>(expr)};
        }
    }

    template <class Expr>
    [[nodiscard]] constexpr auto as_canonical_expr(Expr&& expr) {
        if constexpr (is_canonical_expr_v<Expr>) {
            return std::forward<Expr>(expr);
        }
        else {
            return canonical_expr<std::decay_t<Expr>>{std::forward<Expr>(expr)};
        }
    }

    template <class Expr>
    [[nodiscard]] constexpr auto as_optimized_expr(Expr&& expr) {
        if constexpr (is_optimized_expr_v<Expr>) {
            return std::forward<Expr>(expr);
        }
        else {
            return optimized_expr<std::decay_t<Expr>>{std::forward<Expr>(expr)};
        }
    }

    template <class Expr>
    [[nodiscard]] constexpr auto as_lowered_expr(Expr&& expr) {
        if constexpr (is_lowered_expr_v<Expr>) {
            return std::forward<Expr>(expr);
        }
        else {
            return lowered_expr<std::decay_t<Expr>>{std::forward<Expr>(expr)};
        }
    }

    // -----------------------------
    // structural_unwrap overloads for phase wrappers.
    //   Found by ADL (argument namespace is lithe). Vākya's structural_equal /
    //   structural_hash call structural_unwrap(x) and, seeing a non-identity
    //   result, recurse on the inner value. This teaches Vākya to transparently
    //   compare/hash through phase wrappers without Vākya naming them.
    // -----------------------------
    template <class T>
    [[nodiscard]] constexpr const auto& structural_unwrap(const surface_expr<T>& x) noexcept {
        return x.value;
    }

    template <class T>
    [[nodiscard]] constexpr const auto& structural_unwrap(const canonical_expr<T>& x) noexcept {
        return x.value;
    }

    template <class T>
    [[nodiscard]] constexpr const auto& structural_unwrap(const optimized_expr<T>& x) noexcept {
        return x.value;
    }

    template <class T>
    [[nodiscard]] constexpr const auto& structural_unwrap(const lowered_expr<T>& x) noexcept {
        return x.value;
    }
} // namespace lithe
