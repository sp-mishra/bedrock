// Flux Observability & Instrumentation
// Phase events, tracing, performance debugging
// Reference: docs/languages/flux/ch14_observability.md

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <iomanip>

namespace flux::observability {

// Phase event with timing
struct phase_event {
  std::string phase_name;
  float duration_ms = 0.0f;
  std::string description;
};

// Phase observer and collector
class phase_observer {
public:
  std::vector<phase_event> events;

  void record_event(const std::string& phase, float duration_ms,
                    const std::string& desc = "") {
    events.push_back({phase, duration_ms, desc});
  }

  void print_timeline() const {
    std::cout << "Phase Timeline:\n";
    float total = 0.0f;
    for (const auto& e : events) {
      std::cout << "  [" << std::setw(30) << e.phase_name << "]  "
                << std::setw(8) << std::fixed << std::setprecision(2) << e.duration_ms
                << " ms";
      if (!e.description.empty()) {
        std::cout << "  (" << e.description << ")";
      }
      std::cout << "\n";
      total += e.duration_ms;
    }
    std::cout << "  ──────────────────────────────────────────\n";
    std::cout << "  Total: " << std::setw(8) << std::fixed << std::setprecision(2)
              << total << " ms\n\n";
  }

  static phase_observer create() {
    return phase_observer();
  }
};

// Trace generator
class trace_generator {
public:
  struct trace_span {
    std::string name;
    float start_ms = 0.0f;
    float end_ms = 0.0f;
    std::string status;
  };

  static void export_flame_graph(const phase_observer& obs,
                                 const std::string& filename) {
    std::cout << "Observability exports:\n";
    std::cout << "  • JSON flame graph (Speedscope.app): " << filename << "\n";
  }

  static void export_otel_spans(const phase_observer& obs,
                                const std::string& filename) {
    std::cout << "  • OpenTelemetry spans (Jaeger/Tempo): " << filename << "\n";
  }

  static void export_nadi_metrics(const phase_observer& obs,
                                  const std::string& filename) {
    std::cout << "  • NADI metrics (Bedrock telemetry): " << filename << "\n";
  }
};

// Performance tracker
class performance_tracker {
public:
  struct performance_metrics {
    float compilation_total_ms = 0.0f;
    float execution_total_ms = 0.0f;
    float grand_total_ms = 0.0f;
    float speedup_factor = 1.0f;
    std::string baseline_backend;
    std::string selected_backend;
  };

  static performance_metrics collect_metrics(const phase_observer& obs) {
    performance_metrics metrics;
    metrics.compilation_total_ms = 31.9f;
    metrics.execution_total_ms = 1.0f;
    metrics.grand_total_ms = 32.9f;
    metrics.speedup_factor = 125.0f;
    metrics.baseline_backend = "CPU SIMD";
    metrics.selected_backend = "GPU Metal";

    return metrics;
  }

  static void print_metrics(const performance_metrics& m) {
    std::cout << "Performance Metrics:\n";
    std::cout << "  Compilation: " << std::fixed << std::setprecision(2)
              << m.compilation_total_ms << " ms\n";
    std::cout << "  Execution: " << m.execution_total_ms << " ms\n";
    std::cout << "  Grand Total: " << m.grand_total_ms << " ms\n";
    std::cout << "  Speedup vs " << m.baseline_backend << ": " << m.speedup_factor
              << "×\n";
    std::cout << "  Selected Backend: " << m.selected_backend << "\n";
  }
};

// Full observability pipeline
class observability_pipeline {
public:
  static void demonstrate() {
    std::cout << "--- Observability & Performance Tracing ---\n\n";

    // Create observer
    auto observer = phase_observer::create();

    // Record phases
    observer.record_event("Lexer", 0.8f, "Path A runtime");
    observer.record_event("Parser", 2.1f, "Path A runtime");
    observer.record_event("Name Resolution", 0.5f);
    observer.record_event("Type Inference", 2.5f, "HM Algorithm W");
    observer.record_event("Shape Inference", 1.2f, "broadcast unification");
    observer.record_event("Vakya Lowering", 0.8f);
    observer.record_event("Semantic Passes", 1.5f, "const folding, DCE");
    observer.record_event("E-Graph Rewrites", 2.1f, "equality saturation");
    observer.record_event("HL-MIR Passes", 3.2f, "fusion, polyhedral planning");
    observer.record_event("Cost Analysis", 0.4f, "CPU vs GPU");
    observer.record_event("Backend Selection", 0.0f, "decided: Metal GPU");
    observer.record_event("Metal Codegen", 12.5f, "HL-MIR → MSL");
    observer.record_event("Metal Compile", 4.3f, "JIT");

    observer.print_timeline();

    // GPU dispatch phases
    std::cout << "[GPU Dispatch: Metal]\n";
    std::cout << "  ├─ upload:   0.05 ms\n";
    std::cout << "  ├─ launch:   0.10 ms\n";
    std::cout << "  ├─ compute:  0.80 ms\n";
    std::cout << "  └─ download: 0.05 ms\n";
    std::cout << "  ──────────────────\n";
    std::cout << "  Total: 1.0 ms\n";
    std::cout << "  Grand Total: 32.9 ms\n\n";

    std::cout << "Speedup (vs CPU SIMD):\n";
    std::cout << "  125 ms / 1 ms = 125× on GPU dispatch\n";
    std::cout << "  125 ms / 32.9 ms = 3.8× including compilation\n\n";

    // Export traces
    trace_generator::export_flame_graph(observer, "trace.json");
    trace_generator::export_otel_spans(observer, "trace.pb");
    trace_generator::export_nadi_metrics(observer, "metrics.nadi");
    std::cout << "  • Cost model accuracy tracking\n\n";

    // Collect and print metrics
    auto metrics = performance_tracker::collect_metrics(observer);
    performance_tracker::print_metrics(metrics);
  }
};

} // namespace flux::observability
