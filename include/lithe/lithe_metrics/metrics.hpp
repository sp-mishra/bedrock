#pragma once

// =============================================================================
// lithe_metrics/metrics.hpp — opt-in umbrella for the stage-metrics spine
//
// Aggregates: stage.hpp + recorder.hpp + metrics_view.hpp
//
// NOT pulled by lithe.hpp.  Include explicitly when metrics collection is needed.
// Absent → engine behaves exactly as without instrumentation.
// =============================================================================

#include "stage.hpp"
#include "recorder.hpp"
#include "metrics_view.hpp"
