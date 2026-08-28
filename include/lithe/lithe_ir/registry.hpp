#pragma once

// =============================================================================
// lithe_ir/registry.hpp — optional IR provider registry
//
// A SEPARATE registry from ::lithe::execution::backend_registry.
// Different concerns: capabilities / lifetimes / selection / security.
//
// ir_provider_registry stores named IR providers (importers + exporters + validators).
// Intended for dynamic tooling only; the static engine never instantiates it.
//
// Design:
//   • Providers are registered by string id.
//   • Each entry holds a type-erased bundle of available CPO functions
//     (import_text, import_binary, validate_ir, etc.).
//   • Lifetime is simpler than backend_registry: providers are assumed long-lived
//     (registered at startup, removed at shutdown) — no live-resource refcount.
//   • ir_provider_registry is in namespace lithe::ir (separate from ::lithe::execution).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <any>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../lithe_execution/foundation.hpp"  // ir_error
#include "format.hpp"                         // text_ir_view, binary_ir_view, owned_text_ir, etc.
#include "provider.hpp"                       // ir_resolution_state, no_ir_provider
#include "upgrade.hpp"                        // upgrade_registry

namespace lithe::ir {
    // =========================================================================
    //a.9 provider_descriptor — stable capability description of a provider
    //
    // Carries enough metadata for capability-based selection without running the
    // provider.  Intended for tooling / registry queries.
    // =========================================================================

    // Bitmask over stage enum (8 stages max in the current design).
    struct stage_set {
        std::uint8_t bits = 0;

        constexpr void add(const stage s) noexcept {
            bits |= static_cast<std::uint8_t>(std::uint8_t{1} << static_cast<std::uint8_t>(s));
        }

        [[nodiscard]] constexpr bool has(const stage s) const noexcept {
            return (bits >> static_cast<std::uint8_t>(s)) & 1;
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }
    };

    // Bitmask over encoding enum (4 encodings).
    struct encoding_set {
        std::uint8_t bits = 0;

        constexpr void add(const encoding e) noexcept {
            bits |= static_cast<std::uint8_t>(std::uint8_t{1} << static_cast<std::uint8_t>(e));
        }

        [[nodiscard]] constexpr bool has(const encoding e) const noexcept {
            return (bits >> static_cast<std::uint8_t>(e)) & 1;
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }
    };

    struct provider_descriptor {
        stable_ir_id id; // stable cross-process provider identity
        schema_version provider_version; // provider implementation version

        stage_set readable_stages; // stages the provider can import
        stage_set writable_stages; // stages the provider can export
        encoding_set import_encodings; // encodings accepted for import
        encoding_set export_encodings; // encodings produced on export

        bool deterministic = true; // same input → same output
        bool preserves_unknown_ops = false; // round-trips unknown optional ops
        bool supports_validation = false; // implements validate_ir CPO
        bool supports_round_trip = false; // export(import(x)) == x
    };

    // Concept: a provider that exposes a static provider_descriptor descriptor().
    template <class P>
    concept described_ir_provider =
        requires {
            { P::provider_descriptor() } -> std::convertible_to<provider_descriptor>;
        };

    // =========================================================================
    //a.9 provider_capabilities — bitmask of supported CPO operations
    // =========================================================================

    enum class provider_capability : std::uint8_t {
        import_text = 0,
        import_binary = 1,
        export_text = 2,
        export_binary = 3,
        validate = 4,
        upgrade = 5,
    };

    struct provider_capability_set {
        std::uint8_t bits = 0;

        constexpr void add(const provider_capability c) noexcept {
            bits |= static_cast<std::uint8_t>(std::uint8_t{1} << static_cast<std::uint8_t>(c));
        }

        [[nodiscard]] constexpr bool has(const provider_capability c) const noexcept {
            return (bits >> static_cast<std::uint8_t>(c)) & 1;
        }
    };

    // =========================================================================
    //a.9 erased_provider — type-erased provider ops bundle
    // =========================================================================

    struct erased_provider_ops {
        // Owning string — id may come from provider_descriptor().id.view() which
        // lives on a stack-local value; must not be a dangling string_view (G6 fix).
        std::string id;
        provider_capability_set caps;
        std::optional<provider_descriptor> descriptor; // filled if described_ir_provider

        // import_text: text_ir_view → expected<any, ir_error>
        std::function<std::expected<std::any, ::lithe::execution::ir_error>(text_ir_view)>
        import_text_fn;

        // import_binary: binary_ir_view → expected<any, ir_error>
        std::function<std::expected<std::any, ::lithe::execution::ir_error>(binary_ir_view)>
        import_binary_fn;

        // validate: any (IR) → ir_resolution_state (G7 fix: now populated when provider
        // satisfies erased_validator concept — see register_provider<P>)
        std::function<ir_resolution_state(const std::any &)>
        validate_fn;

        [[nodiscard]] bool valid() const noexcept { return !id.empty(); }
    };

    // =========================================================================
    //a.9 erased_validator — concept for providers that expose type-erased
    // validation via validate_erased(const std::any& ir).
    //
    // Providers that only expose the typed CPO (tag_invoke(validate_ir_t,...))
    // cannot be wired into erased_provider_ops::validate_fn because the IR type
    // is erased at registry registration time.  Adding validate_erased() to a
    // provider allows the registry to capture it (G7 fix).
    // =========================================================================

    template <class P>
    concept erased_validator =
        requires(const P& p, const std::any& ir) {
            { p.validate_erased(ir) } -> std::same_as<ir_resolution_state>;
        };

    // =========================================================================
    //a.9 ir_provider_registry
    //
    // Thread-safe registry keyed by provider id (string_view into static storage).
    // find() returns a const pointer; the registry owns all entries.
    // =========================================================================

    class ir_provider_registry {
    public:
        ir_provider_registry() = default;
        ~ir_provider_registry() = default;

        ir_provider_registry(const ir_provider_registry&) = delete;
        ir_provider_registry& operator=(const ir_provider_registry&) = delete;
        // Not movable — shared_mutex is non-movable; registry identity is stable.
        ir_provider_registry(ir_provider_registry&&) = delete;
        ir_provider_registry& operator=(ir_provider_registry&&) = delete;

        // ====================================================================
        // register_provider<P>(provider) — type-erased registration
        //
        // Wires all available CPOs from provider P.
        // P must have a static constexpr member `id` (std::string_view).
        // ====================================================================

        template <class P>
            requires std::move_constructible<P>
        void register_provider(P provider) {
            erased_provider_ops ops;

            auto shared = std::make_shared<P>(std::move(provider));

            // Derive id — prefer described_ir_provider (G6 fix):
            //   If P satisfies described_ir_provider, id comes from provider_descriptor().id.
            //   Otherwise fall back to static P::id member.
            // erased_provider_ops::id is std::string (owning) to avoid dangling views.
            if constexpr (described_ir_provider<P>) {
                ops.descriptor = P::provider_descriptor();
                ops.id = std::string{ops.descriptor->id.view()};
            }
            else {
                static_assert(requires { P::id; },
                              "register_provider<P>: P must satisfy described_ir_provider "
                              "or have a static P::id string_view member");
                ops.id = std::string{P::id};
            }

            // import_text if available.
            if constexpr (requires(P& p, text_ir_view v) { cpo::import_text(p, v); }) {
                ops.caps.add(provider_capability::import_text);
                ops.import_text_fn = [shared](text_ir_view view)
                    -> std::expected<std::any, ::lithe::execution::ir_error> {
                        auto r = cpo::import_text(*shared, view);
                        if (!r) return std::unexpected(r.error());
                        return std::any{std::move(*r)};
                    };
            }

            // import_binary if available.
            if constexpr (requires(P& p, binary_ir_view v) { cpo::import_binary(p, v); }) {
                ops.caps.add(provider_capability::import_binary);
                ops.import_binary_fn = [shared](binary_ir_view view)
                    -> std::expected<std::any, ::lithe::execution::ir_error> {
                        auto r = cpo::import_binary(*shared, view);
                        if (!r) return std::unexpected(r.error());
                        return std::any{std::move(*r)};
                    };
            }

            // validate — wire validate_erased if the provider exposes it (G7 fix).
            // Providers that only expose typed tag_invoke(validate_ir_t,...) cannot be
            // captured here without knowing the IR type; they should also implement
            // validate_erased(const std::any&) for registry use.
            if constexpr (erased_validator<P>) {
                ops.caps.add(provider_capability::validate);
                ops.validate_fn = [shared](const std::any& ir) -> ir_resolution_state {
                    return shared->validate_erased(ir);
                };
            }

            std::unique_lock lock{mutex_};
            providers_[ops.id] = std::move(ops);
        }

        // Manual registration of a pre-built ops bundle (for advanced use).
        void register_ops(erased_provider_ops ops) {
            std::unique_lock lock{mutex_};
            providers_[std::string{ops.id}] = std::move(ops);
        }

        // find — returns nullptr if not registered.
        [[nodiscard]] const erased_provider_ops*
        find(const std::string_view id) const noexcept {
            std::shared_lock lock{mutex_};
            const auto it = providers_.find(std::string{id});
            return it == providers_.end() ? nullptr : &it->second;
        }

        // find_by_descriptor — capability-based selection.
        // Returns the first registered provider whose descriptor satisfies all
        // of: readable_stage set, import_encoding supported, and caps match.
        // Returns nullptr if no match found.
        [[nodiscard]] const erased_provider_ops*
        find_by_descriptor(const stage required_stage,
                           const encoding required_encoding,
                           const bool need_round_trip = false) const noexcept {
            std::shared_lock lock{mutex_};
            for (const auto& [key, ops] : providers_) {
                if (!ops.descriptor) continue;
                const auto& d = *ops.descriptor;
                if (!d.readable_stages.has(required_stage)) continue;
                if (!d.import_encodings.has(required_encoding)) continue;
                if (need_round_trip && !d.supports_round_trip) continue;
                return &ops;
            }
            return nullptr;
        }

        void unregister(const std::string_view id) {
            std::unique_lock lock{mutex_};
            providers_.erase(std::string{id});
        }

        [[nodiscard]] std::size_t size() const noexcept {
            std::shared_lock lock{mutex_};
            return providers_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            std::shared_lock lock{mutex_};
            return providers_.empty();
        }

        // Convenience: also expose an upgrade_registry for schema upgrades.
        [[nodiscard]] upgrade_registry& upgrades() noexcept { return upgrades_; }
        [[nodiscard]] const upgrade_registry& upgrades() const noexcept { return upgrades_; }

    private:
        mutable std::shared_mutex mutex_;
        std::unordered_map<std::string, erased_provider_ops> providers_;
        upgrade_registry upgrades_;
    };

    // =========================================================================
    //a.9 no_ir_provider_registry — zero-cost sentinel
    // =========================================================================

    struct no_ir_provider_registry {
        static constexpr bool active = false;
    };

    static_assert(std::is_empty_v<no_ir_provider_registry>);
} // namespace lithe::ir
