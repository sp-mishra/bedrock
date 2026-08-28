#pragma once

// =============================================================================
// lithe_exec/memory_summary.hpp — Memory access rollup over HL MIR regions
//
// Namespace: lithe::exec
//
// Provides:
//   access_kind      — read / write / read_write
//   stride_info      — per-dimension stride (affine coefficient + constant)
//   memory_access    — one memref access: base, affine index, kind, stride, layout
//   alias_summary    — alias relation built over PDG alias results
//   memory_summary   — rollup: reads + writes + alias + unknown-alias flag
//
//   extract_memory_summary(fn, pdg, poly) — builds a memory_summary from HL MIR
//
// Design:
//   - Affine index/range encoded as (coeff × iv + offset) pair — mirrors
//     poly::loop_bounds without pulling the full poly header.
//   - alias_summary wraps PDG alias results; does NOT recompute alias analysis.
//   - No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lithe::exec {
    // =========================================================================
    // access_kind
    // =========================================================================

    enum class access_kind : std::uint8_t {
        read = 0,
        write = 1,
        read_write = 2,
    };

    [[nodiscard]] inline constexpr std::string_view to_string(access_kind k) noexcept {
        switch (k) {
        case access_kind::read: return "read";
        case access_kind::write: return "write";
        case access_kind::read_write: return "read_write";
        }
        return "read";
    }

    // =========================================================================
    // stride_info — linear stride: coefficient * iv + offset (per dimension)
    // =========================================================================

    struct stride_info {
        std::int64_t coeff = 1; // stride coefficient (1 = unit stride)
        std::int64_t offset = 0;
        bool known = true; // false = symbolic / non-affine stride
    };

    static_assert(std::is_trivially_copyable_v<stride_info>);

    // =========================================================================
    // affine_index — linear affine form: coeff * iv_id + constant
    //
    // Describes one access index dimension using existing IV ids from poly.
    // =========================================================================

    struct affine_index {
        std::uint32_t iv_id = 0; // induction variable preg id (0 = constant)
        std::int64_t coeff = 1;
        std::int64_t constant = 0;
        bool is_affine = true; // false = symbolic / indirect

        [[nodiscard]] constexpr bool is_unit_stride() const noexcept {
            return is_affine && coeff == 1 && constant == 0;
        }
    };

    static_assert(std::is_trivially_copyable_v<affine_index>);

    // =========================================================================
    // memory_access — one memref access in a region
    // =========================================================================

    struct memory_access {
        std::uint32_t base_id = 0; // base memref value id (from HL MIR)
        std::uint32_t instr_id = 0; // source instruction id
        access_kind kind = access_kind::read;
        affine_index index; // primary dimension index
        stride_info stride;
        std::uint32_t layout_id = 0; // layout_summary id (0 = none)

        [[nodiscard]] constexpr bool is_read() const noexcept {
            return kind == access_kind::read || kind == access_kind::read_write;
        }

        [[nodiscard]] constexpr bool is_write() const noexcept {
            return kind == access_kind::write || kind == access_kind::read_write;
        }
    };

    // =========================================================================
    // alias_pair — a pair of accesses that may alias
    // =========================================================================

    struct alias_pair {
        std::uint32_t access_a = 0; // index into memory_summary::reads/writes
        std::uint32_t access_b = 0;
        bool proven_no_alias = false; // PDG proved disjoint
    };

    // =========================================================================
    // alias_summary — alias relation over the region's accesses
    // =========================================================================

    struct alias_summary {
        std::vector<alias_pair> pairs;
        bool has_unknown_aliasing = false;

        [[nodiscard]] bool all_no_alias() const noexcept {
            if (has_unknown_aliasing) return false;
            for (const auto& p : pairs)
                if (!p.proven_no_alias) return false;
            return true;
        }
    };

    // =========================================================================
    // memory_summary — full memory access rollup for one region
    // =========================================================================

    struct memory_summary {
        std::vector<memory_access> reads;
        std::vector<memory_access> writes;
        alias_summary aliases;

        [[nodiscard]] bool has_unknown_aliasing() const noexcept {
            return aliases.has_unknown_aliasing;
        }

        // True iff there are write accesses (needed for threaded legality).
        [[nodiscard]] bool has_writes() const noexcept { return !writes.empty(); }

        // True iff all accesses are provably non-aliasing.
        [[nodiscard]] bool proven_no_alias() const noexcept {
            return aliases.all_no_alias();
        }
    };
} // namespace lithe::exec
