// =============================================================================
// test_crank_tx_savepoint.cpp — §v2.12 nested transactions + savepoints.
//
// Verifies the write-set journal + savepoint/rollback model and the RAII
// nested_transaction scope:
//   1.  make_savepoint captures the current write-set position.
//   2.  rollback_to undoes writes since the savepoint (LIFO) and retains reads.
//   3.  rollback_to on a stale/out-of-range savepoint is a no-op.
//   4.  release() reports how many writes fold into the parent.
//   5.  nested_transaction::commit folds writes into the parent.
//   6.  nested_transaction::rollback undoes the nested writes via the undo sink.
//   7.  A nested_transaction that escapes its scope auto-rolls-back.
//   8.  execute_transaction_journaled runs and accounts applied writes.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/tx_savepoint.hpp"
#include "languages/crank/execute_tx.hpp"

#include <vector>

using namespace crank;

namespace {
    crank_value iv(std::int64_t x) { return crank_value::from(x); }
}

TEST_CASE (

"savepoint captures write-set position"
,
"[crank][tx][savepoint][v2]"
)
 {
    tx_journal j;
    j.record_write("R", "k0", "0", std::nullopt, false);
    auto sp = j.make_savepoint("sp");
    CHECK(sp.marker == 1u);
    j.record_write("R", "k1", "1", std::nullopt, false);
    CHECK(j.write_count() == 2u);
    CHECK(j.release(sp) == 1u); // one write since the savepoint
}

TEST_CASE (

"rollback_to undoes writes since savepoint, retains reads"
,
"[crank][tx][savepoint][v2]"
)
 {
    tx_journal j;
    j.record_read("R", "k0");
    j.record_write("R", "k0", "10", iv(1), true);
    auto sp = j.make_savepoint();
    j.record_write("R", "k1", "20", std::nullopt, false);
    j.record_write("R", "k2", "30", std::nullopt, false);

    std::vector<std::string> undone;
    auto n = j.rollback_to(sp, [&](const journaled_write& w) { undone.push_back(w.key_expr); });

    CHECK(n == 2u);
    // LIFO order: k2 undone before k1.
    REQUIRE(undone.size() == 2u);
    CHECK(undone[0] == "k2");
    CHECK(undone[1] == "k1");
    // Writes truncated back to the savepoint; read-set retained.
    CHECK(j.write_count() == 1u);
    CHECK(j.read_count() == 1u);
}

TEST_CASE (

"rollback_to on stale savepoint is a no-op"
,
"[crank][tx][savepoint][v2]"
)
 {
    tx_journal j;
    j.record_write("R", "k0", "0", std::nullopt, false);
    auto sp = j.make_savepoint();          // marker == 1
    // Roll back everything past marker 0 without using sp, shrinking below sp.
    auto other = savepoint{0, 0, ""};
    j.rollback_to(other, [](const journaled_write&) {});
    CHECK(j.write_count() == 0u);
    // sp.marker (1) now > size (0): must be a no-op.
    std::size_t calls = 0;
    auto n = j.rollback_to(sp, [&](const journaled_write&) { ++calls; });
    CHECK(n == 0u);
    CHECK(calls == 0u);
}

TEST_CASE (

"nested_transaction commit folds writes into parent"
,
"[crank][tx][savepoint][v2]"
)
 {
    tx_journal j;
    j.record_write("R", "outer", "1", std::nullopt, false);
    std::size_t undo_calls = 0;
    {
        auto nested = begin_nested(j, [&](const journaled_write&) { ++undo_calls; }, "inner");
        j.record_write("R", "inner1", "2", std::nullopt, false);
        j.record_write("R", "inner2", "3", std::nullopt, false);
        auto folded = nested.commit();
        CHECK(folded == 2u);
    }
    CHECK(undo_calls == 0u);        // commit ⇒ no undo
    CHECK(j.write_count() == 3u);   // all writes survive
}

TEST_CASE (

"nested_transaction rollback undoes nested writes"
,
"[crank][tx][savepoint][v2]"
)
 {
    tx_journal j;
    j.record_write("R", "outer", "1", std::nullopt, false);
    std::vector<std::string> undone;
    {
        auto nested = begin_nested(j, [&](const journaled_write& w) { undone.push_back(w.key_expr); }, "inner");
        j.record_write("R", "inner1", "2", std::nullopt, false);
        j.record_write("R", "inner2", "3", std::nullopt, false);
        auto n = nested.rollback();
        CHECK(n == 2u);
    }
    REQUIRE(undone.size() == 2u);
    CHECK(undone[0] == "inner2"); // LIFO
    CHECK(undone[1] == "inner1");
    CHECK(j.write_count() == 1u); // only the outer write remains
}

TEST_CASE (

"nested_transaction auto-rolls-back on scope escape"
,
"[crank][tx][savepoint][v2]"
)
 {
    tx_journal j;
    j.record_write("R", "outer", "1", std::nullopt, false);
    std::size_t undo_calls = 0;
    {
        auto nested = begin_nested(j, [&](const journaled_write&) { ++undo_calls; }, "inner");
        j.record_write("R", "inner1", "2", std::nullopt, false);
        // neither commit() nor rollback() called → destructor must roll back
        (void)nested;
    }
    CHECK(undo_calls == 1u);
    CHECK(j.write_count() == 1u);
}

TEST_CASE (

"execute_transaction_journaled accounts applied writes"
,
"[crank][tx][savepoint][v2]"
)
 {
    tx_lowering_result lowered;
    lowered.options = {};
    // one write op so the journal records something on replay
    lowered.writes.push_back(transaction_write_op{"R", "k", "42", {}});

    tx_evaluator eval; // null sinks → always succeed
    auto res = execute_transaction_journaled(
        lowered, {}, eval,
        [](tx_journal& jr) {
            // no savepoint activity; commit as-is
            CHECK(jr.write_count() >= 1u);
            return true;
        });

    CHECK(res.ok());
    CHECK(res.writes_rolled_back == 0u);
    CHECK(res.writes_applied >= 1u);
}

// ============================================================================
// §v2.12 compensation — post-commit best-effort compensating actions.
// ============================================================================

TEST_CASE (

"v2.12 compensation_registry rejects non-idempotent registration"
,
"[crank][tx][compensate][v2]"
)
 {
    compensation_registry reg;
    CHECK(reg.register_compensation({"refund", /*idempotent=*/true, 1}));
    CHECK_FALSE(reg.register_compensation({"send-email", /*idempotent=*/false, 3}));
    CHECK(reg.size() == 1u);
}

TEST_CASE (

"v2.12 compensation_registry run_all reports every registered ran"
,
"[crank][tx][compensate][v2]"
)
 {
    compensation_registry reg;
    CHECK(reg.register_compensation({"a", true, 1}));
    CHECK(reg.register_compensation({"b", true, 1}));

    auto report = reg.run_all([](const compensation&, std::uint32_t) { return true; });
    CHECK(report.ran == 2u);
    CHECK(report.failed == 0u);
    CHECK(report.total() == 2u);
    CHECK(report.all_ran());
}

TEST_CASE (

"v2.12 compensation retries up to retry_limit before failing"
,
"[crank][tx][compensate][v2]"
)
 {
    compensation_registry reg;
    CHECK(reg.register_compensation({"flaky", true, 3}));   // succeeds on attempt 3
    CHECK(reg.register_compensation({"dead", true, 2}));    // never succeeds

    auto report = reg.run_all([](const compensation& c, std::uint32_t attempt) {
        if (c.label == "flaky") return attempt == 3u;
        return false;                                       // "dead" always fails
    });
    CHECK(report.ran == 1u);
    CHECK(report.failed == 1u);
    CHECK_FALSE(report.all_ran());
}

TEST_CASE (

"v2.12 compensation zero retry_limit is clamped to one attempt"
,
"[crank][tx][compensate][v2]"
)
 {
    compensation_registry reg;
    CHECK(reg.register_compensation({"once", true, 0}));    // clamped to 1

    std::uint32_t attempts = 0;
    auto report = reg.run_all([&](const compensation&, std::uint32_t) {
        ++attempts;
        return true;
    });
    CHECK(attempts == 1u);
    CHECK(report.ran == 1u);
}
