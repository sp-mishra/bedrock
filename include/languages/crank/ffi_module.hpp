#pragma once

// crank/ffi_module.hpp — FFI module descriptor + registry for host-side modules (§X, §M).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// An ffi_module groups a set of registered host symbols (functions, types, resources)
// under a module name that Crank source can `import`. When an engine resolves
// `import "math"`, the ffi_module_registry is checked before the filesystem tiers
// (native tier 1 of the 9-tier resolver order, crank.md §Module Resolver Order).
//
// ffi_symbol — one exported symbol visible to Crank source.
// ffi_module_descriptor — full module identity block: name + exported symbols.
// ffi_module_builder    — fluent builder: fn/type/resource → build().
// ffi_module_registry   — singleton-free per-context registry: register/lookup.
//
// Usage:
//   auto mod = crank::ffi_module_builder{"math"}
//       .fn("dot",  2, crank::ffi_symbol_kind::function)
//       .fn("cross", 2, crank::ffi_symbol_kind::function)
//       .build();
//   registry.register_module(std::move(mod));
//   // or via engine:
//   e.context().register_ffi_module(std::move(mod));
//
//   // Load in crank source: import "math"   →  Dot / Cross become extern fn decls.
//
// The registry hands out a module_descriptor (module.hpp) to the module_resolver's
// native tier so it participates in the normal 9-tier resolve(). The exported symbol
// list is used by engine::load() to populate module_handle::exports().

#include "languages/crank/module.hpp"
#include "languages/crank/host.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace crank {

    // =========================================================================
    // ffi_symbol_kind — what kind of host entity the symbol names
    // =========================================================================

    enum class ffi_symbol_kind : std::uint8_t {
        function  = 0,  // registered via register_function<>
        type      = 1,  // registered via register_type<>
        resource  = 2,  // registered via register_transactional/register_resource
        constant  = 3,  // compile-time constant value
    };

    [[nodiscard]] constexpr std::string_view to_string(ffi_symbol_kind k) noexcept {
        switch (k) {
        case ffi_symbol_kind::function: return "function";
        case ffi_symbol_kind::type:     return "type";
        case ffi_symbol_kind::resource: return "resource";
        case ffi_symbol_kind::constant: return "constant";
        }
        return "unknown";
    }

    // =========================================================================
    // ffi_symbol — one exported symbol in an ffi_module
    // =========================================================================

    struct ffi_symbol {
        std::string       name;          // qualified host registration name ("math.dot")
        std::string       crank_name;    // surface name in Crank ("Dot") — may equal name
        ffi_symbol_kind   kind  = ffi_symbol_kind::function;
        std::size_t       arity = 0;     // 0 for non-functions
        descriptor_fingerprint fingerprint = 0; // from function_descriptor when available

        [[nodiscard]] bool is_function() const noexcept {
            return kind == ffi_symbol_kind::function;
        }
    };

    // =========================================================================
    // ffi_module_descriptor — static identity block for a host-side FFI module
    // =========================================================================

    struct ffi_module_descriptor {
        std::string                name;     // module name — matches import "name"
        std::vector<ffi_symbol>    symbols;  // exported symbols
        module_hash                content_hash{};

        [[nodiscard]] bool empty() const noexcept { return symbols.empty(); }

        // Find symbol by crank-side name.
        [[nodiscard]] const ffi_symbol* find(std::string_view crank_name) const noexcept {
            for (const auto& s : symbols)
                if (s.crank_name == crank_name || s.name == crank_name) return &s;
            return nullptr;
        }

        // Convert to a module_descriptor for the 9-tier resolver.
        [[nodiscard]] module_descriptor to_module_descriptor() const {
            module_descriptor d;
            d.name         = name;
            d.kind         = module_kind::native;
            d.content_hash = content_hash;
            return d;
        }
    };

    // =========================================================================
    // ffi_module_builder — fluent construction of ffi_module_descriptor
    //
    // Usage:
    //   auto mod = crank::ffi_module_builder{"math"}
    //       .fn("math.dot",   "Dot",   2)
    //       .fn("math.cross", "Cross", 2)
    //       .type("math.Vec3", "Vec3")
    //       .build();
    // =========================================================================

    class ffi_module_builder {
    public:
        explicit ffi_module_builder(std::string module_name)
            : name_(std::move(module_name)) {}

        // Add a function symbol: host_name = qualified registration key,
        // crank_name = surface identifier in Crank source, arity = param count.
        ffi_module_builder& fn(std::string host_name,
                               std::string crank_name,
                               std::size_t arity,
                               descriptor_fingerprint fp = 0) {
            ffi_symbol s;
            s.name       = std::move(host_name);
            s.crank_name = std::move(crank_name);
            s.kind       = ffi_symbol_kind::function;
            s.arity      = arity;
            s.fingerprint = fp;
            symbols_.push_back(std::move(s));
            return *this;
        }

        // Convenience: host_name and crank_name are the same.
        ffi_module_builder& fn(std::string name, std::size_t arity) {
            return fn(name, name, arity);
        }

        // Add a type symbol.
        ffi_module_builder& type(std::string host_name, std::string crank_name) {
            ffi_symbol s;
            s.name       = std::move(host_name);
            s.crank_name = std::move(crank_name);
            s.kind       = ffi_symbol_kind::type;
            symbols_.push_back(std::move(s));
            return *this;
        }

        // Add a resource symbol.
        ffi_module_builder& resource(std::string host_name, std::string crank_name) {
            ffi_symbol s;
            s.name       = std::move(host_name);
            s.crank_name = std::move(crank_name);
            s.kind       = ffi_symbol_kind::resource;
            symbols_.push_back(std::move(s));
            return *this;
        }

        // Add a fully-formed symbol (e.g. a constant). Escape hatch for kinds
        // without a dedicated fluent helper.
        ffi_module_builder& symbol(ffi_symbol s) {
            symbols_.push_back(std::move(s));
            return *this;
        }

        // Build the descriptor. Computes content_hash from name + symbol names.
        [[nodiscard]] ffi_module_descriptor build() {
            ffi_module_descriptor d;
            d.name    = std::move(name_);
            d.symbols = std::move(symbols_);
            // Hash: FNV-1a over module name + all symbol names for stable identity.
            std::string combined = d.name;
            for (const auto& s : d.symbols) combined += '|' + s.name;
            d.content_hash = hash_source(combined);
            return d;
        }

    private:
        std::string            name_;
        std::vector<ffi_symbol> symbols_;
    };

    // =========================================================================
    // ffi_module_registry — per-context registry of FFI module descriptors
    //
    // Registered modules are exposed to the module_resolver as native tier entries.
    // engine::load() checks this registry to populate module_handle::exports().
    // =========================================================================

    class ffi_module_registry {
    public:
        // Register an FFI module. If a module with the same name already exists,
        // it is replaced. Also seeds the given module_resolver's native tier so
        // `import "name"` resolves immediately after registration.
        void register_module(ffi_module_descriptor desc,
                             module_resolver* resolver = nullptr) {
            if (resolver) {
                resolver->add_native(desc.to_module_descriptor());
            }
            modules_[desc.name] = std::move(desc);
        }

        // Lookup by module name. Returns nullptr if not registered.
        [[nodiscard]] const ffi_module_descriptor*
        find(std::string_view name) const noexcept {
            auto it = modules_.find(std::string(name));
            if (it == modules_.end()) return nullptr;
            return &it->second;
        }

        [[nodiscard]] bool empty()  const noexcept { return modules_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return modules_.size(); }

        // Iterate all registered modules.
        template <class Fn>
        void for_each(Fn&& fn) const {
            for (const auto& [k, v] : modules_) fn(v);
        }

        // Collect all symbols with a given kind across all modules.
        [[nodiscard]] std::vector<const ffi_symbol*>
        symbols_of_kind(ffi_symbol_kind kind) const {
            std::vector<const ffi_symbol*> result;
            for (const auto& [_, m] : modules_)
                for (const auto& s : m.symbols)
                    if (s.kind == kind) result.push_back(&s);
            return result;
        }

    private:
        std::unordered_map<std::string, ffi_module_descriptor> modules_;
    };

} // namespace crank
