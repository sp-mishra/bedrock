// =============================================================================
// test_taranga_memory.cpp — Wasm linear memory as memref<?xi8>.
//
// Verifies: include/languages/taranga/memory.hpp
//           include/languages/taranga/wasm_types.hpp
//
//   1. linear_memory::min_bytes / max_bytes derive from k_wasm_page_bytes.
//   2. linear_memory::from(mem_type) copies page limits.
//   3. unbounded memory has no max_bytes.
//   4. access_byte_width: i32/f32 → 4, i64/f64 → 8.
//   5. linear_memory_view() is a rank-1 dynamic byte memref.
//   6. the byte memref is contiguous, 8-bit integer, unit-strided.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/taranga/memory.hpp"
#include "languages/taranga/wasm_types.hpp"

using namespace taranga;

// ============================================================================
// Test 1 — byte extents derive from the page size
// ============================================================================

TEST_CASE("taranga: linear_memory byte extents", "[taranga][memory]") {
    linear_memory lm{2u, std::optional<std::uint32_t>{4u}};
    CHECK(lm.min_bytes() == 2ull * k_wasm_page_bytes);
    REQUIRE(lm.max_bytes().has_value());
    CHECK(*lm.max_bytes() == 4ull * k_wasm_page_bytes);
}

// ============================================================================
// Test 2 — from(mem_type) copies the page limits
// ============================================================================

TEST_CASE("taranga: linear_memory from mem_type", "[taranga][memory]") {
    mem_type mt;
    mt.page_limits.min = 3u;
    mt.page_limits.max = std::optional<std::uint32_t>{9u};

    auto lm = linear_memory::from(mt);
    CHECK(lm.min_pages == 3u);
    REQUIRE(lm.max_pages.has_value());
    CHECK(*lm.max_pages == 9u);
}

// ============================================================================
// Test 3 — unbounded memory: no max
// ============================================================================

TEST_CASE("taranga: linear_memory unbounded", "[taranga][memory]") {
    linear_memory lm{1u, std::nullopt};
    CHECK(lm.min_bytes() == static_cast<std::uint64_t>(k_wasm_page_bytes));
    CHECK_FALSE(lm.max_bytes().has_value());
}

// ============================================================================
// Test 4 — access byte widths per value_type
// ============================================================================

TEST_CASE("taranga: access_byte_width", "[taranga][memory]") {
    CHECK(access_byte_width(value_type::i32) == 4u);
    CHECK(access_byte_width(value_type::f32) == 4u);
    CHECK(access_byte_width(value_type::i64) == 8u);
    CHECK(access_byte_width(value_type::f64) == 8u);
}

// ============================================================================
// Test 5 — linear_memory_view() shape/rank
// ============================================================================

TEST_CASE("taranga: linear_memory_view is dynamic byte memref", "[taranga][memory]") {
    auto m = linear_memory_view();
    CHECK(m.rank == 1);
    CHECK(m.elem_bits == 8);
    CHECK(m.shape[0] == 0);   // dynamic extent — memory.grow
}

// ============================================================================
// Test 6 — byte memref is contiguous, integer, unit-strided
// ============================================================================

TEST_CASE("taranga: linear_memory_view element traits", "[taranga][memory]") {
    namespace cg = lithe::codegen;
    auto m = linear_memory_view();
    CHECK(m.elem_kind == cg::abstract_value_kind::integer);
    CHECK(m.strides[0] == 1);
    CHECK(m.contiguous);
    CHECK(m.alignment_bytes == 1);
}
