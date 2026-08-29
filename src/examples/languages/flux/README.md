# Flux Language — Complete Professional Example

Single unified example demonstrating end-to-end compiler architecture with real Lithe APIs.

## Quick Start

```cpp
#include "flux.hpp"

int main() {
  flux::run_complete_example();
  return 0;
}
```

Compile:
```bash
g++ -std=c++23 -I include main.cpp -o flux_demo
./flux_demo
```

## What It Covers

| Phase | Chapter | Content |
|-------|---------|---------|
| Three-path frontend | ch00, ch01–ch03 | Lexy, Samasa, EDSL → identical vakya trees |
| Semantic analysis | ch04–ch06 | Name resolution, HM type inference, shape inference |
| Optimization | ch08 | E-graphs, rewrites, const folding |
| Lowering to MIR | ch09 | vakya → HL-MIR → Lithe MIR |
| Cost modeling | ch12 | Decision engine, CPU vs GPU scenarios |
| GPU execution | ch13 | Metal/Vulkan dispatch, MSL/SPIR-V codegen |
| Observability | ch14 | Phase events, tracing, performance metrics |
| Testing | ch11 | Three-path invariant verification |

## Architecture

```
flux::run_complete_example()
  ├─ testing::verify_invariant()           → ch00, ch01–ch03
  ├─ semantic::analyze()                   → ch04–ch06
  ├─ optimization::optimize()              → ch08
  ├─ lowering::lower_to_mir()              → ch09
  ├─ cost_model::demonstrate()             → ch12
  ├─ gpu_execution::demonstrate()          → ch13
  └─ observability::demonstrate()          → ch14
```

Each function is self-contained and shows real Lithe API usage:
- `lithe/lithe.hpp` — core expressions & operations
- `lithe/lithe_cost_model.hpp` — cost estimation
- `lithe/lithe_execution_admission.hpp` — GPU memory budgets
- `lithe/backends/lithe_codegen_metal.hpp` — Metal codegen
- `lithe/backends/lithe_codegen_vulkan_spirv_ir.hpp` — Vulkan codegen
- `vakya/vakya.hpp` — expression trees
- `vakya/vakya_types.hpp` — type system

## Output

Running shows:

1. **Three-path invariant**: All paths produce identical `structural_hash`
2. **Type inference**: HM Algorithm W unification + shape broadcast
3. **Optimization**: E-graph rewrites, algebraic simplification
4. **Cost analysis**: Small ops (CPU), large ops (GPU 100× faster)
5. **GPU codegen**: Metal MSL + Vulkan SPIR-V generation
6. **Observability**: Phase events with timings (compilation vs execution)

## Tutorial Integration

Links to all Flux chapters:
- `docs/languages/flux/ch00_overview.md` — architecture
- `docs/languages/flux/ch01_lexer.md` — ch07_vakya_lowering.md (frontend)
- `docs/languages/flux/ch08_rewrites.md` — ch11_validation.md (middle-end)
- `docs/languages/flux/ch12_cost_modeling.md` — ch14_observability.md (execution)

## Testing

Tests complement this example:
```bash
src/tests/languages/flux/
  test_flux_lexer.cpp           # 9 tests
  test_flux_ast.cpp             # 9 tests
  test_flux_types.cpp           # 10 tests
  test_flux_vakya_lowering.cpp  # 10 tests
  test_flux_cost_gpu.cpp        # 13 tests
  test_flux_end_to_end.cpp      # 7 tests
```

Run:
```bash
cmake -DBUILD_TESTS=ON -B build
cmake --build build
ctest --test-dir build -R flux
```

## Key Features Demonstrated

1. **Multi-path architecture**: Three entry points (runtime, compile-time, EDSL)
2. **Correctness by invariant**: All paths → identical trees (verified)
3. **Type safety**: Hindley-Milner with shape inference
4. **Cost-driven execution**: Intelligent backend selection
5. **GPU support**: Metal + Vulkan with automatic fallback
6. **Observability**: Phase events for profiling & debugging
7. **Modern C++**: C++23, no virtuals, constexpr-friendly

---

**See `flux.hpp` source code for detailed comments on each phase.**
