#pragma once

// crank/obligations.hpp — Implicit safety obligation builder (Module 3).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Builds vakya::types::proof_obligation records for obligation families:
//   kBoundsFamily   — index access: 0 <= i, i < len(container)
//   kDivFamily      — integer / or %: divisor != 0
//   kRangeFamily    — narrowing/sign-change `as`: value in target range
//   kParallelFamily — parallel/SIMD/GPU regions: independence + effect-allowed
//   kViewFamily     — domain view construction: viewable + user requires obligations
//
// kViewFamily predicate builtins (pure, declared here, lowered to SMT or runtime guard):
//   contiguous(base)                   — storage is contiguous in memory
//   aligned(base, alignment)           — storage is aligned to N bytes
//   rank(base) == R                    — rank of base matches R
//   shape_matches(base, Shape)         — shape dimensions match
//   strides_compatible(base, strides)  — stride layout is compatible
//
// crank-local thin wrapper over vakya/verify.hpp per-obligation API.
//
// Usage:
//   crank::obligation_builder bld;
//   bld.add_index(span, container_name, index_name);
//   bld.add_div  (span, divisor_name);
//   bld.add_as   (span, src_type, dst_type, value_name);
//   bld.add_parallel_safe(span, fn_name);
//   auto obs = bld.take();   // SmallVector<crank::obligation_record>

#include "languages/crank/source_span.hpp"
#include "vakya/verify.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
    // ============================================================================
    // obligation_family — which safety category an obligation belongs to
    // ============================================================================

    enum class obligation_family : std::uint8_t {
        bounds, // index range check
        div_by_zero, // divisor != 0
        range_cast, // narrowing `as` value-in-range
        parallel_safe, // parallel/SIMD/GPU independence
        view, // domain view: viewable + user requires predicates
    };

    // ============================================================================
    // obligation_record — one implicit obligation with metadata
    // ============================================================================

    struct obligation_record {
        vakya::types::proof_obligation ob; // the underlying vakya obligation
        obligation_family family;
        source_span at; // source location
        std::string label; // human-readable description

        // discharge outcome — filled by the verify driver
        vakya::types::proof_status outcome = vakya::types::proof_status::unknown;
    };

    // ============================================================================
    // obligation_builder
    //
    // Collects implicit obligations emitted during sema walking.
    // Thin crank wrapper: crank owns policy (what to prove); vakya owns mechanism.
    // ============================================================================

    class obligation_builder {
    public:
        obligation_builder() = default;

        // Index access: xs[i]
        // Emits two obligations: 0 <= i, i < len(xs)
        void add_index(source_span at,
                       std::string_view container,
                       std::string_view index) {
            add_ob(obligation_family::bounds, at,
                   "lower: 0 <= " + std::string(index),
                   vakya::types::kArithKind);
            add_ob(obligation_family::bounds, at,
                   "upper: " + std::string(index) + " < len(" + std::string(container) + ")",
                   vakya::types::kArithKind);
        }

        // Integer division or modulo: a / b, a % b
        // Emits: b != 0
        void add_div(source_span at, std::string_view divisor) {
            add_ob(obligation_family::div_by_zero, at,
                   std::string(divisor) + " != 0",
                   vakya::types::kArithKind);
        }

        // Narrowing/sign-change cast: value as DstType
        // Emits: value in [dst_min, dst_max]
        void add_as(source_span at,
                    std::string_view src_type,
                    std::string_view dst_type,
                    std::string_view value) {
            add_ob(obligation_family::range_cast, at,
                   std::string(value) + " in range of " + std::string(dst_type)
                   + " (from " + std::string(src_type) + ")",
                   vakya::types::kRefineKind);
        }

        // Parallel/SIMD/GPU region: independence + effect-allowed
        // Emits combined obligation: no loop-carried dependency, memory disjoint
        void add_parallel_safe(source_span at, std::string_view region_label) {
            add_ob(obligation_family::parallel_safe, at,
                   "parallel-safe: " + std::string(region_label),
                   vakya::types::kProveKind);
        }

        // -------------------------------------------------------------------------
        // View obligations (obligation_family::view)
        // -------------------------------------------------------------------------

        // Structural dtype match: dtype(backing) == target_dtype
        void add_view_dtype(source_span at, std::string_view backing, std::string_view dtype) {
            add_ob(obligation_family::view, at,
                   "dtype(" + std::string(backing) + ") == " + std::string(dtype),
                   vakya::types::kRefineKind);
        }

        // Rank match: rank(backing) == R
        void add_view_rank(source_span at, std::string_view backing, std::uint32_t rank) {
            add_ob(obligation_family::view, at,
                   "rank(" + std::string(backing) + ") == " + std::to_string(rank),
                   vakya::types::kArithKind);
        }

        // Shape match: shape(backing) == Shape
        void add_view_shape(source_span at, std::string_view backing, std::string_view shape) {
            add_ob(obligation_family::view, at,
                   "shape(" + std::string(backing) + ") == " + std::string(shape),
                   vakya::types::kArithKind);
        }

        // contiguous(backing) — predeclared pure view predicate builtin
        void add_view_contiguous(source_span at, std::string_view backing) {
            add_ob(obligation_family::view, at,
                   "contiguous(" + std::string(backing) + ")",
                   vakya::types::kProveKind);
        }

        // aligned(backing, alignment) — predeclared pure view predicate builtin
        void add_view_aligned(source_span at, std::string_view backing, std::uint32_t alignment) {
            add_ob(obligation_family::view, at,
                   "aligned(" + std::string(backing) + ", " + std::to_string(alignment) + ")",
                   vakya::types::kProveKind);
        }

        // strides_compatible(backing, strides) — predeclared pure view predicate builtin
        void add_view_strides(source_span at, std::string_view backing, std::string_view strides) {
            add_ob(obligation_family::view, at,
                   "strides_compatible(" + std::string(backing) + ", " + std::string(strides) + ")",
                   vakya::types::kProveKind);
        }

        // User requires-predicate from view_decl: arbitrary label lowered from pred_expr
        void add_view_requires(source_span at, std::string_view predicate_text) {
            add_ob(obligation_family::view, at,
                   "requires: " + std::string(predicate_text),
                   vakya::types::kProveKind);
        }

        // Lifetime: view must not outlive backing (CRANK-VIEW-007 trigger)
        void add_view_lifetime(source_span at, std::string_view view_name, std::string_view backing) {
            add_ob(obligation_family::view, at,
                   "lifetime: " + std::string(view_name) + " does not outlive " + std::string(backing),
                   vakya::types::kProveKind);
        }

        // Aliasing: ≤1 mutable view XOR N immutable views (CRANK-VIEW-008 trigger)
        void add_view_aliasing(source_span at, std::string_view backing) {
            add_ob(obligation_family::view, at,
                   "aliasing: exclusive mutable-view access on " + std::string(backing),
                   vakya::types::kProveKind);
        }

        // Constant divisor refutation: add_div_constant(at, 0) → refuted at compile time
        // Caller should call this when the divisor is a literal 0.
        void add_div_constant_zero(source_span at) {
            obligation_record rec;
            rec.family = obligation_family::div_by_zero;
            rec.at = at;
            rec.label = "constant divisor = 0 (refuted)";
            rec.outcome = vakya::types::proof_status::refuted;
            rec.ob.kind = vakya::types::kArithKind;
            rec.ob.description = rec.label;
            obs_.push_back(std::move(rec));
        }

        [[nodiscard]] std::vector<obligation_record> take() noexcept {
            return std::move(obs_);
        }

        [[nodiscard]] const std::vector<obligation_record>& view() const noexcept {
            return obs_;
        }

        void clear() noexcept { obs_.clear(); }

    private:
        void add_ob(obligation_family fam, source_span at,
                    std::string label, vakya::types::constraint_kind kind) {
            obligation_record rec;
            rec.family = fam;
            rec.at = at;
            rec.label = std::move(label);
            rec.ob.kind = kind;
            rec.ob.description = rec.label;
            obs_.push_back(std::move(rec));
        }

        std::vector<obligation_record> obs_;
    };

    // ============================================================================
    // stats_obligations — aggregate statistics over a batch of obligations
    // ============================================================================

    struct obligation_stats {
        std::uint32_t total = 0;
        std::uint32_t proven = 0;
        std::uint32_t unknown = 0;
        std::uint32_t refuted = 0;
        std::uint32_t deferred = 0;

        std::uint32_t bounds_count = 0;
        std::uint32_t div_count = 0;
        std::uint32_t range_count = 0;
        std::uint32_t parallel_count = 0;
        std::uint32_t view_count = 0;
    };

    [[nodiscard]] inline obligation_stats
    collect_obligation_stats(const std::vector<obligation_record>& obs) noexcept {
        obligation_stats s;
        s.total = static_cast<std::uint32_t>(obs.size());
        for (const auto& r : obs) {
            switch (r.outcome) {
            case vakya::types::proof_status::proven: ++s.proven;
                break;
            case vakya::types::proof_status::unknown: ++s.unknown;
                break;
            case vakya::types::proof_status::refuted: ++s.refuted;
                break;
            case vakya::types::proof_status::deferred: ++s.deferred;
                break;
            }
            switch (r.family) {
            case obligation_family::bounds: ++s.bounds_count;
                break;
            case obligation_family::div_by_zero: ++s.div_count;
                break;
            case obligation_family::range_cast: ++s.range_count;
                break;
            case obligation_family::parallel_safe: ++s.parallel_count;
                break;
            case obligation_family::view: ++s.view_count;
                break;
            }
        }
        return s;
    }
} // namespace crank
