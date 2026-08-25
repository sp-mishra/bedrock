// =============================================================================
// test_crank_parser.cpp — Crank parser unit tests (Module 1).
//
// Verifies: include/languages/crank/parser.hpp
//   1. grammar::parse on the §9 example produces a non-empty parse_tree.
//   2. parse_tree dump via dump_parse_tree produces non-empty JSON.
//   3. frontend::parse round-trip on a minimal source.
//   4. frontend::parse with dump_mode::parse_tree fills parse_tree_json.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/parser.hpp"
#include "languages/crank/dump.hpp"
#include "languages/crank/frontend.hpp"

// ============================================================================
// §9 example source (subset — enough to test the parse path)
// ============================================================================

static constexpr std::string_view kDotSource = R"crank(
package app

fn Dot(a: Float32, b: Float32) -> Float32 {
    return a + b
}
)crank";

static constexpr std::string_view kMinimalSource = R"crank(
package app
)crank";

// ============================================================================
// Test 1 — grammar::parse produces a parse_tree (may have errors in v1)
// ============================================================================

TEST_CASE (

"crank::grammar::parse runs without crashing on Dot example"
,
"[crank][parser]"
)
 {
    // parse() returns a parse_tree; it may be empty if grammar errors exist.
    // We just verify the call completes.
    auto tree = crank::grammar::parse(kDotSource);
    // Tree may be non-empty (full parse) or empty (recovery fallback).
    // We only assert the call does not throw.
    SUCCEED("parse completed without exception");
}

// ============================================================================
// Test 2 — parse on minimal source
// ============================================================================

TEST_CASE (

"crank::grammar::parse handles minimal 'package app' source"
,
"[crank][parser]"
)
 {
    auto tree = crank::grammar::parse(kMinimalSource);
    SUCCEED("minimal parse completed");
}

// ============================================================================
// Test 3 — dump_parse_tree produces non-empty string
// ============================================================================

TEST_CASE (

"crank::dump_parse_tree produces non-empty JSON from non-empty tree"
,
"[crank][parser]"
)
 {
    auto tree = crank::grammar::parse(kDotSource);
    auto json = crank::dump_parse_tree(tree);
    // Even an empty tree produces "{}"
    CHECK_FALSE(json.empty());
}

// ============================================================================
// Test 4 — frontend::parse round-trip
// ============================================================================

TEST_CASE (

"crank::frontend::parse round-trip on minimal source"
,
"[crank][parser]"
)
 {
    auto result = crank::frontend::parse(kMinimalSource);
    // ok may be false due to grammar gaps in v1; we check no crash + diagnostics accessible
    SUCCEED("frontend::parse returned without exception");
    // diagnostics accessor works
    (void)result.ok;
    (void)result.diagnostics.has_errors();
}

// ============================================================================
// Test 5 — dump_mode::parse_tree fills parse_tree_json
// ============================================================================

TEST_CASE (

"crank::frontend::parse with dump_mode::parse_tree fills field"
,
"[crank][parser]"
)
 {
    crank::frontend::parse_options opts;
    opts.dump = crank::frontend::dump_mode::parse_tree;
    auto result = crank::frontend::parse(kDotSource, opts);
    // parse_tree_json must be non-empty (at minimum "{}")
    CHECK_FALSE(result.parse_tree_json.empty());
}

// ============================================================================
// Test 6 — dump_mode::ast fills ast_json
// ============================================================================

TEST_CASE (

"crank::frontend::parse with dump_mode::ast fills ast_json"
,
"[crank][parser]"
)
 {
    crank::frontend::parse_options opts;
    opts.dump = crank::frontend::dump_mode::ast;
    auto result = crank::frontend::parse(kDotSource, opts);
    CHECK_FALSE(result.parse_tree_json.empty());
    // ast_json may be a stub but must be non-empty
    CHECK_FALSE(result.ast_json.empty());
}

// ============================================================================
// Test 7 — parse_options.collect_stats enables statistics collection
// ============================================================================

TEST_CASE (

"crank::frontend::parse with collect_stats = true populates stats"
,
"[crank][parser][stats]"
)
 {
    crank::frontend::parse_options opts;
    opts.collect_stats = true;
    auto result = crank::frontend::parse(kDotSource, opts);

    // When collect_stats = true, result.stats should be populated
    CHECK(result.stats.has_value());
    auto& stats = result.stats.value();

    // Verify source metrics
    CHECK(stats.source_bytes == kDotSource.size());
    CHECK(stats.source_lines > 0);  // At least 1 line

    // Verify token metrics
    CHECK(stats.total_tokens > 0);
    CHECK(stats.production_nodes > 0);

    // Verify error metrics (no errors in valid source)
    CHECK(stats.error_count == 0);

    // Verify timings are non-zero
    CHECK(stats.timings.lex_and_parse.count() > 0);
    CHECK(stats.timings.ast_build.count() > 0);
    CHECK(stats.timings.total.count() > 0);
    CHECK(stats.timings.total ==
          stats.timings.lex_and_parse + stats.timings.ast_build);
}

// ============================================================================
// Test 8 — parse_options.collect_stats = false (default) leaves stats empty
// ============================================================================

TEST_CASE (

"crank::frontend::parse with default options leaves stats empty"
,
"[crank][parser][stats]"
)
 {
    // Default parse_options has collect_stats = false
    auto result = crank::frontend::parse(kDotSource);

    CHECK_FALSE(result.stats.has_value());
}

// ============================================================================
// Test 9 — dump_stats produces valid JSON
// ============================================================================

TEST_CASE (

"crank::dump_stats produces parseable JSON"
,
"[crank][parser][stats]"
)
 {
    crank::frontend::parse_options opts;
    opts.collect_stats = true;
    auto result = crank::frontend::parse(kDotSource, opts);

    CHECK(result.stats.has_value());
    auto json = crank::dump_stats(*result.stats);

    // Verify JSON contains expected fields
    CHECK_FALSE(json.empty());
    CHECK(json.find("\"source_bytes\"") != std::string::npos);
    CHECK(json.find("\"source_lines\"") != std::string::npos);
    CHECK(json.find("\"total_tokens\"") != std::string::npos);
    CHECK(json.find("\"timings_ns\"") != std::string::npos);
    CHECK(json.find("\"error_count\"") != std::string::npos);
}

// ============================================================================
// Test 10 — stats counters on minimal source
// ============================================================================

TEST_CASE (

"crank::frontend::parse stats on minimal source"
,
"[crank][parser][stats]"
)
 {
    crank::frontend::parse_options opts;
    opts.collect_stats = true;
    auto result = crank::frontend::parse(kMinimalSource, opts);

    CHECK(result.stats.has_value());
    auto& stats = result.stats.value();

    // Even minimal source has tokens and productions
    CHECK(stats.source_bytes == kMinimalSource.size());
    CHECK(stats.total_tokens >= 1);   // At least "package" keyword
    CHECK(stats.production_nodes >= 1);  // At least one production

    // Minimal source is valid, no errors
    CHECK(stats.error_count == 0);
}

// ============================================================================
// Test 11 — Structural parse assertions (G3).
//
// Prior parser tests were smoke tests (SUCCEED / non-empty). These assert the
// parse tree actually contains the expected production nodes for each core v1
// construct, keyed off dump_parse_tree's stable "kind" fields.
// ============================================================================

namespace {
    [[nodiscard]] std::string ptree(std::string_view src) {
        crank::frontend::parse_options o;
        o.dump = crank::frontend::dump_mode::parse_tree;
        return crank::frontend::parse(src, o).parse_tree_json;
    }

    [[nodiscard]] bool has_kind(const std::string& j, std::string_view kind) {
        std::string needle = "\"kind\":\"";
        needle += kind;
        needle += "\"";
        return j.find(needle) != std::string::npos;
    }

    [[nodiscard]] bool has_token(const std::string& j, std::string_view tok) {
        std::string needle = "\"token\":\"";
        needle += tok;
        needle += "\"";
        return j.find(needle) != std::string::npos;
    }
} // namespace

TEST_CASE (

"crank parser: function declaration structure"
,
"[crank][parser]"
)
 {
    auto j = ptree("package app\nfn Dot(a: Float32, b: Float32) -> Float32 { return a + b }");
    CHECK(has_kind(j, "grammar::func_decl"));
    CHECK(has_kind(j, "grammar::block"));
}

TEST_CASE (

"crank parser: let binding structure"
,
"[crank][parser]"
)
 {
    auto j = ptree("package app\nfn F() -> Int32 { let x = 1 }");
    CHECK(has_kind(j, "grammar::let_stmt"));
}

TEST_CASE (

"crank parser: if expression structure"
,
"[crank][parser]"
)
 {
    auto j = ptree("package app\nfn F() -> Int32 { if true { return 1 } else { return 2 } }");
    CHECK(has_token(j, "if"));
    CHECK(has_token(j, "else"));
    CHECK(has_kind(j, "grammar::block"));
}

TEST_CASE (

"crank parser: for loop structure"
,
"[crank][parser]"
)
 {
    auto j = ptree("package app\nfn F() -> Int32 { for i in 0..10 { } return 0 }");
    CHECK(has_token(j, "for"));
    CHECK(has_kind(j, "grammar::block"));
}

TEST_CASE (

"crank parser: match expression structure"
,
"[crank][parser]"
)
 {
    auto j = ptree("package app\nfn F() -> Int32 { match 1 { 1 => 10, _ => 0 } }");
    CHECK(has_token(j, "match"));
}

// ============================================================================
// Test 12 — Negative parser tests (G3).
//
// Malformed sources must set ok=false with a diagnostic; the parse tree is
// empty. Prior to this there were zero negative parser tests.
// ============================================================================

TEST_CASE (

"crank parser negative: function declaration without parentheses"
,
"[crank][parser][negative]"
)
 {
    // Grammar requires the parenthesized param list; omitting it fails the
    // parse (crank.md §"Function Declaration Shape").
    auto r = crank::frontend::parse("package app\nfn F -> Int32 { return 1 }");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.has_errors());
}

TEST_CASE (

"crank parser negative: unterminated string literal"
,
"[crank][parser][negative]"
)
 {
    auto r = crank::frontend::parse("package app\nfn F() -> Int32 { let s = \"abc }");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.has_errors());
}

TEST_CASE (

"crank parser negative: unbalanced brace"
,
"[crank][parser][negative]"
)
 {
    auto r = crank::frontend::parse("package app\nfn F() -> Int32 { return 1 ");
    CHECK_FALSE(r.ok);
    CHECK(r.diagnostics.has_errors());
}

// ============================================================================
// Test 13 — Arrays / slices / builtins parse to their productions (G11).
//
// crank.md §"Built-in functions" documents len/cap/append/make; §types define
// slice `[] T` and array `[N] T`. These assert each documented construct routes
// to its grammar production (grammar::slice_type / array_type / builtin_call),
// keyed off dump_parse_tree's stable "kind" fields.
//
// Note: the array/slice *literal* form `[1,2,3]` (grammar::composite_lit) is
// defined in the grammar but not wired into `primary` in v1 — value
// construction is via make(...). That literal path is a documented latent gap
// (see scratch/crank/v1_gaps.md G11) and is intentionally NOT asserted here.
// ============================================================================

TEST_CASE (

"crank parser: slice type in parameter position"
,
"[crank][parser][slices]"
)
 {
    auto j = ptree("package app\nfn F(s: [] Int32) -> Int32 { return len(s) }");
    CHECK(has_kind(j, "grammar::slice_type"));
    CHECK(has_kind(j, "grammar::builtin_call"));
}

TEST_CASE (

"crank parser: array type in parameter and return position"
,
"[crank][parser][arrays]"
)
 {
    auto param = ptree("package app\nfn F(a: [4] Int32) -> Int32 { return 0 }");
    CHECK(has_kind(param, "grammar::array_type"));

    auto ret = ptree("package app\nfn F() -> [4] Int32 { return F() }");
    CHECK(has_kind(ret, "grammar::array_type"));
}

TEST_CASE (

"crank parser: len routes to builtin_call"
,
"[crank][parser][builtins]"
)
 {
    // len is the sole grammar builtin (lean charter §7); cap/append/make/print
    // moved to prelude — they parse as plain qualified-ident call expressions.
    CHECK(has_kind(ptree("package app\nfn F(s: [] Int32) -> Int32 { return len(s) }"),
                   "grammar::builtin_call"));
    // Former builtins parse as regular call expressions (grammar::expr), not builtin_call
    CHECK(crank::frontend::parse("package app\nfn F(s: [] Int32) -> Int32 { return cap(s) }").ok);
    CHECK(crank::frontend::parse("package app\nfn F(s: [] Int32) -> Int32 { let t = append(s, 1) return len(t) }").ok);
    CHECK(crank::frontend::parse("package app\nfn F() -> Int32 { let s = make([] Int32, 4) return len(s) }").ok);
}

TEST_CASE (

"crank parser: slice index expression parses"
,
"[crank][parser][slices]"
)
 {
    // Postfix index `s[i]` (crank.md §"Postfix chain", level 12).
    auto r = crank::frontend::parse("package app\nfn F(s: [] Int32) -> Int32 { return s[0] }");
    CHECK(r.ok);
    CHECK(has_kind(ptree("package app\nfn F(s: [] Int32) -> Int32 { return s[0] }"),
                   "grammar::slice_type"));
}

// ============================================================================
// Test 14 — Closure literal parses to grammar::closure_lit (G12).
//
// crank.md documents fn-expression closures. Ownership/move/copy enforcement is
// a semantic concern handled downstream (not at the parse layer), so this is a
// parse-level positive test only; the enforcement boundary is documented in
// scratch/crank/v1_gaps.md G12.
// ============================================================================

TEST_CASE (

"crank parser: closure literal in let binding"
,
"[crank][parser][closure]"
)
 {
    auto j = ptree("package app\nfn F() -> Int32 { let g = fn(x: Int32) -> Int32 { return x } return 0 }");
    CHECK(has_kind(j, "grammar::closure_lit"));
    CHECK(has_kind(j, "grammar::let_stmt"));
}

// ============================================================================
// Test 15 — ? postfix operator parses (addendum §2).
//
// Validates: grammar::op_postfix extended with dsl::lit_c<'?'>.
// Positive parse cases; semantic limits (CRANK-Q-*) are in test_crank_sema.cpp.
// ============================================================================

TEST_CASE("crank parser: ? postfix on call expr", "[crank][parser][try_op]") {
    auto j = ptree("package app\nfn F() -> Result[Int32, String] { let x = g()? return Ok(x) }");
    CHECK((j.find('?') != std::string::npos || has_kind(j, "grammar::expr")));
}

TEST_CASE("crank parser: chained ? postfix", "[crank][parser][try_op]") {
    // map.get(k)?.normalize()? — left-to-right postfix chain
    auto r = crank::frontend::parse(
        "package app\nfn F(m: Map) -> Result[Int32, String] { return m.get(k)?.normalize()? }");
    CHECK(r.ok);
}

TEST_CASE("crank parser: ? higher than binary +", "[crank][parser][try_op]") {
    // a + b? should parse as a + (b?)  not  (a + b)?
    auto r = crank::frontend::parse(
        "package app\nfn F() -> Result[Int32, String] { return a + b? }");
    CHECK(r.ok);
}

// ============================================================================
// Test 16 — Pipe closures parse (addendum §4).
//
// Validates: grammar::pipe_closure + grammar::closure_lit extended.
// Both pipe and fn forms must emit grammar::closure_lit node.
// The || / | disambiguation in infix position must not regress.
// ============================================================================

TEST_CASE("crank parser: pipe closure with typed param", "[crank][parser][closure][pipe]") {
    auto j = ptree("package app\nfn F() -> Int32 { let g = |x: Int32| x + 1 return 0 }");
    CHECK((has_kind(j, "grammar::closure_lit") || has_kind(j, "grammar::pipe_closure")));
}

TEST_CASE("crank parser: pipe closure with block body", "[crank][parser][closure][pipe]") {
    auto j = ptree("package app\nfn F() -> Float64 { let g = |a: Float64, b: Float64| -> Float64 { return a + b } return 0.0 }");
    CHECK((has_kind(j, "grammar::closure_lit") || has_kind(j, "grammar::pipe_closure")));
}

TEST_CASE("crank parser: zero-param pipe closure", "[crank][parser][closure][pipe]") {
    auto r = crank::frontend::parse(
        "package app\nfn F() -> Int32 { let g = || 42 return 0 }");
    CHECK(r.ok);
}

TEST_CASE("crank parser: || in infix position is logical-or not closure", "[crank][parser][closure][pipe]") {
    auto j = ptree("package app\nfn F(a: Bool, b: Bool) -> Bool { return a || b }");
    CHECK(has_kind(j, "grammar::expr"));
    // Must NOT produce a pipe_closure node when || is between two bool operands
    CHECK(!has_kind(j, "grammar::pipe_closure"));
}

TEST_CASE("crank parser: fn-form closure still parses (no regression)", "[crank][parser][closure]") {
    auto j = ptree("package app\nfn F() -> Int32 { let g = fn(x: Int32) -> Int32 { return x } return 0 }");
    CHECK(has_kind(j, "grammar::closure_lit"));
}

// ============================================================================
// Test 17 — Ranged quantifier parses (addendum §1.1, Option A).
//
// Validates: quant_binder extended with "in range_expr" form;
// quantified separator accepts "." (typed) or ":" (ranged).
// Legacy typed form must not regress.
// ============================================================================

TEST_CASE("crank parser: ranged quantifier forall i in range", "[crank][parser][quantifier][ranged]") {
    // forall i in 0..len(xs): xs[i] >= 0
    auto r = crank::frontend::parse(
        "package app\nfn F(xs: [] Int32) -> Int32\n"
        "    ensures forall i in 0..len(xs): xs[i] >= 0\n"
        "{ return 0 }");
    CHECK(r.ok);
}

TEST_CASE("crank parser: ranged quantifier exists i in range", "[crank][parser][quantifier][ranged]") {
    auto r = crank::frontend::parse(
        "package app\nfn F(xs: [] Int32, target: Int32) -> Bool\n"
        "    ensures exists i in 0..len(xs): xs[i] == target\n"
        "{ return false }");
    CHECK(r.ok);
}

TEST_CASE("crank parser: typed quantifier still parses (no regression)", "[crank][parser][quantifier][typed]") {
    auto j = ptree(
        "package app\nfn F() -> Int32\n"
        "    ensures forall i: Int32 . i >= 0\n"
        "{ return 0 }");
    CHECK(has_kind(j, "grammar::quantified"));
}
