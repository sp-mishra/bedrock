#pragma once

// =============================================================================
// lithe_ir.hpp — umbrella header for the lithe IR layer
//
// Aggregates all lithe_ir/ sub-headers and provider/adapter extensions:
//   format             — encoding, stage, wire_endian, schema_version,
//                        stable_ir_id, format_descriptor, IR views + owned docs
//   provider           — CPO tags, ir_resolution_state, provider concepts
//   integration        — imported_ir, compile helpers, engine_integration
//   hooks              — pipeline hook seam
//   security_envelope  — binary IR envelope, section_entry, validation
//   upgrade            — upgrade_registry, upgrade_key
//   registry           — provider_descriptor, stage_set, encoding_set,
//                        ir_provider_registry, no_ir_provider_registry
//   providers/binary_provider — binary_provider<DV,SV>
//   providers/text_provider   — text_provider
//   adapters/graph     — graph IR stage adapter
//   adapters/hl_mir    — HL MIR stage adapter
//   adapters/physical_mir — physical MIR stage adapter
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "lithe_ir/format.hpp"
#include "lithe_ir/provider.hpp"
#include "lithe_ir/integration.hpp"
#include "lithe_ir/hooks.hpp"
#include "lithe_ir/security_envelope.hpp"
#include "lithe_ir/upgrade.hpp"
#include "lithe_ir/registry.hpp"
#include "lithe_ir/providers/binary_provider.hpp"
#include "lithe_ir/providers/text_provider.hpp"
#include "lithe_ir/adapters/graph.hpp"
#include "lithe_ir/adapters/hl_mir.hpp"
#include "lithe_ir/adapters/physical_mir.hpp"
