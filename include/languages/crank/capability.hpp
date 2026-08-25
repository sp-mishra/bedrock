#pragma once

// crank/capability.hpp — Backend capability discovery (design §8).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Describes what each execution backend can do and discovers the set available
// under a given policy. Static adapters are matched by the ExecutionBackend
// concept (zero-overhead, no vtable); a type-erased `erased_backend` exists only
// for the plugin boundary. Discovery is deterministic under a fixed environment
// (design §19.13) and cached per policy fingerprint.
//
// The policy input is `execution_options` (context.hpp) — the allow_* flags and
// backend_policy that gate which backends may be offered. Taking the options
// struct rather than the whole context keeps this header light for the planner.

#include "languages/crank/exec_result.hpp"
#include "languages/crank/verify_mir.hpp"
#include "languages/crank/context.hpp"  // execution_options, backend_policy

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// GPU probe is opt-in: only query the device layer when the backend header is
// present and the build enabled a GPU backend.
#if __has_include("languages/crank/gpu_backend.hpp")
#  include "languages/crank/gpu_backend.hpp"
#  define CRANK_CAP_HAS_GPU_BACKEND 1
#endif

namespace crank {
    // ============================================================================
    // execution_kind — the coarse family of a backend (design §8)
    // ============================================================================

    enum class execution_kind : std::uint8_t {
        scalar, // interpreter / native scalar — the semantic reference
        simd, // CPU SIMD (Highway)
        threaded, // CPU parallel (Pravaha jthread pool)
        gpu, // device execution (Metal / Vulkan)
        async, // coroutine scheduler
        host, // registered host / plugin functions
    };

    [[nodiscard]] constexpr std::string_view to_string(execution_kind k) noexcept {
        switch (k) {
        case execution_kind::scalar: return "scalar";
        case execution_kind::simd: return "simd";
        case execution_kind::threaded: return "threaded";
        case execution_kind::gpu: return "gpu";
        case execution_kind::async: return "async";
        case execution_kind::host: return "host";
        }
        return "unknown";
    }

    using backend_id = std::uint32_t;

    // ============================================================================
    // backend_capabilities — what a backend supports (design §8.1)
    // ============================================================================

    struct backend_capabilities {
        std::vector<std::string> supported_types; // e.g. "i64", "f64"
        std::vector<std::string> supported_ops; // opcode names, empty = all scalar
        std::vector<std::string> address_spaces; // e.g. "host", "device"

        std::uint32_t vector_width_bits = 0;
        std::uint32_t max_threads = 0;
        std::uint32_t max_workgroup = 0;
        std::uint32_t max_shared_memory = 0;
        std::uint64_t device_memory_bytes = 0;

        bool supports_async = false;
        bool supports_cancellation = false;
        bool supports_deadlines = false;
        bool supports_transactions = false;
        bool supports_deterministic_reduction = false;
        bool supports_unified_memory = false;
    };

    // ============================================================================
    // backend_descriptor — identity + capabilities (design §8.3)
    // ============================================================================

    struct backend_descriptor {
        backend_id id = 0;
        execution_kind kind = execution_kind::scalar;
        std::string name;
        backend_capabilities caps;
    };

    // ============================================================================
    // ExecutionBackend — static adapter concept (design §8.3, zero overhead)
    //
    // A backend type advertises its descriptor at compile time and prepares/executes
    // a verified_mir. Concrete adapters are dispatched directly (no vtable); the
    // concept is the sole contract.
    // ============================================================================

    template <class B>
    concept ExecutionBackend = requires(B backend, const verified_mir& fn) {
        { B::descriptor() } -> std::same_as<backend_descriptor>;
        backend.prepare(fn);
        backend.execute(fn);
    };

    // ============================================================================
    // capability_set — the discovered backends for a policy (design §8)
    // ============================================================================

    struct capability_set {
        std::vector<backend_descriptor> backends;

        [[nodiscard]] const backend_descriptor* find(execution_kind k) const noexcept {
            for (const auto& b : backends)
                if (b.kind == k) return &b;
            return nullptr;
        }

        [[nodiscard]] bool has(execution_kind k) const noexcept { return find(k) != nullptr; }
    };

    // ============================================================================
    // built-in descriptors
    // ============================================================================

    namespace detail {
        [[nodiscard]] inline backend_descriptor scalar_descriptor() {
            backend_descriptor d;
            d.id = 1;
            d.kind = execution_kind::scalar;
            d.name = "interpreter";
            d.caps.supported_types = {"i64"};
            d.caps.address_spaces = {"host"};
            d.caps.max_threads = 1;
            d.caps.supports_cancellation = true;
            d.caps.supports_deadlines = true;
            d.caps.supports_transactions = true;
            d.caps.supports_deterministic_reduction = true;
            return d;
        }

        [[nodiscard]] inline backend_descriptor simd_descriptor() {
            backend_descriptor d;
            d.id = 2;
            d.kind = execution_kind::simd;
            d.name = "simd";
            d.caps.supported_types = {"i64", "f64"};
            d.caps.address_spaces = {"host"};
            d.caps.vector_width_bits = 128; // conservative baseline; Highway may widen
            d.caps.supports_deterministic_reduction = true;
            return d;
        }

        [[nodiscard]] inline backend_descriptor threaded_descriptor() {
            backend_descriptor d;
            d.id = 3;
            d.kind = execution_kind::threaded;
            d.name = "jthread";
            d.caps.supported_types = {"i64", "f64"};
            d.caps.address_spaces = {"host"};
            d.caps.max_threads = 0; // 0 = hardware_concurrency, resolved at runtime
            d.caps.supports_async = true;
            d.caps.supports_cancellation = true;
            d.caps.supports_deadlines = true;
            return d;
        }

        [[nodiscard]] inline backend_descriptor gpu_descriptor() {
            backend_descriptor d;
            d.id = 4;
            d.kind = execution_kind::gpu;
            d.name = "gpu";
            d.caps.supported_types = {"f32"};
            d.caps.address_spaces = {"host", "device"};
            d.caps.max_workgroup = 256;
            d.caps.supports_unified_memory = true;
            return d;
        }

        // Runtime GPU availability probe. Without a GPU backend compiled in, GPU is
        // never offered — discovery stays deterministic.
        [[nodiscard]] inline bool gpu_available() noexcept {
#ifdef CRANK_CAP_HAS_GPU_BACKEND
            return gpu_backend::available();
#else
            return false;
#endif
        }
    } // namespace detail

    // ============================================================================
    // discover_backends — deterministic, policy-gated, cached (design §8.2)
    //
    // Cache keyed on the policy's allow_* / backend_policy fingerprint. Under a
    // fixed environment + policy this returns the same set every call. The scalar
    // backend is always present (semantic reference, design §7.1).
    // ============================================================================

    [[nodiscard]] inline capability_set
    build_capability_set(const execution_options& opts) {
        capability_set set;

        // Scalar reference is always available.
        set.backends.push_back(detail::scalar_descriptor());

        if (opts.allow_simd)
            set.backends.push_back(detail::simd_descriptor());
        if (opts.allow_threads)
            set.backends.push_back(detail::threaded_descriptor());
        if (opts.allow_gpu && detail::gpu_available())
            set.backends.push_back(detail::gpu_descriptor());

        // backend_policy narrows the offered set.
        if (opts.backend == backend_policy::inline_only) {
            capability_set narrowed;
            if (const auto* s = set.find(execution_kind::scalar)) narrowed.backends.push_back(*s);
            return narrowed;
        }
        if (opts.backend == backend_policy::threaded_only) {
            capability_set narrowed;
            if (const auto* s = set.find(execution_kind::scalar)) narrowed.backends.push_back(*s);
            if (const auto* t = set.find(execution_kind::threaded)) narrowed.backends.push_back(*t);
            return narrowed;
        }
        return set;
    }

    namespace detail {
        // Fingerprint the policy fields that affect discovery.
        [[nodiscard]] inline std::uint64_t policy_fingerprint(const execution_options& o) noexcept {
            std::uint64_t f = 0;
            auto bit = [&](bool b) { f = (f << 1) | (b ? 1u : 0u); };
            bit(o.allow_simd);
            bit(o.allow_gpu);
            bit(o.allow_threads);
            bit(o.allow_async);
            bit(o.allow_distributed);
            bit(o.use_pravaha);
            f = (f << 8) | static_cast<std::uint64_t>(o.backend);
            // GPU availability is environmental; fold it in so the cache invalidates
            // when a device appears/disappears between policies.
            f = (f << 1) | (gpu_available() ? 1u : 0u);
            return f;
        }
    } // namespace detail

    // Cached discovery. Recomputes only when the policy fingerprint changes.
    [[nodiscard]] inline const capability_set&
    discover_backends(const execution_options& opts) {
        static std::uint64_t cached_fp = ~std::uint64_t{0};
        static capability_set cached;
        const std::uint64_t fp = detail::policy_fingerprint(opts);
        if (fp != cached_fp) {
            cached = build_capability_set(opts);
            cached_fp = fp;
        }
        return cached;
    }
} // namespace crank
