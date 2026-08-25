// =============================================================================
// test_crank_module_facade.cpp — Module system facade + FFI module unit tests.
//
// Verifies: include/languages/crank/engine.hpp (module paths)
//           include/languages/crank/ffi_module.hpp
//           include/languages/crank/context.hpp (ffi_module_registry)
//
//  1.  ffi_module_builder round-trip: name, symbol count, symbol kind.
//  2.  ffi_module_builder: fn convenience overload (name == crank_name).
//  3.  ffi_module_descriptor::find() — success path.
//  4.  ffi_module_descriptor::find() — returns nullptr for unknown name.
//  5.  ffi_module_descriptor::to_module_descriptor() — kind = native.
//  6.  ffi_module_descriptor::content_hash — non-zero for non-empty module.
//  7.  ffi_module_registry::register_module + find round-trip.
//  8.  ffi_module_registry::find — nullptr for unregistered module.
//  9.  ffi_module_registry::size / empty / for_each.
// 10.  ffi_module_registry::symbols_of_kind — only function symbols returned.
// 11.  context::register_ffi_module seeds module_resolver native tier.
// 12.  engine::register_ffi_module — fluent, returns engine&.
// 13.  engine::load registered FFI module — returns module_handle.
// 14.  module_handle::exports() populated from ffi_module_descriptor symbols.
// 15.  engine::load unregistered module — CRANK-MOD-001.
// 16.  module_graph_view after FFI module load — module appears.
// 17.  ffi_symbol_kind to_string covers all values.
// 18.  ffi_module_registry replace: re-register same name replaces descriptor.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/engine.hpp"
#include "languages/crank/ffi_module.hpp"

#include <string>
#include <string_view>

// ============================================================================
// Test domain: math FFI module with Dot and Cross functions
// ============================================================================

struct FVec3 { float x, y, z; };

static float fvec_dot(FVec3 a, FVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static FVec3 fvec_cross(FVec3 a, FVec3 b) noexcept {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}

template <>
struct crank::type_descriptor<FVec3> {
    static constexpr std::string_view name = "FVec3";
    static constexpr auto fields = std::tuple{
        crank::field<"x", &FVec3::x>{},
        crank::field<"y", &FVec3::y>{},
        crank::field<"z", &FVec3::z>{}
    };
};

// ============================================================================
// Test 1 — ffi_module_builder round-trip
// ============================================================================

TEST_CASE("ffi_module: builder round-trip", "[crank][ffi_module][builder]") {
    auto mod = crank::ffi_module_builder{"math"}
        .fn("math.dot",   "Dot",   2)
        .fn("math.cross", "Cross", 2)
        .type("math.Vec3", "Vec3")
        .build();

    CHECK(mod.name == "math");
    CHECK(mod.symbols.size() == 3u);
}

// ============================================================================
// Test 2 — fn convenience overload (name == crank_name)
// ============================================================================

TEST_CASE("ffi_module: fn convenience overload", "[crank][ffi_module][builder]") {
    auto mod = crank::ffi_module_builder{"util"}
        .fn("scale", 2)
        .build();

    REQUIRE(mod.symbols.size() == 1u);
    CHECK(mod.symbols[0].name       == "scale");
    CHECK(mod.symbols[0].crank_name == "scale");
    CHECK(mod.symbols[0].kind       == crank::ffi_symbol_kind::function);
    CHECK(mod.symbols[0].arity      == 2u);
}

// ============================================================================
// Test 3 — ffi_module_descriptor::find success
// ============================================================================

TEST_CASE("ffi_module: descriptor find success", "[crank][ffi_module][descriptor]") {
    auto mod = crank::ffi_module_builder{"math"}
        .fn("math.dot", "Dot", 2)
        .build();

    const auto* sym = mod.find("Dot");
    REQUIRE(sym != nullptr);
    CHECK(sym->name       == "math.dot");
    CHECK(sym->crank_name == "Dot");
    CHECK(sym->arity      == 2u);
    CHECK(sym->is_function());
}

// ============================================================================
// Test 4 — ffi_module_descriptor::find nullptr for unknown
// ============================================================================

TEST_CASE("ffi_module: descriptor find nullptr for unknown", "[crank][ffi_module][descriptor]") {
    auto mod = crank::ffi_module_builder{"math"}
        .fn("math.dot", "Dot", 2)
        .build();

    CHECK(mod.find("Norm") == nullptr);
    CHECK(mod.find("") == nullptr);
}

// ============================================================================
// Test 5 — to_module_descriptor kind = native
// ============================================================================

TEST_CASE("ffi_module: to_module_descriptor kind native", "[crank][ffi_module][descriptor]") {
    auto mod = crank::ffi_module_builder{"math"}
        .fn("math.dot", "Dot", 2)
        .build();

    auto md = mod.to_module_descriptor();
    CHECK(md.name == "math");
    CHECK(md.kind == crank::module_kind::native);
}

// ============================================================================
// Test 6 — content_hash non-zero
// ============================================================================

TEST_CASE("ffi_module: content_hash non-zero for non-empty module", "[crank][ffi_module][hash]") {
    auto mod = crank::ffi_module_builder{"math"}
        .fn("math.dot", "Dot", 2)
        .build();

    CHECK_FALSE(mod.content_hash.empty());
}

// ============================================================================
// Test 7 — ffi_module_registry register + find round-trip
// ============================================================================

TEST_CASE("ffi_module: registry register + find round-trip", "[crank][ffi_module][registry]") {
    crank::ffi_module_registry reg;

    auto mod = crank::ffi_module_builder{"math"}
        .fn("math.dot", "Dot", 2)
        .build();
    reg.register_module(mod); // copy to keep local mod alive for check

    const auto* found = reg.find("math");
    REQUIRE(found != nullptr);
    CHECK(found->name == "math");
    CHECK(found->symbols.size() == 1u);
}

// ============================================================================
// Test 8 — registry find nullptr for unregistered
// ============================================================================

TEST_CASE("ffi_module: registry find nullptr for unregistered", "[crank][ffi_module][registry]") {
    crank::ffi_module_registry reg;
    CHECK(reg.find("math") == nullptr);
    CHECK(reg.find("") == nullptr);
}

// ============================================================================
// Test 9 — registry size / empty / for_each
// ============================================================================

TEST_CASE("ffi_module: registry size empty for_each", "[crank][ffi_module][registry]") {
    crank::ffi_module_registry reg;
    CHECK(reg.empty());
    CHECK(reg.size() == 0u);

    reg.register_module(crank::ffi_module_builder{"a"}.fn("a.f", 1).build());
    reg.register_module(crank::ffi_module_builder{"b"}.fn("b.g", 0).build());

    CHECK_FALSE(reg.empty());
    CHECK(reg.size() == 2u);

    std::size_t count = 0;
    reg.for_each([&](const crank::ffi_module_descriptor&) { ++count; });
    CHECK(count == 2u);
}

// ============================================================================
// Test 10 — symbols_of_kind returns only functions
// ============================================================================

TEST_CASE("ffi_module: symbols_of_kind function only", "[crank][ffi_module][registry]") {
    crank::ffi_module_registry reg;
    reg.register_module(
        crank::ffi_module_builder{"math"}
            .fn("math.dot",  "Dot",  2)
            .fn("math.norm", "Norm", 1)
            .type("math.Vec3", "Vec3")
            .build());

    auto fns = reg.symbols_of_kind(crank::ffi_symbol_kind::function);
    CHECK(fns.size() == 2u);
    for (const auto* s : fns)
        CHECK(s->kind == crank::ffi_symbol_kind::function);

    auto types = reg.symbols_of_kind(crank::ffi_symbol_kind::type);
    CHECK(types.size() == 1u);
}

// ============================================================================
// Test 11 — context::register_ffi_module seeds native tier
// ============================================================================

TEST_CASE("ffi_module: context register seeds resolver native tier", "[crank][ffi_module][context]") {
    crank::context ctx;
    auto mod = crank::ffi_module_builder{"math"}
        .fn("math.dot", "Dot", 2)
        .build();
    ctx.register_ffi_module(mod);

    // After registration, the resolver's native tier should resolve "math".
    auto desc = ctx.modules().resolver().resolve("math");
    REQUIRE(desc.has_value());
    CHECK(desc->name == "math");
    CHECK(desc->kind == crank::module_kind::native);
}

// ============================================================================
// Test 12 — engine::register_ffi_module is fluent
// ============================================================================

TEST_CASE("ffi_module: engine register_ffi_module fluent", "[crank][ffi_module][engine]") {
    crank::engine e;
    auto& ref = e.register_ffi_module(
        crank::ffi_module_builder{"math"}
            .fn("math.dot", "Dot", 2)
            .build());

    // Fluent — must return the engine reference.
    CHECK(&ref == &e);
}

// ============================================================================
// Test 13 — engine::load registered FFI module returns module_handle
// ============================================================================

TEST_CASE("ffi_module: engine load registered FFI module", "[crank][ffi_module][engine][load]") {
    crank::engine e;
    e.context().register_function<"math.dot", fvec_dot>();
    e.register_ffi_module(
        crank::ffi_module_builder{"math"}
            .fn("math.dot", "Dot", 2)
            .build());

    auto result = e.load("math");
    REQUIRE(result.has_value());
    CHECK(result->name() == "math");
    CHECK(result->valid());
}

// ============================================================================
// Test 14 — module_handle::exports() populated from ffi_module symbols
// ============================================================================

TEST_CASE("ffi_module: load populates exports from ffi_module", "[crank][ffi_module][engine][load]") {
    crank::engine e;
    e.register_ffi_module(
        crank::ffi_module_builder{"math"}
            .fn("math.dot",  "Dot",  2)
            .fn("math.norm", "Norm", 1)
            .type("math.Vec3", "Vec3")
            .build());

    auto result = e.load("math");
    REQUIRE(result.has_value());

    auto exps = result->exports();
    CHECK(exps.size() == 3u);

    // Verify at least one function symbol is present.
    bool has_dot = false;
    for (const auto& s : exps) {
        if (s.name == "Dot") {
            has_dot = true;
            CHECK(s.sym_kind == crank::symbol::kind::function);
        }
    }
    CHECK(has_dot);
}

// ============================================================================
// Test 15 — engine::load unregistered module returns CRANK-MOD-001
// ============================================================================

TEST_CASE("ffi_module: engine load unregistered returns CRANK-MOD-001", "[crank][ffi_module][engine][load]") {
    crank::engine e;
    auto result = e.load("no.such.module");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "CRANK-MOD-001");
}

// ============================================================================
// Test 16 — module_graph_view after FFI module load
// ============================================================================

TEST_CASE("ffi_module: module_graph includes loaded FFI module", "[crank][ffi_module][engine][graph]") {
    crank::engine e;
    e.register_ffi_module(
        crank::ffi_module_builder{"math"}
            .fn("math.dot", "Dot", 2)
            .build());

    auto r = e.load("math");
    REQUIRE(r.has_value());

    auto gv = e.module_graph();
    CHECK_FALSE(gv.empty());
    CHECK(gv.find("math") != nullptr);
}

// ============================================================================
// Test 17 — ffi_symbol_kind to_string covers all values
// ============================================================================

TEST_CASE("ffi_module: ffi_symbol_kind to_string", "[crank][ffi_module][to_string]") {
    CHECK(crank::to_string(crank::ffi_symbol_kind::function) == "function");
    CHECK(crank::to_string(crank::ffi_symbol_kind::type)     == "type");
    CHECK(crank::to_string(crank::ffi_symbol_kind::resource) == "resource");
    CHECK(crank::to_string(crank::ffi_symbol_kind::constant) == "constant");
}

// ============================================================================
// Test 18 — registry replace: re-register same name replaces descriptor
// ============================================================================

TEST_CASE("ffi_module: registry replace on re-register", "[crank][ffi_module][registry]") {
    crank::ffi_module_registry reg;

    reg.register_module(crank::ffi_module_builder{"math"}
        .fn("math.dot", "Dot", 2)
        .build());
    CHECK(reg.find("math")->symbols.size() == 1u);

    // Re-register with an extra symbol — should replace.
    reg.register_module(crank::ffi_module_builder{"math"}
        .fn("math.dot",  "Dot",  2)
        .fn("math.norm", "Norm", 1)
        .build());
    CHECK(reg.find("math")->symbols.size() == 2u);
}
