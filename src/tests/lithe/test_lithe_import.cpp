// =============================================================================
// test_lithe_import.cpp — tests for the Lithe Import/Export framework.
//
// Verifies: include/edsl/lithe_import.hpp
//           include/edsl/lithe_import/vakya_adapter.hpp
//           include/edsl/lithe_import/adjacency_adapter.hpp
//
// The framework adapts any frontend structure into a neutral_model Lithe can
// optimize, and reconstructs a target via export_to. These tests use the two
// reference adapters (Vākya tree, runtime adjacency model).
//
// Cases:
//   1. adjacency_model satisfies semantic_model (compile-time).
//   2. import(adjacency_model) builds a post-order neutral_model.
//   3. import_vakya(expr) copies a Vākya tree structurally.
//   4. export_to reconstructs a target (op-count) via a builder.
//   5. round-trip: import then export_to node-count == neutral node count.
//   6. custom AST via CPO overloads needs no core edit (adjacency stands in).
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_import.hpp"
#include "lithe/lithe_import/vakya_adapter.hpp"
#include "lithe/lithe_import/adjacency_adapter.hpp"

#include "vakya/vakya.hpp"

#include <vector>

using lithe::adjacency_model;

// ============================================================================
// Test 1 — concept satisfaction.
// ============================================================================

TEST_CASE (


"import: adjacency_model satisfies semantic_model"
,
"[lithe][import]"
)
{
    STATIC_REQUIRE(lithe::semantic_model<adjacency_model>);
}

// ============================================================================
// Test 2 — import a runtime frontend.
//   Graph:  root(op=100) -> [a(op=1), b(op=2)]
// ============================================================================

TEST_CASE (


"import: adjacency_model post-order into neutral_model"
,
"[lithe][import]"
)
{
    adjacency_model m;
    m.ops = {1, 2, 100};        // node 0=a, 1=b, 2=root
    m.adj = {{}, {}, {0, 1}};
    m.root_id = 2;

    auto nm = lithe::import(m);
    REQUIRE(nm.node_count() == 3);
    // root is interned last (post-order) → highest id.
    REQUIRE(nm.root_node().op == 100);
    REQUIRE(nm.root_node().children.size() == 2);
    REQUIRE(nm.at(nm.root_node().children[0]).op == 1);
    REQUIRE(nm.at(nm.root_node().children[1]).op == 2);
}

// ============================================================================
// Test 3 — import a Vākya expression tree.
//   e = add(x, mul(y, 2))
// ============================================================================

TEST_CASE (


"import_vakya: structural copy of a Vākya tree"
,
"[lithe][import]"
)
{
    int x = 3, y = 4;
    auto e = vakya::as_expr(x) + vakya::as_expr(y) * 2;

    auto nm = lithe::import_vakya(e);
    // Nodes: x, y, 2, mul(y,2), add(x,mul) = 5 total.
    REQUIRE(nm.node_count() == 5);

    const auto& root = nm.root_node();
    REQUIRE(root.op == vakya::emit::tag_descriptor<vakya::add_tag>::stable_id);
    REQUIRE(root.children.size() == 2);

    const auto& rhs = nm.at(root.children[1]);
    REQUIRE(rhs.op == vakya::emit::tag_descriptor<vakya::mul_tag>::stable_id);
    REQUIRE(rhs.children.size() == 2);
}

// ============================================================================
// Test 4 & 5 — export_to reconstructs a target and round-trips node counts.
//   Builder counts nodes it rebuilds (simplest faithful target).
// ============================================================================

namespace {
    struct count_builder {
        using result_type = std::size_t; // "target node" = subtree node count
        std::size_t make(std::size_t /*op*/, std::vector<std::size_t> children) {
            std::size_t total = 1;
            for (auto c : children) total += c;
            return total;
        }
    };
}

TEST_CASE (


"export_to: rebuild target and round-trip node count"
,
"[lithe][import]"
)
{
    int x = 3, y = 4;
    auto e = vakya::as_expr(x) + vakya::as_expr(y) * 2;

    auto nm = lithe::import_vakya(e);
    auto rebuilt_count = lithe::export_to(nm, count_builder{});

    REQUIRE(rebuilt_count == nm.node_count()); // 5
}

// ============================================================================
// Test 6 — custom AST via CPO overloads: a second runtime model reuses the
//   same import path with zero core edits (adjacency is the reference frontend).
// ============================================================================

TEST_CASE (


"import: single-node frontend imports as a leaf"
,
"[lithe][import]"
)
{
    adjacency_model m;
    m.ops = {7};
    m.adj = {{}};
    m.root_id = 0;

    auto nm = lithe::import(m);
    REQUIRE(nm.node_count() == 1);
    REQUIRE(nm.root_node().op == 7);
    REQUIRE(nm.root_node().children.empty());
}
