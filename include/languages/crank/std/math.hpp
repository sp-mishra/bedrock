#pragma once

// crank/std/math.hpp — std.math module: <cmath>/<numbers> projected into Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Every entry is a pure, deterministic C++ free function wrapped through the
// host typed-thunk path (see std/detail/register.hpp). install_std_math builds
// one ffi_module named "std.math" and seeds the resolver's native tier so
// `import "std.math"` resolves.

#include "languages/crank/std/detail/register.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>

namespace crank::stdlib {
    namespace math_fns {
        // Unary double transcendentals.
        [[nodiscard]] inline double sin_(double x) noexcept { return std::sin(x); }
        [[nodiscard]] inline double cos_(double x) noexcept { return std::cos(x); }
        [[nodiscard]] inline double tan_(double x) noexcept { return std::tan(x); }
        [[nodiscard]] inline double asin_(double x) noexcept { return std::asin(x); }
        [[nodiscard]] inline double acos_(double x) noexcept { return std::acos(x); }
        [[nodiscard]] inline double atan_(double x) noexcept { return std::atan(x); }
        [[nodiscard]] inline double sqrt_(double x) noexcept { return std::sqrt(x); }
        [[nodiscard]] inline double cbrt_(double x) noexcept { return std::cbrt(x); }
        [[nodiscard]] inline double exp_(double x) noexcept { return std::exp(x); }
        [[nodiscard]] inline double log_(double x) noexcept { return std::log(x); }
        [[nodiscard]] inline double log2_(double x) noexcept { return std::log2(x); }
        [[nodiscard]] inline double log10_(double x) noexcept { return std::log10(x); }
        [[nodiscard]] inline double floor_(double x) noexcept { return std::floor(x); }
        [[nodiscard]] inline double ceil_(double x) noexcept { return std::ceil(x); }
        [[nodiscard]] inline double round_(double x) noexcept { return std::round(x); }
        [[nodiscard]] inline double trunc_(double x) noexcept { return std::trunc(x); }
        [[nodiscard]] inline double fabs_(double x) noexcept { return std::fabs(x); }

        // Binary double.
        [[nodiscard]] inline double atan2_(double y, double x) noexcept { return std::atan2(y, x); }
        [[nodiscard]] inline double pow_(double b, double e) noexcept { return std::pow(b, e); }
        [[nodiscard]] inline double fmod_(double a, double b) noexcept { return std::fmod(a, b); }
        [[nodiscard]] inline double hypot_(double a, double b) noexcept { return std::hypot(a, b); }

        // Min / max for the two numeric domains crank cares about.
        [[nodiscard]] inline std::int64_t min_i64(std::int64_t a, std::int64_t b) noexcept { return a < b ? a : b; }
        [[nodiscard]] inline std::int64_t max_i64(std::int64_t a, std::int64_t b) noexcept { return a > b ? a : b; }
        [[nodiscard]] inline double min_f64(double a, double b) noexcept { return a < b ? a : b; }
        [[nodiscard]] inline double max_f64(double a, double b) noexcept { return a > b ? a : b; }

        // Constants exposed as nullary functions (flow through the typed thunk).
        [[nodiscard]] inline double pi_() noexcept { return std::numbers::pi; }
        [[nodiscard]] inline double e_() noexcept { return std::numbers::e; }
        [[nodiscard]] inline double tau_() noexcept { return 2.0 * std::numbers::pi; }
        [[nodiscard]] inline double sqrt2_() noexcept { return std::numbers::sqrt2; }
    } // namespace math_fns

    // install_std_math — register the std.math module into ctx + resolver.
    inline void install_std_math(crank::context& ctx) {
        namespace m = math_fns;
        ffi_module_builder mod{"std.math"};
        const function_options pure{.flags = kPure};

        detail::add_fn<"std.math.sin", &m::sin_>(mod, ctx, "Sin", pure);
        detail::add_fn<"std.math.cos", &m::cos_>(mod, ctx, "Cos", pure);
        detail::add_fn<"std.math.tan", &m::tan_>(mod, ctx, "Tan", pure);
        detail::add_fn<"std.math.asin", &m::asin_>(mod, ctx, "Asin", pure);
        detail::add_fn<"std.math.acos", &m::acos_>(mod, ctx, "Acos", pure);
        detail::add_fn<"std.math.atan", &m::atan_>(mod, ctx, "Atan", pure);
        detail::add_fn<"std.math.sqrt", &m::sqrt_>(mod, ctx, "Sqrt", pure);
        detail::add_fn<"std.math.cbrt", &m::cbrt_>(mod, ctx, "Cbrt", pure);
        detail::add_fn<"std.math.exp", &m::exp_>(mod, ctx, "Exp", pure);
        detail::add_fn<"std.math.log", &m::log_>(mod, ctx, "Log", pure);
        detail::add_fn<"std.math.log2", &m::log2_>(mod, ctx, "Log2", pure);
        detail::add_fn<"std.math.log10", &m::log10_>(mod, ctx, "Log10", pure);
        detail::add_fn<"std.math.floor", &m::floor_>(mod, ctx, "Floor", pure);
        detail::add_fn<"std.math.ceil", &m::ceil_>(mod, ctx, "Ceil", pure);
        detail::add_fn<"std.math.round", &m::round_>(mod, ctx, "Round", pure);
        detail::add_fn<"std.math.trunc", &m::trunc_>(mod, ctx, "Trunc", pure);
        detail::add_fn<"std.math.abs", &m::fabs_>(mod, ctx, "Abs", pure);

        detail::add_fn<"std.math.atan2", &m::atan2_>(mod, ctx, "Atan2", pure);
        detail::add_fn<"std.math.pow", &m::pow_>(mod, ctx, "Pow", pure);
        detail::add_fn<"std.math.fmod", &m::fmod_>(mod, ctx, "Fmod", pure);
        detail::add_fn<"std.math.hypot", &m::hypot_>(mod, ctx, "Hypot", pure);

        detail::add_fn<"std.math.min_i64", &m::min_i64>(mod, ctx, "MinInt", pure);
        detail::add_fn<"std.math.max_i64", &m::max_i64>(mod, ctx, "MaxInt", pure);
        detail::add_fn<"std.math.min_f64", &m::min_f64>(mod, ctx, "MinFloat", pure);
        detail::add_fn<"std.math.max_f64", &m::max_f64>(mod, ctx, "MaxFloat", pure);

        detail::add_const<"std.math.pi", &m::pi_>(mod, ctx, "Pi");
        detail::add_const<"std.math.e", &m::e_>(mod, ctx, "E");
        detail::add_const<"std.math.tau", &m::tau_>(mod, ctx, "Tau");
        detail::add_const<"std.math.sqrt2", &m::sqrt2_>(mod, ctx, "Sqrt2");

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
