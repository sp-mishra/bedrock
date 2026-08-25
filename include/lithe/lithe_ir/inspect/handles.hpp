#pragma once

// =============================================================================
// lithe_ir/inspect/handles.hpp — stable inspection vocabulary types
//
// Namespace: lithe::ir::inspect
//
// Family-neutral handle PODs and option types for the IR introspection facade.
// No walking logic; depends only on ../format.hpp.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <string>

#include "../format.hpp"  // stage, schema_version

namespace lithe::ir::inspect {
    // =============================================================================
    // ir_family — the three wire IR families
    // =============================================================================

    enum class ir_family : std::uint8_t {
        graph = 0, // Graph IR: stages surface / canonical / optimized
        hl_mir = 1, // High-level MIR: stage lowered (+ optimized-HL)
        physical_mir = 2, // Physical MIR: stages physical / managed
    };

    // =============================================================================
    // Handle PODs — trivially copyable, no pointers, no ownership
    // =============================================================================

    // Opaque identity of an inspected compilation unit (module / function group).
    struct unit_id {
        std::uint64_t value = 0;
        [[nodiscard]] constexpr bool operator==(const unit_id&) const noexcept = default;
    };

    // Stable entity reference within a family+stage — canonical wire id (impl-1).
    // Values are the exact dense ids assigned by canonical_encode; cross-reference
    // a digest preimage offset to a displayed entity by id equality.
    struct entity_ref {
        std::uint32_t id = 0;
        [[nodiscard]] constexpr bool operator==(const entity_ref&) const noexcept = default;
    };

    // Addresses "which representation" — family + stage + schema.
    struct stage_key {
        lithe::ir::stage s;
        lithe::ir::schema_version schema;
        ir_family family;

        [[nodiscard]] constexpr bool operator==(const stage_key&) const noexcept = default;
    };

    // =============================================================================
    // ir_dump_format — controls which form dump() produces
    // =============================================================================

    enum class ir_dump_format : std::uint8_t {
        // Deterministic text form; round-trippable (routes through text_provider).
        canonical_text = 0,
        // Little-endian binary (routes through binary_provider).
        binary = 1,
        // Human-annotated text; NON-NORMATIVE — never feed to a decoder.
        human_pretty = 2,
    };

    // =============================================================================
    // ir_text_options — hints for canonical_text and human_pretty dumps
    // (ignored for binary)
    // =============================================================================

    struct ir_text_options {
        bool include_types = true;
        bool include_provenance = false;
        // Restrict output to stable ids only (no pass-specific names).
        bool stable_ids_only = true;
    };

    // =============================================================================
    // inspect_error — error returned by fallible facade operations
    // =============================================================================

    struct inspect_error {
        enum class code : std::uint8_t {
            unknown_function = 0,
            stage_unavailable = 1,
            dump_failed = 2,
            provider_missing = 3,
        };

        code ec;
        std::string detail;
    };
} // namespace lithe::ir::inspect
