#pragma once

// crank/std/detail/uv_loop.hpp — generic, dependency-free RAII wrappers over
// libuv primitives. Guarded on __has_include(<uv.h>): when libuv is absent the
// header compiles to an empty translation unit and CRANK_STD_HAS_UV == 0.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::uvx
//
// This layer knows nothing about crank types — it is a thin, reusable C++
// veneer (loop / timer / error mapping) intended to back std.fs async, std.net,
// and std.process, and to be liftable into a standalone utility later. It is
// intentionally minimal: an owning event loop, a one-shot/periodic timer, and
// error-code → message mapping. Sockets and process spawning are layered on top
// by the consuming modules.

#if defined(__has_include)
#  if __has_include(<uv.h>)
#    define CRANK_STD_HAS_UV 1
#  else
#    define CRANK_STD_HAS_UV 0
#  endif
#else
#  define CRANK_STD_HAS_UV 0
#endif

#if CRANK_STD_HAS_UV

#include <uv.h>

#include <cstdint>
#include <string>
#include <utility>

namespace crank::uvx {
    // error_message — human-readable text for a libuv error code (< 0).
    [[nodiscard]] inline std::string error_message(int code) {
        if (code >= 0) return {};
        return std::string(uv_strerror(code));
    }

    // loop — owns a uv_loop_t. Non-copyable, movable.
    class loop {
    public:
        loop() { uv_loop_init(&loop_); }

        loop(const loop&) = delete;
        loop& operator=(const loop&) = delete;

        loop(loop&& o) noexcept : loop_(o.loop_), moved_(o.moved_) {
            o.moved_ = true;
        }

        ~loop() {
            if (!moved_) {
                // Drain any pending closes before closing the loop.
                uv_run(&loop_, UV_RUN_NOWAIT);
                uv_loop_close(&loop_);
            }
        }

        [[nodiscard]] uv_loop_t* raw() noexcept { return &loop_; }

        // Run the loop until there are no active handles/requests.
        int run() { return uv_run(&loop_, UV_RUN_DEFAULT); }
        // Process one iteration without blocking.
        int poll() { return uv_run(&loop_, UV_RUN_NOWAIT); }
        void stop() noexcept { uv_stop(&loop_); }

    private:
        uv_loop_t loop_{};
        bool moved_ = false;
    };

    // timer — a one-shot or periodic timer bound to a loop. The callback is a
    // plain function pointer + void* state (no std::function, no heap).
    class timer {
    public:
        explicit timer(loop& l) {
            uv_timer_init(l.raw(), &timer_);
            timer_.data = this;
        }

        timer(const timer&) = delete;
        timer& operator=(const timer&) = delete;

        ~timer() {
            uv_close(reinterpret_cast<uv_handle_t*>(&timer_), nullptr);
        }

        // Start the timer. cb(state) fires after timeout_ms, then every
        // repeat_ms (0 = one-shot).
        void start(void (*cb)(void*), void* state,
                   std::uint64_t timeout_ms, std::uint64_t repeat_ms = 0) {
            cb_ = cb;
            state_ = state;
            uv_timer_start(&timer_, &timer::trampoline, timeout_ms, repeat_ms);
        }

        void stop() noexcept { uv_timer_stop(&timer_); }

    private:
        static void trampoline(uv_timer_t* h) {
            auto* self = static_cast<timer*>(h->data);
            if (self && self->cb_) self->cb_(self->state_);
        }

        uv_timer_t timer_{};
        void (*cb_)(void*) = nullptr;
        void* state_ = nullptr;
    };
} // namespace crank::uvx

#endif // CRANK_STD_HAS_UV
