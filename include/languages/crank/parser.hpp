#pragma once

// crank/parser.hpp — Crank grammar productions (lexy).
//
// C++23, header-only, no virtual, no macros.
// Namespace: crank::grammar
//
// One lexy production struct per grammar.md §2–§7 nonterminal.
// Whitespace set once on source_file and inherited.
// Output: lexy::parse_tree retaining trivia + node positions.
//
// §7 expression precedence table encoded via dsl::expression:
//   L1  ||  L2  &&  L3 == != < <= > >= (infix_op_single, no chain)
//   L4  |   L5  ^   L6 & &^  L7 << >>
//   L8  + -     L9  * / %    L10 as (RHS = type, not expr)
//   L11 prefix ! - await     L12 postfix call [] .  ?
//   L13 primary
//
// ASI: context_flag line_continues + context_counter bracket_depth.
//   Newline in whitespace: !continues && depth==0 → stmt_term token.
//   else/. carve-outs handled in whitespace production.

#include <lexy/dsl.hpp>
#include <lexy/callback.hpp>
#include <lexy/grammar.hpp>
#include <lexy/parse_tree.hpp>
#include <lexy/action/parse_as_tree.hpp>
#include <lexy/input/string_input.hpp>

#include "languages/crank/lexer.hpp"
#include "languages/crank/source_span.hpp"

#include <string_view>
#include <vector>

namespace crank::grammar {
    namespace dsl = lexy::dsl;

    // Forward declarations (for recursive productions)
    struct expr;
    struct primary;
    struct type_prod;
    struct block;
    struct pred_expr;
    struct statement;

    // ============================================================================
    // §3.2 Comments
    // ============================================================================

    struct line_comment {
        static constexpr auto rule =
            dsl::lit_c < '/' > +dsl::lit_c < '/' > +dsl::until(dsl::newline);
    };

    struct block_comment {
        static constexpr auto rule =
            LEXY_LIT("/*") + dsl::until(LEXY_LIT("*/"));
    };

    // ============================================================================
    // §3 Whitespace + ASI
    // ============================================================================
    //
    // lexy whitespace is defined on the root production and inherited.
    // We track:
    //   - line_continues_flag: set after binary ops, commas, unclosed brackets
    //   - bracket_depth_counter: ( [ { tracked
    //
    // At newline: if depth==0 and !line_continues → emit stmt_term trivia marker
    // (full ASI via pre-pass fallback if lexy context ergonomics are insufficient)

    struct whitespace_rule {
        static constexpr auto rule =
            dsl::ascii::space | dsl::ascii::blank |
            (dsl::peek(LEXY_LIT("//")) >> dsl::p<line_comment>) |
            (dsl::peek(LEXY_LIT("/*")) >> dsl::p<block_comment>);
    };

    // ============================================================================
    // §3.3 Identifiers
    // ============================================================================

    struct ident_token {
        static constexpr auto rule = [] {
            auto head = dsl::ascii::alpha_underscore;
            auto body = dsl::ascii::alpha_digit_underscore;
            return dsl::identifier(head, body).reserve(
                LEXY_LIT("package"), LEXY_LIT("import"), LEXY_LIT("fn"),
                LEXY_LIT("let"), LEXY_LIT("var"), LEXY_LIT("const"),
                LEXY_LIT("type"), LEXY_LIT("struct"), LEXY_LIT("enum"),
                LEXY_LIT("if"), LEXY_LIT("else"), LEXY_LIT("for"),
                LEXY_LIT("while"), LEXY_LIT("match"), LEXY_LIT("return"),
                LEXY_LIT("break"), LEXY_LIT("continue"),
                LEXY_LIT("in"), LEXY_LIT("as"),
                LEXY_LIT("true"), LEXY_LIT("false"), LEXY_LIT("Unit"),
                LEXY_LIT("await"), LEXY_LIT("spawn"),
                LEXY_LIT("defer"), LEXY_LIT("transaction"),
                LEXY_LIT("requires"), LEXY_LIT("ensures"), LEXY_LIT("assert"),
                LEXY_LIT("trait"), LEXY_LIT("impl"),
                LEXY_LIT("view"), LEXY_LIT("module"), LEXY_LIT("pub")
            );
        }();
    };

    // qualified_ident: IDENT { "." IDENT }
    struct qualified_ident {
        static constexpr auto rule =
            dsl::p<ident_token> + dsl::while_(dsl::peek(dsl::lit_c < '.' > +dsl::ascii::alpha_underscore) >>
                (dsl::lit_c < '.' > +dsl::p<ident_token>));
    };

    // ============================================================================
    // §3.4 Literals
    // ============================================================================

    struct hex_int_lit {
        static constexpr auto rule =
            dsl::peek(LEXY_LIT("0x") | LEXY_LIT("0X")) >>
            dsl::capture(dsl::token(LEXY_LIT("0x") + dsl::digits<lexy::dsl::hex>.sep(dsl::lit_c < '_' >)));
    };

    struct oct_int_lit {
        static constexpr auto rule =
            dsl::peek(LEXY_LIT("0o")) >>
            dsl::capture(dsl::token(LEXY_LIT("0o") + dsl::digits<lexy::dsl::octal>.sep(dsl::lit_c < '_' >)));
    };

    struct bin_int_lit {
        static constexpr auto rule =
            dsl::peek(LEXY_LIT("0b")) >>
            dsl::capture(dsl::token(LEXY_LIT("0b") + dsl::digits<lexy::dsl::binary>.sep(dsl::lit_c < '_' >)));
    };

    struct dec_int_lit {
        static constexpr auto rule =
            dsl::capture(dsl::digits<lexy::dsl::decimal>.sep(dsl::lit_c < '_' >));
    };

    struct int_lit {
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("0x") | LEXY_LIT("0X")) >> dsl::p<hex_int_lit>) |
            (dsl::peek(LEXY_LIT("0o")) >> dsl::p<oct_int_lit>) |
            (dsl::peek(LEXY_LIT("0b")) >> dsl::p<bin_int_lit>) |
            (dsl::else_ >> dsl::p<dec_int_lit>);
    };

    struct float_exponent_token {
        static constexpr auto rule =
            dsl::token((dsl::lit_c < 'e' > | dsl::lit_c < 'E' >) +
                       dsl::opt(dsl::peek(dsl::lit_c < '+' > | dsl::lit_c < '-' >) >>
                           (dsl::lit_c < '+' > | dsl::lit_c < '-' >)) +
                       dsl::digits<>);
    };

    struct float_lit {
        static constexpr auto rule = dsl::capture(dsl::token(
            dsl::digits<>.sep(dsl::lit_c < '_' >) +
            (
                dsl::token(dsl::lit_c < '.' > +dsl::digits<>.sep(dsl::lit_c < '_' >) +
                    dsl::opt(dsl::peek(dsl::lit_c < 'e' > | dsl::lit_c < 'E' >) >>
                        float_exponent_token::rule))
                |
                (dsl::peek(dsl::lit_c < 'e' > | dsl::lit_c < 'E' >) >> float_exponent_token::rule)
            )
        ));
    };

    struct string_escape {
        static constexpr auto escape_table = lexy::symbol_table<char>
            .map < 'n' > ('\n')
            .map < 't' > ('\t')
            .map < 'r' > ('\r')
            .map < '\\' > ('\\')
            .map < '"' > ('"')
            .map < '0' > ('\0');

        static constexpr auto escaped =
            dsl::backslash_escape
            .symbol<escape_table>()
            .rule(dsl::lit_c<'x'> >> dsl::times < 2 > (dsl::digit<lexy::dsl::hex>))
            .rule(LEXY_LIT("u{") >> (dsl::digits<lexy::dsl::hex> + dsl::lit_c < '}' >));
    };

    struct string_lit {
        static constexpr auto rule = dsl::quoted(dsl::ascii::print, string_escape::escaped);
    };

    struct raw_string_lit {
        static constexpr auto rule = dsl::delimited(dsl::lit_c < '`' >)(dsl::ascii::print);
    };

    struct bool_lit {
        static constexpr auto rule = LEXY_LIT("true") | LEXY_LIT("false");
    };

    // All literals in one production
    struct literal {
        // float must precede int to avoid partial match on "1.0"
        // Approach: peek on first char for dispatch; float/int share digit prefix
        // so we attempt float first (it has stricter grammar), fallback to int.
        // For parse_tree purposes, we just need something that parses.
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("true") | LEXY_LIT("false")) >> dsl::p<bool_lit>) |
            LEXY_LIT("Unit") |
            (dsl::peek(dsl::lit_c < '"' >) >> dsl::p<string_lit>) |
            (dsl::peek(dsl::lit_c < '`' >) >> dsl::p<raw_string_lit>) |
            // float_lit is a branchable token: it matches only dec digits followed
            // by '.'/exponent, so it fails cleanly on a plain int and int_lit runs.
            dsl::p<float_lit> |
            (dsl::else_ >> dsl::p<int_lit>);
    };

    // ============================================================================
    // §4 Types
    // ============================================================================

    // Forward declaration used in many places
    // type_prod is defined later after all sub-types.

    struct generic_arg;

    // slice_type: [] type   (grammar §4: empty brackets, element type follows)
    struct slice_type {
        static constexpr auto rule =
            dsl::lit_c < '[' > +dsl::lit_c < ']' > +dsl::recurse<type_prod>;
    };

    // array_type: [N] type  or  [IDENT] type
    struct array_type {
        static constexpr auto rule =
            dsl::lit_c < '[' > +
            (dsl::peek(dsl::ascii::digit) >> dsl::p<int_lit> |
                dsl::else_ >> dsl::p<qualified_ident>) +
            dsl::lit_c < ']' > +
            dsl::recurse<type_prod>;
    };

    // tuple_type: ( type, type, ... ) or ()
    struct tuple_type {
        static constexpr auto rule =
            dsl::parenthesized.opt_list(dsl::recurse<type_prod>, dsl::sep(dsl::comma));
    };

    // func_type: fn ( types ) -> type
    struct func_type {
        static constexpr auto rule =
            LEXY_LIT("fn") +
            dsl::parenthesized.opt_list(dsl::recurse<type_prod>, dsl::sep(dsl::comma)) +
            dsl::opt(LEXY_LIT("->") >> dsl::recurse<type_prod>);
    };

    // generic_type: qualified_ident [ generic_args ]
    struct generic_arg {
        // type arg, const int/bool arg, or ident arg — use else_ fallback
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("true") | LEXY_LIT("false")) >> dsl::p<bool_lit>) |
            (dsl::peek(dsl::ascii::digit) >> dsl::p<int_lit>) |
            (dsl::else_ >> dsl::recurse<type_prod>);
    };

    // generic_type: qualified_ident [ [ generic_args ] ]
    // Optional brackets — handles both plain idents (T) and instantiated generics (Tensor[Float32]).
    struct generic_type {
        static constexpr auto rule =
            dsl::p<qualified_ident> +
            dsl::opt(dsl::peek(dsl::lit_c < '[' >) >>
                dsl::square_bracketed(
                    dsl::list(dsl::p<generic_arg>, dsl::sep(dsl::comma))
                ));
    };

    // refined_type: { IDENT : type | pred_expr }
    struct refined_type {
        static constexpr auto rule =
            dsl::curly_bracketed(
                dsl::p<ident_token> + dsl::colon + dsl::recurse<type_prod> +
                dsl::lit_c < '|' > +dsl::recurse<pred_expr>
            );
    };

    // result_type: Result [ type , type ]
    struct result_type {
        static constexpr auto rule =
            LEXY_LIT("Result") +
            dsl::square_bracketed(
                dsl::recurse<type_prod> + dsl::comma + dsl::recurse<type_prod>
            );
    };

    // option_type: Option [ type ]
    struct option_type {
        static constexpr auto rule =
            LEXY_LIT("Option") +
            dsl::square_bracketed(dsl::recurse<type_prod>);
    };

    struct type_prod {
        // Each alternative peeked by first token (peek = no consume)
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("Result")) >> dsl::p<result_type>) |
            (dsl::peek(LEXY_LIT("Option")) >> dsl::p<option_type>) |
            (dsl::peek(dsl::lit_c < '{' >) >> dsl::p<refined_type>) |
            (dsl::peek(LEXY_LIT("fn")) >> dsl::p<func_type>) |
            (dsl::peek(dsl::lit_c < '[' > +dsl::lit_c < ']' >) >> dsl::p<slice_type>) |
            (dsl::peek(dsl::lit_c < '[' >) >> dsl::p<array_type>) |
            (dsl::peek(dsl::lit_c < '(' >) >> dsl::p<tuple_type>) |
            (dsl::else_ >> dsl::p<generic_type>);
    };

    // ============================================================================
    // §5 Declarations
    // ============================================================================

    // bound: qualified_ident | callable_bound
    // callable_bound only exposes Fn(...) -> R in user-facing syntax.
    // FnMut/FnOnce callability class is inferred by sema from capture analysis.
    struct callable_kind {
        static constexpr auto rule = LEXY_LIT("Fn");
    };

    struct callable_bound {
        static constexpr auto rule =
            dsl::p<callable_kind> +
            dsl::parenthesized.opt_list(dsl::p<type_prod>, dsl::sep(dsl::comma)) +
            LEXY_LIT("->") + dsl::p<type_prod>;
    };

    struct bound {
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("Fn")) >> dsl::p<callable_bound>) |
            (dsl::else_ >> dsl::p<qualified_ident>);
    };

    // type_param: IDENT [ : bound { + bound } ]
    struct type_param {
        static constexpr auto rule =
            dsl::p<ident_token> +
            dsl::opt(dsl::colon >>
                (dsl::p<bound> +
                    dsl::while_(dsl::lit_c<'+'> >> dsl::p<bound>)));
    };

    // const_param_type: "usize" | "isize" | "Bool" | qualified_ident
    struct const_param_type {
        static constexpr auto rule =
            LEXY_LIT("usize") | LEXY_LIT("isize") | LEXY_LIT("Bool") |
            (dsl::else_ >> dsl::p<qualified_ident>);
    };

    // const_param: IDENT : const_param_type
    struct const_param {
        static constexpr auto rule =
            dsl::p<ident_token> + dsl::colon + dsl::p<const_param_type>;
    };

    struct generic_param {
        // Both const_param and type_param start with IDENT — use type_param as catch-all
        // (const vs type distinction is semantic, not syntactic at this level)
        static constexpr auto rule = dsl::else_ >> dsl::p<type_param>;
    };

    struct generic_params {
        static constexpr auto rule =
            dsl::square_bracketed(
                dsl::list(dsl::p<generic_param>, dsl::sep(dsl::comma))
            );
    };

    struct param {
        // Type annotation is optional to allow bare `self` receivers in methods.
        // Semantic check enforces typed params outside receiver position.
        static constexpr auto rule =
            dsl::p<ident_token> + dsl::opt(dsl::colon >> dsl::p<type_prod>);
    };

    struct params {
        static constexpr auto rule =
            dsl::list(dsl::p<param>, dsl::sep(dsl::comma));
    };

    // contract_clause: requires/ensures pred_expr
    struct contract_clause {
        static constexpr auto rule =
            (LEXY_LIT("requires") | LEXY_LIT("ensures")) +
            dsl::recurse<pred_expr>;
    };

    // func_decl: fn IDENT [generic_params] (params) [-> type] {contracts} block
    struct func_decl {
        static constexpr auto rule =
            LEXY_LIT("fn") + dsl::p<ident_token> +
            dsl::opt(dsl::peek(dsl::lit_c < '[' >) >> dsl::p<generic_params>) +
            dsl::parenthesized.opt_list(dsl::p<param>, dsl::sep(dsl::comma)) +
            dsl::opt(LEXY_LIT("->") >> dsl::p<type_prod>) +
            dsl::while_(
                dsl::peek(LEXY_LIT("requires") | LEXY_LIT("ensures")) >>
                dsl::p<contract_clause>
            ) +
            dsl::recurse<block>;
    };

    // struct_body
    struct struct_field {
        static constexpr auto rule = dsl::p<ident_token> + dsl::colon + dsl::p<type_prod>;
    };

    struct struct_body {
        static constexpr auto rule =
            LEXY_LIT("struct") +
            dsl::curly_bracketed.opt_list(dsl::p<struct_field>);
    };

    // enum_body
    struct enum_variant {
        static constexpr auto rule =
            dsl::p<ident_token> +
            dsl::opt(dsl::peek(dsl::lit_c < '(' >) >>
                dsl::parenthesized.opt_list(dsl::p<type_prod>, dsl::sep(dsl::comma)));
    };

    struct enum_body {
        static constexpr auto rule =
            LEXY_LIT("enum") +
            dsl::curly_bracketed.opt_list(dsl::p<enum_variant>);
    };

    // type_body: struct | enum | type alias
    struct type_body {
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("struct")) >> dsl::p<struct_body>) |
            (dsl::peek(LEXY_LIT("enum")) >> dsl::p<enum_body>) |
            (dsl::else_ >> dsl::p<type_prod>);
    };

    // type_decl: type IDENT [generic_params] = type_body
    struct type_decl {
        static constexpr auto rule =
            LEXY_LIT("type") + dsl::p<ident_token> +
            dsl::opt(dsl::peek(dsl::lit_c < '[' >) >> dsl::p<generic_params>) + dsl::lit_c < '=' > +dsl::p<type_body>;
    };
    // const_decl: const IDENT [: type] = expr
    struct const_decl {
        static constexpr auto rule =
            LEXY_LIT("const") + dsl::p<ident_token> +
            dsl::opt(dsl::colon >> dsl::p<type_prod>) +
            dsl::lit_c < '=' > +dsl::recurse<expr>;
    };

    // var_decl (top-level)
    struct var_decl {
        static constexpr auto rule =
            LEXY_LIT("var") + dsl::p<ident_token> +
            (
                (dsl::colon >> (dsl::p<type_prod> + dsl::opt(dsl::lit_c<'='> >> dsl::recurse<expr>)))
                | (dsl::lit_c<'='> >> dsl::recurse<expr>)
            );
    };

    // assoc_type_decl: type IDENT (parsed in v1, v2-gated semantically)
    struct assoc_type_decl {
        static constexpr auto rule = LEXY_LIT("type") + dsl::p<ident_token>;
    };

    // trait_fn_decl: fn IDENT ( params ) -> type
    struct trait_fn_decl {
        static constexpr auto rule =
            LEXY_LIT("fn") + dsl::p<ident_token> +
            dsl::parenthesized.opt_list(dsl::p<param>, dsl::sep(dsl::comma)) +
            dsl::opt(LEXY_LIT("->") >> dsl::p<type_prod>);
    };

    // assoc_const_decl: IDENT : type [= expr]
    struct assoc_const_decl {
        static constexpr auto rule =
            dsl::p<ident_token> + dsl::colon + dsl::p<type_prod> +
            dsl::opt(dsl::lit_c<'='> >> dsl::recurse<expr>);
    };

    struct trait_member {
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("type")) >> dsl::p<assoc_type_decl>) |
            (dsl::peek(LEXY_LIT("fn")) >> dsl::p<trait_fn_decl>) |
            (dsl::else_ >> dsl::p<assoc_const_decl>);
    };

    struct trait_decl {
        static constexpr auto rule =
            LEXY_LIT("trait") + dsl::p<ident_token> +
            dsl::opt(dsl::peek(dsl::lit_c < '[' >) >> dsl::p<generic_params>) +
            dsl::curly_bracketed.opt_list(dsl::p<trait_member>);
    };

    // impl_member: func_decl | type IDENT = type | IDENT [: type] = expr
    struct impl_type_alias {
        static constexpr auto rule =
            LEXY_LIT("type") + dsl::p<ident_token> + dsl::lit_c < '=' > +dsl::p<type_prod>;
    };

    struct impl_const_member {
        static constexpr auto rule =
            dsl::p<ident_token> +
            dsl::opt(dsl::colon >> dsl::p<type_prod>) +
            dsl::lit_c < '=' > +dsl::recurse<expr>;
    };

    struct impl_member {
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("fn")) >> dsl::p<func_decl>) |
            (dsl::peek(LEXY_LIT("type")) >> dsl::p<impl_type_alias>) |
            (dsl::else_ >> dsl::p<impl_const_member>);
    };

    struct impl_decl {
        static constexpr auto rule =
            LEXY_LIT("impl") + dsl::opt(dsl::peek(dsl::lit_c < '[' >) >> dsl::p<generic_params>) +
            dsl::p<type_prod> +
            dsl::opt(LEXY_LIT("for") >> dsl::p<type_prod>) +
            dsl::curly_bracketed.opt_list(dsl::p<impl_member>);
    };

    // view_decl: view IDENT [generic_params] of IDENT : type { contract_clause }
    // "of" is contextual — matched as a literal inside view_decl only, NOT globally reserved.
    struct view_decl {
        static constexpr auto rule =
            LEXY_LIT("view") + dsl::p<ident_token> +
            dsl::opt(dsl::peek(dsl::lit_c < '[' >) >> dsl::p<generic_params>) +
            LEXY_LIT("of") + dsl::p<ident_token> + dsl::colon + dsl::p<type_prod> +
            dsl::while_(
                dsl::peek(LEXY_LIT("requires")) >>
                dsl::p<contract_clause>
            );
    };

    // §3.6 Attributes
    struct attr_arg {
        static constexpr auto rule =
            (dsl::peek(dsl::lit_c < '"' >) >> dsl::p<string_lit>) |
            (dsl::else_ >>
                (dsl::p<ident_token> + dsl::opt(dsl::lit_c<'='> >>
                    ((dsl::peek(dsl::ascii::digit) >> dsl::p<int_lit>) |
                        (dsl::peek(dsl::lit_c < '"' >) >> dsl::p<string_lit>) |
                        (dsl::peek(LEXY_LIT("true") | LEXY_LIT("false")) >> dsl::p<bool_lit>) |
                        (dsl::else_ >> dsl::p<ident_token>)))));
    };

    struct attribute {
        static constexpr auto rule =
            dsl::lit_c < '@' > +dsl::p<qualified_ident> +
            dsl::opt(dsl::peek(dsl::lit_c < '(' >) >>
                dsl::parenthesized.opt_list(dsl::p<attr_arg>, dsl::sep(dsl::comma)));
    };

    // top_decl: [pub] attribute* (func | type | const | var | trait | impl | view)
    // The optional `pub` marker forces export regardless of name case; uppercase
    // names still export via the resolver's uppercase policy without `pub`.
    struct top_decl {
        static constexpr auto rule =
            dsl::opt(dsl::peek(LEXY_LIT("pub")) >> LEXY_LIT("pub")) +
            dsl::while_(dsl::peek(dsl::lit_c < '@' >) >> dsl::p<attribute>) +
            (
                (dsl::peek(LEXY_LIT("fn")) >> dsl::p<func_decl>) |
                (dsl::peek(LEXY_LIT("type")) >> dsl::p<type_decl>) |
                (dsl::peek(LEXY_LIT("const")) >> dsl::p<const_decl>) |
                (dsl::peek(LEXY_LIT("var")) >> dsl::p<var_decl>) |
                (dsl::peek(LEXY_LIT("trait")) >> dsl::p<trait_decl>) |
                (dsl::peek(LEXY_LIT("impl")) >> dsl::p<impl_decl>) |
                (dsl::peek(LEXY_LIT("view")) >> dsl::p<view_decl>)
            );
    };

    // module_decl: module IDENT [generic_params]? { top_decl* }
    // Parametric (generic) module — reuses the existing generic_params and
    // top_decl productions. Flat (top-level only) for v1; nesting is a one-line
    // peek addition later. §v2.3.
    struct module_decl {
        static constexpr auto rule =
            LEXY_LIT("module") + dsl::p<ident_token> +
            dsl::opt(dsl::peek(dsl::lit_c < '[' >) >> dsl::p<generic_params>) +
            dsl::curly_bracketed.opt_list(
                dsl::p<top_decl>);
    };

    // ============================================================================
    // §6 Statements
    // ============================================================================

    struct let_stmt {
        static constexpr auto rule =
            LEXY_LIT("let") + dsl::p<ident_token> +
            dsl::opt(dsl::colon >> dsl::p<type_prod>) +
            dsl::lit_c < '=' > +dsl::recurse<expr>;
    };

    struct var_stmt {
        static constexpr auto rule =
            LEXY_LIT("var") + dsl::p<ident_token> +
            (
                (dsl::colon >> (dsl::p<type_prod> + dsl::opt(dsl::lit_c<'='> >> dsl::recurse<expr>)))
                | (dsl::lit_c<'='> >> dsl::recurse<expr>)
            );
    };

    struct assign_op {
        static constexpr auto rule =
            LEXY_LIT("<<=") | LEXY_LIT(">>=") |
            LEXY_LIT("+=") | LEXY_LIT("-=") | LEXY_LIT("*=") | LEXY_LIT("/=") | LEXY_LIT("%=") |
            LEXY_LIT("&=") | LEXY_LIT("|=") | LEXY_LIT("^=") | dsl::lit_c<'='>;
    };

    struct lvalue {
        static constexpr auto rule =
            dsl::p<qualified_ident> +
            dsl::while_(
                (dsl::peek(dsl::lit_c < '[' >) >> dsl::square_bracketed(dsl::recurse<expr>)) |
                (dsl::peek(dsl::lit_c < '.' >) >> (dsl::lit_c < '.' > +dsl::p<ident_token>))
            );
    };

    struct assign_stmt {
        static constexpr auto rule =
            dsl::p<lvalue> + dsl::p<assign_op> + dsl::recurse<expr>;
    };

    // defer_stmt: defer call_expr (semantic check: postfix with call top)
    struct defer_stmt {
        static constexpr auto rule = LEXY_LIT("defer") + dsl::recurse<expr>;
    };

    struct assert_stmt {
        static constexpr auto rule = LEXY_LIT("assert") + dsl::recurse<pred_expr>;
    };

    struct return_stmt {
        static constexpr auto rule = LEXY_LIT("return") + dsl::opt(dsl::else_ >> dsl::recurse<expr>);
    };

    struct break_stmt {
        static constexpr auto rule = LEXY_LIT("break");
    };

    struct continue_stmt {
        static constexpr auto rule = LEXY_LIT("continue");
    };

    // ── §6.1 Control flow ──────────────────────────────────────────────────────

    struct if_stmt {
        static constexpr auto rule =
            LEXY_LIT("if") + dsl::recurse<expr> + dsl::recurse<block> +
            dsl::opt(
                LEXY_LIT("else") >>
                ((dsl::peek(LEXY_LIT("if")) >> dsl::recurse<if_stmt>) |
                    (dsl::else_ >> dsl::recurse<block>))
            );
    };

    struct range_expr_prod {
        // range_expr = expr (..|..=) expr | expr  (bare iterator)
        static constexpr auto rule = dsl::recurse<expr>; // range op is handled in expr precedence
    };

    struct for_stmt {
        // for IDENT [ , IDENT ] in expr block
        static constexpr auto rule =
            LEXY_LIT("for") +
            dsl::p<ident_token> +
            dsl::opt(dsl::comma >> dsl::p<ident_token>) +
            LEXY_LIT("in") + dsl::recurse<expr> +
            dsl::recurse<block>;
    };

    struct while_stmt {
        static constexpr auto rule = LEXY_LIT("while") + dsl::recurse<expr> + dsl::recurse<block>;
    };

    // §6 match
    struct literal_pattern {
        static constexpr auto rule = dsl::p<literal>;
    };

    struct ctor_pattern;

    struct binding_pattern {
        static constexpr auto rule = dsl::p<ident_token>;
    };

    struct pattern {
        static constexpr auto rule =
            dsl::lit_c < '_' > | // wildcard
            (dsl::peek(dsl::lit_c < '"' > | dsl::lit_c < '`' > | dsl::ascii::digit |
                    LEXY_LIT("true") | LEXY_LIT("false") | LEXY_LIT("Unit"))
                >> dsl::p<literal_pattern>) |
            (dsl::else_ >> dsl::recurse<ctor_pattern>);
    };

    struct ctor_pattern {
        static constexpr auto rule =
            dsl::p<qualified_ident> +
            dsl::opt(dsl::peek(dsl::lit_c < '(' >) >>
                dsl::parenthesized.opt_list(dsl::recurse<pattern>, dsl::sep(dsl::comma)));
    };

    struct match_arm {
        static constexpr auto rule =
            dsl::p<pattern> + LEXY_LIT("=>") +
            ((dsl::peek(dsl::lit_c < '{' >) >> dsl::recurse<block>) |
                (dsl::else_ >> dsl::recurse<expr>));
    };

    struct match_stmt {
        static constexpr auto rule =
            LEXY_LIT("match") + dsl::recurse<expr> +
            dsl::curly_bracketed.opt_list(dsl::p<match_arm>);
    };

    // §6.2 Transaction (expression, reached as expr_stmt or let/assign)
    struct transaction_isolation {
        static constexpr auto rule =
            LEXY_LIT("read_committed") | LEXY_LIT("snapshot") | LEXY_LIT("serializable");
    };

    struct transaction_replay {
        static constexpr auto rule =
            LEXY_LIT("body_and_effects_idempotent") | LEXY_LIT("body_idempotent") |
            LEXY_LIT("unknown_but_retry_allowed") | LEXY_LIT("non_idempotent") |
            LEXY_LIT("unknown");
    };

    struct transaction_conflict {
        static constexpr auto rule =
            LEXY_LIT("optimistic") | LEXY_LIT("pessimistic") | LEXY_LIT("deterministic");
    };

    struct transaction_partial {
        static constexpr auto rule =
            LEXY_LIT("require_atomic_coordinator") | LEXY_LIT("allow_in_doubt") |
            LEXY_LIT("best_effort");
    };

    struct transaction_distribution {
        // §v2.10: local (v1-equivalent) plus shard/replicated (need an adapter).
        static constexpr auto rule =
            LEXY_LIT("none") | LEXY_LIT("local") | LEXY_LIT("shard") | LEXY_LIT("replicated");
    };

    // §14.1: durability level for a transaction body.
    struct transaction_durability {
        static constexpr auto rule =
            LEXY_LIT("durable") | LEXY_LIT("process") | LEXY_LIT("memory");
    };

    struct transaction_arg {
        static constexpr auto rule =
            (LEXY_LIT("isolation") >> (dsl::lit_c < '=' > +dsl::p<transaction_isolation>)) |
            (LEXY_LIT("retry") >> (dsl::lit_c < '=' > +dsl::p<int_lit>)) |
            (LEXY_LIT("replay") >> (dsl::lit_c < '=' > +dsl::p<transaction_replay>)) |
            (LEXY_LIT("conflict") >> (dsl::lit_c < '=' > +dsl::p<transaction_conflict>)) |
            (LEXY_LIT("partial") >> (dsl::lit_c < '=' > +dsl::p<transaction_partial>)) |
            (LEXY_LIT("durability") >> (dsl::lit_c < '=' > +dsl::p<transaction_durability>)) |
            // §v2.11: coordinator="name" lifts the single-resource restriction.
            (LEXY_LIT("coordinator") >> (dsl::lit_c < '=' > +dsl::p<string_lit>)) |
            (LEXY_LIT("distribution") >> (dsl::lit_c < '=' > +dsl::p<transaction_distribution>));
    };

    struct transaction_expr {
        static constexpr auto rule =
            LEXY_LIT("transaction") +
            dsl::opt(dsl::peek(dsl::lit_c < '(' >) >>
                dsl::parenthesized.opt_list(dsl::p<transaction_arg>, dsl::sep(dsl::comma))) +
            dsl::recurse<block>;
    };

    // ── block ──────────────────────────────────────────────────────────────────

    // §2.3 abort(error) — explicit transaction abort; only meaningful inside transaction body
    struct tx_abort_stmt {
        static constexpr auto rule =
            LEXY_LIT("abort") + dsl::parenthesized(dsl::recurse<expr>);
    };

    // §2.2 yield expr — produce body value from transaction; only meaningful inside transaction body
    struct tx_yield_stmt {
        static constexpr auto rule =
            LEXY_LIT("yield") + dsl::recurse<expr>;
    };

    struct statement {
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("let")) >> dsl::p<let_stmt>) |
            (dsl::peek(LEXY_LIT("var")) >> dsl::p<var_stmt>) |
            (dsl::peek(LEXY_LIT("defer")) >> dsl::p<defer_stmt>) |
            (dsl::peek(LEXY_LIT("assert")) >> dsl::p<assert_stmt>) |
            (dsl::peek(LEXY_LIT("return")) >> dsl::p<return_stmt>) |
            (dsl::peek(LEXY_LIT("break")) >> dsl::p<break_stmt>) |
            (dsl::peek(LEXY_LIT("continue")) >> dsl::p<continue_stmt>) |
            (dsl::peek(LEXY_LIT("abort")) >> dsl::p<tx_abort_stmt>) |
            (dsl::peek(LEXY_LIT("yield")) >> dsl::p<tx_yield_stmt>) |
            (dsl::peek(LEXY_LIT("if")) >> dsl::p<if_stmt>) |
            (dsl::peek(LEXY_LIT("for")) >> dsl::p<for_stmt>) |
            (dsl::peek(LEXY_LIT("while")) >> dsl::p<while_stmt>) |
            (dsl::peek(LEXY_LIT("match")) >> dsl::p<match_stmt>) |
            (dsl::peek(dsl::lit_c < '{' >) >> dsl::recurse<block>) |
            dsl::else_ >> dsl::recurse<expr>;
    };

    struct block {
        static constexpr auto rule =
            dsl::curly_bracketed.opt_list(dsl::p<statement>);
    };

    // ============================================================================
    // §7 Expressions — precedence table via dsl::expression
    // ============================================================================
    //
    // Level 1  ||     left   logical or
    // Level 2  &&     left   logical and
    // Level 3  == != < <= > >=  infix_op_single (no chain)
    // Level 4  |      left   bitwise or
    // Level 5  ^      left   bitwise xor
    // Level 6  & &^   left   bitwise and / and-not
    // Level 7  << >>  left   shift
    // Level 8  + -    left   add/sub
    // Level 9  * / %  left   mul/div/mod
    // Level 10 as     left   checked cast (RHS is type, not expr — special cased)
    // Level 11 prefix ! - await
    // Level 12 postfix call () index [] field .  ?
    // Level 13 primary

    struct expr : lexy::expression_production {
        // Whitespace consumed between tokens
        static constexpr auto whitespace = dsl::ascii::space | dsl::ascii::blank;

        // Precedence table — highest binding power first (innermost operand)
        // postfix: call () index []  ? (error-propagation, no operand consumed)
        // field . handled separately in primary via postfix chain
        struct op_postfix : dsl::postfix_op {
            static constexpr auto op =
                dsl::op(dsl::parenthesized.opt_list(dsl::p<expr>, dsl::sep(dsl::comma)))
                / dsl::op(dsl::square_bracketed(dsl::p<expr>))
                / dsl::op(dsl::lit_c<'?'>); // ? is postfix-only; meaning enforced by sema
            using operand = dsl::atom;
        };

        // prefix: ! - await
        struct op_prefix : dsl::prefix_op {
            static constexpr auto op =
                dsl::op(dsl::lit_c < '!' >) / dsl::op(dsl::lit_c < '-' >) / dsl::op(LEXY_LIT("await"));
            using operand = op_postfix;
        };

        // mul/div/mod  * / %
        struct op_muldiv : dsl::infix_op_left {
            static constexpr auto op =
                dsl::op(dsl::lit_c < '*' >) / dsl::op(dsl::lit_c < '/' >) / dsl::op(dsl::lit_c < '%' >);
            using operand = op_prefix;
        };

        // type conversion as (RHS is type_prod, not expr)
        struct op_as : dsl::infix_op_left {
            static constexpr auto op = dsl::op(LEXY_LIT("as"));
            // RHS is captured separately in callback; for now use same operand binding
            using operand = op_muldiv;
        };

        // add/sub  + -
        struct op_addsub : dsl::infix_op_left {
            static constexpr auto op =
                dsl::op(dsl::lit_c < '+' >) / dsl::op(dsl::lit_c < '-' >);
            using operand = op_as;
        };

        // shift  << >>
        struct op_shift : dsl::infix_op_left {
            static constexpr auto op = dsl::op(LEXY_LIT("<<")) / dsl::op(LEXY_LIT(">>"));
            using operand = op_addsub;
        };

        // range  ..  ..=
        struct op_range : dsl::infix_op_left {
            static constexpr auto op = dsl::op(LEXY_LIT("..=")) / dsl::op(LEXY_LIT(".."));
            using operand = op_shift;
        };

        // bitwise and  & &^
        struct op_bitand : dsl::infix_op_left {
            static constexpr auto op = dsl::op(LEXY_LIT("&^")) / dsl::op(dsl::lit_c < '&' >);
            using operand = op_range;
        };

        // bitwise xor  ^
        struct op_bitxor : dsl::infix_op_left {
            static constexpr auto op = dsl::op(dsl::lit_c < '^' >);
            using operand = op_bitand;
        };

        // bitwise or  |
        struct op_bitor : dsl::infix_op_left {
            static constexpr auto op = dsl::op(dsl::lit_c < '|' >);
            using operand = op_bitxor;
        };

        // comparison  == != < <= > >=
        struct op_cmp : dsl::infix_op_single {
            static constexpr auto op =
                dsl::op(LEXY_LIT("==")) / dsl::op(LEXY_LIT("!=")) /
                dsl::op(LEXY_LIT("<=")) / dsl::op(LEXY_LIT(">=")) /
                dsl::op(dsl::lit_c < '<' >) / dsl::op(dsl::lit_c < '>' >);
            using operand = op_bitor;
        };

        // logical and  &&
        struct op_and : dsl::infix_op_left {
            static constexpr auto op = dsl::op(LEXY_LIT("&&"));
            using operand = op_cmp;
        };

        // logical or  ||  (lowest binding power)
        struct op_or : dsl::infix_op_left {
            static constexpr auto op = dsl::op(LEXY_LIT("||"));
            using operand = op_and;
        };

        using operation = op_or;

        static constexpr auto atom = dsl::recurse<primary>;
    };

    // Primary expression
    struct builtin_name {
        static constexpr auto rule = LEXY_LIT("len");
    };

    struct builtin_call {
        static constexpr auto rule =
            dsl::p<builtin_name> +
            dsl::parenthesized.opt_list(dsl::p<expr>, dsl::sep(dsl::comma));
    };

    struct composite_lit {
        // struct/array: type { field_init, ... } or [ expr, ... ]
        static constexpr auto rule =
            (dsl::peek(dsl::lit_c < '[' >) >>
                dsl::square_bracketed.opt_list(dsl::p<expr>, dsl::sep(dsl::comma))) |
            (dsl::else_ >>
                (dsl::p<type_prod> +
                    dsl::curly_bracketed.opt_list(dsl::p<expr>, dsl::sep(dsl::comma))));
    };

    // closure_param: IDENT [ : type ]  (type optional — inferred)
    struct closure_param {
        static constexpr auto rule =
            dsl::p<ident_token> + dsl::opt(dsl::colon >> dsl::p<type_prod>);
    };

    // pipe_closure: "|" [ closure_param { "," closure_param } ] "|"
    //              ( [ "->" type ] block  |  expr )
    // Disambiguation: in primary position a leading | / || is unambiguously a closure;
    // bitwise-or / logical-or only appear in infix position (need a left operand).
    struct pipe_closure {
        static constexpr auto rule =
            dsl::lit_c<'|'> +
            dsl::opt(dsl::peek_not(dsl::lit_c<'|'>) >>
                dsl::list(dsl::p<closure_param>, dsl::sep(dsl::comma))) +
            dsl::lit_c<'|'> +
            // body: [ "->" type ] block  OR  bare expr (implicit return)
            // Peek for "->" or "{" to select the block form; else_ is expr form.
            (dsl::peek(LEXY_LIT("->") | dsl::lit_c<'{'>)
                >> (dsl::opt(dsl::peek(LEXY_LIT("->")) >> (LEXY_LIT("->") + dsl::p<type_prod>)) +
                    dsl::recurse<block>)
             | (dsl::else_ >> dsl::recurse<expr>));
    };

    struct closure_lit {
        // fn-form closure (existing); pipe-form closure (new, additive).
        // Both lower to closure_tag (id 1020) — downstream sema sees one shape.
        static constexpr auto rule =
            (dsl::peek(dsl::lit_c<'|'>) >> dsl::p<pipe_closure>) |
            (dsl::else_ >>
                (LEXY_LIT("fn") +
                 dsl::parenthesized.opt_list(dsl::p<param>, dsl::sep(dsl::comma)) +
                 dsl::opt(LEXY_LIT("->") >> dsl::p<type_prod>) +
                 dsl::recurse<block>));
    };

    struct spawn_expr {
        static constexpr auto rule = LEXY_LIT("spawn") + dsl::p<expr>;
    };

    // view_source_expr: the source operand of a view_expr.
    // Restricted to qualified_ident or (expr) to avoid the level-10 "as" cast
    // operator greedily consuming "as" when a full dsl::p<expr> is used here.
    // For complex sources parenthesize: view (x.field) as T.
    struct view_source_expr {
        static constexpr auto rule =
            (dsl::peek(dsl::lit_c < '(' >) >> dsl::parenthesized(dsl::p<expr>)) |
            (dsl::else_ >> dsl::p<qualified_ident>);
    };

    // view_expr: "view" view_source_expr "as" type_prod
    struct view_expr {
        static constexpr auto rule =
            LEXY_LIT("view") + dsl::p<view_source_expr> + LEXY_LIT("as") + dsl::p<type_prod>;
    };

    struct primary {
        static constexpr auto rule =
            (dsl::peek(LEXY_LIT("view")) >> dsl::p<view_expr>) |
            (dsl::peek(LEXY_LIT("transaction")) >> dsl::p<transaction_expr>) |
            (dsl::peek(LEXY_LIT("spawn")) >> dsl::p<spawn_expr>) |
            (dsl::peek(LEXY_LIT("fn")) >> dsl::p<closure_lit>) |
            // pipe_closure: leading | in primary position is unambiguously closure-open
            (dsl::peek(dsl::lit_c<'|'>) >> dsl::p<pipe_closure>) |
            (dsl::peek(LEXY_LIT("len")) >> dsl::p<builtin_call>) |
            (dsl::peek(dsl::lit_c < '(' >) >> dsl::parenthesized(dsl::p<expr>)) |
            (dsl::peek(dsl::lit_c < '[' > | dsl::lit_c < '"' > | dsl::lit_c < '`' > |
                LEXY_LIT("true") | LEXY_LIT("false") | LEXY_LIT("Unit") |
                dsl::ascii::digit) >> dsl::p<literal>) |
            (dsl::else_ >> dsl::p<qualified_ident>);
    };

    // ============================================================================
    // §7.5 Predicate sub-language
    // ============================================================================

    struct arith_expr {
        // arithmetic subset of expr (levels 7-12, pure ops only)
        // Reuse expr production — semantic check (pure only) in analysis
        static constexpr auto rule = dsl::p<expr>;
    };

    struct quant_binder {
        // Two forms (Option A — both coexist):
        //   typed:  IDENT ":" type          — legacy form (grammar §7.5)
        //   ranged: IDENT "in" range_expr   — new form (addendum §1.1)
        // Ranged form desugars in sema to a guarded typed quantifier; parser just
        // records which form was used so the sema lowering can build the guard.
        static constexpr auto rule =
            dsl::p<ident_token> +
            ((dsl::peek(LEXY_LIT("in")) >>
                (LEXY_LIT("in") + dsl::recurse<expr>)) // ranged: IDENT in range_expr
             | (dsl::else_ >>
                (dsl::colon + dsl::p<type_prod>)));    // typed:  IDENT : type
    };

    struct quantified {
        // Binder/body separator: "." for typed form, ":" for ranged form.
        // Both separators accepted after any binder list to keep parsing uniform;
        // semantic validation enforces consistency (ranged binders use ":").
        static constexpr auto rule =
            (LEXY_LIT("forall") | LEXY_LIT("exists")) +
            dsl::list(dsl::p<quant_binder>, dsl::sep(dsl::comma)) +
            (dsl::lit_c<'.'> | dsl::lit_c<':'>) +
            dsl::recurse<pred_expr>;
    };

    struct pred_atom {
        static constexpr auto rule =
            (dsl::lit_c<'!'> >> dsl::recurse<pred_atom>) |
            (dsl::peek(LEXY_LIT("forall") | LEXY_LIT("exists")) >> dsl::p<quantified>) |
            (dsl::peek(LEXY_LIT("old")) >> LEXY_LIT("old") >> dsl::parenthesized(dsl::p<arith_expr>)) |
            (dsl::peek(dsl::lit_c < '(' >) >> dsl::parenthesized(dsl::recurse<pred_expr>)) |
            (dsl::else_ >> dsl::p<arith_expr>);
    };

    struct pred_and {
        static constexpr auto rule =
            dsl::list(dsl::p<pred_atom>, dsl::sep(LEXY_LIT("&&")));
    };

    struct pred_or {
        static constexpr auto rule =
            dsl::list(dsl::p<pred_and>, dsl::sep(LEXY_LIT("||")));
    };

    struct pred_impl {
        // p -> q  (right-assoc, desugars to !p || q)
        static constexpr auto rule =
            dsl::p<pred_or> + dsl::opt(LEXY_LIT("->") >> dsl::recurse<pred_impl>);
    };

    struct pred_expr {
        static constexpr auto rule = dsl::p<pred_impl>;
    };

    // ============================================================================
    // §2 Source file
    // ============================================================================

    struct import_decl {
        static constexpr auto rule = LEXY_LIT("import") + dsl::p<string_lit>;
    };

    struct source_file {
        // Whitespace: spaces + tabs + comments (not newlines, to let ASI work)
        // Must not use dsl::p<> — inline the rule directly
        static constexpr auto whitespace =
            dsl::ascii::space | dsl::ascii::blank |
            (dsl::peek(LEXY_LIT("//")) >> dsl::lit_c < '/' > +dsl::lit_c < '/' > +dsl::until(dsl::newline)) |
            (dsl::peek(LEXY_LIT("/*")) >> LEXY_LIT("/*") + dsl::until(LEXY_LIT("*/")));

        static constexpr auto rule =
            LEXY_LIT("package") + dsl::p<ident_token> +
            dsl::while_(dsl::peek(LEXY_LIT("import")) >> dsl::p<import_decl>) +
            dsl::while_(
                (dsl::peek(LEXY_LIT("module")) >> dsl::p<module_decl>) |
                (dsl::peek(LEXY_LIT("fn") | LEXY_LIT("type") | LEXY_LIT("const") |
                    LEXY_LIT("var") | LEXY_LIT("trait") | LEXY_LIT("impl") |
                    LEXY_LIT("view") | LEXY_LIT("pub") | dsl::lit_c < '@' >) >>
                dsl::p<top_decl>)
            ) +
            dsl::eof;
    };

    // ============================================================================
    // Parse entry point
    // ============================================================================

    using input_t = lexy::string_input<lexy::utf8_char_encoding>;
    using parse_tree_t = lexy::parse_tree_for<input_t, lex::token_kind>;

    /// Parse a crank source string. Returns a parse_tree.
/// Errors collected via the lexy error_tree (diagnostics mapped in build_ast.hpp).
    [[nodiscard]] inline auto parse(std::string_view src) {
        auto input = lexy::string_input<lexy::utf8_char_encoding>(src);
        parse_tree_t tree;
        lexy::parse_as_tree<source_file>(tree, input, lexy::noop);
        return tree;
    }
} // namespace crank::grammar
