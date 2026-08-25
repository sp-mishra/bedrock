// =============================================================================
// test_crank_simd_legality.cpp — loop dependence / alias analysis (design §10).
//
// Covers:
//   1. Distinct-base read + write → distinct_base, vectorizable.
//   2. Same base, a[i] vs a[i+1] write → illegal (loop-carried dependence).
//   3. Same base, same subscript → affine_provable (same-iteration, safe).
//   4. Trip / tail sizing: floor((end-begin)/W)*W and the remainder.
//   5. Reduction recognized + carried; associative op stays legal.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/simd_legality.hpp"

using namespace crank;

namespace {
    affine_subscript acc(std::uint32_t base, std::int64_t off, std::int64_t coeff, bool write) {
        affine_subscript s;
        s.base_id = base;
        s.base = off;
        s.coeff = coeff;
        s.is_write = write;
        return s;
    }
} // namespace

TEST_CASE (

"distinct bases never alias"
,
"[crank][simd_legality]"
)
 {
    simd_loop l;
    l.begin = 0; l.end = 100;
    l.accesses = {acc(1, 0, 1, false), acc(2, 0, 1, true)};
    auto r = analyze_simd_legality(l, 4);
    REQUIRE(r.tier == dependence_tier::distinct_base);
    REQUIRE(is_vectorizable(r.tier));
    REQUIRE(r.ok());
}

TEST_CASE (

"a[i] vs a[i+1] write is a loop-carried dependence"
,
"[crank][simd_legality]"
)
 {
    simd_loop l;
    l.begin = 0; l.end = 100;
    l.accesses = {acc(1, 0, 1, false), acc(1, 1, 1, true)};
    auto r = analyze_simd_legality(l, 4);
    REQUIRE(r.tier == dependence_tier::illegal);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error->kind == execution_error_kind::simd_alias_violation);
}

TEST_CASE (

"same subscript is affine_provable"
,
"[crank][simd_legality]"
)
 {
    simd_loop l;
    l.begin = 0; l.end = 100;
    l.accesses = {acc(1, 0, 1, false), acc(1, 0, 1, true)};
    auto r = analyze_simd_legality(l, 4);
    REQUIRE(r.tier == dependence_tier::affine_provable);
    REQUIRE(is_vectorizable(r.tier));
}

TEST_CASE (

"trip and tail sizing"
,
"[crank][simd_legality]"
)
 {
    simd_loop l;
    l.begin = 0; l.end = 103;  // 103 trips, W=4 → 100 vector + 3 tail
    l.accesses = {acc(1, 0, 1, false)};
    auto r = analyze_simd_legality(l, 4);
    REQUIRE(r.vector_trip == 100);
    REQUIRE(r.scalar_tail == 3);
}

TEST_CASE (

"associative reduction is recognized and legal"
,
"[crank][simd_legality]"
)
 {
    simd_loop l;
    l.begin = 0; l.end = 64;
    l.accesses = {acc(1, 0, 1, false)};
    l.reduction = reduction_op::add;
    auto r = analyze_simd_legality(l, 8);
    REQUIRE(r.reduction.has_value());
    REQUIRE(*r.reduction == reduction_op::add);
    REQUIRE(r.ok());
    REQUIRE(r.vector_trip == 64);
    REQUIRE(r.scalar_tail == 0);
}
