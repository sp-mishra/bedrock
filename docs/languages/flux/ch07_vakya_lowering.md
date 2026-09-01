# Chapter 7 — Vakya Lowering: AST → Vakya

## Theory

**Vakya lowering** translates the typed Flux AST into a `vakya::node` expression tree. This is the bridge between the
language frontend and the Lithe compiler pipeline.

### Why Not Build Vakya Directly in the Parser?

Separating parsing from Vakya construction enables:

1. **Independent testing** — parse a file, check the AST without running any optimizer
2. **Multiple frontends** — both the Flux parser and the C++ EDSL produce the same Vakya tree
3. **Semantic passes first** — name resolution and type inference annotate the AST before any Vakya node is created
4. **AST-level optimizations** — some rewrites (e.g., constant folding of literal arithmetic) are easier on the AST than
   on the Vakya DAG

### What Is a Vakya Node?

`vakya::node<Tag, Children...>` is a statically-typed expression node:

```cpp
auto e = vakya::as_expr(x) + vakya::as_expr(y) * 2;
// e : vakya::node<add_tag,
//         vakya::expr_ref<x_type>,
//         vakya::node<mul_tag, vakya::expr_ref<y_type>, vakya::expr<int>>>
```

Tags carry metadata via `vakya::emit::tag_descriptor<Tag>`:

- `symbol` — printable name (`"add"`, `"sqrt"`, …)
- `stable_id` — deterministic integer ID for caching
- `arity` — expected child count
- `is_commutative` — enables commutative pattern matching

The `structural_hash` of a Vakya node is topology-only by default. Leaf values opt in via
`structural_payload_hash`.

---

## The Lowerer

```cpp
// include/languages/flux/lower_vakya.hpp
#pragma once
#include "ast.hpp"
#include "ast_arena.hpp"
#include <lithe/lithe.hpp>  // re-exports vakya:: under lithe::

namespace flux {

// A type-erased Vakya expression wrapper.
// Because Vakya node types are structural (compile-time), we need a runtime
// wrapper for the lowerer to return.  lithe::shared_expr (DAG carrier) or
// an std::any with a fixed protocol is appropriate here.
//
// We use lithe::shared_expr — a heap-boxed, structurally-hashed node that
// satisfies the Expression concept and supports dag_builder (CSE).
using vakya_expr = lithe::shared_expr;

class vakya_lowerer {
public:
    explicit vakya_lowerer(ast_arena const& arena)
        : arena_(arena) {}

    // Lower the whole program; returns the top-level expression
    vakya_expr lower(node_idx root);

private:
    vakya_expr lower_expr(node_idx idx);
    vakya_expr lower_binary(binary_expr_node const& node);
    vakya_expr lower_call(call_expr_node const& node);
    vakya_expr lower_if(if_expr_node const& node);
    vakya_expr lower_block(block_node const& node);
    vakya_expr lower_lambda(lambda_node const& node);
    vakya_expr lower_literal(integer_literal_node const& node);
    vakya_expr lower_literal(float_literal_node const& node);
    vakya_expr lower_literal(bool_literal_node const& node);
    vakya_expr lower_identifier(identifier_node const& node);
    vakya_expr lower_array(array_expr_node const& node);

    ast_arena const&    arena_;
    lithe::dag_builder  dag_;  // CSE: identical subtrees share nodes

    // Bound variable environment: name → shared_expr
    std::unordered_map<std::string, vakya_expr> env_;
};

} // namespace flux
```

---

## Lowering Rules

### Binary Expressions → Vakya Operators

```cpp
vakya_expr vakya_lowerer::lower_binary(binary_expr_node const& node) {
    auto lhs = lower_expr(node.lhs);
    auto rhs = lower_expr(node.rhs);

    if (node.op == "+")  return dag_.build(lithe::make_node<lithe::add_tag>(lhs, rhs));
    if (node.op == "-")  return dag_.build(lithe::make_node<lithe::sub_tag>(lhs, rhs));
    if (node.op == "*")  return dag_.build(lithe::make_node<lithe::mul_tag>(lhs, rhs));
    if (node.op == "/")  return dag_.build(lithe::make_node<lithe::div_tag>(lhs, rhs));
    if (node.op == "%")  return dag_.build(lithe::make_node<lithe::mod_tag>(lhs, rhs));
    if (node.op == "^")  return dag_.build(lithe::make_node<lithe::pow_tag>(lhs, rhs));
    if (node.op == "<")  return dag_.build(lithe::make_node<lithe::lt_tag>(lhs, rhs));
    if (node.op == "<=") return dag_.build(lithe::make_node<lithe::le_tag>(lhs, rhs));
    if (node.op == ">")  return dag_.build(lithe::make_node<lithe::gt_tag>(lhs, rhs));
    if (node.op == ">=") return dag_.build(lithe::make_node<lithe::ge_tag>(lhs, rhs));
    if (node.op == "==") return dag_.build(lithe::make_node<lithe::eq_tag>(lhs, rhs));
    if (node.op == "!=") return dag_.build(lithe::make_node<lithe::ne_tag>(lhs, rhs));
    if (node.op == "and") return dag_.build(lithe::make_node<lithe::and_tag>(lhs, rhs));
    if (node.op == "or")  return dag_.build(lithe::make_node<lithe::or_tag>(lhs, rhs));

    // Should not reach here after resolve + type check
    throw std::logic_error("Unknown operator in lowering: " + node.op);
}
```

### Function Calls → Vakya Function Nodes

```cpp
vakya_expr vakya_lowerer::lower_call(call_expr_node const& node) {
    std::vector<vakya_expr> args;
    args.reserve(node.args.size());
    for (auto idx : node.args)
        args.push_back(lower_expr(idx));

    // Built-in math functions map to Lithe built-in tags
    if (node.callee == "sqrt")
        return dag_.build(lithe::make_node<lithe::sqrt_tag>(args[0]));
    if (node.callee == "abs")
        return dag_.build(lithe::make_node<lithe::abs_tag>(args[0]));
    if (node.callee == "exp")
        return dag_.build(lithe::make_node<lithe::exp_tag>(args[0]));
    if (node.callee == "log")
        return dag_.build(lithe::make_node<lithe::log_tag>(args[0]));
    if (node.callee == "sin")
        return dag_.build(lithe::make_node<lithe::sin_tag>(args[0]));
    if (node.callee == "cos")
        return dag_.build(lithe::make_node<lithe::cos_tag>(args[0]));
    if (node.callee == "pow")
        return dag_.build(lithe::make_node<lithe::pow_tag>(args[0], args[1]));

    // Tensor operations
    if (node.callee == "matmul")
        return dag_.build(lithe::make_node<lithe::matmul_tag>(args[0], args[1]));
    if (node.callee == "dot")
        return dag_.build(lithe::make_node<lithe::dot_tag>(args[0], args[1]));
    if (node.callee == "transpose")
        return dag_.build(lithe::make_node<lithe::transpose_tag>(args[0]));
    if (node.callee == "reshape")
        return dag_.build(lithe::make_node<lithe::reshape_tag>(args[0], args[1]));

    // Functional combinators
    if (node.callee == "map")
        return dag_.build(lithe::make_node<lithe::map_tag>(args[0], args[1]));
    if (node.callee == "filter")
        return dag_.build(lithe::make_node<lithe::filter_tag>(args[0], args[1]));
    if (node.callee == "reduce")
        return dag_.build(lithe::make_node<lithe::reduce_tag>(args[0], args[1]));
    if (node.callee == "zip")
        return dag_.build(lithe::make_node<lithe::zip_tag>(args[0], args[1]));
    if (node.callee == "range")
        return dag_.build(lithe::make_node<lithe::range_tag>(args[0], args[1]));

    // User-defined function call
    if (auto it = env_.find(node.callee); it != env_.end())
        return dag_.build(lithe::make_node<lithe::apply_tag>(it->second, args));

    throw std::logic_error("Undefined in lowering: " + node.callee);
}
```

### Conditionals → Vakya If Node

```cpp
vakya_expr vakya_lowerer::lower_if(if_expr_node const& node) {
    auto cond      = lower_expr(node.cond);
    auto then_body = lower_expr(node.then_body);
    auto else_body = lower_expr(node.else_body);
    return dag_.build(lithe::make_node<lithe::if_tag>(cond, then_body, else_body));
}
```

### Lambdas → Vakya Function Values

```cpp
vakya_expr vakya_lowerer::lower_lambda(lambda_node const& node) {
    // Create symbolic variable nodes for parameters
    std::vector<vakya_expr> param_vars;
    for (auto const& p : node.params) {
        auto var = lithe::make_symbolic(p);
        env_.emplace(p, var);
        param_vars.push_back(var);
    }

    auto body = lower_expr(node.body);

    // Remove param bindings from env (scope exit)
    for (auto const& p : node.params) env_.erase(p);

    return dag_.build(lithe::make_node<lithe::lambda_tag>(param_vars, body));
}
```

---

## CSE via dag_builder

`lithe::dag_builder` (backed by `vakya::graph::dag_builder`) automatically deduplicates identical subtrees. This means:

```flux
let x2 = x*x
let y2 = y*y
sqrt(x2 + y2)
```

If `x*x` appears twice (once in distance, once in some other expression), the `dag_builder` gives both the same
`shared_expr` node. No redundant computation.

```cpp
// dag_builder uses structural_hash for deduplication
auto a = dag_.build(lithe::make_node<lithe::mul_tag>(x, x));
auto b = dag_.build(lithe::make_node<lithe::mul_tag>(x, x));
assert(a.hash() == b.hash());  // same node
```

---

## Structural Hash Invariant

After lowering, the `structural_hash` of the Vakya tree produced from Flux source must equal the hash of the equivalent
C++ EDSL expression. This is the core invariant of the Flux system.

```cpp
// Flux source path:
auto flux_result = flux::lower("sqrt(x*x + y*y)", {{"x", x_sym}, {"y", y_sym}});

// C++ EDSL path:
auto cpp_result  = lithe::sqrt(x_sym*x_sym + y_sym*y_sym);

// Must be equal:
assert(lithe::structural_hash(flux_result) == lithe::structural_hash(cpp_result));
```

---

## Complete Example

```cpp
// ch07_example.cpp
#include <lithe/lithe.hpp>
#include <print>

#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"
#include "type_inference.hpp"
#include "lower_vakya.hpp"

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

    // Lower to Vakya
    flux::vakya_lowerer lowerer(arena);
    auto vakya_tree = lowerer.lower(0);

    // C++ EDSL equivalent
    auto x_sym = lithe::make_symbolic("x");
    auto y_sym = lithe::make_symbolic("y");
    auto cpp_dist = lithe::sqrt(x_sym*x_sym + y_sym*y_sym);

    // Verify structural equality
    bool same = lithe::structural_equal(vakya_tree, cpp_dist);
    std::println("Flux == C++ EDSL: {}", same);

    // Print Vakya tree
    lithe::emit::dump(vakya_tree);
}
```

Expected output:

```
Flux == C++ EDSL: true
sqrt
└── add
    ├── mul(x, x)
    └── mul(y, y)
```

---

## What We Have

- `vakya_lowerer` — walks typed Flux AST → `shared_expr` Vakya tree
- Binary ops → `add_tag`, `mul_tag`, …
- Builtin calls → `sqrt_tag`, `matmul_tag`, …
- Lambdas → symbolic variables + `lambda_tag`
- `dag_builder` for automatic CSE
- Structural hash invariant: Flux source = C++ EDSL

---

## Next

[Chapter 8 → Rewrites & E-Graphs](ch08_rewrites.md) — apply algebraic rewrites and equality saturation to the Vakya
tree.
