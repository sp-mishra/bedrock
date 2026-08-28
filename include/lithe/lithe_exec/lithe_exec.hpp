#pragma once

// =============================================================================
// lithe_exec/lithe_exec.hpp — opt-in umbrella for the automatic execution
//                             analysis and planning layer
//
// Namespace: lithe::exec
//
// Includes all lithe_exec sub-headers in dependency order.
//
// IMPORTANT: This header is NOT included by lithe/lithe.hpp.
// Include it explicitly when you need the automatic execution analysis layer:
//   #include "lithe/lithe_exec/lithe_exec.hpp"
//
// Layer boundary:
//   lithe::exec (this layer)          — WHAT mode? (legality, profitability, plan)
//   lithe::execution (lithe_execution/) — HOW to run? (compile, install, registry)
// These are complementary, not redundant. exec_bridge.hpp provides the mapping
// between execution_kind (lithe::exec) and execution_mode (lithe::execution).
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

// Vocabulary / enums (no deps)
#include "exec_kinds.hpp"
#include "exec_hint.hpp"

// Analysis summaries (depend on exec_kinds)
#include "effect_summary.hpp"
#include "memory_summary.hpp"
#include "reduction.hpp"
#include "layout_summary.hpp"
#include "runtime_guard.hpp"

// Plan aggregate (depends on summaries + lithe_codegen_hl)
#include "execution_plan.hpp"

// Analysis passes (depend on all summaries + plan)
#include "legality.hpp"
#include "profitability.hpp"
#include "selection.hpp"

// Orchestrator (depends on everything)
#include "analysis_pipeline.hpp"

// Bridge to lithe::execution infrastructure (execution_kind ↔ execution_mode)
#include "exec_bridge.hpp"
