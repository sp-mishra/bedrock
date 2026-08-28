# Chapter 5b — Vakya Type System Deep Dive

## Why Go Deeper?

Chapter 5 covered basic HM type inference: deduce `f32`, catch `f32 + bool`, infer `∀α. α → α`.
That is sufficient for *type checking*. But Flux needs more:

- `requires gpu` — track which operations *need* GPU execution
- `pure fn` — track which functions have no side effects
- `prove(x*x >= 0)` — verify mathematical properties
- `show_effects()` — report what a computation touches
- `show_types()` extended — show not just `f32` but full semantic annotations

These require the **V3 constraint reasoning stack** built into Vakya. This chapter explains it and
shows exactly how Flux uses it.

---

## The Vakya Type System Stack

The complete stack, from bottom to top:

```
vakya/vakya.hpp              ← AST nodes, structural_hash, pattern DSL
       ↓
vakya/types.hpp              ← type terms (τ ::= κ | α | C(τ) | τ→τ | ∀ᾱ.τ)
       ↓
vakya/unification.hpp        ← Robinson MGU, substitution (union-find)
       ↓
vakya/constraints.hpp        ← constraint_kind, composite_solver
       ↓
vakya/constraint_solvers.hpp ← rule/graph/egraph solvers
       ↓
vakya/smt.hpp                ← SMT backend (no_smt_backend / tarka_smt_backend)
       ↓
vakya/type_checking.hpp      ← type_environment, typing_rule<Tag>
       ↓
vakya/type_inference.hpp     ← Algorithm W, kosha LRU memoization
       ↓
vakya/rewrite.hpp            ← guarded_rule<Pattern, Rewrite, Guard>
       ↓
vakya/validation.hpp         ← validator<Checks...>
       ↓  V3 extensions (all opt-in via vakya/vakya_types.hpp)
types/type_registry.hpp      ← descriptor_registry of type constructors
types/capability.hpp         ← capability_descriptor, capability_mask (uint64_t)
types/effect.hpp             ← effect_descriptor, effect_mask (uint64_t)
types/shape.hpp              ← shape_type_tag, intern_shape, make_matmul_constraints
constraint_registry.hpp      ← batch_by_class, solve_batch fixpoint
analysis_store.hpp           ← analysis_record, thread-safe analysis_store
analysis.hpp                 ← analyze() orchestrates all passes
typed_pattern.hpp            ← typed<Ctor>(inner), with_trait<TraitId>(inner)
type_rewrite.hpp             ← type_rewrite_engine, normalize
verify.hpp                   ← proof_obligation, verification_report
query.hpp                    ← query_builder, predicates (type/effect/cap/proven)
```

Pull everything: `#include <vakya/vakya_types.hpp>`

---

## The analysis_record: All Knowledge About One Expression

Every subexpression in the Flux program gets an `analysis_record` — a compact struct that accumulates
everything the compiler learns:

```cpp
struct analysis_record {
    vakya::types::type_ref  type;          // inferred type (f32, vec<i64>, ...)
    vakya::types::type_ref  shape;         // shape if tensor (dims stored in type_arena)
    effect_mask             effects;       // which side effects this expr has
    capability_mask         caps;          // which capabilities it requires
    proof_status            proofs;        // unknown/proven/refuted/deferred
    uint64_t                trait_set;     // bitset of satisfied trait IDs
    uint64_t                features;      // backend feature requirements
    uint64_t                refutation_payload; // SMT evidence for refutation
};
```

Records are stored in a thread-safe `analysis_store` keyed by `structural_hash`:

```cpp
vakya::types::analysis_store astore;

// Write a record (thread-safe, locks for entire closure)
astore.update_for(expr, [](auto& rec) {
    rec.type   = f32_ref;
    rec.effects = effect_mask{0};       // pure
    rec.caps    = capability_mask{0};    // CPU-only
});

// Read a record
auto* rec = astore.find_for(expr);
if (rec) std::println("type: {}", type_name(rec->type));
```

---

## Effect System

### What effects are

An **effect** represents a side effect that an expression can perform. Flux tracks:

| Effect | Meaning |
|--------|---------|
| `FileSystem` | Reads or writes files |
| `Memory` | Allocates or frees heap memory |
| `IO` | Prints to stdout/stderr or reads stdin |
| `Network` | Makes network calls |
| `Exception` | Can throw |

Built-in effects have `stable_id` 1–5. Custom effects extend with `stable_id >= 1000`.

### Effect masks

Effects are represented as a `uint64_t` bitmask — O(1) union, intersection, and membership:

```cpp
auto filesystem_id = 1;   // stable
auto memory_id     = 2;
auto io_id         = 3;

// An expression that reads files and might print:
auto my_effects = add_effect(add_effect(effect_mask{0}, filesystem_id), io_id);
// my_effects = 0b0000...00101

// Check: does this expr do IO?
bool does_io = has_effect(my_effects, io_id);  // true
```

### Pure functions

A `pure fn` in Flux must have `effects == 0`:

```flux
pure fn square(x : f32) -> f32 {
    x * x
}
```

```flux
-- Error: pure function cannot have IO effect
pure fn bad(x : f32) -> f32 {
    print(x)    -- IO effect!
    x * x
}
```

The effect checker traverses the expression tree, collecting effects from each node, and verifies
that `pure` declarations have none.

### Using Vakya's effect system in Flux

```cpp
// After analysis, read effects from analysis_store
void show_effects(vakya::types::analysis_store const& store,
                  vakya::types::type_arena const& arena,
                  lithe::shared_expr const& expr)
{
    auto* rec = store.find_for(expr);
    if (!rec) { std::println("(no analysis)"); return; }

    if (rec->effects.bits == 0) {
        std::println("pure (no effects)");
        return;
    }

    auto reg = vakya::types::make_builtin_effect_registry();
    std::println("effects:");
    reg.discover([&](auto const& desc) {
        if (has_effect(rec->effects, desc.stable_id))
            std::println("  {}", desc.name);
    });
}
```

### Flux example

```flux
-- Pure: no effects
pure fn distance(x : f32, y : f32) -> f32 {
    sqrt(x*x + y*y)
}
distance.show_effects()   -- pure (no effects)

-- Effectful: IO
fn print_distance(x : f32, y : f32) {
    let d = sqrt(x*x + y*y)
    print(d)   -- IO effect
}
print_distance.show_effects()   -- effects: IO
```

---

## Capability System

### What capabilities are

A **capability** is a hardware or runtime resource that an expression requires to execute:

| Capability | Meaning |
|------------|---------|
| `Read` | Reads external data |
| `Write` | Writes external data |
| `Network` | Network access |
| `Execute` | Process execution |
| `Allocate` | Dynamic memory |

For Flux's compute focus, we add backend capabilities:

```cpp
// Flux-specific capabilities (stable_id >= 1000)
constexpr uint32_t cap_gpu   = 1001;
constexpr uint32_t cap_simd  = 1002;
constexpr uint32_t cap_f64   = 1003;   // requires f64 support (not all GPUs)
constexpr uint32_t cap_tensor= 1004;   // requires tensor hardware (TPU/NPU)
```

### requires declarations

```flux
input A : tensor<f32>[1024,1024]
input B : tensor<f32>[1024,1024]

requires gpu

let C = matmul(A, B)
C.run(gpu)
```

The `requires gpu` statement adds `cap_gpu` to the capability mask of `C`. The backend selector
checks: can the chosen backend satisfy `cap_gpu`? If not, it is an error.

```cpp
// Check capability in analysis_store
auto* rec = astore.find_for(C_expr);
if (rec && has_capability(rec->caps, cap_gpu)) {
    // Must route to GPU backend
    compile<lithe::BackendKind::GPU>(C_expr);
}
```

### SIMD capability

```flux
let result =
    range(1, 1000000)
        .map(fn(x) { x*x + 1.0 })
        .reduce(sum)

requires simd
result.run(simd)
```

The inferrer propagates `cap_simd` from the `requires simd` declaration through to all expressions
that need it.

---

## Analysis Orchestration

The `analyze()` function in `vakya/analysis.hpp` orchestrates all phases in one call:

```cpp
// Flux type_inferrer calls this after inference to populate analysis_store
vakya::types::analyze_options opts{
    .emit_effects  = true,
    .emit_caps     = true,
    .max_depth     = 1024
};

// Run: type_check + effect/cap propagation → analysis_store
vakya::types::analyze(
    flux_vakya_expr,    // the lowered vakya::node expression
    type_env_,          // type environment from inference
    solver_,            // composite_solver
    tara_,              // type_arena
    gen_,               // type_var_generator
    subst_,             // substitution
    astore_,            // analysis_store (output)
    opts
);
```

After this call, `astore_` has a complete `analysis_record` for every subexpression.

---

## Guarded Rewrites: Type-Aware Optimization

Plain pattern rewrites (Chapter 8) fire regardless of types. **Guarded rewrites** add a type check:
the rewrite only fires if the types satisfy a condition.

### Example: float-specific strength reduction

```
x * 2.0f  →  x + x   (only for f32, not f64 — different rounding behavior)
```

```cpp
// Guarded rule: multiply by 2.0 → add to self, only for f32
auto float_double_rule = vakya::types::make_guarded(
    // Pattern: x * lit(2.0)
    vakya::pattern::rule("f32-double",
        vakya::pattern::mul(vakya::pattern::pv<0>(),
                            vakya::pattern::lit<2.0f>()),
        vakya::pattern::add(vakya::pattern::pv<0>(),
                            vakya::pattern::pv<0>())),
    // Guard: only fire if x has type f32
    [&astore](auto const& match_env, auto const& type_env) -> bool {
        auto x_expr = match_env.get<0>();
        auto* rec   = astore.find_for(x_expr);
        return rec && type_is_f32(rec->type);
    }
);
```

### Example: matmul associativity only for compatible shapes

```
matmul(matmul(A,B), C)  →  matmul(A, matmul(B,C))
```

This is only valid if shapes allow it. The guard checks shape compatibility:

```cpp
auto matmul_assoc = vakya::types::make_guarded(
    vakya::pattern::rule("matmul-assoc",
        matmul(matmul(pv<0>(), pv<1>()), pv<2>()),
        matmul(pv<0>(), matmul(pv<1>(), pv<2>()))),
    [&astore](auto const& env, auto const&) -> bool {
        auto A = env.get<0>(), B = env.get<1>(), C = env.get<2>();
        return shapes_matmul_compatible(astore, A, B, B, C);
    }
);
```

---

## Formal Verification: Prove and Assert

### The proof system

Flux provides two verification constructs:

```flux
assert(x > 0)       -- runtime assertion (panics if false)
prove(x*x >= 0)     -- compile-time proof obligation (discharged by SMT)
```

`prove` emits a `proof_obligation` to the verification engine. Vakya's `verify.hpp` collects these
and discharges them through the `smt_backend`:

```cpp
// Proof obligations are emitted during analysis
proof_obligation po{
    .kind    = proof_obligation_kind::arithmetic,
    .formula = /* x*x >= 0 as SMT term */,
    .source  = node_idx,
};

// verify() collects and discharges all obligations
auto vreport = vakya::types::verify(
    astore.find(structural_hash(expr))->proofs,
    astore,
    smt_solver   // tarka_smt_backend or no_smt_backend
);

if (vreport.all_proven) {
    std::println("prove(x*x >= 0): PROVEN  ✓");
} else if (vreport.any_refuted) {
    std::println("prove(...): REFUTED ✗");
}
```

### Using Tarka as SMT backend

When `<tarka/tarka.hpp>` is available, Vakya uses it for proof discharge:

```cpp
#include <vakya/smt.hpp>  // or vakya/vakya_types.hpp

// Create Tarka-backed SMT solver
vakya::types::tarka_smt_backend<tarka::Z3Backend> smt{z3_backend};

// Prove x*x >= 0 for symbolic x
auto x_term = smt.make_variable("x", tarka::sort::real());
smt.assert_tarka(x_term * x_term >= tarka::literal(0.0));
auto status = smt.check_sat();
// status == unsat → ¬(x*x >= 0) is unsatisfiable → x*x >= 0 is proven
```

### Flux examples

```flux
-- Always true: proven at compile time
prove(x*x >= 0.0)     -- PROVEN (quadratic non-negativity)

-- Requires precondition
input x : f32
assert(x > 0.0)
prove(sqrt(x) >= 0.0)  -- PROVEN (sqrt of positive is non-negative)

-- False — SMT finds counterexample
prove(x > 0.0)         -- REFUTED (x = -1.0 is a counterexample)
```

---

## The Query Engine

After analysis, Flux can query the `analysis_store` to answer questions like:
"find all subexpressions that require GPU capability and return a tensor."

```cpp
#include <vakya/query.hpp>

using namespace vakya::query;

// Build a query
auto gpu_tensors = make_query(astore)
    .where(capability_pred(cap_gpu))        // requires GPU
    .where(type_pred<tensor_type_tag>())    // produces tensor
    .execute();

for (auto const& result : gpu_tensors) {
    std::println("GPU tensor expr hash: 0x{:016x}", result.hash);
}
```

Available predicates:

| Predicate | Matches |
|-----------|---------|
| `type_pred<Ctor>()` | Expressions with type constructor `Ctor` |
| `typed_pred(type_ref)` | Expressions with exactly this type |
| `effect_pred(effect_id)` | Expressions with this effect |
| `capability_pred(cap_id)` | Expressions requiring this capability |
| `proven_pred()` | Expressions with `proof_status::proven` |
| `trait_pred(trait_id)` | Expressions satisfying this trait |

Composed predicates with `&&`:

```cpp
// Proven pure GPU computations on f32 tensors
auto q = make_query(astore)
    .where(capability_pred(cap_gpu) && proven_pred() &&
           type_pred<tensor_type_tag>() && effect_pred_none())
    .execute();
```

---

## show_types() Deep Output

When `show_types()` is called, it reads from the `analysis_store` and prints the full record, not
just the bare type:

```flux
input A : tensor<f32>[4,8]
input B : tensor<f32>[8,16]
let C = matmul(A, B)
C.show_types()
```

```
Expression: matmul(A, B)
  Type:         tensor<f32>
  Shape:        [4, 16]
  Effects:      pure (none)
  Capabilities: gpu, simd
  Proofs:       shape-compatible (proven)
  Traits:       Numeric, Tensor, MatMulCompatible
```

---

## Type Rewriting: Normalize the Type Universe

`type_rewrite_engine` applies rules to simplify type terms. Example built-in rule:
`Optional<Optional<T>>` → `Optional<T>` (option collapsing).

For Flux, we register a shape normalization rule: `tensor<T>[]` (rank-0 tensor) → scalar `T`:

```cpp
vakya::types::type_rewrite_engine engine;

// Custom rule: rank-0 tensor is a scalar
engine.add_rule({
    .lhs_stable_id = tensor_type_tag::stable_id,
    .name = "tensor-rank0-scalar",
    .rewrite = [](vakya::types::type_ref ref,
                  vakya::types::type_arena& arena) -> std::optional<vakya::types::type_ref>
    {
        auto rank = vakya::types::shape_rank(ref, arena);
        if (rank == 0)
            return vakya::types::shape_element(ref, arena);  // unwrap to scalar
        return std::nullopt;
    }
});

// Apply: normalize all types in the analysis store
for each subexpression:
    auto normalized = engine.normalize(rec.type, arena);
    rec.type = normalized;
```

---

## Putting It All Together: The Flux Analysis Pipeline

```
Flux AST (typed, resolved)
         ↓  flux::vakya_lowerer
vakya::node expression tree
         ↓  vakya::types::analyze()
         │    ├── type_check() — post-order walk with typing_rule<Tag>
         │    ├── shape inference — make_matmul_constraints etc.
         │    ├── effect propagation — union effects up the tree
         │    ├── capability propagation — union caps up the tree
         │    └── proof obligations emitted
         ↓
analysis_store (complete records for all subexpressions)
         ↓  verify() — SMT discharge
         ↓  type_rewrite_engine::normalize()
         ↓  query_builder — find interesting nodes
         ↓
Lithe: cost-aware backend selection using caps from analysis_store
```

### Full Flux example

```flux
input A : tensor<f32>[1024,1024]
input B : tensor<f32>[1024,1024]

requires gpu

let C = matmul(A, B)

C.show_types()
-- Type:         tensor<f32>
-- Shape:        [1024, 1024]
-- Effects:      pure
-- Capabilities: gpu
-- Proofs:       shape-compatible

prove(shape_rank(C) == 2)   -- PROVEN

C.verify_backends()
-- Interpreter: PASS
-- CPU:         PASS (slow)
-- SIMD:        PASS
-- GPU:         PASS

C.run(gpu)
```

---

## What We Have

| Feature | Vakya Header | Flux Use |
|---------|-------------|----------|
| Type terms + arena | `vakya/types.hpp` | All type inference |
| Unification | `vakya/unification.hpp` | Equation solving |
| Constraint routing | `vakya/constraints.hpp` | Capabilities, traits |
| Effect system | `types/effect.hpp` | `pure fn`, IO tracking |
| Capability system | `types/capability.hpp` | `requires gpu/simd` |
| Shape algebra | `types/shape.hpp` | Tensor dimensions |
| Analysis store | `analysis_store.hpp` | Per-expression records |
| Analysis orchestration | `analysis.hpp` | `analyze()` one-call |
| Guarded rewrites | `vakya/rewrite.hpp` | Type-aware optimization |
| SMT verification | `vakya/smt.hpp` + Tarka | `prove()` |
| Query engine | `vakya/query.hpp` | `show_types()`, routing |
| Type normalization | `type_rewrite.hpp` | Simplify type terms |

---

## Next

[Chapter 6 → Shape Inference](ch06_shape_inference.md) — deep dive into how tensor shapes are
inferred, constrained, and verified.
