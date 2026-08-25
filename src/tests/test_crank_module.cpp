// =============================================================================
// test_crank_module.cpp — Crank module resolver unit tests (Module 2).
//
// Verifies: include/languages/crank/module.hpp
//           include/languages/crank/dump.hpp (dump_module_graph)
//
//  1. module_hash is stable across identical source.
//  2. module_hash differs for different source.
//  3. Resolver: native beats project path (precedence).
//  4. Resolver: import "math.vector" maps to math/vector.crank path.
//  5. Resolver: embedded src is retrievable after registration.
//  6. Resolver: returns nullopt for unknown module.
//  7. dependency_graph: topo_order on linear A→B→C.
//  8. dependency_graph: cycle detection returns empty topo_order.
//  9. dump_module_graph produces non-empty JSON.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/module.hpp"
#include "languages/crank/module_generics.hpp"
#include "languages/crank/dump.hpp"

#include <fstream>

using namespace crank;

// ============================================================================
// Test 1 — module_hash is stable across identical source
// ============================================================================

TEST_CASE (

"crank module_hash: stable across identical source"
,
"[crank][module]"
)
 {
    std::string src = "package math\nfn Dot() -> Float32 { return 0.0 }";
    module_hash h1 = hash_source(src);
    module_hash h2 = hash_source(src);
    CHECK(h1 == h2);
    CHECK_FALSE(h1.empty());
}

// ============================================================================
// Test 2 — module_hash differs for different source
// ============================================================================

TEST_CASE (

"crank module_hash: differs for different source"
,
"[crank][module]"
)
 {
    module_hash ha = hash_source("package a");
    module_hash hb = hash_source("package b");
    CHECK(ha != hb);
}

// ============================================================================
// Test 3 — resolver: native beats project path
// ============================================================================

TEST_CASE (

"crank module_resolver: native takes precedence over project path"
,
"[crank][module]"
)
 {
    module_resolver res;

    // Register a project path that contains "math.vector"
    auto tmp = std::filesystem::temp_directory_path() / "crank_test_proj";
    std::filesystem::create_directories(tmp / "math");
    {
        std::ofstream f(tmp / "math" / "vector.crank");
        f << "package math";
    }
    res.add_project_path(tmp);

    // Register a native descriptor with the same name
    module_descriptor native_d;
    native_d.name = "math.vector";
    native_d.kind = module_kind::native;
    res.add_native(native_d);

    auto result = res.resolve("math.vector");
    REQUIRE(result.has_value());
    CHECK(result->kind == module_kind::native);

    // Cleanup
    std::filesystem::remove_all(tmp);
}

// ============================================================================
// Test 4 — resolver: import "math.vector" maps to math/vector.crank path
// ============================================================================

TEST_CASE (

"crank module_resolver: import maps to filesystem path"
,
"[crank][module]"
)
 {
    module_resolver res;

    auto tmp = std::filesystem::temp_directory_path() / "crank_test_path";
    std::filesystem::create_directories(tmp / "math");
    {
        std::ofstream f(tmp / "math" / "vector.crank");
        f << "package math";
    }
    res.add_project_path(tmp);

    auto result = res.resolve("math.vector");
    REQUIRE(result.has_value());
    CHECK(result->kind == module_kind::source);
    CHECK(result->source_path.find("math") != std::string::npos);
    CHECK(result->source_path.find("vector.crank") != std::string::npos);

    std::filesystem::remove_all(tmp);
}

// ============================================================================
// Test 5 — resolver: embedded src is retrievable
// ============================================================================

TEST_CASE (

"crank module_resolver: embedded src is retrievable"
,
"[crank][module]"
)
 {
    module_resolver res;
    std::string src = "package utils\nfn helper() -> Unit {}";
    res.add_embedded_src("utils.helper", src);

    auto result = res.resolve("utils.helper");
    REQUIRE(result.has_value());
    CHECK(result->kind == module_kind::embedded_src);
    CHECK_FALSE(result->content_hash.empty());

    auto text = res.source_text("utils.helper");
    REQUIRE(text.has_value());
    CHECK(*text == src);
}

// ============================================================================
// Test 6 — resolver: unknown module returns nullopt
// ============================================================================

TEST_CASE (

"crank module_resolver: unknown module returns nullopt"
,
"[crank][module]"
)
 {
    module_resolver res;
    auto result = res.resolve("no.such.module");
    CHECK_FALSE(result.has_value());
}

// ============================================================================
// Test 7 — dependency_graph: topo_order on A→B→C
// ============================================================================

TEST_CASE (

"crank dependency_graph: topo_order linear A→B→C"
,
"[crank][module]"
)
 {
    dependency_graph g;

    auto mk = [](std::string n) {
        module_descriptor d;
        d.name = n;
        d.kind = module_kind::embedded_src;
        return d;
    };

    g.add_module(mk("A"));
    g.add_module(mk("B"));
    g.add_module(mk("C"));
    g.add_import("A", "B");
    g.add_import("B", "C");

    auto order = g.topo_order();
    REQUIRE(order.size() == 3u);
    // C must come before B, B before A in dependency order
    auto pos = [&](std::string_view name) {
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i] == name) return i;
        return std::size_t(-1);
    };
    CHECK(pos("C") < pos("B"));
    CHECK(pos("B") < pos("A"));
}

// ============================================================================
// Test 8 — dependency_graph: cycle detection returns empty topo_order
// ============================================================================

TEST_CASE (

"crank dependency_graph: cycle returns empty topo_order"
,
"[crank][module]"
)
 {
    dependency_graph g;

    auto mk = [](std::string n) {
        module_descriptor d;
        d.name = n;
        return d;
    };
    g.add_module(mk("X"));
    g.add_module(mk("Y"));
    g.add_import("X", "Y");
    g.add_import("Y", "X");  // cycle

    auto order = g.topo_order();
    CHECK(order.empty());
}

// ============================================================================
// Test 9 — dump_module_graph produces non-empty JSON
// ============================================================================

TEST_CASE (

"crank dump_module_graph produces JSON"
,
"[crank][module][dump]"
)
 {
    dependency_graph g;
    module_descriptor a, b;
    a.name = "app"; b.name = "utils";
    g.add_module(a); g.add_module(b);
    g.add_import("app", "utils");

    std::string json = dump_module_graph(g);
    REQUIRE_FALSE(json.empty());
    CHECK(json.find("app")   != std::string::npos);
    CHECK(json.find("utils") != std::string::npos);
}

// =============================================================================
// §v2.3 generic modules — appended for v2. Existing tests above are unchanged.
// =============================================================================

static crank::generic_module_descriptor make_ring_module() {
    crank::generic_module_descriptor gm;
    gm.base.name = "Ring";
    gm.type_params.push_back({"T", {"Numeric"}});
    gm.const_params.push_back({"N", crank::const_param_kind::usize});
    gm.exports.push_back({"add", true});
    gm.exports.push_back({"mul", true});
    gm.exports.push_back({"secret_helper", false}); // not pub
    return gm;
}

TEST_CASE (

"v2.3 generic_module_descriptor reports arity + generic-ness"
,
"[crank][module][generic][v2]"
)
 {
    auto gm = make_ring_module();
    CHECK(gm.is_generic());
    CHECK(gm.arity() == 2u);
    CHECK(gm.exports_symbol("add"));
    CHECK(gm.exports_symbol("mul"));
    CHECK_FALSE(gm.exports_symbol("secret_helper")); // non-pub
    CHECK_FALSE(gm.exports_symbol("nope"));
}

TEST_CASE (

"v2.3 instantiate_module monomorphizes with a distinct AOT key"
,
"[crank][module][generic][v2]"
)
 {
    auto gm = make_ring_module();

    crank::type_arg t_i64{1, "Int64", 0xABCD};
    crank::const_arg n4{crank::const_param_kind::usize, 4, false, ""};

    auto r = crank::instantiate_module(gm, {t_i64}, {n4});
    REQUIRE(r.ok());
    CHECK(r.aot_key != 0u);
    CHECK(r.descriptor.name.rfind("Ring#", 0) == 0u);     // "Ring#<fp>"
    CHECK(r.descriptor.content_hash.value != gm.base.content_hash.value);
}

TEST_CASE (

"v2.3 distinct arguments yield distinct instantiations"
,
"[crank][module][generic][v2]"
)
 {
    auto gm = make_ring_module();
    crank::type_arg t_i64{1, "Int64", 0xABCD};
    crank::const_arg n4{crank::const_param_kind::usize, 4, false, ""};
    crank::const_arg n8{crank::const_param_kind::usize, 8, false, ""};

    auto r4 = crank::instantiate_module(gm, {t_i64}, {n4});
    auto r8 = crank::instantiate_module(gm, {t_i64}, {n8});
    REQUIRE(r4.ok());
    REQUIRE(r8.ok());
    CHECK(r4.aot_key != r8.aot_key);
    CHECK(r4.descriptor.name != r8.descriptor.name);
}

TEST_CASE (

"v2.3 identical arguments dedup to the same AOT key"
,
"[crank][module][generic][v2]"
)
 {
    auto gm = make_ring_module();
    crank::type_arg t_i64{1, "Int64", 0xABCD};
    crank::const_arg n4{crank::const_param_kind::usize, 4, false, ""};

    auto a = crank::instantiate_module(gm, {t_i64}, {n4});
    auto b = crank::instantiate_module(gm, {t_i64}, {n4});
    CHECK(a.aot_key == b.aot_key);
    CHECK(a.descriptor.name == b.descriptor.name);
}

TEST_CASE (

"v2.3 arity mismatch → CRANK-MOD-GEN-001"
,
"[crank][module][generic][v2]"
)
 {
    auto gm = make_ring_module();
    // Missing the const argument.
    crank::type_arg t_i64{1, "Int64", 0xABCD};
    auto r = crank::instantiate_module(gm, {t_i64}, {});
    REQUIRE_FALSE(r.ok());
    CHECK(r.diagnostics.front().find("CRANK-MOD-GEN-001") != std::string::npos);
}

