#pragma once

// crank/std/json.hpp — std.json module: Glaze-backed JSON for Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Guarded on __has_include(<glaze/glaze.hpp>) (CRANK_STD_HAS_GLAZE). When Glaze
// is absent the module is not installed and the header is a no-op. The JSON DOM
// (glz::generic) is wrapped in an opaque, copyable json_value registered as a
// host type; accessors read a single top-level key with a typed fallback.

#if defined(__has_include)
#  if __has_include(<glaze/glaze.hpp>)
#    define CRANK_STD_HAS_GLAZE 1
#  else
#    define CRANK_STD_HAS_GLAZE 0
#  endif
#else
#  define CRANK_STD_HAS_GLAZE 0
#endif

#include "languages/crank/std/detail/register.hpp"

#if CRANK_STD_HAS_GLAZE

#include <glaze/glaze.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>

namespace crank::stdlib {
    // json_value — opaque owner of a parsed Glaze DOM. Copyable value type so it
    // crosses the typed-thunk boundary like any other host value.
    struct json_value {
        glz::generic dom;
        bool ok = false; // false when the source failed to parse
    };

    namespace json_fns {
        [[nodiscard]] inline json_value parse(std::string text) {
            json_value v;
            auto ec = glz::read_json(v.dom, text);
            v.ok = !ec;
            return v;
        }

        [[nodiscard]] inline bool is_ok(json_value v) noexcept { return v.ok; }

        [[nodiscard]] inline std::string stringify(json_value v) {
            auto r = v.dom.dump();
            return r ? *r : std::string{};
        }

        [[nodiscard]] inline bool has_key(json_value v, std::string key) {
            return v.dom.is_object() && v.dom.contains(key);
        }

        [[nodiscard]] inline std::string get_string(json_value v, std::string key,
                                                    std::string fallback) {
            if (!v.dom.is_object() || !v.dom.contains(key)) return fallback;
            const auto& e = v.dom[key];
            return e.is_string() ? e.get_string() : fallback;
        }

        [[nodiscard]] inline double get_f64(json_value v, std::string key, double fallback) {
            if (!v.dom.is_object() || !v.dom.contains(key)) return fallback;
            const auto& e = v.dom[key];
            return e.is_number() ? e.get_number() : fallback;
        }

        [[nodiscard]] inline std::int64_t get_i64(json_value v, std::string key,
                                                  std::int64_t fallback) {
            if (!v.dom.is_object() || !v.dom.contains(key)) return fallback;
            const auto& e = v.dom[key];
            return e.is_number() ? static_cast<std::int64_t>(e.get_number()) : fallback;
        }

        [[nodiscard]] inline bool get_bool(json_value v, std::string key, bool fallback) {
            if (!v.dom.is_object() || !v.dom.contains(key)) return fallback;
            const auto& e = v.dom[key];
            return e.is_boolean() ? e.get_boolean() : fallback;
        }
    } // namespace json_fns
} // namespace crank::stdlib

// Expose json_value as an opaque host type (no inspectable fields).
template <>
struct crank::type_descriptor<crank::stdlib::json_value> {
    static constexpr std::string_view name = "std.json.Json";
    static constexpr auto fields = std::tuple{};
};

namespace crank::stdlib {
    inline void install_std_json(crank::context& ctx) {
        namespace j = json_fns;
        ffi_module_builder mod{"std.json"};
        const function_options pure{.flags = kPure};

        ctx.register_type<json_value>();
        mod.type("std.json.Json", "Json");

        detail::add_fn<"std.json.parse", &j::parse>(mod, ctx, "Parse", pure);
        detail::add_fn<"std.json.is_ok", &j::is_ok>(mod, ctx, "IsOk", pure);
        detail::add_fn<"std.json.stringify", &j::stringify>(mod, ctx, "Stringify", pure);
        detail::add_fn<"std.json.has_key", &j::has_key>(mod, ctx, "HasKey", pure);
        detail::add_fn<"std.json.get_string", &j::get_string>(mod, ctx, "GetString", pure);
        detail::add_fn<"std.json.get_f64", &j::get_f64>(mod, ctx, "GetFloat", pure);
        detail::add_fn<"std.json.get_i64", &j::get_i64>(mod, ctx, "GetInt", pure);
        detail::add_fn<"std.json.get_bool", &j::get_bool>(mod, ctx, "GetBool", pure);

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib

#endif // CRANK_STD_HAS_GLAZE
