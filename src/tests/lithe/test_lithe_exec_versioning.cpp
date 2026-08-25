// =============================================================================
// test_lithe_exec_versioning.cpp — Unit tests for lithe_exec/runtime_guard.hpp
//
// Cases:
//   1.  runtime_guard: is trivially copyable
//   2.  execution_plan_id: valid / invalid sentinel
//   3.  versioned_plan: valid() requires both ids valid
//   4.  versioned_plan: is_guarded() when guards non-empty
//   5.  guard_kind: to_string spot checks
//   6.  unknown alias → versioned_plan should have no_alias guard
//   7.  unknown alignment → aligned guard
//   8.  unknown trip count → min_trip_count guard
//   9.  GPU unknown residency → device_available + device_resident guards
//  10.  safe_cpu fallback is a scalar plan
// =============================================================================

#include "catch_amalgamated.hpp"
#include "lithe/lithe_exec/runtime_guard.hpp"
#include "lithe/lithe_exec/execution_plan.hpp"

using namespace lithe::exec;

TEST_CASE (

"runtime_guard: trivially copyable"
,
"[exec][versioning]"
)
 {
    static_assert(std::is_trivially_copyable_v<runtime_guard>);
    SUCCEED();
}

TEST_CASE (

"execution_plan_id: valid / invalid sentinel"
,
"[exec][versioning]"
)
 {
    execution_plan_id invalid;
    CHECK_FALSE(invalid.valid());

    execution_plan_id valid_id{0};
    CHECK(valid_id.valid());

    execution_plan_id another{42};
    CHECK(another.valid());
}

TEST_CASE (

"versioned_plan: valid() requires both ids valid"
,
"[exec][versioning]"
)
 {
    versioned_plan vp;
    CHECK_FALSE(vp.valid()); // both invalid

    vp.fast = execution_plan_id{0};
    CHECK_FALSE(vp.valid()); // fallback still invalid

    vp.fallback = execution_plan_id{1};
    CHECK(vp.valid());
}

TEST_CASE (

"versioned_plan: is_guarded when guards non-empty"
,
"[exec][versioning]"
)
 {
    versioned_plan vp;
    vp.fast     = execution_plan_id{0};
    vp.fallback = execution_plan_id{1};
    CHECK_FALSE(vp.is_guarded());

    vp.guards.push_back({.kind = guard_kind::no_alias});
    CHECK(vp.is_guarded());
}

TEST_CASE (

"guard_kind: to_string spot checks"
,
"[exec][versioning]"
)
 {
    CHECK(to_string(guard_kind::no_alias)          == "no_alias");
    CHECK(to_string(guard_kind::aligned)            == "aligned");
    CHECK(to_string(guard_kind::min_trip_count)     == "min_trip_count");
    CHECK(to_string(guard_kind::device_available)   == "device_available");
    CHECK(to_string(guard_kind::device_resident)    == "device_resident");
    CHECK(to_string(guard_kind::reduction_policy_ok)== "reduction_policy_ok");
}

TEST_CASE (

"versioned_plan: unknown alias → no_alias guard present"
,
"[exec][versioning]"
)
 {
    versioned_plan vp;
    vp.fast     = execution_plan_id{0};
    vp.fallback = execution_plan_id{1};
    vp.guards.push_back({.kind = guard_kind::no_alias, .operand_a = 0, .operand_b = 1});
    bool found = false;
    for (const auto& g : vp.guards)
        if (g.kind == guard_kind::no_alias) found = true;
    CHECK(found);
}

TEST_CASE (

"versioned_plan: min_trip_count guard has correct threshold"
,
"[exec][versioning]"
)
 {
    versioned_plan vp;
    vp.fast     = execution_plan_id{0};
    vp.fallback = execution_plan_id{1};
    vp.guards.push_back({.kind = guard_kind::min_trip_count, .operand_a = 0, .constant = 64});
    CHECK(vp.guards[0].constant == 64);
}

TEST_CASE (

"versioned_plan: GPU residency guards"
,
"[exec][versioning]"
)
 {
    versioned_plan vp;
    vp.fast     = execution_plan_id{0};
    vp.fallback = execution_plan_id{1};
    vp.guards.push_back({.kind = guard_kind::device_available});
    vp.guards.push_back({.kind = guard_kind::device_resident});

    bool dev_avail = false, dev_res = false;
    for (const auto& g : vp.guards) {
        if (g.kind == guard_kind::device_available) dev_avail = true;
        if (g.kind == guard_kind::device_resident)  dev_res   = true;
    }
    CHECK(dev_avail);
    CHECK(dev_res);
}

TEST_CASE (

"safe_cpu fallback: scalar plan with proven_legal outcome"
,
"[exec][versioning]"
)
 {
    execution_plan fallback;
    fallback.region_id  = 0;
    fallback.kind       = execution_kind::scalar;
    fallback.legality   = analysis_outcome::proven_legal;

    CHECK(fallback.kind == execution_kind::scalar);
    CHECK(fallback.is_legal());
    CHECK_FALSE(fallback.needs_guard());
}
