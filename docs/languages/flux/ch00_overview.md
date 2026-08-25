# Chapter 0 — Flux Architecture Overview

## What Is Flux?

Flux is a statically-typed computational language whose design satisfies four goals:

1. **Reference frontend for Vakya** — every Flux expression compiles to the same `vakya::node` tree as the equivalent C++ EDSL.
2. **Reference frontend for Lithe** — Flux feeds Lithe's optimization, e-graph, and codegen pipeline.
3. **Validation suite** — the same program runs on CPU, SIMD, and GPU; results must agree.
4. **"Build Your Own Compiler" tutorial language** — each compiler phase can be taught and tested independently.

---

## The Three Paths and Their Invariant

Flux is unique: three completely different entry points produce the same result.

```
┌──────────────────────────────────────────────────────────────────────┐
│  Path A — Runtime source (lexy)                                      │
│  std::string_view src = "sqrt(x*x + y*y)";                          │
│  lexy scanner + parser → lexy parse_tree → build_ast                 │
├──────────────────────────────────────────────────────────────────────┤
│  Path B — Compile-time source (Samasa consteval)                     │
│  constexpr auto src = "sqrt(x*x + y*y)";                            │
│  samasa::parse_static<G,src>() at consteval → green_arena CST        │
│  → build_ast (can also be consteval)                                 │
├──────────────────────────────────────────────────────────────────────┤
│  Path C — C++ EDSL (Vakya direct)                                    │
│  auto e = lithe::sqrt(x_sym*x_sym + y_sym*y_sym);                   │
│  vakya operator overloads → vakya::node (no scan, no parse)          │
└──────────────────────────────────────────────────────────────────────┘
                           ↓  ↓  ↓
                    ALL THREE CONVERGE AT:

           vakya::node — same tree, same structural_hash
                           ↓
           Same optimizations, same IR, same execution
```

**The invariant:**
```cpp
structural_hash(path_A_tree) == structural_hash(path_B_tree)
                             == structural_hash(path_C_tree)
```

This is enforced by the test suite (`test_flux_invariant.cpp`). If it ever breaks,
a frontend diverged from the EDSL semantics.

---

## Full Pipeline

```
PATH A: lexy          PATH B: Samasa consteval      PATH C: C++ EDSL
"sqrt(x*x+y*y)"       "sqrt(x*x+y*y)"              sqrt(x*x + y*y)
      ↓                       ↓                           ↓
lexy scanner            samasa::scan<>            vakya operator+,*
      ↓                       ↓                     make_node<Tag>
lexy parse_tree       green_arena CST                     │
      ↓ build_ast             ↓ build_ast                 │
      └───────────────────────┴───────────────────────────┘
                              ↓
              Flux AST  (lang::ast_arena<flux_node>)
                              ↓  Name resolution  (lang::symbol_table)
                              ↓  Type inference   (vakya::types — HM Algorithm W)
                              ↓  Shape inference  (vakya::types::shape)
                              ↓  Capability analysis
                              ↓  Effect analysis
                         VakyaLowerer
                              ↓
                       vakya::node tree
                     [ structural_hash H ]
                              ↓  Lithe semantic passes
                              ↓  Lithe rewrites + e-graph
                     Optimized vakya::node
                              ↓  Lithe codegen pipeline
                          Lithe MIR
                              ↓  Backend selection
                   CPU / SIMD / GPU (Metal/Vulkan)
```

---

## Design Principles

### Principle 1 — Grammar Must Be Boring

Flux grammar resembles Go: braces, explicit keywords, no significant whitespace.

```flux
if x > 0 {
    x
} else {
    -x
}
```

Not:

```flux
if x > 0 then x else -x    -- WRONG: too terse
```

### Principle 2 — EDSL ≈ Flux

Flux source and the C++ EDSL should look nearly identical:

```flux
sqrt(x*x + y*y)            // Flux
```

```cpp
sqrt(x*x + y*y)            // C++ EDSL
```

### Principle 3 — Compiler Features Are Library Functions

No special syntax for introspection:

```flux
distance.show_vakya()
distance.show_types()
distance.run(cpu)
```

### Principle 4 — Everything Lowers to Vakya

The parser never constructs Vakya nodes. The separation is:

```
Source → Flux AST → VakyaLowerer → vakya::node
```

This lets you test parsing and semantics without touching Lithe.

---

## Key Libraries

| Library | Role | Header | Path |
|---------|------|--------|------|
| **Lexy** | Runtime fused lex+parse → parse_tree | `lexy/dsl.hpp` | A |
| **Samasa** | PEG scanner + parser + CST (runtime + consteval) | `languages/samasa/samasa.hpp` | B |
| **Generic** | Symbol tables, IR, AST arena | `languages/generic/generic.hpp` | A, B |
| **Vakya** | Expression trees + type system | `vakya/vakya.hpp` + `vakya/vakya_types.hpp` | A, B, C |
| **Lithe** | Optimization + codegen + execution | `lithe/lithe.hpp` | A, B, C |
| **Pravaha** | Task-graph execution backend | `pravaha/pravaha.hpp` | A, B, C |

---

## Namespace Map

| Namespace | Source |
|-----------|--------|
| `flux::` | Flux language frontend (chapters 1–7) |
| `lang::samasa::` | Samasa scanner + parser |
| `lang::` | Generic infrastructure |
| `vakya::` | Expression construction |
| `vakya::types::` | Type inference, shape, effects |
| `lithe::` | Lithe compiler pipeline |

---

## Chapter Map

| Chapter | Title | Key Concepts |
|---------|-------|-------------|
| [01](ch01_lexer.md) | Lexer | DFA theory, lexy/Samasa/EDSL paths, tutorials |
| [02](ch02_grammar.md) | Grammar | PEG vs CFG, Pratt parsing, Samasa grammar API, tutorials |
| [03](ch03_ast.md) | AST | Flat arena, CST → AST, flux_node variants |
| [04](ch04_name_resolution.md) | Name Resolution | Scope stacks, symbol table, two-pass |
| [05](ch05_type_inference.md) | Type Inference | HM Algorithm W, unification, occurs check |
| [05b](ch05b_vakya_types.md) | Vakya Type System | Effects, capabilities, analysis_store, SMT prove() |
| [06](ch06_shape_inference.md) | Shape Inference | Dimension unification, broadcasting, rank-polymorphism |
| [07](ch07_vakya_lowering.md) | Vakya Lowering | AST → vakya::node, dag_builder CSE |
| [08](ch08_rewrites.md) | Rewrites & E-Graphs | Pattern DSL, equality saturation |
| [09](ch09_ir.md) | IR & Backends | Lithe MIR, CPU/SIMD/GPU codegen |
| [10](ch10_execution.md) | Execution | run(), show_vakya(), benchmark() |
| [11](ch11_validation.md) | Backend Validation | verify_backends(), cross-backend correctness |
| [12](ch_ffi.md) | FFI: C++ ↔ Flux | extern fn, tag-based FFI, tensor marshalling |
| [13](ch_debug_repl.md) | Debugging & REPL | Diagnostics, introspection, interactive REPL |
| [14](ch_stdlib.md) | Standard Library | std.math/string/io/tensor/random/algo, import resolution, effect annotations |

## Continue

[Chapter 1 → Lexer](ch01_lexer.md)
