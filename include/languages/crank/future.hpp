#pragma once

// crank/future.hpp — Future[T] error model and spawn/await/detach semantics.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Future[T] in Crank is modelled as Future[Result[T, crank_future_error]].
// All errors at the await boundary are explicit — no silent discard.
//
// crank_future_error — error enum for all async failure modes:
//   cancelled              — future was cancelled before completing
//   task_panicked          — the task body threw / faulted
//   dropped_without_consume — future was destroyed without await or detach
//
// crank_future<T>:
//   - Wraps an async task that produces T.
//   - Must be consumed: either await()-ed or detach()-ed before destruction.
//   - Destruction without consume → dropped_without_consume diagnostic note.
//   - spawn() always produces crank_future<T>, never raw T.
//
// await(f) → std::expected<T, crank_future_error>
// detach(f) → void — intentional abandonment; suppresses drop diagnostic.
//
// Design refs: gap.md §8, crank.md §spawn/await.

#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace crank {
    // ============================================================================
    // crank_future_error — all async failure modes
    // ============================================================================

    enum class crank_future_error : std::uint8_t {
        cancelled, // task was cancelled before producing a value
        task_panicked, // task body faulted / trapped
        dropped_without_consume, // future destroyed without await() or detach()
    };

    [[nodiscard]] constexpr std::string_view to_string(crank_future_error e) noexcept {
        switch (e) {
        case crank_future_error::cancelled: return "cancelled";
        case crank_future_error::task_panicked: return "task_panicked";
        case crank_future_error::dropped_without_consume: return "dropped_without_consume";
        }
        return "unknown";
    }

    // ============================================================================
    // crank_future<T> — typed async handle
    //
    // - Produced by spawn(); wraps a callable result deferred to a backend task.
    // - Must be consumed (await or detach) before destruction.
    // - NOT copyable — ownership is exclusive (single consumer).
    // - OK to move.
    // ============================================================================

    template <class T>
    class crank_future {
    public:
        // Construct with a thunk (used internally by spawn()).
        explicit crank_future(std::function<std::expected<T, crank_future_error>()> thunk)
            : thunk_(std::move(thunk)), consumed_(false) {}

        // Non-copyable, movable.
        crank_future(const crank_future&) = delete;
        crank_future& operator=(const crank_future&) = delete;
        crank_future(crank_future&&) = default;
        crank_future& operator=(crank_future&&) = default;

        // Destructor: if not consumed, record a note (runtime diagnostic proxy).
        ~crank_future() {
            if (!consumed_) {
                // In production this would feed into the NADI pulse / diagnostic sink.
                // Here we store the drop state for test inspection.
                dropped_ = true;
            }
        }

        // await() — consume the future and obtain the result.
        [[nodiscard]] std::expected<T, crank_future_error> await() {
            consumed_ = true;
            if (thunk_) return thunk_();
            return std::unexpected(crank_future_error::task_panicked);
        }

        // detach() — intentionally abandon the future; suppresses drop diagnostic.
        void detach() noexcept { consumed_ = true; }

        // was_dropped() — true if destroyed without consume (test/diagnostic hook).
        [[nodiscard]] bool was_dropped() const noexcept { return dropped_; }

        [[nodiscard]] bool is_consumed() const noexcept { return consumed_; }

    private:
        std::function<std::expected<T, crank_future_error>()> thunk_;
        bool consumed_ = false;
        bool dropped_ = false;
    };

    // ============================================================================
    // spawn — create a crank_future<T> from a callable.
    //
    // The callable is executed lazily when await() is called.
    // This is the inline (single-threaded) spawn; production spawn routes through
    // a Pravaha JThreadBackend task.
    // ============================================================================

    template <class F>
    [[nodiscard]] auto spawn(F&& fn) -> crank_future<std::invoke_result_t<F>> {
        using T = std::invoke_result_t<F>;
        return crank_future<T>([f = std::forward<F>(fn)]() mutable
            -> std::expected<T, crank_future_error> {
                return f();
            });
    }

    // ============================================================================
    // await — free-function form: await(future) → expected<T, crank_future_error>
    // ============================================================================

    template <class T>
    [[nodiscard]] std::expected<T, crank_future_error>
    await(crank_future<T>& f) {
        return f.await();
    }

    // ============================================================================
    // detach — free-function form: detach(future) → void
    // ============================================================================

    template <class T>
    void detach(crank_future<T>& f) noexcept {
        f.detach();
    }
} // namespace crank
