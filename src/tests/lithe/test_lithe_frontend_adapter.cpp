#include "catch_amalgamated.hpp"

#include "lithe/lithe_lowering.hpp"

#include <string>
#include <type_traits>

// All imported_node / op / source_span types live in lithe::frontend.
// The lowering helper functions that use a frontend_context take the
// lithe::lowering::frontend_context from the outer namespace.

// ---------------------------------------------------------------------------
// Test 1 — imported_node represents an external AST node
// ---------------------------------------------------------------------------
TEST_CASE (



"imported_node represents external AST node"
,
"[lithe][frontend][imported_node]"
)
 {
    using namespace lithe::frontend;

    imported_node node;
    node.operation = op::add;
    node.attributes["lhs"] = "x";
    node.attributes["rhs"] = "y";

    source_location begin;
    begin.file_id = 1;
    begin.line    = 3;
    begin.column  = 5;
    begin.offset  = 20;

    source_location end;
    end.file_id  = 1;
    end.line     = 3;
    end.column   = 10;
    end.offset   = 25;

    node.span.begin = begin;
    node.span.end   = end;

    REQUIRE(node.operation == op::add);
    REQUIRE(node.attributes.at("lhs") == "x");
    REQUIRE(node.span.valid());
    REQUIRE(node.span.length() == 5);
    REQUIRE(node.children.empty());
}

// ---------------------------------------------------------------------------
// Test 2 — imported_node operation_id maps to an abstract operation name
// ---------------------------------------------------------------------------
TEST_CASE (



"imported_node operation_id maps to abstract operation in lowered AST"
,
"[lithe][frontend][imported_node]"
)
 {
    using namespace lithe::frontend;

    imported_node add_node;
    add_node.operation = op::add;

    imported_node mul_node;
    mul_node.operation = op::mul;

    imported_node lit_node;
    lit_node.operation = op::literal;

    const auto r_add = lower_imported_node(add_node);
    const auto r_mul = lower_imported_node(mul_node);
    const auto r_lit = lower_imported_node(lit_node);

    REQUIRE(r_add.ok());
    REQUIRE(r_add.ast.node_kind == "add");

    REQUIRE(r_mul.ok());
    REQUIRE(r_mul.ast.node_kind == "mul");

    REQUIRE(r_lit.ok());
    REQUIRE(r_lit.ast.node_kind == "literal");
}

// ---------------------------------------------------------------------------
// Test 3 — source_span survives the lowering step
// ---------------------------------------------------------------------------
TEST_CASE (



"source_span survives lowering from imported_node to frontend_ast"
,
"[lithe][frontend][source_span]"
)
 {
    using namespace lithe::frontend;

    imported_node node;
    node.operation = op::variable;

    source_location b;
    b.file_id = 2;
    b.line    = 7;
    b.column  = 3;
    b.offset  = 100;

    source_location e;
    e.file_id = 2;
    e.line    = 7;
    e.column  = 10;
    e.offset  = 107;

    node.span.begin = b;
    node.span.end   = e;

    const auto result = lower_imported_node(node);
    REQUIRE(result.ok());
    REQUIRE(result.ast.span.has_value());

    // The ast span copies: offset=begin.offset, length=end-begin, line, column.
    REQUIRE(result.ast.span->line   == 7u);
    REQUIRE(result.ast.span->column == 3u);
    REQUIRE(result.ast.span->offset == 100u);
    REQUIRE(result.ast.span->length == 7u);
}

// ---------------------------------------------------------------------------
// Test 4 — unknown operation ID produces a diagnostic
// ---------------------------------------------------------------------------
TEST_CASE (



"unknown operation id produces a diagnostic during node lowering"
,
"[lithe][frontend][diagnostic]"
)
 {
    using namespace lithe::frontend;

    imported_node node;
    node.operation = 0xDEAD;  // not a known op::* constant

    lithe::lowering::frontend_context ctx;
    const auto result = lower_imported_node(node, ctx);

    // lowered_node_result::ok() returns false when diagnostics are non-empty.
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.diagnostics.empty());
    REQUIRE(result.diagnostics.front().label.find("unknown operation id") != std::string::npos);

    // The frontend_context also receives a warning-level diagnostic.
    REQUIRE_FALSE(ctx.diagnostics.empty());
    REQUIRE(ctx.diagnostics.front().severity == lithe::lowering::frontend_diagnostic::level::warning);
}

// ---------------------------------------------------------------------------
// Test 5 — no lexy dependency is required to use the imported-node API
// ---------------------------------------------------------------------------
TEST_CASE (



"imported_node API compiles without lexy dependency"
,
"[lithe][frontend][no_lexy]"
)
 {
    // lithe::lowering::frontend_kind::lexy is an enum value for the lexy
    // parser.  The imported_node path does not include any lexy header.
    // If this test compiles and runs, the import layer is lexy-free.

    using namespace lithe::frontend;
    using lithe::lowering::frontend_kind;

    STATIC_REQUIRE(!std::is_same_v<frontend_kind, void>);

    // Confirm the lexy constant exists as a compile-time value only.
    constexpr auto lexy_val = frontend_kind::lexy;
    (void) lexy_val;

    // Build and lower a known node — no lexy involvement.
    imported_node node;
    node.operation = op::ret;
    const auto result = lower_imported_node(node);
    REQUIRE(result.ok());
    REQUIRE(result.ast.node_kind == "ret");
}
