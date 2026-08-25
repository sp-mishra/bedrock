#pragma once

// crank/coherence.hpp — Use-site (cross-module) coherence (generics maturation, §v2.1a).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// generics.hpp `check_coherence` enforces the *definition-site* orphan rule: an impl
// is legal only if its module owns the trait or the type. That is necessary but not
// sufficient for a multi-module program. This header adds the *use-site* rule that a
// mature generic system needs:
//
//   An impl is usable at a call site only if its defining module is in scope —
//   i.e. the current module IS the impl's module, or the current module (transitively)
//   imports it. Built-in impls (empty / "std.core" module) are always in scope.
//
// Without this, an `impl` written in module A leaks into module B even when B never
// imports A, which breaks separate compilation guarantees (B could compile, then fail
// to link once A is absent) and makes conformance non-local.
//
// Diagnostic: CRANK-GEN-013 — an impl EXISTS but is not in scope. This is distinct
// from CRANK-GEN-001 (no impl exists at all): the fix differs (add an import vs. write
// an impl), so the codes and the `help` text differ.
//
// Surfaces:
//   impl_visibility_ctx     — current module + its transitive import set
//   is_builtin_impl_module  — treats "" / "std.core" as always in scope
//   check_module_in_scope   — one impl module → optional CRANK-GEN-013 explanation
//   check_witness_visibility— a witness vector → monomorphize_diagnostic list
//   monomorphize_in_scope   — monomorphize + use-site coherence in one call

#include "languages/crank/generics.hpp"
#include "languages/crank/monomorphize.hpp"
#include "languages/crank/diagnostic.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
    // ============================================================================
    // impl_visibility_ctx — the module performing a lookup + what it can see.
    //
    // `imported` is the current module's TRANSITIVE import closure (the caller builds
    // it from module.hpp's import graph). nullptr ⇒ no imports; only same-module and
    // built-in impls are in scope.
    // ============================================================================

    struct impl_visibility_ctx {
        std::string current_module;
        const std::vector<std::string>* imported = nullptr;

        [[nodiscard]] bool imports(std::string_view m) const noexcept {
            if (!imported) return false;
            return std::ranges::find(*imported, m) != imported->end();
        }
    };

    // A built-in / std-core impl carries no owning module (or the shared "std.core"
    // base) and is always in scope — the language prelude is implicitly imported.
    [[nodiscard]] inline bool is_builtin_impl_module(std::string_view m) noexcept {
        return m.empty() || m == "std.core";
    }

    // ============================================================================
    // check_impl_in_scope — is `impl_module` visible from `ctx`?
    //
    // Returns nullopt when in scope; otherwise a CRANK-GEN-013 explanation. Works on a
    // module name (used by both the impl_record and impl_witness overloads below).
    // ============================================================================

    [[nodiscard]] inline std::optional<diag_explanation>
    check_module_in_scope(std::string_view impl_module,
                          std::string_view trait_name,
                          std::string_view type_name,
                          const impl_visibility_ctx& ctx,
                          source_span at) {
        if (is_builtin_impl_module(impl_module)) return std::nullopt;
        if (impl_module == ctx.current_module) return std::nullopt;
        if (ctx.imports(impl_module)) return std::nullopt;

        std::string summary = std::string("impl of '") + std::string(trait_name)
            + "' for '" + std::string(type_name) + "' is defined in module '"
            + std::string(impl_module) + "', which is not imported by '"
            + std::string(ctx.current_module) + "'";
        return explain("CRANK-GEN-013", std::move(summary), at)
               .note("an impl is only usable where its defining module is in scope")
               .help(std::string("add `import \"") + std::string(impl_module)
                   + "\"` to module '" + std::string(ctx.current_module) + "'")
               .build();
    }

    // ============================================================================
    // check_witness_visibility — verify every resolved witness's impl is in scope.
    //
    // Run AFTER resolve_witnesses succeeds (witnesses found). For each witness whose
    // impl module is not in scope, emit a CRANK-GEN-013 monomorphize_diagnostic. An
    // out-of-scope impl is a hard error: the program would not link.
    // ============================================================================

    [[nodiscard]] inline std::vector<monomorphize_diagnostic>
    check_witness_visibility(const std::vector<impl_witness>& witnesses,
                             const impl_visibility_ctx& ctx,
                             source_span at) {
        std::vector<monomorphize_diagnostic> diags;
        for (const auto& w : witnesses) {
            auto ex = check_module_in_scope(w.impl_module_name, w.trait_name,
                                            w.type_name, ctx, at);
            if (ex) {
                monomorphize_diagnostic m{ex->render_message(), at, true};
                m.explanation = std::move(ex);
                diags.push_back(std::move(m));
            }
        }
        return diags;
    }

    // ============================================================================
    // monomorphize_in_scope — monomorphize + use-site coherence in one step.
    //
    // Runs the standard monomorphizer, then (if witnesses resolved) appends
    // CRANK-GEN-013 diagnostics for any witness whose impl module is out of scope.
    // Kept as a free function so monomorphize.hpp has no dependency on coherence
    // (the check is opt-in: callers without cross-module context use the plain
    // monomorphizer, preserving existing single-module behavior exactly).
    // ============================================================================

    [[nodiscard]] inline monomorphize_result
    monomorphize_in_scope(const monomorphizer& mm,
                          const instantiation_key& key,
                          const trait_registry& registry,
                          const trait_set& required,
                          std::uint64_t fn_type_hash,
                          std::string_view fn_type_name,
                          const impl_visibility_ctx& ctx,
                          source_span at) {
        auto res = mm.monomorphize(key, registry, required, fn_type_hash, fn_type_name, at);
        // Only gate visibility once the impls themselves resolved; a missing impl is
        // already CRANK-GEN-001 and takes precedence.
        if (!res.witnesses.empty()) {
            for (auto& d : check_witness_visibility(res.witnesses, ctx, at))
                res.diagnostics.push_back(std::move(d));
        }
        return res;
    }
} // namespace crank
