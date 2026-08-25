#pragma once

// ============================================================================
// lithe/lithe_rt.hpp — Lithe managed runtime (opt-in aggregate)
//
// The shared execution foundation + managed-heap / GC layer + roots, safepoints,
// managed engine, and exceptions — layered ON TOP of the existing
// lithe::codegen / lithe::runtime headers without modifying them.
//
//   lithe_rt/foundation.hpp     typed-MIR value model + unified trap model
//   lithe_rt/heap.hpp           GC object model + generational_gc collector
//   lithe_rt/execution.hpp      RAII roots + thread attachment + machine stack
//                               maps + stop-the-world safepoint coordinator
//   lithe_rt/code_metadata.hpp  shared code_version_metadata + code_resource
//   lithe_rt/instance.hpp       owning runtime_instance (shared factory)
//   lithe_rt/engine.hpp         managed MIR passes + backend thunks + compile /
//                               invoke surface + language exceptions
//
// Everything lives in namespace lithe::rt.  This header is NOT included by
// lithe/lithe.hpp — the runtime foundation is opt-in, so core Lithe users pay
// nothing for it.
// ============================================================================

#include "lithe_rt/foundation.hpp"
#include "lithe_rt/heap.hpp"
#include "lithe_rt/execution.hpp"
#include "lithe_rt/code_metadata.hpp"
#include "lithe_rt/instance.hpp"
#include "lithe_rt/engine.hpp"
