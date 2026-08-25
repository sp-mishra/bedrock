#pragma once

// crank/std/io.hpp — std.io module: console output for Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// print/println/eprintln over std::print. Every entry carries the IO effect and
// the Write capability so the effect checker accounts for it. Functions are
// flagged blocking (stdout may block on a slow sink).

#include "languages/crank/std/detail/register.hpp"
#include "languages/crank/effects.hpp"

#include <cstdint>
#include <print>
#include <string>

namespace crank::stdlib {
    namespace io_fns {
        // Each returns Unit-as-i64 (0) so it flows through the typed thunk with a
        // concrete return type; Crank maps the result to Unit.
        inline std::int64_t print_str(std::string s) {
            std::print("{}", s);
            return 0;
        }
        inline std::int64_t println_str(std::string s) {
            std::println("{}", s);
            return 0;
        }
        inline std::int64_t eprintln_str(std::string s) {
            std::println(stderr, "{}", s);
            return 0;
        }
        inline std::int64_t println_i64(std::int64_t v) {
            std::println("{}", v);
            return 0;
        }
        inline std::int64_t println_f64(double v) {
            std::println("{}", v);
            return 0;
        }
        inline std::int64_t println_bool(bool v) {
            std::println("{}", v);
            return 0;
        }
    } // namespace io_fns

    inline void install_std_io(crank::context& ctx) {
        namespace io = io_fns;
        ffi_module_builder mod{"std.io"};
        const function_options w{
            .effects = vakya::types::kEffectMaskIO,
            .capabilities = vakya::types::kCapMaskWrite,
            .flags = static_cast<function_flags>(function_flag::blocking) |
                     static_cast<function_flags>(function_flag::thread_safe),
            .blocking = blocking_class::potentially_blocking,
        };

        detail::add_fn<"std.io.print", &io::print_str>(mod, ctx, "Print", w);
        detail::add_fn<"std.io.println", &io::println_str>(mod, ctx, "Println", w);
        detail::add_fn<"std.io.eprintln", &io::eprintln_str>(mod, ctx, "EPrintln", w);
        detail::add_fn<"std.io.println_i64", &io::println_i64>(mod, ctx, "PrintlnInt", w);
        detail::add_fn<"std.io.println_f64", &io::println_f64>(mod, ctx, "PrintlnFloat", w);
        detail::add_fn<"std.io.println_bool", &io::println_bool>(mod, ctx, "PrintlnBool", w);

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
