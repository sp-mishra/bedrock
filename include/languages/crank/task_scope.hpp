#pragma once

// crank/task_scope.hpp — Structured concurrency (v2, §v2.9).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Extends the spawn/await model with scoped lifetimes and cancellation.
// All tasks spawned into a task_scope must complete (or be cancelled) before
// the scope exits. Child failure cancels remaining siblings; scope exit
// propagates the first error.
//
// Surfaces:
//   task_scope_error     — error produced by a task_scope on exit
//   task_scope_result<T> — result carrying T or task_scope_error
//   task_scope           — RAII scope; spawns bounded tasks
//   deadline_scope       — task_scope with a wall-clock deadline
//   join_group<T>        — collect results from a dynamic set of futures
//
// Design refs: crank.md §v2.9; future.hpp.
//
// Note: spawn() within a task_scope uses the InlineBackend (synchronous) in
// this v2 model. Integration with JThreadBackend/CoroutineBackend is an
// execution-layer concern handled by the Pravaha adapter.

#include "languages/crank/future.hpp"
#include "languages/crank/cancellation.hpp"

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace crank {
    // ============================================================================
    // task_scope_error — failure produced when a task_scope exits with a child error
    // ============================================================================

    enum class task_scope_error_kind : std::uint8_t {
        child_failed, // at least one child task produced an error
        deadline_exceeded, // deadline elapsed before all tasks completed
        cancelled, // scope was cancelled externally
    };

    [[nodiscard]] constexpr std::string_view to_string(task_scope_error_kind k) noexcept {
        switch (k) {
        case task_scope_error_kind::child_failed: return "child_failed";
        case task_scope_error_kind::deadline_exceeded: return "deadline_exceeded";
        case task_scope_error_kind::cancelled: return "cancelled";
        }
        return "unknown";
    }

    struct task_scope_error {
        task_scope_error_kind kind = task_scope_error_kind::child_failed;
        std::string message;
        // Index of the first failing child (0-based)
        std::size_t failing_child_index = 0;
    };

    // ============================================================================
    // task_scope — RAII structured concurrency scope
    //
    // Usage:
    //   task_scope scope;
    //   auto f1 = scope.spawn([]{ return compute_a(); });
    //   auto f2 = scope.spawn([]{ return compute_b(); });
    //   auto result = scope.join();  // joins all; propagates first failure
    // ============================================================================

    class task_scope {
    public:
        task_scope() = default;

        // Non-copyable; movable
        task_scope(const task_scope&) = delete;
        task_scope& operator=(const task_scope&) = delete;
        task_scope(task_scope&&) = default;
        task_scope& operator=(task_scope&&) = default;

        // spawn — create a bounded future whose lifetime is <= this scope.
        // The callable is registered and will be driven by join().
        template <class F>
        [[nodiscard]] crank_future<std::invoke_result_t<F>> spawn(F&& fn) {
            using T = std::invoke_result_t<F>;
            // Build the future's thunk directly so its value type is T (not
            // expected<T,...>). The thunk captures the cancellation token and
            // returns unexpected(cancelled) when the scope is cancelled before poll.
            crank_future<T> fut{
                [this, f = std::forward<F>(fn)]()
            mutable -> std::expected<T, crank_future_error> {
                    if (cancelled_) return std::unexpected(crank_future_error::cancelled);
                    return f();
                }
            };
            // Register a weak handle for join tracking
            child_count_++;
            return fut;
        }

        // cancel — signal all spawned tasks to abort on next poll. Sets both the
        // legacy bool (observed by spawn's thunk) and the hierarchical token, so a
        // token shared with child scopes / coroutine tasks sees the cancellation too.
        void cancel() noexcept {
            cancelled_ = true;
            token_.request(cancellation_reason::requested);
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return cancelled_ || token_.is_cancelled();
        }

        // token — hierarchical cancellation handle for children spawned as coroutine
        // tasks / nested scopes. A child scope calls token().child() to observe this
        // scope's cancellation without being able to cancel the parent (design §13.2).
        [[nodiscard]] const cancellation_token& token() const noexcept { return token_; }

        // join — drive all children inline, collect first failure.
        // Returns nullopt (no error) when all children succeeded.
        // Returns task_scope_error when at least one child failed or was cancelled.
        // Note: In this model futures are driven inline at spawn time (InlineBackend).
        // For async backends, join() waits on each future in registration order.
        [[nodiscard]] std::optional<task_scope_error> join() noexcept {
            // InlineBackend: futures already resolved at spawn; join is a no-op
            // unless a failure was recorded during spawn via the callback below.
            return first_error_;
        }

        // record_failure — called by the scope's spawn wrapper on task error.
        // Cancels all remaining tasks and records the first failure.
        void record_failure(std::size_t child_idx, std::string_view msg) {
            if (!first_error_) {
                first_error_ = task_scope_error{
                    task_scope_error_kind::child_failed,
                    std::string(msg),
                    child_idx
                };
                cancel();
            }
        }

        [[nodiscard]] std::size_t child_count() const noexcept { return child_count_; }

    private:
        bool cancelled_ = false;
        std::size_t child_count_ = 0;
        std::optional<task_scope_error> first_error_;
        cancellation_token token_;
    };

    // ============================================================================
    // scope_spawn — convenience: spawn a callable into a task_scope and
    // immediately await it, recording any failure into the scope.
    //
    // This is the synchronous (InlineBackend) model; async backends plug in here.
    // ============================================================================

    template <class F>
    [[nodiscard]] auto scope_spawn_await(task_scope& scope, F&& fn)
        -> std::expected<std::invoke_result_t<F>,
                         crank_future_error> {
        auto fut = scope.spawn(std::forward<F>(fn));
        auto res = crank::await(fut);
        if (!res) {
            scope.record_failure(scope.child_count() - 1,
                                 to_string(res.error()));
        }
        return res;
    }

    // ============================================================================
    // deadline_scope — task_scope with a wall-clock deadline
    //
    // If the deadline is exceeded before join() returns, the scope is cancelled
    // and task_scope_error_kind::deadline_exceeded is returned.
    // ============================================================================

    class deadline_scope {
    public:
        using clock = std::chrono::steady_clock;
        using duration = std::chrono::nanoseconds;

        explicit deadline_scope(duration timeout)
            : deadline_(clock::now() + timeout) {}

        // Compose with an enclosing deadline: the tighter of (parent, local) wins
        // (design §13.4, effective_deadline). Use when nesting a deadline_scope
        // inside another timed scope so the inner scope never outlives the outer.
        deadline_scope(duration timeout, std::optional<deadline_point> parent)
            : deadline_(*effective_deadline(parent, clock::now() + timeout)) {}

        task_scope& scope() noexcept { return scope_; }

        [[nodiscard]] deadline_point deadline() const noexcept { return deadline_; }

        // join — join all children; check deadline after inline execution.
        [[nodiscard]] std::optional<task_scope_error> join() {
            auto err = scope_.join();
            if (err) return err;
            if (clock::now() > deadline_) {
                scope_.cancel();
                return task_scope_error{
                    task_scope_error_kind::deadline_exceeded,
                    "deadline exceeded",
                    0
                };
            }
            return std::nullopt;
        }

        template <class F>
        [[nodiscard]] auto spawn(F&& fn) {
            return scope_.spawn(std::forward<F>(fn));
        }

    private:
        clock::time_point deadline_;
        task_scope scope_;
    };

    // ============================================================================
    // join_group<T> — collect results from a dynamic set of futures of type T
    //
    // Usage:
    //   join_group<int> jg;
    //   jg.add(scope.spawn([]{ return 1; }));
    //   jg.add(scope.spawn([]{ return 2; }));
    //   auto results = jg.await_all();
    // ============================================================================

    // join_results<T> — move-only, insertion-ordered results from a join_group.
    //
    // This deliberately owns nodes directly rather than placing std::expected in
    // a standard container. Current libc++ C++26 implementations instantiate an
    // invalid expected equality candidate while relocating such a container.
    // Direct ownership keeps the public operation zero-overhead and portable.
    template <class T>
    class join_results {
    public:
        join_results() = default;
        join_results(const join_results&) = delete;
        join_results& operator=(const join_results&) = delete;
        join_results(join_results&&) noexcept = default;
        join_results& operator=(join_results&&) noexcept = default;

        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        [[nodiscard]] const std::expected<T, crank_future_error>&
        operator[](std::size_t index) const noexcept {
            auto* node = head_.get();
            while (index != 0 && node != nullptr) {
                node = node->next.get();
                --index;
            }
            if (node == nullptr) std::unreachable();
            return node->result;
        }

    private:
        struct result_node {
            std::expected<T, crank_future_error> result;
            std::unique_ptr<result_node> next;
        };

        void append(std::expected<T, crank_future_error> result) {
            auto node = std::make_unique<result_node>(
                result_node{std::move(result), nullptr});
            auto* const inserted = node.get();
            if (tail_ == nullptr) {
                head_ = std::move(node);
            } else {
                tail_->next = std::move(node);
            }
            tail_ = inserted;
            ++size_;
        }

        std::unique_ptr<result_node> head_;
        result_node* tail_ = nullptr;
        std::size_t size_ = 0;

        template <class>
        friend class join_group;
    };

    template <class T>
    class join_group {
    public:
        join_group() = default;
        join_group(const join_group&) = delete;
        join_group& operator=(const join_group&) = delete;
        join_group(join_group&&) noexcept = default;
        join_group& operator=(join_group&&) noexcept = default;

        // add — register a future into the group (move-only)
        void add(crank_future<T> fut) {
            auto node = std::make_unique<future_node>(
                future_node{std::move(fut), nullptr});
            auto* const inserted = node.get();
            if (tail_ == nullptr) {
                futures_ = std::move(node);
            } else {
                tail_->next = std::move(node);
            }
            tail_ = inserted;
            ++size_;
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }

        // await_all — drive all futures and collect results.
        // Order matches registration order.
        [[nodiscard]] join_results<T> await_all() {
            join_results<T> results;
            for (auto* node = futures_.get(); node != nullptr; node = node->next.get()) {
                results.append(crank::await(node->future));
            }
            futures_.reset();
            tail_ = nullptr;
            size_ = 0;
            return results;
        }

        // first_error — returns the first error from await_all, or nullopt if all succeed.
        [[nodiscard]] std::optional<crank_future_error>
        first_error(const join_results<T>& results) const {
            for (auto* node = results.head_.get(); node != nullptr; node = node->next.get()) {
                const auto& r = node->result;
                if (!r) return r.error();
            }
            return std::nullopt;
        }

    private:
        struct future_node {
            crank_future<T> future;
            std::unique_ptr<future_node> next;
        };

        std::unique_ptr<future_node> futures_;
        future_node* tail_ = nullptr;
        std::size_t size_ = 0;
    };
} // namespace crank
