#pragma once

// taranga/std/core.hpp — Taranga std.core host projections.

#include "languages/taranga/std/detail/register.hpp"

#include <span>

namespace taranga::stdlib {
    namespace core_fns {
        [[nodiscard]] inline std::int64_t identity_i64(std::span<const std::int64_t> args) {
            return args.empty() ? 0 : args[0];
        }

        [[nodiscard]] inline std::int64_t clamp_i64(std::span<const std::int64_t> args) {
            if (args.size() < 3) return 0;
            const auto v = args[0];
            const auto lo = args[1];
            const auto hi = args[2];
            if (v < lo) return lo;
            if (v > hi) return hi;
            return v;
        }
    } // namespace core_fns

    inline void install_std_core(registry& reg) {
        add_fn(reg, "std.core", "IdentityI64", "std.core.identity_i64", 1, &core_fns::identity_i64);
        add_fn(reg, "std.core", "ClampI64", "std.core.clamp_i64", 3, &core_fns::clamp_i64);
    }
} // namespace taranga::stdlib

