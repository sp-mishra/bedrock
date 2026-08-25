#pragma once

// crank/cancellation.hpp — Hierarchical cancellation + deadlines + task FSM (design §13).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// A cancellation_token observes a shared cancellation_state. Tokens form a tree:
// a child observes its parent (parent cancellation propagates down), but a child
// cancelling does NOT affect its parent (design §13.2: "cancellation flows from
// parent to child, never the reverse"). Deadlines compose by taking the minimum
// (the tightest deadline wins). The task_status FSM tracks a single task's
// lifecycle with a validated transition table shared by coroutine.hpp and
// task_scope.hpp.
//
// check_interruption is the single query a running task polls: it reports
// cancellation OR deadline expiry as a typed interruption, so callers map one
// result onto execution_status::cancelled / timed_out (design §3.1).

#include "languages/crank/exec_result.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace crank {
    // ============================================================================
    // cancellation_reason — why a token entered the cancelled state (design §13.1)
    // ============================================================================

    enum class cancellation_reason : std::uint8_t {
        none, // not cancelled
        requested, // explicit request on this token
        parent_cancelled, // propagated from an ancestor
        deadline, // deadline expired
    };

    [[nodiscard]] constexpr std::string_view to_string(cancellation_reason r) noexcept {
        switch (r) {
        case cancellation_reason::none: return "none";
        case cancellation_reason::requested: return "requested";
        case cancellation_reason::parent_cancelled: return "parent_cancelled";
        case cancellation_reason::deadline: return "deadline";
        }
        return "unknown";
    }

    // ============================================================================
    // cancellation_state — the shared flag a token observes
    //
    // One state per node; children hold a shared_ptr to their own state AND a
    // shared_ptr to the parent token so parent cancellation is visible without a
    // callback registry (poll-based, lock-free).
    // ============================================================================

    struct cancellation_state {
        std::atomic<bool> requested{false};
        std::atomic<cancellation_reason> reason{cancellation_reason::none};
    };

    // ============================================================================
    // cancellation_token — an observer into a cancellation tree (design §13.2)
    // ============================================================================

    class cancellation_token {
    public:
        cancellation_token()
            : state_(std::make_shared<cancellation_state>()) {}

        // Spawn a child that also observes this token. Parent-not-affected-by-child
        // is structural: the child owns a fresh state and a back-pointer to the
        // parent; cancelling the child touches only the child's state.
        [[nodiscard]] cancellation_token child() const {
            cancellation_token c;
            c.parent_ = std::make_shared<cancellation_token>(*this);
            return c;
        }

        // Request cancellation on THIS token only. Idempotent; first reason wins.
        void request(cancellation_reason r = cancellation_reason::requested) noexcept {
            bool expected = false;
            if (state_->requested.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
                state_->reason.store(r, std::memory_order_release);
            }
        }

        // Cancelled if this token was requested, or any ancestor is cancelled.
        [[nodiscard]] bool is_cancelled() const noexcept {
            if (state_->requested.load(std::memory_order_acquire)) return true;
            return parent_ && parent_->is_cancelled();
        }

        // The effective reason: this token's own reason if set, else the ancestor's
        // reason surfaced as parent_cancelled.
        [[nodiscard]] cancellation_reason reason() const noexcept {
            if (state_->requested.load(std::memory_order_acquire))
                return state_->reason.load(std::memory_order_acquire);
            if (parent_ && parent_->is_cancelled())
                return cancellation_reason::parent_cancelled;
            return cancellation_reason::none;
        }

    private:
        std::shared_ptr<cancellation_state> state_;
        std::shared_ptr<cancellation_token> parent_; // nullptr at the root
    };

    // ============================================================================
    // interruption — a running task's poll result (design §13.3)
    // ============================================================================

    struct interruption {
        enum class kind_t : std::uint8_t { cancelled, timed_out };

        kind_t kind = kind_t::cancelled;
        cancellation_reason reason = cancellation_reason::none;
    };

    // ============================================================================
    // deadlines
    // ============================================================================

    using deadline_clock = std::chrono::steady_clock;
    using deadline_point = deadline_clock::time_point;

    // The tighter of two deadlines wins (design §13.4). Either may be absent.
    [[nodiscard]] inline std::optional<deadline_point>
    effective_deadline(std::optional<deadline_point> parent,
                       std::optional<deadline_point> local) noexcept {
        if (!parent) return local;
        if (!local) return parent;
        return std::min(*parent, *local);
    }

    // Single poll a running task issues: reports cancellation first (it is the more
    // specific signal), then deadline expiry. nullopt = keep running.
    [[nodiscard]] inline std::optional<interruption>
    check_interruption(const cancellation_token& token,
                       std::optional<deadline_point> deadline = std::nullopt,
                       deadline_point now = deadline_clock::now()) noexcept {
        if (token.is_cancelled())
            return interruption{interruption::kind_t::cancelled, token.reason()};
        if (deadline && now >= *deadline)
            return interruption{interruption::kind_t::timed_out, cancellation_reason::deadline};
        return std::nullopt;
    }

    // Map an interruption onto a failed/cancelled/timed_out execution_result.
    template <class T>
    [[nodiscard]] execution_result<T>
    interrupt_result(const interruption& i, std::string fn_name = {}) {
        if (i.kind == interruption::kind_t::timed_out)
            return make_timed_out<T>(std::move(fn_name));
        return make_cancelled<T>(std::move(fn_name));
    }

    // ============================================================================
    // task_status — single-task lifecycle FSM (design §13.5)
    // ============================================================================

    enum class task_status : std::uint8_t {
        created, // constructed, not yet scheduled
        scheduled, // handed to a scheduler
        running, // executing
        completed, // finished with a value (terminal)
        failed, // finished with an error (terminal)
        cancellation_requested, // cancel seen while running; unwinding
        cancelled, // finished cancelled (terminal)
        timed_out, // finished by deadline (terminal)
    };

    [[nodiscard]] constexpr std::string_view to_string(task_status s) noexcept {
        switch (s) {
        case task_status::created: return "created";
        case task_status::scheduled: return "scheduled";
        case task_status::running: return "running";
        case task_status::completed: return "completed";
        case task_status::failed: return "failed";
        case task_status::cancellation_requested: return "cancellation_requested";
        case task_status::cancelled: return "cancelled";
        case task_status::timed_out: return "timed_out";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr bool is_terminal(task_status s) noexcept {
        return s == task_status::completed || s == task_status::failed ||
            s == task_status::cancelled || s == task_status::timed_out;
    }

    // The legal edges. A task may be cancelled/timed-out from any non-terminal
    // state; otherwise it walks created→scheduled→running→terminal.
    [[nodiscard]] constexpr bool
    valid_transition(task_status from, task_status to) noexcept {
        if (is_terminal(from)) return false; // terminal states are absorbing

        // Cancellation/deadline may strike any live task.
        if (to == task_status::cancellation_requested ||
            to == task_status::cancelled ||
            to == task_status::timed_out) {
            return true;
        }

        switch (from) {
        case task_status::created:
            return to == task_status::scheduled || to == task_status::running;
        case task_status::scheduled:
            return to == task_status::running;
        case task_status::running:
            return to == task_status::completed || to == task_status::failed;
        case task_status::cancellation_requested:
            return to == task_status::cancelled || to == task_status::failed ||
                to == task_status::completed;
        default:
            return false;
        }
    }

    // Atomic FSM cell: rejects illegal transitions, returns whether it moved.
    class task_state {
    public:
        task_state() = default;

        [[nodiscard]] task_status get() const noexcept {
            return status_.load(std::memory_order_acquire);
        }

        // CAS-loop: advance to `to` iff valid_transition from the current state.
        [[nodiscard]] bool transition(task_status to) noexcept {
            task_status cur = status_.load(std::memory_order_acquire);
            while (valid_transition(cur, to)) {
                if (status_.compare_exchange_weak(cur, to,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
                    return true;
            }
            return false;
        }

    private:
        std::atomic<task_status> status_{task_status::created};
    };
} // namespace crank
