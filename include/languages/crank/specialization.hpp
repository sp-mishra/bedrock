#pragma once

// crank/specialization.hpp — Controlled type-specialization (v2, §v2.5).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Lifts CRANK-GEN-005 and provides deterministic priority + overlap checks.
// Unrestricted specialization remains prohibited: every specialization must be
// strictly more specific than the base impl and must not overlap with any peer.
//
// Surfaces:
//   specialization_record  — one type-specialized impl
//   specificity_order      — strict ordering: specialized > generic
//   check_specialization_overlap — CRANK-GEN-007/008/009 diagnostics
//   select_impl            — given a set of candidates, pick the most specific
//
// Design refs: crank.md §v2.5; generics.hpp conformance_table.

#include "languages/crank/generics.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
    // ============================================================================
    // specialization_record — one specialized impl entry
    //
    // A specialization is more specific than a generic impl for the same trait when
    // it provides a concrete type binding (`concrete_type_name`) instead of a type
    // parameter. Multiple specializations for the same (trait, base_type) must not
    // overlap (CRANK-GEN-007).
    // ============================================================================

    struct specialization_record {
        trait_id trait = 0;
        std::string base_type_name; // e.g. "Vector[T]"
        std::string concrete_type_name; // e.g. "Vector[Float32]"
        std::uint64_t concrete_type_hash = 0;
        std::string module_name;
        std::string trait_module_name;
        // Priority: higher = more specific. Computed from specificity_score().
        std::uint32_t priority = 0;
        // Stable secondary sort key (§v2.1a determinism). Defaults to
        // concrete_type_hash via normalize_tiebreak(); ensures select_impl produces a
        // reproducible order even when two candidates share a priority band. The
        // full comparison order is (priority, tiebreak, concrete_type_name).
        std::uint64_t tiebreak = 0;
        // Optional associated type bindings provided by this specialization.
        std::vector<std::pair<std::string, std::string>> assoc_type_bindings;
        // Optional combine_fn_name override for Monoid specializations.
        std::optional<std::string> combine_fn_name;
        // Must not weaken safety/effect constraints from the base impl.
        // Checker compares effect_mask: if base has bit set, spec must have it too.
        std::uint64_t effect_mask = 0;
        std::uint64_t capability_mask = 0;
        // Optional bound-set on the specialization's type parameter (§8.2). When two
        // candidates both carry a non-empty set and one strictly implies the other
        // (Ordered ⇒ Comparable), the stronger-bounded impl is more specific — checked
        // by the registry-aware select_impl before the numeric (priority, tiebreak,
        // name) key. Empty (default) preserves the pre-§8.2 numeric-only ordering.
        trait_set required_bounds{};
    };

    // ============================================================================
    // specificity_order — total order on specialization candidates
    //
    // A specialization wins over a generic impl when its concrete_type_hash
    // matches the instantiation type. Among specializations, ordering is TOTAL and
    // deterministic: compare priority (desc), then tiebreak (desc), then
    // concrete_type_name (lexical asc). A full-key tie is a genuine ambiguity
    // (CRANK-GEN-007).
    // ============================================================================

    // normalize_tiebreak — default a record's tiebreak to its concrete_type_hash when
    // the caller left it 0. Keeps the secondary key meaningful without forcing every
    // call site to set it.
    inline void normalize_tiebreak(specialization_record& r) noexcept {
        if (r.tiebreak == 0) r.tiebreak = r.concrete_type_hash;
    }

    [[nodiscard]] inline bool is_more_specific(
        const specialization_record& a,
        const specialization_record& b) noexcept {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.tiebreak != b.tiebreak) return a.tiebreak > b.tiebreak;
        return a.concrete_type_name < b.concrete_type_name;
    }

    // full_key_tie — true when a and b are indistinguishable under the total order,
    // i.e. a genuine ambiguity that must be diagnosed rather than silently ordered.
    [[nodiscard]] inline bool full_key_tie(
        const specialization_record& a,
        const specialization_record& b) noexcept {
        return a.priority == b.priority
            && a.tiebreak == b.tiebreak
            && a.concrete_type_name == b.concrete_type_name;
    }

    // ============================================================================
    // bound_set_covers — does bound-set `a` cover every bound in `b` under trait
    // implication? (§8.2) Every bound required by `b` is satisfied by some bound in
    // `a` that implies it (a bound trivially implies itself). Used to rank a
    // stronger-bounded specialization above a weaker-bounded one.
    // ============================================================================

    [[nodiscard]] inline bool bound_set_covers(
        const trait_registry& registry,
        const trait_set& a,
        const trait_set& b) noexcept {
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(bound_kind::_Count); ++i) {
            const auto need = static_cast<bound_kind>(i);
            if (!b.has(need)) continue;
            bool covered = false;
            for (std::uint8_t j = 0; j < static_cast<std::uint8_t>(bound_kind::_Count); ++j) {
                const auto have = static_cast<bound_kind>(j);
                if (a.has(have) && trait_implies(registry, have, need)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) return false;
        }
        return true;
    }

    // bounds_strictly_more_specific — `a`'s bound-set is strictly stronger than `b`'s:
    // a covers all of b's bounds, but b does NOT cover all of a's (proper). e.g.
    // {Ordered} vs {Comparable} with Ordered ⇒ Comparable.
    [[nodiscard]] inline bool bounds_strictly_more_specific(
        const trait_registry& registry,
        const specialization_record& a,
        const specialization_record& b) noexcept {
        if (a.required_bounds.empty() || b.required_bounds.empty()) return false;
        const bool a_covers_b = bound_set_covers(registry, a.required_bounds, b.required_bounds);
        const bool b_covers_a = bound_set_covers(registry, b.required_bounds, a.required_bounds);
        return a_covers_b && !b_covers_a;
    }

    // ============================================================================
    // specialization_diag_kind + diagnostic
    // ============================================================================

    enum class specialization_diag_kind : std::uint8_t {
        overlap, // CRANK-GEN-007: two specializations with same priority / coverage
        weakens_constraints, // CRANK-GEN-008: specialization removes effect/safety bits
        orphan_violation, // CRANK-GEN-009: specialization violates orphan rule
    };

    [[nodiscard]] constexpr std::string_view to_string(specialization_diag_kind k) noexcept {
        switch (k) {
        case specialization_diag_kind::overlap: return "CRANK-GEN-007";
        case specialization_diag_kind::weakens_constraints: return "CRANK-GEN-008";
        case specialization_diag_kind::orphan_violation: return "CRANK-GEN-009";
        }
        return "CRANK-GEN-???";
    }

    struct specialization_diagnostic {
        specialization_diag_kind kind;
        source_span at;
        std::string message;
        bool is_error = true;
        std::optional<diag_explanation> explanation; // §v2.1a structured explanation
    };

    // ============================================================================
    // specialization_table — registry of type-specialized impls for a trait
    // ============================================================================

    class specialization_table {
    public:
        // Register a specialization. Caller must call check_specialization_overlap
        // first to verify no conflicts.
        void add(specialization_record rec) {
            normalize_tiebreak(rec);
            records_.push_back(std::move(rec));
        }

        // Find all specializations for a given (trait, concrete_type_hash).
        [[nodiscard]] std::vector<const specialization_record*>
        find_for(trait_id tid, std::uint64_t type_hash) const {
            std::vector<const specialization_record*> out;
            for (const auto& r : records_)
                if (r.trait == tid && r.concrete_type_hash == type_hash)
                    out.push_back(&r);
            return out;
        }

        [[nodiscard]] const std::vector<specialization_record>& all() const noexcept {
            return records_;
        }

        [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

    private:
        std::vector<specialization_record> records_;
    };

    // ============================================================================
    // check_specialization_overlap — validate a candidate specialization
    //
    // Checks:
    //   CRANK-GEN-007: no peer with same (trait, concrete_type_hash, priority)
    //   CRANK-GEN-008: specialization must not weaken base impl's effect bits
    //   CRANK-GEN-009: must satisfy orphan rule (owns trait OR owns type)
    // ============================================================================

    [[nodiscard]] inline std::vector<specialization_diagnostic>
    check_specialization_overlap(
        const specialization_table& table,
        const specialization_record& candidate,
        std::uint64_t base_effect_mask,
        std::string_view current_module,
        source_span at) {
        std::vector<specialization_diagnostic> diags;

        // CRANK-GEN-009: orphan rule
        const bool owns_trait = (candidate.trait_module_name == current_module);
        const bool owns_type = (candidate.module_name == current_module);
        if (!owns_trait && !owns_type) {
            diags.push_back({
                specialization_diag_kind::orphan_violation, at,
                std::string("CRANK-GEN-009: specialization of trait id ")
                + std::to_string(candidate.trait)
                + " for '" + candidate.concrete_type_name
                + "' violates orphan rule in module '" + std::string(current_module) + "'"
            });
        }

        // CRANK-GEN-008: must not weaken base effect bits
        const std::uint64_t missing_bits = base_effect_mask & ~candidate.effect_mask;
        if (missing_bits != 0) {
            diags.push_back({
                specialization_diag_kind::weakens_constraints, at,
                std::string("CRANK-GEN-008: specialization of '")
                + candidate.concrete_type_name
                + "' removes effect/safety bits that the base impl declares; "
                "missing mask bits: 0x" + [&] {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%llx",
                                  static_cast<unsigned long long>(missing_bits));
                    return std::string(buf);
                }()
            });
        }

        // CRANK-GEN-007: overlap check — same (trait, concrete_type_hash, priority)
        for (const auto& existing : table.all()) {
            if (existing.trait != candidate.trait) continue;
            if (existing.concrete_type_hash != candidate.concrete_type_hash) continue;
            if (existing.priority == candidate.priority) {
                diags.push_back({
                    specialization_diag_kind::overlap, at,
                    std::string("CRANK-GEN-007: specialization '")
                    + candidate.concrete_type_name
                    + "' overlaps with existing specialization '"
                    + existing.concrete_type_name
                    + "' (both have priority "
                    + std::to_string(candidate.priority)
                    + "); add a distinct priority value to disambiguate"
                });
            }
        }

        return diags;
    }

    // ============================================================================
    // select_impl — pick the best specialization (or null if none match)
    //
    // Given all candidates for (trait, concrete_type_hash), returns the single
    // most-specific one. Returns nullopt if the candidates list is empty.
    // Asserts (returns first + emits CRANK-GEN-007) if two have identical priority.
    // ============================================================================

    [[nodiscard]] inline std::optional<const specialization_record*>
    select_impl(
        const specialization_table& table,
        trait_id tid,
        std::uint64_t type_hash,
        std::vector<specialization_diagnostic>& diags,
        source_span at) {
        auto candidates = table.find_for(tid, type_hash);
        if (candidates.empty()) return std::nullopt;
        if (candidates.size() == 1u) return candidates[0];

        // Sort by priority descending
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const auto* a, const auto* b) { return is_more_specific(*a, *b); });

        // Detect a genuine full-key tie at the top (unorderable ambiguity).
        if (full_key_tie(*candidates[0], *candidates[1])) {
            diags.push_back({
                specialization_diag_kind::overlap, at,
                std::string("CRANK-GEN-007: ambiguous specialization — two impls have equal priority ")
                + std::to_string(candidates[0]->priority)
                + " for type hash 0x"
                + [&] {
                    char buf[24];
                    std::snprintf(buf, sizeof(buf), "%llx",
                                  static_cast<unsigned long long>(type_hash));
                    return std::string(buf);
                }()
            });
        }

        return candidates[0];
    }

    // ============================================================================
    // select_impl (registry-aware, §8.2) — like the numeric overload, but when two
    // top candidates tie under the numeric (priority, tiebreak, name) key AND one
    // carries a strictly stronger bound-set (Ordered ⇒ Comparable), the
    // stronger-bounded impl wins. Falls back to the numeric order otherwise, so any
    // candidate set with empty required_bounds behaves exactly as the registry-less
    // overload. A residual full-key + bound tie is a genuine CRANK-GEN-007 ambiguity.
    // ============================================================================

    [[nodiscard]] inline std::optional<const specialization_record*>
    select_impl(
        const specialization_table& table,
        const trait_registry& registry,
        trait_id tid,
        std::uint64_t type_hash,
        std::vector<specialization_diagnostic>& diags,
        source_span at) {
        auto candidates = table.find_for(tid, type_hash);
        if (candidates.empty()) return std::nullopt;
        if (candidates.size() == 1u) return candidates[0];

        // Order by (bound-implication, then numeric key): a strictly stronger
        // bound-set beats a weaker one; equal bounds fall back to is_more_specific.
        std::stable_sort(candidates.begin(), candidates.end(),
                         [&registry](const specialization_record* a, const specialization_record* b) {
                             if (bounds_strictly_more_specific(registry, *a, *b)) return true;
                             if (bounds_strictly_more_specific(registry, *b, *a)) return false;
                             return is_more_specific(*a, *b);
                         });

        // Ambiguity only when the top two are numerically tied AND neither's bounds
        // strictly dominate the other.
        if (full_key_tie(*candidates[0], *candidates[1])
            && !bounds_strictly_more_specific(registry, *candidates[0], *candidates[1])
            && !bounds_strictly_more_specific(registry, *candidates[1], *candidates[0])) {
            diags.push_back({
                specialization_diag_kind::overlap, at,
                std::string("CRANK-GEN-007: ambiguous specialization — two impls have equal priority ")
                + std::to_string(candidates[0]->priority)
                + " for type hash 0x"
                + [&] {
                    char buf[24];
                    std::snprintf(buf, sizeof(buf), "%llx",
                                  static_cast<unsigned long long>(type_hash));
                    return std::string(buf);
                }()
            });
        }

        return candidates[0];
    }
} // namespace crank
