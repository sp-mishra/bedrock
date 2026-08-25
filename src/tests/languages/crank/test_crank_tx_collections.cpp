// =============================================================================
// test_crank_tx_collections.cpp — §v2.13 transactional collections.
//
// Verifies:
//   1.  TxMap get/put/erase/contains + snapshot/restore round-trip.
//   2.  TxSet add/remove/contains + snapshot/restore.
//   3.  TxQueue FIFO enqueue/dequeue/front + snapshot/restore.
//   4.  TxLog append/at + snapshot/restore.
//   5.  medha::resource_traits<> marks all four transactional.
//   6.  Each collection registers via crank::register_transactional.
//   7.  A collection snapshot drives savepoint rollback (integration with
//       tx_savepoint.hpp).
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/tx_collections.hpp"
#include "languages/crank/host.hpp"
#include "languages/crank/transaction.hpp"

#include <string>

using namespace crank;

TEST_CASE (

"TxMap get/put/erase/contains + snapshot restore"
,
"[crank][tx][collections][v2]"
)
 {
    TxMap<std::string, int> m;
    CHECK_FALSE(m.get("a").has_value());
    m.put("a", 1);
    m.put("b", 2);
    CHECK(m.get("a") == 1);
    CHECK(m.contains("b"));
    CHECK(m.size() == 2u);

    auto snap = m.snapshot();
    m.put("a", 99);
    m.erase("b");
    CHECK(m.get("a") == 99);
    CHECK_FALSE(m.contains("b"));

    m.restore(snap);
    CHECK(m.get("a") == 1);
    CHECK(m.contains("b"));
    CHECK(m.size() == 2u);
}

TEST_CASE (

"TxSet add/remove/contains + snapshot restore"
,
"[crank][tx][collections][v2]"
)
 {
    TxSet<int> s;
    CHECK(s.add(1));
    CHECK(s.add(2));
    CHECK_FALSE(s.add(1)); // duplicate
    CHECK(s.contains(1));
    CHECK(s.size() == 2u);

    auto snap = s.snapshot();
    s.remove(1);
    s.add(3);
    CHECK_FALSE(s.contains(1));
    CHECK(s.contains(3));

    s.restore(snap);
    CHECK(s.contains(1));
    CHECK_FALSE(s.contains(3));
}

TEST_CASE (

"TxQueue FIFO + snapshot restore"
,
"[crank][tx][collections][v2]"
)
 {
    TxQueue<int> q;
    CHECK(q.empty());
    q.enqueue(10);
    q.enqueue(20);
    CHECK(q.front() == 10);
    CHECK(q.size() == 2u);

    auto snap = q.snapshot();
    CHECK(q.dequeue() == 10);
    CHECK(q.dequeue() == 20);
    CHECK(q.empty());
    CHECK_FALSE(q.dequeue().has_value());

    q.restore(snap);
    CHECK(q.size() == 2u);
    CHECK(q.front() == 10);
}

TEST_CASE (

"TxLog append/at + snapshot restore"
,
"[crank][tx][collections][v2]"
)
 {
    TxLog<std::string> log;
    log.append("first");
    log.append("second");
    CHECK(log.size() == 2u);
    CHECK(log.at(0) == "first");
    CHECK(log.at(1) == "second");
    CHECK_FALSE(log.at(2).has_value());

    auto snap = log.snapshot();
    log.append("third");
    CHECK(log.size() == 3u);
    log.restore(snap);
    CHECK(log.size() == 2u);
}

TEST_CASE (

"tx collections are marked transactional by resource_traits"
,
"[crank][tx][collections][v2]"
)
 {
    CHECK(medha::resource_traits<TxMap<int, int>>::transactional);
    CHECK(medha::resource_traits<TxSet<int>>::transactional);
    CHECK(medha::resource_traits<TxQueue<int>>::transactional);
    CHECK(medha::resource_traits<TxLog<int>>::transactional);

    CHECK(medha::resource_traits<TxMap<int, int>>::supports_snapshot);
    CHECK(medha::resource_traits<TxMap<int, int>>::supports_rollback);
    CHECK(medha::resource_traits<TxMap<int, int>>::commit_protocol
          == medha::commit_capability::atomic_multi_key_within_resource);
}

TEST_CASE (

"tx collections register via register_transactional"
,
"[crank][tx][collections][v2]"
)
 {
    auto md = crank::register_transactional<TxMap<int, int>>("Accounts");
    CHECK(md.name == "Accounts");
    CHECK(md.is_transactional);
    CHECK(md.supports_snapshot);
    CHECK(md.supports_rollback);

    auto qd = crank::register_transactional<TxQueue<int>>("Jobs");
    CHECK(qd.name == "Jobs");
    CHECK(qd.is_transactional);
}

// =============================================================================
//  7.  Collection snapshot drives savepoint rollback (tx_savepoint.hpp)
// =============================================================================

#include "languages/crank/tx_savepoint.hpp"

TEST_CASE (

"TxMap snapshot drives savepoint rollback via tx_journal"
,
"[crank][tx][collections][savepoint][v2]"
)
 {
    TxMap<std::string, int> m;
    m.put("a", 1);

    crank::tx_journal journal;

    // Capture snapshot and savepoint before inner writes
    auto snap = m.snapshot();
    auto sp   = journal.make_savepoint("inner");

    // Inner writes recorded in journal, applied to map
    m.put("b", 2);
    journal.record_write("Accounts", "b", "2", std::nullopt, false);
    m.put("a", 99);
    journal.record_write("Accounts", "a", "99", crank::crank_value::from(int64_t{1}), true);

    CHECK(m.get("a") == 99);
    CHECK(m.contains("b"));

    // Rollback via journal — undo sink restores map from snapshot
    journal.rollback_to(sp, [&](const crank::journaled_write&) {
        m.restore(snap); // whole-container restore (correct for savepoint semantics)
    });

    // Map must be back to pre-inner state
    CHECK(m.get("a") == 1);
    CHECK_FALSE(m.contains("b"));
    CHECK(journal.write_count() == 0u); // journal truncated to savepoint
    CHECK(journal.read_count()  == 0u); // no reads were recorded
}

TEST_CASE (

"TxCounter read/set/add/compare-and-set"
,
"[crank][tx][collections][v2]"
)
 {
    TxCounter<int> c{10};
    CHECK(c.read() == 10);

    c.add(5);
    CHECK(c.read() == 15);

    c.set(0);
    CHECK(c.read() == 0);

    CHECK(c.compare_and_set(0, 42));
    CHECK(c.read() == 42);

    CHECK_FALSE(c.compare_and_set(0, 99)); // expected mismatch
    CHECK(c.read() == 42);
}

TEST_CASE (

"TxCounter snapshot/restore round-trip"
,
"[crank][tx][collections][v2]"
)
 {
    TxCounter<long> c{100};
    auto snap = c.snapshot();
    c.add(50);
    CHECK(c.read() == 150);
    c.restore(snap);
    CHECK(c.read() == 100);
}

TEST_CASE (

"TxCounter default-constructed value is zero"
,
"[crank][tx][collections][v2]"
)
 {
    TxCounter<int> c;
    CHECK(c.read() == 0);
}

TEST_CASE (

"TxCounter resource_traits is transactional with snapshot/rollback"
,
"[crank][tx][collections][v2]"
)
 {
    CHECK(medha::resource_traits<TxCounter<int>>::transactional);
    CHECK(medha::resource_traits<TxCounter<int>>::supports_snapshot);
    CHECK(medha::resource_traits<TxCounter<int>>::supports_rollback);
    CHECK(medha::resource_traits<TxCounter<int>>::resource_stages_values);
    CHECK(medha::resource_traits<TxCounter<int>>::commit_protocol
          == medha::commit_capability::atomic_multi_key_within_resource);
    CHECK(medha::resource_traits<TxCounter<double>>::transactional);
}

TEST_CASE (

"TxCounter registers via register_transactional"
,
"[crank][tx][collections][v2]"
)
 {
    auto cd = crank::register_transactional<TxCounter<int>>("HitCount");
    CHECK(cd.name == "HitCount");
    CHECK(cd.is_transactional);
    CHECK(cd.supports_snapshot);
}

TEST_CASE (

"TxCounter snapshot drives savepoint rollback via tx_journal"
,
"[crank][tx][collections][savepoint][v2]"
)
 {
    TxCounter<int> ctr{5};
    crank::tx_journal journal;

    auto snap = ctr.snapshot();
    auto sp   = journal.make_savepoint("counter-inner");

    ctr.add(10);
    journal.record_write("HitCount", "0", "15", crank::crank_value::from(int64_t{5}), true);

    CHECK(ctr.read() == 15);

    journal.rollback_to(sp, [&](const crank::journaled_write&) {
        ctr.restore(snap);
    });

    CHECK(ctr.read() == 5);
    CHECK(journal.write_count() == 0u);
}
