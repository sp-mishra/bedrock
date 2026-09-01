# Chapter 12 — FFI: Calling C++ from Flux

## What Is FFI and Why Does Flux Need It?

Flux ships with a rich standard library of builtins: `sqrt`, `exp`, `log`, `sin`, `matmul`, `map`,
`filter`, `reduce`, and more. These are hard-coded in the lowerer (Chapter 7) — each maps to a specific Lithe tag at
compile time.

But real programs need more:

- **Custom BLAS routines** — `cblas_sgemm` with explicit row/column strides, or vendor-tuned MKL kernels
- **Domain-specific kernels** — custom activation functions (`swish`, `gelu`), specialized signal processing filters
- **System functions** — high-resolution clocks, secure random sources, hardware performance counters
- **Library integration** — Eigen reductions, OpenCV morphological operators, libm extensions that are not in the Flux
  standard library

Without FFI, every new function requires modifying the Flux compiler itself: add a case to
`lower_call`, add a tag header, add a type scheme to `install_builtin_types`. That is a compiler contribution, not an
application concern.

**FFI lets users register C++ functions as Flux builtins from outside the compiler.** The compiler sees them exactly
like built-ins: resolved in ch04, typed in ch05, lowered in ch07, analyzed in ch05b. The only difference is that the
function pointer and its metadata came from user code, not from the compiler source tree.

### The goal

```flux
-- Declare C++ bindings (Level 1 — simple extern)
extern fn relu(x : f32) -> f32
extern fn clamp(x : f32, lo : f32, hi : f32) -> f32

-- Declare a tag-based binding (Level 2 — full vakya integration)
extern tag cblas_sgemm

let activations = relu(raw_logits)
let C = cblas_sgemm(A, B)
```

The remainder of this chapter explains exactly how both levels work, end-to-end.

---

## Design: Two Levels of FFI

The two levels differ in how deeply the registered function participates in the Vakya pipeline.

### Level 1 — Simple `extern fn`: direct C++ function call

The function is registered by name, type scheme, and raw pointer. The lowerer (ch07) emits an
`ffi_call_tag` node carrying the pointer. The CPU backend calls through it at runtime.

Level 1 is sufficient for:

- Pure math functions with known monomorphic signatures
- Functions where no rewrite rules or e-graph reasoning is needed
- Quick integrations where compile-time optimization is not the goal

The full ch07 → analysis → backend pipeline still runs; it just treats `ffi_call_tag` as an opaque call with the
declared type and effect bits.

### Level 2 — Tag-based FFI: full vakya integration

Register a C++ function as a first-class vakya `Tag`. The function then participates in:

| Pipeline stage       | What it gains                                    |
|----------------------|--------------------------------------------------|
| ch04 name resolution | Looked up by name like any builtin               |
| ch05 type inference  | Has a full polymorphic type scheme               |
| ch05b analysis       | Carries effect bits and capability bits          |
| ch07 lowering        | Lowers to its own dedicated tag node             |
| ch08 rewrites        | Can appear in pattern-matching rules             |
| e-graph optimization | Has a cost model entry; can be inlined/reordered |
| ch06 shape inference | Can carry custom shape constraint rules          |

Level 2 is the right choice for performance-critical kernels that the optimizer should see and reason about.

---

## Level 1: `extern fn` Declarations

### Syntax

```flux
-- Declare a C++ function binding
extern fn relu(x : f32) -> f32
extern fn clamp(x : f32, lo : f32, hi : f32) -> f32
extern fn dot_product(u : tensor<f32>, v : tensor<f32>) -> f32
extern fn scale_inplace(t : tensor<f32>, s : f32) -> tensor<f32>
```

The `extern fn` declaration:

1. Adds the name to the symbol table during ch04 name resolution
2. Installs the declared type scheme into the type environment for ch05 HM inference
3. Registers an `ffi_call_tag` node at the ch07 lowering stage

No `fn` body is written in Flux. The implementation lives entirely in C++.

### Lowering `extern fn` calls

At the ch07 lowering stage, `extern fn` calls lower to `lithe::ffi_call_tag`. The lowerer carries an `ffi_registry`
reference alongside the usual `ast_arena`:

```cpp
// The ffi_call_tag — a vakya tag for all Level-1 FFI calls
struct ffi_call_tag {};

template<> struct vakya::emit::tag_descriptor<ffi_call_tag> {
    static constexpr std::string_view symbol    = "ffi_call";
    static constexpr uint32_t         stable_id = 2001;
    static constexpr uint32_t         arity     = 0;   // variadic via children
};

// During lowering (ch07), an extern fn call becomes an ffi_call_tag node
vakya_expr vakya_lowerer::lower_extern_call(
    call_expr_node const& node,
    ffi_registry const& ffi)
{
    auto const* binding = ffi.lookup(node.callee);
    if (!binding)
        throw std::logic_error("Unknown extern fn in lowering: " + node.callee);

    std::vector<vakya_expr> args;
    args.reserve(node.args.size());
    for (auto idx : node.args)
        args.push_back(lower_expr(idx));

    // Build an ffi_call node carrying the function pointer and name
    return dag_.build(lithe::make_ffi_node(binding->fn_ptr, binding->name, args));
}
```

The updated `lower_call` checks the `ffi_registry` after exhausting the builtin dispatch table:

```cpp
vakya_expr vakya_lowerer::lower_call(call_expr_node const& node) {
    std::vector<vakya_expr> args;
    args.reserve(node.args.size());
    for (auto idx : node.args)
        args.push_back(lower_expr(idx));

    // Built-in dispatch (unchanged from ch07)
    if (node.callee == "sqrt")   return dag_.build(lithe::make_node<lithe::sqrt_tag>(args[0]));
    if (node.callee == "matmul") return dag_.build(lithe::make_node<lithe::matmul_tag>(args[0], args[1]));
    // ... (full table in ch07) ...

    // User-defined function (lambda/apply path)
    if (auto it = env_.find(node.callee); it != env_.end())
        return dag_.build(lithe::make_node<lithe::apply_tag>(it->second, args));

    // FFI fallback — look up in the registry
    if (ffi_) {
        if (auto const* binding = ffi_->lookup(node.callee))
            return dag_.build(lithe::make_ffi_node(binding->fn_ptr, binding->name, args));
    }

    throw std::logic_error("Undefined in lowering: " + node.callee);
}
```

The `ffi_` pointer is injected at construction time:

```cpp
// include/languages/flux/lower_vakya.hpp (updated constructor)
explicit vakya_lowerer(ast_arena const& arena,
                       ffi_registry const* ffi = nullptr)
    : arena_(arena), ffi_(ffi) {}

private:
    ast_arena const&     arena_;
    ffi_registry const*  ffi_;   // nullable — no FFI if null
    lithe::dag_builder   dag_;
    std::unordered_map<std::string, vakya_expr> env_;
```

### The FFI Registry

The registry is the central data structure for Level-1 FFI. It stores bindings by name and also integrates with the type
environment and the resolver.

```cpp
// include/languages/flux/ffi.hpp
#pragma once
#include <vakya/vakya_types.hpp>
#include <string>
#include <unordered_map>

namespace flux {

// Bitmask constants for effects (stable_id matches Vakya built-ins)
struct effect_bits {
    static constexpr uint64_t Pure       = 0;
    static constexpr uint64_t IO         = (1ull << 2);  // stable_id 3 in builtin registry
    static constexpr uint64_t FileSystem = (1ull << 0);  // stable_id 1
    static constexpr uint64_t Memory     = (1ull << 1);  // stable_id 2
    static constexpr uint64_t Network    = (1ull << 3);  // stable_id 4
    static constexpr uint64_t Exception  = (1ull << 4);  // stable_id 5
};

// Capability constants for backend routing
struct cap_bits {
    static constexpr uint64_t CPU      = 0;          // default — no special capability
    static constexpr uint64_t GPU      = (1ull << 0); // stable_id 1001 mapped to bit 0
    static constexpr uint64_t SIMD     = (1ull << 1); // stable_id 1002 mapped to bit 1
    static constexpr uint64_t CPUBlas  = (1ull << 2); // stable_id 1005 — requires BLAS
    static constexpr uint64_t F64      = (1ull << 3); // stable_id 1003 — f64 capable
};

// Type-erased C++ function binding
struct ffi_binding {
    std::string                name;
    void*                      fn_ptr;           // raw function pointer (C ABI)
    vakya::types::type_ref     type_scheme;      // from type_arena; may be quantified
    uint64_t                   effect_bits;      // 0 = pure
    uint64_t                   capability_bits;  // 0 = CPU-only
};

class ffi_registry {
public:
    // Register a C++ function as a Flux extern fn.
    // fn_ptr must be a plain function (not member, not capturing lambda).
    // scheme must be a type_ref already interned into the shared type_arena.
    template<typename Fn>
    void register_fn(std::string         name,
                     Fn*                 fn_ptr,
                     vakya::types::type_ref scheme,
                     uint64_t            effects = effect_bits::Pure,
                     uint64_t            caps    = cap_bits::CPU)
    {
        bindings_.emplace(name, ffi_binding{
            .name            = name,
            .fn_ptr          = reinterpret_cast<void*>(fn_ptr),
            .type_scheme     = scheme,
            .effect_bits     = effects,
            .capability_bits = caps,
        });
    }

    // Look up by name — returns nullptr if not registered
    ffi_binding const* lookup(std::string const& name) const {
        auto it = bindings_.find(name);
        return it != bindings_.end() ? &it->second : nullptr;
    }

    // Inject all registered names into the resolver's builtin scope (ch04)
    void install_into_resolver(flux::resolver& res) const {
        for (auto const& [name, binding] : bindings_)
            res.declare_builtin(name);
    }

    // Inject all registered type schemes into the type environment (ch05)
    void install_into_type_env(
        std::unordered_map<std::string, vakya::types::type_ref>& env) const
    {
        for (auto const& [name, binding] : bindings_)
            env.insert_or_assign(name, binding.type_scheme);
    }

    // Propagate effect/capability bits into the analysis_store (ch05b)
    void install_into_analysis(
        vakya::types::analysis_store& store,
        std::unordered_map<std::string, lithe::shared_expr> const& name_to_expr) const
    {
        for (auto const& [name, binding] : bindings_) {
            auto it = name_to_expr.find(name);
            if (it == name_to_expr.end()) continue;
            store.update_for(it->second, [&](auto& rec) {
                rec.effects.bits = binding.effect_bits;
                rec.caps.bits    = binding.capability_bits;
            });
        }
    }

    bool empty() const noexcept { return bindings_.empty(); }

private:
    std::unordered_map<std::string, ffi_binding> bindings_;
};

} // namespace flux
```

### How the compiler consumes the registry

The `flux::compiler` class (the public entry point) threads the registry through each pipeline stage:

```cpp
// Sketch of flux::compiler::compile() with FFI wiring
auto flux::compiler::compile(std::string_view src) -> compiled_program
{
    auto parse_result = flux::parse(src);
    auto arena        = flux::build_ast(parse_result, src);

    // ch04: install FFI names into the builtin scope before resolving
    flux::resolver resolver(arena);
    if (ffi_) ffi_->install_into_resolver(resolver);
    resolver.resolve(0);

    // ch05: install FFI type schemes before inferring
    flux::type_inferrer inferrer(arena);
    if (ffi_) ffi_->install_into_type_env(inferrer.type_env_mutable());
    auto errs = inferrer.infer(0);
    if (!errs.empty()) throw flux::type_error_list{std::move(errs)};

    // ch07: pass registry to lowerer
    flux::vakya_lowerer lowerer(arena, ffi_.get());
    auto vakya_tree = lowerer.lower(0);

    // ch05b: propagate effect/cap bits from registry into analysis_store
    vakya::types::analyze(vakya_tree, inferrer.type_env(),
                          solver_, inferrer.type_arena(),
                          inferrer.type_var_gen(), inferrer.subst(),
                          astore_, opts_);
    if (ffi_) ffi_->install_into_analysis(astore_, lowerer.name_to_expr());

    return compiled_program{std::move(vakya_tree), astore_};
}
```

---

## Level 2: Tag-Based FFI — Full Vakya Integration

Level 2 gives a C++ function its own dedicated vakya `Tag` with a `tag_descriptor`. This means the optimizer can match
it by tag type, apply rewrite rules against it, and assign it a cost model entry. From the optimizer's perspective, it
is indistinguishable from a built-in like
`lithe::matmul_tag`.

### Step 1 — Define your tag type

```cpp
// In your application headers — outside the compiler
struct cblas_sgemm_tag {};

template<> struct vakya::emit::tag_descriptor<cblas_sgemm_tag> {
    static constexpr std::string_view symbol        = "cblas_sgemm";
    static constexpr uint32_t         stable_id     = 3001; // >= 1000 for user-defined tags
    static constexpr uint32_t         arity         = 2;    // two tensor arguments
    static constexpr bool             is_commutative = false;
};
```

**Stable ID allocation**: IDs 1–999 are reserved for the Flux/Lithe compiler. User tags must use
`stable_id >= 1000`. Assign IDs consistently across program runs — they are part of the
`structural_hash` of any Vakya tree containing the tag, so changing them invalidates caches.

### Step 2 — Provide the CPU backend evaluation

```cpp
namespace lithe::backends::cpu {
    template<>
    auto eval<cblas_sgemm_tag>(
        flux_tensor_view const& A,
        flux_tensor_view const& B,
        flux_tensor_view&       C) -> void
    {
        int m = static_cast<int>(A.dims[0]);
        int k = static_cast<int>(A.dims[1]);
        int n = static_cast<int>(B.dims[1]);

        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    m, n, k,
                    /*alpha=*/1.0f,
                    static_cast<float const*>(A.data), k,
                    static_cast<float const*>(B.data), n,
                    /*beta=*/0.0f,
                    static_cast<float*>(C.data), n);
    }
} // namespace lithe::backends::cpu
```

### Step 3 — Register the type scheme

Add the type scheme in the same `install_builtin_types` style used in ch05, but from your own setup code (not inside the
compiler):

```cpp
// In your application setup, called before flux::compiler is constructed
void install_cblas_types(
    std::unordered_map<std::string, vakya::types::type_ref>& env,
    vakya::types::type_arena& tara,
    vakya::types::type_var_generator& gen)
{
    // cblas_sgemm : ∀T M K N. tensor<T>[M,K] → tensor<T>[K,N] → tensor<T>[M,N]
    auto T = tara.intern_variable(gen.fresh());
    auto M = tara.intern_dim_variable(gen.fresh_dim());
    auto K = tara.intern_dim_variable(gen.fresh_dim());
    auto N = tara.intern_dim_variable(gen.fresh_dim());

    auto A_t = vakya::types::intern_shape(tara, {M, K}, T);
    auto B_t = vakya::types::intern_shape(tara, {K, N}, T);
    auto C_t = vakya::types::intern_shape(tara, {M, N}, T);

    vakya::types::type_ref params[] = {A_t, B_t};
    auto body = tara.intern_callable(params, C_t);

    vakya::types::type_ref qs[] = {T, M, K, N};
    env["cblas_sgemm"] = tara.intern_quantified(qs, body);
}
```

The dimension variables `M`, `K`, `N` participate in ch06 shape inference: when `cblas_sgemm(A, B)`
is type-checked, the constraint `A.cols == B.rows` (i.e., `K == K`) is unified automatically because both use the same
dim variable.

### Step 4 — Register shape constraints

For the shape checker in ch06 to understand the matmul-compatible constraint on `cblas_sgemm`, register a rule against
the tag:

```cpp
void install_cblas_shape_constraints(
    vakya::types::constraint_registry& creg,
    vakya::types::type_arena& tara)
{
    // Reuse the built-in matmul constraint generator for cblas_sgemm
    creg.register_rule<cblas_sgemm_tag>(
        [&tara](auto& arena, vakya::types::type_ref A_ref,
                              vakya::types::type_ref B_ref,
                              vakya::types::solve_context& ctx)
        {
            // Emits: A.shape[1] == B.shape[0] and result.shape = [A.shape[0], B.shape[1]]
            return vakya::types::make_matmul_constraints(arena, A_ref, B_ref, ctx);
        });
}
```

### Step 5 — Wire into the lowerer for the tag path

For Level-2 tags, the lowerer uses a tag dispatch table rather than `ffi_call_tag`:

```cpp
// ffi_registry::register_tag — Level-2 variant
template<typename TagT>
void ffi_registry::register_tag(
    std::string name,
    uint64_t    effects = effect_bits::Pure,
    uint64_t    caps    = cap_bits::CPU)
{
    tag_dispatchers_.emplace(name,
        tag_dispatcher{
            .name    = name,
            .effects = effects,
            .caps    = caps,
            .lower_fn = [](lithe::dag_builder& dag,
                           std::vector<vakya_expr> args) -> vakya_expr
            {
                // Construct a statically-typed node using the user's tag
                return dag.build(lithe::make_node<TagT>(args));
            }
        });
}
```

Then in `lower_call`, after the FFI Level-1 check:

```cpp
// Level-2 tag dispatch
if (ffi_) {
    if (auto const* td = ffi_->lookup_tag(node.callee))
        return td->lower_fn(dag_, args);
}
```

### Flux usage of a Level-2 tag

```flux
extern tag cblas_sgemm   -- Level 2: the tag is registered in C++

input A : tensor<f32>[512, 512]
input B : tensor<f32>[512, 512]

let C = cblas_sgemm(A, B)
C.show_types()
```

```text
Expression: cblas_sgemm(A, B)
  Type:         tensor<f32>
  Shape:        [512, 512]
  Effects:      pure (none)
  Capabilities: cpu_blas
  Proofs:       shape-compatible (proven)
```

---

## Type Mapping: Flux ↔ C++ Types

When a Flux value is passed to a C++ FFI function, the backend marshals it according to the following table.

| Flux type          | C++ type                      | Notes                                   |
|--------------------|-------------------------------|-----------------------------------------|
| `f32`              | `float`                       | IEEE-754 single                         |
| `f64`              | `double`                      | IEEE-754 double                         |
| `i32`              | `int32_t`                     |                                         |
| `i64`              | `int64_t`                     |                                         |
| `u32`              | `uint32_t`                    |                                         |
| `u64`              | `uint64_t`                    |                                         |
| `bool`             | `bool`                        |                                         |
| `string`           | `std::string_view`            | read-only, no ownership transfer        |
| `tensor<f32>[M,N]` | `flux_tensor_view`            | row-major, see below                    |
| `tensor<f64>[N]`   | `flux_tensor_view`            | 1D, `ndim=1`                            |
| `fn(f32) -> f32`   | `std::function<float(float)>` | higher-order                            |
| `vec<T>`           | `flux_tensor_view`            | `ndim=1`, element type from `elem_size` |
| `tuple<A, B>`      | `std::pair<A, B>`             | layout by member type                   |

### Marshalling tensors: `flux_tensor_view`

Tensors passed across the FFI boundary are represented as a lightweight view struct with no ownership. The tensor buffer
is managed by the Flux runtime; the C++ function receives a view and must not outlive the call.

```cpp
// include/languages/flux/ffi.hpp (continued)

// Read-write view of a Flux tensor buffer.
// Layout: row-major (C-order). strides[i] is in element units, not bytes.
struct flux_tensor_view {
    void*    data;          // pointer to element buffer
    uint32_t ndim;          // number of dimensions (0 for scalar)
    size_t   dims[8];       // dimension extents; dims[ndim..] are 1
    size_t   strides[8];    // strides in elements; strides[ndim..] are 1
    uint32_t elem_size;     // sizeof one element in bytes (4 for f32, 8 for f64)
    bool     is_const;      // true → caller must not write through data
};

// Convenience accessors
inline float* tensor_f32(flux_tensor_view& v) noexcept {
    return static_cast<float*>(v.data);
}
inline float const* tensor_f32c(flux_tensor_view const& v) noexcept {
    return static_cast<float const*>(v.data);
}
inline size_t tensor_numel(flux_tensor_view const& v) noexcept {
    size_t n = 1;
    for (uint32_t i = 0; i < v.ndim; ++i) n *= v.dims[i];
    return n;
}
```

### How the backend calls a Level-1 FFI function

The CPU backend codegen for `ffi_call_tag` nodes:

```cpp
// CPU backend codegen — emit_ffi_call
void emit_ffi_call(ffi_binding const&                     binding,
                   std::span<flux_tensor_view const>      args,
                   flux_tensor_view&                      result)
{
    // Standard calling convention: fn(args_ptr, nargs, result_ptr)
    using ffi_fn_t = void(*)(flux_tensor_view const*, size_t, flux_tensor_view*);
    auto fn = reinterpret_cast<ffi_fn_t>(binding.fn_ptr);
    fn(args.data(), args.size(), &result);
}
```

C++ functions registered via `register_fn` must therefore have the signature:

```cpp
void my_fn(flux_tensor_view const* args, size_t nargs, flux_tensor_view* result);
```

For scalar functions (`f32 -> f32`), the backend uses a thinner wrapper that unpacks the scalar directly:

```cpp
using scalar_f32_fn_t = float(*)(float);
auto fn = reinterpret_cast<scalar_f32_fn_t>(binding.fn_ptr);
float arg = *static_cast<float const*>(args[0].data);
*static_cast<float*>(result->data) = fn(arg);
```

The `register_fn` template uses the function pointer type to select the right call wrapper:

```cpp
template<typename Fn>
void ffi_registry::register_fn(std::string name, Fn* fn_ptr,
                                vakya::types::type_ref scheme,
                                uint64_t effects, uint64_t caps)
{
    using fn_traits = lithe::function_traits<Fn>;
    call_convention cc = classify_convention<fn_traits>();  // scalar vs tensor
    bindings_.emplace(name, ffi_binding{
        .name            = name,
        .fn_ptr          = reinterpret_cast<void*>(fn_ptr),
        .type_scheme     = scheme,
        .effect_bits     = effects,
        .capability_bits = caps,
        .convention      = cc,
    });
}
```

---

## Worked Example: Registering `relu` (Level 1)

Complete end-to-end example showing every step from C++ registration to Flux execution.

### C++ side — implementation and registration

```cpp
// relu_impl.cpp
#include <languages/flux/ffi.hpp>
#include <vakya/vakya_types.hpp>
#include <cmath>

// The actual C++ implementation — plain function, no captures
float relu_impl(float x) noexcept { return x > 0.0f ? x : 0.0f; }

// Called once at startup to install into the ffi_registry
void register_flux_extensions(flux::ffi_registry&              reg,
                               vakya::types::type_arena&        tara,
                               vakya::types::type_var_generator& gen)
{
    // relu : f32 → f32
    auto f32_ref    = tara.intern_primitive("f32");
    vakya::types::type_ref p[] = {f32_ref};
    auto relu_type  = tara.intern_callable(p, f32_ref);

    reg.register_fn(
        "relu",
        relu_impl,
        relu_type,
        /*effects=*/flux::effect_bits::Pure,
        /*caps=*/flux::cap_bits::CPU);
}
```

### Flux source

```flux
-- flux_with_relu.flux
extern fn relu(x : f32) -> f32

input x : f32
let activated = relu(x)
activated.show_types()
activated.run(cpu)
```

```text
Expression: relu(x)
  Type:    f32
  Effects: pure (none)
  Caps:    cpu
```

### C++ driver

```cpp
// main.cpp
#include <languages/flux/compiler.hpp>
#include <print>
#include "relu_impl.cpp"

int main() {
    vakya::types::type_arena        tara;
    vakya::types::type_var_generator gen;
    flux::ffi_registry               ffi;

    register_flux_extensions(ffi, tara, gen);

    // Construct compiler with the registry
    flux::compiler compiler{ffi};

    auto prog = compiler.compile(R"(
        extern fn relu(x : f32) -> f32

        input x : f32
        let activated = relu(x)
    )");

    std::println("relu(-3.5) = {}", prog.run<float>({{"x", -3.5f}}));
    std::println("relu( 0.0) = {}", prog.run<float>({{"x",  0.0f}}));
    std::println("relu( 2.1) = {}", prog.run<float>({{"x",  2.1f}}));
}
```

Expected output:

```text
relu(-3.5) = 0
relu( 0.0) = 0
relu( 2.1) = 2.1
```

---

## Worked Example: Registering `cblas_sgemm` (Level 2)

Full Level-2 path: custom tag, type scheme with dimension variables, shape constraints, and CPU backend evaluation using
BLAS.

### Tag definition

```cpp
// cblas_sgemm_tag.hpp
#pragma once
#include <vakya/vakya.hpp>

struct cblas_sgemm_tag {};

template<> struct vakya::emit::tag_descriptor<cblas_sgemm_tag> {
    static constexpr std::string_view symbol        = "cblas_sgemm";
    static constexpr uint32_t         stable_id     = 3001;
    static constexpr uint32_t         arity         = 2;
    static constexpr bool             is_commutative = false;
};
```

### CPU backend specialization

```cpp
// cblas_sgemm_cpu.cpp
#include "cblas_sgemm_tag.hpp"
#include <languages/flux/ffi.hpp>   // flux_tensor_view
#include <cblas.h>

namespace lithe::backends::cpu {
    template<>
    auto eval<cblas_sgemm_tag>(flux_tensor_view const& A,
                                flux_tensor_view const& B,
                                flux_tensor_view&       C) -> void
    {
        int m = static_cast<int>(A.dims[0]);
        int k = static_cast<int>(A.dims[1]);
        int n = static_cast<int>(B.dims[1]);

        cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    m, n, k,
                    1.0f,
                    tensor_f32c(A), k,
                    tensor_f32c(B), n,
                    0.0f,
                    tensor_f32(C), n);
    }
} // namespace lithe::backends::cpu
```

### Registration

```cpp
// setup.cpp
#include "cblas_sgemm_tag.hpp"
#include <languages/flux/ffi.hpp>
#include <vakya/vakya_types.hpp>

void install_cblas(flux::ffi_registry&              reg,
                   vakya::types::type_arena&        tara,
                   vakya::types::type_var_generator& gen,
                   vakya::types::constraint_registry& creg)
{
    // Type scheme: ∀T M K N. tensor<T>[M,K] → tensor<T>[K,N] → tensor<T>[M,N]
    auto T = tara.intern_variable(gen.fresh());
    auto M = tara.intern_dim_variable(gen.fresh_dim());
    auto K = tara.intern_dim_variable(gen.fresh_dim());
    auto N = tara.intern_dim_variable(gen.fresh_dim());

    auto A_t = vakya::types::intern_shape(tara, {M, K}, T);
    auto B_t = vakya::types::intern_shape(tara, {K, N}, T);
    auto C_t = vakya::types::intern_shape(tara, {M, N}, T);

    vakya::types::type_ref params[] = {A_t, B_t};
    auto body = tara.intern_callable(params, C_t);
    vakya::types::type_ref qs[] = {T, M, K, N};
    auto scheme = tara.intern_quantified(qs, body);

    // Register as Level-2 tag
    reg.register_tag<cblas_sgemm_tag>(
        "cblas_sgemm",
        scheme,
        flux::effect_bits::Pure,
        flux::cap_bits::CPUBlas);

    // Shape constraints for ch06
    creg.register_rule<cblas_sgemm_tag>(
        [&tara](auto& arena, vakya::types::type_ref A_ref,
                              vakya::types::type_ref B_ref,
                              vakya::types::solve_context& ctx)
        {
            return vakya::types::make_matmul_constraints(arena, A_ref, B_ref, ctx);
        });
}
```

### Flux usage

```flux
extern tag cblas_sgemm

input A : tensor<f32>[512, 512]
input B : tensor<f32>[512, 512]

let C = cblas_sgemm(A, B)
C.show_types()
-- Type:         tensor<f32>
-- Shape:        [512, 512]
-- Effects:      pure (none)
-- Capabilities: cpu_blas
-- Proofs:       shape-compatible (proven)

C.run(cpu)
```

---

## Inline C++ Blocks (Advanced)

For truly one-off operations where no reuse is expected, Flux provides the `cpp { ... }` escape hatch: a block of
arbitrary C++ source that the CPU backend emits verbatim.

```flux
-- Inline C++ block: arbitrary C++ executed by the CPU backend
input x : f32
let result : f32 = cpp {
    float xv = inputs.get_f32("x");
    return std::sin(xv) + std::cos(xv);
}
```

The `cpp { ... }` block compiles to a `cpp_block_tag` node. The source text is carried as a payload string literal in
the Vakya DAG.

```cpp
// cpp_block_tag — tag for inline C++ blocks
struct cpp_block_tag {};

template<> struct vakya::emit::tag_descriptor<cpp_block_tag> {
    static constexpr std::string_view symbol    = "cpp_block";
    static constexpr uint32_t         stable_id = 2999;
    static constexpr uint32_t         arity     = 0;  // captures env via source string
};

// The lowerer creates a cpp_block node with the source as payload
struct cpp_block_payload {
    std::string_view source;   // the literal C++ text from the flux source
};

vakya_expr vakya_lowerer::lower_cpp_block(cpp_block_node const& node) {
    return dag_.build(lithe::make_cpp_block(node.source_text));
}
```

### Constraints on `cpp_block`

Because the compiler cannot analyze arbitrary C++, `cpp_block` is subject to conservative restrictions:

| Property         | Value                    | Reason                                     |
|------------------|--------------------------|--------------------------------------------|
| Effect bits      | `IO` (conservatively)    | May call arbitrary system functions        |
| Capability       | CPU-only                 | GPU/SIMD backends cannot emit raw C++      |
| Shape inference  | Must annotate explicitly | No shape information is derivable          |
| Pattern matching | Opaque                   | Cannot appear on the LHS of a rewrite rule |
| E-graph cost     | Maximum                  | Optimizer will not move or duplicate it    |

```flux
-- Shape annotation required for cpp_block returning a tensor
let result : tensor<f32>[4, 4] = cpp {
    float buf[16];
    for (int i = 0; i < 16; ++i) buf[i] = static_cast<float>(i);
    return buf;
}
```

If no type annotation is given, the type inferrer assigns a fresh type variable and requires the user to resolve the
ambiguity.

### When to use `cpp_block` vs `extern fn`

| Situation                                      | Use                                |
|------------------------------------------------|------------------------------------|
| One-off expression, no reuse                   | `cpp_block`                        |
| Reusable function, pure math                   | `extern fn` + `register_fn`        |
| Performance kernel, needs optimizer visibility | Level-2 `register_tag`             |
| Complex expression with multiple outputs       | `cpp_block` with struct return     |
| Gradients / autodiff through the function      | Level-2 only (optimizer needs tag) |

---

## Safety and Effect Tracking

The effect system (described in ch05b) propagates through FFI calls just like built-ins. The key rule: **declare the
effects your C++ function actually has**.

```cpp
// Pure math — no effects
reg.register_fn("relu",    relu_impl,    relu_type, flux::effect_bits::Pure);
reg.register_fn("sigmoid", sigmoid_impl, sig_type,  flux::effect_bits::Pure);
reg.register_fn("tanh_act", tanh_impl,  tanh_type,  flux::effect_bits::Pure);

// Prints to stdout — IO effect
reg.register_fn("log_tensor", log_fn, log_type,
    flux::effect_bits::IO);

// Writes to disk — FileSystem effect
reg.register_fn("save_tensor", save_fn, save_type,
    flux::effect_bits::FileSystem);

// Allocates on the heap — Memory effect
reg.register_fn("clone_tensor", clone_fn, clone_type,
    flux::effect_bits::Memory);
```

The effect checker in ch05b then propagates these bits upward through the Vakya tree. A `pure fn`
in Flux that calls `log_tensor` will fail the effect check:

```flux
-- Error: pure fn body has IO effect from log_tensor
pure fn bad_pure(x : f32) -> f32 {
    log_tensor(x)   -- declared with effect_bits::IO
    x * x
}
```

```text
Error: pure fn 'bad_pure' has undeclared effect: IO
  Caused by: call to extern fn log_tensor (effect_bits::IO)
```

### Capability mismatch

If a function is declared `caps = cap_bits::GPU` but the user requests CPU execution, the backend selector catches it:

```flux
extern fn gpu_kernel(t : tensor<f32>) -> tensor<f32>

input x : tensor<f32>[256]
let y = gpu_kernel(x)
y.run(cpu)   -- ERROR: gpu_kernel requires cap_bits::GPU; cpu backend cannot satisfy
```

```text
Error: Backend 'cpu' cannot satisfy capability 'GPU' required by extern fn gpu_kernel
  Hint: use y.run(gpu) or remove the gpu capability requirement
```

---

## ASCII Diagram: FFI Call Flow

Level-1 path (`extern fn relu`):

```
Flux source: "extern fn relu(x:f32)->f32  ...  relu(x)"
       |
       v  ch04 name resolution
       resolve "relu" -> found in ffi_registry, added to builtin scope OK
       |
       v  ch05 type inference
       relu : f32 -> f32  (from ffi_registry.type_scheme, installed into type_env)
       relu(x) : f32      (unify f32 with type(x)=f32, result f32)
       |
       v  ch07 vakya lowering
       ffi_call_tag node { fn_ptr=relu_impl, name="relu", convention=scalar, args=[x] }
       |
       v  ch05b analysis (analyze())
       analysis_record{ type=f32, effects=Pure, caps=CPU }
       |
       v  lithe backend selection
       CPU backend chosen (caps=CPU, run(cpu) requested)
       |
       v  CPU backend codegen
       scalar call thunk: fn = (float(*)(float))(binding.fn_ptr)
       result = fn(input_val)
       |
       v  execution
       relu_impl(-3.5f) -> 0.0f
```

Level-2 path (`extern tag cblas_sgemm`):

```
Flux source: "extern tag cblas_sgemm  ...  cblas_sgemm(A, B)"
       |
       v  ch04 name resolution
       resolve "cblas_sgemm" -> found in ffi_registry tag table OK
       |
       v  ch05 type inference
       cblas_sgemm : forall T M K N. tensor<T>[M,K] -> tensor<T>[K,N] -> tensor<T>[M,N]
       Instantiate: A:[512,512] B:[512,512] -> result:[512,512]
       |
       v  ch06 shape inference
       make_matmul_constraints: A.cols(512) == B.rows(512) -- PROVEN
       |
       v  ch07 vakya lowering
       cblas_sgemm_tag node { children=[A_expr, B_expr] }  (dedicated tag, not ffi_call_tag)
       |
       v  ch05b analysis
       analysis_record{ type=tensor<f32>, shape=[512,512], effects=Pure, caps=CPUBlas }
       |
       v  ch08 rewrites / e-graph
       cblas_sgemm_tag visible to optimizer; cost model entry used for fusion decisions
       |
       v  CPU backend
       eval<cblas_sgemm_tag>(A_view, B_view, C_view) -> cblas_sgemm(...)
```

---

## Complete Tutorial: Custom Activation Suite

A self-contained program that registers `sigmoid`, `relu`, and `tanh_act` as Flux externs, then runs a mini inference
pass showing all outputs.

```cpp
// ffi_tutorial.cpp — complete self-contained example
#include <lithe/lithe.hpp>
#include <languages/flux/compiler.hpp>
#include <languages/flux/ffi.hpp>
#include <vakya/vakya_types.hpp>
#include <print>
#include <cmath>
#include <array>

// C++ implementations — plain functions, no captures, no exceptions
static float relu_impl(float x)    noexcept { return std::max(0.0f, x); }
static float sigmoid_impl(float x) noexcept { return 1.0f / (1.0f + std::exp(-x)); }
static float tanh_impl(float x)    noexcept { return std::tanh(x); }

int main() {
    // Shared type arena and var generator
    vakya::types::type_arena        tara;
    vakya::types::type_var_generator gen;

    // Build the f32 -> f32 type scheme once; all three functions share it
    auto f32_ref    = tara.intern_primitive("f32");
    vakya::types::type_ref p[] = {f32_ref};
    auto f32_to_f32 = tara.intern_callable(p, f32_ref);

    // Register all three
    flux::ffi_registry ffi;
    ffi.register_fn("relu",     relu_impl,    f32_to_f32,
                    flux::effect_bits::Pure, flux::cap_bits::CPU);
    ffi.register_fn("sigmoid",  sigmoid_impl, f32_to_f32,
                    flux::effect_bits::Pure, flux::cap_bits::CPU);
    ffi.register_fn("tanh_act", tanh_impl,    f32_to_f32,
                    flux::effect_bits::Pure, flux::cap_bits::CPU);

    // Compile a Flux program that uses all three
    flux::compiler compiler{ffi};

    std::string_view src = R"(
        extern fn relu(x : f32) -> f32
        extern fn sigmoid(x : f32) -> f32
        extern fn tanh_act(x : f32) -> f32

        input raw : f32
        let r = relu(raw)
        let s = sigmoid(raw)
        let t = tanh_act(raw)
    )";

    auto prog = compiler.compile(src);

    // Run over a range of test inputs
    constexpr std::array inputs = {-2.0f, -0.5f, 0.0f, 0.5f, 2.0f};

    for (float v : inputs) {
        auto [r, s, t] = prog.run<float, float, float>(
            {{"raw", v}}, "r", "s", "t");
        std::println("x={:+.1f}  relu={:.3f}  sigmoid={:.3f}  tanh={:.3f}",
                     v, r, s, t);
    }
}
```

Expected output:

```text
x=-2.0  relu=0.000  sigmoid=0.119  tanh=-0.964
x=-0.5  relu=0.000  sigmoid=0.378  tanh=-0.462
x= 0.0  relu=0.000  sigmoid=0.500  tanh= 0.000
x=+0.5  relu=0.500  sigmoid=0.622  tanh= 0.462
x=+2.0  relu=2.000  sigmoid=0.881  tanh= 0.964
```

### Verifying type annotations

After compilation, the analysis store can be queried to confirm every subexpression was correctly typed:

```cpp
auto const& astore = prog.analysis_store();

// Check that relu(raw) is typed f32, pure, CPU
auto* rec = astore.find_by_name("r");
assert(rec != nullptr);
assert(type_name(rec->type, tara) == "f32");
assert(rec->effects.bits == flux::effect_bits::Pure);
assert(rec->caps.bits    == flux::cap_bits::CPU);
std::println("r: type={} effects={} caps={}",
             type_name(rec->type, tara),
             rec->effects.bits == 0 ? "pure" : "effectful",
             rec->caps.bits    == 0 ? "cpu"  : "other");
```

```text
r: type=f32 effects=pure caps=cpu
```

---

## Interaction with Rewrites (ch08)

Level-2 tags can appear in algebraic rewrite rules. This enables the optimizer to reason about user-defined operations.

### Example: relu (relu (x)) → relu (x) (idempotency)

```cpp
// Register an idempotency rule for relu (Level-2 tag)
auto relu_idem_rule = vakya::pattern::rule(
    "relu-idempotent",
    // Pattern: relu(relu(x))
    vakya::pattern::node<custom_relu_tag>(
        vakya::pattern::node<custom_relu_tag>(
            vakya::pattern::pv<0>())),
    // Rewrite: relu(x)
    vakya::pattern::node<custom_relu_tag>(
        vakya::pattern::pv<0>())
);

rewrite_engine.add_rule(relu_idem_rule);
```

This rule fires when the optimizer sees a double-relu, eliminating one call. Level-1 `ffi_call_tag`
nodes cannot participate in this kind of reasoning because the optimizer has no tag to match against — all Level-1 calls
look identical from the outside.

### Example: cblas_sgemm associativity

```cpp
// cblas_sgemm(cblas_sgemm(A,B), C) → cblas_sgemm(A, cblas_sgemm(B,C))
// Guarded: only when shapes are compatible (B is square)
auto sgemm_assoc = vakya::types::make_guarded(
    vakya::pattern::rule(
        "sgemm-assoc",
        sgemm(sgemm(pv<0>(), pv<1>()), pv<2>()),
        sgemm(pv<0>(), sgemm(pv<1>(), pv<2>()))),
    [&astore](auto const& env, auto const&) -> bool {
        return shapes_matmul_associative(astore, env.get<0>(), env.get<1>(), env.get<2>());
    }
);
```

This follows the same guarded-rule pattern as `matmul` associativity from ch05b, now applied to the user-supplied BLAS
tag.

---

## Error Messages

The compiler produces structured errors for FFI problems, consistent with the type-error format from ch05.

### Unknown extern fn

```flux
extern fn unknwon_fn(x : f32) -> f32
let y = unknwon_fn(1.0)
```

```text
Error: extern fn 'unknwon_fn' declared but not registered in ffi_registry
  at: line 1, col 1
  Hint: call ffi_registry::register_fn("unknwon_fn", ...) before constructing flux::compiler
```

### Type mismatch on extern fn call

```flux
extern fn relu(x : f32) -> f32
input flag : bool
let bad = relu(flag)
```

```text
Error: Type mismatch in call to extern fn relu
  Parameter 0: f32
  Argument:    bool
  Cannot unify f32 with bool
```

### Effect violation through FFI

```flux
extern fn save_tensor(t : tensor<f32>) -> tensor<f32>  -- FileSystem effect

pure fn process(t : tensor<f32>) -> tensor<f32> {
    save_tensor(t)   -- ERROR
}
```

```text
Error: pure fn 'process' has undeclared effect: FileSystem
  Caused by: call to extern fn save_tensor
  Declared with: effect_bits::FileSystem
```

### Backend capability not satisfied

```flux
extern fn gpu_only_kernel(t : tensor<f32>) -> tensor<f32>  -- cap: GPU

input x : tensor<f32>[64]
let y = gpu_only_kernel(x)
y.run(cpu)
```

```text
Error: Backend 'cpu' cannot satisfy capability requirement for extern fn gpu_only_kernel
  Required: cap_bits::GPU
  Provided: cap_bits::CPU
  Hint: route this computation to the GPU backend with y.run(gpu)
```

---

## What We Have

| Feature                | Mechanism                                            | Header                  |
|------------------------|------------------------------------------------------|-------------------------|
| Simple extern fn       | `ffi_registry::register_fn` + `ffi_call_tag`         | `flux/ffi.hpp`          |
| Tag-based FFI          | `ffi_registry::register_tag` + user `tag_descriptor` | `vakya/vakya.hpp`       |
| Type scheme            | `type_arena::intern_callable` / `intern_quantified`  | `vakya/vakya_types.hpp` |
| Shape constraints      | `constraint_registry::register_rule`                 | `vakya/vakya_types.hpp` |
| Effect declaration     | `effect_bits` in `register_fn` / `register_tag`      | `flux/ffi.hpp`          |
| Capability declaration | `cap_bits` in `register_fn` / `register_tag`         | `flux/ffi.hpp`          |
| Tensor marshalling     | `flux_tensor_view`                                   | `flux/ffi.hpp`          |
| Inline C++             | `cpp_block_tag` via `cpp { ... }` syntax             | compiler extension      |
| Rewrite integration    | Level-2 tags in `vakya::pattern::rule`               | `vakya/vakya.hpp`       |
| Analysis readback      | `analysis_store::find_by_name`                       | `vakya/vakya_types.hpp` |

---

## Next

[Chapter 13 → Debugging & REPL](ch_debug_repl.md)
