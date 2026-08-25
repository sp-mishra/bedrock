# Anvaya — Relational Compiler Framework

**Version:** v1.1  
**Location:** `include/anvaya/` (umbrella: `anvaya/anvaya.hpp`)  
**Namespace:** `anvaya`

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture Diagram](#2-architecture-diagram)
3. [Relational Core Calculus](#3-relational-core-calculus)
4. [Type System](#4-type-system)
5. [Three-Valued Logic (3VL)](#5-three-valued-logic-3vl)
6. [Fluent API](#6-fluent-api)
7. [Equivalence & Canonicalization](#7-equivalence--canonicalization)
8. [Backend Contract](#8-backend-contract)
9. [Explain (Five-Tier)](#9-explain-five-tier)
10. [Operator Identity Invariant](#10-operator-identity-invariant)
11. [Opt-In Extensions](#11-opt-in-extensions)
12. [Optimization Framework](#12-optimization-framework)
    - [12a. Algorithms Used](#12a-algorithms-used)
13. [Compile-Time AQL (Track 2)](#13-compile-time-aql-track-2)
14. [Library Utilization](#14-library-utilization)
15. [Conformance Requirements](#15-conformance-requirements)
16. [Naming Conventions](#16-naming-conventions)

---

## 1. Overview

Anvaya is a header-only, zero-virtual, C++23 relational compiler framework for authoring typed, optimizable relational queries over C++ POJOs. It provides:

- Compile-time schema derivation from any `meta::Reflectable` type
- 3VL-correct null semantics (Kleene K₃)
- Structural AST hashing and pattern-based canonicalization
- Backend-independent plan portability (in-memory, SQL, and custom)
- Five-tier explain output for diagnostics and optimization observability

**Design axioms:**
- Users pay only for what they use (zero-overhead abstractions, opt-in layers)
- No virtual functions, no macros in the semantic core
- Single Vākya waist: every surface lowers to `vakya::node<Tag, …>`
- Tag IDs are append-only after v1.0 (band 2000–2099, never reused)
- No silent wrong answers: unsupported operators error loudly

---

## 2. Architecture Diagram

```
User surface
  fluent builder    pipe combinator    fragment    SQL literal    raw_member
        │                 │               │              │              │
        └─────────────────┴───────────────┴──────────────┴──────────────┘
                                          │
                             vakya::node<Tag, ...>  ← single canonical AST
                                          │
                ┌─────────────────────────┼─────────────────────────┐
                │                         │                          │
           sema/validate            opt/canonicalize          explain (5-tier)
         (scope + type check)    (predicate_merge,         Surface/Lowered/
                                  distinct_dedup)         Logical/Optimized/Physical
                                          │
                              backend contract (G4)
                         relational_backend<B> concept
                                  │              │
                          in_memory_engine    sql_backend
```

---

## 3. Relational Core Calculus

Anvaya's plan language is a closed operator set **𝒜** (bag/multiset semantics unless `distinct`):

| Operator | Signature | Determinism |
|----------|-----------|-------------|
| `Scan(R)` | `→ Rel(R)` | deterministic |
| `Filter(r, p)` | `Rel(S) × Pred(S) → Rel(S)` | deterministic |
| `Project(r, c…)` | `Rel(S) × cols → Rel(c)` | deterministic |
| `Join(a, b, p)` | `Rel(S) × Rel(T) × Pred(S⊎T) → Rel(S⊎T)` | barrier |
| `Aggregate(r, k, a)` | `Rel(S) × keys × aggs → Rel(k ⊎ derived(a))` | barrier |
| `Window(r, spec)` | `Rel(S) × spec → Rel(S ⊎ derived)` | barrier |
| `Order(r, keys)` | `Rel(S) × sort-keys → SeqRel(S)` | sequence-defining |
| `Limit(r, n)` | `Rel(S) × ℕ → Rel(S)` | deterministic |
| `Distinct(r)` | `Rel(S) → Rel(S)` | deterministic |
| `Union(a, b)` | `Rel(S) × Rel(S) → Rel(S)` | barrier |
| `Intersect(a, b)` | `Rel(S) × Rel(S) → Rel(S)` | barrier |
| `Except(a, b)` | `Rel(S) × Rel(S) → Rel(S)` | barrier |

**Closure claim:** every ergonomic surface (`between`, `fragment`, SQL literal, pipe) lowers into **𝒜** and nothing else. If a feature cannot be expressed in **𝒜**, it does not enter v1.

### Tag ID Band (append-only after v1.0)

```
2000 scan      2001 filter     2002 project    2003 join
2004 aggregate 2005 window     2006 order      2007 limit
2008 distinct  2009 union      2010 intersect  2011 except
2020 column    2021 literal
2030 rel_eq    2031 rel_ne     2032 rel_lt     2033 rel_le
2034 rel_gt    2035 rel_ge
2040 rel_and   2041 rel_or     2042 rel_not
2043 is_null   2044 is_not_null 2045 between
2050 agg_sum   2051 agg_count  2052 agg_min    2053 agg_max  2054 agg_avg
2060 fragment
```

A `consteval` uniqueness + range guard in `tags.hpp` enforces this statically.

---

## 4. Type System

### Scalar Lattice (join-semilattice ⊑)

```
        Decimal
        /      \
   Float64    Int64
                |
              Int32
```
`Bool`, `String`, `Date`, `Timestamp` are incomparable islands.

**Promotion rule:** `promote(a, b) = a ⊔ b`. Incomparable → `Top` sentinel → `type_error` diagnostic.

### Nullable as Idempotent Modifier

`Nullable<T>` is a 1-argument constructor, not `std::optional` identity:
- `Nullable(Nullable(T)) = Nullable(T)` (idempotent, enforced by arena collapse)
- `Nullable(T) cmp Nullable(T) → tri_bool` via 3VL (§5)

### C++ Type Mapping (`cpp_to_rel_kind<T>`)

| C++ type | `rel_scalar_kind` |
|----------|-------------------|
| `bool` | `Bool` |
| `int`, `int32_t` | `Int32` |
| `int64_t` | `Int64` |
| `float`, `double` | `Float64` |
| `std::string` | `String` |
| `std::optional<T>` | `Nullable` |

### Layering

- **`vakya::types`** (reused verbatim): `type_arena`, `type_ref`, `intern_primitive`, `intern_constructor`, `type_rewrite`
- **`anvaya::types`** (Anvaya-local, relational policy): `make_relational_type_registry()`, `promote()`, `cpp_to_rel_kind<T>`, `column_rel_kind<R,I>`

---

## 5. Three-Valued Logic (3VL)

Anvaya uses Kleene K₃ throughout.

### Truth Tables

```
 ∧ | T U F        ∨ | T U F        ¬ | T U F
───+──────       ───+──────       ───+──────
 T | T U F        T | T T T        T | F
 U | U U F        U | T U U        U | U
 F | F F F        F | T U F        F | T
```

**Filter rule:** only `T` passes (`tri_passes_filter`). `U` and `F` rows are excluded.

### NULL Ordering (for ORDER BY)

Default: **NULLs last** for ASC, **NULLs first** for DESC (SQL standard). Overridable per sort-key via `null_ordering::nulls_first` / `nulls_last`.

---

## 6. Fluent API

```cpp
// Entry point
auto q = anvaya::query<Order>();

// Filtering
q.where(field<&Order::status>() == "paid")
 .where(field<&Order::total>() > 100.0)

// Sorting (G5)
q.order_by(sort_key{field<&Order::total>(), sort_direction::desc})

// Aggregation (G5)
q.aggregate(make_key_list(field<&Order::customer_id>()),
            make_agg_specs(agg_sum(field<&Order::total>()),
                           agg_count(field<&Order::id>())))

// Set operations (G5)
q_a.union_(q_b)
q_a.intersect(q_b)
q_a.except(q_b)

// Joining
q.join<Customer>(field<&Order::customer_id>() == field<&Customer::id>())

// Projection + limit
q.select(field<&Order::id>(), field<&Order::total>())
 .limit(10)
 .distinct()

// Skip rows (Track 3)
q.offset(5)

// Post-aggregation filter (Track 3)
q.having(field<&Order::total>() > 1000.0)

// Window function (Track 3)
anvaya::window_spec spec;
spec.fn_name = "row_number";
spec.partition_cols[0] = "customer_id"; spec.partition_count = 1;
spec.order_cols[0] = {.col = "total", .is_desc = false}; spec.order_count = 1;
q.window(spec)

// Execution pipeline (G6): validate → canonicalize → negotiate → execute
auto result = q.execute(engine, rows);
auto result = q.execute(engine, rows, exec_options{.optimize_level = 0}); // no optimize
```

### Pipe Combinators

```cpp
using namespace anvaya::ergonomics;
auto q = query<Order>()
    | where(field<&Order::status>() == "paid")
    | limit(10);
```

### Track 3 eDSL (`anvaya/frontend/aql_edsl.hpp`)

The preferred query-building surface. Import with `using namespace anvaya::edsl`.

```cpp
using namespace anvaya::edsl;

// col<> — zero-paren field ref (same type as field<&T::m>())
auto q = query<Order>()
    | where(col<&Order::status> == "paid")
    | order_by(desc(col<&Order::total>))
    | limit(10);

// group_by / agg — aggregate pipeline
auto agg_q = query<Order>()
    | where(col<&Order::status> == "paid")
    | group_by(col<&Order::customer_id>).agg(
        sum(col<&Order::total>),
        count(col<&Order::id>));

// is_null / is_not_null — null predicates
auto nulls = query<Order>() | where(is_null(col<&Order::status>));

// over() — lowercase window builder (OVER() kept for compat)
auto w = query<Order>()
    | window(over("row_number")
        .partition_by("customer_id")
        .order_by("total")
        .rows(unbounded_preceding, current_row_bound));

// offset pipe combinator
auto p = query<Order>() | offset(5);
```

**Short-form agg aliases** in `anvaya::edsl`: `sum`, `count`, `min`, `max`, `avg` (wrap `agg_sum`, `agg_count`, etc.).

**Sort helpers**: `asc(col)` / `desc(col)` return `sort_key<ColExpr>` with the correct direction.

The `anvaya::edsl` namespace re-exports all core symbols (`query`, `field`, `col`, `sort_key`, `agg_*`, `window_spec`, `between`, etc.) — `using namespace anvaya::edsl` is sufficient.

---

## 7. Equivalence & Canonicalization

Three tiers:

| Tier | Function | Definition |
|------|----------|------------|
| Surface | `same_surface_shape(a, b)` | Raw structural equality (bytewise AST) |
| Canonical | `same_canonical_shape(a, b)` | Equality after `predicate_merge` + `distinct_dedup` |
| Semantic | `same_semantics(a, b)` | Equality under commutativity (conformance/tooling) |

`same_shape(a, b)` is a deprecated alias of `same_surface_shape` (one-release compat).

### Canonicalization Rules

- **`predicate_merge`**: `filter(filter(r,p),q) → filter(r, and(p,q))`
- **`distinct_dedup`**: `distinct(distinct(r)) → distinct(r)`

---

## 8. Backend Contract

```cpp
// Concept (G4) — zero virtual, static polymorphism
template <class B>
concept relational_backend = requires(B& b, const compiled_plan& cp) {
    { B::capabilities() }  -> std::convertible_to<backend_capability_info>;
    { b.explain(cp) }      -> std::convertible_to<explain_physical_tier>;
};

// Full contract:
//   compile(plan)          → expected<compiled_plan, backend_error>
//   execute(plan, rows)    → expected<result_set<Row>, error>
//   explain(compiled)      → explain_physical_tier
//   estimate(plan)         → cost_vector
```

**Capability bitset** (`backend_capabilities`): a backend lacking `aggregate` never receives an aggregate plan — the planner rejects it before execution via `negotiate_backends`.

### In-Memory Compile Footprint Controls

The in-memory backend keeps compile-time memory bounded by **erasing plan type early** (sutra-style), so the heavy operator helpers instantiate once per `Row` instead of once per plan-subtree shape:

- `execute<Row>(plan, rows)` lowers the typed plan tree ONCE into a flat, Row-only `rel_program<Row>` (a linear `vector<rel_op<Row>>`; binary nodes — join/set-op — carry nested sub-programs run via runtime recursion, not compile-time recursion).
- Row-typed behaviour is erased into `std::function` closures built once at lowering: `build_pred<Row,Pred>` composes a predicate subtree into one `tri_bool(const Row&)`; `build_less<Row>` wraps `sort_key_list_cmp` into one `bool(const Row&,const Row&)`; aggregate/window/user thunks capture their spec into an `xform` closure. Pure runtime data (limit `n`, set-op kind, user `stable_id`) needs no closure.
- A single non-recursive interpreter `run<Row>()` walks the flat op-list (`switch` on opcode), calling the shared `_impl` helpers (`exec_filter_impl`, `exec_order_impl`, `exec_window_impl`, `exec_aggregate_impl`, `exec_set_op_impl`, `exec_join_impl`, `exec_limit_impl`, `exec_offset_impl`, `exec_distinct_impl`) — each now one Row instantiation.
- `eval_plan` / `eval_plan_with_engine` remain as thin wrappers that route through `lower<Row> + run<Row>` (engine ref threads user-op registry for the §ext path). `backend/rel_program.hpp` is a stable include point; the IR and `lower`/`run` live in `in_memory.hpp`.

This is the fix for the ~30GB template-instantiation blowup (three type-recursive walks per unique plan shape → one O(n) lowering walk with tiny per-node bodies), and preserves runtime semantics. The full typed optimizer pipeline stays available opt-in via `exec_options` (`optimize`/`optimize_level`); the default in-memory path is lightweight (primary use = ORM layer over a SQL DB that does the real optimization).

### Cost Vector

```cpp
struct cost_vector {
    double latency;    // ms
    double memory;     // MB
    double throughput; // rows/sec
    double power;      // arbitrary
};
```

> **Note:** Row cardinality is tracked separately via `cardinality_estimate` (`opt/cost.hpp`), not `cost_vector`. Cost models may optionally implement `CostModelWithCardinality` to provide cardinality estimates alongside the cost vector.

---

## 9. Explain (Five-Tier)

```cpp
struct explain_node {
    std::string surface;        // ergonomic form (from provenance)
    std::string lowered;        // tag symbol: "filter", "join", …
    std::string canonical_hash; // hex structural_hash
    std::string logical;        // relational algebra: Filter(Scan(…))
    std::string optimized;      // post-canonicalize + rules_fired
    std::string physical;       // backend operator + cost
    std::vector<std::string> rules_fired;
};

// Full five-tier explain string:
std::string s = anvaya::explain_full(plan, store, &phys_tier);
```

---

## 10. Operator Identity Invariant

**Invariant (G10):** every relational operator's `op_id` annotation (`"anvaya.rel.join"`, etc.) must survive from lowering to physical-operator selection. No optimizer pass may drop an `op_id` without replacing it.

```cpp
// Conformance verifier:
auto result = lower::verify_op_identity_invariant(plan);
REQUIRE(result.passed);  // fails if any node has unknown/empty op_id
```

---

## 11. Opt-In Extensions

### Tarka Proof Hooks (G12)

```cpp
#include "anvaya/sema/verify.hpp"  // no-op when <tarka/tarka.hpp> absent
auto res = anvaya::sema::verify_null_obligations(plan);
// res.null_obligations — collected is_not_null obligations
// res.interval_obligations — collected between non-empty obligations
// res.report — notes (never hard errors when SMT absent)
```

### Relational Type System (G1)

```cpp
#include "anvaya/types/types.hpp"
using namespace anvaya::types;
static_assert(column_rel_kind<Order, 0> == rel_scalar_kind::Int32);
static_assert(promote(rel_scalar_kind::Int32, rel_scalar_kind::Int64)
              == rel_scalar_kind::Int64);
```

---

## 12. Optimization Framework

Anvaya is an abstract framework — SQL backends (SQLite, Postgres, DuckDB) do their own optimization. Anvaya's optimizer rewrites the AQL AST before handing off to the backend (predicate merging, distinct deduplication, projection pruning). No physical planner is needed or included by default.

**For DB builders** (implementing their own execution engine): include `anvaya/ext/planner/planner.hpp` for the physical planning extension.

### Default Pipeline (ORM / query-builder path)

```
Logical plan
  → logical opt    (rule sets / passes)      opt/rules.hpp
  → logical cost   (cost_vector data type)   backend/registry.hpp
  → capability     (backend negotiation)     backend/registry.hpp
  → backend.execute (SQL backend handles the rest)
```

### Concepts (`opt/concepts.hpp`)

| Concept | Models |
|---------|--------|
| `OptimizationRule<T,Node>` | `predicate_merge`, `distinct_dedup` |
| `OptimizationPass<T,Plan>` | any pass with `.run(plan)` |
| `CostModel<T,Node>` | `relational_model`, `nested_model`, `graph_model` |
| `CostModelWithCardinality<T,Node>` | `relational_model` (generic fallback) |
| `StatisticsProvider<T>` | `basic_statistics_provider` |

Physical planner concepts (`PhysicalPlanner`, `JoinSearchStrategy`, `OptimizationBundle`) are in `anvaya/ext/planner/concepts.hpp`.

### Cost Vector (`backend/registry.hpp`)

```cpp
struct cost_vector {
    double latency    = 0;  // ms
    double memory     = 0;  // MB
    double throughput = 0;  // rows/sec
    double power      = 0;  // arb. units
    double io         = 0;  // block reads        (v1.1)
    double network    = 0;  // bytes shipped       (v1.1)
    cost_vector& operator+=(const cost_vector&) noexcept;
    double total(w_lat=1.0, w_mem=0.1, ...) const noexcept; // compat alias
};
```

### Composite Cost Model (`opt/cost.hpp`)

```cpp
// Contributor-sum aggregation
composite_cost_model<relational_model, nested_model> cm;
auto cv = cm.estimate(my_node);

// Objective functions (pluggable)
minimize_latency   // default — mirrors .total()
minimize_memory
minimize_network
maximize_throughput
minimize_cloud_cost
```

### Cardinality Estimation (`opt/cost.hpp`)

```cpp
struct cardinality_estimate {
    double input_rows  = 1.0;
    double output_rows = 1.0;
    double selectivity = 1.0;
    double confidence  = 1.0;  // [0,1]; 1=exact, 0=guess
};
```

### Statistics (`opt/statistics.hpp`)

```cpp
basic_statistics_provider sp;
sp.set_row_count(schema_id, 1'000'000.0);
sp.set_distinct_count(schema_id, col_idx, 500.0);
```

### Optimizer Context (`opt/context.hpp`)

Default context for ORM use — rules + passes only, no planner:

```cpp
auto ctx = default_optimizer_context{}  // = all_opt_rules + no passes
    .use(my_rule_set{})                 // swap rules
    .use(my_pass{})                     // append pass
    .use(pipeline<"normalize">{}        // replace passes
             .append(predicate_push_down{}));

// execute with explicit optimizer context + options
auto res = query<Order>()
    .where(field<&Order::status>() == "paid")
    .execute(engine, rows,
             exec_options{.optimize = true, .optimize_level = 1},
             ctx);
```

The execution pipeline applies optimization in this order:

1. validation (if `exec_options.validate`)
2. typed canonicalization (e.g. filter-merge, distinct-dedup)
3. configured pass tuple from optimizer context
4. backend execution

Track 1/2 AQL wrappers (`aql<Row>(text)` and `aql<"...", Row>()`) forward `exec_options` into the same typed canonicalization stage before plan erasure.

### Named Pipeline (`opt/context.hpp`)

```cpp
auto warehouse = pipeline<"warehouse">{}
    .append(relational_o2{})
    .append(parquet_pushdown{});
// warehouse.run(plan) folds all stages left-to-right
```

### Profiles (`opt/profiles.hpp`)

`relational.o0` … `relational.o3` profiles remain unchanged.  
Default for `query<R>().execute(…)` is `relational.o2`.

---

## 12a. Algorithms Used

Concrete named algorithms in the implementation, with the header they live in.

| Concern | Algorithm | Where |
|---|---|---|
| Null semantics | Kleene three-valued logic K3 (`tri_bool` {false_, true_, unknown}); AND/OR/NOT truth tables with null propagation | `null3vl.hpp` |
| Relational core | Relational-algebra operator set: project, select, join, aggregate, order, limit, distinct, union, difference | `ast.hpp`, `query.hpp` |
| Canonicalization / CSE | Structural-hash equivalence + operator-identity canonicalization (dedup of identical subplans) | §7, `identity.hpp` |
| Logical optimization | Rule-based rewrite passes: `predicate_merge`, `distinct_dedup`, projection pruning (`OptimizationRule`/`OptimizationPass`) | `opt/rules.hpp`, `opt/concepts.hpp` |
| Cost modeling | Multi-objective `cost_vector` (latency/memory/throughput/power/io/network) with weighted-sum `total()`; `relational`/`nested`/`graph` cost models | `backend/registry.hpp` |
| Physical planning (opt-in) | MILP-based physical join/plan selection via Siddhanta; join-search strategy concept | `opt/physical_milp.hpp`, `ext/planner/` |
| Explain | Five-tier explain node (logical / typed / optimized+cost / physical / backend) | `explain.hpp` |
| Type system | Join-semilattice column/row types; nullability lattice | `types/types.hpp` |

---

## 12b. Extension: Building a DB on Anvaya

For backends that implement their own execution engine (not SQL pass-through):

```cpp
#include "anvaya/ext/planner/planner.hpp"      // planners + join search
#include "anvaya/ext/planner/context_ext.hpp"  // planner_context
```

### Planners (`ext/planner/planner.hpp`)

| Planner | Default? | Mechanism |
|---------|----------|-----------|
| `heuristic_planner` | ✅ | Zero cost vector; fast; `strategy_note="heuristic:hash_join"` |
| `cascades_planner` | v2 target | Memo seam reserved; delegates to heuristic at v1 |

### Search Strategies

| Strategy | Notes |
|----------|-------|
| `greedy_search` | Default; sorts tables by cardinality; O(n log n) |
| `milp_search` | **Dormant**: struct compiles and satisfies `JoinSearchStrategy`; delegates to greedy hook. Full MILP activation requires including `physical_milp.hpp` — not active: Siddhanta `MILPSolver::solve` compile time is prohibitive (minutes per TU) until the backend stabilises. |

### Optimization Bundles (`ext/planner/context_ext.hpp`)

```cpp
auto ctx = planner_context<>{}
    .use(relational_optimizer_bundle{})   // configure planner+cost+stats from bundle
    .use(minimize_memory);                // swap objective
```

Built-in bundles: `relational_optimizer_bundle`, `nested_optimizer_bundle`, `graph_optimizer_bundle`.

### Extended Planner Context

```cpp
auto ctx = default_planner_context{}
    .use(heuristic_planner{})             // swap planner
    .use(default_cost_model{})            // swap cost model
    .use(basic_statistics_provider{});    // swap statistics

auto result = ctx.planner().plan(my_plan);

---

## 13. AQL — Three Tracks

AQL supports three surfaces, all lowering to the same `vakya::node` AST:

| Track | Surface | Entry point | Validation |
|-------|---------|-------------|------------|
| 1 (Lexy runtime) | SQL/pipeline/relational text | `anvaya::aql<Row>(text)` | Runtime parse error |
| 2 (Samasa consteval) | SQL text as NTTP | `anvaya::aql<"...", Row>()` | `static_assert` at compile time |
| 3 (eDSL) | C++ fluent builder | `anvaya::query<Row>()` | Type-safe at compile time |

### Track 1 — Lexy Runtime AQL

Supports: SELECT, WHERE, JOIN (INNER/LEFT/RIGHT/CROSS), DISTINCT, GROUP BY, HAVING, ORDER BY, LIMIT, OFFSET, UNION, INTERSECT, EXCEPT, relational algebra (Σ/π/⋈).

```cpp
auto q = anvaya::aql<Order>("SELECT * FROM order WHERE status = 'paid' LIMIT 10");
auto res = q.exec(rows);  // returns std::expected<result_set<Row>, in_memory_error>
```

### Track 2 — Samasa Compile-Time AQL

AQL queries validated at compile time via `akshara::fixed_string` NTTP + Samasa consteval lexer.

```cpp
// Mixed-case accepted (consteval lowercase fold applied before lex)
constexpr auto d = anvaya::frontend::samasa_aql::ce_parse_impl<
    "SELECT * FROM Orders WHERE status = 'paid'">();
static_assert(d.ok, "AQL compile-time parse error");
// d.from_table  == "orders"
// d.has_where   == true
// d.where.count >= 1

// High-level: validated at compile time, executed at runtime
auto q = anvaya::aql<"select * from order where status = 'paid'", Order>();
auto res = q.exec(rows);
```

**Error diagnostics:** On failure, `d.ok == false`, `d.error_view()` returns the message, and `d.error_pos` holds the byte offset of the failure — rich `static_assert` text shows both.

**Libraries used:**  
- `akshara::fixed_string` — NTTP query carrier  
- `lang::samasa::parse_static` — compile-time lexing  
- `meta::Reflectable` — Row schema derivation (shared build stages)

### Track 3 — eDSL

See §6 Fluent API and Track 3 eDSL subsection. `anvaya/frontend/aql_edsl.hpp` provides the `anvaya::edsl` namespace with `col<>`, sort/agg/null helpers, `group_by/agg` pipe combinators, and `over()` window builder.

---

## 14. Library Utilization

| Concern | Library / Component |
|---------|---------------------|
| Expression AST | `vakya::node`, `structural_hash`, `tag_descriptor` |
| Pattern rewrites | `vakya::pattern` (`opt/rules.hpp`) |
| Type interning | `vakya::types::type_arena` / `intern_primitive` |
| Reflection / schema | `meta::reflect_t`, `schema_hash` |
| 3VL | `anvaya::null3vl` (Kleene K₃) |
| Validation / domain | `sutra::domain` + `sema/*` |
| Plan cache | `containers/cache/kosha` (via `artifact_key`) |
| Parallel execution | `pravaha::InlineBackend` |
| Compile-time lex | `lang::samasa::parse_static` (AQL Track 2) |
| NTTP string | `akshara::fixed_string` (AQL Track 2) |
| Profiles / passes | `lithe::profile`, `lithe::passes::pass_bundle` |
| Join-order MILP | `siddhanta` (`ext/planner/physical_milp.hpp`, **dormant** — high compile cost; greedy active) |
| Optimizer seams (ORM) | `opt/{concepts,cost,statistics,context}.hpp` |
| Cardinality estimation | `opt/cost.hpp` (`cardinality_estimate`, `CostModelWithCardinality`) |
| Physical planner (DB builder) | `ext/planner/planner.hpp` (`PhysicalPlanner`, `heuristic_planner`, `cascades_planner`) |
| Join search strategies | `ext/planner/planner.hpp` (`JoinSearchStrategy`, `greedy_search`, `milp_search`) |
| Optimization bundles | `ext/planner/context_ext.hpp` (`OptimizationBundle`, `*_optimizer_bundle`) |
| Window functions | `window_spec.hpp`, `in_memory.hpp` (`exec_window_impl`) |
| eDSL / Track 3 | `frontend/aql_edsl.hpp` (`anvaya::edsl`, `col<>`, `asc/desc/sum/count/is_null`, `over()`) |
| SQLite backend | `backend/sqlite.hpp` (`sqlite_backend`, opt-in via `HAS_SQLITE3`) |

---

## 15. Conformance Requirements

1. **Single Vākya waist** — every surface lowers to `vakya::node<Tag,…>`.
2. **Tag IDs append-only** — band 2000–2099; enforced by `static_assert` in `tags.hpp`.
3. **Operator identity survives** — `verify_op_identity_invariant` passes on all plans.
4. **Nullable is a modifier** — all comparisons route through `relational_compare`; `NULL = NULL → UNKNOWN`.
5. **No silent wrong answers** — unsupported operators return `error`, never pass-through.
6. **Core header-only** — no virtual, no macros in semantic core.

---

## 16. Naming Conventions

| Layer | Name |
|-------|------|
| Public API | Anvaya Core / Explain / Native Backend / SQL Backend |
| AST / Hashing | Vākya (`vakya::`) |
| Validation / Domain | Sutra (`sutra::`) |
| Cost model / Profiles | Lithe (`lithe_cost_model::`) |
| Parallel execution | Pravaha (`pravaha::`) |
| SMT proofs (opt-in) | Tarka (`tarka::`) |
| Reflection | Meta (`meta::`) |

**C++23/C++26** — not C23/C26. Core is header-only; backends may be compiled (opt-in).
