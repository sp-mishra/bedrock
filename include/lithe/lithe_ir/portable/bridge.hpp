#pragma once

// =============================================================================
// lithe_ir/portable/bridge.hpp — opt-in umbrella for the live MIR bridge
//
// Includes freeze.hpp and thaw.hpp which pull lithe_codegen.hpp.
// The light IR core (lithe_ir_core.hpp) does NOT include this file so that
// lithe_ir remains free of the codegen dependency.
//
// Include this header (or freeze.hpp / thaw.hpp individually) only when the
// live↔portable MIR bridge is required.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "freeze.hpp"
#include "thaw.hpp"
