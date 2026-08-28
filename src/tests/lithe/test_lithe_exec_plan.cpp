// =============================================================================
// test_lithe_exec_plan.cpp — Unit tests for lithe_exec/execution_plan.hpp
//
// Cases:
//   1.  execution_plan: default is scalar + unknown legality
//   2.  execution_plan: is_legal() / needs_guard() predicates
//   3.  execution_plan: needs_guard() true when guards non-empty
//   4.  execution_plan: needs_guard() true when legality==unknown
//   5.  execution_cost: dominates
//   6.  execution_cost: weighted_sum
//   7.  to_task_decomposition_plan: rank + bounds correctly filled
//   8.  to_task_decomposition_plan: task_decomposition_plan is trivially copyable
//   9.  to_task_decomposition_plan: chunk=1 default
//  10.  execution_cost: cost_vector reuse (latency axis)
// =============================================================================

#include "catch_amalgamated.hpp"
#include "lithe/lithe_exec/execution_plan.hpp"

using namespace lithe::exec;

TEST_CASE (

"execution_plan: default is scalar + unknown"
,
"[exec][plan]"
)
 {
    execution_plan p;
    CHECK(p.kind     == execution_kind::scalar);
    CHECK(p.legality == analysis_outcome::unknown);
    CHECK(p.region_id == 0);
}

TEST_CASE (

"execution_plan: is_legal predicate"
,
"[exec][plan]"
)
 {
    execution_plan p;
    p.legality = analysis_outcome::proven_legal;
    CHECK(p.is_legal());

    p.legality = analysis_outcome::proven_illegal;
    CHECK_FALSE(p.is_legal());
}

TEST_CASE (

"execution_plan: needs_guard false by default"
,
"[exec][plan]"
)
 {
    execution_plan p;
    p.legality = analysis_outcome::proven_legal;
    CHECK_FALSE(p.needs_guard());
}

TEST_CASE (

"execution_plan: needs_guard true when guards non-empty"
,
"[exec][plan]"
)
 {
    execution_plan p;
    p.legality = analysis_outcome::proven_legal;
    p.guards.push_back({.kind = guard_kind::no_alias});
    CHECK(p.needs_guard());
}

TEST_CASE (

"execution_plan: needs_guard true when legality==unknown"
,
"[exec][plan]"
)
 {
    execution_plan p;
    p.legality = analysis_outcome::unknown;
    CHECK(p.needs_guard());
}

TEST_CASE (

"execution_cost: dominates"
,
"[exec][plan]"
)
 {
    execution_cost cheap, expensive;
    cheap.cv     = {.latency = 1.f, .memory = 1.f, .power = 1.f, .throughput = 1.f};
    expensive.cv = {.latency = 2.f, .memory = 2.f, .power = 2.f, .throughput = 2.f};
    CHECK(cheap.dominates(expensive));
    CHECK_FALSE(expensive.dominates(cheap));
}

TEST_CASE (

"execution_cost: weighted_sum"
,
"[exec][plan]"
)
 {
    execution_cost c;
    c.cv = {.latency = 1.f, .memory = 2.f, .power = 3.f, .throughput = 4.f};
    const float ws = c.weighted_sum(1.f, 1.f, 1.f, 1.f);
    CHECK(ws == Catch::Approx(10.f));
}

TEST_CASE (

"to_task_decomposition_plan: fills rank and bounds"
,
"[exec][plan]"
)
 {
    execution_plan plan;
    plan.kind    = execution_kind::threaded;
    plan.legality = analysis_outcome::proven_legal;

    std::array<lithe::codegen::hl::loop_range,
               lithe::codegen::hl::task_decomposition_plan::max_rank> bounds{};
    bounds[0] = {.start = 0, .end = 1024, .step = 1};

    auto tdp = to_task_decomposition_plan(plan, bounds, 1, 32);
    CHECK(tdp.rank  == 1);
    CHECK(tdp.chunk == 32);
    CHECK(tdp.bounds[0].start == 0);
    CHECK(tdp.bounds[0].end   == 1024);
    CHECK(tdp.bounds[0].step  == 1);
}

TEST_CASE (

"to_task_decomposition_plan: output is trivially copyable"
,
"[exec][plan]"
)
 {
    static_assert(std::is_trivially_copyable_v<lithe::codegen::hl::task_decomposition_plan>);
    SUCCEED();
}

TEST_CASE (

"to_task_decomposition_plan: default chunk=1"
,
"[exec][plan]"
)
 {
    execution_plan plan;
    plan.kind = execution_kind::threaded;
    std::array<lithe::codegen::hl::loop_range,
               lithe::codegen::hl::task_decomposition_plan::max_rank> bounds{};
    auto tdp = to_task_decomposition_plan(plan, bounds, 1);
    CHECK(tdp.chunk == 1);
}

TEST_CASE (

"execution_cost: cost_vector latency axis accessible"
,
"[exec][plan]"
)
 {
    execution_cost c;
    c.cv.latency = 42.f;
    CHECK(c.cv.latency == Catch::Approx(42.f));
}
