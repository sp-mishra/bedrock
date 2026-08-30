#pragma once

// taranga/std/std.hpp — Taranga standard library umbrella.

#include "languages/taranga/std/core.hpp"
#include "languages/taranga/std/io.hpp"
#include "languages/taranga/std/math.hpp"
#include "languages/taranga/std/time.hpp"

namespace taranga::stdlib {
    inline void install_std_all(registry& reg) {
        install_std_core(reg);
        install_std_math(reg);
        install_std_time(reg);
        install_std_io(reg);
    }
} // namespace taranga::stdlib

