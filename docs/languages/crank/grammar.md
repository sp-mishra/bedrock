# Crank — Language Grammar

Companion to `design.md`. This file owns the **complete lexical + syntactic grammar** and its mapping onto **lexy**
productions. `design.md` owns everything else (architecture, pipeline, semantics, runtime, host embedding, AOT, library
gaps).

Crank is Go- *inspired* but not Go- *compatible*. It is deliberately minimal: no `int`/`uint`
aliases, no implicit numeric conversions, no `nil`, no pointer arithmetic, no `panic`/`recover`, no C-style `switch`
fallthrough. Fixed-width numeric types only, so SIMD/GPU/serialization/AOT layouts are predictable.

---

## 1. Notation

- EBNF: `=` defines, `|` alternation, `{ }` zero-or-more, `[ ]` optional, `( )` grouping,
  `" "` terminal literal, `UPPER` = lexical token, `lower` = grammar production.
- Grammar is defined over a **token stream** produced by the lexer (§3), not raw bytes. The lexy frontend fuses lexing +
  parsing (§8), but the two layers are described separately for clarity.
- Encoding: source is UTF-8. Identifiers are ASCII-only in v1 (see gap `G-LEX-1` in `design.md`).

---

## 2. Source Structure

```ebnf
source_file   = package_clause NEWLINE
                { import_decl NEWLINE }
                { top_decl NEWLINE } ;

package_clause = "package" IDENT ;
```

- Exactly one `package` clause, first non-comment token.
- A directory of `.crank` files sharing a `package` name forms one package (see `design.md`
  §Module System). `module.crank` names the package root.
- `import` declarations are source-level module dependencies; at the host embedding API level
  `engine::load("name")` resolves modules through the 9-tier `module_resolver` before evaluation (see `crank.md` §Module
  Facade). The two are complementary: `import` is language syntax, `load`
  is the host API for runtime-controlled resolution.

---

## 3. Lexical Structure

### 3.1 Whitespace & Newlines

```ebnf
NEWLINE   = "\n" | "\r\n" ;
WS        = " " | "\t" ;
```

- Crank is **newline-sensitive** at the statement/decl level (like Go): a `NEWLINE` terminates a statement unless the
  line ends in a state that clearly continues (open binary operator, open
  `(`/`[`/`{`, trailing `,`). Implemented via lexy `context_flag`-driven automatic semicolon insertion (ASI); see §8.4.
  There is **no** explicit `;` in idiomatic source (allowed but redundant).
- WS between tokens is insignificant.

### 3.2 Comments

```ebnf
line_comment  = "//" { any_char_except_newline } ;
block_comment = "/*" { any_char } "*/" ;   (* non-nesting *)
```

Comments are whitespace to the parser but retained as trivia on the lexy `parse_tree` for source maps / diagnostics.

### 3.3 Identifiers & Keywords

```ebnf
IDENT   = letter { letter | digit } ;
letter  = "A"…"Z" | "a"…"z" | "_" ;
digit   = "0"…"9" ;
```

**Keywords** (reserved, cannot be identifiers):

```
package  import   fn      let     var     const
pub      type     struct  enum    if      else
for      while    match   return  break   continue
in       as       true    false   Unit
await    spawn    defer   transaction
requires ensures  assert
trait    impl     view    extern
```

**Verification keywords** (`requires ensures assert`) form the *verification surface* — Tarka-backed language constructs
(see `design.md` §Verification-in-the-Language). They are always parsed; whether they are *discharged* depends on build
policy (default: runtime guard; opt-in: Tarka/Z3 static proof via `@verify(static)`). They carry **zero runtime cost
when proven** and never appear in
`@pure` hot paths unless the user writes them.

**Contextual-only identifiers** (not globally reserved; only special inside their enclosing production — safe to use as
variable names in normal code):

- `forall`, `exists` — only inside `pred_expr` (requires/ensures/assert predicates)
- `async` — not yet a keyword (no `async fn` production; reserved name space, may be activated later)

**`transaction`** is a reserved keyword introducing a transactional-memory block (§6.2). It lowers onto the **Medha**
substrate (see `design.md` §Transactional Memory via Medha); because it is a language keyword, Medha is a required crank
runtime component. Transaction *option words* used inside `transaction(...)` args (`isolation`, `retry`, `replay`,
`conflict`, `partial`,
`durability`, `distribution`, and their values `snapshot`/`serializable`/`memory`/`durable`/`optimistic`/… listed in
§6.2) are **contextual** — reserved only inside a `transaction` argument list, not global keywords.

**`view`** is a reserved keyword introducing a domain view declaration (`view_decl`, §5.5) or a view construction
expression (`view_expr`, §7.13). **Feature-gated** by the `domain_views` edition flag — without it, `view` is still
parsed but sema rejects it (`CRANK-VIEW-003`). The word **`of`**
that follows `view IDENT` in a `view_decl` is **contextual** — matched as a literal only inside the
`view_decl` production (dispatched via the reserved `view` peek), never entered into the global reserve set. User code
that names something `of` is therefore unaffected.

**Contextual words** (not reserved; only special in their enclosing construct, as noted):

```
parallel  simd  gpu  pure  reads  writes  io  net  host
required  preference  deterministic  on_safety_failure  overflow
normal  strong  return_result  trap  terminate  host_handler  checked
of        (* contextual inside view_decl only; §5.5 *)
range     (* contextual: only appears inside `..`/`..=` range expressions; not a statement keyword *)
forall    (* contextual: pred_expr quantifier only *)
exists    (* contextual: pred_expr quantifier only *)
async     (* reserved name-space placeholder; no grammar production; may be activated later *)
```

`trait` and `impl` are **reserved keywords** (generic-bound declarations + conformance, §5.2). The generic-model bound
names (`Numeric`, `Copy`, `ParallelSafe`, …) are **predeclared identifiers**
resolved by the trait registry (like the primitive types), **not** keywords.

### 3.4 Literals

```ebnf
INT_LIT    = dec_lit | hex_lit | oct_lit | bin_lit ;
dec_lit    = digit { digit | "_" } ;
hex_lit    = "0x" hex_digit { hex_digit | "_" } ;
oct_lit    = "0o" oct_digit { oct_digit | "_" } ;
bin_lit    = "0b" bin_digit { bin_digit | "_" } ;

FLOAT_LIT  = dec_lit "." dec_lit [ exponent ]
           | dec_lit exponent ;
exponent   = ( "e" | "E" ) [ "+" | "-" ] dec_lit ;

BOOL_LIT   = "true" | "false" ;

STRING_LIT     = '"' { str_char | escape } '"' ;
RAW_STRING_LIT = "`" { any_char_except_backtick } "`" ;
escape         = "\" ( "n" | "t" | "r" | "\" | '"' | "0" | "x" hex hex | "u{" hex+ "}" ) ;
```

- Numeric literals are **untyped constants** until context assigns a fixed-width type (§ literal typing rule in
  `design.md` semantics). No implicit narrowing at runtime — the constant must fit the target type at compile time or it
  is a diagnostic.
- `_` digit separators are stripped by the lexer.

### 3.5 Operators & Punctuation

```
+  -  *  /  %                arithmetic
== != <  <= >  >=            comparison
&& || !                      logical
&  |  ^  <<  >>  &^          bitwise (&^ = and-not)
=  :=  +=  -=  *=  /=  %=    assignment
&=  |=  ^=  <<=  >>=         bitwise-assign
->  =>  ..  ..=  .  ,  :     misc (-> return, => match arm, .. range excl, ..= incl)
?                            postfix error-propagation (Result/Option unwrap-or-return)
(  )  [  ]  {  }             brackets
@                            attribute sigil
```

`?` is an **operator**, not a keyword. It only appears in postfix position (after an expression); see §7.x for
semantics. It does not form part of any ternary or optional-type syntax — options are written `Option[T]`.

### 3.6 Attributes

```ebnf
attribute      = "@" attr_name [ "(" attr_args ")" ] ;
attr_name      = builtin_attr | qualified_ident ;      (* qualified = namespaced extension attr *)
builtin_attr   = "parallel" | "simd" | "gpu" | "pure" | "reads" | "writes"
               | "io" | "net" | "host" | "on_safety_failure" | "overflow"
               | "verify" ;   (* @verify(static) — mandate static-only discharge; replaces proof stmt *)
attr_args      = attr_arg { "," attr_arg } ;
attr_arg       = IDENT [ "=" ( INT_LIT | IDENT | BOOL_LIT | STRING_LIT ) ]
               | STRING_LIT ;                          (* positional string, e.g. @domain.finance("risk") *)
```

Attributes attach to the **following** declaration or statement. Semantics in `design.md`
(§Attributes & Effects, §4.6a, §5b). A `builtin_attr` is the closed, **unqualified** execution/effect set. A
**namespaced** `qualified_ident` attr (`@lithe.cacheline`, `@pravaha.device_resident`,
`@domain.finance`, `@tarka.assume`) is an extension resolved through the **typed annotation registry**
(design.md §5b): its identity is fully qualified, its arguments are schema-validated, and an unknown or ill-typed
annotation is preserved, handed to a host handler, or rejected per module policy. There is **no** global (unqualified)
extension-attribute namespace — every user annotation carries a `.`
namespace (`lithe.*`/`pravaha.*`/`medha.*`/`tarka.*`/`sutra.*`/`domain.*`/`company.*`/`user.*`).

**Execution-attribute arguments (shared model).** `@parallel`/`@simd`/`@gpu` share one small argument set that lowers
into a Lithe `execution_hint` (design.md §4.6a):

```crank
@gpu                          // soft: prefer GPU; fall back to SIMD/CPU
@gpu(required=false)          // same as @gpu
@gpu(preference=strong)       // strong bias toward GPU; lower profitability-confidence; fallback kept
@gpu(required=true)           // hard: compile error if no permitted/eligible GPU backend
@simd(preference=strong)      // strong SIMD bias
@parallel(deterministic=true) // parallel only if determinism is preserved (legality filter)
```

- `required : Bool = false` — `true` promotes an unmet preference to a compile-time diagnostic;
  `required` **dominates** `preference`.
- `preference : (normal | strong) = normal` — `strong` biases backend selection more heavily and allows a lower
  profitability threshold, but still preserves legality + safe fallback. `strong`
  replaces the earlier `aggressive` idea (no unsafe implication). `weak` is reserved for later.
- `deterministic : Bool = false` — on `@parallel`, keeps only plans that preserve deterministic semantics (drops
  reorder-unsafe reductions).

Any other argument name/value on a built-in execution attribute is a diagnostic (the set is closed). Standalone
`@sequential`/`@no_gpu`/`@deterministic` attributes are **not** provided — the argument model above expresses those
shades, and automatic analysis (design.md §4.6a) is the default anyway.

**Hard vs soft execution attributes.** `@parallel`, `@simd`, and `@gpu` are **preferences** by default — if the
target/backend/legality is unavailable, crank falls back (parallel → sequential, gpu → simd → cpu) and emits a NADI
pulse (Pravaha's "no silent degradation" invariant). To demand execution, pass `required=true`:

```crank
@parallel                 // soft: prefer Pravaha parallel; fall back to sequential if illegal
@parallel(required=true)  // hard: compile error if the loop cannot be proven parallel-safe
@gpu                      // soft: prefer GPU; fall back to SIMD/CPU if unavailable
@gpu(required=true)       // hard: compile error if no permitted/eligible GPU backend
@simd(required=true)      // hard: compile error if the region is not SIMD-vectorizable
```

`required=true` turns an unmet preference from a runtime fallback into a **compile-time diagnostic**
(design.md §Attributes & Effects). The bare `@must_parallel`/`@must_gpu` spellings are **not** added — the single
`required=<bool>` argument keeps the attribute set closed and uniform. `required`
defaults to `false`; any other value is a diagnostic.

---

## 4. Types

```ebnf
type        = generic_type
            | slice_type
            | array_type
            | tuple_type
            | func_type
            | result_type
            | option_type
            | refined_type ;

named_type   = qualified_ident ;   (* alias: generic_type with no brackets *)
qualified_ident = IDENT { "." IDENT } ;         (* module-qualified: math.Vec3 *)

slice_type   = "[" "]" type ;                   (* []Float32 *)
array_type   = "[" array_len "]" type ;         (* [4]Float32 fixed len; [N]Float32 const-generic len *)
array_len    = INT_LIT | qualified_ident ;      (* literal, or a const param resolving to usize/isize/enum (§5.3) *)
tuple_type   = "(" [ type { "," type } ] ")" ;  (* () = Unit *)
func_type    = "fn" "(" [ type { "," type } ] ")" [ "->" type ] ;

generic_type = qualified_ident [ "[" generic_arg { "," generic_arg } "]" ] ;  (* plain (T) or instantiated (Map[String, Int32]) *)
generic_arg  = type | INT_LIT | BOOL_LIT | qualified_ident ;  (* type or const-generic value (§5.3) *)
result_type  = "Result" "[" type "," type "]" ;
option_type  = "Option" "[" type "]" ;

(* refinement type: a base type carrying a predicate the compiler must prove.       *)
(* { name: BaseType | predicate }   e.g.  { n: Int32 | n > 0 }  (a positive Int32)  *)
refined_type = "{" IDENT ":" type "|" pred_expr "}" ;
```

**Primitive named types** (predeclared, not keywords — resolved by the type registry):

```
Int8 Int16 Int32 Int64      UInt8 UInt16 UInt32 UInt64
Float32 Float64             Bool  String  Unit
```

There is **no** bare `int`/`uint`, no `nil`, no raw pointer type in the surface language.

---

## 5. Declarations

```ebnf
top_decl   = attribute* [ "pub" ] ( func_decl | type_decl | const_decl | var_decl
                         | trait_decl | impl_decl | view_decl
                         | extern_fn_decl ) ;  (* §5.6 adds extern_fn_decl; pub forces export *)

func_decl  = "fn" IDENT [ generic_params ] "(" [ params ] ")" [ "->" type ]
             { contract_clause } block ;
generic_params = "[" generic_param { "," generic_param } "]" ;
generic_param  = type_param | const_param ;
type_param     = IDENT [ ":" bound { "+" bound } ] ;   (* T: Numeric + Copy + ParallelSafe *)
const_param    = IDENT ":" const_param_type ;          (* N: usize  (const generic, §5.3) *)
const_param_type = "usize" | "isize" | "Bool" | qualified_ident ;  (* enum-valued allowed *)
bound          = qualified_ident                       (* trait name: Numeric / GpuCompatible / TransactionalResource *)
               | callable_bound ;
callable_bound = callable_kind "(" [ type { "," type } ] ")" "->" type ;  (* F: Fn(T) -> U *)
callable_kind  = "Fn" ;   (* user-facing: Fn only. FnMut/FnOnce are inferred by sema from capture analysis. *)
params     = param { "," param } ;
param      = IDENT [ ":" type ] ;   (* type required except for bare `self` receiver *)

(* function contracts — Tarka-backed pre/postconditions on the signature.          *)
(*   requires <pred>   precondition, proven at each call site (or guarded)          *)
(*   ensures  <pred>   postcondition, proven at each return (`result` binds retval) *)
contract_clause = "requires" pred_expr
                | "ensures"  pred_expr ;

type_decl  = "type" IDENT [ generic_params ] "=" type_body ;
type_body  = struct_body | enum_body | type ;          (* alias, struct, or enum *)
struct_body = "struct" "{" { field NEWLINE } "}" ;
field      = IDENT ":" type ;
enum_body  = "enum" "{" { variant NEWLINE } "}" ;
variant    = IDENT [ "(" type { "," type } ")" ] ;

const_decl = "const" IDENT [ ":" type ] "=" expr ;

(* var requires an explicit type OR an initializer (or both); a bare `var x` is a    *)
(* syntax error. This removes uninitialized-variable ambiguity — every var has a     *)
(* known type and a known initial value at the point of declaration.                 *)
var_decl   = "var" IDENT ":" type "=" expr        (* typed + initialized            *)
           | "var" IDENT ":" type                 (* typed → zero-valued (§5.1)     *)
           | "var" IDENT "=" expr ;               (* inferred from initializer      *)
```

- **Public boundary rule**: `fn`/`type`/`const` names starting with an uppercase letter are exported from the package;
  lowercase = package-private (Go convention, kept). Exported `fn`
  signatures **require explicit parameter and return types** — no inference across the public API boundary. Local `let`/
  `var` may infer.

### 5.1 `var` initialization & zero values

`var x` (no type, no initializer) is rejected — a variable's type and initial value must both be determinable at its
declaration. The three legal forms:

| Form               | Type source        | Initial value                     |
|--------------------|--------------------|-----------------------------------|
| `var x: Int32 = 0` | explicit           | the initializer                   |
| `var x: Int32`     | explicit           | the type's **zero value** (below) |
| `var x = 0`        | inferred from expr | the initializer                   |

**Zero values** (Go-style, only used by the `var x: T` form — never implicit anywhere else):

| Type family                      | Zero value                                                         |
|----------------------------------|--------------------------------------------------------------------|
| `Int8…Int64`, `UInt8…UInt64`     | `0`                                                                |
| `Float32`, `Float64`             | `0.0`                                                              |
| `Bool`                           | `false`                                                            |
| `String`                         | `""` (empty)                                                       |
| `Unit`                           | `Unit`                                                             |
| `[]T` (slice)                    | empty slice (len 0)                                                |
| `[N]T` (array)                   | N × zero-value of T                                                |
| `struct{…}`                      | each field zeroed                                                  |
| `Option[T]`                      | `Option.None`                                                      |
| `Result[T,E]`, `enum`, `fn(...)` | **no zero value** — `var x: T` is a compile error; must initialize |

`Result`, bare `enum` (no designated default variant), and function types have no meaningful zero, so they force the
`var x: T = e` form. This keeps "zero value" total only where it is unambiguous.

### 5.2 Traits & implementations (generic bounds)

Bounds in `[T: Bound]` are **traits** — explicit semantic capabilities, declared and implemented with `trait`/`impl` (no
implicit structural conformance). Full model in `design.md` §17.

`Self` is a **contextual type identifier** available only inside `trait` and `impl` bodies, where it names the
implementing type. It is **not** a global keyword (not in §3 reserved words) and cannot be bound as a local/parameter
name in those contexts.

```ebnf
trait_decl  = "trait" IDENT [ generic_params ] "{" { trait_member NEWLINE } "}" ;
trait_member= assoc_type_decl
            | trait_fn_decl
            | assoc_const_decl ;
assoc_type_decl  = "type" IDENT ;                       (* associated type: `type Item` — v2-gated, see below *)
trait_fn_decl    = "fn" IDENT "(" [ params ] ")" [ "->" type ] ;  (* signature only; params/return may name Self *)
assoc_const_decl = IDENT ":" type [ "=" expr ] ;        (* associated const, opt default: `commutative: Bool = false` *)

impl_decl   = "impl" [ generic_params ] type [ "for" type ] "{" { impl_member NEWLINE } "}" ;
(* one unified form: with `for`, the first `type` is the trait and the second the      *)
(* implementing type (`impl Monoid for Int64`); without `for`, the single `type` is an  *)
(* inherent/specialized impl (`impl Vector[T] { … }`). The trait slot is parsed as a    *)
(* full `type` (so `Monoid`, `AddInto[A,B,C]` both fit); trait-vs-type validity is       *)
(* checked in analysis. *)
impl_member = func_decl | ( "type" IDENT "=" type ) | ( IDENT [ ":" type ] "=" expr ) ;

(* §5.5 Domain view declaration — feature-gated by `domain_views` (§11) *)
(* "of" is contextual here; never globally reserved.                     *)
view_decl   = "view" IDENT [ generic_params ] "of" IDENT ":" type
              { "requires" pred_expr } ;
(* IDENT after "of" is the backing binding name (e.g. "base"); referenced as self.base inside methods. *)
(* Only "requires" is accepted in v1; "ensures" / "where" on a view_decl → CRANK-VIEW-011 (reserved). *)
```

- **`Self`-first traits.** For single-type algebraic traits (`Monoid`, `Numeric`, `Ordered`,
  `Iterator`), `Self` names the implementing type — no trait type parameter is needed. Generic trait parameters
  (`trait AddInto[A, B, C]`) are reserved for genuinely **multi-type** relations. Prefer the `Self` style:

  ```crank
  trait Monoid {
      fn identity() -> Self
      fn combine(a: Self, b: Self) -> Self
      associative: Bool
      commutative: Bool = false
  }
  impl Monoid for Int64 {
      fn identity() -> Int64 { return 0 }
      fn combine(a: Int64, b: Int64) -> Int64 { return a + b }
      associative = true
      commutative = true
  }
  ```

- **Static-member lookup (trait members through a type parameter).** Inside a generic function where
  `T: Monoid`, static trait members may be referenced as `T.identity()` / `T.combine(a, b)`. This is **syntactic sugar
  for the selected `impl Monoid for T` witness**. If two of `T`'s bounds provide the same member name, the bare
  `T.member` reference is **ambiguous** and is a diagnostic — it must be qualified through the trait:
  `Monoid[T].identity()` / `Monoid[T].combine(acc, x)`. The qualified form `Trait[T].member(...)` is always legal and is
  the disambiguator (design.md §17.4a).
- Conformance is **explicit**: a type satisfies `T: Numeric` only if an `impl Numeric for T` exists.
- **Associated types are v2-gated.** `assoc_type_decl` (`type Item`) and its projection (`Self.Item`,
  `I.Item`) **parse** in v1 for forward compatibility but are **rejected** unless the compiler enables the v2
  `associated_types` feature (design.md §17.4/§17.12). v1 diagnoses their use; the syntax is reserved so v2 needs no
  grammar break.
- **Type specialization is v2-gated.** A *type-specialized* impl (`impl Vector[Float32] { … }`)
  **parses** in v1 but is **rejected** unless the compiler enables the v2 `specialization` feature — the same
  parse-but-gate treatment as associated types. Generic (parametric) impls (`impl[T: Numeric] Vector[T] { … }`) ship in
  v1. When enabled, a specialization must be unambiguous, non-overlapping, and cannot weaken safety/effect constraints
  (`design.md` §17.9). Higher-kinded traits and variance are out of scope for v1 (§10.2).
- **Coherence / orphan rule (v1, always enforced).** An `impl Trait for Type` is legal only if the defining module
  **owns the trait or owns the implementing type** — forbidding foreign-trait-for- foreign-type impls, so cross-package
  impls can never conflict (`design.md` §17.9).
- **Callable bounds.** A callable parameter is bounded by `Fn(ArgTypes) -> Ret`, stating arity + signature, combined
  with effect bounds (`F: Fn(T) -> U + Pure`). Bare `fn(T) -> U` remains the concrete function-pointer *type*; the `Fn`
  *bound* additionally admits closures while staying monomorphized (`design.md` §17.6). `FnMut`/`FnOnce` are inferred by
  sema — not user-writeable.

### 5.3 Const generics

A `const_param` (`N: usize`) is a **type-level constant** for array/vector/tile/matrix/tensor dimensions.

```ebnf
const_param      = IDENT ":" const_param_type ;
const_param_type = "usize" | "isize" | "Bool" | qualified_ident ;  (* qualified = enum-valued *)
```

v1 const-parameter kinds: `usize`, `isize`, `Bool`, enum values — **no** arbitrary compile-time expressions. A const
generic passed as a `generic_arg` is a **literal or a bare parameter reference only**: an `INT_LIT`/`BOOL_LIT`/enum
value, or a const-parameter name (§4 `generic_type`). Arithmetic on const generics (`[N + 1]`, `Matrix[T, M, K*2]`) is
**not valid in v1** and is a diagnostic; reusing the same parameter across positions (`M`, `K`, `N`) is a reference, not
an expression, and is fine. Restricted const expressions with overflow checking are **v2**
(`design.md` §17.5/§17.12).

- **`usize`/`isize` are const-generic parameter kinds only.** They are **not** runtime value types and cannot be used as
  ordinary variable/field/parameter types in v1 — that role stays with the fixed-width `Int8…UInt64` set (§4),
  preserving fixed-width determinism (no bare `int`/`uint`
  re-entering through a target-width type). Runtime lengths remain `Int64`/`UInt64`; `usize`/`isize`
  exist purely to write ergonomic dimension parameters like `[N: usize]`.

```crank
fn Dot[N: usize](a: [N]Float32, b: [N]Float32) -> Float32
fn MatMul[T: Numeric, M: usize, K: usize, N: usize](
    a: Matrix[T, M, K], b: Matrix[T, K, N]) -> Matrix[T, M, N]
```

Const-generic dimensions feed Vākya's shape algebra so shape compatibility (`cols(a) == rows(b)`) is provable at compile
time (`design.md` §7a.5 / §17.5), not deferred to runtime.

### 5.6 Extern function declarations (`@host.link`)

`extern fn` is a **body-less** function declaration that binds a crank name to a registered host function. The
`@host.link("name")` attribute specifies the qualified host symbol to bind. Analysis verifies the symbol is registered
in the context's host function table and that arity matches (§X in `crank.md`).

```ebnf
extern_fn_decl = "@host.link" "(" STRING_LIT ")" "extern" "fn" IDENT
                 "(" [ params ] ")" [ "->" type ] ;
```

- `@host.link("qualified.name")` must appear immediately before the `extern fn` — no other attributes between them
  (parsed as part of the extern declaration, not as a general attribute). The string is the **host-side** qualified name
  registered via `ctx.register_function<"name", fn>()`.
- The `fn` body is **absent** — the parser rejects a `{…}` block following an `extern fn`. Analysis emits
  `CRANK-EXT-010` if the host symbol is not registered, `CRANK-EXT-011` if the declared arity does not match the
  registered descriptor.
- Generics on `extern fn` are not supported in v1 — a `generic_params` clause is a diagnostic.
- Effect attributes (`@pure`, `@reads`, `@writes`, …) may precede `@host.link`; they are validated against the
  registered host descriptor's effect mask (§X). Effect escalation (declaring fewer effects than the host function
  provides) emits `CRANK-EXT-012`.

```crank
@host.link("math.dot")
extern fn Dot(a: Vec3, b: Vec3) -> Float32

@pure
@host.link("math.cross")
extern fn Cross(a: Vec3, b: Vec3) -> Vec3
```

lexy mapping: `dsl::keyword<"extern">` → peek `dsl::keyword<"fn">` → IDENT + params + optional return type (no block).
The `@host.link` attribute is parsed by the §3.6 attribute production with name `"host.link"` (a namespaced qualified
attr); the `extern_fn_decl` production validates that the immediately following token is `extern`.

---

## 6. Statements

```ebnf
block      = "{" { statement NEWLINE } "}" ;

statement  = let_stmt
           | var_stmt
           | assign_stmt
           | expr_stmt
           | if_stmt
           | for_stmt
           | while_stmt
           | match_stmt
           | return_stmt
           | break_stmt
           | continue_stmt
           | defer_stmt
           | assert_stmt
           | block ;

let_stmt   = "let" IDENT [ ":" type ] "=" expr ;       (* immutable; always initialized *)
var_stmt   = "var" IDENT ":" type "=" expr             (* mutable: typed + initialized  *)
           | "var" IDENT ":" type                      (*          typed → zero value   *)
           | "var" IDENT "=" expr ;                     (*          inferred from expr   *)
assign_stmt= lvalue assign_op expr ;
assign_op  = "=" | "+=" | "-=" | "*=" | "/=" | "%="
           | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
lvalue     = qualified_ident { index | field_access } ;
index      = "[" expr "]" ;
field_access = "." IDENT ;

expr_stmt  = expr ;                                    (* call w/ effects, etc. *)
(* defer takes a CALL only (not an arbitrary expr) — the deferred action must be a   *)
(* function/method/closure invocation, so there is a well-defined effect to run at   *)
(* scope exit. `defer x + 1` is a syntax error. Full semantics in design.md §Runtime.*)
(* Syntactically parsed as postfix_expr; semantic validation requires the outermost  *)
(* postfix chain to include a call suffix (this is a semantic, not a pure-syntax,     *)
(* constraint — the parser accepts postfix_expr and rejects non-call in analysis).   *)
defer_stmt = "defer" call_expr ;
call_expr  = postfix_expr ;   (* parsed as postfix_expr; semantic check: top of chain is a call *)

(* assert <pred> — proven→erased; unknown→runtime guard; refuted→compile error.     *)
(* Use @verify(static) assert <pred> to require static-only discharge (no runtime   *)
(* guard fallback); this replaces the former `proof` statement.                     *)
assert_stmt = "assert" pred_expr ;

(* transaction — a transactional-memory block lowering onto Medha (design.md §7c).  *)
(*   It is an EXPRESSION (transaction_expr, §7 primary) of type CommitReport; as a   *)
(*   statement it is reached via expr_stmt (report discarded). Body reads/writes on  *)
(*   transactional resources become Medha load/stage; a failed transaction yields    *)
(*   Medha's TxError, mapped to Result.Err per §7c.3 (never falls through).          *)
(*   The argument productions below are shared by transaction_expr (§7).             *)
transaction_args = transaction_arg { "," transaction_arg } [ "," ] ;

transaction_arg  = "isolation"    "=" transaction_isolation
                 | "retry"        "=" INT_LIT
                 | "replay"       "=" transaction_replay
                 | "conflict"     "=" transaction_conflict
                 | "partial"      "=" transaction_partial
                 | "durability"   "=" transaction_durability
                 | "coordinator"  "=" STRING_LIT
                 | "distribution" "=" transaction_distribution ;

transaction_isolation = "read_committed" | "snapshot" | "serializable" ;

transaction_replay    = "unknown"
                      | "non_idempotent"
                      | "body_idempotent"
                      | "body_and_effects_idempotent"
                      | "unknown_but_retry_allowed" ;

transaction_conflict  = "optimistic" | "pessimistic" | "deterministic" ;

transaction_partial   = "require_atomic_coordinator" | "allow_in_doubt" | "best_effort" ;

transaction_durability = "memory" | "process" | "durable" ;
(*   memory  = in-process visibility only (default)     §14.1  *)
(*   process = runtime journal; survives worker failure  §14.1  *)
(*   durable = flushed to external durable storage       §14.1  *)

transaction_distribution = "none" | "local" | "shard" | "replicated" ;
(*   local == none (v1); shard/replicated require coordinator (v2 §11.6)            *)

(* §2.3 abort — explicit transaction abort; contextual keyword inside tx body *)
tx_abort_stmt = "abort" "(" expr ")" ;

(* §2.2 yield — produce body value from transaction; contextual keyword inside tx body *)
tx_yield_stmt = "yield" expr ;

return_stmt= "return" [ expr ] ;
break_stmt = "break" ;
continue_stmt = "continue" ;
```

### 6.1 Control Flow

```ebnf
if_stmt    = "if" expr block [ "else" ( if_stmt | block ) ] ;

for_stmt   = "for" IDENT [ "," IDENT ] "in" range_expr block ;
(* `for i in 0..n` / `for k, v in map_expr`; the optional second IDENT is the     *)
(* value binding for index,value iteration over a slice; without it, IDENT ranges  *)
(* over the range_expr. The parser accepts any `expr` in the range_expr slot;      *)
(* a non-range iterator expr is validated in analysis.                              *)
range_expr = expr ;   (* `a..b` / `a..=b` / bare iterator — all just `expr` (§7 L7) *)

while_stmt = "while" expr block ;

match_stmt = "match" expr "{" { match_arm NEWLINE } "}" ;
match_arm  = pattern "=>" ( expr | block ) ;
pattern    = "_"                                    (* wildcard *)
           | literal_pattern
           | ctor_pattern
           | binding_pattern ;
literal_pattern = INT_LIT | FLOAT_LIT | STRING_LIT | BOOL_LIT ;
ctor_pattern    = qualified_ident [ "(" pattern { "," pattern } ")" ] ; (* Result.Ok(x) *)
binding_pattern = IDENT ;
```

`match` is **exhaustive** over `enum`/`Result`/`Option`; a non-exhaustive match is a compile diagnostic (unless a `_`
arm is present).

### 6.2 Transactions

`transaction { … }` runs its body as a transactional-memory transaction on the **Medha** substrate (full semantics in
`design.md` §Transactional Memory via Medha). The optional argument list sets a
`TransactionOptions` (`design.md` §7c.2) — each `transaction_arg` maps 1:1 onto a `medha::options`
field; omitted args take Medha's defaults (`isolation = snapshot`, `retry = 0`,
`conflict = optimistic`, `replay = unknown`, `partial = require_atomic_coordinator`,
`distribution = none`).

```crank
transaction {                                   // defaults: snapshot / optimistic / no retry
    let a = accounts["a"]                        // read  → Medha load  (recorded in read set)
    accounts["a"] = a - amount                   // write → Medha stage (recorded in write set)
}                                                // CommitReport discarded; failure still propagates

let report = transaction(isolation = serializable, retry = 3,
                         replay = body_and_effects_idempotent) {
    // retry>0 requires a retry-safe replay value (grammar-checked, design.md §7c.5)
    let b = accounts["b"]
    accounts["b"] = b + amount
}                                                // report : CommitReport (always committed here)
```

**Explicit abort (§2.3):** `abort(error)` immediately stops the body, rolls back staged writes, runs controlled defers,
and propagates the error. It is a contextual keyword — valid only inside a transaction body; it is not a reserved word
at function scope.

```crank
transaction {
    let balance = accounts[id]
    if balance < amount {
        abort(TxError.insufficient_funds)        // rolls back; no commit
    }
    accounts[id] = balance - amount
}
```

**Body value (§2.2):** `yield expr` produces a value from the transaction body. When present the expression type is
`Result[TransactionResult[T], TxError]` where `T` is the yielded type; without `yield`, `T` is `Unit`. Like `abort`,
`yield` is contextual — valid only inside a transaction body.

```crank
let result = transaction {
    let updated = accounts[id] + interest
    accounts[id] = updated
    yield updated                                // type: Result[TransactionResult[Int], TxError]
}
```

A `transaction { … }` is an **expression** of type `CommitReport` (design.md §7c.2a): bind it with
`let report = transaction { … }` to keep the report (attempts/conflicts/telemetry), or use it as a bare statement to
discard the report. On failure control never falls through — the `TxError`
propagates per §7c.3.

Grammar-level constraints (enforced in semantic analysis, see `design.md` §7c for the full rules):

- **Isolation levels.** Three levels are accepted: `read_committed` (no dirty reads; non-repeatable reads and phantoms
  possible), `snapshot` (read-stable; write-skew possible), `serializable` (full; v1 single-resource only — see
  cross-resource rule above).
- **Retry/replay pairing (§7c.5).** `retry = N` with `N > 0` requires
  `replay ∈ { body_idempotent, body_and_effects_idempotent, unknown_but_retry_allowed }`.
  `replay = non_idempotent` or bare `replay = unknown` with `retry > 0` is a compile diagnostic.
- **Transactional-resource writes only.** `resource[key] = value` inside a `transaction` is legal only if `resource`'s
  host type is registered transactional (`medha::resource_traits<R>`); writing a non-transactional resource is a compile
  diagnostic.
- **Cross-resource serializable (§7c.4).** `transaction(isolation = serializable)` touching **more than one**
  transactional resource is a compile diagnostic in v1 (no cross-resource coordinator); single-resource serializable and
  any-resource `snapshot` are allowed.
- **`old(resource[key])` needs snapshot capability (§7c.4).** legal only if the resource is snapshot-capable
  (`resource_traits<R>::supports_snapshot`); otherwise a compile diagnostic. `old` over a pure local needs no
  capability.
- **Concurrency restrictions (v1, §7c.6).** `await`, `spawn`, a nested `transaction` under
  `@parallel`, and GPU lowering of a transaction body are **rejected** inside a `transaction`
  (Medha's `transaction_context` is thread-affine in v1). Same-thread nested `transaction` blocks are allowed and
  flatten into the parent.
- **Failure propagation (§7c.3).** A non-committed transaction yields a `TxError` (a structured error record, not an
  enum); in a `fn -> Result[T, E: FromTxError]` it becomes `Result.Err`, otherwise the function must select
  `@on_safety_failure(trap|terminate|host_handler)` or the
  `transaction` is a compile diagnostic (same rule as `SafetyError`, `design.md` §7b.3).
- **`abort` scope (§2.3).** `abort(error)` is a contextual statement — a compile diagnostic if used outside a
  transaction body. The error argument is any expression assignable to `TxError`.
- **`yield` scope (§2.2).** `yield expr` is a contextual statement — a compile diagnostic if used outside a transaction
  body. All `yield` expressions in one transaction body must have the same type.

---

## 7. Expressions

Precedence, lowest → highest (binds tighter downward). This table is the single source of truth for the lexy
`expression` production (§8.3).

| Lvl | Operators                          | Assoc   | Notes                         |
|-----|------------------------------------|---------|-------------------------------|
| 1   | `\|\|`                             | left    | logical or                    |
| 2   | `&&`                               | left    | logical and                   |
| 3   | `== != < <= > >=`                  | single  | comparison (non-chaining)     |
| 4   | `\|`                               | left    | bitwise or                    |
| 5   | `^`                                | left    | bitwise xor                   |
| 6   | `&` `&^`                           | left    | bitwise and / and-not         |
| 7   | `.. ..=`                           | left    | range (excl / incl)           |
| 8   | `<< >>`                            | left    | shifts                        |
| 9   | `+ -`                              | left    | add / sub                     |
| 10  | `* / %`                            | left    | mul / div / mod               |
| 11  | `as`                               | left    | checked type conversion       |
| 12  | unary `! -` `await`                | prefix  | no address-of in surface lang |
| 13  | call `()` index `[]` field `.` `?` | postfix | left-assoc chain              |
| 14  | literals, `(expr)`, composite lits | —       | primary                       |

```ebnf
expr        = or_expr ;
(* levels 1..11 fold via the precedence table above. The range operators `..`/`..=` *)
(* are level-7 binary operators inside `expr` (a `range_node` in the AST), NOT a     *)
(* separate `range_expr` production — §6.1 `range_expr` names the operand slot the    *)
(* `for` loop reads, but the operator itself lives in this precedence table.          *)

unary_expr  = { "!" | "-" | "await" } postfix_expr ;
postfix_expr= primary { call_suffix | index | field_access | "?" } ;
call_suffix = "(" [ arg_list ] ")" ;
arg_list    = expr { "," expr } ;

primary     = literal
            | qualified_ident
            | "(" expr ")"
            | composite_lit
            | closure_lit
            | spawn_expr
            | transaction_expr
            | builtin_call
            | view_expr ;       (* §7.13 domain view construction (feature-gated: domain_views §11) *)

(* §7.13 view_expr — domain view construction expression.
   The whole view_expr is a primary; postfix chains attach to the view result:
     (view a as Tensor[...]).matmul(x)
   Source is a qualified_ident or parenthesized expr — not a full expr.
   Using a full expr would cause the level-10 "as" cast operator to greedily
   consume the keyword, making the view_expr "as" unreachable. For complex
   sources parenthesize: view (x.field) as T. *)
view_source = qualified_ident | "(" expr ")" ;
view_expr   = "view" view_source "as" type ;

(* transaction is an EXPRESSION yielding CommitReport (design.md §7c.2a). As a statement it     *)
(* is the same production reached through expr_stmt (report discarded); `let r = transaction{}` *)
(* binds the CommitReport. Failure never falls through — it propagates per §7c.3.               *)
transaction_expr = "transaction" [ "(" transaction_args ")" ] block ;

literal     = INT_LIT | FLOAT_LIT | STRING_LIT | RAW_STRING_LIT | BOOL_LIT | "Unit" ;

composite_lit = type "{" [ expr { "," expr } ] "}"               (* struct / array — positional *)
              | "[" [ expr { "," expr } ] "]" ;                  (* slice literal *)
(* v1 composite literals are POSITIONAL only: the curly/bracket body is a comma list of *)
(* `expr`. Named field init (`IDENT ":" expr`) is not parsed in v1; fields bind by       *)
(* declaration order and the type checker maps them onto the struct's field list.        *)

closure_lit   = fn_closure | pipe_closure ;
fn_closure    = "fn" "(" [ params ] ")" [ "->" type ] block ;    (* anon fn / lambda — existing form *)
pipe_closure  = "|" [ closure_params ] "|"                       (* pipe-style closure — new form *)
                ( [ "->" type ] block | expr ) ;                 (*   block body or bare expr (implicit return) *)
closure_params= closure_param { "," closure_param } ;
closure_param = IDENT [ ":" type ] ;                             (*   type optional — inferred *)
(* Both closure forms lower to the same AST node (closure_tag, id 1020).               *)
(* Zero-param pipe closure: `|| expr` (two `|` tokens, no params).                     *)
(* Disambiguation: a leading `|` in PRIMARY position is unambiguously a closure-open.  *)
(* In INFIX position `|` / `||` retain their bitwise-or / logical-or meaning.          *)

spawn_expr    = "spawn" expr ;                                  (* async task -> Future[T] (operand parsed as full expr; a postfix call is the idiomatic form) *)

builtin_call  = builtin_name "(" [ arg_list ] ")" ;
builtin_name  = "len" ;
(* Lean Charter §7: only `len` remains a grammar-level builtin.                     *)
(* cap, append, make, print, indices, parallel.map/reduce/for moved to prelude;     *)
(* they parse as plain call expressions (qualified_ident + call_suffix).            *)
```

- `as` is **checked** conversion between fixed-width types; narrowing that loses information at a compile-time-known
  constant is a diagnostic, at runtime returns via the guard path (`design.md` §Safety). No implicit conversions
  anywhere.
- Comparison operators do **not** chain (`a < b < c` is a syntax error) — removes a common bug class and simplifies the
  precedence-climbing parser.

### 7.5 Predicate Sublanguage (`pred_expr`)

Predicates appear in refinement types (`{ x: T | pred }`), function contracts (`requires`/`ensures`), and `assert`
statements. `pred_expr` is a **pure, total, side-effect-free** subset of expressions — it is what crank hands to Tarka
to build a
`tarka::Term` (see `design.md` §Verification-in-the-Language).

```ebnf
pred_expr   = pred_impl ;
pred_impl   = pred_or [ "->" pred_impl ] ;               (* p -> q  (right-assoc, desugars !p||q) *)
pred_or     = pred_and { "||" pred_and } ;
pred_and    = pred_atom { "&&" pred_atom } ;
pred_atom   = "!" pred_atom
            | quantified
            | "old" "(" arith_expr ")"
            | arith_expr                                 (* relations fold in via §7 L3 inside expr *)
            | "(" pred_expr ")" ;

(* rel_expr is not a separate production: comparison operators (==, !=, <, <=, >, >=)  *)
(* live at §7 level 3 inside `expr`, so a relation is just an `arith_expr`. The pred    *)
(* layer only owns implication / or / and / not / quantifiers / old(...) on top.        *)
rel_op      = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
arith_expr  = expr ;   (* full §7 expr; restricted to the pure/total subset in analysis (no effectful calls) *)

quantified  = ( "forall" | "exists" ) quant_binder { "," quant_binder } ( "." | ":" ) pred_expr ;
quant_binder= typed_binder | ranged_binder ;
typed_binder = IDENT ":" type ;                          (* forall i: Int32 . 0 <= i && i < n — legacy form *)
ranged_binder= IDENT "in" range_expr ;                   (* forall i in 0..len(xs): xs[i] >= 0 — new form *)
(* Ranged binder desugars to a guarded typed quantifier:                                *)
(*   forall i in a..b: p  ≡  forall i: <IntType> . (a <= i && i < b) -> p             *)
(*   (for ..= upper bound the guard uses i <= b instead of i < b)                      *)
(* Both forms produce quantifier_tag (id 1021) with quant_bound_kind = typed | ranged. *)
```

Rules:

- Only **pure** calls are allowed inside a predicate (`@pure` fns, `len`, arithmetic, field/index). Any effectful call
  is a diagnostic — predicates must lower cleanly to SMT terms.
- The special identifier `result` is bound to the return value inside an `ensures` predicate.
- `old(expr)` (reserved builtin) refers to a value at function entry inside `ensures` (framing). Inside a `transaction`
  block, `old(expr)` refers to `expr` evaluated at **transaction entry** (the first transactional read snapshot), per
  Medha snapshot semantics — and `old(resource[key])`
  requires the resource to be **snapshot-capable**
  (`medha::resource_traits<R>::supports_snapshot`); otherwise it is a compile diagnostic (design.md §7c.4). Outside
  these two contexts —
  `ensures` predicates and transaction bodies — `old(expr)` is a diagnostic.
- Quantifiers map to Tarka/Z3 `forall`/`exists`; unbounded quantifiers over infinite domains are accepted syntactically
  but may return `unknown` from the solver → treated per verification policy:
  `@verify(static) assert` unknown = compile error; plain `assert` unknown = runtime guard where expressible, else
  error.
- Quantified predicates must have **pure, total** bodies (`CRANK-Q-004` if an effectful call or `?`
  appears inside a predicate). Bodies lowering to non-canonical proof terms are rejected.

### 7.6 Error Propagation — `?` operator

`?` is a **postfix** operator at precedence level 13 (same level as call/index/field).

```ebnf
postfix_expr = primary { call_suffix | index | field_access | "?" } ;
```

**v1 semantics (Result/Option only):**

| Operand type   | On success | On failure                |
|----------------|------------|---------------------------|
| `Result[T, E]` | yields `T` | returns `Err(convert(e))` |
| `Option[T]`    | yields `T` | returns `None`            |

For `Result` operands, the enclosing function's return type must be `Result[_, F]` where `F:
From[E]` (the `From`-based residual conversion). There is no generalized `Try` trait in v1.

**Sema diagnostics:**

| Code          | Condition                                                                      |
|---------------|--------------------------------------------------------------------------------|
| `CRANK-Q-001` | `?` applied to a non-`Result`/`Option` value                                   |
| `CRANK-Q-002` | No `From[E]` impl for residual conversion                                      |
| `CRANK-Q-003` | Enclosing function return type is incompatible                                 |
| `CRANK-Q-004` | `?` inside a `pred_expr` (predicates are pure/total)                           |
| `CRANK-Q-005` | `?` crossing a `transaction`/`async` boundary without an explicit error policy |

**ASI:** `?` is chain-closing — a line ending in `?` does **not** set `line_continues`. A `?.`
continuation across a newline is covered by the existing ASI carve-out for `.` after
`)`/`]`/`}`.

**Prelude form (`Type.try_from`):** For explicit `Result`-returning conversion without a guard, the prelude exposes
`Type.try_from(value) -> Result[Type, ConversionError]`. Distinct from `as`:
`as` inserts a proof/guard per the active safety policy; `try_from` always returns a `Result`.

### 7.7 Closures

Crank supports two syntactically equivalent closure forms that both lower to `closure_tag` (id 1020):

```ebnf
closure_lit   = fn_closure | pipe_closure ;
fn_closure    = "fn" "(" [ params ] ")" [ "->" type ] block ;
pipe_closure  = "|" [ closure_params ] "|" ( [ "->" type ] block | expr ) ;
closure_params= closure_param { "," closure_param } ;
closure_param = IDENT [ ":" type ] ;
```

Examples:

```crank
let double = fn(x: Int64) -> Int64 { return x * 2 }    // fn-form
let add    = |a: Int64, b: Int64| a + b                 // pipe-form, bare expr
let norm   = |v: Vec| -> Float64 { return v.magnitude } // pipe-form, block + return type
let unit   = || 42                                      // zero-param pipe closure
```

**Capture semantics (v1):**

| Capture kind                       | Rule                            |
|------------------------------------|---------------------------------|
| `Copy` type                        | copied (no move, no borrow)     |
| move-only                          | moved into the closure          |
| borrow that may escape owner scope | **rejected** (`CRANK-CLOS-001`) |

Callability class is inferred: consuming capture → `FnOnce`; mutable capture → `FnMut`; else `Fn`. These classes are
internal; user-facing `FnMut`/`FnOnce` bounds are gated (grammar §5.2).

`spawn expr` requires the operand's captured values to be transfer-safe (thread-safe, lifetime-safe, move/copy-safe, no
unsynchronized mutable alias). Violation → `CRANK-SPAWN-001`.

---

## 8. lexy Mapping

lexy (`dependencies/lexy/include`) is the sole parser. Crank uses lexy's **rule DSL** for the fixed grammar and lexy's
**`dsl::expression` / Pratt operator** support (`lexy/dsl/expression.hpp`,
`lexy/dsl/operator.hpp`) for §7 precedence. Output is a lexy `parse_tree` that the crank frontend walks to build the
Vākya AST (see `design.md` §Frontend / Parse phase).

### 8.1 Production ↔ lexy header map

| Crank production          | lexy construct                                                                                     |
|---------------------------|----------------------------------------------------------------------------------------------------|
| `IDENT`, keywords         | `dsl::identifier(dsl::ascii::alpha_underscore, …)` + `.reserve(kw…)`                               |
| `INT_LIT`/`FLOAT_LIT`     | `dsl::integer`, `dsl::digits`, `dsl::sign`, custom float scan                                      |
| `STRING_LIT`              | `dsl::quoted` + `dsl::escape` (`lexy/dsl/delimited.hpp`)                                           |
| `RAW_STRING_LIT`          | `dsl::delimited(dsl::lit_c<'`'>)` no-escape                                                        |
| comments/trivia           | `dsl::whitespace` rule + retained trivia policy                                                    |
| newline / ASI             | `dsl::newline` + `dsl::context_flag` continuation tracking (§8.4)                                  |
| `block` `{…}`             | `dsl::curly_bracketed.list(...)`                                                                   |
| `params`,`arg_list`       | `dsl::parenthesized.opt_list(item, sep=dsl::comma)`                                                |
| `slice`/`array` types     | `dsl::square_bracketed` variants                                                                   |
| `attribute`               | `dsl::lit_c<'@'>` + (builtin kw \| qualified_ident) + optional `parenthesized.opt_list`            |
| `trait_decl`/`impl_decl`  | `dsl::keyword<trait/impl>` + optional generic_params + `dsl::curly_bracketed.list(member)`         |
| `match`/`if`/`for`        | keyword `dsl::keyword<…>` + `dsl::p<production>` recursion                                         |
| precedence expr (§7)      | `dsl::expression` with an `operation` per level (§8.3)                                             |
| `refined_type` `{x:T\|p}` | `dsl::curly_bracketed(ident + colon + type + `\|` + p<pred_expr>)`                                 |
| contract clauses          | `dsl::keyword<requires/ensures>` + `dsl::p<pred_expr>`                                             |
| `assert`                  | `dsl::keyword<assert>` + `dsl::p<pred_expr>`                                                       |
| `transaction_expr`        | `dsl::keyword<transaction>` + optional `parenthesized.opt_list(transaction_arg)` + `dsl::p<block>` |
| `extern_fn_decl`          | `dsl::keyword<"extern">` + `dsl::keyword<"fn">` + IDENT + params + optional return type; no block  |
| `forall`/`exists`         | `dsl::keyword<…>` + binder list + `dsl::lit_c<'.'>` + `p<pred_expr>`                               |
| error recovery            | `dsl::try_(rule, dsl::recover(dsl::newline))` at statement boundaries                              |
| source spans              | `lexy::parse_tree` node positions → crank `source_span`                                            |

### 8.2 Production skeleton (illustrative, not code)

Each nonterminal maps to a lexy `struct <Name> { static constexpr auto rule = …; };`. The frontend defines one
production struct per §2–§7 nonterminal. Whitespace is set once via a
`whitespace` member on the root production and inherited.

### 8.3 Expression via `dsl::expression`

The §7 table is encoded as nested lexy `operation` structs, one per precedence level, ordered so lexy's Pratt engine
binds level 10 (`* / %`) tighter than level 9 (`+ -`), etc. Left-associative levels use `dsl::infix_op_left`;
`unary_expr` uses `dsl::prefix_op`; postfix call/index/field use
`dsl::postfix_op`. The range operators `..`/`..=` are a level-7 `infix_op_left` (looser than shift, tighter than
bitwise-and), so `a..b` and `a..=b` are ordinary expressions. `as` is a level-11 left
`infix_op_left` whose RHS is a `type` production, not an `expr` (special-cased). Non-chaining comparison (level 3) is
enforced by making it a `dsl::infix_op_single` (single, non-associative) so
`a<b<c` fails to parse.

### 8.4 Automatic Semicolon Insertion (ASI)

Crank has no visible statement terminator. ASI is implemented at the lexy layer:

1. A `context_flag` `line_continues` is **set** whenever the last significant token on a line is a binary operator, `,`,
   or an unclosed opening bracket (tracked by lexy's bracket productions).
2. At a `NEWLINE`, if `line_continues` is clear **and** bracket depth is zero, the frontend emits a synthetic statement
   terminator into the token stream; otherwise the newline is trivia.
3. Inside `( )` / `[ ]` (function calls, slice literals, multi-line expressions) newlines are always trivia — bracket
   depth suppresses ASI.
4. **`else` carve-out**: a synthetic terminator is *not* inserted after a `}` when the next significant token is
   `else` — so `}` and `else` may sit on separate lines (§10.1). Same rule applies to the `.` of a postfix chain
   continued on the next line after `)`/`]`/`}`.

This mirrors Go's rule set but is expressed declaratively through lexy context state rather than a hand-written scanner.
See gap `G-LEX-2` in `design.md` if lexy context-flag ergonomics prove insufficient.

### 8.5 Diagnostics

lexy `error` productions carry a stable string code + `source_span`. The frontend maps every lexy error into a
`vakya::diag::diagnostic` (or `lithe::diag::diagnostic`) so parse, semantic, and backend diagnostics share one sink
(`design.md` §Diagnostics). Error recovery synchronizes on
`NEWLINE` / `}` so one syntax error does not cascade.

---

## 9. Full Example (parses under this grammar)

```crank
package app

import "host.math"

type Vec3 = struct {
    x: Float32
    y: Float32
    z: Float32
}

@pure
pub fn Dot(a: Vec3, b: Vec3) -> Float32 {
    return a.x * b.x + a.y * b.y + a.z * b.z
}

// automatic execution is the default (§4.6a): ordinary code, no annotation.
// Lithe discovers this loop is a parallel-safe element-wise map on its own.
pub fn Scale(xs: []Float32, out: []Float32, factor: Float32) -> Result[Unit, String]
    requires len(xs) == len(out)                 // precondition (proven at call site)
{
    // len(xs)==len(out) is now an assumption; bounds checks on out[i] discharge for free
    for i in 0..len(xs) {
        out[i] = xs[i] * factor
    }
    return Result.Ok(Unit)
}

// power-user variant: @parallel(required=true) is a HARD constraint — compile error
// if Lithe cannot prove the region parallel-safe (opt-in, not the normal style).
@parallel(required=true)
pub fn ScaleForced(xs: []Float32, out: []Float32, factor: Float32) -> Result[Unit, String]
    requires len(xs) == len(out)
{
    for i in 0..len(xs) {
        out[i] = xs[i] * factor
    }
    return Result.Ok(Unit)
}

// non-Result fn keeping a runtime guard MUST pick a non-return_result policy (§7b.3)
@on_safety_failure(trap)
fn Mean(xs: []Float32) -> Float32 {              // returns plain Float32, not Result
    var sum = 0.0                                 // var: inferred from initializer
    for i in 0..len(xs) {
        sum += xs[i]
    }
    return sum / len(xs) as Float32              // div guard: len==0 → trap (per policy)
}

// refinement type: SafeDiv only accepts a provably-nonzero divisor → no div guard at all
fn SafeDiv(a: Int32, b: { d: Int32 | d != 0 }) -> Int32
    ensures result == a / b
{
    return a / b
}

fn Sorted(xs: []Int32) -> Bool
    ensures result == forall i in 0..len(xs)-1: xs[i] <= xs[i+1]
{
    @verify(static) assert len(xs) >= 0         // static: must discharge or compile error
    for i in 1..len(xs) {
        if xs[i-1] > xs[i] { return false }
    }
    return true
}

// ? propagates Result.Err without pattern matching — caller sees the error transparently
pub fn Parse(input: String) -> Result[Int32, String] {
    let v = validate(input)?                     // ? unwraps Ok or early-returns Err
    return Result.Ok(transform(v))
}

// pipe closure: |x| expr — concise single-expression closures; fn-form for blocks
pub fn ApplyAll(xs: []Int32, f: Fn(Int32) -> Int32) -> []Int32 {
    var out: []Int32 = Vec[Int32].with_len(len(xs))
    for i in 0..len(xs) {
        out[i] = f(xs[i])
    }
    return out
}

// transaction: reads/writes on a transactional resource lower to Medha load/stage.
// Returns Result[_, TxError] → a non-committed transaction becomes Result.Err (design.md §7c.3).
fn Transfer(accounts: AccountStore, a: AccountId, b: AccountId, amount: Int64)
    -> Result[Unit, TxError]
    requires amount >= 0
{
    let report = transaction(isolation = serializable, retry = 3,
                             replay = body_and_effects_idempotent) {
        let from = accounts[a]                    // → Medha load
        let to   = accounts[b]
        accounts[a] = from - amount               // → Medha stage
        accounts[b] = to + amount
        assert accounts[a] == old(accounts[a]) - amount
    }
    return Result.Ok(Unit)
}

fn Classify(v: Vec3) -> String {
    let m = Dot(v, v)
    assert m >= 0.0                              // proven (dot of self) → erased
    match m {
        0.0        => "zero"
        _          => "nonzero"
    }
}

// --- stronger generics (design.md §17) ------------------------------------------

trait Monoid {
    fn identity() -> Self
    fn combine(a: Self, b: Self) -> Self
    associative: Bool
    commutative: Bool = false
}

impl Monoid for Int64 {
    fn identity() -> Int64 { return 0 }
    fn combine(a: Int64, b: Int64) -> Int64 { return a + b }
    associative = true
    commutative = true
}

// generic reduce: associativity fact + ParallelSafe bound → auto-parallelizable
pub fn Reduce[T: Monoid + ParallelSafe](xs: []T) -> T {
    var acc = T.identity()
    for i in 0..len(xs) {
        acc = T.combine(acc, xs[i])
    }
    return acc
}

// const generics: dimension proven at compile time via the shape algebra (§5.3)
pub fn Dot2[N: usize](a: [N]Float32, b: [N]Float32) -> Float32 {
    var sum: Float32 = 0.0
    for i in 0..N {                              // N is a type-level constant, bounds trivially proven
        sum += a[i] * b[i]
    }
    return sum
}

// callable bound: Fn(T)->U carries arity + return type; Pure + GpuCompatible on the callable
@gpu(preference=strong)
pub fn MapGpu[T, U, F: Fn(T) -> U + Pure + GpuCompatible](xs: []T, f: F) -> []U {
    var out: []U = Vec[U].with_len(len(xs))
    for i in 0..len(xs) {
        out[i] = f(xs[i])
    }
    return out
}

// pipe closure usage — inline transform
fn double_all(xs: []Int32) -> []Int32 {
    return ApplyAll(xs, |x| x * 2)
}

// namespaced extension annotation: typed, schema-validated metadata (design.md §5b)
@lithe.cacheline(align=64)
type Particle = struct {
    px: Float32
    py: Float32
    pz: Float32
}
```

---

## 10. Resolved Semantics & Open Questions

Cross-referenced in `design.md` §Library Gaps where they touch framework code.

### 10.1 Resolved (normative)

- **Integer overflow.** Signed and unsigned fixed-width arithmetic **wraps** (two's-complement)
  by default — this matches Lithe MIR integer ops (`reference.md`: "MIR integer ops are wrapping two's-complement"), so
  the surface language and the IR agree with zero lowering surprise. A checked form is available per-operation via host
  builtins (`add_checked`/`mul_checked` →
  `Result[T, Overflow]`) and a module-level `@overflow(checked)` attribute flips the default to trap for a scope. No
  undefined behavior in either mode.
- **Division / modulo by zero.** Integer `/` and `%` by zero is a **runtime guard** (same machinery as bounds): the
  compiler emits the obligation `divisor != 0`; proven → no check; unknown → guard; refuted (constant `0`) → compile
  error. A failed runtime guard follows the function's `safety_failure` policy (design.md §Safety-Failure Policy). Float
  division by zero follows IEEE-754 (`inf`/`nan`, no trap).
- **`as` conversions (signed/unsigned/narrowing).** `as` is **checked and explicit**:
    - widening (e.g. `Int32 as Int64`) is always lossless, no guard.
    - narrowing (`Int64 as Int32`) and sign-change (`Int32 as UInt32`) that would lose information emit a
      `value-in-range` obligation: constant out of range → compile error; runtime unknown → guard (per
      `safety_failure`). Two's-complement reinterpretation is **not** implicit — request it with the `bitcast[T](x)`
      host builtin.
    - `Float ↔ Int` uses round-toward-zero with a range guard; out-of-range → guard.
- **`else` on a new line.** `}` `else` on separate lines is **allowed**. ASI (§8.4) suppresses the synthetic terminator
  after a `}` that is immediately followed (modulo trivia/newlines) by
  `else` — so both `} else {` and `}⏎else {` parse identically. This is a specific ASI carve-out, mirroring the
  operator/comma/open-bracket continuation rules.
- **`defer` accepts calls only.** Grammar §6 restricts `defer` to `call_expr`. Rationale + full runtime semantics:
  design.md §Runtime/`defer`.
- **`Result.Ok` / `Option.Some` etc.** These are **built-in enum constructor syntax**, not ordinary function calls.
  `Result`/`Option` are predeclared `enum`s; `Result.Ok(x)`, `Result.Err(e)`,
  `Option.Some(x)`, `Option.None`, and any user `Enum.Variant(...)` parse as `ctor_pattern`-shaped constructor
  expressions (a `qualified_ident` naming `Type.Variant` in call/nullary position). The type checker resolves them
  against the enum's variants — they are not looked up in the value namespace, so a user cannot shadow `Ok` with a
  function.
- **`parallel.map/reduce/for` (prelude functions).** These former grammar builtins are now prelude functions — they
  parse as plain call expressions (`qualified_ident` + call suffix). Loops with
  `@parallel` lower through Lithe for automatic parallelization via Pravaha. No `import "parallel"`
  is required; the names are resolved through the prelude binding.
- **`transaction` lowers to Medha.** The `transaction` block (§6.2) is a language keyword, not a library call; it is an
  **expression** yielding `CommitReport` (design.md §7c.2a) and lowers onto the Medha substrate (`design.md` §7c). Medha
  is a **required** crank runtime component. Transaction option words (`isolation`/`retry`/`replay`/`conflict`/
  `partial`/
  `distribution` and their values) are **contextual** — reserved only inside a `transaction(...)`
  argument list. `distribution = none` (alias `local`) is the only v1 mode (Medha is local-process); the other
  `distribution` values (`shard`/`replicated`) are reserved v2 metadata. Transactional resource indexing, retry/replay
  pairing, effect admission, and the v1 concurrency restrictions (`await`/`spawn`/`@parallel`/GPU forbidden in a tx
  body) are all specified in `design.md` §7c.

### 10.2 Open / deferred

- **Generics surface**: v1 supports monomorphized generics with **multiple trait bounds**
  (`[T: Numeric + Copy]`), explicit `trait`/`impl` conformance, ownership traits (`Copy`/`Clone`/`Move`/`Drop`),
  **callable bounds** (`Fn(T) -> U`), the coherence/orphan rule, and **basic const generics** for dimensions
  (`[N: usize]`, literal/parameter references only) — full model in `design.md` §17. **Associated types** + projection
  syntax (`R.Key`), generic modules, shape/layout constraints, type specialization, and const-generic expressions are
  **v2**; higher-kinded types, variance, and type-level functions are **v3 / out of scope for v1** (§17.12).
- **Operator overloading**: not in the surface language (keeps precedence table closed and hetero codegen predictable).
  Host C++ ops are exposed as named functions.
- **String interpolation**: deferred; use `+` concatenation or a host `format` function in v1.
- **`defer`**: parses in v1; lowering is scoped-cleanup only (no exceptions to unwind), see
  `design.md` §Runtime.
- **Predicate implication `->`**: inside `pred_expr`, `p -> q` reads as logical implication (desugars to `!p || q`). The
  token `->` is otherwise the return-type arrow; disambiguated by context (only valid inside a predicate). If lexy
  context-sensitivity proves awkward, a dedicated
  `==>` implication token is the fallback (see `design.md` gap `G-LEX-3`).
- **Proof-surface scope (v1)**: refinement types, `requires`/`ensures`, `assert` (with `@verify(static)` for mandatory
  proof),
  `forall`/`exists`, `result`/`old`. Loop invariants (`invariant` clause on `for`/`while`) and
  `decreases` termination measures are designed but deferred to v2 (noted in `design.md`
  §Verification-in-the-Language).
- **Transaction scope (v1)**: `transaction` blocks are single-thread, `distribution = none` only. Distributed
  transactions, cross-resource atomic commit beyond a single resource's
  `commit_capability`, and Pravaha-scheduled replay-safe attempts are designed but opt-in / deferred (Medha exposes the
  distributed `tx_status` values + adapters as metadata only in v1;
  `design.md` §7c.6/§7c.7).

```

---

## 11. v2 Grammar Extensions (feature-gated)

All productions in this section **parse** so v2 needs no grammar break, but each is **rejected
unless the matching v2 feature flag is enabled** — the same parse-but-gate treatment already applied
to associated types and type specialization (§5.2, §10.2). When the flag is off, v1 diagnoses the
construct's use. Each entry lists the EBNF, the Vākya AST tag, the guarding diagnostic, and a
one-line lowering note. Feature-word semantics live in `crank.md` §v2.x (cross-referenced per row).

### 11.1 Structured concurrency — `task_scope` / `deadline` / `scope.spawn` (§v2.9)

```ebnf
task_scope_stmt = "task_scope" IDENT block ;              (* scoped task group; children join at `}` *)
deadline_stmt   = "deadline" "(" expr ")" block ;         (* cancel scope if wall time exceeds duration *)
scope_spawn_expr= IDENT "." "spawn" "(" [ expr ] ")" ;    (* bounded spawn; child lifetime ≤ scope *)
scope_cancel_expr = IDENT "." "cancel" "(" ")" ;          (* signal cancellation to all children *)
```

- AST tag: `vakya::task_scope`, `vakya::deadline`, `vakya::scope_spawn` (spawn/cancel are method-call nodes on the scope
  binding).
- Feature flag: `structured_concurrency`. Diagnostic when disabled: `CRANK-CONC-001`
  (structured concurrency requires v2).
- Lowering: `task_scope` lowers to a Pravaha join-group with scope-lifetime enforcement;
  `scope.spawn` captures by value and registers a child bounded by the group; `deadline(d)` arms a wall-clock cancel via
  `crank_future_error::cancelled`. See `crank.md` §v2.9.

### 11.2 Savepoints & compensation — `savepoint` / `rollback_to` / `compensate` (§v2.12)

```ebnf
savepoint_expr    = "savepoint" "(" ")" ;                 (* capture current write-set position *)
rollback_to_stmt  = "rollback_to" "(" expr ")" ;          (* undo writes since a savepoint; reads retained *)
compensate_block  = "compensate" block ;                  (* post-commit compensating action, NOT a rollback *)
```

- AST tag: `vakya::savepoint`, `vakya::rollback_to`, `vakya::compensate`.
- Feature flag: `nested_transactions`. Diagnostic when disabled: `CRANK-TX-SP-001`
  (savepoints/compensation require v2). `compensate` additionally: it is a **post-commit** hook, must be **idempotent**,
  and may **not** perform irreversible effects (reuses the §7c.5 irreversible-effect rule) — a `compensate` block that
  performs an irreversible effect is a diagnostic.
- Lowering: `savepoint()` → `tx_journal::make_savepoint`; `rollback_to(sp)` → `tx_journal::rollback_to`
  (LIFO write undo); `compensate { }` registers a `compensation` in the post-commit
  `compensation_registry` (best-effort, bounded-retry, `compensation_report`). See `crank.md` §v2.12.

### 11.3 Generic module header — `module M[T: Bound] { }` (§v2.3)

```ebnf
module_decl    = "module" IDENT [ generic_params ] block ;  (* generic (parametric) module *)
```

- AST tag: `vakya::module_decl` (adds an optional `generic_params` on the existing module node).
- Feature flag: `generic_modules`. Diagnostic when disabled: `CRANK-GEN-MOD-001`
  (generic modules require v2). `generic_params` reuses §3 (`type_param`/`const_param`); the bound is the module's
  public contract.
- Lowering: each instantiation `M[Float32]` produces a monomorphized module with its own AOT cache key
  (`module_instantiation_result`); the bound `T: Bound` is exported as the module contract. See
  `crank.md` §v2.3.

### 11.4 Layout / Device bounds (§v2.6)

Extends `bound` (§3, production at `bound = qualified_ident | callable_bound`) with two v2-gated constraint forms:

```ebnf
bound          =/ layout_bound | device_bound ;           (* v2 extension of the §3 bound production *)
layout_bound   = "Layout" "[" IDENT "]" ;                 (* Layout[RowMajor] | Layout[ColMajor] *)
device_bound   = "Device" "[" IDENT "]" ;                 (* Device[Gpu] | Device[Simd] | Device[Host] *)
```

- AST tag: `vakya::layout_bound`, `vakya::device_bound` (bound-list entries).
- Feature flag: `layout_device_bounds`. Diagnostic when disabled: `CRANK-GEN-BND-001`
  (layout/device bounds require v2).
- Lowering: layout/device bounds feed the execution planner's backend ranking; an unsatisfiable constraint (no adapter
  provides it) is a diagnostic. See `crank.md` §v2.6.

### 11.5 v2 attributes — `@distributed` / `@reflect`

These are **namespaced-style builtins gated to v2**; the §3.6 `builtin_attr` set is unchanged for v1. When the flag is
off they diagnose rather than lower.

```ebnf
distributed_attr = "@" "distributed" "(" distributed_arg { "," distributed_arg } ")" ;
distributed_arg  = "placement" "=" IDENT                  (* named node / node group *)
                 | "required"  "=" BOOL_LIT               (* hard placement: miss is an error *)
                 | "preferred" "=" BOOL_LIT ;             (* soft placement: local fallback allowed *)

reflect_attr     = "@" "reflect" "(" reflect_facet { "," reflect_facet } ")" ;
reflect_facet    = "fields" | "traits" | "capabilities"
                 | "host_registration" | "backend_adapters" ;
```

- AST tag: `vakya::attr` with resolved kind `distributed` / `reflect` (facet/arg list attached).
- Feature flags: `distributed_execution` (`@distributed`), `reflection` (`@reflect`). Diagnostics when disabled:
  `CRANK-DIST-000` / `CRANK-REFLECT-000` (attribute requires v2).
- Lowering: `@distributed(required=true)` → unplaceable miss is `CRANK-DIST-003` (no local fallback);
  `preferred` (default) relaxes to local with a NADI pulse (`CRANK-DIST-001`). `@reflect(facets…)`
  selects which `type_descriptor<T>` facets a `reflect_builder` emits (pay-for-use). See `crank.md`
  §v2.10 / §v2.16.

### 11.6 Transaction args — `coordinator` + non-`none` `distribution` (§v2.11)

Extends the §6 `transaction_arg` / `transaction_distribution` productions. `coordinator` and the
`shard`/`replicated` distribution modes parse but are v2-gated (v1 accepts only `distribution =
none`, §10.2).

```ebnf
transaction_arg          =/ "coordinator" "=" STRING_LIT ;   (* v2: name an atomic multi-resource coordinator *)
transaction_distribution =/ "shard" | "replicated" ;         (* v2: non-local tx distribution modes *)
```

- AST tag: `vakya::transaction_expr` (adds `coordinator` string + extended `distribution` value).
- Feature flag: `multi_resource_tx`. Diagnostics when disabled: `CRANK-TX-DIST-001` (non-`none`
  distribution requires v2). Related v2 semantic gates already enforced in code: `CRANK-TX-009`
  (`distribution != none` needs a distribution adapter), `CRANK-TX-010`/`011` (coordinator participant/registration),
  `CRANK-TX-012` (snapshot multi-resource write without a coordinator).
- Lowering: `coordinator="name"` binds the tx to a registered multi-resource coordinator for atomic commit;
  `distribution = shard|replicated` requires a distribution adapter (else `CRANK-DIST-010`). See `crank.md` §v2.11.

### 11.7 Domain views — `view_decl` + `view_expr` (§domain_views)

```ebnf
view_decl   = "view" IDENT [ generic_params ] "of" IDENT ":" type
              { "requires" pred_expr } ;
view_source = qualified_ident | "(" expr ")" ;
view_expr   = "view" view_source "as" type ;
primary     =/ view_expr ;
top_decl    =/ view_decl ;
builtin_name =/ "indices" ;   (* pure const-dim range over Shape; used in view method bodies *)
```

- AST tags: `crank::view_decl_tag` (stable_id 1016), `crank::view_expr_tag` (stable_id 1017).
- Feature flag: `domain_views`. Diagnostic when disabled: `CRANK-VIEW-000`
  (`view` used without `domain_views` feature flag). Parse-but-gate: the grammar is always accepted so the parser never
  breaks even when the feature is off; the analysis phase rejects with
  `CRANK-VIEW-000`.
- `"of"` is contextual — matched as a literal inside `view_decl` only; never globally reserved.
- `indices(Shape)` is a core const-dim range builtin (not Sutra); it lowers to an ordinary loop nest.
- Lowering: view construction → erased / base-ref / Lithe `memref_type` (§4.6 of the design); view method calls → named
  `("lithe.hl","call")` + plan-time backend selection. No new IR op family. Domain metadata (`@sutra.*`) is
  compile-stage ephemeral in v1; wire contract deferred to Lithe 1.6.0
  `domain_attr` (v2).
- See `crank.md` §Domain Views for full semantics.
