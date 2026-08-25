#pragma once

// =============================================================================
// lithe_exec/exec_hint.hpp — Frontend hint + planning policy objects
//
// Namespace: lithe::exec
//
// Provides:
//   execution_hint       — soft/hard placement request from a frontend attribute
//                          (@parallel, @gpu(required=true), @sequential, ...)
//   backend_policy       — which backend class to prefer/force
//   fallback_policy      — what to do when the fast path is unavailable
//   auto_execution_policy— full host/frontend planning policy
//
// NOTE: auto_execution_policy is distinct from the engine's compile-time
//       ExecutionPolicy template parameter (constexpr/runtime/jit_execution_policy).
//       The prefix "auto_" signals it governs the automatic planning layer only.
//
// All types are trivially copyable PODs. No virtual, no macros.
// Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include "exec_kinds.hpp"

#include <cstdint>
#include <optional>
#include <type_traits>

namespace lithe::exec {
    // =========================================================================
    // backend_policy — coarse backend-class preference
    // =========================================================================

    enum class backend_policy : std::uint8_t {
        best_available = 0, // let profitability select
        force_scalar = 1, // never parallelize / vectorize
        prefer_gpu = 2, // prefer device compute when legal + profitable
        prefer_cpu = 3, // prefer CPU (scalar or threaded) over GPU
    };

    [[nodiscard]] inline constexpr std::string_view to_string(backend_policy p) noexcept {
        switch (p) {
        case backend_policy::best_available: return "best_available";
        case backend_policy::force_scalar: return "force_scalar";
        case backend_policy::prefer_gpu: return "prefer_gpu";
        case backend_policy::prefer_cpu: return "prefer_cpu";
        }
        return "best_available";
    }

    // =========================================================================
    // fallback_policy — behavior when the fast-path plan is unavailable
    // =========================================================================

    enum class fallback_policy : std::uint8_t {
        none = 0, // no fallback; emit diagnostic on failure
        safe_cpu = 1, // always produce a semantically-equivalent scalar fallback
    };

    [[nodiscard]] inline constexpr std::string_view to_string(fallback_policy p) noexcept {
        switch (p) {
        case fallback_policy::none: return "none";
        case fallback_policy::safe_cpu: return "safe_cpu";
        }
        return "safe_cpu";
    }

    // =========================================================================
    // execution_hint — soft/hard placement request from a frontend attribute
    //
    // Maps from Crank (or any other frontend) attributes to a neutral object:
    //   @parallel              → hint_parallel()
    //   @gpu(required=true)    → hint_gpu_required()
    //   @sequential            → hint_sequential()
    //   @deterministic         → hint_deterministic()
    //   @no_gpu                → hint_no_gpu()
    //   @no_parallel           → hint_no_parallel()
    // =========================================================================

    struct execution_hint {
        std::optional<execution_kind> preferred = std::nullopt;
        bool required = false; // hint is a hard constraint (emit error if violated)
        bool forbid_parallel = false;
        bool forbid_gpu = false;
        bool deterministic = false;

        // Merge this hint with another: the stricter / more specific wins.
        [[nodiscard]] constexpr execution_hint merged_with(const execution_hint& o) const noexcept {
            execution_hint out = *this;
            if (!out.preferred && o.preferred) out.preferred = o.preferred;
            out.required |= o.required;
            out.forbid_parallel |= o.forbid_parallel;
            out.forbid_gpu |= o.forbid_gpu;
            out.deterministic |= o.deterministic;
            return out;
        }
    };

    // execution_hint is not trivially copyable (holds std::optional) — intentional.

    // Named constructors — zero-overhead since they are consteval.
    [[nodiscard]] consteval execution_hint hint_parallel() noexcept {
        return {.preferred = execution_kind::threaded};
    }

    [[nodiscard]] consteval execution_hint hint_simd() noexcept {
        return {.preferred = execution_kind::simd};
    }

    [[nodiscard]] consteval execution_hint hint_gpu_required() noexcept {
        return {.preferred = execution_kind::gpu, .required = true};
    }

    [[nodiscard]] consteval execution_hint hint_sequential() noexcept {
        return {
            .preferred = execution_kind::scalar,
            .required = true,
            .forbid_parallel = true,
            .forbid_gpu = true
        };
    }

    [[nodiscard]] consteval execution_hint hint_deterministic() noexcept {
        return {.deterministic = true};
    }

    [[nodiscard]] consteval execution_hint hint_no_gpu() noexcept {
        return {.forbid_gpu = true};
    }

    [[nodiscard]] consteval execution_hint hint_no_parallel() noexcept {
        return {.forbid_parallel = true};
    }

    // =========================================================================
    // auto_execution_policy — full planning policy for auto_execution_pass
    //
    // Distinct from the engine's compile-time ExecutionPolicy template param.
    // All fields are plain bool or enum — trivially copyable.
    // =========================================================================

    struct auto_execution_policy {
        bool automatic = true; // run auto analysis; false = pass-through scalar
        bool allow_threads = true;
        bool allow_simd = true;
        bool allow_gpu = true;
        bool allow_async = true;
        bool allow_distributed = false;
        bool deterministic = false; // global determinism constraint

        backend_policy backend = backend_policy::best_available;
        fallback_policy fallback = fallback_policy::safe_cpu;

        [[nodiscard]] constexpr bool allows(execution_kind k) const noexcept {
            switch (k) {
            case execution_kind::scalar: return true;
            case execution_kind::simd: return allow_simd;
            case execution_kind::threaded: return allow_threads;
            case execution_kind::gpu: return allow_gpu;
            case execution_kind::distributed: return allow_distributed;
            }
            return false;
        }
    };

    static_assert(std::is_trivially_copyable_v<auto_execution_policy>);
} // namespace lithe::exec
