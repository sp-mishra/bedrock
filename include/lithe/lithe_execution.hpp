#pragma once

// =============================================================================
// lithe_execution.hpp — umbrella header for the lithe execution layer
//
// Aggregates all lithe_execution/ sub-headers:
//   foundation  — ir_kind, ir_error, execution_mode, backend_capability_set
//   capability  — compile_requirements, mode_gate, security_policy
//   identity    — backend_id, persisted_backend_id
//   facet       — facet_set, no_facet
//   artifact    — artifact_descriptor, aot_image_ref
//   resource    — resource_limits, resource_scope
//   entry       — entry_point, entry_lease
//   aot         — aot_header, aot_image
//   registry    — backend_registry, default_backend_registry
//
// Unified local execution (impl-4, OPT-IN via lithe_execution/lithe_execution.hpp):
//   execution_result   — execution_outcome<T>, execution_failure, failure_stage
//   target_profile     — target_profile, discover_target_profile, fingerprint
//   execution_plan     — execution_plan, make_execution_plan, execute_plan
//   backend_persist    — object_persist_codec, spirv_persist_codec, backend_persist_tag
//   exec_profiling     — exec_profiler<Sink>, NADI "lithe.exec" hooks
//   engine_execute     — run() portable-first end-to-end entry
//
// Note: the unified execution layer (impl-4) is NOT pulled by lithe.hpp —
// it is opt-in, matching the convention of lithe_execution/store/ and lithe_rt/.
// Include this umbrella explicitly when you need the full execution layer.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "lithe_execution/foundation.hpp"
#include "lithe_execution/capability.hpp"
#include "lithe_execution/identity.hpp"
#include "lithe_execution/facet.hpp"
#include "lithe_execution/artifact.hpp"
#include "lithe_execution/resource.hpp"
#include "lithe_execution/entry.hpp"
#include "lithe_execution/aot.hpp"
#include "lithe_execution/registry.hpp"

// impl-4 unified execution layer (opt-in)
#include "lithe_execution/execution_result.hpp"
#include "lithe_execution/target_profile.hpp"
#include "lithe_execution/execution_plan.hpp"
#include "lithe_execution/backend_persist.hpp"
#include "lithe_execution/exec_profiling.hpp"
#include "lithe_execution/engine_execute.hpp"
