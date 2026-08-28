#pragma once

// =============================================================================
// lithe_ir/portable/module.hpp — multi-function portable module container
//
// Namespace: lithe::ir::portable
//
// The wire form (adapters::lithe_hl_mir_ir) is per-function.  portable_module
// is the multi-function envelope that carries functions, constants, globals,
// imports, exports, declared capabilities, and a manifest.
//
// Design: wraps existing lithe_hl_mir_ir unchanged (Option A from impl-1 spec).
// All fields are fixed-width integers or std::string — no pointers, trivially
// serializable.
//
// Capability set lives in portable::capability_set — independent from
// lithe_execution so the light IR core (lithe_ir_core.hpp) has no new include
// edge toward the execution layer.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../adapters/hl_mir.hpp"  // adapters::lithe_hl_mir_ir, schema_version, stage
#include "../format.hpp"           // schema_version (also via hl_mir.hpp)

namespace lithe::ir::portable {
    // =============================================================================
    // capability_set — declared capability requirements of a portable module
    //
    // Independent from lithe_execution to preserve the include DAG.
    // impl-4 maps portable_capability_bit → execution capability constraints.
    // =============================================================================

    enum class portable_capability_bit : std::uint32_t {
        exceptions = 1u << 0,
        transactions = 1u << 1,
        defer_scopes = 1u << 2,
        atomics = 1u << 3,
        simd_hint = 1u << 4,
        gpu_hint = 1u << 5,
        reflection = 1u << 6,
        external_calls = 1u << 7,
    };

    struct capability_set {
        std::uint32_t bits = 0;

        [[nodiscard]] constexpr bool has(portable_capability_bit b) const noexcept {
            return (bits & static_cast<std::uint32_t>(b)) != 0;
        }

        constexpr void set(portable_capability_bit b) noexcept {
            bits |= static_cast<std::uint32_t>(b);
        }

        constexpr void merge(capability_set other) noexcept {
            bits |= other.bits;
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }
        [[nodiscard]] constexpr bool operator==(const capability_set&) const noexcept = default;
    };

    // =============================================================================
    // portable_import — external symbol declaration
    // =============================================================================

    struct portable_import {
        std::string module;
        std::string symbol;
        std::string signature_str; // stable type string of the callee
        schema_version abi{1, 0, 0};
        bool required = true;
    };

    // =============================================================================
    // portable_export — function exported from this module
    // =============================================================================

    struct portable_export {
        std::string symbol;
        std::uint32_t function_index = 0; // index into portable_module::functions
        std::string signature_str;
    };

    // =============================================================================
    // portable_global — module-level mutable or constant global
    // =============================================================================

    struct portable_global {
        std::string name;
        std::string type_str; // stable type string e.g. "i64", "f64"
        std::uint32_t const_index = 0; // index into portable_constant_pool
        bool mutable_ = false;
    };

    // =============================================================================
    // portable_constant_pool — index-addressed constants (little-endian bytes)
    // =============================================================================

    struct portable_constant_pool {
        std::vector<std::string> types; // parallel to data
        std::vector<std::vector<std::uint8_t>> data; // canonical LE bytes

        [[nodiscard]] bool valid() const noexcept {
            return types.size() == data.size();
        }

        [[nodiscard]] std::size_t size() const noexcept { return types.size(); }
    };

    // =============================================================================
    // portable_manifest — module-level metadata + semantic digest slot
    // =============================================================================

    struct portable_manifest {
        std::string producer;
        schema_version producer_version{1, 0, 0};
        std::string source_language;

        // Semantic digest (content-hash of canonical_encode output, impl-1 §13).
        // Distinct from the payload integrity digest in binary_ir_envelope.
        std::array<std::uint8_t, 64> semantic_digest{};
        std::uint8_t digest_len = 0; // actual populated bytes (0 = not computed)
    };

    // =============================================================================
    // portable_module — the multi-function portable container
    // =============================================================================

    struct portable_module {
        // Functions — each is a portable wire-form function
        std::vector<adapters::lithe_hl_mir_ir> functions;

        // Module-level data
        portable_constant_pool constants;
        std::vector<portable_global> globals;
        std::vector<portable_import> imports;
        std::vector<portable_export> exports;

        // Declared capabilities required by this module
        capability_set declared_capabilities;

        // Manifest (including the semantic digest slot)
        portable_manifest manifest;

        // Schema version for the module container format
        schema_version schema{1, 0, 0};

        // -------------------------------------------------------------------------
        // structurally_complete: lightweight validation (no semantic checks).
        // Full semantic validation: verify.hpp portable::verify_portable().
        // -------------------------------------------------------------------------
        [[nodiscard]] bool structurally_complete() const noexcept {
            if (functions.empty()) return false;
            // Every export must reference a valid function index
            for (const auto& ex : exports)
                if (ex.function_index >= static_cast<std::uint32_t>(functions.size()))
                    return false;
            // Every global must reference a valid constant index
            for (const auto& g : globals)
                if (g.const_index >= static_cast<std::uint32_t>(constants.size()))
                    return false;
            if (!constants.valid()) return false;
            return true;
        }
    };
} // namespace lithe::ir::portable
