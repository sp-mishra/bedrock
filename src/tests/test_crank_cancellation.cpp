// =============================================================================
// test_crank_cancellation.cpp — hierarchical cancellation + FSM (design §13).
//
// Covers:
//   1. Parent cancellation propagates to a child token.
//   2. A child cancelling does NOT cancel its parent.
//   3. effective_deadline picks the tighter (min) of two deadlines.
//   4. check_interruption reports cancellation, then deadline.
//   5. task_status transition table: legal + illegal edges; terminal absorbing.
//   6. task_state CAS advances only on valid transitions.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/cancellation.hpp"

using namespace crank;

TEST_CASE (

"parent cancellation propagates to child"
,
"[crank][cancellation]"
)
 {
    cancellation_token parent;
    auto child = parent.child();
    REQUIRE_FALSE(child.is_cancelled());
    parent.request(cancellation_reason::requested);
    REQUIRE(child.is_cancelled());
    REQUIRE(child.reason() == cancellation_reason::parent_cancelled);
}

TEST_CASE (

"child cancellation does not affect parent"
,
"[crank][cancellation]"
)
 {
    cancellation_token parent;
    auto child = parent.child();
    child.request(cancellation_reason::requested);
    REQUIRE(child.is_cancelled());
    REQUIRE_FALSE(parent.is_cancelled());
}

TEST_CASE (

"effective_deadline takes the tighter bound"
,
"[crank][cancellation]"
)
 {
    auto now = deadline_clock::now();
    auto soon = now + std::chrono::milliseconds(10);
    auto later = now + std::chrono::milliseconds(100);

    REQUIRE(*effective_deadline(soon, later) == soon);
    REQUIRE(*effective_deadline(later, soon) == soon);
    REQUIRE(*effective_deadline(std::nullopt, soon) == soon);
    REQUIRE(*effective_deadline(soon, std::nullopt) == soon);
    REQUIRE_FALSE(effective_deadline(std::nullopt, std::nullopt).has_value());
}

TEST_CASE (

"check_interruption reports cancellation then deadline"
,
"[crank][cancellation]"
)
 {
    cancellation_token tok;
    // No cancellation, no deadline → keep running.
    REQUIRE_FALSE(check_interruption(tok).has_value());

    // Expired deadline → timed_out.
    auto past = deadline_clock::now() - std::chrono::seconds(1);
    auto i = check_interruption(tok, past);
    REQUIRE(i.has_value());
    REQUIRE(i->kind == interruption::kind_t::timed_out);

    // Cancellation wins even with a live deadline.
    tok.request();
    auto future = deadline_clock::now() + std::chrono::seconds(10);
    auto j = check_interruption(tok, future);
    REQUIRE(j.has_value());
    REQUIRE(j->kind == interruption::kind_t::cancelled);
}

TEST_CASE (

"task_status transition table"
,
"[crank][cancellation]"
)
 {
    REQUIRE(valid_transition(task_status::created, task_status::scheduled));
    REQUIRE(valid_transition(task_status::scheduled, task_status::running));
    REQUIRE(valid_transition(task_status::running, task_status::completed));
    // Cancellation may strike any live state.
    REQUIRE(valid_transition(task_status::running, task_status::cancelled));
    REQUIRE(valid_transition(task_status::created, task_status::timed_out));
    // Terminal states are absorbing.
    REQUIRE_FALSE(valid_transition(task_status::completed, task_status::running));
    REQUIRE_FALSE(valid_transition(task_status::cancelled, task_status::completed));
    // Illegal skip.
    REQUIRE_FALSE(valid_transition(task_status::scheduled, task_status::completed));
}

TEST_CASE (

"task_state advances only on valid transitions"
,
"[crank][cancellation]"
)
 {
    task_state st;
    REQUIRE(st.get() == task_status::created);
    REQUIRE(st.transition(task_status::running));
    REQUIRE(st.get() == task_status::running);
    REQUIRE_FALSE(st.transition(task_status::scheduled));  // running→scheduled illegal
    REQUIRE(st.transition(task_status::completed));
    REQUIRE_FALSE(st.transition(task_status::failed));     // terminal is absorbing
}
