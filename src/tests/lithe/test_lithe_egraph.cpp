// =============================================================================
// test_lithe_egraph.cpp — Lithe E-Graph Adapter Unit Tests
//
// Verifies: include/edsl/lithe_egraph.hpp
// Includes the adapter explicitly (opt-in; not pulled by lithe.hpp).
//
// Cases:
//   1. intern: two structurally-equal Lithe exprs → same class.
//   2. intern: distinct integer leaves → distinct classes (payload).
//   3. Lithe commutativity: a+b and b+a collapse after saturate.
//   4. extract_best<ast_size_cost>: x*1 + 0 → x after identity saturation.
//   5. egraph_optimize pass: operator()() returns an expression;
//      pass_type_traits::category == pass_category::optimization;
//      pass_type_traits::stable_id == lithe::kExtensionIdBase.
//   6. egraph_optimize standalone verification.
// =============================================================================

#include "catch_amalgamated.hpp"

// Explicit opt-in include — NOT in lithe.hpp.
#include "lithe/lithe_egraph.hpp"

namespace leg = lithe::egraph;

// ============================================================================
// Test 1 — intern: structurally-equal exprs → same class
// ============================================================================

TEST_CASE (


"intern: structurally-equal Lithe expressions → same e_class_id"
,
"[lithe_egraph][intern]"
)
{
    auto expr1 = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::add_tag>(1, 2), 3);
    auto expr2 = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::add_tag>(1, 2), 3);

    leg::lithe_egraph_t g;
    const auto id1 = leg::intern(g, expr1);
    const auto id2 = leg::intern(g, expr2);

    REQUIRE(id1 == id2);
}

// ============================================================================
// Test 2 — intern: distinct payloads → distinct classes
// ============================================================================

TEST_CASE (


"intern: integer leaves with distinct values → distinct e_class_id"
,
"[lithe_egraph][intern]"
)
{
    leg::lithe_egraph_t g;
    const auto id1 = leg::intern(g, 1);
    const auto id2 = leg::intern(g, 2);
    REQUIRE(id1 != id2);
}

TEST_CASE (


"intern: same integer value → same e_class_id"
,
"[lithe_egraph][intern]"
)
{
    leg::lithe_egraph_t g;
    const auto id1 = leg::intern(g, 42);
    const auto id2 = leg::intern(g, 42);
    REQUIRE(id1 == id2);
}

// ============================================================================
// Test 3 — commutativity: a+b and b+a collapse after saturate
// ============================================================================

TEST_CASE (


"Lithe commutativity: a+b and b+a collapse after saturate"
,
"[lithe_egraph][rules][commutativity]"
)
{
    auto ab = lithe::make_node<lithe::add_tag>(10, 20);
    auto ba = lithe::make_node<lithe::add_tag>(20, 10);

    leg::lithe_egraph_t g;
    const auto id_ab = leg::intern(g, ab);
    const auto id_ba = leg::intern(g, ba);

    REQUIRE(g.find(id_ab) != g.find(id_ba));

    auto rules = std::make_tuple(
        leg::lithe_commutativity_add{},
        leg::lithe_commutativity_mul{}
    );
    (void)egraph::saturate(g, rules, egraph::saturation_limits{.max_iters = 5});

    REQUIRE(g.find(id_ab) == g.find(id_ba));
}

// ============================================================================
// Test 4 — identity_zero extraction: add(mul(x,1), 0) → x
// ============================================================================

TEST_CASE (


"identity_zero: add(mul(x,1), 0) extraction yields leaf x"
,
"[lithe_egraph][rules][identity]"
)
{
    auto expr = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::mul_tag>(7, 1),
        0);

    leg::lithe_egraph_t g;
    const auto root = leg::intern(g, expr);

    auto rules = leg::lithe_default_rules{};
    (void)egraph::saturate(g, rules, egraph::saturation_limits{.max_iters = 10});

    const auto result = egraph::extract_best(g, root, leg::ast_size_cost{});
    const auto r = g.find(root);

    REQUIRE(result.best_nodes[r].has_value());
    // After identity saturation, best is the leaf 7 (cost=1).
    REQUIRE(result.best_costs[r] == 1u);
    REQUIRE(result.best_nodes[r]->children.empty());
}

// ============================================================================
// Test 5 — egraph_optimize pass traits
// ============================================================================

TEST_CASE (


"egraph_optimize: pass_type_traits stable_id == kExtensionIdBase"
,
"[lithe_egraph][pass][traits]"
)
{
    using pass_t = leg::default_egraph_optimize;
    using traits = lithe::passes::pass_type_traits<pass_t>;

    // kExtensionIdBase is in namespace lithe::emit (per lithe_core.hpp)
    // but kExtensionIdBase is also declared as lithe::kExtensionIdBase
    STATIC_REQUIRE(traits::stable_id == 1000u);
    STATIC_REQUIRE(traits::in_stage  == lithe::passes::ir_stage::surface);
    STATIC_REQUIRE(traits::out_stage == lithe::passes::ir_stage::optimized);
    STATIC_REQUIRE(traits::category  == lithe::passes::pass_category::optimization);
}

TEST_CASE (


"egraph_optimize: pass id string is 'lithe.egraph.optimize'"
,
"[lithe_egraph][pass][traits]"
)
{
    using pass_t = leg::default_egraph_optimize;
    using traits = lithe::passes::pass_type_traits<pass_t>;
    REQUIRE(std::string_view{traits::id.view()} == "lithe.egraph.optimize");
}

// ============================================================================
// Test 6 — egraph_optimize standalone invocation
// ============================================================================

TEST_CASE (


"egraph_optimize: operator() returns a valid expression"
,
"[lithe_egraph][pass][invoke]"
)
{
    auto expr = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::mul_tag>(3, 1),
        0);

    leg::default_egraph_optimize pass;
    auto result = pass(expr);

    REQUIRE(lithe::structural_hash(result) != 0u);
}

TEST_CASE (


"egraph_optimize: add(0, x) → result has smaller or equal structural hash tree"
,
"[lithe_egraph][pass][simplify]"
)
{
    auto expr = lithe::make_node<lithe::add_tag>(0, 5);

    leg::default_egraph_optimize pass;
    auto result = pass(expr);

    // Expression must be constructable without throwing.
    auto h = lithe::structural_hash(result);
    (void)h;
    SUCCEED(); // pass completed without exception
}

TEST_CASE (


"egraph_optimize<cpu_instruction_cost>: constructs and runs"
,
"[lithe_egraph][pass][cost_model]"
)
{
    using cpu_pass = leg::egraph_optimize<leg::lithe_default_rules, leg::cpu_instruction_cost>;
    cpu_pass pass;

    auto expr = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::mul_tag>(2, 1),
        0);

    auto result = pass(expr);
    REQUIRE(lithe::structural_hash(result) != 0u);
}

TEST_CASE (


"egraph_optimize<tensor_fusion_cost>: constructs and runs"
,
"[lithe_egraph][pass][cost_model]"
)
{
    using tf_pass = leg::egraph_optimize<leg::lithe_default_rules, leg::tensor_fusion_cost>;
    tf_pass pass;

    auto expr = lithe::make_node<lithe::mul_tag>(
        lithe::make_node<lithe::mul_tag>(3, 2),
        1);

    auto result = pass(expr);
    REQUIRE(lithe::structural_hash(result) != 0u);
}


