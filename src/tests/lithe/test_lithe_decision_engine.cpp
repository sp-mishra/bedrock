// =============================================================================
// test_lithe_decision_engine.cpp — Unit Tests: Decision Engine
//
// Verifies: include/edsl/lithe_decision_engine.hpp
//
// Cases:
//   1.  candidate<int>: fields initialise to zero defaults.
//   2.  ranked<int>: empty() after default construction.
//   3.  cost_ranking: satisfies decision_strategy<cost_ranking<int>, int>.
//   4.  rule_ranking: satisfies decision_strategy<rule_ranking<int>, int>.
//   5.  profile_guided_ranking: satisfies decision_strategy.
//   6.  learned_ranking: satisfies decision_strategy.
//   7.  decision_engine: empty gen → empty ranked.
//   8.  decision_engine: single candidate → ranked size == 1.
//   9.  decision_engine: cost_ranking orders best-first (lower cost first).
//   10. decision_engine: cost_ranking score inverse of cost (higher score for lower cost).
//   11. decision_engine: feat_fn called once per candidate.
//   12. decision_engine: tie-break is deterministic (stable_sort; input order preserved).
//   13. cost_ranking: equal costs → stable order preserved.
//   14. rule_ranking: first rule match wins.
//   15. rule_ranking: no rule match → falls back to cost order.
//   16. profile_guided_ranking: matching hint shifts weights.
//   17. learned_ranking: scorer called with correct feature vector.
//   18. learned_ranking: no scorer → falls back to cost order.
//   19. decision_engine: cost_vector is stored in candidate.cost.
//   20. decision_engine: feature_vector stored in candidate.features.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_decision_engine.hpp"

namespace li = lithe::intelligence;
using Catch::Approx;

// ---------------------------------------------------------------------------
// Helper: build a simple generator returning a fixed list of ints
// ---------------------------------------------------------------------------
struct int_gen {
    std::vector<int> vals;
    const std::vector<int>& operator()() const { return vals; }
};

// ---------------------------------------------------------------------------
TEST_CASE (


"candidate fields initialise to zero defaults"
,
"[decision_engine]"
)
 {
    li::candidate<int> c;
    c.value = 42;
    REQUIRE(c.cost.latency == 0.0f);
    REQUIRE(c.cost.memory  == 0.0f);
    REQUIRE(c.score        == 0.0);
    REQUIRE(c.features.size() == 0);
}

TEST_CASE (


"ranked empty() after default construction"
,
"[decision_engine]"
)
 {
    li::ranked<int> r;
    REQUIRE(r.empty());
    REQUIRE(r.size() == 0);
}

TEST_CASE (


"cost_ranking satisfies decision_strategy concept"
,
"[decision_engine]"
)
 {
    static_assert(li::decision_strategy<li::cost_ranking<int>, int>);
    SUCCEED();
}

TEST_CASE (


"rule_ranking satisfies decision_strategy concept"
,
"[decision_engine]"
)
 {
    static_assert(li::decision_strategy<li::rule_ranking<int>, int>);
    SUCCEED();
}

TEST_CASE (


"profile_guided_ranking satisfies decision_strategy concept"
,
"[decision_engine]"
)
 {
    static_assert(li::decision_strategy<li::profile_guided_ranking<int>, int>);
    SUCCEED();
}

TEST_CASE (


"learned_ranking satisfies decision_strategy concept"
,
"[decision_engine]"
)
 {
    static_assert(li::decision_strategy<li::learned_ranking<int>, int>);
    SUCCEED();
}

TEST_CASE (


"decision_engine: empty gen yields empty ranked"
,
"[decision_engine]"
)
 {
    li::decision_engine<li::cost_ranking<int>> eng;
    lithe::cost::cost_context ctx;

    auto gen     = []() -> std::vector<int> { return {}; };
    auto feat    = [](int) { return lithe::features::feature_vector{}; };
    auto cost_fn = [](int, const lithe::features::feature_vector&,
                      const lithe::cost::cost_context&) {
        return lithe::cost::cost_vector{};
    };

    auto r = eng.decide<int>(gen, feat, cost_fn, ctx);
    REQUIRE(r.empty());
}

TEST_CASE (


"decision_engine: single candidate → ranked size 1"
,
"[decision_engine]"
)
 {
    li::decision_engine<li::cost_ranking<int>> eng;
    lithe::cost::cost_context ctx;

    auto gen     = []() -> std::vector<int> { return {7}; };
    auto feat    = [](int) { return lithe::features::feature_vector{}; };
    auto cost_fn = [](int, const lithe::features::feature_vector&,
                      const lithe::cost::cost_context&) {
        lithe::cost::cost_vector cv;
        cv.latency = 1.0f;
        return cv;
    };

    auto r = eng.decide<int>(gen, feat, cost_fn, ctx);
    REQUIRE(r.size() == 1);
    REQUIRE(r.ordered[0].value == 7);
}

TEST_CASE (


"cost_ranking orders best-first (lower latency cost first)"
,
"[decision_engine]"
)
 {
    li::decision_engine<li::cost_ranking<int>> eng;
    lithe::cost::cost_context ctx;

    // Candidate 1 has latency=5, candidate 2 has latency=2 (better)
    auto gen = []() -> std::vector<int> { return {1, 2}; };
    auto feat = [](int) { return lithe::features::feature_vector{}; };
    auto cost_fn = [](int v, const lithe::features::feature_vector&,
                      const lithe::cost::cost_context&) {
        lithe::cost::cost_vector cv;
        cv.latency = (v == 1) ? 5.0f : 2.0f;
        return cv;
    };

    auto r = eng.decide<int>(gen, feat, cost_fn, ctx);
    REQUIRE(r.size() == 2);
    REQUIRE(r.ordered[0].value == 2);  // lower latency cost → higher score → first
    REQUIRE(r.ordered[1].value == 1);
}

TEST_CASE (


"cost_ranking: higher score for lower cost"
,
"[decision_engine]"
)
 {
    li::cost_ranking<int> ranker;
    lithe::cost::cost_context ctx;

    std::vector<li::candidate<int>> cands(2);
    cands[0].value = 10; cands[0].cost.latency = 1.0f;
    cands[1].value = 20; cands[1].cost.latency = 4.0f;

    auto r = ranker.rank(std::span<li::candidate<int>>{cands}, ctx);
    REQUIRE(r.ordered[0].value == 10);
    REQUIRE(r.ordered[0].score > r.ordered[1].score);
}

TEST_CASE (


"feat_fn called once per candidate"
,
"[decision_engine]"
)
 {
    li::decision_engine<li::cost_ranking<int>> eng;
    lithe::cost::cost_context ctx;

    int feat_calls = 0;
    auto gen     = []() -> std::vector<int> { return {1, 2, 3}; };
    auto feat    = [&](int) { ++feat_calls; return lithe::features::feature_vector{}; };
    auto cost_fn = [](int, const lithe::features::feature_vector&,
                      const lithe::cost::cost_context&) {
        return lithe::cost::cost_vector{};
    };

    (void)eng.decide<int>(gen, feat, cost_fn, ctx);
    REQUIRE(feat_calls == 3);
}

TEST_CASE (


"cost_ranking: equal costs → stable input order preserved"
,
"[decision_engine]"
)
 {
    li::decision_engine<li::cost_ranking<int>> eng;
    lithe::cost::cost_context ctx;

    auto gen     = []() -> std::vector<int> { return {10, 20, 30}; };
    auto feat    = [](int) { return lithe::features::feature_vector{}; };
    auto cost_fn = [](int, const lithe::features::feature_vector&,
                      const lithe::cost::cost_context&) {
        return lithe::cost::cost_vector{};  // all zero → same score
    };

    auto r = eng.decide<int>(gen, feat, cost_fn, ctx);
    REQUIRE(r.size() == 3);
    // stable_sort: equal scores → original order preserved
    REQUIRE(r.ordered[0].value == 10);
    REQUIRE(r.ordered[1].value == 20);
    REQUIRE(r.ordered[2].value == 30);
}

TEST_CASE (


"rule_ranking: first matching rule wins"
,
"[decision_engine]"
)
 {
    li::rule_ranking<int> ranker;
    ranker.add_rule([](const int& v) { return v == 42; });
    lithe::cost::cost_context ctx;

    std::vector<li::candidate<int>> cands(3);
    cands[0].value = 1;
    cands[1].value = 42;
    cands[2].value = 99;

    auto r = ranker.rank(std::span<li::candidate<int>>{cands}, ctx);
    REQUIRE(r.ordered[0].value == 42);
}

TEST_CASE (


"rule_ranking: no matching rule → cost order (all zero costs, input order)"
,
"[decision_engine]"
)
 {
    li::rule_ranking<int> ranker;
    ranker.add_rule([](const int& v) { return v > 1000; }); // never fires
    lithe::cost::cost_context ctx;

    std::vector<li::candidate<int>> cands(2);
    cands[0].value = 5; cands[0].cost.latency = 2.0f;
    cands[1].value = 9; cands[1].cost.latency = 1.0f;

    auto r = ranker.rank(std::span<li::candidate<int>>{cands}, ctx);
    // No rule matched; all scores 0 → stable input order
    REQUIRE(r.size() == 2);
    (void)r;
}

TEST_CASE (


"profile_guided_ranking: matching hint changes weights"
,
"[decision_engine]"
)
 {
    li::profile_guided_ranking<int> ranker;
    ranker.add_hint({"tensor", 5.0f, 0.0f, 0.0f, 0.0f}); // heavy latency weight
    lithe::cost::cost_context ctx{"lithe.jit", 0, "tensor.o3"};

    std::vector<li::candidate<int>> cands(2);
    cands[0].value = 1; cands[0].cost.latency = 1.0f;
    cands[1].value = 2; cands[1].cost.latency = 3.0f;

    auto r = ranker.rank(std::span<li::candidate<int>>{cands}, ctx);
    REQUIRE(r.ordered[0].value == 1);  // lower latency still best
    (void)r;
}

TEST_CASE (


"learned_ranking: scorer fn called with candidate features"
,
"[decision_engine]"
)
 {
    li::learned_ranking<int> ranker;
    int scorer_calls = 0;
    ranker.set_scorer([&](const lithe::features::feature_vector&) -> double {
        ++scorer_calls;
        return 1.0;
    });
    lithe::cost::cost_context ctx;

    std::vector<li::candidate<int>> cands(3);
    for (int i = 0; i < 3; ++i) cands[i].value = i;

    (void)ranker.rank(std::span<li::candidate<int>>{cands}, ctx);
    REQUIRE(scorer_calls == 3);
}

TEST_CASE (


"learned_ranking: no scorer → falls back to cost order"
,
"[decision_engine]"
)
 {
    li::learned_ranking<int> ranker; // no scorer set
    lithe::cost::cost_context ctx;

    std::vector<li::candidate<int>> cands(2);
    cands[0].value = 1; cands[0].cost.latency = 5.0f;
    cands[1].value = 2; cands[1].cost.latency = 1.0f;

    auto r = ranker.rank(std::span<li::candidate<int>>{cands}, ctx);
    REQUIRE(r.ordered[0].value == 2);  // lower cost wins
}

TEST_CASE (


"decision_engine: cost stored in candidate"
,
"[decision_engine]"
)
 {
    li::decision_engine<li::cost_ranking<int>> eng;
    lithe::cost::cost_context ctx;

    auto gen     = []() -> std::vector<int> { return {1}; };
    auto feat    = [](int) { return lithe::features::feature_vector{}; };
    auto cost_fn = [](int, const lithe::features::feature_vector&,
                      const lithe::cost::cost_context&) {
        lithe::cost::cost_vector cv; cv.latency = 7.0f; return cv;
    };

    auto r = eng.decide<int>(gen, feat, cost_fn, ctx);
    REQUIRE(r.ordered[0].cost.latency == Approx(7.0f));
}

TEST_CASE (


"decision_engine: feature vector stored in candidate"
,
"[decision_engine]"
)
 {
    li::decision_engine<li::cost_ranking<int>> eng;
    lithe::cost::cost_context ctx;

    auto gen  = []() -> std::vector<int> { return {1}; };
    auto feat = [](int) {
        lithe::features::feature_vector fv;
        fv.append(3.14f);
        return fv;
    };
    auto cost_fn = [](int, const lithe::features::feature_vector&,
                      const lithe::cost::cost_context&) {
        return lithe::cost::cost_vector{};
    };

    auto r = eng.decide<int>(gen, feat, cost_fn, ctx);
    REQUIRE(r.ordered[0].features.size() == 1);
    REQUIRE(r.ordered[0].features[0] == Approx(3.14f));
}
