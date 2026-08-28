#pragma once

// =============================================================================
// lithe_ir/inspect/inspect.hpp — umbrella include for IR introspection (opt-in)
//
// Namespace: lithe::ir::inspect
//
// Aggregates the four pure (codegen-free) inspect headers:
//   handles.hpp       — ir_family, unit_id, entity_ref, stage_key, options, error
//   view.hpp          — ir_view concept, graph_view, hl_mir_view,
//                       physical_mir_view, any_ir_view
//   provenance_view.hpp — provenance_view + guarded summarize() adapters
//   inspector.hpp     — ir_inspector facade
//
// NOT pulled by lithe.hpp — include explicitly when introspection is needed.
// Pulls codegen-free headers only.
//
// To also get inspect_live (requires lithe_codegen.hpp via freeze.hpp):
//   #include "lithe/lithe_ir/inspect/live.hpp"
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "handles.hpp"
#include "view.hpp"
#include "provenance_view.hpp"
#include "inspector.hpp"
