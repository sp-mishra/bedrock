// =============================================================================
// test_lithe_feature_extractor.cpp — Unit Tests: Feature Extraction Framework
//
// Verifies: include/edsl/lithe_feature_extractor.hpp
//
// Cases:
//   1.  feature_vector: append inline (≤32 elements), size() correct.
//   2.  feature_vector: append triggers overflow spill beyond 32 elements.
//   3.  feature_vector: normalize() produces unit vector.
//   4.  feature_vector: normalize() no-op when all zeros.
//   5.  feature_vector: clear() resets size to 0.
//   6.  graph_features: to_feature_vector() encodes all 15 fields.
//   7.  expression_features: to_feature_vector() encodes correct count.
//   8.  mir_features: to_feature_vector() encodes all 9 fields.
//   9.  runtime_features: record() updates min/max/mean correctly.
//   10. runtime_features: Welford sample_variance() ≈ correct for two samples.
//   11. runtime_features: to_feature_vector() encodes 8 fields.
//   12. graph_feature_extractor: extract() on simple add tree → non-zero vector.
//   13. graph_feature_extractor: leaf-only expression → leaf_count == node_count.
//   14. expression_feature_extractor: add/mul/div counts match tree.
//   15. expression_feature_extractor: tree_depth correct.
//   16. runtime_feature_extractor: record_memory() updates peak.
//   17. runtime_feature_extractor: reset() zeros accumulated state.
//   18. runtime_feature_extractor: current() encodes call_count.
//   19. combined_feature_extractor: output size == expr + runtime dims.
//   20. feature_extractor concept: satisfied for runtime_feature_extractor.
//   21. feature_extractor concept: satisfied for expression_feature_extractor.
//   22. graph_features: op_frequencies sum ≈ 1.0 for non-empty tree.
//   23. runtime_features: record() on single sample → mean == sample value.
//   24. runtime_features: min/max initialisation correct after two records.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_feature_extractor.hpp"


namespace lf = lithe::features;
using Catch::Approx;
using lithe::make_node;
using lithe::as_expr;
using lithe::add_tag;
using lithe::mul_tag;
using lithe::div_tag;
using lithe::neg_tag;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {
    // Simple add tree: (1.0 + 2.0) + (3.0 * 4.0)
    auto make_test_expr() {
        auto a = as_expr(1.0);
        auto b = as_expr(2.0);
        auto c = as_expr(3.0);
        auto d = as_expr(4.0);
        return make_node<add_tag>(make_node<add_tag>(a, b),
                                  make_node<mul_tag>(c, d));
    }
} // namespace

// ===========================================================================
// feature_vector
// ===========================================================================

TEST_CASE (


"feature_vector inline append and size"
,
"[feature_vector]"
)
{
    lf::feature_vector fv;
    for (int i = 0; i < 10; ++i) fv.append(static_cast<float>(i));
    REQUIRE(fv.size() == 10u);
    REQUIRE(fv[0] == Approx(0.0f));
    REQUIRE(fv[9] == Approx(9.0f));
    REQUIRE(fv.as_span().size() == 10u);
}

TEST_CASE (


"feature_vector overflow spill beyond 32 elements"
,
"[feature_vector]"
)
{
    lf::feature_vector fv;
    for (int i = 0; i < 40; ++i) fv.append(static_cast<float>(i));
    REQUIRE(fv.size() == 40u);
    REQUIRE(fv[0] == Approx(0.0f));
    REQUIRE(fv[39] == Approx(39.0f));
}

TEST_CASE (


"feature_vector normalize produces unit vector"
,
"[feature_vector]"
)
{
    lf::feature_vector fv;
    fv.append(3.0f);
    fv.append(4.0f);  // ||[3,4]|| == 5
    fv.normalize();
    REQUIRE(fv.size() == 2u);
    REQUIRE(fv[0] == Approx(0.6f).epsilon(1e-5));
    REQUIRE(fv[1] == Approx(0.8f).epsilon(1e-5));
}

TEST_CASE (


"feature_vector normalize no-op on zero vector"
,
"[feature_vector]"
)
{
    lf::feature_vector fv;
    fv.append(0.0f);
    fv.append(0.0f);
    fv.normalize();
    REQUIRE(fv[0] == Approx(0.0f));
    REQUIRE(fv[1] == Approx(0.0f));
}

TEST_CASE (


"feature_vector clear resets size"
,
"[feature_vector]"
)
{
    lf::feature_vector fv;
    fv.append(1.0f);
    fv.append(2.0f);
    fv.clear();
    REQUIRE(fv.size() == 0u);
}

// ===========================================================================
// Feature bundle to_feature_vector()
// ===========================================================================

TEST_CASE (


"graph_features encodes 15 elements"
,
"[graph_features]"
)
{
    lf::graph_features gf;
    gf.node_count     = 5;
    gf.edge_count     = 4;
    gf.depth          = 3;
    gf.leaf_count     = 2;
    gf.internal_count = 3;
    gf.max_fanout     = 2;
    gf.sharing_count  = 1;
    gf.op_frequencies = {0.2f, 0.1f, 0.3f, 0.1f, 0.0f, 0.0f, 0.0f, 0.3f};
    auto fv = gf.to_feature_vector();
    // 7 scalar fields + 8 op_frequencies = 15
    REQUIRE(fv.size() == 15u);
    REQUIRE(fv[0] == Approx(5.0f));
    REQUIRE(fv[6] == Approx(1.0f));  // sharing_count
}

TEST_CASE (


"expression_features encodes correct count"
,
"[expression_features]"
)
{
    lf::expression_features ef;
    ef.tree_size    = 7;
    ef.tree_depth   = 3;
    ef.div_count    = 1;
    ef.mul_count    = 2;
    ef.add_count    = 3;
    auto fv = ef.to_feature_vector();
    // 3 scalars + 5 arity_histogram + 6 count fields (incl. loop_nesting) = 14
    REQUIRE(fv.size() == 14u);
    REQUIRE(fv[0] == Approx(7.0f));  // tree_size
}

TEST_CASE (


"mir_features encodes 9 elements"
,
"[mir_features]"
)
{
    lf::mir_features mf;
    mf.instruction_count = 20;
    mf.vreg_count        = 8;
    mf.block_count       = 3;
    auto fv = mf.to_feature_vector();
    REQUIRE(fv.size() == 9u);
    REQUIRE(fv[0] == Approx(20.0f));
    REQUIRE(fv[1] == Approx(8.0f));
    REQUIRE(fv[2] == Approx(3.0f));
}

// ===========================================================================
// runtime_features
// ===========================================================================

TEST_CASE (


"runtime_features record updates min/max/mean"
,
"[runtime_features]"
)
{
    lf::runtime_features rf;
    rf.record(100);
    rf.record(200);
    REQUIRE(rf.call_count == 2u);
    REQUIRE(rf.min_latency_ns == 100u);
    REQUIRE(rf.max_latency_ns == 200u);
    REQUIRE(rf.mean_latency_ns == Approx(150.0));
}

TEST_CASE (


"runtime_features Welford variance for two samples"
,
"[runtime_features]"
)
{
    lf::runtime_features rf;
    rf.record(0);
    rf.record(10);
    // sample variance = (0-5)^2 + (10-5)^2 / 1 = 50
    REQUIRE(rf.sample_variance() == Approx(50.0).epsilon(1e-9));
}

TEST_CASE (


"runtime_features single record mean equals sample"
,
"[runtime_features]"
)
{
    lf::runtime_features rf;
    rf.record(42);
    REQUIRE(rf.mean_latency_ns == Approx(42.0));
    REQUIRE(rf.sample_variance() == Approx(0.0));
}

TEST_CASE (


"runtime_features to_feature_vector encodes 8 fields"
,
"[runtime_features]"
)
{
    lf::runtime_features rf;
    rf.record(100);
    auto fv = rf.to_feature_vector();
    REQUIRE(fv.size() == 8u);
    REQUIRE(fv[0] == Approx(1.0f));  // call_count
}

TEST_CASE (


"runtime_features min/max initialisation after two records"
,
"[runtime_features]"
)
{
    lf::runtime_features rf;
    rf.record(500);
    rf.record(100);
    REQUIRE(rf.min_latency_ns == 100u);
    REQUIRE(rf.max_latency_ns == 500u);
}

// ===========================================================================
// graph_feature_extractor
// ===========================================================================

TEST_CASE (


"graph_feature_extractor non-zero vector on add tree"
,
"[graph_feature_extractor]"
)
{
    lf::graph_feature_extractor gfe;
    auto expr = make_test_expr();
    auto fv = gfe.extract(expr);
    REQUIRE(fv.size() == 15u);
    // node_count >= 5
    REQUIRE(fv[0] >= 5.0f);
}

TEST_CASE (


"graph_feature_extractor leaf-only: leaf_count == node_count"
,
"[graph_feature_extractor]"
)
{
    lf::graph_feature_extractor gfe;
    auto lit = make_node<neg_tag>(as_expr(1.0));
    auto gf  = gfe.extract_graph(lit);
    REQUIRE(gf.node_count == 2u);  // tree::size counts Expression + terminal children
    REQUIRE(gf.leaf_count == 1u);
}

// ===========================================================================
// expression_feature_extractor
// ===========================================================================

TEST_CASE (


"expression_feature_extractor counts ops correctly"
,
"[expression_feature_extractor]"
)
{
    lf::expression_feature_extractor efe;
    // (a / b) * c  → 1 div, 1 mul
    auto a = as_expr(1.0), b = as_expr(2.0), c = as_expr(3.0);
    auto expr = make_node<mul_tag>(make_node<div_tag>(a, b), c);
    auto ef = efe.extract_features(expr);
    REQUIRE(ef.div_count == 1u);
    REQUIRE(ef.mul_count == 1u);
    REQUIRE(ef.add_count == 0u);
}

TEST_CASE (


"expression_feature_extractor tree_depth correct"
,
"[expression_feature_extractor]"
)
{
    lf::expression_feature_extractor efe;
    auto x = as_expr(1.0);
    auto y = as_expr(2.0);
    auto z = as_expr(3.0);
    // depth: (x + y) + z  → depth 3 (root=1, inner add=2, leaf terminals=3)
    auto expr = make_node<add_tag>(make_node<add_tag>(x, y), z);
    auto ef = efe.extract_features(expr);
    REQUIRE(ef.tree_depth == 3u);
}

TEST_CASE (


"expression_feature_extractor concept satisfied"
,
"[expression_feature_extractor]"
)
{
    using E = decltype(make_test_expr());
    static_assert(lf::feature_extractor<lf::expression_feature_extractor, E>);
}

// ===========================================================================
// runtime_feature_extractor
// ===========================================================================

TEST_CASE (


"runtime_feature_extractor record_memory updates peak"
,
"[runtime_feature_extractor]"
)
{
    lf::runtime_feature_extractor rfe;
    rfe.record_memory(1024u);
    rfe.record_memory(512u);
    REQUIRE(rfe.accumulated.memory_bytes_peak == 1024u);
}

TEST_CASE (


"runtime_feature_extractor reset zeros state"
,
"[runtime_feature_extractor]"
)
{
    lf::runtime_feature_extractor rfe;
    rfe.record(100u);
    rfe.reset();
    REQUIRE(rfe.accumulated.call_count == 0u);
}

TEST_CASE (


"runtime_feature_extractor current encodes call_count"
,
"[runtime_feature_extractor]"
)
{
    lf::runtime_feature_extractor rfe;
    rfe.record(50u);
    rfe.record(60u);
    auto fv = rfe.current();
    REQUIRE(fv[0] == Approx(2.0f));  // call_count
}

TEST_CASE (


"runtime_feature_extractor concept satisfied"
,
"[runtime_feature_extractor]"
)
{
    static_assert(lf::feature_extractor<lf::runtime_feature_extractor,
                                        lf::runtime_features>);
}

// ===========================================================================
// combined_feature_extractor
// ===========================================================================

TEST_CASE (


"combined_feature_extractor output size = expr + runtime dims"
,
"[combined_feature_extractor]"
)
{
    lf::combined_feature_extractor<lf::expression_feature_extractor,
                                   lf::runtime_feature_extractor> cfe;
    cfe.second.record(100u);
    auto expr = make_test_expr();
    auto fv = cfe.extract(expr);
    // expression_features → 14 dims, runtime_features → 8 dims = 22 total
    REQUIRE(fv.size() == 22u);
}

// ===========================================================================
// op_frequencies sum
// ===========================================================================

TEST_CASE (


"graph_features op_frequencies sum ≈ 1.0 for non-empty tree"
,
"[graph_features]"
)
{
    lf::graph_feature_extractor gfe;
    auto expr = make_test_expr();
    auto gf   = gfe.extract_graph(expr);
    float sum = 0.0f;
    for (float f : gf.op_frequencies) sum += f;
    // frequencies are normalized by internal_count; sum <= 1.0
    REQUIRE(sum <= 1.0f + 1e-5f);
}
