#pragma once

// crank/tx_savepoint.hpp — §v2.12 nested transactions + savepoints.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// v1 flattens nested same-thread transactions into the parent (CRANK-TX-NOTE-001).
// v2 keeps that flattening for isolation/retry inheritance, but adds *partial
// rollback* inside a flattened region: a savepoint captures the current
// write-set marker, and rollback_to(sp) undoes every write recorded since the
// savepoint while retaining the read-set (so validation still sees the full
// read footprint). A nested `transaction` block becomes an implicit savepoint on
// entry: if the inner body fails, its writes are rolled back and the parent
// continues; if it succeeds, its writes are folded into the parent write-set.
//
// This header owns only the journaling data model + the scoped nesting helper.
// The runtime replay (calling the host write sink / undo sink) lives in
// execute_tx.hpp (execute_transaction_journaled). Kept dependency-light so a
// host can use the journal directly without pulling in the Medha runtime.
//
// Design refs: §v2.12; extends the CRANK-TX-NOTE-001 flatten path in
// transaction.hpp. See [[project-crank-module1]] for the v1 tx model.

#include "languages/crank/transaction.hpp"
#include "languages/crank/crank_value.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crank {
    // ============================================================================
    // journaled_write — one recorded write in the write-set, with the value it
    // overwrote so a rollback can restore the prior state. prior==nullopt means the
    // key had no committed value before this write (a fresh insert); undo therefore
    // removes the key rather than restoring a value.
    // ============================================================================

    struct journaled_write {
        std::string resource_name;
        std::string key_expr;
        std::string value_expr; // the value written (staged)
        std::optional<crank_value> prior; // value before this write (for undo)
        bool was_present = false; // key existed before this write
    };

    // ============================================================================
    // savepoint — an opaque marker into the write-set journal. Created by
    // tx_journal::savepoint(); consumed by tx_journal::rollback_to(). A savepoint is
    // only valid against the journal that produced it and only while no earlier
    // rollback has truncated past it.
    // ============================================================================

    struct savepoint {
        std::size_t marker = 0; // index into tx_journal::writes_ at capture time
        std::uint32_t id = 0; // monotonic id for diagnostics / equality
        std::string label; // optional human label (nested-tx name, etc.)

        [[nodiscard]] bool operator==(const savepoint&) const noexcept = default;
    };

    // ============================================================================
    // tx_journal — append-only write-set journal with savepoint/rollback.
    //
    // The read-set is tracked separately and is NEVER truncated by rollback_to:
    // partial rollback undoes effects, not the fact that a key was observed, so the
    // transaction's validation footprint (optimistic conflict detection) stays
    // sound. This mirrors Medha's snapshot-validation contract.
    // ============================================================================

    class tx_journal {
    public:
        // Record a read into the read-set (idempotent duplicates are allowed;
        // Medha de-dups at validation time).
        void record_read(std::string resource_name, std::string key_expr) {
            reads_.push_back(read_entry{std::move(resource_name), std::move(key_expr)});
        }

        // Record a write. `prior` is the value the key held before this write (or
        // nullopt for a fresh key); it is what rollback restores.
        void record_write(std::string resource_name, std::string key_expr,
                          std::string value_expr,
                          std::optional<crank_value> prior, bool was_present) {
            writes_.push_back(journaled_write{
                std::move(resource_name), std::move(key_expr), std::move(value_expr),
                std::move(prior), was_present
            });
        }

        // Capture the current write-set position. Writes appended after this point
        // are the ones rollback_to(sp) will undo (in LIFO order).
        [[nodiscard]] savepoint make_savepoint(std::string label = {}) {
            return savepoint{writes_.size(), ++savepoint_seq_, std::move(label)};
        }

        // Undo every write recorded since `sp`, LIFO, invoking `undo` for each so the
        // host can restore the resource (undo receives the journaled_write, whose
        // `prior`/`was_present` describe how to revert). The read-set is retained.
        // Returns the number of writes rolled back. A stale/out-of-range savepoint
        // (marker past the current end) is treated as a no-op and returns 0.
        template <class UndoFn>
        std::size_t rollback_to(const savepoint& sp, UndoFn&& undo) {
            if (sp.marker > writes_.size()) return 0;
            std::size_t undone = 0;
            while (writes_.size() > sp.marker) {
                undo(writes_.back());
                writes_.pop_back();
                ++undone;
            }
            return undone;
        }

        // Discard a savepoint's rollback capability by folding its writes into the
        // parent (i.e. "commit" the nested region). No-op on the journal contents —
        // provided for symmetry / readability at nested-tx exit. Returns the number
        // of writes now owned by the parent since the savepoint.
        [[nodiscard]] std::size_t release(const savepoint& sp) const noexcept {
            return sp.marker <= writes_.size() ? writes_.size() - sp.marker : 0;
        }

        [[nodiscard]] const std::vector<journaled_write>& writes() const noexcept { return writes_; }
        [[nodiscard]] std::size_t read_count() const noexcept { return reads_.size(); }
        [[nodiscard]] std::size_t write_count() const noexcept { return writes_.size(); }

        struct read_entry {
            std::string resource_name;
            std::string key_expr;
        };

        [[nodiscard]] const std::vector<read_entry>& reads() const noexcept { return reads_; }

    private:
        std::vector<journaled_write> writes_;
        std::vector<read_entry> reads_;
        std::uint32_t savepoint_seq_ = 0;
    };

    // ============================================================================
    // nested_transaction — RAII-scoped implicit savepoint (§v2.12).
    //
    // Entering a nested transaction captures a savepoint on the parent journal.
    // The scope is committed with commit() (writes fold into parent) or rolled back
    // with rollback() (writes undone via the supplied undo sink). If neither is
    // called before destruction, the scope auto-rolls-back — a nested transaction
    // that escapes without an explicit commit must not leak partial effects.
    //
    // No virtual, no macros: the undo sink is a template parameter captured by the
    // factory `begin_nested`. Inheritance of isolation/retry is the parent's
    // responsibility (flatten semantics, CRANK-TX-NOTE-001); this type owns only the
    // write-set restore point.
    // ============================================================================

    template <class UndoFn>
    class nested_transaction {
    public:
        nested_transaction(tx_journal& j, UndoFn undo, std::string label)
            : journal_(&j), undo_(std::move(undo)),
              savepoint_(j.make_savepoint(std::move(label))) {}

        nested_transaction(const nested_transaction&) = delete;
        nested_transaction& operator=(const nested_transaction&) = delete;

        nested_transaction(nested_transaction&& o) noexcept
            : journal_(o.journal_), undo_(std::move(o.undo_)),
              savepoint_(o.savepoint_), settled_(o.settled_) {
            o.journal_ = nullptr;
            o.settled_ = true;
        }

        nested_transaction& operator=(nested_transaction&&) = delete;

        ~nested_transaction() {
            if (!settled_ && journal_) rollback(); // escape ⇒ auto-rollback
        }

        // Fold this nested region's writes into the parent. Returns the count folded.
        std::size_t commit() noexcept {
            settled_ = true;
            return journal_ ? journal_->release(savepoint_) : 0;
        }

        // Undo this nested region's writes (LIFO) via the undo sink; parent continues.
        std::size_t rollback() {
            settled_ = true;
            return journal_ ? journal_->rollback_to(savepoint_, undo_) : 0;
        }

        [[nodiscard]] const savepoint& sp() const noexcept { return savepoint_; }
        [[nodiscard]] bool settled() const noexcept { return settled_; }

    private:
        tx_journal* journal_;
        UndoFn undo_;
        savepoint savepoint_;
        bool settled_ = false;
    };

    // begin_nested — factory so UndoFn is deduced (no explicit template args at the
    // call site). `undo` is invoked once per rolled-back write.
    template <class UndoFn>
    [[nodiscard]] nested_transaction<UndoFn>
    begin_nested(tx_journal& j, UndoFn undo, std::string label = {}) {
        return nested_transaction<UndoFn>(j, std::move(undo), std::move(label));
    }

    // ============================================================================
    // compensation — a §v2.12 post-commit compensating action. NOT a rollback: the
    // transaction has already committed; a compensation is a *second* action that
    // best-effort undoes an externally-visible effect. Contract (see crank.md
    // §v2.12): best-effort (not guaranteed to run), retryable up to retry_limit,
    // MUST be idempotent (a compensation with idempotent=false is rejected at
    // registration), and on final failure surfaces a CompensationError — it never
    // rolls the committed tx back and may not perform irreversible effects.
    // ============================================================================

    struct compensation {
        std::string label; // human name for diagnostics
        bool idempotent = false; // required true — safe to run more than once
        std::uint32_t retry_limit = 1; // bounded retries (>=1) before it is a failure
    };

    // compensation_report — outcome of run_all: how many compensations ran (reached
    // a successful state) and how many failed after exhausting their retries.
    struct compensation_report {
        std::size_t ran = 0;
        std::size_t failed = 0;

        [[nodiscard]] std::size_t total() const noexcept { return ran + failed; }
        [[nodiscard]] bool all_ran() const noexcept { return failed == 0; }
    };

    // compensation_registry — records post-commit compensations and runs them.
    //
    // register_compensation rejects a non-idempotent compensation (returns false),
    // mirroring the §v2.12 idempotence requirement; a zero retry_limit is clamped
    // to 1 (at least one attempt). run_all invokes an effect sink for each recorded
    // compensation, retrying up to retry_limit; the sink reports success/failure
    // per attempt. Header-only, no virtual — the sink is a deduced callable.
    class compensation_registry {
    public:
        // Record a compensation. Returns false (rejected) if it is not idempotent.
        bool register_compensation(compensation c) {
            if (!c.idempotent) return false; // §v2.12: idempotence is required
            if (c.retry_limit == 0) c.retry_limit = 1;
            entries_.push_back(std::move(c));
            return true;
        }

        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

        // Run every recorded compensation via `sink`. `sink(const compensation&,
        // std::uint32_t attempt)` returns true on success. A compensation is retried
        // (attempt = 1..retry_limit) until it succeeds or the limit is reached.
        template <class EffectSink>
        [[nodiscard]] compensation_report run_all(EffectSink sink) const {
            compensation_report report{};
            for (const auto& c : entries_) {
                bool ok = false;
                for (std::uint32_t attempt = 1; attempt <= c.retry_limit; ++attempt) {
                    if (sink(c, attempt)) {
                        ok = true;
                        break;
                    }
                }
                if (ok) ++report.ran;
                else ++report.failed;
            }
            return report;
        }

    private:
        std::vector<compensation> entries_;
    };
} // namespace crank
