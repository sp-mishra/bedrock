# Chapter 9 — IR Generation & Backends

## Theory

After optimization, the Vakya tree is **lowered to MIR** (Medium-level Intermediate Representation). MIR is a flat,
sequential instruction format that backends can compile to native code.

### MIR Structure

```
MIR Program
  ├── Function: distance(x: f32, y: f32) → f32
  │     ├── %0 = mul x, x
  │     ├── %1 = mul y, y
  │     ├── %2 = add %0, %1
  │     └── %3 = sqrt %2
  │           return %3
  └── ...
```

MIR is:

- **SSA form** — each value defined exactly once
- **Typed** — every instruction has a type
- **Backend-agnostic** — the same MIR goes to CPU, SIMD, GPU

### Lithe Compilation Pipeline

```
vakya::node (optimized)
    ↓  lithe::semantic_analysis_pass
Typed Vakya
    ↓  lithe::lowering_pass
Lithe HL-MIR (High-Level MIR)
    ↓  lithe::codegen_pipeline
Lithe MIR
    ↓  Backend
CPU / SIMD / GPU
```

---

## The Lithe Pipeline

Lithe's `codegen_pipeline` orchestrates all phases. For Flux, we assemble a standard pipeline:

```cpp
// include/languages/flux/pipeline.hpp
#pragma once
#include <lithe/lithe.hpp>

namespace flux {

struct compile_result {
    lithe::program          program;    // compiled program object
    lithe::execution_report report;     // timing, cost metrics
};

// Compile a Vakya expression to a callable program for the given backend.
template<lithe::BackendKind BK = lithe::BackendKind::CPU>
compile_result compile(lithe::shared_expr const& expr,
                       lithe::compile_options const& opts = {})
{
    // 1. Semantic analysis: type-check Vakya tree via Lithe's semantic pass
    lithe::semantic_context sctx;
    auto sem_result = lithe::analyze_semantics(expr, sctx);
    if (!sem_result.ok()) throw std::runtime_error("Semantic error");

    // 2. Optimization passes
    lithe::pass_pipeline passes;
    passes.add<lithe::constant_folding_pass>();
    passes.add<lithe::algebraic_simplification_pass>();
    passes.add<lithe::dead_code_elimination_pass>();
    if constexpr (BK == lithe::BackendKind::SIMD)
        passes.add<lithe::vectorization_pass>();

    auto opt_expr = passes.run(expr, sctx);

    // 3. Lower to MIR
    auto mir = lithe::lower_to_mir(opt_expr, sctx);

    // 4. Select and run backend
    if constexpr (BK == lithe::BackendKind::CPU)
        return { lithe::cpu_backend::compile(mir, opts), {} };
    else if constexpr (BK == lithe::BackendKind::SIMD)
        return { lithe::simd_backend::compile(mir, opts), {} };
    else if constexpr (BK == lithe::BackendKind::GPU)
        return { lithe::gpu_backend::compile(mir, opts), {} };
    else
        return { lithe::auto_backend::compile(mir, opts), {} };
}

} // namespace flux
```

---

## Semantic Analysis Pass

Lithe's semantic pass (`lithe_semantic.hpp`) adds domain type information to the Vakya tree:

```
domain_type: scalar | vector | matrix | tensor | predicate
```

It also performs **backend routing**: annotates subtrees with which backend can execute them.

```cpp
// After semantic analysis:
// node.semantic_info().domain == domain_type::scalar
// node.semantic_info().routing == backend_routing_policy::cpu_only
//   (or .simd_capable, .gpu_capable, .any)
```

---

## Lowering to MIR

`lithe::lower_to_mir` walks the typed Vakya tree depth-first and emits SSA instructions:

```
sqrt(add(mul(x,x), mul(y,y)))

Emit:
  %0 = load f32, x
  %1 = load f32, y
  %2 = fmul %0, %0
  %3 = fmul %1, %1
  %4 = fadd %2, %3
  %5 = sqrt %4
  ret %5
```

MIR is represented as `lithe::mir_module` (a `lang::ir_module<lithe::mir_kind, lithe::mir_ext>`
under the hood — the same generic IR infrastructure used by Crank).

---

## Backends

### CPU Backend

The CPU backend emits portable C code or uses LLVM IR via the Lithe code generation pipeline. For embedded use (no
LLVM), it uses an interpreter over MIR.

```cpp
auto prog = lithe::cpu_backend::compile(mir);
float result = prog.call<float>(3.0f, 4.0f);  // distance(3,4) = 5
```

### SIMD Backend

The SIMD backend emits vectorized operations using Google Highway (via Pravaha's `host_simd.hpp`). A scalar expression
over `f32` becomes a vector operation over `f32x8` or wider, processing 8 values in parallel.

```cpp
auto prog = lithe::simd_backend::compile(mir);
// Now processes 8 (x,y) pairs at once
std::array<float,8> xs = { 3,4,5,6,7,8,9,10 };
std::array<float,8> ys = { 4,3,12,8,24,15,40,24 };
auto results = prog.call_batch(xs, ys);
// results = { 5, 5, 13, 10, 25, 17, 41, 26 }
```

### GPU Backend (Metal / Vulkan)

The GPU backend emits Metal Shading Language (MSL) for macOS/iOS or SPIR-V for Vulkan.

```cpp
// Metal backend (macOS first per project conventions)
auto prog = lithe::gpu_backend::compile(mir, lithe::gpu_options{
    .backend = lithe::gpu_backend_kind::metal,
    .workgroup_size = 256
});

// Dispatch on GPU
auto result = prog.dispatch_gpu(A_buffer, B_buffer);
```

### Auto Backend

`lithe::auto_backend` profiles all available backends and picks the fastest:

```cpp
auto prog = lithe::auto_backend::compile(mir);  // benchmarks CPU vs SIMD vs GPU
auto result = prog.call(x, y);
```

---

## MIR Inspection

```cpp
void show_mir(lithe::mir_module const& mir) {
    for (auto const& fn : mir.functions()) {
        std::println("fn {}:", fn.name());
        for (auto const& bb : fn.blocks()) {
            std::println("  {}:", bb.label());
            for (auto const& instr : bb.instructions()) {
                std::println("    {}", instr.to_string());
            }
        }
    }
}
```

---

## Cost Vectors

Every compilation produces a `cost_vector` — the basis for adaptive learning:

```cpp
struct cost_vector {
    double compute;    // FLOPs
    double memory;     // bytes moved
    double latency;    // wall-clock ns
    double power;      // estimated mW
};
```

These feed the `decision_engine` for future backend selection.

---

## Complete Example

```cpp
// ch09_example.cpp
#include <lithe/lithe.hpp>
#include <print>

#include "lower_vakya.hpp"
#include "pipeline.hpp"

int main() {
    std::string_view src = R"(
        input x : f32
        input y : f32
        let distance = sqrt(x*x + y*y)
    )";

    // ... parse, resolve, infer, lower (as in ch07)
    auto vakya_tree = /* lower(src) */ ...;

    // ── CPU ───────────────────────────────────────────────────────────────────
    auto cpu_prog = flux::compile<lithe::BackendKind::CPU>(vakya_tree);
    float d_cpu   = cpu_prog.program.call<float>(3.0f, 4.0f);
    std::println("CPU: distance(3,4) = {}", d_cpu);  // 5.0

    // ── SIMD ──────────────────────────────────────────────────────────────────
    auto simd_prog = flux::compile<lithe::BackendKind::SIMD>(vakya_tree);
    std::println("SIMD program compiled");

    // ── GPU ───────────────────────────────────────────────────────────────────
    auto gpu_prog = flux::compile<lithe::BackendKind::GPU>(vakya_tree);
    std::println("GPU program compiled");

    // ── Show MIR ──────────────────────────────────────────────────────────────
    auto mir = lithe::lower_to_mir(vakya_tree, lithe::semantic_context{});
    flux::show_mir(mir);
}
```

---

## What We Have

- `compile<BK>()` — template function: Vakya → compiled program for a given backend
- CPU, SIMD, GPU, auto backends via Lithe
- MIR inspection (`show_lowered()`)
- Cost vector output

---

## Next

[Chapter 10 → Execution & Introspection](ch10_execution.md) — `run()`, `show_vakya()`, `show_types()`,
`benchmark()`, `tune()`.
