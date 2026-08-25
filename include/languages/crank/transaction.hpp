#pragma once

// crank/transaction.hpp — Transaction lowering + compile-time policy checks (Module 5 Part A).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Surfaces:
//   transaction_state  — runtime state machine (§3.1)
//   transaction_context — runtime context snapshot (§3.2)
//   TxErrorKind        — typed error discriminant (§5.1)
//   backoff_kind / retry_policy — retry + backoff configuration (§10.3)
//   transaction_read_op / transaction_write_op — lowered tx-indexing ops
//   tx_policy_flags   — compile-time constraint record for a transaction block
//   tx_policy_checker — validates policy rules (Step A3) before lowering
//   resource_capability_checker — per-resource capability validation (§6.3)
//   tx_lowering_result — lowered transaction: ops + policy flags + diagnostics
//   lower_transaction  — lower an AST tx-block to a tx_lowering_result
//   tx_plan_record     — snapshot for JSON dump (dump.hpp Step B4)
//
// Design refs: design.md §7c.2/§7c.3/§7c.4/§7c.5/§7c.6; impl-5.md Steps A1–A4.
//
// Ownership: crank owns syntax + policy; Medha owns read/write-set at runtime.
// No Medha API change required (G-TX-1).
//
// core.tx type mapping (§7c.2):
//   TxStatus           ↔ medha::tx_status
//   TxError            ↔ medha::tx_error     (status/message accessors)
//   CommitReport       ↔ medha::commit_report
//   TransactionOptions ↔ medha::options
//   Isolation          ↔ medha::isolation
//   ReplaySafety       ↔ medha::replay_safety
//   ConflictPolicy     ↔ medha::conflict::optimistic/pessimistic/deterministic
//   PartialCommit      ↔ medha::partial_commit_policy
//   ProofStatus        ↔ medha::proof_status

#include "languages/crank/source_span.hpp"
#include "languages/crank/std_types.hpp"
#include "languages/crank/safety.hpp"
#include "languages/crank/effects.hpp"
#include "languages/crank/aot.hpp"
#include "medha/medha.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace crank {
    // ============================================================================
    // transaction_state — runtime state machine (§3.1)
    //
    // Transitions (only forward; committed/aborted are terminal):
    //   created → active → validating → preparing → prepared → committing → committed
    //   Any non-terminal state may transition to aborting → aborted
    //   prepared/committing under process interruption → in_doubt
    // ============================================================================

    enum class transaction_state : std::uint8_t {
        created, // allocated, not yet started
        active, // body is executing
        validating, // read-set validation in progress
        preparing, // 2PC phase 1 in progress (participants being prepared)
        prepared, // all participants voted prepared; awaiting coordinator decision
        committing, // 2PC phase 2: commit being applied
        committed, // terminal — all writes visible, durability point reached
        aborting, // rollback in progress
        aborted, // terminal — writes discarded
        in_doubt, // coordinator decision uncertain after interruption
    };

    [[nodiscard]] constexpr bool is_terminal(transaction_state s) noexcept {
        return s == transaction_state::committed || s == transaction_state::aborted;
    }

    [[nodiscard]] constexpr std::string_view to_string(transaction_state s) noexcept {
        switch (s) {
        case transaction_state::created: return "created";
        case transaction_state::active: return "active";
        case transaction_state::validating: return "validating";
        case transaction_state::preparing: return "preparing";
        case transaction_state::prepared: return "prepared";
        case transaction_state::committing: return "committing";
        case transaction_state::committed: return "committed";
        case transaction_state::aborting: return "aborting";
        case transaction_state::aborted: return "aborted";
        case transaction_state::in_doubt: return "in_doubt";
        }
        return "unknown";
    }

    // ============================================================================
    // transaction_context — runtime snapshot carried through a live transaction (§3.2)
    //
    // Crank owns this descriptor; Medha owns the actual read/write set objects.
    // The context is reset between retry attempts (§10.4): attempt increments,
    // snapshot_version is refreshed, read/write set sizes are cleared.
    // ============================================================================

    struct transaction_context {
        std::uint64_t id = 0; // stable transaction identity across retries
        transaction_state state = transaction_state::created;
        medha::isolation isolation = medha::isolation::snapshot;
        medha::replay_safety replay = medha::replay_safety::unknown;
        std::uint32_t attempt = 0; // 0-based; increments on each retry
        std::uint64_t snapshot_version = 0; // version at which the read snapshot was taken
        std::uint32_t read_count = 0; // keys in the read set for this attempt
        std::uint32_t write_count = 0; // keys in the write set for this attempt
        std::uint32_t savepoint_depth = 0; // active nested savepoints
        bool has_deadline = false;
        bool cancelled = false;
    };

    // ============================================================================
    // TxErrorKind — typed error discriminant (§5.1)
    //
    // Crank owns a richer kind enum than medha::tx_status provides.  The mapping
    // to medha::tx_status is many-to-one; crank carries the fine-grained kind for
    // diagnostics while Medha carries the coarser runtime outcome.
    // ============================================================================

    enum class TxErrorKind : std::uint8_t {
        Conflict,
        ValidationFailed,
        SnapshotUnavailable,
        ResourceNotTransactional,
        StagingUnsupported,
        RollbackUnsupported,
        RollbackFailed,
        CommitFailed,
        PrepareFailed,
        CoordinatorUnavailable,
        CoordinatorRejected,
        DeadlineExceeded,
        Cancelled,
        ReplayUnsafe,
        SerializationFailure,
        PartialCommit,
        InDoubt,
        HostFailure,
        InternalInvariant,
    };

    [[nodiscard]] constexpr std::string_view to_string(TxErrorKind k) noexcept {
        switch (k) {
        case TxErrorKind::Conflict: return "Conflict";
        case TxErrorKind::ValidationFailed: return "ValidationFailed";
        case TxErrorKind::SnapshotUnavailable: return "SnapshotUnavailable";
        case TxErrorKind::ResourceNotTransactional: return "ResourceNotTransactional";
        case TxErrorKind::StagingUnsupported: return "StagingUnsupported";
        case TxErrorKind::RollbackUnsupported: return "RollbackUnsupported";
        case TxErrorKind::RollbackFailed: return "RollbackFailed";
        case TxErrorKind::CommitFailed: return "CommitFailed";
        case TxErrorKind::PrepareFailed: return "PrepareFailed";
        case TxErrorKind::CoordinatorUnavailable: return "CoordinatorUnavailable";
        case TxErrorKind::CoordinatorRejected: return "CoordinatorRejected";
        case TxErrorKind::DeadlineExceeded: return "DeadlineExceeded";
        case TxErrorKind::Cancelled: return "Cancelled";
        case TxErrorKind::ReplayUnsafe: return "ReplayUnsafe";
        case TxErrorKind::SerializationFailure: return "SerializationFailure";
        case TxErrorKind::PartialCommit: return "PartialCommit";
        case TxErrorKind::InDoubt: return "InDoubt";
        case TxErrorKind::HostFailure: return "HostFailure";
        case TxErrorKind::InternalInvariant: return "InternalInvariant";
        }
        return "Unknown";
    }

    // Map medha::tx_status to the closest TxErrorKind (used when constructing
    // CrankTxError from a Medha runtime outcome).
    [[nodiscard]] constexpr TxErrorKind tx_error_kind_from_status(medha::tx_status s) noexcept {
        switch (s) {
        case medha::tx_status::conflict: return TxErrorKind::Conflict;
        case medha::tx_status::validation_failed: return TxErrorKind::ValidationFailed;
        case medha::tx_status::serialization_unavailable: return TxErrorKind::SerializationFailure;
        case medha::tx_status::unsupported_resource: return TxErrorKind::ResourceNotTransactional;
        case medha::tx_status::unsupported_effect: return TxErrorKind::StagingUnsupported;
        case medha::tx_status::rejected: return TxErrorKind::CommitFailed;
        case medha::tx_status::partial_commit: return TxErrorKind::PartialCommit;
        case medha::tx_status::in_doubt: return TxErrorKind::InDoubt;
        case medha::tx_status::recovery_required: return TxErrorKind::InDoubt;
        case medha::tx_status::remote_timeout: return TxErrorKind::DeadlineExceeded;
        case medha::tx_status::participant_failed: return TxErrorKind::CoordinatorRejected;
        case medha::tx_status::retry_exhausted: return TxErrorKind::Conflict;
        case medha::tx_status::out_of_memory: return TxErrorKind::HostFailure;
        case medha::tx_status::internal_error: return TxErrorKind::InternalInvariant;
        case medha::tx_status::aborted: return TxErrorKind::CommitFailed;
        case medha::tx_status::committed: return TxErrorKind::InternalInvariant;
        }
        return TxErrorKind::InternalInvariant;
    }

    // ============================================================================
    // backoff_kind / retry_policy — crank-owned retry + backoff configuration (§10.3)
    //
    // Crank owns this so tests can inject a deterministic jitter source (jitter=false).
    // The policy is separate from medha::retry because crank adds jitter + fine-grained
    // backoff configuration beyond what medha::retry::backoff exposes.
    // ============================================================================

    enum class backoff_kind : std::uint8_t {
        none, // no delay between retries
        linear, // delay = initial_delay × attempt
        exponential, // delay = min(maximum_delay, initial_delay × 2^attempt)
        constant, // delay = initial_delay (fixed)
    };

    struct retry_policy {
        std::uint32_t max_attempts = 1;
        std::chrono::nanoseconds initial_delay{0};
        std::chrono::nanoseconds maximum_delay{0};
        backoff_kind kind = backoff_kind::none;
        bool jitter = false; // add bounded random jitter

        // Compute the delay for a given attempt (0-based).
        // Returns 0 when kind==none or attempt==0.
        [[nodiscard]] std::chrono::nanoseconds delay_for(std::uint32_t attempt) const noexcept {
            if (kind == backoff_kind::none || initial_delay.count() == 0) {
                return std::chrono::nanoseconds{0};
            }
            auto base = initial_delay.count();
            std::int64_t d = 0;
            switch (kind) {
            case backoff_kind::none:
                break;
            case backoff_kind::constant:
                d = base;
                break;
            case backoff_kind::linear:
                d = base * static_cast<std::int64_t>(attempt + 1);
                break;
            case backoff_kind::exponential: {
                std::int64_t exp = static_cast<std::int64_t>(1) << attempt;
                d = base * exp;
                break;
            }
            }
            if (maximum_delay.count() > 0 && d > maximum_delay.count()) {
                d = maximum_delay.count();
            }
            return std::chrono::nanoseconds{d};
        }

        // Convert to medha::retry for use in Medha options (maps attempts only;
        // backoff scheduling is crank's responsibility between retries).
        [[nodiscard]] std::variant<medha::retry::none, medha::retry::bounded>
        to_medha() const noexcept {
            if (max_attempts <= 1) return medha::retry::none{};
            return medha::retry::bounded{max_attempts - 1}; // medha counts retries not attempts
        }
    };

    // ============================================================================
    // core.tx C++ type mapping (§7c.2)
    // These aliases make the 1:1 relationship explicit; crank never re-invents them.
    // ============================================================================

    // Enum mappings
    using CrankTxStatus = medha::tx_status;
    using CrankIsolation = medha::isolation;
    using CrankReplaySafety = medha::replay_safety;
    using CrankPartialCommit = medha::partial_commit_policy;
    using CrankProofStatus = medha::proof_status;

    // Conflict policy is a tag-based variant in Medha; expose the useful discriminant
    enum class CrankConflictPolicy : std::uint8_t {
        optimistic = 0,
        pessimistic = 1,
        deterministic = 2,
    };

    // durability_level — §14.1 durability levels for transactions.
    //
    // | Level   | Commit point                                          |
    // |---------|-------------------------------------------------------|
    // | memory  | Visible in process memory only                        |
    // | process | Runtime journal updated; survives worker failure       |
    // | durable | Required records flushed to durable storage            |
    //
    // A CrankCommitReport carries the *achieved* durability level (§15.1).
    // The requested level is part of CrankTransactionOptions; the runtime
    // may achieve a stronger level but must never claim a weaker one.
    enum class durability_level : std::uint8_t {
        memory, // committed to in-process memory
        process, // committed to process-local durable journal (WAL)
        durable, // committed to external durable storage
    };

    [[nodiscard]] constexpr std::string_view to_string(durability_level d) noexcept {
        switch (d) {
        case durability_level::memory: return "memory";
        case durability_level::process: return "process";
        case durability_level::durable: return "durable";
        }
        return "unknown";
    }

    // TxError: structured record with named accessors and typed kind discriminant.
    // Crank owns copied error strings — no string_view from Medha escaping lifetime.
    struct CrankTxError {
        medha::tx_error inner;
        TxErrorKind kind = TxErrorKind::InternalInvariant;
        std::string resource; // resource name, if applicable
        std::string key; // key, if applicable
        bool retryable = false;
        source_span at; // source location of the transaction expression

        [[nodiscard]] CrankTxStatus status() const noexcept { return inner.status; }
        [[nodiscard]] std::string_view message() const noexcept { return inner.message; }

        // Construct from a Medha tx_error; kind is inferred from status.
        [[nodiscard]] static CrankTxError from(const medha::tx_error& e,
                                               source_span span = {}) noexcept {
            CrankTxError r;
            r.inner = e;
            r.kind = tx_error_kind_from_status(e.status);
            r.retryable = (e.status == medha::tx_status::conflict
                || e.status == medha::tx_status::retry_exhausted);
            r.at = span;
            return r;
        }

        // FromSafetyError concept: TxError can wrap a SafetyError
        [[nodiscard]] static CrankTxError from(const SafetyError& se) noexcept {
            return CrankTxError{
                medha::tx_error{
                    medha::tx_status::rejected,
                    to_string(se.kind)
                },
                TxErrorKind::ValidationFailed,
                {}, {}, false, {}
            };
        }
    };

    // CommitReport: full structured record per §15.1.
    // A report with status==committed may be produced only after:
    //   - commit decision is final
    //   - required participants accepted
    //   - requested durability point reached
    //   - externally visible staged state published
    struct CrankCommitReport {
        medha::commit_report inner;

        // Stable transaction identity for lookup_commit_report (§15.3).
        // Set by execute_transaction; 0 when unknown.
        std::uint64_t transaction_id = 0;

        // Timing (crank-owned; not derived from medha::commit_report)
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;
        time_point started_at;
        time_point committed_at;

        // Extended read/write accounting
        std::uint32_t resources_read = 0;
        std::uint32_t resources_written = 0;
        std::uint64_t keys_read = 0;
        std::uint64_t keys_written = 0;

        // Coordinator identity (empty = no coordinator / single-resource)
        std::string coordinator;

        // Achieved durability level (§14.1 / §15.1). Set by execute_transaction
        // to the level the commit actually reached. Defaults to memory (lowest).
        durability_level durability = durability_level::memory;

        // Proof status from Tarka (§15.1 proof_status)
        CrankProofStatus proof_status = CrankProofStatus::deferred;

        // Trace identity for distributed tracing
        std::uint64_t trace_id = 0;

        // Primary accessors (delegate to medha::commit_report)
        [[nodiscard]] CrankTxStatus status() const noexcept { return inner.status; }
        [[nodiscard]] std::uint32_t attempts() const noexcept { return inner.attempts; }
        [[nodiscard]] std::uint32_t reads() const noexcept { return inner.reads; }
        [[nodiscard]] std::uint32_t writes() const noexcept { return inner.writes; }
        [[nodiscard]] std::uint32_t conflicts() const noexcept { return inner.conflicts; }

        [[nodiscard]] std::uint64_t duration_ns() const noexcept {
            if (committed_at == time_point{}) return 0;
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    committed_at - started_at).count());
        }

        // Invariant: only callable when status==committed (§15.2)
        [[nodiscard]] bool is_committed() const noexcept {
            return inner.status == medha::tx_status::committed;
        }
    };

    // TransactionOptions: aggregate (mirrors medha::options defaults)
    // Defaults: isolation=snapshot, retry=0, conflict=optimistic,
    //           distribution=none, replay=unknown, partial=require_atomic_coordinator
    struct CrankTransactionOptions {
        CrankIsolation isolation = CrankIsolation::snapshot;
        CrankReplaySafety replay = CrankReplaySafety::unknown;
        CrankConflictPolicy conflict = CrankConflictPolicy::optimistic;
        CrankPartialCommit partial = CrankPartialCommit::require_atomic_coordinator;
        durability_level durability = durability_level::memory; // §14.1 requested level
        std::uint32_t retry = 0;
        // distribution=none always in v1

        // §v2.11 multi-resource coordinator. Empty = no coordinator (v1 behavior).
        // A named coordinator lifts the v1 single-resource restriction (CRANK-TX-002)
        // by promising a 2PC/atomic-commit coordinator spans the participants.
        std::string coordinator;

        [[nodiscard]] bool has_coordinator() const noexcept { return !coordinator.empty(); }

        // Convert to medha::options for runtime use
        [[nodiscard]] medha::options to_medha() const noexcept {
            medha::options o;
            o.isolation = isolation;
            o.replay = replay;
            o.partial = partial;
            if (retry == 0) {
                o.retry = medha::retry::none{};
            }
            else {
                o.retry = medha::retry::bounded{retry};
            }
            switch (conflict) {
            case CrankConflictPolicy::optimistic:
                o.conflict = medha::conflict::optimistic{};
                break;
            case CrankConflictPolicy::pessimistic:
                o.conflict = medha::conflict::pessimistic{};
                break;
            case CrankConflictPolicy::deterministic:
                o.conflict = medha::conflict::deterministic{};
                break;
            }
            // §v2.11: a named coordinator maps to Medha's coordinated distribution so
            // it runs the cross-resource atomic-commit path; without one, v1
            // distribution=none is preserved exactly.
            if (has_coordinator()) {
                o.distribution = medha::distribution::coordinated{};
            }
            else {
                o.distribution = medha::distribution::none{};
            }
            return o;
        }
    };

    // ============================================================================
    // Resource traits query helpers (§7c.3, §9.5)
    // Tested at compile time; templates instantiated per resource type.
    // ============================================================================

    template <class R>
    inline constexpr bool is_transactional_resource =
        medha::resource_traits<R>::transactional;

    template <class R>
    inline constexpr bool resource_supports_snapshot =
        medha::resource_traits<R>::supports_snapshot;

    template <class R>
    inline constexpr medha::commit_capability resource_commit_capability =
        medha::resource_traits<R>::commit_protocol;

    template <class R>
    inline constexpr bool resource_aba_safe =
        medha::resource_traits<R>::aba_safe;

    // ============================================================================
    // tx_index_kind — which type of tx-indexing lowering was applied (§7c.4)
    // ============================================================================

    enum class tx_index_kind : std::uint8_t {
        point_read, // resource[key]   → ctx.load  (read set)
        point_write, // resource[key]=v → ctx.store (write set)
        range_read, // resource[lo..hi]→ ctx.load  (read_kind::range)
        old_snapshot, // old(resource[key]) → value at tx entry (snapshot)
    };

    // ============================================================================
    // transaction_read_op / transaction_write_op — lowered tx-indexing ops
    // ============================================================================

    struct transaction_read_op {
        std::string resource_name;
        std::string key_expr;
        tx_index_kind kind = tx_index_kind::point_read;
        bool is_old = false; // old(resource[key]) in tx body
        source_span at;
        std::uint32_t sequence_index = 0; // source-order index for interleaved replay
    };

    struct transaction_write_op {
        std::string resource_name;
        std::string key_expr;
        std::string value_expr;
        source_span at;
        std::uint32_t sequence_index = 0; // source-order index for interleaved replay
    };

    // ============================================================================
    // tx_diagnostic_kind — compile-time tx policy diagnostic codes (§7c.3/§7c.5/§7c.6)
    // ============================================================================

    enum class tx_diagnostic_kind : std::uint8_t {
        non_transactional_write, // write to non-transactional resource in tx
        cross_resource_serializable, // serializable + >1 transactional resource (v1)
        old_needs_snapshot, // old(resource[k]) where resource !supports_snapshot
        retry_replay_conflict, // retry>0 + replay=non_idempotent|unknown (MEDHA-004/RSF-005)
        async_in_tx, // await/spawn inside transaction
        tx_under_parallel, // transaction nested under @parallel
        non_result_no_policy, // non-Result fn without @on_safety_failure + tx
        irreversible_effect_in_tx, // irreversible effect inside transaction body
        nested_tx_flatten_note, // info: nested same-thread tx flattened
        distribution_v1_only_none, // distribution != none attempted (v1)
        coordinator_resource_not_transactional, // §v2.11: participant under coordinator is non-transactional
        coordinator_unregistered, // §v2.11: coordinator name not registered on the context
        snapshot_multi_write_no_coordinator, // §v2.11: snapshot writes to >1 resource with no coordinator
    };

    [[nodiscard]] constexpr std::string_view to_string(tx_diagnostic_kind k) noexcept {
        switch (k) {
        case tx_diagnostic_kind::non_transactional_write: return "CRANK-TX-001";
        case tx_diagnostic_kind::cross_resource_serializable: return "CRANK-TX-002";
        case tx_diagnostic_kind::old_needs_snapshot: return "CRANK-TX-003";
        case tx_diagnostic_kind::retry_replay_conflict: return "CRANK-TX-004";
        case tx_diagnostic_kind::async_in_tx: return "CRANK-TX-005";
        case tx_diagnostic_kind::tx_under_parallel: return "CRANK-TX-006";
        case tx_diagnostic_kind::non_result_no_policy: return "CRANK-TX-007";
        case tx_diagnostic_kind::irreversible_effect_in_tx: return "CRANK-TX-008";
        case tx_diagnostic_kind::nested_tx_flatten_note: return "CRANK-TX-NOTE-001";
        case tx_diagnostic_kind::distribution_v1_only_none: return "CRANK-TX-009";
        case tx_diagnostic_kind::coordinator_resource_not_transactional: return "CRANK-TX-010";
        case tx_diagnostic_kind::coordinator_unregistered: return "CRANK-TX-011";
        case tx_diagnostic_kind::snapshot_multi_write_no_coordinator: return "CRANK-TX-012";
        }
        return "CRANK-TX-???";
    }

    struct tx_compile_diagnostic {
        tx_diagnostic_kind kind;
        source_span at;
        std::string message;
        bool is_error = true; // false = note/info
    };

    // ============================================================================
    // tx_policy_flags — compile-time constraint record for a transaction block
    // Captures analysis inputs fed into tx_policy_checker.
    // ============================================================================

    struct tx_policy_flags {
        // Options from the @tx(...) annotation or defaults
        CrankTransactionOptions options;

        // Resource analysis
        std::uint32_t transactional_resource_count = 0;
        std::uint32_t non_transactional_write_count = 0;
        bool any_resource_supports_snapshot = false;
        bool all_reads_provably_immutable = false;

        // §v2.11 coordinator analysis. Set when a coordinator name is present on the
        // options and the sema pass has resolved participant/registration facts.
        bool coordinator_registered = false; // name found via ctx.register_coordinator
        std::uint32_t coordinator_nontx_participant_count = 0;
        // participants under the coordinator that are non-transactional
        bool multi_resource_write = false; // body stages writes to >1 distinct transactional resource

        // Body analysis
        bool has_async_in_body = false; // await/spawn present
        bool under_parallel_attr = false; // enclosing @parallel
        bool has_nested_tx = false; // nested same-thread transaction
        bool has_irreversible_effect = false; // irreversible effect in body (module 2 effects)
        bool has_old_expr = false; // old(resource[key]) used
        bool fn_returns_result = false; // fn's return type is Result<T,E>
        bool fn_has_safety_attr = false; // fn has @on_safety_failure(...)

        // AOT
        std::uint64_t resource_traits_hash = 0; // FNV-1a of per-resource traits (§7c.7)
        std::uint64_t medha_dialect_version = 1; // bumped on protocol change
    };

    // ============================================================================
    // tx_policy_checker — validates all §7c.3/§7c.5/§7c.6 rules, pre-lowering
    // ============================================================================

    class tx_policy_checker {
    public:
        tx_policy_checker() = default;

        [[nodiscard]] std::vector<tx_compile_diagnostic>
        check(const tx_policy_flags& flags, source_span tx_span) const {
            std::vector<tx_compile_diagnostic> diags;

            // (1) Non-transactional writes (§7c.3)
            if (flags.non_transactional_write_count > 0) {
                diags.push_back({
                    tx_diagnostic_kind::non_transactional_write, tx_span,
                    "transaction body writes to a non-transactional resource; "
                    "only transactional resources (medha::resource_traits<R>::transactional==true) "
                    "may be written inside a transaction"
                });
            }

            // (2) Cross-resource serializable restriction (§7c.4 v1). §v2.11 lifts
            // this when a named coordinator is present: the coordinator provides the
            // cross-resource atomic-commit path the v1 restriction assumed absent.
            if (flags.options.isolation == CrankIsolation::serializable
                && flags.transactional_resource_count > 1
                && !flags.options.has_coordinator()) {
                diags.push_back({
                    tx_diagnostic_kind::cross_resource_serializable, tx_span,
                    "CRANK-TX-002: serializable isolation with >1 transactional resource is "
                    "not supported in v1 (no cross-resource coordinator); "
                    "use isolation=snapshot, a single transactional resource, or declare "
                    "coordinator=\"name\" (§v2.11)"
                });
            }

            // (2b) §v2.11 coordinator well-formedness. Only meaningful when a
            // coordinator name is declared on the transaction options.
            if (flags.options.has_coordinator()) {
                if (!flags.coordinator_registered) {
                    diags.push_back({
                        tx_diagnostic_kind::coordinator_unregistered, tx_span,
                        "CRANK-TX-011: transaction declares coordinator=\""
                        + flags.options.coordinator + "\" but no coordinator with that "
                        "name is registered on the context (ctx.register_coordinator<C>(\"name\"))"
                    });
                }
                if (flags.coordinator_nontx_participant_count > 0) {
                    diags.push_back({
                        tx_diagnostic_kind::coordinator_resource_not_transactional, tx_span,
                        "CRANK-TX-010: a resource enrolled under coordinator=\""
                        + flags.options.coordinator + "\" is non-transactional; every "
                        "participant in a coordinated transaction must have "
                        "medha::resource_traits<R>::transactional==true"
                    });
                }
            }

            // (2c) §v2.11 snapshot multi-resource write atomicity. snapshot gives a
            // consistent multi-resource *read* view, but not an atomic multi-resource
            // *commit*: writes to >1 distinct transactional resource under snapshot
            // would commit independently. Require a coordinator for atomic commit.
            if (flags.options.isolation == CrankIsolation::snapshot
                && flags.multi_resource_write
                && flags.transactional_resource_count > 1
                && !flags.options.has_coordinator()) {
                diags.push_back({
                    tx_diagnostic_kind::snapshot_multi_write_no_coordinator, tx_span,
                    "CRANK-TX-012: snapshot isolation gives consistent multi-resource reads "
                    "but not atomic multi-resource writes; writing >1 transactional resource "
                    "under snapshot without a coordinator would commit each resource "
                    "independently. Declare coordinator=\"name\" (§v2.11) for atomic commit, "
                    "or restrict writes to a single resource"
                });
            }

            // (3) old(resource[key]) snapshot capability (§7c.3)
            if (flags.has_old_expr && !flags.any_resource_supports_snapshot) {
                diags.push_back({
                    tx_diagnostic_kind::old_needs_snapshot, tx_span,
                    "CRANK-TX-003: old(resource[key]) requires resource to have "
                    "medha::resource_traits<R>::supports_snapshot==true"
                });
            }

            // (4) Retry/replay (§7c.5): crank is stricter than Medha
            //   reject retry>0 + replay=non_idempotent (MEDHA-004)
            //   reject retry>0 + replay=unknown (MEDHA-RSF-005; crank stricter)
            if (flags.options.retry > 0) {
                if (flags.options.replay == CrankReplaySafety::non_idempotent) {
                    diags.push_back({
                        tx_diagnostic_kind::retry_replay_conflict, tx_span,
                        "CRANK-TX-004 (MEDHA-004): retry>0 with replay=non_idempotent is forbidden; "
                        "use replay=body_idempotent or body_and_effects_idempotent"
                    });
                }
                else if (flags.options.replay == CrankReplaySafety::unknown) {
                    diags.push_back({
                        tx_diagnostic_kind::retry_replay_conflict, tx_span,
                        "CRANK-TX-004 (MEDHA-RSF-005): retry>0 with replay=unknown is rejected by crank; "
                        "use replay=body_idempotent, body_and_effects_idempotent, or "
                        "unknown_but_retry_allowed to opt in explicitly"
                    });
                }
            }

            // (5) Irreversible effect in tx body (§7c.5)
            if (flags.has_irreversible_effect) {
                diags.push_back({
                    tx_diagnostic_kind::irreversible_effect_in_tx, tx_span,
                    "CRANK-TX-008: irreversible effect inside transaction body; "
                    "use the transactional-outbox pattern (append=staged) for I/O"
                });
            }

            // (6) async in tx (§7c.6)
            if (flags.has_async_in_body) {
                diags.push_back({
                    tx_diagnostic_kind::async_in_tx, tx_span,
                    "CRANK-TX-005: await/spawn inside a transaction is not supported in v1; "
                    "complete async operations before entering the transaction"
                });
            }

            // (7) tx under @parallel (§7c.6)
            if (flags.under_parallel_attr) {
                diags.push_back({
                    tx_diagnostic_kind::tx_under_parallel, tx_span,
                    "CRANK-TX-006: transaction nested under @parallel is not supported in v1; "
                    "transaction blocks must run sequentially"
                });
            }

            // (8) Non-Result fn without failure policy (§7c.3)
            //   If fn doesn't return Result<T,E> and has no @on_safety_failure, it's a diagnostic.
            if (!flags.fn_returns_result && !flags.fn_has_safety_attr) {
                diags.push_back({
                    tx_diagnostic_kind::non_result_no_policy, tx_span,
                    "CRANK-TX-007: function containing a transaction does not return Result<T,E> "
                    "and has no @on_safety_failure attribute; "
                    "add @on_safety_failure(trap|terminate|host_handler) or change return type to Result"
                });
            }

            // (9) Nested same-thread tx — flatten note (not an error)
            if (flags.has_nested_tx) {
                diags.push_back({
                    tx_diagnostic_kind::nested_tx_flatten_note, tx_span,
                    "CRANK-TX-NOTE-001: nested same-thread transaction block flattened "
                    "(inherits parent retry/isolation options)",
                    false // not an error
                });
            }

            return diags;
        }
    };

    // ============================================================================
    // tx_lowering_result — output of transaction block lowering
    // ============================================================================

    struct tx_lowering_result {
        std::vector<transaction_read_op> reads;
        std::vector<transaction_write_op> writes;
        CrankTransactionOptions options;
        std::vector<tx_compile_diagnostic> diagnostics;
        bool nested_flattened = false; // nested tx merged into parent

        [[nodiscard]] bool ok() const noexcept {
            for (const auto& d : diagnostics) if (d.is_error) return false;
            return true;
        }
    };

    // ============================================================================
    // lower_transaction — lower an AST transaction block to tx_lowering_result
    //
    // In the real compiler this walks the typed AST. Here we provide the
    // interface + policy-check integration. AST-walk is plugged in by the
    // semantic analysis pass; this function owns only the policy checking.
    // ============================================================================

    [[nodiscard]] inline tx_lowering_result
    lower_transaction(const tx_policy_flags& flags, source_span tx_span) {
        tx_lowering_result res;
        res.options = flags.options;
        res.nested_flattened = flags.has_nested_tx;

        tx_policy_checker checker;
        res.diagnostics = checker.check(flags, tx_span);

        return res;
    }

    // ============================================================================
    // resource_capability_check_result — per-resource capability validation (§6.3)
    // ============================================================================

    struct resource_capability_check_result {
        std::vector<tx_compile_diagnostic> diagnostics;

        [[nodiscard]] bool ok() const noexcept {
            for (const auto& d : diagnostics) if (d.is_error) return false;
            return true;
        }
    };

    // resource_capability_checker — validates a resource's capabilities against
    // the operations requested in a transaction (§6.3 capability-check algorithm).
    //
    // Usage:
    //   resource_capability_checker<MyResource> checker;
    //   auto result = checker.check(operations, options, tx_span);
    //
    // Steps (§6.3):
    //   1. Require transactional for every write.
    //   2. Require staging or rollback for writes.
    //   3. Require snapshot for snapshot isolation or old().
    //   4. Require range-read support for range access.
    //   5. Require predicate validation for serializable range predicates.
    //   6. Require prepare/recovery for coordinated commits.
    //   7. Require savepoint support for nested/savepoint use.
    //   All errors produce stable CRANK-TX-* diagnostics.

    struct resource_capability_spec {
        bool has_write = false;
        bool has_range_read = false;
        bool has_serializable_range = false;
        bool has_old_expr = false;
        bool has_savepoint = false;
        bool needs_coordinator = false;

        // Per-resource traits (from medha::resource_traits<R>)
        bool transactional = false;
        bool resource_stages_values = false;
        bool supports_rollback_trait = false;
        bool supports_snapshot_trait = false;
        bool supports_range_reads_trait = false;
        bool supports_predicate_validation_trait = false;
        bool supports_savepoints_trait = false;
        bool supports_prepare_trait = false;
        bool supports_recovery_trait = false;
    };

    class resource_capability_checker {
    public:
        [[nodiscard]] resource_capability_check_result
        check(const resource_capability_spec& spec, source_span tx_span) const {
            resource_capability_check_result res;
            auto& diags = res.diagnostics;

            // Step 1+2: writes require transactional + staging or rollback
            if (spec.has_write) {
                if (!spec.transactional) {
                    diags.push_back({
                        tx_diagnostic_kind::non_transactional_write, tx_span,
                        "CRANK-TX-001: write to non-transactional resource inside transaction"
                    });
                }
                else if (!spec.resource_stages_values && !spec.supports_rollback_trait) {
                    diags.push_back({
                        tx_diagnostic_kind::non_transactional_write, tx_span,
                        "CRANK-TX-001: transactional resource must support staging or rollback for writes"
                    });
                }
            }

            // Step 3: snapshot for old() or snapshot isolation
            if (spec.has_old_expr && !spec.supports_snapshot_trait) {
                diags.push_back({
                    tx_diagnostic_kind::old_needs_snapshot, tx_span,
                    "CRANK-TX-003: old(resource[key]) requires resource supports_snapshot=true"
                });
            }

            // Step 4: range-read support
            if (spec.has_range_read && !spec.supports_range_reads_trait) {
                diags.push_back({
                    tx_diagnostic_kind::non_transactional_write, tx_span,
                    "CRANK-TX-CAP-001: range read requires resource supports_range_reads=true"
                });
            }

            // Step 5: predicate validation for serializable range queries
            if (spec.has_serializable_range && !spec.supports_predicate_validation_trait) {
                diags.push_back({
                    tx_diagnostic_kind::cross_resource_serializable, tx_span,
                    "CRANK-TX-CAP-002: serializable range query requires resource supports_predicate_validation=true"
                });
            }

            // Step 6: prepare/recovery for coordinated commits
            if (spec.needs_coordinator) {
                if (!spec.supports_prepare_trait) {
                    diags.push_back({
                        tx_diagnostic_kind::coordinator_resource_not_transactional, tx_span,
                        "CRANK-TX-010: coordinated commit requires resource supports_prepare=true"
                    });
                }
            }

            // Step 7: savepoints
            if (spec.has_savepoint && !spec.supports_savepoints_trait) {
                // Note only — savepoints can be emulated by the crank journal
                diags.push_back({
                    tx_diagnostic_kind::nested_tx_flatten_note, tx_span,
                    "CRANK-TX-NOTE-002: resource does not declare supports_savepoints; "
                    "crank journal emulates partial rollback",
                    false
                });
            }

            return res;
        }
    };

    // Convenience: build a resource_capability_spec from medha::resource_traits<R> fields.
    // Uses if constexpr to handle specializations that predate the capability extensions.
    template <class R>
    [[nodiscard]] constexpr resource_capability_spec
    make_resource_capability_spec() noexcept {
        using RT = medha::resource_traits<R>;
        resource_capability_spec s;
        s.transactional = RT::transactional;
        s.resource_stages_values = RT::resource_stages_values;
        s.supports_rollback_trait = RT::supports_rollback;
        s.supports_snapshot_trait = RT::supports_snapshot;

        if constexpr (requires { RT::supports_range_reads; })
            s.supports_range_reads_trait = RT::supports_range_reads;

        if constexpr (requires { RT::supports_predicate_validation; })
            s.supports_predicate_validation_trait = RT::supports_predicate_validation;

        if constexpr (requires { RT::supports_savepoints; })
            s.supports_savepoints_trait = RT::supports_savepoints;

        if constexpr (requires { RT::supports_prepare; })
            s.supports_prepare_trait = RT::supports_prepare;

        if constexpr (requires { RT::supports_recovery; })
            s.supports_recovery_trait = RT::supports_recovery;

        return s;
    }

    // ============================================================================
    // transaction_event_kind — observability events emitted by the tx runtime (§16.1)
    //
    // Events carry no resource values by default (pay-for-use, §16.2 invariant).
    // ============================================================================

    enum class transaction_event_kind : std::uint8_t {
        started,
        read,
        write_staged,
        validation_started,
        conflict_detected,
        retry_scheduled,
        prepare_started,
        participant_prepared,
        commit_decided,
        participant_committed,
        committed,
        rollback_started,
        rolled_back,
        cancelled,
        deadline_exceeded,
        in_doubt,
        recovered,
    };

    [[nodiscard]] constexpr std::string_view to_string(transaction_event_kind k) noexcept {
        switch (k) {
        case transaction_event_kind::started: return "started";
        case transaction_event_kind::read: return "read";
        case transaction_event_kind::write_staged: return "write_staged";
        case transaction_event_kind::validation_started: return "validation_started";
        case transaction_event_kind::conflict_detected: return "conflict_detected";
        case transaction_event_kind::retry_scheduled: return "retry_scheduled";
        case transaction_event_kind::prepare_started: return "prepare_started";
        case transaction_event_kind::participant_prepared: return "participant_prepared";
        case transaction_event_kind::commit_decided: return "commit_decided";
        case transaction_event_kind::participant_committed: return "participant_committed";
        case transaction_event_kind::committed: return "committed";
        case transaction_event_kind::rollback_started: return "rollback_started";
        case transaction_event_kind::rolled_back: return "rolled_back";
        case transaction_event_kind::cancelled: return "cancelled";
        case transaction_event_kind::deadline_exceeded: return "deadline_exceeded";
        case transaction_event_kind::in_doubt: return "in_doubt";
        case transaction_event_kind::recovered: return "recovered";
        }
        return "unknown";
    }

    // transaction_event — payload for a single observability event (§16.1).
    // Carries no resource values; resource/key fields are optional identifiers only.
    struct transaction_event {
        transaction_event_kind kind;
        std::uint64_t transaction_id = 0;
        std::uint32_t attempt = 0;
        transaction_state state = transaction_state::active;
        std::string resource; // resource identity (name, not value)
        std::string error_category; // TxErrorKind name if error, else empty
        std::uint64_t duration_ns = 0;
        std::uint64_t trace_id = 0;
    };

    // ============================================================================
    // log_record_kind — WAL record types for coordinator durability (§14.2)
    // ============================================================================

    enum class log_record_kind : std::uint8_t {
        begin,
        participant_prepared,
        commit_decision,
        abort_decision,
        participant_committed,
        participant_aborted,
        complete,
    };

    [[nodiscard]] constexpr std::string_view to_string(log_record_kind k) noexcept {
        switch (k) {
        case log_record_kind::begin: return "begin";
        case log_record_kind::participant_prepared: return "participant_prepared";
        case log_record_kind::commit_decision: return "commit_decision";
        case log_record_kind::abort_decision: return "abort_decision";
        case log_record_kind::participant_committed: return "participant_committed";
        case log_record_kind::participant_aborted: return "participant_aborted";
        case log_record_kind::complete: return "complete";
        }
        return "unknown";
    }

    // ============================================================================
    // tx_plan_record — snapshot for JSON dump (dump.hpp Module 5 dump_tx_plan)
    // ============================================================================

    struct tx_plan_record {
        // options snapshot
        std::string isolation;
        std::string replay;
        std::string conflict;
        std::uint32_t retry = 0;
        std::string partial_commit;
        std::string durability; // §14.1: requested durability level ("memory"/"process"/"durable")
        bool distribution_none = true;
        std::string coordinator; // §v2.11: empty = none

        // resource participation
        std::uint32_t transactional_resource_count = 0;
        std::uint32_t non_transactional_write_count = 0;
        bool supports_snapshot = false;

        // read/write set summary
        std::uint32_t read_count = 0;
        std::uint32_t write_count = 0;

        // metadata
        std::uint64_t resource_traits_hash = 0;
        std::uint64_t medha_dialect_version = 1;

        [[nodiscard]] static tx_plan_record
        from(const tx_lowering_result& res, const tx_policy_flags& flags) {
            tx_plan_record r;

            switch (flags.options.isolation) {
            case CrankIsolation::read_committed:
                r.isolation = "read_committed";
                break;
            case CrankIsolation::snapshot:
                r.isolation = "snapshot";
                break;
            case CrankIsolation::serializable:
                r.isolation = "serializable";
                break;
            }

            switch (flags.options.replay) {
            case CrankReplaySafety::unknown:
                r.replay = "unknown";
                break;
            case CrankReplaySafety::non_idempotent:
                r.replay = "non_idempotent";
                break;
            case CrankReplaySafety::body_idempotent:
                r.replay = "body_idempotent";
                break;
            case CrankReplaySafety::body_and_effects_idempotent:
                r.replay = "body_and_effects_idempotent";
                break;
            case CrankReplaySafety::unknown_but_retry_allowed:
                r.replay = "unknown_but_retry_allowed";
                break;
            }

            switch (flags.options.conflict) {
            case CrankConflictPolicy::optimistic:
                r.conflict = "optimistic";
                break;
            case CrankConflictPolicy::pessimistic:
                r.conflict = "pessimistic";
                break;
            case CrankConflictPolicy::deterministic:
                r.conflict = "deterministic";
                break;
            }

            r.retry = flags.options.retry;
            r.coordinator = flags.options.coordinator; // §v2.11
            r.distribution_none = !flags.options.has_coordinator(); // coordinated ⇒ not none
            r.durability = std::string(to_string(flags.options.durability)); // §14.1

            switch (flags.options.partial) {
            case CrankPartialCommit::require_atomic_coordinator:
                r.partial_commit = "require_atomic_coordinator";
                break;
            case CrankPartialCommit::best_effort:
                r.partial_commit = "best_effort";
                break;
            }

            r.transactional_resource_count = flags.transactional_resource_count;
            r.non_transactional_write_count = flags.non_transactional_write_count;
            r.supports_snapshot = flags.any_resource_supports_snapshot;
            r.resource_traits_hash = flags.resource_traits_hash;
            r.medha_dialect_version = flags.medha_dialect_version;
            r.read_count = static_cast<std::uint32_t>(res.reads.size());
            r.write_count = static_cast<std::uint32_t>(res.writes.size());
            return r;
        }
    };

    // ============================================================================
    // transactional_resource_registration — compile-time metadata record (§9.5)
    //
    // crank::register_transactional<R>("Name") lives in host.hpp (extended there).
    // This header contributes the trait query + AOT key extension logic.
    // ============================================================================

    // resource_traits_hash<R> — FNV-1a fingerprint of the traits that affect
    // Medha commit semantics, fed into crank_aot_key::descriptor_hashes (§7c.7).
    //
    // Fields hashed: transactional, supports_snapshot, commit_protocol, aba_safe.
    // Any change invalidates the AOT artifact.

    template <class R>
    [[nodiscard]] constexpr std::uint64_t resource_traits_hash() noexcept {
        using RT = medha::resource_traits<R>;
        // Stable bit layout: [transactional:1][supports_snapshot:1][aba_safe:1][commit_protocol:8]
        std::uint64_t bits = 0;
        bits |= (static_cast<std::uint64_t>(RT::transactional) << 0);
        bits |= (static_cast<std::uint64_t>(RT::supports_snapshot) << 1);
        bits |= (static_cast<std::uint64_t>(RT::aba_safe) << 2);
        bits |= (static_cast<std::uint64_t>(
            static_cast<std::uint8_t>(RT::commit_protocol)) << 8);

        // FNV-1a fold
        constexpr std::uint64_t kOffset = 14695981039346656037ULL;
        constexpr std::uint64_t kPrime = 1099511628211ULL;
        std::uint64_t h = kOffset;
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<std::uint64_t>((bits >> (i * 8)) & 0xFFu);
            h *= kPrime;
        }
        return h;
    }

    // extend_aot_key_with_resource<R> — fold a resource's traits hash into an
    // existing crank_aot_key::descriptor_hashes (§7c.7).
    template <class R>
    void extend_aot_key_with_resource(crank_aot_key& key) {
        key.descriptor_hashes.push_back(resource_traits_hash<R>());
        // Stamp distribution=none (v1). A new medha dialect version forces recompile.
        // The dialect version is embedded in enabled_features bits [63:48].
        constexpr std::uint64_t kDialectShift = 48u;
        constexpr std::uint64_t kDialectV1 = 1ULL;
        key.enabled_features = (key.enabled_features & ~(0xFFFFULL << kDialectShift))
            | (kDialectV1 << kDialectShift);
    }

    // ============================================================================
    // transaction_participant concept — §13.2
    //
    // A transaction participant is any type that provides idempotent prepare/commit/
    // rollback operations, and can be enrolled by a coordinator. Crank defines the
    // concept; implementations are provided by resource adapters.
    //
    // Required operations:
    //   p.id()                                       → participant_id (uint64_t)
    //   p.prepare(tx_id, write_count)                → bool (true = vote prepared)
    //   p.commit(tx_id)                              → bool (idempotent)
    //   p.rollback(tx_id)                            → bool (idempotent)
    //
    // Properties enforced by caller (not verifiable in concept alone):
    //   - prepare/commit/rollback are idempotent.
    //   - A prepared participant preserves state to commit after restart.
    //   - A committed participant never later reports aborted.
    // ============================================================================

    template <class P>
    concept TransactionParticipant = requires(P& p,
                                              std::uint64_t tx_id,
                                              std::uint32_t write_count) {
        { p.id() } -> std::convertible_to<std::uint64_t>;
        { p.prepare(tx_id, write_count) } -> std::convertible_to<bool>;
        { p.commit(tx_id) } -> std::convertible_to<bool>;
        { p.rollback(tx_id) } -> std::convertible_to<bool>;
    };

    // ============================================================================
    // crank_2pc_result — outcome of a two-phase commit coordination round (§13.3)
    // ============================================================================

    enum class crank_2pc_outcome : std::uint8_t {
        committed, // all participants committed
        aborted, // one or more participants aborted (clean rollback completed)
        in_doubt, // commit decision durable but at least one participant uncertain
    };

    struct crank_2pc_result {
        crank_2pc_outcome outcome = crank_2pc_outcome::aborted;
        std::uint32_t participants_prepared = 0;
        std::uint32_t participants_committed = 0;
        std::uint32_t participants_aborted = 0;

        [[nodiscard]] bool committed() const noexcept {
            return outcome == crank_2pc_outcome::committed;
        }
    };

    // ============================================================================
    // crank_coordinator_2pc — two-phase commit coordinator (§13.3–§13.4)
    //
    // Implements the §13.4 coordinator algorithm for in-process single-trust-domain
    // use. Distributed consensus is a non-goal (§20). Participants must satisfy
    // TransactionParticipant. Participants are sorted by stable id before prepare
    // to reduce deadlocks (§9.2 canonical order).
    //
    // Usage:
    //   crank_coordinator_2pc coord;
    //   coord.enlist(p1).enlist(p2);
    //   auto result = coord.coordinate(tx_id, write_count_per_participant);
    // ============================================================================

    class crank_coordinator_2pc {
    public:
        struct participant_record {
            std::uint64_t id;
            std::function<bool(std::uint64_t, std::uint32_t)> prepare_fn;
            std::function<bool(std::uint64_t)> commit_fn;
            std::function<bool(std::uint64_t)> rollback_fn;
        };

        template <TransactionParticipant P>
        crank_coordinator_2pc& enlist(P& p) {
            participant_record r;
            r.id = p.id();
            r.prepare_fn = [&p](std::uint64_t tx_id, std::uint32_t wc) { return p.prepare(tx_id, wc); };
            r.commit_fn = [&p](std::uint64_t tx_id) { return p.commit(tx_id); };
            r.rollback_fn = [&p](std::uint64_t tx_id) { return p.rollback(tx_id); };
            participants_.push_back(std::move(r));
            return *this;
        }

        // Execute the §13.4 coordinate algorithm.
        [[nodiscard]] crank_2pc_result coordinate(std::uint64_t tx_id,
                                                  std::uint32_t write_count = 0) {
            // Sort by stable id (canonical order, §9.2) to reduce deadlocks.
            std::ranges::sort(participants_, {}, &participant_record::id);

            crank_2pc_result result;

            // Phase 1: Prepare
            std::vector<std::size_t> prepared_indices;
            for (std::size_t i = 0; i < participants_.size(); ++i) {
                const bool voted = participants_[i].prepare_fn(tx_id, write_count);
                if (voted) {
                    prepared_indices.push_back(i);
                    ++result.participants_prepared;
                }
                else {
                    // Abort: roll back all prepared participants.
                    for (std::size_t j : prepared_indices) {
                        participants_[j].rollback_fn(tx_id);
                        ++result.participants_aborted;
                    }
                    result.outcome = crank_2pc_outcome::aborted;
                    return result;
                }
            }

            // Phase 2: Commit (decision is durable from here; cancellation cannot reverse it).
            bool all_committed = true;
            for (auto& pr : participants_) {
                const bool ok = pr.commit_fn(tx_id);
                if (ok) ++result.participants_committed;
                else all_committed = false;
            }

            result.outcome = all_committed
                                 ? crank_2pc_outcome::committed
                                 : crank_2pc_outcome::in_doubt;
            return result;
        }

        [[nodiscard]] std::size_t participant_count() const noexcept {
            return participants_.size();
        }

    private:
        std::vector<participant_record> participants_;
    };

    // ============================================================================
    // FromTxError — §5.2 error conversion trait concept.
    //
    // A function returning Result[T, E] may use transaction failure propagation only
    // when E satisfies FromTxError. No implicit conversion is attempted for unrelated
    // error types. Implementations provide `from_tx_error(CrankTxError) -> E`.
    // ============================================================================

    template <class E>
    concept FromTxError = requires(const CrankTxError& e) {
        { E::from_tx_error(e) } -> std::convertible_to<E>;
    };

    // Convenience: CrankTxError itself satisfies FromTxError (identity conversion).
    // Other error types can specialize by adding a static from_tx_error member.

    // ============================================================================
    // tx_option — key/value pair from a parsed transaction_arg (§6.2 grammar).
    //
    // Used by tx_options_from_ast() below. Matches the `tx_option_node` shape in
    // build_ast.hpp without introducing a dependency on that header.
    // ============================================================================

    struct tx_option {
        std::string key;
        std::string value;
    };

    // tx_options_from_ast — map a list of tx_option key/value string pairs to a
    // CrankTransactionOptions aggregate. Unrecognised keys are silently ignored.
    //
    // Grammar → CrankTransactionOptions mapping:
    //   isolation  = read_committed | snapshot | serializable
    //   replay     = unknown | non_idempotent | body_idempotent |
    //                body_and_effects_idempotent | unknown_but_retry_allowed
    //   conflict   = optimistic | pessimistic | deterministic
    //   partial    = require_atomic_coordinator | best_effort | allow_in_doubt
    //   durability = memory | process | durable        (§14.1)
    //   retry      = INT_LIT
    //   coordinator= STRING_LIT
    //
    // This function is the canonical sema-side bridge between the AST (string
    // key/value from tx_option_node) and the typed CrankTransactionOptions.
    [[nodiscard]] inline CrankTransactionOptions
    tx_options_from_ast(const std::vector<tx_option>& opts) {
        CrankTransactionOptions out; // defaults: snapshot / optimistic / memory / retry=0 / ...
        for (const auto& o : opts) {
            if (o.key == "isolation") {
                if (o.value == "read_committed") out.isolation = CrankIsolation::read_committed;
                else if (o.value == "snapshot") out.isolation = CrankIsolation::snapshot;
                else if (o.value == "serializable") out.isolation = CrankIsolation::serializable;
            }
            else if (o.key == "replay") {
                if (o.value == "unknown") out.replay = CrankReplaySafety::unknown;
                else if (o.value == "non_idempotent") out.replay = CrankReplaySafety::non_idempotent;
                else if (o.value == "body_idempotent") out.replay = CrankReplaySafety::body_idempotent;
                else if (o.value == "body_and_effects_idempotent") out.replay =
                    CrankReplaySafety::body_and_effects_idempotent;
                else if (o.value == "unknown_but_retry_allowed") out.replay =
                    CrankReplaySafety::unknown_but_retry_allowed;
            }
            else if (o.key == "conflict") {
                if (o.value == "optimistic") out.conflict = CrankConflictPolicy::optimistic;
                else if (o.value == "pessimistic") out.conflict = CrankConflictPolicy::pessimistic;
                else if (o.value == "deterministic") out.conflict = CrankConflictPolicy::deterministic;
            }
            else if (o.key == "partial") {
                if (o.value == "require_atomic_coordinator") out.partial =
                    CrankPartialCommit::require_atomic_coordinator;
                else if (o.value == "best_effort") out.partial = CrankPartialCommit::best_effort;
                    // allow_in_doubt is a grammar alias; map to best_effort for now
                else if (o.value == "allow_in_doubt") out.partial = CrankPartialCommit::best_effort;
            }
            else if (o.key == "durability") { // §14.1
                if (o.value == "memory") out.durability = durability_level::memory;
                else if (o.value == "process") out.durability = durability_level::process;
                else if (o.value == "durable") out.durability = durability_level::durable;
            }
            else if (o.key == "retry") {
                // Value is a decimal integer string from INT_LIT
                try { out.retry = static_cast<std::uint32_t>(std::stoul(o.value)); }
                catch (...) { /* malformed literal — keep default */ }
            }
            else if (o.key == "coordinator") {
                // Strip surrounding quotes if present (string_lit from parser includes them)
                auto v = o.value;
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                    v = v.substr(1, v.size() - 2);
                out.coordinator = std::move(v);
            }
            // distribution is a hint; not mapped into CrankTransactionOptions (no field for it)
        }
        return out;
    }

    // ============================================================================
    // commit_report_store — §15.3 durable report lookup.
    //
    // Stores committed CrankCommitReports keyed by transaction_id so a client that
    // loses the response after commit can query whether the operation succeeded.
    //
    // Thread-safety: not provided — callers must synchronize externally.
    // The lookup is idempotent (querying twice for the same id is safe).
    //
    // Usage:
    //   commit_report_store store;
    //   store.record(report);                         // called by execute_transaction
    //   auto r = lookup_commit_report(store, tx_id);  // query by id later
    // ============================================================================

    class commit_report_store {
    public:
        // Record a committed report. Silently ignores reports that are not
        // committed (status != committed) — only committed reports are queryable.
        void record(const CrankCommitReport& r) {
            if (r.is_committed()) {
                reports_[r.transaction_id] = r;
            }
        }

        // Lookup a previously recorded committed report by transaction_id.
        // Returns unexpected(TxErrorKind::InternalInvariant) if not found.
        [[nodiscard]] std::expected<CrankCommitReport, CrankTxError>
        lookup(std::uint64_t tx_id) const {
            auto it = reports_.find(tx_id);
            if (it == reports_.end()) {
                CrankTxError e;
                e.kind = TxErrorKind::InternalInvariant;
                e.inner = medha::tx_error{
                    medha::tx_status::internal_error,
                    "no committed report for transaction_id"
                };
                e.retryable = false;
                return std::unexpected(e);
            }
            return it->second;
        }

        [[nodiscard]] std::size_t size() const noexcept { return reports_.size(); }

    private:
        std::unordered_map<std::uint64_t, CrankCommitReport> reports_;
    };

    // Free-function convenience wrapper matching the §15.3 API shape.
    [[nodiscard]] inline std::expected<CrankCommitReport, CrankTxError>
    lookup_commit_report(const commit_report_store& store, std::uint64_t tx_id) {
        return store.lookup(tx_id);
    }
} // namespace crank
