// Flux Cost Modeling & Backend Selection
// Decision engine: CPU scalar vs SIMD vs GPU (Metal/Vulkan)
// Reference: docs/languages/flux/ch12_cost_modeling.md

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include "flux_frontend.hpp"

namespace flux::cost_model {

using shared_expr = flux::frontend::shared_expr;

// Cost vector: (compute_ops, memory_bytes, latency_ms, power_mw)
struct cost_vector {
  long long compute_ops = 0;
  long long memory_bytes = 0;
  float latency_ms = 0.0f;
  float power_mw = 0.0f;

  std::string to_string() const {
    std::string s = "(ops=" + std::to_string(compute_ops);
    s += ", mem=" + std::to_string(memory_bytes / 1024) + "KB";
    s += ", lat=" + std::to_string(static_cast<int>(latency_ms * 1000)) + "µs";
    s += ", pwr=" + std::to_string(static_cast<int>(power_mw)) + "mW)";
    return s;
  }
};

// Backend enumeration
enum class backend_kind {
  cpu_scalar,
  cpu_simd,
  gpu_metal,
  gpu_vulkan
};

// Backend cost estimation
class backend_cost_estimator {
public:
  static cost_vector estimate(const shared_expr& expr,
                              backend_kind backend,
                              int element_count = 1024) {
    cost_vector cv;

    switch (backend) {
      case backend_kind::cpu_scalar:
        cv.compute_ops = element_count;
        cv.memory_bytes = 3 * element_count * 4;
        cv.latency_ms = (element_count / 1000.0f) / 1.0f;  // 1 GHz
        cv.power_mw = 10;
        break;

      case backend_kind::cpu_simd:
        cv.compute_ops = element_count;
        cv.memory_bytes = 3 * element_count * 4;
        cv.latency_ms = (element_count / 1000.0f) / 8.0f;  // 8 GFLOPS
        cv.power_mw = 15;
        break;

      case backend_kind::gpu_metal:
        cv.compute_ops = element_count;
        cv.memory_bytes = 3 * element_count * 4;
        cv.latency_ms = 0.1f + 0.8f + 0.1f;  // launch + compute + overhead
        cv.power_mw = 8;
        break;

      case backend_kind::gpu_vulkan:
        cv.compute_ops = element_count;
        cv.memory_bytes = 3 * element_count * 4;
        cv.latency_ms = 0.15f + 0.8f + 0.15f;  // slightly higher overhead
        cv.power_mw = 10;
        break;
    }

    return cv;
  }

  static std::string backend_name(backend_kind b) {
    switch (b) {
      case backend_kind::cpu_scalar:
        return "CPU Scalar";
      case backend_kind::cpu_simd:
        return "CPU SIMD";
      case backend_kind::gpu_metal:
        return "GPU Metal";
      case backend_kind::gpu_vulkan:
        return "GPU Vulkan";
    }
    return "Unknown";
  }
};

// Decision engine: which backend minimizes cost?
class decision_engine {
public:
  struct decision_result {
    backend_kind selected_backend;
    std::string reason;
    cost_vector selected_cost;
  };

  static decision_result select_backend(const shared_expr& expr,
                                        int element_count = 1024) {
    std::cout << "--- Cost Modeling & Backend Selection ---\n\n";

    // Scenario 1: Small vector
    if (element_count <= 1024) {
      std::cout << "Scenario: Vector operation (" << element_count << " elements)\n";
      std::cout << "  Estimate CPU Scalar: ";
      auto cpu_scalar =
          backend_cost_estimator::estimate(expr, backend_kind::cpu_scalar, element_count);
      std::cout << cpu_scalar.to_string() << "\n";

      std::cout << "  Estimate CPU SIMD:   ";
      auto cpu_simd =
          backend_cost_estimator::estimate(expr, backend_kind::cpu_simd, element_count);
      std::cout << cpu_simd.to_string() << "\n";

      std::cout << "  Estimate GPU Metal:  ";
      auto gpu_metal =
          backend_cost_estimator::estimate(expr, backend_kind::gpu_metal, element_count);
      std::cout << gpu_metal.to_string() << "\n";

      std::cout << "  Decision: CPU SIMD (launch overhead not worth it)\n\n";

      decision_result result;
      result.selected_backend = backend_kind::cpu_simd;
      result.reason = "Launch overhead dominates for small workloads";
      result.selected_cost = cpu_simd;
      return result;
    }

    // Scenario 2: Large matrix
    std::cout << "Scenario: Large matrix multiply (" << element_count << " elements)\n";
    std::cout << "  Estimate CPU SIMD:   ";
    auto cpu_simd =
        backend_cost_estimator::estimate(expr, backend_kind::cpu_simd, element_count);
    std::cout << cpu_simd.to_string() << "\n";

    std::cout << "  Estimate GPU Metal:  ";
    auto gpu_metal =
        backend_cost_estimator::estimate(expr, backend_kind::gpu_metal, element_count);
    std::cout << gpu_metal.to_string() << "\n";

    std::cout << "  Decision: GPU Metal (100× speedup, lower power)\n\n";

    decision_result result;
    result.selected_backend = backend_kind::gpu_metal;
    result.reason = "Large workload benefits from GPU parallelism";
    result.selected_cost = gpu_metal;
    return result;
  }
};

// Execution admission control (GPU memory budget)
class execution_admission_control {
public:
  struct admission_result {
    bool admitted = true;
    std::string reason;
    long long device_budget = 256 * 1024 * 1024;  // 256 MB
    long long graph_memory = 0;
  };

  static admission_result check_admission(const shared_expr& expr,
                                          long long required_memory) {
    std::cout << "Execution Admission Control (GPU memory budget):\n";
    std::cout << "  Device budget: 256 MB\n";
    std::cout << "  Reserved for cache: 64 MB (25%)\n";

    admission_result result;
    result.device_budget = 256 * 1024 * 1024;
    result.graph_memory = required_memory;

    long long available = result.device_budget - 64 * 1024 * 1024;
    if (required_memory <= available) {
      result.admitted = true;
      result.reason = "Within budget";
      std::cout << "  Graph memory: " << (required_memory / (1024 * 1024)) << " MB → admitted ✓\n";
    } else {
      result.admitted = false;
      result.reason = "Exceeds budget, fallback to CPU";
      std::cout << "  Graph memory: " << (required_memory / (1024 * 1024))
                << " MB → rejected → fallback to CPU\n";
    }

    std::cout << "\n";
    return result;
  }
};

} // namespace flux::cost_model
