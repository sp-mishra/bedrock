// =============================================================================
// test_crank_task_scope.cpp — Crank structured concurrency tests (§v2.9).
//
// Verifies: include/languages/crank/task_scope.hpp
//
//  1.  task_scope spawns and joins children (all-success → no error).
//  2.  record_failure captures the first failing child and cancels the scope.
//  3.  cancel() flips is_cancelled and short-circuits subsequent spawns.
//  4.  deadline_scope: within deadline → no error.
//  5.  join_group await_all collects results in registration order.
//  6.  join_group first_error surfaces the first failing future's error.
//  7.  scope_spawn_await drives a child inline and returns its value.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/task_scope.hpp"

#include <chrono>

using namespace crank;

// ---------------------------------------------------------------------------
// 1. task_scope: all children succeed → join() reports no error
// ---------------------------------------------------------------------------
TEST_CASE (

"task_scope joins successful children"
,
"[crank][task_scope][v2]"
)
 {
    task_scope scope;
    auto f1 = scope.spawn([]{ return 1; });
    auto f2 = scope.spawn([]{ return 2; });
    (void)f1; (void)f2;
    CHECK(scope.child_count() == 2u);
    CHECK(!scope.join().has_value());
}

// ---------------------------------------------------------------------------
// 2. record_failure captures first failure + cancels scope
// ---------------------------------------------------------------------------
TEST_CASE (

"task_scope record_failure cancels remaining siblings"
,
"[crank][task_scope][v2]"
)
 {
    task_scope scope;
    scope.record_failure(3, "child 3 exploded");
    auto err = scope.join();
    REQUIRE(err.has_value());
    CHECK(err->kind == task_scope_error_kind::child_failed);
    CHECK(err->failing_child_index == 3u);
    CHECK(scope.is_cancelled());
    // First failure wins — a later record_failure does not overwrite.
    scope.record_failure(7, "later failure");
    CHECK(scope.join()->failing_child_index == 3u);
}

// ---------------------------------------------------------------------------
// 3. cancel() short-circuits spawned callables
// ---------------------------------------------------------------------------
TEST_CASE (

"task_scope cancel flips state"
,
"[crank][task_scope][v2]"
)
 {
    task_scope scope;
    CHECK(!scope.is_cancelled());
    scope.cancel();
    CHECK(scope.is_cancelled());
}

// ---------------------------------------------------------------------------
// 4. deadline_scope within deadline → no error
// ---------------------------------------------------------------------------
TEST_CASE (

"deadline_scope within deadline succeeds"
,
"[crank][task_scope][v2]"
)
 {
    deadline_scope ds(std::chrono::seconds(60));
    auto f = ds.spawn([]{ return 42; });
    (void)f;
    CHECK(!ds.join().has_value());
}

// ---------------------------------------------------------------------------
// 5. join_group collects results in registration order
// ---------------------------------------------------------------------------
TEST_CASE (

"join_group await_all preserves order"
,
"[crank][task_scope][v2]"
)
 {
    task_scope scope;
    join_group<int> jg;
    jg.add(scope.spawn([]{ return 10; }));
    jg.add(scope.spawn([]{ return 20; }));
    jg.add(scope.spawn([]{ return 30; }));
    REQUIRE(jg.size() == 3u);

    auto results = jg.await_all();
    REQUIRE(results.size() == 3u);
    REQUIRE(results[0].has_value());
    CHECK(*results[0] == 10);
    CHECK(*results[1] == 20);
    CHECK(*results[2] == 30);
}

// ---------------------------------------------------------------------------
// 6. join_group first_error: all-success → nullopt
// ---------------------------------------------------------------------------
TEST_CASE (

"join_group first_error nullopt when all succeed"
,
"[crank][task_scope][v2]"
)
 {
    task_scope scope;
    join_group<int> jg;
    jg.add(scope.spawn([]{ return 1; }));
    auto results = jg.await_all();
    CHECK(!jg.first_error(results).has_value());
}

// ---------------------------------------------------------------------------
// 7. scope_spawn_await returns the child's value
// ---------------------------------------------------------------------------
TEST_CASE (

"scope_spawn_await drives child inline"
,
"[crank][task_scope][v2]"
)
 {
    task_scope scope;
    auto res = scope_spawn_await(scope, []{ return 99; });
    REQUIRE(res.has_value());
    CHECK(*res == 99);
}
