#pragma once

// taranga/std/io.hpp — Taranga std.io host projections.

#include "languages/taranga/std/detail/register.hpp"

#include <print>
#include <span>

namespace taranga::stdlib {
    namespace io_fns {
        [[nodiscard]] inline std::int64_t println_i64(std::span<const std::int64_t> args) {
            const auto v = args.empty() ? 0 : args[0];
            std::println("{}", v);
            return 0;
        }
    } // namespace io_fns

    inline void install_std_io(registry& reg) {
        add_fn(reg, "std.io", "PrintlnI64", "std.io.println_i64", 1, &io_fns::println_i64);
    }
} // namespace taranga::stdlib

