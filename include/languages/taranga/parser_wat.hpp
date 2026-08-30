#pragma once

// taranga/parser_wat.hpp — lexy grammar for the WebAssembly text format (WAT).
//
// C++23, header-only, no virtual, no macros (LEXY_LIT is a lexy library macro).
// Namespace: taranga::grammar
//
// WAT is a fully-parenthesised S-expression language: a module is a tree of
// lists whose head is a keyword. Unlike a full expression language, there is no
// operator precedence — structure is entirely explicit parens. So the grammar
// is tiny: whitespace/comments, atoms (keyword / id / string / number), and a
// recursive parenthesised list. build_ast.hpp walks the resulting parse_tree and
// reflects it onto the generic AST using the head keyword to pick a taranga tag.
//
// Trivia (whitespace + comments) is retained on the parse_tree so byte spans stay
// exact. Comments: ";;" line comment and "(; … ;)" block comment (nesting-free v1).
//
// The token_kind here is the parse_tree TokenKind parameter; it mirrors the WAT
// lexical classes in lexer.hpp (taranga::token_kind) but is a lexy-facing enum.

#include <lexy/dsl.hpp>
#include <lexy/grammar.hpp>
#include <lexy/parse_tree.hpp>
#include <lexy/action/parse_as_tree.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/callback.hpp>

#include <cstdint>
#include <string_view>

namespace taranga::grammar {
    namespace dsl = lexy::dsl;

    // =========================================================================
    // wat_token_kind — lexy parse_tree token classification.
    // =========================================================================

    enum class wat_token_kind : std::uint16_t {
        l_paren = 0,
        r_paren,
        keyword,      // atom starting with a letter: module, func, i32.add, …
        identifier,   // $name
        string_lit,   // "…"
        int_lit,      // 42, 0x2a, +7, -3
        float_lit,    // 3.14, 0x1.5p3, inf, nan, nan:0x…
        reserved,     // any other atom
    };

    // =========================================================================
    // Whitespace + comments (trivia)
    // =========================================================================

    struct line_comment {
        static constexpr auto rule = LEXY_LIT(";;") + dsl::until(dsl::newline).or_eof();
    };

    struct block_comment {
        // (; … ;) — non-nesting in v1. until() consumes through the first ";)".
        static constexpr auto rule = LEXY_LIT("(;") + dsl::until(LEXY_LIT(";)"));
    };

    struct whitespace_rule {
        static constexpr auto rule =
            dsl::ascii::space |
            (dsl::peek(LEXY_LIT(";;")) >> dsl::inline_<line_comment>) |
            (dsl::peek(LEXY_LIT("(;")) >> dsl::inline_<block_comment>);
    };

    // =========================================================================
    // Atoms
    //
    // WAT idchars are the printable ASCII set minus whitespace, parens, quote,
    // and comment introducers. A "keyword" atom begins with a lowercase letter;
    // "$…" is an identifier; a quoted run is a string; the rest classify by their
    // leading character. We keep the *token* coarse and let build_ast refine.
    // =========================================================================

    // idchar per the WAT spec: 0x21..0x7E excluding " ( ) ; and space. The atom
    // rule below inlines this class directly (a named production would force it
    // into the parse_tree as its own node, which we don't want).

    // A run of idchars — the raw atom. Classified by build_ast via its text.
    struct atom {
        static constexpr auto rule =
            dsl::identifier(dsl::ascii::alpha_digit / dsl::lit_c<'_'> / dsl::lit_c<'.'> /
                            dsl::lit_c<'+'> / dsl::lit_c<'-'> / dsl::lit_c<'*'> /
                            dsl::lit_c<'/'> / dsl::lit_c<'\\'> / dsl::lit_c<'^'> /
                            dsl::lit_c<'~'> / dsl::lit_c<'='> / dsl::lit_c<'<'> /
                            dsl::lit_c<'>'> / dsl::lit_c<'!'> / dsl::lit_c<'?'> /
                            dsl::lit_c<'#'> / dsl::lit_c<'$'> / dsl::lit_c<'%'> /
                            dsl::lit_c<'&'> / dsl::lit_c<'\''> / dsl::lit_c<':'> /
                            dsl::lit_c<'@'> / dsl::lit_c<'`'> / dsl::lit_c<'|'>);
        static constexpr auto value = lexy::noop;
    };

    // Quoted string literal — "…" with backslash escapes (kept raw; build_ast
    // unescapes only when a byte payload is needed, e.g. data segments).
    struct string_lit {
        static constexpr auto rule = [] {
            auto escape = dsl::backslash_escape
                              .rule(dsl::ascii::print); // \\, \", \t, \n, \XX all pass as printable
            return dsl::quoted(dsl::ascii::print - dsl::lit_c<'"'>, escape);
        }();
        static constexpr auto value = lexy::noop;
    };

    // =========================================================================
    // Parenthesised list — the sole recursive production.
    //   sexpr := "(" { sexpr | atom | string } ")"
    // =========================================================================

    struct sexpr;

    struct element {
        static constexpr auto rule =
            (dsl::peek(dsl::lit_c<'('>) >> dsl::recurse<sexpr>) |
            (dsl::peek(dsl::lit_c<'"'>) >> dsl::p<string_lit>) |
            dsl::else_ >> dsl::p<atom>;
    };

    struct sexpr {
        static constexpr auto rule =
            dsl::parenthesized.list(dsl::p<element>);
        static constexpr auto value = lexy::noop;
    };

    // =========================================================================
    // Root — a WAT file is one top-level "(module …)" form, optionally wrapped
    // in surrounding whitespace/comments. We accept any leading s-expr so that
    // both `(module …)` and bare `(func …)` fragments (used in tests) parse.
    // =========================================================================

    struct root {
        static constexpr auto whitespace = dsl::inline_<whitespace_rule>;
        static constexpr auto rule = dsl::p<sexpr> + dsl::eof;
        static constexpr auto value = lexy::noop;
    };

    // =========================================================================
    // parse_wat — build a parse_tree from WAT source text.
    // Mirrors crank::grammar::parse: utf8 string input, parse_as_tree, noop errors
    // (diagnostics are produced by build_ast on the tree, and by validate later).
    // =========================================================================

    using input_t = lexy::string_input<lexy::utf8_char_encoding>;
    using parse_tree_t = lexy::parse_tree_for<input_t, wat_token_kind>;

    [[nodiscard]] inline parse_tree_t parse_wat(std::string_view src) {
        auto input = lexy::string_input<lexy::utf8_char_encoding>(src);
        parse_tree_t tree;
        lexy::parse_as_tree<root>(tree, input, lexy::noop);
        return tree;
    }

} // namespace taranga::grammar
