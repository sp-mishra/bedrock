#pragma once

// =============================================================================
// lithe_ir_core.hpp — lightweight IR format + generic provider declarations
//
// Aggregates:
//   lithe_ir/format.hpp   — encoding, stage, schema_version, format_descriptor,
//                           text/binary IR views and owned documents
//   lithe_ir/provider.hpp — import/export/validate CPOs, typed concepts,
//                           no_ir_provider, ir_resolution_state,
//                           diagnostic_text_stub
//
// NO codegen dependency.  NO parser or serializer.  Declarations only.
// Concrete text/binary codecs arrive in separate provider implementations.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "lithe_ir/format.hpp"
#include "lithe_ir/provider.hpp"

// Portable sub-namespace — pure wire form (no codegen dependency).
// Always aggregated: module, verify, digest, cfg_adapter.
// Opt-in (pulls lithe_codegen.hpp): use lithe_ir/portable/bridge.hpp.
#include "lithe_ir/portable/module.hpp"
#include "lithe_ir/portable/cfg_adapter.hpp"
#include "lithe_ir/portable/verify.hpp"
#include "lithe_ir/portable/digest.hpp"
#include "lithe_ir/portable/codec.hpp"

// Introspection sub-namespace — pure (no codegen).
// Opt-in live path (freeze-then-view): lithe_ir/inspect/live.hpp.
#include "lithe_ir/inspect/handles.hpp"
#include "lithe_ir/inspect/view.hpp"
#include "lithe_ir/inspect/provenance_view.hpp"
#include "lithe_ir/inspect/inspector.hpp"
