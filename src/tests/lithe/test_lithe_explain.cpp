// =============================================================================
// test_lithe_explain.cpp — Unit Tests: Explain Engine (pass + decision level)
//
// Verifies: include/edsl/lithe_explain.hpp
//
// Cases (decision_explanation — new):
//   1.  explain_decision: empty ranked → chosen=="<none>", reason=="no candidates".
//   2.  explain_decision: single candidate → chosen==that label, reason=="only candidate".
//   3.  explain_decision: two candidates → reason contains "% lower estimated latency".
//   4.  explain_decision: candidates vector ordered best-first in output.
//   5.  explain_decision: format(plain) contains chosen label.
//   6.  explain_decision: format(markdown) contains "## Decision Explanation".
//   7.  explain_decision: equal latency → reason == "best score".
//   8.  explain_decision: best latency == 0 and runner-up > 0 → reason contains "best score" or %.
//   9.  explain_decision: string_view T value appears in output label.
//   10. explain_decision: int T value → to_string representation in output.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_explain.hpp"

namespace lie = lithe::explain;
namespace li = lithe::intelligence;

// ---------------------------------------------------------------------------
TEST_CASE (


"explain_decision: empty ranked → <none>"
,
"[explain]"
)
 {
    li::ranked<int> r;
    auto expl = lie::explain_decision(r);
    REQUIRE(expl.chosen == "<none>");
    REQUIRE(expl.reason == "no candidates");
}

TEST_CASE (


"explain_decision: single candidate"
,
"[explain]"
)
 {
    li::ranked<int> r;
    li::candidate<int> c;
    c.value = 5;
    c.cost.latency = 1.0f;
    c.score = 1.0;
    r.ordered.push_back(c);

    auto expl = lie::explain_decision(r);
    REQUIRE(expl.chosen == "5");
    REQUIRE(expl.reason == "only candidate");
    REQUIRE(expl.candidates.size() == 1);
}

TEST_CASE (


"explain_decision: two candidates → reason contains pct lower latency"
,
"[explain]"
)
 {
    li::ranked<int> r;
    li::candidate<int> best, second;
    best.value = 1; best.cost.latency = 1.0f; best.score = 2.0;
    second.value = 2; second.cost.latency = 4.0f; second.score = 1.0;
    r.ordered.push_back(best);
    r.ordered.push_back(second);

    auto expl = lie::explain_decision(r);
    REQUIRE(expl.chosen == "1");
    // 75% lower latency: (4-1)/4 * 100 = 75
    REQUIRE(expl.reason.find("75") != std::string::npos);
    REQUIRE(expl.reason.find("lower estimated latency") != std::string::npos);
}

TEST_CASE (


"explain_decision: candidates vector ordered best-first"
,
"[explain]"
)
 {
    li::ranked<int> r;
    li::candidate<int> a, b, c;
    a.value = 10; a.score = 3.0;
    b.value = 20; b.score = 2.0;
    c.value = 30; c.score = 1.0;
    r.ordered = {a, b, c};

    auto expl = lie::explain_decision(r);
    REQUIRE(expl.candidates[0].label == "10");
    REQUIRE(expl.candidates[1].label == "20");
    REQUIRE(expl.candidates[2].label == "30");
}

TEST_CASE (


"explain_decision: format(plain) contains chosen label"
,
"[explain]"
)
 {
    li::ranked<int> r;
    li::candidate<int> c;
    c.value = 42; c.cost.latency = 0.5f;
    r.ordered.push_back(c);

    auto expl = lie::explain_decision(r);
    auto text = expl.format(false);
    REQUIRE(text.find("42") != std::string::npos);
    REQUIRE(text.find("Chosen") != std::string::npos);
}

TEST_CASE (


"explain_decision: format(markdown) contains header"
,
"[explain]"
)
 {
    li::ranked<int> r;
    li::candidate<int> c;
    c.value = 1;
    r.ordered.push_back(c);

    auto expl = lie::explain_decision(r);
    auto md = expl.format(true);
    REQUIRE(md.find("## Decision Explanation") != std::string::npos);
}

TEST_CASE (


"explain_decision: equal latency → reason == 'best score'"
,
"[explain]"
)
 {
    li::ranked<int> r;
    li::candidate<int> a, b;
    a.value = 1; a.cost.latency = 2.0f;
    b.value = 2; b.cost.latency = 2.0f;
    r.ordered = {a, b};

    auto expl = lie::explain_decision(r);
    REQUIRE(expl.reason == "best score");
}

TEST_CASE (


"explain_decision: string_view T appears in output"
,
"[explain]"
)
 {
    li::ranked<std::string_view> r;
    li::candidate<std::string_view> c;
    c.value = "lithe.jit.asmjit";
    c.cost.latency = 1.0f;
    r.ordered.push_back(c);

    auto expl = lie::explain_decision(r);
    REQUIRE(expl.chosen == "lithe.jit.asmjit");
}

TEST_CASE (


"explain_decision: int T → to_string in output label"
,
"[explain]"
)
 {
    li::ranked<int> r;
    li::candidate<int> c;
    c.value = -7;
    r.ordered.push_back(c);

    auto expl = lie::explain_decision(r);
    REQUIRE(expl.candidates[0].label == "-7");
}
