#pragma once

// =============================================================================
// lithe_execution/store/store.hpp — opt-in umbrella for the store subsystem
//
// Aggregates all impl-3 durable-artifact headers under a single opt-in include.
// NOT pulled by lithe.hpp — consumers must include this explicitly.
//
// The stable catalog contract and in-memory implementation are always
// available.  Durable persistence is supplied by the Petika adapter, which is
// opt-in and keeps Petika types out of the core store surface.
//
// Includes:
//   artifact_record.hpp  — artifact_record, artifact_key, provenance, compat_manifest
//   envelope.hpp         — artifact_envelope codec (encode/decode_artifact)
//   blob_store.hpp       — artifact_store concept + filesystem_blob_store
//   catalog.hpp          — catalog concept, memory_catalog, compatibility and publish flow
//   petika_catalog.hpp   — opt-in Petika-backed catalog adapter
//   resident_cache.hpp   — decoded_ir_cache, installed_code_cache, retirement_queue
//
// Generic library dependencies (not Lithe-specific, documented in reference.md):
//   containers/content_store.hpp  (G3) — filesystem_content_store
//   utils/single_flight.hpp       (G4) — per-key compute-once-under-contention
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include "artifact_record.hpp"
#include "envelope.hpp"
#include "blob_store.hpp"
#include "catalog.hpp"
#include "petika_catalog.hpp"
#include "resident_cache.hpp"
