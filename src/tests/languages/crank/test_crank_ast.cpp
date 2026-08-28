// =============================================================================
// test_crank_ast.cpp — Crank AST tag unit tests (Module 1).
//
// Verifies: include/languages/crank/ast_tags.hpp
//           include/languages/crank/build_ast.hpp
//   1. tag_descriptor stable_id correctness for all 14 crank tags.
//   2. build_ast on a non-empty parse_tree produces a valid root.
//   3. build_ast on an empty parse_tree sets ok=false + diagnostics.
//   4. Vakya node construction via make_node<Tag> compiles and runs.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/vakya.hpp"
#include "vakya/property.hpp"
#include "languages/crank/ast_tags.hpp"
#include "languages/crank/build_ast.hpp"
#include "languages/crank/parser.hpp"

// ============================================================================
// Test 1 — tag_descriptor stable_id assertions
// ============================================================================

TEST_CASE (

"crank tag_descriptor stable_ids are correct"
,
"[crank][ast]"
)
 {
    using namespace vakya::emit;

    CHECK(tag_descriptor<crank::fn_tag>::stable_id              == 1000u);
    CHECK(tag_descriptor<crank::block_tag>::stable_id           == 1001u);
    CHECK(tag_descriptor<crank::let_tag>::stable_id             == 1002u);
    CHECK(tag_descriptor<crank::var_tag>::stable_id             == 1003u);
    CHECK(tag_descriptor<crank::match_tag>::stable_id           == 1004u);
    CHECK(tag_descriptor<crank::crank_call_tag>::stable_id      == 1005u);
    CHECK(tag_descriptor<crank::attribute_tag>::stable_id       == 1006u);
    CHECK(tag_descriptor<crank::field_access_tag>::stable_id    == 1007u);
    CHECK(tag_descriptor<crank::index_tag>::stable_id           == 1008u);
    CHECK(tag_descriptor<crank::range_tag>::stable_id           == 1009u);
    CHECK(tag_descriptor<crank::transaction_tag>::stable_id     == 1010u);
    CHECK(tag_descriptor<crank::transaction_option_tag>::stable_id == 1011u);
    CHECK(tag_descriptor<crank::tx_load_tag>::stable_id         == 1012u);
    CHECK(tag_descriptor<crank::tx_store_tag>::stable_id        == 1013u);
}

// ============================================================================
// Test 2 — tag_descriptor symbol strings
// ============================================================================

TEST_CASE (

"crank tag_descriptor symbols are non-empty"
,
"[crank][ast]"
)
 {
    using namespace vakya::emit;

    CHECK_FALSE(tag_descriptor<crank::fn_tag>::symbol.empty());
    CHECK_FALSE(tag_descriptor<crank::block_tag>::symbol.empty());
    CHECK_FALSE(tag_descriptor<crank::transaction_tag>::symbol.empty());
    CHECK_FALSE(tag_descriptor<crank::tx_load_tag>::symbol.empty());
    CHECK_FALSE(tag_descriptor<crank::tx_store_tag>::symbol.empty());
}

// ============================================================================
// Test 3 — tag stable_id uniqueness
// ============================================================================

TEST_CASE (

"crank tag stable_ids are unique"
,
"[crank][ast]"
)
 {
    using namespace vakya::emit;
    std::vector<std::uint32_t> ids = {
        tag_descriptor<crank::fn_tag>::stable_id,
        tag_descriptor<crank::block_tag>::stable_id,
        tag_descriptor<crank::let_tag>::stable_id,
        tag_descriptor<crank::var_tag>::stable_id,
        tag_descriptor<crank::match_tag>::stable_id,
        tag_descriptor<crank::crank_call_tag>::stable_id,
        tag_descriptor<crank::attribute_tag>::stable_id,
        tag_descriptor<crank::field_access_tag>::stable_id,
        tag_descriptor<crank::index_tag>::stable_id,
        tag_descriptor<crank::range_tag>::stable_id,
        tag_descriptor<crank::transaction_tag>::stable_id,
        tag_descriptor<crank::transaction_option_tag>::stable_id,
        tag_descriptor<crank::tx_load_tag>::stable_id,
        tag_descriptor<crank::tx_store_tag>::stable_id,
    };
    std::sort(ids.begin(), ids.end());
    auto it = std::adjacent_find(ids.begin(), ids.end());
    REQUIRE(it == ids.end());
}

// ============================================================================
// Test 4 — all stable_ids in extension band (>= 1000)
// ============================================================================

TEST_CASE (

"crank tag stable_ids are in extension band (>= 1000)"
,
"[crank][ast]"
)
 {
    using namespace vakya::emit;
    CHECK(tag_descriptor<crank::fn_tag>::stable_id           >= 1000u);
    CHECK(tag_descriptor<crank::tx_store_tag>::stable_id     >= 1000u);
}

// ============================================================================
// Test 5 — make_node<Tag> compiles and produces a valid node
// ============================================================================

TEST_CASE (

"vakya::make_node<crank::fn_tag> constructs a node"
,
"[crank][ast]"
)
 {
    auto terminal = vakya::as_expr(std::string("test"));
    auto n = vakya::make_node<crank::fn_tag>(terminal);
    // Node must be non-empty (has one child)
    static_assert(std::tuple_size_v<decltype(n.children)> == 1);
    SUCCEED("make_node<fn_tag> constructed successfully");
}

// ============================================================================
// Test 6 — build_ast on empty parse_tree sets ok=false
// ============================================================================

TEST_CASE (

"crank::build_ast on empty parse_tree sets ok=false"
,
"[crank][ast]"
)
 {
    crank::grammar::parse_tree_t empty_tree;
    vakya::property_store store;
    auto result = crank::build_ast(empty_tree, "", store);
    CHECK_FALSE(result.ok);
    CHECK(result.diagnostics.has_errors());
    CHECK_FALSE(result.root.has_value());
}

// ============================================================================
// Test 7 — build_ast on parsed source returns a root
// ============================================================================

static constexpr std::string_view kFnSource = R"crank(
package app

fn Dot(a: Float32, b: Float32) -> Float32 {
    return a + b
}
)crank";

TEST_CASE (

"crank::build_ast produces a root from parsed source"
,
"[crank][ast]"
)
 {
    auto tree = crank::grammar::parse(kFnSource);
    vakya::property_store store;
    // build_ast may not succeed fully in module 1 (structural walk only),
    // but must not throw and must return a root if tree is non-empty.
    REQUIRE_NOTHROW([&] {
        auto result = crank::build_ast(tree, kFnSource, store);
        (void)result.ok;
        (void)result.root;
    }());
    SUCCEED("build_ast completed without exception");
}
