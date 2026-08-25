# Chapter 8 — Rewrites & E-Graphs

## Theory

**Algebraic rewrites** replace expression patterns with equivalent expressions. When combined with
**e-graphs** (equality saturation), the optimizer can explore an exponential number of equivalent forms
in polynomial space.

### Rewrite Rules

A rewrite rule is a (pattern, rewrite) pair:

```
x + 0  →  x           (additive identity)
x * 1  →  x           (multiplicative identity)
x * 0  →  0           (zero elimination)
a + b  →  b + a       (commutativity, when beneficial)
(x + y) * z  →  x*z + y*z   (distributivity)
sqrt(x*x + y*y) can be recognized as norm2(x,y)
```

Rewrites are **sound**: both sides are semantically equivalent. The optimizer picks whichever form has
lower cost.

### Pattern Matching

Vakya's pattern DSL uses `pattern_var` (wildcards) and `literal_pattern` (constant matchers):

```cpp
using namespace vakya::pattern;
auto x = pv<0>();   // pattern variable 0
auto y = pv<1>();   // pattern variable 1

// x + 0  →  x
auto additive_identity = rule("additive-identity",
    add(x, lit(0)),    // lhs
    x                  // rhs
);
```

Non-linear patterns (same variable on both sides) check structural equality:

```cpp
// x * x  →  square(x)
auto square_rule = rule("square",
    mul(pv<0>(), pv<0>()),   // pv<0> used twice: both children must be structurally equal
    square(pv<0>())
);
```

### E-Graphs

An **e-graph** (equality graph) stores equivalence classes of expressions. Adding a rewrite does not
destroy the original expression — it adds the rewritten form to the same equivalence class.

```
e-class 1: { x + 0,  x }
e-class 2: { x * 1,  x }
e-class 3: { 2 * x,  x + x,  x << 1 }
```

After saturation, the **extractor** picks the minimum-cost representative from each class.

Lithe integrates its e-graph via `lithe_egraph.hpp`:

```cpp
lithe::egraph_context ctx;
ctx.add(expr);
ctx.saturate(rules);
auto opt = ctx.extract(lithe::default_cost_model());
```

---

## Vakya Pattern DSL

```cpp
// include/languages/flux/rewrite_rules.hpp
#pragma once
#include <vakya/pattern.hpp>
#include <vakya/rule_registry.hpp>
#include <lithe/lithe.hpp>

namespace flux {

// Build the standard Flux rewrite rule set
inline auto make_flux_rules() {
    using namespace vakya::pattern;
    using namespace lithe;  // tag names

    auto x = pv<0>();
    auto y = pv<1>();
    auto z = pv<2>();

    auto rules = make_rule_set(
        // ── Arithmetic identities ──────────────────────────────────────────
        rule("add-zero-r",    add(x, lit(0)),      x),
        rule("add-zero-l",    add(lit(0), x),      x),
        rule("mul-one-r",     mul(x, lit(1)),      x),
        rule("mul-one-l",     mul(lit(1), x),      x),
        rule("mul-zero-r",    mul(x, lit(0)),      lit(0)),
        rule("mul-zero-l",    mul(lit(0), x),      lit(0)),
        rule("sub-self",      sub(x, x),           lit(0)),

        // ── Commutativity (used for pattern normalization) ─────────────────
        // Note: add_tag is_commutative=true — commutative retry is automatic
        // in match_pattern; no explicit rule needed for matching purposes.
        // Explicit rules useful for cost-guided extraction:
        rule("add-comm",      add(x, y),           add(y, x)),
        rule("mul-comm",      mul(x, y),           mul(y, x)),

        // ── Associativity ──────────────────────────────────────────────────
        rule("add-assoc-l",   add(add(x, y), z),   add(x, add(y, z))),
        rule("add-assoc-r",   add(x, add(y, z)),   add(add(x, y), z)),
        rule("mul-assoc-l",   mul(mul(x, y), z),   mul(x, mul(y, z))),

        // ── Distributivity ─────────────────────────────────────────────────
        rule("dist-mul-add",  mul(x, add(y, z)),   add(mul(x, y), mul(x, z))),
        rule("factor-add",    add(mul(x, y), mul(x, z)), mul(x, add(y, z))),

        // ── Strength reduction ─────────────────────────────────────────────
        rule("square",        mul(pv<0>(), pv<0>()), square(pv<0>())),

        // ── Negation ───────────────────────────────────────────────────────
        rule("neg-neg",       neg(neg(x)),          x),
        rule("sub-as-neg",    sub(x, y),            add(x, neg(y))),

        // ── Division strength ──────────────────────────────────────────────
        rule("div-one",       div_(x, lit(1)),      x),
        rule("div-self",      div_(x, x),           lit(1))   // x≠0 assumed
    );

    return rules;
}

// Register rules with Vakya rule registry for named lookup
inline auto make_flux_registry() {
    auto rules = make_flux_rules();
    return vakya::rule_registry::make_arithmetic_registry(std::move(rules));
}

} // namespace flux
```

---

## Applying Rewrites

### Single-pass rewrite

Apply rules once to an expression tree. First matching rule wins.

```cpp
auto rules   = flux::make_flux_rules();
auto result  = vakya::pattern::rewrite_once(expr, rules);
// result is std::optional<vakya_expr>
```

### Fixpoint rewrite

Apply rules repeatedly until no more rules fire.

```cpp
auto result = expr;
while (true) {
    auto next = vakya::pattern::rewrite_once(result, rules);
    if (!next) break;
    result = *next;
}
```

### E-Graph saturation (via Lithe)

```cpp
// Run equality saturation over the expression
lithe::egraph_context ctx;
auto root_id = ctx.add(expr);

// Add all Flux rules
for (auto const& r : flux::make_flux_rules())
    ctx.add_rule(r);

// Saturate (with iteration limit)
ctx.saturate(lithe::saturation_options{.max_iterations = 20});

// Extract minimum-cost representative
auto cost_model = lithe::default_cost_model();
auto optimized  = ctx.extract(root_id, cost_model);
```

---

## Cost Model

The Lithe cost model assigns costs to tags. The extractor minimizes total cost.

```
add  : 1
mul  : 2
div_ : 4
sqrt : 8
neg  : 1
square : 3  (fused squaring, cheaper than mul + mul)
```

After saturation, `x*x` and `square(x)` are in the same e-class. The extractor picks `square(x)` if
its cost (3) < `mul(x,x)` cost (2+1+1=4 roughly, depending on x's cost). The cost model makes this
decision automatically.

---

## Domain-Specific Rules

For tensor/linear algebra, Flux registers additional rules:

```cpp
// Tensor algebra rules
rule("transpose-transpose", transpose(transpose(x)),      x),
rule("matmul-assoc",
    matmul(matmul(x, y), z),
    matmul(x, matmul(y, z))),
rule("matmul-transpose",
    matmul(transpose(x), transpose(y)),
    transpose(matmul(y, x))),
// norm2 recognition
rule("norm2-sqrt",
    sqrt(add(mul(pv<0>(), pv<0>()), mul(pv<1>(), pv<1>()))),
    norm2(pv<0>(), pv<1>()))
```

---

## Showing Rewrites

The `show_rewrites()` introspection method dumps which rules fired and what they produced:

```cpp
void show_rewrites(lithe::egraph_context const& ctx) {
    std::println("Rewrites applied:");
    for (auto const& [rule_name, from_hash, to_hash] : ctx.rewrite_log()) {
        std::println("  {:30} 0x{:016x} → 0x{:016x}",
            rule_name, from_hash, to_hash);
    }
}
```

---

## Complete Example

```cpp
// ch08_example.cpp
#include <lithe/lithe.hpp>
#include <print>

#include "lower_vakya.hpp"
#include "rewrite_rules.hpp"

int main() {
    std::string_view src = R"(
        input x : f32
        input y : f32
        let distance = sqrt(x*x + y*y)
    )";

    // ... (parse, resolve, infer, lower as in ch07)
    auto vakya_tree = /* lower(src) */ ...;

    // ── Option A: single-pass rewrite ─────────────────────────────────────────
    auto rules = flux::make_flux_rules();
    auto once  = vakya::pattern::rewrite_once(vakya_tree, rules);
    std::println("Single-pass rewrite: {}", once ? "changed" : "no change");

    // ── Option B: e-graph saturation ─────────────────────────────────────────
    lithe::egraph_context ctx;
    auto root = ctx.add(vakya_tree);

    for (auto const& r : rules) ctx.add_rule(r);
    ctx.saturate({.max_iterations = 10});

    auto optimized = ctx.extract(root, lithe::default_cost_model());

    std::println("\nOriginal:");
    lithe::emit::dump(vakya_tree);
    std::println("Optimized:");
    lithe::emit::dump(optimized);
}
```

Expected output:
```
Original:
sqrt
└── add
    ├── mul(x, x)
    └── mul(y, y)
Optimized:
norm2(x, y)    -- if norm2 is cheaper in the cost model
```

---

## What We Have

- `make_flux_rules()` — 15+ algebraic rewrite rules
- Pattern DSL usage: `pv<>`, `lit<>`, `add/mul/sub/div_/neg/square`
- Single-pass `rewrite_once` and fixpoint rewrite loops
- E-graph saturation via `lithe::egraph_context`
- `show_rewrites()` introspection

---

## Next

[Chapter 9 → IR Generation & Backends](ch09_ir.md) — lower the optimized Vakya tree into Lithe MIR
and emit CPU/SIMD/GPU code.
