#pragma once

// =============================================================================
// lithe_exec/layout_summary.hpp — Memory layout / address-space rollup
//
// Namespace: lithe::exec
//
// Provides:
//   address_space    — host / device / shared / constant / unknown
//   layout_summary   — per-region: dims, strides, alignment, contiguity,
//                      device residency. Derived from memref_type analysis.
//
// Design:
//   - Fixed arrays (max_rank = task_decomposition_plan::max_rank = 8).
//   - Trivially copyable POD — safe for handoff to Pravaha.
//   - dims/strides/contiguous derived from memref_type.
//   - device_resident / address_space default to host/unknown; become guard
//     predicates when unprovable statically.
//   - No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lithe::exec {
    // =========================================================================
    // address_space — memory address space classification
    // =========================================================================

    enum class address_space : std::uint8_t {
        host = 0, // standard host (CPU) memory
        device = 1, // GPU-local device memory
        shared = 2, // GPU shared memory (block-local)
        constant = 3, // GPU constant memory
        unknown = 4,
    };

    [[nodiscard]] inline constexpr std::string_view to_string(address_space s) noexcept {
        switch (s) {
        case address_space::host: return "host";
        case address_space::device: return "device";
        case address_space::shared: return "shared";
        case address_space::constant: return "constant";
        case address_space::unknown: return "unknown";
        }
        return "unknown";
    }

    // =========================================================================
    // layout_summary — POD layout descriptor for one memref
    //
    // max_rank = 8, matching task_decomposition_plan::max_rank.
    // =========================================================================

    struct layout_summary {
        static constexpr std::uint8_t max_rank = 8;

        std::uint8_t rank = 0;
        std::array<std::int64_t, max_rank> dims{}; // extent per dimension
        std::array<std::int64_t, max_rank> strides{}; // stride per dimension (in elements)
        std::uint32_t alignment = 1; // byte alignment of base pointer
        address_space space = address_space::unknown;
        bool contiguous = false; // strides form a row-major dense layout
        bool device_resident = false; // data currently on device

        // True iff the layout is provably unit-stride in the innermost dimension.
        [[nodiscard]] constexpr bool is_innermost_unit() const noexcept {
            return rank > 0 && strides[rank - 1] == 1;
        }

        // True iff SIMD vectorization is feasible (unit innermost + aligned).
        [[nodiscard]] constexpr bool simd_eligible(std::uint32_t vector_width_bytes) const noexcept {
            return contiguous && is_innermost_unit() && alignment >= vector_width_bytes;
        }
    };

    static_assert(std::is_trivially_copyable_v<layout_summary>);
    static_assert(std::is_standard_layout_v<layout_summary>);
} // namespace lithe::exec
