#pragma once

// =============================================================================
// lithe_exec/exec_bridge.hpp — bridge between lithe::exec and lithe::execution
//
// Provides constexpr conversion between the two parallel execution enumerations:
//
//   lithe::exec::execution_kind  — parallelism model (WHAT: analysis & planning)
//   lithe::execution::execution_mode — backend compile strategy (HOW: infra)
//
// These are orthogonal axes:
//   execution_kind answers "what parallelism model does this region use?"
//   execution_mode answers "how should the backend compile / install the code?"
//
// The mapping is a best-fit heuristic, not a bijection:
//   scalar      → interpret   (low overhead, no JIT needed for scalar fallback)
//   simd        → jit_tier1   (fast JIT; SIMD vectorization added at codegen)
//   threaded    → jit_tier1   (fast JIT; thread dispatch added at codegen)
//   gpu         → device      (GPU device compute tier)
//   distributed → out_of_proc (isolated execution for distributed dispatch)
//
// Callers may override: the bridge provides a sensible default, not a mandate.
//
// No virtual, no macros. Header-only C++23. Opt-in (not in lithe.hpp).
// =============================================================================

#include "exec_kinds.hpp"
#include "../lithe_execution/foundation.hpp"

namespace lithe::exec {
    // =========================================================================
    // to_execution_mode — best-fit mapping from execution_kind to execution_mode
    // =========================================================================

    [[nodiscard]] constexpr lithe::execution::execution_mode
    to_execution_mode(execution_kind kind) noexcept {
        switch (kind) {
        case execution_kind::scalar: return lithe::execution::execution_mode::interpret;
        case execution_kind::simd: return lithe::execution::execution_mode::jit_tier1;
        case execution_kind::threaded: return lithe::execution::execution_mode::jit_tier1;
        case execution_kind::gpu: return lithe::execution::execution_mode::device;
        case execution_kind::distributed: return lithe::execution::execution_mode::out_of_proc;
        }
        return lithe::execution::execution_mode::interpret; // unreachable; scalar fallback
    }

    // =========================================================================
    // to_execution_kind — reverse mapping (approximate; not injective)
    //
    // Returns the most natural execution_kind for a given execution_mode.
    // jit_tier1 / jit_tier2 / aot / native_inline map to scalar (lowest
    // assumption); callers with more context should not rely on this.
    // =========================================================================

    [[nodiscard]] constexpr execution_kind
    to_execution_kind(lithe::execution::execution_mode mode) noexcept {
        switch (mode) {
        case lithe::execution::execution_mode::interpret: return execution_kind::scalar;
        case lithe::execution::execution_mode::jit_tier1: return execution_kind::simd;
        case lithe::execution::execution_mode::jit_tier2: return execution_kind::simd;
        case lithe::execution::execution_mode::aot: return execution_kind::scalar;
        case lithe::execution::execution_mode::native_inline: return execution_kind::scalar;
        case lithe::execution::execution_mode::device: return execution_kind::gpu;
        case lithe::execution::execution_mode::out_of_proc: return execution_kind::distributed;
        }
        return execution_kind::scalar;
    }

    // =========================================================================
    // Compile-time sanity: round-trip gpu/distributed must survive
    // =========================================================================

    static_assert(to_execution_mode(execution_kind::gpu)
                  == lithe::execution::execution_mode::device,
                  "gpu → device bridge invariant");
    static_assert(to_execution_mode(execution_kind::distributed)
                  == lithe::execution::execution_mode::out_of_proc,
                  "distributed → out_of_proc bridge invariant");
    static_assert(to_execution_kind(lithe::execution::execution_mode::device)
                  == execution_kind::gpu,
                  "device → gpu reverse bridge invariant");
} // namespace lithe::exec
