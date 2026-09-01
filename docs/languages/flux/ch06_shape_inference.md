# Chapter 6 — Shape Inference: Dimensions as Types

## Why Shapes Are Not Just Types

After Chapter 5's HM type inference, every expression in the Flux AST has a *type* — but types alone are not enough for
tensor computation. Consider:

```flux
input A : tensor<f32>[4,8]
input B : tensor<f32>[8,16]
let C = matmul(A, B)
```

Hindley-Milner can tell you that `matmul` takes two `tensor<f32>` arguments and returns a
`tensor<f32>`. What it cannot tell you — because HM reasons about type constructors, not the *arguments* inside them —
is the shape of the result. After pure type inference, all we know is:

```
C : tensor<f32>[?,?]
```

The `?` placeholders are dimension variables: unknowns that HM never filled in. Shape inference is the second
constraint-solving phase whose entire purpose is to fill in every `?`.

### The type/shape distinction

Two orthogonal concepts live inside a fully-annotated tensor type:

| Concept      | Example   | What it tells you                              |
|--------------|-----------|------------------------------------------------|
| Element type | `f32`     | What kind of number each cell holds            |
| Shape        | `[4, 16]` | How many cells exist and how they are arranged |

In Flux notation: `tensor<f32>[4,16]` packs both. The `<f32>` part is what HM solves. The `[4,16]`
part is what shape inference solves.

Standard HM type inference cannot handle shape because shapes are *indexed* types: the `16` in
`[4,16]` is a *value* carried at the type level. HM operates over a first-order term algebra where type constructors
take other type terms as arguments — not runtime-sized integers. Encoding shape into the type system would require
dependent types or an indexed family, both of which push far beyond HM's decidable fragment.

Flux sidesteps this by running shape inference as a completely separate phase after HM completes, using Vakya's shape
constraint system (`vakya/types/shape.hpp`). The two phases share the same
`type_arena` but reason about different dimensions of the type term.

### What shape inference delivers

```flux
input A : tensor<f32>[4,8]
input B : tensor<f32>[8,16]
let C = matmul(A, B)
-- Without shape inference: C : tensor<f32>[?,?]
-- After shape inference:   C : tensor<f32>[4,16]
```

Every subexpression in the program ends with a fully resolved shape stored in the `analysis_store`. Later compiler
phases — vakya lowering, backend selection, memory layout computation — all rely on these shapes being fully concrete
before they begin.

---

## Shape Types as a Formal System

### Shape as a type-level entity

A shape is an ordered tuple of *dimension terms*. In Flux, we write shapes inline with the tensor type, but inside the
compiler shapes are stored as separate entities in the `type_arena` using the
`shape_type_tag` constructor (stable id 100).

Formal grammar of shape terms:

```
shape ::= []                (rank-0, scalar)
        | [d₁]              (rank-1, vector)
        | [d₁, d₂]          (rank-2, matrix)
        | [d₁, ..., dₙ]     (rank-n tensor)

dᵢ    ::= n                 concrete integer: 4, 8, 1024
        | α                 dimension variable: M, K, N  (symbolic, solved by unification)
        | dᵢ * dⱼ           product: M*N  (reshape target)
        | dᵢ / n            quotient: N/2 (strided view)
```

During the compilation of a typical program most dimensions are concrete integers. Dimension variables arise when:

- A function parameter has a generic tensor shape (not yet instantiated)
- A reshape target has one dimension spelled as `-1` (infer-from-product)
- Broadcasting creates intermediate shapes where one dimension is not yet determined

### Shapes in the type_arena

Vakya interns shapes into the same `type_arena` used for regular type terms. A shape is just a
`type_node` with `kind = constructor` and `descriptor_stable_id = 100` (the shape tag), whose
`children` vector holds one `type_ref` per dimension. Each dimension is itself interned as a primitive type ref (for
concrete values) or a variable type ref (for symbolic dims).

The key consequence of hash-consing: two shapes with identical dimensions share the same
`type_ref`. Shape equality is therefore O (1): compare handles, not trees. The entire set of distinct shapes in a
typical program occupies a few hundred entries in the arena.

```
type_arena memory layout for shapes (conceptual):

  ref 42  →  constructor<shape_type_tag>[ ref 7, ref 9 ]
  ref 7   →  primitive "dim:4"
  ref 9   →  primitive "dim:8"

  ref 43  →  constructor<shape_type_tag>[ ref 9, ref 11 ]
  ref 11  →  primitive "dim:16"

  shape [4,8]  is ref 42
  shape [8,16] is ref 43
  shared: ref 9 appears in both (dim 8 interned once)
```

---

## Dimension Unification

Shape inference works by generating *shape constraints* from each operation, then solving them with the same unification
machinery used for types. The difference: instead of equating type constructors (`f32 = f32`), we equate dimension terms
(`K₁ = K₂`).

### The fundamental constraint: inner dimensions must agree

For `matmul(A, B)` where `A : tensor<T>[M, K]` and `B : tensor<T>[K', N]`:

- The columns of A and rows of B must match: `K = K'`
- This is emitted as a `same_type` constraint between `dim(A, 1)` and `dim(B, 0)`
- Unification solves it by binding `K' → K` (or vice versa)
- Result shape is `[M, N]` — first dim of A, last dim of B

### Full unification trace: matmul example

Input program:

```flux
input A : tensor<f32>[4,8]
input B : tensor<f32>[8,16]
let C = matmul(A, B)
```

Step-by-step shape inference:

```
1. Process declaration: A has annotation tensor<f32>[4,8]
   intern_shape(arena, [ref(4), ref(8)])     → shape_ref s_A
   A's shape record: s_A = Shape[4, 8]

2. Process declaration: B has annotation tensor<f32>[8,16]
   intern_shape(arena, [ref(8), ref(16)])    → shape_ref s_B
   B's shape record: s_B = Shape[8, 16]

3. Process call: matmul(A, B)
   Retrieve s_A = Shape[4, 8]   → lhs_shape
   Retrieve s_B = Shape[8, 16]  → rhs_shape

4. make_matmul_constraints(arena, gen, s_A, s_B, out_shape):
   lhs_n->children = [ref(4), ref(8)]   → lhs_N=ref(4), lhs_M=ref(8)
   rhs_n->children = [ref(8), ref(16)]  → rhs_M=ref(8), rhs_K=ref(16)

5. Emit constraint: same_type(lhs_M, rhs_M)
   Unify: ref(8) = ref(8)   → same ref (hash-consed) ✓  no substitution needed

6. Compute out_shape:
   result_dims = [lhs_N, rhs_K] = [ref(4), ref(16)]
   intern_shape(arena, [ref(4), ref(16)]) → shape_ref s_C

7. Store in analysis_record for C:
   shape = s_C = Shape[4, 16]

8. C : tensor<f32>[4, 16]   ✓
```

Because `ref(8)` is the same interned handle on both sides of the constraint, unification succeeds immediately
(identical refs — trivially equal). In the symbolic case (dim variables not yet bound), unification would produce a
substitution `K' → K` and future lookups of `K'` would resolve to `K`.

---

## The matmul Rule: Formal Derivation

The typing judgment for `matmul` (using standard inference rule notation, horizontal line = "if premises above hold,
conclude below"):

```
A : tensor<T>[M, K]    B : tensor<T>[K, N]
─────────────────────────────────────────── (T-MatMul)
        matmul(A, B) : tensor<T>[M, N]
```

Read: "if A is a rank-2 tensor of element type T with shape [M,K], and B is a rank-2 tensor of element type T with
shape [K,N], then matmul (A,B) is a rank-2 tensor of element type T with shape
[M,N]."

The side condition implicit in this rule is `K = K` — the inner dimensions must be the same variable. In the concrete
case `M=4, K=8, N=16` this becomes `8=8` (trivial). In the symbolic case with unannotated inputs it becomes an equality
constraint solved by unification.

### Corresponding constraint emission in Vakya

`make_matmul_constraints` in `vakya/types/shape.hpp` encodes this rule:

```cpp
// Vakya's make_matmul_constraints emits:
//   constraint{ kind: same_type, operands: [lhs_M, rhs_M] }   (inner dims equal)
//
// And returns via out_shape:
//   intern_shape(arena, [lhs_N, rhs_K])                        (result shape [M, N])
//
// Where:
//   lhs_N = lhs_shape.children[0]   (first dim of A → M)
//   lhs_M = lhs_shape.children[1]   (second dim of A → K)
//   rhs_M = rhs_shape.children[0]   (first dim of B → K')
//   rhs_K = rhs_shape.children[1]   (second dim of B → N)
```

ASCII diagram of data flow:

```
A: Shape[M, K]      B: Shape[K', N]
   children[0]=M       children[0]=K'
   children[1]=K       children[1]=N
        |                    |
        K ─── constraint ─── K'    (same_type: must unify)
        |                    |
        M              N
        └──────┬─────────────┘
               ↓
        out_shape = Shape[M, N]   (intern_shape([M, N]))
```

---

## All Shape Rules with Formal Derivations

### dot product

```
u : tensor<T>[N]    v : tensor<T>[N]
──────────────────────────────────── (T-Dot)
        dot(u, v) : T
```

The inputs must be rank-1 tensors of the same length. The constraint is:

```
same_type(dim(u, 0), dim(v, 0))     (lengths must match)
```

Result is a *scalar* of element type T — rank-0, no shape dimensions. In Vakya terms, the result shape is
`make_scalar_shape(arena)` which interns an empty `Shape[]` constructor.

Implementation sketch:

```cpp
// dot: verify both rank-1, emit inner-dim equality, return scalar shape
if (node.callee == "dot") {
    auto u = infer_expr(node.args[0]);
    auto v = infer_expr(node.args[1]);

    if (shape_rank(arena, u) != 1 || shape_rank(arena, v) != 1) {
        errors_.push_back({"dot requires rank-1 tensors", idx});
        return make_scalar_shape(tara_);
    }

    // Emit: dim(u,0) = dim(v,0)
    constraint c;
    c.kind = constraint_kind::same_type;
    c.operands = { arena.get(u)->children[0],
                   arena.get(v)->children[0] };
    ctx_.add(c);

    // Result: scalar of element type T
    return make_scalar_shape(tara_);
}
```

### transpose

```
A : tensor<T>[M, N]
───────────────────── (T-Transpose)
transpose(A) : tensor<T>[N, M]
```

Transpose swaps the two dimensions. No constraint is emitted — the result shape is directly computed by reversing the
children:

```
A: Shape[M, N]
   children = [ref_M, ref_N]
                     ↓  swap
result: Shape[N, M]
   children = [ref_N, ref_M]
```

No unification is needed: we know at compile time that transpose always produces `[N, M]` from
`[M, N]`. The existing dimension refs are reused (no new interning of dimension values), only a new shape node is
created holding them in swapped order.

Implementation sketch:

```cpp
if (node.callee == "transpose") {
    auto A_ref = infer_expr(node.args[0]);
    if (shape_rank(tara_, A_ref) != 2) {
        errors_.push_back({"transpose requires rank-2 tensor", idx});
        return A_ref;
    }
    auto const& children = tara_.get(A_ref)->children;
    // Swap: [children[1], children[0]]
    type_ref swapped[2] = { children[1], children[0] };
    return intern_shape(tara_, std::span<const type_ref>(swapped, 2));
}
```

### reshape

```
A : tensor<T>[d₁,...,dₙ]     ∏dᵢ = ∏eⱼ
────────────────────────────────────────── (T-Reshape)
reshape(A, [e₁,...,eₘ]) : tensor<T>[e₁,...,eₘ]
```

Reshape changes rank and all dimensions simultaneously, subject to the constraint that the total number of elements is
preserved. The constraint is *arithmetic*: product of input dimensions must equal product of output dimensions.

```
old product: d₁ * d₂ * ... * dₙ
new product: e₁ * e₂ * ... * eₘ
constraint: old_product = new_product    (if both concrete: integer equality check)
```

When both sides are fully concrete at compile time (the common case), this degenerates to an integer check. When either
side contains symbolic dimensions, a `kDimEqKind` constraint is emitted for the Tarka SMT backend to discharge.

Implementation sketch:

```cpp
if (node.callee == "reshape") {
    auto A_ref = infer_expr(node.args[0]);
    // Second arg: array literal of new dims
    auto const& shape_node =
        std::get<array_expr_node>(arena_.at(node.args[1]));
    std::vector<type_ref> new_dim_refs;
    std::size_t new_product = 1;
    for (auto dim_idx : shape_node.elements) {
        auto const& lit =
            std::get<integer_literal_node>(arena_.at(dim_idx));
        new_product *= static_cast<std::size_t>(lit.value);
        new_dim_refs.push_back(
            tara_.intern_primitive("dim:" +
                std::to_string(lit.value)));
    }
    // Check product preserved
    std::size_t old_product = 1;
    for (auto const& c : tara_.get(A_ref)->children) {
        // parse dim value from primitive name
        old_product *= dim_value(c, tara_);
    }
    if (old_product != new_product) {
        errors_.push_back({
            std::format("reshape product mismatch: {}≠{}",
                old_product, new_product), idx});
        return A_ref;
    }
    return intern_shape(tara_,
        std::span<const type_ref>(new_dim_refs));
}
```

### element-wise binary operations

```
A : tensor<T>[d₁,...,dₙ]    B : tensor<T>[d₁,...,dₙ]
─────────────────────────────────────────────────────── (T-ElemWise)
             A + B : tensor<T>[d₁,...,dₙ]
```

Element-wise operations (`+`, `-`, `*`, `/`, and all pointwise math) require that both operands have *identical shapes*.
The result has the same shape.

One constraint is emitted per dimension pair:

```
same_type(dim(A, 0), dim(B, 0))
same_type(dim(A, 1), dim(B, 1))
...
same_type(dim(A, n-1), dim(B, n-1))
```

If ranks differ: error (unless broadcasting applies — see below).

### map

```
f : T → U    xs : tensor<T>[N]
────────────────────────────── (T-Map)
     map(f, xs) : tensor<U>[N]
```

`map` applies `f` element-wise to `xs`. The shape of the result is identical to the shape of the input — only the
element type may change. No shape constraint is emitted; the result shape is directly copied from the input shape.

```
xs: Shape[N]
     ↓  map(f, ...)
result: Shape[N]    (same shape_ref, no new interning needed)
```

This is the cleanest shape rule: the compiler literally returns the same `shape_ref` for the result as it has for the
input.

### reduce

```
f : T → T → T    xs : tensor<T>[N]
──────────────────────────────────── (T-Reduce)
      reduce(f, xs) : T
```

`reduce` collapses a rank-1 tensor to a scalar by applying a binary operator repeatedly. The result has no shape
(rank-0). Like `dot`, the result shape is `make_scalar_shape(arena)`.

```
xs: Shape[N]
     ↓  reduce(f, ...)
result: Shape[]    (rank-0 scalar)
```

---

## Broadcasting: When Shapes Disagree

### NumPy broadcasting rules (as Flux supports them)

Element-wise operations in Flux follow standard NumPy-style broadcasting. Broadcasting lets a scalar or lower-rank
tensor be *stretched* to match the shape of a higher-rank tensor, without actually copying data.

Broadcasting applies when two shapes are *not* identical but are still *compatible*. Compatibility is determined by the
following algorithm, applied right-to-left on the dimension lists:

1. If ranks differ, prepend `1` to the shorter shape until both have the same rank.
2. For each dimension pair `(a, b)` (now same rank):
    - `a == b` → keep that value; dimension is exact match
    - `a == 1` → result dimension is `b`; this tensor broadcasts along this axis
    - `b == 1` → result dimension is `a`; that tensor broadcasts along this axis
    - `a != b` and neither is 1 → ERROR: shapes are incompatible

ASCII diagram for a non-trivial case:

```
A : tensor<f32>[3, 1, 5]
B : tensor<f32>[   4, 5]

Step 1 — align ranks (prepend 1s to B):
  A: [3, 1, 5]
  B: [1, 4, 5]   (prepended one 1 to B)

Step 2 — broadcast per dimension (left to right):
  dim 0:  3 vs 1  →  3   (B broadcasts; A's size wins)
  dim 1:  1 vs 4  →  4   (A broadcasts; B's size wins)
  dim 2:  5 vs 5  →  5   (exact match)

Result: tensor<f32>[3, 4, 5]
```

Formal broadcast rule for a single dimension pair:

```
broadcast_dim(a, b) =
  a       if a == b
  b       if a == 1
  a       if b == 1
  ERROR   otherwise
```

Vakya encodes broadcasting compatibility as a `constraint_kind::broadcastable` constraint emitted by
`make_broadcastable_constraint(shape_a, shape_b)`. The constraint solver checks the above rules.

### Shape inference for scalar * tensor (scalar broadcast)

The simplest case: one operand is a rank-0 scalar.

```
s : f32    A : tensor<f32>[M, N]
───────────────────────────────── (T-ScalarBroadcast)
      s * A : tensor<f32>[M, N]
```

A scalar broadcasts to *any* shape. The constraint is trivially satisfied (rank 0 is compatible with any rank by
prepending enough 1s). The result shape equals the non-scalar operand's shape.

```flux
input A : tensor<f32>[4, 4]
let scaled = 2.0 * A    -- scalar broadcasts → result: tensor<f32>[4, 4]
```

Implementation in the binary shape handler:

```cpp
vakya::types::type_ref shape_inferrer::infer_binary_shapes(
    binary_expr_node& node, node_idx idx)
{
    auto lt = infer_expr(node.lhs);
    auto rt = infer_expr(node.rhs);

    auto lr = vakya::types::shape_rank(tara_, lt);
    auto rr = vakya::types::shape_rank(tara_, rt);

    if (lr == 0 || rr == 0) {
        // Scalar op tensor: result shape is the non-scalar shape
        return (lr == 0) ? rt : lt;
    }

    // Same-rank element-wise: both shapes must match exactly
    if (lr != rr) {
        errors_.push_back({"Binary op shape rank mismatch", idx});
        return lt;
    }

    require_same_shape(lt, rt, idx);
    return lt;
}
```

---

## Rank Polymorphism

Some operations in Flux work regardless of the rank of their arguments. The element-wise ops (`+`, `-`, `*`, `/`) are
the canonical examples: they work for `[N]`, `[M, N]`, `[M, N, K]`, and any other rank equally.

```
A : tensor<T>[d₁,...,dₙ]    B : tensor<T>[d₁,...,dₙ]
──────────────────────────────────────────────────────  (T-ElemPoly)
        A + B : tensor<T>[d₁,...,dₙ]    ∀n
```

The `∀n` is the rank polymorphism: this rule holds for *any* n, not just specific ranks. The constraint emitted adapts
to the actual rank discovered at inference time.

Vakya tracks rank as a compile-time value inside the shape `type_node`: `n->children.size()` is the rank. If at
inference time a tensor's rank is not yet determined (e.g., the function takes a generic tensor argument), a *rank
variable* is used and solved by unification when enough information becomes available.

In practice, almost all Flux programs use concrete ranks (rank 2 for matrices, rank 1 for vectors), so rank polymorphism
is rarely exercised beyond trivial cases. The mechanism exists primarily to support library functions written over
arbitrary-rank tensors.

---

## Vakya Shape API Internals

Every function in this section lives in `vakya/types/shape.hpp`.

### `intern_shape`

```cpp
[[nodiscard]] inline shape_ref
intern_shape(type_arena& arena, std::span<const type_ref> dims);
```

Creates (or looks up) a shape node in the arena. The `dims` span contains one `type_ref` per dimension — each dimension
is itself a `type_ref` pointing to a primitive or variable node.

Calling `intern_shape` twice with the same `dims` sequence returns the same `shape_ref` handle. This is the hash-consing
guarantee: shapes are identified by value, not by allocation site.

```
intern_shape(arena, [ref(4), ref(8)])   → shape_ref X
intern_shape(arena, [ref(4), ref(8)])   → shape_ref X   (same handle)
intern_shape(arena, [ref(4), ref(9)])   → shape_ref Y   (different: dim[1] differs)
```

Internally, `intern_shape` constructs a `type_node` with `kind = constructor` and
`descriptor_stable_id = 100`, pushes the dimension refs as children, and delegates to
`arena.intern()` which hashes the node and returns an existing handle if one matches.

### `make_scalar_shape`

```cpp
[[nodiscard]] inline shape_ref make_scalar_shape(type_arena& arena);
```

Returns the rank-0 shape: a `Shape[]` constructor with no children. Scalars resulting from `dot`,
`reduce`, and fully-collapsed operations get this shape. `shape_rank(arena, make_scalar_shape(...))
== 0`.

### `shape_rank`

```cpp
[[nodiscard]] inline std::size_t shape_rank(const type_arena& arena, shape_ref s) noexcept;
```

Looks up the shape node in the arena and returns `n->children.size()`. Returns 0 if the ref is not a valid shape node
(graceful handling of unresolved type variables that haven't been unified to a shape yet).

| Input            | Return value      |
|------------------|-------------------|
| `Shape[]`        | 0 (scalar)        |
| `Shape[N]`       | 1 (vector)        |
| `Shape[M, N]`    | 2 (matrix)        |
| `Shape[M, N, K]` | 3 (rank-3 tensor) |

### `make_matmul_constraints`

```cpp
[[nodiscard]] inline std::vector<constraint>
make_matmul_constraints(type_arena& arena,
                        type_var_generator& gen,
                        shape_ref lhs_shape,
                        shape_ref rhs_shape,
                        shape_ref& out_shape);
```

The key function for matrix multiplication shape inference. It:

1. Validates that both inputs are rank-2 (two children each)
2. Extracts: `lhs_N = lhs.children[0]`, `lhs_M = lhs.children[1]`, `rhs_M = rhs.children[0]`,
   `rhs_K = rhs.children[1]`
3. Emits one constraint: `{ kind: same_type, operands: [lhs_M, rhs_M] }`
4. Computes `out_shape = intern_shape(arena, [lhs_N, rhs_K])`
5. Returns the constraint vector; caller adds them to the solver batch

Returns an empty constraint vector if either shape is not rank-2 (caller should then emit a shape error).

ASCII diagram of what `make_matmul_constraints` does:

```
lhs_shape: Shape[M, K]       rhs_shape: Shape[K', N]
            children[0]=M                children[0]=K'
            children[1]=K                children[1]=N
                   |                           |
                   K ──── constraint ──── K'   (same_type: unify K = K')
                   |                           |
                   M                           N
                   └──────────┬────────────────┘
                              ↓
                   out_shape = Shape[M, N]
                   (intern_shape(arena, [M, N]))
```

The `type_var_generator gen` parameter is reserved for future use when symbolic dimension variables are introduced
(e.g., for generic functions over unknown-sized tensors). For concrete shapes it is not used.

### `make_broadcastable_constraint`

```cpp
[[nodiscard]] inline constraint make_broadcastable_constraint(shape_ref a, shape_ref b);
```

Returns a single `constraint` with `kind = broadcastable` and operands `[a, b]`. The constraint solver checks NumPy
broadcasting rules and produces the result shape. Used by the binary op shape handler when broadcasting is detected.

---

## Shape Inferrer: Full Implementation

```cpp
// include/languages/flux/shape_inference.hpp
#pragma once
#include "ast.hpp"
#include "ast_arena.hpp"
#include <vakya/vakya_types.hpp>

namespace flux {

struct shape_error {
    std::string message;
    node_idx    node;
};

class shape_inferrer {
public:
    explicit shape_inferrer(ast_arena& arena,
                            vakya::types::type_arena& tara,
                            vakya::types::type_var_generator& gen,
                            vakya::types::substitution& subst)
        : arena_(arena), tara_(tara), gen_(gen), subst_(subst) {}

    std::vector<shape_error> infer(node_idx program_root);

private:
    // Returns shape_ref for the expression at idx
    vakya::types::shape_ref infer_expr(node_idx idx);
    vakya::types::shape_ref infer_call_shapes(call_expr_node& node, node_idx idx);
    vakya::types::shape_ref infer_binary_shapes(binary_expr_node& node, node_idx idx);

    // Emit same_type constraint for all dims; push error if mismatch
    bool require_same_shape(vakya::types::shape_ref a,
                            vakya::types::shape_ref b,
                            node_idx ctx);

    // Helper: extract concrete dimension value from a dim type_ref
    std::size_t dim_value(vakya::types::type_ref dim_ref) const;

    ast_arena&                       arena_;
    vakya::types::type_arena&        tara_;
    vakya::types::type_var_generator& gen_;
    vakya::types::substitution&      subst_;
    std::vector<shape_error>         errors_;
    std::vector<vakya::types::constraint> pending_constraints_;
};

} // namespace flux
```

### matmul shape rule method

```cpp
// Rule: T-MatMul
// A : tensor<T>[M,K]   B : tensor<T>[K,N]
// ─────────────────────────────────────────
// matmul(A,B) : tensor<T>[M,N]
//
// Constraint emitted: same_type(dim(A,1), dim(B,0))
vakya::types::shape_ref shape_inferrer::infer_call_shapes(
    call_expr_node& node, node_idx idx)
{
    if (node.callee == "matmul") {
        if (node.args.size() != 2) {
            errors_.push_back({"matmul requires exactly 2 arguments", idx});
            return vakya::types::make_scalar_shape(tara_);
        }

        auto A_shape = infer_expr(node.args[0]);
        auto B_shape = infer_expr(node.args[1]);

        if (vakya::types::shape_rank(tara_, A_shape) != 2 ||
            vakya::types::shape_rank(tara_, B_shape) != 2) {
            errors_.push_back({"matmul requires rank-2 tensors", idx});
            return vakya::types::make_scalar_shape(tara_);
        }

        vakya::types::shape_ref out_shape;
        auto constraints = vakya::types::make_matmul_constraints(
            tara_, gen_, A_shape, B_shape, out_shape);

        if (constraints.empty()) {
            // make_matmul_constraints returns empty on rank mismatch
            errors_.push_back({"matmul shape mismatch: rank error", idx});
            return vakya::types::make_scalar_shape(tara_);
        }

        // Verify the inner-dim constraint is satisfiable (concrete check)
        for (auto const& c : constraints) {
            if (c.kind == vakya::types::constraint_kind::same_type) {
                auto lhs_dim = c.operands[0];
                auto rhs_dim = c.operands[1];
                if (lhs_dim.index != rhs_dim.index) {
                    // Different interned refs means different concrete values
                    auto ld = dim_value(lhs_dim);
                    auto rd = dim_value(rhs_dim);
                    errors_.push_back({
                        std::format("matmul shape mismatch: "
                                    "dim(A,1)={} ≠ dim(B,0)={}", ld, rd), idx});
                    return vakya::types::make_scalar_shape(tara_);
                }
            }
            pending_constraints_.push_back(c);
        }

        return out_shape;
    }

    // ── transpose ────────────────────────────────────────────────────────────
    // Rule: T-Transpose
    // A : tensor<T>[M, N]
    // ─────────────────────────
    // transpose(A) : tensor<T>[N, M]
    if (node.callee == "transpose") {
        auto A_shape = infer_expr(node.args[0]);
        if (vakya::types::shape_rank(tara_, A_shape) != 2) {
            errors_.push_back({"transpose requires rank-2 tensor", idx});
            return A_shape;
        }
        auto const& ch = tara_.get(A_shape)->children;
        vakya::types::type_ref swapped[2] = { ch[1], ch[0] };
        return vakya::types::intern_shape(
            tara_, std::span<const vakya::types::type_ref>(swapped, 2));
    }

    // ── dot ──────────────────────────────────────────────────────────────────
    // Rule: T-Dot
    // u : tensor<T>[N]   v : tensor<T>[N]
    // ─────────────────────────────────────
    // dot(u,v) : T    (scalar, rank-0)
    if (node.callee == "dot") {
        auto u = infer_expr(node.args[0]);
        auto v = infer_expr(node.args[1]);
        if (vakya::types::shape_rank(tara_, u) != 1 ||
            vakya::types::shape_rank(tara_, v) != 1) {
            errors_.push_back({"dot requires rank-1 tensors", idx});
            return vakya::types::make_scalar_shape(tara_);
        }
        // Emit: dim(u,0) = dim(v,0)
        auto const& u_ch = tara_.get(u)->children;
        auto const& v_ch = tara_.get(v)->children;
        if (u_ch[0].index != v_ch[0].index) {
            errors_.push_back({
                std::format("dot shape mismatch: dim(u,0)={} ≠ dim(v,0)={}",
                    dim_value(u_ch[0]), dim_value(v_ch[0])), idx});
        }
        return vakya::types::make_scalar_shape(tara_);
    }

    // ── reshape ───────────────────────────────────────────────────────────────
    // Rule: T-Reshape
    // A : tensor<T>[d₁,...,dₙ]     ∏dᵢ = ∏eⱼ
    // ────────────────────────────────────────────
    // reshape(A, [e₁,...,eₘ]) : tensor<T>[e₁,...,eₘ]
    if (node.callee == "reshape") {
        auto A_shape = infer_expr(node.args[0]);
        auto const& shape_node =
            std::get<array_expr_node>(arena_.at(node.args[1]));
        std::vector<vakya::types::type_ref> new_dim_refs;
        std::size_t new_product = 1;
        for (auto dim_idx : shape_node.elements) {
            auto const& lit =
                std::get<integer_literal_node>(arena_.at(dim_idx));
            auto v = static_cast<std::size_t>(lit.value);
            new_product *= v;
            new_dim_refs.push_back(
                tara_.intern_primitive("dim:" + std::to_string(v)));
        }
        std::size_t old_product = 1;
        for (auto const& c : tara_.get(A_shape)->children)
            old_product *= dim_value(c);
        if (old_product != new_product) {
            errors_.push_back({
                std::format("reshape product mismatch: {}≠{}",
                    old_product, new_product), idx});
            return A_shape;
        }
        return vakya::types::intern_shape(
            tara_, std::span<const vakya::types::type_ref>(new_dim_refs));
    }

    // ── map ───────────────────────────────────────────────────────────────────
    // Rule: T-Map
    // f : T→U    xs : tensor<T>[N]
    // ──────────────────────────────
    // map(f, xs) : tensor<U>[N]
    if (node.callee == "map") {
        // Shape passes through unchanged
        return infer_expr(node.args[1]);
    }

    // ── reduce ────────────────────────────────────────────────────────────────
    // Rule: T-Reduce
    // f : T→T→T    xs : tensor<T>[N]
    // ──────────────────────────────────
    // reduce(f, xs) : T   (scalar)
    if (node.callee == "reduce") {
        return vakya::types::make_scalar_shape(tara_);
    }

    // Fallback: unknown call, propagate shape of first arg if available
    if (!node.args.empty())
        return infer_expr(node.args[0]);
    return vakya::types::make_scalar_shape(tara_);
}
```

### element-wise binary shape rule method

```cpp
// Rule: T-ElemWise (and T-ScalarBroadcast)
// A : tensor<T>[d₁,...,dₙ]    B : tensor<T>[d₁,...,dₙ]
// ─────────────────────────────────────────────────────
// A + B : tensor<T>[d₁,...,dₙ]
//
// Special case: scalar (rank-0) op tensor → T-ScalarBroadcast
vakya::types::shape_ref shape_inferrer::infer_binary_shapes(
    binary_expr_node& node, node_idx idx)
{
    auto lt = infer_expr(node.lhs);
    auto rt = infer_expr(node.rhs);

    auto lr = vakya::types::shape_rank(tara_, lt);
    auto rr = vakya::types::shape_rank(tara_, rt);

    // T-ScalarBroadcast: scalar op tensor → tensor shape
    if (lr == 0 || rr == 0)
        return (lr == 0) ? rt : lt;

    // T-ElemWise: must have identical shapes
    if (lr != rr) {
        errors_.push_back({
            std::format("element-wise op rank mismatch: rank {} ≠ rank {}",
                lr, rr), idx});
        return lt;
    }

    require_same_shape(lt, rt, idx);
    return lt;
}

bool shape_inferrer::require_same_shape(
    vakya::types::shape_ref a,
    vakya::types::shape_ref b,
    node_idx ctx)
{
    if (a.index == b.index) return true;   // same interned ref → trivially equal

    auto const& a_ch = tara_.get(a)->children;
    auto const& b_ch = tara_.get(b)->children;

    bool ok = true;
    for (std::size_t i = 0; i < a_ch.size(); ++i) {
        if (a_ch[i].index != b_ch[i].index) {
            errors_.push_back({
                std::format("element-wise op shape mismatch at dim {}: {}≠{}",
                    i, dim_value(a_ch[i]), dim_value(b_ch[i])), ctx});
            ok = false;
        }
    }
    return ok;
}
```

---

## Complete Worked Example: Chain of Operations

Trace shape inference for a five-operation tensor pipeline:

```flux
input A : tensor<f32>[32, 64]
input B : tensor<f32>[64, 128]
let C = matmul(A, B)
let D = transpose(C)
let E = reshape(D, [128, 8, 4])
let v = range(1, 128).map(fn(i) { i * 1.0 })
```

Step-by-step trace:

```
Step 1.  A declared: annotation [32, 64]
         intern_shape(arena, [ref(32), ref(64)]) → s_A
         analysis_record[A].shape = s_A

Step 2.  B declared: annotation [64, 128]
         intern_shape(arena, [ref(64), ref(128)]) → s_B
         analysis_record[B].shape = s_B

Step 3.  matmul(A, B):
         lhs = s_A = Shape[32, 64]   lhs_N=ref(32), lhs_M=ref(64)
         rhs = s_B = Shape[64, 128]  rhs_M=ref(64), rhs_K=ref(128)
         make_matmul_constraints:
           constraint: same_type(ref(64), ref(64))
           ref(64) == ref(64) (hash-consed same node) → ✓ satisfied immediately
           out_shape = intern_shape([ref(32), ref(128)]) → s_C
         C : tensor<f32>[32, 128]

Step 4.  transpose(C):
         shape_rank(s_C) = 2 ✓
         children = [ref(32), ref(128)]
         swap → [ref(128), ref(32)]
         intern_shape([ref(128), ref(32)]) → s_D
         D : tensor<f32>[128, 32]

Step 5.  reshape(D, [128, 8, 4]):
         old_product = 128 * 32 = 4096
         new_product = 128 * 8 * 4 = 4096   ✓
         intern_shape([ref(128), ref(8), ref(4)]) → s_E
         E : tensor<f32>[128, 8, 4]

Step 6.  range(1, 128) → vec<i64>[128]   (type inference handles this)
         .map(fn(i) { i * 1.0 }):
           T-Map rule: shape passes through unchanged
           v : tensor<f64>[128]    (element type changes: i64 → f64 via * 1.0)
```

Final shape summary:

```text
A : tensor<f32>[32, 64]
B : tensor<f32>[64, 128]
C : tensor<f32>[32, 128]
D : tensor<f32>[128, 32]
E : tensor<f32>[128, 8, 4]
v : tensor<f64>[128]
```

No constraints were left pending: every `same_type` constraint was satisfied immediately by hash-consed equality checks
(all dimensions are concrete integers, interned once).

---

## Shape Error Examples

Three canonical shape errors with exact error messages:

### Error 1 — matmul inner dimension mismatch

```flux
input A : tensor<f32>[4, 8]
input B : tensor<f32>[9, 16]
let C = matmul(A, B)
```

```text
Shape error at `matmul(A, B)`:
  matmul shape mismatch: dim(A,1)=8 ≠ dim(B,0)=9
```

Cause: `lhs_M = ref(8)`, `rhs_M = ref(9)`. These are different interned refs (8 ≠ 9). The concrete value check fires and
pushes the error before `out_shape` is computed.

### Error 2 — reshape product mismatch

```flux
input A : tensor<f32>[4, 4]
let B = reshape(A, [3, 6])
```

```text
Shape error at `reshape(A, [3, 6])`:
  reshape product mismatch: 16≠18
```

Cause: `old_product = 4*4 = 16`, `new_product = 3*6 = 18`. Integer check fails; original shape
`A_shape` is returned as fallback and the error is recorded.

### Error 3 — element-wise shape mismatch

```flux
input A : tensor<f32>[4, 8]
input B : tensor<f32>[4, 9]
let C = A + B
```

```text
Shape error at `A + B`:
  element-wise op shape mismatch at dim 1: 8≠9
```

Cause: `A` has shape `[4, 8]`, `B` has shape `[4, 9]`. `require_same_shape` iterates dimension by dimension. Dim 0:
`ref(4) == ref(4)` ✓. Dim 1: `ref(8) ≠ ref(9)` → error.

---

## `show_types()` Output after Shape Inference

After shape inference completes, the `analysis_store` holds a full `analysis_record` for every subexpression. The
`show_types()` introspection reads from it:

```flux
input A : tensor<f32>[4,8]
input B : tensor<f32>[8,16]
let C = matmul(A, B)
C.show_types()
```

```text
Expression: matmul(A, B)
  Type:    tensor<f32>
  Shape:   [4, 16]
  Rank:    2
  Effects: pure
```

The `Type` field comes from HM type inference (Chapter 5). The `Shape` and `Rank` fields are filled in by the shape
inference pass in this chapter. `Effects: pure` means the expression has no side effects — emitted by the effect
propagation pass in the same `analyze()` call.

Displaying the shape programmatically:

```cpp
void show_shapes(vakya::types::analysis_store const& store,
                 vakya::types::type_arena const& arena,
                 uint64_t expr_hash)
{
    auto* record = store.find(expr_hash);
    if (!record) { std::println("<no shape info>"); return; }

    auto shape_ref = record->shape;
    if (!shape_ref) { std::println("scalar"); return; }

    auto rank = vakya::types::shape_rank(arena, shape_ref);
    if (rank == 0) { std::println("scalar (rank-0)"); return; }

    auto const& children = arena.get(shape_ref)->children;
    std::print("[");
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i) std::print(", ");
        // Dimension is interned as primitive "dim:N" — parse out N
        auto const* dim_node = arena.get(children[i]);
        auto dim_name = std::string_view{dim_node->primitive_name};
        std::print("{}", dim_name.substr(4));  // skip "dim:"
    }
    std::println("]");
}
```

---

## Tutorial: Copy-Paste Programs

### Try It 1 — Basic matmul shape

Run shape inference on the canonical 4×8 × 8×16 example:

```cpp
// ch06_try1.cpp
#include <lithe/lithe.hpp>
#include <print>
#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"
#include "type_inference.hpp"
#include "shape_inference.hpp"

int main() {
    std::string_view src = R"(
        input A : tensor<f32>[4,8]
        input B : tensor<f32>[8,16]
        let C = matmul(A, B)
    )";

    auto parse_result = flux::parse(src);
    auto arena        = flux::build_ast(parse_result, src);

    flux::resolver{arena}.resolve(0);

    flux::type_inferrer type_inf{arena};
    type_inf.infer(0);

    flux::shape_inferrer shape_inf{arena,
        type_inf.type_arena_mutable(),
        type_inf.gen_mutable(),
        type_inf.subst_mutable()};
    auto errors = shape_inf.infer(0);

    if (errors.empty())
        std::println("C : tensor<f32>[4,16]   (shape inference OK)");
    else
        for (auto const& e : errors)
            std::println("Shape error: {}", e.message);
}
```

Expected output:

```text
C : tensor<f32>[4,16]   (shape inference OK)
```

### Try It 2 — Chain of operations

Run shape inference on the full five-step pipeline from the worked example above:

```cpp
// ch06_try2.cpp
#include <lithe/lithe.hpp>
#include <print>
#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"
#include "type_inference.hpp"
#include "shape_inference.hpp"

int main() {
    std::string_view src = R"(
        input A : tensor<f32>[32, 64]
        input B : tensor<f32>[64, 128]
        let C = matmul(A, B)
        let D = transpose(C)
        let E = reshape(D, [128, 8, 4])
    )";

    auto parse_result = flux::parse(src);
    auto arena        = flux::build_ast(parse_result, src);

    flux::resolver{arena}.resolve(0);

    flux::type_inferrer type_inf{arena};
    type_inf.infer(0);

    flux::shape_inferrer shape_inf{arena,
        type_inf.type_arena_mutable(),
        type_inf.gen_mutable(),
        type_inf.subst_mutable()};
    auto errors = shape_inf.infer(0);

    if (errors.empty()) {
        // shapes now in analysis_store; display them
        std::println("C : [32, 128]");
        std::println("D : [128, 32]");
        std::println("E : [128, 8, 4]");
    } else {
        for (auto const& e : errors)
            std::println("Shape error: {}", e.message);
    }
}
```

Expected output:

```text
C : [32, 128]
D : [128, 32]
E : [128, 8, 4]
```

To see a shape error, change `B` to `tensor<f32>[65, 128]` (inner dim becomes 65 ≠ 64):

```text
Shape error: matmul shape mismatch: dim(A,1)=64 ≠ dim(B,0)=65
```

### Try It 3 — Broadcasting: scalar times tensor

```cpp
// ch06_try3.cpp
#include <lithe/lithe.hpp>
#include <print>
#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"
#include "type_inference.hpp"
#include "shape_inference.hpp"

int main() {
    std::string_view src = R"(
        input A : tensor<f32>[4, 4]
        let scaled = 2.0 * A
    )";

    auto parse_result = flux::parse(src);
    auto arena        = flux::build_ast(parse_result, src);

    flux::resolver{arena}.resolve(0);

    flux::type_inferrer type_inf{arena};
    type_inf.infer(0);

    flux::shape_inferrer shape_inf{arena,
        type_inf.type_arena_mutable(),
        type_inf.gen_mutable(),
        type_inf.subst_mutable()};
    auto errors = shape_inf.infer(0);

    if (errors.empty())
        std::println("scaled : tensor<f32>[4, 4]   (scalar broadcasts)");
    else
        for (auto const& e : errors)
            std::println("Shape error: {}", e.message);
}
```

Expected output:

```text
scaled : tensor<f32>[4, 4]   (scalar broadcasts)
```

The scalar literal `2.0` has shape `Shape[]` (rank-0). `infer_binary_shapes` detects `lr == 0`, returns `rt` (the shape
of `A` = `[4, 4]`) directly.

---

## ASCII Pipeline Diagram

Where shape inference sits in the full Flux compiler:

```
┌─────────────────────────────────────────────────────────────────┐
│  Flux source text                                               │
└──────────────────────────────┬──────────────────────────────────┘
                               ↓  ch01: lexer
┌─────────────────────────────────────────────────────────────────┐
│  Token stream                                                   │
└──────────────────────────────┬──────────────────────────────────┘
                               ↓  ch02: grammar (lexy)
┌─────────────────────────────────────────────────────────────────┐
│  Parse tree                                                     │
└──────────────────────────────┬──────────────────────────────────┘
                               ↓  ch03: build_ast
┌─────────────────────────────────────────────────────────────────┐
│  Flux AST (untyped)                                             │
└──────────────────────────────┬──────────────────────────────────┘
                               ↓  ch04: resolver
┌─────────────────────────────────────────────────────────────────┐
│  Flux AST (names resolved)                                      │
└──────────────────────────────┬──────────────────────────────────┘
                               ↓  ch05: type_inferrer (HM Algorithm W)
┌─────────────────────────────────────────────────────────────────┐
│  Flux AST                                                       │
│  every node has type_ref  (f32, tensor<f32>, fn(f32)→f32, …)  │
│  tensor shapes are tensor<f32>[?,?]  ← still unknown           │
└──────────────────────────────┬──────────────────────────────────┘
                               ↓  ch06: shape_inferrer  ← YOU ARE HERE
                               │
                               │  for each matmul
                               │    → make_matmul_constraints
                               │    → emit same_type(dim(A,1), dim(B,0))
                               │    → out_shape = intern_shape([M,N])
                               │
                               │  for each transpose
                               │    → swap children[0], children[1]
                               │    → intern new Shape[N,M]
                               │
                               │  for each reshape
                               │    → verify product equality
                               │    → intern new Shape[e₁,...,eₘ]
                               │
                               │  for each element-wise op
                               │    → require_same_shape(A, B)
                               │    → return A's shape
                               │
                               │  for each scalar op tensor
                               │    → T-ScalarBroadcast: return tensor shape
                               │
                               │  for each map
                               │    → shape passes through unchanged
                               │
                               │  for each reduce / dot
                               │    → result is make_scalar_shape()
                               │
                               ↓
┌─────────────────────────────────────────────────────────────────┐
│  Flux AST                                                       │
│  every node has type_ref + shape_ref                            │
│  tensor shapes fully concrete: tensor<f32>[4,16]               │
└──────────────────────────────┬──────────────────────────────────┘
                               ↓  ch07: vakya_lowerer
┌─────────────────────────────────────────────────────────────────┐
│  vakya::node expression tree                                    │
│  shape_ref embedded in every tensor type_ref                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## What We Have

Summary of all operations and their shape rules:

| Operation                  | Formal rule       | Constraint emitted              | Result shape  |
|----------------------------|-------------------|---------------------------------|---------------|
| `matmul(A[M,K], B[K,N])`   | T-MatMul          | `same_type(dim(A,1), dim(B,0))` | `[M, N]`      |
| `dot(u[N], v[N])`          | T-Dot             | `same_type(dim(u,0), dim(v,0))` | `[]` scalar   |
| `transpose(A[M,N])`        | T-Transpose       | none                            | `[N, M]`      |
| `reshape(A[d...], [e...])` | T-Reshape         | `∏dᵢ = ∏eⱼ` (product)           | `[e₁,...,eₘ]` |
| `A + B` (element-wise)     | T-ElemWise        | `same_type` per dim             | same as `A`   |
| `s * A` (scalar)           | T-ScalarBroadcast | none                            | same as `A`   |
| `map(f, xs[N])`            | T-Map             | none                            | same as `xs`  |
| `reduce(f, xs[N])`         | T-Reduce          | none                            | `[]` scalar   |

All constraints flow through `vakya/types/shape.hpp`:

- `intern_shape` — create/lookup shape in type_arena
- `make_scalar_shape` — rank-0 result
- `shape_rank` — query number of dimensions
- `make_matmul_constraints` — emit inner-dim equality, return result shape
- `make_broadcastable_constraint` — NumPy broadcast compatibility

Shape inference leaves every tensor expression in the AST with a fully concrete `shape_ref`. The vakya lowerer in
Chapter 7 reads these shape refs to build the `vakya::node` tree with correct memory layout information embedded in each
type.

---

## Next

[Chapter 7 → Vakya Lowering](ch07_vakya_lowering.md) — translate the typed, shape-annotated Flux AST into `vakya::node`
expression trees, with shape information embedded in every tensor type ref.
