// =============================================================================
// test_crank_ir_storage.cpp — Stage 8/8b: crank storage convergence onto ir_module.
//
// Verifies: include/languages/crank/build_ast.hpp
//           include/languages/generic/ir/ir_module.hpp
//
// Stage 8 (type-alias correctness):
//   1. crank_ir_module is ir_module<crank_kind, crank_node_ext> (static_assert).
//   2. crank_kind covers all crank_ast_node variant alternatives.
//   3. ir_node<crank_kind, crank_node_ext> field round-trip (push + access).
//   4. append_children + children() round-trip.
//   5. structural_hash carried in ir_node (no separate side table).
//   6. crank_ast_arena still compiles and is unchanged (backward compat).
//   7. as_egraph_view() / as_adjacency() return valid views on crank_ir_module.
//   8. crank_ir_module::reset() clears nodes and children, preserves capacity.
//
// Stage 8b (live parse → dual-write):
//   9.  build_ast populates ir_mod — node count matches arena node count.
//  10.  ir_mod root is set after a successful parse.
//  11.  fn node in ir_mod has kind == crank_kind::fn and ext.name populated.
//  12.  ir_mod as_egraph_view() adjacency is non-empty for a fn with a block child.
//  13.  crank_ast_arena and ir_mod agree on node count (dual-write parity).
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/build_ast.hpp"
#include "languages/crank/parser.hpp"
#include "languages/generic/ir/ir_module.hpp"
#include "languages/generic/ir/node.hpp"
#include "vakya/property.hpp"

#include <cstdint>
#include <string>
#include <vector>

// =============================================================================
// Test 1 — compile-time: crank_ir_module is ir_module<crank_kind, crank_node_ext>
// =============================================================================

static_assert(std::is_same_v<crank::crank_ir_module,
                              lang::ir_module<crank::crank_kind, crank::crank_node_ext>>,
              "crank_ir_module must alias lang::ir_module<crank_kind, crank_node_ext>");

// =============================================================================
// Test 2 — crank_kind ordinals and coverage
// =============================================================================

TEST_CASE("crank_kind covers all variant alternatives", "[crank][ir_storage]")
{
    using K = crank::crank_kind;
    // All ordinals must be distinct — check key boundaries.
    CHECK(static_cast<std::uint8_t>(K::fn)           ==  0);
    CHECK(static_cast<std::uint8_t>(K::block)        ==  1);
    CHECK(static_cast<std::uint8_t>(K::let)          ==  2);
    CHECK(static_cast<std::uint8_t>(K::var)          ==  3);
    CHECK(static_cast<std::uint8_t>(K::match)        ==  4);
    CHECK(static_cast<std::uint8_t>(K::call)         ==  5);
    CHECK(static_cast<std::uint8_t>(K::attribute)    ==  6);
    CHECK(static_cast<std::uint8_t>(K::field_access) ==  7);
    CHECK(static_cast<std::uint8_t>(K::index)        ==  8);
    CHECK(static_cast<std::uint8_t>(K::range)        ==  9);
    CHECK(static_cast<std::uint8_t>(K::tx)           == 10);
    CHECK(static_cast<std::uint8_t>(K::tx_option)    == 11);
    CHECK(static_cast<std::uint8_t>(K::tx_load)      == 12);
    CHECK(static_cast<std::uint8_t>(K::tx_store)     == 13);
    CHECK(static_cast<std::uint8_t>(K::tx_abort)     == 14);
    CHECK(static_cast<std::uint8_t>(K::tx_yield)     == 15);
    CHECK(static_cast<std::uint8_t>(K::view_decl)    == 16);
    CHECK(static_cast<std::uint8_t>(K::view_expr)    == 17);
    CHECK(static_cast<std::uint8_t>(K::extern_fn)    == 18);
    CHECK(static_cast<std::uint8_t>(K::if_)          == 19);
    CHECK(static_cast<std::uint8_t>(K::for_)         == 20);
    CHECK(static_cast<std::uint8_t>(K::while_)       == 21);
    CHECK(static_cast<std::uint8_t>(K::return_)      == 22);
    CHECK(static_cast<std::uint8_t>(K::spawn)        == 23);
    CHECK(static_cast<std::uint8_t>(K::await)        == 24);
    CHECK(static_cast<std::uint8_t>(K::defer)        == 25);
    CHECK(static_cast<std::uint8_t>(K::type_decl)    == 26);
    CHECK(static_cast<std::uint8_t>(K::module_decl)  == 27);
    CHECK(static_cast<std::uint8_t>(K::literal)      == 28);
    CHECK(static_cast<std::uint8_t>(K::ident)        == 29);
}

// =============================================================================
// Test 3 — ir_node field round-trip
// =============================================================================

TEST_CASE("crank_ir_module push and access round-trip", "[crank][ir_storage]")
{
    crank::crank_ir_module mod;
    REQUIRE(mod.empty());
    REQUIRE(mod.size() == 0);

    // Push a fn node.
    lang::ir_node<crank::crank_kind, crank::crank_node_ext> fn_nd{};
    fn_nd.kind             = crank::crank_kind::fn;
    fn_nd.structural_hash  = 0xDEADBEEF'CAFEBABE;
    fn_nd.ext.name         = "my_function";
    const lang::ir_node_id fn_id = mod.push(fn_nd);

    CHECK(fn_id == 0);
    CHECK(mod.size() == 1);
    CHECK_FALSE(mod.empty());

    const auto& got = mod[fn_id];
    CHECK(got.kind             == crank::crank_kind::fn);
    CHECK(got.structural_hash  == 0xDEADBEEF'CAFEBABE);
    CHECK(got.ext.name         == "my_function");

    // Push a literal child.
    lang::ir_node<crank::crank_kind, crank::crank_node_ext> lit_nd{};
    lit_nd.kind      = crank::crank_kind::literal;
    lit_nd.ext.text  = "42";
    const lang::ir_node_id lit_id = mod.push(lit_nd);
    CHECK(lit_id == 1);
    CHECK(mod[lit_id].ext.text == "42");
}

// =============================================================================
// Test 4 — append_children + children() round-trip
// =============================================================================

TEST_CASE("crank_ir_module append_children and children round-trip", "[crank][ir_storage]")
{
    crank::crank_ir_module mod;

    // Push block node (parent) and two ident children.
    lang::ir_node<crank::crank_kind, crank::crank_node_ext> blk{};
    blk.kind = crank::crank_kind::block;
    const lang::ir_node_id blk_id = mod.push(blk);

    lang::ir_node<crank::crank_kind, crank::crank_node_ext> id0{};
    id0.kind = crank::crank_kind::ident;
    id0.ext.name = "x";
    const lang::ir_node_id id0_id = mod.push(id0);

    lang::ir_node<crank::crank_kind, crank::crank_node_ext> id1{};
    id1.kind = crank::crank_kind::ident;
    id1.ext.name = "y";
    const lang::ir_node_id id1_id = mod.push(id1);

    const std::vector<lang::ir_node_id> kids = {id0_id, id1_id};
    mod.append_children(blk_id, kids);

    auto ch = mod.children(blk_id);
    REQUIRE(ch.size() == 2);
    CHECK(ch[0] == id0_id);
    CHECK(ch[1] == id1_id);
    CHECK(mod[ch[0]].ext.name == "x");
    CHECK(mod[ch[1]].ext.name == "y");
}

// =============================================================================
// Test 5 — structural_hash in ir_node (no side table)
// =============================================================================

TEST_CASE("crank_ir_module structural_hash lives in ir_node", "[crank][ir_storage]")
{
    crank::crank_ir_module mod;

    lang::ir_node<crank::crank_kind, crank::crank_node_ext> nd{};
    nd.kind            = crank::crank_kind::fn;
    nd.structural_hash = 0x1234'5678'9ABC'DEF0;
    nd.ext.name        = "hash_test";
    const lang::ir_node_id nid = mod.push(nd);

    // structural_hash accessible directly — no side table.
    CHECK(mod[nid].structural_hash == 0x1234'5678'9ABC'DEF0);
    CHECK(mod[nid].ext.name        == "hash_test");
}

// =============================================================================
// Test 6 — crank_ast_arena unchanged (backward compatibility)
// =============================================================================

TEST_CASE("crank_ast_arena unchanged after Stage 8", "[crank][ir_storage]")
{
    crank::crank_ast_arena arena;
    REQUIRE(arena.empty());

    const lang::ast_node_id fid = arena.push(crank::fn_node{"main", {}});
    CHECK(fid == 0);
    CHECK(arena.size() == 1);
    CHECK_FALSE(arena.empty());

    const auto& fn = std::get<crank::fn_node>(arena[fid]);
    CHECK(fn.name == "main");
    CHECK(fn.children.empty());
}

// =============================================================================
// Test 7 — as_egraph_view() / as_adjacency() on crank_ir_module
// =============================================================================

TEST_CASE("crank_ir_module graph views are valid", "[crank][ir_storage]")
{
    crank::crank_ir_module mod;

    lang::ir_node<crank::crank_kind, crank::crank_node_ext> root{};
    root.kind = crank::crank_kind::block;
    const lang::ir_node_id root_id = mod.push(root);

    lang::ir_node<crank::crank_kind, crank::crank_node_ext> child{};
    child.kind = crank::crank_kind::literal;
    child.ext.text = "0";
    const lang::ir_node_id child_id = mod.push(child);

    const std::vector<lang::ir_node_id> kids = {child_id};
    mod.append_children(root_id, kids);
    mod.set_root(root_id);

    const auto egv = mod.as_egraph_view();
    CHECK(egv.node_count() == 2);
    const auto adj = egv.adj(root_id);
    REQUIRE(adj.size() == 1);
    CHECK(adj[0] == child_id);

    // as_adjacency() is the same view.
    const auto adv = mod.as_adjacency();
    CHECK(adv.node_count() == 2);
    CHECK(adv.adj(root_id).size() == 1);
}

// =============================================================================
// Test 8 — reset() clears nodes and children
// =============================================================================

TEST_CASE("crank_ir_module reset clears state", "[crank][ir_storage]")
{
    crank::crank_ir_module mod;

    lang::ir_node<crank::crank_kind, crank::crank_node_ext> nd{};
    nd.kind = crank::crank_kind::fn;
    nd.ext.name = "f";
    [[maybe_unused]] const lang::ir_node_id nid = mod.push(nd);
    REQUIRE(mod.size() == 1);

    mod.reset();
    CHECK(mod.empty());
    CHECK(mod.size() == 0);
    CHECK(mod.root() == lang::k_null_ir);
}

// =============================================================================
// Stage 8b — live parse → dual-write into ir_mod
// =============================================================================

static constexpr std::string_view k8bSource = R"crank(
package app

fn Dot(a: Float32, b: Float32) -> Float32 {
    return a + b
}
)crank";

// Test 9 — build_ast populates ir_mod with at least as many nodes as arena
TEST_CASE("Stage8b: build_ast populates ir_mod alongside arena", "[crank][ir_storage][8b]")
{
    auto tree = crank::grammar::parse(k8bSource);
    vakya::property_store store;
    auto result = crank::build_ast(tree, k8bSource, store);

    REQUIRE(result.typed_ast_root != nullptr);
    const auto& sf = *result.typed_ast_root;

    // ir_mod must be populated (Stage 8b dual-write).
    CHECK_FALSE(sf.ir_mod.empty());
    // arena still populated (backward compat).
    CHECK_FALSE(sf.arena.empty());
    // Both stores must have the same node count (dual-write parity).
    CHECK(sf.ir_mod.size() == sf.arena.size());
}

// Test 10 — ir_mod root is set after a successful parse
TEST_CASE("Stage8b: ir_mod root is set after parse", "[crank][ir_storage][8b]")
{
    auto tree = crank::grammar::parse(k8bSource);
    vakya::property_store store;
    auto result = crank::build_ast(tree, k8bSource, store);

    REQUIRE(result.typed_ast_root != nullptr);
    const auto& sf = *result.typed_ast_root;

    CHECK(sf.ir_mod.root() != lang::k_null_ir);
    CHECK(sf.ir_mod.root() < sf.ir_mod.size());
}

// Test 11 — fn node in ir_mod has kind::fn and non-empty ext.name
TEST_CASE("Stage8b: fn node in ir_mod has correct kind and name", "[crank][ir_storage][8b]")
{
    auto tree = crank::grammar::parse(k8bSource);
    vakya::property_store store;
    auto result = crank::build_ast(tree, k8bSource, store);

    REQUIRE(result.typed_ast_root != nullptr);
    const auto& ir_mod = result.typed_ast_root->ir_mod;

    bool found_fn = false;
    for (std::uint32_t i = 0; i < ir_mod.size(); ++i) {
        const auto& nd = ir_mod[static_cast<lang::ir_node_id>(i)];
        if (nd.kind == crank::crank_kind::fn && !nd.ext.name.empty()) {
            found_fn = true;
            CHECK(nd.ext.name == "Dot");
            break;
        }
    }
    CHECK(found_fn);
}

// Test 12 — as_egraph_view() adjacency is non-empty for fn with children
TEST_CASE("Stage8b: ir_mod egraph view has adjacency for fn node", "[crank][ir_storage][8b]")
{
    auto tree = crank::grammar::parse(k8bSource);
    vakya::property_store store;
    auto result = crank::build_ast(tree, k8bSource, store);

    REQUIRE(result.typed_ast_root != nullptr);
    const auto& ir_mod = result.typed_ast_root->ir_mod;
    const auto view = ir_mod.as_egraph_view();

    CHECK(view.node_count() > 0);

    // At least one node must have children (fn wraps a block).
    bool any_adj = false;
    for (std::uint32_t i = 0; i < ir_mod.size(); ++i) {
        if (!view.adj(static_cast<lang::ir_node_id>(i)).empty()) {
            any_adj = true;
            break;
        }
    }
    CHECK(any_adj);
}

// Test 13 — crank_ast_arena and ir_mod agree on node count
TEST_CASE("Stage8b: arena and ir_mod node count parity", "[crank][ir_storage][8b]")
{
    auto tree = crank::grammar::parse(k8bSource);
    vakya::property_store store;
    auto result = crank::build_ast(tree, k8bSource, store);

    REQUIRE(result.typed_ast_root != nullptr);
    const auto& sf = *result.typed_ast_root;
    CHECK(sf.arena.size() == sf.ir_mod.size());
}
