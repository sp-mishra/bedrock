# Chapter 10 — Execution & Introspection

## Overview

Flux (and the equivalent C++ EDSL) exposes a fluent introspection API on every expression. These methods mirror each
other exactly — the same call on a Vakya expression works whether it came from parsing Flux source or from writing C++
directly.

```
expr.show_vakya()       -- print the expression tree
expr.show_types()       -- print inferred types
expr.show_shapes()      -- print tensor shapes
expr.show_effects()     -- print effect annotations
expr.show_hash()        -- print structural hash
expr.show_rewrites()    -- print which rules fired
expr.show_egraph()      -- print e-graph equivalence classes
expr.show_surface()     -- print as Flux source
expr.show_canonical()   -- print canonical (hash-normalized) form
expr.show_optimized()   -- print post-optimization tree
expr.show_lowered()     -- print MIR
expr.show_physical()    -- print physical execution plan

expr.run(cpu)           -- execute on CPU, return result
expr.run(simd)          -- execute on SIMD
expr.run(gpu)           -- execute on GPU
expr.run(auto_)         -- run on auto-selected backend

expr.benchmark()        -- measure performance
expr.tune()             -- autotune backend parameters
```

---

## Implementing the Introspection Layer

The introspection API is implemented as a thin wrapper around `lithe::shared_expr`. In the C++ EDSL,
`lithe::shared_expr` gains these methods via a CRTP mixin or free functions.

For the Flux frontend, we wrap the lowered `shared_expr` in a `flux::value` type that delegates to the same underlying
machinery.

```cpp
// include/languages/flux/value.hpp
#pragma once
#include <lithe/lithe.hpp>
#include "pipeline.hpp"

namespace flux {

class value {
public:
    explicit value(lithe::shared_expr expr) : expr_(std::move(expr)) {}

    // ── Tree display ──────────────────────────────────────────────────────────

    value const& show_vakya() const {
        lithe::emit::dump(expr_);
        return *this;
    }

    value const& show_types() const {
        lithe::semantic_context ctx;
        lithe::analyze_semantics(expr_, ctx);
        lithe::emit::dump_types(expr_, ctx);
        return *this;
    }

    value const& show_shapes() const {
        lithe::semantic_context ctx;
        lithe::analyze_semantics(expr_, ctx);
        lithe::emit::dump_shapes(expr_, ctx);
        return *this;
    }

    value const& show_effects() const {
        lithe::semantic_context ctx;
        lithe::analyze_semantics(expr_, ctx);
        lithe::emit::dump_effects(expr_, ctx);
        return *this;
    }

    value const& show_hash() const {
        std::println("hash: 0x{:016x}", lithe::structural_hash(expr_));
        return *this;
    }

    value const& show_egraph() const {
        lithe::egraph_context ctx;
        ctx.add(expr_);
        for (auto const& r : make_flux_rules()) ctx.add_rule(r);
        ctx.saturate({});
        ctx.dump();
        return *this;
    }

    value const& show_surface() const {
        // Print expression as Flux source text (round-trip)
        std::println("{}", lithe::emit::to_surface(expr_));
        return *this;
    }

    value const& show_canonical() const {
        auto canonical = lithe::canonicalize(expr_);
        lithe::emit::dump(canonical);
        return *this;
    }

    value const& show_optimized() const {
        lithe::pass_pipeline passes;
        passes.add<lithe::constant_folding_pass>();
        passes.add<lithe::algebraic_simplification_pass>();
        lithe::semantic_context ctx;
        auto opt = passes.run(expr_, ctx);
        lithe::emit::dump(opt);
        return *this;
    }

    value const& show_lowered() const {
        lithe::semantic_context ctx;
        lithe::analyze_semantics(expr_, ctx);
        auto mir = lithe::lower_to_mir(expr_, ctx);
        lithe::emit::dump_mir(mir);
        return *this;
    }

    value const& show_physical() const {
        // Show the execution plan chosen by the decision engine
        lithe::semantic_context ctx;
        lithe::analyze_semantics(expr_, ctx);
        auto plan = lithe::execution_planner::plan(expr_, ctx,
            lithe::execution_plan_options{});
        lithe::emit::dump_plan(plan);
        return *this;
    }

    value const& show_rewrites() const {
        lithe::egraph_context ctx;
        ctx.add(expr_);
        for (auto const& r : make_flux_rules()) ctx.add_rule(r);
        ctx.saturate({.max_iterations = 10, .track_rewrites = true});
        for (auto const& [name, from, to] : ctx.rewrite_log())
            std::println("  {:30} {:016x} → {:016x}", name, from, to);
        return *this;
    }

    // ── Execution ─────────────────────────────────────────────────────────────

    template<typename... Args>
    auto run(lithe::cpu_target, Args&&... args) const {
        auto prog = compile<lithe::BackendKind::CPU>(expr_);
        return prog.program.call(std::forward<Args>(args)...);
    }

    template<typename... Args>
    auto run(lithe::simd_target, Args&&... args) const {
        auto prog = compile<lithe::BackendKind::SIMD>(expr_);
        return prog.program.call(std::forward<Args>(args)...);
    }

    template<typename... Args>
    auto run(lithe::gpu_target, Args&&... args) const {
        auto prog = compile<lithe::BackendKind::GPU>(expr_);
        return prog.program.call(std::forward<Args>(args)...);
    }

    template<typename... Args>
    auto run(lithe::auto_target, Args&&... args) const {
        auto prog = compile<lithe::BackendKind::Auto>(expr_);
        return prog.program.call(std::forward<Args>(args)...);
    }

    // ── Benchmarking ──────────────────────────────────────────────────────────

    value const& benchmark() const {
        lithe::benchmark_options opts{
            .warmup_iterations   = 10,
            .measure_iterations  = 100,
            .backends            = { lithe::BackendKind::CPU,
                                     lithe::BackendKind::SIMD,
                                     lithe::BackendKind::GPU }
        };
        auto report = lithe::benchmark(expr_, opts);
        std::println("Benchmark results:");
        for (auto const& r : report.by_backend()) {
            std::println("  {:8}  {:8.3f} ns  {:6.1f} GFLOP/s",
                lithe::backend_name(r.backend),
                r.median_ns,
                r.gflops);
        }
        return *this;
    }

    // ── Autotuning ────────────────────────────────────────────────────────────

    value const& tune() const {
        lithe::autotuner tuner;
        auto best = tuner.tune(expr_, lithe::tune_options{
            .search = lithe::tune_search::bayesian,
            .budget = std::chrono::seconds{5}
        });
        std::println("Tuning complete: {} configuration(s) evaluated", best.evaluated);
        std::println("Best backend: {}", lithe::backend_name(best.backend));
        std::println("Best latency: {:.3f} ns", best.latency_ns);
        return *this;
    }

    lithe::shared_expr const& expr() const noexcept { return expr_; }

private:
    lithe::shared_expr expr_;
};

// ── Backend target tags (used as arguments to run()) ─────────────────────────

inline constexpr lithe::cpu_target  cpu{};
inline constexpr lithe::simd_target simd{};
inline constexpr lithe::gpu_target  gpu{};
inline constexpr lithe::auto_target auto_{};

} // namespace flux
```

---

## Fluent Chaining

Because `show_*` returns `value const&`, calls chain naturally:

```cpp
distance
    .show_vakya()
    .show_types()
    .show_shapes()
    .show_optimized()
    .run(flux::cpu);
```

---

## Complete Example

```cpp
// ch10_example.cpp
#include <lithe/lithe.hpp>
#include <print>

#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"
#include "type_inference.hpp"
#include "lower_vakya.hpp"
#include "value.hpp"

int main() {
    std::string_view src = R"(
        input x : f32
        input y : f32
        let distance = sqrt(x*x + y*y)
    )";

    // Frontend pipeline
    auto parse_result = flux::parse(src);
    auto arena        = flux::build_ast(parse_result, src);
    flux::resolver resolver(arena);
    resolver.resolve(0);
    flux::type_inferrer inferrer(arena);
    inferrer.infer(0);

    // Lower to Vakya, wrap in value
    flux::vakya_lowerer lowerer(arena);
    flux::value distance(lowerer.lower_decl("distance"));

    // Introspection
    distance
        .show_vakya()      // print tree
        .show_types()      // print type: f32
        .show_shapes()     // scalar
        .show_optimized()  // post-optimization tree
        .show_hash();      // structural hash

    // Execution
    float result = distance.run(flux::cpu, 3.0f, 4.0f);
    std::println("distance(3, 4) = {}", result);  // 5.0

    // Benchmark
    distance.benchmark();

    // Autotune
    distance.tune();
}
```

---

## Data Pipeline Example

```cpp
std::string_view src = R"(
    let result =
        range(1, 10000)
            .map(fn(x) { x*x })
            .filter(fn(x) { x > 100 })
            .reduce(sum)
)";

// ...frontend pipeline...
flux::value result(lowerer.lower_decl("result"));

result
    .show_types()
    .show_optimized()
    .benchmark()
    .run(flux::cpu);
```

---

## GPU Matrix Multiply Example

```cpp
std::string_view src = R"(
    input A : tensor<f32>[1024,1024]
    input B : tensor<f32>[1024,1024]
    let C = matmul(A, B)
)";

// ...frontend pipeline...
flux::value C(lowerer.lower_decl("C"));

C
    .show_shapes()      // [1024, 1024]
    .show_optimized()
    .run(flux::gpu, A_buf, B_buf);
```

---

## What We Have

- `flux::value` — fluent wrapper around `lithe::shared_expr`
- All 11 `show_*` methods
- `run(cpu/simd/gpu/auto_)` — backend dispatch
- `benchmark()` — multi-backend performance measurement
- `tune()` — Bayesian autotuning
- `flux::cpu`, `flux::simd`, `flux::gpu`, `flux::auto_` — backend target tags

---

## Next

[Chapter 11 → Backend Validation](ch11_validation.md) — `verify_backends()` ensures all backends produce identical
results.
