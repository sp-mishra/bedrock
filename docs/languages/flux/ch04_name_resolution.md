# Chapter 4 — Name Resolution

## Theory

**Name resolution** binds every identifier in the AST to the declaration that introduces it.

```
let x = 10
let y = x + 1   -- "x" resolves to the let_decl_node above
```

### Environment (Scope Stack)

A resolution environment is a **stack of scopes**. Each scope maps name → `node_idx`.

```
scope 3 (fn body): { x → 5, acc → 6 }
scope 2 (module) : { square → 3, filter → 4 }
scope 1 (builtin): { sqrt → 0, dot → 1, matmul → 2 }
```

Lookup: search from top (innermost) to bottom. First match wins. Lookup failure = unresolved reference.

### Algorithms

| Step | Algorithm |
|------|-----------|
| Scope push/pop | Stack — O(1) |
| Symbol lookup | Hash map per scope — O(1) average |
| Forward references | Two-pass: hoist declarations, then resolve |
| Cycle detection | Topological sort over the dependency graph |

### Forward References

Flux allows using a `fn` before its definition (Go-style):

```flux
let y = square(4)
fn square(x) { x*x }
```

Algorithm: **two-pass**
1. First pass: insert all top-level `fn` and `input` declarations into the module scope.
2. Second pass: resolve all expressions.

---

## Implementation

```cpp
// include/languages/flux/resolve.hpp
#pragma once
#include "ast.hpp"
#include "ast_arena.hpp"
#include <languages/generic/semantic/symbol_table.hpp>
#include <unordered_map>
#include <string>
#include <vector>
#include <optional>
#include <expected>

namespace flux {

// ── Diagnostics ───────────────────────────────────────────────────────────────

struct resolve_error {
    std::string message;
    std::string name;
};

// ── Resolver ──────────────────────────────────────────────────────────────────

class resolver {
public:
    // symbol_table from lang::generic provides scope-stack with pluggable visibility.
    // For Flux we use the default (lexical scoping, all-visible).
    using sym_table = lang::symbol_table<std::string, node_idx>;

    explicit resolver(ast_arena& arena) : arena_(arena) {}

    // Entry point
    std::vector<resolve_error> resolve(node_idx program_root);

private:
    // Scope management
    void push_scope() { syms_.push(); }
    void pop_scope()  { syms_.pop();  }

    void define(std::string const& name, node_idx decl_idx) {
        syms_.define(name, decl_idx);
    }

    std::optional<node_idx> lookup(std::string const& name) const {
        return syms_.lookup(name);
    }

    // Visitors
    void resolve_program(program_node& prog);
    void resolve_decl(node_idx idx);
    void resolve_let_decl(let_decl_node& node, node_idx self_idx);
    void resolve_fn_decl(fn_decl_node& node);
    void resolve_input_decl(input_decl_node& node, node_idx self_idx);
    void resolve_expr(node_idx idx);
    void resolve_binary_expr(binary_expr_node& node);
    void resolve_call_expr(call_expr_node& node);
    void resolve_method_call(method_call_expr_node& node);
    void resolve_if_expr(if_expr_node& node);
    void resolve_block(block_node& node);
    void resolve_lambda(lambda_node& node);

    ast_arena&              arena_;
    sym_table               syms_;
    std::vector<resolve_error> errors_;

    // Builtin names → synthetic node indices
    void install_builtins();
    node_idx next_builtin_ = static_cast<node_idx>(0xF000'0000);
};

} // namespace flux
```

### Core Resolution Loop

```cpp
void resolver::resolve_program(program_node& prog) {
    push_scope();  // module scope

    // Pass 1: hoist all top-level declarations
    for (auto idx : prog.decls) {
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, fn_decl_node>)
                define(node.name, idx);
            else if constexpr (std::is_same_v<T, input_decl_node>)
                define(node.name, idx);
        }, arena_.at(idx));
    }

    // Pass 2: resolve bodies
    for (auto idx : prog.decls)
        resolve_decl(idx);

    if (prog.expr != null_node)
        resolve_expr(prog.expr);

    pop_scope();
}

void resolver::resolve_fn_decl(fn_decl_node& node) {
    push_scope();  // function parameter scope
    for (auto const& p : node.params)
        define(p.name, null_node);  // params are leaves, no decl node
    resolve_expr(node.body);        // body is a block
    pop_scope();
}

void resolver::resolve_expr(node_idx idx) {
    std::visit([&](auto& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, identifier_node>) {
            if (auto decl = lookup(node.name)) {
                node.decl = *decl;
            } else {
                errors_.push_back({ "Undefined: " + node.name, node.name });
            }
        }
        else if constexpr (std::is_same_v<T, binary_expr_node>)
            resolve_binary_expr(node);
        else if constexpr (std::is_same_v<T, call_expr_node>)
            resolve_call_expr(node);
        else if constexpr (std::is_same_v<T, if_expr_node>)
            resolve_if_expr(node);
        else if constexpr (std::is_same_v<T, block_node>)
            resolve_block(node);
        else if constexpr (std::is_same_v<T, lambda_node>)
            resolve_lambda(node);
        // literals: nothing to resolve
    }, arena_.at(idx));
}
```

---

## Builtin Functions

Flux provides a set of builtin functions. They are installed into the root scope before user code is
processed.

```cpp
void resolver::install_builtins() {
    // Math
    define("sqrt",      next_builtin_++);
    define("abs",       next_builtin_++);
    define("exp",       next_builtin_++);
    define("log",       next_builtin_++);
    define("sin",       next_builtin_++);
    define("cos",       next_builtin_++);
    define("tan",       next_builtin_++);
    define("pow",       next_builtin_++);
    define("floor",     next_builtin_++);
    define("ceil",      next_builtin_++);
    define("round",     next_builtin_++);
    define("clamp",     next_builtin_++);
    define("min",       next_builtin_++);
    define("max",       next_builtin_++);

    // Tensor
    define("dot",       next_builtin_++);
    define("matmul",    next_builtin_++);
    define("transpose", next_builtin_++);
    define("reshape",   next_builtin_++);

    // Functional
    define("map",       next_builtin_++);
    define("filter",    next_builtin_++);
    define("reduce",    next_builtin_++);
    define("zip",       next_builtin_++);
    define("range",     next_builtin_++);
    define("sum",       next_builtin_++);

    // Reductions
    define("mean",      next_builtin_++);
    define("variance",  next_builtin_++);
    define("norm",      next_builtin_++);
}
```

---

## Lang::Symbol Table

The `lang::symbol_table<Name, Value>` from `languages/generic/semantic/symbol_table.hpp` provides:

```
push()                          // enter new scope
pop()                           // leave scope
define(name, value)             // bind in current scope
lookup(name) → optional<Value>  // search from innermost scope outward
lookup_local(name) → optional<Value>  // current scope only (shadow detection)
```

It supports a pluggable **visibility policy** (defaulting to lexical/all-visible). Crank uses the same
table for its module system. Flux uses the default policy.

---

## Dependency Graph for Let Bindings

For `let` bindings that reference each other (disallowed in Flux top level, allowed inside blocks), we
detect cycles:

```cpp
// Cycle check: DFS over the "rhs references name" graph
// If a let binding references another let binding that has not yet been
// defined at parse time, it is a forward reference — error in Flux
// (unlike fn declarations which are hoisted).
void resolver::resolve_let_decl(let_decl_node& node, node_idx self_idx) {
    // Define AFTER resolving RHS — prevents self-reference
    resolve_expr(node.rhs);
    define(node.name, self_idx);
}
```

---

## Complete Example

```cpp
// ch04_example.cpp
#include <lithe/lithe.hpp>
#include <languages/samasa/samasa.hpp>
#include <print>

#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"

int main() {
    std::string_view src = R"(
        input x : f32
        input y : f32
        let distance = sqrt(x*x + y*y)
    )";

    auto parse_result = flux::parse(src);
    auto arena = flux::build_ast(parse_result, src);

    flux::resolver resolver(arena);
    auto errors = resolver.resolve(0);  // arena[0] = program_node

    if (errors.empty()) {
        std::println("Name resolution: OK");
    } else {
        for (auto const& e : errors)
            std::println("  Error: {}", e.message);
    }
}
```

---

## What We Have

- Two-pass resolution: hoist declarations → resolve bodies
- Scope stack via `lang::symbol_table`
- Builtin function table (`sqrt`, `matmul`, `map`, …)
- Error collection with name and message

---

## Next

[Chapter 5 → Type Inference](ch05_type_inference.md) — infer types for every expression using
Hindley-Milner Algorithm W.
