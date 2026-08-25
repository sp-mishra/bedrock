#pragma once

// crank/std/detail/register.hpp — shared registration funnel for the crank
// standard library.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib::detail
//
// The standard library is a reflection-driven projection of C++/STL facilities
// into Crank (crank stdlib design). Every std module registers a batch of C++
// free functions through the SAME two seams the host embedding already exposes:
//
//   crank::context::register_function_descriptor(fd)  — typed thunk + options
//   crank::ffi_module_builder::fn(host, crank, arity)  — import-visible symbol
//
// add_fn<HostName, Fn> ties both together in one call: it builds a typed
// function_descriptor (via make_host_fn_descriptor, so the direct thunk is
// present with no std::any in the hot path), pushes it into the context, and
// records the matching ffi symbol so `import "std.x"` sees it. Arity is taken
// from callable_traits — never hand-typed.

#include "languages/crank/context.hpp"
#include "languages/crank/ffi_module.hpp"
#include "languages/crank/host.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace crank::stdlib {
    // Common effect/capability presets so modules read declaratively.
    inline constexpr function_flags kPure =
        static_cast<function_flags>(function_flag::pure) |
        static_cast<function_flags>(function_flag::deterministic) |
        static_cast<function_flags>(function_flag::thread_safe);

    namespace detail {
        // add_fn — register one std free function into both the context (typed
        // thunk + options) and the module builder (import-visible symbol).
        //
        // HostName is the qualified registration key, e.g. "std.math.sqrt".
        // crank_name is the surface identifier used in Crank source, e.g. "Sqrt".
        template <lithe::fixed_string HostName, auto Fn>
        void add_fn(ffi_module_builder& mod, crank::context& ctx,
                    std::string crank_name, function_options opts = {}) {
            function_descriptor fd = make_host_fn_descriptor<HostName, Fn>(opts);
            const std::string host = fd.name;       // "std.math.sqrt"
            const std::size_t arity = fd.arity;
            const descriptor_fingerprint fp = fd.fingerprint;
            ctx.register_function_descriptor(std::move(fd));
            mod.fn(host, std::move(crank_name), arity, fp);
        }

        // add_const — record a compile-time constant symbol (value handed to
        // crank source as a nullary extern). The C++ value itself is exposed as
        // a zero-arg function so it flows through the same typed-thunk path.
        template <lithe::fixed_string HostName, auto Fn>
        void add_const(ffi_module_builder& mod, crank::context& ctx,
                       std::string crank_name) {
            function_descriptor fd = make_host_fn_descriptor<HostName, Fn>(
                function_options{.flags = kPure});
            const std::string host = fd.name;
            const descriptor_fingerprint fp = fd.fingerprint;
            ctx.register_function_descriptor(std::move(fd));
            // Surface as a `constant` symbol for tooling, arity 0.
            ffi_symbol s;
            s.name = host;
            s.crank_name = std::move(crank_name);
            s.kind = ffi_symbol_kind::constant;
            s.arity = 0;
            s.fingerprint = fp;
            mod.symbol(std::move(s));
        }
    } // namespace detail
} // namespace crank::stdlib
