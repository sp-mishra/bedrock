#pragma once

// crank/exec_result.hpp — Unified typed execution result (design §3).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Single source of truth for the execution contract. Every execution request
// finishes in exactly one explicit `execution_status` and — for completed
// results — carries a value; for non-completed results, a typed
// `execution_error`. `execution_result<T>` makes impossible states
// unrepresentable via the make_* factories (value present iff completed).
//
// This header is dependency-light on purpose: it is included by execute.hpp,
// verify_mir.hpp, capability.hpp, plan.hpp, cancellation.hpp and coroutine.hpp.
// `profiling_report` lives here (not plan.hpp) because execution_result embeds
// it — keeping it here breaks a would-be circular include.

#include "languages/crank/source_span.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crank {
    // ============================================================================
    // execution_status — the 7 canonical terminal states (design §3.1)
    //
    // Migration note: the 4 legacy enumerators (ok / unsupported_control_flow /
    // lowering_failed / runtime_error) previously lived in execute.hpp. They are
    // retained here as *alias* enumerators sharing the canonical values, so pre-
    // existing callers (e.g. `execution_status::ok`) keep compiling unchanged.
    // Duplicate enumerator VALUES are legal in C++ as long as the NAMES differ.
    // `to_string` switches on the 7 canonical labels only.
    // ============================================================================

    enum class execution_status : std::uint8_t {
        completed = 0, // finished with the promised value
        failed = 1, // finished with a typed error
        cancelled = 2, // cancelled before completion
        timed_out = 3, // deadline exceeded
        unsupported = 4, // opcode/region unsupported by any backend
        backend_unavailable = 5, // no legal backend could be selected
        invalid_plan = 6, // plan construction produced no viable path

        // ── legacy aliases (source-compat; equal to a canonical value above) ──
        ok = 0, // == completed
        unsupported_control_flow = 4, // == unsupported
        lowering_failed = 1, // == failed
        runtime_error = 1, // == failed
    };

    [[nodiscard]] constexpr std::string_view to_string(execution_status s) noexcept {
        switch (s) {
        case execution_status::completed: return "completed";
        case execution_status::failed: return "failed";
        case execution_status::cancelled: return "cancelled";
        case execution_status::timed_out: return "timed_out";
        case execution_status::unsupported: return "unsupported";
        case execution_status::backend_unavailable: return "backend_unavailable";
        case execution_status::invalid_plan: return "invalid_plan";
        }
        return "unknown";
    }

    // ============================================================================
    // execution_error_kind — typed error categories (design §3.2)
    // ============================================================================

    enum class execution_error_kind : std::uint8_t {
        backend_unavailable,
        required_backend_illegal,
        runtime_guard_rejected,
        simd_alias_violation,
        gpu_transfer_failure,
        gpu_sync_failure,
        cancelled,
        deadline_exceeded,
        unsupported_opcode,
        missing_return_value,
        result_type_mismatch,
        unsafe_fallback_after_effects,
        lowering_failed,
        verification_failed,
        capability_mismatch,
        plan_construction_failed,
        task_panicked,
        resource_exhausted,
    };

    [[nodiscard]] constexpr std::string_view to_string(execution_error_kind k) noexcept {
        switch (k) {
        case execution_error_kind::backend_unavailable: return "backend_unavailable";
        case execution_error_kind::required_backend_illegal: return "required_backend_illegal";
        case execution_error_kind::runtime_guard_rejected: return "runtime_guard_rejected";
        case execution_error_kind::simd_alias_violation: return "simd_alias_violation";
        case execution_error_kind::gpu_transfer_failure: return "gpu_transfer_failure";
        case execution_error_kind::gpu_sync_failure: return "gpu_sync_failure";
        case execution_error_kind::cancelled: return "cancelled";
        case execution_error_kind::deadline_exceeded: return "deadline_exceeded";
        case execution_error_kind::unsupported_opcode: return "unsupported_opcode";
        case execution_error_kind::missing_return_value: return "missing_return_value";
        case execution_error_kind::result_type_mismatch: return "result_type_mismatch";
        case execution_error_kind::unsafe_fallback_after_effects: return "unsafe_fallback_after_effects";
        case execution_error_kind::lowering_failed: return "lowering_failed";
        case execution_error_kind::verification_failed: return "verification_failed";
        case execution_error_kind::capability_mismatch: return "capability_mismatch";
        case execution_error_kind::plan_construction_failed: return "plan_construction_failed";
        case execution_error_kind::task_panicked: return "task_panicked";
        case execution_error_kind::resource_exhausted: return "resource_exhausted";
        }
        return "unknown";
    }

    // Stable diagnostic code for a kind (design §15.4). 001/002 are reserved by
    // exec_hint.hpp (hard_requirement_unmet / soft_fallback); these start at 010.
    [[nodiscard]] constexpr std::string_view diag_code(execution_error_kind k) noexcept {
        switch (k) {
        case execution_error_kind::backend_unavailable: return "CRANK-E-EXEC-010";
        case execution_error_kind::required_backend_illegal: return "CRANK-E-EXEC-011";
        case execution_error_kind::runtime_guard_rejected: return "CRANK-E-EXEC-012";
        case execution_error_kind::simd_alias_violation: return "CRANK-E-EXEC-013";
        case execution_error_kind::gpu_transfer_failure: return "CRANK-E-EXEC-014";
        case execution_error_kind::gpu_sync_failure: return "CRANK-E-EXEC-015";
        case execution_error_kind::cancelled: return "CRANK-E-EXEC-016";
        case execution_error_kind::deadline_exceeded: return "CRANK-E-EXEC-017";
        case execution_error_kind::unsupported_opcode: return "CRANK-E-EXEC-018";
        case execution_error_kind::missing_return_value: return "CRANK-E-EXEC-019";
        case execution_error_kind::verification_failed: return "CRANK-E-EXEC-020";
        case execution_error_kind::unsafe_fallback_after_effects: return "CRANK-E-EXEC-021";
        case execution_error_kind::result_type_mismatch: return "CRANK-E-EXEC-022";
        case execution_error_kind::capability_mismatch: return "CRANK-E-EXEC-023";
        case execution_error_kind::task_panicked: return "CRANK-E-EXEC-024";
        case execution_error_kind::plan_construction_failed: return "CRANK-E-EXEC-025";
        case execution_error_kind::lowering_failed: return "CRANK-E-EXEC-026";
        case execution_error_kind::resource_exhausted: return "CRANK-E-EXEC-027";
        }
        return "CRANK-E-EXEC-000";
    }

    // ============================================================================
    // execution_error — typed failure record (design §3.2)
    // ============================================================================

    struct execution_error {
        execution_error_kind kind = execution_error_kind::backend_unavailable;
        std::optional<source_span> span;
        std::string fn_name;
        std::string ir_op; // IR operation or block, when applicable
        std::string backend_id; // selected/attempted backend identity
        std::uint64_t plan_id = 0;
        std::string message; // human-readable explanation
        std::vector<execution_error> nested; // nested backend/host errors

        [[nodiscard]] std::string_view code() const noexcept { return diag_code(kind); }
    };

    [[nodiscard]] inline execution_error
    make_error(execution_error_kind kind, std::string message,
               std::string fn_name = {}, std::string backend_id = {}) {
        execution_error e;
        e.kind = kind;
        e.message = std::move(message);
        e.fn_name = std::move(fn_name);
        e.backend_id = std::move(backend_id);
        return e;
    }

    // ============================================================================
    // profiling — pay-for-use event log (design §15.2)
    //
    // When `enabled == false`, record() is a no-op with no allocation, so the hot
    // path pays nothing. Lives here because execution_result embeds it.
    // ============================================================================

    struct profiling_event {
        std::string_view name; // static label, e.g. "kernel_dispatch"
        std::int64_t ns = 0; // duration or timestamp, ns
        std::uint32_t backend_id = 0;
    };

    struct profiling_report {
        bool enabled = false;
        std::vector<profiling_event> events;

        void record(std::string_view name, std::int64_t ns,
                    std::uint32_t backend_id = 0) {
            if (!enabled) return;
            events.push_back(profiling_event{name, ns, backend_id});
        }
    };

    // ============================================================================
    // execution_trace — non-fatal narrative for plan/fallback diagnostics (§15.3)
    // ============================================================================

    struct execution_trace {
        std::vector<std::string> notes;
        std::uint64_t plan_id = 0;
    };

    // ============================================================================
    // execution_result<T> — the unified typed result (design §3.1)
    //
    // Invariant (enforced by the make_* factories):
    //   status == completed  ⇒ value present, error absent
    //   status != completed  ⇒ value absent,  error present
    //                          (cancelled/timed_out still carry a typed error)
    // ============================================================================

    template <class T>
    struct execution_result {
        execution_status status = execution_status::failed;
        std::optional<T> value;
        std::optional<execution_error> error;
        execution_trace trace;
        profiling_report profiling;

        [[nodiscard]] bool completed() const noexcept {
            return status == execution_status::completed;
        }

        [[nodiscard]] bool has_value() const noexcept { return value.has_value(); }

        // Precondition: completed(). Callers should check completed() first.
        [[nodiscard]] const T& unwrap() const & { return *value; }
        [[nodiscard]] T&& unwrap() && { return std::move(*value); }
    };

    // Void / Unit specialization: a completed result still carries an explicit
    // "value" (design §3.1) — modelled here as the completed status with no payload.
    template <>
    struct execution_result<void> {
        execution_status status = execution_status::failed;
        std::optional<execution_error> error;
        execution_trace trace;
        profiling_report profiling;

        [[nodiscard]] bool completed() const noexcept {
            return status == execution_status::completed;
        }
    };

    // ── factories ───────────────────────────────────────────────────────────────

    template <class T>
    [[nodiscard]] execution_result<T> make_completed(T value) {
        execution_result<T> r;
        r.status = execution_status::completed;
        r.value = std::move(value);
        return r;
    }

    [[nodiscard]] inline execution_result<void> make_completed_void() {
        execution_result<void> r;
        r.status = execution_status::completed;
        return r;
    }

    template <class T>
    [[nodiscard]] execution_result<T> make_failed(execution_error error) {
        execution_result<T> r;
        r.status = execution_status::failed;
        r.error = std::move(error);
        return r;
    }

    template <class T>
    [[nodiscard]] execution_result<T> make_cancelled(std::string fn_name = {}) {
        execution_result<T> r;
        r.status = execution_status::cancelled;
        r.error = make_error(execution_error_kind::cancelled, "execution cancelled",
                             std::move(fn_name));
        return r;
    }

    template <class T>
    [[nodiscard]] execution_result<T> make_timed_out(std::string fn_name = {}) {
        execution_result<T> r;
        r.status = execution_status::timed_out;
        r.error = make_error(execution_error_kind::deadline_exceeded, "deadline exceeded",
                             std::move(fn_name));
        return r;
    }

    // Map a non-completed error kind onto the terminal status it implies.
    [[nodiscard]] constexpr execution_status status_for(execution_error_kind k) noexcept {
        switch (k) {
        case execution_error_kind::cancelled: return execution_status::cancelled;
        case execution_error_kind::deadline_exceeded: return execution_status::timed_out;
        case execution_error_kind::backend_unavailable: return execution_status::backend_unavailable;
        case execution_error_kind::unsupported_opcode: return execution_status::unsupported;
        case execution_error_kind::plan_construction_failed:
        case execution_error_kind::capability_mismatch: return execution_status::invalid_plan;
        default: return execution_status::failed;
        }
    }

    // Build a result whose status is derived from the error kind (keeps the
    // status/error pair consistent for cancelled/timed_out/unavailable/etc.).
    template <class T>
    [[nodiscard]] execution_result<T> make_error_result(execution_error error) {
        execution_result<T> r;
        r.status = status_for(error.kind);
        r.error = std::move(error);
        return r;
    }
} // namespace crank
