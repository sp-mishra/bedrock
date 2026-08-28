# Samasa — Generic Compile-Time Grammar & Parser Framework

`pebble/include/languages/samasa/` provides a compile-time, header-only PEG grammar and CST parser
framework for building language frontends. It sits between meta/akshara (compile-time primitives) and
language-specific frontends (custom grammars, DSLs).

> **Location note:** Samasa is a Pebble library at
> `pebble/include/languages/samasa/`. The deprecated
> `languages/generic/samasa/` compatibility shim remains only for source
> compatibility. Namespace remains `lang::samasa` / `lang::parser`.

**Namespace:** `lang::samasa` (aliased as `lang::parser`)  
**Umbrella:** `pebble/include/languages/samasa/samasa.hpp`  
**C++23, header-only. No virtual functions. No macros.**  
**No heap allocation inside matcher execution, cursor movement, combinator dispatch, or Pratt
recursion.** Buffer growth happens only at policy-owned boundaries.

---

## Table of Contents

1. [Stack Position](#stack-position)
2. [Architecture](#architecture)
3. [Algorithms Used](#algorithms-used)
4. [Performance](#performance)
5. [Dependency Contract](#dependency-contract)
3. [Quick Start](#quick-start)
4. [Entry Points](#entry-points)
5. [Lexer (Scanner)](#lexer-scanner)
6. [Matcher DSL](#matcher-dsl)
7. [Grammar & Validation](#grammar--validation)
8. [FIRST Sets](#first-sets)
9. [FOLLOW Sets](#follow-sets)
10. [Pratt Expression Parser](#pratt-expression-parser)
10. [Parse Tree (Green/Red)](#parse-tree-greenred)
11. [Error Recovery](#error-recovery)
12. [Parse Tracing](#parse-tracing)
13. [Memoization Policies](#memoization-policies)
14. [Parse Context](#parse-context)
15. [Integration Contract](#integration-contract)
16. [Tooling](#tooling)
17. [File Reference](#file-reference)
18. [Status](#status)
19. [See Also](#see-also)

---

## Stack Position

```
akshara                  (compile-time strings, charsets, hashing)
  ↓
meta                     (TypeList, ct_array, type-level utilities)
  ↓
Samasa                   ← HERE (scanner + PEG grammar + CST parser)
  ↓
Language frontend        (custom language or DSL frontend)
  ↓
Vakya / HLIR             (expression trees, type inference)
  ↓
Lithe                    (portable IR, codegen)
```

**Samasa owns:** source scanning, PEG parsing, CST construction (green/red trees), syntax diagnostics,
syntax error recovery.

**Samasa does not own:** type systems, name resolution, semantic analysis, AST construction, IR lowering,
any knowledge of specific language frontends.

Language frontends consume green/red trees and lower them to their own AST/IR.

---

## Architecture

Samasa is a compile-time parser-generator framework: the grammar is a set of C++ types, validated and
lowered to a parser at compile time. Runtime data flow is a straight scanner → PEG/Pratt parse → CST
pipeline; nothing is interpreted dynamically.

```
Grammar types (Matcher DSL)                  compile-time
     │  grammar validation (FIRST/FOLLOW, left-recursion, ambiguity checks)
     v
┌──────────────────────────────────────────────────────────┐
│ Scanner (lexer.hpp)   source text → token stream          │
└──────────────────────────────────────────────────────────┘
     │  tokens
     v
┌──────────────────────────────────────────────────────────┐
│ PEG parser + Pratt expression sub-parser                  │
│   memoized (packrat policies) · error recovery            │
└──────────────────────────────────────────────────────────┘
     │  parse events
     v
┌──────────────────────────────────────────────────────────┐
│ CST: green tree (immutable, shared) + red tree (cursor)   │
│   incremental reparse: apply_edit → find_affected_root →  │
│   rescan/reparse → splice_subtree → recompute_hashes      │
└──────────────────────────────────────────────────────────┘
     │  green/red CST
     v
Language frontend (lowers CST → its own AST/IR)
```

---

## Algorithms Used

Concrete named algorithms in the implementation, with the header they live in.

| Concern | Algorithm | Where |
|---|---|---|
| Grammar analysis | FIRST-set / FOLLOW-set computation (fixpoint iteration) for LL decisioning + ambiguity checks | `samasa.hpp` (FIRST/FOLLOW sections) |
| Grammar validation | Compile-time grammar well-formedness: left-recursion detection, ambiguity + reachability checks | `grammar/*` |
| Keyword lookup | Sorted-hash table + binary search: FNV-1a64 hashes sorted at compile time, runtime `lower_bound` on hash then byte-compare the (tiny) equal-hash run — O(log N) per identifier | `lex/keyword_table.hpp` |
| Operator precedence lookup | Per-fixity sorted arrays keyed by token value + `lower_bound` — O(log N) per Pratt step | `expr/operator_table.hpp` |
| Parsing | PEG (parsing-expression-grammar) recursive-descent over the Matcher DSL | `samasa.hpp` matcher/grammar |
| Expression parsing | Pratt / top-down operator-precedence parser (binding-power driven) | `expr/pratt.hpp` |
| Memoization | Packrat-style memoization policies (bounded / full) over `unordered_map` with `reserve()` to avoid rehash churn; guarantees linear-time PEG parse | `policies/memo_policy.hpp` |
| Recovery sync set | Sorted token array + `binary_search` membership — O(log N) per recovery check | `recovery/sync_set.hpp` |
| CST representation | Green/red tree (immutable shared green nodes + lazy red cursor with parent links) | `tree/*` (Parse Tree §) |
| Incremental reparse | Edit-localized reparse: `apply_edit → find_affected_root → rescan/reparse → splice_subtree → recompute_ancestor_hashes`; full-reparse fallback | `tree/incremental.hpp` |
| Error recovery | Synchronizing-token / panic-mode recovery to continue after syntax errors | `samasa.hpp` (Error Recovery) |

---

## Performance

Samasa is tuned on both the run-time hot path (per-token scanning + parsing) and compile-time
include cost. All optimizations are behavior-preserving — the conformance suite passes unchanged.

### Runtime — O(log N) lexer & parser lookups

The three per-token dispatch tables replaced linear folds over all entries with sorted
`static constexpr` arrays plus binary search:

- **`keyword_table<KWs...>`** — builds a sorted-by-hash entry array at compile time. Runtime
  `lookup(sv)` hashes the word (FNV-1a64), `lower_bound`s the sorted hashes, then byte-compares only
  the (typically length-1) equal-hash run. O(log N + collisions) per identifier instead of O(N).
- **`operator_table<Ops...>`** — three sorted arrays (prefix/infix/postfix) keyed by token value;
  `prefix_bp/infix_bp/postfix_bp` binary-search their array. Binding-power / associativity math is
  byte-identical to the previous fold.
- **`sync_set<TokenKinds...>`** — sorted token array; `contains(k)` is `binary_search`. Empty set is
  a constant `false`.

### Memoization — pre-sized tables

`selective_memo` and `packrat_memo` expose `reserve(std::size_t)` so callers can pre-size the
`unordered_map` to the token count before parsing, avoiding rehash + malloc churn under packrat.
`no_memo::reserve` is a `constexpr` no-op (zero cost when disabled).

### Compile-time — lean tokenizer aggregate

Tokenizer-only consumers (syntax highlighters, line counters, lexer front-ends) can include
`languages/samasa/samasa_lex.hpp` — it pulls `core/` + `lex/` only, skipping the grammar, DSL, tree,
Pratt, recovery, and policy layers. `samasa.hpp` remains the full aggregate for parsing.

```cpp
#include "languages/samasa/samasa_lex.hpp"   // scanner + tokens only
using namespace lang::samasa;
auto tokens = scan<KW, OT, LinePolicy, TK>(source, kinds, lp, sink);
```

---

## Dependency Contract

| Direction    | What                                               |
|--------------|----------------------------------------------------|
| Samasa uses  | `meta/meta.hpp`, `meta/akshara.hpp`                |
| Samasa uses  | `languages/generic/core/` (diagnostics, identity)  |
| Samasa does NOT use | Vakya, Lithe, or any specific language frontend |
| Language frontends use Samasa | For parsing only — no semantic calls back down |

---

## Quick Start

```cpp
#include "languages/samasa/samasa.hpp"
using namespace lang::samasa;

// 1. Define token and syntax kind enums (must be enum types)
enum class TK : std::uint8_t { eof, ident, kw_let, op_eq };
enum class SK : std::uint8_t { root, decl };

// 2. Define grammar rules — wrap patterns in node_t to emit CST nodes
using decl_pattern =
    node_t<SK::decl,
        seq_t<tok<TK::kw_let>, tok<TK::ident>, tok<TK::op_eq>, tok<TK::ident>>
    >;
using decl_rule = rule<"decl", decl_pattern>;

// 3. Root rule wraps decl + eof in a root node
using root_pattern =
    node_t<SK::root,
        seq_t<decl_rule, eof>
    >;
using root_rule_t = rule<"root", root_pattern>;

// 4. Define grammar
using MyGrammar = grammar<SK, TK, root_rule_t, root_rule_t, decl_rule>;

// 5. Validate at compile time
static_assert(grammar_valid<MyGrammar>());

// 6. Define keywords and operators for the scanner
using KW = keyword_table<keyword<"let", TK::kw_let>>;
using OT = operator_trie<operator_token<"=", TK::op_eq>>;

// 7. Parse at runtime
constexpr scan_token_kinds<TK> kinds{ TK::eof, TK::ident, TK::eof, TK::eof, TK::eof, TK::eof };
auto output = parse<MyGrammar, KW, OT>("let x = y", {}, kinds, no_line_sensitivity<TK>{});
// output.tree is a green_tree<SK> with root → decl → tokens
// output.success == true if no hard failures
```

---

## Entry Points

### `parse<Grammar, KWTable, OpTrie, LinePolicy>(source, opts, tok_kinds, lp)` — runtime

Scans source text, runs the PEG grammar, returns `parse_output<SK, TK>`.

```cpp
template <class Grammar,
          class KWTable = keyword_table<>,
          class OpTrie  = operator_trie<>,
          class LinePol = no_line_sensitivity<typename Grammar::token_kind>>
parse_output<typename Grammar::syntax_kind, typename Grammar::token_kind>
parse(std::string_view source,
      const parse_options& opts  = {},
      const scan_token_kinds<typename Grammar::token_kind>& tok_kinds = {},
      const LinePol& lp = {});
```

`parse_output<SK,TK>` fields — runtime, heap/arena backed:

| Field         | Type                           | Meaning                         |
|---------------|--------------------------------|---------------------------------|
| `tree`        | `green_tree<SK>`               | Immutable CST                   |
| `tokens`      | `token_buffer<TK>`             | Scanned token array             |
| `diagnostics` | `collecting_sink<diagnostic>`  | All lex and parse diagnostics   |
| `stats`       | `parse_tree_stats`             | Node count, depth, timing       |
| `success`     | `bool`                         | No hard failures and no errors  |

### `parse_static<Grammar, Src, MaxTokens, MaxEvents, MaxDiags, MaxDepth>()` — consteval

Compile-time parse of a `fixed_string` source. Scans, then runs the grammar root rule at compile time.
Returns `static_parse_output<SK,TK>` with fixed-capacity arrays — no heap allocation.

The consteval product is the **event log** (`out.events[0..event_count]`) plus diagnostics; no
`green_tree` is built at compile time (arena allocation is not consteval-friendly). To obtain a tree,
reconstruct one at runtime from `out.events` using `green_tree<SK>::build`.

Scanner configuration (KWTable, OpTrie, LinePol) is supplied as function arguments with defaults so
simple grammars need no extra arguments.

```cpp
template <class Grammar,
          akshara::fixed_string Src,
          std::uint32_t MaxTokens = 4096,
          std::uint32_t MaxEvents = 4096,
          std::uint32_t MaxDiags  = 256,
          std::uint32_t MaxDepth  = 256,
          class KWTable        = keyword_table<>,
          class OpTrie         = operator_trie<>,
          class LinePol        = no_line_sensitivity<typename Grammar::token_kind>>
consteval auto parse_static(
    scan_token_kinds<typename Grammar::token_kind> tok_kinds = {},
    LinePol lp = {})
    -> static_parse_output<typename Grammar::syntax_kind,
                           typename Grammar::token_kind,
                           MaxTokens, MaxEvents, MaxDiags>;
```

**Grammar rule requirement:** every `match()` in the grammar's rule/combinator chain must be
`constexpr`. All built-in samasa matchers (`tok<>`, `seq`, `choice`, `opt`, `many`, `rule<>`,
`node<>`, etc.) satisfy this. User-defined rule wrappers must also mark `match()` `constexpr`.

`static_parse_output<SK,TK,MaxTokens,MaxEvents,MaxDiags>` fields — consteval, fixed capacity:

| Field         | Type                                               | Meaning                                    |
|---------------|----------------------------------------------------|--------------------------------------------|
| `tokens`      | `std::array<token<TK>, MaxTokens>`                 | Scanned tokens                             |
| `token_count` | `std::uint32_t`                                    | Number of valid tokens                     |
| `events`      | `std::array<static_parse_event<SK>, MaxEvents>`    | Full CST event log (begin/end/token/error) |
| `event_count` | `std::uint32_t`                                    | Number of events                           |
| `diagnostics` | `std::array<diagnostic, MaxDiags>`                 | Diagnostics                                |
| `diag_count`  | `std::uint32_t`                                    | Number of diagnostics                      |
| `success`     | `bool`                                             | No failures and no overflow                |

`static_parse_event<SK>` carries `event_kind`, `node_kind` (SK), `token_index`, and `byte_span` —
preserving full precedence structure for later green tree reconstruction.

Capacity overflow at compile time sets `success=false`. It is never a hard compile error unless the
caller opts in: `static_assert(out.success)`.

### Allocation Policies (type tags)

Three zero-storage policy tags convey buffer strategy at the type level:

```cpp
struct fixed_buffer_policy  {};  // std::array-backed, capacity-checked (consteval)
struct arena_buffer_policy  {};  // bump arena owned by caller
struct dynamic_buffer_policy{}; // std::vector/pmr (runtime default)
```

### `parse_options`

```cpp
struct parse_options {
    limits budget;
    bool   preserve_trivia = true;   // retain whitespace/comment trivia in tree
    bool   build_red_tree  = false;  // red tree is lazy; call red_tree<SK>::build() explicitly
};
```

---

## Lexer (Scanner)

### `scan<KWTable, OpTrie, LinePolicy, TokenKind, ScannerPolicy>(source, kinds, lp, sink)`

```cpp
template <class KWTable, class OpTrie, class LinePolicyT, class TokenKind,
          class ScannerPolicy = scanner_policy<TokenKind>>
token_buffer<TokenKind> scan(
    std::string_view source,
    const scan_token_kinds<TokenKind>& kinds,
    const LinePolicyT& lp,
    lang::collecting_sink<diagnostic>& sink,
    const ScannerPolicy& policy = {});
```

Scanner loop order: custom token hook → whitespace trivia → newline+line_policy → `//` line comment →
`/* */` block comment → identifier/keyword → integer/float literal → string literal → operator
longest-match → unknown character.

### Token Layout

```cpp
template <class TokenKind>
struct token {
    TokenKind     kind;
    std::uint32_t offset;        // byte offset in source
    std::uint32_t length;        // byte length (uint32 — supports large literals/blobs)
    std::uint32_t trivia_start;  // first index in token_buffer::trivia_arena
    std::uint16_t trivia_count;  // number of consecutive trivia entries
    std::uint16_t flags;         // reserved (doc-comment, synthetic, etc.)
};
static_assert(std::is_trivially_copyable_v<token<std::uint32_t>>);
// Size is NOT ABI-stable across builds; do not hardcode sizeof(token<>).
```

Trivia (whitespace, comments) is stored in `token_buffer::trivia_arena` — a separate `std::vector<trivia>`
indexed by `trivia_start`/`trivia_count` per token.

### `scanner_policy<TK>` — Customization Hook

Language-specific scanner behavior is expressed through a policy type:

```cpp
template <class TokenKind>
struct scanner_policy {
    static constexpr bool nested_block_comments = false;
    static constexpr bool unicode_identifiers   = false;

    // Return true if a custom token was recognized and emitted.
    template <class ScannerView>
    static bool scan_custom_token(ScannerView& sv) { return false; }
};
```

`ScannerView` exposes `source`, `pos` (by ref), `emit_tok`, `emit_trivia`, `sink`, and `kinds`.
Custom tokens are checked first on each character — they can intercept any input.

Pass a derived policy as the fifth template parameter to `scan<>`:

```cpp
struct MyPolicy : scanner_policy<TK> {
    template <class SV>
    static bool scan_custom_token(SV& sv) {
        if (sv.source[sv.pos] == '#') {
            sv.emit_tok(TK::hash, sv.pos, 1); ++sv.pos; return true;
        }
        return false;
    }
};
auto buf = scan<KW, OT, LP, TK, MyPolicy>(source, kinds, lp, sink);
```

### `ascii_identifier_policy` / `unicode_identifier_policy<TK>`

Controls which code points are valid identifier starts and continuations.

```cpp
struct ascii_identifier_policy {
    static constexpr bool is_ident_start(char32_t c) noexcept;    // [a-zA-Z_]
    static constexpr bool is_ident_continue(char32_t c) noexcept; // [a-zA-Z0-9_]
};

// Specialize for full Unicode identifier support:
template <class TokenKind>
struct unicode_identifier_policy : ascii_identifier_policy {};
```

Specialize `unicode_identifier_policy<YourTK>` to accept Unicode identifier categories.
Enable via `scanner_policy<TK>::unicode_identifiers = true`.

### `scanner_mode_stack`

Opt-in mode stack for contextual lexing (template interpolation, raw strings, indentation blocks).

```cpp
using scanner_mode_id = std::uint16_t;
inline constexpr scanner_mode_id k_scanner_mode_default = 0;

class scanner_mode_stack {
public:
    static constexpr std::size_t kMaxDepth = 32;

    scanner_mode_id mode()  const noexcept;   // current mode (default=0 when empty)
    std::size_t     depth() const noexcept;   // current stack depth

    void push_mode(scanner_mode_id m) noexcept;
    void pop_mode()  noexcept;                // safe no-op on empty stack
};
```

Use inside `scanner_policy::scan_custom_token` to push/pop modes as bracket tokens are scanned.
`pop_mode()` on an empty stack is a safe no-op.

### `keyword_table<KWs...>`

```cpp
using KW = keyword_table<
    keyword<"let",   TK::kw_let>,
    keyword<"fn",    TK::kw_fn>
>;
```

Compile-time sorted-by-hash entry table; runtime `lookup(sv)` hashes (FNV-1a64), binary-searches the
sorted hashes, then byte-compares the equal-hash run. O(log N) per identifier. No heap allocation.

### `operator_trie<OTs...>`

```cpp
using OT = operator_trie<
    operator_token<"==", TK::op_eqeq>,
    operator_token<"=",  TK::op_eq>
>;
```

Longest-match: `==` matched before `=`. Compile-time trie.

### Line Sensitivity Policies

| Policy                            | Behavior                                             |
|-----------------------------------|------------------------------------------------------|
| `no_line_sensitivity<TK>`         | Newlines are trivia (default)                        |
| `newline_terminates<TK,SepKind>`  | Newline emits a separator token (Go/JS-style)        |
| `statement_ending<TK>`            | Newline triggers synthetic separator on statement end |

---

## Matcher DSL

All matchers live in `lang::samasa`. They are zero-cost value types; `match(ctx)` returns
`parse_result<Stream>`.

**PEG ordered-choice rule:** `choice(a, b, c)` tries `a` first. If `a` soft-fails, the stream is
reset to the pre-`a` position and `b` is tried. This differs from CFG ambiguity resolution: the first
matching alternative always wins. A later alternative is never tried if an earlier one succeeds, even
if the later alternative would match more input.

### Primitives

Two concept groups separate char-stream and token-stream matchers:

- `char_stream_like` — matchers valid on raw character input
- `token_stream_like` — matchers valid on a scanned `token_stream<TK>`

| Matcher                    | Stream          | Matches                                              |
|----------------------------|-----------------|------------------------------------------------------|
| `tok<Kind>`                | token           | Single token of the given `TokenKind` enum value     |
| `eof`                      | token           | End of token stream                                  |
| `token_text<S>`            | token           | Token whose source spelling equals `S` (text check) |
| `char_lit<S>`              | char            | Raw character sequence equal to `S`                  |
| `char_in<Set>`             | char            | Single character in compile-time `ct_char_set`       |
| `contextual_keyword<Word>` | token           | Identifier token whose text matches `Word`           |

Note: `lit<S>` from earlier versions is retired — use `char_lit<S>` for char streams or
`token_text<S>` for token streams.

### Combinators

| Combinator              | Factory                  | Semantics                                          |
|-------------------------|--------------------------|----------------------------------------------------|
| `seq_t<Ms...>`          | `seq(a, b, c)`           | All in order; see cut below                        |
| `choice_t<Ms...>`       | `choice(a, b, c)`        | PEG ordered choice; backtracks on soft fail        |
| `opt_t<M>`              | `opt(m)`                 | Zero or one; never fails                           |
| `many_t<M>`             | `many(m)`                | Zero or more; M must not be nullable               |
| `many1_t<M>`            | `many1(m)`               | One or more                                        |
| `sep_by_t<A,Sep>`       | `sep_by(a, sep)`         | `A (Sep A)*`; zero matches allowed                 |
| `sep_by1_t<A,Sep>`      | `sep_by1(a, sep)`        | `A (Sep A)*`; at least one match                   |
| `lookahead_t<M>`        | `lookahead(m)`           | Peek without consuming                             |
| `not_followed_by_t<M>`  | `not_followed_by(m)`     | Fail if M matches (no consumption)                 |
| `cut`                   | `cut{}`                  | Seq-local: commits seq; converts next soft fail to hard fail |

### cut semantics (seq-local)

`cut` is **seq-local** — it commits the enclosing `seq`, not any outer context. Concretely:

- Inside `seq(a, cut, b)`: once `a` and `cut` have matched, a subsequent failure of `b` becomes a
  hard fail. The enclosing `choice` **does not** try its next alternative.
- A `cut` inside an inner `seq` does not commit any outer `seq` or `choice`. Each `seq` tracks its
  own committed state via a local `bool`.

```cpp
// choice tries only the first branch because cut committed it:
choice(
    seq(tok<TK::kw_let>{}, cut{}, tok<TK::ident>{}),  // hard-fail on missing ident
    some_other_rule{}
)
```

### Backtracking & Checkpoints

All combinators that backtrack (`seq`, `choice`, `lookahead`, `not_followed_by`, `sep_by`) take a
full **checkpoint** before attempting alternatives and **roll back** atomically on failure.

A checkpoint covers:
- Cursor position
- `event_stream` snapshot (O(1) via index)
- Diagnostic sink size (`collecting_sink::truncate()` on rollback)
- Repair counter

This guarantees that failed alternatives leave **no observable side-effects** — no leaked diagnostics,
no leaked event nodes, no incremented repair counts.

```cpp
// parse_checkpoint captures all rollback state atomically:
auto cp = ctx.checkpoint();
// ... attempt match ...
ctx.rollback(cp);  // restores cursor + events + diagnostics + repairs
```

### Result States

```cpp
enum class parse_status { success, soft_fail, hard_fail };

template <class Stream>
struct parse_result {
    parse_status    status;
    cursor<Stream>  next;           // position after match
    std::uint32_t   furthest_error; // rightmost error offset seen

    static parse_result success_at(cursor, uint32_t furthest = 0);
    static parse_result soft_failure(cursor, uint32_t furthest = 0);
    static parse_result hard_failure(cursor, uint32_t furthest = 0);

    bool ok()        const;
    bool soft_fail() const;
    bool hard_fail() const;
    parse_result harden() const;  // soft_fail → hard_fail
};
```

- `soft_fail`: backtrackable; `choice` will try the next alternative.
- `hard_fail`: non-backtrackable; `seq` propagates immediately; `choice` does not retry.

### `rule<Name, Pattern>`

```cpp
template <akshara::fixed_string Name, class Pattern>
struct rule {
    static constexpr std::string_view name_sv;
    using pattern_type = Pattern;

    template <class Ctx>
    auto match(Ctx& ctx) const -> parse_result<typename Ctx::stream_type>;
};
```

Adds depth tracking (guards infinite recursion via `ctx.over_depth()`).

### `node_t<Kind, Pattern>`

Wraps a pattern, emits `begin_node`/`end_node` events on success, rolls back on failure.

```cpp
auto decl = node<SK::decl>(seq(tok<TK::kw_let>{}, tok<TK::ident>{}));
```

---

## Grammar & Validation

### `grammar<SK, TK, RootRule, Rules...>`

```cpp
template <class SyntaxKindT, class TokenKindT, class RootRule, class... Rules>
struct grammar {
    static_assert(std::is_enum_v<SyntaxKindT>);  // SK must be an enum
    static_assert(std::is_enum_v<TokenKindT>);   // TK must be an enum
    using syntax_kind = SyntaxKindT;
    using token_kind  = TokenKindT;
    using root_rule   = RootRule;
    using rules       = meta::TypeList<Rules...>;
    static constexpr std::size_t rule_count = sizeof...(Rules);
};
```

EOF token kind is supplied through `scan_token_kinds<TK>{ .eof = TK::eof, ... }`. No specific
enumerator value is mandated for EOF.

### `validate_grammar<G>()` — structured result

Returns a `grammar_validation_result<N>` with a typed array of issues. `ok()` returns `true` when
there are no **error-severity** issues (warnings do not fail `ok()`).

```cpp
constexpr auto result = validate_grammar<MyGrammar>();
static_assert(result.ok());           // no error-severity issues
// result.issues[0].code             // grammar_diag_code enum
// result.issues[0].severity         // grammar_issue_severity (error/warning/note)
// result.issues[0].rule             // std::string_view into rule::name_sv static storage
```

### `grammar_issue_severity` — three levels

```cpp
enum class grammar_issue_severity : std::uint8_t {
    error   = 0,  // hard: ok() returns false
    warning = 1,  // soft: ok() still true; may indicate logic issue
    note    = 2,  // informational
};
```

`grammar_validation_result<N>::ok()` returns `false` only if any issue has `severity == error`.
`has_warnings()` returns `true` if any issue has `severity == warning`.

### `grammar_diag_code` — 11 validation codes

| Code                  | Enum value | Severity | Problem detected                                        |
|-----------------------|------------|----------|---------------------------------------------------------|
| `empty_many`          | 0          | error    | `many<M>` where `M` is nullable → infinite loop         |
| `duplicate_rule`      | 1          | error    | Two rules with the same name string                     |
| `unreachable_rule`    | 2          | error    | Rule not reachable from root (ref-name walk)            |
| `left_recursion`      | 3          | error    | Direct left-recursion: leftmost element references own rule |
| `unknown_ref`         | 4          | error    | Pattern references undefined rule name                  |
| `duplicate_operator`  | 5          | error    | `operator_table` contains two operators with same spelling |
| `bad_pratt_table`     | 6          | error    | Malformed Pratt table (duplicate operator, invalid binding power) |
| `nullable_root`       | 7          | error    | Root rule is nullable → parser may accept empty input unintentionally |
| `choice_shadowing`    | 8          | error    | First alternative of `choice_t` is nullable → later alternatives unreachable |
| `empty_separator`     | 9          | error    | `sep_by<A, Sep>` where `Sep` is nullable → infinite separator loop |
| `choice_first_overlap`| 10         | warning  | Two alternatives of `choice_t` share a FIRST token (order-sensitive in PEG) |

`choice_first_overlap` is a **warning**: overlapping FIRST sets are not always wrong in PEG parsers
(order matters), but they indicate that alternative order is semantically significant and should be
reviewed.

### `validate_grammar<G>()`, `grammar_valid<G>()`, `require_valid_grammar<G>()`

Three entry points for grammar validation — choose based on how you want to handle failures:

```cpp
// Inspect issues programmatically (no hard compile error):
constexpr auto result = validate_grammar<MyGrammar>();
static_assert(result.ok());
// result.issues[0].code      — grammar_diag_code enum value
// result.issues[0].severity  — grammar_issue_severity (error/warning/note)
// result.issues[0].rule      — std::string_view into rule::name_sv static storage

// Pure boolean predicate — never fires static_assert by itself:
static_assert(grammar_valid<MyGrammar>());

// Hard compile error on any error-severity violation (use at grammar definition point):
require_valid_grammar<MyGrammar>();
```

`grammar_valid<G>()` is a **pure consteval bool predicate** and never hard-errors by itself.  
`require_valid_grammar<G>()` performs hard compile-time enforcement via `static_assert`.

### Nullable analysis (`nullable_v<Pattern>`)

| Pattern            | Nullable?                              |
|--------------------|----------------------------------------|
| `tok<K>`           | No                                     |
| `eof`              | No                                     |
| `char_lit<S>`      | Yes iff S is empty                     |
| `token_text<S>`    | No                                     |
| `char_in<Set>`     | No                                     |
| `seq_t<Ms...>`     | Yes iff all `Ms` are nullable          |
| `choice_t<Ms...>`  | Yes iff any `M` is nullable            |
| `opt_t<M>`         | Always yes                             |
| `many_t<M>`        | Always yes (zero matches allowed)      |
| `many1_t<M>`       | Yes iff `M` is nullable                |
| `sep_by_t<A,Sep>`  | Always yes                             |
| `lookahead_t<M>`   | Always yes (no consumption)            |
| `cut`              | Always yes (no consumption)            |
| `rule<N,P>`        | Same as `P`                            |
| `node_t<K,P>`      | Same as `P`                            |
| `recover_with<P,R>`| Same as `P`                            |

### `operator_table_valid<Table>()`

Standalone check for `operator_table` uniqueness:

```cpp
static_assert(operator_table_valid<MyOps>());
```

### `grammar_fingerprint<G>()`

```cpp
template <class G>
consteval lang::descriptor_fingerprint grammar_fingerprint();
```

Stable fingerprint (FNV-1a of rule names + count). Use for cache invalidation when grammar changes
between builds.

---

## FIRST Sets

### `expected_at<Rule, TK>()` — FIRST set for a rule

Returns a `std::array<TK, N>` of all token kinds that can begin `Rule`, sorted by underlying
integer value and deduplicated. Computed entirely `consteval`.

```cpp
constexpr auto arr = expected_at<my_rule, MyTK>();
// arr[0], arr[1], ... are the tokens in FIRST(my_rule)
STATIC_REQUIRE(arr[0] == MyTK::kw_let);
```

Algorithm:
- `tok<K>` → FIRST = `{K}`
- `seq<A,B,...>` → FIRST(A); if A is nullable, also FIRST(B); etc.
- `choice<A,B,...>` → FIRST(A) ∪ FIRST(B) ∪ ...
- `opt<M>` / `many<M>` → FIRST(M) (set; opt/many are nullable themselves)
- `rule<N,P>` → delegates to P

### `first_sets<G>()` — whole-grammar FIRST table

```cpp
constexpr auto fs = first_sets<SimpleGrammar>();
// fs.rule_count                 — number of rules
// fs.descriptors[i].nullable    — whether rule i is nullable
// fs.descriptors[i].token_count — size of FIRST set
// fs.descriptors[i].tokens[j]   — j-th token in FIRST(rule i)
```

Returns a `grammar_first_sets<G>` with one descriptor per rule. All computation is `consteval`.

### Usage: LL(1) conflict checking

```cpp
// Detect choice_shadowing manually if needed:
constexpr auto fs = first_sets<MyGrammar>();
for (std::size_t i = 0; i < fs.rule_count; ++i)
    if (fs.descriptors[i].nullable) { /* root is nullable — may want to check */ }

// Or let validate_grammar<G>() do it:
constexpr auto r = validate_grammar<MyGrammar>();
static_assert(r.ok());
```

---

## FOLLOW Sets

FOLLOW sets identify which tokens can appear **immediately after** a rule successfully matches in any
calling context. They complement FIRST sets for recovery boundaries and error-message construction.

**Algorithm (fixed-point iteration):**
- `FOLLOW(root)` always includes the EOF sentinel (`TK{}` default-constructed value).
- For `seq(A, B, ...)`: `FOLLOW(A) ⊇ FIRST(B, ...)`, propagating through nullable elements.
- **Fixed-point:** iterate until no FOLLOW set grows. For mutually-recursive grammars this is more
  precise than a single pass — the result is the least-fixed-point superset (never misses a token).

### `follow_sets<G>()` — whole-grammar FOLLOW table

```cpp
constexpr auto fs = follow_sets<MyGrammar>();
// fs.rule_count               — number of rules
// fs.entries[i].name          — rule name
// fs.entries[i].has_eof       — true for root rule
// fs.entries[i].tokens[j]     — j-th token in FOLLOW(rule i)
// fs.entries[i].token_count   — size of FOLLOW set
```

Returns `grammar_follow_sets<G>`. All computation is `consteval`.

### `follow_of<G, Rule>()` — FOLLOW set for a specific rule

```cpp
constexpr auto f = follow_of<MyGrammar, my_rule>();
// f is std::array<TK, N> sorted and deduplicated.
// Root rule always includes TK{} (EOF sentinel).
```

### `expected_after<G, Rule>()` — FIRST ∪ FOLLOW

```cpp
constexpr auto ea = expected_after<MyGrammar, my_rule>();
// Union of FIRST(my_rule) and FOLLOW(my_rule) — tokens expected at and after the rule.
// Useful for recovery sync sets and error diagnostics.
```

### Usage: recovery sync set construction

```cpp
// expected_after gives all tokens that safely follow a rule — good sync boundary:
constexpr auto sync_tokens = expected_after<MyGrammar, stmt_rule>();
```

---

## Pratt Expression Parser

### `operator_table<Ops...>`

```cpp
using MyOps = operator_table<
    op<"+",  TK::plus,  10, associativity::left,  fixity::infix>,
    op<"-",  TK::minus,  0, associativity::none,  fixity::prefix>,
    op<"!",  TK::bang,  20, associativity::left,  fixity::postfix>
>;
```

Binding power semantics:
- Left-associative: `rbp = lbp + 1`
- Right-associative: `rbp = lbp`
- Non-associative: `rbp = lbp + 1` (prevents chaining)

### `pratt_expression<Table, PrimaryRule, Action>`

```cpp
template <class Table, class PrimaryRule, class Action = flat_pratt_action>
struct pratt_expression {
    PrimaryRule primary;
    template <class Ctx>
    auto match(Ctx& ctx) const -> parse_result<typename Ctx::stream_type>;
};

template <class Table, class Primary, class Action = flat_pratt_action>
constexpr pratt_expression<Table,Primary,Action> pratt(Primary p);
```

### Action policies

Two built-in action policies control how Pratt parsing emits CST events:

#### `flat_pratt_action` (default)

Emits token events only — no begin/end node wrapping. Expression structure is **not** preserved
as nested CST nodes. Use for simple languages or frontends that re-derive expression structure
after parsing.

```cpp
using cst_pratt_action = flat_pratt_action; // alias for backward compatibility
```

#### `structured_pratt_action<BinaryKind, PrefixKind, PostfixKind>`

Wraps each binary, prefix, and postfix operation in `begin`/`end` node events so operator
precedence is preserved as CST structure with correct tree shape. Infix and postfix nodes
fully wrap their left operand via retroactive `insert_begin_at`. All nodes carry exact
`hull` spans covering their full operand range. Language authors supply their `SyntaxKind`
enum values.

```cpp
template <auto BinaryKind, auto PrefixKind = BinaryKind, auto PostfixKind = BinaryKind>
struct structured_pratt_action;
```

Example — CST for `a + b * c` with `structured_pratt_action`:

```
binary_expr (SK::binary_expr)   ← span covers "a + b * c"
  ident "a"                     ← left operand wrapped inside binary_expr
  binary_expr (SK::binary_expr) ← span covers "b * c"
    ident "b"
    ident "c"
```

Event order for `a + b`:
```
begin_node(binary_expr)    ← retroactively inserted before left operand
  token(a)                 ← left operand
  token(+)                 ← operator
  token(b)                 ← right operand
end_node(binary_expr, span=hull("a","b"))
```

Usage:

```cpp
using MyAction = structured_pratt_action<SK::binary_expr, SK::prefix_expr, SK::postfix_expr>;
using expr_rule = rule<"expr", pratt<MyOps, primary_rule, MyAction>>;
```

AST construction belongs in the language frontend after parsing, not inside Samasa.

---

## Parse Tree (Green/Red)

### Green Tree

Immutable, parentless. Built from the `event_stream` after a successful parse.

**Stage 3 (generic substrate):** `green_tree<SK>` is a thin wrapper over `lang::green_arena<SK>`
(owner: `languages/generic/tree/green_arena.hpp`). Layout is identical — `green_node<SK>` ==
`lang::green_node<SK>`, `green_id` == `lang::arena_id`. Structural hashes are byte-identical to
prior releases (FNV recipe unchanged). The samasa CST is stored as `ir_module<SK, std::monostate>`
in layout terms; no runtime cost.

```cpp
struct green_node<SK> {
    SK          kind;
    byte_span   span;           // offset+length in source
    green_id    first_child;    // index into child_ids (green_id == lang::arena_id)
    uint32_t    child_count;
    uint64_t    structural_hash;
};

// green_tree<SK> inherits lang::green_arena<SK> — same layout.
struct green_tree<SK> : lang::green_arena<SK> {
    static green_tree build(const event_stream<SK>&,
                            const token_stream<TK>&,
                            std::string_view source);     // samasa-flavored entry
    const green_node<SK>& operator[](green_id) const;
    std::span<const green_id> children(green_id) const;
    green_id root() const;
    bool     empty() const;
};

// Free function alias (used internally by samasa.hpp):
// build_green<SK>(events, tokens, source) → green_tree<SK>
```

`structural_hash` enables fast subtree equality and incremental reparse.

### Red Tree — lazy, opt-in

The red tree adds parent pointers to the green tree. **It is not built automatically by `parse<>` —
it is opt-in.** `parse_options::build_red_tree` defaults to `false`.

Build on demand:

```cpp
// build_red_tree=false (default) — no red tree in parse_output
auto output = parse<G>(source);

// Build red tree explicitly when needed:
auto rt = red_tree<SK>::build(output.tree);
// rt.root(), rt[id].parent, rt.parent_of(id)
```

`red_tree<SK>::build()` is O(N) via BFS. The red tree is not cached; build it once and keep the
object alive for the traversal.

```cpp
struct red_node<SK> {
    green_id      green;
    red_id        parent;      // k_null_red for the root
    std::uint32_t child_index;
};

class red_tree<SK> {
    static red_tree build(const green_tree<SK>&);
    red_id  root() const;
    bool    empty() const;
    const red_node<SK>& operator[](red_id) const;
    red_id  parent_of(red_id) const;
    green_id green_of(red_id) const;
};
```

### Event Stream

The parser writes flat events; the green tree is built from them after parse completes.

```cpp
enum class event_kind { begin_node, token, end_node, error, tombstone };

// Stage 3: event_stream<SK> == lang::event_log<SK, samasa_diag_code>
// Owner: languages/generic/tree/event_log.hpp
// All marker/rollback/tombstone logic lives in the generic layer.
struct event_stream<SK> {
    marker begin(SK kind);              // open a node; returns marker
    void   token(uint32_t idx);         // emit a token reference
    void   error(samasa_diag_code, byte_span);
    void   end(marker m, byte_span span);

    marker   snapshot() const;          // O(1) cursor for backtracking
    void     rollback(marker snap);     // truncate or tombstone since snap
    uint32_t event_count() const;
};
```

Backtracking via `snapshot`/`rollback` is O(1).

### Incremental Reparse

`text_edit` now stores `inserted_text` as `std::string` (not `string_view`) to survive the
original source string going out of scope.

```cpp
struct text_edit {
    uint32_t    offset;           // byte offset of the edit start
    uint32_t    removed_length;   // bytes removed at offset
    std::string inserted_text;    // bytes inserted at offset
};

// Apply a text edit to a source string:
std::string apply_edit(std::string_view source, const text_edit& edit);
```

#### `incremental_stats`

Returned by `reparse<G>()` and `diff_trees<G>()` to measure reuse quality:

```cpp
struct incremental_stats {
    uint32_t reused_nodes;      // green nodes with matching structural_hash
    uint32_t rebuilt_nodes;     // green nodes rebuilt
    uint32_t rescanned_tokens;  // tokens re-scanned in affected window
    uint32_t reparsed_tokens;   // tokens processed by reparse
    bool     full_reparse;      // true when conservative full reparse used
};
```

#### `diff_trees<G>(old_output, new_output)` — post-hoc stats

Compare two parse outputs and return reuse statistics. Use after a full reparse to measure
how many nodes could theoretically be reused:

```cpp
auto new_src = apply_edit(old_src, edit);
auto new_out = parse<G>(new_src, opts);
auto stats   = diff_trees<G>(old_out, new_out);
// stats.reused_nodes — how many new nodes share structural_hash with old
```

#### `token_range_for_span(tokens, span)` — byte span → token window

Binary-search the token array for the half-open window `[start, end)` whose byte spans
overlap `span`. Tokens must be offset-ordered (scanner guarantee).

```cpp
lang::token_range tr = token_range_for_span(out.tokens.view(), affected_span);
// tr.start / tr.end: half-open index range; tr.empty() when no overlap
```

#### `find_affected_root<BoundaryPolicy>(tree, edit, policy)` — locate affected subtree

Walk the green tree to find the smallest node fully containing the edit byte range.
The boundary policy can widen the result to a stable reparse boundary.

```cpp
default_reparse_boundary_policy<SK> pol;   // no expansion (default)
auto r = find_affected_root<decltype(pol)>(tree, edit, pol);
// r.id   — arena_id of the affected subtree root (k_null_green if tree empty)
// r.span — byte_span of that subtree
```

Custom policy that always expands to tree root:
```cpp
template <class SK>
struct always_expand_policy {
    static constexpr bool should_expand(SK) noexcept { return true; }
};
```

#### `reparse_window<G, BoundaryPolicy>(old_output, edit, new_source, stats_out, opts, policy)`

Real partial reparse (v2.3). Algorithm:

1. `find_affected_root` → subtree id + byte span in the old tree.
2. Full scan + parse of `new_source` → `new_full` (offset-consistent tokens).
3. Locate corresponding subtree in `new_full.tree` by matching span.
4. Extract sub-arena via DFS event replay (same leaf hash recipe as `build_green` →
   structural hashes are bit-for-bit identical to a full rebuild).
5. Copy old tree, `splice_subtree(affected_id, sub_arena)`, `recompute_ancestor_hashes`.
6. Populate `incremental_stats`.

Degenerate path: when the affected root equals the tree root (edit too broad, or policy
forces root), the function returns `new_full` directly and sets `stats_out.full_reparse = true`.
This is correct, not an error.

**Partial == full invariant:** for any edit, `reparse_window` produces a tree with
structural hashes identical to `parse<G>(new_source)` at every node.

```cpp
incremental_stats st;
auto new_out = reparse_window<G>(old_out, edit, new_src, st);
// st.full_reparse — true only when whole-file fallback was triggered
// st.rebuilt_nodes < old_out.tree.size() for localized edits
```

#### `reparse<G>(old_output, edit, new_source, stats_out)` — thin wrapper

Calls `reparse_window<G, default_reparse_boundary_policy<SK>>`. Existing callers gain
real partial behavior with no signature change.

```cpp
incremental_stats st;
auto new_out = reparse<G>(old_out, edit, new_src, st);
```

#### `default_reparse_boundary_policy<SK>` — customization point

Controls when `find_affected_root` widens the affected node upward. Default: no expansion
(stop at the tightest fully-containing node). Implement `should_expand(SK) → bool` to
change behavior.

```cpp
template <class SK>
struct default_reparse_boundary_policy {
    static constexpr bool should_expand(SK) noexcept { return false; }
};
```

#### `green_fingerprint` — parse identity triple

Identifies a parse result for caching (LSP document cache, snapshot tests):

```cpp
struct green_fingerprint {
    uint64_t grammar_hash;  // grammar_fingerprint<G>()
    uint64_t source_hash;   // FNV-1a of source text
    uint64_t tree_hash;     // structural_hash of root green node

    bool valid() const noexcept;  // true when grammar_hash != 0 && source_hash != 0
    bool operator==(const green_fingerprint&) const noexcept = default;
};

// Build from a parse result:
template <class SK, class TK>
green_fingerprint fingerprint(const parse_output<SK,TK>& output,
                               lang::descriptor_fingerprint grammar_fp,
                               std::string_view source) noexcept;
```

---

### Round-Trip Printing

#### `print_original` — round-trip printer (smoke test)

Reconstructs the original source text from a `token_buffer` and `source` view by concatenating
each token's leading trivia followed by the token text. Use as a round-trip smoke test to verify
that scanner offsets and trivia spans are correct.

```cpp
template <class SK, class TK>
std::string print_original(
    const green_tree<SK>& tree,
    const token_buffer<TK>& tokens,
    std::string_view source);
```

Invariant: `print_original(out.tree, out.tokens, source) == source` for any well-scanned input.

```cpp
auto out     = parse<G>(source);
auto printed = print_original(out.tree, out.tokens, source);
assert(printed == source);  // round-trip smoke test
```

---

## Error Recovery

### Manual strategies (call from rule matchers)

#### `skip_until_sync<SyncSet>`

Advance cursor until a sync token or eof. Emits `recover_skipped` diagnostic.

```cpp
using MySyncSet = sync_set<TK::semicolon, TK::rbrace>;
skip_until_sync<MySyncSet> strategy;
strategy(ctx);  // cursor now at first sync token (or eof)
```

#### `delete_unexpected`

Consume one unexpected token. Emits `recover_deleted` diagnostic.

#### `insert_missing<TK>`

Increment repair count without advancing. Emits `recover_inserted` diagnostic.

#### `wrap_error_node`

Wrap a byte range in an error node in the event stream.

### `recovery_makes_progress_v<Recovery>` — progress guarantee trait

Compile-time predicate: `true` iff the recovery strategy guarantees forward progress (cursor
advance, token insertion, or synchronization). `false` for `wrap_error_node` alone (which emits an
event but does not advance).

```cpp
template <class Recovery>
inline constexpr bool recovery_makes_progress_v<Recovery>;

// Built-in values:
static_assert( recovery_makes_progress_v<skip_until_sync<SyncSet>>); // advances to sync point
static_assert( recovery_makes_progress_v<insert_missing<TK>>);       // synthesizes token
static_assert( recovery_makes_progress_v<delete_unexpected>);         // consumes one token
static_assert(!recovery_makes_progress_v<wrap_error_node>);           // no advance alone
```

Custom strategies can opt in by defining `static constexpr bool makes_progress = true;` inside the
strategy struct. Prevents infinite recovery loops: a strategy that never advances can loop forever
on the same token.

### Declarative recovery: `recover_with<Pattern, Recovery>`

Attach a recovery strategy directly to a pattern via a combinator:

```cpp
// Syntax: recover_with{pattern, recovery_strategy}
auto stmt = make_recover_with(stmt_pattern{}, skip_until_sync<MySyncSet>{});
```

Behavior:
- Pattern **succeeds** → transparent pass-through.
- Pattern **soft_fails** → soft_fail propagated (normal backtracking).
- Pattern **hard_fails** → run `Recovery(ctx)`, emit `recover_wrapped` error event, return **success**
  so parsing continues past the error.

Nullable: same as `Pattern`. Recovery strategy does not affect nullability.

### Scoring-based recovery: `recover_with_repair<Pattern, SyncSet, RepairPolicy>`

Higher-level declarative recovery that tries multiple repair strategies and picks the cheapest:

```cpp
template <class Pattern, class SyncSet,
          class RepairPolicy = default_repair_policy>
struct recover_with_repair {
    // On hard-fail: tries repairs in cost order, picks lowest cost, emits diagnostic
};

// Factory:
auto stmt = make_recover_repair<stmt_pattern, MySyncSet>(stmt_pattern{});
```

`default_repair_policy` ordering: delete unexpected (cost 1) → skip to sync (cost varies).

### `repair_kind` and costs

```cpp
enum class repair_kind : std::uint8_t {
    none, insert_missing, delete_token, replace_token,
    skip_tokens, wrap_subtree, abort_rule
};

constexpr std::uint8_t repair_cost(repair_kind k); // none=0 insert=1 delete=1 replace=2 wrap=4 abort=255
```

### Rich diagnostics: `expected_item` and `parse_diagnostic<MaxExpected>`

```cpp
struct expected_item {
    enum class kind : uint8_t { token_kind, token_text, rule, end_of_file };
    kind            type;
    std::string_view spelling;   // for token_text and rule kinds
    std::uint32_t   token_id;    // for token_kind
};

template <std::size_t MaxExpected = 16>
struct parse_diagnostic {
    expected_item  expected[MaxExpected];
    std::uint32_t  expected_count = 0;
    std::uint32_t  actual_token_pos = 0;
    std::uint32_t  furthest_offset  = 0;
    repair_info    repair;
    std::string    message;

    void add_expected(expected_item item); // silently drops overflow beyond MaxExpected
};
```

### `sync_set<TokenKinds...>`

```cpp
template <auto... TokenKinds>
struct sync_set {
    // Sorted constexpr array of token values; membership is binary_search — O(log N).
    static constexpr bool contains(auto k);
    static constexpr std::size_t size;
};
// sync_set<> (empty) is a specialization whose contains() is a constant false.
```

### Diagnostic Codes

| Code                        | Meaning                                                     |
|-----------------------------|-------------------------------------------------------------|
| `lex_unknown_char`          | Unrecognized character in source                            |
| `lex_unterminated_string`   | String literal without closing quote                        |
| `lex_bad_number`            | Malformed numeric literal                                   |
| `lex_unterminated_comment`  | Block comment without `*/`                                  |
| `parse_unexpected_token`    | Token not expected at this position                         |
| `parse_expected`            | Expected specific token, got other                          |
| `parse_missing`             | Required element absent                                     |
| `parse_depth_exceeded`      | Recursion depth limit hit                                   |
| `parse_node_limit`          | Node count limit hit                                        |
| `recover_deleted`           | Token deleted during error recovery                         |
| `recover_inserted`          | Token inserted during error recovery                        |
| `recover_skipped`           | Tokens skipped during sync recovery                         |
| `recover_wrapped`           | Subtree wrapped in error node                               |
| `recover_repair_limit`      | Max repairs exhausted                                       |
| `grammar_fingerprint_mismatch` | Cached parse used different grammar                      |
| `recover_replace`           | Token replaced during scoring repair (code 25)              |
| `recover_wrap_subtree`      | Subtree wrapped by scoring repair (code 26)                 |

String codes via `to_code()`: `"SAMASA-RECOVER-REPLACE"`, `"SAMASA-RECOVER-WRAP-SUBTREE"`.

---

## Parse Tracing

Tracing is zero-cost when disabled — `no_trace` methods are `constexpr` no-ops; the compiler
eliminates them entirely.

### `no_trace` (default, zero overhead)

```cpp
struct no_trace {
    static constexpr bool enabled = false;

    constexpr void on_event(trace_event) noexcept {}  // no-op
    constexpr void enter(std::string_view, uint32_t) noexcept {}
    constexpr void exit(std::string_view, uint32_t)  noexcept {}
    constexpr void token(uint32_t)                   noexcept {}
    constexpr void fail(std::string_view, uint32_t, bool hard) noexcept {}
    constexpr void cut(std::string_view, uint32_t)   noexcept {}
    constexpr void rollback(std::string_view, uint32_t) noexcept {}
    constexpr void node(uint32_t)                    noexcept {}
};
```

### `collecting_trace`

Accumulates `trace_event` records into a `std::vector`. Enable by passing as a parse policy.

```cpp
enum class trace_event_kind : uint8_t {
    enter_rule, exit_rule, match_token, soft_fail, hard_fail,
    cut, rollback, recover, emit_node
};

struct trace_event {
    trace_event_kind  kind;
    std::string_view  rule_name;  // valid for enter/exit/fail/cut/rollback
    std::uint32_t     token_pos;
};

struct collecting_trace {
    static constexpr bool enabled = true;
    std::vector<trace_event> events;

    std::size_t size() const noexcept;
    void clear() noexcept;

    void enter(std::string_view rule, uint32_t pos);
    void exit(std::string_view rule, uint32_t pos);
    void token(uint32_t pos);
    void fail(std::string_view rule, uint32_t pos, bool hard);
    void cut(std::string_view rule, uint32_t pos);
    void rollback(std::string_view rule, uint32_t pos);
    void node(uint32_t pos);

    void on_event(trace_event e);  // generic event insertion
};
```

### Wire-up in `rule<>` (automatic)

`rule<Name, Pattern>::match(ctx)` checks `if constexpr (requires { ctx.trace(); })` and calls
`enter`/`exit`/`fail` automatically. No changes needed in rule patterns or combinators.

---

## Memoization Policies

### `no_memo` (default)

```cpp
struct no_memo {
    static constexpr bool enabled = false;

    bool lookup(memo_key, memo_value&) const noexcept { return false; }
    void store(memo_key, memo_value)         noexcept {}
    void reserve(std::size_t)                noexcept {}  // constexpr no-op
};
```

Zero overhead. `memoized<Rule>` wrapper detects `enabled=false` at compile time and removes
the cache lookup entirely.

### Key and value types

```cpp
struct memo_key {
    std::uint64_t rule_hash;   // FNV-1a of rule name (consteval)
    std::uint32_t token_pos;
};

struct memo_key_hash {
    std::size_t operator()(memo_key k) const noexcept;  // FNV-style mix
};

struct memo_value {
    parse_status  status;
    std::uint32_t next_pos;
    std::uint32_t furthest_err;
    bool          valid = false;
};
```

### `selective_memo` — selective memoization

Stores results in an `unordered_map`. Use for rules that are frequently re-entered
at the same position (e.g. expression rules in ambiguous grammars).

```cpp
struct selective_memo {
    static constexpr bool enabled = true;
    bool lookup(memo_key k, memo_value& out) const;
    void store(memo_key k, memo_value v);
    void reserve(std::size_t n);  // pre-size table to token count — avoids rehash churn
};
```

### `packrat_memo` — full packrat

Same storage as `selective_memo`; intended for full packrat (memoize every rule invocation).
Guarantees O(n) parse time for PEG grammars at the cost of memory proportional to rule_count × token_count.

```cpp
struct packrat_memo {
    static constexpr bool enabled = true;
    bool lookup(memo_key k, memo_value& out) const;
    void store(memo_key k, memo_value v);
    void reserve(std::size_t n);  // pre-size table — packrat inserts up to rules × positions
};
```

### `memoized<Rule>` — per-rule wrapper

Wraps any rule to add memoization. Uses `if constexpr (requires { ctx.memo(); })` — zero
overhead when the parse context has no memo policy.

```cpp
using memo_expr = memoized<expr_rule>;
// memo_expr::match(ctx) → checks ctx.memo().lookup() before delegating to expr_rule::match
```

---

## Parse Context

`parse_context<SK, TK>` is the parse-time state. It holds references — not copyable.

```cpp
template <class SyntaxKind, class TokenKind, class... Policies>
class parse_context {
public:
    using syntax_kind  = SyntaxKind;
    using token_kind   = TokenKind;
    using stream_type  = token_stream<TokenKind>;
    using cursor_type  = cursor<stream_type>;
    using event_marker = typename event_stream<SyntaxKind>::marker;
    using checkpoint_type = parse_checkpoint<cursor_type, event_marker>;

    parse_context(const token_stream<TK>& stream,
                  std::string_view source,
                  event_stream<SK>& events,
                  collecting_sink<diagnostic>& sink,
                  parse_tree_stats& stats,
                  limits budget = {});

    cursor_type cursor() const;
    void        set_cursor(cursor_type c);

    event_stream<SK>& events();
    void              emit(diagnostic d);

    // Full checkpoint: cursor + event snapshot + diag count + repair count.
    checkpoint_type checkpoint() const;
    void            rollback(const checkpoint_type& cp);

    void     push_depth();
    void     pop_depth();
    bool     over_depth() const;

    uint32_t repairs() const;
    bool     over_repair_limit() const;
    void     inc_repairs();
};
```

Note: `committed()`, `commit()`, `reset_commit()` are **removed**. Cut is seq-local; use
`checkpoint()`/`rollback()` for explicit backtracking.

**In tests, use `std::optional<parse_context<SK,TK>>`** because copy-assign is deleted:

```cpp
std::optional<parse_context<SK,TK>> ctx;
ctx.emplace(stream, source, events, sink, stats);
ctx->cursor(); (*ctx).events();
```

### `limits`

```cpp
struct limits {
    uint32_t max_depth   = 512;
    uint32_t max_nodes   = 1'000'000;
    uint32_t max_repairs = 16;
};
```

---

## Integration Contract

### Implementing a Language Frontend

1. **Define `TK` and `SK` as enum types** — both must satisfy `std::is_enum_v`. No specific
   enumerator value is mandated for EOF; supply it via `scan_token_kinds<TK>`.
2. **Write grammar rules** using DSL combinators and `rule<Name, Pattern>`.
3. **Define `grammar<SK,TK,RootRule,Rules...>`** — root rule first in Rules.
4. **Run `static_assert(grammar_valid<G>())`** at definition time.
5. **Define `keyword_table<>` and `operator_trie<>`** for the scanner.
6. **Choose a `LinePolicy`** (`no_line_sensitivity` default).
7. **Optionally define a `scanner_policy<TK>`** for custom token hooks.
8. **Call `parse<G,...>(source, opts, tok_kinds, lp)`** to get `parse_output`.
9. **Walk `output.tree`** to build your language's AST. Samasa does not do this.
10. **Check `output.diagnostics`** for lex/parse errors.
11. **Build red tree on demand** via `red_tree<SK>::build(output.tree)` when parent traversal is needed.

### What Samasa does NOT do

- Type inference or name resolution
- AST node type assignment or construction
- Vakya/HLIR construction
- Module loading or execution

---

## Tooling

### `describe<G>()` — structured descriptor

```cpp
template <class G>
consteval grammar_description<G> describe();
```

Returns a `grammar_description<G>` with static-storage `string_view` references (no dangling). Safe
to use in consteval contexts.

```cpp
struct grammar_description<G> {
    static constexpr std::string_view name;    // G::root_rule::name_sv
    static constexpr auto             rules;   // std::array<grammar_rule_descriptor, N>
    static constexpr std::size_t      rule_count;
};

struct grammar_rule_descriptor {
    std::string_view name;   // references rule::name_sv static storage
    std::size_t      index;  // 0-based position in rules list
};
```

### `describe_text<G>()` — text summary

```cpp
template <class G>
consteval auto describe_text() -> akshara::fixed_string<N>;
// Returns a fixed_string with "grammar: <name>\n  rule: <name>\n ..."
// N is computed at compile time from rule name lengths.
```

### `railroad_model<G>()` — railroad diagram model

Returns a `grammar_railroad_model<G>` — renderer-neutral structured data with one entry per rule.

```cpp
struct railroad_rule_entry {
    std::string_view name;     // rule name
    std::string_view shape;    // "sequence" | "choice" | "repetition" | "optional" | "separated" | "terminal" | "other"
    bool             nullable; // true if the rule's pattern is nullable
};

struct grammar_railroad_model<G> {
    static constexpr std::size_t               rule_count;
    static constexpr railroad_rule_entry        entries[rule_count];
};

consteval grammar_railroad_model<G> railroad_model();
```

Shape strings map to combinator types:

| Shape         | Pattern type                         |
|---------------|--------------------------------------|
| `"sequence"`  | `seq_t<...>`                         |
| `"choice"`    | `choice_t<...>`                      |
| `"repetition"`| `many_t<>` / `many1_t<>`            |
| `"optional"`  | `opt_t<>`                            |
| `"separated"` | `sep_by_t<>` / `sep_by1_t<>`        |
| `"terminal"`  | `tok<>` / `eof`                      |
| `"other"`     | Any other pattern                    |

### `render_markdown` — Markdown renderers

```cpp
// Grammar rule table:
std::string render_markdown(const grammar_description<G>&);
// Output: "## Grammar: <name>\n| Rule | Index |\n|------|-------|\n..."

// Railroad model table:
std::string render_markdown(const grammar_railroad_model<G>&);
// Output: "## Railroad Model\n| Rule | Shape | Nullable |\n..."

// FIRST sets table:
std::string render_markdown(const grammar_first_sets<G>&);
// Output: "## First Sets\n| Rule | Nullable | FIRST |\n..."
```

All renderers produce valid GitHub-flavored Markdown tables. Safe to embed in docs.

### `render_json` — JSON renderers

```cpp
// Grammar description:
std::string render_json(const grammar_description<G>&);
// {"grammar":"<name>","rules":[{"name":"...","index":N},...]}

// Railroad model:
std::string render_json(const grammar_railroad_model<G>&);
// {"railroad":[{"rule":"...","shape":"...","nullable":true/false},...]}

// FIRST sets:
std::string render_json(const grammar_first_sets<G>&);
// {"first_sets":[{"rule":"...","nullable":true/false,"tokens":[...]},...]}
```

All JSON output is compact (no extra whitespace). String values are escaped.

### `highlight_class`

```cpp
enum class highlight_class : std::uint8_t {
    keyword, identifier, literal, operator_, comment, error, whitespace
};
```

---

## File Reference

| Header                              | Purpose                                              | Depends On                      |
|-------------------------------------|------------------------------------------------------|---------------------------------|
| `samasa.hpp`                        | Umbrella include + `parse<>` + `parse_static<>`      | All submodules                  |
| `samasa_lex.hpp`                    | Lean tokenizer-only aggregate — `core/` + `lex/` only (scanner + tokens; no grammar/tree/expr) | core + lex headers |
| `core/source_view.hpp`              | `file_id`, `source_view`; `byte_span` alias of `lang::byte_span` (owner: `languages/generic/tree/spans.hpp`) | `languages/generic/tree/spans.hpp` |
| `core/cursor.hpp`                   | `cursor<Stream>` — value-type stream pos             | token_stream.hpp                |
| `core/result.hpp`                   | `parse_result<Stream>`, `parse_status`               | cursor.hpp                      |
| `core/diagnostic.hpp`               | `samasa_diag_code` (26 codes), `diagnostic`, `expected_item`, `repair_kind`, `repair_cost`, `repair_info`, `parse_diagnostic<>` | `lang/core/diagnostics.hpp` |
| `core/limits.hpp`                   | `limits` struct                                      | None                            |
| `core/context.hpp`                  | `parse_context<SK,TK>`, `parse_checkpoint<C,M>`      | result.hpp, diagnostic.hpp      |
| `core/parse_output.hpp`             | `parse_output<SK,TK>`, `static_parse_output<>`, `static_parse_event<SK>`, `parse_options`, policy tags; `static_event_stream<SK,N>` = alias of `lang::static_event_buffer<SK,samasa_diag_code,N>` (owner: `languages/generic/tree/static_buffers.hpp`); `static_token_buffer`/`static_diagnostic_sink` backed by `containers::static_vector` | context.hpp, green_tree.hpp, event_stream.hpp |
| `core/parse_options.hpp`            | Re-exports `parse_output.hpp`; `default_parse_options` alias | parse_output.hpp          |
| `lex/token.hpp`                     | `token<TK>`, `trivia_kind`, `trivia`                 | source_view.hpp                 |
| `lex/token_stream.hpp`              | `token_stream<TK>`, `token_buffer<TK>`               | token.hpp                       |
| `lex/keyword_table.hpp`             | `keyword<Name,Kind>`, `keyword_table<>`              | akshara.hpp                     |
| `lex/operator_trie.hpp`             | `operator_token<>`, `operator_trie<>`                | akshara.hpp                     |
| `lex/line_policy.hpp`               | Line sensitivity policies                            | token.hpp                       |
| `lex/scanner.hpp`                   | `scan<>`, `scan_token_kinds<TK>`, `scanner_policy<TK>`, `ascii_identifier_policy`, `unicode_identifier_policy<TK>`, `scanner_mode_id`, `k_scanner_mode_default`, `scanner_mode_stack`, `scanner_view<>` | All lex headers |
| `dsl/matcher.hpp`                   | Matcher concepts                                     | result.hpp                      |
| `dsl/primitive.hpp`                 | `tok<>`, `eof`, `char_lit<>`, `token_text<>`, `char_in<>`, `contextual_keyword<>` | matcher.hpp |
| `dsl/combinators.hpp`               | `seq_t`, `choice_t`, `opt_t`, `many_t`, `cut`, …    | primitive.hpp                   |
| `dsl/rule.hpp`                      | `rule<Name,Pattern>` (with trace integration)        | combinators.hpp                 |
| `dsl/node.hpp`                      | `node_t<Kind,Pattern>`, `node<>()`                   | rule.hpp, event_stream.hpp      |
| `tree/event_stream.hpp`             | `event_stream<SK>` (alias of `lang::event_log<SK, samasa_diag_code>`), `parse_event<SK>`, `event_kind` — owner: `languages/generic/tree/event_log.hpp` | core headers                    |
| `tree/green_tree.hpp`               | `green_tree<SK>` (inherits `lang::green_arena<SK>`), `green_node<SK>`, `green_id`, `k_null_green`, `build_green<SK>()` — owner: `languages/generic/tree/green_arena.hpp` | event_stream.hpp                |
| `tree/red_tree.hpp`                 | `red_tree<SK>`, `red_node<SK>` (lazy, opt-in)        | green_tree.hpp                  |
| `tree/incremental.hpp`              | `text_edit` / `token_range` aliases of `lang::text_edit` / `lang::token_range` (owner: `languages/generic/tree/spans.hpp`); `apply_edit`, `incremental_stats`, `reparse_boundary_policy`, `reparse<G>()`, `diff_trees<G>()` | green_tree.hpp, spans.hpp |
| `grammar/grammar.hpp`               | `grammar<SK,TK,Root,Rules...>` (enum enforcement)    | meta.hpp, type_traits           |
| `grammar/grammar_ir.hpp`            | `nullable_v<>`, `grammar_ir<G>`                      | grammar.hpp, DSL headers, recovery.hpp |
| `grammar/validation.hpp`            | `validate_grammar<G>()`, `grammar_valid<G>()`, `require_valid_grammar<G>()`, `grammar_diag_code` (10 codes), `grammar_validation_result<N>`, `detail::has_empty_sep<>`, `detail::has_shadowed_choice<>` | grammar_ir.hpp |
| `grammar/fingerprint.hpp`           | `grammar_fingerprint<G>()`, `green_fingerprint`, `fingerprint()` | grammar.hpp, akshara.hpp, parse_output.hpp |
| `grammar/expected_sets.hpp`         | `expected_at<Rule,TK>()`, `first_sets<G>()`, `grammar_first_sets<G>` | grammar_ir.hpp |
| `expr/precedence.hpp`               | `associativity`, `fixity`                            | None                            |
| `expr/operator_table.hpp`           | `op<>`, `operator_table<>`                           | precedence.hpp                  |
| `expr/pratt.hpp`                    | `pratt_expression<>`, `flat_pratt_action`, `cst_pratt_action` (alias), `structured_pratt_action<>`, `pratt<>()` | operator_table.hpp, result.hpp  |
| `recovery/sync_set.hpp`             | `sync_set<TokenKinds...>`                            | None                            |
| `recovery/recovery.hpp`             | `skip_until_sync<>`, `delete_unexpected`, `insert_missing<>`, `wrap_error_node`, `recover_with<>`, `recover_with_repair<>`, `default_repair_policy` | sync_set.hpp |
| `policies/memo_policy.hpp`          | `memo_key`, `memo_key_hash`, `memo_value`, `no_memo`, `selective_memo`, `packrat_memo`, `memoized<Rule>` | None |
| `policies/trace_policy.hpp`         | `trace_event_kind`, `trace_event`, `no_trace`, `collecting_trace` | None |
| `policies/error_policy.hpp`         | `error_policy_off`, `error_policy_on`                | None                            |
| `policies/execution_policy.hpp`     | `runtime_execution`, `consteval_execution`           | None                            |
| `tooling/describe.hpp`              | `describe<G>()`, `describe_text<G>()`, `grammar_description<G>`, `grammar_rule_descriptor` | grammar.hpp, akshara.hpp |
| `tooling/highlight.hpp`             | `highlight_class` enum                               | None                            |
| `tooling/railroad.hpp`              | `railroad_rule_entry`, `railroad_node_kind`, `grammar_railroad_model<G>`, `railroad_model<G>()` | grammar_ir.hpp |
| `tooling/render_markdown.hpp`       | `render_markdown(grammar_description<G>)`, `render_markdown(grammar_railroad_model<G>)`, `render_markdown(grammar_first_sets<G>)` | railroad.hpp, expected_sets.hpp, describe.hpp |
| `tooling/render_json.hpp`           | `render_json(grammar_description<G>)`, `render_json(grammar_railroad_model<G>)`, `render_json(grammar_first_sets<G>)` | railroad.hpp, expected_sets.hpp, describe.hpp |

---

## Status

### Implemented (v1)

- PEG grammar definition (`grammar<>`, `rule<>`, all combinators)
- Runtime scanner: `scanner_policy` hook, `keyword_table<>`, `operator_trie<>`, trivia arena, line/block comments, synthetic separator
- Token stream + event stream
- Green tree (immutable, parentless CST) + lazy red tree (opt-in parent pointers)
- `parse_checkpoint` — full atomic backtracking (cursor, event stream, diagnostic sink, repair count)
- Seq-local `cut`
- Pratt parser: `flat_pratt_action`, `structured_pratt_action<>` — infix/postfix nodes fully wrap left operand (exact hull spans; retroactive `insert_begin_at`)
- `recover_with<Pattern,Recovery>` declarative recovery combinator
- `grammar_description<G>`, `describe_text<G>()`
- Grammar validation: `empty_many`, `duplicate_rule` structurally detected; `operator_table_valid<>`
- `nullable_v<Pattern>` compile-time predicate
- `static_parse_output<>` + `parse_static<>` consteval full parse (scan → match → event log → `success`)

### Implemented (v2)

- **FIRST-set computation** — `expected_at<Rule,TK>()` consteval sorted+deduped array; `first_sets<G>()` whole-grammar table
- **FOLLOW-set computation** — `follow_sets<G>()`, `follow_of<G,Rule>()`, `expected_after<G,Rule>()` — **fixed-point iteration** consteval (iterates until no FOLLOW set grows; correct for mutually-recursive grammars)
- **Full grammar validation** — all 12 `grammar_diag_code` values actively checked:
  `empty_many`, `duplicate_rule`, `unreachable_rule` (ref-name walk), `left_recursion` (leftmost element check),
  `unknown_ref`, `duplicate_operator`, `bad_pratt_table`, `nullable_root`, `choice_shadowing`, `empty_separator`,
  `choice_first_overlap` (warning: shared FIRST token between alternatives),
  `recovery_no_progress` (error: `recover_with<P,R>` where R does not advance cursor)
- **`grammar_issue_severity`** — `error`/`warning`/`note` per issue; `ok()` only false on errors; `has_warnings()` for soft issues
- **Pure predicate `grammar_valid<G>()`** — never fires `static_assert`; `require_valid_grammar<G>()` holds all enforcement
- **Parse tracing** — `no_trace` (zero-cost constexpr no-ops), `collecting_trace` (vector of `trace_event`); auto-integrated in `rule<>`
- **Memoization** — `memo_key`/`memo_value`, `no_memo`, `selective_memo`, `packrat_memo`, `memoized<Rule>` wrapper
- **Rich diagnostics** — `expected_item`, `parse_diagnostic<MaxExpected>`, `repair_kind`, `repair_cost()`, `repair_info`; new diag codes `recover_replace=25`, `recover_wrap_subtree=26`
- **Scoring recovery** — `recover_with_repair<Pattern,SyncSet,RepairPolicy>`, `default_repair_policy`
- **`recovery_makes_progress_v<R>`** — compile-time trait; `true` for skip/insert/delete strategies; `false` for `wrap_error_node` alone
- **Railroad model** — `grammar_railroad_model<G>`, `railroad_rule_entry{name, shape, nullable}`, `railroad_model<G>()` consteval
- **Markdown renderer** — `render_markdown()` for grammar description, railroad model, FIRST sets
- **JSON renderer** — `render_json()` for grammar description, railroad model, FIRST sets
- **Green fingerprint** — `green_fingerprint{grammar_hash, source_hash, tree_hash}`, `fingerprint()`, `valid()`
- **Scanner extensions** — `ascii_identifier_policy`, `unicode_identifier_policy<TK>`, `scanner_mode_id`, `scanner_mode_stack`
- **Incremental reparse** — `text_edit`, `apply_edit()`, `incremental_stats`, `diff_trees<G>()`,
  `token_range_for_span()`, `find_affected_root<Policy>()`, `reparse_window<G,Policy>()`, `reparse<G>()`,
  `default_reparse_boundary_policy` (v2.3 partial-window reparse; partial == full invariant holds)
- **Round-trip printing** — `print_original()` (reconstructs source from token_buffer; invariant: output == source)
- **`static_parse_output`** — renamed: `MaxNodes→MaxEvents`, `node_kinds→events`, `node_count→event_count`

### Implemented (v2.4)

- **Conformance suite** — flat test files covering: ordered-choice semantics, checkpoint/rollback,
  FIRST/FOLLOW fixed-point (mutually-recursive grammar proof), recovery behavioral contract,
  memoization policy, lossless round-trip.
- **FOLLOW fixed-point** — verified; doc corrected from "single-pass" to fixed-point iteration.
- **`recovery_no_progress` validation** — `grammar_diag_code::recovery_no_progress` (value 11,
  error severity) wired into `validate_grammar<G>()` and `require_valid_grammar<G>()` via
  `recovery_makes_progress_v`; `recover_with<P,R>` also `static_assert`s progress at instantiation.

### Partial

- `parse_static` full consteval scan+parse execution path (fixed-capacity scaffolding in place; full execution requires constexpr-compatible containers)
- Incremental reparse window — **Implemented (v2.3)**; partial-window rescan with splice + ancestor hash recompute

---

## See Also

- **Akshara:** Compile-time string/charset primitives used by Samasa scanner. (`include/meta/akshara.hpp`)
- **Meta:** TypeList, ct_array, type utilities used by grammar validation. (`include/meta/meta.hpp`)
- **Generic Language Layer:** Diagnostics, identity, parse_stats. (`docs/languages/generic.md`)
- **Language frontends:** Any frontend consumes green/red trees and lowers to its own AST/IR.
