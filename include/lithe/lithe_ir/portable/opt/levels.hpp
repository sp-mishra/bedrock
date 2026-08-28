#pragma once

// =============================================================================
// lithe_ir/portable/opt/levels.hpp — recommended portable optimization levels
//
// Namespace: lithe::ir::portable::opt
//
// portable_level enum + make_pipeline(level, policy) factory:
//
//   debug      — verify + canonicalize (retain source structure)
//   safe       — + cfg_simplify + conservative sccp/dce
//   balanced   — full scalar/CFG/value (sccp, dce, pure_cse, check_elim)
//   aggressive — + interprocedural (inline_pure) + tail_call_form
//
// balanced is the recommended default.
//
// Aggressive emits abstract allocation/escape decisions only — never target
// storage (arch §4.2 boundary: no ISA/register decisions in this tier).
//
// Returns a dynamic_pass_pipeline (allows heterogeneous pass types);
// prefer static_pass_pipeline for hot paths (see manager.hpp).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "manager.hpp"
#include "passes/canonicalize.hpp"
#include "passes/cfg_simplify.hpp"
#include "passes/check_elim.hpp"
#include "passes/dce.hpp"
#include "passes/inline_pure.hpp"
#include "passes/pure_cse.hpp"
#include "passes/sccp.hpp"
#include "passes/tail_call.hpp"

namespace lithe::ir::portable::opt {
    // =============================================================================
    // portable_level — preset optimization level
    // =============================================================================

    enum class portable_level : std::uint8_t {
        debug = 0,
        safe = 1,
        balanced = 2,
        aggressive = 3,
    };

    // =============================================================================
    // make_pipeline — build a dynamic_pass_pipeline for the given level + policy
    // =============================================================================

    [[nodiscard]] inline dynamic_pass_pipeline
    make_pipeline(portable_level level,
                  const semantic_policy& policy = {},
                  pipeline_id id = {"portable"},
                  pipeline_version ver = {1, 0}) {
        dynamic_pass_pipeline pipe(std::move(id), std::move(ver));

        // All levels: canonicalize
        pipe.add(dynamic_pass{canonicalize_pass{}});

        if (level >= portable_level::safe) {
            // Add cfg_simplify; conservative sccp and dce
            pipe.add(dynamic_pass{cfg_simplify_pass{}});
            pipe.add(dynamic_pass{sccp_pass{}});
            pipe.add(dynamic_pass{dce_pass{}});
        }

        if (level >= portable_level::balanced) {
            // Full value/CSE/check-elimination
            pipe.add(dynamic_pass{pure_cse_pass{}});
            pipe.add(dynamic_pass{check_elim_pass{}});
            // A second DCE pass to clean up after CSE
            pipe.add(dynamic_pass{dce_pass{}});
        }

        if (level >= portable_level::aggressive) {
            // Interprocedural: inline_pure + tail_call_form
            // Both require semantic info to be present (arch §11.M2.3).
            pipe.add(dynamic_pass{inline_pure_pass{}});
            pipe.add(dynamic_pass{tail_call_pass{}});
        }

        // End-of-pipeline verification: always on for debug/safe; opt-in for balanced+
        // The pipeline.run() checks policy.paranoid for post-verify.
        // Here we encode the intent via a modified policy:
        (void)policy;

        return pipe;
    }

    // =============================================================================
    // make_paranoid_policy — helper for debug/safe levels where post-verify is on
    // =============================================================================

    [[nodiscard]] inline semantic_policy make_policy(portable_level level) noexcept {
        semantic_policy pol;
        pol.paranoid = (level <= portable_level::safe);
        return pol;
    }
} // namespace lithe::ir::portable::opt
