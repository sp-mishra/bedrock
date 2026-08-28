#pragma once

// crank/std/time.hpp — std.time module: std::chrono projected into Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Clocks are pure reads of wall/steady time (deterministic flag NOT set — the
// value depends on when it runs). Sleep is a blocking effect.

#include "languages/crank/std/detail/register.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace crank::stdlib {
    namespace time_fns {
        [[nodiscard]] inline std::int64_t now_ns() noexcept {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        }
        [[nodiscard]] inline std::int64_t steady_ns() noexcept {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        }
        [[nodiscard]] inline std::int64_t ns_to_ms(std::int64_t ns) noexcept { return ns / 1'000'000; }
        [[nodiscard]] inline std::int64_t ns_to_us(std::int64_t ns) noexcept { return ns / 1'000; }

        inline std::int64_t sleep_ms(std::int64_t ms) {
            if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return ms;
        }
    } // namespace time_fns

    inline void install_std_time(crank::context& ctx) {
        namespace t = time_fns;
        ffi_module_builder mod{"std.time"};
        // Clock reads: thread-safe + pure (no external mutation), but not deterministic.
        const function_options clock_opts{
            .flags = static_cast<function_flags>(function_flag::thread_safe)
        };
        const function_options pure{.flags = kPure};
        const function_options sleep_opts{
            .flags = static_cast<function_flags>(function_flag::blocking) |
                     static_cast<function_flags>(function_flag::thread_safe),
            .blocking = blocking_class::potentially_blocking,
        };

        detail::add_fn<"std.time.now_ns", &t::now_ns>(mod, ctx, "NowNanos", clock_opts);
        detail::add_fn<"std.time.steady_ns", &t::steady_ns>(mod, ctx, "SteadyNanos", clock_opts);
        detail::add_fn<"std.time.ns_to_ms", &t::ns_to_ms>(mod, ctx, "NanosToMillis", pure);
        detail::add_fn<"std.time.ns_to_us", &t::ns_to_us>(mod, ctx, "NanosToMicros", pure);
        detail::add_fn<"std.time.sleep_ms", &t::sleep_ms>(mod, ctx, "SleepMillis", sleep_opts);

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
