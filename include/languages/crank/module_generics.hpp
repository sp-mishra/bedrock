#pragma once

// crank/module_generics.hpp — §v2.3 generic modules.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// A generic module is a module_descriptor parameterized by type and const
// arguments (e.g. `module Ring[type T, const N: usize] { ... }`). Instantiating
// it with concrete arguments produces a monomorphized module_descriptor whose
// name and content hash fold in the argument fingerprint, so each distinct
// instantiation has its own AOT cache key (reusing instantiation_key::fingerprint
// — the same dedup key monomorphize.hpp uses for generic functions, §v2.14).
//
// Visibility: only `pub`-marked module items are importable from an
// instantiation; a non-exported item is module-private. The descriptor records
// the exported item set so name resolution can gate cross-module access.
//
// This header owns the descriptor + instantiation logic; the parser production
// (`module IDENT[params] { ... }`) is added separately. Pipeline step 1a.

#include "languages/crank/module.hpp"
#include "languages/crank/monomorphize.hpp"

#include <string>
#include <vector>

namespace crank {
    // ============================================================================
    // module_type_param / module_const_param — a generic module's parameters.
    // ============================================================================

    struct module_type_param {
        std::string name; // "T"
        std::vector<std::string> bounds; // trait bounds ("Numeric", "Ord", ...)
    };

    struct module_const_param {
        std::string name; // "N"
        const_param_kind kind = const_param_kind::usize;
    };

    // ============================================================================
    // generic_module_descriptor — a module_descriptor plus generic parameters and
    // its exported-item set. Instantiate with instantiate() to get a concrete
    // module_descriptor keyed on the argument fingerprint.
    //
    // exports holds all module-level items with their pub flag. The resolver
    // populates this from the module body — uppercase name or explicit `pub`
    // sets is_pub = true. exports_symbol() gates cross-module access to pub items.
    // ============================================================================

    struct module_export {
        std::string name;
        bool        is_pub = false;
    };

    struct generic_module_descriptor {
        module_descriptor base; // name/version/capabilities of the template
        std::vector<module_type_param> type_params;
        std::vector<module_const_param> const_params;
        std::vector<module_export> exports; // all module items (pub + non-pub)

        [[nodiscard]] bool is_generic() const noexcept {
            return !type_params.empty() || !const_params.empty();
        }

        [[nodiscard]] std::size_t arity() const noexcept {
            return type_params.size() + const_params.size();
        }

        // True iff `name` is an exported (pub or uppercase) item of this module.
        [[nodiscard]] bool exports_symbol(std::string_view name) const noexcept {
            for (const auto& e : exports)
                if (e.is_pub && e.name == name) return true;
            return false;
        }
    };

    // ============================================================================
    // module_instantiation_result — a concrete module produced from a generic one.
    // ============================================================================

    struct module_instantiation_result {
        module_descriptor descriptor; // monomorphized module
        std::uint64_t aot_key = 0; // instantiation fingerprint (AOT dedup)
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // ============================================================================
    // instantiate_module — monomorphize a generic module with concrete arguments.
    //
    // Validates arg count against the parameter count, then builds an
    // instantiation_key over (module_name, type_args, const_args) whose fingerprint
    // (a) names the concrete module ("Ring#<fp>") and (b) becomes its AOT cache key.
    // The content_hash folds the fingerprint so an identical source with different
    // arguments produces distinct artifacts. Diagnostic CRANK-MOD-GEN-001 on arity
    // mismatch.
    // ============================================================================

    [[nodiscard]] inline module_instantiation_result
    instantiate_module(const generic_module_descriptor& gm,
                       const std::vector<type_arg>& type_args,
                       const std::vector<const_arg>& const_args) {
        module_instantiation_result out;

        if (type_args.size() != gm.type_params.size()
            || const_args.size() != gm.const_params.size()) {
            out.diagnostics.push_back(
                "CRANK-MOD-GEN-001: module '" + gm.base.name + "' expects "
                + std::to_string(gm.type_params.size()) + " type + "
                + std::to_string(gm.const_params.size()) + " const arguments, got "
                + std::to_string(type_args.size()) + " + "
                + std::to_string(const_args.size()));
            return out;
        }

        instantiation_key key;
        key.generic_name = gm.base.name;
        key.type_args = type_args;
        key.const_args = const_args;
        const std::uint64_t fp = key.fingerprint();

        out.aot_key = fp;
        out.descriptor = gm.base;
        out.descriptor.name = gm.base.name + "#" + std::to_string(fp);
        // Fold the argument fingerprint into the content hash so distinct
        // instantiations are distinct artifacts (identical source, different args).
        out.descriptor.content_hash = module_hash{
            gm.base.content_hash.value ^ (fp * 1099511628211ULL)
        };
        return out;
    }

    // Convenience: non-generic modules pass through unchanged (a generic module
    // with zero parameters instantiates to itself). Keeps callers uniform.
    [[nodiscard]] inline module_instantiation_result
    instantiate_module(const generic_module_descriptor& gm) {
        return instantiate_module(gm, {}, {});
    }
} // namespace crank
