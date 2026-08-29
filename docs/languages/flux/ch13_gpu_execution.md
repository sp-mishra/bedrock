# Chapter 13 — GPU Execution (Metal & Vulkan)

## Overview

Flux leverages **Lithe's native GPU backends** to dispatch tensor operations to Metal (macOS) or Vulkan (cross-platform).

The pipeline:
```
Flux AST
  ↓
Vakya lowering
  ↓
Lithe HL-MIR (high-level IR)
  ↓  Fusion pass
  ↓  Region detection
  ↓
Execution planning
  ↓
Backend selection (Metal/Vulkan/SIMD/CPU)
  ↓
Code generation (MSL / SPIR-V)
  ↓
GPU dispatch via Pravaha
```

---

## Device Compatibility

### Metal (macOS, A/M-series)

- **Supported operations**: element-wise (add, mul, div, sin, cos, etc.), reductions
- **Data types**: f32, f64 (on M-series)
- **Memory**: unified memory (no explicit upload/download for most workflows)
- **Launch overhead**: ~0.1 ms

### Vulkan (Linux, Windows, macOS via MoltenVK)

- **Supported operations**: same as Metal
- **Data types**: f32 (f64 with extensions)
- **Memory**: discrete buffer management
- **Launch overhead**: ~0.2 ms

---

## Execution Graph

Lithe represents device work as a **polyhedral execution graph**:

```cpp
struct gpu_execution_graph {
  int num_nodes;              // computation kernels
  int num_edges;              // data dependencies
  std::vector<tensor> inputs;
  std::vector<tensor> outputs;
  std::vector<tensor> intermediates;
  backend_kind backend;
  float estimated_memory_mb;
};
```

Each node is a **rank-1 contiguous tensor operation** (for now; sparse and irregular patterns require explicit ABI later).

---

---

## See It in Action

Architecture example demonstrates the full GPU pipeline:
[`src/examples/languages/flux/flux_compiler_architecture.hpp`](../../../src/examples/languages/flux/flux_compiler_architecture.hpp)
(Search for `compilation_pipeline::compile()`)

The example shows:
- Name resolution → type inference → shape inference
- Vakya lowering → Lithe HL-MIR transformation
- Cost model estimates Metal GPU vs CPU SIMD
- Backend selection and codegen dispatch

---

## Supported Kernels

### Element-wise (Broadcasting)

```flux
fn element_wise_add(x: [f32; N], y: [f32; N]) -> [f32; N] {
    x + y
}
```

Flux lowers this to **HL-MIR element-wise node**, which Metal emitter converts to MSL:

```metal
kernel void add_kernel(device float *out [[buffer(0)]],
                       device const float *x [[buffer(1)]],
                       device const float *y [[buffer(2)]],
                       uint id [[thread_position_in_grid]]) {
  out[id] = x[id] + y[id];
}
```

### Reductions

```flux
fn sum_array(x: [f32; N]) -> f32 {
    lithe::reduce::sum(x)
}
```

Metal dispatches a **two-phase reduction**: per-threadgroup partial sums, then final aggregation.

### Convolution & Matrix Operations

Flux delegates to **Metal Performance Shaders** (via Lithe's MPS backend):

```flux
fn matmul(a: [f32; M, K], b: [f32; K, N]) -> [f32; M, N] {
    lithe::matmul(a, b)
}
```

---

## Memory Management

### Unified Memory (Metal on M-series)

Host and device share physical memory. No explicit sync needed in most cases.

```cpp
// Flux user perspective (implicit):
auto result = flux_kernel(input);  // stays on device if next op is also GPU
```

### Discrete Memory (Vulkan, Intel GPUs)

Separate host/device heaps. Flux manages transfers automatically:

```
User input (CPU)
    ↓ upload (DMA)
Device buffer
    ↓ compute
Device output buffer
    ↓ download (async)
User output (CPU)
```

---

## Admission Control

**Execution Admission** gates whether a graph dispatches to GPU:

```cpp
lithe::execution_admission admitter;
admitter.set_max_device_memory(256.0f);  // MB

if (admitter.admit(gpu_graph)) {
  dispatch_to_device();
} else {
  dispatch_to_cpu_fallback();
}
```

**Rejection reasons:**
- Graph memory exceeds `max_device_memory`
- Intermediate tensors don't fit in device cache
- Backend unavailable (e.g., Metal on non-macOS)

On rejection, **automatic fallback** to CPU SIMD (no user intervention).

---

## Async Execution

For independent chains of GPU work, Flux supports **non-blocking dispatch**:

```cpp
// Path A: block until result
auto result = dispatch_f32_binary_sync(graph);

// Path B: return completion token, poll later
auto token = dispatch_f32_binary_async(graph);
// ... do CPU work ...
auto result = token.wait();  // blocks here
```

Async is opt-in; default is sync (simple semantics).

---

## Observability

GPU execution is instrumented via **phase events**:

```
[GPU Dispatch: Metal] 1.1 ms
  ├─ upload: 0.05 ms
  ├─ compute: 0.8 ms
  └─ download: 0.25 ms
```

Each phase is observable via **Nadi telemetry** (see Chapter 14).

---

## Code Generation

### MSL (Metal Shading Language)

Lithe's Metal backend generates MSL kernels directly from HL-MIR:

```
HL-MIR binary_op node (add, f32, rank-1)
  ↓
Legality check (contiguous, f32, no loops)
  ↓
Lower to MSL kernel
  ↓
Compile via metal compiler
  ↓
Cache bytecode by structural hash
```

### SPIR-V (Vulkan)

Similar pipeline for Vulkan:

```
HL-MIR → SPIR-V IR → spirv-tools optimization → shader module
```

Both use **semantic caching** by Kosha: if two Flux programs have identical HL-MIR nodes, they share compiled GPU code.

---

## Example: GPU Vector Add

```flux
#[execution(prefer_device)]
fn gpu_vector_add(
  x: [f32; 1_000_000],
  y: [f32; 1_000_000]
) -> [f32; 1_000_000] {
    x + y
}
```

**Compilation:**

```
1. Parse & type-check: vector [f32; 1M]
2. Vakya lower: + op on two [f32; 1M] tensors
3. Lithe HL-MIR: binary_op(add, f32, rank-1)
4. Cost model: GPU faster (1B ops)
5. Metal codegen: launch 1M threads, each computes x[i] + y[i]
6. MSL compile: 0.8 ms
7. Dispatch: 0.1 ms launch + 0.05 ms upload + 0.8 ms compute + 0.05 ms download
8. Total: 1.0 ms on GPU vs. 125 ms on CPU SIMD
```

---

## Debugging GPU Code

Flux provides **GPU debugging hints**:

```flux
#[debug::gpu_trace]
fn debug_add(x: [f32; 100], y: [f32; 100]) {
    let z = x + y;
    lithe::debug_print(z[0]);  // logs first result
    z
}
```

This emits GPU-side printfs (Metal) or buffer readback (Vulkan).

---

## Limitations (Current)

1. **Rank constraint**: HL-MIR elementwise supports only rank-1 contiguous tensors (no multi-dim stencils yet)
2. **Scalar ops**: No GPU dispatch for truly scalar work
3. **Control flow**: GPU kernels cannot branch on data-dependent conditions
4. **Intra-kernel communication**: No explicit workgroup sync (Metal can use threadgroup memory, but Flux doesn't expose it yet)

---

## See Also

- [Chapter 12: Cost Modeling](ch12_cost_modeling.md)
- [Chapter 14: Observability](ch14_observability.md)
- Lithe: `lithe_codegen_metal.hpp`, `lithe_codegen_vulkan_spirv_ir.hpp`
- Pravaha: device buffer management and dispatch
