#pragma once

// crank/std/collections.hpp — std.collections module: STL containers for Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Registers the common STL containers (via host.hpp container_traits) plus a
// small set of value-semantic helpers. Helpers take containers by value and
// return new containers (the typed-thunk boundary is copy-in/copy-out), which
// keeps them pure and side-effect free.

#include "languages/crank/std/detail/register.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace crank {
    // Associative container traits (host.hpp only ships contiguous ones).
    template <class K, class V, class H, class E, class A>
    struct container_traits<std::unordered_map<K, V, H, E, A>> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = true;
        using element_type = V;
        static std::size_t size(const std::unordered_map<K, V, H, E, A>& c) noexcept { return c.size(); }
    };

    template <class K, class H, class E, class A>
    struct container_traits<std::unordered_set<K, H, E, A>> {
        static constexpr bool is_gpu_visible = false;
        static constexpr bool is_resizable = true;
        using element_type = K;
        static std::size_t size(const std::unordered_set<K, H, E, A>& c) noexcept { return c.size(); }
    };
} // namespace crank

namespace crank::stdlib {
    namespace coll_fns {
        using ivec = std::vector<std::int64_t>;
        using fvec = std::vector<double>;
        using svec = std::vector<std::string>;
        using smap = std::unordered_map<std::string, std::int64_t>;
        using sset = std::unordered_set<std::string>;

        [[nodiscard]] inline std::int64_t vec_len_i64(ivec v) noexcept {
            return static_cast<std::int64_t>(v.size());
        }
        [[nodiscard]] inline ivec vec_push_i64(ivec v, std::int64_t x) {
            v.push_back(x);
            return v;
        }
        // Bounds-checked get: returns fallback when out of range.
        [[nodiscard]] inline std::int64_t vec_get_i64(ivec v, std::int64_t i, std::int64_t fallback) noexcept {
            if (i < 0 || i >= static_cast<std::int64_t>(v.size())) return fallback;
            return v[static_cast<std::size_t>(i)];
        }
        [[nodiscard]] inline bool vec_contains_i64(ivec v, std::int64_t x) noexcept {
            for (auto e : v) if (e == x) return true;
            return false;
        }

        [[nodiscard]] inline std::int64_t vec_len_str(svec v) noexcept {
            return static_cast<std::int64_t>(v.size());
        }

        [[nodiscard]] inline std::int64_t map_size(smap m) noexcept {
            return static_cast<std::int64_t>(m.size());
        }
        [[nodiscard]] inline bool map_contains(smap m, std::string k) noexcept {
            return m.find(k) != m.end();
        }
        [[nodiscard]] inline std::int64_t map_get(smap m, std::string k, std::int64_t fallback) {
            auto it = m.find(k);
            return it == m.end() ? fallback : it->second;
        }
        [[nodiscard]] inline smap map_put(smap m, std::string k, std::int64_t v) {
            m[std::move(k)] = v;
            return m;
        }

        [[nodiscard]] inline std::int64_t set_size(sset s) noexcept {
            return static_cast<std::int64_t>(s.size());
        }
        [[nodiscard]] inline bool set_contains(sset s, std::string k) noexcept {
            return s.find(k) != s.end();
        }
        [[nodiscard]] inline sset set_add(sset s, std::string k) {
            s.insert(std::move(k));
            return s;
        }
    } // namespace coll_fns

    inline void install_std_collections(crank::context& ctx) {
        namespace c = coll_fns;
        ffi_module_builder mod{"std.collections"};
        const function_options pure{.flags = kPure};

        ctx.register_container<c::ivec>("std.collections.VecInt");
        ctx.register_container<c::fvec>("std.collections.VecFloat");
        ctx.register_container<c::svec>("std.collections.VecStr");
        ctx.register_container<c::smap>("std.collections.MapStrInt");
        ctx.register_container<c::sset>("std.collections.SetStr");
        mod.type("std.collections.VecInt", "VecInt");
        mod.type("std.collections.VecFloat", "VecFloat");
        mod.type("std.collections.VecStr", "VecStr");
        mod.type("std.collections.MapStrInt", "MapStrInt");
        mod.type("std.collections.SetStr", "SetStr");

        detail::add_fn<"std.collections.vec_len_i64", &c::vec_len_i64>(mod, ctx, "VecIntLen", pure);
        detail::add_fn<"std.collections.vec_push_i64", &c::vec_push_i64>(mod, ctx, "VecIntPush", pure);
        detail::add_fn<"std.collections.vec_get_i64", &c::vec_get_i64>(mod, ctx, "VecIntGet", pure);
        detail::add_fn<"std.collections.vec_contains_i64", &c::vec_contains_i64>(mod, ctx, "VecIntContains", pure);
        detail::add_fn<"std.collections.vec_len_str", &c::vec_len_str>(mod, ctx, "VecStrLen", pure);
        detail::add_fn<"std.collections.map_size", &c::map_size>(mod, ctx, "MapSize", pure);
        detail::add_fn<"std.collections.map_contains", &c::map_contains>(mod, ctx, "MapContains", pure);
        detail::add_fn<"std.collections.map_get", &c::map_get>(mod, ctx, "MapGet", pure);
        detail::add_fn<"std.collections.map_put", &c::map_put>(mod, ctx, "MapPut", pure);
        detail::add_fn<"std.collections.set_size", &c::set_size>(mod, ctx, "SetSize", pure);
        detail::add_fn<"std.collections.set_contains", &c::set_contains>(mod, ctx, "SetContains", pure);
        detail::add_fn<"std.collections.set_add", &c::set_add>(mod, ctx, "SetAdd", pure);

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
