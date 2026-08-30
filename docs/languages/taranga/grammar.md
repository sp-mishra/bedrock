# Taranga WAT Grammar

This document specifies the subset of WebAssembly Text Format (WAT) accepted by `taranga::parser_wat`.

---

## Notation

```
symbol      := alternative1 | alternative2
symbol*     := zero or more repetitions
symbol+     := one or more repetitions
symbol?     := zero or one
'text'      := literal token
id          := $[a-zA-Z0-9!#$%&'*+\-./:;<=>?@\\^_`|~]+
nat         := [0-9]+  |  0x[0-9A-Fa-f]+
int         := [+-]? nat
float       := [+-]? nat ('.' nat?)? ([eE] [+-]? nat)?
              | [+-]? 'inf'
              | [+-]? 'nan'
              | [+-]? 'nan:0x' [0-9A-Fa-f]+
string      := '"' char* '"'
```

---

## Top-Level

```
module      := '(' 'module' id? module_field* ')'

module_field :=
    type_field
  | import_field
  | func_field
  | table_field
  | memory_field
  | global_field
  | export_field
  | start_field
  | elem_field
  | data_field
```

---

## Type Section

```
type_field  := '(' 'type' id? '(' 'func' functype ')' ')'

functype    := param* result*

param       := '(' 'param' id? valtype ')'
             | '(' 'param' valtype* ')'

result      := '(' 'result' valtype* ')'

valtype     := 'i32' | 'i64' | 'f32' | 'f64' | 'v128'
             | 'funcref' | 'externref'
```

---

## Import Section

```
import_field := '(' 'import' string string import_desc ')'

import_desc  :=
    '(' 'func'   id? typeuse ')'
  | '(' 'table'  id? tabletype ')'
  | '(' 'memory' id? memtype ')'
  | '(' 'global' id? globaltype ')'
```

---

## Function Section

```
func_field  := '(' 'func' id? typeuse local* instr* ')'

typeuse     := '(' 'type' typeidx ')'
             | param* result*

local       := '(' 'local' id? valtype ')'
             | '(' 'local' valtype* ')'

typeidx     := nat | id
funcidx     := nat | id
```

---

## Table, Memory, Global

```
table_field  := '(' 'table' id? tabletype ')'
tabletype    := limits reftype
reftype      := 'funcref' | 'externref'

memory_field := '(' 'memory' id? memtype ')'
memtype      := limits
limits       := nat nat?

global_field := '(' 'global' id? globaltype instr ')'
globaltype   := valtype
             | '(' 'mut' valtype ')'
```

---

## Export, Start

```
export_field := '(' 'export' string exportdesc ')'
exportdesc   :=
    '(' 'func'   funcidx ')'
  | '(' 'table'  tableidx ')'
  | '(' 'memory' memidx ')'
  | '(' 'global' globalidx ')'

start_field  := '(' 'start' funcidx ')'
```

---

## Instructions

Instructions appear both in plain and folded (S-expression) form.

```
instr       := plain_instr | folded_instr

plain_instr :=
    'unreachable'
  | 'nop'
  | 'return'
  | 'drop'
  | 'select' valtype?
  | 'local.get'  localidx
  | 'local.set'  localidx
  | 'local.tee'  localidx
  | 'global.get' globalidx
  | 'global.set' globalidx
  | 'i32.const' int
  | 'i64.const' int
  | 'f32.const' float
  | 'f64.const' float
  | 'i32.load' memarg
  | 'i64.load' memarg
  | 'f32.load' memarg
  | 'f64.load' memarg
  | 'i32.store' memarg
  | ...
  | memory_instr
  | numeric_instr
  | control_instr

folded_instr := '(' plain_instr instr* ')'
             |  '(' block_instr ')'
             |  '(' loop_instr ')'
             |  '(' if_instr ')'

memarg      := 'offset=' nat? 'align=' nat?
             | (both optional, parsed as keyword=value)
```

---

## Control Instructions

```
control_instr :=
    'block' id? blocktype instr* 'end' id?
  | 'loop'  id? blocktype instr* 'end' id?
  | 'if'    id? blocktype instr* ('else' id? instr*)? 'end' id?
  | 'br'       labelidx
  | 'br_if'    labelidx
  | 'br_table' labelidx+ labelidx
  | 'call'         funcidx
  | 'call_indirect' typeuse tableidx?

blocktype := valtype?
           | '(' 'type' typeidx ')'

labelidx  := nat | id
```

---

## Numeric Instructions (selected)

```
numeric_instr :=
  -- i32 --
    'i32.clz' | 'i32.ctz' | 'i32.popcnt'
  | 'i32.add' | 'i32.sub' | 'i32.mul'
  | 'i32.div_s' | 'i32.div_u' | 'i32.rem_s' | 'i32.rem_u'
  | 'i32.and' | 'i32.or' | 'i32.xor'
  | 'i32.shl' | 'i32.shr_s' | 'i32.shr_u' | 'i32.rotl' | 'i32.rotr'
  | 'i32.eqz' | 'i32.eq' | 'i32.ne' | 'i32.lt_s' | 'i32.lt_u'
  | 'i32.le_s' | 'i32.le_u' | 'i32.gt_s' | 'i32.gt_u'
  | 'i32.ge_s' | 'i32.ge_u'
  -- i64 / f32 / f64 — analogous --
  -- conversions --
  | 'i32.wrap_i64'
  | 'i64.extend_i32_s' | 'i64.extend_i32_u'
  | 'f32.demote_f64'   | 'f64.promote_f32'
  | 'i32.reinterpret_f32' | 'f32.reinterpret_i32'
  | 'i64.reinterpret_f64' | 'f64.reinterpret_i64'
  | 'i32.trunc_f32_s' | 'i32.trunc_f32_u'
  | ...
  | 'i32.extend8_s' | 'i32.extend16_s'
  | 'i64.extend8_s'  | 'i64.extend16_s' | 'i64.extend32_s'
  | 'memory.size' | 'memory.grow'
```

---

## Comments

```
line_comment  := ';;' … newline
block_comment := '(;' … ';)'   (nestable)
```

---

## Accepted Character Classes

Identifiers start with `$` followed by:

```
[a-zA-Z0-9!#$%&'*+\-./:;<=>?@\\^_`|~]+
```

Strings are double-quoted with `\nn` hex escapes and `\t \n \r \\ \"` shorthand.
