# Crank Language — Complete Reference

> **Bedrock migration status.** Crank now belongs under
> `bedrock/include/languages/crank/`, consumes `lithe/...` headers, and uses
> Pebble's `pebble/include/languages/generic/` foundation. Its
> execution contract is local: native Metal, optional Vulkan/MoltenVK when the
> dependency is supplied, Highway SIMD, then interpreter. Pravaha remains the
> separate owner of parallel and distributed orchestration. Legacy `lithe/...`,
> AsmJit references below are historical until updated; durable artifacts use
> Lithe's opt-in Petika catalog adapter.

Crank is a compiler-friendly, embeddable DSL/language targeting production use: small scripts to large distributed
programs. Design goals: minimal syntax, zero overhead, program correctness, AI/ML ready, distributed capable.

C++23, header-only, no virtual, no macros. Namespace: `crank`.

---

## Table of Contents

This reference is large (127 sections). Entries are grouped by the seven-module compiler band; jump to a
band, then scan its sections.

- **Overview** — [Design Goals](#design-goals) · [Grammar Summary](#grammar-summary) · [Architecture](#architecture) · [Module Architecture](#module-architecture) · [Algorithms Used](#algorithms-used) · [Capability Matrix](#capability-matrix) · [Entry Point](#entry-point)
- **Generic foundation** — [Generic Language Layer](#generic-language-layer) · [Storage & the Generic IR](#storage--the-generic-ir) · [Two Module Concepts](#two-module-concepts)
- **Module 1 — Frontend** — [Lexical Grammar](#lexical-grammar) · [Automatic Semicolon Insertion (ASI)](#automatic-semicolon-insertion-asi) · [AST Tag Band (1000–1015)](#ast-tag-band-10001015) · [Parse → Vakya Flow](#parse--vakya-flow) · [Typed AST Node Types](#typed-ast-node-types-build_asthpp) · [Function Declaration Shape](#function-declaration-shape) · [Postfix Operators & Special Builtins](#postfix-operators--special-builtins) · [JSON Dump API](#json-dump-api) · [Parse Statistics](#parse-statistics) · [Source Spans](#source-spans)
- **Module 2 — Semantics** — [Host Embedding Architecture](#host-embedding-architecture) · [Standard Library (`std/`)](#standard-library-std) · [Type System](#type-system) · [Inference vs Public Boundary](#inference-vs-public-boundary) · [Effect / Capability Model](#effect--capability-model) · [Module Resolver Order](#module-resolver-order) · [Host Embedding (no macros)](#host-embedding-no-macros) · [Quick Start: Host Embedding](#quick-start-host-embedding-contexthpp) · [Semantic JSON Dumps](#semantic-json-dumps)
- **Module 3 — Verification** — [Obligation Families](#obligation-families) · [Three-Way Discharge Outcome](#three-way-discharge-outcome) · [`verify_policy` Modes](#verify_policy-modes) · [`safety_failure` Policy + `SafetyError`](#safety_failure-policy--safetyerror) · [Predicate Sublanguage](#predicate-sublanguage) · [Assumption Context Algorithm](#assumption-context-algorithm) · [Refinement Types](#refinement-types) · [Verification JSON Dumps](#verification-json-dumps)
- **Module 4 — Execution** — [Frontend Lowering Contract](#frontend-lowering-contract) · [Lowering to Lithe HL MIR](#lowering-to-lithe-hl-mir) · [Compilation Pipeline](#compilation-pipeline) · [Engine Façade (`engine.hpp`)](#engine-façade-enginehpp) · [Backend Intelligence](#backend-intelligence-plan_view-i) · [Module Façade](#module-façade-engineload-module_graph_view) · [Extern Functions](#extern-functions-hostlink-x) · [Optimization Profiles](#optimization-profiles) · [HL MIR Lowering](#hl-mir-lowering) · [Scalar Path + Interpreter](#scalar-path--interpreter) · [Automatic Execution Planning](#automatic-execution-planning) · [Pravaha Extraction + `spawn`/`await`](#pravaha-extraction--spawnawait) · [AOT Cache](#aot-cache) · [AOT Security Policy](#aot-security-policy) · [Execution Policy](#execution-policy)
- **Module 5A — Transactions** — [Transaction Runtime Lowering](#transaction-runtime-lowering) · [Transaction State Machine](#transaction-state-machine-transaction_state-31) · [Transaction Runtime Context](#transaction-runtime-context-transaction_context-32) · [Typed Error Discriminant](#typed-error-discriminant-txerrorkind-51) · [Retry Policy](#retry-policy-retry_policy-backoff_kind-103) · [Isolation Levels](#isolation-levels-8) · [Resource Capability Checker](#resource-capability-checker-63) · [Full Commit Report](#full-commit-report-crankcommitreport-151) · [Observability](#observability-transaction_event_kind-161) · [WAL Record Kinds](#wal-record-kinds-log_record_kind-142) · [Transaction Participant Concept + 2PC](#transaction-participant-concept--2pc-transactionparticipant-132134)
- **Physical MIR / backend** — [Physical-MIR Verifier Gate](#physical-mir-verifier-gate-verify_mirhpp) · [Capability Discovery](#capability-discovery-capabilityhpp) · [SIMD Legality](#simd-legality-simd_legalityhpp) · [GPU Residency, Transfers, Events](#gpu-residency-transfers-events-gpu_memoryhpp) · [Cancellation, Deadlines, Task FSM](#cancellation-deadlines-task-fsm-cancellationhpp) · [Plan Construction + Execution](#plan-construction--execution-planhpp) · [Coroutine Backend](#coroutine-backend-coroutinehpp)
- **Module 5B — Generics** — [Monomorphization Model](#monomorphization-model) · [Trait/Impl Conformance](#traitimpl-conformance) · [Bound Vocabulary](#bound-vocabulary) · [Const Generics](#const-generics) · [Monomorphizer API](#monomorphizer-api) · [Generics Feature Status](#generics-feature-status)
- **Module 6 — Extensions** — [`annotation_kind` and `annotation_strength`](#annotation_kind-and-annotation_strength) · [Descriptor Registration](#descriptor-registration) · [Namespacing Rules](#namespacing-rules) · [Resolution Policy Flow](#resolution-policy-flow) · [Annotation Resolution Algorithm](#annotation-resolution-algorithm) · [Plugin Model](#plugin-model)
- **Views / linear types** — [Conceptual Model](#conceptual-model) · [Surface Syntax](#surface-syntax) · [View Type Model](#view-type-model-43) · [Obligation Model](#obligation-model-44) · [Borrow Rule](#borrow-rule-84) · [Sutra Domain Framework Binding](#sutra-domain-framework-binding-45) · [Lowering Model](#lowering-model-46) · [Host Interop](#host-interop-47)
- **Feature charter** — [Language Features](#language-features) · [Execution Features](#execution-features) · [Transaction Features](#transaction-features) · [Tooling and Artifact Features](#tooling-and-artifact-features) · [Lean Charter](#lean-charter--feature-placement) · [Explicit Non-Goals](#explicit-non-goals) · [See Also](#see-also)

---

## Architecture

Crank is a seven-module compiler over a language-neutral foundation layer (`languages/generic/`). Strict
downward dependency: each module consumes the artifact of the one above and never reaches back up. The
frontend produces a Vakya AST; every later stage is a transform over typed, obligation-checked tree or MIR.

```
Source text
    │
    v
┌─────────────────────────────────────────────────────────────────────┐
│ Module 1 — Frontend      lexer.hpp · parser.hpp (lexy) · build_ast    │
│   lex → parse (LL(1), 13-level precedence, ASI) → Vakya AST + ir_module│
└─────────────────────────────────────────────────────────────────────┘
    │  typed Vakya tree (dual: crank_ast_arena + crank_ir_module, Stage 8b)
    v
┌─────────────────────────────────────────────────────────────────────┐
│ Module 2 — Semantics     resolve · sema_types · effects · module · host│
│   Hindley-Milner inference · name resolution (9-tier resolver)        │
│   effect/capability tracking · host embedding (context_builder)       │
└─────────────────────────────────────────────────────────────────────┘
    │  typed + resolved tree
    v
┌─────────────────────────────────────────────────────────────────────┐
│ Module 3 — Verification  verify.hpp · obligations.hpp                 │
│   safety-obligation discharge (Tarka) · refinement predicates         │
└─────────────────────────────────────────────────────────────────────┘
    │  obligation-checked tree
    v
┌─────────────────────────────────────────────────────────────────────┐
│ Module 5B — Generics     generics · monomorphize · coherence          │
│   monomorphization · trait/impl conformance · bound checking          │
└─────────────────────────────────────────────────────────────────────┘
    │  monomorphized tree
    v
┌─────────────────────────────────────────────────────────────────────┐
│ Module 4 — Execution     lower_hl → Lithe HL MIR · execute · plan     │
│   5-phase lowering (A control · B int-ops · C safety · D defer ·      │
│   E transaction) · scalar interpreter · Pravaha extraction · AOT      │
│   Module 5A — Transactions: tx.region lowering · 2PC · Medha commit   │
└─────────────────────────────────────────────────────────────────────┘
    │  HL MIR → Lithe IR (lowering_contract.hpp, Lithe-owned)
    v
   Lithe backends (interpreter / asmjit JIT / SIMD / Vulkan)

Module 6 — Extensions (annotation.hpp) is orthogonal: a typed annotation
registry consulted by modules 2–4 for @parallel/@simd/@gpu/@pure/etc.
Module 7 — Engine (engine.hpp) is the one-call façade wrapping the whole pipeline.
```

---

## Design Goals

| Property    | Design decision                                                         |
|-------------|-------------------------------------------------------------------------|
| Safe        | Effect/capability tracking, tx isolation, proof obligations             |
| Performant  | Zero-overhead abstractions, pay-for-use, Pravaha parallelism            |
| Correct     | Type inference, effect checking, verification, generic conformance      |
| AI/ML ready | Annotation system, execution hints, AOT cache, plan adapters            |
| Distributed | tx isolation levels, distribution policy (default: `none`)              |
| Embeddable  | Single `context` entry point, host function/type/container registration |

---

## Grammar Summary

Grammar lives in `docs/languages/crank/grammar.md`. Key constructs:

Status column reflects what the parser and semantic pipeline accept today.

| Construct                                                 | §ref      | Status                                                                                                                                                                                          |
|-----------------------------------------------------------|-----------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Functions, closures                                       | §3        | Implemented                                                                                                                                                                                     |
| `for`/`parallel.map`/`parallel.reduce`                    | §6        | Implemented — `for i in expr` (lean charter); `parallel.*` are prelude functions, not grammar builtins                                                                                          |
| `spawn`/`await`                                           | §7        | Implemented                                                                                                                                                                                     |
| Transactions: `transaction(...)`/`resource[key]`          | §6.2, §7  | Implemented                                                                                                                                                                                     |
| Multi-resource transactions: `transaction(coordinator=…)` | §7c       | Implemented — parses; single-resource enforced at sema (`CRANK-TX-002`)                                                                                                                         |
| `abort(error)` — explicit tx abort inside body            | §2.3      | Implemented — `tx_abort_stmt` in parser; `tx_abort_node`/`tx_abort_tag` in AST                                                                                                                  |
| `yield expr` — tx body value                              | §2.2      | Implemented — `tx_yield_stmt` in parser; `tx_yield_node`/`tx_yield_tag` in AST                                                                                                                  |
| Generics: `[T: Trait]`                                    | §8        | Implemented                                                                                                                                                                                     |
| Associated types: `trait T { type Item; }` / `C.Item`     | §8, §v2.1 | Implemented (sema) — `type Item`, `Self.Item`/`C.Item` resolved; `CRANK-GEN-006` gate lifted (`assoc_types.hpp`)                                                                                |
| Const-generic arithmetic: `[N+1]`, `[M*K]`                | §8        | Implemented                                                                                                                                                                                     |
| Controlled specialization: `impl Trait for Concrete`      | §8        | Implemented                                                                                                                                                                                     |
| Annotations: `@name(args)`                                | §3.6      | Implemented                                                                                                                                                                                     |
| Layout/device bounds: `Layout[RowMajor]`, `Device[Gpu]`   | §8        | Implemented                                                                                                                                                                                     |
| `defer`                                                   | §6        | Implemented                                                                                                                                                                                     |
| Safety/overflow attributes                                | §10.1     | Implemented                                                                                                                                                                                     |
| Reflection: `@reflect(fields, traits, capabilities)`      | §16       | Implemented — parses via generic `@attribute`; descriptors program-built                                                                                                                        |
| `task_scope`/`deadline`, `savepoint()`/`rollback_to(sp)`  | —         | Not available in grammar — host C++ APIs only (see Roadmap)                                                                                                                                     |
| Generic modules: `module M[T: Bound] { … }`               | §v2.3     | Implemented — grammar (`module_decl` reuses `generic_params`) + `module_decl_node` + `generic_module_descriptor`/`instantiate_module` (`module_generics.hpp`); `pub` marker for explicit export |
| `host module math { … }`                                  | §21       | Not available — generated from C++ descriptors via `finalized_context`                                                                                                                          |
| `@host.link("symbol")` extern fn                          | §21       | Implemented — semantic verification via `verify_extern_fn_decl`; CRANK-EXT-010/011/012                                                                                                          |

Built-in unqualified annotations (closed set):
`@parallel @simd @gpu @pure @reads @writes @io @net @host`

All extension annotations require a namespace prefix (e.g. `@company.my_hint`).

---

## Module Architecture

| Module            | Headers                                                                                             | Responsibilities                                                                                                                                                                                                                                                                                                 |
|-------------------|-----------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1 — Frontend      | `lexer.hpp`, `parser.hpp`, `build_ast.hpp`, `frontend.hpp`                                          | Lex, parse (lexy), build Vakya AST                                                                                                                                                                                                                                                                               |
| 2 — Semantics     | `host.hpp`, `module.hpp`, `resolve.hpp`, `sema_types.hpp`, `effects.hpp`, `context.hpp`             | Type inference (HM), name resolution, effect/capability tracking, module system, host embedding; stable entity IDs, typed invocation thunks, `context_builder`/`finalized_context`, resource/backend descriptors, error adapters, cancellation injection, observability options, `host_value`/`owned_host_value` |
| 3 — Verification  | `verify.hpp`, `obligations.hpp`                                                                     | Safety obligations, proof discharge (Tarka), verification policy                                                                                                                                                                                                                                                 |
| 4 — Execution     | `lower_hl.hpp`, `execute.hpp`, `exec_hint.hpp`, `parallel.hpp`, `aot.hpp`, `dump.hpp`, `engine.hpp` | HL MIR lowering, scalar interpretation, parallel planning, Pravaha extraction, AOT cache, compiler-phase parallelism (`parse_modules_parallel`, `batch_load_modules`)                                                                                                                                            |
| 5A — Transactions | `transaction.hpp`, `host.hpp` (extended)                                                            | Tx policy, compile-time checks, Medha integration                                                                                                                                                                                                                                                                |
| 5B — Generics     | `generics.hpp`, `monomorphize.hpp`                                                                  | Monomorphization, trait/impl, bound checking                                                                                                                                                                                                                                                                     |
| 6 — Extensions    | `annotation.hpp`                                                                                    | Typed annotation registry, plugin model                                                                                                                                                                                                                                                                          |
| 7 — Engine        | `engine.hpp`                                                                                        | One-call façade: `eval`/`run`/`compile`/`load`; `engine_options`, `program`, `value`, `module_handle`, `module_graph_view`, `run_report`, `plan_view`; `verify_extern_fn_decl`                                                                                                                                   |
| Lithe contract    | `lithe/lithe_ir/frontend/lowering_contract.hpp`                                                      | Normative Crank→IR type map + capability map (Lithe-owned)                                                                                                                                                                                                                                                       |

---

## Generic Language Layer

Crank builds on a language-neutral foundation layer (`languages/generic/`) that provides reusable infrastructure for all
DSL frontends. This section documents how Crank uses that layer and where to find generic abstractions.

### Generic Layer Location & Purpose

**Headers:** `pebble/include/languages/generic/`  
**Documentation:** `docs/languages/generic.md`

The generic layer provides:

- **Core primitives** (`core/`): Stable entity IDs (deterministic 128-bit identifiers), FNV-1a fingerprinting,
  diagnostics, reflection metadata.
- **Host descriptors** (`host/`): Function/type/field/resource descriptor base classes. Crank extends these for
  source-location tracking and effect metadata.
- **Module system** (`module/`): Language-agnostic 9-tier module resolver, dependency graph with cycle detection, module
  versioning.
- **Semantic infrastructure** (`semantic/`): Scope-stack symbol tables with pluggable visibility policies (Crank:
  uppercase→exported); proof obligation registries.
- **Flat AST storage** (`ast/`): Index-based `ast_arena<>` (eliminates pointer invalidation on growth; safe for parallel
  allocation).
- **Lexer utilities** (`lexer/`): Numeric literal helpers (digit separators).

### How Crank Uses Generic

#### 1. AST Arena (Index-Based Nodes)

Crank's AST is stored in a flat `lang::ast_arena<crank_ast_node>` rather than a pointer tree.

```cpp
// include/languages/crank/build_ast.hpp
using crank_node_id = lang::ast_node_id;
using crank_ast_arena = lang::ast_arena<crank_ast_node>;
inline constexpr crank_node_id k_null_node = lang::k_null_node;
```

**Benefit:** No pointer invalidation when adding nodes; safe to parallelize frontend phases.

#### 2. Module Resolution & Dependency Graphs

Crank's module resolver is a lightweight wrapper around `lang::module_resolver` with `.crank` file-extension preset.

```cpp
// Crank configures the resolver:
lang::resolver_config cfg;
cfg.source_extension = ".crank";
lang::module_resolver resolver{cfg};
resolver.add_project_path("/workspace/src");
```

**Module tiers (9-level lookup):**

1. Native (registered C++ modules)
2. Embedded artifacts (pre-compiled binaries)
3. Embedded source (in-memory source)
4. In-memory (runtime-injected)
5. Project paths (source on disk)
6. App paths (application-level)
7. Cache paths (compiled cache)
8. System paths (if enabled)
9. Package registry (if enabled)

Crank uses tiers 1–7 by default. Tier 1 is Crank's primary extension: host modules (written in C++, registered via
descriptors).

**Dependency graphs** detect cycles via DFS coloring:

```cpp
auto cycle = dep_graph.cycle_nodes();  // Returns modules forming a cycle (if any)
```

#### 3. Module Descriptors & Version Tracking

Each module has a stable identity and version:

```cpp
// include/languages/crank/module.hpp extends lang::module_descriptor
struct module_descriptor : lang::module_descriptor {
    // Crank-specific fields:
    crank_isolation_level isolation = crank_isolation_level::none;
    // ... other crank fields
};
```

**Stable IDs:** Every crank module gets a deterministic 128-bit ID (from `lang::detail::make_id`) derived from its
qualified name. Identical inputs always produce the same ID, enabling reproducible artifact caching.

**Content hash:** Module source is FNV-1a hashed (`lang::module_hash`), enabling change detection.

#### 4. Symbol Tables with Visibility Policies

Crank's symbol table uses an `uppercase_export_policy`: if a symbol's first character is uppercase (or explicitly marked
`pub`), it is exported; otherwise module-local.

```cpp
// include/languages/crank/host.hpp
using crank_symbol_table = lang::symbol_table<lang::uppercase_export_policy>;
```

**Symbol metadata:**

```cpp
struct symbol_entry {
    std::string name;
    lang::sym_kind kind;  // function, type, variable, etc.
    lang::sym_visibility visibility;  // module_local or exported
    lang::sym_mutability mutability;  // immutable, mutable_, constant
    // Crank adds: source_span, type_var_id, ...
};
```

#### 5. Effect & Capability Masks

Crank extends the generic effect/capability system from Vakya. Builtin effects (FileSystem, Memory, IO, Network,
Exception) occupy stable_id 1–5. Crank adds its own effects in the extension band (stable_id ≥1000):

```cpp
// include/languages/crank/effects.hpp
auto effects = lang::make_builtin_effect_registry();  // from vakya

// Crank adds:
// @host (1000) — function calls out to C++ (side effects)
// @gpu  (1001) — GPU-executable
// @parallel_safe (1002) — safe to parallelize (no data races)
```

Effect masks are uint64_t bitmasks associated with functions and modules. Effect checking ensures that functions
declared `@pure` do not call functions with side effects.

#### 6. Stable Entity IDs for Caching

Every function, type, and module in Crank gets a stable ID from `lang::detail::make_id(qualified_name, kind)`:

```cpp
constexpr auto id = lang::detail::make_id("math.dot", lang::kKindFunction);
// -> stable_entity_id with deterministic 128-bit hash
```

Paired with a descriptor fingerprint (`lang::descriptor_fingerprint`), this enables:

- Reproducible AOT cache keys
- Incremental compilation (detect descriptor changes)
- Cross-version artifact compatibility (same ID = same semantics)

#### 7. Descriptor Fingerprinting

Function/type/field descriptors include a fingerprint for change detection:

```cpp
// Fingerprints combine via XOR + rotate (no symmetry collapse):
auto fp = lang::detail::fp_combine(fp_namespace, lang::detail::fp_with_scalar(fp_name, arity));
```

When a descriptor's signature changes (arity, effect mask, etc.), its fingerprint changes, invalidating cached
artifacts.

#### 8. Host Embedding (C++ Registration)

Crank integrates C++ functions/types via descriptor registration. Generic provides the base types; Crank extends them:

```cpp
// Generic base:
lang::function_descriptor_base fd;
fd.name = "math.dot";
fd.arity = 2;
fd.typed_thunk = &typed_thunk_for_dot;

// Crank extends with source_location, effects, etc.
crank::function_descriptor cfd;
cfd = fd;  // inherit generic fields
cfd.effect_mask = lang::kEffectExtBase;  // host calls -> effect
cfd.source_location = {...};
```

**Typed thunks:** Generic provides `make_function_descriptor<Name, Fn>()` to auto-generate type-safe thunks from C++
function pointers. Crank uses these for host functions.

### Symbol Flow at Import Boundary

When a module imports a symbol from another module, the exported symbol goes through an adapter:

```cpp
// include/languages/crank/context.hpp
[[nodiscard]] inline lang::symbol_entry
to_lang_symbol(const crank::symbol_entry& e) {
    lang::symbol_entry o;
    o.name = e.name;
    o.kind = static_cast<lang::sym_kind>(static_cast<std::uint8_t>(e.kind));
    o.visibility = e.visibility;
    return o;
}
```

This allows the import graph (`lang::import_graph`) to operate on generic symbol tables while Crank preserves its own
metadata (source spans, HM type-var IDs).

### Generic -> Crank Conversions

**Module conversion** (for import graph):

```cpp
lang::module_descriptor to_lang_module(const crank::module_descriptor& d) {
    lang::module_descriptor o;
    o.kind = static_cast<lang::module_kind>(static_cast<std::uint8_t>(d.kind));
    o.content_hash = lang::module_hash{d.content_hash.value};
    o.capabilities = lang::module_capabilities{d.capabilities.effect_mask, d.capabilities.capability_mask};
    return o;
}
```

**Version conversion**:

```cpp
lang::version_triple to_lang_version(const crank::version_triple& v) {
    return {v.major, v.minor, v.patch};  // field-identical
}
```

### Diagram: Generic Layer in Crank's Pipeline

```
parse
  |
  v
build_ast (crank_ast_arena — lang::ast_arena<crank_ast_node>)
  |
  v
resolve (crank_symbol_table<uppercase_export_policy> — subclass of lang::symbol_table<>)
  |
  v
resolve_imports
  |- lang::module_resolver.resolve("x.y")  [9-tier, .crank preset]
  |- lang::import_graph.declare_imports(...)
  |- lang::dependency_graph.cycle_nodes()  [cycle detection]
  `- lang::symbol_entry flow (crank adapters)
  |
  v
compile_order (lang::dependency_graph.topo_order() — Kahn's algorithm)
  |
  v
execute
  |- lang::stable_entity_id + fingerprint  [stable caching keys]
  `- lang::function_descriptor + typed_thunk  [host embedding]
```

### When to Use Generic Directly

**For Language Developers:**

1. **Custom DSLs.** Need module resolution and symbol tables without rewriting Crank? Use generic.
2. **Stable IDs for Caching.** `lang::detail::make_id()` and fingerprinting work for any language.
3. **Diagnostics.** `lang::diagnostic` + `lang::collecting_sink` are language-agnostic.
4. **Effect/Capability Tracking.** Build your own effect system on `lang::effect_registry`.

---

## Storage & the Generic IR

Crank's AST storage has two representations, both in `include/languages/crank/build_ast.hpp`, and both are **populated simultaneously** during every parse (dual-write in `AstBuilder`):

| Name               | Type                                          | Use case                                                   |
|--------------------|-----------------------------------------------|------------------------------------------------------------|
| `crank_ast_arena`  | `lang::ast_arena<crank_ast_node>`             | Existing code — variant nodes, children embedded inline    |
| `crank_ir_module`  | `lang::ir_module<crank_kind, crank_node_ext>` | New code — flat ir_node store, children in sidecar vector  |

Both live in `crank_source_file::arena` and `crank_source_file::ir_mod`.

### crank_ir_module (typed-AST flavor of ir_module)

`crank_ir_module` is `lang::ir_module<crank_kind, crank_node_ext>` — the **typed-AST** end of the simple→complex ExtPayload ladder defined in the generic IR layer.

```cpp
// include/languages/crank/build_ast.hpp
enum class crank_kind : std::uint8_t { fn, block, let, var, ..., ident };  // 30 values

struct crank_node_ext {
    std::string name;            // fn, let, var, call, attribute, module_decl, ident, extern_fn
    std::string type_hint;       // let, var
    std::string text;            // literal
    std::string backing_name;    // view_decl
    std::string host_link;       // extern_fn
    std::string key, value;      // tx_option
    std::string return_type_hint;
    std::vector<std::string> param_names, param_type_hints;  // extern_fn
    bool has_params = false;     // module_decl
};

using crank_ir_module = lang::ir_module<crank_kind, crank_node_ext>;
```

**Children** are stored in `ir_module`'s flat `child_ids_` sidecar (via `append_children`).  
**`structural_hash`** lives directly in `ir_node` — no external side-table needed.

### Dual-write: what the walker does (Stage 8b)

`AstBuilder` performs a **single parse pass** and writes every node to both stores:

```
lexy parse_tree event
  → build_typed_node()  → crank_ast_arena::push(variant_node)   → crank_node_id
  → build_ir_node()     → crank_ir_module::push(ir_node)        → ir_node_id
                          + ir_module::append_children(ir_children)
```

`crank_source_file` after a parse:
```cpp
sf.arena.size() == sf.ir_mod.size()   // always equal (parity invariant)
sf.ir_mod.root() != lang::k_null_ir   // root set at source_file exit
sf.ir_mod.as_egraph_view()            // immediately usable
```

### Capabilities gained

Because `crank_ir_module` is a full `ir_module` instance, crank gains:

- `as_egraph_view()` / `as_adjacency()` — feed egraph / DominatorTree directly
- `ir_interner` interning (via `interning.hpp`) — opt-in hash-cons dedup
- `children(id)` — flat `std::span` over the sidecar (no recursion)
- `reset()` — clear + reuse allocation in multi-pass pipelines

All opt-in; zero cost when unused.

### Parser stays lexy (design constraint)

Crank parses with **lexy** (`languages/crank/lexer.hpp`, `parser.hpp`). This is a permanent design constraint — there is no samasa parser migration planned. `crank_ir_module` is a **storage** change only; the parse path is byte-for-byte unchanged.

---

## Two Module Concepts

Crank has **two orthogonal "module" concepts**. Both are supported; they do not overlap.
(Rust precedent: `mod`/`use` file layer vs generics parametric layer.)

| Concept                   | Surface syntax                 | Unit                        | Descriptor                         | Purpose                                                                |
|---------------------------|--------------------------------|-----------------------------|------------------------------------|------------------------------------------------------------------------|
| **File module** (package) | `package a.b` + `import "x.y"` | a source file / directory   | `crank::module_descriptor`         | namespace + cross-file symbol flow                                     |
| **Parametric module**     | `module M[T: Bound] { … }`     | a named block inside a file | `crank::generic_module_descriptor` | reusable type/const-parameterized code, monomorphized on instantiation |

**Single-source visibility.** Exports/visibility live in exactly one place — the
`symbol_table` (uppercase name → exported; explicit `pub` forces export regardless of
case). Both file modules (`import` pulls exported symbols) and parametric modules (`pub`
items) read visibility from it. There is no separate export/visibility struct.

**Generic-layer backing.** Module-2 semantics build on the language-neutral `lang::` layer
(`languages/generic/module/module_system.hpp` + `import_resolver.hpp`): file-module
identity/resolution, the dependency graph (with `cycle_nodes()` diagnostics), and the import
pipeline (version/capability/circular checks + symbol flow) are `lang::`-owned. Crank retains
only its extensions — the `.crank` resolver preset, monomorphization instantiation keys, and
`@host/@gpu` ext-band effects — and converts at the import boundary (see the flow below) so
crank's own fingerprints and symbol-table fields (source span, HM type-var binding) are
unchanged.

### File-module flow (`package` / `import`)

```text
parse(package/import)
  → build_ast: crank_source_file{ package_name, imports[] }   (imports captured, not discarded)
  → resolve: uppercase/pub items → symbol_table (exported)
  → context.resolve_imports(package, imports):
        resolver.resolve("x.y")               (9-tier, .crank preset)
        lang::import_graph.declare_imports(package, {import_spec{"x.y"}})
        import_graph.resolve(mirror, caps, symbol_provider)
          → circular  LANG-IMP-003
          → version   LANG-IMP-004
          → capability LANG-IMP-005
          → exported symbols flow into importer (crank → lang::symbol_entry adapter)
  → compile_order (topo, importees first) drives engine::load
```

The import pipeline is invoked from `engine::load` for source modules; a hard import error
surfaces as a `module_resolve`-stage `crank_error` carrying the `LANG-IMP-00x` code.

### Parametric-module flow (`module M[…]{}`)

```text
parse(module_decl, reuses existing generic_params grammar)
  → build_ast: module_decl_node → crank::generic_module_descriptor
  → resolve: pub/uppercase items → symbol_table.exported (the export set; no bespoke struct)
  → instantiate_module(gm, type_args, const_args):
        instantiation_key.fingerprint → concrete "M#<fp>" module_descriptor
        → AOT cache key (monomorphize, §v2.14) — crank-specific, stays in crank
```

---

## Host Embedding Architecture

Crank embeds inside C++ applications through a three-stage lifecycle:

```text
context_builder
    ↓  register functions/types/containers/resources/backends/annotations
finalized_context   (immutable; produced by builder.finalize())
    ↓  frozen registry snapshots, descriptor fingerprints, direct thunks
runtime_instance    (adds scheduler, cancellation, observability)
```

### Typed vs Dynamic Boundary

| Path             | When                                           | Mechanism                                                                                               |
|------------------|------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| Typed fast path  | free functions, non-capturing lambdas          | `function_descriptor::typed_thunk` — direct `void(*)(const void* const*, void*)` pointer, no `std::any` |
| Dynamic boundary | plugins, capturing callables, REPL, reflection | `function_descriptor::trampoline` — `std::function<std::any(...)>` at explicit boundary only            |

### Stable Entity Identities

All descriptors carry a `stable_entity_id` (two FNV-1a 64-bit hashes over namespace+name, kind, schema version). IDs are
independent of pointer values, RTTI, and registration order.

| Type alias           | Kind constant        | Purpose                                   |
|----------------------|----------------------|-------------------------------------------|
| `stable_function_id` | `kKindFunction = 1`  | identifies a registered host function     |
| `stable_type_id`     | `kKindType = 2`      | identifies a registered C++ type          |
| `stable_field_id`    | `kKindField = 3`     | identifies a field within a type          |
| `stable_resource_id` | `kKindResource = 4`  | identifies a host resource                |
| `host_backend_id`    | `kKindBackend = 5`   | identifies a registered execution backend |
| —                    | `kKindContainer = 6` | identifies a container registration       |

`descriptor_fingerprint` (`uint64_t`) is a stronger hash over the full descriptor. The`finalized_context::fingerprint()`
is a fold over all descriptor fingerprints — use it for AOT cache keys and artifact compatibility checks.

### Key Types in `host.hpp`

| Type                              | Purpose                                                                                                   |
|-----------------------------------|-----------------------------------------------------------------------------------------------------------|
| `function_descriptor`             | full FFI descriptor: stable ID, arity, effects, capabilities, flags, typed thunk, trampoline, fingerprint |
| `function_options`                | per-registration overrides: effects, capabilities, flags, boundary policy, blocking class                 |
| `host_type_descriptor`            | C++ type: name, size, alignment, category, ownership, layout, fields, op-table, fingerprint               |
| `field_descriptor`                | field: stable ID, name, type, offset, byte_size, access, mutable/nullable/transient                       |
| `type_operation_table`            | copy/move/destroy/equal/hash function pointers derived from C++ traits                                    |
| `container_descriptor`            | container: stable IDs, name, `container_capabilities` bitmask, element/key/value type IDs                 |
| `resource_descriptor`             | host resource: stable ID, lifetime, threading, tx integration, factory                                    |
| `host_backend_descriptor`         | execution backend: kind, version, capabilities, limits, async/cancel/deadline support                     |
| `host_error` / `error_adapter<E>` | typed error conversion at host/Crank boundary                                                             |
| `host_value`                      | borrowed typed reference (stable_type_id + void* + lifetime_token)                                        |
| `owned_host_value`                | owned typed value with 24-byte SBO; no `std::any`                                                         |
| `dynamic_callable`                | erased callable with explicit state ownership (replaces `std::function` in plugins)                       |
| `context_builder`                 | mutable builder; fluent API; consumed by `finalize()`                                                     |
| `finalized_context`               | immutable registry snapshots; thread-safe reads; `host_build_result` factory                              |
| `host_build_result`               | `std::expected<finalized_context, std::vector<build_diagnostic>>`                                         |

### Finalization Algorithm (§18)

`context_builder::finalize()` runs:

1. Detect stable-name collisions (functions, types, resources).
2. For duplicate function names: compare normalized `parameter_types`; identical signatures → error.
3. Compute per-descriptor fingerprints.
4. Fold all fingerprints into `registry_fingerprint`.
5. Construct immutable `finalized_context`.
6. Return `host_build_result` (success or aggregated diagnostics).

### Usage Example

```cpp
crank::context_builder builder;

builder
    .register_function<"math.dot", dot>(
        crank::function_options{.flags = static_cast<crank::function_flags>(
            crank::function_flag::pure)})
    .register_type<Vec3>()
    .register_container<std::vector<float>>("FloatVector")
    .register_resource<AccountStore>("accounts",
        crank::resource_options{.threading = crank::resource_threading::concurrent});

auto result = std::move(builder).finalize();
if (!result)
    return report(result.error());

// Use result->functions(), result->types(), result->fingerprint() etc.
```

### Grammar Extensions (§21)

| Construct                                                   | §ref | Status                                                                     |
|-------------------------------------------------------------|------|----------------------------------------------------------------------------|
| `host module math { fn dot(a: Vec3, b: Vec3) -> Float32; }` | §21  | Not available — generated from C++ descriptors by `finalized_context`      |
| `@host.link("math.dot") extern fn dot(…)`                   | §21  | Implemented — grammar §5.6; `verify_extern_fn_decl`; CRANK-EXT-010/011/012 |

---

## Standard Library (`std/`)

The standard library is **not a separate runtime**. It is a reflection-driven
projection of existing C++/STL (and a thin libuv veneer) into Crank through the
same host-embedding seams described above — `context::register_function_descriptor`
(typed thunk + options) plus `ffi_module_builder` (import-visible `std.x` symbol).
The language core is untouched, and users pay only for the modules they install.

Location: `include/languages/crank/std/`. Umbrella: `std/std.hpp`. Shared
registration funnel: `std/detail/register.hpp` (`detail::add_fn<HostName, Fn>`
builds the typed descriptor and records the ffi symbol in one call; arity comes
from `callable_traits`, never hand-typed).

### Install API

```cpp
#include "languages/crank/std/std.hpp"

crank::engine e;
crank::stdlib::install_std_all(e.context());     // every available module
// …or opt in per module:
crank::stdlib::install_std_math(e.context());
crank::stdlib::install_std_string(e.context());
```

`install_std_all` installs the always-on modules and adds the guarded ones only
when their dependency is present (see guards below).

### Modules

| Module            | Install fn                    | Backing                | Effect / capability        | Representative surface                              |
|-------------------|-------------------------------|------------------------|----------------------------|-----------------------------------------------------|
| `std.core`        | `install_std_core`            | `<optional>`/`<expected>` | pure                    | `IsSome`, `UnwrapOr`, `Clamp`                       |
| `std.math`        | `install_std_math`            | `<cmath>`/`<numbers>`  | pure · deterministic       | `Sqrt`, `Pow`, `MinInt`, `Pi`                       |
| `std.string`      | `install_std_string`          | `std::string`          | pure                       | `Trim`, `ToUpper`, `Split`, `Join`, `Contains`      |
| `std.collections` | `install_std_collections`     | `vector`/`unordered_map`/`unordered_set` | pure     | `VecLen`, `MapGet`, `SetContains`                   |
| `std.containers`  | `install_std_containers`      | `containers::union_find` + STL `vector` | pure      | `ConnectedComponents`, `TopoOrder`, `BfsOrder`, `VecIntSort` |
| `std.time`        | `install_std_time`            | `<chrono>`/`<thread>`  | thread-safe; `SleepMillis` blocking | `NowNanos`, `SleepMillis`                  |
| `std.io`          | `install_std_io`              | `<print>` + `stdin`    | IO · Read/Write · blocking | `Print`, `Println`, `EPrintln`, `ReadLine`, `ReadAllStdin` |
| `std.fs`          | `install_std_fs`              | `std::filesystem`      | FileSystem · Read/Write · blocking | `ReadFile`, `WriteFile`, `Exists`, `Remove` |
| `std.process`     | `install_std_process`         | `std::getenv` + libuv  | IO · Read/Execute          | `Env`, `HasEnv`, `SpawnWait`†                       |
| `std.net`         | `install_std_net`             | libuv (`uv_tcp`/`uv_udp`) | Network · Network       | `ConnectTcp`, `ListenTcp`, `BindUdp`†               |
| `std.json`        | `install_std_json`            | Glaze (`glz::generic`) | pure                       | `Parse`, `Stringify`, `GetString`, `GetInt`‡        |

† libuv-gated · ‡ Glaze-gated (see guards).

### Guards (pay-for-use)

Optional dependencies are detected via `__has_include`, so the tree builds with
or without them:

- `CRANK_STD_HAS_UV` (`<uv.h>`) — enables `std.net`, `std.process.SpawnWait`, and
  the generic RAII veneer in `std/detail/uv_loop.hpp` (`crank::uvx::loop`/`timer`,
  dependency-free of crank types so it can be lifted out later). When absent, the
  whole `std.net` header compiles to a no-op and `install_std_all` skips it.
- `CRANK_STD_HAS_GLAZE` (`<glaze/glaze.hpp>`) — enables `std.json`. The DOM is
  wrapped in an opaque, copyable `json_value` registered as a host type
  (`type_descriptor` with empty `fields`); accessors read one top-level key with
  a typed fallback. Absent → header no-op, skipped by `install_std_all`.

Verification uses the same tooling API as any extern:
`verify_extern_fn_decl(ctx, crank_name, "std.math.sqrt", 1)` resolves the typed
thunk; `crank::invoke_typed<double>(*decl.descriptor, 4.0)` invokes it directly.

### `std.containers` — internal container algorithms

Where `std.collections` exposes STL container *storage*, `std.containers`
projects our internal container *algorithms* (`containers::union_find` plus a
locally-built CSR adjacency) as pure functions. The typed-thunk boundary is
copy-in/copy-out, so a graph is passed as two parallel `VecInt` endpoint arrays
plus a node count rather than a stateful graph object — the same value-semantic
shape the rest of the stdlib uses. Endpoints out of `[0, n)` are ignored (no
UB). Surface: `ConnectedComponents`/`ComponentCount`/`SameComponent` (union-find),
`BfsOrder`/`DfsOrder`/`TopoOrder`/`HasCycle`/`ReachableCount` (directed
traversal; `TopoOrder` returns an empty vector for a cyclic graph), and the
`VecIntSort`/`VecIntUnique`/`VecIntReverse`/`VecIntSum`/`VecIntConcat` helpers.

### cranki auto-installs the standard library

The `cranki` REPL (`src/cranki/`) calls `install_std_all` on every fresh
`crank::context` (construction and `reset()`), so interpreter users get the same
stdlib surface as a C++ embedder — `import "std.containers"` and the other
always-on modules resolve out of the box, with the libuv/glaze-guarded modules
added when their dependency is present.

---

## Frontend Lowering Contract

Lithe owns the authoritative type map between Crank source types and Lithe IR
type strings (`lithe-ir-spec.md §5`). All Crank lowering passes MUST use
`lithe::ir::frontend` APIs; informal ad-hoc mappings are prohibited.

Header: `include/lithe/lithe_ir/frontend/lowering_contract.hpp`

### Scalar Type Map

| Crank source type | Lithe IR type string |
|-------------------|----------------------|
| `Bool`            | `"i1"`               |
| `Int8` / `i8`     | `"i8"`               |
| `Int16` / `i16`   | `"i16"`              |
| `Int32` / `i32`   | `"i32"`              |
| `Int64` / `i64`   | `"i64"`              |
| `UInt8` / `u8`    | `"i8"`               |
| `UInt16` / `u16`  | `"i16"`              |
| `UInt32` / `u32`  | `"i32"`              |
| `UInt64` / `u64`  | `"i64"`              |
| `Float32` / `f32` | `"f32"`              |
| `Float64` / `f64` | `"f64"`              |

### Tensor/Slice Type Map

| Crank type            | Lithe IR memref string |
|-----------------------|------------------------|
| `[]T` (dynamic slice) | `"memref<?x<T_ir>>"`   |
| `[N]T` (static array) | `"memref<Nx<T_ir>>"`   |
| `[M][N]T` (2D)        | `"memref<MxNx<T_ir>>"` |

Derived via `lithe::ir::frontend::tensor_type_to_ir_str(elem, rank, dims)`.
Dimension -1 maps to `?` (dynamic).

### Capability Map

| Crank feature                | Required `portable_capability_bit` |
|------------------------------|------------------------------------|
| `transaction { }` block      | `transactions`                     |
| `@host` / host function call | `external_calls`                   |
| `defer` statement            | `defer_scopes`                     |
| `@atomic` access             | `atomics`                          |
| `@simd` annotation           | `simd_hint`                        |
| `@gpu` / `Device[Gpu]`       | `gpu_hint`                         |
| `@reflect(…)`                | `reflection`                       |
| exception propagation        | `exceptions`                       |

Derived via `lithe::ir::frontend::crank_capability_required(crank_feature::X)`.

### Usage in lower_hl.hpp

`tensor_info::elem_crank_type` (e.g. `"Float64"`) is resolved by
`lower_to_hl` through `lower_tensor_type(elem_crank_type, rank, dims)`.
Unknown element types produce a diagnostic; lowering aborts that tensor.

---

## Lowering to Lithe HL MIR

`lower_to_hl` lowers each Crank source construct to portable HL MIR ops. The module must declare the corresponding
`portable_capability_bit` for any capability-gated op it emits.

### Source → Portable HL MIR op map

| Crank source                    | Portable HL MIR ops                                               | Required capability |
|---------------------------------|-------------------------------------------------------------------|---------------------|
| `if (c) { A } else { B }`       | `icmp`/`fcmp` → `branch_cond` → `branch` (then→join, else→join)   | —                   |
| `while (c) { body }`            | header `icmp`/`fcmp` → `branch_cond`; body `branch` back-edge     | —                   |
| `match expr { arm => … }`       | chain of `icmp` + `branch_cond` per arm; default = final `branch` | —                   |
| `return expr` / fall-off        | `ret` (variadic; zero operands for Unit return)                   | —                   |
| `break`                         | `branch` to loop exit block                                       | —                   |
| `continue`                      | `branch` to loop header block                                     | —                   |
| `cond ? a : b` / simple if-expr | `icmp`/`fcmp` → `select`                                          | —                   |
| comparisons `== != < <= > >=`   | `icmp` (int/bool) / `fcmp` (float) → `i1` result                  | —                   |
| signed `/`, `%`                 | `sdiv`, `srem`                                                    | —                   |
| unsigned `/`, `%`               | `udiv`, `urem`                                                    | —                   |
| `& \| ^ ~` (bitwise)            | `bit_and`, `bit_or`, `bit_xor`, `bit_not`                         | —                   |
| `&^` (and-not)                  | `bit_and` of lhs and `bit_not` rhs                                | —                   |
| `<<`, `>>` arithmetic           | `shl`, `ashr` (signed operand)                                    | —                   |
| `<<`, `>>` logical              | `shl`, `lshr` (unsigned operand)                                  | —                   |
| unknown safety obligation       | `icmp(ne)` + `guard` (kind/policy/diag)                           | —                   |
| trap/terminate failure path     | `trap` terminator in guard-failure block                          | —                   |
| `defer call()`                  | `cleanup_region` + `call` stubs (LIFO) + `cleanup_yield`          | `defer_scopes`      |
| `transaction(cfg) { body }`     | `tx.region` + `tx.read`/`tx.write` + `tx.yield` or `tx.abort`     | `transactions`      |
| `@host` / host call             | `call`                                                            | `external_calls`    |

### Compare predicate mapping

Integer comparisons (`icmp`): predicates come from `k_icmp_predicates` (§17.7 of the Lithe IR spec).

| Crank op | Signed int | Unsigned int | Float (`fcmp`) |
|----------|------------|--------------|----------------|
| `==`     | `eq`       | `eq`         | `oeq`          |
| `!=`     | `ne`       | `ne`         | `one`          |
| `<`      | `slt`      | `ult`        | `olt`          |
| `<=`     | `sle`      | `ule`        | `ole`          |
| `>`      | `sgt`      | `ugt`        | `ogt`          |
| `>=`     | `sge`      | `uge`        | `oge`          |

Predicate strings are consumed from `lithe::ir::frontend::k_icmp_predicates` / `k_fcmp_predicates` — never hardcoded in
Crank.

### Safety obligation → guard kind mapping

| Crank obligation | `guard_kind`      | Trap kind          |
|------------------|-------------------|--------------------|
| bounds check     | `bounds`          | `bounds_violation` |
| div-by-zero      | `div_by_zero`     | `div_by_zero`      |
| narrowing cast   | `range_cast`      | `range_conversion` |
| user assertion   | `assert`          | `assert_failed`    |
| overflow         | `overflow`        | `overflow_checked` |
| tx precondition  | `transaction`     | `tx_failed`        |
| parallel safety  | `parallel_safety` | `unreachable`      |

- **proven** obligation → no guard emitted (Tarka discharged it).
- **unknown** obligation → `icmp(ne)` + `guard` with kind/policy; trap/terminate policy also emits `trap` terminator in
  the failure block.
- **refuted** obligation → compile-time diagnostic; no runtime op.

### Defer → cleanup_region

When a function/scope has `defer` statements:

- Body is wrapped in a `cleanup_region` that owns a cleanup block.
- Deferred calls (args evaluated at defer site) are emitted as `call` stubs in LIFO order.
- Cleanup block ends in `cleanup_yield`.
- **Controlled** exit edges (return/break/continue/guard-return_result) route through cleanup in LIFO order.
- **Trap/terminate** exit edges bypass the cleanup region entirely.
- Module declares `defer_scopes` capability.

### Transaction → tx.region

`transaction(cfg) { body }` lowers to:

- `tx.region` op carrying `tx_attr` (isolation/retry/replay/conflict/partial/durability/distribution/coordinator) mapped
  1:1 from the Crank transaction config.
- Body region: `resource[key]` read → `tx.read %res, %key`; write → `tx.write %res, %key, %v`.
- `old(resource[key])` snapshot: `tx.read` with snapshot bit in `tx_attr`.
- Normal completion / `yield expr` → `tx.yield`.
- `abort(err)` → `tx.abort %err` (terminator).
- `tx.region` produces one SSA result (commit report / `TransactionResult[T]`).
- Module declares `transactions` capability.

### Verifier delegation

Crank's pre-freeze verifier (`verify_mir.hpp`) keeps only Crank-specific invariants:

- Refuted obligation not resolved before lowering.
- Defer body contains only `call` ops.
- Transactional write appears outside a `tx.region`.

All structural checks (block terminator, branch-target existence, SSA dominance, region nesting, capability coverage)are
delegated to `lithe::ir::portable::verify_portable` post-freeze — single source of truth, no drift.

Link: `docs/lithe/lithe.md` and `docs/lithe/lithe-ir-spec.md §8, §13` for the IR-side detail.

---

## Compilation Pipeline

| Step | Description                                          | Key types                                       |
|------|------------------------------------------------------|-------------------------------------------------|
| 1    | Lex + parse                                          | `frontend::parse_result`                        |
| 2    | Name resolution                                      | `resolve_result`                                |
| 3    | Type inference                                       | `sema_context`                                  |
| 4    | Effect checking                                      | `effects_result`                                |
| 5    | Safety obligations                                   | `obligation_record[]`                           |
| 6    | Generic monomorphization                             | `monomorphize_result`                           |
| 7    | Annotation resolution                                | `annotation_resolution[]`                       |
| 8    | Optimization profile selection                       | `crank::o0..o3_profile`                         |
| 9    | HL MIR lowering (uses frontend lowering contract)    | `lower_hl_result`                               |
| 10   | Tx compile-time checks                               | `tx_lowering_result`                            |
| 10a  | Tx runtime lowering                                  | `execute_transaction` / `lower_transaction_aot` |
| 11   | Parallel plan extraction                             | `parallel_plan_result`                          |
| 12   | Lower to physical MIR (cached per `lower_hl_result`) | `lower_to_physical` → `lower_phase_result`      |
| 13   | Execute (interpreter / AOT)                          | `execute_physical` → `crank_execute_result`     |

---

## Algorithms Used

Concrete named algorithms in the compiler implementation, with the header they live in.

| Concern | Algorithm | Where |
|---|---|---|
| Lexing/parsing | lexy PEG/LL(1) combinator grammar; 13-level operator-precedence table (`dsl::expression`, L1…L13 primary) | `parser.hpp` |
| Statement termination | Automatic semicolon insertion via `line_continues` context flag + `bracket_depth` context counter | `parser.hpp` |
| AST construction | Post-order parse-tree walk → dual store (`crank_ast_arena` variant store + `crank_ir_module` flat SoA, Stage 8b) | `build_ast.hpp` |
| Type inference | Hindley-Milner (Algorithm-W style) inference over the Vakya tree; unification + generalization | `sema_types.hpp`, `context.hpp` |
| Name resolution | 9-tier module resolver order (local → params → module → imports → prelude → host → …) | `resolve.hpp`, `module.hpp` |
| Effect/capability tracking | Effect-mask propagation + capability lattice join | `effects.hpp` |
| Safety verification | Obligation discharge via Tarka (proven / unknown / refuted three-way outcome); assumption-context refinement | `verify.hpp`, `obligations.hpp` |
| Generics | Monomorphization by type substitution; trait/impl coherence checking; const-generic arithmetic eval | `monomorphize.hpp`, `generics.hpp`, `coherence.hpp` |
| Lowering | 5-phase HL MIR lowering: A control-flow → branch/icmp, B integer ops, C safety → guard/trap, D defer → cleanup_region (LIFO), E transaction → tx.region | `lower_hl.hpp` |
| Parallel extraction | Pravaha task-graph extraction from `spawn`/`await` + `@parallel` structured loops | `parallel.hpp` |
| Transactions | 2-phase commit over `TransactionParticipant`; WAL record log; retry with backoff; Medha isolation levels | `transaction.hpp` |
| AOT cache | Content-addressed artifact cache keyed on `lower_hl_result`; security-policy gate | `aot.hpp` |
| Annotations | Namespaced annotation resolution: closed builtin set + prefixed-plugin lookup, kind→decision routing | `annotation.hpp` |
| Compiler-phase parallelism | `parse_modules_parallel` / `batch_load_modules` fan-out over module set | `engine.hpp`, `execute.hpp` |

---

## Quick Start: Host Embedding (`context.hpp`)

```cpp
#include "languages/crank/context.hpp"
#include "languages/crank/frontend.hpp"

crank::context ctx;

// 1. Module paths
ctx.modules().add_path("/my/scripts");

// 2. Host functions
ctx.register_function<"math.dot", dot>();

// 3. Host types
ctx.register_type<Vec3>();

// 4. Host containers
ctx.register_container<std::vector<float>>("float_vec");

// 5. Execution policy
ctx.execution()
   .use_pravaha()
   .scheduler(crank::scheduler_policy::work_stealing)
   .fallback(crank::fallback_policy::safe_cpu);

// 6. Transactional resources (Module 5A)
ctx.register_transactional<AccountStore>("accounts");

// 7. Parse + analyse
auto parse = crank::frontend::parse(source_string);
auto ar    = ctx.analyse(parse);              // preferred: accepts parse_result directly
// or: ctx.analyse(parse.ok, "my_module");    // legacy bool overload

// 8. Lower + execute
auto hl = crank::lower_to_hl(build_input(parse));
auto r  = crank::execute_via_interpreter(hl);       // one-shot: lower + execute

// Staged (benchmarks / hot loops): lower once, interpret many times.
auto lp = crank::lower_to_physical(hl);             // phase 1 (cached in hl)
auto r2 = crank::execute_physical(*lp.phys);        // phase 2 (execute only)
```

**Phase timing.** `crank_execute_result::stats` reports `lower_ns` and
`execute_ns` separately. Lowering (HL MIR → physical MIR via
`coordinate_lowering_pass`) is cached in `lower_hl_result::cached_phys`, so it
runs once per result; `lower_to_physical` returns `lower_ns == 0` on a cache hit.
`execute_physical` measures interpretation only — use it to isolate execute cost
from lowering cost when benchmarking against native C++.

---

## Engine Façade (`engine.hpp`)

`crank::engine` is a **one-call embedder API** that orchestrates the full pipeline
(`frontend::parse` → `ctx.analyse` → `lower_to_hl` → `execute_via_interpreter`)
with ergonomic entry points, diagnostics collation, and module/extern support.
No pipeline stage is reimplemented; the engine delegates to the same functions as
the staged API above.

### Quick Start

```cpp
#include "languages/crank/engine.hpp"

// Simplest: eval returns a value directly
crank::engine e;
auto r = e.eval("fn Main() -> Int64 { return 42 }");
if (r) {
    auto v = r->as<std::int64_t>(); // expected<int64_t, crank_error>
}

// Fluent option presets
auto e2 = crank::engine{crank::engine_options::strict()};  // verify::check, aot on

// Free functions (own engine per call — use a persistent engine for loops)
auto val = crank::eval("fn Main() -> Int64 { return 1 }");
auto rep = crank::run("fn Main() -> Int64 { return 2 }");
```

### `engine_options`

| Field                 | Default                 | Meaning                                                       |
|-----------------------|-------------------------|---------------------------------------------------------------|
| `verify`              | `verify_policy::assume` | Pre/postcondition discharge mode (see §`verify_policy` Modes) |
| `aot_cache`           | `false`                 | Cache lowered physical MIR for reuse                          |
| `diagnostics_verbose` | `false`                 | Populate `run_report::plan()` backend-selection entries       |
| `target`              | `target_kind::host`     | Advisory capability pin for the backend planner               |
| `permit_parallel`     | `true`                  | Allow the planner to select threaded backends                 |
| `permit_simd`         | `true`                  | Allow the planner to select SIMD backends                     |
| `permit_gpu`          | `true`                  | Allow the planner to select GPU backends                      |

Named presets:

- `engine_options::scripting()` — `verify::assume`, all backends permitted, aot off.
- `engine_options::strict()` — `verify::check`, aot on.

`permit_*` options **cap** what the automatic Lithe planner may select (deployment/security gate).
They never suppress language features. A region annotated `@gpu(required=true)` under
`permit_gpu=false` is a diagnostic — no silent degradation.

### Staged API: `compile` + `execute`

```cpp
// Lower once, run many times (avoids re-parsing and re-lowering on each call)
auto prog = e.compile("fn Main() -> Int64 { return 99 }");
if (prog) {
    auto v1 = prog->execute();
    auto v2 = prog->execute();  // HL MIR reused; only execution cost
}

// execute_report: same but returns run_stats + diagnostics + plan
run_report rr = prog->execute_report();
auto stats = rr.stats;  // lower_ns, execute_ns, instr_count, …
```

### `run` → `run_report`

`engine::run()` returns a full `run_report` instead of a bare `value`:

```cpp
auto rr = e.run("fn Main() -> Int64 { return 7 }");
if (rr && rr->ok()) {
    std::int64_t v = *rr->result.as<std::int64_t>();
    // rr->stats.lower_ns, execute_ns, instr_count, fallback_used
    // rr->notes — non-fatal observations
    // rr->plan() — backend-selection plan_view (see §Backend Intelligence)
}
```

### Error handling

All `engine` methods return `std::expected<T, crank_error>`. `crank_error` carries:

- `stage` — which pipeline stage failed (
  `error_stage::parse / analyse / lower / execute / module_resolve / extern_fn / options`)
- `code` — stable diagnostic code string (e.g. `"CRANK-PARSE-001"`, `"CRANK-EXT-010"`)
- `message` — human-readable description
- `notes` — additional context lines
- `format()` — single string for logging

### Error propagation — `?` operator

The postfix `?` operator applies to `Result[T, E]` and `Option[T]` values inside a function:

```crank
fn read_config(path: String) -> Result[Config, AppError]
{
    let raw = io.read_file(path)?      // Option → None-return or String
    let cfg = parse_config(raw)?       // Result[Config, ParseError] → AppError via From
    return Ok(cfg)
}
```

**Rules:**

- `Result[T, E]?` yields `T` on Ok; returns `Err(F.from(e))` on Err where the enclosing
  function's return is `Result[_, F]` and `F: From[E]`.
- `Option[T]?` yields `T` on Some; returns `None` on None.
- No generalized `Try` trait in v1 — `From`-based conversion only.
- Lowering: `?` desugars to a `match` + early `return` (no new IR op).

**Sema limits (`?` is a compile error when):**

| Code          | Condition                                                    |
|---------------|--------------------------------------------------------------|
| `CRANK-Q-001` | `?` on a non-`Result`/`Option` value                         |
| `CRANK-Q-002` | No `From[E]` impl — residual not convertible                 |
| `CRANK-Q-003` | Enclosing function return type incompatible                  |
| `CRANK-Q-004` | `?` inside a `pred_expr` (predicates are pure/total)         |
| `CRANK-Q-005` | `?` crossing a `transaction`/`async` boundary without policy |

For explicitly result-oriented conversion, the prelude provides:

```crank
let converted = Int32.try_from(value)    // Result[Int32, ConversionError]
let ratio     = Float64.try_from(count)  // Result[Float64, ConversionError]
```

`Type.try_from(value)` is the explicit `Result`-returning form. Contrast:

- `value as Type` — checked conversion with proof/guard policy (may trap/terminate depending on safety policy)
- `Type.try_from(value)` — always returns `Result[Type, ConversionError]`, no guard insertion

### Closures

Two syntactically equivalent closure forms; both lower to `closure_tag` (id 1020):

```crank
let sq  = fn(x: Float64) -> Float64 { return x * x }  // fn-form
let add = |a: Int64, b: Int64| a + b                   // pipe-form, bare-expr body
let dot = |u: Vec, v: Vec| -> Float64 { u.dot(v) }    // pipe-form, block body + return type
let one = || 1                                         // zero-param pipe closure
```

Capture semantics:

- `Copy` types: copied. Move-only types: moved. Borrowed values that may escape scope: **rejected** (`CRANK-CLOS-001`).
- Callability class inferred: consuming capture → `FnOnce`; mutable → `FnMut`; else `Fn`.
- `spawn closure` requires all captured values to be transfer-safe; violation → `CRANK-SPAWN-001`.

---

## Backend Intelligence (`plan_view`, §I)

Backend selection is **automatic per region** — Lithe decides per function/loop body based on
platform capability → legality → profitability → ranking. The engine does not hardcode backends;
`permit_*` options cap the candidate set and `target_kind` pins the assumed capability set for
cross-compile or reproducible benchmark scenarios.

```cpp
auto e = crank::engine{crank::engine_options{.diagnostics_verbose = true}};
auto rr = e.run(source);
if (rr) {
    auto pv = rr->plan();            // plan_view
    for (const auto& reg : pv.regions) {
        // reg.region_name       — function or loop body identifier
        // reg.selected_backend  — "scalar" / "simd" / "gpu" / "threaded"
        // reg.was_fallback       — true if a higher-ranked backend was tried first
        // reg.fallback_reason    — why preferred was not chosen (empty if not fallback)
        // reg.profitability_score — planner score (0 = not computed)
    }
    if (pv.any_fallback()) { /* at least one region fell back */ }
}
```

`plan_view` is populated only when `diagnostics_verbose = true`; otherwise `regions` is empty
(zero overhead in production). `plan_id` is a stable session-unique identifier for correlating
plan snapshots across calls.

**Acceptance criterion 26 — release gate:** Every unmet `@parallel`/`@simd`/`@gpu` preference
**must** produce a `region` with `was_fallback = true` and a non-empty `fallback_reason`
categorising why the preferred backend was not chosen (dependence/aliasing/cost/layout/
capability/device). Silently discarding a preference is a bug. Using `@gpu(required=true)` on a
target that cannot support it is a **hard compile diagnostic** (not a soft fallback).

### `target_kind` — advisory capability pin

| Value              | Effect on the planner                             |
|--------------------|---------------------------------------------------|
| `host`             | Discover the actual machine at runtime (default)  |
| `cpu_only`         | Cap to scalar interpreter only; SIMD/GPU disabled |
| `simd`             | Cap to scalar + SIMD; GPU excluded                |
| `gpu_if_available` | Full set; GPU included if present                 |

`target_kind` does NOT override legality or profitability filters — an ineligible backend
is still rejected even if the target includes it.

---

## Module Façade (`engine::load`, `module_graph_view`)

```cpp
// Add resolution paths on the context
e.context().modules().add_path("/my/project/scripts");

// Load a module by name — resolves through the 9-tier resolver order
auto mh = e.load("math.vector");
if (mh) {
    mh->name();          // "math.vector"
    mh->content_hash();  // FNV-1a 64-bit hash
    mh->was_cached();    // true if served from AOT cache
    mh->exports();       // std::span<const symbol> — loaded symbols
    mh->imports();       // std::span<const module_ref> — declared dependencies
}

// Topological view of all resolved modules (dependencies before dependents)
auto gv = e.module_graph();
for (const auto& entry : gv.modules) {
    // entry.name, entry.content_hash, entry.was_cached, entry.imports
}
const auto* m = gv.find("math.vector");  // nullptr if not loaded
```

`engine::module_graph()` merges the `dep_graph` (modules resolved via `import` statements during
analysis) with modules explicitly loaded via `engine::load()`. The order is topological — safe
for compilation ordering.

---

## Extern Functions (`@host.link`, §X)

`extern fn` binds a crank-side declared function to a registered host function without a body.
Analysis verifies the binding against the registered descriptor.

### Grammar (§5.6 of `grammar.md`)

```crank
@host.link("math.dot")
extern fn Dot(a: Vec3, b: Vec3) -> Float32

@pure
@host.link("math.cross")
extern fn Cross(a: Vec3, b: Vec3) -> Vec3
```

### Host-side registration

```cpp
// Register the C++ function before analysis
e.context().register_function<"math.dot", dot>();
```

### Verification (`verify_extern_fn_decl`)

```cpp
auto result = crank::verify_extern_fn_decl(e.context(), "Dot", "math.dot", 2);
if (result) {
    // result->descriptor  — pointer to the live function_descriptor
    // result->thunk       — direct typed_thunk (no std::any at call time)
    // result->fingerprint — fingerprint of the bound descriptor
}
```

### Diagnostic codes

| Code            | Condition                                           |
|-----------------|-----------------------------------------------------|
| `CRANK-EXT-010` | `@host.link` name not found in registered functions |
| `CRANK-EXT-011` | Declared arity ≠ registered descriptor arity        |
| `CRANK-EXT-012` | Effect escalation: declared effects < host provides |

`verify_extern_fn_decl` is exposed as a **host-side API** for tooling and explicit verification;
the analysis phase runs the same check automatically for each `extern fn` declaration encountered
during `ctx.analyse()`.

---

## Capability Matrix

| Feature                                                                                        | Status                                                                                                                                                           | §ref                    |
|------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------|
| Scalar (straight-line) execution                                                               | Implemented                                                                                                                                                      | §10.2                   |
| CFG interpretation (branches, loops, match, early return, defer, guards, transactions)          | Implemented — `branch`/`branch_cond` interpreted; loop-carried scalar results are pending                                                                        | §10.2                   |
| Parallel for/reduce/map                                                                        | Implemented — lowers to Pravaha                                                                                                                                  | §6.2                    |
| spawn/await                                                                                    | Implemented                                                                                                                                                      | §7                      |
| Transactions (compile-time checks)                                                             | Implemented                                                                                                                                                      | §7c                     |
| Transactions (runtime/AOT lowering)                                                            | Implemented — `execute_tx.hpp`                                                                                                                                   | §7c                     |
| Transaction state machine (`transaction_state`)                                                | Implemented — `transaction.hpp`                                                                                                                                  | §3.1                    |
| Transaction runtime context (`transaction_context`)                                            | Implemented — `transaction.hpp`                                                                                                                                  | §3.2                    |
| Transaction result type `Result[CommitReport, TxError]`                                        | Implemented — `transaction_tag` typing rule in `sema_types.hpp` (fresh var; Result wrapper resolved in module 5)                                                 | §2.1                    |
| `abort(error)` typing — Never result in tx body                                                | Implemented — `tx_abort_tag` typing rule in `sema_types.hpp`                                                                                                     | §2.3                    |
| `yield expr` typing — propagates T to enclosing tx                                             | Implemented — `tx_yield_tag` typing rule in `sema_types.hpp`; runtime `body_value` in `tx_runtime_result` via `tx_evaluator::on_yield`                           | §2.2                    |
| `FromTxError` error conversion concept                                                         | Implemented — `transaction.hpp`                                                                                                                                  | §5.2                    |
| Typed error discriminant (`TxErrorKind`)                                                       | Implemented — `transaction.hpp`                                                                                                                                  | §5.1                    |
| Resource capability checker                                                                    | Implemented — `resource_capability_checker` in `transaction.hpp`                                                                                                 | §6.3                    |
| `read_committed` isolation level                                                               | Implemented — `medha::isolation::read_committed`; `transaction_isolation` parser production updated                                                              | §8.1                    |
| Retry policy with backoff (`retry_policy`, `backoff_kind`)                                     | Implemented — `transaction.hpp`                                                                                                                                  | §10.3                   |
| Read-your-writes staging (§7.1)                                                                | Implemented — `detail::tx_staged_write_set` in `execute_tx.hpp`; staged writes visible to in-body reads; `old_snapshot` bypasses                                 | §7.1, §7.3              |
| Durability levels (`durability_level`)                                                         | Implemented — `durability_level` enum in `transaction.hpp`; `CrankTransactionOptions::durability`; `CrankCommitReport::durability`; `tx_plan_record::durability` | §14.1                   |
| Commit report lookup (`commit_report_store`, `lookup_commit_report`)                           | Implemented — `transaction.hpp`                                                                                                                                  | §15.3                   |
| Observability event enum (`transaction_event_kind`)                                            | Implemented — `transaction.hpp`                                                                                                                                  | §16.1                   |
| WAL record kinds (`log_record_kind`)                                                           | Implemented — `transaction.hpp`                                                                                                                                  | §14.2                   |
| Full commit report fields                                                                      | Implemented — `CrankCommitReport` in `transaction.hpp`; adds `transaction_id` and `durability`                                                                   | §15.1                   |
| `TransactionParticipant` concept                                                               | Implemented — `transaction.hpp`                                                                                                                                  | §13.2                   |
| In-process two-phase commit (`crank_coordinator_2pc`)                                          | Implemented — `transaction.hpp`                                                                                                                                  | §13.3–§13.4             |
| Generics + monomorphization                                                                    | Implemented                                                                                                                                                      | §8                      |
| Typed annotations                                                                              | Implemented                                                                                                                                                      | §5b, Extensions section |
| Execution policy (scheduler/fallback)                                                          | Implemented                                                                                                                                                      | §9.4, §6.3              |
| AOT MIR cache (verified physical MIR artifacts)                                                | Implemented                                                                                                                                                      | §10.4                   |
| AOT native code generation                                                                     | Implemented — adapter required (backend step beyond MIR)                                                                                                         | §10.4                   |
| AOT artifact signing / secure loader                                                           | Stubbed — `serialize()` write-only; no deserialize/parse; SEC checks are placeholders                                                                            | §10.4                   |
| GPU analysis / annotation routing                                                              | Implemented                                                                                                                                                      | §6.3                    |
| GPU code emission (SPIR-V)                                                                     | Implemented                                                                                                                                                      | §6.3                    |
| GPU dispatch                                                                                   | Implemented — adapter required; Vulkan-gated (`LITHE_VULKAN_BACKEND_AVAILABLE`)                                                                                  | §6.3                    |
| Metal GPU backend                                                                              | Implemented — optional native Metal provider over the shared HL-MIR device plan                                                                                | §6.3                    |
| SIMD analysis / annotation routing                                                             | Implemented                                                                                                                                                      | §6.3                    |
| SIMD execution backend                                                                         | Implemented — Highway (`lithe_codegen_simd.hpp`)                                                                                                                 | §6.3                    |
| Distribution analysis / policy                                                                 | Implemented — `allow_distributed` field; effects tracked                                                                                                         | §6.3                    |
| Distribution execution backend                                                                 | Implemented — host/plugin boundary only (metadata; no in-tree remote runtime)                                                                                    | §6.3                    |
| Coroutine analysis/planning (`spawn`/`await`, `crank_future<T>`)                               | Implemented                                                                                                                                                      | §7                      |
| Coroutine backends                                                                             | Implemented — `InlineBackend`, `JThreadBackend`; async `CoroutineBackend` planned                                                                                | §7                      |
| Futures runtime                                                                                | Stubbed — eager-inline (`future.hpp`)                                                                                                                            | §7                      |
| Structured concurrency (task scopes, cancellation, deadlines)                                  | Not available in grammar — host C++ APIs only                                                                                                                    | §7                      |
| Multi-resource transactions                                                                    | Parse-only in v1 — syntax accepted; sema enforces single-resource (`CRANK-TX-002`)                                                                                | §7c                     |
| Nested transactions / savepoints                                                               | Host C++ APIs only — not in grammar                                                                                                                              | §7c                     |
| Transactional collections `TxMap`/`TxSet`/`TxQueue`/`TxLog`                                    | Implemented — `tx_collections.hpp`                                                                                                                               | §12                     |
| Transactional counter `TxCounter<T>`                                                           | Implemented — `tx_collections.hpp`                                                                                                                               | §12.2                   |
| Associated types in generics                                                                   | Parse + sema coverage — lowering/backend integration remains scoped to supported v1 generic paths                                                                  | §v2.1                   |
| Generic modules                                                                                | Implemented (frontend + sema) — runtime/backend behavior follows current monomorphized module paths                                                               | §v2.3                   |
| Const-generic arithmetic                                                                       | Partial in v1 — literal/parameter forms implemented; arithmetic forms are reserved for v2                                                                          | §8                      |
| Controlled specialization                                                                      | Implemented                                                                                                                                                      | §8                      |
| Reflection + generated adapters                                                                | Implemented — program-built descriptors                                                                                                                          | §16                     |
| Runtime debugging (data model)                                                                 | Implemented                                                                                                                                                      | Debugging section       |
| Runtime debugging (stepping backend)                                                           | Stubbed — event/hook vocabulary defined; live pause/step not wired                                                                                               | Debugging section       |

---

# Module 1: Frontend — Lexer, Parser, AST

## Entry Point

```cpp
#include "languages/crank/frontend.hpp"

auto result = crank::frontend::parse(source_sv);
// result.ok              — true if no errors
// result.diagnostics     — collecting_sink
// result.parse_tree_json — JSON string (if dump_mode::parse_tree)
// result.ast_json        — JSON string (if dump_mode::ast)
```

Parse options:

```cpp
crank::frontend::parse_options opts;
opts.dump = crank::frontend::dump_mode::parse_tree; // or ast, or none
auto result = crank::frontend::parse(src, opts);
```

## Lexical Grammar

- **Identifiers**: `[a-zA-Z_][a-zA-Z0-9_]*` with reserved keywords.
- **Integer literals**: decimal, `0x`/`0X` hex, `0o` octal, `0b` binary. Digit separator `_` stripped.
- **Float literals**: `digits '.' digits [exponent]` or `digits exponent`. Exponent: `(e|E)[+-]digits`.
- **String literals**: `"..."` with `\n \t \r \\ \" \0 \xHH \u{HHHH}` escapes.
- **Raw strings**: `` `...` `` — no escape processing.
- **Bool literals**: `true`, `false`.
- **Line comments**: `// ... \n`.
- **Block comments**: `/* ... */`.

## Automatic Semicolon Insertion (ASI)

ASI fires at a newline when:

1. `line_continues_flag` is NOT set (last significant token does not continue the line), AND
2. `bracket_depth_counter` == 0 (not inside `()` or `[]`).

Tokens that set `line_continues_flag`:
operators, `else`, `.` (member access), `,`, `->`, `=>`, `(`, `[`, `{`.

A synthetic `stmt_term` token is emitted. The `else`/`.` carve-out prevents ASI before `else` or method chains.

## AST Tag Band (1000–1015)

| stable_id | Symbol               | Tag struct                      | Arity    |
|-----------|----------------------|---------------------------------|----------|
| 1000      | `fn`                 | `crank::fn_tag`                 | variadic |
| 1001      | `block`              | `crank::block_tag`              | variadic |
| 1002      | `let`                | `crank::let_tag`                | 2        |
| 1003      | `var`                | `crank::var_tag`                | 2        |
| 1004      | `match`              | `crank::match_tag`              | variadic |
| 1005      | `call`               | `crank::crank_call_tag`         | variadic |
| 1006      | `attribute`          | `crank::attribute_tag`          | variadic |
| 1007      | `field_access`       | `crank::field_access_tag`       | 2        |
| 1008      | `index`              | `crank::index_tag`              | 2        |
| 1009      | `range`              | `crank::range_tag`              | 2        |
| 1010      | `transaction`        | `crank::transaction_tag`        | variadic |
| 1011      | `transaction_option` | `crank::transaction_option_tag` | 2        |
| 1012      | `tx_load`            | `crank::tx_load_tag`            | 1        |
| 1013      | `tx_store`           | `crank::tx_store_tag`           | 2        |
| 1014      | `tx_abort`           | `crank::tx_abort_tag`           | 1        |
| 1015      | `tx_yield`           | `crank::tx_yield_tag`           | 1        |

## Parse → Vakya Flow

```
source_sv
  └─ grammar::parse()           → lexy parse_tree_t
       └─ AstBuilder::walk()    → std::any (root Vakya node)
            └─ build_node()     → vakya::make_node<Tag>(terminal)
                                   each production → extension tag node
                                   literals → vakya::as_expr(std::string)
  └─ build_result
       .root          — opaque std::any wrapping vakya::node<Tag,...>
       .typed_ast_root — shared_ptr<crank_source_file>; non-null on success
       .diagnostics   — collecting_sink
       .ok            — false if diagnostics.has_errors()
```

Module 1 uses a structural (untyped) walk: all children are flattened into a single `expr<std::string>` summary
terminal. Full typed unpack is deferred to module 2.

## Typed AST Node Types (`build_ast.hpp`)

The second walk output is a fully typed `crank_source_file` tree accessible via `build_result::typed_ast_root`. Children
are stored as index vectors (`std::vector<crank_node_id>`) into a flat `crank_ast_arena` — no `std::any` in the semantic
path.

```cpp
using crank_node_id = uint32_t;
inline constexpr crank_node_id k_null_node = UINT32_MAX;

struct fn_node          { string name; vector<crank_node_id> children; };
struct let_node         { string name; string type_hint; vector<crank_node_id> children; };
struct literal_node     { string text; };
struct ident_node       { string name; };
// … and: block_node, var_node, match_node, call_node, attribute_node,
//         field_access_node, index_node, range_node, tx_node, tx_option_node,
//         tx_load_node, tx_store_node, if_node, for_node, while_node,
//         return_node, spawn_node, await_node, defer_node, type_decl_node

using crank_ast_node = variant<fn_node, block_node, let_node, /*…*/ ident_node>;

class crank_ast_arena {
public:
    crank_node_id push(crank_ast_node);          // returns stable id
    const crank_ast_node& operator[](crank_node_id) const;
    // …
};

struct crank_source_file {
    string                package_name;
    vector<crank_node_id> top_level;   // fn/let/var/const/type declaration ids
    crank_ast_arena       arena;       // owns all nodes
};
```

Pattern-match on `crank_ast_node` with `std::visit` or `std::holds_alternative`. Traverse children by indexing into
`arena[child_id]`.

`std::any` is retained **only** at the plugin/host boundary (`build_result::root`) — the Vakya structural node used by
the legacy dump path. The semantic path (module 2 onwards) operates entirely on typed arena nodes.

Module 2 consumes `typed_ast_root` to drive name resolution and type inference.

## Function Declaration Shape

Grammar `func_decl` (grammar.md §5):

```
fn IDENT [generic_params] "(" [params] ")" ["-> type"] {contract_clause} block
```

The parenthesized params list is **always present** (may be empty). Missing it causes parse failure on any function
declaration — the parser sees an unexpected `(` after the function name, producing an empty parse_tree and skipping all
downstream dumps.

## Postfix Operators & Special Builtins

**Postfix chain** (highest precedence, level 12): `call ()`, `index []` chain left-to-right. Examples:

- `f(x)[0]` — call then index
- `arr[i](j)` — index then call

**Field access** (`.`): `obj.field` — available in assignment lvalues and via qualified identifiers.

**Grammar builtins** (primary expressions, Lean Charter §7):

- `len(expr)` — slice/array length (sole grammar-level builtin)

**Prelude functions** (parse as plain call expressions, not grammar::builtin_call):

- `cap(expr)` — slice capacity
- `append(slice, elem)` — append to slice
- `make(type, len)` — allocate slice/array
- `print(args)` — formatted output (capability-controlled I/O)
- `indices(shape)` — const-dim range over Shape (§5.5 view method bodies)
- `parallel.map(expr, f)` — parallel map
- `parallel.reduce(expr, init, f)` — parallel reduce
- `parallel.for(range, body)` — parallel for loop

**Range operators** (precedence level ~7, between shifts and bitwise):

- `expr .. expr` — exclusive range (excludes end)
- `expr ..= expr` — inclusive range (includes end)

**Type conversion** (`as`, precedence level 10, between arithmetic and prefix):

- `x as Int32` — checked conversion (narrowing emits guard if needed)
- `y as Float64` — numeric type conversion

**Predicates** (`pred_expr`):

- `old(expr)` — value at function entry (valid in `ensures` only) or transaction entry (valid in transaction body).
  Side-effect-free expressions only.
- `forall i: Int32 . predicate` — universal quantification (typed binder, legacy form)
- `exists i: Int32 . predicate` — existential quantification (typed binder, legacy form)
- `forall i in 0..len(xs): xs[i] >= 0` — universal quantification with ranged binder (new form)
- `exists i in 0..n: xs[i] == target` — existential quantification with ranged binder (new form)

Ranged binders desugar to guarded typed quantifiers:
`forall i in a..b: p` ≡ `forall i: <IntType> . (a <= i && i < b) -> p`.
Both forms produce `quantifier_tag` (id 1021); `quant_bound_kind` distinguishes `typed` from `ranged`.

## JSON Dump API

```cpp
// parse_tree → JSON
auto json = crank::dump_parse_tree(tree);
// {"kind":"source_file","children":[{"kind":"ident","token":"app"},...]}

// AST root → JSON (module 1: placeholder structural record)
auto json = crank::dump_ast(result.root, store);
// {"tag":"fn","id":1000}
```

Glaze (`glaze/json.hpp`) is used when available (`CRANK_HAS_GLAZE`). Falls back to a minimal hand-written serializer
otherwise.

## Parse Statistics

Collect detailed parsing metrics (token counts, error distribution, wall-clock timings) by enabling stats collection:

```cpp
crank::frontend::parse_options opts;
opts.collect_stats = true;
auto result = crank::frontend::parse(src, opts);

if (result.stats) {
    auto& stats = result.stats.value();
    std::cout << "Source: " << stats.source_bytes << " bytes, " 
              << stats.source_lines << " lines\n";
    std::cout << "Tokens: " << stats.total_tokens << " total, "
              << stats.trivia_tokens << " trivia, "
              << stats.asi_injections << " ASI\n";
    std::cout << "Parse time: " << stats.timings.lex_and_parse.count() << " ns\n";
    std::cout << "Build time: " << stats.timings.ast_build.count() << " ns\n";
}

// Dump stats to JSON
auto stats_json = crank::dump_stats(*result.stats);
```

`parse_stats` struct fields:

- **source_bytes** — input byte count
- **source_lines** — newline count + 1
- **total_tokens** — all leaf tokens (including trivia + ASI)
- **trivia_tokens** — whitespace and comment tokens
- **asi_injections** — synthetic `stmt_term` insertions
- **production_nodes** — non-leaf parse tree nodes
- **max_depth** — maximum tree depth during walk
- **token_by_kind** — frequency array indexed by `lex::token_kind` ordinal
- **production_by_name** — map of production name → count
- **error_count**, **warning_count**, **note_count** — diagnostic severity breakdown
- **timings** — `phase_timings` struct with `lex_and_parse`, `ast_build` (both `nanoseconds`), and `total`

When `collect_stats = false` (default), `result.stats` is an empty `std::optional` — zero overhead.

## Source Spans

```cpp
crank::source_span sp = crank::decode_span(src, offset, length);
// sp.line   — 1-based line number
// sp.col    — 1-based column number
// sp.offset — byte offset into src
// sp.length — byte length
```

Spans are stored externally in `vakya::property_store` keyed by structural hash (module 2). `source_span.hpp` also
provides `make_error()` for constructing `vakya::diag::diagnostic` records.

---

# Module 2: Semantics — Type System, Effects, Modules, Host Embedding

## Type System

### Primitives

Fixed-width types — canonical spelling is PascalCase. Lowercase aliases (`i8`…`i64`, `u8`…`u64`, `f32`, `f64`) are *
*lexical aliases** (identical types, not distinct): the compiler resolves them to the canonical name before
type-checking. No numeric difference; use either spelling in source, but public API documentation uses canonical names.

```
Int8  Int16  Int32  Int64    (aliases: i8  i16  i32  i64)
UInt8 UInt16 UInt32 UInt64   (aliases: u8  u16  u32  u64)
Float32  Float64             (aliases: f32 f64)
Bool  String  Unit
```

All are registered in the crank type band (stable_id 2000–2012) via `make_crank_type_registry()`.

### Result and Option

| Crank syntax   | C++ boundary type     | Vakya stable_id |
|----------------|-----------------------|-----------------|
| `Result[T, E]` | `std::expected<T, E>` | 17 (builtin)    |
| `Option[T]`    | `std::optional<T>`    | 16 (builtin)    |

`Result.Ok`, `Option.Some`, `Enum.Variant(...)` are **builtin enum-constructor syntax** resolved against their variant
registry, not value-namespace lookups.

### core.tx Types

Predeclared language types for transactional memory:

| Type                 | Crank kind    | C++ representation          |
|----------------------|---------------|-----------------------------|
| `TxStatus`           | enum          | `crank::TxStatus`           |
| `Isolation`          | enum          | `crank::Isolation`          |
| `ReplaySafety`       | enum          | `crank::ReplaySafety`       |
| `ConflictPolicy`     | enum          | `crank::ConflictPolicy`     |
| `PartialCommit`      | enum          | `crank::PartialCommit`      |
| `ProofStatus`        | enum          | `crank::ProofStatus`        |
| `TxError`            | opaque struct | `crank::TxError`            |
| `CommitReport`       | opaque struct | `crank::CommitReport`       |
| `TransactionOptions` | aggregate     | `crank::TransactionOptions` |

## Inference vs Public Boundary

- **Locals** (`let`/`var` within fn body): HM inference via `vakya::types::infer`.
  Numeric literals are *untyped constants* unified against their declared target type.
  Silent narrowing never occurs — a constant that does not fit its target is a diagnostic.
- **Exported declarations** (functions, types, constants visible outside the module):
    - All parameters and return types require explicit type annotations (except bare `self` receiver).
    - Missing annotation → diagnostic (`CRANK-TYPE-001`).
    - Generic parameters on exported items require explicit bounds.
- **Module-private declarations**: type inference is allowed; explicit annotation is optional.
  `type_check` (`typing_rule<Tag>`) validates exported signatures only.

### var zero-value rules

`var x: T` without an initializer is valid only when `T` has a zero-value.
`Result`, bare `enum`, and `fn` types **must** be initialized:

```crank
var x: Result[Int32, String]   // error: must initialise
var x: Result[Int32, String] = Result.Ok(0)  // ok
```

## Effect / Capability Model

Effect bits (`effect_mask`) and capability bits (`capability_mask`) are `uint64_t`.

### Builtin effects (bits 0–4)

| Mask constant           | Meaning     |
|-------------------------|-------------|
| `kEffectMaskFileSystem` | File I/O    |
| `kEffectMaskMemory`     | Memory ops  |
| `kEffectMaskIO`         | General I/O |
| `kEffectMaskNetwork`    | Network     |
| `kEffectMaskException`  | Exceptions  |

### Crank ext-band effects (bits 32–34)

| Mask constant             | Attribute   |
|---------------------------|-------------|
| `kEffectMaskHost`         | `@host`     |
| `kEffectMaskGpu`          | `@gpu`      |
| `kEffectMaskParallelSafe` | `@parallel` |

Registered via `make_crank_effects_registry()` / `make_crank_caps_registry()` in the
`extension` category (stable_id ≥ 1000). **No edits to Vakya built-in registries.**

### Attribute refinement

| Attribute   | Effect                                                               |
|-------------|----------------------------------------------------------------------|
| `@pure`     | Declares EffectMask = 0. Conflict with inferred effect → diagnostic. |
| `@io`       | Ensures IO bit in final_effects.                                     |
| `@net`      | Ensures Network bit.                                                 |
| `@host`     | Ensures Host effect + Host cap bit.                                  |
| `@reads`    | Ensures Read capability.                                             |
| `@writes`   | Ensures Write capability.                                            |
| `@gpu`      | Ensures GPU effect + GPU cap bit.                                    |
| `@parallel` | Ensures ParallelSafe effect + cap bit.                               |

## Module Resolver Order

```
1. native           (registered C++ modules — highest precedence)
2. embedded artifact (pre-compiled in-memory binary)
3. embedded src      (in-memory source text)
4. in-memory         (runtime-added source strings)
5. project paths     (searched in add_project_path order)
6. app paths         (add_app_path)
7. cache             (add_cache_path)
8. system            (if policy.allow_system_paths)
9. package registry  (if policy.allow_package_registry)
```

Import name → path mapping: `import "math.vector"` → `<base>/math/vector.crank`.
`module.crank` = package root. Content hash is FNV-1a 64-bit over source bytes — stable across identical source, drives
incremental rebuild and AOT keys.

After resolution, captured imports run through the `lang::import_graph` pipeline
(`context::resolve_imports` / `engine::load`): circular (`LANG-IMP-003`), version
(`LANG-IMP-004`) and capability (`LANG-IMP-005`) checks, then exported-symbol flow from
each importee into the importer. The dependency graph additionally exposes `cycle_nodes()`,
`find_dependents()` and `find_dependencies()` for diagnostics; `topo_order()` (importees
first) drives compile order.

## Host Embedding (no macros)

### Registering a function

```cpp
#include "languages/crank/context.hpp"

float dot(Vec3 a, Vec3 b) noexcept { return a.x*b.x + a.y*b.y + a.z*b.z; }

crank::context ctx;
ctx.register_function<"math.dot", dot>();
```

Non-capturing lambdas and free functions bind through a compile-time trampoline
(arity ≤ 8). Capturing callables use the explicit form:

```cpp
ctx.register_function("math.dot", 2, [capture](std::span<const std::any> args) -> std::any {
    // ...
});
```

### Registering a type

Specialise `crank::type_descriptor<T>` with a name and field list:

```cpp
template <>
struct crank::type_descriptor<Vec3> {
    static constexpr std::string_view name = "Vec3";
    static constexpr auto fields = std::tuple{
        crank::field<"x", &Vec3::x>{},
        crank::field<"y", &Vec3::y>{},
        crank::field<"z", &Vec3::z>{}
    };
};

ctx.register_type<Vec3>();
```

### Registering a container

```cpp
ctx.register_container<std::vector<float>>("float_vec");
```

Built-in specialisations: `std::span`, `std::vector`, `std::array`, `std::string`,
`std::string_view`, `std::optional` → `Option`, `std::expected` → `Result`, `std::mdspan`.
`std::mdspan` sets `is_gpu_visible = true` for Pravaha hetero routing (module 4).

## Semantic JSON Dumps

```cpp
#include "languages/crank/dump.hpp"

// Typed AST (tag + type_ref index + effect/cap masks per node)
std::string json = crank::dump_typed_ast(root, store, sema_ctx);

// Symbol table (module-qualified symbols)
std::string json = crank::dump_symbols(resolver_result.symbols);

// Module dependency graph
std::string json = crank::dump_module_graph(dep_graph);
```

All use glaze when available (`CRANK_HAS_GLAZE`), with a minimal fallback serializer.

---

# Module 3: Verification — Safety Obligations and Proof Discharge

## Obligation Families

Three implicit obligation families are emitted during semantic analysis.

| Family          | Trigger                           | Obligations emitted           |
|-----------------|-----------------------------------|-------------------------------|
| `bounds`        | `xs[i]` index access              | `0 <= i`, `i < len(xs)`       |
| `div_by_zero`   | Integer `/` or `%`                | `divisor != 0`                |
| `range_cast`    | Narrowing `as`                    | `value in [dst_min, dst_max]` |
| `parallel_safe` | `@parallel`, `@gpu`, SIMD regions | independence + effect-allowed |

A constant literal divisor of `0` is **immediately refuted** at compile time — no runtime path.

## Three-Way Discharge Outcome

Each obligation is discharged to one of three outcomes.

| Outcome   | Action                            |
|-----------|-----------------------------------|
| `proven`  | Guard dropped — zero runtime cost |
| `unknown` | Runtime guard inserted            |
| `refuted` | Compile error                     |

`deferred` (no SMT backend active) maps to `unknown` → guard inserted.

## `verify_policy` Modes

Set per-compilation-unit or per-function via `@on_safety_failure`.

| Policy     | Behaviour                                                                                     |
|------------|-----------------------------------------------------------------------------------------------|
| `off`      | Verification disabled; `assert`→always guard; `@verify(static)` is a compile error if present |
| `assume`   | Trust all contracts without discharge (fast dev, unsound)                                     |
| `check`    | Discharge via backend — **default**                                                           |
| `paranoid` | Discharge implicit + explicit; `@tarka.assume` overrides rejected                             |

Default backend: `no_smt_backend` — all obligations `deferred` → guards, zero SMT cost, no Tarka dependency required.

Opt-in backend: `tarka_smt_backend<tarka::backend::z3_backend>` (behind `__has_include(<tarka/tarka.hpp>)`).

## `safety_failure` Policy + `SafetyError`

Controls what a **failed runtime guard** does. Obligations that discharge to `proven` have **no guard** — `SafetyError`
is unreachable in fully-verified code.

```
enum class safety_failure { return_result, trap, terminate, host_handler };
```

| Policy          | Runtime behaviour                          | `defer` interaction       |
|-----------------|--------------------------------------------|---------------------------|
| `return_result` | Returns `Result.Err(E::from(SafetyError))` | Scope unwinds; defers run |
| `trap`          | Immediate process trap                     | Defers do **not** run     |
| `terminate`     | `std::terminate` equivalent                | Defers do **not** run     |
| `host_handler`  | Calls registered host callback             | Scope unwinds; defers run |

`SafetyError` is a POD struct — no heap allocation on the failure path:

```crank
struct SafetyError { kind: safety_kind, at: source_span }
```

`safety_kind` values: `BoundsViolation`, `DivByZero`, `RangeConversion`, `AssertFailed`, `OverflowChecked`, `TxFailed`.

### v1 Frontend Diagnostic Codes

Stable codes emitted by the resolve / type / effect / safety passes. Each code
is permanent — never reused for a second meaning. Produced by the `to_code`
mappings on `resolve_diagnostic`, `effect_diagnostic`, and `safety_diagnostic_kind`.

| Code             | Pass    | Meaning                                                                                                        |
|------------------|---------|----------------------------------------------------------------------------------------------------------------|
| `CRANK-RES-001`  | resolve | Uninitialized `var` of a no-zero-value type (`Result` / bare `enum` / `fn`)                                    |
| `CRANK-RES-002`  | resolve | Duplicate symbol in the same scope                                                                             |
| `CRANK-RES-003`  | resolve | Use of an undefined symbol                                                                                     |
| `CRANK-RES-004`  | resolve | Shadow warning (inner binding shadows an outer one) — not an error                                             |
| `CRANK-TYPE-001` | type    | Exported fn missing return-type annotation                                                                     |
| `CRANK-TYPE-002` | type    | Exported fn parameter missing type annotation                                                                  |
| `CRANK-EFF-001`  | effect  | `@pure` fn conflicts with an inferred effect                                                                   |
| `CRANK-EFF-002`  | effect  | `@io` fn missing the IO effect bit                                                                             |
| `CRANK-SAFE-001` | safety  | Non-`Result` fn with a live guard under `return_result` policy (see §"Non-`Result` fn + `return_result` Rule") |

### Policy Resolution Order

1. Function `@on_safety_failure(policy)` attribute
2. Module policy declaration
3. Context default (`trap`)

### Non-`Result` fn + `return_result` Rule

If a function:

- does **not** return `Result[T, E]`, **and**
- has `safety_failure::return_result` in effect, **and**
- has at least one live guard (unknown obligation after discharge)

→ **compile-time error**. Fix by: changing the return type, picking `trap`/`terminate`/`host_handler`, or strengthening
the contract to eliminate the live guard.

## Predicate Sublanguage

Predicates (`pred_expr`) appear in `requires`, `ensures`, and `assert` constructs. They must be **pure**
— effectful calls are rejected.

| Node kind                | Description                                           |
|--------------------------|-------------------------------------------------------|
| `literal_int/bool/float` | Numeric and boolean constants                         |
| `ident`                  | Variable reference                                    |
| `result`                 | Return value (valid inside `ensures` only)            |
| `old(expr)`              | Value at function entry (valid inside `ensures` only) |
| `len(expr)`              | Sequence length                                       |
| `arith`                  | `+`, `-`, `*`, `/`, `%`                               |
| `cmp`                    | `==`, `!=`, `<`, `<=`, `>`, `>=`                      |
| `logic`                  | `&&`, `\|\|`, `!`                                     |
| `->`                     | Implication (`!p \|\| q`)                             |
| `forall x: T, p`         | Universal quantifier                                  |
| `exists x: T, p`         | Existential quantifier                                |
| `call(...)`              | **Rejected** — effectful calls not allowed            |

Predicates are lowered to `vakya::types::proof_obligation` via `crank::predicate_lowerer`. On the no-SMT path, a
structural hash is used as the term payload. On the Tarka path, the hash is replaced with a `tarka::Term*`.

## Assumption Context Algorithm

The proof environment is a per-scope assumption stack. Assumptions are asserted into the SMT solver before each
obligation is discharged.

**Push rules:**

| Event                            | Assumption pushed   |
|----------------------------------|---------------------|
| Enter function with `requires p` | `p`                 |
| Proven `assert p`                | `p`                 |
| Bind `v: {x: T \| pred}`         | `pred[x:=v]`        |
| Enter `if cond` then-arm         | `cond`              |
| Enter `if cond` else-arm         | `!cond`             |
| Enter `for i in lo..hi` body     | `lo <= i`, `i < hi` |

**Worked example — `Scale` function:**

```crank
fn Scale(xs: []Float32, out: []Float32) requires len(xs) == len(out) {
    for i in 0..len(out) {
        out[i] = xs[i] * 2.0   // two bounds obligations each
    }
}
```

Under `check` policy:

1. `requires len(xs)==len(out)` is pushed as an assumption on entry.
2. `push_for_range` pushes `0 <= i` and `i < len(out)`.
3. `out[i]` emits `0 <= i` (discharged by for_range lower) and `i < len(out)` (discharged by for_range upper).
4. `xs[i]` emits `0 <= i` (discharged) and `i < len(xs)` — discharged by combining `i < len(out)` + `len(xs)==len(out)`.

Result: **all bounds obligations proven → zero guards in `Scale`**.

Without the `requires`, the `xs[i]` upper bound would be `unknown` → guard inserted.

## Refinement Types

Refinement type syntax: `{ x: T | pred }`.

```crank
fn SafeDiv(a: Int32, b: { d: Int32 | d != 0 }) -> Int32 {
    return a / b
}
```

- At every call site, `b != 0` must be proven (emits a `kRefineKind` obligation).
- Inside `SafeDiv`, `b != 0` is in the assumption context → the `a / b` div-by-zero obligation discharges `proven` → *
  *no guard**.

## `@verify(static) assert` vs `assert` Under Unknown Outcome

`proof` statement removed (lean charter P0). Use `@verify(static) assert p` to mandate static-only discharge.

| Construct                  | Outcome `unknown`      | Outcome `refuted` |
|----------------------------|------------------------|-------------------|
| `@verify(static) assert p` | **Compile error**      | Compile error     |
| `assert p`                 | Runtime guard inserted | Compile error     |

## Verification JSON Dumps

Enable with `opts.dump = { obligations | assumptions | guards }`.

**`dump_obligations`** — each obligation:

```json
{
  "label": "i < len(xs)",
  "family": "bounds",
  "outcome": "proven",
  "guard": false,
  "line": 5
}
```

**`dump_assumptions`** — active proof environment at a point:

```json
{
  "kind": "requires",
  "description": "len(xs)==len(out)",
  "line": 1
}
```

**`dump_guards`** — obligations that resulted in runtime guards:

```json
{
  "label": "b != 0",
  "safety_policy": "trap",
  "line": 12
}
```

---

# Module 4: Execution — Lowering, Planning, AOT Cache

## Optimization Profiles

| Profile             | Inherits                 | Bundle     | Id           |
|---------------------|--------------------------|------------|--------------|
| `crank::o0_profile` | `lithe::profile::std_o0` | O0 (empty) | `"crank.o0"` |
| `crank::o1_profile` | `lithe::profile::std_o1` | O1 passes  | `"crank.o1"` |
| `crank::o2_profile` | `lithe::profile::std_o2` | O2 passes  | `"crank.o2"` |
| `crank::o3_profile` | `lithe::profile::std_o3` | O3 passes  | `"crank.o3"` |

All profiles use `profile_inherit<std_oN, crank_extra_bundle, desc>`. The `crank_extra_bundle` is currently empty;
extend it with crank-specific passes without editing `std_*`.

```cpp
static_assert(lithe::profile::profile_valid<crank::o3_profile>());
auto result = crank::o3_profile{}(expr);
```

## HL MIR Lowering

`lower_to_hl(lower_input)` → `lower_hl_result`

Input: per-function descriptor — scalar SSA, loop bounds, tensors, defer sites, exit edges, safety policy.

| Step                                  | Output                                                       |
|---------------------------------------|--------------------------------------------------------------|
| Loop → `structured_for`               | `is_parallel` flag from `loop_bounds_info`                   |
| Scalar SSA → `constant` / `add` / `sub` / `mul` / `div` | Explicit operands/results; optional scalar `ret` |
| Tensor → `memref_load`/`memref_store` | Row-major `memref_type` derived from `tensor_info`           |
| Defer sites → `crank_defer_list`      | Per-block LIFO cleanup list                                  |
| Exit edges → `crank_exit_edge`        | `controlled` edges carry LIFO defer list; `trap` edges don't |

### `defer` Semantics

- Arguments evaluated at the `defer` statement (snapshot).
- Cleanup applied in **LIFO** order on every **controlled** exit edge (fall-off / `return` / `break` / `continue` /
  guard failure).
- `trap` / `terminate` exit edges → defers do **not** run (design §4.5, safety.hpp §7b.4).
- No unwinder, no landing pads — GPU/AOT-compatible.

## Scalar Path + Interpreter

```
lower_to_hl → lower_hl_result
    └─ coordinate_lowering_pass → physical_mir_function
           └─ interpreter_backend::emit()
```

**`execute_via_interpreter(hl_res, args, opts)`** — always-available path, zero external deps.

**Value-carrying subset.** `lower_input::scalar` is the current executable
integer SSA contract: constants and `add`/`sub`/`mul`/`div` retain explicit
operands, results, and an optional return value through physical MIR. A rank-1
integer reduction carries one accumulator through structured-loop block
arguments and returns its final value through `region_yield`. Nested and
multi-accumulator reductions remain pending. Device reduction lowering is a
separate workgroup ABI and currently falls back to CPU execution.

**`execute_with_auto_fallback(hl_res, args, opts)`** — capability-aware; tries `opts.primary_backend_name`, falls back
to interpreter + diagnostic trace.

**`execute_physical_native(phys, args, opts)`** — native JIT entry (asmjit primary / interpreter fallback). Correct for
both straight-line and CFG functions — native follows branches so counted loops return a scalar value.
`fallback_fired=true` when asmjit unavailable.

`execute_options::execution_path` now supports four modes:

| Path               | Behavior                                                                                           |
|--------------------|----------------------------------------------------------------------------------------------------|
| `auto_select`      | Heuristic route: native for CFG/branch-heavy or larger MIR; interpreter for tiny straight-line MIR |
| `jit_preferred`    | Prefer native JIT for this call; safe interpreter fallback if unavailable                          |
| `interpreter_only` | Force interpreter                                                                                  |
| `native_only`      | Force native compile+invoke path                                                                   |

Native execution also reuses a digest-keyed in-process compile cache for repeated calls of equivalent physical MIR,
reducing repeated JIT compile overhead in hot execute-only loops.

**`execute_planned(hl_res, args, opts, hints)`** — single unified entry (L-1 W1). Resurrects the crank planner:
`construct_plan` ranks backends from annotations + cost model; `execute_plan` drives a lithe-native `run` closure (
`lithe::execution::compile` → `invoke`). Interpreter enters only as the planner's scalar fallback candidate. CFG
functions (counted loops) return a scalar via the native path.

```
execute_planned:
  lower_to_physical (cached)
    ↓
  construct_plan(verified_mir, opts, hints)  ← @simd/@gpu/@parallel attrs fed directly
    ↓
  execute_plan → run_closure per candidate:
      lithe::execution::compile(phys, req)   ← hint overridden per candidate kind
      lithe::execution::invoke(cr, args)
    ↓ on failure
  interpreter fallback (scalar candidate or construct_plan failure)
```

> **Interpreter control flow:** `interpreter_backend` executes control-flow graphs, including `branch` and
> `branch_cond`. A scalar result is produced only when the lowered MIR carries a value to `ret`; the current
> `scalar_program` path does so for straight-line integer expressions. Structured loops currently model control flow
> only; loop-carried results wait for structured-region arguments/yields. Opcodes requiring host linkage — `call` into
> host functions and `load_symbol` — route to a backend or host boundary.

`crank_execute_result::status` reports the outcome. The legacy `execution_status::unsupported_control_flow` enumerator
is retained for source compatibility but is no longer produced by CFG functions:

```cpp
auto res = crank::execute_via_interpreter(hl, {}, {});
if (res.ok()) {
    // res.return_value holds the scalar result, including loop/branch functions
}
```

### Integer Overflow

| Mode                           | Behaviour                                       |
|--------------------------------|-------------------------------------------------|
| Default                        | Wrapping two's-complement                       |
| `opts.overflow_checked = true` | Trap on overflow — maps `safety_failure` policy |

### Safety Guard Failure

Runtime bounds guards that fire follow the function's `safety_failure` policy (`execute_options::safety_policy`).
Default: `trap`.

## Automatic Execution Planning

### Attribute → `execution_hint` mapping table

| Annotation                 | `preferred` | `required` | `deterministic` |
|----------------------------|-------------|------------|-----------------|
| `@parallel`                | `threaded`  | false      | false           |
| `@parallel(required=true)` | `threaded`  | **true**   | false           |
| `@simd`                    | `simd`      | false      | false           |
| `@gpu`                     | `gpu`       | false      | false           |
| `@gpu(preference=strong)`  | `gpu`       | false      | false           |
| `@gpu(required=true)`      | `gpu`       | **true**   | false           |
| `@gpu(deterministic=true)` | `gpu`       | false      | **true**        |

Valid attribute arguments: `required`, `preference`, `deterministic`. Unknown arg names → `validate_exec_attr()` error.

### Hard vs soft

- `required=true` + legality check fails → `hard_requirement_unmet_diagnostic()` (CRANK-E-EXEC-001).
- `preference=strong` + backend unavailable → fallback + `soft_fallback_note()` (CRANK-I-EXEC-002).
- Annotations **never rewrite IR** (§5b.1 / §4.6a.5).

G-LIT-4 note: Hints are currently attached as crank region metadata and bias Lithe's candidate ranking via the existing
hint fields. The `execution_preference` enum is crank-local until G-LIT-4 (a) lands in the framework.

**Wiring (perf-L1 C-1):** `@parallel/@simd/@gpu` attributes are parsed to `crank_exec_attr` list, merged via
`merge_exec_hints`, and fed directly to `construct_plan(..., hints)` when calling `execute_planned`. The planner
consumes them natively — no grammar change required. Force policy: `required=true` + unavailable backend →
`hard_requirement_unmet_diagnostic` (CRANK-E-EXEC-001); soft `preference=strong` unmet → `soft_fallback_note` (
CRANK-I-EXEC-002) + interpreter fallback.

## Pravaha Extraction + `spawn`/`await`

### Parallel constructs → Pravaha DSL

| Crank construct   | Pravaha mapping                               |
|-------------------|-----------------------------------------------|
| `@parallel for`   | `lazy_parallel_for` via `plan_to_pravaha_for` |
| `parallel.map`    | `lazy_parallel_transform`                     |
| `parallel.reduce` | `lazy_parallel_reduce` + reduction contract   |
| `spawn call(...)` | `crank_future<T>` + `TaskExpr` wrapper        |
| `await f`         | `seq(a, b)` dependency edge                   |

### Reduction contract

Legal ops: `add`, `mul`, `min`, `max`, `and_`, `or_`, `xor_` (all associative + commutative).

`validate_reduction_op(op)` returns an error string for illegal ops; only legal ops reach Pravaha.

### `spawn`/`await` ownership rules

- Capture is **by value** (Copy-types copied; others moved).
- Mutable-ref capture → `crank_future_error::mutable_ref_capture` compile diagnostic.
- **Must-consume**: dropping a `crank_future` without `await()` or `detach()` → `dropped_without_consume` diagnostic.
- `await` is legal inside parallel regions.

### `crank_future<T>` and `future.hpp`

`crank_future<T>` (`future.hpp`) is the non-copyable, movable handle to a spawned task.

**Language-level model:** `spawn expr` produces a value of type `Future[T]`, where `T` is the return type of the spawned
expression. `Future[T]` is not `Future[Result[T, E]]` — errors surface only if the task panics (maps to
`crank_future_error::task_panicked`) or is dropped (maps to `dropped_without_consume`). If you want typed error
propagation, the spawned body must return `Result[T, E]` explicitly, giving `Future[Result[T, E]]`.

**Lowering:** `Future[T]` lowers to `crank_future<T>`. `await f` lowers to `crank::await(std::move(f))` which returns
`std::expected<T, crank_future_error>`.

**`await` in a `Result`-returning function:**

```crank
fn fetch() -> Result[Int64, TxError] {
    let f = spawn compute();         // Future[Int64]
    let v = await f;                 // expected<Int64, crank_future_error>
    // Map future error to Result error at the language boundary:
    match v {
        Ok(x)  => Result.Ok(x),
        Err(_) => Result.Err(TxError.from_future_panic()),
    }
}
```

`await` is allowed inside `Result`-returning functions. The `crank_future_error` does not auto-coerce to the function's
error type `E` — the programmer must explicitly convert, typically in a `match` or via a `From` impl.

```cpp
// Spawn a task
auto f = crank::spawn([]{ return int64_t{42}; });

// Consume by awaiting (returns expected<T, crank_future_error>)
auto result = crank::await(std::move(f));

// Or detach (suppresses drop diagnostic)
crank::detach(std::move(f));
```

`crank_future_error` values:

| Variant                   | Meaning                                        |
|---------------------------|------------------------------------------------|
| `cancelled`               | Task was cancelled before completion           |
| `task_panicked`           | Task body threw or trapped                     |
| `dropped_without_consume` | Future dropped without `await()` or `detach()` |

### Backends

| Backend            | Usage                                           |
|--------------------|-------------------------------------------------|
| `InlineBackend`    | Single-threaded, synchronous; default for tests |
| `JThreadBackend`   | Multi-threaded `std::jthread` pool              |
| `CoroutineBackend` | Planned                                         |

Hetero priority: **Metal > Vulkan > Host SIMD** (§6.3). Every backend fallback emits a NADI pulse.

### Plan adapter

`task_decomposition_plan → Pravaha DSL` mapping lives in `crank::parallel.hpp`.
`plan_to_pravaha_for(plan, body, chunk)` builds a `lazy_parallel_for` expression from a plan.

### Compiler-phase acceleration (§M.parallel)

`engine.hpp` accelerates **compiler phases** (parsing, batch module loading) using the
same Pravaha + Kosha stack. These types live in `engine.hpp` (after `frontend::parse` and
`crank::context` are fully defined) rather than `parallel.hpp` — the include chain requires
`frontend::parse_result` to be visible, which it is only after `frontend.hpp` completes.

#### `module_parse_info` + `module_parse_cache`

`module_parse_info{package_name, imports, parse_ok}` captures the lightweight metadata
a parse produces (package clause + import list). `module_parse_cache<S>` is a
`kosha::ShardedLRUCache<content_hash, module_parse_info, S>` (thread-safe, S shards,
default S=8). On cache hit the full `frontend::parse` is skipped — unchanged modules cost
only a hash lookup on incremental rebuilds.

#### `parse_modules_parallel`

```cpp
crank::module_parse_cache<> cache{1024};
std::vector<std::string> names = {"math.core", "math.vector", "app.main"};
auto source_provider = [](std::string_view name) -> std::optional<std::string> {
    // return source text for name, or nullopt
};
auto infos = crank::parse_modules_parallel(names, source_provider, &cache);
// infos[i] = { package_name, imports, parse_ok } for names[i]
```

Parses N independent modules concurrently via `pravaha::lazy_parallel_for` +
`JThreadBackend` (work-stealing). Serial fallback when `n <= serial_threshold` (default 1)
avoids overhead for single-module workloads.

#### `batch_load_modules`

```cpp
auto order = ctx.dep_graph().topo_order(); // importees first
auto result = crank::batch_load_modules(order, source_provider, ctx, &cache);
// result.entries: each entry has .name, .ok, .imports, .error_msg
// dep graph is updated with add_import for each successfully parsed module.
```

Groups modules into **dependency waves** (modules whose importees are all already done in
the batch), then parses each wave in parallel. Preserves compile order across wave
boundaries. Seeded module dep-graph edges (`ctx.dep_graph().add_import`) after each wave.

## AOT Cache

### Cache key fields

`crank_aot_key` contains: module name, source hash, dependency hashes, compiler version, target triple, opt profile id,
backend id, enabled features, native ABI hash, type/fn/container descriptor hashes.

`fingerprint()` — FNV-1a over all fields in stable order. Any change → recompile.

G-LIT-3 note: Key is currently assembled in `crank::` (fallback path). Swaps to `lithe::aot_cache_key` when landed.

### Artifact levels

Source → Vakya/Lithe graph → HL MIR → physical MIR → backend artifact → AOT.

> **AOT artifact vs native executable:** An AOT artifact stored in the cache is a *verified physical MIR binary* — not a
> native executable. It has been type-checked, obligation-verified, and serialized. Generating a native executable (e.g.
> LLVM IR → machine code) requires an additional backend compilation step beyond what the current AOT cache provides.

### `compile_and_cache(cache, key, hl_res, args, opts)`

An AOT artifact is a *compilation product* (lowered + verified physical MIR), then compiled to native code via
`lithe::execution::compile`.

1. Fingerprint the key.
2. Cache hit → return stored bytes.
3. Miss → lower HL→physical MIR → `verify_physical_mir`. Fatal on lower/verify failure (`diagnostics`, `!ok()`).
4. Call `lithe::execution::compile(phys, req)` — selects the best available backend (asmjit for hot CPU code,
   interpreter as fallback). The live `jit_function_handle` is in the returned `compile_result`; a scalar is obtained
   via `lithe::execution::invoke`.
5. Store 8-byte backward-compat bytes in `aot_cache`; the live native handle is available via the `compile_result`before
   storage.

> **Performance note.** When asmjit is available (`LITHE_HAS_ASMJIT`), `compile_and_cache` now routes through the native
> JIT path. CFG functions (counted loops such as `sum`/`harmonic`) return a correct scalar via the native path. On the
> interpreter-only fallback, CFG functions still return a value because the interpreter follows branches. See
`docs/lithe/lithe.md#execution-model` for the execution-tier breakdown (322× → ~1× speedup expected for hot counted
> loops).

`aot_cache_result.diagnostics` holds fatal compile diagnostics only; `aot_cache_result.notes` holds non-fatal runtime
notes. `aot_cache_result.fallback_fired = true` when the interpreter ran instead of native JIT. `ok()` reflects
`diagnostics` only.

The key's `backend_id` (e.g. `"lithe.backend.interpreter"`) is descriptive for the fingerprint; it is normalized to the
bare registry name (`"interpreter"`) before backend dispatch.

Validation: `validate_aot_view` verifies the FNV-1a checksum. The optional signature envelope is **write-only today**:
`module_link_metadata::serialize()` emits the footer, but there is no deserialize/parse path — `validate_aot_view`
computes the signature hash and discards it, and the `aot_signature_provider` / trust-level checks below are policy
placeholders, not enforced authentication.

## AOT Security Policy

`aot_security_policy` (`aot.hpp`) controls artifact loading strictness:

```cpp
enum class aot_trust_level : uint8_t { internal, trusted_host, untrusted };
enum class aot_signature_algorithm : uint8_t { none, ed25519, ecdsa_p256 };

struct aot_security_policy {
    aot_trust_level            trust              = aot_trust_level::internal;
    aot_signature_algorithm    sig_algorithm      = aot_signature_algorithm::none;
    optional<vector<byte>>     public_key;         // Ed25519: 32 B; ECDSA P-256: 65 B
    string                     key_identity;       // e.g. "org.example.key-2025-01"
    vector<string>             allowed_backends;   // empty = all allowed
    vector<string>             allowed_capabilities; // empty = all allowed (untrusted only)
    uint64_t                   max_artifact_bytes = 0;  // 0 = unlimited
    bool                       require_abi_match  = true;
    bool                       require_target_match = true;
    bool                       allow_executable_memory = false;
};
```

**Trust level semantics:**

| Level          | Checks performed                                                                                          |
|----------------|-----------------------------------------------------------------------------------------------------------|
| `internal`     | FNV-1a integrity only (corruption detection, not authentication)                                          |
| `trusted_host` | FNV-1a + ABI match + backend allowlist                                                                    |
| `untrusted`    | All of the above + mandatory `public_key`/`sig_algorithm` + capability allowlist + executable-memory gate |

**FNV-1a is an integrity/checksum mechanism, not authentication.** It detects accidental corruption or truncation. A
malicious actor can compute a matching FNV-1a hash. For artifact authentication (proving origin), use `trust=untrusted`
with Ed25519 or ECDSA P-256.

**`untrusted` mandatory requirements** — all four must hold or `CRANK-AOT-SEC-001`/`CRANK-AOT-SEC-002` fires:

```
sig_algorithm != none
public_key    present (Ed25519: 32 bytes; ECDSA P-256: 65 bytes uncompressed)
signature     present in artifact footer (length-prefixed signature envelope, below)
signature     verification succeeds over the signed payload
```

**Signature envelope (footer).** The signature is variable-length, so it is stored length-prefixed rather than at a
fixed offset:

```
signature footer = u16 sig_len (little-endian) || sig_bytes[sig_len]
    Ed25519:     sig_len = 64, raw RFC 8032 signature
    ECDSA P-256: sig_len = DER length of SEQUENCE { INTEGER r, INTEGER s }
                 (70–72 bytes typical), no padding
```

The 2-byte length prefix immediately precedes the signature bytes and is the last `2 + sig_len` bytes of the artifact.

**Signed payload definition** — the bytes covered by the signature are the artifact header + body, **excluding** the
trailing signature envelope. Specifically: `bytes[0 .. artifact.size() - (2 + sig_len)]`, where `sig_len` is read from
the u16 footer prefix. Implementations must use this range exactly; signing or verifying a different byte range is a
conformance defect and will produce `CRANK-AOT-SEC-002`.

**Signature encoding:**

- **Ed25519**: raw 64-byte RFC 8032 signature (`sig_len = 64`).
- **ECDSA P-256**: DER-encoded `SEQUENCE { INTEGER r, INTEGER s }`, no padding; `sig_len` is its actual DER length (
  70–72 typical). The length prefix, not a fixed offset, delimits it.

**Key rotation:** `key_identity` is an audit string (e.g. `"org.example.signing-key-2025-01"`). Rotate keys by deploying
a new `aot_security_policy` with updated `public_key` and `key_identity`. Old artifacts signed with the previous key
will fail `CRANK-AOT-SEC-002` and must be recompiled.

`validate_aot_view(key, artifact_bytes, policy)` → `vector<string>` diagnostics:

| Code                | Condition                                            |
|---------------------|------------------------------------------------------|
| `CRANK-AOT-SEC-001` | `untrusted` without `public_key` / `sig_algorithm`   |
| `CRANK-AOT-SEC-002` | Signature verification failed                        |
| `CRANK-AOT-SEC-003` | Backend not in `allowed_backends`                    |
| `CRANK-AOT-SEC-004` | `artifact_bytes.size() > max_artifact_bytes`         |
| `CRANK-AOT-SEC-005` | Artifact requests executable memory (policy forbids) |
| `CRANK-AOT-SEC-006` | Artifact requests disallowed capability              |
| `CRANK-AOT-SEC-007` | ABI or target triple mismatch                        |

`internal` trust (default) skips signature and allowlist checks. Use `untrusted` for any third-party artifact loaded at
runtime.

## JSON Dumps

| Function                                       | Emits                                              |
|------------------------------------------------|----------------------------------------------------|
| `dump_hl_mir(lower_hl_result)`                 | `structured_for` ops, bounds, parallel flag, stats |
| `dump_physical_mir(exec_result, fn_name)`      | `instr_count`, `lower_ns`                          |
| `dump_execution_plan(exec_result, opts, hint)` | backend, fallback_fired, hint_kind                 |
| `dump_task_plan(parallel_plan_result)`         | rank, chunk, bounds per plan                       |
| `dump_aot_key(crank_aot_key)`                  | all key fields + fingerprint hex                   |

All outputs are round-trippable JSON (glaze when available, manual fallback otherwise).

`opts.dump` flags: `hl_mir | physical_mir | exec_plan | task_plan | aot_key`.

## Execution Policy

### Policy enums

```cpp
enum class scheduler_policy : uint8_t { fifo, priority, work_stealing, critical_path, locality, gpu };
enum class fallback_policy  : uint8_t { safe_cpu, none };
enum class backend_policy   : uint8_t { best_available, inline_only, threaded_only };
```

`to_string(p)` overloads for each. Defaults: `work_stealing`, `safe_cpu`, `best_available`.

### `execution_options` fields

| Field               | Type               | Default          | §ref |
|---------------------|--------------------|------------------|------|
| `use_pravaha`       | `bool`             | `false`          | §6.2 |
| `allow_simd`        | `bool`             | `true`           | §6.3 |
| `allow_gpu`         | `bool`             | `false`          | §6.3 |
| `allow_threads`     | `bool`             | `true`           | §6.3 |
| `allow_async`       | `bool`             | `true`           | §6.3 |
| `allow_distributed` | `bool`             | `false`          | §6.3 |
| `scheduler`         | `scheduler_policy` | `work_stealing`  | §9.4 |
| `fallback`          | `fallback_policy`  | `safe_cpu`       | §6.3 |
| `backend`           | `backend_policy`   | `best_available` | §6.3 |

### Fluent `execution_config` usage

```cpp
ctx.execution()
   .use_pravaha()
   .scheduler(crank::scheduler_policy::work_stealing)
   .fallback(crank::fallback_policy::safe_cpu)
   .allow_gpu();
```

### `map_scheduler` / `map_fallback`

`map_scheduler(p)` → `scheduler_mapping{backend_hint, scheduler_hint}`:

| `scheduler_policy` | `backend_hint`   | `scheduler_hint`              |
|--------------------|------------------|-------------------------------|
| `work_stealing`    | `JThreadBackend` | `work_stealing`               |
| `fifo`             | `InlineBackend`  | `fifo`                        |
| `priority`         | `InlineBackend`  | `priority`                    |
| `critical_path`    | `JThreadBackend` | `critical_path`               |
| `locality`         | `JThreadBackend` | `locality`                    |
| `gpu`              | `HeteroBackend`  | `gpu` (Metal > Vulkan > SIMD) |

`map_fallback(p, ctx_name)` → NADI-pulse note string on `safe_cpu`; empty string for `none`. Every fallback must emit a
NADI pulse (no silent degradation, §6.3).

## Transaction Runtime Lowering

Bridges the compile-time `tx_lowering_result` (from `transaction.hpp`) with the Medha runtime. Sits between HL MIR (step

10) and planning (step 11) in the §10.2 pipeline.

### `execute_transaction` — interpreter path

```cpp
tx_runtime_result execute_transaction(
    const tx_lowering_result& lowered,
    const CrankTransactionOptions& opts = {},
    const tx_evaluator& eval = {});
```

Algorithm:

1. Gate on `!lowered.ok()` — surface compile diagnostics, return early.
2. `opts.to_medha()` → `medha::options`.
3. `medha::atomic(medha_opts, body)` — body replays `lowered.reads`/`writes` via `eval.on_read`/`on_write`.
4. `committed` status → `CrankCommitReport`; set `committed = true`.
5. `partial_commit` / `in_doubt` are **never** `committed`.

`tx_evaluator` — host-supplied typed value resolution sink:

```cpp
struct tx_evaluator {
    // Returns the value read, or CrankTxError to abort the transaction.
    std::function<std::expected<crank_value, CrankTxError>(const transaction_read_op&)>  on_read;
    // Returns success (void) or CrankTxError to abort.
    std::function<std::expected<void,        CrankTxError>(const transaction_write_op&)> on_write;
};
```

> **Note:** `std::function` in `tx_evaluator` callbacks is acceptable at host/embedding boundaries where the overhead of
> type erasure is the expected cost. It is not the zero-overhead execution path — hot inner loops should use templated
> alternatives.

`crank_value` (`crank_value.hpp`) is the typed host value carrier:

```cpp
struct crank_value {
    enum class ownership : uint8_t { owned, borrowed, staged };
    std::any  payload;
    ownership own = ownership::owned;
    template <class T> static crank_value from(T&& v, ownership o = ownership::owned);
    template <class T> std::expected<T, std::string> to() const;
    bool has_value() const noexcept;
};
```

> **Performance contract:** `std::any payload` is **not** zero-overhead. `crank_value` is an explicit host/plugin
> boundary type; its type erasure cost is the expected price at that boundary.
> - Core compiled Crank transactions use typed/generated adapters (module 4 physical MIR lowering path). `crank_value`is
    not on that path.
> - Dynamic `crank_value` transaction values are **not permitted in GPU/AOT hot paths** unless serialized through an
    approved typed adapter.
> - Use `crank_value` only at host embedding boundaries (evaluator callbacks, plugin returns, debug tooling). Do not
    propagate it through inner execution loops.

### `lower_transaction_aot` — AOT path

```cpp
tx_aot_lowering lower_transaction_aot(const tx_lowering_result& lowered, crank_aot_key& key);
```

Algorithm:

1. Assemble `medha::dsl::plan` from `lowered.reads`/`writes`/`options`.
2. `medha::adapters::lithe::lower(plan)` → `lithe_region_descriptor` + `lithe_transaction_metadata`.
3. Fold `region.metadata.resource_hashes` into `key.descriptor_hashes`.
4. Stamp dialect version into `key.enabled_features` bits `[63:48]`.

Falls back to metadata-only lowering when Lithe headers unavailable (`region.has_lithe == false`); a note is recorded (
`CRANK-TX-AOT-NOTE-001`). Correct per §17.1.

### `execute_with_transactions` — dispatch wrapper

Zero cost when `tx_regions` is empty — scalar path unchanged:

```cpp
crank_execute_result execute_with_transactions(
    const lower_hl_result& hl_res,
    std::span<const tx_lowering_result> tx_regions,
    tx_context_provider& provider,
    const execute_options& opts = {},
    const CrankTransactionOptions& tx_opts = {});
```

### `tx_runtime_result`

```cpp
struct tx_runtime_result {
    std::optional<CrankCommitReport> report;
    std::vector<std::string>         diagnostics;
    std::vector<std::string>         notes;
    bool                             committed = false;
    bool ok() const noexcept;
};
```

## Transaction State Machine (`transaction_state`, §3.1)

```cpp
enum class transaction_state : std::uint8_t {
    created, active, validating, preparing, prepared,
    committing, committed,   // terminal
    aborting,  aborted,      // terminal
    in_doubt
};

bool is_terminal(transaction_state s) noexcept;  // true for committed/aborted
std::string_view to_string(transaction_state s) noexcept;
```

Valid forward transitions: `created → active → validating → preparing → prepared → committing → committed`. Any
non-terminal state may transition to `aborting → aborted`. After process interruption in `prepared`/`committing`, the
coordinator may enter `in_doubt`.

## Transaction Runtime Context (`transaction_context`, §3.2)

```cpp
struct transaction_context {
    uint64_t      id;               // stable identity across retries
    transaction_state state;
    medha::isolation  isolation;
    medha::replay_safety replay;
    uint32_t      attempt;          // 0-based; increments on each retry
    uint64_t      snapshot_version;
    uint32_t      read_count;
    uint32_t      write_count;
    uint32_t      savepoint_depth;
    bool          has_deadline;
    bool          cancelled;
};
```

Reset between retry attempts: `attempt` increments, `snapshot_version` refreshes, read/write counts clear.

## Typed Error Discriminant (`TxErrorKind`, §5.1)

```cpp
enum class TxErrorKind : uint8_t {
    Conflict, ValidationFailed, SnapshotUnavailable, ResourceNotTransactional,
    StagingUnsupported, RollbackUnsupported, RollbackFailed, CommitFailed,
    PrepareFailed, CoordinatorUnavailable, CoordinatorRejected,
    DeadlineExceeded, Cancelled, ReplayUnsafe, SerializationFailure,
    PartialCommit, InDoubt, HostFailure, InternalInvariant
};

std::string_view to_string(TxErrorKind k) noexcept;
TxErrorKind tx_error_kind_from_status(medha::tx_status) noexcept;
```

`CrankTxError` now carries a `kind` field (inferred from `medha::tx_status` via `tx_error_kind_from_status`), plus
optional `resource`, `key`, `retryable`, and `at` (source span). Crank owns copied error strings — no `string_view` from
Medha escapes lifetime boundaries.

## Retry Policy (`retry_policy`, `backoff_kind`, §10.3)

```cpp
enum class backoff_kind : uint8_t { none, linear, exponential, constant };

struct retry_policy {
    uint32_t          max_attempts;
    nanoseconds       initial_delay;
    nanoseconds       maximum_delay;
    backoff_kind      kind;
    bool              jitter;   // bounded random jitter; disable for deterministic tests

    nanoseconds delay_for(uint32_t attempt) const noexcept;  // §10.3 formula
    variant<retry::none, retry::bounded> to_medha() const noexcept;
};
```

Exponential backoff: `delay = min(maximum_delay, initial_delay × 2^attempt)`.
`to_medha()` converts attempts to the medha retry count (max_attempts − 1 = retries).

## Isolation Levels (§8)

Three levels are now defined in `medha::isolation`:

| Level            | Guarantees                                                  | May allow                                         |
|------------------|-------------------------------------------------------------|---------------------------------------------------|
| `read_committed` | No dirty reads                                              | Non-repeatable reads, phantoms, write skew        |
| `snapshot`       | Consistent snapshot, repeatable reads, write/write conflict | Write skew across independently read keys         |
| `serializable`   | Equivalent serial order                                     | Nothing — requires coordinator for multi-resource |

The compiler/runtime must reject an isolation level a participant cannot support. Silent weakening is forbidden.

## Resource Capability Checker (§6.3)

```cpp
struct resource_capability_spec {
    bool has_write;  bool has_range_read;  bool has_serializable_range;
    bool has_old_expr;  bool has_savepoint;  bool needs_coordinator;
    // per-resource trait values:
    bool transactional;  bool resource_stages_values;  bool supports_rollback_trait;
    bool supports_snapshot_trait;  bool supports_range_reads_trait;
    bool supports_predicate_validation_trait;
    bool supports_savepoints_trait;  bool supports_prepare_trait;  bool supports_recovery_trait;
};

class resource_capability_checker {
public:
    resource_capability_check_result check(const resource_capability_spec&, source_span) const;
};

// Build spec from medha::resource_traits<R> (handles missing fields via if constexpr)
template <class R>
constexpr resource_capability_spec make_resource_capability_spec() noexcept;
```

Produces stable `CRANK-TX-*` diagnostics before lowering. A savepoint on a resource without `supports_savepoints` is a
note (not error) — the crank journal emulates partial rollback.

New `medha::resource_traits<R>` fields (primary template defaults to `false`):
`supports_savepoints`, `supports_prepare`, `supports_recovery`, `supports_range_reads`, `supports_predicate_validation`.

## Full Commit Report (`CrankCommitReport`, §15.1)

```cpp
struct CrankCommitReport {
    medha::commit_report inner;         // status, attempts, reads, writes, conflicts

    // Timing
    steady_clock::time_point started_at;
    steady_clock::time_point committed_at;
    uint64_t duration_ns() const noexcept;  // 0 if timing not set

    // Extended accounting
    uint32_t resources_read;   uint32_t resources_written;
    uint64_t keys_read;        uint64_t keys_written;

    string           coordinator;   // empty = no coordinator
    proof_status     proof_status;  // from Tarka
    uint64_t         trace_id;

    bool is_committed() const noexcept;  // true iff status == committed
    // Delegating accessors: status(), attempts(), reads(), writes(), conflicts()
};
```

**Invariant (§15.2):** `is_committed()` is `true` only after the commit decision is final, required participants have
accepted, the requested durability point is reached, and externally visible staged state is published.

## Observability (`transaction_event_kind`, §16.1)

```cpp
enum class transaction_event_kind : uint8_t {
    started, read, write_staged, validation_started, conflict_detected,
    retry_scheduled, prepare_started, participant_prepared, commit_decided,
    participant_committed, committed, rollback_started, rolled_back,
    cancelled, deadline_exceeded, in_doubt, recovered
};

struct transaction_event {
    transaction_event_kind kind;
    uint64_t transaction_id;  uint32_t attempt;  transaction_state state;
    string   resource;        string   error_category;
    uint64_t duration_ns;     uint64_t trace_id;
};
```

Events carry no resource values by default (pay-for-use, §16.2). `resource` and `error_category` are name identifiers,
never raw values.

## WAL Record Kinds (`log_record_kind`, §14.2)

```cpp
enum class log_record_kind : uint8_t {
    begin, participant_prepared, commit_decision, abort_decision,
    participant_committed, participant_aborted, complete
};
```

Used by the coordinator durability layer. Each record includes: transaction ID, attempt number, participant ID, sequence
number, checksum, protocol version, timestamp, and decision data.

## Transaction Participant Concept + 2PC (`TransactionParticipant`, §13.2–§13.4)

```cpp
template <class P>
concept TransactionParticipant = requires(P& p, uint64_t tx_id, uint32_t write_count) {
    { p.id()                        } -> convertible_to<uint64_t>;
    { p.prepare(tx_id, write_count) } -> convertible_to<bool>;
    { p.commit(tx_id)               } -> convertible_to<bool>;
    { p.rollback(tx_id)             } -> convertible_to<bool>;
};
```

Required properties: `prepare`/`commit`/`rollback` are idempotent; a prepared participant preserves state across
restart; a committed participant never later reports aborted.

```cpp
class crank_coordinator_2pc {
public:
    template <TransactionParticipant P>
    crank_coordinator_2pc& enlist(P& p);

    crank_2pc_result coordinate(uint64_t tx_id, uint32_t write_count = 0);
};

enum class crank_2pc_outcome : uint8_t { committed, aborted, in_doubt };
```

**Phase 1 (Prepare):** participants sorted by stable id (canonical order, §9.2) then each votes. On any abort, all
prepared participants are rolled back. **Phase 2 (Commit):** after a durable commit decision, each participant is
committed; cancellation cannot reverse it. A partial commit leaves the result `in_doubt`.

Scope: single-process/single-trust-domain. Distributed consensus is a non-goal (§20).

Every execution request finishes in exactly one **terminal state**. `execution_status`
has 7 canonical values, plus 4 legacy alias enumerators (same underlying value, kept
so existing callers of `::ok` compile unchanged):

| Canonical             | Value | Legacy alias                       |
|-----------------------|-------|------------------------------------|
| `completed`           | 0     | `ok`                               |
| `failed`              | 1     | `lowering_failed`, `runtime_error` |
| `cancelled`           | 2     | —                                  |
| `timed_out`           | 3     | —                                  |
| `unsupported`         | 4     | `unsupported_control_flow`         |
| `backend_unavailable` | 5     | —                                  |
| `invalid_plan`        | 6     | —                                  |

`execution_result<T>` makes impossible states unrepresentable via the `make_*`
factories: a result carries a value **iff** it is `completed`; otherwise it carries a
typed `execution_error` (`kind`, `span`, `fn_name`, `ir_op`, `backend_id`, `plan_id`,
`message`, nested errors). `profiling_report` is pay-for-use — `record()` is a no-op
with no allocation when `enabled == false`.

```cpp
auto r = make_completed<std::int64_t>(42);   // completed, value present
auto e = make_failed<T>(make_error(execution_error_kind::verification_failed, "..."));
auto c = make_cancelled<T>("fn");            // status = cancelled
auto t = make_timed_out<T>("fn");            // status = timed_out
```

## Physical-MIR Verifier Gate (`verify_mir.hpp`)

`verify_crank_mir(fn, expects_value)` wraps lithe's `verify_physical_mir` and adds the
crank-level checks it does not perform, then mints a **`verified_mir`** token. Only a
`verified_mir` may be passed to a backend/plan (private ctor + friend; a default token
is invalid). It is a **non-owning view** — the `physical_mir_function` must outlive it.

Adds beyond lithe's structural pass: (1) a reachable block that ends without a
terminator → `verification_failed` (`path_without_return_or_trap`); (2) a
value-returning function whose `ret` carries no value operand → `missing_return_value`.
Region-legality hooks (GPU/SIMD/defer) are named seams for future passes.

## Capability Discovery (`capability.hpp`)

`discover_backends(execution_options)` returns a policy-gated, deterministic,
fingerprint-cached `capability_set`. The scalar interpreter is always present (semantic
reference); `allow_simd`/`allow_threads`/`allow_gpu` add their descriptors, and
`backend_policy::inline_only`/`threaded_only` narrow the offered set. Static adapters
match the vtable-free `ExecutionBackend` concept; a type-erased boundary is reserved
for plugins. GPU is offered only when a device is actually present
(`gpu_backend::available()`), keeping discovery honest.

## SIMD Legality (`simd_legality.hpp`)

`analyze_simd_legality(loop, width)` classifies a counted loop's memory accesses into a
`dependence_tier` (`distinct_base` → `non_overlapping` → `affine_provable` →
`needs_runtime_guard` → `illegal`) using an affine `base + coeff*iv` subscript model.
Distinct base pointers cannot alias; a same-base `a[i]` vs `a[i+k]` write is a
loop-carried dependence (illegal); differing strides need a runtime alias guard.
Reductions are recognized via `parallel.hpp`'s `reduction_op` (all associative → legal).
It also sizes the vector body vs. scalar tail:
`vector_trip = floor((end−begin)/W)·W`, `scalar_tail = remainder`.

## GPU Residency, Transfers, Events (`gpu_memory.hpp`)

A pure data model of device buffers (`address_space`, `residency_state`,
`buffer_access`) layered over `gpu_backend.hpp`. `plan_transfers(region)` produces a
dependency-ordered `transfer_plan`: upload a device-read buffer only when the host copy
is current; download a device-written buffer and mark the plan's
`visible_device_writes`. `synchronize(plan, allow_replay)` is the **replay-safe fallback
gate** — once visible device writes have committed, it refuses a re-run with
`unsafe_fallback_after_effects` rather than double-applying effects.

## Cancellation, Deadlines, Task FSM (`cancellation.hpp`)

`cancellation_token` observes a lock-free `cancellation_state` tree: parent cancellation
propagates to children, but a child **never** cancels its parent (§13.2).
`effective_deadline(parent, local)` composes deadlines by taking the tighter (min).
`check_interruption(token, deadline)` is the single poll a running task issues — it
reports cancellation first, then deadline expiry, mapping onto `cancelled` / `timed_out`.
The `task_status` FSM (`created → scheduled → running → terminal`, with
cancellation/deadline reachable from any live state) is enforced by `task_state`'s
CAS-guarded `transition`. `task_scope` now carries a `cancellation_token` (its legacy
`cancelled_` bool stays in sync), and `deadline_scope` composes with an enclosing
deadline.

## Plan Construction + Execution (`plan.hpp`)

`construct_plan(verified_mir, opts, hints, transfers)` discovers backends, generates one
`backend_candidate` per backend, and ranks them: **required > preferred > advisory**,
then lowest cost, then determinism. `from_preference` bridges `exec_hint.hpp`'s
`execution_preference` + `required` flag onto a `requirement_strength` (it does not
redefine that enum). A scalar fallback is attached unless a required non-scalar backend
forbids it. Two conflicting `required` hints → `plan_construction_failed`
(`invalid_plan`); a required-but-unavailable backend → `required_backend_illegal` (no
silent fallback). `execution_plan_record` / `to_record` give dump.hpp a serializable
mirror.

`execute_plan<T>(plan, run, token, deadline)` runs the selected candidate through a
caller-supplied `CandidateExecutor` and, on failure, walks the fallback chain — **unless**
the failed candidate committed visible device writes, in which case it returns
`unsafe_fallback_after_effects`. Cancellation/deadline are polled before each attempt.

## Coroutine Backend (`coroutine.hpp`)

`crank_task<T>` is a lazily-started C++20 coroutine carrying a typed
`execution_result<T>`. It binds a `cancellation_token` + optional deadline; a
cancelled/expired task yields `cancelled`/`timed_out`, an escaped exception becomes
`task_panicked`. Suspension is scheduler-driven (the `Scheduler` concept; pravaha pools
wrap via a thin adapter; `inline_scheduler` is the dependency-free default) — a ready
handle is never resumed inline under a crank-held lock. `co_await`ing a task uses
symmetric transfer and yields the inner result. `host.hpp` gains a `blocking_class`
(`non_blocking` / `potentially_blocking` / `async` / `thread_affine`) so the async
planner can place host calls correctly.

## Stable Execution Diagnostic Codes (§15.4)

| Code               | Kind                                  |
|--------------------|---------------------------------------|
| `CRANK-E-EXEC-010` | backend unavailable                   |
| `CRANK-E-EXEC-011` | required backend illegal              |
| `CRANK-E-EXEC-012` | runtime guard rejected                |
| `CRANK-E-EXEC-013` | SIMD alias violation                  |
| `CRANK-E-EXEC-014` | GPU transfer failure                  |
| `CRANK-E-EXEC-015` | GPU sync failure                      |
| `CRANK-E-EXEC-016` | cancellation                          |
| `CRANK-E-EXEC-017` | deadline exceeded                     |
| `CRANK-E-EXEC-018` | unsupported opcode                    |
| `CRANK-E-EXEC-019` | missing return value                  |
| `CRANK-E-EXEC-020` | verification failed                   |
| `CRANK-E-EXEC-021` | unsafe fallback after visible effects |
| `CRANK-E-EXEC-022` | result-type mismatch                  |
| `CRANK-E-EXEC-023` | capability mismatch                   |
| `CRANK-E-EXEC-024` | task panicked                         |
| `CRANK-E-EXEC-025` | plan construction failed              |
| `CRANK-E-EXEC-026` | lowering failed                       |
| `CRANK-E-EXEC-027` | resource exhausted                    |

(`CRANK-E-EXEC-001`/`002` are reserved by `exec_hint.hpp` for hard-requirement-unmet /
soft-fallback.)

---

# Module 5A: Transactions — Multi-Key Serializable Operations

## Overview

Transactions let Crank programs express multi-key, serializable operations on transactional
resources. Ownership split:

| Responsibility                                      | Owner                      |
|-----------------------------------------------------|----------------------------|
| Syntax + policy rules                               | crank                      |
| Read/write-set, conflict detection, commit protocol | Medha                      |
| Proof obligations                                   | Tarka (module 3)           |
| AOT cache metadata                                  | module 4 + transaction.hpp |

No Medha or Lithe API change required (G-TX-1). Crank wraps and policy-checks; Medha executes.

## core.tx Type Mapping

Each Crank language type maps 1:1 to a Medha C++ type:

| Crank type           | Medha C++ type                                   | Kind              |
|----------------------|--------------------------------------------------|-------------------|
| `TxStatus`           | `medha::tx_status`                               | enum              |
| `TxError`            | `medha::tx_error` (via `CrankTxError`)           | structured record |
| `CommitReport`       | `medha::commit_report` (via `CrankCommitReport`) | structured record |
| `TransactionOptions` | `medha::options`                                 | aggregate         |
| `Isolation`          | `medha::isolation`                               | enum              |
| `ReplaySafety`       | `medha::replay_safety`                           | enum              |
| `ConflictPolicy`     | `CrankConflictPolicy`                            | enum              |
| `PartialCommit`      | `medha::partial_commit_policy`                   | enum              |
| `ProofStatus`        | `medha::proof_status`                            | enum              |

`TxError` and `CommitReport` expose named accessors (`status()`, `message()`, `attempts()`, etc.).
Crank never re-invents transaction semantics; these are all pure mappings.

### Defaults

```
isolation      = snapshot
retry          = 0
conflict       = optimistic
distribution   = none  (only supported value)
replay         = unknown
partial_commit = require_atomic_coordinator
```

## Transaction Expression

`transaction { … }` is an **expression** of type `CommitReport`:

```crank
let report = transaction { … }        // binds committed report
transaction { … }                     // bare statement; discards report (no must-use)
```

**Why an explicit keyword?** Atomicity, isolation, conflict resolution, and rollback cannot be
inferred from ordinary reads and writes — they depend on participant capabilities, concurrency
policies, and failure semantics that must be stated at the call site. The `transaction` keyword
makes these properties unambiguous to both the compiler and the reader.

`transaction { … }` is an **expression** whose value is the commit report
(`CrankCommitReport`). It does **not** yield the programmer's `return` value:
a `return` inside the body triggers rollback of the uncommitted writes, and the
transaction expression still evaluates to a report. On a failed commit the
runtime does not return a `Result.Err`/`TxError` value — it surfaces the failure
as a diagnostic (`CRANK-TX-RUNTIME-001` / `CRANK-TX-RUNTIME-002` from
`execute_tx.hpp`). Model typed error propagation explicitly if you need it (e.g.
have the body return `Result[T, E]`).

## Tx-Indexing Lowering

Inside a `transaction` block, resource accesses lower to Medha ops:

| Crank syntax         | Lowered op    | Medha operation    |
|----------------------|---------------|--------------------|
| `resource[key]`      | `tx_read`     | `ctx.load`         |
| `resource[key] = v`  | `tx_stage`    | `ctx.store`        |
| `resource[lo..hi]`   | range read    | `ctx.load` (range) |
| `old(resource[key])` | snapshot read | entry snapshot     |

- Reads observe **read-your-writes**: write set checked first, then resource.
- `load` returns a **copy** — crank binds the copy, never a borrow.
- `old(expr)` in a tx body = value at transaction entry (Medha snapshot).
  This is the second legal `old` context besides `ensures` (module 3).

## Compile-Time Policy Checks

All checks happen in semantic analysis **before lowering**. A failed check is a
compile diagnostic; no illegal combination ever reaches runtime.

| Rule                        | Diagnostic          | Condition                                                                           |
|-----------------------------|---------------------|-------------------------------------------------------------------------------------|
| Non-transactional write     | `CRANK-TX-001`      | Write to resource where `resource_traits<R>::transactional == false`                |
| Cross-resource serializable | `CRANK-TX-002`      | `isolation=serializable` + >1 transactional resource                                |
| old() needs snapshot        | `CRANK-TX-003`      | `old(resource[k])` where `resource_traits<R>::supports_snapshot == false`           |
| Retry/replay conflict       | `CRANK-TX-004`      | `retry>0` + `replay=non_idempotent` (MEDHA-004) or `replay=unknown` (MEDHA-RSF-005) |
| Async in tx                 | `CRANK-TX-005`      | `await`/`spawn` inside transaction body                                             |
| Tx under @parallel          | `CRANK-TX-006`      | Transaction nested under `@parallel`                                                |
| No failure policy           | `CRANK-TX-007`      | Non-`Result` fn without `@on_safety_failure(...)` containing a transaction          |
| Irreversible effect         | `CRANK-TX-008`      | Irreversible effect in tx body (use transactional-outbox instead)                   |
| Nested tx                   | `CRANK-TX-NOTE-001` | Nested same-thread transaction (note, not error; flattened)                         |

### Failure Propagation

A non-committed tx yields `TxError`. Resolution order:

1. `fn -> Result[T, E: FromTxError]` → wraps as `Result.Err(E::from(TxError))`.
2. Fn has `@on_safety_failure(trap|terminate|host_handler)` → uses that policy
   (reuses module 3 `safety_kind::tx_failed` machinery).
3. Neither → `CRANK-TX-007` compile diagnostic.

### Early-Exit Semantics

The `transaction` block is an expression; every exit path has a defined outcome:

| Exit path                                             | Transaction outcome                                           | `defer` actions |
|-------------------------------------------------------|---------------------------------------------------------------|-----------------|
| Normal block completion                               | attempt commit; success → `CommitReport`; failure → `TxError` | run (LIFO)      |
| `return` inside tx body                               | rollback, then return from enclosing function                 | run (LIFO)      |
| `break` / `continue` inside tx body                   | rollback, then perform the control-flow transfer              | run (LIFO)      |
| `await` failure (`crank_future_error`) inside tx body | rollback, then propagate the future error                     | run (LIFO)      |
| Error / `panic` / cancellation                        | rollback, then propagate                                      | run (LIFO)      |
| `trap` / `terminate` (safety policy)                  | rollback attempted best-effort; no guarantee                  | **do not** run  |
| Commit failure (Medha)                                | rolled back by Medha; yields `TxError`                        | run (LIFO)      |

**Rules:**

- A commit is attempted **only** on normal block completion. No partial write is observable to external readers on any
  non-commit path.
- `defer` statements inside a `transaction` body are evaluated at the `defer` site and run in LIFO order on every *
  *controlled** exit (all rows above except `trap`/`terminate`).
- `return` does **not** commit the transaction; it rolls back the uncommitted writes first, then returns. The
  transaction expression itself evaluates to a commit report, not the returned value.

**Interaction with `CRANK-TX-005` (async in tx rejected):** `await` cannot appear inside a transaction body (compile
diagnostic). The "await failure" row above applies when a future's result is consumed just outside the boundary.

### Cross-Resource Restriction

`serializable` isolation touching >1 transactional resource is rejected (`CRANK-TX-002`).
There is no cross-resource coordinator: multi-resource `transaction(coordinator=…)`
syntax parses, but sema still enforces single-resource. Single-resource `serializable`
and any-resource `snapshot` are both allowed.

### Retry/Replay

Crank is stricter than Medha:

- `retry>0` + `replay=non_idempotent` → rejected (MEDHA-004)
- `retry>0` + `replay=unknown` → rejected (MEDHA-RSF-005; crank adds this rule)
- Legal: `body_idempotent`, `body_and_effects_idempotent`, `unknown_but_retry_allowed`

### Concurrency Restrictions

- `await`/`spawn` inside a transaction → rejected.
- Transaction nested under `@parallel` → rejected.
- Same-thread nested `transaction` blocks → flattened (inherit parent retry/isolation).
  GPU lowering of a tx body is also rejected.

## Resource Registration

Register a C++ transactional resource with crank's semantic layer (no macros):

```cpp
auto desc = crank::register_transactional<AccountStore>("AccountStore");
```

`AccountStore` must have `medha::resource_traits<AccountStore>` specialised with
`transactional = true`. The returned `transactional_resource_descriptor` carries:

| Field                      | Source                                         |
|----------------------------|------------------------------------------------|
| `is_transactional`         | `resource_traits<R>::transactional`            |
| `supports_snapshot`        | `resource_traits<R>::supports_snapshot`        |
| `aba_safe`                 | `resource_traits<R>::aba_safe`                 |
| `commit_protocol_ordinal`  | `resource_traits<R>::commit_protocol`          |
| `value_trivially_copyable` | `resource_traits<R>::value_trivially_copyable` |
| `value_move_only`          | `resource_traits<R>::value_move_only`          |
| `resource_stages_values`   | `resource_traits<R>::resource_stages_values`   |
| `supports_rollback`        | `resource_traits<R>::supports_rollback`        |

## AOT Metadata Folding

A change in a resource's transactional protocol **must** invalidate the AOT artifact.
`extend_aot_key_with_resource<R>(key)` folds the resource's traits hash into the
`crank_aot_key::descriptor_hashes` (module 4 key):

```cpp
crank::extend_aot_key_with_resource<AccountStore>(key);
```

Fields hashed per resource: `transactional`, `supports_snapshot`, `aba_safe`, `commit_protocol`,
`value_trivially_copyable`, `value_move_only`, `resource_stages_values`, `supports_rollback`.

`resource_stages_values` and `supports_rollback` currently only feed this AOT
fingerprint — there is no dedicated compile diagnostic for a resource lacking
staging or rollback capability. The only capability diagnostic wired today is
`CRANK-TX-003` (a resource without `supports_snapshot` used under `old()`).

The Medha **dialect version** is stamped into `enabled_features` bits [63:48]:

- `distribution=none` = 1.
- A new distribution dialect forces `enabled_features` to change → AOT cache miss → recompile.
  Distributed mode is never silently reinterpreted.

## Transaction JSON Dump

`dump_tx_plan(tx_plan_record)` emits the transaction plan as JSON:

```json
{
  "isolation": "snapshot",
  "replay": "body_and_effects_idempotent",
  "conflict": "optimistic",
  "retry": 3,
  "partial_commit": "require_atomic_coordinator",
  "transactional_resource_count": 1,
  "read_count": 2,
  "write_count": 1,
  "resource_traits_hash": "0x...",
  "medha_dialect_version": 1
}
```

Build a `tx_plan_record` from a `tx_lowering_result` + `tx_policy_flags` via
`tx_plan_record::from(res, flags)`.

---

# Module 5B: Generics — Monomorphization and Trait Conformance

## Overview

Crank generics are monomorphized (one concrete Vākya subtree per instantiation) with
explicit `trait`/`impl` conformance. No runtime dispatch in hot loops — witness objects
only at explicit dynamic boundaries (host-erased values, plugin edges, forced erasure).

G-VAK-6 fallback (b): the generic Vākya trait machinery is planned but not yet landed.
Crank currently encodes bounds as `trait_set` bits and stores conformance in a crank-local side-table
keyed by `structural_hash`. No Vākya framework change required.

## Monomorphization Model

Each instantiation is a distinct entity:

```
Generic Reduce[T: Monoid + ParallelSafe]
  + concrete type Int64
  → Reduce[Int64]: distinct Vākya subtree
                 → distinct Lithe compilation
                 → distinct AOT cache key
```

Trait calls are **statically resolved** (the selected `impl` is inlined at the call site).
No runtime dictionary is emitted in a hot loop. Witness/table objects are only used at
explicit dynamic boundaries.

## Trait/Impl Conformance

### Explicit only

Conformance is **explicit** — no structural/implicit conformance.
An `impl Trait for Type` must be declared somewhere.

```crank
trait Monoid {
  associative: Bool
  commutative: Bool
}

impl Monoid for Int64 {
  associative = true
  commutative = true
}
```

### Multiple Bounds

Bounds compose with `+`:

```crank
fn Reduce[T: Monoid + ParallelSafe + Copy](xs: [T]) -> T { … }
```

All bounds must be satisfied for the instantiation to succeed.

### Coherence / Orphan Rule (always enforced)

`impl Trait for Type` is legal only when the **current module** owns the trait **or** owns the type.
No overlapping impls for the same (trait, type) pair. Diagnostics:

| Diagnostic                  | Code            | Condition                                                                                            |
|-----------------------------|-----------------|------------------------------------------------------------------------------------------------------|
| Missing impl                | `CRANK-GEN-001` | Required bound not satisfied                                                                         |
| Ambiguous member            | `CRANK-GEN-002` | Member matched by >1 bound; must qualify as `Trait[T].member(...)`                                   |
| Orphan violation            | `CRANK-GEN-003` | Neither trait nor type owned by current module                                                       |
| Duplicate impl              | `CRANK-GEN-004` | Two impls for same (trait, type)                                                                     |
| Overlapping specialization  | `CRANK-GEN-005` | `impl Trait<X> for Type` — controlled specialization is implemented; this fires on overlap/ambiguity |
| Associated type (sema gate) | `CRANK-GEN-006` | `type Item` / `Self.Item` projection — parses, but rejected at sema (not yet supported)              |

## Bound Vocabulary

All bounds encode as `trait_set` bits (bit per `bound_kind`):

| Category    | Bounds                                                                                                     |
|-------------|------------------------------------------------------------------------------------------------------------|
| Ownership   | `Copy` / `Clone` / `Move` / `Drop`                                                                         |
| Numeric     | `Numeric` / `Comparable` / `Ordered`                                                                       |
| Callable    | `Fn(arity+sig)` (user-writeable bound; `FnMut`/`FnOnce` inferred by sema)                                  |
| Execution   | `Pure` / `ParallelSafe` / `GpuCompatible` / `SimdEligible` / `Send` / `Sync`                               |
| Transaction | `Transactional` / `TransactionalResource` / `SnapshotCapable` / `AbaSafe` / `AtomicMultiKeyWithinResource` |
| Algebraic   | `Monoid` / `Semiring` (Self-first, with associated consts)                                                 |

### Callable Bounds

`Fn(T) -> U` is a bound, not a concrete type. Arity and signature are stored
in `callable_sig` inside `impl_record`. An effectful callable + `GpuCompatible` →
`CRANK-GEN-GPU` diagnostic (effectful functions cannot lower to GPU).

### Algebraic Bounds (Monoid)

`Monoid` is Self-first (operates on `Self`). Associated constants carry algebraic facts:

| Constant      | Type   | Meaning                      |
|---------------|--------|------------------------------|
| `associative` | `Bool` | `(a ∘ b) ∘ c == a ∘ (b ∘ c)` |
| `commutative` | `Bool` | `a ∘ b == b ∘ a`             |

Associated functions declared by the trait:

| Function  | Arity | Meaning                                              |
|-----------|-------|------------------------------------------------------|
| `combine` | 2     | `combine(Self, Self) -> Self` — the binary operation |

`impl_record::combine_fn_name` carries the concrete function name (e.g. `"Int64::combine"`) set at `impl` registration.
`impl_witness::combine_fn_name` propagates it through `resolve_witnesses` to the planner.

These facts are forwarded to the execution planner (module 4) so a generic `Reduce`
can parallelize while respecting `@parallel(deterministic=true)`.

## Const Generics

Const generic parameters carry compile-time dimensions:

```crank
fn Dot2[N: usize](a: [N]Float32, b: [N]Float32) -> Float32 { … }
fn MatMul[M: usize, K: usize, N: usize](…) -> … { … }
```

### Rules

| Rule                                 | Description                                                                                              |
|--------------------------------------|----------------------------------------------------------------------------------------------------------|
| Literals, param refs, and arithmetic | `[N]`, `[4]`, `[N+1]`, `[M*K]` are all accepted; a compile-time evaluator folds the dimension expression |
| `usize`/`isize` are parameter kinds  | Not runtime value types; use `Int64`/`UInt64` for runtime                                                |

Const dims feed Vākya's shape algebra (`types/shape.hpp`, `make_matmul_constraints`) so
`cols(a)==rows(b)` is provable at compile time (module 3 discharges it). Shape errors
become compile-time without every program paying for it.

## generic_capability_summary

Each instantiation produces a `generic_capability_summary` — the single fact source
consumed by the execution planner and Tarka:

```cpp
struct generic_capability_summary {
    std::string   generic_name;
    instantiation_key key;
    std::vector<impl_witness> witnesses;   // resolved impl per bound
    std::uint64_t effect_mask;
    std::uint64_t capability_mask;
    trait_set     satisfied_bounds;
    bool          associative;             // from Monoid assoc const
    bool          commutative;
    bool          parallel_safe;           // ParallelSafe bound satisfied
    bool          gpu_compatible;
    bool          transactional;
    bool          snapshot_capable;
    std::optional<std::string> combine_fn_name;  // Monoid combine op name, if impl registered
    std::uint64_t cache_key_fingerprint;   // distinct per instantiation
    // …
};
```

**Source of truth** — the planner and verifier consume only this summary, not the raw bounds.

## Monomorphizer API

```cpp
monomorphizer mono;

instantiation_key key;
key.generic_name = "Reduce";
key.type_args.push_back({kTypeInt64, "Int64", int64_hash});

trait_set required;
required.add(bound_kind::Monoid);
required.add(bound_kind::ParallelSafe);

auto res = mono.monomorphize(key, registry, required, int64_hash, "Int64", span);
// res.ok()                → all bounds satisfied
// res.summary.associative → true (from Monoid impl's assoc consts)
// res.summary.parallel_safe → true
// res.has_runtime_dictionary → false (hot path: static resolution)
```

### Cache Key

Each instantiation gets a distinct `cache_key_fingerprint` (FNV-1a over generic name +
all type arg hashes + const arg values). Feed into `crank_aot_key::descriptor_hashes`
via `instantiation_registry::extend_aot_key(key)`.

## Generics Feature Status

| Feature                                                                                       | Status                                                                                                                                                                                      |
|-----------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| trait/impl + bounds                                                                           | Implemented                                                                                                                                                                                 |
| Coherence/orphan rule (definition site)                                                       | Implemented                                                                                                                                                                                 |
| Cross-module coherence (use site)                                                             | Implemented — an impl is usable only where its defining module is in scope (`CRANK-GEN-013`, `coherence.hpp`)                                                                               |
| Const generics (literals + param refs)                                                        | Implemented                                                                                                                                                                                 |
| Const generic arithmetic (`[N+1]`, `[M*K]`)                                                   | Implemented — compile-time dimension evaluator                                                                                                                                              |
| Type-specialized impls                                                                        | Implemented — controlled specialization, overlap checks (`CRANK-GEN-005`)                                                                                                                   |
| Deterministic specialization selection                                                        | Implemented — total (priority, tiebreak, name) order; `stable_sort` in `select_impl`                                                                                                        |
| Richer callable/effect bounds (`Fn(T)->U`, `SimdEligible`, `Layout[RowMajor]`, `Device[Gpu]`) | Implemented                                                                                                                                                                                 |
| Associated types (`type Item`, `Self.Item` / `C.Item`)                                        | Implemented (sema) — resolved; `CRANK-GEN-006` gate lifted                                                                                                                                  |
| Generic associated constraints (`type Scalar: Numeric`)                                       | Implemented (sema) — bounds enforced (`CRANK-GEN-012`)                                                                                                                                      |
| Generic modules (`module algebra[T: Numeric]`)                                                | Implemented — grammar (`module_decl`, reuses `generic_params`) + `module_decl_node` + instantiation library                                                                                 |
| Separate compilation of instantiations                                                        | Implemented — fingerprint dedup + ABI-gated link (`CRANK-LINK-001`)                                                                                                                         |
| Stable metadata / ABI                                                                         | Implemented — canonical (sorted) `serialize()` + `canonical_hash()`; order-independent AOT keying                                                                                           |
| Structured type-error explanations                                                            | Implemented — `diag_explanation` (expected/found/notes/help) on generic diagnostics (`diagnostic.hpp`)                                                                                      |
| Trait implication (`Ordered ⇒ Comparable`)                                                    | Implemented — `implied_bounds` + `trait_implies`/`satisfies_bound`; implication-aware conformance, witnesses, and specialization (`generics.hpp`, `monomorphize.hpp`, `specialization.hpp`) |
| Instantiation recursion detection                                                             | Implemented — depth + structural-growth guard (`CRANK-GEN-014` / `CRANK-GEN-015`, `limits.hpp`) with expansion-chain reporting                                                              |
| Compile-time resource limits                                                                  | Implemented — per-generic monomorphization budget + candidate-set cap (`CRANK-GEN-016`, `limits.hpp`)                                                                                       |
| Higher-kinded / dependent types                                                               | Not planned for v2 (explicit non-goal)                                                                                                                                                      |
| Generic Vākya machinery (G-VAK-6)                                                             | Currently a crank-local side-table; lift to Vākya framework planned                                                                                                                         |

## Generics JSON Dumps

| Function                          | Description                                                     |
|-----------------------------------|-----------------------------------------------------------------|
| `dump_trait_witnesses(witnesses)` | Resolved `impl` per bound at a call site                        |
| `dump_instantiations(reg)`        | All monomorphized instantiations + `generic_capability_summary` |

```json
// dump_trait_witnesses
[{"bound":"Monoid","trait":"Monoid","type":"Int64"},
 {"bound":"ParallelSafe","trait":"ParallelSafe","type":"Int64"}]

// dump_instantiations
[{"generic":"Reduce","cache_key":"0x...","associative":true,
  "parallel_safe":true,"witness_count":2}]
```

---

# Module 6: Extensions — Typed Annotation System

Typed, namespaced, versioned, capability-declared, policy-checked annotations. Implemented in
`include/languages/crank/annotation.hpp`.

## Overview

Annotations attach metadata, hints, constraints, and proof obligations to crank constructs without mutating IR.
Resolution flow: parse → classify namespace → registry lookup → validate args → apply defaults → capability check →
routing by kind.

## `annotation_kind` and `annotation_strength`

### Kind

| Kind                     | Meaning                                   |
|--------------------------|-------------------------------------------|
| `metadata`               | Attach key/value metadata to a region     |
| `optimization_hint`      | Bias cost model; maps to `execution_hint` |
| `constraint`             | Legality invariant                        |
| `capability_declaration` | Declare required capability               |
| `effect_declaration`     | Declare effect mask                       |
| `proof_annotation`       | Assert/prove an obligation                |
| `syntax_extension`       | Register a frontend syntax hook           |

### Strength (orthogonal to kind)

| Strength     | Meaning                                         |
|--------------|-------------------------------------------------|
| `advisory`   | Hint; may be ignored                            |
| `preferred`  | Bias cost model; keep fallback                  |
| `required`   | Mandatory; hard diagnostic if unmet             |
| `assumption` | Caller-asserted; verify under `paranoid` policy |

## Descriptor Registration

### Schema definition

```cpp
// arg<Name, Type>: compile-time typed argument element
template <lithe::fixed_string Name, annotation_arg_type Ty>
struct arg {};

// annotation_schema<Args...>: variadic schema
template <class... Args>
struct annotation_schema {};
```

`annotation_arg_type` vocabulary: `u32`, `i64`, `f64`, `boolean`, `string`, `ident`.

### Registering a descriptor via `context`

```cpp
ctx.register_annotation<"lithe.cacheline",
    annotation_schema<arg<"align", annotation_arg_type::u32>>>(
    annotation_kind::optimization_hint,
    annotation_strength::advisory,
    2000  // stable_id >= 1000
);
```

`consteval` flattens `annotation_schema` to a `std::array<schema_field, N>` at compile time; no allocation on the hot
path.

### `annotation_descriptor` fields

| Field              | Type                  | Description                                     |
|--------------------|-----------------------|-------------------------------------------------|
| `name`             | `string_view`         | Fully-qualified name (e.g. `"lithe.cacheline"`) |
| `kind`             | `annotation_kind`     | Routing category                                |
| `default_strength` | `annotation_strength` | Policy-level strength                           |
| `stable_id`        | `uint32_t`            | Stable identity; ext-band `>= 1000`             |
| `version`          | `uint32_t`            | Schema version (default 1)                      |
| `effects`          | `effect_mask`         | Declared effect bits                            |
| `capabilities`     | `capability_mask`     | Declared capability bits                        |
| `name_hash`        | `uint64_t`            | FNV hash for fast lookup                        |

## Namespacing Rules

**Unqualified annotations are forbidden for extensions.** Only the closed built-in set may be unqualified:

```
@parallel  @simd  @gpu          ← from exec_hint.hpp crank_attr_kind
@pure  @reads  @writes  @io  @net  @host   ← from effects.hpp fn_attribute_set
```

Any other unqualified annotation → **CRANK-ANN-001** (hard error, never preserved).

**Reserved namespaces** (9 total):
`crank.` `lithe.` `pravaha.` `medha.` `tarka.` `sutra.` `domain.` `company.` `user.`

Use `company.*` or `user.*` for project-local annotations.

Helpers:

```cpp
is_builtin_unqualified(name)   // true iff in the 9-element closed set
has_namespace(name)            // true iff name contains '.'
is_reserved_namespace(ns)      // true iff ns is one of the 9 reserved prefixes
```

**Drift guard** (test 13 in `test_crank_annotation.cpp`): asserts the built-in set == exec-attr ∪ effect-attr spellings.
The lists cannot diverge silently.

## Resolution Policy Flow

```
parse → annotation_resolver::resolve(parsed_annotation)
  1. has_namespace? NO  → is_builtin_unqualified? NO  → CRANK-ANN-001 (hard)
                         YES → look up "crank.<name>" in registry
  2.                YES → registry.resolve(fq_name)
       found?      YES → validate_args_with_schema → check capabilities → return resolved
       not found?  → host_handler? YES → preserved=true (host handles)
                    → preserve_unknown policy? YES → preserved=true
                    → strict policy (default)  → CRANK-ANN-002 (error)
```

`annotation_policy`:

- `strict` (default): unknown namespaced annotation → CRANK-ANN-002.
- `preserve_unknown`: unknown namespaced annotation → kept, no error.

## Argument Validation

`validate_args_with_schema(desc, reg, ann)` → `vector<annotation_diagnostic>`:

| Code          | Condition                   |
|---------------|-----------------------------|
| CRANK-ANN-003 | Argument name not in schema |
| CRANK-ANN-004 | Argument type mismatch      |
| CRANK-ANN-005 | Required argument missing   |

```cpp
// Example: validate after manual resolution
auto* desc = reg.resolve("lithe.cacheline");
auto diags = validate_args_with_schema(*desc, reg, ann);
```

## Kind → Decision Routing

`consume(desc, args)` → `annotation_effect`:

| Kind                     | Routing                                                     |
|--------------------------|-------------------------------------------------------------|
| `optimization_hint`      | `map_exec_attr` → `execution_hint` (reuses `exec_hint.hpp`) |
| `capability_declaration` | `add_caps = desc.capabilities` (ext-band mask)              |
| `effect_declaration`     | `add_effects = desc.effects` (ext-band mask)                |
| `constraint`             | `constraints` vector entry                                  |
| `proof_annotation`       | `proof_obligations` vector entry                            |
| `metadata`               | `metadata` key/value pair                                   |
| `syntax_extension`       | Registered frontend only; no runtime routing                |

`annotation_effect` is a pure data record — annotations **never** emit/mutate IR.

## Diagnostic Code Table

| Code          | Kind                         | `is_error` | Trigger                                       |
|---------------|------------------------------|------------|-----------------------------------------------|
| CRANK-ANN-001 | `unqualified_extension`      | true       | Unqualified non-builtin                       |
| CRANK-ANN-002 | `unknown_namespaced_strict`  | true       | Unknown name, strict policy                   |
| CRANK-ANN-003 | `arg_name_not_in_schema`     | true       | Arg key not in schema                         |
| CRANK-ANN-004 | `arg_type_mismatch`          | true       | Arg wrong type                                |
| CRANK-ANN-005 | `missing_required_arg`       | true       | Required arg absent                           |
| CRANK-ANN-006 | `capability_not_satisfied`   | true       | Capability check failed                       |
| CRANK-ANN-007 | `assumption_paranoid_verify` | true       | `assumption` strength under `paranoid` verify |

## Plugin Model

### `CrankExtension` concept

```cpp
template <class E>
concept CrankExtension = requires(E& e, annotation_registry& ar) {
    e.register_annotations(ar);
};
```

No virtual — dispatch via `if constexpr` / `requires`.

### Defining an extension

```cpp
struct MyExtension {
    static constexpr std::uint32_t id      = 5000;
    static constexpr std::uint32_t version = 1;

    void register_annotations(annotation_registry& reg) {
        annotation_descriptor d;
        d.name             = "company.my_hint";
        d.kind             = annotation_kind::optimization_hint;
        d.default_strength = annotation_strength::advisory;
        d.stable_id        = 5001;
        d.name_hash        = containers::desc_name_hash("company.my_hint");
        reg.register_desc(d, {});
    }
};
```

### Installing

```cpp
ctx.install_extension(MyExtension{});
// or directly:
crank::install_extension(reg, MyExtension{});
```

Only `register_annotations` is wired today. The remaining hooks (`register_types`, `register_functions`,
`register_containers`, `register_passes`) are deferred per §5b.9 "static first" (see Roadmap).

`consteval auto extension_descriptor<E>()` returns `{id, version}` at compile time.

## Annotation Resolution Algorithm

```
Input: parsed_annotation{name, args[], at}
       annotation_registry (seeded by make_crank_annotation_registry())
       annotation_policy   (strict | preserve_unknown)

1. classify(name):
   - dot present?  → QUALIFIED
   - in kBuiltinUnqualifiedAnnotations?  → BUILTIN_UNQUALIFIED
   - else → UNQUALIFIED_EXTENSION  →  emit CRANK-ANN-001, STOP

2. BUILTIN_UNQUALIFIED: fq = "crank." + name; go to LOOKUP(fq)

3. QUALIFIED: fq = name; go to LOOKUP(fq)

LOOKUP(fq):
4. desc = registry.resolve(fq)  [by name_hash]
5. desc found? → VALIDATE_ARGS(desc, args)
               → capability_check(desc)
               → return {desc, diags, resolved_args}

6. desc not found:
   a. host_handler present && host_handler(ann)? → preserved=true, return
   b. policy == preserve_unknown?               → preserved=true, return
   c. else (strict)                             → emit CRANK-ANN-002, return

VALIDATE_ARGS(desc, args):
7. For each arg in args:
   a. find schema_field by name_hash  → not found: CRANK-ANN-003
   b. type_check(arg.value, sf.type)  → mismatch: CRANK-ANN-004
8. For each required schema_field: not in args → CRANK-ANN-005

Output: annotation_resolution{desc*, diags[], resolved_args[], preserved}
```

## Extensions JSON Dump

`make_annotation_records(annotations, resolutions)` → `vector<annotation_record>`.

`dump_annotations(records)` → JSON string. Fields: `fq_name`, `kind`, `strength`, `stable_id`, `version`,
`resolved_args`, `diagnostics`.

Assumption-strength annotations are visible in diagnostics + AOT metadata.

---

## Worked Examples

### Scalar function

```crank
fn add(a: Int64, b: Int64) -> Int64 {
    a + b
}
```

### Parallel for

```crank
@parallel
for i in 0..n {
    result[i] = compute(input[i]);
}
```

### Generic function

```crank
fn dot<T: Numeric>(a: Vec<T>, b: Vec<T>) -> T {
    parallel.reduce(zip(a, b).map(|(x, y)| x * y), 0, add)
}
```

### Transaction

`transaction(isolation) { … }` is the canonical syntax. The isolation argument is optional; default is `snapshot`.

```crank
fn transfer(from: accounts, to: accounts, amount: i64) -> Result[Unit, TxError] {
    transaction(serializable) {
        let bal = accounts[from];   // accounts is one resource; from/to are keys
        if bal < amount { return Err(insufficient); }
        accounts[from] = bal - amount;
        accounts[to]   = accounts[to] + amount;
    }
}
```

`accounts[from]` and `accounts[to]` are two key accesses on the **same** transactional resource `accounts`, not two
separate resources.

### Transaction rollback conformance

The `return` inside a `transaction` body **rolls back** before returning from the enclosing function — no write is
observable to external readers on the early-exit path.

```crank
fn transfer(from: AccountId, to: AccountId, amount: Int64) -> Result[Unit, TxError] {
    transaction(serializable) {
        let bal = accounts[from];
        if bal < amount {
            return Err(TxError.from(insufficient));  // rollback; zero writes visible
        }
        accounts[from] = bal - amount;               // only reached if bal >= amount
        accounts[to]   = accounts[to] + amount;
    }
}
```

**Conformance requirement:** when the `if` branch fires, the failed path performs no writes and leaves `accounts[from]`
and `accounts[to]` unchanged. This follows directly from the early-exit rule
in [§Early-Exit Semantics](#early-exit-semantics):

| Exit path                                | Writes visible?                |
|------------------------------------------|--------------------------------|
| `return` inside tx body                  | No — rolled back before return |
| Normal block completion (commit success) | Yes                            |
| Commit failure (Medha)                   | No — rolled back by Medha      |

---

# Debugging & Introspection

Crank exposes a debugger/language-server surface split across two headers:

- `include/languages/crank/debug_info.hpp` — the static **data model** (POD-ish
  aggregates, no serialization): scopes, variables, line table, breakpoints, and
  the runtime debug-event/hook vocabulary.
- `include/languages/crank/debug.hpp` — **builders** (assemble the model from
  existing pipeline artifacts, no re-parsing), **JSON serializers** (DAP/LSP
  shaped), a **pipeline stats snapshot**, and a guarded NADI pulse helper.

Everything is pay-for-use: no debug data is built until a builder is called,
hooks default to no-op, and NADI compiles out when `observability/nadi.hpp` is
absent.

> **Runtime stepping is stubbed.** The interpreter executes control flow, but
> the debugger does not yet drive it interactively: there is no live pause/step
> loop wired to execution. The event/hook interfaces and `debug_stepping_state`
> are defined and wireable now, so a host can attach breakpoints/watches; the
> stepping backend that pauses the interpreter mid-CFG is not yet implemented.

## Data model (`debug_info.hpp`)

| Type                  | Purpose                                                                                                                                                                        |
|-----------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `debug_scope_kind`    | `module / function / block / loop / transaction`                                                                                                                               |
| `debug_variable`      | `{ name, type_name, type_ref, decl_span, is_param, is_mutable }`                                                                                                               |
| `debug_scope`         | flat-tree node: `{ kind, name, span, parent, depth, locals, structural_hash }` — tree encoded by `parent` index (`kNoParent` for roots), pointer-free & trivially serializable |
| `debug_line_entry`    | one source↔IR row: `{ src, fn_name, hl_op_index, block_index, is_stmt_boundary }`                                                                                              |
| `breakpoint_location` | `{ line, col, resolved, fn_name, verified }`                                                                                                                                   |
| `debug_info`          | unit aggregate `{ module_name, scopes, line_table, globals }` + queries `scopes_at(span)`, `variables_in_scope(i)`, `enclosing_function(line)`                                 |

`resolve_breakpoint(di, line, col=0)` snaps a requested line to the nearest
statement-boundary row at or after it; `verified == false` when none exists.

`source_span` (`source_span.hpp`) is the single location currency — no new
location type is introduced.

## Runtime events + hooks (stubbed)

- `debug_event_kind`: `breakpoint_hit / step_line / step_in / step_out /
  watch_write / scope_enter / scope_exit / trap`.
- `debug_event` — `{ kind, at, scope_index, detail }`.
- `DebugEventSink` concept — any `S` with `void on_event(const debug_event&)`.
  Dispatch is non-virtual (concept + `if constexpr`), never a vtable.
- `debug_hooks` — optional `std::function` callbacks (`on_breakpoint / on_step /
  on_watch`) plus breakpoint/watch registries; `dispatch(event)` routes to the
  matching callback (no-op when unset). `null_debug_hooks` is the zero-cost
  default. `std::function` here is acceptable at host/tool boundaries; it is not
  on the critical execution path.

## Builders (`debug.hpp`)

```cpp
#include "languages/crank/debug.hpp"

auto pr = crank::frontend::parse(src);
auto ar = ctx.analyse(pr);                       // gives resolve_result

crank::debug_info di = crank::build_debug_info(ar.resolve, "mymod");
// fold HL loop/defer/exit spans into the line table:
auto lr = crank::lower_to_hl(std::move(input));
crank::append_line_table(di, lr);

auto bp = crank::resolve_breakpoint(di, /*line=*/12);
```

`build_debug_info` walks the resolver symbol table into a flat scope tree:
module root + one scope per function; depth-0 values become `globals`, deeper
values attach to the enclosing function scope. `decl_span` is zeroed where the
symbol table cannot supply a span (spans live in the `property_store` keyed by
`structural_hash`, not joinable to symbols by name). Type names are emitted as a
stable `type#N` token from the Vakya `type_id`; editors map the id via the
`type_registry`.

## JSON serializers

Follow the `dump.hpp` pattern — one `glz::meta<>` per data struct plus a manual
fallback, guarded by `CRANK_HAS_GLAZE`:

- `dump_debug_info(di)` — full DAP-style bundle (module, scopes, line table, globals).
- `dump_scopes(di)`, `dump_line_table(di)`.
- `dump_breakpoints(span<const breakpoint_location>)`.
- `dump_debug_event(event)`.

## Pipeline stats snapshot

`pipeline_stats_snapshot { optional<parse_stats>, optional<hl_lowering_stats>,
optional<execute_stats> }` bundles every stage; `dump_pipeline_stats(snap)`
emits only the present stages. Each stage struct gained editor-facing counters
(additive, defaulted — existing consumers unaffected):

- `parse_stats`: `identifier_count`, `literal_count`, `comment_bytes`, `deepest_fn_name_len`.
- `hl_lowering_stats`: `block_count`, `max_loop_nest`.
- `execute_stats`: `branch_count`, `block_count`.

## NADI pulse (guarded)

Behind `#if __has_include("observability/nadi.hpp")`, `debug.hpp` defines
`crank_debug_pulse = utils::nadi::Pulse<"crank.debug", ...>` and
`emit_debug_pulse<Sink = NoSink>(const debug_event&)`. When NADI is absent the
helper is an inline no-op, keeping call sites portable.

## `context` accessor

`ctx.debug()` returns a fluent `debug_config` (mirrors `execution_config`):

```cpp
ctx.debug()
   .collect(true)
   .on_breakpoint([](const crank::debug_event& e){ /* ... */ })
   .add_breakpoint(bp)
   .watch("acc");
```

Purely opt-in; an untouched `debug_config` holds empty hooks and
`collect_debug_info() == false`, so a non-debug session costs nothing.

---

# Domain Views

Domain views provide a *checked semantic interpretation* of an existing value without copying.
Crank splits `type` (storage) from `view` (meaning) from `impl` (behavior):

```crank
type  Array[T, Shape] struct { }            // storage
view  Tensor[T, Shape] of base: Array[T, Shape]  // view
      requires contiguous(base)
impl  Tensor[T, Shape] { ... }             // behavior on the view
```

**Feature gate:** `domain_views`. When off, `view_decl` and `view_expr` parse but are rejected
with `CRANK-VIEW-000`. Enable via `feature_set::enable(crank_feature::domain_views)`.

## Conceptual Model

```
type  = storage / representation
view  = semantic interpretation  (NEW: view_decl)
impl  = behavior on storage OR view
```

Three relations the compiler tracks for `view V of b: S`:

1. **Backing**: `V` is backed by source type `S`; `b` names the binding inside methods.
2. **Obligations**: `requires` clauses checked at every view construction site.
3. **Domain metadata**: optional `@sutra.*` annotations that survive to lowering/planning.

Key invariants:

- **No silent view** — construction always uses `view expr as ViewType`.
- **No silent copy** — a view never allocates; requiring a copy → `CRANK-VIEW-005`.
- **No silent metadata loss** — domain metadata either propagates or is a diagnostic (`CRANK-VIEW-010`).

## Surface Syntax

```ebnf
view_decl   = "view" IDENT [ generic_params ] "of" IDENT ":" type { "requires" pred_expr } ;
view_source = qualified_ident | "(" expr ")" ;
view_expr   = "view" view_source "as" type ;
```

`"of"` is contextual — only special inside `view_decl`, never globally reserved.
The `view_expr` is a primary; postfix chains attach to the view result:
`(view a as Tensor[...]).matmul(x)`.
Source is a `qualified_ident` or parenthesized `(expr)`. For complex source
expressions parenthesize: `view (x.field) as T`. Using a full expr would allow
the level-10 `as` cast operator to greedily consume the keyword.

**`impl` on views** reuses existing `impl_decl` syntax — the analysis pass resolves the
target against both type and view symbol tables.

## View Type Model (§4.3)

A view type is a thin wrapper `view_type = { backing_type_ref, view_descriptor_id, generic_subst }`.
It is **not** a new type category — it is a hash-consed node in the type arena.

Four rules:

1. **Monomorphization key** = `(view_descriptor_id, generic_subst)`.
2. **Method dispatch is view-table-only.** `A.matmul(x)` looks up the view's `method_table` only.
3. **`self.<backing>` is the only bridge to storage.** Inside a view method, `self.base` resolves
   to the backing value.
4. **No implicit un-view.** A view type unifies only with the same `(view_id, subst)`.

## Obligation Model (§4.4)

View construction emits obligations via `obligation_builder` (family `obligation_family::view`):

| Predicate          | Discharged by                       |
|--------------------|-------------------------------------|
| `contiguous(base)` | SMT (z3, opt-in) or runtime guard   |
| `aligned(base, N)` | Same                                |
| `rank(base) == R`  | Const-dim algebra (`const_dim.hpp`) |
| `shape_matches`    | Same                                |
| user `requires`    | SMT or runtime guard                |

Discharge outcomes (via `verify.hpp`):

- **proven** → view erased; no runtime guard
- **unknown/deferred** → runtime guard inserted (`CRANK-VIEW-004`, informational)
- **refuted** → compile error (`CRANK-VIEW-002` or `CRANK-VIEW-003`)

## Borrow Rule (§8.4)

A view borrows its backing. At view-construction points:

- ≤1 mutable view XOR N immutable views of the same backing.
- Backing is treated as *borrowed* while any live view exists.
- Static where provable → `CRANK-VIEW-008` at compile time.
- Unknown aliasing → runtime guard (`obligation_family::view`, `add_view_aliasing`).
- View outliving backing → `CRANK-VIEW-007`.

Mutability lives on the Crank-side view wrapper; it is **not** lowered into the Lithe `memref_desc`.

## Sutra Domain Framework Binding (§4.5)

`@sutra.*` annotations bind to real Sutra Domain Framework components — consumed **at Crank compile
time only**. No IR pipes through Sutra. `Crank → Lithe` is direct.

| Annotation                            | Sutra component                                                                                       |
|---------------------------------------|-------------------------------------------------------------------------------------------------------|
| `@sutra.domain(name=…)`               | domain descriptor / `domain_traits<Tag>`; foundation ids: scalar=0, tensor=1, tree=2, graph=3, data=4 |
| `@sutra.op(name=…)`                   | three backend seams: `register_blas_hook` / `op_descriptor.simd_fn` / `op_descriptor.msl_fragment`    |
| `@sutra.laws(pure, deterministic, …)` | `operation_metadata` / `op_metadata_of<Tag>`; `Pure+Deterministic` → zero SMT, no Tarka round-trip    |
| `@sutra.affinity(simd, gpu, …)`       | `execution_affinity`; advisory — platform decides execution                                           |

**Cross-domain ops** (`Tensor + Scalar`): resolved via Sutra `resolve_interaction` (5-phase, opt-in).
Proof obligations share the same Tarka `verify()` sink as view obligations. Undefined interaction →
`CRANK-VIEW-002` with the full resolver trace.

**Materialize-out (un-view)**: zero-copy = lossless edge (free); copying = costed edge (explicit
`materialize(…)` only; implicit → `CRANK-VIEW-005`). Implemented via Sutra `conversion.hpp`
weighted least-cost query.

## Lowering Model (§4.6)

Three lowering shapes:

1. **Erased** — fully-static shape, obligations proven, view doesn't escape → view disappears.
2. **Base-reference only** — metadata compile-time known, view escapes → backing ref + compile-time
   side-table.
3. **Lithe `memref_type`** — runtime-known shape/stride → `memref_desc` (rank, element_kind,
   shape, strides). No new IR op family. Runtime view guards → `guard` + `trap` (HL `assert`).

View method-call lowering:

- Pure-Crank body → inline (existing phases A–E).
- `@host.fn` → host thunk.
- `@sutra.op(name=…)` → `("lithe.hl","call")` to the registered hook; backend at plan time:
  `blas_hook` → `simd_fn` → `msl_fragment` → scalar fallback.

**Domain metadata is compile-stage ephemeral in v1** — steers Crank's planner but is dropped at the
IR trust boundary. Wire contract deferred to Lithe 1.6.0 `domain_attr` (v2 additive minor).

## Host Interop (§4.7)

- **Extern backing type**: `@host.type(name=…) type T extern` + `view V of base: T` → zero-copy
  view onto a C++ buffer.
- **View provider**: `context_builder::register_view_{check,metadata,lowering}` callbacks. Missing
  provider → `CRANK-VIEW-009`.

## Diagnostics — CRANK-VIEW-000..011

| Code           | Kind                 | Trigger                                                                             |
|----------------|----------------------|-------------------------------------------------------------------------------------|
| CRANK-VIEW-000 | feature_disabled     | `view` used without `domain_views` feature                                          |
| CRANK-VIEW-001 | unknown_target       | target view type not found / not a view                                             |
| CRANK-VIEW-002 | not_viewable         | source type cannot be viewed as target (structural refuted OR no legal interaction) |
| CRANK-VIEW-003 | requirement_failed   | a `requires` obligation refuted at compile time                                     |
| CRANK-VIEW-004 | runtime_guard (info) | unknown obligation → runtime view guard inserted                                    |
| CRANK-VIEW-005 | would_copy           | un-view takes a costed conversion edge; use `materialize(…)`                        |
| CRANK-VIEW-006 | ambiguous_decl       | ambiguous view declaration / overlapping backing                                    |
| CRANK-VIEW-007 | lifetime             | view outlives its backing storage                                                   |
| CRANK-VIEW-008 | mutable_conflict     | conflicting mutable views of the same storage                                       |
| CRANK-VIEW-009 | provider_missing     | host-backed view has no registered provider                                         |
| CRANK-VIEW-010 | metadata_conflict    | conflicting domain metadata at Crank lowering                                       |
| CRANK-VIEW-011 | reserved             | `ensures`/`where` on a `view_decl` (reserved)                                       |

Orphan-rule violations reuse the existing **coherence** diagnostic (not a new VIEW code).

## v1 Minimal Set

Syntax: `view Name[Params] of base: SourceType [requires …]`, `view primary as ViewType`.
Semantics: view decl, explicit view construction, `requires` obligations, `impl` methods on views,
pure-Crank method bodies, optional `@sutra.*` annotations, obligation emission, metadata propagation.
Initial domain: **Tensor** only (Image/GraphAdjacency are pure-Crank examples in docs).

Deferred to v2: implicit views, type-directed shorthand, non-tensor domain lowering, distributed views,
autograd, wire-portable metadata (Lithe 1.6.0 `domain_attr`).

## Full Linear Example

```crank
package app
import "sutra.tensor"

type Array[T, Shape] struct { }

@sutra.domain(name = "tensor")
view Tensor[T, Shape] of base: Array[T, Shape]
requires contiguous(base)

impl[T, Shape] Tensor[T, Shape] {
    @sutra.op(name = "tensor.reduce_sum")
    @sutra.laws(pure = true, deterministic = true)
    fn reduce_sum(self) -> T {
        var acc: T = T.identity()
        for i in indices(Shape) { acc = acc + self.base[i] }
        return acc
    }
}

fn linear(w: Array[Float32,[M,K]], x: Array[Float32,[K,N]]) -> Tensor[Float32,[M,N]] {
    let W = view w as Tensor[Float32, [M, K]]
    let X = view x as Tensor[Float32, [K, N]]
    return W.matmul(X)
}
```

---

# Roadmap & Non-Goals

This section is the forward-looking backlog. It does **not** describe currently
available behavior — for what the compiler accepts today, see the Grammar
Summary and Capability Matrix above, which carry per-feature status labels.

> **Already landed.** Several items once tracked here now ship in the current
> implementation and are documented in the sections above: CFG-aware scalar
> execution, const-generic arithmetic, controlled specialization, the SIMD
> backend (Highway), GPU SPIR-V emission, and program-built reflection
> descriptors. They remain listed below only for design context; treat the
> Capability Matrix as authoritative for status.

**Still planned / not available:**

- Grammar-level `module` keyword — generic modules are supported by the sema library
  (`module_generics.hpp`), but the surface `module M[T: Bound] { … }` production is not yet parsed.
- Structured concurrency surface in the language (`task_scope`, `deadline`,
  `savepoint`, `rollback_to`, `@reflect` as grammar keywords) — these exist only
  as standalone host-side C++ APIs.
- Async `CoroutineBackend`; the futures runtime is currently eager-inline.
- Multi-resource transactions past parsing — sema still enforces single-resource
  (`CRANK-TX-002`).
- AOT artifact signing / secure loader (`serialize()` is write-only; no parse path).
- In-tree distributed execution runtime; Metal currently covers the shared f32 elementwise GPU subset.

---

## Language Features

### §v2.1 Associated Types

Lift `CRANK-GEN-006` gate. `type Item` in a trait body and `Self.Item` / `C.Item` projections are resolved and
normalized in sema.

```crank
trait Collection {
    type Item;
    fn get(self, i: Int64) -> Self.Item;
    fn len(self) -> Int64;
}

fn first[C: Collection](c: C) -> Option[C.Item] {
    if c.len() == 0 { return Option.None }
    return Option.Some(c.get(0))
}
```

**Sema changes:**

- `assoc_type_decl` (`type Item`) in trait body: now resolved, stored in `trait_record::assoc_types`.
- `Self.Item` inside `impl` body: resolved to the concrete type declared in `impl member`.
- `C.Item` in generic context: resolved via the witness for `C: Collection`; stored in `impl_witness::assoc_type_map`.
- Projection `C.Item` used as a type argument: normalized before monomorphization.
- Diagnostic `CRANK-GEN-006` removed; replaced by full projection diagnostics (`CRANK-GEN-010` ambiguous projection,
  `CRANK-GEN-011` missing assoc type impl).

### §v2.2 Generic Associated Constraints

Associated types may carry bounds:

```crank
trait Matrix {
    type Scalar: Numeric;
    const Rows: usize;
    const Cols: usize;
}
```

At every use site, `M.Scalar` is known to satisfy `Numeric` — the bound is part of the projection type. The
`const Rows` / `const Cols` associated constants supply compile-time dimensions to Vākya's shape algebra.

**Sema changes:**

- `assoc_type_decl` may carry `":" bound { "+" bound }` — stored in `assoc_type_record::bounds`.
- `assoc_const_decl` in trait bodies: already supported for non-generic constants; extended to type-parametric bounds.
- At projection sites, the projection type carries the declared bounds as a constraint set; downstream obligation checks
  treat the bound as an assumption.

### §v2.3 Generic Modules

```crank
module algebra[T: Numeric] {
    fn dot(a: []T, b: []T) -> T { ... }
    fn norm(v: []T) -> T { ... }
}
```

**Semantics:**

- A generic module is separately compiled; the bound `T: Numeric` is the module's public contract.
- Instantiation `algebra[Float32]` produces a monomorphized module with its own AOT cache key.
- Exported items obey the same explicit-annotation rule as exported functions.
- Visibility keywords (`pub`, default package-private) apply at the module level.
- Module-level substitutions (`where T = Float32`) are sugar for instantiation.
- Cross-module generic coherence: an `impl` from module `A` is visible in module `B` only if `B` imports `A` or the impl
  is in a shared base module.

**New module resolver step** (inserted between steps 1 and 2 of the Compilation Pipeline):

```
1a | Generic module instantiation | `module_instantiation_result`
```

### §v2.4 Const-Generic Expressions

> **Status: Implemented.** Const-generic arithmetic is available today — see [Const Generics](#const-generics). This
> entry is retained for design context only.

A compile-time dimension evaluator handles:

```crank
[N + 1]
[M * K]
[N / 2]      // integer division; N must be even or diagnostic CRANK-GEN-DIM-001
```

**Rules:**

- Operators: `+`, `-`, `*`, `/`, `%` over `usize`/`isize` parameters and literals.
- Overflow: evaluated at the instantiation site; out-of-range → `CRANK-GEN-DIM-002`.
- Division by zero: constant zero denominator → `CRANK-GEN-DIM-003` (compile error).
- Arithmetic result used where `usize` is expected must not underflow (e.g. `N - M` when `N < M` → diagnostic).
- Shape algebra (`types/shape.hpp`) consumes the evaluated constant; existing shape constraints continue to work.

| Diagnostic          | Condition                              |
|---------------------|----------------------------------------|
| `CRANK-GEN-DIM-001` | Non-integer result in `usize` context  |
| `CRANK-GEN-DIM-002` | Dimension overflow at instantiation    |
| `CRANK-GEN-DIM-003` | Division by zero constant in dimension |

### §v2.5 Controlled Specialization

> **Status: Implemented.** Controlled specialization is available today; `CRANK-GEN-005` fires on overlap/ambiguity —
> see [Generics Feature Status](#generics-feature-status). Retained for design context.

Overlap and coherence checks:

```crank
impl[T: Numeric] VectorOps for Vector[T] { ... }        // generic base
impl VectorOps for Vector[Float32] { ... }               // type-specialized override
```

**Rules:**

- The generic base implementation may overlap with a specialization; the specialization wins at any instantiation it
  covers (the base is not a peer).
- Two peer specializations may not overlap unless one is strictly more specific than the other; an unordered overlap is
  `CRANK-GEN-007` (ambiguous specialization).
- A specialization cannot weaken safety/effect constraints from the base impl.
- Specialization coherence: a specialization `impl Trait for Concrete` is legal only if the module owns `Trait` **or**
  `Concrete` (orphan rule still applies).
- Higher-kinded specialization and variance are **v3**.

| Diagnostic      | Condition                                       |
|-----------------|-------------------------------------------------|
| `CRANK-GEN-007` | Two specializations overlap with no ordering    |
| `CRANK-GEN-008` | Specialization weakens safety/effect constraint |
| `CRANK-GEN-009` | Specialization violates orphan rule             |

### §v2.6 Richer Callable and Effect Bounds

Extend bound vocabulary:

```crank
F: Fn(T) -> U + Pure + SimdEligible
A: Layout[RowMajor] + Device[Gpu]
```

**New bounds:**

| Bound              | Meaning                                            |
|--------------------|----------------------------------------------------|
| `SimdEligible`     | Body may lower to SIMD vector ops                  |
| `GpuCompatible`    | Body may lower to GPU compute (existing, promoted) |
| `Layout[RowMajor]` | Memory layout constraint for tensor/matrix types   |
| `Layout[ColMajor]` | Column-major layout                                |
| `Device[Gpu]`      | Declares affinity for GPU execution                |
| `Device[Simd]`     | Declares affinity for SIMD execution               |
| `Device[Host]`     | Declares affinity for CPU host                     |

Layout and device bounds feed the execution planner's backend ranking and emit diagnostics when a constraint cannot be
satisfied by any available adapter.

### §v2.1a Generics Maturity

The generic system is matured with four production properties on top of the v2 feature set. None change the meaning of
an existing single-module program; each is additive.

**Cross-module coherence (use-site).** The orphan rule (`CRANK-GEN-003`) governs where an `impl` may be *defined*. A
separately-compiled program also needs a *use-site* rule: an `impl` is usable at a call site only if its defining module
is **in scope** — the current module is the impl's module, or (transitively) imports it. Built-in / prelude impls (
`std.core`) are always in scope. An impl that exists but is out of scope is `CRANK-GEN-013` (distinct from
`CRANK-GEN-001` "no impl exists": the fix is an `import`, not a new `impl`).

```text
CRANK-GEN-013: impl of 'Numeric' for 'Matrix' is defined in module 'linalg',
               which is not imported by 'app'
  help: add `import "linalg"` to module 'app'
```

C++ surface (`coherence.hpp`): `impl_visibility_ctx{current_module, imported*}`,
`check_witness_visibility(witnesses, ctx, at)`, and
`monomorphize_in_scope(mm, key, registry, required, hash, name, ctx, at)` — the visibility-aware driver. Callers without
cross-module context keep using the plain `monomorphizer` (no gate; existing behavior byte-for-byte).

**Structured type-error explanations.** Generic diagnostics carry an optional `diag_explanation` (`diagnostic.hpp`)
alongside the legacy `.message` string: `code`, `summary`, `expected`/`found`, secondary `label`s, `note`s (why) and
`help` (how to fix). `render_message()` reproduces the exact legacy one-liner; `render_full()` produces the multi-line
form. The `.message` field is unchanged, so existing tooling that greps it is unaffected.

**Deterministic specialization.** `select_impl` orders candidates by a **total** key —
`(priority desc, tiebreak desc, concrete_type_name asc)` — via `stable_sort`. `tiebreak` defaults to
`concrete_type_hash`. Only a genuine full-key tie is reported as ambiguous (`CRANK-GEN-007`); identical inputs always
pick the same winner.

**Stable metadata / ABI.** `module_link_metadata::serialize()` emits instantiations in ascending-fingerprint order (
canonical bytes), `canonical_hash()` gives an order-independent identity for a module's instantiation set,
`link_modules()` returns its merged set sorted by fingerprint, and `instantiation_registry::extend_aot_key()` folds
fingerprints in sorted order. Two builds that record the same instantiations in different orders produce identical
metadata and identical AOT keys.

**Non-goals (unchanged).** Higher-kinded types and dependent types are deliberately out of scope until a real library
requires them.

---

### §v2.1b Termination and Trait Implication

Three more production properties finish the design's type/generic system. Like §v2.1a they are additive: no existing
single-module program changes meaning, and every guard is opt-in at the call site.

**Trait implication (`Ordered ⇒ Comparable`).** A trait may *imply* a weaker bound: a type that implements `Ordered`also
satisfies a `Comparable` requirement without a separate `impl`. Implication lives on builtin traits (
`trait_descriptor::implied_bounds`, seeded in `register_builtin_traits`) and is consulted through
`trait_implies(registry, stronger, weaker)` (transitive closure over `implied_bounds`) and
`satisfies_bound(table, registry, type_hash, bound)`. `check_conformance` (`generics.hpp`) and `resolve_witnesses` (
`monomorphize.hpp`) both honor it — when no direct impl exists but an implying impl does, the witness is synthesized
from the implying impl (carrying its module/hash) while the recorded `bound` stays the *required* kind, so capability
distillation is unchanged. A direct impl always wins over an implied one (deterministic). Specialization uses the same
relation: `select_impl`'s registry-aware overload ranks a strictly stronger bound-set (`Vector[T: Ordered]`) above a
weaker one (`Vector[T: Comparable]`) *before* the numeric `(priority, tiebreak, name)` key; equal or empty bound-sets
fall back to the pre-§8.2 order exactly.

**Instantiation termination (`CRANK-GEN-014` / `CRANK-GEN-015`).** Unbounded generic expansion (
`Foo[T] → Foo[Vector[T]] → …`) is detected by `instantiation_guard` (`limits.hpp`) with two independent signals. *
*Depth:** a stack deeper than `max_nesting_depth` is `CRANK-GEN-014`. **Growth:** the *same* generic recurring on the
stack with a **strictly growing** structural size is divergence — `CRANK-GEN-015` — while a *repeated* (equal-size) key
is ordinary, terminating recursion and is allowed. Both diagnostics carry the full **expansion chain** (each active
frame rendered `generic[fp=…,size=…]`) as `diag_explanation` notes, per §14's requirement to report the chain rather
than only "limit exceeded".

**Compile-time resource limits (`CRANK-GEN-016`).** `instantiation_limits` sets generous, caller-overridable caps (no
call site hardcodes). `monomorphization_budget::charge(name)` counts monomorphizations per generic and emits
`CRANK-GEN-016` (naming the *monomorphizations* limit) past `max_monomorphizations_per_generic`;
`check_candidate_count(n, generic, limits, at)` guards a gathered candidate-impl set against `max_trait_candidates` (
`CRANK-GEN-016` naming the *trait candidates* limit). The `monomorphize_bounded(mm, guard, budget, …)` driver charges
the budget, pushes the guard, runs the plain monomorphizer, pops, and merges any limit diagnostic; a tripped limit
short-circuits so the monomorphizer never runs. Callers that do not care about limits keep calling
`monomorphizer::monomorphize` directly — existing behavior byte-for-byte.

---

## Execution Features

### §v2.7 CFG-Aware Scalar Execution

> **Status: Implemented.** The interpreter executes control-flow graphs today — conditional branches (`if`/`else`,
`match`), loops (`for`/`while`), early `return`, `defer` LIFO cleanup, runtime safety guards, `transaction` blocks, and
`break`/`continue` with defer unwinding. `branch`/`branch_cond` are first-class opcodes; the retained
`execution_status::unsupported_control_flow` enumerator is no longer produced. A branch to a nonexistent block is
> diagnosed (`branch target block id N does not exist in function`).
> See [Scalar Path + Interpreter](#scalar-path--interpreter). The only remaining gap is the interactive stepping
> backend (
> see Debugging & Introspection).

### §v2.8 Real SIMD and GPU Backends

> **Status: partially implemented.** CPU SIMD is implemented via Highway (`lithe_codegen_simd.hpp`). GPU lowering shares
> Lithe's HL-MIR device plan: Metal compiles and dispatches the currently supported f32 elementwise subset, while Vulkan/
> MoltenVK remains an opt-in provider until its headers and loader are supplied. The remainder of this entry is the target design.

At least one non-scalar backend is working per target:

**CPU SIMD:**

- Map `@simd`-annotated loops to platform SIMD intrinsics (e.g. NEON on Apple Silicon, AVX2 on x86).
- Layout constraints (`Layout[RowMajor]`) feed stride calculation.
- Fallback to scalar when lane count does not divide the range evenly (no partial-vector write outside bounds).
- NADI pulse on fallback (no silent degradation).

**GPU (Metal / Vulkan):**

- `@gpu`-annotated regions lower to Metal Compute Shaders (macOS) or Vulkan Compute (other platforms).
- Explicit device transfer ops: `device_upload(buf)`, `device_download(buf)` in the Pravaha DSL.
- Capability check: `allow_gpu = true` in `execution_options` required; hard error if `required=true` and no GPU backend
  available.
- Execution planner priority: **Metal > Vulkan > Host SIMD** (unchanged from v1 design intent).

The language reference specifies only: supported ops, layout requirements, device-transfer semantics, capability checks,
and fallback behavior. The SPIR-V/MoltenVK encoding details are backend-implementation specifics — see the non-normative
notes below.

**Fallback contract (unchanged from v1):**
Every backend fallback emits a NADI pulse; no silent performance degradation.

**Backend implementation notes (non-normative, `gpu_backend.hpp`).**
These are implementation constraints of the current SPIR-V emitter, not part of the language definition. The
hand-assembled float elementwise kernel (`out[i] = a[i] OP b[i]`, OP ∈ {add, mul}) must satisfy three
MoltenVK/SPIRV-Cross constraints or MSL conversion fails on Apple Silicon:

- Each of the three std430 storage buffers gets its **own** `Block`-decorated struct type. Sharing one struct across
  bindings makes SPIRV-Cross index MSL resource tables past their size (crash in `is_msl_resource_binding_used`).
- Type opcodes are exact: `OpTypeInt` = 21, `OpTypeFloat` = 22, `OpTypeVector` = 23. An off-by-one shifts `OpTypeFloat`
  onto a value that reads a spurious FP-encoding operand (`Unrecognized FP encoding mode for OpTypeFloat`).
- The `OpEntryPoint` name literal is NUL-terminated and word-padded: `"main"` (4 bytes) forces a whole second name
  word (`0x00000000`), else SPIRV-Cross reads into the interface id and reports `Entry point does not exist`.

### §v2.9 Structured Concurrency

Extends the `spawn`/`await` model with scoped lifetime and cancellation:

```crank
task_scope scope {
    let f1 = scope.spawn(compute_a());
    let f2 = scope.spawn(compute_b());
    // scope exit: join all child tasks; propagate first failure
}
```

**New constructs:**

| Construct                  | Semantics                                       |
|----------------------------|-------------------------------------------------|
| `task_scope scope { … }`   | Scoped task group; all children join at `}`     |
| `scope.spawn(expr)`        | Bounded spawn — child lifetime ≤ scope lifetime |
| `scope.cancel()`           | Signal cancellation to all children             |
| `deadline(duration) { … }` | Cancel scope if wall time exceeds duration      |
| `join_group`               | Wait for a dynamic set of futures               |

**Failure propagation:** first child failure cancels remaining siblings; the scope exit propagates the error to the
enclosing `Result`.

**Cancellation:** `crank_future_error::cancelled` is the mechanism; `scope.cancel()` signals it to all un-awaited
children.

**New ownership rules:**

- `scope.spawn` captures by value (same as `spawn`).
- A `task_scope` must not outlive its enclosing function — enforced at compile time by scope-lifetime checking (
  analogous to borrow-check in scope level).

### §v2.10 Distributed Execution

Opt-in distribution via explicit placement and serialization:

```crank
@distributed(placement = remote_node)
fn compute(data: []Float32) -> Float32 { ... }
```

**New concepts:**

| Concept                              | Description                                                          |
|--------------------------------------|----------------------------------------------------------------------|
| `remote_future[T]`                   | Future whose result lives on a remote node                           |
| `placement`                          | Named node or node group; resolved by the execution planner          |
| `serialization_boundary`             | Explicit marker that data crosses a process boundary                 |
| `retry(n, replay = body_idempotent)` | Remote retry policy (same as tx retry, applied to distributed calls) |

**Policy:** distribution is **never silently selected**. A function is distributed only when `@distributed(...)` is
present. The execution planner emits a NADI pulse when a placement constraint is relaxed.

**Hard vs soft placement (required/preferred).** A distributed placement is a *preference* by
default — like `@parallel`/`@gpu`, an unmet placement falls back to local execution. A remote
function must **not** silently become local when that changes latency, isolation, or
data-residency guarantees, so `required=true` promotes the miss to an error:

```text
@distributed(required=true)  → missing adapter / unplaceable is an error (CRANK-DIST-003);
                               the task does NOT run local — the remote_future carries the error.
@distributed(preferred=true) → local fallback allowed with a NADI pulse (CRANK-DIST-001).
```

`preferred` is the default (`placement_mode::preferred`); `required` maps to `placement_mode::required`.
`spawn_remote(where, fn, mode)` (and the adapter overload) take the mode; `remote_future::required_unmet()`
reports a `required` miss and `await()` yields an error rather than a local result.

**v2 `distribution` field:** `transaction(distribution = local | shard | replicated)` — `local` is the only non-gated
mode; `shard`/`replicated` require an explicit distributed adapter.

**C++ surface (`include/languages/crank/distributed.hpp`):** `spawn_remote(placement, fn[, mode])` /
`spawn_remote(adapter, placement, fn[, mode])` return a `remote_future<T>` (same await/detach consume discipline as
`crank_future<T>`); `resolve_tx_distribution(mode, adapter_available)` gates the tx distribution field.

**Diagnostics:**

| Code             | Condition                                                                                              |
|------------------|--------------------------------------------------------------------------------------------------------|
| `CRANK-DIST-001` | placement relaxed to local (no adapter, `preferred`) — NADI pulse, not fatal                           |
| `CRANK-DIST-002` | payload type is not serialization-boundary-safe (ran local)                                            |
| `CRANK-DIST-003` | `required` placement could not be honored (no adapter / `can_place` false) — error, does not run local |
| `CRANK-DIST-004` | `retry(n)` with a non-replay-safe body                                                                 |
| `CRANK-DIST-010` | `shard`/`replicated` distribution requested with no distributed adapter                                |

**What stays out of v2:** implicit distributed transactions, automatic data migration, consensus protocols. Those are
v3.

---

## Transaction Features

### §v2.11 Multi-Resource Transactions

Lift `CRANK-TX-002` for transactions with an explicit coordinator:

```crank
transaction(serializable, coordinator = my_coordinator) {
    let bal_a = accounts[from];
    let bal_b = balances[to];    // second transactional resource — now legal
    accounts[from] = bal_a - amount;
    balances[to]   = bal_b + amount;
}
```

**Rules:**

- `coordinator` argument names a registered `multi_resource_coordinator` (C++ registered via
  `ctx.register_coordinator<C>("name")`).
- Without `coordinator`, `CRANK-TX-002` still fires for `serializable` + >1 resource (v1 behavior preserved).
- `snapshot` isolation across multiple resources — the multi-resource write rule:

  ```text
  multi-resource snapshot without coordinator:
      reads may be consistent (a single snapshot across resources);
      writes to >1 distinct resource are a diagnostic (CRANK-TX-012);
      atomic multi-resource commit requires a coordinator.
  ```

  Consistent multi-resource *reads* under `snapshot` remain allowed without a coordinator (v1
  behavior preserved). Multi-resource *writes* do **not** commit atomically under bare `snapshot` —
  each resource would commit independently — so they are rejected (`CRANK-TX-012`) unless a
  `coordinator` provides the atomic-commit path. Single-resource `snapshot` writes are unaffected.
- The coordinator is responsible for 2PC or equivalent; Crank wraps and policy-checks; Medha executes per-resource.

**New diagnostic:**

| Code           | Condition                                                                                                                |
|----------------|--------------------------------------------------------------------------------------------------------------------------|
| `CRANK-TX-010` | `coordinator` argument present but a participant resource is not transactional                                           |
| `CRANK-TX-011` | `coordinator` name not registered on the context                                                                         |
| `CRANK-TX-012` | `snapshot` isolation writing >1 distinct transactional resource with no `coordinator` (non-atomic multi-resource commit) |

**Diagnostic-code ownership (permanent — codes are never reused for a second meaning):**

- `CRANK-TX-009` = the v1 `distribution != none` gate (§v2.10 lifts *when* it fires, not its meaning).
- `CRANK-TX-010` = a coordinator participant resource is non-transactional.
- `CRANK-TX-011` = a declared coordinator name is unregistered.
- `CRANK-TX-012` = snapshot multi-resource write without a coordinator.

Rollback/staging-capability failures are **not** `CRANK-TX-009`; they carry their own codes
(`CRANK-TX-001`/`-003`/`-008` for non-transactional write, missing snapshot capability, and
irreversible effect respectively). Every diagnostic code has exactly one permanent meaning.

### §v2.12 Nested Transactions and Savepoints

```crank
transaction(serializable) {
    let sp = savepoint();               // create savepoint
    accounts[from] = accounts[from] - amount;
    if error_condition {
        rollback_to(sp);                // partial rollback to savepoint
    }
    accounts[to] = accounts[to] + amount;
}
```

**Constructs:**

| Construct                       | Semantics                                                                                             |
|---------------------------------|-------------------------------------------------------------------------------------------------------|
| `savepoint()` → `Savepoint`     | Capture current write set as a named restore point                                                    |
| `rollback_to(sp)`               | Undo all writes since `sp`; read set retained                                                         |
| Nested `transaction { }` blocks | Inherit parent isolation; create implicit savepoint on entry; rollback to savepoint on inner failure  |
| `compensate { }`                | Register a **post-commit** compensating action (see contract below) — a second action, not a rollback |

**`compensate` contract.** A `compensate` block is a *post-commit hook*: it runs **after** the enclosing transaction has
already committed, to best-effort undo an externally-visible effect (e.g. issue a refund for a charge). It is **not** a
rollback and cannot create a hidden second consistency model. Rules:

- **Not guaranteed to run** — best-effort, executed post-commit; a crash between commit and compensation may skip it.
- **Retryable, bounded** — retried up to a fixed `retry_limit` (≥ 1); no unbounded loops.
- **Must be idempotent** — required. Running it more than once (via retry) must be safe. A non-idempotent compensation
  is rejected at registration.
- **Must not perform irreversible effects** — reuses the §7c.5 irreversible-effect rule; a compensation that would
  perform an irreversible effect is a diagnostic.
- **Failure is isolated** — on final failure (retries exhausted) the outcome is surfaced as a separate
  `CompensationError`. It **never** rolls the committed transaction back; the commit stands.

Runtime model: `compensation_registry` (`tx_savepoint.hpp`) records post-commit compensations; `run_all(sink)` invokes
each with bounded retries and returns a `compensation_report` (`ran` / `failed` counts).

**Semantics of nested failure:** inner `transaction` failure rolls back to the inner entry savepoint; the outer
transaction may continue or propagate the `TxError` depending on whether the inner result is handled.

### §v2.13 Transactional Collections

Typed transactional containers registered as first-class resources:

| Type          | Semantics                                        |
|---------------|--------------------------------------------------|
| `TxMap[K, V]` | Key-value store with serializable get/put/delete |
| `TxSet[T]`    | Membership store with add/remove/contains        |
| `TxQueue[T]`  | FIFO with transactional enqueue/dequeue          |
| `TxLog[T]`    | Append-only transactional log                    |

All implement `medha::resource_traits<R>` with `transactional = true`. Registration:

```cpp
ctx.register_transactional<TxMap<AccountId, Balance>>("accounts");
```

Inside a `transaction` block, the typed API replaces raw `resource[key]` indexing:

```crank
transaction {
    let bal = accounts.get(from);    // TxMap typed read
    accounts.put(from, bal - amount) // TxMap typed write
}
```

---

## Tooling and Artifact Features

### §v2.14 Separate Compilation

Generic instantiations are cached and linked across modules with stable metadata:

- Each instantiation has a stable `cache_key_fingerprint` (FNV-1a over all type and const args).
- Cross-module instantiation deduplication: if two modules produce `Reduce[Int64]`, the linker uses one.
- ABI checks: the `native_abi_hash` in `crank_aot_key` gates linkage; ABI mismatch → `CRANK-LINK-001`.
- Instantiation metadata exported as a compact binary record (parallel to the existing AOT artifact).

### §v2.15 Better AOT Artifacts

Extend `aot_security_policy` and the artifact format:

| Addition                  | Description                                                                  |
|---------------------------|------------------------------------------------------------------------------|
| Signed manifest           | Covers all capability declarations + resource hashes in addition to the body |
| Relocation records        | Allow the loader to fix up absolute addresses without recompilation          |
| Capability declarations   | Explicit list of backend capabilities the artifact requires                  |
| Version negotiation       | `min_runtime_version` / `max_runtime_version` fields in the artifact header  |
| Secure loader integration | `aot_security_policy::allow_relocation` gate; off by default                 |

**New artifact header fields:**

```cpp
struct aot_artifact_header_v2 {
    uint32_t  magic;                 // kAotArtifactMagicV2 ("CAO2")
    uint32_t  min_runtime_version;
    uint32_t  max_runtime_version;
    uint32_t  relocation_count;
    uint64_t  capability_mask;
    uint64_t  manifest_sig_offset;   // offset to manifest signature envelope (0 = absent)
    uint64_t  reflection_layout_hash;// §v2.16 layout_fingerprint bound into the artifact
    bool version_in_range(uint32_t runtime_version) const;
};
```

The manifest signature at `manifest_sig_offset` uses the **same length-prefixed signature envelope** as the artifact
footer (§ AOT security: `u16 sig_len || sig_bytes`) — there is no second encoding rule.

Validation entry point: `validate_aot_view_v2(key, bytes, header, runtime_version, policy)` runs all v1
`validate_aot_view` checks, then gates on the header's runtime-version window (SEC-008), relocation/executable-memory
opt-in (SEC-005), and the untrusted capability allowlist (SEC-006).

Separate-compilation linkage lives in the same header: `module_link_metadata` (serializable instantiation records) +
`link_modules(modules)` → `link_result` (dedups identical instantiations, emits `CRANK-LINK-001` on ABI clash).

New diagnostic:

| Code                | Condition                            |
|---------------------|--------------------------------------|
| `CRANK-AOT-SEC-008` | Runtime version outside `[min, max]` |
| `CRANK-LINK-001`    | Cross-module ABI hash mismatch       |

### §v2.16 Reflection and Generated Adapters

> **Status: Implemented (program-built).** Reflection descriptors are built by the program today (`@reflect` parses via
> the generic `@attribute` rule). Grammar-level `@reflect` keyword support and the full adapter-generation surface below
> remain the target design.

Restricted reflection for host registration, serialization, and backend adapters — no arbitrary runtime type inspection:

```crank
@reflect(fields, traits, capabilities)
type Particle = struct {
    px: Float32
    py: Float32
    pz: Float32
}
```

Reflection scopes:

| Scope               | What is exposed                                                       |
|---------------------|-----------------------------------------------------------------------|
| `fields`            | Field names, types, offsets — for serialization and host registration |
| `traits`            | Satisfied trait set at the call site — for generic dispatch           |
| `capabilities`      | Effect/capability mask — for backend adapter selection                |
| `host_registration` | Auto-generate `crank::type_descriptor<T>` from field list             |
| `backend_adapters`  | Auto-generate typed GPU/SIMD struct layouts                           |

**Not exposed by v2 reflection:** arbitrary runtime type queries, code generation beyond layout/adapters, or
modification of the type registry at runtime. Those are v3.

**Layout stability (field offsets are context-bound).** A reflected field offset is only meaningful
under a specific target layout. Every `type_descriptor<T>` carries a `layout_context`
(`target_abi_hash`, `packing`, `alignment`, `endianness`, `layout_version`); by default it is the
native context of `T` in the compiling process. Two descriptors are only interchangeable when their
`layout_context` values are identical (`reflection_matches(artifact, current)`).

`type_descriptor<T>::layout_fingerprint()` hashes the layout context **and** every field offset, so
any change to ABI, packing, alignment, endianness, layout-policy version, or a field offset produces
a different fingerprint. An AOT artifact that embeds reflected offsets records this fingerprint in
`aot_artifact_header_v2::reflection_layout_hash`; `validate_aot_view_v2(..., current_reflection_layout_hash)`
emits **`CRANK-AOT-SEC-009`** when the artifact's fingerprint does not match the current layout —
reflected offsets never silently leak an unstable layout across an ABI/packing/endianness/version
change; the artifact must be recompiled.

---

## Lean Charter — Feature Placement

Per the Crank Lean Charter Final Addendum, the following table defines where each feature lives:

| Feature                                           | Placement             | Reason                                                              |
|---------------------------------------------------|-----------------------|---------------------------------------------------------------------|
| `for`, `if`, `match`, `defer`                     | Grammar               | Structured control flow                                             |
| `transaction`                                     | Grammar               | Atomicity/isolation/rollback contract                               |
| `spawn`, `await`, `?`                             | Grammar / operators   | Explicit asynchronous and failure control flow                      |
| `requires`, `ensures`, `assert`                   | Grammar               | Correctness contracts                                               |
| `forall`, `exists`, `result`, `old`               | Predicate context     | Verification expressiveness without global keywords                 |
| `as`                                              | Operator              | Explicit checked conversion boundary                                |
| `len`                                             | Grammar builtin       | Sole grammar-level length builtin (lean charter §7)                 |
| `cap`, `append`, `make`, `indices`                | Prelude function      | Parse as plain call expressions                                     |
| `print`                                           | Host/prelude module   | Capability-controlled I/O; not core grammar                         |
| `parallel.map`, `parallel.reduce`, `parallel.for` | Prelude function      | Execution planning expressed in source; not grammar builtins        |
| SIMD, task graph, GPU transfer                    | Compiler IR / backend | Execution planning, not source scheduling                           |
| `FnMut`, `FnOnce` syntax                          | Inferred by sema      | Callability class derived from capture analysis; not user-writeable |

**Core operators** (present in grammar surface):

```
..  ..=   range (exclusive / inclusive)
as        checked type conversion
?         postfix error-propagation
```

**Contextual forms** (special meaning only inside their scope; do not consume global keyword space):

```
abort     transaction body only
forall    predicate only
exists    predicate only
result    ensures only
old(...)  ensures and transaction snapshot only
```

---

## Lean Charter — Acceptance Criteria

These criteria gate the Crank Lean Charter freeze. All must be satisfied before the charter is considered complete.

| #  | Criterion                                                                                                                                             |
|----|-------------------------------------------------------------------------------------------------------------------------------------------------------|
| 19 | Quantified predicates lower to canonical proof terms and reject effectful bodies.                                                                     |
| 20 | `result` and `old` are valid only in their documented contextual scopes.                                                                              |
| 21 | `?` propagates only compatible `Result` or `Option` residuals and produces clear conversion diagnostics.                                              |
| 22 | Checked `as` conversions either prove safety, insert a policy-controlled guard, or reject invalid conversions.                                        |
| 23 | Closure capture classification is deterministic and prevents escaping borrowed state.                                                                 |
| 24 | `spawn` rejects captures that violate thread-safety, lifetime, or move-capture rules.                                                                 |
| 25 | Transaction syntax is justified and documented as atomicity/isolation semantics, not merely shared-write detection.                                   |
| 26 | Execution-plan diagnostics explicitly explain why `@parallel`/`@simd`/`@gpu` preferences were not honored (release gate — see §Backend Intelligence). |

**Diagnostic codes for new surface:**

| Code              | Feature | Condition                                 |
|-------------------|---------|-------------------------------------------|
| `CRANK-Q-001`     | `?`     | Applied to non-`Result`/`Option`          |
| `CRANK-Q-002`     | `?`     | No `From[E]` for residual conversion      |
| `CRANK-Q-003`     | `?`     | Enclosing return incompatible             |
| `CRANK-Q-004`     | `?`     | Inside `pred_expr`                        |
| `CRANK-Q-005`     | `?`     | Crossing tx/async boundary without policy |
| `CRANK-CLOS-001`  | closure | Escaping borrow capture                   |
| `CRANK-SPAWN-001` | `spawn` | Non-transfer-safe capture                 |

---

## Lean Charter — Final Freeze Criterion

The Crank Lean Charter is frozen when **all** of the following are true:

```
governing syntax rule adopted
layer separation documented and enforced
core grammar fixed for the language edition
contextual predicate forms specified (forall/exists/result/old)
error propagation and conversion semantics explicit (?, as, try_from)
closure capture rules explicit (copy/move/reject-escaping-borrow)
transaction semantics stated in terms of atomicity/isolation
migration diagnostics exist for removed syntax
execution-plan explanation is a testable release gate (criterion 26)
```

Future language additions are evaluated against:

1. Does the feature express a semantic contract, requirement, safety boundary, or uninferable value?
2. Which layer owns it?
3. Can it be a library API, host capability, annotation, or IR concept instead?
4. Does it preserve the lean user model?
5. Can its behavior be tested through an acceptance criterion?

If those questions do not justify grammar, the feature must not become syntax.

---

## Explicit Non-Goals

| Feature                                        | Reason deferred                                                            |
|------------------------------------------------|----------------------------------------------------------------------------|
| Unrestricted higher-kinded types               | Requires variance analysis beyond current scope                            |
| Dependent types                                | Expressive power exceeds current proof infrastructure                      |
| Arbitrary compile-time metaprogramming         | Scope creep; only practical dimensions are covered                         |
| Unrestricted specialization                    | Coherence complexity; controlled specialization (§v2.5) is sufficient      |
| Implicit distributed transactions              | Correctness risk; opt-in (§v2.10) is the right default                     |
| Dynamic dispatch in performance-critical loops | Zero-overhead design principle; witness objects at dynamic boundaries only |

---

## See Also

- `docs/languages/crank/grammar.md` — full grammar
- `include/languages/crank/` — all headers
- `src/examples/crank/example_crank.hpp` — comprehensive 55-exercise tutorial
  (ex01–ex30 per-stage; ex31–ex39 end-to-end pipeline runs; ex40–ex46
  performance and data-structure comparisons; ex47–ex55 domain views)
