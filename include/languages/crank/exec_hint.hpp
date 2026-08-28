#pragma once

// crank/exec_hint.hpp — @parallel/@simd/@gpu → execution_hint mapping (Module 4).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Maps the closed set of crank execution attributes to lithe::exec::execution_hint
// (G-LIT-4 fallback (b): crank attaches hints as region metadata and applies
// bias/filter over Lithe's candidate ranking via the existing hint fields).
//
// crank_attr_kind: closed set — @parallel, @simd, @gpu
// execution_preference: crank-local preference level (normal / strong / required)
//   used to express §4.6a preference before it lands in the framework as G-LIT-4.
//
// crank_exec_attr: parsed attribute record (kind + preference + required flag)
//
// map_exec_attr(attr) → lithe::exec::execution_hint
//   Applies the §4.6a.3 mapping table:
//     @parallel           → preferred=threaded
//     @simd               → preferred=simd
//     @gpu                → preferred=gpu
//     required=true       → hint.required = true
//     preference=strong   → currently: sets hint.required=false (strong is soft)
//     deterministic=true  → hint.deterministic = true
//
// validate_exec_attr(attr, bad_arg_name) → optional<string> error
//   Returns a diagnostic string on unknown arg names.
//
// hard_requirement_unmet_diagnostic(fn_name, attr) → string
//   Returns the compile-error message for required=true on an unprovable loop.
//
// design §4.6a. G-LIT-4 fallback (b). No framework edits.

#include "lithe/lithe_exec/exec_hint.hpp"
#include "lithe/lithe_exec/exec_kinds.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
    // ============================================================================
    // crank_attr_kind — closed set of crank execution annotations
    // ============================================================================

    enum class crank_attr_kind : std::uint8_t {
        parallel = 0, // @parallel — threaded execution
        simd = 1, // @simd    — vector/SIMD execution
        gpu = 2, // @gpu     — device compute
    };

    [[nodiscard]] constexpr std::string_view to_string(crank_attr_kind k) noexcept {
        switch (k) {
        case crank_attr_kind::parallel: return "parallel";
        case crank_attr_kind::simd: return "simd";
        case crank_attr_kind::gpu: return "gpu";
        }
        return "unknown";
    }

    // ============================================================================
    // execution_preference — crank-local preference level (§4.6a.2)
    //
    // G-LIT-4 (a) will add this to lithe::exec::execution_hint as a framework field;
    // in v1 we keep it crank-local and map it to the existing hint fields.
    // ============================================================================

    enum class execution_preference : std::uint8_t {
        normal = 0, // use this backend when legal + profitable
        strong = 1, // bias cost model + lower confidence threshold; keep fallback
        // required is expressed via crank_exec_attr::required = true (not via preference)
    };

    [[nodiscard]] constexpr std::string_view to_string(execution_preference p) noexcept {
        switch (p) {
        case execution_preference::normal: return "normal";
        case execution_preference::strong: return "strong";
        }
        return "normal";
    }

    // ============================================================================
    // crank_exec_attr — parsed crank execution attribute record
    //
    // Corresponds to one @parallel / @simd / @gpu annotation.
    // All fields default to the §4.6a "not constrained" state.
    // ============================================================================

    struct crank_exec_attr {
        crank_attr_kind kind = crank_attr_kind::parallel;
        execution_preference preference = execution_preference::normal;
        bool required = false;
        bool deterministic = false;
    };

    // Known attribute argument names per attr kind.
    namespace detail {
        // Returns true if arg_name is a valid argument for the given attr kind.
        [[nodiscard]] constexpr bool valid_arg(crank_attr_kind /*kind*/, std::string_view name) noexcept {
            // All three attrs share the same argument vocabulary.
            return name == "required"
                || name == "preference"
                || name == "deterministic";
        }
    } // namespace detail

    // ============================================================================
    // validate_exec_attr — check that the attribute's arg names are all valid
    //
    // bad_args contains any argument names that the caller determined are unknown.
    // Returns a diagnostic string if any bad args are present.
    // ============================================================================

    [[nodiscard]] inline std::optional<std::string>
    validate_exec_attr(const crank_exec_attr& attr,
                       const std::vector<std::string>& bad_args) {
        if (bad_args.empty()) return std::nullopt;
        std::string msg = "invalid argument";
        if (bad_args.size() > 1) msg += 's';
        msg += " on @";
        msg += to_string(attr.kind);
        msg += ": ";
        for (std::size_t i = 0; i < bad_args.size(); ++i) {
            if (i) msg += ", ";
            msg += "'";
            msg += bad_args[i];
            msg += "'";
        }
        msg += ". Valid args: required, preference, deterministic";
        return msg;
    }

    // ============================================================================
    // hard_requirement_unmet_diagnostic — compile error for required=true unproven
    //
    // Emitted when required=true is set on an annotation but the legality check
    // cannot prove the loop is parallel-safe (§6.3 / module 3 obligations).
    // ============================================================================

    [[nodiscard]] inline std::string
    hard_requirement_unmet_diagnostic(std::string_view fn_name,
                                      const crank_exec_attr& attr) {
        return std::string("CRANK-E-EXEC-001: @")
            + std::string(to_string(attr.kind))
            + "(required=true) in '"
            + std::string(fn_name)
            + "': loop cannot be proven legally parallel-safe — obligation unresolved. "
            "Remove `required=true` or resolve the safety obligation.";
    }

    // ============================================================================
    // map_exec_attr — §4.6a.3 mapping table
    //
    // @parallel           → preferred=threaded
    // @simd               → preferred=simd
    // @gpu                → preferred=gpu
    // required=true       → hint.required = true
    // preference=strong   → no extra hint.required change (soft); note: G-LIT-4 will
    //                        add a bias field; in v1 this is a no-op on the hint
    // deterministic=true  → hint.deterministic = true
    //
    // Annotations NEVER rewrite IR (§5b.1 / §4.6a.5).
    // ============================================================================

    [[nodiscard]] inline lithe::exec::execution_hint
    map_exec_attr(const crank_exec_attr& attr) noexcept {
        using lithe::exec::execution_kind;

        lithe::exec::execution_hint h;

        switch (attr.kind) {
        case crank_attr_kind::parallel:
            h.preferred = execution_kind::threaded;
            break;
        case crank_attr_kind::simd:
            h.preferred = execution_kind::simd;
            break;
        case crank_attr_kind::gpu:
            h.preferred = execution_kind::gpu;
            break;
        }

        h.required = attr.required;
        h.deterministic = attr.deterministic;
        // preference=strong: in v1 no additional hint field — G-LIT-4 (a) adds bias.
        // For gpu: forbid_gpu stays false (we're requesting gpu, not forbidding it).

        return h;
    }

    // ============================================================================
    // merge_exec_hints — merge hints from multiple attributes on the same region
    //
    // Uses lithe::exec::execution_hint::merged_with; stricter / more specific wins.
    // ============================================================================

    [[nodiscard]] inline lithe::exec::execution_hint
    merge_exec_hints(const std::vector<crank_exec_attr>& attrs) noexcept {
        lithe::exec::execution_hint acc{};
        for (const auto& a : attrs)
            acc = acc.merged_with(map_exec_attr(a));
        return acc;
    }

    // ============================================================================
    // soft_fallback_note — NADI pulse message for a soft-unmet hint
    //
    // Called when preference=strong and the primary backend is unavailable.
    // In a full integration this would emit a NADI Pulse; in v1 it returns
    // the message string (caller passes it to NADI or diagnostic sink).
    // ============================================================================

    [[nodiscard]] inline std::string
    soft_fallback_note(std::string_view fn_name, const crank_exec_attr& attr) {
        return std::string("CRANK-I-EXEC-002: @")
            + std::string(to_string(attr.kind))
            + "(preference="
            + std::string(to_string(attr.preference))
            + ") in '"
            + std::string(fn_name)
            + "': preferred backend unavailable — falling back to interpreter.";
    }
} // namespace crank
