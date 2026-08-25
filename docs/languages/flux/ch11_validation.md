# Chapter 11 — Backend Validation

## Theory

**Backend validation** answers: do all backends produce numerically consistent results?

This is non-trivial because:
- CPU uses IEEE-754 sequential arithmetic
- SIMD may use FMA (fused multiply-add), changing rounding
- GPU uses reduced-precision paths for performance
- Different backends may reorder operations (commutativity / associativity)

### Validation Strategy

1. **Interpreter baseline** — a pure C++ tree-walking interpreter is always available as a reference.
   It is slow but produces exact IEEE-754 results.
2. **Epsilon comparison** — floating-point results must agree within `ε = 1e-5` for `f32`,
   `1e-13` for `f64`.
3. **Structural hash check** — the Vakya tree hash must be identical across paths (no accidental
   rewrite divergence).
4. **Type check** — all backends must agree on the output type.

---

## Validation Protocol

```
verify_backends()
    ├── Compile with interpreter
    ├── Compile with CPU backend
    ├── Compile with SIMD backend
    ├── Compile with GPU backend (if available)
    ├── Run all with test inputs
    ├── Compare outputs pairwise (|a - b| < ε)
    └── Report: PASS / FAIL per backend
```

---

## Implementation

```cpp
// include/languages/flux/verify.hpp
#pragma once
#include <lithe/lithe.hpp>
#include <cmath>
#include <format>
#include <print>
#include <vector>
#include <functional>

namespace flux {

struct backend_result {
    lithe::BackendKind backend;
    std::vector<double> outputs;   // all test-case outputs
    double compile_ns;
    double run_ns;
};

struct verify_report {
    bool all_pass;
    std::vector<std::pair<lithe::BackendKind, std::string>> failures;

    void print() const {
        if (all_pass) {
            std::println("verify_backends(): ALL PASS");
            return;
        }
        std::println("verify_backends(): FAILURES");
        for (auto const& [bk, msg] : failures)
            std::println("  {:8}: FAIL — {}", lithe::backend_name(bk), msg);
    }
};

class backend_verifier {
public:
    // Add test inputs as (input_name → value) maps
    using input_map = std::unordered_map<std::string, double>;

    backend_verifier& add_test(input_map inputs) {
        test_cases_.push_back(std::move(inputs));
        return *this;
    }

    // Use default test suite (random + boundary values)
    backend_verifier& use_default_tests() {
        default_tests_ = true;
        return *this;
    }

    // Tolerance for floating-point comparison
    backend_verifier& tolerance(double eps) {
        eps_ = eps;
        return *this;
    }

    verify_report verify(lithe::shared_expr const& expr,
                         std::vector<std::string> const& input_names) const;

private:
    std::vector<input_map> test_cases_;
    bool   default_tests_ = true;
    double eps_           = 1e-5;

    static std::vector<input_map> make_default_tests(
        std::vector<std::string> const& names, std::size_t n = 100);

    backend_result run_backend(
        lithe::shared_expr const& expr,
        lithe::BackendKind bk,
        std::vector<std::string> const& names,
        std::vector<input_map> const& tests) const;

    bool within_tolerance(double a, double b) const noexcept {
        return std::abs(a - b) <= eps_ * (1.0 + std::abs(a));
    }
};

} // namespace flux
```

### Core Verification Logic

```cpp
verify_report backend_verifier::verify(
    lithe::shared_expr const& expr,
    std::vector<std::string> const& input_names) const
{
    auto tests = default_tests_
        ? make_default_tests(input_names)
        : test_cases_;

    // Available backends
    std::vector<lithe::BackendKind> backends = {
        lithe::BackendKind::Interpreter,
        lithe::BackendKind::CPU,
    };
    if (lithe::simd_backend::available())
        backends.push_back(lithe::BackendKind::SIMD);
    if (lithe::gpu_backend::available())
        backends.push_back(lithe::BackendKind::GPU);

    // Run all backends
    std::vector<backend_result> results;
    for (auto bk : backends)
        results.push_back(run_backend(expr, bk, input_names, tests));

    // Compare: every backend vs interpreter baseline
    auto const& baseline = results[0];  // interpreter
    verify_report report{.all_pass = true};

    for (std::size_t b = 1; b < results.size(); ++b) {
        auto const& r = results[b];
        for (std::size_t i = 0; i < tests.size(); ++i) {
            if (!within_tolerance(baseline.outputs[i], r.outputs[i])) {
                report.all_pass = false;
                report.failures.push_back({r.backend,
                    std::format("test[{}]: interpreter={:.8f} {}={:.8f} diff={:.2e}",
                        i, baseline.outputs[i],
                        lithe::backend_name(r.backend), r.outputs[i],
                        std::abs(baseline.outputs[i] - r.outputs[i]))
                });
            }
        }
    }

    return report;
}
```

### Default Test Generation

```cpp
std::vector<backend_verifier::input_map>
backend_verifier::make_default_tests(
    std::vector<std::string> const& names, std::size_t n)
{
    std::vector<input_map> tests;
    tests.reserve(n + 4);

    // Boundary / corner cases
    for (auto const& name : names) {
        tests.push_back({{name, 0.0}});
        tests.push_back({{name, 1.0}});
        tests.push_back({{name, -1.0}});
        tests.push_back({{name, 1e6}});
    }

    // Random values in [-100, 100]
    std::mt19937_64 rng{42};
    std::uniform_real_distribution<double> dist(-100.0, 100.0);
    for (std::size_t i = 0; i < n; ++i) {
        input_map m;
        for (auto const& name : names)
            m[name] = dist(rng);
        tests.push_back(std::move(m));
    }

    return tests;
}
```

---

## Integration Into the Value API

```cpp
// In value.hpp — add verify_backends() method

value const& verify_backends() const {
    // Collect input names from symbolic variables in the expression
    auto inputs = lithe::collect_symbolic_inputs(expr_);

    backend_verifier verifier;
    verifier.use_default_tests().tolerance(1e-5);

    auto report = verifier.verify(expr_, inputs);
    report.print();
    return *this;
}
```

---

## Structural Hash Consistency Check

Beyond numeric consistency, we also verify that the Vakya tree hash is identical regardless of which
path produced the expression:

```cpp
// Flux source path:
auto flux_tree = /* lower from flux source */;
// C++ EDSL path:
auto cpp_tree  = lithe::sqrt(x*x + y*y);

// Both hashes must agree
assert(lithe::structural_hash(flux_tree) == lithe::structural_hash(cpp_tree));
std::println("Hash consistency: OK (0x{:016x})", lithe::structural_hash(flux_tree));
```

This is checked by the Flux test suite (`test_flux_invariant.cpp`).

---

## Full Validation Pipeline

```cpp
void verify_all(lithe::shared_expr const& expr,
                std::vector<std::string> const& inputs)
{
    std::println("=== Backend Validation ===");

    backend_verifier verifier;
    verifier.use_default_tests().tolerance(1e-5);
    verifier.verify(expr, inputs).print();

    std::println("\n=== Structural Hash ===");
    std::println("hash: 0x{:016x}", lithe::structural_hash(expr));

    std::println("\n=== Type Consistency ===");
    lithe::semantic_context sctx;
    auto sem = lithe::analyze_semantics(expr, sctx);
    std::println("type check: {}", sem.ok() ? "OK" : "FAIL");

    std::println("\n=== Done ===");
}
```

---

## Complete Example

```cpp
// ch11_example.cpp
#include <lithe/lithe.hpp>
#include <print>

#include "value.hpp"
#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"
#include "type_inference.hpp"
#include "lower_vakya.hpp"

int main() {
    // ── Distance example ──────────────────────────────────────────────────────
    std::string_view src1 = R"(
        input x : f32
        input y : f32
        let distance = sqrt(x*x + y*y)
    )";
    auto parse1 = flux::parse(src1);
    auto arena1 = flux::build_ast(parse1, src1);
    flux::resolver(arena1).resolve(0);
    flux::type_inferrer(arena1).infer(0);
    flux::value distance(flux::vakya_lowerer(arena1).lower_decl("distance"));

    distance
        .show_vakya()
        .show_types()
        .show_optimized()
        .verify_backends();

    // ── Matmul example ────────────────────────────────────────────────────────
    std::string_view src2 = R"(
        input A : tensor<f32>[64,64]
        input B : tensor<f32>[64,64]
        let C = matmul(A, B)
    )";
    // ...pipeline...
    // C.show_shapes().verify_backends().run(flux::gpu, A, B);

    // ── Data pipeline example ─────────────────────────────────────────────────
    std::string_view src3 = R"(
        let result =
            range(1, 1000)
                .map(fn(x) { x*x })
                .filter(fn(x) { x > 100 })
                .reduce(sum)
    )";
    // ...pipeline...
    // result.show_types().show_optimized().verify_backends()
}
```

Expected output:
```
sqrt
└── add
    ├── mul(x, x)
    └── mul(y, y)
type: f32
sqrt(norm2)    (after norm2 rule)
verify_backends(): ALL PASS
```

---

## What We Have

- `backend_verifier` — runs expr on all available backends with test inputs
- Interpreter baseline as ground truth
- Epsilon-relative floating-point comparison
- Default test suite (boundary + 100 random)
- Structural hash consistency check
- `value::verify_backends()` — one-call façade

---

## The Complete Flux Pipeline

Assembling all chapters:

```cpp
// Complete end-to-end function
flux::value flux_compile(std::string_view source) {
    // Ch 1-2: parse
    auto tokens = flux::scan(source);
    auto cst    = flux::parse(source);

    // Ch 3: build AST
    auto arena = flux::build_ast(cst, source);

    // Ch 4: resolve names
    flux::resolver(arena).resolve(0);

    // Ch 5: infer types
    flux::type_inferrer(arena).infer(0);

    // Ch 6: infer shapes
    // flux::shape_inferrer(arena, ...).infer(0);

    // Ch 7: lower to Vakya
    auto expr = flux::vakya_lowerer(arena).lower(0);

    // Ch 8: rewrites (done inside Lithe optimization passes in Ch 9)

    // Wrap in value with full introspection API
    return flux::value(std::move(expr));
}

// Usage:
auto dist = flux_compile(R"(
    input x : f32
    input y : f32
    sqrt(x*x + y*y)
)");

dist
    .show_vakya()
    .show_types()
    .show_optimized()
    .verify_backends()
    .benchmark()
    .run(flux::cpu, 3.0f, 4.0f);
```

---

## Congratulations

You have built a complete compiler:

| Step | Artifact |
|------|---------|
| Ch 1 | `flux::scan()` — character stream → tokens |
| Ch 2 | `flux::parse()` — tokens → green CST |
| Ch 3 | `flux::build_ast()` — CST → flat AST arena |
| Ch 4 | `flux::resolver` — bind identifiers to declarations |
| Ch 5 | `flux::type_inferrer` — Algorithm W type inference |
| Ch 6 | `flux::shape_inferrer` — tensor shape constraints |
| Ch 7 | `flux::vakya_lowerer` — AST → `vakya::node` tree |
| Ch 8 | `flux::make_flux_rules()` — rewrite rules + e-graph |
| Ch 9 | `flux::compile<BK>()` — Vakya → MIR → native code |
| Ch 10 | `flux::value` — fluent introspection + execution API |
| Ch 11 | `flux::backend_verifier` — cross-backend correctness |

The same Vakya tree, the same structural hash, the same optimizations, the same IR — whether you write
Flux source or C++ EDSL.

---

[← Chapter 10](ch10_execution.md) | [Back to Overview](ch00_overview.md)
