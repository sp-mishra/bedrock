#pragma once

// crank/std/core.hpp — std.core module: Option/Result helpers + basics.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Crank has no monomorphising generics at the host boundary yet, so the v1
// helpers are provided per concrete element type (i64/f64/String). Each maps to
// std::optional<T> (Option[T]) via the container_traits already in host.hpp.
// The optional itself crosses by value through the typed thunk.

#include "languages/crank/std/detail/register.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace crank::stdlib {
    namespace core_fns {
        template <class T>
        [[nodiscard]] bool is_some(std::optional<T> o) noexcept { return o.has_value(); }

        template <class T>
        [[nodiscard]] bool is_none(std::optional<T> o) noexcept { return !o.has_value(); }

        template <class T>
        [[nodiscard]] T unwrap_or(std::optional<T> o, T fallback) {
            return o.has_value() ? std::move(*o) : std::move(fallback);
        }

        // Concrete instantiations addressable as function pointers.
        inline bool is_some_i64(std::optional<std::int64_t> o) noexcept { return is_some(std::move(o)); }
        inline bool is_none_i64(std::optional<std::int64_t> o) noexcept { return is_none(std::move(o)); }
        inline std::int64_t unwrap_or_i64(std::optional<std::int64_t> o, std::int64_t f) { return unwrap_or(std::move(o), f); }

        inline bool is_some_f64(std::optional<double> o) noexcept { return is_some(std::move(o)); }
        inline bool is_none_f64(std::optional<double> o) noexcept { return is_none(std::move(o)); }
        inline double unwrap_or_f64(std::optional<double> o, double f) { return unwrap_or(std::move(o), f); }

        inline bool is_some_str(std::optional<std::string> o) noexcept { return is_some(std::move(o)); }
        inline bool is_none_str(std::optional<std::string> o) noexcept { return is_none(std::move(o)); }
        inline std::string unwrap_or_str(std::optional<std::string> o, std::string f) { return unwrap_or(std::move(o), std::move(f)); }

        // Scalar identity/clamp helpers.
        inline std::int64_t clamp_i64(std::int64_t v, std::int64_t lo, std::int64_t hi) noexcept {
            return v < lo ? lo : (v > hi ? hi : v);
        }
        inline double clamp_f64(double v, double lo, double hi) noexcept {
            return v < lo ? lo : (v > hi ? hi : v);
        }
    } // namespace core_fns

    inline void install_std_core(crank::context& ctx) {
        namespace c = core_fns;
        ffi_module_builder mod{"std.core"};
        const function_options pure{.flags = kPure};

        detail::add_fn<"std.core.is_some_i64", &c::is_some_i64>(mod, ctx, "IsSomeInt", pure);
        detail::add_fn<"std.core.is_none_i64", &c::is_none_i64>(mod, ctx, "IsNoneInt", pure);
        detail::add_fn<"std.core.unwrap_or_i64", &c::unwrap_or_i64>(mod, ctx, "UnwrapOrInt", pure);
        detail::add_fn<"std.core.is_some_f64", &c::is_some_f64>(mod, ctx, "IsSomeFloat", pure);
        detail::add_fn<"std.core.is_none_f64", &c::is_none_f64>(mod, ctx, "IsNoneFloat", pure);
        detail::add_fn<"std.core.unwrap_or_f64", &c::unwrap_or_f64>(mod, ctx, "UnwrapOrFloat", pure);
        detail::add_fn<"std.core.is_some_str", &c::is_some_str>(mod, ctx, "IsSomeStr", pure);
        detail::add_fn<"std.core.is_none_str", &c::is_none_str>(mod, ctx, "IsNoneStr", pure);
        detail::add_fn<"std.core.unwrap_or_str", &c::unwrap_or_str>(mod, ctx, "UnwrapOrStr", pure);
        detail::add_fn<"std.core.clamp_i64", &c::clamp_i64>(mod, ctx, "ClampInt", pure);
        detail::add_fn<"std.core.clamp_f64", &c::clamp_f64>(mod, ctx, "ClampFloat", pure);

        // Expose the Option element containers so tooling sees the mapping.
        ctx.register_container<std::optional<std::int64_t>>("std.core.OptionInt");
        ctx.register_container<std::optional<double>>("std.core.OptionFloat");
        ctx.register_container<std::optional<std::string>>("std.core.OptionStr");

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
