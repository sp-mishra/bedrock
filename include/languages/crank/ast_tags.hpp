#pragma once

// crank/ast_tags.hpp — Crank-owned Vakya tag descriptors (extension band ≥ 1000).
//
// C++23, header-only, no virtual, no macros.
// Namespace: crank
//
// Each tag is an empty struct; its vakya::emit::tag_descriptor specialisation
// carries symbol, stable_id (>= kExtensionIdBase = 1000), arity, is_commutative.
// No fork of vakya.hpp. No crank-specific tag leaks into arithmetic bands.
//
// Tag stable_id allocation (crank reserved: 1000 – 1099):
//   1000 fn_tag             function declaration
//   1001 block_tag          statement block  {}
//   1002 let_tag            let binding
//   1003 var_tag            var declaration
//   1004 match_tag          match expression
//   1005 call_tag           function call (overrides vakya built-in at 1005, fine — ext band)
//   1006 attribute_tag      @ attribute
//   1007 field_access_tag   expr.field
//   1008 index_tag          expr[idx]
//   1009 range_tag          a..b / a..=b
//   1010 transaction_tag    transaction { }
//   1011 transaction_option_tag  isolation = … inside transaction(…)
//   1012 tx_load_tag        Medha transactional load
//   1013 tx_store_tag       Medha transactional store
//   1014 tx_abort_tag       abort(error) — explicit tx abort (§2.3)
//   1015 tx_yield_tag       yield expr   — tx body value (§2.2)
//   1016 view_decl_tag      view declaration  (domain views)
//   1017 view_expr_tag      view construction expression  (domain views)
//   1018 extern_fn_tag      extern fn declaration (@host.link binding)
//   1019 try_op_tag         postfix ? error-propagation (Result/Option only)
//   1020 closure_tag        closure literal — both |..| and fn(..) forms
//   1021 quantifier_tag     forall/exists — typed binder (quant_bound_kind=typed) or ranged (quant_bound_kind=ranged)

#include "vakya/vakya.hpp"

namespace crank {
    // ── Tag structs ──────────────────────────────────────────────────────────────

    struct fn_tag {};

    struct block_tag {};

    struct let_tag {};

    struct var_tag {};

    struct match_tag {};

    struct crank_call_tag {}; // crank call — distinct from vakya::call_tag
    struct attribute_tag {};

    struct field_access_tag {};

    struct index_tag {};

    struct range_tag {};

    struct transaction_tag {};

    struct transaction_option_tag {};

    struct tx_load_tag {};

    struct tx_store_tag {};

    struct tx_abort_tag {}; // abort(error) — explicit tx abort (§2.3)
    struct tx_yield_tag {}; // yield expr   — tx body value (§2.2)
    struct view_decl_tag {}; // view declaration
    struct view_expr_tag {}; // view construction expression
    struct extern_fn_tag {}; // extern fn declaration — body-less, @host.link bound

    // ── New surface (addendum) ───────────────────────────────────────────────

    struct try_op_tag {}; // postfix ? — error-propagation (Result/Option only)

    // quant_bound_kind discriminant for quantifier_tag
    enum class quant_bound_kind : std::uint8_t { typed, ranged };

    struct closure_tag {}; // closure literal — |..| and fn(..) both lower here

    struct quantifier_tag {}; // forall/exists; carry quant_bound_kind in node metadata
} // namespace crank

// ── Descriptor specialisations ───────────────────────────────────────────────

namespace vakya::emit {
    template <>
    struct tag_descriptor<crank::fn_tag> {
        static constexpr std::string_view symbol = "fn";
        static constexpr std::uint32_t stable_id = 1000u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::block_tag> {
        static constexpr std::string_view symbol = "block";
        static constexpr std::uint32_t stable_id = 1001u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::let_tag> {
        static constexpr std::string_view symbol = "let";
        static constexpr std::uint32_t stable_id = 1002u;
        static constexpr std::uint8_t arity = 2u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::var_tag> {
        static constexpr std::string_view symbol = "var";
        static constexpr std::uint32_t stable_id = 1003u;
        static constexpr std::uint8_t arity = 2u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::match_tag> {
        static constexpr std::string_view symbol = "match";
        static constexpr std::uint32_t stable_id = 1004u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::crank_call_tag> {
        static constexpr std::string_view symbol = "call";
        static constexpr std::uint32_t stable_id = 1005u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::attribute_tag> {
        static constexpr std::string_view symbol = "attribute";
        static constexpr std::uint32_t stable_id = 1006u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::field_access_tag> {
        static constexpr std::string_view symbol = "field_access";
        static constexpr std::uint32_t stable_id = 1007u;
        static constexpr std::uint8_t arity = 2u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::index_tag> {
        static constexpr std::string_view symbol = "index";
        static constexpr std::uint32_t stable_id = 1008u;
        static constexpr std::uint8_t arity = 2u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::range_tag> {
        static constexpr std::string_view symbol = "range";
        static constexpr std::uint32_t stable_id = 1009u;
        static constexpr std::uint8_t arity = 2u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::transaction_tag> {
        static constexpr std::string_view symbol = "transaction";
        static constexpr std::uint32_t stable_id = 1010u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::transaction_option_tag> {
        static constexpr std::string_view symbol = "transaction_option";
        static constexpr std::uint32_t stable_id = 1011u;
        static constexpr std::uint8_t arity = 2u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::tx_load_tag> {
        static constexpr std::string_view symbol = "tx_load";
        static constexpr std::uint32_t stable_id = 1012u;
        static constexpr std::uint8_t arity = 1u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::tx_store_tag> {
        static constexpr std::string_view symbol = "tx_store";
        static constexpr std::uint32_t stable_id = 1013u;
        static constexpr std::uint8_t arity = 2u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::tx_abort_tag> {
        static constexpr std::string_view symbol = "tx_abort";
        static constexpr std::uint32_t stable_id = 1014u;
        static constexpr std::uint8_t arity = 1u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::tx_yield_tag> {
        static constexpr std::string_view symbol = "tx_yield";
        static constexpr std::uint32_t stable_id = 1015u;
        static constexpr std::uint8_t arity = 1u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::view_decl_tag> {
        static constexpr std::string_view symbol = "view_decl";
        static constexpr std::uint32_t stable_id = 1016u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::view_expr_tag> {
        static constexpr std::string_view symbol = "view_expr";
        static constexpr std::uint32_t stable_id = 1017u;
        static constexpr std::uint8_t arity = 2u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::extern_fn_tag> {
        static constexpr std::string_view symbol = "extern_fn";
        static constexpr std::uint32_t stable_id = 1018u;
        static constexpr std::uint8_t arity = 0;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::try_op_tag> {
        static constexpr std::string_view symbol = "try";
        static constexpr std::uint32_t stable_id = 1019u;
        static constexpr std::uint8_t arity = 1u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::closure_tag> {
        static constexpr std::string_view symbol = "closure";
        static constexpr std::uint32_t stable_id = 1020u;
        static constexpr std::uint8_t arity = kVariadicArity; // params… + body
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<crank::quantifier_tag> {
        static constexpr std::string_view symbol = "quantifier";
        static constexpr std::uint32_t stable_id = 1021u;
        static constexpr std::uint8_t arity = kVariadicArity; // binders… + body
        static constexpr bool is_commutative = false;
    };
} // namespace vakya::emit
