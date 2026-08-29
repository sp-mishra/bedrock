# Chapter 14 — Observability & Instrumentation

## Overview

Flux programs are **fully observable** at compile-time and runtime via **Nadi telemetry**.

Every compilation and execution produces structured **phase events** that reveal:
- Where time is spent (lexing? typechecking? GPU dispatch?)
- Backend decisions and why
- Cost model predictions vs. actual performance
- Device utilization

---

---

## See It in Action

Architecture example shows observability instrumentation:
[`src/examples/languages/flux/flux_compiler_architecture.hpp`](../../../src/examples/languages/flux/flux_compiler_architecture.hpp)
(Search for `verify_three_path_invariant()` and `compilation_pipeline`)

Output shows phase breakdown:
- Path tracing (lexy → parser → AST builder)
- Type pipeline (name resolution → type inference → shape inference)
- Lowering and optimization phases
- Backend selection rationale
- Phase event recording

---

## Phase Events

A **phase event** captures one step in the pipeline:

```cpp
struct phase_event {
  std::string phase_name;        // "type_inference", "metal_codegen", etc.
  int64_t timestamp_us;          // microseconds since epoch
  float duration_ms;             // how long this phase took
  std::string backend;           // "cpu", "simd", "metal", "vulkan"
  float cost_score;              // cost model prediction for this step
  std::string attributes;        // JSON metadata: {"threads": 1024, ...}
};
```

---

## Compilation Pipeline Phases

### Lexical Analysis

```
[Lexer] 0.5 ms (CPU)
```

Tokenizes source. Time grows with source size O(n).

### Parsing

```
[Parser] 1.2 ms (CPU)
```

Converts tokens to AST. Time is O(n) for well-formed input.

### Name Resolution

```
[Name Resolution] 0.3 ms (CPU)
```

Builds symbol table, detects undefined identifiers.

### Type Inference

```
[Type Inference] 2.1 ms (CPU)
  ├─ Algorithm W (HM): 1.8 ms
  ├─ Shape unification: 0.3 ms
```

Hindley-Milner unification; normally linear in program size (worst-case: quadratic for pathological cases).

### Vakya Lowering

```
[Vakya Lowering] 0.8 ms (CPU)
  ├─ AST → vakya::node: 0.5 ms
  ├─ Structural hash: 0.3 ms
```

Converts Flux AST to Lithe's internal IR.

### Lithe Optimization

```
[Semantic Passes] 1.5 ms (CPU)
  ├─ Const folding: 0.3 ms
  ├─ Dead code elimination: 0.5 ms
  ├─ E-graph rewrite: 0.7 ms
[High-Level MIR Passes] 3.2 ms (CPU)
  ├─ Fusion: 1.8 ms
  ├─ Polyhedral planning: 1.4 ms
```

Optimizations are modular; each emits its own phase event.

### Cost Analysis

```
[Cost Analysis] 0.4 ms (CPU)
  ├─ Estimate CPU Scalar: 0.1 ms
  ├─ Estimate CPU SIMD: 0.1 ms
  ├─ Estimate Metal: 0.1 ms
  └─ Backend Selection: decided Metal (score: 1.1 vs. 125 ms)
```

Predicts execution time on each backend and selects winner.

### Code Generation

```
[Metal Codegen] 12.5 ms (Metal JIT)
  ├─ HL-MIR → MSL: 8.2 ms
  ├─ Compile MSL: 4.3 ms
  └─ Semantic cache: MISS (new kernel)
```

Generates and compiles GPU code. Cache hits are ~1 ms (bytecode reuse).

---

## Execution Phases

After compilation, execution also produces events:

### GPU Dispatch

```
[GPU Dispatch: Metal] 1.1 ms (Metal)
  ├─ Buffer upload: 0.05 ms (8 MB → 160 GB/s)
  ├─ Kernel launch: 0.1 ms
  ├─ Compute: 0.8 ms (1B ops @ 1.2 TFLOPS)
  ├─ Download: 0.05 ms
  └─ CPU wait: sync
```

Timing breakdown by sub-phase.

### CPU SIMD Dispatch

```
[CPU Dispatch: SIMD] 125 ms (CPU)
  ├─ Highway SIMD library: 125 ms
```

---

## Observability API

### Collectors

Users can install custom collectors:

```cpp
class my_observer : public phase_observer {
  void on_phase_start(const phase_event& e) override {
    log("Starting: " + e.phase_name);
  }
  void on_phase_end(const phase_event& e) override {
    log("Finished: " + e.phase_name + " in " + to_string(e.duration_ms) + " ms");
  }
};

flux::compilation_context ctx;
ctx.add_observer(std::make_unique<my_observer>());
auto result = flux::compile_and_run(src, ctx);
```

### Flame Graph Export

Flux can export events as JSON for visualization:

```cpp
auto events = ctx.get_all_events();
flux::write_flame_graph_json("trace.json", events);
// Load in speedscope.app or Perfetto
```

JSON format:
```json
[
  {
    "name": "Type Inference",
    "ph": "X",
    "ts": 1000000,
    "dur": 2100,
    "args": {
      "backend": "cpu",
      "cost_score": 42.0
    }
  }
]
```

---

## Cost Model Feedback

After execution, **actual performance** is recorded:

```
Phase: [Metal Codegen]
  Predicted: 12 ms (cost_score = 50)
  Actual: 8 ms
  Error: 8 / 12 = 0.67 ✓ (within budget)

Phase: [GPU Dispatch]
  Predicted: 1.1 ms
  Actual: 0.95 ms
  Error: 0.95 / 1.1 = 0.86 ✓

Overall prediction error: 0.76 (good calibration)
```

Errors feed back into cost model ML components (if enabled).

---

## Memory Profiling

Flux tracks allocations during execution:

```
Memory profile for GPU dispatch:
  Device buffers:
    - x (8 MB): kept on device
    - y (8 MB): kept on device
    - result (8 MB): produced on device
  Total device allocation: 24 MB
  Peak device memory: 24 MB (well under 256 MB budget)

  Host buffers:
    - input x (8 MB): uploaded
    - input y (8 MB): uploaded
    - output (8 MB): downloaded
```

---

## Distributed Tracing

For networked workloads (future), Flux emits **OpenTelemetry spans**:

```python
# Export to Jaeger/Tempo
tracer.start_span("flux_compilation")
  # ... nested spans for each phase ...
tracer.end_span()
```

Timestamps are absolute (UTC), enabling cross-service correlation.

---

## Example: Full Trace

```flux
fn compute_norm(x: [f32; 1_000_000]) -> f32 {
    let xx = x * x;
    lithe::reduce::sum(xx)
}
```

**Trace output:**

```
Flux Compilation & Execution Trace
═════════════════════════════════════

Compilation:
  [Lexer] 0.8 ms
  [Parser] 2.1 ms
  [Name Resolution] 0.5 ms
  [Type Inference] 1.2 ms
  [Vakya Lowering] 0.6 ms
  [Semantic Passes] 1.3 ms
  [HL-MIR Passes] 2.8 ms
    ├─ Fusion: 1.5 ms (fused multiply-sum)
    └─ Polyhedral Planning: 1.3 ms (1D rank planned for GPU)
  [Cost Analysis] 0.4 ms
    └─ Backend Selection: Metal (score: 3.5 vs. CPU: 250)
  [Metal Codegen] 15.2 ms
    ├─ HL-MIR → MSL: 10.1 ms
    ├─ MSL compile: 5.1 ms
    └─ Cache: MISS
  
  Total Compilation: 25 ms

Execution:
  [GPU Dispatch: Metal] 1.3 ms
    ├─ Buffer upload: 0.08 ms
    ├─ Kernel launch: 0.1 ms
    ├─ Compute: 0.95 ms
    ├─ Download: 0.07 ms
  
  Total Execution: 1.3 ms

Overall: 26.3 ms
```

Compare to CPU-only:

```
CPU SIMD dispatch: 250 ms
GPU: 1.3 ms
Speedup: 192×
```

---

## Debugging

Enable verbose tracing for debugging:

```rust
flux::set_log_level(flux::LogLevel::Trace);
```

Output includes:
- Every kernel compiled
- Every decision point (why Metal won over SIMD?)
- Device memory allocations
- GPU dispatch details

---

## Performance Tips

### 1. Check Cost Model Calibration

If `actual_ms > predicted_ms × 2`, cost model is off:

```
Rerun with --observe=detailed
Look for mispredicted phase
File issue with trace JSON
```

### 2. Identify Bottlenecks

```
Compilation: 25 ms (Metal Codegen: 15 ms)
Execution: 1.3 ms

Bottleneck: Metal Codegen (60% of total time)
Action: Use #[noinline] to prevent re-codegen
```

### 3. Monitor Device Utilization

```
GPU kernel: 0.95 ms to compute 1B ops
= 1B / (0.95e-3 sec) = 1.05 TFLOPS
M1 peak: 3.2 TFLOPS
Utilization: 1.05 / 3.2 = 33% (low for such large ops!)

Action: Check if memory bandwidth is bottleneck
```

---

## Integration with NADI

Flux reports phase events to **Nadi** (Bedrock observability library):

```cpp
// In lithe codebase, each phase calls:
nadi::phase_observer::record(
  "flux_type_inference",
  duration_ms,
  {{"inputs", program_size}, {"backend", "cpu"}}
);
```

Nadi aggregates across processes and reports SLOs.

---

## See Also

- [Chapter 12: Cost Modeling](ch12_cost_modeling.md)
- [Chapter 13: GPU Execution](ch13_gpu_execution.md)
- Lithe: `lithe_telemetry.hpp`, `lithe_phase_observer.hpp`
- Nadi: `lithe_nadi.hpp`
