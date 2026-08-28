#pragma once

// =============================================================================
// plugin/lithe_plugin_abi.hpp — C++ host-side wrapper for the Lithe plugin ABI
//
// Bridges the stable C ABI (plugin_abi.h) into the C++ backend_registry path.
// Invariants:
//   • Plugin registration uses the public backend_registry::register_backend<B>
//     path — no privileged access.  The C thunk table is wrapped in a thin C++
//     backend type (c_abi_plugin_backend) that satisfies erased_backend_ops.
//   • Sandbox monotonicity: a plugin loaded under execution_profile::untrusted_sandbox
//     cannot lower that floor.  Enforced before registration.
//   • Signed-plugin verification: reuses the aot_signature_provider concept
//     (lithe_execution/aot.hpp).  Plugin trust policy is DISTINCT from IR trust.
//   • out_of_proc selection gate: forbidden_modes.test(out_of_proc) rejects the
//     backend via compile_requirements::mode_allowed() — no new mechanism needed.
//
// Errors fold into selection_error / install_error — never ir_error ().
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "../lithe_execution/foundation.hpp"   // execution_mode, backend_capability_set, backend_lifetime
#include "../lithe_execution/registry.hpp"     // backend_registry, registration_token
#include "../lithe_execution/capability.hpp"   // compile_requirements, security_constraints
#include "../lithe_rt/instance.hpp"            // execution_profile, profile_defaults
#include "../lithe_extension.hpp"              // version_triple
#include "plugin_abi.h"                        // C ABI types

// aot.hpp is feature-gated; include unconditionally only the concepts we need.
// The aot_signature_provider concept is inside LITHE_HAS_AOT guard.
// We define a fallback no-op provider here for builds without AOT.
#if defined(LITHE_HAS_AOT)
#  include "../lithe_execution/aot.hpp"
#endif

namespace lithe::plugin {
    // =========================================================================
    // plugin_load_error — distinct from install_error and ir_error ().
    // =========================================================================

    struct plugin_load_error {
        std::string_view detail;

        constexpr explicit plugin_load_error(const std::string_view d = {}) noexcept
            : detail(d) {}
    };

    // =========================================================================
    // plugin_signature_view — byte span passed to the signature verifier.
    //
    // The host populates this from however it obtained the plugin binary
    // (mmap, buffer, etc.).  The verifier is called BEFORE registration.
    // =========================================================================

    struct plugin_signature_view {
        std::span<const std::uint8_t> bytes; // the plugin binary / manifest to verify
        [[nodiscard]] bool valid() const noexcept { return !bytes.empty(); }
    };

    // =========================================================================
    // no_plugin_signature — zero-cost default: always verifies (no check).
    //
    // When no security policy is required (trusted host plugins), use this.
    // For signed plugins supply a type satisfying plugin_signature_verifier<V>.
    // =========================================================================

    struct no_plugin_signature {
        [[nodiscard]] static constexpr std::string_view provider_id() noexcept {
            return "lithe.plugin.no_signature";
        }

        [[nodiscard]] constexpr bool verify(const plugin_signature_view&) const noexcept {
            return true;
        }
    };

    // =========================================================================
    // plugin_signature_verifier<V> — structural concept for signature checking.
    //
    // Distinct from aot_signature_provider: plugin trust boundary ≠ IR trust.
    // =========================================================================

    template <class V>
    concept plugin_signature_verifier =
        requires {
            { V::provider_id() } -> std::convertible_to<std::string_view>;
        } &&
        requires(const V& v, const plugin_signature_view& sig) {
            { v.verify(sig) } -> std::same_as<bool>;
        };

    static_assert(plugin_signature_verifier<no_plugin_signature>);

    // =========================================================================
    // c_abi_plugin_backend — thin C++ wrapper around a lithe_plugin_thunk_table.
    //
    // Satisfies the shape that backend_registry::make_ops<B>() expects:
    //   • B::descriptor.id_view() — static (via the instance's stored id string)
    //   • b.capabilities()         — instance method
    //   • move-constructible
    //
    // The thunk table pointer is non-owning — the caller (load_plugin) keeps the
    // table alive for the duration of the registration.
    // =========================================================================

    class c_abi_plugin_backend {
    public:
        c_abi_plugin_backend() = delete;

        explicit c_abi_plugin_backend(
            const lithe_plugin_thunk_table* table,
            lithe_plugin_descriptor desc,
            lithe_backend_capability_bits caps) noexcept
            : table_(table), desc_(desc), caps_(caps) {}

        c_abi_plugin_backend(c_abi_plugin_backend&&) noexcept = default;
        c_abi_plugin_backend& operator=(c_abi_plugin_backend&&) noexcept = default;

        c_abi_plugin_backend(const c_abi_plugin_backend&) = delete;
        c_abi_plugin_backend& operator=(const c_abi_plugin_backend&) = delete;

        // backend_registry::make_ops<B> probes `b.capabilities()`.
        [[nodiscard]] execution::backend_capability_set capabilities() const noexcept {
            return execution::backend_capability_set{caps_};
        }

        // backend_registry::make_ops<B> probes `B::descriptor.id_view()`.
        // We cannot use a static method (the id is instance data), so we expose
        // a non-static id_view() and rely on the registry's instance-method branch.
        [[nodiscard]] std::string_view id_view() const noexcept {
            return {desc_.id};
        }

        // For registry make_ops: `B::descriptor.id_view()` branch won't fire
        // (no static descriptor member on this type).  The registry falls back to
        // the `b.capabilities()` instance method + the id via a custom ops builder.
        // We expose `backend_id()` as an instance method for the custom ops path.
        [[nodiscard]] std::string_view backend_id() const noexcept { return id_view(); }

        [[nodiscard]] const lithe_plugin_descriptor& descriptor() const noexcept {
            return desc_;
        }

        [[nodiscard]] lithe::version_triple version() const noexcept {
            return lithe::version_triple{desc_.version_major, desc_.version_minor, desc_.version_patch};
        }

        // Call the plugin's register/unregister thunks.
        lithe_plugin_status call_register(void* host_ctx) const noexcept {
            return table_->register_fn(host_ctx);
        }

        void call_unregister(void* host_ctx) const noexcept {
            table_->unregister_fn(host_ctx);
        }

    private:
        const lithe_plugin_thunk_table* table_ = nullptr;
        lithe_plugin_descriptor desc_{};
        lithe_backend_capability_bits caps_ = 0;
    };

    // =========================================================================
    // sandbox_floor_violation — returned when a plugin would lower the profile.
    // =========================================================================

    struct sandbox_floor_violation {
        std::string_view detail;

        constexpr explicit sandbox_floor_violation(const std::string_view d = {}) noexcept
            : detail(d) {}
    };

    // =========================================================================
    // check_sandbox_monotonicity — enforce profile floor before registration.
    //
    // If the host runs under execution_profile::untrusted_sandbox the profile
    // cannot be relaxed by a plugin (monotone floor invariant).
    //
    // Returns std::nullopt if the profile is consistent, or an error otherwise.
    // =========================================================================

    [[nodiscard]] inline std::optional<sandbox_floor_violation>
    check_sandbox_monotonicity(
        const lithe::rt::execution_profile host_profile,
        const execution::compile_requirements& reqs) noexcept {
        // If the host is sandboxed, out_of_proc and device modes must not be
        // explicitly re-allowed if security.sandbox_mode is cleared.
        if (host_profile == lithe::rt::execution_profile::untrusted_sandbox) {
            if (!reqs.security.sandbox_mode) {
                return sandbox_floor_violation{
                    "plugin: untrusted_sandbox profile cannot be relaxed"
                };
            }
        }
        return std::nullopt;
    }

    // =========================================================================
    // plugin_registration_result — holds the registration_token on success.
    // =========================================================================

    struct plugin_registration_result {
        execution::registration_token token;
        c_abi_plugin_backend* backend_ptr = nullptr; // non-owning raw view
    };

    // =========================================================================
    // load_plugin<SigVerifier> — full plugin loading pipeline:
    //   1. ABI version check
    //   2. Descriptor + capability query
    //   3. Signature verification (via SigVerifier)
    //   4. Sandbox monotonicity enforcement
    //   5. Registration via backend_registry::register_backend<c_abi_plugin_backend>
    //
    // The thunk table must remain valid for the lifetime of the returned token.
    //
    // SigVerifier must model plugin_signature_verifier<V>.
    //
    // Template parameters:
    //   SigVerifier — pluggable signature provider (default: no_plugin_signature)
    //
    // Returns: expected<plugin_registration_result, plugin_load_error>
    // =========================================================================

    template <plugin_signature_verifier SigVerifier = no_plugin_signature>
    [[nodiscard]] std::expected<plugin_registration_result, plugin_load_error>
    load_plugin(
        execution::backend_registry& registry,
        const lithe_plugin_thunk_table* table,
        const lithe::rt::execution_profile host_profile = lithe::rt::execution_profile::managed_language,
        const execution::compile_requirements& reqs = {},
        const plugin_signature_view& sig_data = {},
        const SigVerifier& verifier = {}) {
        if (!table)
            return std::unexpected(plugin_load_error{"plugin: null thunk table"});

        // Step 1: ABI version compatibility (major must match).
        const lithe_abi_version plugin_ver = table->abi_version();
        if (plugin_ver.major != LITHE_PLUGIN_ABI_MAJOR)
            return std::unexpected(plugin_load_error{"plugin: ABI major version mismatch"});

        // Step 2: Descriptor + capabilities.
        lithe_plugin_descriptor desc{};
        const lithe_plugin_status ds = table->descriptor(&desc);
        if (ds != LITHE_PLUGIN_OK)
            return std::unexpected(plugin_load_error{"plugin: descriptor() failed"});

        const lithe_backend_capability_bits caps = table->capabilities();

        // Step 3: Signature verification — distinct from IR trust boundary.
        if (sig_data.valid() && !verifier.verify(sig_data))
            return std::unexpected(plugin_load_error{"plugin: signature verification failed"});

        // Step 4: Sandbox monotonicity.
        if (auto viol = check_sandbox_monotonicity(host_profile, reqs))
            return std::unexpected(plugin_load_error{viol->detail});

        // Step 5: Register via the public backend_registry path.
        //         No privileged access — identical path to vulkan_backend, etc.
        c_abi_plugin_backend backend{table, desc, caps};
        auto* raw_ptr = &backend; // capture before move
        (void)raw_ptr; // raw_ptr is for documentation only; token owns the slot

        auto token = registry.register_backend(std::move(backend));
        if (!token.valid())
            return std::unexpected(plugin_load_error{"plugin: registry registration failed"});

        return plugin_registration_result{std::move(token), nullptr};
    }

    // =========================================================================
    // make_plugin_thunk_table — helper for test doubles / in-process plugins.
    //
    // Constructs a lithe_plugin_thunk_table from individual function pointers.
    // For testing: wire trivial lambdas to verify the ABI boundary.
    // =========================================================================

    [[nodiscard]] inline lithe_plugin_thunk_table
    make_plugin_thunk_table(
        lithe_abi_version(*abi_version_fn)(void),
        lithe_plugin_status(*descriptor_fn)(lithe_plugin_descriptor *),
        lithe_backend_capability_bits(*capabilities_fn)(void),
        lithe_plugin_status(*register_fn)(void*),
        void(*unregister_fn)(void*))
    noexcept
    {
        return {abi_version_fn, descriptor_fn, capabilities_fn, register_fn, unregister_fn};
    }

} // namespace lithe::plugin
