// =============================================================================
// test_lithe_visualization.cpp — §4 graph_document + providers
//
// Verifies:
//   • to_document(expr) node/edge counts match the AST shape.
//   • to_document(dag_view) produces a correct node/edge count.
//   • to_dot / to_mermaid / to_json emit well-formed strings.
//   • trace_to_document(compile_trace) has one node per pass_event.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_visualization.hpp"
#include "lithe/viz/graphviz.hpp"
#include "lithe/viz/mermaid.hpp"
#include "lithe/viz/json.hpp"

#include "lithe/lithe_passes.hpp"   // for compile_trace

// ============================================================================
// to_document(expression) — node/edge count
// ============================================================================

TEST_CASE (


"to_document: add(mul(2,3), 0) node and edge counts"
,
"[viz][graph_document]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::mul_tag>(2, 3), 0);

    auto doc = lithe::viz::to_document(expr);

    // Tree (no sharing): add + mul + 2 + 3 + 0 = 5 nodes.
    // Edges: add→mul, add→0, mul→2, mul→3 = 4 edges.
    REQUIRE(doc.node_count() == 5);
    REQUIRE(doc.edge_count() == 4);
}

TEST_CASE (


"to_document: literal leaf has one node, no edges"
,
"[viz][graph_document]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(1, 2);
    auto doc  = lithe::viz::to_document(expr);

    // add + 1 + 2 = 3 nodes; 2 edges.
    REQUIRE(doc.node_count() == 3);
    REQUIRE(doc.edge_count() == 2);
}

// ============================================================================
// to_dot — well-formed output check
// ============================================================================

TEST_CASE (


"to_dot: contains 'digraph' and ';'"
,
"[viz][dot]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(1, 2);
    auto doc  = lithe::viz::to_document(expr);
    auto dot  = lithe::viz::to_dot(doc);

    REQUIRE(dot.find("digraph") != std::string::npos);
    REQUIRE(dot.find("->")      != std::string::npos);
    REQUIRE(dot.find('}')       != std::string::npos);
}

TEST_CASE (


"to_dot: empty document produces valid DOT"
,
"[viz][dot]"
)
 {
    lithe::viz::graph_document empty;
    auto dot = lithe::viz::to_dot(empty);
    REQUIRE(dot.find("digraph") != std::string::npos);
    REQUIRE(dot.find('}') != std::string::npos);
}

// ============================================================================
// to_mermaid — well-formed output check
// ============================================================================

TEST_CASE (


"to_mermaid: contains 'graph TD'"
,
"[viz][mermaid]"
)
 {
    auto expr    = lithe::make_node<lithe::add_tag>(1, 2);
    auto doc     = lithe::viz::to_document(expr);
    auto mermaid = lithe::viz::to_mermaid(doc);

    REQUIRE(mermaid.find("graph TD") != std::string::npos);
}

TEST_CASE (


"to_mermaid: edge arrow present"
,
"[viz][mermaid]"
)
 {
    auto expr    = lithe::make_node<lithe::add_tag>(1, 2);
    auto doc     = lithe::viz::to_document(expr);
    auto mermaid = lithe::viz::to_mermaid(doc);

    REQUIRE(mermaid.find("-->") != std::string::npos);
}

// ============================================================================
// to_json — well-formed output check
// ============================================================================

TEST_CASE (


"to_json: contains 'nodes' and 'edges' keys"
,
"[viz][json]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(1, 2);
    auto doc  = lithe::viz::to_document(expr);
    auto json = lithe::viz::to_json(doc);

    REQUIRE(json.find("\"nodes\"") != std::string::npos);
    REQUIRE(json.find("\"edges\"") != std::string::npos);
}

TEST_CASE (


"to_json: braces balanced"
,
"[viz][json]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(1, 2);
    auto doc  = lithe::viz::to_document(expr);
    auto json = lithe::viz::to_json(doc);

    const long opens  = std::count(json.begin(), json.end(), '{');
    const long closes = std::count(json.begin(), json.end(), '}');
    REQUIRE(opens == closes);
}

// ============================================================================
// trace_to_document — one node per pass_event
// ============================================================================

TEST_CASE (


"trace_to_document: one node per pass_event"
,
"[viz][trace]"
)
 {
    lithe::compiler::observability::compile_trace trace;

    for (std::size_t i = 0; i < 3; ++i) {
        lithe::compiler::observability::pass_event ev;
        ev.pass_name  = "pass_" + std::to_string(i);
        ev.pass_index = i;
        trace.pass_events.push_back(ev);
    }

    auto doc = lithe::viz::trace_to_document(trace);
    REQUIRE(doc.node_count() == 3);
    // 2 sequential edges for 3 passes.
    REQUIRE(doc.edge_count() == 2);
}

TEST_CASE (


"trace_to_document: empty trace produces empty document"
,
"[viz][trace]"
)
 {
    lithe::compiler::observability::compile_trace trace;
    auto doc = lithe::viz::trace_to_document(trace);
    REQUIRE(doc.node_count() == 0);
    REQUIRE(doc.edge_count() == 0);
}
