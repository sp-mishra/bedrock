#pragma once

#include "lithe_core.hpp"
#include "lithe_extension.hpp"
#include "lithe_semantic.hpp"
#include "lithe_passes.hpp"
#include "lithe_lowering.hpp"
#include "lithe_codegen.hpp"
#include "lithe_codegen_device.hpp"
#include "backends/lithe_codegen_debug_text_backend.hpp"
#include "backends/lithe_codegen_interpreter.hpp"
#include "backends/lithe_codegen_backend_registry.hpp"
#include "lithe_execution.hpp"
#include "lithe_engine.hpp"

// Adaptive learning, scheduling, distributed execution, durable storage, and
// target-specific backends are deliberately opt-in headers.  The stable Lithe
// façade remains a local compiler/execution surface with no policy engine.
