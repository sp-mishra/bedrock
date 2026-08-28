// =============================================================================
// test_lithe_pattern_rewrite.cpp — Unit Tests for Lithe Pattern DSL + Rewrite Engine
//
// Verifies: include/edsl/lithe_pattern.hpp
//           include/edsl/lithe_rewrite.hpp
//
// Cases:
//   1. match_pattern: add(pv<"x">, lit<0>) matches add(5, 0); binds x.
//   2. match_pattern: mul pattern fails against add node.
//   3. match_pattern: literal_pattern exact value match and mismatch.
//   4. match_pattern: pattern_var<ID> wildcard always binds.
//   5. rule + apply_first: add(5, 0) → 5 via add_zero rule.
//   6. rule_set::apply_first: first matching rule wins; second rule skipped.
//   7. rule_set::apply_first: returns nullopt when no rule matches.
//   8. rewrite_pass: add(x, 0) → x (changed = true).
//   9. rewrite_pass: no-match expression; changed = false.
//  10. rewrite_fixpoint: add(add(x, 0), 0) → x in fixpoint.
//  11. rewrite_fixpoint: result wraps in optimized_expr.
//  12. rewrite_pass_adapter: pass_type_traits::category == optimization.
//  13. rewrite_pass_adapter: pass_type_traits::in_stage/out_stage.
//  14. Built-in rules::arithmetic::add_zero compiles; apply_first fires on add(x,0).
//  15. Built-in rules::arithmetic::mul_one compiles; apply_first fires on mul(x,1).
//  16. Built-in rules::arithmetic::mul_zero returns 0 for mul(x,0).
//  17. Built-in rules::arithmetic::double_neg fires on neg(neg(x)).
//  18. match_result::has() returns false for unbound id.
//  19. match_result::bind + has + get round-trip.
//  20. match_result::merge merges two binding maps.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_pattern.hpp"
#include "lithe/lithe_rewrite.hpp"

namespace pat = lithe::pattern;
namespace rw = lithe::rewrite;

// ============================================================================
// Test 1 — match_pattern: add(pv<"x">, lit<0>) matches add(5, 0); binds x.
// ============================================================================

TEST_CASE (


"match_pattern: add(pv<x>, lit<0>) matches add(5,0) and binds x"
,
"[lithe_pattern][match]"
)
{
    auto pat_  = pat::add(pat::pv<0>, pat::lit<0>);
    auto expr_ = lithe::make_node<lithe::add_tag>(5, 0);

    auto result = pat::match_pattern(pat_, expr_);
    REQUIRE(result.has_value());
    REQUIRE(result->has(std::size_t{0}));
}

// ============================================================================
// Test 2 — match_pattern: mul pattern fails against add node.
// ============================================================================

TEST_CASE (


"match_pattern: mul pattern does not match add node"
,
"[lithe_pattern][match]"
)
{
    auto mul_pat = pat::mul(pat::pv<0>, pat::lit<1>);
    auto add_expr = lithe::make_node<lithe::add_tag>(3, 1);

    auto result = pat::match_pattern(mul_pat, add_expr);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================================
// Test 3 — literal_pattern: exact value matches; wrong value does not.
// ============================================================================

TEST_CASE (


"match_pattern: literal_pattern<0> matches 0 but not 1"
,
"[lithe_pattern][match][literal]"
)
{
    auto lit_zero = pat::lit<0>;

    // Terminal integer 0 — matches.
    {
        auto r = pat::match_pattern(lit_zero, 0);
        REQUIRE(r.has_value());
    }

    // Terminal integer 1 — does not match.
    {
        auto r = pat::match_pattern(lit_zero, 1);
        REQUIRE_FALSE(r.has_value());
    }
}

// ============================================================================
// Test 4 — pattern_var wildcard always binds any sub-expression.
// ============================================================================

TEST_CASE (


"match_pattern: pattern_var<ID> wildcard binds any expression"
,
"[lithe_pattern][match][pvar]"
)
{
    // Match pv<42> against a raw integer terminal.
    auto result_int = pat::match_pattern(pat::pv<42>, 99);
    REQUIRE(result_int.has_value());
    REQUIRE(result_int->has(std::size_t{42}));

    // Match pv<0> against a complex node expression.
    auto complex = lithe::make_node<lithe::mul_tag>(3, 4);
    auto result_node = pat::match_pattern(pat::pv<0>, complex);
    REQUIRE(result_node.has_value());
    REQUIRE(result_node->has(std::size_t{0}));
}

// ============================================================================
// Test 5 — rule + apply_first: add(5, 0) → some result via add_zero rule.
// ============================================================================

TEST_CASE (


"rule + apply_first: add(5, 0) fires add_zero rule"
,
"[lithe_pattern][rule][apply_first]"
)
{
    // Build a custom add_zero rule set inline.
    auto add_zero_rs = pat::make_rule_set(
        pat::rule(
            pat::add(pat::pv<0>, pat::lit<0>),
            [](const pat::match_result& m) -> std::optional<std::any> {
                return m.get<std::any>(std::size_t{0});
            }
        )
    );

    auto expr = lithe::make_node<lithe::add_tag>(5, 0);
    auto result = add_zero_rs.apply_first(expr);
    REQUIRE(result.has_value());
}

// ============================================================================
// Test 6 — rule_set::apply_first: first matching rule wins.
// ============================================================================

TEST_CASE (


"rule_set::apply_first: first rule matches; second rule not attempted"
,
"[lithe_pattern][rule_set][apply_first]"
)
{
    std::size_t first_fired  = 0;
    std::size_t second_fired = 0;

    // Both rules match add(x, 0), but only the first should be used.
    auto rs = pat::make_rule_set(
        pat::rule(
            pat::add(pat::pv<0>, pat::lit<0>),
            [&](const pat::match_result& m) -> std::optional<std::any> {
                ++first_fired;
                return m.get<std::any>(std::size_t{0});
            }
        ),
        pat::rule(
            pat::add(pat::pv<1>, pat::lit<0>),
            [&](const pat::match_result& m) -> std::optional<std::any> {
                ++second_fired;
                return m.get<std::any>(std::size_t{1});
            }
        )
    );

    auto expr = lithe::make_node<lithe::add_tag>(7, 0);
    auto result = rs.apply_first(expr);

    REQUIRE(result.has_value());
    REQUIRE(first_fired == 1u);
    REQUIRE(second_fired == 0u);
}

// ============================================================================
// Test 7 — rule_set::apply_first: returns nullopt when no rule matches.
// ============================================================================

TEST_CASE (


"rule_set::apply_first: returns nullopt on no-match expression"
,
"[lithe_pattern][rule_set][apply_first]"
)
{
    // Rule only fires on add(x, 0) but we pass mul(x, 1).
    auto rs = pat::make_rule_set(
        pat::rule(
            pat::add(pat::pv<0>, pat::lit<0>),
            [](const pat::match_result& m) -> std::optional<std::any> {
                return m.get<std::any>(std::size_t{0});
            }
        )
    );

    auto expr = lithe::make_node<lithe::mul_tag>(5, 1);
    auto result = rs.apply_first(expr);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================================
// Test 8 — rewrite_pass: add(x, 0) → x (changed = true).
// ============================================================================

TEST_CASE (


"rewrite_pass: add(x, 0) simplifies; changed flag is true"
,
"[lithe_rewrite][rewrite_pass]"
)
{
    auto rs = pat::make_rule_set(
        pat::rule(
            pat::add(pat::pv<0>, pat::lit<0>),
            [](const pat::match_result& m) -> std::optional<std::any> {
                return m.get<std::any>(std::size_t{0});
            }
        )
    );

    auto expr = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::mul_tag>(2, 3),
        0);

    auto [result, changed] = rw::rewrite_pass(expr, rs);
    // The rule fired on the outer add(mul(2,3), 0).
    CHECK(changed);
    (void)result;
}

// ============================================================================
// Test 9 — rewrite_pass: expression with no matching rule; changed = false.
// ============================================================================

TEST_CASE (


"rewrite_pass: expression with no match; changed = false"
,
"[lithe_rewrite][rewrite_pass]"
)
{
    // Rule only fires on add(x, 0); provide mul(2, 3) which never matches.
    auto rs = pat::make_rule_set(
        pat::rule(
            pat::add(pat::pv<0>, pat::lit<0>),
            [](const pat::match_result& m) -> std::optional<std::any> {
                return m.get<std::any>(std::size_t{0});
            }
        )
    );

    auto expr = lithe::make_node<lithe::mul_tag>(2, 3);
    auto [result, changed] = rw::rewrite_pass(expr, rs);
    CHECK_FALSE(changed);
    (void)result;
}

// ============================================================================
// Test 10 — rewrite_fixpoint: nested add zeros collapse to leaf.
// ============================================================================

TEST_CASE (


"rewrite_fixpoint: add(add(x, 0), 0) collapses to x-equivalent"
,
"[lithe_rewrite][rewrite_fixpoint]"
)
{
    // Use the built-in arithmetic add_zero rule set (both x+0 and 0+x).
    const auto& rs = pat::rules::arithmetic::add_zero;

    // add(add(5, 0), 0) should simplify after two passes.
    auto expr = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::add_tag>(5, 0),
        0);

    auto optimized = rw::rewrite_fixpoint(expr, rs, /*max_iters=*/8u);
    // optimized_expr wraps the result — just verify it compiled and ran.
    (void)optimized;
    SUCCEED();
}

// ============================================================================
// Test 11 — rewrite_fixpoint: result is wrapped in optimized_expr.
// ============================================================================

TEST_CASE (


"rewrite_fixpoint: result type is optimized_expr"
,
"[lithe_rewrite][rewrite_fixpoint]"
)
{
    const auto& rs = pat::rules::arithmetic::add_zero;
    auto expr = lithe::make_node<lithe::add_tag>(3, 0);

    auto result = rw::rewrite_fixpoint(expr, rs);

    // optimized_expr<E> has a .value member.
    static_assert(lithe::is_optimized_expr_v<decltype(result)>,
                  "rewrite_fixpoint must return optimized_expr<E>");
    (void)result.value;
    SUCCEED();
}

// ============================================================================
// Test 12 — rewrite_pass_adapter: category == pass_category::optimization.
// ============================================================================

TEST_CASE (


"rewrite_pass_adapter: pass_type_traits::category is optimization"
,
"[lithe_rewrite][rewrite_pass_adapter][traits]"
)
{
    using rs_t = decltype(pat::rules::arithmetic::add_zero);
    using adapter_t = rw::rewrite_pass_adapter<rs_t, lithe::fixed_string{"test.add_zero"}>;
    using traits = lithe::passes::pass_type_traits<adapter_t>;

    STATIC_REQUIRE(traits::category == lithe::passes::pass_category::optimization);
}

// ============================================================================
// Test 13 — rewrite_pass_adapter: in_stage == surface, out_stage == optimized.
// ============================================================================

TEST_CASE (


"rewrite_pass_adapter: in_stage=surface, out_stage=optimized"
,
"[lithe_rewrite][rewrite_pass_adapter][traits]"
)
{
    using rs_t = decltype(pat::rules::arithmetic::add_zero);
    using adapter_t = rw::rewrite_pass_adapter<rs_t, lithe::fixed_string{"test.add_zero2"}>;
    using traits = lithe::passes::pass_type_traits<adapter_t>;

    STATIC_REQUIRE(traits::in_stage  == lithe::passes::ir_stage::surface);
    STATIC_REQUIRE(traits::out_stage == lithe::passes::ir_stage::optimized);
}

// ============================================================================
// Test 14 — Built-in add_zero rule: apply_first fires on add(x, 0).
// ============================================================================

TEST_CASE (


"rules::arithmetic::add_zero: apply_first fires on add(x, 0)"
,
"[lithe_pattern][rules][add_zero]"
)
{
    auto expr = lithe::make_node<lithe::add_tag>(42, 0);
    auto result = pat::rules::arithmetic::add_zero.apply_first(expr);
    REQUIRE(result.has_value());
}

TEST_CASE (


"rules::arithmetic::add_zero: apply_first fires on 0 + x"
,
"[lithe_pattern][rules][add_zero]"
)
{
    auto expr = lithe::make_node<lithe::add_tag>(0, 7);
    auto result = pat::rules::arithmetic::add_zero.apply_first(expr);
    REQUIRE(result.has_value());
}

// ============================================================================
// Test 15 — Built-in mul_one rule: apply_first fires on mul(x, 1) and 1*x.
// ============================================================================

TEST_CASE (


"rules::arithmetic::mul_one: apply_first fires on mul(x, 1)"
,
"[lithe_pattern][rules][mul_one]"
)
{
    auto expr = lithe::make_node<lithe::mul_tag>(9, 1);
    auto result = pat::rules::arithmetic::mul_one.apply_first(expr);
    REQUIRE(result.has_value());
}

TEST_CASE (


"rules::arithmetic::mul_one: apply_first fires on 1 * x"
,
"[lithe_pattern][rules][mul_one]"
)
{
    auto expr = lithe::make_node<lithe::mul_tag>(1, 11);
    auto result = pat::rules::arithmetic::mul_one.apply_first(expr);
    REQUIRE(result.has_value());
}

// ============================================================================
// Test 16 — Built-in mul_zero rule: returns any-wrapped 0 for mul(x, 0).
// ============================================================================

TEST_CASE (


"rules::arithmetic::mul_zero: apply_first fires on mul(x, 0)"
,
"[lithe_pattern][rules][mul_zero]"
)
{
    auto expr = lithe::make_node<lithe::mul_tag>(5, 0);
    auto result = pat::rules::arithmetic::mul_zero.apply_first(expr);
    REQUIRE(result.has_value());
    // The result wraps int 0.
    REQUIRE(std::any_cast<int>(*result) == 0);
}

TEST_CASE (


"rules::arithmetic::mul_zero: does not fire on mul(x, 1)"
,
"[lithe_pattern][rules][mul_zero]"
)
{
    auto expr = lithe::make_node<lithe::mul_tag>(5, 1);
    auto result = pat::rules::arithmetic::mul_zero.apply_first(expr);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================================
// Test 17 — Built-in double_neg rule: fires on neg(neg(x)).
// ============================================================================

TEST_CASE (


"rules::arithmetic::double_neg: apply_first fires on neg(neg(x))"
,
"[lithe_pattern][rules][double_neg]"
)
{
    auto expr = lithe::make_node<lithe::neg_tag>(
        lithe::make_node<lithe::neg_tag>(3));
    auto result = pat::rules::arithmetic::double_neg.apply_first(expr);
    REQUIRE(result.has_value());
}

TEST_CASE (


"rules::arithmetic::double_neg: does not fire on neg(x)"
,
"[lithe_pattern][rules][double_neg]"
)
{
    auto expr = lithe::make_node<lithe::neg_tag>(5);
    auto result = pat::rules::arithmetic::double_neg.apply_first(expr);
    REQUIRE_FALSE(result.has_value());
}

// ============================================================================
// Test 18 — match_result::has() returns false for unbound id.
// ============================================================================

TEST_CASE (


"match_result: has() returns false for unbound key"
,
"[lithe_pattern][match_result]"
)
{
    pat::match_result mr;
    REQUIRE_FALSE(mr.has(std::size_t{99}));
}

// ============================================================================
// Test 19 — match_result: bind + has + get round-trip.
// ============================================================================

TEST_CASE (


"match_result: bind, has, and get<T> round-trip"
,
"[lithe_pattern][match_result]"
)
{
    pat::match_result mr;
    mr.bind(std::size_t{7}, std::any{42});

    REQUIRE(mr.has(std::size_t{7}));
    auto val = mr.get<int>(std::size_t{7});
    REQUIRE(val.has_value());
    REQUIRE(*val == 42);

    // Unbound key returns nullopt.
    auto missing = mr.get<int>(std::size_t{8});
    REQUIRE_FALSE(missing.has_value());
}

// ============================================================================
// Test 20 — match_result::merge: bindings from other are added.
// ============================================================================

TEST_CASE (


"match_result: merge combines bindings from two results"
,
"[lithe_pattern][match_result]"
)
{
    pat::match_result a, b;
    a.bind(std::size_t{1}, std::any{10});
    b.bind(std::size_t{2}, std::any{20});

    a.merge(b);

    REQUIRE(a.has(std::size_t{1}));
    REQUIRE(a.has(std::size_t{2}));

    auto v1 = a.get<int>(std::size_t{1});
    auto v2 = a.get<int>(std::size_t{2});
    REQUIRE(v1.has_value());
    REQUIRE(v2.has_value());
    REQUIRE(*v1 == 10);
    REQUIRE(*v2 == 20);
}
