// =============================================================================
// test_crank_exec_result.cpp — unified typed execution result (design §3).
//
// Covers:
//   1. make_completed<T> → completed(), value present, no error.
//   2. make_failed<T> → not completed, error present, correct kind + diag code.
//   3. make_cancelled / make_timed_out → status + kind mapping.
//   4. Legacy alias enumerators equal their canonical values (ok==completed).
//   5. status_for maps error kinds onto terminal statuses.
//   6. void specialization completes with no payload.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/exec_result.hpp"

using namespace crank;

TEST_CASE (

"make_completed carries a value and reports completed"
,
"[crank][exec_result]"
)
 {
    auto r = make_completed<std::int64_t>(42);
    REQUIRE(r.completed());
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r.error.has_value());
    REQUIRE(r.unwrap() == 42);
    REQUIRE(r.status == execution_status::completed);
}

TEST_CASE (

"make_failed carries a typed error, no value"
,
"[crank][exec_result]"
)
 {
    auto r = make_failed<std::int64_t>(
        make_error(execution_error_kind::verification_failed, "bad mir", "fn"));
    REQUIRE_FALSE(r.completed());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error.has_value());
    REQUIRE(r.error->kind == execution_error_kind::verification_failed);
    REQUIRE(r.error->code() == "CRANK-E-EXEC-020");
    REQUIRE(r.error->fn_name == "fn");
}

TEST_CASE (

"cancelled and timed_out map to correct status + kind"
,
"[crank][exec_result]"
)
 {
    auto c = make_cancelled<int>("f");
    REQUIRE(c.status == execution_status::cancelled);
    REQUIRE(c.error->kind == execution_error_kind::cancelled);

    auto t = make_timed_out<int>("f");
    REQUIRE(t.status == execution_status::timed_out);
    REQUIRE(t.error->kind == execution_error_kind::deadline_exceeded);
}

TEST_CASE (

"legacy alias enumerators equal canonical values"
,
"[crank][exec_result]"
)
 {
    REQUIRE(execution_status::ok == execution_status::completed);
    REQUIRE(execution_status::unsupported_control_flow == execution_status::unsupported);
    REQUIRE(execution_status::lowering_failed == execution_status::failed);
    REQUIRE(execution_status::runtime_error == execution_status::failed);
    // to_string reports the canonical label for the shared value.
    REQUIRE(to_string(execution_status::ok) == "completed");
}

TEST_CASE (

"status_for maps error kinds to terminal statuses"
,
"[crank][exec_result]"
)
 {
    REQUIRE(status_for(execution_error_kind::cancelled) == execution_status::cancelled);
    REQUIRE(status_for(execution_error_kind::deadline_exceeded) == execution_status::timed_out);
    REQUIRE(status_for(execution_error_kind::backend_unavailable) == execution_status::backend_unavailable);
    REQUIRE(status_for(execution_error_kind::unsupported_opcode) == execution_status::unsupported);
    REQUIRE(status_for(execution_error_kind::plan_construction_failed) == execution_status::invalid_plan);
    REQUIRE(status_for(execution_error_kind::runtime_guard_rejected) == execution_status::failed);
}

TEST_CASE (

"void specialization completes with no payload"
,
"[crank][exec_result]"
)
 {
    auto r = make_completed_void();
    REQUIRE(r.completed());
    REQUIRE(r.status == execution_status::completed);

    auto e = make_error_result<void>(
        make_error(execution_error_kind::gpu_sync_failure, "sync"));
    REQUIRE_FALSE(e.completed());
    REQUIRE(e.error->kind == execution_error_kind::gpu_sync_failure);
}
