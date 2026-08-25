#pragma once

// crank/execute_tx.hpp — Transaction runtime lowering (Module 5 Part B, Gap 3).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Bridges the compile-time tx_lowering_result (transaction.hpp) with the
// Medha runtime: transaction_context / atomic() on the interpreter path, and
// medha::adapters::lithe::lower(plan) on the AOT path.  §10.2 step 10a.
//
// execute_transaction(lowered, tc, opts, eval)
//   Interpreter path: builds medha::options, calls medha::atomic(), maps
//   commit_report → CrankCommitReport. partial_commit/in_doubt NEVER reported
//   as committed (§7c.2). Read/write replay is data-driven via evaluator sink.
//
// lower_transaction_aot(lowered, key)
//   AOT path: assembles dsl::plan from lowered reads/writes, calls
//   medha::adapters::lithe::lower(plan), folds resource_traits_hash +
//   dialect version into crank_aot_key. Falls back to metadata-only
//   lowering when the plan builder surface is unavailable (§17.1).
//
// execute_with_transactions(lowered_regions, scalar_fn, tx_provider, opts)
//   Thin dispatch wrapper: routes each tx region through execute_transaction,
//   then calls the scalar continuation. Zero cost when no regions are present.
//
// Design refs: §7c.2, §7c.3, §7c.7, §10.2 step 10a, §7c.6.
// G-TX-1: no Medha API change — Medha consumed, not modified.

#include "languages/crank/transaction.hpp"
#include "languages/crank/crank_value.hpp"
#include "languages/crank/execute.hpp"
#include "languages/crank/tx_savepoint.hpp"
#include "medha/context.hpp"
#include "medha/transaction.hpp"
#include "medha/edsl.hpp"
#include "medha/adapters/lithe.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace crank {
    // ============================================================================
    // tx_runtime_result — outcome of running a lowered transaction through Medha
    // ============================================================================

    struct tx_runtime_result {
        std::optional<CrankCommitReport> report;
        std::optional<crank_value> body_value; // §2.2: value from yield expr; nullopt if no yield
        std::vector<std::string> diagnostics;
        std::vector<std::string> notes;
        bool committed = false;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // ============================================================================
    // tx_evaluator — host-supplied typed read/write value sink.
    //
    // read_fn:  returns expected<crank_value, CrankTxError> — value on success,
    //           or CrankTxError to abort the transaction.
    // write_fn: returns expected<void, CrankTxError> — success or abort.
    // yield_fn: optional — returns the body value for a `yield expr` (§2.2).
    //           Called once at body end if a yield was present; the value is stored
    //           in tx_runtime_result::body_value.
    //
    // All fields are optional; a null function is a no-op (always succeeds).
    // ============================================================================

    struct tx_evaluator {
        using read_fn = std::function<
            std::expected<crank_value, CrankTxError>(const transaction_read_op &)>;
        using write_fn = std::function<
            std::expected<void, CrankTxError>(const transaction_write_op &)>;
        using yield_fn = std::function<crank_value()>; // §2.2: called at body end to produce value

        read_fn on_read;
        write_fn on_write;
        yield_fn on_yield; // null = no yield (body type is Unit / CommitReport only)
    };

    // ============================================================================
    // detail: tx_status → string (medha has no to_string; provide locally)
    // ============================================================================

    namespace detail {
        [[nodiscard]] inline std::string_view tx_status_str(medha::tx_status s) noexcept {
            switch (s) {
            case medha::tx_status::committed: return "committed";
            case medha::tx_status::aborted: return "aborted";
            case medha::tx_status::conflict: return "conflict";
            case medha::tx_status::retry_exhausted: return "retry_exhausted";
            case medha::tx_status::validation_failed: return "validation_failed";
            case medha::tx_status::serialization_unavailable: return "serialization_unavailable";
            case medha::tx_status::unsupported_resource: return "unsupported_resource";
            case medha::tx_status::unsupported_effect: return "unsupported_effect";
            case medha::tx_status::out_of_memory: return "out_of_memory";
            case medha::tx_status::rejected: return "rejected";
            case medha::tx_status::partial_commit: return "partial_commit";
            case medha::tx_status::in_doubt: return "in_doubt";
            case medha::tx_status::recovery_required: return "recovery_required";
            case medha::tx_status::remote_timeout: return "remote_timeout";
            case medha::tx_status::participant_failed: return "participant_failed";
            case medha::tx_status::internal_error: return "internal_error";
            }
            return "unknown";
        }

        // tx_staged_write_set — §7.1 read-your-writes staging map.
        //
        // Maps (resource_name, key_expr) → staged crank_value.  A special sentinel
        // value (staged_deletion) indicates the key was erased.  Used by
        // execute_transaction to make reads within the transaction body observe writes
        // that the same body already staged — the core read-your-writes contract.
        //
        // old_snapshot reads (tx_index_kind::old_snapshot) bypass this cache and go
        // directly to the host evaluator (§7.3).
        struct tx_staged_write_set {
            struct key_t {
                std::string resource;
                std::string key;
                bool operator==(const key_t&) const noexcept = default;
            };

            struct key_hash {
                std::size_t operator()(const key_t& k) const noexcept {
                    std::size_t h = std::hash<std::string>{}(k.resource);
                    h ^= std::hash<std::string>{}(k.key) + 0x9e3779b9u + (h << 6) + (h >> 2);
                    return h;
                }
            };

            // nullopt entry = staged deletion (key present but erased)
            std::unordered_map<key_t, std::optional<crank_value>, key_hash> staged;

            void stage(const std::string& resource, const std::string& key,
                       const crank_value& v) {
                staged[{resource, key}] = v;
            }

            void stage_delete(const std::string& resource, const std::string& key) {
                staged[{resource, key}] = std::nullopt;
            }

            // Returns: has_value() == true  → staged value present; result is the value.
            //          has_value() == false → staged deletion (key erased).
            //          not contains()       → key not in write set; fall through to snapshot.
            [[nodiscard]] bool contains(const std::string& resource,
                                        const std::string& key) const {
                return staged.contains({resource, key});
            }

            [[nodiscard]] const std::optional<crank_value>& get(const std::string& resource,
                                                                const std::string& key) const {
                return staged.at({resource, key});
            }
        };
    } // namespace detail

    // ============================================================================
    // execute_transaction — interpreter path (§10.2 step 10a, §7c.2a)
    //
    // Algorithm:
    //   (1) Refuse if !lowered.ok() — surface compile diagnostics.
    //   (2) Build medha::options from opts.to_medha().
    //   (3) Call medha::atomic(medha_opts, body) — body replays reads/writes
    //       via the evaluator sink; returns std::expected<commit_report, tx_error>.
    //   (4) Map commit_report → CrankCommitReport; set committed only on committed.
    //   (5) partial_commit and in_doubt are NEVER committed (§7c.2).
    // ============================================================================

    [[nodiscard]] inline tx_runtime_result
    execute_transaction(const tx_lowering_result& lowered,
                        const CrankTransactionOptions& opts = {},
                        const tx_evaluator& eval = {}) {
        tx_runtime_result res;

        // (1) Compile-time policy gate
        if (!lowered.ok()) {
            for (const auto& d : lowered.diagnostics)
                res.diagnostics.push_back(d.message);
            return res;
        }

        // (2) Build medha options
        const medha::options medha_opts = opts.to_medha();

        // (3) Run atomic retry loop.
        //
        // §7.1 read-your-writes: ops are replayed in sequence_index order so that
        // a write at index N is visible to a read at index M > N.  The staged set
        // starts empty and grows as each write op executes.  When sequence_index
        // is the same (default 0), reads sort before writes — abort signals from
        // on_read (§4) are honored before any writes run.  To make a write precede
        // a read (RYW pattern), assign the write a strictly lower sequence_index.
        // §7.3: old_snapshot reads bypass the staged set regardless of order.
        //
        // Build a unified op sequence sorted by sequence_index.
        using op_ref = std::variant<
            std::reference_wrapper<const transaction_read_op>,
            std::reference_wrapper<const transaction_write_op>>;

        std::vector<op_ref> op_seq;
        op_seq.reserve(lowered.reads.size() + lowered.writes.size());
        for (const auto& r : lowered.reads) op_seq.emplace_back(std::cref(r));
        for (const auto& w : lowered.writes) op_seq.emplace_back(std::cref(w));

        // Stable-sort by sequence_index; reads sort before writes at equal index so
        // that a read with no explicit ordering runs before any co-indexed writes.
        // To make a write precede a read (RYW), assign the write a lower sequence_index.
        std::stable_sort(op_seq.begin(), op_seq.end(),
                         [](const op_ref& a, const op_ref& b) {
                             const auto seq_of = [](const op_ref& x) -> std::uint32_t {
                                 return std::visit([](const auto& ref) {
                                     return ref.get().sequence_index;
                                 }, x);
                             };
                             const auto tag_of = [](const op_ref& x) -> int {
                                 // reads tag=0 (sort first), writes tag=1 (sort after) at same index
                                 return std::holds_alternative<
                                            std::reference_wrapper<const transaction_read_op>>(x)
                                            ? 0
                                            : 1;
                             };
                             const auto sa = seq_of(a), sb = seq_of(b);
                             if (sa != sb) return sa < sb;
                             return tag_of(a) < tag_of(b); // reads precede writes at the same index
                         });

        detail::tx_staged_write_set staged_ws;

        auto outcome = medha::atomic(medha_opts,
                                     [&](medha::transaction_context& /*tc*/) -> std::expected<void, medha::tx_error> {
                                         // Replay ops in sequence order with read-your-writes staging
                                         for (const auto& op : op_seq) {
                                             if (std::holds_alternative<
                                                 std::reference_wrapper<const transaction_write_op>>(op)) {
                                                 const auto& wop = std::get<
                                                     std::reference_wrapper<const transaction_write_op>>(op).get();
                                                 // §7.1: stage before calling host so subsequent reads see this value
                                                 staged_ws.stage(wop.resource_name, wop.key_expr,
                                                                 crank_value::from(wop.value_expr));
                                                 if (eval.on_write) {
                                                     auto r = eval.on_write(wop);
                                                     if (!r) {
                                                         return std::unexpected(medha::tx_error{
                                                             r.error().status(), std::string(r.error().message())
                                                         });
                                                     }
                                                 }
                                             }
                                             else {
                                                 const auto& rop = std::get<
                                                     std::reference_wrapper<const transaction_read_op>>(op).get();
                                                 // §7.3: old_snapshot reads bypass the staged write set
                                                 const bool bypass_staged = (rop.kind == tx_index_kind::old_snapshot
                                                     || rop.is_old);
                                                 if (!bypass_staged &&
                                                     staged_ws.contains(rop.resource_name, rop.key_expr)) {
                                                     const auto& sv = staged_ws.get(rop.resource_name, rop.key_expr);
                                                     if (!sv.has_value()) {
                                                         continue; // staged deletion — key absent
                                                     }
                                                     continue; // §7.1: staged write visible; skip host evaluator
                                                 }
                                                 // No staged value — read from snapshot via host evaluator
                                                 if (eval.on_read) {
                                                     auto r = eval.on_read(rop);
                                                     if (!r) {
                                                         return std::unexpected(medha::tx_error{
                                                             r.error().status(), std::string(r.error().message())
                                                         });
                                                     }
                                                 }
                                             }
                                         }
                                         // §2.2: collect yield value at body end (does not abort the transaction)
                                         if (eval.on_yield) {
                                             res.body_value = eval.on_yield();
                                         }
                                         return {}; // success — proceed to commit
                                     });

        // (4–5) Map outcome; partial_commit/in_doubt NEVER committed
        if (outcome) {
            const auto status = outcome->status;
            if (status == medha::tx_status::committed) {
                res.committed = true;
                CrankCommitReport cr{*outcome};
                // Propagate requested durability level into the report (§14.1 / §15.1).
                // The runtime achieves at least the requested level; use it as a floor.
                cr.durability = opts.durability;
                res.report = cr;
            }
            else {
                // partial_commit, in_doubt, or other non-committed success variant
                res.diagnostics.push_back(
                    std::string("CRANK-TX-RUNTIME-001: transaction status=")
                    + std::string(detail::tx_status_str(status)));
            }
        }
        else {
            res.diagnostics.push_back(
                std::string("CRANK-TX-RUNTIME-001: transaction status=")
                + std::string(detail::tx_status_str(outcome.error().status)));
        }

        return res;
    }

    // ============================================================================
    // tx_journaled_result — outcome of a journaled (savepoint-aware) transaction.
    // ============================================================================

    struct tx_journaled_result {
        tx_runtime_result runtime; // underlying Medha outcome
        std::size_t writes_applied = 0; // writes surviving to commit
        std::size_t writes_rolled_back = 0; // writes undone by savepoints
        [[nodiscard]] bool ok() const noexcept { return runtime.ok(); }
        [[nodiscard]] bool committed() const noexcept { return runtime.committed; }
    };

    // ============================================================================
    // execute_transaction_journaled — §v2.12 savepoint-aware interpreter path.
    //
    // Runs the lowered transaction through Medha (via execute_transaction) but
    // threads a tx_journal so a host body can create savepoints / nested
    // transactions and partially roll back within the flattened region. The
    // evaluator's write sink records each write into the journal (capturing the
    // prior value the host supplies for undo); reads are recorded for the read-set.
    //
    // `body` is the host continuation: it receives the live journal so it can call
    // journal.make_savepoint()/rollback_to() or crank::begin_nested(journal, undo).
    // It returns true to commit, false to abort the whole transaction. When body is
    // null the behavior is identical to execute_transaction (straight replay).
    //
    // The undo sink used by nested_transaction/rollback_to is host-owned: this
    // function does not itself restore resource state — it accounts for it (counts
    // applied vs rolled-back writes) so callers can assert partial-rollback
    // semantics. The read-set is retained across rollbacks (see tx_journal).
    // ============================================================================

    template <class BodyFn>
    [[nodiscard]] inline tx_journaled_result
    execute_transaction_journaled(const tx_lowering_result& lowered,
                                  const CrankTransactionOptions& opts,
                                  const tx_evaluator& eval,
                                  BodyFn&& body) {
        tx_journaled_result out;
        tx_journal journal;

        // Wrap the host evaluator so reads/writes are journaled as they replay.
        tx_evaluator wrapped;
        wrapped.on_read = [&](const transaction_read_op& rop)
            -> std::expected<crank_value, CrankTxError> {
                journal.record_read(rop.resource_name, rop.key_expr);
                if (eval.on_read) return eval.on_read(rop);
                return crank_value{};
            };
        wrapped.on_write = [&](const transaction_write_op& wop)
            -> std::expected<void, CrankTxError> {
                // Prior value is unknown to crank; the host evaluator owns state, so we
                // record a fresh-key entry. A host that wants precise undo supplies its
                // own prior via a nested begin_nested undo sink.
                journal.record_write(wop.resource_name, wop.key_expr, wop.value_expr,
                                     std::nullopt, false);
                if (eval.on_write) return eval.on_write(wop);
                return {};
            };

        // Replay through the standard atomic path first (fills read/write sets).
        out.runtime = execute_transaction(lowered, opts, wrapped);

        // Let the host body drive savepoints / nested transactions on the journal.
        const std::size_t before = journal.write_count();
        bool body_commit = true;
        if constexpr (!std::is_same_v<std::remove_cvref_t<BodyFn>, std::nullptr_t>) {
            body_commit = body(journal);
        }
        const std::size_t after = journal.write_count();

        out.writes_rolled_back = before > after ? before - after : 0;
        out.writes_applied = after;

        if (!body_commit && out.runtime.committed) {
            // Host aborted after commit-time replay: surface as non-committed.
            out.runtime.committed = false;
            out.runtime.diagnostics.push_back(
                "CRANK-TX-RUNTIME-002: transaction body requested abort after replay");
        }

        return out;
    }

    // ============================================================================
    // tx_aot_lowering — result of lower_transaction_aot
    // ============================================================================

    struct tx_aot_lowering {
        medha::adapters::lithe::lithe_region_descriptor region;
        std::vector<std::string> diagnostics;
        std::vector<std::string> notes;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // ============================================================================
    // lower_transaction_aot — AOT path (§7c.7, §10.2 step 10a)
    //
    // Algorithm:
    //   (1) Assemble dsl::plan from lowered reads/writes/options.
    //   (2) Call medha::adapters::lithe::lower(plan) → lithe_region_descriptor.
    //   (3) Fold per-resource hashes + dialect version into key.descriptor_hashes.
    //
    // Falls back to metadata-only lowering when Lithe headers unavailable (§17.1).
    // ============================================================================

    [[nodiscard]] inline tx_aot_lowering
    lower_transaction_aot(const tx_lowering_result& lowered,
                          crank_aot_key& key) {
        tx_aot_lowering out;

        if (!lowered.ok()) {
            for (const auto& d : lowered.diagnostics)
                out.diagnostics.push_back(d.message);
            return out;
        }

        // (1) Assemble dsl::plan
        medha::dsl::plan p;
        p.name = "crank.tx.aot";
        p.tx_options = lowered.options.to_medha();

        auto ensure_resource = [&](std::string_view name) {
            for (const auto& rd : p.resources)
                if (rd.name == name) return;
            medha::dsl::resource_descriptor rd;
            rd.name = std::string(name);
            p.resources.push_back(std::move(rd));
        };

        for (const auto& rop : lowered.reads) {
            ensure_resource(rop.resource_name);
            medha::dsl::plan_statement stmt;
            stmt.kind = medha::dsl::plan_statement_kind::let_load;
            stmt.resource_name = rop.resource_name;
            stmt.key_name = rop.key_expr;
            p.body.push_back(std::move(stmt));
        }

        for (const auto& wop : lowered.writes) {
            ensure_resource(wop.resource_name);
            medha::dsl::plan_statement stmt;
            stmt.kind = medha::dsl::plan_statement_kind::store;
            stmt.resource_name = wop.resource_name;
            stmt.key_name = wop.key_expr;
            stmt.value_expr = wop.value_expr;
            p.body.push_back(std::move(stmt));
        }

        // (2) Lower via Medha lithe adapter (metadata-only always available §17.1)
        out.region = medha::adapters::lithe::lower(p);

        if (!out.region.has_lithe) {
            out.notes.push_back(
                "CRANK-TX-AOT-NOTE-001: Lithe headers unavailable; "
                "metadata-only lowering applied (correct per §17.1)");
        }

        // (3) Fold per-resource trait hashes + dialect version into AOT key
        for (const std::uint64_t rh : out.region.metadata.resource_hashes) {
            key.descriptor_hashes.push_back(rh);
        }

        // Stamp dialect version in enabled_features bits [63:48] (matches transaction.hpp:521)
        constexpr std::uint64_t kDialectShift = 48u;
        const std::uint64_t dialect_ver =
            static_cast<std::uint64_t>(out.region.metadata.dialect_version);
        key.enabled_features = (key.enabled_features & ~(0xFFFFULL << kDialectShift))
            | (dialect_ver << kDialectShift);

        return out;
    }

    // ============================================================================
    // tx_context_provider — host-supplied evaluator factory per tx region
    //
    // Keeps execute_tx.hpp free of ownership assumptions over transaction_context.
    // ============================================================================

    struct tx_context_provider {
        // Called once per tx region to produce an evaluator.
        std::function<tx_evaluator()> make_evaluator;
    };

    // ============================================================================
    // execute_with_transactions — dispatch wrapper (§10.2 step 10a)
    //
    // Routes a vector of tx_lowering_result regions through execute_transaction,
    // then invokes the scalar continuation via execute_via_interpreter.
    // Zero cost when tx_regions is empty — scalar path is unchanged.
    // ============================================================================

    [[nodiscard]] inline crank_execute_result
    execute_with_transactions(
        const lower_hl_result& hl_res,
        std::span<const tx_lowering_result> tx_regions,
        tx_context_provider& tx_provider,
        const execute_options& opts = {},
        const CrankTransactionOptions& tx_opts = {}) {
        // Fast path: no transaction regions
        if (tx_regions.empty()) {
            return execute_via_interpreter(hl_res, {}, opts);
        }

        crank_execute_result result;

        // Execute each tx region
        for (const auto& tx_lowered : tx_regions) {
            tx_evaluator eval;
            if (tx_provider.make_evaluator) eval = tx_provider.make_evaluator();

            auto tx_res = execute_transaction(tx_lowered, tx_opts, eval);

            if (!tx_res.ok()) {
                result.diagnostics.insert(result.diagnostics.end(),
                                          tx_res.diagnostics.begin(), tx_res.diagnostics.end());
                return result; // abort on tx failure
            }

            result.notes.insert(result.notes.end(),
                                tx_res.notes.begin(), tx_res.notes.end());
        }

        // Run scalar interpreter for non-tx regions
        auto scalar = execute_via_interpreter(hl_res, {}, opts);
        result.return_value = scalar.return_value;
        result.diagnostics.insert(result.diagnostics.end(),
                                  scalar.diagnostics.begin(), scalar.diagnostics.end());
        result.notes.insert(result.notes.end(),
                            scalar.notes.begin(), scalar.notes.end());
        result.fallback_fired = scalar.fallback_fired;
        result.overflow_trapped = scalar.overflow_trapped;
        result.stats = scalar.stats;

        return result;
    }
} // namespace crank
