// =============================================================================
// test_crank_extern.cpp — extern fn AST node + verification unit tests.
//
// Verifies: include/languages/crank/engine.hpp (verify_extern_fn_decl)
//           include/languages/crank/build_ast.hpp (extern_fn_node)
//           include/languages/crank/context.hpp (analyse extern integration)
//           include/languages/crank/resolve.hpp (extern_unknown_host / arity_mismatch kinds)
//
//  1.  extern_fn_node default construction: name and host_link empty.
//  2.  extern_fn_node populated fields: name, host_link, param_names.
//  3.  extern_fn_tag stable_id = 1018.
//  4.  extern_fn_tag symbol = "extern_fn".
//  5.  verify_extern_fn_decl — success path: all fields populated.
//  6.  verify_extern_fn_decl — CRANK-EXT-010 unknown host symbol.
//  7.  verify_extern_fn_decl — CRANK-EXT-011 arity mismatch.
//  8.  extern_fn_decl descriptor and thunk non-null on success.
//  9.  extern_fn_decl fingerprint non-zero on success.
// 10.  resolve_diagnostic::kind::extern_unknown_host code = "CRANK-EXT-010".
// 11.  resolve_diagnostic::kind::extern_arity_mismatch code = "CRANK-EXT-011".
// 12.  resolve_diagnostic::is_error() true for extern kinds.
// 13.  context::analyse() with empty typed_ast — no extern diagnostics emitted.
// 14.  context::analyse(bool) — unaffected by extern walk (no typed_ast).
// 15.  crank_ast_node variant holds extern_fn_node alternative.
// 16.  verify_extern_fn_decl with multiple registered functions — finds correct one.
// 17.  extern_fn_error_kind to_string codes correct.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/engine.hpp"
#include "languages/crank/build_ast.hpp"

#include <string>
#include <string_view>
#include <variant>

// ============================================================================
// Test domain: Vec3 + dot + norm functions
// ============================================================================

struct EVec3 { float x, y, z; };

static float e_dot(EVec3 a, EVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static float e_norm(EVec3 v) noexcept {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

template <>
struct crank::type_descriptor<EVec3> {
    static constexpr std::string_view name = "EVec3";
    static constexpr auto fields = std::tuple{
        crank::field<"x", &EVec3::x>{},
        crank::field<"y", &EVec3::y>{},
        crank::field<"z", &EVec3::z>{}
    };
};

// ============================================================================
// Test 1 — extern_fn_node default construction
// ============================================================================

TEST_CASE("extern_fn: node default construction", "[crank][extern_fn][ast]") {
    crank::extern_fn_node n;
    CHECK(n.name.empty());
    CHECK(n.host_link.empty());
    CHECK(n.param_names.empty());
    CHECK(n.param_type_hints.empty());
    CHECK(n.return_type_hint.empty());
    CHECK(n.children.empty());
}

// ============================================================================
// Test 2 — extern_fn_node populated fields
// ============================================================================

TEST_CASE("extern_fn: node populated fields", "[crank][extern_fn][ast]") {
    crank::extern_fn_node n;
    n.name       = "Dot";
    n.host_link  = "math.dot";
    n.param_names       = {"a", "b"};
    n.param_type_hints  = {"Vec3", "Vec3"};
    n.return_type_hint  = "Float32";

    CHECK(n.name            == "Dot");
    CHECK(n.host_link       == "math.dot");
    CHECK(n.param_names.size()      == 2u);
    CHECK(n.param_type_hints.size() == 2u);
    CHECK(n.return_type_hint        == "Float32");
}

// ============================================================================
// Test 3 — extern_fn_tag stable_id = 1018
// ============================================================================

TEST_CASE("extern_fn: tag stable_id is 1018", "[crank][extern_fn][tag]") {
    constexpr auto id = vakya::emit::tag_descriptor<crank::extern_fn_tag>::stable_id;
    CHECK(id == 1018u);
}

// ============================================================================
// Test 4 — extern_fn_tag symbol = "extern_fn"
// ============================================================================

TEST_CASE("extern_fn: tag symbol is extern_fn", "[crank][extern_fn][tag]") {
    constexpr auto sym = vakya::emit::tag_descriptor<crank::extern_fn_tag>::symbol;
    CHECK(sym == "extern_fn");
}

// ============================================================================
// Test 5 — verify_extern_fn_decl success path
// ============================================================================

TEST_CASE("extern_fn: verify_extern_fn_decl success", "[crank][extern_fn][verify]") {
    crank::engine e;
    e.context().register_function<"math.dot", e_dot>();

    auto result = crank::verify_extern_fn_decl(e.context(), "Dot", "math.dot", 2);
    REQUIRE(result.has_value());
    CHECK(result->name      == "Dot");
    CHECK(result->host_link == "math.dot");
    CHECK(result->arity     == 2u);
}

// ============================================================================
// Test 6 — verify_extern_fn_decl CRANK-EXT-010 unknown symbol
// ============================================================================

TEST_CASE("extern_fn: verify_extern_fn_decl unknown symbol CRANK-EXT-010", "[crank][extern_fn][verify]") {
    crank::engine e;
    // No function registered.
    auto result = crank::verify_extern_fn_decl(e.context(), "Dot", "math.dot", 2);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code  == "CRANK-EXT-010");
    CHECK(result.error().stage == crank::error_stage::extern_fn);
    CHECK(result.error().message.find("math.dot") != std::string::npos);
}

// ============================================================================
// Test 7 — verify_extern_fn_decl CRANK-EXT-011 arity mismatch
// ============================================================================

TEST_CASE("extern_fn: verify_extern_fn_decl arity mismatch CRANK-EXT-011", "[crank][extern_fn][verify]") {
    crank::engine e;
    e.context().register_function<"math.dot", e_dot>(); // arity = 2

    auto result = crank::verify_extern_fn_decl(e.context(), "Dot", "math.dot", 3);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code  == "CRANK-EXT-011");
    CHECK(result.error().stage == crank::error_stage::extern_fn);
    CHECK(result.error().message.find("Dot") != std::string::npos);
}

// ============================================================================
// Test 8 — descriptor and thunk non-null on success
// ============================================================================

TEST_CASE("extern_fn: verify success descriptor and thunk non-null", "[crank][extern_fn][verify]") {
    crank::engine e;
    e.context().register_function<"math.dot", e_dot>();

    auto result = crank::verify_extern_fn_decl(e.context(), "Dot", "math.dot", 2);
    REQUIRE(result.has_value());
    CHECK(result->descriptor != nullptr);
    CHECK(result->thunk      != nullptr);
}

// ============================================================================
// Test 9 — fingerprint non-zero on success
// ============================================================================

TEST_CASE("extern_fn: verify success fingerprint non-zero", "[crank][extern_fn][verify]") {
    crank::engine e;
    e.context().register_function<"math.dot", e_dot>();

    auto result = crank::verify_extern_fn_decl(e.context(), "Dot", "math.dot", 2);
    REQUIRE(result.has_value());
    CHECK(result->fingerprint != 0u);
}

// ============================================================================
// Test 10 — resolve_diagnostic extern_unknown_host code
// ============================================================================

TEST_CASE("extern_fn: resolve_diagnostic extern_unknown_host code", "[crank][extern_fn][diagnostic]") {
    using kind = crank::resolve_diagnostic::kind;
    CHECK(crank::resolve_diagnostic::to_code(kind::extern_unknown_host) == "CRANK-EXT-010");
}

// ============================================================================
// Test 11 — resolve_diagnostic extern_arity_mismatch code
// ============================================================================

TEST_CASE("extern_fn: resolve_diagnostic extern_arity_mismatch code", "[crank][extern_fn][diagnostic]") {
    using kind = crank::resolve_diagnostic::kind;
    CHECK(crank::resolve_diagnostic::to_code(kind::extern_arity_mismatch) == "CRANK-EXT-011");
}

// ============================================================================
// Test 12 — resolve_diagnostic is_error true for extern kinds
// ============================================================================

TEST_CASE("extern_fn: resolve_diagnostic extern kinds are errors", "[crank][extern_fn][diagnostic]") {
    crank::resolve_diagnostic d1;
    d1.k = crank::resolve_diagnostic::kind::extern_unknown_host;
    CHECK(d1.is_error());

    crank::resolve_diagnostic d2;
    d2.k = crank::resolve_diagnostic::kind::extern_arity_mismatch;
    CHECK(d2.is_error());
}

// ============================================================================
// Test 13 — context::analyse() with null typed_ast — no extern diagnostics
// ============================================================================

TEST_CASE("extern_fn: analyse null typed_ast no extern diagnostics", "[crank][extern_fn][context]") {
    crank::context ctx;
    // analyse(bool) path — no parse_result / typed_ast.
    auto ar = ctx.analyse(true, "test");
    // No extern_fn_node walk — diagnostics vector must be empty.
    bool any_extern_diag = false;
    for (const auto& d : ar.resolve.diagnostics) {
        if (d.k == crank::resolve_diagnostic::kind::extern_unknown_host ||
            d.k == crank::resolve_diagnostic::kind::extern_arity_mismatch) {
            any_extern_diag = true;
        }
    }
    CHECK_FALSE(any_extern_diag);
}

// ============================================================================
// Test 14 — context::analyse(bool) unaffected (no typed_ast walk)
// ============================================================================

TEST_CASE("extern_fn: analyse(bool) never emits extern diagnostics", "[crank][extern_fn][context]") {
    crank::context ctx;
    ctx.register_function<"math.dot", e_dot>();
    // analyse(bool) — no extern_fn_node available to check.
    auto ar = ctx.analyse(true, "test");
    for (const auto& d : ar.resolve.diagnostics) {
        // Should not contain EXT codes — there was no AST to walk.
        CHECK(d.k != crank::resolve_diagnostic::kind::extern_unknown_host);
        CHECK(d.k != crank::resolve_diagnostic::kind::extern_arity_mismatch);
    }
}

// ============================================================================
// Test 15 — crank_ast_node variant holds extern_fn_node alternative
// ============================================================================

TEST_CASE("extern_fn: crank_ast_node variant contains extern_fn_node", "[crank][extern_fn][ast]") {
    crank::extern_fn_node efn;
    efn.name      = "Dot";
    efn.host_link = "math.dot";

    crank::crank_ast_node node{efn};
    CHECK(std::holds_alternative<crank::extern_fn_node>(node));

    const auto* p = std::get_if<crank::extern_fn_node>(&node);
    REQUIRE(p != nullptr);
    CHECK(p->name      == "Dot");
    CHECK(p->host_link == "math.dot");
}

// ============================================================================
// Test 16 — verify_extern_fn_decl finds correct function among multiple
// ============================================================================

TEST_CASE("extern_fn: verify finds correct function among multiple", "[crank][extern_fn][verify]") {
    crank::engine e;
    e.context().register_function<"math.dot",  e_dot>();
    e.context().register_function<"math.norm", e_norm>();

    // Verify dot (arity 2)
    auto r_dot = crank::verify_extern_fn_decl(e.context(), "Dot",  "math.dot",  2);
    REQUIRE(r_dot.has_value());
    CHECK(r_dot->host_link == "math.dot");
    CHECK(r_dot->arity     == 2u);

    // Verify norm (arity 1)
    auto r_norm = crank::verify_extern_fn_decl(e.context(), "Norm", "math.norm", 1);
    REQUIRE(r_norm.has_value());
    CHECK(r_norm->host_link == "math.norm");
    CHECK(r_norm->arity     == 1u);
}

// ============================================================================
// Test 17 — extern_fn_error_kind codes correct
// ============================================================================

TEST_CASE("extern_fn: extern_fn_diag_code covers all kinds", "[crank][extern_fn][diagnostic]") {
    CHECK(crank::extern_fn_diag_code(crank::extern_fn_error_kind::unknown_host_symbol) == "CRANK-EXT-010");
    CHECK(crank::extern_fn_diag_code(crank::extern_fn_error_kind::signature_mismatch)  == "CRANK-EXT-011");
    CHECK(crank::extern_fn_diag_code(crank::extern_fn_error_kind::effect_escalation)   == "CRANK-EXT-012");
}
