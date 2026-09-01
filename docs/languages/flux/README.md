# Flux — Build Your Own Compiler

A step-by-step tutorial for building a statically-typed computational language using **Lexy** (runtime parsing),
**Samasa** (consteval parsing), **Vakya** (expression trees + type system), and **Lithe** (optimization + codegen).

Flux is both a reference language and a teaching tool. Every chapter introduces one compiler phase, explains the theory,
shows the algorithm, and gives working C++ code you can copy and run.

---

## The Three Entry Paths

Flux is uniquely designed: the **same optimizer and backend** is reachable from three different starting points. All
three converge at a single `vakya::node` tree with the same `structural_hash`.

| Path  | Input                      | Library              | Use case                               |
|-------|----------------------------|----------------------|----------------------------------------|
| **A** | Runtime source string      | Lexy                 | User source files, REPL, API           |
| **B** | Compile-time source string | Samasa (`consteval`) | Embedded DSLs, CT-verified expressions |
| **C** | C++ operator expressions   | Vakya EDSL           | Programmatic tree construction         |

```
Path A  "sqrt(x*x + y*y)"  ──lexy────────────────────┐
Path B  "sqrt(x*x + y*y)"  ──samasa consteval─────────┼──► vakya::node  ──► Lithe ──► CPU/SIMD/GPU
Path C   sqrt(x*x + y*y)   ──vakya operators──────────┘
                                                ▲
                                    Same tree. Same hash. Same result.
```

---

## What You Will Build

**Flux** is a Go-style computational language. Expressions like:

```flux
input x : f32
input y : f32
let distance = sqrt(x*x + y*y)
distance.show_vakya()
distance.run(cpu)
```

compile to the same Vakya tree as the equivalent C++ EDSL:

```cpp
input<f32> x;
input<f32> y;
auto distance = sqrt(x*x + y*y);
distance.show_vakya();
distance.run(cpu);
```

Same tree. Same hash. Same optimizations. Same backend execution.

---

## Chapters

| #                             | Chapter                      | Compiler Phase                                                       |
|-------------------------------|------------------------------|----------------------------------------------------------------------|
| [00](ch00_overview.md)        | Overview & Architecture      | Full pipeline map                                                    |
| [01](ch01_lexer.md)           | Lexer — Source → Tokens      | Scanning, keyword tables, operator tries                             |
| [02](ch02_grammar.md)         | Grammar — Tokens → CST       | PEG grammar, Samasa DSL, Pratt expressions                           |
| [03](ch03_ast.md)             | AST — CST → Flux AST         | Flat arena, node kinds, CST lowering                                 |
| [04](ch04_name_resolution.md) | Name Resolution              | Scope stacks, symbol tables, forward refs                            |
| [05](ch05_type_inference.md)  | Type Inference               | Hindley-Milner, unification, Algorithm W                             |
| [06](ch06_shape_inference.md) | Shape Inference              | Tensor shapes, matmul constraints, Vakya shape algebra               |
| [07](ch07_vakya_lowering.md)  | Vakya Lowering — AST → Vakya | FluxAST → vakya::node, lowering visitors                             |
| [08](ch08_rewrites.md)        | Rewrites & E-Graphs          | Pattern DSL, rule sets, equality saturation                          |
| [09](ch09_ir.md)              | IR Generation & Backends     | Lithe MIR, CPU/SIMD/GPU codegen                                      |
| [05b](ch05b_vakya_types.md)   | Vakya Type System Deep Dive  | Effects, capabilities, analysis_store, guarded rewrites, SMT prove() |
| [10](ch10_execution.md)       | Execution & Introspection    | run(), show_vakya(), benchmark(), tune()                             |
| [11](ch11_validation.md)      | Backend Validation           | verify_backends(), cross-backend correctness                         |
| [12](ch_ffi.md)               | FFI: Calling C++ from Flux   | extern fn, tag-based FFI, tensor marshalling, inline C++             |
| [13](ch_debug_repl.md)        | Debugging & REPL             | Diagnostics, introspection, interactive REPL, watch points           |
| [14](ch_stdlib.md)            | Standard Library             | std.math, std.string, std.io, std.tensor, std.random, std.algo       |

---

## Library Stack

```
Path A: runtime source          Path B: constexpr source        Path C: C++ EDSL
  std::string_view src              constexpr auto src              auto e = sqrt(x*x+y*y)
         ↓ lexy scanner                  ↓ samasa consteval                ↓ vakya operators
  lexy::parse_tree                  green_arena<flux_kind>          vakya::node (direct)
         ↓                                  ↓
  lexy build_ast          ──────────────────┘
         ↓
  Flux AST  (lang::ast_arena<flux_node>)
         ↓  Name resolution  (lang::symbol_table)
         ↓  Type inference   (vakya::types — Algorithm W)
         ↓  Shape inference  (vakya::types::shape)
         ↓  Capability analysis
         ↓  Effect analysis
  VakyaLowerer ─────────────────────────────────────────────────────────────────┐
         ↓                                                                       │
  vakya::node tree  ◄─────────────────────────────────────────────────────────────┘
         ↓  Lithe semantic passes
         ↓  Lithe optimization (constant fold / DCE / strength reduce)
         ↓  Lithe e-graph rewrites (equality saturation)
         ↓  Lithe MIR
  CPU / SIMD / GPU backends
```

---

## Prerequisites

- C++23 compiler (Clang 17+ or GCC 13+)
- turbo_twig headers on include path
- `#include <lithe/lithe.hpp>` — Lithe + Vakya umbrella
- `#include <languages/samasa/samasa.hpp>` — Samasa parser framework
- `#include <languages/generic/generic.hpp>` — Generic language infrastructure

---

## Running Examples

Each chapter ends with a complete, self-contained `.cpp` snippet. All snippets are structured so that copying them into
a file that includes the three headers above will compile and run.
