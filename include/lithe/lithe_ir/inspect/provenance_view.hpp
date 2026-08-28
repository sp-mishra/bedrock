#pragma once

// =============================================================================
// lithe_ir/inspect/provenance_view.hpp — unified read-only provenance projection
//
// Namespace: lithe::ir::inspect
//
// provenance_view aggregates value summaries from:
//   impl-2  opt::pass_record          → pass_record_summary
//   impl-3  store::provenance / keys  → artifact_provenance_summary
//   impl-4  execution::execution_plan → plan_summary
//
// Each summary is a plain value type (no live engine pointers).
// summarize() adapters are guarded with __has_include so the facade compiles
// without any upstream milestone present; missing upstream → optional empty.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Conditionally include upstream milestone headers.
// Each include is at file scope (not inside a namespace) to avoid ordering
// and redefinition issues; the summarize() free functions are gated on the
// same __has_include guards.

#if __has_include("../../lithe_ir/portable/opt/manager.hpp")
#  include "../../lithe_ir/portable/opt/manager.hpp"
#  define LITHE_INSPECT_HAS_PASS_RECORD 1
#endif

#if __has_include("../../lithe_execution/store/artifact_record.hpp")
#  include "../../lithe_execution/store/artifact_record.hpp"
#  define LITHE_INSPECT_HAS_ARTIFACT_RECORD 1
#endif

#if __has_include("../../lithe_execution/execution_plan.hpp")
#  include "../../lithe_execution/execution_plan.hpp"
#  define LITHE_INSPECT_HAS_EXECUTION_PLAN 1
#endif

namespace lithe::ir::inspect {
    // =============================================================================
    // pass_record_summary — value summary of impl-2 optimizer provenance
    // =============================================================================

    struct pass_run_summary {
        std::uint16_t pass_id = 0;
        std::uint16_t version_major = 1;
        std::uint16_t version_minor = 0;
        bool applied = false;
        bool skipped = false;
    };

    struct pass_record_summary {
        std::string pipeline_name;
        std::uint16_t pipeline_major = 0;
        std::uint16_t pipeline_minor = 0;
        std::vector<pass_run_summary> runs;
        std::array<std::uint8_t, 64> provenance_digest{};
    };

    // =============================================================================
    // artifact_provenance_summary — value summary of impl-3 artifact provenance
    // =============================================================================

    struct upgrade_step_summary {
        std::string upgrader_id;
        std::uint16_t from_major = 0;
        std::uint16_t from_minor = 0;
        std::uint16_t to_major = 0;
        std::uint16_t to_minor = 0;
    };

    struct artifact_provenance_summary {
        std::string pipeline_name;
        std::uint16_t pipeline_major = 0;
        std::uint16_t pipeline_minor = 0;
        std::string backend_name;
        std::uint16_t backend_major = 0;
        std::uint16_t backend_minor = 0;
        std::vector<upgrade_step_summary> upgrade_chain;
    };

    // =============================================================================
    // plan_summary — value summary of impl-4 execution_plan
    // =============================================================================

    struct plan_summary {
        std::string selected_backend;
        std::string selected_reason;
        std::uint32_t fallback_count = 0;
        double estimated_latency = 0.0;
        double estimated_throughput = 0.0;
        bool cancellable = false;
    };

    // =============================================================================
    // provenance_view — single read of all pipeline/backend/plan provenance
    // =============================================================================

    struct provenance_view {
        std::optional<pass_record_summary> optimizer;
        std::optional<artifact_provenance_summary> artifact;
        std::optional<plan_summary> plan;
    };

    // =============================================================================
    // summarize() free functions — each guarded by upstream milestone availability
    // =============================================================================

#ifdef LITHE_INSPECT_HAS_PASS_RECORD
    [[nodiscard]] inline pass_record_summary
    summarize(const lithe::ir::portable::opt::pass_record& rec) {
        pass_record_summary s;
        s.pipeline_name = rec.id.name;
        s.pipeline_major = rec.version.major;
        s.pipeline_minor = rec.version.minor;
        s.runs.reserve(rec.entries.size());
        for (const auto& e : rec.entries) {
            pass_run_summary r;
            r.pass_id = static_cast<std::uint16_t>(e.id);
            r.version_major = e.version.major;
            r.version_minor = e.version.minor;
            r.applied = (e.outcome == lithe::ir::portable::opt::pass_outcome::changed);
            r.skipped = e.skipped;
            s.runs.push_back(r);
        }
        s.provenance_digest = lithe::ir::portable::opt::pipeline_provenance_digest(rec);
        return s;
    }
#endif

#ifdef LITHE_INSPECT_HAS_ARTIFACT_RECORD
    [[nodiscard]] inline artifact_provenance_summary
    summarize(const ::lithe::execution::store::provenance& prov) {
        artifact_provenance_summary s;
        s.pipeline_name = prov.pipe.name;
        s.pipeline_major = prov.pipe_ver.major;
        s.pipeline_minor = prov.pipe_ver.minor;
        if (prov.backend) {
            s.backend_name = prov.backend->name;
            s.backend_major = prov.backend_ver ? prov.backend_ver->major : std::uint16_t{0};
            s.backend_minor = prov.backend_ver ? prov.backend_ver->minor : std::uint16_t{0};
        }
        s.upgrade_chain.reserve(prov.upgrades.size());
        for (const auto& u : prov.upgrades) {
            upgrade_step_summary us;
            us.upgrader_id = u.upgrader_id;
            us.from_major = u.from_major;
            us.from_minor = u.from_minor;
            us.to_major = u.to_major;
            us.to_minor = u.to_minor;
            s.upgrade_chain.push_back(us);
        }
        return s;
    }
#endif

#ifdef LITHE_INSPECT_HAS_EXECUTION_PLAN
    [[nodiscard]] inline plan_summary
    summarize(const ::lithe::execution::execution_plan& plan) {
        plan_summary s;
        s.selected_backend = std::string{plan.selected.value};
        s.selected_reason = std::string{plan.selected_reason};
        s.fallback_count = static_cast<std::uint32_t>(plan.fallback_chain.size());
        s.estimated_latency = plan.estimated_cost.latency;
        s.estimated_throughput = plan.estimated_cost.throughput;
        s.cancellable = plan.cancellable;
        return s;
    }
#endif
} // namespace lithe::ir::inspect
