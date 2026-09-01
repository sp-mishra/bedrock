# Tutorial: Zero to Hero with Lithe — A Compiler Framework, Explained Like a Book

Welcome to the **Lithe Tutorial**. This is not a reference card. It is meant to read like the opening chapters of a
compilers book — every idea built from the ground up, in order, with Lithe as the running example. If you have ever
wanted to understand *how a modern optimizing compiler actually works* — not the 1970s textbook version, but the
data-oriented, header-only, zero-overhead C++23 version — this is for you.

**Lithe** (from *lithe*: supple, flexible) is Bedrock's header-only C++23 embedded DSL and compiler framework. You build
an expression graph in ordinary C++ — with operators, a builder, or a custom DSL — and Lithe carries it all the way to
execution: it analyzes it, optimizes it, lowers it through a two-level IR, *chooses a backend for you based on cost*,
and runs it on an interpreter, a native JIT, or a GPU. It uses **no virtual functions and no macros**; every dispatch is
a template, a concept, or `if constexpr`.

> **How to read this.** Follow it top to bottom the first time — each chapter assumes the previous one. The code is real
> Lithe API. If you arrived here from the [Taranga tutorial](taranga.md), the `portable_module` it produced is a valid
> Lithe input; you can join this story at [Chapter 7](#chapter-7-lowering-the-two-level-ir).

---

## 📑 Table of Contents

- [Prologue: What a Compiler Really Is](#prologue-what-a-compiler-really-is)
- [Chapter 1 — The Expression AST: Programs as Data](#chapter-1--the-expression-ast-programs-as-data)
- [Chapter 2 — Traversal: Doing Something With the Tree](#chapter-2--traversal-doing-something-with-the-tree)
- [Chapter 3 — Structural Identity: Hashing and DAGs](#chapter-3--structural-identity-hashing-and-dags)
- [Chapter 4 — Semantic Analysis: What Does It Mean?](#chapter-4--semantic-analysis-what-does-it-mean)
- [Chapter 5 — Optimization Passes: Making It Better](#chapter-5--optimization-passes-making-it-better)
- [Chapter 6 — Equality Saturation: Optimization Without Regret](#chapter-6--equality-saturation-optimization-without-regret)
- [Chapter 7 — Lowering: The Two-Level IR](#chapter-7--lowering-the-two-level-ir)
- [Chapter 8 — Cost Models: Choosing Between Choices](#chapter-8--cost-models-choosing-between-choices)
- [Chapter 9 — Backends: Where Code Actually Runs](#chapter-9--backends-where-code-actually-runs)
- [Chapter 10 — The Engine: Compile and Invoke](#chapter-10--the-engine-compile-and-invoke)
- [Chapter 11 — The Intelligence Layer: A Compiler That Decides](#chapter-11--the-intelligence-layer-a-compiler-that-decides)
- [Chapter 12 — IR Interchange, AOT, and the Managed Runtime](#chapter-12--ir-interchange-aot-and-the-managed-runtime)
- [Chapter 13 — The Complete Flow, End to End](#chapter-13--the-complete-flow-end-to-end)
- [Extending Lithe: A Worked Recipe](#extending-lithe-a-worked-recipe)
- [Lithe Cheat Sheet](#lithe-cheat-sheet)

---

## Prologue: What a Compiler Really Is

Strip away the jargon and a compiler is a sequence of **meaning-preserving transformations** on a data structure. You
start with a representation of a program, and you repeatedly rewrite it into an equivalent-but-better one, until the
last representation is something a machine can execute.

```
build → analyze → optimize → lower → select → execute
```

Two principles make Lithe's version modern:

1. **The program is data, not control flow.** A traditional compiler uses class hierarchies and `node->accept(visitor)`
   virtual dispatch. Lithe represents the program as *values* — flat, hashable, statically typed nodes — and transforms
   them with functions. This is the **data-oriented** turn, and it is why Lithe has no virtual functions.
2. **You never pay for what you don't use.** Every layer past the core is opt-in. Building an expression requires no
   compiler context at all. Optimization, cost modeling, GPU codegen, the managed GC — each is a header you include only
   if you need it. This is the C++ ideal of *zero-overhead abstraction* applied to a whole compiler.

Keep these two ideas in mind; every chapter is an instance of them.

> **A note on Vākya.** Lithe's expression-construction layer *is* the standalone **Vākya** library. `lithe_core.hpp`
> re-exports Vākya's `node`, `make_node`, traversals, hashing, and pattern DSL under the `lithe::` namespace and adds the
> compiler layers on top. So `lithe::add_tag` is literally `vakya::add_tag`. When this tutorial says "the AST," that is
> Vākya. Everything else — semantics, passes, codegen, backends, intelligence — is Lithe proper.

---

## Chapter 1 — The Expression AST: Programs as Data

Every compiler's heart is its **AST** — Abstract Syntax Tree — the data structure that represents the program. In Lithe
an AST node is a template:

```cpp
template <class Tag, class... Children>
struct node : interface<node<Tag, Children...>> {
    using tag_type = Tag;
    std::tuple<Children...> children;
};
```

Two things are radical here. First, the **operation is a type**, not a runtime enum: `add_tag`, `mul_tag`, `lt_tag` are
empty structs (*tags*) that name what a node does. Second, the **children's types are part of the node's type**.
`node<add_tag, X, Y>` is a distinct type from `node<mul_tag, X, Y>`. This means the *shape of the whole expression is
known at compile time* — the C++ compiler can inline traversals completely, with nothing left to dispatch at runtime.

You rarely write `node<...>` by hand. There are three ergonomic ways to build one.

**Operators.** Terminals wrapped with `as_expr` get the full operator set:

```cpp
#include "lithe/lithe_core.hpp"
using namespace lithe;

double x = 3.0, y = 5.0;
auto xr = as_expr(x);              // expr_ref<double> — holds a reference
auto expr = xr + as_expr(y) * as_expr(2.0);   // node<add_tag, ..., node<mul_tag,...>>
```

`as_expr` on an lvalue gives an `expr_ref<T>` (stores a reference, no copy); on an rvalue it gives an `expr<T>` (stores
by value). This *capture policy* means no unnecessary copies and no dangling — the type records how the value is held.

**The factory**, when you want to name the tag explicitly:

```cpp
auto e = make_node<add_tag>(lhs, rhs);   // constexpr-friendly
```

**The builder**, an LLVM-flavored fluent API for larger programs:

```cpp
using namespace lithe::builder;
auto e = IR.if_then_else(IR.lt(a, b), IR.add(a, b), IR.sub(a, b));
auto f = IR.for_loop(i, cond, update, body);
```

Lithe ships tags for arithmetic, comparison, logic, bitwise ops, control flow (`if_tag`, `while_tag`, `for_tag`,
`let_tag`, `seq_tag`, `call_tag`), and memory (`deref_tag`, `subscript_tag`, `get_element_ptr_tag`, …). That is enough
to express real programs — and in Chapter "Extending Lithe" you will add your own tag with zero edits to Lithe's
headers.

The crucial takeaway: **building an AST requires no compiler, no context, no allocation for small nodes.** It is pure
value construction. The compiler machinery only wakes up when you ask it to.

---

## Chapter 2 — Traversal: Doing Something With the Tree

An AST is inert until you walk it. Textbook compilers use the *visitor pattern* with virtual `accept` methods. Lithe
replaces that with four **constexpr traversal functions** that take a plain struct — no base class, no virtuals.

The workhorse is `evaluate`, a **bottom-up fold**: it computes results for the children first, then combines them at the
parent. You supply a struct with `on_terminal` (for leaves) and `on_node(tag, child_results...)` (for internal nodes):

```cpp
auto expr = as_expr(3.0) + as_expr(5.0) * as_expr(2.0);   // 3 + 5*2

struct eval_t {
    double on_terminal(const expr<double>& e) const { return e.value; }
    double on_node(add_tag, double l, double r) const { return l + r; }
    double on_node(mul_tag, double l, double r) const { return l * r; }
    double on_node(auto, auto...)               const { return 0.0; }  // catch-all
};
double result = evaluate(expr, eval_t{});   // 13.0
```

Because the tree shape is a compile-time type, the C++ compiler unrolls this into straight-line arithmetic — the fold
*is* `3.0 + 5.0 * 2.0` after inlining. That is the zero-overhead promise made concrete: a "tree walk" that compiles to
no walk at all.

The four traversals differ in what they hand your callback:

| Function                   | Purpose                                                      |
|----------------------------|--------------------------------------------------------------|
| `evaluate(expr, t)`        | Bottom-up: sees only child *results*                         |
| `visit(expr, v)`           | Same shape; visitor sees the original children too           |
| `transform(expr, t)`       | Rebuilds a tree; sees both original and transformed children |
| `rewrite_once(expr, rule)` | Single-pass structural rewrite                               |

For structure questions there is a `tree::` toolkit — `tree::size(e)`, `tree::depth(e)`, `tree::arity(e)`,
`tree::for_each_child`, `tree::map_children`, and compile-time folds over the *node type* itself
(`tree::all_tags_satisfy<E, Pred>()`). And for debugging, `emit::dump(expr)` prints prefix form:
`(+ 3.000000 (* 5.000000 2.000000))`.

You now have programs-as-data (Ch. 1) and a way to compute over them (Ch. 2). Everything else is more sophisticated
things to compute.

---

## Chapter 3 — Structural Identity: Hashing and DAGs

To optimize, a compiler must recognize *sameness*. If `(a + b)` appears twice in an expression, computing it twice is
waste. Detecting that requires answering "are these two subtrees structurally equal?" fast.

Lithe gives every expression a **structural hash** — a stable fingerprint of its shape and tags:

```cpp
auto e1 = as_expr(x) + as_expr(y);
auto e2 = as_expr(x) + as_expr(y);
assert(structural_hash(e1) == structural_hash(e2));
assert(structural_equal(e1, e2));
```

The hash folds in each tag's `stable_id`, so different operations hash into different buckets automatically. By default
it is **topology-only** — it hashes shape, not leaf values, so `lit(1.0)` and `lit(2.0)` collide. If a payload must
distinguish otherwise-identical trees, you opt in with a single ADL hook in *your* header, at zero cost to tags that
don't:

```cpp
std::size_t structural_payload_hash(const MyLitNode&) noexcept;   // detected by concept
```

Structural hashing unlocks the **DAG view**. A tree forces duplicated subtrees to exist twice; a *directed acyclic
graph* lets them share one node. `build_dag` interns nodes by structural hash so equal subtrees become one:

```cpp
auto dag    = graph::build_dag(expr);        // shared_expr<E>
auto shared = dag.sharing_count();           // how many nodes are used more than once
auto order  = graph::topo_order(dag.dag);    // leaf → root evaluation order
std::cout << emit::dump(dag);                // SSA-style: "%1 = +(2,3) [uses=1]"
```

This is **Common Subexpression Elimination (CSE)** in its purest form: represent the program as a DAG and shared work is
computed once. The `use_count` on each node tells you exactly how many places depend on it — the foundation for the
liveness and dead-code reasoning that comes later.

---

## Chapter 4 — Semantic Analysis: What Does It Mean?

Syntax (Ch. 1) tells you *shape*. **Semantics** tells you *meaning*: what type is this value, what domain does it live
in (integer arithmetic? floating-point? boolean logic?), does it have side effects, where should it eventually run? A
compiler that optimizes without this is guessing.

`lithe_semantic.hpp` attaches semantic facts to nodes. The central idea is `domain_type` — a bitmask classifying an
expression's domain (arithmetic, comparison, logical, memory, …) — carried in a `semantic_info` record and answered
through a `semantic_query` interface. A `backend_routing_policy` uses these facts to hint *where* an expression wants to
run before any cost model is consulted (a heavily floating-point, data-parallel subgraph leans GPU; a scalar
control-flow subgraph leans CPU).

Semantic analysis also **canonicalizes**. `lithe_semantic_passes.hpp` runs a `semantic_canonicalization_pass`: a
rewrite-rule normalization that puts semantically-equivalent forms into one preferred spelling (e.g. commutative
operands in a canonical order), then dedups by structural hash. Canonicalization is optimization's best friend — it
makes "are these the same?" fire far more often, because two programs that *mean* the same thing now *look* the same.

The theme: before you transform a program you must understand it. Semantics is where Lithe earns the right to rewrite.

---

## Chapter 5 — Optimization Passes: Making It Better

An **optimization pass** is a function `IR → IR` that preserves meaning while improving some metric — fewer operations,
cheaper operations, less dead work. Real compilers run *many* passes, often repeatedly until nothing changes (a
**fixpoint**).

Lithe bundles passes into named **presets**, familiar from any `-O` flag:

| Preset  | What runs                                                        |
|---------|------------------------------------------------------------------|
| `O0`    | Nothing — identity                                               |
| `O1`    | `simplify_add_zero`, `simplify_mul_identity` (fixpoint, 4 iters) |
| `O2`    | O1 + canonicalize + `constant_fold_arith` (6 iters)              |
| `O3`    | O2 + `strength_reduction` (8 iters)                              |
| `Debug` | O2 with full tracing                                             |

Applying one is a call:

```cpp
#include "lithe/lithe_passes.hpp"

auto x    = as_expr(a);
auto expr = x + as_expr(0.0);        // x + 0
auto opt  = lithe::preset::O2{}(expr);   // → x   (the +0 folded away)
```

The individual passes are the classics, each doing exactly one job:

- **simplify / algebraic identities** — `x + 0 → x`, `x * 1 → x`, `x * 0 → 0`.
- **constant folding** — evaluate `2 + 3` to `5` at compile time.
- **strength reduction** — replace an expensive op with a cheaper equivalent, e.g. `x * 2 → x << 1`.
- **CSE / dead-subtree elimination** — remove work that is duplicated or never used (these run in the HL-MIR optimizer
  of Ch. 7, where sharing and liveness are representable; the tree-level presets focus on transforms that make sense on
  a tree).

You can also compose a **custom pass chain** directly, wrapping any pass in `fixpoint(pass, n)` to iterate it:

```cpp
auto result = lithe::compiler::compile(expr,
    lithe::passes::fixpoint(lithe::passes::simplify_add_zero_pass{}, 4),
    lithe::passes::fixpoint(lithe::passes::constant_fold_arith_pass{}, 4));
```

Passes carry **metadata** and are registered without macros, and a `pass_local_cache` avoids recomputing on unchanged
subtrees. There is also `compiler::observability` — a `trace_observer` that emits a `pass_event` for each
transformation, so you can *watch* the optimizer work. Optimization is not magic; it is a stack of small, inspectable,
meaning-preserving rewrites.

---

## Chapter 6 — Equality Saturation: Optimization Without Regret

Traditional pass pipelines have a dirty secret: **order matters, and greedy choices are often wrong.** Applying
`strength_reduction` before `constant_fold` might block a folding that would have been better. Once a pass rewrites `A`
into `B`, `A` is gone — even if a later pass would have preferred it. This is the *phase-ordering problem*.

**Equality saturation** solves it by refusing to choose too early. Instead of destroying `A` to make `B`, it records
that `A == B` in an **e-graph** — a data structure that stores *whole equivalence classes* of expressions compactly (via
union-find over hash-consed nodes). You apply *all* your rewrite rules, growing the set of known-equivalent forms, until
either no rule adds anything new (*saturation*) or you hit a bound. Only *then* do you extract the single cheapest form
according to a cost model.

```cpp
// Conceptually: x*1 + 0  →  x
// intern → saturate(rules, limits) → extract_best<CostModel> → rebuild_expr
auto optimized = lithe::egraph::egraph_optimize(expr, rules, saturation_limits{...});
```

The flow is: **intern** the expression into the e-graph, **saturate** by applying the rule set until fixpoint or the
`saturation_limits` are reached, **extract** the best member of the root's equivalence class under a chosen `CostModel`,
and **rebuild** a normal expression. Because every equivalent form coexists until extraction, phase ordering stops
mattering — you get the best reachable form, not the first one a greedy pass stumbled into. This is *egg-style*
optimization, and it is one of the most important ideas in modern compiler research; Lithe gives you both a generic
engine and a Lithe-specific adapter.

Use presets (Ch. 5) for cheap, predictable wins; reach for the e-graph when the rewrite space is rich and you want the
genuinely optimal form.

---

## Chapter 7 — Lowering: The Two-Level IR

So far we have optimized a *tree*. But machines do not run trees; they run linear instructions over registers.
**Lowering** is the descent from the high-level tree toward the machine, and Lithe does it in two deliberate levels.

**Level 1 — HL MIR (High-Level Machine IR).** `lithe_lowering.hpp` turns the AST into a control-flow graph, and
`lithe_codegen_hl.hpp` expresses it as an `hl_mir_function`: still *structured* (it keeps loops as loops via
`structured_for_attr`), aware of memory as typed `memref`s, and rich enough to describe **affine, polyhedral** loop
nests. Why keep this level? Because the juiciest optimizations — **loop fusion, tiling, vectorization, polyhedral
interchange** (in `lithe_codegen_hl_passes.hpp`) — need to *see* loops as loops. Flatten too early and you throw away
the structure they depend on.

*(This is exactly the level Taranga hands you. Its `portable_module` is HL MIR. If you came from the Taranga tutorial,
you have been standing here the whole time.)*

**Level 2 — Physical (flat register) MIR.** When the high-level transforms are done, `coordinate_lowering_pass` flattens
HL MIR into `lithe_codegen.hpp`'s physical form: virtual registers (`vreg`), physical registers (`preg`), SSA value ids,
spill slots, addressing modes. Now the program is a classic register machine, ready for a backend. Analyses live here
too: CFG construction, def-use / use-def chains, `value_flow_analysis_result`, and edge kinds that even model
asynchronous forks and RPC boundaries for distributed execution.

```
AST (tree)
   │  lithe_lowering.hpp
   ▼
HL MIR (structured: loops, memrefs, polyhedral)  ← Taranga's portable_module lives here
   │  fusion / tiling / vectorization / polyhedral passes
   │  coordinate_lowering_pass
   ▼
Physical MIR (vreg/preg, SSA, CFG, PDG)
   │
   ▼
Backend
```

Two analyses deserve names. The **Program Dependence Graph** (`lithe_pdg.hpp`) records both data and control
dependencies as edges — it is what tells you which operations *must* stay ordered and which are free to move or
parallelize. The **polyhedral model** (`lithe_poly.hpp`) represents loop iteration spaces as affine matrices, enabling
loop fusion and interchange to be computed rather than guessed. This two-level design — optimize high, then lower — is
the same architecture MLIR made famous, expressed here without a line of virtual dispatch.

---

## Chapter 8 — Cost Models: Choosing Between Choices

By now the compiler faces *choices*: which extracted e-graph form, which backend, which schedule. To choose rationally
it needs a **cost model** — a way to estimate "how expensive is this?" before running it.

Lithe's cost model is a **vector**, not a scalar, because "cost" is multi-dimensional — latency, memory traffic, energy,
code size:

```cpp
// A cost_vector carries several named metrics (metric_id → value).
// A cost_estimator<C, Node> maps a node + cost_context to a cost_vector.
```

The framework is deliberately open. Built-in estimators cover the common cases; you can drop in a **learned estimator**
(a small model trained on real timings) by satisfying the `cost_estimator` concept — no changes to Lithe. Costs feed
into an **adaptive** layer that blends the static estimate with *observed* samples from real runs, so the model gets
more accurate the more the program runs. That closed loop — estimate, execute, measure, refine — is the difference
between a compiler that guesses once and one that learns.

The lesson: every intelligent decision downstream rests on being able to *compare* alternatives numerically. The cost
model is the ruler.

---

## Chapter 9 — Backends: Where Code Actually Runs

A **backend** turns MIR into something executable. Lithe ships several, all behind one uniform interface (again: no
virtuals — the interface is a set of *customization-point objects* and concepts):

| Backend                | What it is                                                      |
|------------------------|-----------------------------------------------------------------|
| `interpreter`          | Bytecode interpreter — the correct, portable reference vertical |
| `asmjit`               | Native JIT — emits x64 or AArch64, auto-selected for the host   |
| `debug_text`           | Human-readable pseudo-assembly, for inspection                  |
| `text_assembly`        | Offline text-assembly target                                    |
| `vulkan` / MoltenVK    | Compiles to SPIR-V and runs a GPU compute pipeline              |
| SIMD (Highway)         | Vectorized CPU kernels                                          |
| `null`                 | Does nothing — for measuring overhead and testing the machinery |
| `plugin` / out-of-proc | A C-ABI thunk table, so a backend can live in another process   |

They register in a `backend_registry` built on generational handles and a slot map with a shared-lock acquire protocol —
so backends can be added, referenced, and retired at runtime safely. The key API is `execute_with_fallback`: it tries a
capability-appropriate primary backend and falls back automatically if, say, no GPU is present. Your code says "run
this"; the registry sorts out *where*.

The design payoff: the *same* MIR runs on an interpreter during development, a JIT in production, and a GPU for the
parallel hot loop — because the boundary between "what to compute" and "where to compute it" is a clean interface, not a
fork in your source.

---

## Chapter 10 — The Engine: Compile and Invoke

The **engine** (`lithe_engine.hpp`) is the front door that ties backends, selection, and execution together.
`basic_lithe_engine` offers three compile APIs at rising levels of "let the compiler decide":

| API                                            | Use when                                      | Returns                          |
|------------------------------------------------|-----------------------------------------------|----------------------------------|
| `compile_with<B,Sig,IR>(backend, ir)`          | You know the backend at compile time          | `selected_entry<B,IR,Sig>`       |
| `compile_best<Sig,IR>(ir)`                     | Let the engine pick the best eligible backend | a variant over eligible backends |
| `compile_and_invoke_best<Sig,IR>(ir, args...)` | One-shot compile + call                       | the native result                |

```cpp
// I choose the backend:
auto r = engine.compile_with<interpreter_backend, my_sig, my_ir>(backend, ir);
if (r) { auto out = (*r)(arg1, arg2); }          // call it directly

// The engine chooses:
auto best = engine.compile_best<my_sig, my_ir>(ir);
if (best) { auto out = std::visit([](auto& e){ return e(a, b); }, *best); }
```

Errors are **variant types**, not exceptions or error codes — `engine_compile_error` is a `std::variant` of
`selection_error`, `compile_error`, `install_error`, `compile_install_error`. You `std::visit` to discover *which stage*
failed. This is `std::expected`-based error handling done properly: no false successes, and the failing phase is part of
the type. If a backend produced no usable code, the engine tells you so — it never pretends.

`compile_best` even fails at **compile time** (a `static_assert`) if *no* backend in the set could ever handle your IR
and signature. Impossible configurations are rejected before the program runs.

---

## Chapter 11 — The Intelligence Layer: A Compiler That Decides

Everything so far — semantics, costs, backends — feeds the layer that makes Lithe feel less like a tool and more like an
advisor. The **intelligence layer** (`lithe_decision_engine.hpp`) turns "which option?" into a pipeline:

```
candidates → extract features → estimate cost → rank → select → explain
```

A `decision_engine<Strategy>` is parameterized by a **selector strategy**, and you pick the one that fits your
situation:

- **cost_based** — rank purely by the cost model (Ch. 8).
- **profile_guided** — use real profile data from previous runs.
- **rule_based** — follow hand-written policies.
- **learned** — a trained model ranks the candidates.

The same engine dispatches three kinds of decision from one framework: **backend selection** (the 10-step cost-based
pipeline in `lithe_selector_strategy.hpp`), **pass-pipeline decisions** (which optimizations to run), and **schedule
policy** (how to place work, via `lithe_schedule_bridge.hpp`).

Two supporting systems make this practical. **Feature extraction** (`lithe_feature_extractor.hpp`) reduces an
expression, its MIR, and its runtime behavior to a `feature_vector`; a **feature store** caches those vectors keyed by
structural hash, so an expression the compiler has seen before skips re-extraction entirely. And every decision is
**explainable**: the engine can emit a `selection_explanation` telling you *why* it chose what it chose — which is the
difference between a compiler you trust and a black box.

This is the frontier: a compiler whose decisions are data-driven, cached, learnable, and auditable — and, thanks to the
strategy parameter, entirely swappable.

---

## Chapter 12 — IR Interchange, AOT, and the Managed Runtime

Three capabilities graduate Lithe from a library into a platform.

**IR interchange (`lithe_ir/`).** The IR can be *serialized* — written to a canonical text format or a compact binary
format — and read back. This is what lets a program compiled on one machine run on another, or lets Taranga hand its
`portable_module` across the boundary. The binary provider wraps the IR in a **security envelope**: ordered structural,
digest, and signature checks that a decoded IR must pass before it is trusted. There is a provider registry, and an
*upgrade registry* that migrates older IR schemas at decode time — so yesterday's serialized IR still loads tomorrow.

**Ahead-of-time compilation (`lithe_execution/aot.hpp`).** Serialize a *compiled artifact*, not just the IR, so all the
analysis, optimization, and codegen happens offline and startup merely memory-maps the result and runs. The round-trip
is verified: what you load is provably what you saved.

**The managed runtime (`lithe_rt.hpp`, opt-in).** For languages that need it, Lithe has a full managed-execution overlay
you pay for *only* if you include it: a **generational garbage collector** (copying young generation, mark-sweep old,
large-object space, remembered sets, weak refs, finalizers), a rooting and safepoint system for stopping threads at
GC-safe points, W^X executable memory management, and a trap model. Core users who just want to compile and run an
expression never link a byte of it. This is the zero-overhead principle at its most dramatic: a garbage collector that
costs nothing until asked for.

---

## Chapter 13 — The Complete Flow, End to End

Here is the whole journey on one page — the path any production expression takes from C++ to adaptive feedback. Every
stage is a chapter you have now read.

```
User Expression         C++ operators / make_node / IRBuilder / DSL extension   (Ch. 1)
      ↓
Vākya AST               node<Tag,...>, structural_hash, dag_view                (Ch. 1, 3)
      ↓
Pattern / Rewrite       rule registry, rewrite engine                           (Ch. 5)
      ↓
Semantic Analysis       domain_type, type inference, canonicalization, routing  (Ch. 4)
      ↓
Optimization            O0..O3 presets, fixpoint, custom bundles                (Ch. 5)
      ↓
E-Graph                 equality saturation → extract_best                      (Ch. 6)
      ↓
Cost + Features         cost_vector, feature_vector, feature store (hash-keyed) (Ch. 8, 11)
      ↓
Intelligence Layer      candidate → feature → cost → rank → select → explain    (Ch. 11)
      ↓  dispatches to →   backend selection · pass decision · schedule policy
      ↓
Lowering                AST → HL MIR (structured, polyhedral)                   (Ch. 7)
      ↓                 coordinate_lowering_pass
      ↓
Physical MIR            vreg/preg, SSA, CFG, PDG                                (Ch. 7)
      ↓
Backend                 interpreter / AsmJIT / Vulkan / plugin / AOT            (Ch. 9)
      ↓
Execution               compile_best / compile_and_invoke_best / managed rt     (Ch. 10, 12)
      ↓
Telemetry               NADI pass events, saturation stats, decision explains   (Ch. 11)
      ↓
Adaptive Feedback       observed timings blended back into the cost model       (Ch. 8)
```

Notice that the last arrow loops back to the eighth: Lithe *closes the loop*. Real execution timings flow back through
the feedback bridge into the adaptive cost model, so the next compilation of the same program is smarter than the last.
A compiler that learns from what it ran is the point the whole architecture was building toward.

---

## Extending Lithe: A Worked Recipe

The truest test of a framework is adding to it *without editing it*. Here is a `pow` (power) operator, start to finish,
touching zero Lithe headers.

**1. Declare the tag** — macro-free, from a string:

```cpp
#include "lithe/lithe_extension.hpp"
using pow_tag = lithe::dsl_extension::extension_tag<"pow">;
```

**2. Give it metadata** — arity, precedence, commutativity — by specializing a trait in *your* header:

```cpp
template <>
struct lithe::dsl_extension::extension_tag_traits<"pow"> {
    static constexpr int         precedence     = 8;
    static constexpr bool        is_commutative = false;
    static constexpr std::size_t arity          = 2;
};
```

Custom tags must use `stable_id >= kExtensionIdBase` (1000) so their structural hashes never collide with built-ins —
the extension machinery handles this for you.

**3. Build with it** — it is a first-class node now:

```cpp
auto e = lithe::make_node<pow_tag>(base, exp);
```

**4. Teach traversals about it** — add an `on_node(pow_tag, ...)` arm to your evaluator, a rewrite rule
(`pow(x, 2) → x * x`), and a cost estimate. Each is an *addition* in your code; nothing in Lithe changes.

That is the extensibility contract in one page: **new operations are new declarations, not edits.** The same pattern
extends to custom passes (via `plugin_descriptor`), custom cost estimators (via the `cost_estimator` concept), custom
backends (via the execution concepts), and custom selector strategies. Lithe is open at every layer precisely because
every layer is a concept you can satisfy from outside.

---

## Lithe Cheat Sheet

| Task                       | Code                                                                    |
|:---------------------------|:------------------------------------------------------------------------|
| **Include core**           | `#include "lithe/lithe_core.hpp"`                                       |
| **Include everything**     | `#include "lithe/lithe.hpp"` (opt into RT with `lithe_rt.hpp`)          |
| **Wrap a terminal**        | `as_expr(lvalue)` → `expr_ref<T>` · `as_expr(rvalue)` → `expr<T>`       |
| **Build via operators**    | `as_expr(x) + as_expr(y) * as_expr(2.0)`                                |
| **Build via factory**      | `make_node<add_tag>(lhs, rhs)`                                          |
| **Build via builder**      | `lithe::builder::IR.if_then_else(c, t, e)`                              |
| **Evaluate (fold)**        | `evaluate(expr, my_eval{})` with `on_terminal` / `on_node`              |
| **Tree metrics**           | `tree::size(e)` · `tree::depth(e)` · `tree::arity(e)`                   |
| **Dump**                   | `emit::dump(expr)` → `"(+ 3.0 (* 5.0 2.0))"`                            |
| **Structural identity**    | `structural_hash(e)` · `structural_equal(a, b)`                         |
| **Build a DAG (CSE)**      | `graph::build_dag(expr)` → `.sharing_count()`, `topo_order(...)`        |
| **Optimize (preset)**      | `lithe::preset::O2{}(expr)`                                             |
| **Custom pass chain**      | `compiler::compile(expr, passes::fixpoint(p, 4), ...)`                  |
| **Equality saturation**    | `egraph::egraph_optimize(expr, rules, limits)`                          |
| **Compile, known backend** | `engine.compile_with<B, Sig, IR>(backend, ir)`                          |
| **Compile, engine picks**  | `engine.compile_best<Sig, IR>(ir)`                                      |
| **Compile + invoke**       | `engine.compile_and_invoke_best<Sig, IR>(ir, args...)`                  |
| **Custom tag (no macros)** | `dsl_extension::extension_tag<"name">` + `extension_tag_traits<"name">` |

**Phase wrappers:** `surface_expr` (as-built) → `canonical_expr` → `optimized_expr` → `lowered_expr`. `unwrap_expr(e)`
strips them.

**Where to start upstream:** the [Taranga tutorial](taranga.md) shows how a WebAssembly module becomes the
`portable_module` (HL MIR) that enters this pipeline at Chapter 7.
