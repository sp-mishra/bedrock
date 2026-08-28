#pragma once

// crank/diagnostic.hpp — Structured diagnostic explanations (generics maturation, §v2.1a).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Re-exports from lang (generic layer):
//   diag_severity, diag_label, diag_explanation, explain
//
// Crank-specific additions:
//   view_diag_kind + view_diagnostic_code() — CRANK-VIEW-000..011 view diagnostics

#include "languages/crank/source_span.hpp"
#include "languages/generic/core/rich_diagnostic.hpp"

namespace crank {
    // Re-export generic rich diagnostic types into crank namespace.
    using diag_severity    = lang::diag_severity;
    using diag_label       = lang::diag_label;
    using diag_explanation = lang::diag_explanation;
    using explain          = lang::explain;
} // namespace crank


// ============================================================================
// CRANK-VIEW diagnostic codes  (domain views feature)
// ============================================================================

namespace crank {
    // view_diag_kind — identifies each domain-view diagnostic.
    // Emitted via explain(view_diagnostic_code(k), …).build().

    enum class view_diag_kind : std::uint8_t {
        feature_disabled = 0, // CRANK-VIEW-000: view used without `domain_views` feature
        unknown_target = 1, // CRANK-VIEW-001: target view type not found / not a view
        not_viewable = 2,
        // CRANK-VIEW-002: source type cannot be viewed as target (structural refuted OR no legal interaction)
        requirement_failed = 3, // CRANK-VIEW-003: a `requires` obligation refuted at compile time
        runtime_guard = 4, // CRANK-VIEW-004: unknown obligation → runtime view guard inserted (informational)
        would_copy = 5,
        // CRANK-VIEW-005: un-view / view would take a costed conversion edge (copy); use explicit materialize(...)
        ambiguous_decl = 6, // CRANK-VIEW-006: ambiguous view declaration / overlapping backing
        lifetime = 7, // CRANK-VIEW-007: view outlives its backing storage
        mutable_conflict = 8, // CRANK-VIEW-008: conflicting mutable views of the same storage
        provider_missing = 9, // CRANK-VIEW-009: host-backed domain view has no registered provider
        metadata_conflict = 10, // CRANK-VIEW-010: conflicting domain metadata at Crank lowering
        reserved_011 = 11, // CRANK-VIEW-011: reserved — ensures/where on a view_decl
    };

    [[nodiscard]] constexpr std::string_view
    view_diagnostic_code(view_diag_kind k) noexcept {
        switch (k) {
        case view_diag_kind::feature_disabled: return "CRANK-VIEW-000";
        case view_diag_kind::unknown_target: return "CRANK-VIEW-001";
        case view_diag_kind::not_viewable: return "CRANK-VIEW-002";
        case view_diag_kind::requirement_failed: return "CRANK-VIEW-003";
        case view_diag_kind::runtime_guard: return "CRANK-VIEW-004";
        case view_diag_kind::would_copy: return "CRANK-VIEW-005";
        case view_diag_kind::ambiguous_decl: return "CRANK-VIEW-006";
        case view_diag_kind::lifetime: return "CRANK-VIEW-007";
        case view_diag_kind::mutable_conflict: return "CRANK-VIEW-008";
        case view_diag_kind::provider_missing: return "CRANK-VIEW-009";
        case view_diag_kind::metadata_conflict: return "CRANK-VIEW-010";
        case view_diag_kind::reserved_011: return "CRANK-VIEW-011";
        }
        return "CRANK-VIEW-000";
    }
} // namespace crank
