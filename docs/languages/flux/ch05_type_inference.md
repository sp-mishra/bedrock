# Chapter 5 — Type Inference: Teaching the Compiler What Things Are

## What Is a Type System?

Before we can run `sqrt(x*x + y*y)`, the compiler needs to know: *what kind of value is `x`?*
Is it a 32-bit float? A 64-bit integer? A 1024×1024 tensor?

A **type system** is the compiler's model of the *kinds of values* that expressions can produce.
It serves three purposes:

1. **Error detection** — catch `matmul(scalar, scalar)` before it segfaults at runtime
2. **Code selection** — choose the right machine instruction (`fadd` not `iadd`) for `x + y`
3. **Optimization** — prove `x > 0` → `abs(x) = x`, eliminating a branch

Flux uses a **static, sound type system**: every expression is assigned a type at compile time, and
that type is guaranteed to be correct at runtime (no surprise casts, no undefined behavior from type
confusion).

---

## The Type Universe

### Primitive types

A primitive type is an atom — it has no internal structure the type system cares about:

```flux
i32    -- 32-bit signed integer
i64    -- 64-bit signed integer
u32    -- 32-bit unsigned integer
u64    -- 64-bit unsigned integer
f32    -- 32-bit IEEE-754 float
f64    -- 64-bit IEEE-754 float
bool   -- boolean (true / false)
string -- UTF-8 string
```

### Compound types

Compound types are built from primitives:

```flux
vec<f32>              -- variable-length vector of f32
mat<f32, 4, 4>        -- 4×4 matrix of f32
tensor<f32>[1024,1024] -- rank-2 tensor: 1M f32 values
tuple<i32, f32>        -- product type: (i32, f32) pair
fn(f32) -> f32         -- function type: f32 → f32
```

### Type variables

During inference, we use **type variables** (written `α`, `β`, `T`, `U`) to represent *unknown*
types that will be solved later:

```
let x = 10          -- what is the type of 10? call it α
let y = x + 1       -- α must support "+", so α is numeric
                    -- 1 is also α (must match x)
```

Type variables are placeholders, not dynamically typed values. They are fully resolved before any code runs.

---

## The Central Problem: Type Inference

**Type checking** is easy: if the programmer writes `x : f32`, we trust it.

**Type inference** is harder: deduce `x : f32` *without* the annotation, just from how `x` is used.

```flux
fn distance(x, y) {          -- no type annotations!
    sqrt(x*x + y*y)
}
```

The compiler must deduce:
1. `sqrt` expects a `f32` argument and returns `f32`
2. `x*x + y*y` must have type `f32` (to pass to `sqrt`)
3. `x*x` must have type `f32` (to add with `y*y`)
4. `x` and `y` must each have type `f32` (to multiply)

It reaches this conclusion without any programmer help.

---

## Hindley-Milner Type Inference

### Historical context

Hindley-Milner (HM) type inference was invented by Roger Hindley (1969) and independently by Robin
Milner (1978) for the ML language. It is the foundation of the type systems in:

- **ML** and **OCaml** — the original
- **Haskell** — extended with type classes
- **F#** — Microsoft's ML dialect
- **Rust** — core inference engine (without lifetimes)
- **Swift** — partial HM for expressions
- **Scala** — bidirectional extension of HM

Flux uses a direct implementation of HM Algorithm W.

### The key insight: types as equations

Instead of asking "what is the type of this expression?", HM asks "what type equations must hold for
this program to be well-typed?" Then it *solves* those equations.

```flux
let distance = sqrt(x*x + y*y)
```

Generated equations:
```
type(x)        = α₁               -- x has some type
type(y)        = α₂               -- y has some type
type(x*x)      = α₁               -- x*x has the same type as x (mul preserves type)
type(y*y)      = α₂               -- y*y has same type as y
type(x*x+y*y)  = α₁               -- + requires both operands same type, returns same
α₂             = α₁               -- ... so α₁ = α₂
type(sqrt(...)) = f32             -- sqrt : f32→f32 requires argument is f32
α₁             = f32              -- therefore x : f32
α₂             = f32              -- therefore y : f32
type(distance) = f32              -- sqrt returns f32
```

The solving algorithm is called **unification**.

---

## Unification: Solving Type Equations

### What unification does

Unification takes two type expressions `T1` and `T2` and tries to find a substitution `σ` such that
`σ(T1) = σ(T2)`.

```
Unify(f32, f32)          → succeed (already equal)
Unify(α, f32)            → σ = { α → f32 }
Unify(f32, i32)          → FAIL (constructor clash)
Unify(fn(α)→β, fn(f32)→f32) → σ = { α → f32, β → f32 }
Unify(α, fn(α)→f32)     → FAIL (occurs check: α appears in fn(α)→f32)
```

The **occurs check** prevents infinite types: if `α` appears inside `T`, then `σ = { α → T }` would
create an infinite chain `α = fn(fn(fn(...)→f32)→f32)→f32`. We reject this.

### The Robinson MGU (Most General Unifier)

The standard algorithm due to J.A. Robinson (1965):

```
Unify(T1, T2):
    T1 = apply_substitution(σ, T1)   -- chase any previous bindings
    T2 = apply_substitution(σ, T2)

    if T1 == T2:                      -- syntactically identical → done
        return σ

    if T1 is variable α:
        if α ∈ free_vars(T2):         -- occurs check
            FAIL (infinite type)
        σ = σ ∪ { α → T2 }
        return σ

    if T2 is variable β:              -- symmetric case
        return Unify(T2, T1)

    if T1 = C(args1...) and T2 = C(args2...):  -- same constructor
        for each (a1, a2) in zip(args1, args2):
            σ = Unify(a1, a2)         -- unify children recursively
        return σ

    FAIL (constructor clash or arity mismatch)
```

### Vakya's unification: Robinson MGU + union-find

Vakya implements this in `vakya/unification.hpp` using a **union-find** (disjoint-set forest) data
structure for the substitution. Union-find gives near-O(1) variable chaining with path compression:

```
substitution: union_find over type_var_id values
  find(α)  → chase chains until reaching a non-variable type_ref or root variable
  unite(α, β)  → merge two equivalence classes
```

The `substitution::apply()` method walks a type term, replacing each variable with its current
binding (path-splitting as it goes, amortizing future lookups).

```cpp
// Vakya unify API — returns std::expected<subst_delta, unify_error>
auto result = vakya::types::unify(ref_a, ref_b, subst, arena);
if (!result) {
    switch (result.error().kind) {
        case unify_error_kind::constructor_clash: ...   // f32 ≠ i32
        case unify_error_kind::infinite_type:    ...   // α = fn(α)→β
        case unify_error_kind::arity_mismatch:   ...   // fn(f32,f32) ≠ fn(f32)
    }
}
// result.value() is subst_delta — SmallVector<binding_record, 8>
// apply it: subst.apply_delta(*result)
```

The `subst_delta` (a `SmallVector<8>`) avoids heap allocation for the common case of ≤8 new bindings
per unification step.

---

## Algorithm W in Detail

Algorithm W is Milner's bottom-up type reconstruction procedure. It processes the AST post-order
(leaves first, root last), building up the substitution as it goes.

### Notation

| Symbol | Meaning |
|--------|---------|
| `Γ` | Type environment: name → type scheme |
| `σ` | Substitution: type variable → type |
| `τ` | Monomorphic type (no quantifiers) |
| `∀ᾱ.τ` | Polymorphic type scheme (quantified over variables ᾱ) |
| `⊢` | "entails" / "proves" |
| `Γ ⊢ e : τ` | Under environment Γ, expression e has type τ |

### Typing rules as derivation trees

#### Variables

```
  x : ∀ᾱ.τ ∈ Γ
  β₁,...,βₙ are fresh type variables
─────────────────────────────────────
  Γ ⊢ x : τ[ᾱ → β₁,...,βₙ]
```

When we use `x`, we *instantiate* its type scheme: replace the quantified variables with fresh ones.
This is what allows `identity` to be both `identity(42) : i64` and `identity(3.14) : f64` — each
call site gets a fresh type variable.

#### Let bindings

```
  W(Γ, e) = (σ, τ)
  τ' = generalize(σ(Γ), τ)    -- quantify over free variables not in environment
──────────────────────────────────────────────────────────────
  W(Γ, let x = e in body) adds x : τ' to Γ, then infers body
```

`generalize` is what makes HM polymorphic:

```flux
let identity = fn(x) { x }        -- infer: fn(α) → α
                                    -- generalize: ∀α. α → α
let a = identity(42)               -- instantiate: β₁ → β₁, unify β₁ = i64
let b = identity(3.14)             -- fresh instantiate: β₂ → β₂, unify β₂ = f64
```

Both uses type-check, even though `identity` was defined once.

#### Function definition

```
  β is a fresh type variable
  W(Γ[x : β], body) = (σ, τ)
──────────────────────────────────────────────────────
  W(Γ, fn(x) { body }) = (σ, σ(β) → τ)
```

Give the parameter a fresh type variable, infer the body, and the function type is parameter type → body type.

#### Application / call

```
  W(Γ, f) = (σ₁, τ₁)
  W(σ₁(Γ), e) = (σ₂, τ₂)
  β is a fresh type variable
  σ₃ = unify(σ₂(τ₁), τ₂ → β)
──────────────────────────────────────────────────────
  W(Γ, f(e)) = (σ₃ ∘ σ₂ ∘ σ₁, σ₃(β))
```

The call unifies the function type with the argument type → fresh return variable.

#### If expressions

```
  W(Γ, cond) = (σ₁, τ₁)   unify τ₁ = bool
  W(σ₁(Γ), then) = (σ₂, τ₂)
  W(σ₂(σ₁(Γ)), else_) = (σ₃, τ₃)
  σ₄ = unify(σ₃(τ₂), τ₃)
──────────────────────────────────────────────────────
  W(Γ, if cond then else_) = (σ₄∘..., σ₄(τ₂))
```

Both branches must have the same type — the `if` is an expression, not a statement.

---

## Flux Examples: Inference in Action

### Example 1 — scalar distance

```flux
input x : f32
input y : f32
let distance = sqrt(x*x + y*y)
```

Inference trace:
```
1. x : f32, y : f32  (from annotations)
2. x*x:  Unify(f32, f32) → f32
3. y*y:  Unify(f32, f32) → f32
4. x*x + y*y:  Unify(f32, f32) → f32
5. sqrt : f32 → f32  (builtin type)
   Unify(f32, f32) → f32
6. distance : f32   ✓
```

### Example 2 — polymorphic identity

```flux
fn identity(x) {
    x
}
let a = identity(42)
let b = identity(3.14f)
```

Inference trace:
```
1. Infer identity:
   x : α₁ (fresh)
   body: x : α₁
   identity : α₁ → α₁
   Generalize: ∀α. α → α

2. identity(42):
   Instantiate: ∀α. α → α  →  β₁ → β₁
   42 : i64 (default integer type)
   Unify(β₁, i64) → β₁ = i64
   a : i64  ✓

3. identity(3.14f):
   Instantiate: ∀α. α → α  →  β₂ → β₂   (fresh β₂, not β₁)
   3.14f : f32
   Unify(β₂, f32) → β₂ = f32
   b : f32  ✓
```

### Example 3 — type error

```flux
input x : f32
input flag : bool
let bad = x + flag    -- ERROR: cannot add f32 and bool
```

Inference:
```
1. x : f32
2. flag : bool
3. x + flag: Unify(f32, bool) → FAIL (constructor clash)
   Error: "Type mismatch: cannot unify f32 with bool"
```

### Example 4 — higher-order functions

```flux
fn apply(f, x) {
    f(x)
}
let result = apply(fn(n) { n * n }, 5)
```

Inference:
```
1. Infer apply:
   f : α, x : β (fresh)
   f(x): Unify(α, β → γ) for fresh γ
   α = β → γ
   apply : (β → γ) → β → γ
   Generalize: ∀β γ. (β → γ) → β → γ

2. apply(fn(n){n*n}, 5):
   Instantiate: (β₁ → γ₁) → β₁ → γ₁
   fn(n){n*n}: n : δ, n*n: Unify(δ,δ)→δ, type = δ → δ
   Unify(β₁ → γ₁, δ → δ) → β₁ = δ, γ₁ = δ
   5 : i64, Unify(β₁, i64) → δ = i64
   result : i64  ✓
```

### Example 5 — pipeline inference

```flux
let result =
    range(1, 10000)
        .map(fn(x) { x*x })
        .filter(fn(x) { x > 100 })
        .reduce(sum)
```

Inference:
```
1. range(1, 10000) : vec<i64>
   (range : i64 → i64 → vec<i64>)

2. .map(fn(x) { x*x }):
   map : ∀T U. (T → U) → vec<T> → vec<U>
   Instantiate: (T₁ → U₁) → vec<T₁> → vec<U₁>
   fn(x){x*x}: x : α, x*x : α, type = α → α
   Unify(T₁ → U₁, α → α) → T₁ = α, U₁ = α
   Unify(vec<T₁>, vec<i64>) → T₁ = i64, so α = i64
   result of .map : vec<i64>

3. .filter(fn(x) { x > 100 }):
   filter : ∀T. (T → bool) → vec<T> → vec<T>
   Instantiate: (T₂ → bool) → vec<T₂> → vec<T₂>
   fn(x){x>100}: x : β, x>100 : bool, type = β → bool
   Unify(T₂ → bool, β → bool) → T₂ = β
   Unify(vec<T₂>, vec<i64>) → T₂ = i64
   result of .filter : vec<i64>

4. .reduce(sum):
   reduce : ∀T. (T → T → T) → vec<T> → T
   sum : i64 → i64 → i64
   Unify(T₃ → T₃ → T₃, i64 → i64 → i64) → T₃ = i64
   result : i64  ✓
```

---

## The Vakya Type System: Architecture

Vakya's type system is a layered opt-in stack on top of the core expression library. You pay only for
what you include.

### Type terms (the language of types)

```
τ ::= κ           -- primitive type (f32, i32, bool, ...)
    | α           -- type variable
    | C(τ₁...τₙ) -- type constructor (vec<τ>, fn(τ₁,τ₂,...)->τ, ...)
    | τ₁ → τ₂    -- function type (callable)
    | ∀ᾱ. τ       -- universally quantified type scheme
    | alias(name, τ) -- named alias (expands to τ)
```

These terms live in a `type_arena` — a hash-consed DAG. Two type terms with identical structure share
the same memory and have the same `type_ref` handle.

```cpp
vakya::types::type_arena arena;
vakya::types::type_var_generator gen;

// Create types
auto f32_t = arena.intern_primitive("f32");
auto i64_t = arena.intern_primitive("i64");
auto alpha  = arena.intern_variable(gen.fresh());  // fresh type variable

// fn(f32) → f32
type_ref params[] = { f32_t };
auto fn_f32_f32 = arena.intern_callable(params, f32_t);

// vec<f32>
type_ref args[] = { f32_t };
auto vec_f32 = arena.intern_constructor("vec", args);

// ∀α. α → α  (identity function type)
type_ref forall_params[] = { alpha };
auto forall_body = arena.intern_callable(forall_params, alpha);
auto poly_id = arena.intern_quantified(forall_params, forall_body);
```

### Hash-consing: why two identical types are the same object

`type_arena` uses **hash-consing**: before creating a new type node, it checks whether an identical
one already exists. If so, it returns the existing handle.

```cpp
auto a1 = arena.intern_primitive("f32");
auto a2 = arena.intern_primitive("f32");
assert(a1.index == a2.index);   // same handle — same object
```

This means:
- Type equality is O(1) handle comparison (not recursive tree equality)
- Memory use is proportional to unique types, not type occurrences
- The entire type universe of a typical program fits in a few kilobytes

### Substitution: the union-find

During unification, we bind type variables to types. The `substitution` structure is a
**path-splitting union-find** over `type_var_id` values:

```cpp
vakya::types::substitution subst;

// After unifying α with f32:
subst.bind(alpha_id, f32_ref);

// Later: look up α
auto resolved = subst.apply(alpha_ref, arena);  // returns f32_ref
```

Path-splitting: when `α → β → f32`, the first `apply(α)` traverses the chain; subsequent calls find
`α → f32` directly (one hop). Amortized O(α(n)) — essentially O(1).

---

## Flux → Vakya Type System: Integration

### Step 1: Map Flux annotations to type_refs

When the user writes `input x : f32`, we create a `type_ref` for `f32` in the shared arena.

```cpp
vakya::types::type_ref flux_annot_to_ref(flux_type const& ft,
                                         vakya::types::type_arena& arena,
                                         vakya::types::type_var_generator& gen)
{
    return std::visit([&](auto const& v) -> vakya::types::type_ref {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, flux_type::prim>) {
            // Map Flux prim_type → Vakya primitive name
            static constexpr std::array names = {
                "i32","i64","u32","u64","f32","f64","bool","string"
            };
            return arena.intern_primitive(names[static_cast<int>(v.kind)]);
        }

        if constexpr (std::is_same_v<T, flux_type::var>) {
            // Flux uses uint32_t IDs; Vakya uses type_var_id
            return arena.intern_variable(v.id);
        }

        if constexpr (std::is_same_v<T, flux_type::fn>) {
            std::vector<vakya::types::type_ref> params;
            params.reserve(v.params.size());
            for (auto const& p : v.params)
                params.push_back(flux_annot_to_ref(p, arena, gen));
            auto ret = flux_annot_to_ref(*v.ret, arena, gen);
            return arena.intern_callable(std::span(params), ret);
        }

        if constexpr (std::is_same_v<T, flux_type::tensor>) {
            // tensor<f32>[4,8] → shape type (see Chapter 6)
            auto elem = flux_annot_to_ref(*v.element, arena, gen);
            return vakya::types::intern_shape(arena, v.shape.dims, elem);
        }

        if constexpr (std::is_same_v<T, flux_type::vec>) {
            auto elem = flux_annot_to_ref(*v.element, arena, gen);
            vakya::types::type_ref args[] = { elem };
            return arena.intern_constructor("vec", args);
        }

        if constexpr (std::is_same_v<T, flux_type::tup>) {
            std::vector<vakya::types::type_ref> elems;
            for (auto const& e : v.elements)
                elems.push_back(flux_annot_to_ref(e, arena, gen));
            return arena.intern_constructor("tuple", std::span(elems));
        }

        // Named type (user-defined): intern by name
        if constexpr (std::is_same_v<T, flux_type::named>)
            return arena.intern_constructor(v.name, {});

        return arena.intern_variable(gen.fresh());  // fallback fresh var
    }, ft.data);
}
```

### Step 2: Install builtin type schemes

Builtin functions have polymorphic type schemes — they work for multiple concrete types.

```cpp
// Install builtins into the type environment
void install_builtin_types(
    std::unordered_map<std::string, vakya::types::type_ref>& env,
    vakya::types::type_arena& arena,
    vakya::types::type_var_generator& gen)
{
    //  sqrt : f32 → f32
    {
        auto f32 = arena.intern_primitive("f32");
        type_ref p[] = { f32 };
        env["sqrt"] = arena.intern_callable(p, f32);
    }

    //  abs, exp, log, sin, cos: f32 → f32  (same)
    for (auto name : {"abs","exp","log","sin","cos","floor","ceil","round"}) {
        auto f32 = arena.intern_primitive("f32");
        type_ref p[] = { f32 };
        env[name] = arena.intern_callable(p, f32);
    }

    //  pow : f32 → f32 → f32
    {
        auto f32 = arena.intern_primitive("f32");
        type_ref p[] = { f32, f32 };
        env["pow"] = arena.intern_callable(p, f32);
    }

    //  ∀T. T → T → T  (min, max, clamp element)
    for (auto name : {"min","max"}) {
        auto T = arena.intern_variable(gen.fresh());
        type_ref p[] = { T, T };
        auto body = arena.intern_callable(p, T);
        type_ref qs[] = { T };
        env[name] = arena.intern_quantified(qs, body);
    }

    //  dot : ∀T. vec<T> → vec<T> → T
    {
        auto T   = arena.intern_variable(gen.fresh());
        type_ref ta[] = { T };
        auto vec = arena.intern_constructor("vec", ta);
        type_ref p[] = { vec, vec };
        auto body = arena.intern_callable(p, T);
        type_ref qs[] = { T };
        env["dot"] = arena.intern_quantified(qs, body);
    }

    //  map : ∀T U. (T → U) → vec<T> → vec<U>
    {
        auto T   = arena.intern_variable(gen.fresh());
        auto U   = arena.intern_variable(gen.fresh());
        type_ref ta[] = {T};  auto vec_T = arena.intern_constructor("vec", ta);
        type_ref ua[] = {U};  auto vec_U = arena.intern_constructor("vec", ua);
        type_ref fp[] = {T};  auto fn_TU = arena.intern_callable(fp, U);
        type_ref pp[] = {fn_TU, vec_T};
        auto body = arena.intern_callable(pp, vec_U);
        type_ref qs[] = {T, U};
        env["map"] = arena.intern_quantified(qs, body);
    }

    //  filter : ∀T. (T → bool) → vec<T> → vec<T>
    {
        auto T    = arena.intern_variable(gen.fresh());
        auto bool_ = arena.intern_primitive("bool");
        type_ref ta[] = {T};   auto vec_T = arena.intern_constructor("vec", ta);
        type_ref fp[] = {T};   auto pred  = arena.intern_callable(fp, bool_);
        type_ref pp[] = {pred, vec_T};
        auto body = arena.intern_callable(pp, vec_T);
        type_ref qs[] = {T};
        env["filter"] = arena.intern_quantified(qs, body);
    }

    //  reduce : ∀T. (T → T → T) → T → vec<T> → T
    {
        auto T   = arena.intern_variable(gen.fresh());
        type_ref ta[] = {T};    auto vec_T = arena.intern_constructor("vec", ta);
        type_ref fp[] = {T, T}; auto binop = arena.intern_callable(fp, T);
        type_ref pp[] = {binop, T, vec_T};
        auto body = arena.intern_callable(pp, T);
        type_ref qs[] = {T};
        env["reduce"] = arena.intern_quantified(qs, body);
    }

    //  range : i64 → i64 → vec<i64>
    {
        auto i64  = arena.intern_primitive("i64");
        type_ref ta[] = {i64}; auto vec_i64 = arena.intern_constructor("vec", ta);
        type_ref p[] = {i64, i64};
        env["range"] = arena.intern_callable(p, vec_i64);
    }
}
```

### Step 3: Full type_inferrer implementation

```cpp
// include/languages/flux/type_inference.hpp
#pragma once
#include "ast.hpp"
#include "ast_arena.hpp"
#include <vakya/vakya_types.hpp>
#include <expected>

namespace flux {

struct type_error {
    std::string message;
    node_idx    node;
    std::string lhs_type;  // for clash errors
    std::string rhs_type;
};

class type_inferrer {
public:
    explicit type_inferrer(ast_arena& arena) : arena_(arena) {
        install_builtin_types(type_env_, tara_, gen_);
    }

    std::vector<type_error> infer(node_idx program_root);

    // Expose arena/subst for shape inference (Chapter 6)
    vakya::types::type_arena&   type_arena_mutable() noexcept { return tara_; }
    vakya::types::substitution& subst_mutable()      noexcept { return subst_; }
    vakya::types::type_arena const& type_arena()     const noexcept { return tara_; }
    vakya::types::substitution const& subst()        const noexcept { return subst_; }

    // Resolve a type_ref through the substitution to its ground type
    vakya::types::type_ref resolve(vakya::types::type_ref ref) const {
        return subst_.apply(ref, tara_);
    }

    // Pretty-print a type_ref for error messages
    std::string type_name(vakya::types::type_ref ref) const;

private:
    // Core inference — returns the type_ref for expr at idx
    vakya::types::type_ref infer_expr(node_idx idx);

    // Per-node-kind handlers
    vakya::types::type_ref infer_program(program_node& n);
    vakya::types::type_ref infer_let(let_decl_node& n, node_idx self);
    vakya::types::type_ref infer_fn(fn_decl_node& n);
    vakya::types::type_ref infer_input(input_decl_node& n);
    vakya::types::type_ref infer_block(block_node& n);
    vakya::types::type_ref infer_binary(binary_expr_node& n, node_idx self);
    vakya::types::type_ref infer_call(call_expr_node& n, node_idx self);
    vakya::types::type_ref infer_method(method_call_expr_node& n, node_idx self);
    vakya::types::type_ref infer_if(if_expr_node& n, node_idx self);
    vakya::types::type_ref infer_lambda(lambda_node& n, node_idx self);
    vakya::types::type_ref infer_ident(identifier_node& n);
    vakya::types::type_ref infer_int_lit(integer_literal_node& n);
    vakya::types::type_ref infer_float_lit(float_literal_node& n);
    vakya::types::type_ref infer_array(array_expr_node& n, node_idx self);

    // Instantiate a polymorphic type scheme (∀ᾱ.τ) with fresh variables
    vakya::types::type_ref instantiate(vakya::types::type_ref scheme);

    // Generalize: close over free variables not in the environment
    vakya::types::type_ref generalize(vakya::types::type_ref mono);

    // Unify two types; record error on failure
    bool unify(vakya::types::type_ref a, vakya::types::type_ref b, node_idx ctx);

    // Write the resolved type back into the AST node
    void write_back(node_idx idx, vakya::types::type_ref ref);

    ast_arena&               arena_;
    vakya::types::type_arena tara_;
    vakya::types::type_var_generator gen_;
    vakya::types::substitution subst_;
    std::vector<type_error>  errors_;

    // Lexical type environment: name → type_ref (monomorphic or scheme)
    std::unordered_map<std::string, vakya::types::type_ref> type_env_;
};

} // namespace flux
```

### Step 4: Constraint solver integration

For more complex checks beyond unification (e.g., `requires gpu` capabilities, effect tracking),
Vakya provides a `composite_solver` that routes constraints to specialized solvers.

Flux uses the basic `unification_solver` for arithmetic and function types. Capability constraints
flow to the `rule_constraint_solver`:

```cpp
// Capability constraint: require_gpu forces backend selection
// This is an extension constraint (kind >= 1000)

struct flux_capability_constraint_solver {
    static bool handles(vakya::types::constraint_kind k) noexcept {
        return k == flux_capability_kind;
    }

    vakya::types::solve_result solve(vakya::types::constraint const& c,
                                     vakya::types::solve_context& ctx) {
        // Extract capability mask from constraint payload
        auto caps = c.payload_as<capability_mask>();
        ctx.analysis_store.update_for(c.source_expr, [caps](auto& record) {
            record.caps |= caps;
        });
        return vakya::types::solve_result::solved;
    }
};

using flux_solver = vakya::types::composite_solver<
    vakya::types::unification_solver,         // arithmetic, function types
    vakya::types::rule_constraint_solver,     // trait implications
    flux_capability_constraint_solver         // GPU/SIMD capability requirements
>;
```

---

## Type Errors in Flux — Complete Examples

### Arity mismatch

```flux
fn add2(x, y) { x + y }
let bad = add2(1, 2, 3)   -- too many arguments
```
```
Error: Arity mismatch calling add2
  Expected: (α, β) → γ
  Got:      3 arguments
```

### Constructor clash

```flux
input x : f32
input flag : bool
let bad = x * flag
```
```
Error: Type mismatch in operator *
  Left:  f32
  Right: bool
  Cannot unify f32 with bool
```

### Infinite type (self-referential)

```flux
fn loop(f) { loop(f(f)) }
```
```
Error: Infinite type — recursive unification
  α = α → β  (occurs check failed)
```

### Missing return type on branch

```flux
fn abs_val(x : f32) -> f32 {
    if x > 0.0 {
        x
    }
    -- missing else branch
}
```
```
Error: If expression branches must have the same type
  Then branch: f32
  Else branch: unit   (implicit unit for missing else)
  Cannot unify f32 with unit
```

---

## Complete Example: Annotated Type Inference

```cpp
// ch05_example.cpp
#include <lithe/lithe.hpp>
#include <languages/samasa/samasa.hpp>
#include <print>
#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"
#include "type_inference.hpp"

int main() {
    // ── Example 1: scalar distance ────────────────────────────────────────────
    {
        auto arena = flux::build_ast(flux::parse(R"(
            input x : f32
            input y : f32
            let distance = sqrt(x*x + y*y)
        )"), "");

        flux::resolver{arena}.resolve(0);
        flux::type_inferrer inf{arena};
        auto errs = inf.infer(0);

        if (errs.empty())
            std::println("distance : {}", inf.type_name(inf.resolve_decl("distance")));
        // distance : f32
    }

    // ── Example 2: polymorphic function ──────────────────────────────────────
    {
        auto arena = flux::build_ast(flux::parse(R"(
            fn identity(x) { x }
            let a = identity(42)
            let b = identity(3.14f)
        )"), "");

        flux::resolver{arena}.resolve(0);
        flux::type_inferrer inf{arena};
        auto errs = inf.infer(0);

        if (errs.empty()) {
            std::println("identity : {}", inf.type_name(inf.resolve_decl("identity")));
            std::println("a        : {}", inf.type_name(inf.resolve_decl("a")));
            std::println("b        : {}", inf.type_name(inf.resolve_decl("b")));
            // identity : ∀α. α → α
            // a        : i64
            // b        : f32
        }
    }

    // ── Example 3: type error ─────────────────────────────────────────────────
    {
        auto arena = flux::build_ast(flux::parse(R"(
            input x : f32
            input flag : bool
            let bad = x + flag
        )"), "");

        flux::resolver{arena}.resolve(0);
        flux::type_inferrer inf{arena};
        auto errs = inf.infer(0);

        for (auto const& e : errs)
            std::println("Error: {}", e.message);
        // Error: Type mismatch in operator +: cannot unify f32 with bool
    }

    // ── Example 4: pipeline with inference ────────────────────────────────────
    {
        auto arena = flux::build_ast(flux::parse(R"(
            let result =
                range(1, 10000)
                    .map(fn(x) { x*x })
                    .filter(fn(x) { x > 100 })
                    .reduce(sum)
        )"), "");

        flux::resolver{arena}.resolve(0);
        flux::type_inferrer inf{arena};
        auto errs = inf.infer(0);

        if (errs.empty())
            std::println("result : {}", inf.type_name(inf.resolve_decl("result")));
        // result : i64
    }
}
```

---

## What We Have

| Component | Purpose |
|-----------|---------|
| `flux_type` | Flux-level type representation (annotations, AST nodes) |
| `flux_annot_to_ref` | Convert Flux types → Vakya `type_ref` |
| `type_arena` | Hash-consed DAG of type terms |
| `type_var_generator` | Fresh type variable IDs |
| `substitution` | Union-find variable binding (path-splitting) |
| `unify()` | Robinson MGU → `std::expected<subst_delta, unify_error>` |
| `type_inferrer` | Algorithm W over Flux AST |
| Builtin schemes | Polymorphic types for `sqrt`, `map`, `filter`, `reduce`, … |
| `composite_solver` | Route constraints to specialized solvers |
| `type_error` | Structured error with type names for diagnostics |

---

## Next

[Chapter 5b → Vakya Type System Deep Dive](ch05b_vakya_types.md) — the V3 constraint reasoning
stack: `analysis_store`, effects, capabilities, guarded rewrites, SMT verification, and query engine.

[Chapter 6 → Shape Inference](ch06_shape_inference.md) — deduce tensor dimensions.
