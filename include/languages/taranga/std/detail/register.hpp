#pragma once

// taranga/std/detail/register.hpp — lightweight stdlib registry for Taranga.
//
// C++23, header-only, no virtual, no macros. Namespace: taranga::stdlib
//
// Taranga currently lowers/import-validates WebAssembly modules but does not yet
// wire imported calls into execution. This registry mirrors crank's install-first
// stdlib shape so embedders can declare, validate, and directly invoke host std
// surfaces today, while keeping runtime call wiring as an additive step.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace taranga::stdlib {

    using host_fn = std::int64_t(*)(std::span<const std::int64_t>);

    struct function_entry {
        std::string module;      // e.g. "std.math"
        std::string name;        // e.g. "AddI64"
        std::string host_symbol; // e.g. "std.math.add_i64"
        std::size_t arity = 0;
        host_fn fn = nullptr;
    };

    class registry {
    public:
        void add(function_entry e) {
            // Replace by host symbol to keep installs idempotent.
            auto it = std::find_if(entries_.begin(), entries_.end(),
                                   [&](const function_entry& cur) {
                                       return cur.host_symbol == e.host_symbol;
                                   });
            if (it == entries_.end()) entries_.push_back(std::move(e));
            else *it = std::move(e);
        }

        [[nodiscard]] const function_entry* find(std::string_view host_symbol) const noexcept {
            auto it = std::find_if(entries_.begin(), entries_.end(),
                                   [&](const function_entry& e) {
                                       return std::string_view{e.host_symbol} == host_symbol;
                                   });
            return (it == entries_.end()) ? nullptr : &*it;
        }

        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    private:
        std::vector<function_entry> entries_;
    };

    inline void add_fn(registry& reg,
                       std::string_view module,
                       std::string_view name,
                       std::string_view host_symbol,
                       std::size_t arity,
                       host_fn fn) {
        reg.add(function_entry{
            .module = std::string(module),
            .name = std::string(name),
            .host_symbol = std::string(host_symbol),
            .arity = arity,
            .fn = fn,
        });
    }

} // namespace taranga::stdlib

