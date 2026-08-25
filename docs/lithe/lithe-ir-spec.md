# Lithe IR — Normative Specification

**spec_version**: 1.5.0  
**Describes IR schema_version**: {1, 0, 0}–{1, 5, 0}  
**Status**: Normative  
**Conformance language**: RFC 2119 (MUST / MUST NOT / SHOULD / SHOULD NOT / MAY)

> This document is the authoritative contract for all Lithe IR families,
> the portable module container, and the canonical/wire encoding rules.
> `docs/lithe/lithe.md` is the explanatory framework guide; it defers to
> this spec on any conflict. Spec constants are pinned to the code by
> `src/tests/test_lithe_ir_spec_conformance.cpp` (fixture in
> `src/tests/spec/lithe_ir_spec_fixture.hpp`).

---

## Table of Contents

- [§1 — Scope, Audience, Conformance](#1--scope-audience-conformance)
- [§2 — Representation Model (live vs portable)](#2--representation-model-live-vs-portable)
- [§3 — Stages and Families](#3--stages-and-families)
- [§4 — Common Wire Conventions](#4--common-wire-conventions)
- [§5 — Type System and Canonical Type Grammar](#5--type-system-and-canonical-type-grammar)
- [§6 — Graph IR](#6--graph-ir)
- [§7 — HL MIR](#7--hl-mir)
- [§8 — Opcode Signature Registry](#8--opcode-signature-registry)
- [§9 — Physical MIR](#9--physical-mir)
- [§10 — Portable Module Container](#10--portable-module-container)
- [§11 — Canonical Encoding and Semantic Digest](#11--canonical-encoding-and-semantic-digest)
- [§12 — Wire Binary and Text Encodings](#12--wire-binary-and-text-encodings)
- [§13 — Verification Rules](#13--verification-rules)
- [§14 — Versioning, Compatibility, and Upgrade](#14--versioning-compatibility-and-upgrade)
- [§15 — Observation API Reference](#15--observation-api-reference)
- [§16 — Stability Guarantees and Non-Guarantees](#16--stability-guarantees-and-non-guarantees)
- [§17 — Frontend Lowering Contract](#17--frontend-lowering-contract)

---

## §1 — Scope, Audience, Conformance

### §1.1 Scope

This specification covers:

- All Lithe IR families: Graph IR, HL MIR, Physical MIR.
- The portable module container (`portable_module`).
- Canonical and wire encodings (binary and text).
- The verifier validity definition (seven checks).
- Schema versioning and the compatibility predicate.
- The stability contract for tool builders and language front-end authors.

This spec does NOT define execution semantics, backend ABI, code generation
strategy, or optimizer pass ordering. Those are implementation concerns.

### §1.2 Audience

External designers who target Lithe IR:

- **Language front-end authors** — emit IR that producers must validate.
- **Backend authors** — consume IR at the portable boundary.
- **Tool builders** — inspect, transform, or round-trip IR artifacts.

### §1.3 Conformance Targets

Three conformance targets are defined:

| Target            | Definition                                                                                                                                                                                                                                    |
|-------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Producer**      | Emits IR that passes `verify_portable` at the declared `schema_version`. A Producer MUST NOT emit a module whose `verify_portable` result is `!ok` under `verify_policy{require_capability_coverage=true, allow_unknown_optional_ops=false}`. |
| **Consumer**      | Accepts any IR whose `verify_portable` passes at a schema it declares support for, OR rejects with a diagnostics-bearing error.  A Consumer MUST NOT silently misinterpret an IR document it cannot verify.                                   |
| **Round-tripper** | Freeze + thaw, or binary encode + decode, preserving `semantic_digest`.  A Round-tripper MUST produce equal `semantic_digest` before and after the round-trip.                                                                                |

### §1.4 RFC 2119 Usage

The key words MUST, MUST NOT, REQUIRED, SHALL, SHALL NOT, SHOULD, SHOULD NOT,
RECOMMENDED, MAY, and OPTIONAL in this document are to be interpreted as
described in RFC 2119.

---

## §2 — Representation Model (live vs portable)

Lithe uses a **two-representation model**. External tools interact exclusively
with the **portable/wire** forms.

| Attribute  | Live (`hl_mir_function`)                     | Portable/Wire (`lithe_*_ir` + `portable_module`)           |
|------------|----------------------------------------------|------------------------------------------------------------|
| Storage    | Process heap, arena-backed pointers          | Dense id tables (`std::vector`), no raw pointers           |
| Mutability | Mutable during construction and optimization | Immutable interchange form                                 |
| Scope      | In-process only                              | Cross-process, cross-language, persistent artifact         |
| Stability  | NOT a wire contract; integers MAY change     | The interchange contract; stable across schema minor bumps |
| Header     | `lithe_codegen_hl.hpp`                       | `lithe_ir/adapters/hl_mir.hpp`, `portable/module.hpp`      |

**Rule**: Only the portable/wire forms are stable across processes or schema
versions. Live forms are implementation detail and MAY change without a schema
bump. A Producer MUST freeze to a wire form before handing IR to an external
consumer.

The freeze/thaw bridge (`lithe_ir/portable/bridge.hpp`) is the only sanctioned
path between live and portable forms.

---

## §3 — Stages and Families

Source: `include/lithe/lithe_ir/format.hpp` — `lithe::ir::stage`.

### §3.1 Stage Enumeration (STABLE integer values)

| Stage       | Integer | Meaning                            |
|-------------|---------|------------------------------------|
| `surface`   | **0**   | Un-optimized surface AST           |
| `canonical` | **1**   | Canonicalized AST                  |
| `optimized` | **2**   | After optimization passes          |
| `lowered`   | **3**   | After lowering (HL MIR)            |
| `physical`  | **4**   | Physical register MIR              |
| `managed`   | **5**   | Managed-annotated MIR (`lithe_rt`) |

Stage integer values are **STABLE**: they MUST NOT be renumbered within a major
schema version. Changing a stage integer is a major-version breaking change.

### §3.2 Family–Stage Mapping

| IR Family                              | Covered stages                                                 |
|----------------------------------------|----------------------------------------------------------------|
| Graph IR (`lithe_graph_ir`)            | `surface`(0), `canonical`(1), `optimized`(2)                   |
| HL MIR (`lithe_hl_mir_ir`)             | `lowered`(3); optimized HL is also `lowered` or `optimized`(2) |
| Physical MIR (`lithe_physical_mir_ir`) | `physical`(4), `managed`(5)                                    |

---

## §4 — Common Wire Conventions

These rules apply to all IR families.

### §4.1 Scalar Encoding

All on-wire scalars MUST be fixed-width little-endian integers:

| C++ type        | Wire width | Wire endianness     |
|-----------------|------------|---------------------|
| `std::uint8_t`  | 1 byte     | n/a                 |
| `std::uint16_t` | 2 bytes    | LE                  |
| `std::uint32_t` | 4 bytes    | LE                  |
| `std::uint64_t` | 8 bytes    | LE                  |
| `std::int64_t`  | 8 bytes    | LE two's complement |
| `double`        | 8 bytes    | IEEE 754 LE         |

`size_t`, raw pointers, and host-native types MUST NOT appear on the wire.
The canonical wire endian is `wire_endian::little` (value 0).

### §4.2 Id Spaces

All ids within a single IR document are dense `std::uint32_t` values assigned
in **canonical first-seen structural order** (as produced by `freeze` or a
spec-conformant encoder). Id 0 is valid.

### §4.3 String Tables

Each IR document carries an independent string table (`std::vector<std::string>`).
Strings are referenced by `uint32_t` index. Duplicate strings MUST be
deduplicated by a conformant encoder. The canonical order of strings in the
`canonical_encode` preimage is **lexicographic by content** (insertion order
MUST NOT appear in the output).

### §4.4 Section Ids (STABLE)

Section ids are stable string constants. Changing a section id string is a
major-version breaking change.

Graph IR sections (`lithe_ir/adapters/graph.hpp`):

| Section id                 | Role                     |
|----------------------------|--------------------------|
| `"lithe.ir.graph.nodes"`   | Node table               |
| `"lithe.ir.graph.strings"` | String literal table     |
| `"lithe.ir.graph.roots"`   | Root node id list        |
| `"lithe.ir.graph.meta"`    | Metadata (stage, schema) |

HL MIR sections (`lithe_ir/adapters/hl_mir.hpp`):

| Section id                  | Role                 |
|-----------------------------|----------------------|
| `"lithe.ir.hl_mir.values"`  | SSA value table      |
| `"lithe.ir.hl_mir.ops"`     | Operation table      |
| `"lithe.ir.hl_mir.blocks"`  | Block table          |
| `"lithe.ir.hl_mir.regions"` | Region table         |
| `"lithe.ir.hl_mir.meta"`    | Metadata             |
| `"lithe.ir.hl_mir.strings"` | String literal table |

Physical MIR sections (`lithe_ir/adapters/physical_mir.hpp`):

| Section id               | Role                    |
|--------------------------|-------------------------|
| `"lithe.ir.phys.vregs"`  | Virtual register table  |
| `"lithe.ir.phys.pregs"`  | Physical register table |
| `"lithe.ir.phys.spills"` | Spill slot table        |
| `"lithe.ir.phys.instrs"` | Instruction table       |
| `"lithe.ir.phys.blocks"` | Block table             |
| `"lithe.ir.phys.meta"`   | Metadata                |

### §4.5 Schema Version Semantics

`schema_version{major, minor, patch}`:

- **major** bump: breaking wire change (removed field, changed encoding,
  renamed section id, renumbered stage). A Consumer MUST reject a document
  whose major version it does not support.
- **minor** bump: additive-compatible change (new optional op, new optional
  field a tolerant consumer may skip). A Consumer that supports minor M MUST
  accept documents at minor ≤ M of the same major.
- **patch** bump: editorial clarification; no wire change.

---

## §5 — Type System and Canonical Type Grammar

Source: `include/lithe/lithe_ir/portable/verify.hpp` — `detail::type_str_parseable`,
`detail::scalar_type_parseable`, `detail::memref_inner_parseable`.

### §5.1 EBNF Grammar

```ebnf
type       = scalar | memref ;
scalar     = int_ty | float_ty | opaque_ty ;
int_ty     = "i", digits ;           (* "i1"=bool, "i8","i16","i32","i64", "iN" *)
float_ty   = "f", digits ;           (* "f16","f32","f64", "fN" *)
digits     = digit, { digit } ;
digit      = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" ;
opaque_ty  = "opaque", digits ;      (* e.g. "opaque64" — host-opaque bytes *)
memref     = "memref<", memref_inner, ">" ;
memref_inner = dim, { "x", dim }, "x", scalar ;
dim        = digits | "?" ;          (* "?" = dynamic / unknown at compile time *)
```

### §5.2 Canonical Spelling Rules

- One string per type: no whitespace, no optional brackets variant.
- The grammar is **case-sensitive**: `"i32"` is valid; `"I32"` is not.
- `"i1"` is the canonical spelling for boolean type.
- `memref<…>` requires at least one explicit dim before the element scalar.
  `memref<xi32>` (missing dim) is INVALID.
- strides are NOT part of the canonical type string grammar for the purpose of
  type identity; stride annotations live in `memref_desc` attribute payloads.

### §5.3 Parser Contract

`detail::type_str_parseable(s)` MUST:

- Accept all strings matching the grammar above.
- Reject all strings not matching (including empty string, whitespace, wrong
  case, wrong prefix).
- Be the single authoritative gate used in the `T`-check of `verify_portable`.

The spec-pinning test (`test_lithe_ir_spec_conformance.cpp §3`) validates the
accept/reject tables:

**Must accept**: `"i1"`, `"i8"`, `"i16"`, `"i32"`, `"i64"`, `"f16"`, `"f32"`,
`"f64"`, `"i128"`, `"opaque64"`, `"memref<4xi32>"`, `"memref<2x?xi64>"`.

**Must reject**: `""`, `"int32"`, `"float"`, `"i 32"`, `"MEMREF<4xi32>"`,
`"memref<xi32>"`.

---

## §6 — Graph IR

Source: `include/lithe/lithe_ir/adapters/graph.hpp` — namespace `lithe::ir::adapters`.

### §6.1 Structure

```
lithe_graph_ir {
    nodes    : vector<graph_node>    -- all AST nodes
    strings  : vector<string>        -- string literal table
    roots    : vector<uint32_t>      -- root node ids
    source_stage : stage             -- surface | canonical | optimized
    schema   : schema_version
}
```

### §6.2 `graph_node`

```
graph_node {
    id           : uint32_t          -- stable per-document id
    op_domain    : string            -- e.g. "lithe.core"
    op_name      : string            -- e.g. "add", "constant"
    child_ids    : vector<uint32_t>  -- indices into graph_ir::nodes
    lit_kind     : graph_literal_kind
    lit_i64      : int64_t           -- present iff lit_kind == i64
    lit_f64      : double            -- present iff lit_kind == f64
    lit_bool     : bool              -- present iff lit_kind == bool_
    lit_str_idx  : uint32_t          -- index into strings iff lit_kind == str
}
```

### §6.3 `graph_literal_kind` Enumeration

| Value   | Integer | Meaning                               |
|---------|---------|---------------------------------------|
| `none`  | 0       | no literal                            |
| `i64`   | 1       | 64-bit integer literal                |
| `f64`   | 2       | 64-bit float literal                  |
| `bool_` | 3       | boolean literal                       |
| `str`   | 4       | string literal (index into `strings`) |

### §6.4 `graph_op_identity`

Op identity is the stable `(op_domain, op_name)` pair plus `schema_version`
for upgrade routing. Integer id fields MUST NOT be used as op identity across
schema versions.

### §6.5 Structural Validity Invariant

`structurally_valid()` MUST hold for a conformant Graph IR document:

- Every `child_ids[i]` is a valid index into `nodes`.
- Every `roots[i]` is a valid index into `nodes`.
- Every `lit_str_idx` (when `lit_kind == str`) is a valid index into `strings`.

A Consumer MUST run `structurally_valid()` before walking the node graph.

### §6.6 Stage Membership

`is_graph_stage(s)` is true iff `s ∈ {surface, canonical, optimized}`.

---

## §7 — HL MIR

Sources: `include/lithe/lithe_ir/adapters/hl_mir.hpp` (wire form),
`include/lithe/lithe_codegen_hl.hpp` (live form, NOT a wire contract).

### §7.1 Structure

```
lithe_hl_mir_ir {
    function_name    : string
    values           : vector<hl_wire_value>
    ops              : vector<hl_wire_op>
    blocks           : vector<hl_wire_block>
    regions          : vector<hl_wire_region>
    entry_block_ids  : vector<uint32_t>
    strings          : vector<string>          -- string literal table
    source_stage     : stage                   -- lowered
    schema           : schema_version
}
```

### §7.2 `hl_wire_value`

```
hl_wire_value {
    id       : uint32_t   -- stable SSA value id
    type_str : string     -- canonical type string per §5
}
```

### §7.3 `hl_wire_op`

```
hl_wire_op {
    id           : uint32_t
    domain       : string            -- e.g. "lithe.hl"
    name         : string            -- e.g. "fadd", "structured_for"
    operand_ids  : vector<uint32_t>  -- SSA value ids consumed
    result_ids   : vector<uint32_t>  -- SSA value ids produced
    block_id     : uint32_t          -- enclosing block
    region_id    : uint32_t          -- enclosing region
    op_schema    : schema_version    -- for upgrade routing
    structured_for : optional<for_attr>
    memref         : optional<memref_desc>
}
```

### §7.4 `hl_wire_block` and `hl_wire_region`

```
hl_wire_block  { id: uint32_t; op_ids: vector<uint32_t>; arg_ids: vector<uint32_t> }
hl_wire_region { id: uint32_t; block_ids: vector<uint32_t>; arg_ids: vector<uint32_t> }
```

### §7.5 Opcode Identity Contract

Opcode identity is the stable `(domain, name)` string pair. The live
`hl_opcode` enum integers are **NOT a contract** — they MAY change across
schema minor bumps without a major-version bump. The `(domain, name)` set at a
given schema version IS the contract. The normative opcode set is the table in
§8.

### §7.6 Attribute Payload Layouts

**`for_attr`** (present iff `name == "structured_for"`):

```
for_attr {
    rank         : uint8_t
    is_parallel  : bool
    lower_bounds : vector<int64_t>   -- rank entries
    upper_bounds : vector<int64_t>   -- rank entries
    steps        : vector<int64_t>   -- rank entries
    tile_sizes   : vector<uint32_t>  -- rank entries; 0 = untiled
}
```

Producer note: implementations MAY track additional bounded-loop optimization hints
(for example `bounds_known`, `stride_regular`, `trip_count_hint`) in live IR or
tool-local side tables. Such hints are non-semantic metadata and MUST NOT change
program meaning; consumers MAY ignore them.

**`memref_desc`** (present for load/store ops):

```
memref_desc {
    rank          : uint8_t
    element_kind  : string   -- "i8","i16","i32","i64","f32","f64"
    elem_bits     : uint8_t
    shape         : vector<uint64_t>
    strides       : vector<int64_t>
}
```

**`branch_wire_attr`** (present iff `name == "branch"`; schema 1.1.0):

```
branch_wire_attr {
    target_block_id : uint32_t
}
```

**`branch_cond_wire_attr`** (present iff `name == "branch_cond"`; schema 1.1.0):

```
branch_cond_wire_attr {
    true_block_id  : uint32_t
    false_block_id : uint32_t
}
```

**`compare_wire_attr`** (present iff `name == "icmp"` or `"fcmp"`; schema 1.1.0):

```
compare_wire_attr {
    predicate_idx : uint32_t   -- index into k_icmp_predicates or k_fcmp_predicates (§17.7)
    ordered       : bool       -- true = ordered (NaN-safe false) for fcmp
}
```

**`guard_wire_attr`** (present iff `name == "guard"`; schema 1.3.0):

```
guard_wire_attr {
    guard_kind_idx   : uint32_t   -- index into k_guard_kinds (§17.7)
    policy_idx       : uint32_t   -- index into k_failure_policies (§17.7)
    diag_code_idx    : uint32_t   -- string table index; 0 = none
    source_span_idx  : uint32_t   -- string table index; 0 = none
}
```

**`trap_wire_attr`** (present iff `name == "trap"`; schema 1.3.0):

```
trap_wire_attr {
    trap_kind_idx  : uint32_t   -- index into k_trap_kinds (§17.7)
    diag_code_idx  : uint32_t   -- string table index; 0 = none
}
```

**`cleanup_wire_attr`** (present iff `name == "cleanup_region"`; schema 1.4.0):

```
cleanup_wire_attr {
    cleanup_ids : vector<uint32_t>   -- region ids of associated cleanup regions
}
```

**`tx_wire_attr`** (present iff `name == "tx.region"`; schema 1.5.0):

```
tx_wire_attr {
    isolation_idx    : uint32_t    -- index into k_tx_isolation_levels (§17.7)
    retry            : uint16_t    -- max retry count; 0 = no limit
    replay_idx       : uint32_t    -- "none" | "on_conflict"
    conflict_idx     : uint32_t    -- "abort" | "retry"
    partial_idx      : uint32_t    -- "disallow" | "allow"
    durability_idx   : uint32_t    -- "volatile_" | "durable" | "best_effort"
    distribution_idx : uint32_t    -- string table index; 0 = local
    coordinator_idx  : uint32_t    -- string table index; 0 = none
}
```

### §7.7 Region Nesting Well-Formedness

A well-formed HL MIR document MUST satisfy (checked by `R`-check in §13):

- Region graph is acyclic.
- Each block belongs to at most one region.
- Region arguments match the parent op's contract (checked by `T`-check).

### §7.8 Stage Membership

`is_hl_mir_stage(s)` is true iff `s == lowered`.

---

## §8 — Opcode Signature Registry

Source: `include/lithe/lithe_ir/portable/verify.hpp` —
`k_opcode_signatures` (48-entry `constexpr` array).

The registry is the **single source of truth** shared by the verifier (T/E/K
checks) and the optimizer legality tables.

### §8.1 Signature Entry Fields

| Field           | Type                      | Meaning                                       |
|-----------------|---------------------------|-----------------------------------------------|
| `domain`        | `string_view`             | Op domain (stable string)                     |
| `name`          | `string_view`             | Op name (stable string)                       |
| `arity_min`     | `uint8_t`                 | Minimum operand count                         |
| `arity_max`     | `uint8_t`                 | Maximum operand count (255 = variadic)        |
| `result_count`  | `uint8_t`                 | Number of SSA results produced                |
| `is_terminator` | `bool`                    | True iff op terminates its block              |
| `reads_memory`  | `bool`                    | True iff op may read memory                   |
| `writes_memory` | `bool`                    | True iff op may write memory                  |
| `may_trap`      | `bool`                    | True iff op may trigger a trap (schema 1.3.0) |
| `required_cap`  | `portable_capability_bit` | Declared capability required (0 = none)       |

### §8.2 Normative Opcode Table (schema 1.0.0–1.5.0)

Column `may_trap` added in schema 1.3.0; false for all pre-1.3.0 ops.

| domain                                 | name                | arity_min | arity_max | results | terminator | reads_mem | writes_mem | may_trap | cap              |
|----------------------------------------|---------------------|-----------|-----------|---------|------------|-----------|------------|----------|------------------|
| `lithe.hl`                             | `structured_for`    | 0         | 255       | 0       | false      | true      | true       | false    | —                |
| `lithe.hl`                             | `structured_reduce` | 0         | 255       | 0       | false      | true      | true       | false    | —                |
| `lithe.hl`                             | `region_yield`      | 0         | 255       | 0       | **true**   | false     | false      | false    | —                |
| `lithe.hl`                             | `loop_index`        | 0         | 0         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `memref_load`       | 1         | 255       | 1       | false      | true      | false      | false    | —                |
| `lithe.hl`                             | `memref_store`      | 2         | 255       | 0       | false      | false     | true       | false    | —                |
| `lithe.hl`                             | `fadd`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `fsub`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `fmul`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `fdiv`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `fneg`              | 1         | 1         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `add`               | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `sub`               | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `mul`               | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `div`               | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `exp`               | 1         | 1         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `log`               | 1         | 1         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `sqrt`              | 1         | 1         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `abs`               | 1         | 1         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `call`              | 0         | 255       | 0       | false      | false     | false      | false    | `external_calls` |
| `lithe.hl`                             | `constant`          | 0         | 0         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `argument`          | 0         | 0         | 1       | false      | false     | false      | false    | —                |
| — schema 1.1.0: CFG + compare/select — |                     |           |           |         |            |           |            |          |                  |
| `lithe.hl`                             | `branch`            | 0         | 0         | 0       | **true**   | false     | false      | false    | —                |
| `lithe.hl`                             | `branch_cond`       | 1         | 1         | 0       | **true**   | false     | false      | false    | —                |
| `lithe.hl`                             | `return`            | 0         | 255       | 0       | **true**   | false     | false      | false    | —                |
| `lithe.hl`                             | `icmp`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `fcmp`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `select`            | 3         | 3         | 1       | false      | false     | false      | false    | —                |
| — schema 1.2.0: integer ops —          |                     |           |           |         |            |           |            |          |                  |
| `lithe.hl`                             | `sdiv`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `udiv`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `srem`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `urem`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `bit_and`           | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `bit_or`            | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `bit_xor`           | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `bit_not`           | 1         | 1         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `shl`               | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `lshr`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| `lithe.hl`                             | `ashr`              | 2         | 2         | 1       | false      | false     | false      | false    | —                |
| — schema 1.3.0: safety ops —           |                     |           |           |         |            |           |            |          |                  |
| `lithe.hl`                             | `guard`             | 1         | 1         | 0       | false      | false     | false      | **true** | —                |
| `lithe.hl`                             | `trap`              | 0         | 255       | 0       | **true**   | false     | false      | **true** | —                |
| — schema 1.4.0: cleanup/defer —        |                     |           |           |         |            |           |            |          |                  |
| `lithe.hl`                             | `cleanup_region`    | 0         | 255       | 0       | false      | false     | false      | false    | `defer_scopes`   |
| `lithe.hl`                             | `cleanup_yield`     | 0         | 255       | 0       | **true**   | false     | false      | false    | `defer_scopes`   |
| — schema 1.5.0: transactions —         |                     |           |           |         |            |           |            |          |                  |
| `lithe.hl`                             | `tx.region`         | 0         | 255       | 1       | false      | true      | true       | false    | `transactions`   |
| `lithe.hl`                             | `tx.read`           | 2         | 2         | 1       | false      | true      | false      | false    | `transactions`   |
| `lithe.hl`                             | `tx.write`          | 3         | 3         | 0       | false      | false     | true       | false    | `transactions`   |
| `lithe.hl`                             | `tx.abort`          | 0         | 1         | 0       | **true**   | false     | false      | **true** | `transactions`   |
| `lithe.hl`                             | `tx.yield`          | 0         | 255       | 0       | **true**   | false     | false      | false    | `transactions`   |

### §8.3 Producer/Consumer Obligations

- A Producer MUST emit ops whose `(arity, result_count)` satisfies their table
  entry.
- A Consumer MUST reject ops violating their table entry.
- Unknown ops MAY be tolerated when `verify_policy::allow_unknown_optional_ops
  == true`; they MUST be rejected otherwise.
- The spec table and the code table MUST agree bidirectionally; divergence fails
  `test_lithe_ir_spec_conformance.cpp §2`.

---

## §9 — Physical MIR

Source: `include/lithe/lithe_ir/adapters/physical_mir.hpp` — namespace
`lithe::ir::adapters`.

### §9.1 `wire_value_kind` Enumeration

| Value       | Integer | Meaning                         |
|-------------|---------|---------------------------------|
| `unknown`   | 0       | —                               |
| `integer`   | 1       | integral scalar                 |
| `floating`  | 2       | floating-point scalar           |
| `pointer`   | 3       | unmanaged pointer               |
| `aggregate` | 4       | struct/array aggregate          |
| `managed`   | 5       | GC-managed pointer (`lithe_rt`) |

### §9.2 Wire-Form Types

```
wire_vreg        { id: uint32_t; kind: wire_value_kind; bit_width: uint8_t }
wire_preg        { id: uint16_t; name: string }   -- name is INFORMATIONAL ONLY
wire_spill_slot  { id: uint32_t; size_bytes: uint32_t; align_log2: uint8_t; frame_offset: int32_t }
```

`wire_preg::name` (e.g. `"x0"`, `"rax"`) is informational only. It MUST NOT
be used as ABI identity; it is not stable across schema versions.

### §9.3 `wire_operand` Kind Union

```
wire_operand::kind_tag: vreg(0) | preg(1) | imm_i64(2) | imm_f64(3) | spill_slot(4) | memory(5)
wire_mem_kind:          stack_frame(0) | direct(1) | offset(2) | indirect(3) | rip_rel(4)
```

### §9.4 `lithe_physical_mir_ir` Structure

```
lithe_physical_mir_ir {
    function_name        : string
    vregs                : vector<wire_vreg>
    pregs                : vector<wire_preg>
    spills               : vector<wire_spill_slot>
    instrs               : vector<wire_instr>
    blocks               : vector<wire_block>
    entry_block_id       : uint32_t
    calling_convention   : string    -- "c", "fast", "cold" (stable string, not enum)
    param_vregs          : vector<wire_vreg>
    return_vreg          : optional<wire_vreg>
    source_stage         : stage     -- physical | managed
    schema               : schema_version
}
```

### §9.5 Managed Stage

`managed`(5) extends `physical`(4) with GC frame annotations provided by
`lithe_rt`. The wire form is the same struct; the stage tag distinguishes them.

### §9.6 Stage Membership

`is_physical_stage(s)` is true iff `s ∈ {physical, managed}`.

---

## §10 — Portable Module Container

Source: `include/lithe/lithe_ir/portable/module.hpp` — namespace `lithe::ir::portable`.

### §10.1 `portable_module` Field List

```
portable_module {
    functions             : vector<lithe_hl_mir_ir>
    constants             : portable_constant_pool
    globals               : vector<portable_global>
    imports               : vector<portable_import>
    exports               : vector<portable_export>
    declared_capabilities : capability_set
    manifest              : portable_manifest
    schema                : schema_version
}
```

### §10.2 Sub-Types

```
portable_import {
    module         : string   -- importing module name
    symbol         : string   -- symbol name
    signature_str  : string   -- stable type string of callee
    abi            : schema_version
    required       : bool
}

portable_export {
    symbol          : string
    function_index  : uint32_t   -- index into functions
    signature_str   : string
}

portable_global {
    name        : string
    type_str    : string      -- canonical type string per §5
    const_index : uint32_t    -- index into constants
    mutable_    : bool
}

portable_constant_pool {
    types : vector<string>             -- parallel to data
    data  : vector<vector<uint8_t>>    -- canonical LE bytes
}

portable_manifest {
    producer          : string
    producer_version  : schema_version
    source_language   : string
    semantic_digest   : array<uint8_t, 64>   -- §11 semantic digest
    digest_len        : uint8_t              -- 0 = not computed
}
```

### §10.3 `capability_set` Bits

```
portable_capability_bit (uint32_t bitmask):
    exceptions       = 1 << 0
    transactions     = 1 << 1
    defer_scopes     = 1 << 2
    atomics          = 1 << 3
    simd_hint        = 1 << 4
    gpu_hint         = 1 << 5
    reflection       = 1 << 6
    external_calls   = 1 << 7
```

### §10.4 `structurally_complete()` Invariants

`portable_module::structurally_complete()` MUST hold for a conformant module:

- `functions` is non-empty.
- Every `export.function_index` is a valid index into `functions`.
- Every `global.const_index` is a valid index into `constants`.
- `constants.types.size() == constants.data.size()`.

### §10.5 ABI Ownership

Lithe does NOT synthesize ABI. Imports, exports, globals, and constants are
**host-supplied** at module construction time. A Producer MUST populate these
fields; a Consumer reads them as-provided and MUST NOT infer ABI from them.

---

## §11 — Canonical Encoding and Semantic Digest

Source: `include/lithe/lithe_ir/portable/digest.hpp` — `canonical_encode`,
`semantic_digest`.

### §11.1 Canonical Encoding Rules

`canonical_encode(module)` produces a deterministic byte sequence (the digest
preimage) by applying the following rules in order:

1. **Fixed section order**: module header, manifest, imports, exports, globals,
   constants, per-function data. Section order is fixed; a Consumer MUST NOT
   assume sections appear in a different order.
2. **Values by canonical id**: SSA values emitted in ascending `id` order.
3. **Ops by structural order**: ops emitted in op-table order, which equals
   canonical dense assignment order (as produced by `freeze`).
4. **String table content-sorted**: all strings are interned during the pass,
   then sorted lexicographically by content before `finalize_string_table()`.
   String references in the output are remapped indices into this sorted table.
   **No unordered-container iteration** reaches the output byte sequence.
5. **Fixed LE widths**: all scalars are fixed-width LE as per §4.1. No padding
   entropy.

### §11.2 Determinism Guarantee

Identical logical `portable_module` (same structure and values, regardless of
construction or allocation order) MUST produce:

- Identical `canonical_encode` bytes.
- Identical `semantic_digest`.

This guarantee holds across processes and platforms that use the same schema
version.

### §11.3 Semantic vs Payload Digest

| Digest                                           | Definition                             | Stability                                                                   | Purpose                                      |
|--------------------------------------------------|----------------------------------------|-----------------------------------------------------------------------------|----------------------------------------------|
| **Semantic** (`semantic_digest`)                 | `digest_alg(canonical_encode(module))` | Stable: same program → same digest across re-encodings                      | Program identity / content-addressed caching |
| **Payload** (`binary_ir_envelope::digest_bytes`) | `digest_alg(wire_bytes)`               | Changes whenever wire representation changes (re-compression, schema patch) | Wire artifact integrity                      |

These two digests are **intentionally distinct**. A Consumer MUST NOT confuse
them. Equality of semantic digests means the programs are semantically
identical; equality of payload digests means the wire bytes are identical.

### §11.4 `semantic_digest` Definition

```
semantic_digest(module, alg) =
    sha256(canonical_encode(module))   -- when alg == sha256
    zero-64                            -- when alg == none (testing only)
```

Default algorithm: `digest_algorithm::sha256` (id=1, digest_size=32 bytes).

---

## §12 — Wire Binary and Text Encodings

Sources: `include/lithe/lithe_ir/security_envelope.hpp`,
`include/lithe/lithe_ir/providers/binary_provider.hpp`,
`include/lithe/lithe_ir/providers/text_provider.hpp`.

### §12.1 Envelope Magic (STABLE)

Every binary Lithe IR document begins with the 4-byte magic:

```
k_binary_ir_magic = { 0x4C, 0x54, 0x49, 0x52 }   // "LTIR"
```

A Consumer MUST reject any document whose first four bytes do not equal this
magic before any further processing.

### §12.2 `binary_ir_envelope` Structure

The envelope header (all fields fixed-width, no `size_t`, no host-native types,
all LE):

```
binary_ir_envelope {
    magic[4]              -- "LTIR" (0x4C 0x54 0x49 0x52)
    format_major : u8     -- 1 (current; mismatch → reject)
    format_minor : u8     -- descriptor-driven compat
    wire_endian_tag : u8  -- wire_endian enum value (0=LE)
    target_address_width : u8  -- MUST be != 0

    ir_stage_tag : u8     -- stage enum value
    ir_kind_tag : u8      -- ir_kind enum value
    dialect_len : u8      -- length of dialect string
    _pad : u8
    schema_major : u16
    schema_minor : u16
    schema_patch : u16
    dialect_bytes[65]     -- stable_ir_id (null-terminated)

    payload_size : u64
    maximum_decoded_size : u64
    decompression_limit : u64

    compression_alg : u8
    _pad0[7]

    digest_alg : u8       -- digest_algorithm enum value
    digest_len : u8
    _pad1[6]
    digest_bytes[64]

    sig_alg : u8          -- signature_algorithm enum value
    sig_len : u8
    sig_key_id_len : u8
    _pad2[5]
    sig_key_id[32]
    sig_bytes[128]

    required_features : u64

    section_count : u32
    section_dir_offset : u32
}
```

### §12.3 Digest Algorithm Ids (STABLE)

| Algorithm  | Id | Digest size (bytes) |
|------------|----|---------------------|
| `none`     | 0  | 0                   |
| `sha256`   | 1  | 32                  |
| `sha3_256` | 2  | 32                  |
| `blake3`   | 3  | 32                  |

### §12.4 Signature Algorithm Ids

| Algorithm     | Id | Signature size (bytes) |
|---------------|----|------------------------|
| `none`        | 0  | 0                      |
| `ed25519`     | 1  | 64                     |
| `hmac_sha256` | 2  | 32                     |

### §12.5 Mandatory Decode Validation Ordering

A Consumer MUST validate in this exact order:

1. **Structural limits** — magic, `format_major`, `target_address_width != 0`,
   `payload_size ≤ max_payload_size`, `maximum_decoded_size ≤ max_decoded_size`,
   `section_count ≤ max_section_count`, section directory bounds. Done BEFORE
   any large allocation.
2. **Integrity** — payload digest verification (`digest_alg` + `digest_bytes`).
   Done BEFORE any decode.
3. **Authenticity** — signature verification (`sig_alg` + `sig_bytes`). Done
   BEFORE the IR is trusted or compiled.
4. **Decode** — parse section data into IR structure.
5. **Compatibility** — run `verify_portable` + compatibility predicate (§14).

Skipping or reordering steps 1–3 violates this spec.

### §12.6 Section Directory

```
section_entry {
    name_bytes[64]    -- section name, null-terminated
    name_len : u32
    is_required : u8  -- 1 = required, 0 = optional
    _pad[3]
    data_offset : u64 -- byte offset from payload start
    data_size : u64
}
```

### §12.7 Text IR

The canonical text form is the deterministic, round-trippable rendering
produced by `text_provider`. Round-tripping MUST preserve `semantic_digest`.

`human_pretty` (from `ir_inspector`) is explicitly **non-normative**: it MUST
NOT be fed to a decoder. Decoders MUST reject any text document whose header
does not conform to the `text_provider` format.

### §12.8 `envelope_limits` Defaults

| Limit               | Default   |
|---------------------|-----------|
| `max_section_count` | 256       |
| `max_nesting`       | 64        |
| `max_block_count`   | 100,000   |
| `max_value_count`   | 1,000,000 |
| `max_op_count`      | 1,000,000 |
| `max_decoded_size`  | 512 MiB   |
| `max_payload_size`  | 256 MiB   |

All limits are configurable via `envelope_limits`; no limit is hardcoded.

---

## §13 — Verification Rules

Source: `include/lithe/lithe_ir/portable/verify.hpp` — `verify_portable`.

A `portable_module` is **valid** if and only if all seven check groups pass.

### §13.1 The Seven Checks

| Check | Name         | Rule                                                                                                                                                                                        |
|-------|--------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **T** | Types        | Every `value.type_str` passes `type_str_parseable`; every known op's `(arity, result_count)` satisfies its signature table entry; `branch_cond` condition operand MUST be type `i1` (T003). |
| **C** | CFG          | Every non-empty block ends in exactly one terminator op (C001); every branch target block id exists (C002); no op may follow a terminator in the same block (C003).                         |
| **S** | SSA          | Every SSA value is defined exactly once (block args + op results); every use is dominated by its definition.                                                                                |
| **Y** | Symbols      | Every `export.function_index` is in range; no duplicate export symbols; every `import.module` and `import.symbol` is non-empty.                                                             |
| **E** | Effects      | Effectful ops (`reads_memory                                                                                                                                                                || writes_memory`) MUST NOT appear in regions annotated pure (E001); `tx.read`, `tx.write`, `tx.abort`, `tx.yield` MUST appear inside a `tx.region` body region (E002); `cleanup_yield` MUST appear inside a `cleanup_region` body region (E003). |
| **R** | Regions      | Region graph is acyclic; no block belongs to more than one region; `tx.region` body regions MUST NOT contain `cleanup_region` ops or vice versa (R003).                                     |
| **K** | Capabilities | Every op whose `required_cap != 0` MUST be covered by `declared_capabilities`.                                                                                                              |

### §13.2 Stable Diagnostic Codes

Each failure emits a `lithe::diag::diagnostic` with a stable `code`:

| Check | Code              | Condition                                                                                   |
|-------|-------------------|---------------------------------------------------------------------------------------------|
| T     | `LITHE-PORT-T001` | value type string unparseable                                                               |
| T     | `LITHE-PORT-T002` | op arity mismatch                                                                           |
| T     | `LITHE-PORT-T003` | `branch_cond` condition operand type is not `i1` (schema 1.1.0)                             |
| C     | `LITHE-PORT-C001` | block missing terminator                                                                    |
| C     | `LITHE-PORT-C002` | branch target missing                                                                       |
| C     | `LITHE-PORT-C003` | op follows terminator in same block (schema 1.1.0)                                          |
| S     | `LITHE-PORT-S001` | value defined more than once                                                                |
| S     | `LITHE-PORT-S002` | use not dominated by definition                                                             |
| Y     | `LITHE-PORT-Y001` | import unresolved (empty module/symbol)                                                     |
| Y     | `LITHE-PORT-Y002` | export function_index out of range                                                          |
| Y     | `LITHE-PORT-Y003` | duplicate export symbol                                                                     |
| E     | `LITHE-PORT-E001` | effectful op in pure region                                                                 |
| E     | `LITHE-PORT-E002` | `tx.read`/`tx.write`/`tx.abort`/`tx.yield` outside `tx.region` (schema 1.5.0)               |
| E     | `LITHE-PORT-E003` | `cleanup_yield` outside `cleanup_region` (schema 1.4.0)                                     |
| R     | `LITHE-PORT-R001` | region cycle detected                                                                       |
| R     | `LITHE-PORT-R002` | block belongs to multiple regions                                                           |
| R     | `LITHE-PORT-R003` | region-kind mismatch (tx region contains cleanup_region or vice versa) (schema 1.4.0/1.5.0) |
| K     | `LITHE-PORT-K001` | required capability not declared                                                            |
| L     | `LITHE-PORT-L001` | count exceeds configured limit                                                              |

Diagnostic codes are **STABLE** — they MUST NOT be changed or removed within a
major schema version.

### §13.3 Trust Boundary Rule

A Consumer MUST run `verify_portable` at the trust boundary before using any
IR. Trusting unverified IR is a security defect.

---

## §14 — Versioning, Compatibility, and Upgrade

Source: `include/lithe/lithe_ir/upgrade.hpp`, `include/lithe/lithe_ir/portable/module.hpp`.

### §14.1 Schema Version Semantics

See §4.5 for the major/minor/patch rules.

### §14.2 Compatibility Predicate

A module is compatible with a consumer iff ALL of the following hold
(conjunctive — never optimistic):

1. **Schema supported**: consumer declares support for the module's `schema_version.major`.
2. **ABI compatible**: import `abi` versions are supported by the runtime.
3. **Capabilities available**: all bits in `declared_capabilities` are available in the target.
4. **Target restrictions satisfied**: `target_address_width` matches or is compatible with the consumer's target.
5. **External symbols resolve**: all `required == true` imports resolve in the consumer's symbol table.
6. **Security policy permits**: envelope signature + digest policy checks pass.

Failure of any one condition MUST produce a rejection with a diagnostic. No
partial acceptance.

### §14.3 Upgrade

An upgrader maps IR at schema `{M, m, p}` to schema `{M, m+1, 0}` or higher
within the same major. Every upgrader MUST be:

- **Versioned**: carries the source and target `schema_version`.
- **Tested**: covered by a unit test asserting round-trip + semantic equivalence.
- **Recorded in provenance**: the upgrader id and versions are appended to the
  module's provenance chain (impl-3).

An op with no registered upgrade path MUST be rejected when schema promotion is
required; it MUST NOT be silently dropped.

### §14.4 What a Minor Bump MAY Add

- New optional ops (unknown by tolerant consumers with
  `allow_unknown_optional_ops == true`).
- New optional fields (ignored by older consumers).

### §14.5 What Requires a Major Bump

- Changed opcode semantics.
- Removed required op or field.
- Changed canonical encoding rules (§11).
- Renumbered stage integer values (§3).
- Renamed section ids (§4.4).

### §14.6 Language-Control Extension — Staged Schema Minors

The language-control extension was introduced across five consecutive minor
bumps. Each minor is strictly additive (§14.4):

| Schema | New ops                                                                                          | New diagnostic codes                                                    | Notes                                                    |
|--------|--------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------|----------------------------------------------------------|
| 1.0.0  | 22 compute ops (baseline)                                                                        | T001/T002, C001/C002, S001/S002, Y001–Y003, E001, R001/R002, K001, L001 | original kernel IR                                       |
| 1.1.0  | `branch`, `branch_cond`, `return`, `icmp`, `fcmp`, `select`                                      | T003, C003                                                              | CFG + compare/select                                     |
| 1.2.0  | `sdiv`, `udiv`, `srem`, `urem`, `bit_and`, `bit_or`, `bit_xor`, `bit_not`, `shl`, `lshr`, `ashr` | —                                                                       | integer arithmetic                                       |
| 1.3.0  | `guard`, `trap`                                                                                  | — (`may_trap` field added to signature)                                 | safety ops                                               |
| 1.4.0  | `cleanup_region`, `cleanup_yield`                                                                | E003, R003                                                              | defer/cleanup scopes; requires `defer_scopes` capability |
| 1.5.0  | `tx.region`, `tx.read`, `tx.write`, `tx.abort`, `tx.yield`                                       | E002                                                                    | transactions; requires `transactions` capability         |

A consumer that supports schema `{1, N, *}` MUST tolerate all ops from minors ≤ N
and MAY reject ops from minors > N when `allow_unknown_optional_ops == false`.

---

## §15 — Observation API Reference

Source: `include/lithe/lithe_ir/inspect/inspect.hpp` (umbrella),
`lithe_ir/inspect/inspector.hpp`, `lithe_ir/inspect/view.hpp`.

### §15.1 Sanctioned Read Surface

`lithe::ir::inspect::ir_inspector` and `ir_view` are the **sanctioned, supported
read surface** for observing Lithe IR. Tools SHOULD use the facade.

Direct adapter walking (`lithe_graph_ir::nodes`, `lithe_hl_mir_ir::ops`, etc.)
is permitted but the facade is the **compatibility-stable path**: the inspector
API is versioned and will receive compatibility shims across schema minor bumps;
direct adapter structs may gain new fields that change access patterns.

### §15.2 Observation = Verification = Digest

What `ir_inspector` observes is what `verify_portable` verifies and what
`canonical_encode` encodes. There is no divergent "pretty" representation on
the normative path.  `human_pretty` (inspector method) is non-normative and
MUST NOT be decoded or round-tripped.

---

## §16 — Stability Guarantees and Non-Guarantees

External designers MUST consult this section first.

### §16.1 Stable (within a major schema version)

- Stage integer values (`surface=0` … `managed=5`).
- `(domain, name)` opcode set + per-opcode signature fields.
- Canonical type grammar terminals and EBNF rules.
- Canonical encoding rules (section order, id ordering, string sorting, LE widths).
- Section id strings for all three IR families.
- Envelope magic bytes (`"LTIR"`, `{0x4C, 0x54, 0x49, 0x52}`).
- Digest and signature algorithm ids and sizes.
- `semantic_digest` definition (`sha256(canonical_encode(m))`).
- Verifier check set (T/C/S/Y/E/R/K) and stable diagnostic code strings.
- `portable_module` field names and wire types.
- `capability_set` bit assignments.
- Frontend stable string tables: `k_icmp_predicates`, `k_fcmp_predicates`, `k_guard_kinds`, `k_trap_kinds`,
  `k_failure_policies`, `k_tx_isolation_levels` (§17.7).

### §16.2 Explicitly NOT Stable (not a contract)

- **Live `hl_opcode` enum integer values** — MAY change without schema bump.
- **Live arena layout** (`hl_mir_function`, `hl_region`, `hl_block`, etc.) — in-process only.
- **`human_pretty` text format** — non-normative; never decode.
- **Physical register names** (`wire_preg::name`) — informational only.
- **In-process `ir_kind` enum hint integers** — fast-path hint, not wire-stable.
- **Pass timing and pass ordering** — implementation detail.
- **`envelope_limits` default values** — configurable per deployment.

---

## §17 — Frontend Lowering Contract

Source: `include/lithe/lithe_ir/frontend/lowering_contract.hpp` — namespace
`lithe::ir::frontend`.

This section documents the authoritative contract between language frontends
(Crank, Sutra, future languages) and the Lithe IR boundary. Every frontend
MUST lower through these APIs. Informal per-frontend type rules are prohibited.

### §17.1 Purpose and Scope

The frontend lowering contract:

1. Provides the single source of truth for source-type → IR-type mapping.
2. Prevents frontends from inventing divergent lowering rules.
3. Validates that derived type strings are §5-conformant before they enter IR.
4. Maps source-level features to the `portable_capability_bit` that a module
   MUST declare when those features are used.

### §17.2 Scalar Type Mapping (STABLE)

`crank_type_to_ir_str(name)` maps source type names to canonical §5 type strings.

| Source type       | IR type string | Notes                        |
|-------------------|----------------|------------------------------|
| `Bool`            | `"i1"`         | canonical boolean            |
| `Int8` / `i8`     | `"i8"`         |                              |
| `Int16` / `i16`   | `"i16"`        |                              |
| `Int32` / `i32`   | `"i32"`        |                              |
| `Int64` / `i64`   | `"i64"`        |                              |
| `UInt8` / `u8`    | `"i8"`         | unsigned shares IR bit-width |
| `UInt16` / `u16`  | `"i16"`        |                              |
| `UInt32` / `u32`  | `"i32"`        |                              |
| `UInt64` / `u64`  | `"i64"`        |                              |
| `Float32` / `f32` | `"f32"`        |                              |
| `Float64` / `f64` | `"f64"`        |                              |

Unknown names return `std::nullopt`. A Producer MUST call `lower_scalar_type`
(which chains §5 validation) rather than `crank_type_to_ir_str` directly.

### §17.3 Tensor / memref Mapping (STABLE)

`tensor_type_to_ir_str(elem, rank, dims)` derives a `memref<…>` type string:

- Each dim `>= 0` renders as the static integer.
- Each dim `== -1` renders as `?` (dynamic).
- Result: `"memref<d0x…xdN-1x<elem_ir>>"`.

A Producer MUST call `lower_tensor_type` which chains §5 validation.

### §17.4 Capability Mapping (STABLE)

`crank_capability_required(crank_feature)` maps source features to capability bits
(§10.3 of this spec).

| `crank_feature` | `portable_capability_bit` |
|-----------------|---------------------------|
| `transaction`   | `transactions`            |
| `host_call`     | `external_calls`          |
| `defer_scope`   | `defer_scopes`            |
| `atomic`        | `atomics`                 |
| `simd`          | `simd_hint`               |
| `gpu`           | `gpu_hint`                |
| `reflection`    | `reflection`              |
| `exception`     | `exceptions`              |
| `none`          | `0` (no capability)       |

### §17.5 Validation

`validate_ir_type_str(s)` is a lightweight §5 gate callable without the full
verifier. It MUST accept all strings matching the §5 grammar and reject all
others. It is used internally by `lower_scalar_type` and `lower_tensor_type`.

### §17.6 Conformance Obligation

A conformant frontend Producer:

- MUST resolve all source scalar types through `lower_scalar_type`.
- MUST resolve all tensor element types through `lower_tensor_type`.
- MUST declare every `portable_capability_bit` returned by
  `crank_capability_required` for each used feature in the module's
  `declared_capabilities`.
- MUST NOT emit an IR type string that was not validated through
  `validate_ir_type_str` (or the higher-level `lower_*` helpers).
- MUST use the stable string tables (§17.7) for all predicate, guard, trap,
  policy, and isolation index fields in wire attr payloads.

### §17.7 Stable String Tables (STABLE)

These tables are the single source of truth for string-indexed attr fields.
Every frontend MUST use these canonical strings; index positions are stable
within a major schema version.

**Integer compare predicates** (`k_icmp_predicates` — 10 entries):

| Index | String | Meaning                   |
|-------|--------|---------------------------|
| 0     | `eq`   | equal                     |
| 1     | `ne`   | not equal                 |
| 2     | `slt`  | signed less-than          |
| 3     | `sle`  | signed less-or-equal      |
| 4     | `sgt`  | signed greater-than       |
| 5     | `sge`  | signed greater-or-equal   |
| 6     | `ult`  | unsigned less-than        |
| 7     | `ule`  | unsigned less-or-equal    |
| 8     | `ugt`  | unsigned greater-than     |
| 9     | `uge`  | unsigned greater-or-equal |

**Float compare predicates** (`k_fcmp_predicates` — 6 entries; all ordered/NaN-safe-false):

| Index | String | Meaning                  |
|-------|--------|--------------------------|
| 0     | `oeq`  | ordered equal            |
| 1     | `one`  | ordered not-equal        |
| 2     | `olt`  | ordered less-than        |
| 3     | `ole`  | ordered less-or-equal    |
| 4     | `ogt`  | ordered greater-than     |
| 5     | `oge`  | ordered greater-or-equal |

**Guard kinds** (`k_guard_kinds` — 7 entries):

| Index | String            | Condition guarded                              |
|-------|-------------------|------------------------------------------------|
| 0     | `bounds`          | array/slice bounds check                       |
| 1     | `div_by_zero`     | integer division denominator check             |
| 2     | `range_cast`      | narrowing integer cast range check             |
| 3     | `assert`          | user-written assertion                         |
| 4     | `overflow`        | checked arithmetic overflow                    |
| 5     | `transaction`     | transaction precondition (resource accessible) |
| 6     | `parallel_safety` | data-race safety assertion                     |

**Trap kinds** (`k_trap_kinds` — 8 entries):

| Index | String             | Trigger                                   |
|-------|--------------------|-------------------------------------------|
| 0     | `bounds_violation` | bounds guard failed                       |
| 1     | `div_by_zero`      | division-by-zero guard failed             |
| 2     | `range_conversion` | narrowing cast out of range               |
| 3     | `assert_failed`    | user assertion failed                     |
| 4     | `overflow_checked` | checked arithmetic overflowed             |
| 5     | `tx_failed`        | transaction abort / precondition violated |
| 6     | `unreachable`      | code marked unreachable was reached       |
| 7     | `host_trap`        | host-provided trap handler                |

**Failure policies** (`k_failure_policies` — 4 entries):

| Index | String          | Behavior                                         |
|-------|-----------------|--------------------------------------------------|
| 0     | `return_result` | return an error/null result on guard failure     |
| 1     | `trap`          | lower guard failure to a trap terminator         |
| 2     | `terminate`     | lower to process-termination (abort/unreachable) |
| 3     | `host_handler`  | delegate to a registered host failure handler    |

**Transaction isolation levels** (`k_tx_isolation_levels` — 3 entries):

| Index | String            |
|-------|-------------------|
| 0     | `read_committed`  |
| 1     | `repeatable_read` |
| 2     | `serializable`    |

---

*End of Lithe IR Normative Specification — spec_version 1.0.0*
