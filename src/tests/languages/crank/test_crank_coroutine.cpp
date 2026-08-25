// =============================================================================
// test_crank_coroutine.cpp — coroutine execution backend (design §14).
//
// Covers:
//   1. A crank_task<T> runs to a completed result.
//   2. request_cancel before run → cancelled result.
//   3. A bound expired deadline → timed_out result.
//   4. co_await composes tasks and yields the inner result.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/coroutine.hpp"

using namespace crank;

namespace {
    crank_task<int> answer() { co_return 42; }

    crank_task<int> add(int a, int b) { co_return a + b; }

    crank_task<int> compose() {
        auto inner = answer();
        auto r = co_await std::move(inner);
        co_return r.completed() ? r.unwrap() + 1 : -1;
    }
} // namespace

TEST_CASE (

"crank_task runs to a completed result"
,
"[crank][coroutine]"
)
 {
    auto t = answer();
    auto r = t.run();
    REQUIRE(r.completed());
    REQUIRE(r.unwrap() == 42);
}

TEST_CASE (

"crank_task with args"
,
"[crank][coroutine]"
)
 {
    auto t = add(3, 4);
    auto r = t.run();
    REQUIRE(r.completed());
    REQUIRE(r.unwrap() == 7);
}

TEST_CASE (

"request_cancel yields a cancelled result"
,
"[crank][coroutine]"
)
 {
    auto t = answer();
    t.request_cancel();
    auto r = t.run();
    REQUIRE_FALSE(r.completed());
    REQUIRE(r.status == execution_status::cancelled);
}

TEST_CASE (

"an expired deadline yields timed_out"
,
"[crank][coroutine]"
)
 {
    auto t = answer();
    cancellation_token tok;
    t.bind(tok, deadline_clock::now() - std::chrono::seconds(1));
    auto r = t.run();
    REQUIRE_FALSE(r.completed());
    REQUIRE(r.status == execution_status::timed_out);
}

TEST_CASE (

"co_await composes task results"
,
"[crank][coroutine]"
)
 {
    auto t = compose();
    auto r = t.run();
    REQUIRE(r.completed());
    REQUIRE(r.unwrap() == 43);
}
