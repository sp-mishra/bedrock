#pragma once

// =============================================================================
// lithe_algorithms.hpp — umbrella header for the lithe algorithms layer
//
// Aggregates all lithe_algorithms/ sub-headers:
//   pipeline   — analysis_id, preserved_analysis_set, pass_descriptor,
//                analysis_cache, pipeline_pass
//   selection  — algorithm_descriptor, backend_selection, backend_selector,
//                algorithm_pack, algorithm_box, cost_based_backend_selector
//   lifecycle  — lifecycle_policy, lifecycle_hooks
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "lithe_algorithms/pipeline.hpp"
#include "lithe_algorithms/selection.hpp"
#include "lithe_algorithms/lifecycle.hpp"
