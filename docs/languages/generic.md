# Generic Language Layer — Framework Reference

`pebble/include/languages/generic/` provides a language-neutral foundation for building embeddable domain-specific languages (
DSLs) and compiler frontends. Designed for C++23, header-only, zero virtual functions, zero macros. All abstractions are
pluggable; no forced coupling.

**Namespace:** `lang::`  
**Zero dependencies:** Core layer uses only `<cstdint>`, `<string_view>`. Host/semantic layers depend on related
facilities (identity, diagnostics) and vakya for effect/capability systems.

---

## Table of Contents

1. [Purpose & Design Goals](#purpose--design-goals)
2. [Architecture Overview](#architecture-overview)
3. [Core Layer (`core/`)](#core-layer-core)
4. [Tree Substrate (`tree/`)](#tree-substrate-tree)
5. [Algorithms Used](#algorithms-used)
6. [Generic IR Layer (`ir/`)](#generic-ir-layer-ir)
6. [Host Layer (`host/`)](#host-layer-host)
7. [Module Layer (`module/`)](#module-layer-module)
8. [Semantic Layer (`semantic/`)](#semantic-layer-semantic)
9. [AST Layer (`ast/`)](#ast-layer-ast)
10. [Lexer Layer (`lexer/`)](#lexer-layer-lexer)
11. [Integration: Crank Example](#integration-crank-example)
12. [Extension Points](#extension-points)
13. [Best Practices](#best-practices)
14. [File Reference](#file-reference)
15. [Examples](#examples)
16. [See Also](#see-also)

---

## Purpose & Design Goals

### Why Generic?

Production language frameworks duplicate infrastructure: module systems, symbol tables, diagnostics, descriptors.
Generic centralizes this:

- **Reusable across languages.** Crank, Sutra, custom frontends — all build on generic.
- **Zero overhead.** Pluggable policies (visibility rules, resolver config, symbol policies) configure behavior without
  runtime cost.
- **Extension-friendly.** Effect bands, capability masks, kind constants use open enums; languages extend without
  collision.
- **AI/ML ready.** Stable entity IDs, fingerprints, and descriptor registries enable reproducible artifact generation
  and caching.

### Design Principles

| Principle               | Implication                                                                                         |
|-------------------------|-----------------------------------------------------------------------------------------------------|
| Language-agnostic       | Generic layer knows nothing about syntax, semantics, or type systems.                               |
| Pluggable policies      | Visibility rules, resolver behavior, symbol metadata configurable by template or init.              |
| Flat, stable structures | No virtual functions. Deterministic hashing. Stable IDs for cross-version identity.                 |
| Pay-for-use             | Link only needed pieces: `core/*` for primitives, `host/*` for descriptors, `module/*` for imports. |

---

## Architecture Overview

### Layer Hierarchy

```
pebble/include/languages/generic/
│
├─ generic.hpp                  (umbrella include)
│
├─ core/                        (zero-dependency primitives)
│  ├─ identity.hpp             (stable IDs, FNV-1a hashing, fingerprints)
│  ├─ diagnostics.hpp          (severity, source_span, diagnostic_sink)
│  ├─ reflection.hpp           (callable_traits, typed thunk builder)
│  ├─ source_location.hpp       (source position, column tracking)
│  ├─ rich_diagnostic.hpp       (source-annotated diagnostics with context)
│  └─ parse_stats.hpp           (parse time/memory metrics)
│
├─ tree/                        (generic CST substrate)
│  ├─ spans.hpp                (byte_span, token_range, text_edit)
│  ├─ event_log.hpp            (event_kind, parse_event<KE,DC>, event_log<KE,DC>)
│  ├─ green_arena.hpp          (green_node<KE>, green_arena<KE> — flat CST arena)
│  └─ static_buffers.hpp       (static_event_buffer<KE,DC,N>, static_span_buffer<T,N>)
│
├─ ir/                          (generic IR layer)
│  ├─ node.hpp                 (ir_node<KE,ExtPayload=monostate>, ir_node_id, symbol_id)
│  ├─ ir_module.hpp            (ir_module<KE,EP,Store>, ir_adjacency_view)
│  ├─ interning.hpp            (ir_interner — name interning + hash-cons dedup)
│  └─ lowering.hpp             (lower_events<KE,EP>() — event_log → ir_module funnel)
│
├─ host/                        (registration & descriptors)
│  ├─ descriptors.hpp          (function/type/field/resource base types)
│  ├─ effects.hpp              (re-export + extend vakya effect/cap)
│  └─ registry.hpp             (descriptor lookup & discovery)
│
├─ module/                      (file & artifact resolution)
│  ├─ module_system.hpp        (module descriptor, kind, version, config)
│  └─ import_resolver.hpp      (9-tier resolver, dependency graph, cycles)
│
├─ semantic/                    (scoping & constraints)
│  ├─ symbol_table.hpp         (scope stacks, pluggable visibility)
│  ├─ rules.hpp                (language-defined rules & constraints)
│  └─ proof.hpp                (proof obligations & verification)
│
├─ ast/                         (storage)
│  └─ ast_arena.hpp            (flat index-based node arena)
│
├─ lexer/                       (lexer utilities)
│  └─ digit_sep.hpp            (numeric literal handling)
│
└─ samasa/                      (DEPRECATED path — compat shim only; see languages/samasa/)
   └─ samasa.hpp               (forwarder to languages/samasa/samasa.hpp)
```

### Umbrella Header

```cpp
#include "languages/generic/generic.hpp"  // Full layer (all submodules)
```

For minimal compilation, include only what you need:

```cpp
#include "languages/generic/core/identity.hpp"      // Just IDs
#include "languages/generic/module/module_system.hpp"  // Just modules
```

---

## Core Layer (`core/`)

Foundation primitives with zero external dependencies.

### `identity.hpp` — Stable Entity Identifiers

**Purpose:** Cross-compile-stable identification for entities (functions, types, modules, rules, resources). Enables
reproducible caching, deterministic artifact generation, version-independent reference integrity.

**Key Types:**

| Type                     | Purpose                                                                                                                            |
|--------------------------|------------------------------------------------------------------------------------------------------------------------------------|
| `stable_entity_id`       | 128-bit deterministic ID (namespace_hash, name_hash, kind, schema_version). Invariant across recompilation if name/kind unchanged. |
| `descriptor_fingerprint` | uint64_t hash of observable descriptor state. Changes on modification → enables change detection.                                  |
| Kind constants           | `kKindFunction`, `kKindType`, `kKindField`, `kKindResource`, `kKindBackend`, `kKindContainer`, `kKindRule`, `kKindModule`          |

**Hash Algorithm:** FNV-1a (constexpr, deterministic, cross-platform).

**Usage Examples:**

```cpp
// Create stable ID from qualified name
constexpr auto id = lang::detail::make_id("math.dot", lang::kKindFunction);
// → namespace_hash("math"), name_hash("dot"), kind=1, schema_version=1

// Combine fingerprints (XOR + rotate; no symmetry collapse)
auto fp_combined = lang::detail::fp_combine(fp1, fp2);

// Hash a string to fingerprint
auto fp_str = lang::detail::fp_from_string("Dot");

// Combine fingerprint with scalar (arity, flags, etc.)
auto fp_with_arity = lang::detail::fp_with_scalar(fp_base, 2);
```

**Benefit:** Same qualified name always → same ID. Enables reproducible cache keys, incremental compilation,
cross-version artifact compatibility.

---

### `diagnostics.hpp` — Diagnostic Infrastructure

**Purpose:** Language-neutral severity and diagnostic reporting; extensible sink interface.

**Key Types:**

```cpp
enum class severity { error, warning, note, remark };

struct source_span {
    std::uint32_t line, column;           // start
    std::uint32_t line_end, column_end;   // end
};

struct diagnostic {
    severity level;
    std::string message;
    source_span span;  // optional
};

// Concept: diagnostic_sink<S>
// Implementations: collecting_sink, nadi_sink (observability)
```

**Usage:**

```cpp
lang::diagnostic diag;
diag.level = lang::severity::error;
diag.message = "Type mismatch: expected int, got string";
diag.span = lang::source_span{10, 5, 10, 15};

lang::collecting_sink<lang::diagnostic> sink;
sink.report(diag);
```

---

### `reflection.hpp` — Callable Traits & Typed Thunks

**Purpose:** Extract metadata from C++ function pointers/lambdas; generate type-safe invocation thunks.

**Key Functions:**

- `callable_traits<Fn>` — Metaprogram extracting arity, parameter types, return type.
- `make_typed_thunk<Fn>()` — Generate thunk casting arguments from void* to correct types.

**Used by `make_function_descriptor<>`:**

```cpp
auto fd = lang::make_function_descriptor<"math.dot", &my_dot_fn>();
// Automatically derives: arity, typed_thunk (no std::any overhead), id, fingerprint
```

---

### `source_location.hpp` — Source Positions

Tracks precise code locations (line, column) for diagnostics, debugging, profiling.

---

### `rich_diagnostic.hpp` — Annotated Diagnostics

Diagnostic + context lines (source_line, caret position, snippet). Human-friendly error reporting with visual
indicators.

---

### `parse_stats.hpp` — Parser Metrics

Collect parse time (clock), memory allocation. Observability for frontend bottlenecks.

---

## Tree Substrate (`tree/`)

Shared CST + event log layer for all language frontends. Zero samasa dependency — samasa aliases these in Stage 2/3.

### `spans.hpp` — Span Primitives

**Consumers:** samasa's `byte_span`, `text_edit`, and `token_range` are aliases of these generic primitives (single owner, no duplication). All three resolve to `lang::byte_span`, `lang::text_edit`, and `lang::token_range` respectively.

| Type | Description |
|------|-------------|
| `byte_span` | `{offset, length}` in source bytes. `hull(a,b)` = minimal covering span (empty-operand identity). |
| `token_range` | Half-open `[start, end)` index range into any token array. |
| `text_edit` | Source mutation: replace `[offset, offset+removed_length)` with `inserted_text`. |

```cpp
lang::byte_span a{0, 5}, b{3, 7};
auto h = lang::byte_span::hull(a, b); // {0, 10}
```

### `event_log.hpp` — Generic Parse-Event Log

Generalizes `samasa::event_stream`. Parameterized on `KindEnum` (node kind) and `DiagCode` (default `uint16_t`).

```cpp
enum class MyKind : uint8_t { root, inner };
lang::event_log<MyKind> log;
auto m = log.begin(MyKind::root);
log.token(0);
log.end(m, {0, 10});
```

**Rollback semantics:** `rollback(m)` truncates if no tokens were committed after `m`; tombstones the `begin_node` otherwise (samasa `event_stream` parity).

**`insert_begin_at(marker, kind)` — retroactive node-open for operator/Pratt trees:**
Inserts a `begin_node(kind)` event at the position recorded in `marker`, shifting all subsequent events right by one slot. Returns a marker pointing to the newly inserted event. Use when parsing an infix or postfix operator after its left operand has already been emitted — snapshot before the operand, then call `insert_begin_at` once the operator is recognized.

```cpp
auto pre_left = log.snapshot();
log.token(0);                                    // left operand
// ... operator recognized ...
auto m = log.insert_begin_at(pre_left, MyKind::binary_expr);
log.token(1);                                    // operator
log.token(2);                                    // right operand
log.end(m, {0, 3});                              // closes binary_expr with hull span
```

O(events-after-insertion) due to the vector shift. Pratt sub-expressions are small so this is
effectively O(1) in practice. Not suitable for very deep left-recursive rewrites at the top of a
large event stream.

**samasa mapping (Stage 3):**

| samasa type | generic type | note |
|---|---|---|
| `samasa::event_stream<SK>` | `lang::event_log<SK, samasa_diag_code>` | alias; single owner |
| `samasa::parse_event<SK>` | `lang::parse_event<SK, samasa_diag_code>` | alias; field `syntax` == `node_kind` (union) |
| `samasa::event_kind` | `lang::event_kind` | alias |

### `green_arena.hpp` — Generic Flat CST Arena

`green_arena<KE>` is layout-identical to `ir_module<KE, monostate>` — Stage 3 adoption is zero-copy.

```cpp
auto arena = lang::green_arena<MyKind>::build(
    log,
    [&](uint32_t i) { return tokens[i].span(); },   // leaf_span_fn
    [&](uint32_t i) { return fnv1a(source, i); });  // leaf_hash_fn
```

`splice_subtree` and `recompute_ancestor_hashes` are implemented (Stage 6) as generic partial-reparse
primitives.

**`splice_subtree(at, sub)`** — replace the subtree rooted at `at` with the nodes from `sub`.
Appends `sub`'s nodes (remapping child-id offsets), overwrites node `at` with the remapped sub-root.
Old subtree slots become holes (reclaimed by a future `reset()`/rebuild — pay-for-use).

**`recompute_ancestor_hashes(from)`** — walk from `from` to the root via a transient parent map
(O(n) build, O(1) lookup, discarded after the call). At each ancestor recomputes `structural_hash`
using the same FNV recipe as `build()`, so hashes are bit-for-bit identical to a full rebuild.
The parent map is not stored in nodes (pay-for-use; bloat-free normal use).

**samasa mapping (Stage 3):**

| samasa type | generic type | note |
|---|---|---|
| `samasa::green_tree<SK>` | `lang::green_arena<SK>` | inherits; same layout |
| `samasa::green_node<SK>` | `lang::green_node<SK>` | alias |
| `samasa::green_id` | `lang::arena_id` | alias (`uint32_t`) |
| `samasa::k_null_green` | `lang::k_null_arena` | alias (`UINT32_MAX`) |
| samasa CST | `ir_module<SK, std::monostate>` | layout identical; structural hashes byte-for-byte unchanged (FNV recipe) |

### `static_buffers.hpp` — Constexpr Parse Buffers

`static_event_buffer<KE, DC, MaxEvents>` — same `begin/token/end/rollback/snapshot` API as `event_log`, but backed by `containers::static_vector` for `consteval` use.

```cpp
constexpr auto result = [] {
    lang::static_event_buffer<MyKind, uint16_t, 64> buf;
    auto m = buf.begin(MyKind::root);
    buf.token(0);
    buf.end(m);
    return buf.event_count();
}();
static_assert(result == 3);
```

`static_span_buffer<T,N>` = alias for `containers::static_vector<T,N>` (readability at samasa call-sites).

---

## Algorithms Used

Concrete named algorithms in the framework, with the layer they live in.

| Concern | Algorithm | Where |
|---|---|---|
| Parse capture | Append-only `event_log` (begin/token/end event stream); `static_event_buffer` fixed-capacity `constexpr` variant | `tree/event_log.hpp`, `tree/static_buffers.hpp` |
| Tree materialization | `green_arena::build` folds an event log into an immutable green tree (shared subtree nodes) | `tree/green_arena.hpp` |
| IR lowering | Event-log → IR module lowering (flat SoA node store + child sidecar) | `ir/lowering.hpp`, `ir/ir_module.hpp` |
| Module resolution | 9-tier resolver order: native → embedded_artifact → embedded_src → in_memory → project_paths → app_paths → cache → system → package_registry | `module/module_system.hpp` |
| Semantic scaffolding | Generic scope/symbol resolution + diagnostic accumulation | `semantic/` |
| Inline storage | SBO buffers via `containers::static_vector` (span/token buffers) | `containers/static_vector.hpp` |

---

## Generic IR Layer (`ir/`)

Reusable IR substrate on top of the tree layer. All optimization hooks are opt-in and zero-cost when unused.

### Extension seams (simple → complex ladder)

| Seam | Meaning |
|------|---------|
| `KindEnum` | Node kind enum — per-language (e.g. `samasa::syntax_kind`, `crank::ast_kind`) |
| `ExtPayload = monostate` | Zero extra bytes — CST / untyped AST frontends |
| `ExtPayload = type_ref` | Typed AST (vakya, crank Stage 8) |
| `Store = default_store` | `std::vector` heap backing |
| `Store = handle_store<Tag>` | `slot_map<ir_node, generational_handle<Tag,uint32_t>>` — realized in Stage 9; vakya uses `handle_store<type_tag>` → `type_ref` |
| `Dedup` policy | `ir_interner` structural hash-consing (opt-in); `kosha_dedup_adapter` satisfies the seam for vakya (reuses `type_intern_cache_t`) |

### `node.hpp` — IR Node

```cpp
template <class KindEnum, class ExtPayload = std::monostate>
struct ir_node {
    KindEnum      kind;
    byte_span     span;
    ir_node_id    first_child;
    uint32_t      child_count;
    uint64_t      structural_hash;
    symbol_id     name;      // InternPool-backed; k_null_symbol = none
    ExtPayload    ext;       // zero bytes when monostate
};
```

### `ir_module.hpp` — Module Storage

```cpp
lang::ir_module<MyKind> mod;
auto id = mod.push(node);
mod.append_children(id, kids_span);
auto adj = mod.as_egraph_view();   // feeds egraph.hpp
auto cfg = mod.as_adjacency();     // feeds DominatorTree / LiteGraph
mod.reset();                       // clear, keep capacity
```

Layout identical to `green_arena<KE>` — Stage 3 adoption is zero-copy.

### `interning.hpp` — Name Interning + Hash-Consing

```cpp
lang::ir_interner interner;
auto sid = interner.intern_name("foo");     // stable symbol_id
auto nid = interner.dedup(hash, node_id);  // returns first id for that hash
```

### `lowering.hpp` — Event Log → IR Module

Single funnel callable for any frontend:

```cpp
struct LeafFns {
    lang::byte_span span(uint32_t i) const;
    uint64_t        hash(uint32_t i) const;
};
auto mod = lang::lower_events<MyKind>(log, LeafFns{});
```

**Mapping table (realized in later stages):**

| Frontend | KindEnum | ExtPayload | Store |
|----------|----------|------------|-------|
| samasa CST | `syntax_kind` | `monostate` | `default_store` |
| crank AST (Stage 8) | `crank_kind` | `crank_node_ext` | `default_store` |
| vakya types (Stage 9) | `type_ir_kind` | `type_ref` | `handle_store<type_tag>` (slot_map/generational_handle/LinearArena, kosha intern) |

---

## Host Layer (`host/`)

Registration and descriptor abstractions for embedding C++ functions/types.

### `descriptors.hpp` — Function/Type/Field/Resource Base Types

**Purpose:** Language-agnostic descriptor base classes. Languages extend these with custom fields (source location,
effects, etc.).

**Base Types:**

```cpp
struct function_descriptor_base {
    stable_function_id id;
    std::string name;                              // "math.dot"
    std::size_t arity = 0;
    std::uint64_t effect_mask = 0;                 // language-defined
    std::uint64_t capability_mask = 0;             // language-defined
    function_flags flags = 0;
    void (*typed_thunk)(const void* const*, void*) = nullptr;  // fast path
    std::function<std::any(std::span<const std::any>)> trampoline;  // slow path
    descriptor_fingerprint fingerprint = 0;
};

struct type_descriptor_base {
    stable_type_id id;
    std::string name;
    std::size_t byte_size = 0;
    std::size_t alignment = 0;
    descriptor_fingerprint fingerprint = 0;
};

struct field_descriptor_base {
    stable_field_id id;
    std::string name;
    std::string type_name;     // qualified type name
    std::size_t offset = 0;
    std::size_t byte_size = 0;
    bool is_mutable = true;
};

struct resource_descriptor_base {
    stable_resource_id id;
    std::string name;
    resource_lifetime_hint lifetime = resource_lifetime_hint::owned;
    descriptor_fingerprint fingerprint = 0;
};
```

**Function Flags:**

```cpp
enum class function_flag : std::uint32_t {
    pure           = 1u << 0,  // no observable side-effects
    thread_safe    = 1u << 1,  // safe from multiple threads
    deterministic  = 1u << 2,  // same inputs → same output
    blocking       = 1u << 3,  // may block on I/O
    asynchronous   = 1u << 4,  // returns future/coroutine
    gpu_compatible = 1u << 5,  // can run on GPU
};
```

**Factory:**

```cpp
auto fd = lang::make_function_descriptor<"math.dot", &my_dot_fn>();
// Returns function_descriptor_base with thunk auto-generated
```

---

### `effects.hpp` — Effect & Capability System

**Purpose:** Re-exports vakya::types effect/capability infrastructure. Languages define effects (e.g., @pure, @io, @net)
and bundle masks into function signatures.

**Key Concepts:**

| Concept                    | Purpose                                                      |
|----------------------------|--------------------------------------------------------------|
| Builtin Effects (1–5)      | FileSystem, Memory, IO, Network, Exception                   |
| Builtin Capabilities (1–5) | Read, Write, Network, Execute, Allocate                      |
| Extension Band (≥1000)     | Language-defined effects. Crank: @host, @gpu, @parallel_safe |

**Extension Pattern (Crank Example):**

```cpp
auto reg = lang::make_builtin_effect_registry();
vakya::types::effect_descriptor ext;
ext.stable_id = 1000;
ext.symbol = "@host";
ext.bit_mask = lang::kEffectExtBase;
reg.register_desc(ext);
```

---

### `registry.hpp` — Descriptor Registry

Centralized registration and lookup of all descriptors (functions, types, fields, resources). Enables discovery and
cross-language symbol resolution.

---

## Module Layer (`module/`)

File-based and artifact resolution.

### `module_system.hpp` — Module Descriptors & Resolution

**Purpose:** Language-agnostic module abstraction. Supports both file-based modules and embedded artifacts.

**Module Kind:**

```cpp
enum class module_kind : std::uint8_t {
    source = 0,              // source file on disk
    embedded_src = 1,        // in-memory source text
    embedded_artifact = 2,   // pre-compiled binary
    native = 3,              // registered C++ module
    package_root = 4,        // package root (module.lang)
};
```

**Module Descriptor:**

```cpp
struct module_descriptor {
    std::string name;                // "math.vector"
    version_triple version;          // {major, minor, patch}
    module_kind kind;
    module_hash content_hash;        // FNV-1a of source
    module_capabilities capabilities; // effect/capability masks
    std::string source_path;         // filesystem path (source modules)
    std::string package_name;        // package clause
};
```

**Version Triplet:**

```cpp
struct version_triple {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;
};
```

**Resolver Config (Pluggable):**

```cpp
struct resolver_config {
    bool allow_system_paths = false;
    bool allow_package_registry = false;
    std::string source_extension = ".lang";  // Override per language
};
```

---

### `import_resolver.hpp` — Module Resolution & Dependency Graphs

**Purpose:** 9-tier module resolution (native → embedded_artifact → embedded_src → in_memory → project → app → cache →
system → registry) + dependency graph with cycle detection.

#### Module Resolver (9-Tier Lookup)

| Tier | Source                             | When                     |
|------|------------------------------------|--------------------------|
| 1    | Native (registered C++ modules)    | Direct host code         |
| 2    | Embedded artifacts (pre-compiled)  | Cached binaries          |
| 3    | Embedded source (in-memory source) | Built-in modules         |
| 4    | In-memory (runtime-injected)       | REPL, dynamic loading    |
| 5    | Project paths (project-relative)   | `.crank`, `.sutra` files |
| 6    | App paths (application-level)      | Vendor libraries         |
| 7    | Cache paths (compiled cache)       | Incremental builds       |
| 8    | System paths (if enabled)          | System-wide libraries    |
| 9    | Package registry (if enabled)      | Package manager          |

**API:**

```cpp
lang::resolver_config cfg;
cfg.source_extension = ".crank";
lang::module_resolver resolver{cfg};
resolver.add_project_path("/workspace/src");
resolver.add_native(my_native_module);

auto desc = resolver.resolve("util.math");          // -> module_descriptor or std::nullopt
auto src = resolver.source_text("util.math");       // -> std::string or std::nullopt
```

#### Dependency Graph

```cpp
lang::dependency_graph graph;
graph.add_module(desc1);
graph.add_module(desc2);
graph.add_import("main", "util.math");  // main imports util.math

// Topological sort (Kahn's algorithm); empty if cycle detected
auto topo = graph.topo_order();

// DFS coloring; returns modules forming the cycle
auto cycle = graph.cycle_nodes();

// Find importers/dependencies
auto dependents = graph.find_dependents("util.math");    // who imports this?
auto dependencies = graph.find_dependencies("main");      // what does main import?
```

---

## Semantic Layer (`semantic/`)

Scoping and constraint infrastructure.

### `symbol_table.hpp` — Scoped Symbol Resolution

**Purpose:** Scope-stack symbol table with pluggable visibility inference policy. Languages define their own
export/visibility conventions (e.g., uppercase → exported in Crank).

**Symbol Metadata:**

```cpp
enum class sym_mutability { immutable, mutable_, constant };
enum class sym_visibility { module_local, exported };
enum class sym_kind { variable, function, type_alias, module, field, resource, constant };

struct symbol_entry {
    std::string name;
    sym_kind kind;
    sym_mutability mutability;
    sym_visibility visibility;
    std::string type_annotation;
    // Language-specific fields via extension
};
```

**Visibility Policy Concept:**

```cpp
template <typename Policy>
class symbol_table {
    // Policy must provide:
    // sym_visibility infer_visibility(std::string_view name)
};
```

**Builtin Policies:**

- `uppercase_export_policy` — Crank convention: name[0] uppercase → exported.
- `explicit_export_policy` — Annotation-based: all local by default.

**Usage:**

```cpp
lang::symbol_table<lang::uppercase_export_policy> tbl;
tbl.push_scope();

lang::symbol_entry e;
e.name = "Dot";              // Uppercase → will be inferred as exported
e.kind = lang::sym_kind::function;
tbl.define(e);

auto* found = tbl.lookup("Dot");  // walk scope chain, newest-first
assert(found && found->visibility == lang::sym_visibility::exported);

tbl.pop_scope();
```

---

### `rules.hpp` — Language Rules Registry

**Purpose:** Pluggable interface for constraint rules, type rules, rewrite rules. Extensible without modifying core.

---

### `proof.hpp` — Proof Obligation Registry

**Purpose:** Track proof obligations (e.g., "all resources freed on exit"). Pluggable checkers (e.g., Tarka solver).

---

## AST Layer (`ast/`)

### `ast_arena.hpp` — Flat Index-Based AST Storage

**Purpose:** Memory-efficient, grow-safe AST node storage for heterogeneous `std::variant` node types. No pointer invalidation; safe to parallelize allocation.

**Relationship to `ir_module`:** `ast_arena<NodeVariant>` stores `std::variant` nodes with children embedded inside each variant alternative. `ir_module<KE, EP>` stores flat `ir_node` records with children in a separate sidecar vector — enabling egraph views, adjacency queries, and hash-cons interning. Frontends needing those capabilities should use `ir_module` directly (see `crank_ir_module` in `build_ast.hpp`).

**API:**

```cpp
template <class NodeVariant>
class ast_arena {
    ast_node_id push(NodeVariant node);
    const NodeVariant& operator[](ast_node_id id) const;
    NodeVariant& operator[](ast_node_id id);
    std::size_t size() const noexcept;
    bool empty() const noexcept;
};

using ast_node_id = std::uint32_t;
inline constexpr ast_node_id k_null_node = std::numeric_limits<ast_node_id>::max();
```

**Usage (Crank Example):**

```cpp
using crank_node_id = lang::ast_node_id;
using crank_ast_arena = lang::ast_arena<crank_ast_node>;

crank_ast_arena arena;
crank_node_id fn_id = arena.push(fn_node{"main", {}});
auto& fn = std::get<fn_node>(arena[fn_id]);

// Grow-safe: adding nodes never invalidates prior references
crank_node_id block_id = arena.push(block_node{{fn_id}});
```

For ir_module-based storage (egraph/adjacency-capable):

```cpp
// crank_ir_module = lang::ir_module<crank_kind, crank_node_ext>
crank::crank_ir_module mod;
lang::ir_node<crank::crank_kind, crank::crank_node_ext> nd{};
nd.kind = crank::crank_kind::fn;
nd.ext.name = "main";
nd.structural_hash = hash_value;       // carried directly — no side-table
const lang::ir_node_id nid = mod.push(nd);
mod.append_children(nid, {child_id});  // children in flat sidecar
auto view = mod.as_egraph_view();      // adjacency-ready
```

---

## Lexer Layer (`lexer/`)

### `digit_sep.hpp` — Numeric Literal Utilities

Parse digit separators (e.g., `1_000_000` → 1000000) for readable numeric syntax.

---

## Integration: Crank Example

Crank demonstrates full integration of the generic layer.

### 1. AST Alignment

Crank provides two storage aliases:

```cpp
// include/languages/crank/build_ast.hpp
using crank_node_id  = lang::ast_node_id;
using crank_ast_arena = lang::ast_arena<crank_ast_node>;        // variant store (legacy)
using crank_ir_module = lang::ir_module<crank_kind, crank_node_ext>;  // flat ir_node store (Stage 8)
```

`crank_ast_arena` retains the variant-node model for backward compatibility.  
`crank_ir_module` is the ir_module-based store — children in a flat sidecar, `structural_hash` in `ir_node`, egraph/adjacency views available. Parser (lexy) is unchanged for both.

### 2. Module System Bridge

Crank extends generic module types. The import resolver configuration is delegated:

```cpp
// include/languages/crank/context.hpp
[[nodiscard]] inline lang::module_descriptor
to_lang_module(const crank::module_descriptor& d) {
    lang::module_descriptor o;
    o.kind = static_cast<lang::module_kind>(static_cast<std::uint8_t>(d.kind));
    o.content_hash = lang::module_hash{d.content_hash.value};
    o.capabilities = lang::module_capabilities{d.capabilities.effect_mask, d.capabilities.capability_mask};
    return o;
}
```

### 3. Effect & Capability Extension

Crank starts from `lang::make_builtin_effect_registry()` and adds @host, @gpu, @parallel_safe:

```cpp
auto effects = make_builtin_effect_registry();
// Crank adds: @host (1000), @gpu (1001), @parallel_safe (1002)
```

### 4. Symbol Table with Crank Visibility

Crank's visibility policy (uppercase → exported) is a `symbol_table<uppercase_export_policy>` specialized at
`crank::context_builder`.

### 5. Stable IDs for Caching

Every crank function/type/module descriptor includes a `stable_entity_id`, enabling reproducible cache keys:

```cpp
auto id = lang::detail::make_id("my.function", lang::kKindFunction);
// Same input → same 128-bit id, deterministic across builds
```

---

## Extension Points

### For Language Designers

#### 1. Custom Effect/Capability Bands

Languages define effects in the extension band (stable_id ≥1000). Crank: @host, @gpu, @parallel_safe. Sutra:
@stochastic, @robust, @decomposable.

```cpp
auto reg = lang::make_builtin_effect_registry();
vakya::types::effect_descriptor ext;
ext.stable_id = 1000;
ext.symbol = "@my_effect";
ext.bit_mask = lang::kEffectExtBase;
reg.register_desc(ext);
```

#### 2. Custom Symbol Visibility Policies

Extend `symbol_table` with custom visibility logic:

```cpp
struct my_visibility_policy {
    lang::sym_visibility infer_visibility(std::string_view name) {
        // Custom logic (e.g., annotation-based, regex-based, etc.)
    }
};
symbol_table<my_visibility_policy> tbl;
```

#### 3. Custom Resolver Configuration

Set `resolver_config::source_extension` to your language's syntax:

```cpp
lang::resolver_config cfg;
cfg.source_extension = ".mylang";
lang::module_resolver resolver{cfg};
```

#### 4. Custom Rule Registries

Implement constraint/rewrite rules by extending `lang::rules::rule_registry` or `lang::proof::obligation_registry`.

---

## Best Practices

| Practice                               | Benefit                                                                         |
|----------------------------------------|---------------------------------------------------------------------------------|
| **Minimize core dependencies**         | Link only `core/` for diagnostics & IDs; avoid pulling `host/` unless needed.   |
| **Use stable IDs for caching**         | Pair `stable_entity_id` with fingerprints for reproducible artifacts.           |
| **Pluggable policies over hardcoding** | Use template parameters (visibility, resolver config) not conditionals.         |
| **Flat AST arenas**                    | Use `ast_arena<>` instead of pointer networks; enables parallel allocation.     |
| **Effect/capability masks**            | Use extension bands (stable_id ≥1000) to avoid collisions with builtin effects. |
| **Scope-aware symbol lookup**          | Use `symbol_table` with pluggable policies, not flat name maps.                 |

---

## File Reference

| Header                       | Purpose                           | Depends On                   |
|------------------------------|-----------------------------------|------------------------------|
| `generic.hpp`                | Umbrella include                  | All submodules               |
| `core/identity.hpp`          | Stable IDs, fingerprints          | None                         |
| `core/diagnostics.hpp`       | Severity, diagnostic sink         | None                         |
| `core/reflection.hpp`        | Callable traits, thunks           | identity.hpp                 |
| `core/source_location.hpp`   | Source positions                  | None                         |
| `core/rich_diagnostic.hpp`   | Annotated diagnostics             | diagnostics.hpp              |
| `core/parse_stats.hpp`       | Parser metrics                    | None                         |
| `host/descriptors.hpp`       | Function/type/field/resource base | identity.hpp, reflection.hpp |
| `host/effects.hpp`           | Effect/capability system          | vakya/types/effect.hpp       |
| `host/registry.hpp`          | Descriptor registry               | descriptors.hpp              |
| `module/module_system.hpp`   | Module descriptors                | identity.hpp                 |
| `module/import_resolver.hpp` | 9-tier resolver, dep graph        | module_system.hpp            |
| `semantic/symbol_table.hpp`  | Scoped symbol table               | diagnostics.hpp              |
| `semantic/rules.hpp`         | Rules registry                    | None                         |
| `semantic/proof.hpp`         | Proof obligations                 | None                         |
| `ast/ast_arena.hpp`          | Flat AST storage                  | None                         |
| `lexer/digit_sep.hpp`        | Numeric literal utilities         | None                         |
| `samasa/samasa.hpp`          | PEG grammar + scanner + CST parser umbrella | meta.hpp, core/     |

---

## Examples

### Minimal Use: Diagnostic Reporting Only

```cpp
#include "languages/generic/core/diagnostics.hpp"

lang::diagnostic diag;
diag.level = lang::severity::error;
diag.message = "Type error: expected int";
diag.span = lang::source_span{10, 5, 10, 15};

lang::collecting_sink<lang::diagnostic> sink;
sink.report(diag);
```

### Module Resolution with Dependency Checking

```cpp
#include "languages/generic/module/import_resolver.hpp"

lang::resolver_config cfg;
cfg.source_extension = ".crank";
lang::module_resolver resolver{cfg};
resolver.add_project_path("/workspace/src");

auto main_desc = resolver.resolve("main");
lang::dependency_graph graph;
graph.add_module(*main_desc);
// ... build full dependency graph ...

auto cycle_nodes = graph.cycle_nodes();
if (!cycle_nodes.empty()) {
    // Error: cyclic import detected
    for (const auto& node : cycle_nodes) {
        std::cerr << "Cycle node: " << node << "\n";
    }
}
```

### Symbol Table with Crank Conventions

```cpp
#include "languages/generic/semantic/symbol_table.hpp"

lang::symbol_table<lang::uppercase_export_policy> tbl;
tbl.push_scope();

lang::symbol_entry entry;
entry.name = "Dot";  // Uppercase -> inferred as exported
entry.kind = lang::sym_kind::function;
tbl.define(entry);

auto* found = tbl.lookup("Dot");
assert(found && found->visibility == lang::sym_visibility::exported);

tbl.pop_scope();
```

### Flat AST Arena (Crank Pattern)

```cpp
#include "languages/generic/ast/ast_arena.hpp"

using my_ast_node = std::variant<fn_node, block_node, lit_node>;
using my_arena = lang::ast_arena<my_ast_node>;

my_arena arena;
lang::ast_node_id fn_id = arena.push(fn_node{"main", {}});
lang::ast_node_id block_id = arena.push(block_node{{fn_id}});

auto& block = std::get<block_node>(arena[block_id]);
```

### Stable IDs & Fingerprinting

```cpp
#include "languages/generic/core/identity.hpp"

// Create deterministic ID for caching
constexpr auto id = lang::detail::make_id("math.dot", lang::kKindFunction);

// Combine fingerprints for descriptor change detection
auto fp_arity = lang::detail::fp_with_scalar(lang::detail::fp_from_string("dot"), 2);
auto fp_combined = lang::detail::fp_combine(id.namespace_hash, fp_arity);

// Same inputs always produce same IDs/fingerprints -> reproducible caching
```

---

## See Also

- **Samasa:** Compile-time grammar + scanner + CST parser built on top of generic. (`docs/languages/samasa/samasa.md`)
- **Vakya:** Expression trees, pattern matching, type inference. (`docs/vakya/vakya.md`)
- **Crank:** Full integration example of generic layer. (`docs/languages/crank/crank.md`)
- **Sutra:** Language semantics with generic infrastructure.
- **Lithe:** IR layer above frontends.
