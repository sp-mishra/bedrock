#pragma once

// taranga/std/time.hpp — Taranga std.time host projections.

#include "languages/taranga/std/detail/register.hpp"

#include <chrono>
#include <span>

namespace taranga::stdlib {
    namespace time_fns {
        [[nodiscard]] inline std::int64_t now_ns(std::span<const std::int64_t>) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        }
    } // namespace time_fns

    inline void install_std_time(registry& reg) {
        add_fn(reg, "std.time", "NowNs", "std.time.now_ns", 0, &time_fns::now_ns);
    }
} // namespace taranga::stdlib

