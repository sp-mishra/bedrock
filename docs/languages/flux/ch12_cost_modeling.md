# Chapter 12 — Cost Modeling and Execution Planning

## Overview

Cost modeling in Flux enables **intelligent backend selection**: CPU (scalar/SIMD) vs. GPU (Metal/Vulkan).

Every Flux program produces a **cost vector** that predicts:
- Compute operations
- Memory bandwidth
- Latency (wall-clock time)
- Power draw

The **decision engine** ranks candidates and selects the best backend based on user **execution policy**.

---

## Cost Vector

```cpp
struct cost_vector {
  float compute_ops;     // FLOPs or operations
  float memory_bytes;    // Allocated + transferred
  float latency_ms;      // Predicted wall-clock
  float power_mw;        // Watts (GPU focus)
};
```

**Total score** combines all dimensions:
```
score = compute_ops + memory_bytes × 0.001 + latency_ms × 100
```

Lower score = better candidate.

---

## Cost Estimation Pipeline

### 1. Region Analysis

A region is a loop or tensor operation block. Flux analyzes:
- **Loop bounds** (static or symbolic)
- **Array shapes** (from vakya::types::shape)
- **Compute intensity** (ops per byte loaded)

```
Example: element-wise add
  shape: [1024, 1024]
  compute_intensity = 1 (one add per 2 loads)
  memory = 1M × 8 bytes = 8 MB
  compute_ops = 1M
```

### 2. Per-Backend Latency Model

Lithe provides empirical latency models for each backend:

| Backend      | Model                          | Overhead     |
|--------------|--------------------------------|--------------|
| CPU Scalar   | ops / 1 GHz                   | 0 ms         |
| CPU SIMD     | ops / (1 GHz × 8)             | 0 ms         |
| Metal        | ops / 1 THz (theoretical)     | 0.1 ms (GPU) |
| Vulkan       | ops / 0.8 THz                 | 0.2 ms       |

Actual latency = model result + backend overhead.

### 3. Memory Bandwidth

GPU bandwidth (Metal: ~200 GB/s on M1/M2) dominates latency for bandwidth-bound ops:

```
latency_ms = memory_bytes / bandwidth_gbps × 1000
```

For compute-bound ops, kernel launch overhead dominates.

---

## Decision Engine

### Execution Policies

Flux supports three policies:

```cpp
enum class execution_policy {
  prefer_device,   // GPU if profitable
  require_device,  // GPU only (for testing)
  host_only,       // CPU/SIMD only
};
```

**Algorithm:**

```
1. Estimate cost for each available backend
2. If policy == host_only:
     return argmin(cpu_scalar, cpu_simd)
3. If policy == require_device:
     return metal (or vulkan if metal unavailable)
4. If policy == prefer_device:
     return argmin(all backends)
```

### Threshold Heuristics

Flux does NOT dispatch to GPU for:
- Small data (< 64 KiB)
- Bandwidth-bound ops with poor GPU utilization
- Ops requiring host-device transfers

**Transfer cost** is modeled as:
```
transfer_ms = bytes_up / upload_bandwidth
            + compute_ms
            + bytes_down / download_bandwidth
```

If `transfer_ms > compute_ms`, stay on CPU.

---

## Cost Registry

Lithe maintains a **cost registry** with platform-specific data:

```cpp
struct platform_profile {
  std::string backend_name;
  float peak_flops;
  float peak_bandwidth_gbps;
  float launch_overhead_ms;
  float device_memory_mb;
};
```

Flux queries this registry during compilation. On macOS:

```
Metal M1/M2:
  peak_flops: 2.6 TFLOPS (f32)
  bandwidth: 200 GB/s
  launch: 0.1 ms
  device_memory: 4-8 GB (shared with CPU)
```

---

## Feedback Loop

After execution, Flux collects **actual metrics** and updates cost estimates:

```
predicted_ms = 5.0
actual_ms = 4.2
error = 4.2 / 5.0 = 0.84 ✓ (within 10% is good)
```

Over time, models converge to real system behavior.

---

## Example: Vector Addition

```flux
fn add_vectors(x: [f32; 1024], y: [f32; 1024]) -> [f32; 1024] {
    x + y
}
```

**Cost estimation:**

```
CPU Scalar:
  compute_ops = 1024
  memory_bytes = 3 × 1024 × 4 = 12 KB
  latency = 1024 / 1e9 = 1 µs
  score = 1024 + 12 + 0.001 ≈ 1036

CPU SIMD (8×):
  latency = 1024 / (1e9 × 8) = 0.125 µs
  score = 1024 + 12 + 0.0125 ≈ 1036

Metal GPU:
  latency = max(transfer_up + compute + transfer_down, compute)
           = 0.1 ms (GPU launch overhead dominates)
  score = 1024 + 12 + 100 ≈ 1136
```

**Decision:** CPU SIMD wins (overhead not worth it for small data).

---

## Example: Large Matrix Multiply

```flux
fn matmul(a: [f32; 1024, 1024], b: [f32; 1024, 1024]) -> [f32; 1024, 1024] {
    lithe::matmul(a, b)
}
```

**Cost estimation:**

```
CPU SIMD:
  compute_ops = 1024³ = 1B
  latency = 1B / 8e9 = 125 ms
  score ≈ 12500

Metal GPU:
  compute_ops = 1B
  memory = 3 × 1024² × 4 ≈ 12 MB
  latency = 1B / 1e12 = 1 ms + 0.1 ms overhead = 1.1 ms
  transfer = 12 MB / 200 GB/s ≈ 0.06 ms (stays on device)
  score ≈ 1000 + 12 + 110 ≈ 1122
```

**Decision:** Metal GPU (100× speedup).

---

---

## See It in Action

Architecture example shows cost-driven backend selection:
[`src/examples/languages/flux/flux_compiler_architecture.hpp`](../../../src/examples/languages/flux/flux_compiler_architecture.hpp)
(Search for `demonstrate_cost_driven_execution()`)

The example compares:
- **Scenario 1**: Small vector (1K elements) → stays on CPU SIMD
- **Scenario 2**: Large matrix (1M elements) → dispatches to Metal GPU (100× speedup)

---

## Integration with Lithe

Flux uses Lithe's built-in admission control:

```cpp
lithe::execution_admission admitter;
admitter.set_max_device_memory(256.0f); // MB

if (admitter.admit(gpu_graph)) {
  dispatch_to_gpu(gpu_graph);
} else {
  dispatch_to_cpu(scalar_fallback);
}
```

If the graph exceeds device memory budget, it **gracefully falls back** to CPU.

---

## Observability

Cost decisions are logged via **phase events**:

```
[Type Inference]     5 ms (CPU)
[Lowering]           3 ms (CPU)
[Cost Analysis]      2 ms (CPU)
[Backend Selection]  1 ms (decided: Metal)
[Metal Codegen]      8 ms (Metal JIT)
[Execution]        1.1 ms (Metal GPU)
─────────────────────────────
Total:             20.1 ms
```

Each phase records:
- Backend used
- Cost score
- Wall-clock duration
- Timestamp (for distributed tracing)

---

## Tuning

Users can control cost modeling via **execution hints**:

```flux
// Force GPU even if small
#[execution(require_device)]
fn gpu_only() { ... }

// Disable GPU for testing
#[execution(host_only)]
fn cpu_only() { ... }

// Use default heuristic
fn auto_select() { ... }
```

---

## See Also

- [Chapter 13: GPU Execution](ch13_gpu_execution.md)
- [Chapter 14: Observability](ch14_observability.md)
- Lithe: `lithe_cost_model.hpp`, `lithe_execution_admission.hpp`
