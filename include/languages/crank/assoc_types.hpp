#pragma once

// crank/assoc_types.hpp — Associated type resolution (v2, §v2.1/§v2.2).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Lifts CRANK-GEN-006. Resolves `type Item` declarations in traits,
// `Self.Item` projections inside `impl` bodies, and `C.Item` projections
// in generic function bodies where C is a type parameter bounded by a trait
// that declares `type Item`.
//
// Surfaces:
//   assoc_type_binding   — concrete type name bound for one assoc type in one impl
//   assoc_type_context   — resolution context: trait descriptors + impl witnesses
//   resolve_self_projection  — resolve Self.Item inside an impl body
//   resolve_param_projection — resolve C.Item where C: Trait with assoc type Item
//   check_assoc_bindings     — verify an impl binds all required assoc types
//
// Diagnostics:
//   CRANK-GEN-010  ambiguous_assoc_projection — two bounds provide same assoc name
//   CRANK-GEN-011  missing_assoc_type_binding — impl omits required assoc type
//
// Design refs: crank.md §v2.1/§v2.2; generics.hpp assoc_type_record.

#include "languages/crank/generics.hpp"
#include "languages/crank/monomorphize.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
    // ============================================================================
    // assoc_type_binding — concrete binding of one associated type name → type name
    // ============================================================================

    struct assoc_type_binding {
        std::string assoc_name; // name declared in trait, e.g. "Item"
        std::string concrete; // resolved concrete type name, e.g. "Int64"
        std::string trait_name; // trait that declares this assoc type
    };

    // ============================================================================
    // assoc_type_context — needed to resolve projections
    //
    // Holds:
    //   trait_registry   — trait descriptors (provides assoc_type_record lists)
    //   witnesses        — resolved impl_witness list from monomorphization
    // ============================================================================

    struct assoc_type_context {
        const trait_registry* registry = nullptr;
        const std::vector<impl_witness>* witnesses = nullptr;
    };

    // ============================================================================
    // resolve_self_projection — resolve `Self.assoc_name` inside an impl body
    //
    // Inside `impl Trait for Type`, `Self.Item` resolves to the binding declared
    // in the impl's assoc_type_bindings. Returns empty string if not found.
    // ============================================================================

    [[nodiscard]] inline std::string
    resolve_self_projection(
        std::string_view assoc_name,
        const std::vector<std::pair<std::string, std::string>>& impl_bindings) {
        for (const auto& [name, val] : impl_bindings)
            if (name == assoc_name) return val;
        return {};
    }

    // ============================================================================
    // resolve_param_projection — resolve `C.assoc_name` where C is a type param
    //
    // Searches all impl_witnesses in ctx to find one whose trait declares
    // `assoc_name`. If exactly one provides a binding, returns it.
    // Emits CRANK-GEN-010 on ambiguity (two traits provide same name).
    // Emits CRANK-GEN-011 when the matching impl has no binding.
    // ============================================================================

    [[nodiscard]] inline std::string
    resolve_param_projection(
        std::string_view assoc_name,
        std::string_view param_name,
        const assoc_type_context& ctx,
        std::vector<conformance_diagnostic>& diags,
        source_span at) {
        if (!ctx.registry || !ctx.witnesses) return {};

        std::vector<std::string> found_values;
        std::vector<std::string> providing_traits;

        for (const auto& w : *ctx.witnesses) {
            // Check if this witness's trait declares assoc_name
            const trait_descriptor* td = ctx.registry->find_trait_by_name(w.trait_name);
            if (!td) continue;

            bool trait_declares = std::ranges::any_of(td->assoc_types,
                                                      [&](const assoc_type_record& ar) {
                                                          return ar.name == assoc_name;
                                                      });
            if (!trait_declares) continue;

            // Found a trait that declares it — look for the binding in the witness
            const std::string* val = nullptr;
            for (const auto& [bname, bval] : w.assoc_type_map)
                if (bname == assoc_name) {
                    val = &bval;
                    break;
                }

            if (!val) {
                diags.push_back(make_missing_assoc_binding_diag(at, assoc_name,
                                                                w.trait_name, param_name));
                continue;
            }

            found_values.push_back(*val);
            providing_traits.push_back(w.trait_name);
        }

        if (found_values.size() > 1) {
            diags.push_back(make_ambiguous_assoc_projection_diag(at, assoc_name, param_name));
            return {};
        }
        if (found_values.empty()) return {};
        return found_values.front();
    }

    // ============================================================================
    // check_assoc_bindings — verify an impl provides all required assoc type bindings
    //
    // Given a trait_descriptor with assoc_types and an impl_record's bindings,
    // emits CRANK-GEN-011 for each assoc type missing from the impl.
    // ============================================================================

    [[nodiscard]] inline std::vector<conformance_diagnostic>
    check_assoc_bindings(
        const trait_descriptor& trait_desc,
        const std::vector<std::pair<std::string, std::string>>& impl_bindings,
        std::string_view type_name,
        source_span at) {
        std::vector<conformance_diagnostic> diags;

        for (const auto& ar : trait_desc.assoc_types) {
            const bool bound = std::ranges::any_of(impl_bindings,
                                                   [&](const auto& p) { return p.first == ar.name; });
            if (!bound) {
                diags.push_back(make_missing_assoc_binding_diag(
                    at, ar.name, trait_desc.name, type_name));
            }
        }
        return diags;
    }
} // namespace crank
