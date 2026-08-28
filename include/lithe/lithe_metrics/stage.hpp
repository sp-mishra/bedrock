#pragma once

// =============================================================================
// lithe_metrics/stage.hpp — pipeline stage axis + stage_metric POD
//
// Namespace: lithe::metrics
//
// Provides:
//   pipeline_stage — full-engine activity stage enumeration (superset of ir::stage)
//   ir_stage_of    — constexpr map pipeline_stage → optional<ir::stage>
//   stage_metric   — one comparable, stage-tagged, unit-keyed metric record
//
// Depends only on lithe_ir/format.hpp (stage, schema_version) and
// lithe_cost_model.hpp (cost_vector).  No NADI here — transport lives in
// recorder.hpp.
//
// All types are trivially copyable PODs.  No virtual, no macros.  C++23.
// =============================================================================

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "../lithe_ir/format.hpp"    // lithe::ir::stage
#include "../lithe_cost_model.hpp"   // lithe::cost::cost_vector

namespace lithe::metrics {
    // =============================================================================
    // pipeline_stage — every engine activity that can be individually metered
    //
    // Values are stable.  Adding new enumerators is a minor bump; renumbering is
    // a major bump (matches §4.5 schema version semantics).
    // =============================================================================

    enum class pipeline_stage : std::uint8_t {
        frontend_parse = 0, // source text → graph IR (surface)
        graph_build = 1, // DAG construction / expression lifting
        canonicalize = 2, // graph surface → canonical IR
        hl_lower = 3, // canonical → HL MIR (lowered)
        portable_verify = 4, // verify_portable (seven checks)
        portable_optimize = 5, // portable optimizer (impl-2 passes)
        physical_codegen = 6, // HL MIR → physical register MIR
        artifact_encode = 7, // physical MIR → wire bytes (binary_provider)
        artifact_publish = 8, // wire bytes → artifact store (impl-3)
        artifact_load = 9, // artifact store → in-process (impl-3)
        backend_compile = 10, // IR → backend-native code (JIT / AOT)
        backend_install = 11, // native code → installable entry point
        execute = 12, // call into installed native code
    };

    inline constexpr std::uint8_t k_pipeline_stage_count = 13;

    // Stable name strings for each pipeline_stage.
    [[nodiscard]] constexpr std::string_view to_string(pipeline_stage s) noexcept {
        switch (s) {
        case pipeline_stage::frontend_parse: return "frontend_parse";
        case pipeline_stage::graph_build: return "graph_build";
        case pipeline_stage::canonicalize: return "canonicalize";
        case pipeline_stage::hl_lower: return "hl_lower";
        case pipeline_stage::portable_verify: return "portable_verify";
        case pipeline_stage::portable_optimize: return "portable_optimize";
        case pipeline_stage::physical_codegen: return "physical_codegen";
        case pipeline_stage::artifact_encode: return "artifact_encode";
        case pipeline_stage::artifact_publish: return "artifact_publish";
        case pipeline_stage::artifact_load: return "artifact_load";
        case pipeline_stage::backend_compile: return "backend_compile";
        case pipeline_stage::backend_install: return "backend_install";
        case pipeline_stage::execute: return "execute";
        }
        return "unknown";
    }

    // Map pipeline_stage → lithe::ir::stage (nullopt for non-IR activity stages).
    [[nodiscard]] constexpr std::optional<lithe::ir::stage>
    ir_stage_of(pipeline_stage s) noexcept {
        switch (s) {
        case pipeline_stage::frontend_parse: return lithe::ir::stage::surface;
        case pipeline_stage::graph_build: return lithe::ir::stage::surface;
        case pipeline_stage::canonicalize: return lithe::ir::stage::canonical;
        case pipeline_stage::hl_lower: return lithe::ir::stage::lowered;
        case pipeline_stage::portable_optimize: return lithe::ir::stage::optimized;
        case pipeline_stage::physical_codegen: return lithe::ir::stage::physical;
        case pipeline_stage::backend_install: return lithe::ir::stage::managed;
        // Non-IR activity stages
        case pipeline_stage::portable_verify:
        case pipeline_stage::artifact_encode:
        case pipeline_stage::artifact_publish:
        case pipeline_stage::artifact_load:
        case pipeline_stage::backend_compile:
        case pipeline_stage::execute:
            return std::nullopt;
        }
        return std::nullopt;
    }

    // =============================================================================
    // stage_metric — one comparable, stage-tagged, unit-keyed metric record
    //
    // Content-addressed by unit_digest (impl-1 semantic_digest — produced by
    // lithe::ir::portable::semantic_digest).  Metrics join to impl-3 artifact
    // keys by digest equality with no re-hashing.
    //
    // All fields are fixed-width.  Trivially copyable.  No pointers.
    // =============================================================================

    struct stage_metric {
        // Unit identity: impl-1 semantic_digest of the portable_module.
        // Ties every metric to the exact IR it measured.
        std::array<std::uint8_t, 64> unit_digest{};
        std::uint8_t unit_digest_len = 0; // 0 = not set / anonymous

        pipeline_stage stage = pipeline_stage::execute;

        std::uint32_t entity_count = 0; // ops/nodes/instrs processed
        std::uint64_t wall_ns = 0; // elapsed wall time in nanoseconds
        std::uint64_t cycles = 0; // TSC cycles (0 if not available)
        std::uint32_t iterations = 0; // fixpoint iterations / pass retries
        std::uint32_t rule_fired = 0; // rewrites / folds / eliminations applied

        lithe::cost::cost_vector estimated{}; // model estimate at this stage
        lithe::cost::cost_vector measured{}; // measured cost (execute stage or benchmarked)

        std::uint16_t diag_errors = 0;
        std::uint16_t diag_warnings = 0;

        std::uint8_t _pad[4] = {}; // explicit padding to 128 bytes total alignment
    };

    static_assert(std::is_trivially_copyable_v<stage_metric>);
} // namespace lithe::metrics
