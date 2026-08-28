#pragma once

// crank/lexer.hpp — Crank lexical definitions + ASI state structures.
//
// C++23, header-only, no virtual, no macros.
// Namespace: crank::lex
//
// Provides:
//   - token_kind enum
//   - Token productions (lexy rule structs) used by parser.hpp
//   - strip_digit_sep() helper (remove '_' from numeric literals)
//   - ASI context types (line_continues_flag, bracket_depth_counter)
//
// Tokens: IDENT (keyword .reserve), INT_LIT (dec/hex/oct/bin, _ stripped),
//         FLOAT_LIT, STRING_LIT, RAW_STRING_LIT, BOOL_LIT, operators/punctuation.
// Comments and whitespace are trivia (retained on parse_tree).
// ASI: context_flag + context_counter in parser whitespace production:
//   At NEWLINE: !line_continues && depth==0 → emit synthetic stmt_term.
//   Bracket depth suppresses ASI inside ()[] (grammar §8.4).
//   else/. carve-out handled in parser.hpp.

#include <lexy/dsl.hpp>
#include <lexy/encoding.hpp>
#include <lexy/lexeme.hpp>
#include <lexy/token.hpp>
#include <lexy/callback.hpp>

#include "languages/generic/lexer/digit_sep.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace crank::lex {
    // ============================================================================
    // token_kind
    // ============================================================================

    enum class token_kind : std::uint16_t {
        // literals
        ident = 0,
        int_lit,
        float_lit,
        string_lit,
        raw_string_lit,
        bool_lit,

        // keywords (distinct from ident after reserve())
        kw_package, kw_import, kw_fn, kw_let, kw_var, kw_const,
        kw_type, kw_struct, kw_enum,
        kw_if, kw_else, kw_for, kw_while, kw_match,
        kw_return, kw_break, kw_continue,
        kw_in, kw_as,
        kw_true, kw_false, kw_Unit,
        kw_await, kw_spawn, kw_defer, kw_transaction,
        kw_requires, kw_ensures, kw_assert,
        kw_trait, kw_impl,
        kw_view,

        // operators / punctuation
        op_plus, op_minus, op_star, op_slash, op_percent,
        op_eq_eq, op_bang_eq, op_lt, op_lt_eq, op_gt, op_gt_eq,
        op_and_and, op_or_or, op_bang,
        op_amp, op_pipe, op_caret, op_lt_lt, op_gt_gt, op_amp_caret,
        op_eq, op_colon_eq,
        op_plus_eq, op_minus_eq, op_star_eq, op_slash_eq, op_percent_eq,
        op_amp_eq, op_pipe_eq, op_caret_eq, op_lt_lt_eq, op_gt_gt_eq,
        op_arrow, // ->
        op_fat_arrow, // =>
        op_dot_dot, // ..
        op_dot_dot_eq, // ..=
        op_question, // ? — postfix error-propagation
        op_dot, op_comma, op_colon, op_at,

        // brackets
        punc_lparen, punc_rparen, punc_lbracket, punc_rbracket,
        punc_lbrace, punc_rbrace,

        // synthetic (ASI)
        stmt_term,

        // trivia
        trivia_ws, trivia_newline, trivia_line_comment, trivia_block_comment,

        unknown,
    };

    // ============================================================================
    // strip_digit_sep — re-exported from lang (generic layer)
    // ============================================================================

    using lang::strip_digit_sep;

    // ============================================================================
    // Token productions
    // ============================================================================

    namespace tokens {
        namespace dsl = lexy::dsl;

        // ── Comments ─────────────────────────────────────────────────────────────────

        struct line_comment {
            static constexpr auto rule =
                dsl::lit_c < '/' > +dsl::lit_c < '/' > +dsl::until(dsl::newline);
        };

        struct block_comment {
            static constexpr auto rule =
                LEXY_LIT("/*") + dsl::until(LEXY_LIT("*/"));
        };

        // ── Identifier ───────────────────────────────────────────────────────────────

        struct ident {
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
                    LEXY_LIT("view")
                );
            }();
            static constexpr auto value = lexy::as_string<std::string>;
        };

        // ── Integer literals ─────────────────────────────────────────────────────────

        struct hex_int_lit {
            static constexpr auto rule =
                dsl::peek(LEXY_LIT("0x") | LEXY_LIT("0X")) >>
                dsl::capture(dsl::token(LEXY_LIT("0x") + dsl::digits<lexy::dsl::hex>.sep(dsl::lit_c < '_' >)));
            static constexpr auto value = lexy::callback<std::string>(
                [](auto lex) {
                    auto sv = std::string_view(lex.begin(), lex.end());
                    return "0x" + strip_digit_sep(sv.substr(2));
                });
        };

        struct oct_int_lit {
            static constexpr auto rule =
                dsl::peek(LEXY_LIT("0o")) >>
                dsl::capture(dsl::token(LEXY_LIT("0o") + dsl::digits<lexy::dsl::octal>.sep(dsl::lit_c < '_' >)));
            static constexpr auto value = lexy::callback<std::string>(
                [](auto lex) {
                    auto sv = std::string_view(lex.begin(), lex.end());
                    return "0o" + strip_digit_sep(sv.substr(2));
                });
        };

        struct bin_int_lit {
            static constexpr auto rule =
                dsl::peek(LEXY_LIT("0b")) >>
                dsl::capture(dsl::token(LEXY_LIT("0b") + dsl::digits<lexy::dsl::binary>.sep(dsl::lit_c < '_' >)));
            static constexpr auto value = lexy::callback<std::string>(
                [](auto lex) {
                    auto sv = std::string_view(lex.begin(), lex.end());
                    return "0b" + strip_digit_sep(sv.substr(2));
                });
        };

        struct dec_int_lit {
            static constexpr auto rule =
                dsl::capture(dsl::digits<lexy::dsl::decimal>.sep(dsl::lit_c < '_' >));
            static constexpr auto value = lexy::callback<std::string>(
                [](auto lex) {
                    return strip_digit_sep(std::string_view(lex.begin(), lex.end()));
                });
        };

        struct int_lit {
            static constexpr auto rule =
                dsl::p<hex_int_lit> | dsl::p<oct_int_lit> | dsl::p<bin_int_lit> | dsl::p<dec_int_lit>;
            static constexpr auto value = lexy::forward<std::string>;
        };

        // ── Float literal ─────────────────────────────────────────────────────────

        // Float: digits '.' digits [exponent] | digits exponent
        // exponent = (e|E) [+|-] digits
        // dsl::opt requires branch rules — use peek >> for optional parts.
        struct float_lit {
            static constexpr auto exponent =
                dsl::token((dsl::lit_c < 'e' > | dsl::lit_c < 'E' >) +
                           dsl::opt(dsl::peek(dsl::lit_c < '+' > | dsl::lit_c < '-' >) >>
                               (dsl::lit_c < '+' > | dsl::lit_c < '-' >)) +
                           dsl::digits<>);

            static constexpr auto rule = dsl::capture(dsl::token(
                dsl::digits<>.sep(dsl::lit_c < '_' >) +
                (
                    dsl::token(dsl::lit_c < '.' > +dsl::digits<>.sep(dsl::lit_c < '_' >) +
                        dsl::opt(dsl::peek(dsl::lit_c < 'e' > | dsl::lit_c < 'E' >) >> exponent))
                    |
                    (dsl::peek(dsl::lit_c < 'e' > | dsl::lit_c < 'E' >) >> exponent)
                )
            ));
            static constexpr auto value = lexy::callback<std::string>(
                [](auto lex) {
                    return strip_digit_sep(std::string_view(lex.begin(), lex.end()));
                });
        };

        // ── String literals ──────────────────────────────────────────────────────────

        struct string_lit {
            // Symbol table for common single-char escapes (grammar §3.4)
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
            static constexpr auto rule = dsl::quoted(dsl::ascii::print, escaped);
            static constexpr auto value = lexy::as_string<std::string>;
        };

        struct raw_string_lit {
            static constexpr auto rule = dsl::delimited(dsl::lit_c < '`' >)(dsl::ascii::print);
            static constexpr auto value = lexy::as_string<std::string>;
        };

        // ── Bool literal ─────────────────────────────────────────────────────────────

        struct bool_lit {
            static constexpr auto rule = LEXY_LIT("true") | LEXY_LIT("false");
            static constexpr auto value = lexy::as_string<std::string>;
        };
    } // namespace tokens

    // ============================================================================
    // ASI context types
    // ============================================================================
    //
    // These tag types are used with lexy's dsl::context_flag / context_counter.
    // The parser's whitespace production (parser.hpp) uses them to implement ASI:
    //
    //   line_continues_flag  — set when last significant token continues a line
    //   bracket_depth_counter — tracks ( [ { depth (0 = top level)
    //
    // At a NEWLINE in the whitespace rule:
    //   if (!line_continues && bracket_depth == 0) → emit synthetic stmt_term
    //   else                                        → newline is trivia

    struct line_continues_flag {}; // tag for dsl::context_flag<line_continues_flag>
    struct bracket_depth_counter {}; // tag for dsl::context_counter<bracket_depth_counter, int>
} // namespace crank::lex
