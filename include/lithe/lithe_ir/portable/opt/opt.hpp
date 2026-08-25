#pragma once

// =============================================================================
// lithe_ir/portable/opt/opt.hpp — umbrella opt-in include
//
// Opt-in via: #include "lithe/lithe_ir/portable/opt/opt.hpp"
//
// NOT aggregated into lithe_ir_core.hpp (optimizer is opt-in like autotune/viz).
// Add a one-line pointer from lithe_ir/portable/bridge.hpp notes.
//
// Aggregates: pass, analysis, manager, levels, and all passes.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "levels.hpp"   // includes all passes + manager + analysis + pass
