#pragma once

// crank/simd_legality.hpp — Loop dependence / alias analysis for SIMD (design §10).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Decides whether a counted loop may be vectorized, and how much of it. This is
// analysis only — Highway lowering is a noted follow-up (design §10.5). The
// planner (plan.hpp) consumes a simd_legality_result to admit or reject the SIMD
// backend candidate and to size the vector body vs. the scalar tail.
//
// Dependence model (design §10.2): array accesses are described as affine
// subscripts `base + coeff * iv` over a single induction variable. Two accesses
// to DISTINCT base pointers cannot alias (distinct_base). Same base with a
// provably non-overlapping affine range is non_overlapping / affine_provable.
// Anything that might overlap needs a runtime guard; a proven overlap with a
// loop-carried dependence is illegal. Reductions are recognized via parallel.hpp
// (reduction_op) and remain legal because the ops there are associative.
//
// Trip counting (design §10.3): for [begin, end) step 1 and vector width W,
// vector_trip = floor((end-begin)/W)*W, scalar_tail = (end-begin) - vector_trip.

#include "languages/crank/parallel.hpp"     // reduction_op
#include "languages/crank/exec_result.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace crank {
    // ============================================================================
    // dependence_tier — the legality verdict, ordered safest → unsafe (design §10.2)
    // ============================================================================

    enum class dependence_tier : std::uint8_t {
        distinct_base, // accesses target different bases — provably no alias
        non_overlapping, // same base, disjoint affine ranges — safe
        affine_provable, // same base, affine test proves no loop-carried dep
        needs_runtime_guard, // may overlap — vectorize only under a runtime alias check
        illegal, // proven loop-carried dependence — cannot vectorize
    };

    [[nodiscard]] constexpr std::string_view to_string(dependence_tier t) noexcept {
        switch (t) {
        case dependence_tier::distinct_base: return "distinct_base";
        case dependence_tier::non_overlapping: return "non_overlapping";
        case dependence_tier::affine_provable: return "affine_provable";
        case dependence_tier::needs_runtime_guard: return "needs_runtime_guard";
        case dependence_tier::illegal: return "illegal";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr bool is_vectorizable(dependence_tier t) noexcept {
        return t != dependence_tier::illegal;
    }

    // ============================================================================
    // affine_subscript — base + coeff*iv access into `base_id` (design §10.2)
    // ============================================================================

    struct affine_subscript {
        std::uint32_t base_id = 0; // identity of the base pointer/array
        std::int64_t base = 0; // constant offset
        std::int64_t coeff = 1; // stride in elements per iv step
        std::uint32_t index_var = 0; // induction-variable id
        std::uint32_t elem_size = 1; // element size in bytes
        bool is_write = false;
    };

    // ============================================================================
    // simd_loop — the counted loop under analysis (design §10.1)
    //
    // crank-local descriptor. begin/end are the [begin,end) trip bounds; accesses
    // are the memory subscripts inside the body. Build one from a lithe
    // structured_for's iv_bounds via from_iv_bounds() below.
    // ============================================================================

    struct simd_loop {
        std::int64_t begin = 0;
        std::int64_t end = 0; // exclusive
        std::int64_t step = 1;
        std::vector<affine_subscript> accesses;
        std::optional<reduction_op> reduction; // set if body is a reduction
    };

    // ============================================================================
    // simd_legality_result — verdict + vectorization sizing (design §10.3)
    // ============================================================================

    struct simd_legality_result {
        dependence_tier tier = dependence_tier::illegal;
        std::uint32_t vector_width = 0; // lanes chosen
        std::uint64_t vector_trip = 0; // iterations run vectorized
        std::uint64_t scalar_tail = 0; // remainder run scalar
        std::optional<reduction_op> reduction;
        std::optional<execution_error> error;

        [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
    };

    // ============================================================================
    // detail — dependence test
    // ============================================================================

    namespace detail {
        // Two same-base accesses (at least one a write) carry a loop-carried dependence
        // if their affine functions can collide at a nonzero iteration distance. For the
        // single-iv affine form base+coeff*iv, a write-after-read/write hazard exists
        // when the constant offsets differ by a nonzero multiple of the (shared) stride
        // — i.e. a[i] and a[i+k], k≠0. Equal subscripts (same base, same coeff, same
        // offset) are a same-iteration access → safe to vectorize.
        [[nodiscard]] inline dependence_tier
        classify_pair(const affine_subscript& x, const affine_subscript& y) noexcept {
            if (x.base_id != y.base_id) return dependence_tier::distinct_base;
            if (!x.is_write && !y.is_write) return dependence_tier::distinct_base; // read/read never conflicts

            // Same base, at least one write.
            if (x.coeff == y.coeff) {
                const std::int64_t delta = x.base - y.base;
                if (delta == 0) return dependence_tier::affine_provable; // same element each iter
                // a[i] vs a[i+k], k≠0 with unit-ish stride: loop-carried dependence.
                if (x.coeff != 0 && (delta % x.coeff) == 0) return dependence_tier::illegal;
                return dependence_tier::non_overlapping; // offsets never align on the stride grid
            }
            // Different strides over the same base: may cross — needs a runtime guard.
            return dependence_tier::needs_runtime_guard;
        }
    } // namespace detail

    // ============================================================================
    // analyze_simd_legality — the analysis entry point (design §10)
    //
    // Walks every access pair, taking the WORST (highest) tier. Then, unless illegal,
    // computes the vector trip / scalar tail for the requested width. A recognized
    // reduction is carried through and keeps the loop legal (associative op).
    // ============================================================================

    [[nodiscard]] inline simd_legality_result
    analyze_simd_legality(const simd_loop& loop, std::uint32_t vector_width) {
        simd_legality_result r;
        r.vector_width = vector_width;
        r.reduction = loop.reduction;

        // A recognized reduction must use an associative op (parallel.hpp guarantees
        // every reduction_op enumerator is associative; guard anyway for new ops).
        if (loop.reduction && !reduction_op_is_legal(*loop.reduction)) {
            r.tier = dependence_tier::illegal;
            r.error = make_error(execution_error_kind::simd_alias_violation,
                                 "non-associative reduction cannot vectorize");
            return r;
        }

        // Worst-case pairwise dependence tier.
        dependence_tier worst = dependence_tier::distinct_base;
        const auto& a = loop.accesses;
        for (std::size_t i = 0; i < a.size(); ++i)
            for (std::size_t j = i + 1; j < a.size(); ++j) {
                const dependence_tier t = detail::classify_pair(a[i], a[j]);
                if (static_cast<std::uint8_t>(t) > static_cast<std::uint8_t>(worst))
                    worst = t;
            }
        r.tier = worst;

        if (worst == dependence_tier::illegal) {
            r.error = make_error(execution_error_kind::simd_alias_violation,
                                 "loop-carried dependence: cannot vectorize");
            return r;
        }
        if (worst == dependence_tier::needs_runtime_guard) {
            // Legal only under a runtime alias check; the planner decides whether to
            // emit the guard. Report the tier and let the planner map a rejected
            // guard onto runtime_guard_rejected (CRANK-E-EXEC-012).
            r.error = std::nullopt; // not an error yet — a guard requirement
        }

        // Trip / tail sizing for [begin, end) step 1.
        if (vector_width == 0 || loop.end <= loop.begin) {
            r.vector_trip = 0;
            r.scalar_tail = (loop.end > loop.begin)
                                ? static_cast<std::uint64_t>(loop.end - loop.begin)
                                : 0;
            return r;
        }
        const std::uint64_t trips = static_cast<std::uint64_t>(loop.end - loop.begin);
        r.vector_trip = (trips / vector_width) * vector_width;
        r.scalar_tail = trips - r.vector_trip;
        return r;
    }

    // ============================================================================
    // from_iv_bounds — adapt a lithe structured_for iv_bounds into a simd_loop.
    //
    // Templated on the bounds type so this header need not include lithe HL headers
    // (keeps it light); any struct with lower/upper/step fits. The caller populates
    // `accesses` + `reduction` from the body separately.
    // ============================================================================

    template <class IvBounds>
    [[nodiscard]] simd_loop from_iv_bounds(const IvBounds& b) {
        simd_loop l;
        l.begin = b.lower;
        l.end = b.upper; // exclusive, matches structured_for_attr::iv_bounds
        l.step = b.step;
        return l;
    }
} // namespace crank
