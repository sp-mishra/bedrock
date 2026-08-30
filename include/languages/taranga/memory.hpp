#pragma once

// taranga/memory.hpp — Wasm linear memory as a byte-composed memref<?xi8>.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// Wasm linear memory is a single flat, byte-addressable, page-granular buffer that
// grows at runtime. Lithe's native aggregate is the memref — a typed, strided,
// possibly-dynamic tensor view. The v1 modelling choice (locked in design) is the
// most faithful one that needs no new Lithe type: represent the whole linear
// memory as a rank-1, dynamic-extent memref of bytes — `memref<?xi8>`. Every typed
// access (i32.load, f64.store, …) is then a *byte-window* into that buffer: the
// address operand indexes bytes, the static offset immediate is added, and the
// access width (1/2/4/8 bytes) comes from the mnemonic. Wider-than-byte loads are
// therefore composite reads over consecutive byte cells — hence "byte-compose".
//
// This header is descriptor-only. It does not allocate or own memory: it produces
// the memref_type that lower_hl stamps onto load/store ops and that the engine
// uses to size and bind the backing buffer. Keeping it allocation-free preserves
// the header-only, side-effect-free contract and lets the engine own lifetime.
//
// Rationale for bytes (not one memref per element width): a single canonical byte
// buffer means loads and stores of different widths alias correctly by
// construction (i32.store then i8.load sees the low byte), which is exactly Wasm's
// untyped-memory semantics. A per-width memref model would silently lose that.

#include "languages/taranga/wasm_types.hpp"

#include "lithe/lithe_codegen.hpp"
#include "lithe/lithe_codegen_hl.hpp"

#include <cstdint>
#include <optional>

namespace taranga {

    // The v1 linear-memory descriptor: a byte memref plus the page bounds it was
    // declared with. Sizes are derived, never hardcoded (page size from
    // wasm_types::k_wasm_page_bytes).
    struct linear_memory {
        std::uint32_t min_pages = 0;
        std::optional<std::uint32_t> max_pages;

        // Byte extent implied by the minimum page count (initial buffer size).
        [[nodiscard]] std::uint64_t min_bytes() const noexcept {
            return static_cast<std::uint64_t>(min_pages) * k_wasm_page_bytes;
        }
        // Byte extent implied by the maximum page count, if bounded.
        [[nodiscard]] std::optional<std::uint64_t> max_bytes() const noexcept {
            if (!max_pages) return std::nullopt;
            return static_cast<std::uint64_t>(*max_pages) * k_wasm_page_bytes;
        }

        [[nodiscard]] static linear_memory from(const mem_type& mt) noexcept {
            return {mt.page_limits.min, mt.page_limits.max};
        }
    };

    namespace detail {

        namespace cg = lithe::codegen;
        namespace hl = lithe::codegen::hl;

        // The canonical linear-memory view: rank-1, dynamic (shape[0]==0),
        // contiguous, 8-bit integer elements — i.e. memref<?xi8>. Dynamic extent
        // because the buffer grows (memory.grow) beyond the module-declared min.
        [[nodiscard]] inline hl::memref_type byte_memref() noexcept {
            hl::memref_type m;
            m.elem_kind = cg::abstract_value_kind::integer;
            m.elem_bits = 8;
            m.rank = 1;
            m.alignment_bytes = 1; // byte buffer imposes no natural alignment
            m.shape[0] = 0;        // dynamic extent
            m.strides[0] = 1;      // one element == one byte
            m.contiguous = true;
            return m;
        }

    } // namespace detail

    // Number of byte cells a typed access of the given value_type touches. This is
    // the composition width for a byte-composed load/store: an i32 access spans 4
    // consecutive cells of the memref<?xi8>. (Sub-i32 packed loads — i32.load8_u
    // etc. — are a later band; v1 covers the natural full-width accesses.)
    [[nodiscard]] inline std::uint32_t access_byte_width(value_type vt) noexcept {
        switch (vt) {
        case value_type::i32:
        case value_type::f32: return 4;
        case value_type::i64:
        case value_type::f64: return 8;
        default:              return 4;
        }
    }

    // Build the memref_type describing linear memory for a typed access. In v1 the
    // element view is always the byte buffer; the access width informs the engine's
    // composition, not the memref shape (which stays byte-granular so widths alias).
    [[nodiscard]] inline lithe::codegen::hl::memref_type
    linear_memory_view() noexcept {
        return detail::byte_memref();
    }

} // namespace taranga
