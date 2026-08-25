#pragma once

// crank/coroutine.hpp — C++20 coroutine execution backend (design §14).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// A crank_task<T> is a lazily-started coroutine carrying a typed execution
// result. It integrates the cancellation/deadline model (cancellation.hpp) and
// the typed error contract (exec_result.hpp): a task finishes in exactly one
// terminal state — completed / failed / cancelled / timed_out (design §3.1).
//
// Suspension is scheduler-driven: awaiting a task registers the awaiter's
// continuation and suspends; the scheduler resumes it later. Nothing is resumed
// inline while a lock is held (design §14.3) — resumption is always handed to a
// Scheduler. Pravaha thread pools wrap via a thin adapter matching the Scheduler
// concept; without pravaha an inline_scheduler runs the handle immediately (still
// off any lock, from the awaiting frame).

#include "languages/crank/cancellation.hpp"
#include "languages/crank/exec_result.hpp"

#include <coroutine>
#include <cstdint>
#include <optional>
#include <utility>

namespace crank {
    // ============================================================================
    // Scheduler — where a ready coroutine handle is resumed (design §14.3)
    // ============================================================================

    template <class S>
    concept Scheduler = requires(S s, std::coroutine_handle<> h) {
        { s.schedule(h) };
    };

    // Default scheduler: resume on the calling frame. Legal because it never runs
    // under a crank-held lock — the awaiter hands the handle here after suspending.
    struct inline_scheduler {
        void schedule(std::coroutine_handle<> h) const {
            if (h && !h.done()) h.resume();
        }
    };

    // ============================================================================
    // crank_task<T> — a typed, cancellable coroutine result (design §14.1)
    // ============================================================================

    template <class T>
    class crank_task {
    public:
        struct promise_type {
            std::optional<T> result;
            std::optional<execution_error> err;
            std::coroutine_handle<> continuation; // awaiter to resume on finish
            cancellation_token token;
            std::optional<deadline_point> deadline;
            std::uint64_t scope_id = 0;
            std::uint64_t profiling_id = 0;

            [[nodiscard]] crank_task get_return_object() noexcept {
                return crank_task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // Lazy start: the coroutine suspends at its initial point so the caller
            // controls when (and on which scheduler) it runs.
            [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }

            // Final suspend hands control back to a registered continuation (the
            // awaiting coroutine), scheduler-resumed — never resumed under a lock.
            struct final_awaiter {
                [[nodiscard]] bool await_ready() const noexcept { return false; }

                [[nodiscard]] std::coroutine_handle<>
                await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    auto cont = h.promise().continuation;
                    return cont ? cont : std::noop_coroutine();
                }

                void await_resume() const noexcept {}
            };

            [[nodiscard]] final_awaiter final_suspend() noexcept { return {}; }

            void return_value(T v) { result = std::move(v); }

            // An escaped exception becomes a typed task_panicked error (design §14.4).
            void unhandled_exception() {
                err = make_error(execution_error_kind::task_panicked,
                                 "coroutine task threw an exception");
            }
        };

        using handle_type = std::coroutine_handle<promise_type>;

        crank_task() = default;
        explicit crank_task(handle_type h) noexcept : handle_(h) {}

        crank_task(const crank_task&) = delete;
        crank_task& operator=(const crank_task&) = delete;
        crank_task(crank_task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}

        crank_task& operator=(crank_task&& o) noexcept {
            if (this != &o) {
                destroy();
                handle_ = std::exchange(o.handle_, {});
            }
            return *this;
        }

        ~crank_task() { destroy(); }

        // Set the cancellation token / deadline before the task is scheduled.
        void bind(cancellation_token token,
                  std::optional<deadline_point> deadline = std::nullopt) {
            if (handle_) {
                handle_.promise().token = std::move(token);
                handle_.promise().deadline = deadline;
            }
        }

        // Cooperatively request cancellation; the running body observes it at its
        // next check_interruption poll.
        void request_cancel() noexcept {
            if (handle_) handle_.promise().token.request(cancellation_reason::requested);
        }

        [[nodiscard]] bool ready() const noexcept { return handle_ && handle_.done(); }

        // Run to completion on a scheduler, then read the typed result. For the
        // lazy-start model, this schedules the initial resume.
        template <class Sched = inline_scheduler>
            requires Scheduler<Sched>
        [[nodiscard]] execution_result<T> run(Sched sched = {}) {
            if (!handle_)
                return make_failed<T>(make_error(execution_error_kind::task_panicked,
                                                 "run() on empty task"));
            // Pre-flight interruption: cancelled/expired before first resume.
            if (auto intr = check_interruption(handle_.promise().token,
                                               handle_.promise().deadline))
                return interrupt_result<T>(*intr);
            sched.schedule(handle_);
            return result();
        }

        // Extract the terminal typed result once the coroutine has finished.
        [[nodiscard]] execution_result<T> result() {
            if (!handle_ || !handle_.done())
                return make_failed<T>(make_error(execution_error_kind::task_panicked,
                                                 "result() before task completed"));
            auto& p = handle_.promise();
            // Cancellation/deadline wins over a produced value (design §13.3).
            if (auto intr = check_interruption(p.token, p.deadline))
                return interrupt_result<T>(*intr);
            if (p.err) return make_failed<T>(*p.err);
            if (p.result) return make_completed<T>(std::move(*p.result));
            return make_failed<T>(make_error(execution_error_kind::missing_return_value,
                                             "coroutine produced no value"));
        }

        [[nodiscard]] handle_type handle() const noexcept { return handle_; }

        // ── awaiter: co_await task suspends the current coro, resumes it when the
        //    awaited task finishes (scheduler-driven, never inline under a lock).
        struct awaiter {
            handle_type awaited;

            [[nodiscard]] bool await_ready() const noexcept {
                return !awaited || awaited.done();
            }

            [[nodiscard]] std::coroutine_handle<>
            await_suspend(std::coroutine_handle<> cont) noexcept {
                awaited.promise().continuation = cont;
                return awaited; // symmetric transfer to the awaited task
            }

            [[nodiscard]] execution_result<T> await_resume() {
                crank_task tmp{awaited};
                auto r = tmp.result();
                (void)tmp.release(); // do not destroy the awaited frame here
                return r;
            }
        };

        [[nodiscard]] awaiter operator co_await() && noexcept { return awaiter{handle_}; }

        // Relinquish ownership without destroying (used by awaiter).
        handle_type release() noexcept { return std::exchange(handle_, {}); }

    private:
        void destroy() noexcept { if (handle_) handle_.destroy(); }
        handle_type handle_{};
    };
} // namespace crank
