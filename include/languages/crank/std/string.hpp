#pragma once

// crank/std/string.hpp — std.string module: std::string operations for Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// All functions take/return owned std::string (or bool / int64 / vector) and
// are pure. Strings cross the boundary through the typed-thunk path by value
// (the thunk packs a const std::string* per argument).

#include "languages/crank/std/detail/register.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace crank::stdlib {
    namespace string_fns {
        [[nodiscard]] inline std::int64_t len(std::string s) noexcept {
            return static_cast<std::int64_t>(s.size());
        }

        [[nodiscard]] inline std::string to_upper(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            return s;
        }

        [[nodiscard]] inline std::string to_lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        }

        [[nodiscard]] inline std::string trim(std::string s) {
            auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
            auto b = std::ranges::find_if(s, not_space);
            auto e = std::find_if(s.rbegin(), s.rend(), not_space).base();
            if (b >= e) return std::string{};
            return std::string(b, e);
        }

        [[nodiscard]] inline bool starts_with(std::string s, std::string prefix) noexcept {
            return std::string_view{s}.starts_with(prefix);
        }

        [[nodiscard]] inline bool ends_with(std::string s, std::string suffix) noexcept {
            return std::string_view{s}.ends_with(suffix);
        }

        [[nodiscard]] inline bool contains(std::string s, std::string needle) noexcept {
            return s.find(needle) != std::string::npos;
        }

        // index_of — first byte offset of needle, or -1 if absent.
        [[nodiscard]] inline std::int64_t index_of(std::string s, std::string needle) noexcept {
            auto p = s.find(needle);
            return p == std::string::npos ? -1 : static_cast<std::int64_t>(p);
        }

        // substr — clamped [start, start+count). Negative-safe via unsigned inputs.
        [[nodiscard]] inline std::string substr(std::string s, std::int64_t start,
                                                std::int64_t count) {
            if (start < 0) start = 0;
            const auto n = static_cast<std::int64_t>(s.size());
            if (start >= n) return std::string{};
            const auto len = (count < 0) ? (n - start) : std::min(count, n - start);
            return s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(len));
        }

        [[nodiscard]] inline std::string replace(std::string s, std::string from,
                                                 std::string to) {
            if (from.empty()) return s;
            std::string out;
            out.reserve(s.size());
            std::size_t pos = 0, prev = 0;
            while ((pos = s.find(from, prev)) != std::string::npos) {
                out.append(s, prev, pos - prev);
                out.append(to);
                prev = pos + from.size();
            }
            out.append(s, prev, s.size() - prev);
            return out;
        }

        [[nodiscard]] inline std::string concat(std::string a, std::string b) {
            a.append(b);
            return a;
        }

        [[nodiscard]] inline std::string repeat(std::string s, std::int64_t times) {
            if (times <= 0) return std::string{};
            std::string out;
            out.reserve(s.size() * static_cast<std::size_t>(times));
            for (std::int64_t i = 0; i < times; ++i) out.append(s);
            return out;
        }

        [[nodiscard]] inline std::vector<std::string> split(std::string s,
                                                            std::string sep) {
            std::vector<std::string> parts;
            if (sep.empty()) {
                parts.push_back(std::move(s));
                return parts;
            }
            std::size_t prev = 0, pos;
            while ((pos = s.find(sep, prev)) != std::string::npos) {
                parts.push_back(s.substr(prev, pos - prev));
                prev = pos + sep.size();
            }
            parts.push_back(s.substr(prev));
            return parts;
        }

        [[nodiscard]] inline std::string join(std::vector<std::string> parts,
                                              std::string sep) {
            std::string out;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i) out.append(sep);
                out.append(parts[i]);
            }
            return out;
        }
    } // namespace string_fns

    inline void install_std_string(crank::context& ctx) {
        namespace s = string_fns;
        ffi_module_builder mod{"std.string"};
        const function_options pure{.flags = kPure};

        detail::add_fn<"std.string.len", &s::len>(mod, ctx, "Len", pure);
        detail::add_fn<"std.string.to_upper", &s::to_upper>(mod, ctx, "ToUpper", pure);
        detail::add_fn<"std.string.to_lower", &s::to_lower>(mod, ctx, "ToLower", pure);
        detail::add_fn<"std.string.trim", &s::trim>(mod, ctx, "Trim", pure);
        detail::add_fn<"std.string.starts_with", &s::starts_with>(mod, ctx, "StartsWith", pure);
        detail::add_fn<"std.string.ends_with", &s::ends_with>(mod, ctx, "EndsWith", pure);
        detail::add_fn<"std.string.contains", &s::contains>(mod, ctx, "Contains", pure);
        detail::add_fn<"std.string.index_of", &s::index_of>(mod, ctx, "IndexOf", pure);
        detail::add_fn<"std.string.substr", &s::substr>(mod, ctx, "Substr", pure);
        detail::add_fn<"std.string.replace", &s::replace>(mod, ctx, "Replace", pure);
        detail::add_fn<"std.string.concat", &s::concat>(mod, ctx, "Concat", pure);
        detail::add_fn<"std.string.repeat", &s::repeat>(mod, ctx, "Repeat", pure);
        detail::add_fn<"std.string.split", &s::split>(mod, ctx, "Split", pure);
        detail::add_fn<"std.string.join", &s::join>(mod, ctx, "Join", pure);

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
