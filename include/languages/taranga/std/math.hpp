#pragma once

// taranga/std/math.hpp — Taranga std.math host projections.

#include "languages/taranga/std/detail/register.hpp"

#include <span>

namespace taranga::stdlib {
    namespace math_fns {
        [[nodiscard]] inline std::int64_t add_i64(std::span<const std::int64_t> args) {
            return args.size() < 2 ? 0 : (args[0] + args[1]);
        }

        [[nodiscard]] inline std::int64_t sub_i64(std::span<const std::int64_t> args) {
            return args.size() < 2 ? 0 : (args[0] - args[1]);
        }

        [[nodiscard]] inline std::int64_t mul_i64(std::span<const std::int64_t> args) {
            return args.size() < 2 ? 0 : (args[0] * args[1]);
        }

        [[nodiscard]] inline std::int64_t abs_i64(std::span<const std::int64_t> args) {
            if (args.empty()) return 0;
            return args[0] < 0 ? -args[0] : args[0];
        }
    } // namespace math_fns

    inline void install_std_math(registry& reg) {
        add_fn(reg, "std.math", "AddI64", "std.math.add_i64", 2, &math_fns::add_i64);
        add_fn(reg, "std.math", "SubI64", "std.math.sub_i64", 2, &math_fns::sub_i64);
        add_fn(reg, "std.math", "MulI64", "std.math.mul_i64", 2, &math_fns::mul_i64);
        add_fn(reg, "std.math", "AbsI64", "std.math.abs_i64", 1, &math_fns::abs_i64);
    }
} // namespace taranga::stdlib

