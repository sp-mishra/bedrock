# Taranga — WebAssembly Compiler Front-End

**Taranga** (Sanskrit: *wave*) is the WebAssembly compiler front-end for the Bedrock stack. It parses both WAT text
(`.wat`) and binary `.wasm`, validates, builds SSA, lowers to Lithe HL MIR, and executes through the Lithe engine.

---

## Six-Band Pipeline

```
Input (.wat / .wasm)
        │
        ▼
┌─────────────────────┐
│  Band 1: Frontend   │  parse()  →  build_result  (dual AST)
│  frontend.hpp       │
│  parser_wat.hpp     │
│  decoder_bin.hpp    │
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  Band 2: View       │  module_view::build()  →  typed section tables
│  module_view.hpp    │
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  Band 3: Validate   │  validate()  →  validated_module  (capability token)
│  validate.hpp       │
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  Band 4: SSA Build  │  build_ssa()  →  ssa_module
│  ssa_build.hpp      │
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  Band 5: Lowering   │  lower_to_hl()  →  hl_result  (portable_module)
│  lower_hl.hpp       │    4-phase: A=control, B=numeric, C=safety, D=memory
│  memory.hpp         │    → freeze_module → verify_portable
│  runtime_prelude.hpp│
└────────┬────────────┘
         │
         ▼
┌─────────────────────┐
│  Band 6: Engine     │  engine::create() / invoke()
│  engine.hpp         │
└─────────────────────┘
```

---

## Quick Start

```cpp
#include "languages/taranga/taranga.hpp"

// Parse WAT source
auto pr = taranga::parse(R"wat(
(module
  (type (func (param i32) (result i32)))
  (func (type 0) local.get 0)
  (export "identity" (func 0))
)
)wat");

if (!pr.ok) { /* handle diagnostics */ }

// Build typed section tables
auto mv = taranga::module_view::build(pr.ir_mod);

// Validate (mints capability token)
auto vr = taranga::validate(mv, pr);
if (!vr.has_value()) { /* handle errors */ }

// Build SSA
auto sm = taranga::build_ssa(*vr, pr);

// Lower to HL MIR
auto hl = taranga::lower_to_hl(sm, *vr);
// hl.portable is the frozen portable_module (passed to Lithe engine)
// hl.hl_text is a human-readable summary

// Create interpreter engine
auto eng = taranga::engine::create(*vr, pr);
if (eng.has_value()) {
    auto r = eng->invoke("identity", {{taranga::wasm_value_type::i32, {.i32 = 7}}});
}
```

---

## Headers

| Header                    | Purpose                                                         |
|---------------------------|-----------------------------------------------------------------|
| `lexer.hpp`               | Token kinds, LEB128 decoders, wasm_opcode/wasm_value_type enums |
| `source_span.hpp`         | Source position + diagnostic helpers                            |
| `ast_tags.hpp`            | vakya tag structs + stable_id 2000–2031                         |
| `opcode_map.hpp`          | `k_wasm_opcode_map` — 139-row data-driven opcode table          |
| `build_ast.hpp`           | taranga_kind, taranga_node_ext, dual-AST builder                |
| `parser_wat.hpp`          | Hand-rolled recursive-descent WAT S-expression parser           |
| `decoder_bin.hpp`         | Section-by-section binary .wasm decoder                         |
| `frontend.hpp`            | `parse()` façade (WAT + binary, auto-sniff)                     |
| `module_view.hpp`         | Typed section tables from dual AST                              |
| `validate.hpp`            | Wasm validation + `validated_module` capability token           |
| `ssa_build.hpp`           | Stack→SSA construction over Wasm structured control             |
| `memory.hpp`              | Linear memory model (`wasm_memory`) + typed load/store          |
| `runtime_prelude.hpp`     | Host intrinsics for conversions, bit-counts, truncation         |
| `lower_hl.hpp`            | 4-phase SSA → Lithe HL MIR lowering + freeze + verify_portable  |
| `engine.hpp`              | Interpreter engine: `create()` / `invoke()`                     |
| `std/detail/register.hpp` | Lightweight stdlib function registry for host projections       |
| `std/core.hpp`            | `std.core` installs scalar helpers (`IdentityI64`, `ClampI64`)  |
| `std/math.hpp`            | `std.math` installs integer arithmetic helpers                  |
| `std/time.hpp`            | `std.time` installs monotonic time helper (`NowNs`)             |
| `std/io.hpp`              | `std.io` installs console helper (`PrintlnI64`)                 |
| `std/std.hpp`             | stdlib umbrella: `install_std_all(registry&)`                   |
| `taranga.hpp`             | Umbrella include                                                |

---

## Dual AST

Every node is written to two stores simultaneously:

- **`taranga_ir_module`** — flat, index-addressed, egraph-usable. Primary.
- **`taranga_ast_arena`** — variant node store for dumps and tools.

**Parity invariant**: `ir_mod.size() == ast.size()` always holds.

---

## validated_module Token

`validated_module` is a move-only capability token. Only `validate()` can construct it. `engine::create()` requires it,
ensuring the engine never runs an unvalidated module.

```cpp
// validated_module is NOT copy-constructible
static_assert(!std::is_copy_constructible_v<taranga::validated_module>);
```

---

## Data-Driven Opcode Table

`k_wasm_opcode_map` in `opcode_map.hpp` has 139 rows. Adding a new Wasm proposal means adding rows — never editing
switches. Each row specifies:

- `opcode` — wasm_opcode enum value
- `hl_op_name` — HL MIR operation name
- `arity` — number of stack operands consumed
- `result_count` — number of values produced
- `result_type` — result type rule
- `guard_kind_name` — trap kind (empty if no trap)
- `capability` — required capability bit (atomics, SIMD, etc.)
- `is_terminator` — true for br/return/unreachable
- `prelude_fn` — if non-empty, lower as prelude call instead

---

## Memory Model

Linear memory is byte-addressed (`memref<?xi8>`) with a page size of 65536 bytes. `wasm_memory` owns a
`std::vector<uint8_t>`:

```cpp
taranga::wasm_memory mem(2 /*pages*/, 64 /*max_pages*/);
auto old = mem.grow(1);  // returns old page count
```

Typed load/store uses `std::memcpy` for strict-aliasing safety:

```cpp
taranga::typed_store<std::int32_t>(mem, addr, offset, value);
auto v = taranga::typed_load<std::int32_t>(mem, addr, offset);
```

---

## AST Tag Stable IDs

Taranga reserves `stable_id` range **2000–2031** in the vakya tag band:

| Tag           | stable_id |
|---------------|-----------|
| module_tag    | 2000      |
| type_tag      | 2001      |
| import_tag    | 2002      |
| export_tag    | 2003      |
| func_tag      | 2004      |
| ...           | ...       |
| vec_instr_tag | 2031      |

---

## Diagnostic Codes

| Prefix              | Source               |
|---------------------|----------------------|
| `TARANGA-PARSE-###` | WAT lexer / parser   |
| `TARANGA-BIN-###`   | Binary decoder       |
| `TARANGA-VAL-###`   | Validation           |
| `TARANGA-SSA-###`   | SSA construction     |
| `TARANGA-LOWER-###` | HL MIR lowering      |
| `TARANGA-EXEC-###`  | Engine / interpreter |

---

## Pay-for-Use

The headers are designed for selective inclusion:

- WAT-only builds: never instantiate `decoder_bin.hpp`
- Interpreter-only builds: never link JIT/GPU paths
- Validation-only tools: stop at `validate.hpp`

---

## Lithe HL MIR Integration

`lower_to_hl()` in `lower_hl.hpp` follows the four-phase lowering contract from the design document:

| Phase           | What it does                                                                                                                                                                |
|-----------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **A — Control** | params (`argument`), block args (MLIR-style φ), `branch`, `branch_cond`, `return`, `trap(unreachable)`                                                                      |
| **B — Numeric** | `add/sub/mul/sdiv/udiv/srem/urem/bit_*/shl/lshr/ashr`, `fadd/fsub/fmul/fdiv/fneg/sqrt/abs`; comparisons as `icmp`/`fcmp` with `compare_predicate` from `lithe::codegen::hl` |
| **C — Safety**  | `i32.div_*/i64.div_*` → `guard(div_by_zero)` on denominator; `unreachable` → `trap(unreachable)`                                                                            |
| **D — Memory**  | `*.load*`/`*.store*` → `memref_load`/`memref_store` over `memref<?xi8>` byte memref                                                                                         |

Conversions without direct HL MIR ops (wrap/extend/trunc/convert/reinterpret, clz/ctz/popcnt, rotl/rotr) lower to `call`
on host prelude functions (capability `external_calls`), as per §12 of the design document.

After building live `hl_mir_function` per Wasm function:

1. `freeze_module(fn_ptrs, opts)` → `portable_module`  (`lithe_ir/portable/bridge.hpp`)
2. `verify_portable(portable, policy)` → `verify_report`  (`lithe_ir/portable/verify.hpp`)

`hl_result.portable` carries the frozen `portable_module` for the Lithe engine.
`hl_result.hl_text` is a human-readable summary (function names, import list).
