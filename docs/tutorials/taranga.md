# Tutorial: Zero to Hero with Taranga — A WebAssembly Compiler Front-End You Can Read

Welcome to the **Taranga Tutorial**. Taranga (तरंग, *"wave"* in Sanskrit) is Bedrock's header-only, C++23 WebAssembly compiler front-end. It reads WebAssembly in both of its official forms — the human-readable text format (`.wat`) and the compact binary format (`.wasm`) — checks that the module is well-formed, turns its stack machine into SSA, lowers it into Lithe's portable intermediate representation, and finally *runs* it.

By the end of this tutorial you will understand, from the ground up, **how a real compiler front-end is built**: what a lexer does, why validation deserves its own phase, what SSA is and why every serious compiler wants it, and how a stack-based bytecode becomes a value-based IR. You will also be able to take a `.wat` file and execute it in a handful of lines of modern C++.

> **Prerequisites.** Comfort with C++, and curiosity about compilers. No prior WebAssembly knowledge is assumed — we build it up. If you want the deeper compiler backend story afterwards, read the companion [Lithe tutorial](lithe.md), which picks up exactly where Taranga's output leaves off.

---

## 📑 Table of Contents

1. [The Big Picture: What Is a Compiler Front-End?](#1-the-big-picture-what-is-a-compiler-front-end)
2. [A Two-Minute Tour of WebAssembly](#2-a-two-minute-tour-of-webassembly)
3. [The Six-Band Pipeline](#3-the-six-band-pipeline)
4. [Step 1 — Parsing: From Text (or Bytes) to a Tree](#step-1--parsing-from-text-or-bytes-to-a-tree)
5. [Step 2 — The Module View: Making Sections Queryable](#step-2--the-module-view-making-sections-queryable)
6. [Step 3 — Validation: Earning a Capability Token](#step-3--validation-earning-a-capability-token)
7. [Step 4 — SSA: Turning a Stack Into Named Values](#step-4--ssa-turning-a-stack-into-named-values)
8. [Step 5 — Lowering: Speaking Lithe's Language](#step-5--lowering-speaking-lithes-language)
9. [Step 6 — Execution: Running the Module](#step-6--execution-running-the-module)
10. [Step 7 — Ahead-of-Time and Binary Round-Trips](#step-7--ahead-of-time-and-binary-round-trips)
11. [Design Themes You Just Absorbed](#11-design-themes-you-just-absorbed)
12. [Taranga Cheat Sheet](#12-taranga-cheat-sheet)

---

## 1. The Big Picture: What Is a Compiler Front-End?

A compiler is usually split into a **front-end** and a **back-end**, joined by an **intermediate representation** (IR):

```
 source text  ──front-end──▶  IR  ──back-end──▶  machine code / execution
```

The **front-end** answers one question: *"What does this program mean, and is it legal?"* It never emits machine code. Instead it turns messy input into a clean, checked, machine-friendly data structure. Splitting the compiler this way lets a single back-end serve many source languages, and a single front-end target many machines.

**Taranga is a front-end.** Its input is WebAssembly; its output is a *portable module* in Lithe's IR. Lithe is the back-end (interpreter, native JIT, GPU). Because the boundary is a well-defined IR, Taranga does not know or care how the module ultimately runs — and Lithe does not know it came from WebAssembly.

The recurring idea in this tutorial is that a front-end is a **pipeline of small, honest phases**, each one taking a trustworthy input and producing a trustworthy output. No phase does two jobs. When you see a bug, you know which phase to look at. This is not decoration — it is what makes compilers maintainable.

---

## 2. A Two-Minute Tour of WebAssembly

WebAssembly (Wasm) is a portable binary instruction format for a **stack machine**. Three ideas are enough to follow the rest of this tutorial.

**A module is a bundle of sections.** Types, functions, memories, globals, imports, exports — each lives in its own section. A module is a static description, like an object file.

**Functions are stack programs.** There are no registers in the source. Instructions push and pop a value stack. `i32.const 3` pushes the integer 3; `i32.add` pops two and pushes their sum. The following function returns `a + a` for its single parameter:

```wat
(func (param i32) (result i32)
  local.get 0      ;; push parameter #0
  local.get 0      ;; push it again
  i32.add)         ;; pop 2, push sum
```

**Control flow is *structured*.** Unlike raw machine code, Wasm has no arbitrary jumps. Instead it has `block`, `loop`, and `if` regions with an explicit `end`. Branches (`br`, `br_if`) may only target enclosing regions. This is a gift to a compiler writer: structured control flow is far easier to turn into SSA than a spaghetti of gotos.

That is the whole vocabulary you need. Everything else — the exact opcodes, the type rules — Taranga handles for you.

---

## 3. The Six-Band Pipeline

Taranga is organized into six *bands*. Each band is a header, has one job, and hands a typed result to the next. This is the map for the entire tutorial — steps 1 through 6 walk down it one band at a time.

```
Input (.wat / .wasm)
        │
        ▼
  Band 1: Frontend     parse()               → build_result   (dual AST)
        │              frontend / parser_wat / decoder_bin
        ▼
  Band 2: View         module_view::build()  → typed section tables
        │
        ▼
  Band 3: Validate     validate()            → validated_module  (capability token)
        │
        ▼
  Band 4: SSA Build    build_ssa()           → ssa_module
        │
        ▼
  Band 5: Lowering     lower_to_hl()         → hl_result (portable_module for Lithe)
        │              4 phases: A control · B numeric · C safety · D memory
        ▼
  Band 6: Engine       engine::create() / invoke()
```

One `#include` gives you the whole pipeline:

```cpp
#include "languages/taranga/taranga.hpp"   // umbrella header
```

You can also include a single band's header if you only need part of the pipeline — a validator tool never has to instantiate the interpreter. We return to this *pay-for-what-you-use* idea at the end.

---

## Step 1 — Parsing: From Text (or Bytes) to a Tree

The first job is to turn a stream of characters (or bytes) into a structured tree the rest of the compiler can walk. This is **parsing**, and it traditionally has two sub-steps.

**Lexing** groups characters into *tokens*. In `(func (param i32))`, the lexer produces the tokens `(`, `func`, `(`, `param`, `i32`, `)`, `)` — it stops caring about whitespace and comments. Taranga's `lexer.hpp` also handles WebAssembly's number formats and the LEB128 variable-length integers used in the binary format.

**Parsing** assembles tokens into a tree according to a *grammar* — the formal rules of what a legal module looks like. WAT is a Lisp-like S-expression language, so Taranga's `parser_wat.hpp` is a *recursive-descent* parser: one function per grammar rule, calling itself for nested rules. (The accepted grammar is written out in `docs/languages/taranga/grammar.md`.) The binary format has no S-expressions; `decoder_bin.hpp` instead walks the module section by section.

You never call the lexer or a parser directly. The `parse()` façade in `frontend.hpp` sniffs the input, picks WAT or binary automatically, and returns a single result:

```cpp
#include "languages/taranga/taranga.hpp"

auto pr = taranga::parse(R"wat(
(module
  (type (func (param i32) (result i32)))
  (func (type 0)
    local.get 0
    local.get 0
    i32.add)
  (export "double" (func 0))
)
)wat");

if (!pr.ok) {
    // pr carries structured diagnostics: TARANGA-PARSE-### / TARANGA-BIN-###
    for (auto& d : pr.diagnostics) report(d);
    return;
}
```

### The dual AST — one parse, two shapes

Here is the first design idea worth pausing on. Parsing writes each node into **two stores at once**:

| Store | Shape | Used for |
|---|---|---|
| `taranga_ir_module` | Flat, index-addressed | The primary data path — cheap to iterate, e-graph friendly |
| `taranga_ast_arena` | Variant node tree | Tooling: dumps, pretty-printing, debugging |

They are kept in lock-step by a **parity invariant**: `ir_mod.size() == ast.size()` always holds. Why two? Because the two consumers want opposite things. The compiler wants a flat array it can index into and iterate without pointer chasing; a human debugging the compiler wants a readable tree. Rather than force one representation to serve both badly, Taranga maintains both cheaply and lets each consumer use the one that fits. This is a *data-oriented* choice, and you will see the theme again in Lithe.

---

## Step 2 — The Module View: Making Sections Queryable

The raw parse result is faithful but awkward to query — "give me the type of function 3" would mean re-walking the tree. **Band 2** fixes that. `module_view::build()` reads the dual AST once and produces **typed section tables**: an array of function types, an array of functions, the exports, the memory limits, and so on.

```cpp
auto mv = taranga::module_view::build(pr.ir_mod);
// mv now exposes typed tables: mv.functions, mv.types, mv.exports, ...
```

Think of the module view as turning a document into an index. Nothing new is *computed* about the program's behavior — that is validation's job — but everything is now O(1) to look up. Every later band builds on the module view rather than re-reading the parse tree, so the expensive tree walk happens exactly once.

---

## Step 3 — Validation: Earning a Capability Token

An input can parse perfectly and still be nonsense: `i32.add` when the stack holds a float, a branch to a label that does not exist, a function whose declared result type it never produces. **Validation** is the phase that proves the module is well-typed and well-formed *before anyone tries to run it*.

WebAssembly has a precise validation specification, and `validate.hpp` implements it: stack-effect typing of every instruction, label/branch checks, index-in-range checks, and so on. But the interesting part is what validation *returns*.

```cpp
auto vr = taranga::validate(mv, pr);   // std::expected<validated_module, ...>
if (!vr.has_value()) {
    // structured TARANGA-VAL-### diagnostics
    return;
}
taranga::validated_module vm = std::move(*vr);
```

### The capability token pattern

`validated_module` is a **capability token**. It has three deliberate properties:

1. Only `validate()` can construct it — its constructor is not public to anyone else.
2. It is **move-only** — you cannot copy it around and accidentally reuse a stale one.
3. The engine *requires* one to run.

```cpp
static_assert(!std::is_copy_constructible_v<taranga::validated_module>);
```

The payoff: **it is structurally impossible to execute a module that was not validated.** You cannot forget to call `validate()`, because without its output you literally cannot construct the engine. The type system enforces the pipeline order for you. This is far stronger than a comment saying "remember to validate first" — the compiler rejects the mistake.

This trick — encode an invariant as a type that only the checking phase can mint — is worth stealing for your own designs. It turns a discipline you *hope* people follow into one the compiler *guarantees*.

---

## Step 4 — SSA: Turning a Stack Into Named Values

Now the deepest idea in the front-end. The Wasm program is a *stack* program: instructions push and pop anonymous values. That is compact, but terrible to optimize — you cannot ask "where does this value come from?" when values have no names.

**SSA — Static Single Assignment** — gives every value a unique name and requires that each name is assigned *exactly once*. Instead of "push, push, add", SSA says:

```
%1 = local.get 0
%2 = local.get 0
%3 = add %1, %2
```

Now every value has a name (`%1`, `%2`, `%3`), and every use points back to the single definition that produced it. Optimizations become easy: constant folding, dead-code elimination, common-subexpression elimination all rely on being able to follow a value to its unique source. **SSA is the representation nearly every modern optimizing compiler uses**, precisely because it makes data-flow explicit.

`build_ssa()` performs *stack-to-SSA construction* over Wasm's structured control flow:

```cpp
auto sm = taranga::build_ssa(vm, pr);   // ssa_module
```

### Why structured control flow makes this easy

The classic SSA-construction problem is deciding where to insert **φ (phi) nodes** — the pseudo-instructions that merge values arriving from different control-flow paths (e.g. the value of a variable after an `if/else`). In arbitrary goto-code this needs dominance-frontier analysis. But recall from §2 that Wasm control flow is *structured*: every merge point is the `end` of a `block`, `loop`, or `if`. Taranga rides that structure directly — block arguments (MLIR-style, the modern spelling of φ nodes) fall out of the region boundaries you already parsed. Structured control flow is not just nicer to read; it makes a whole analysis pass nearly free.

---

## Step 5 — Lowering: Speaking Lithe's Language

The SSA module is still expressed in *WebAssembly's* opcodes. The back-end (Lithe) speaks a different, more general IR: **HL MIR** (High-Level Machine IR). **Lowering** is the translation between them. `lower_to_hl()` does it in four ordered phases:

| Phase | Concern | What it emits |
|---|---|---|
| **A — Control** | Parameters, block args (φ), branches, returns, `unreachable` | `argument`, `branch`, `branch_cond`, `return`, `trap` |
| **B — Numeric** | Arithmetic, bitwise, shifts, floating-point, comparisons | `add/sub/mul/…`, `icmp`/`fcmp` with a compare predicate |
| **C — Safety** | Trapping operations made explicit | `i32.div_*` → `guard(div_by_zero)`; `unreachable` → `trap` |
| **D — Memory** | Loads and stores over linear memory | `memref_load`/`memref_store` over a byte `memref<?xi8>` |

```cpp
auto hl = taranga::lower_to_hl(sm, vm);
// hl.portable  — the frozen portable_module handed to Lithe
// hl.hl_text   — a human-readable summary (function names, imports)
```

Three details reveal how the design stays honest and extensible.

**Safety is a phase, not an afterthought.** A WebAssembly integer division traps on divide-by-zero. Rather than bury that in the divide instruction, phase C emits an *explicit* `guard(div_by_zero)`. The trap becomes a first-class thing the back-end can see, move, or optimize — the obligation is visible in the IR.

**Some opcodes lower to calls, not instructions.** Operations with no direct HL MIR counterpart — `clz`, `ctz`, `popcnt`, `rotl`, integer↔float conversions, `reinterpret` — lower to `call`s on host *prelude* functions (`runtime_prelude.hpp`), gated by an `external_calls` capability. The IR stays small and orthogonal; the long tail of exotic operations lives in a library, not in the instruction set.

**The opcode map is data, not a switch.** How does lowering know that `i32.add` has arity 2, produces one `i32`, never traps, and needs no capability — while `i32.div_s` needs a `div_by_zero` guard? It reads a **139-row table**, `k_wasm_opcode_map` in `opcode_map.hpp`. Each row declares an opcode's arity, result count and type, trap kind, required capability, whether it is a terminator, and an optional prelude function.

```
opcode        hl_op   arity  results  trap          capability     prelude
i32.add       add     2      1        —             —              —
i32.div_s     sdiv    2      1        div_by_zero   —              —
i32.popcnt    —       1      1        —             external_calls popcount_i32
```

Adding support for a new WebAssembly proposal means **adding rows**, never editing a `switch` buried in three different files. Data-driven design turns "extend the compiler" into "append to a table" — the single most maintainable decision in the whole front-end.

### The memory model

Linear memory is a flat byte array, exactly as WebAssembly defines it, with a 64 KiB page size. `wasm_memory` owns a `std::vector<uint8_t>`:

```cpp
taranga::wasm_memory mem(/*pages=*/2, /*max_pages=*/64);
auto old_pages = mem.grow(1);      // returns previous page count
```

Typed access goes through `std::memcpy`, so it is **strict-aliasing safe** — no undefined behavior from reinterpreting bytes as an `int32_t`:

```cpp
taranga::typed_store<std::int32_t>(mem, addr, offset, value);
auto v = taranga::typed_load<std::int32_t>(mem, addr, offset);
```

### Freeze and verify

After lowering builds a live `hl_mir_function` per Wasm function, two final steps produce the object handed across the boundary:

1. `freeze_module(...)` → an immutable `portable_module` — the stable, serializable form of the IR.
2. `verify_portable(portable, policy)` → a `verify_report` — a defensive re-check of the frozen IR against a policy, so a corrupt or malicious module cannot slip into the back-end.

`hl.portable` is that frozen module. It is the sole artifact Lithe consumes. Taranga's job is now done.

---

## Step 6 — Execution: Running the Module

For interpretation you do not even need to build the portable module by hand — the engine takes the validated module and drives the pipeline internally.

```cpp
auto eng = taranga::engine::create(vm, pr);     // needs the capability token
if (eng.has_value()) {
    auto r = eng->invoke("double",
        {{ taranga::wasm_value_type::i32, {.i32 = 21} }});
    // r holds i32 = 42
}
```

Note the signature again: `engine::create` demands the `validated_module`. There is no overload that accepts an unvalidated module. The token you earned in Step 3 is your entry ticket, and it is checked at compile time.

The engine in `engine.hpp` is a **reference interpreter** — a straightforward, correct executor. It is the ground truth against which faster back-ends are checked. When you want native speed or GPU execution, you hand `hl.portable` to Lithe's JIT or Vulkan backends instead; the [Lithe tutorial](lithe.md) shows how backend selection works.

---

## Step 7 — Ahead-of-Time and Binary Round-Trips

Two more capabilities round out the front-end.

**Binary in, binary understood.** Everything above works identically if you feed `parse()` a `.wasm` byte buffer instead of WAT text — `decoder_bin.hpp` handles the binary section format and `parse()` auto-detects it. The rest of the pipeline is byte-for-byte the same, because every band consumes the *module view* and the *dual AST*, not the raw text.

**Ahead-of-time compilation.** `aot.hpp` lets you serialize a compiled artifact so the expensive parse-validate-lower work happens once, offline, and startup just loads the result. This mirrors the AOT support on the Lithe side, so a Taranga module and a Lithe artifact share one on-disk story.

Because these paths are separate headers, a tool that only ever reads text never pays for the binary decoder, and an interpreter-only build never links AOT machinery.

---

## 11. Design Themes You Just Absorbed

Step back and notice the ideas that recurred — they are transferable far beyond WebAssembly:

- **A pipeline of honest phases.** Each band has one input, one output, one job. Bugs localize.
- **Capability tokens over conventions.** `validated_module` makes "validate before you run" a *type* rule, not a *social* rule.
- **Data-driven over hand-written dispatch.** The 139-row opcode table means new opcodes are new rows, not new `switch` arms.
- **Make obligations explicit.** Traps become visible `guard` ops in phase C, so the back-end can reason about them.
- **Dual representations, each fit for purpose.** Flat IR for the machine, arena tree for the human — kept in parity.
- **Pay for what you use.** Selective header inclusion means a validator, an interpreter, and a full JIT build each link only what they touch.
- **SSA as the pivot.** Turning a stack into named single-assignment values is what makes everything downstream optimizable.

You now have a mental model of a complete compiler front-end. The output — a frozen `portable_module` — is exactly the input to the [Lithe tutorial](lithe.md), which takes it the rest of the way to fast native or GPU execution.

---

## 12. Taranga Cheat Sheet

| Task | Code |
| :--- | :--- |
| **Include everything** | `#include "languages/taranga/taranga.hpp"` |
| **Parse (WAT or binary, auto)** | `auto pr = taranga::parse(src);` then check `pr.ok` |
| **Build section tables** | `auto mv = taranga::module_view::build(pr.ir_mod);` |
| **Validate → token** | `auto vr = taranga::validate(mv, pr);` (`std::expected`) |
| **Build SSA** | `auto sm = taranga::build_ssa(*vr, pr);` |
| **Lower to Lithe HL MIR** | `auto hl = taranga::lower_to_hl(sm, *vr);` → `hl.portable` |
| **Create interpreter** | `auto eng = taranga::engine::create(*vr, pr);` |
| **Invoke a function** | `eng->invoke("name", {{wasm_value_type::i32, {.i32 = 7}}});` |
| **Linear memory** | `wasm_memory mem(pages, max_pages); mem.grow(n);` |
| **Typed load/store** | `typed_load<T>(mem, addr, off)` / `typed_store<T>(...)` |
| **Verify capability token** | `static_assert(!std::is_copy_constructible_v<validated_module>);` |

**Diagnostic prefixes:** `TARANGA-PARSE-###` (WAT) · `TARANGA-BIN-###` (binary) · `TARANGA-VAL-###` (validation) · `TARANGA-SSA-###` (SSA) · `TARANGA-LOWER-###` (lowering) · `TARANGA-EXEC-###` (engine).

**Where to go next:** [Lithe tutorial](lithe.md) — take `hl.portable` from optimization through backend selection to native and GPU execution.
