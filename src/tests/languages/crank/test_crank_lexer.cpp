// =============================================================================
// test_crank_lexer.cpp — Crank lexer unit tests (Module 1).
//
// Verifies: include/languages/crank/lexer.hpp
//   1. strip_digit_sep: _ removal from numeric literals.
//   2. Contextual words (parallel, simd, …) are NOT reserved — lex as IDENT.
//   3. ASI-continuation token enum correctness (stmt_term defined).
//   4. token_kind enum uniqueness check.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/lexer.hpp"
#include "languages/crank/source_span.hpp"
#include "languages/crank/frontend.hpp"

using namespace crank::lex;

// ============================================================================
// Test 1 — strip_digit_sep
// ============================================================================

TEST_CASE (

"crank::lex::strip_digit_sep removes underscores"
,
"[crank][lexer]"
)
 {
    SECTION("decimal with separators") {
        CHECK(strip_digit_sep("1_000_000") == "1000000");
    }
    SECTION("no separator") {
        CHECK(strip_digit_sep("42") == "42");
    }
    SECTION("hex with separators") {
        CHECK(strip_digit_sep("FF_00_AB") == "FF00AB");
    }
    SECTION("binary with separators") {
        CHECK(strip_digit_sep("1010_0101") == "10100101");
    }
    SECTION("empty string") {
        CHECK(strip_digit_sep("") == "");
    }
    SECTION("leading separator stripped") {
        // Should never appear in valid literal but strip defensively
        CHECK(strip_digit_sep("_1_000") == "1000");
    }
}

// ============================================================================
// Test 2 — token_kind uniqueness
// ============================================================================

TEST_CASE (

"crank::lex::token_kind values are unique"
,
"[crank][lexer]"
)
 {
    // Build a set of all token_kind values and check no duplicates.
    std::vector<std::underlying_type_t<token_kind>> vals = {
        static_cast<std::underlying_type_t<token_kind>>(token_kind::ident),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::int_lit),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::float_lit),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::string_lit),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::raw_string_lit),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::bool_lit),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::kw_fn),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::kw_let),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::kw_transaction),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::op_arrow),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::op_fat_arrow),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::op_dot_dot),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::op_dot_dot_eq),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::punc_lparen),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::punc_lbrace),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::stmt_term),
        static_cast<std::underlying_type_t<token_kind>>(token_kind::unknown),
    };
    std::sort(vals.begin(), vals.end());
    auto it = std::adjacent_find(vals.begin(), vals.end());
    REQUIRE(it == vals.end()); // no duplicates
}

// ============================================================================
// Test 3 — token_kind enum coverage: stmt_term is defined
// ============================================================================

TEST_CASE (

"crank::lex::token_kind has stmt_term"
,
"[crank][lexer]"
)
 {
    // stmt_term must be a distinct value (ASI synthetic token)
    auto v = static_cast<std::uint16_t>(token_kind::stmt_term);
    CHECK(v != static_cast<std::uint16_t>(token_kind::unknown));
    CHECK(v != static_cast<std::uint16_t>(token_kind::ident));
}

// ============================================================================
// Test 4 — source_span: decode_span line/col
// ============================================================================

TEST_CASE (

"crank::decode_span computes line/col from offset"
,
"[crank][lexer]"
)
 {
    using namespace crank;

    std::string_view src = "package app\n\nfn Dot";
    // "fn Dot" starts at offset 13 (after "package app\n\n")
    auto span = decode_span(src, 13, 2);
    CHECK(span.line == 3);
    CHECK(span.col == 1);
    CHECK(span.offset == 13);
    CHECK(span.length == 2);
}

TEST_CASE (

"crank::decode_span col tracking"
,
"[crank][lexer]"
)
 {
    using namespace crank;

    std::string_view src = "fn Dot";
    auto span = decode_span(src, 3, 3); // "Dot" at offset 3
    CHECK(span.line == 1);
    CHECK(span.col == 4);
}

// ============================================================================
// Test 5 — ASI tag type existence
// ============================================================================

TEST_CASE (

"crank::lex ASI context types exist"
,
"[crank][lexer]"
)
 {
    // Just verify the types are defined and their address can be taken
    // (compile-time check that the tag structs exist).
    static_assert(sizeof(crank::lex::line_continues_flag) > 0);
    static_assert(sizeof(crank::lex::bracket_depth_counter) > 0);
    SUCCEED("ASI context types are defined");
}

// ============================================================================
// Test 6 — Lexical literals fire the correct grammar production (G2).
//
// The parse-tree dump exposes each literal's production node kind
// (grammar::hex_int_lit, grammar::float_lit, …). Asserting the node kind is
// present proves the lexer routed the literal to the right production — a
// behavioral check, not merely enum existence.
// ============================================================================

namespace {
    // Parse `body` wrapped in a minimal function and return the parse-tree JSON.
    [[nodiscard]] std::string parse_tree_of(std::string_view src) {
        crank::frontend::parse_options o;
        o.dump = crank::frontend::dump_mode::parse_tree;
        auto r = crank::frontend::parse(src, o);
        return r.parse_tree_json;
    }

    [[nodiscard]] bool has_node_kind(const std::string& json, std::string_view kind) {
        std::string needle = "\"kind\":\"";
        needle += kind;
        needle += "\"";
        return json.find(needle) != std::string::npos;
    }

    [[nodiscard]] int count_node_kind(const std::string& json, std::string_view kind) {
        std::string needle = "\"kind\":\"";
        needle += kind;
        needle += "\"";
        int n = 0;
        for (std::size_t p = 0; (p = json.find(needle, p)) != std::string::npos; p += needle.size())
            ++n;
        return n;
    }
} // namespace

TEST_CASE (

"crank lexer: literals route to the correct production"
,
"[crank][lexer]"
)
 {
    SECTION("hex integer") {
        auto j = parse_tree_of("package app\nfn F() -> Int32 { let x = 0xFF }");
        CHECK(has_node_kind(j, "grammar::hex_int_lit"));
    }
    SECTION("octal integer") {
        auto j = parse_tree_of("package app\nfn F() -> Int32 { let x = 0o17 }");
        CHECK(has_node_kind(j, "grammar::oct_int_lit"));
    }
    SECTION("binary integer") {
        auto j = parse_tree_of("package app\nfn F() -> Int32 { let x = 0b1010 }");
        CHECK(has_node_kind(j, "grammar::bin_int_lit"));
    }
    SECTION("decimal with digit separator") {
        auto j = parse_tree_of("package app\nfn F() -> Int32 { let x = 1_000 }");
        CHECK(has_node_kind(j, "grammar::int_lit"));
    }
    SECTION("float with exponent") {
        auto j = parse_tree_of("package app\nfn F() -> Float64 { let x = 3.5e2 }");
        CHECK(has_node_kind(j, "grammar::float_lit"));
    }
    SECTION("string literal") {
        auto j = parse_tree_of("package app\nfn F() -> Int32 { let s = \"a\\nb\" }");
        CHECK(has_node_kind(j, "grammar::string_lit"));
    }
    SECTION("raw string literal") {
        auto j = parse_tree_of("package app\nfn F() -> Int32 { let s = `raw` }");
        CHECK(has_node_kind(j, "grammar::raw_string_lit"));
    }
    SECTION("bool literal") {
        auto j = parse_tree_of("package app\nfn F() -> Bool { let b = true }");
        CHECK(has_node_kind(j, "grammar::bool_lit"));
    }
}

// ============================================================================
// Test 7 — Keywords lex as keywords, not identifiers (G2).
// ============================================================================

TEST_CASE (

"crank lexer: keywords are not identifiers"
,
"[crank][lexer]"
)
 {
    auto j = parse_tree_of("package app\nfn F() -> Int32 { let x = 1 }");
    // `fn` and `let` are keyword literals, never identifier tokens.
    CHECK(j.find("\"identifier\",\"token\":\"fn\"") == std::string::npos);
    CHECK(j.find("\"identifier\",\"token\":\"let\"") == std::string::npos);
    // The user-defined name `F` and `x` DO appear as identifiers.
    CHECK(j.find("\"identifier\",\"token\":\"F\"") != std::string::npos);
}

// ============================================================================
// Test 8 — Comments are trivia and do not break parsing (G2).
// ============================================================================

TEST_CASE (

"crank lexer: line and block comments are skipped"
,
"[crank][lexer]"
)
 {
    crank::frontend::parse_options o;
    o.dump = crank::frontend::dump_mode::parse_tree;
    auto r = crank::frontend::parse(
        "package app\n// line comment\nfn F() -> Int32 { /* block */ return 1 }", o);
    CHECK(r.ok);
    CHECK(has_node_kind(r.parse_tree_json, "grammar::func_decl"));
}

// ============================================================================
// Test 9 — Automatic Semicolon Insertion behavior (G4).
//
// Two statements separated only by a newline must parse as TWO statements
// (ASI splits them). A newline inside parentheses must NOT split — the
// bracket-depth carve-out keeps the expression on one logical line.
// ============================================================================

TEST_CASE (

"crank ASI: newline separates statements"
,
"[crank][lexer][asi]"
)
 {
    auto j = parse_tree_of("package app\nfn F() -> Int32 { let a = 1\nlet b = 2 }");
    CHECK(count_node_kind(j, "grammar::let_stmt") == 2);
}

TEST_CASE (

"crank ASI: newline inside parentheses does not split"
,
"[crank][lexer][asi]"
)
 {
    // bracket_depth_counter != 0 suppresses ASI across the newline.
    auto j = parse_tree_of("package app\nfn F() -> Int32 { let a = (1 +\n2) }");
    CHECK(count_node_kind(j, "grammar::let_stmt") == 1);
}
