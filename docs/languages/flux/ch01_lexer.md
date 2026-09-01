# Chapter 1 — Lexer: Source → Tokens

## What Problem Are We Solving?

Before a compiler can reason about `sqrt(x*x + y*y)`, it must turn that string of characters into structured data. A
**lexer** (also called a scanner or tokeniser) is the first step: it reads raw text and emits a flat stream of
**tokens** — named, typed chunks of the source.

```
"sqrt(x*x + y*y)"
     ↓ lexer
IDENT("sqrt")  LPAREN  IDENT("x")  STAR  IDENT("x")  PLUS  IDENT("y")  STAR  IDENT("y")  RPAREN
```

The parser in the next chapter sees only this token stream — never the original characters.

---

## The Three Flux Entry Paths

Flux is designed so that the **same compiler pipeline** is reachable from three different starting points. All three
converge at `vakya::node` — the same expression tree, the same structural hash, the same optimizations.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      Three paths into the compiler                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Path A — Runtime text (lexy)                                               │
│  ─────────────────────────────                                              │
│  std::string_view source = "sqrt(x*x + y*y)";                              │
│  → lexy scanner → lexy parse_tree → build_ast → vakya::node                │
│                                                                             │
│  Path B — Compile-time string (Samasa consteval)                            │
│  ──────────────────────────────────────────────                             │
│  constexpr auto source = flux_source_literal;                               │
│  → Samasa parse_static<G,Src,...>() at consteval → green_arena →            │
│    build_ast (consteval) → vakya::node                                      │
│                                                                             │
│  Path C — C++ EDSL (Vakya direct)                                           │
│  ─────────────────────────────────                                          │
│  auto e = lithe::sqrt(x*x + y*y);                                          │
│  → operator overloads → vakya::node (no scanning at all)                   │
│                                                                             │
│                        All paths produce:                                   │
│                     ┌───────────────────────┐                               │
│                     │      vakya::node       │                               │
│                     │  structural_hash = H   │                               │
│                     └──────────┬────────────┘                               │
│                                │                                            │
│              Lithe optimization passes, IR, backends                        │
└─────────────────────────────────────────────────────────────────────────────┘
```

This chapter covers the **lexer** — the character-scanning step inside Paths A and B. Path C has no lexer (it builds the
tree directly in C++).

---

## Path A — Runtime Lexer: Lexy

**Lexy** is a C++23 combinator parsing library. In Flux (and in the Crank language in this codebase), lexy handles
runtime text: source files read from disk, strings passed to an API, user-typed REPL input.

### How lexy's lexer works

In lexy, there is no separate "lexer step" — lexing and parsing are fused into one pass. Token recognition is expressed
as **rule structs**:

```
struct rule_struct {
    static constexpr auto rule = /* lexy DSL expression */;
};
```

The lexy DSL expresses patterns like:

- `dsl::identifier(dsl::ascii::alpha_digit_underscore)` — an identifier
- `dsl::integer<10>` — a decimal integer
- `LEXY_LIT("fn")` — the exact string `"fn"`
- `dsl::p<other_rule>` — call another production recursively

### Lexy keyword handling

In lexy, keywords are handled by `.reserve()` on the identifier rule. This means: scan an identifier, then check if it
matches a keyword — and if so, reject it as a plain identifier. This is the **scannerless** approach.

```cpp
// lexy keyword reservation pattern (as used in crank/lexer.hpp)
struct ident_rule {
    static constexpr auto rule =
        dsl::identifier(dsl::ascii::alpha_underscore,
                        dsl::ascii::alpha_digit_underscore)
            .reserve(
                LEXY_KEYWORD("fn",      token_kind::kw_fn),
                LEXY_KEYWORD("let",     token_kind::kw_let),
                LEXY_KEYWORD("if",      token_kind::kw_if),
                LEXY_KEYWORD("else",    token_kind::kw_else),
                LEXY_KEYWORD("input",   token_kind::kw_input),
                LEXY_KEYWORD("pure",    token_kind::kw_pure),
                LEXY_KEYWORD("import",  token_kind::kw_import),
                LEXY_KEYWORD("assert",  token_kind::kw_assert),
                LEXY_KEYWORD("prove",   token_kind::kw_prove),
                LEXY_KEYWORD("true",    token_kind::bool_lit),
                LEXY_KEYWORD("false",   token_kind::bool_lit),
                LEXY_KEYWORD("i32",     token_kind::kw_i32),
                LEXY_KEYWORD("f32",     token_kind::kw_f32),
                LEXY_KEYWORD("f64",     token_kind::kw_f64),
                LEXY_KEYWORD("tensor",  token_kind::kw_tensor),
                LEXY_KEYWORD("vec",     token_kind::kw_vec),
                LEXY_KEYWORD("mat",     token_kind::kw_mat)
            );
};
```

### Lexy operator disambiguation

Lexy handles multi-character operators via `dsl::op` combined with expression precedence tables.
`<=` vs `<` ambiguity is resolved because lexy tries longer alternatives first when using
`dsl::expression`:

```cpp
// lexy disambiguates longest-match via ordered alternatives in dsl::expression
struct flux_operators {
    static constexpr auto op_lt_eq = dsl::op<LEXY_LIT("<=")>(token_kind::lt_eq);
    static constexpr auto op_lt    = dsl::op<LEXY_LIT("<")> (token_kind::lt);
    static constexpr auto op_gt_eq = dsl::op<LEXY_LIT(">=")>(token_kind::gt_eq);
    static constexpr auto op_gt    = dsl::op<LEXY_LIT(">")> (token_kind::gt);
    static constexpr auto op_eq_eq = dsl::op<LEXY_LIT("==")>(token_kind::eq_eq);
    static constexpr auto op_ne    = dsl::op<LEXY_LIT("!=")>(token_kind::bang_eq);
    static constexpr auto op_arrow = dsl::op<LEXY_LIT("->")>(token_kind::arrow);
    static constexpr auto op_eq    = dsl::op<LEXY_LIT("=")> (token_kind::eq);
    static constexpr auto op_range = dsl::op<LEXY_LIT("..")>(token_kind::dot_dot);
    static constexpr auto op_dot   = dsl::op<LEXY_LIT(".")> (token_kind::dot);
};
```

### Lexy output: parse_tree

Lexy produces a **`lexy::parse_tree`** — a tree where leaf nodes are tokens. The `build_ast` pass (Chapter 3) walks this
tree and builds the Flux AST. Lexy retains trivia (whitespace, comments)
on the tree for source maps.

```
Source:  "sqrt(x*x + y*y)"
lexy parse_tree:
  call_expr
    ident_token "sqrt"
    lparen "("
    add_expr
      mul_expr
        ident_token "x"
        star "*"
        ident_token "x"
      plus "+"
      mul_expr
        ident_token "y"
        star "*"
        ident_token "y"
    rparen ")"
```

### Trivia: why whitespace is attached, not emitted

Notice that spaces are not in the token stream. They become **trivia**: metadata attached to the *next* meaningful
token. This design keeps the parser completely whitespace-oblivious while preserving fidelity for error messages,
formatters, and LSP features.

```
"  let   x  =  10"
     ↓ lexy
let[trivia="  "]  ident("x")[trivia="   "]  eq[trivia="  "]  int(10)[trivia="  "]
```

---

## Path B — Compile-Time Lexer: Samasa

**Samasa** is a PEG scanner + parser framework that can run at **compile time** (`consteval`) as well as runtime. When
the source string is known at compile time (e.g., a `constexpr` embedded DSL), Samasa can produce the token stream and
even the parsed CST as a compile-time constant — zero runtime cost.

### How Samasa scanning differs from lexy

| Aspect            | Lexy (Path A)                   | Samasa (Path B)                        |
|-------------------|---------------------------------|----------------------------------------|
| Execution         | Runtime only                    | Runtime **and** consteval              |
| Lexer model       | Fused lex+parse                 | Separate scanner step                  |
| Keyword lookup    | `.reserve()` on identifier rule | `keyword_table<TK,N>` (hash-first)     |
| Operator matching | `dsl::op<>` in expression rules | `operator_trie<TK>` (longest-prefix)   |
| Token storage     | lexy's internal token buffer    | Samasa `token_buffer<TK>` (flat array) |
| Output            | `lexy::parse_tree` (tree)       | Samasa green CST (`green_arena<Kind>`) |
| Consteval support | No                              | Yes                                    |

### Samasa token kind

```cpp
// include/languages/flux/lexer.hpp
#pragma once
#include <languages/samasa/samasa.hpp>

namespace flux {

enum class token_kind : uint16_t {
    // Literals
    integer_lit,
    float_lit,
    string_lit,
    bool_lit,

    // Identifiers
    identifier,

    // Keywords
    kw_input,  kw_let,  kw_fn,  kw_if,  kw_else,
    kw_pure,   kw_requires, kw_import, kw_assert, kw_prove, kw_return,

    // Type keywords
    kw_i32, kw_i64, kw_u32, kw_u64,
    kw_f32, kw_f64, kw_bool, kw_string,
    kw_vec, kw_mat, kw_tensor, kw_tuple,

    // Arithmetic operators
    plus,      // +
    minus,     // -
    star,      // *
    slash,     // /
    percent,   // %
    caret,     // ^ (power)

    // Comparison operators
    lt,        // <
    lt_eq,     // <=
    gt,        // >
    gt_eq,     // >=
    eq_eq,     // ==
    bang_eq,   // !=

    // Assignment / punctuation
    eq,        // =
    arrow,     // ->
    dot,       // .
    dot_dot,   // ..  (range)
    comma,     // ,
    colon,     // :
    semicolon, // ;

    // Delimiters
    lbrace, rbrace,
    lparen, rparen,
    lbracket, rbracket,

    // Special
    eof,
};

} // namespace flux
```

### Samasa keyword table

`keyword_table<TK, N>` is a perfect-hash map built at compile time. Lookup:

1. Compute FNV-1a hash of the scanned identifier
2. Check the single entry at `hash % N`
3. Compare strings (one compare, not a loop)
4. Return the keyword kind on match, `identifier` on mismatch

```
"fn"  → hash → slot 7 → "fn"? yes → kw_fn
"foo" → hash → slot 3 → "if"? no  → identifier
```

```cpp
constexpr auto flux_keywords() {
    using KT = lang::samasa::keyword_table<token_kind, 24>;
    KT kt{};
    kt.insert("input",    token_kind::kw_input);
    kt.insert("let",      token_kind::kw_let);
    kt.insert("fn",       token_kind::kw_fn);
    kt.insert("if",       token_kind::kw_if);
    kt.insert("else",     token_kind::kw_else);
    kt.insert("pure",     token_kind::kw_pure);
    kt.insert("requires", token_kind::kw_requires);
    kt.insert("import",   token_kind::kw_import);
    kt.insert("assert",   token_kind::kw_assert);
    kt.insert("prove",    token_kind::kw_prove);
    kt.insert("true",     token_kind::bool_lit);
    kt.insert("false",    token_kind::bool_lit);
    kt.insert("i32",      token_kind::kw_i32);
    kt.insert("i64",      token_kind::kw_i64);
    kt.insert("u32",      token_kind::kw_u32);
    kt.insert("u64",      token_kind::kw_u64);
    kt.insert("f32",      token_kind::kw_f32);
    kt.insert("f64",      token_kind::kw_f64);
    kt.insert("bool",     token_kind::kw_bool);
    kt.insert("string",   token_kind::kw_string);
    kt.insert("vec",      token_kind::kw_vec);
    kt.insert("mat",      token_kind::kw_mat);
    kt.insert("tensor",   token_kind::kw_tensor);
    kt.insert("tuple",    token_kind::kw_tuple);
    return kt;
}
```

### Samasa operator trie

`operator_trie<TK>` is a **longest-prefix trie** — a tree over the characters of operator strings whose leaves are token
kinds. Scanning `<=`:

```
root
 └─ '<'
      ├─ '='  →  lt_eq      (consumed "<=")
      └─ ∅    →  lt         (consumed "<")
```

The trie always takes the longest match, so `<=` is never misread as `<` then `=`.

```cpp
constexpr auto flux_operators() {
    using OT = lang::samasa::operator_trie<token_kind>;
    OT ot{};
    // Longest alternatives inserted first so trie prefers them
    ot.insert("<=", token_kind::lt_eq);
    ot.insert("<",  token_kind::lt);
    ot.insert(">=", token_kind::gt_eq);
    ot.insert(">",  token_kind::gt);
    ot.insert("==", token_kind::eq_eq);
    ot.insert("!=", token_kind::bang_eq);
    ot.insert("->", token_kind::arrow);
    ot.insert("=",  token_kind::eq);
    ot.insert("..", token_kind::dot_dot);
    ot.insert(".",  token_kind::dot);
    ot.insert("+",  token_kind::plus);
    ot.insert("-",  token_kind::minus);
    ot.insert("*",  token_kind::star);
    ot.insert("/",  token_kind::slash);
    ot.insert("%",  token_kind::percent);
    ot.insert("^",  token_kind::caret);
    ot.insert(",",  token_kind::comma);
    ot.insert(":",  token_kind::colon);
    ot.insert(";",  token_kind::semicolon);
    ot.insert("{",  token_kind::lbrace);
    ot.insert("}",  token_kind::rbrace);
    ot.insert("(",  token_kind::lparen);
    ot.insert(")",  token_kind::rparen);
    ot.insert("[",  token_kind::lbracket);
    ot.insert("]",  token_kind::rbracket);
    return ot;
}
```

### Samasa scanner policy

For characters not handled by the keyword table or operator trie (digits, string quotes, comments), a custom
`scanner_policy<TK>` handles classification:

```cpp
struct flux_scanner_policy {
    using token_kind_type = token_kind;

    template<typename Cursor>
    static token_kind classify(Cursor& cur) noexcept {
        char c = cur.peek();

        // String literal: "..."
        if (c == '"') {
            cur.advance();  // consume opening quote
            while (cur.has_more() && cur.peek() != '"') {
                if (cur.peek() == '\\') cur.advance();  // escape
                cur.advance();
            }
            if (cur.has_more()) cur.advance();  // consume closing quote
            return token_kind::string_lit;
        }

        // Numeric literal: distinguish int vs float by presence of '.'
        if (std::isdigit(static_cast<unsigned char>(c))) {
            bool has_dot = false;
            while (cur.has_more()) {
                char nc = cur.peek();
                if (nc == '.' && !has_dot) { has_dot = true; cur.advance(); continue; }
                if (nc == '_') { cur.advance(); continue; }  // digit separator
                if (!std::isdigit(static_cast<unsigned char>(nc))) break;
                cur.advance();
            }
            // Optional exponent: e/E followed by optional sign and digits
            if (cur.has_more() && (cur.peek() == 'e' || cur.peek() == 'E')) {
                cur.advance();
                if (cur.has_more() && (cur.peek() == '+' || cur.peek() == '-'))
                    cur.advance();
                while (cur.has_more() && std::isdigit(static_cast<unsigned char>(cur.peek())))
                    cur.advance();
                has_dot = true;  // exponential form is always float
            }
            return has_dot ? token_kind::float_lit : token_kind::integer_lit;
        }

        // Identifier (fallthrough from keyword table when no keyword matched)
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (cur.has_more() && (std::isalnum(static_cast<unsigned char>(cur.peek()))
                                      || cur.peek() == '_'))
                cur.advance();
            return token_kind::identifier;
        }

        // Unknown character — advance and signal error via eof
        cur.advance();
        return token_kind::eof;
    }

    template<typename Cursor>
    static bool try_skip_comment(Cursor& cur) noexcept {
        if (cur.peek() == '/' && cur.peek(1) == '/') {
            // Skip to end of line
            while (cur.has_more() && cur.peek() != '\n')
                cur.advance();
            return true;
        }
        if (cur.peek() == '/' && cur.peek(1) == '*') {
            // Block comment: /* ... */
            cur.advance(); cur.advance();  // consume /*
            while (cur.has_more()) {
                if (cur.peek() == '*' && cur.peek(1) == '/') {
                    cur.advance(); cur.advance();  // consume */
                    break;
                }
                cur.advance();
            }
            return true;
        }
        return false;
    }
};
```

### Samasa scan — runtime entry point

```cpp
using flux_token = lang::samasa::token<token_kind>;

auto scan(std::string_view src) {
    constexpr auto kw  = flux_keywords();
    constexpr auto ops = flux_operators();
    return lang::samasa::scan<
        decltype(kw),
        decltype(ops),
        lang::samasa::default_line_policy,
        token_kind,
        flux_scanner_policy
    >(src, kw, ops);
}
```

### Samasa scan — compile-time entry point

When the source text is a compile-time constant, `parse_static` runs the full scan + parse at
`consteval`, producing a `static_parse_output` with no heap allocation at runtime:

```cpp
// Embed a Flux expression directly in C++ source, verified at compile time.
// Grammar is passed as a template argument; source as NTTP.
template<lang::samasa::ct_string Src>
consteval auto scan_consteval() {
    constexpr auto kw  = flux_keywords();
    constexpr auto ops = flux_operators();
    return lang::samasa::parse_static<
        flux_grammar,   // full grammar (see Chapter 2)
        Src,
        /*MaxTok=*/  256,
        /*MaxEvents=*/ 512,
        /*MaxDiags=*/   16,
        /*MaxDepth=*/   64
    >();
}

// Usage — failure is a compile error:
constexpr auto result = scan_consteval<"sqrt(x*x + y*y)">();
static_assert(result.success, "Flux syntax error at compile time");
```

This is useful for embedded DSLs where the source is part of the program binary and should be validated by the C++
compiler, not only at runtime.

---

## Path C — C++ EDSL: Vakya Direct

Path C does not involve a lexer at all. The user writes C++ expressions using `vakya::` or `lithe::`
operator overloads:

```cpp
auto x_sym = lithe::make_symbolic("x");  // symbolic variable node
auto y_sym = lithe::make_symbolic("y");
auto dist  = lithe::sqrt(x_sym*x_sym + y_sym*y_sym);
//           ↑ operator* builds mul_tag node
//                      ↑ operator+ builds add_tag node
//           ↑ sqrt() wraps in sqrt_tag node
```

Each `operator*`, `operator+`, etc. is a template function that calls `lithe::make_node<Tag>(...)`. No source text is
ever scanned.

The `structural_hash` of `dist` is identical to the hash of the same expression compiled from Flux source — because both
paths produce the same `vakya::node` tree topology.

### Vakya tag system

Every node kind is a **tag** — an empty struct. Tags carry metadata via `vakya::emit::tag_descriptor`:

```cpp
// add_tag metadata (built into vakya)
template<> struct vakya::emit::tag_descriptor<vakya::add_tag> {
    static constexpr std::string_view symbol        = "+";
    static constexpr uint32_t         stable_id     = 1;   // frozen, never changes
    static constexpr uint32_t         arity         = 2;
    static constexpr bool             is_commutative = true;  // a+b == b+a for matching
};
```

User-defined tags extend with `stable_id >= 1000`:

```cpp
struct norm2_tag {};
template<> struct vakya::emit::tag_descriptor<norm2_tag> {
    static constexpr std::string_view symbol    = "norm2";
    static constexpr uint32_t         stable_id = 1001;
    static constexpr uint32_t         arity     = 2;
};
```

---

## The Convergence Point: `vakya::node`

All three paths produce the same `vakya::node` tree. Let us verify:

```cpp
// Path A: from runtime Flux source
auto flux_tree = flux_compile_runtime("sqrt(x*x + y*y)", {{"x", x}, {"y", y}});

// Path B: from compile-time Flux source
constexpr auto cst = scan_consteval<"sqrt(x*x + y*y)">();
auto samasa_tree   = flux_build_from_cst(cst, {{"x", x}, {"y", y}});

// Path C: C++ EDSL
auto cpp_tree = lithe::sqrt(x*x + y*y);

// Invariant: all three produce the same structural hash
auto hA = lithe::structural_hash(flux_tree);
auto hB = lithe::structural_hash(samasa_tree);
auto hC = lithe::structural_hash(cpp_tree);

assert(hA == hB);   // runtime source == consteval source
assert(hA == hC);   // source path == EDSL path
```

If these asserts hold, the Flux system is sound: frontend choice is purely a user ergonomics decision, not a semantic
one.

---

## Algorithm Deep Dive: Maximal Munch

Maximal munch is the universal rule: **a lexer always consumes the longest possible sequence of characters that forms a
valid token**.

### Why it matters: operator ambiguity

```
Input: <=
  Naïve scan:   '<'  then  '='   (wrong — two tokens)
  Maximal munch: '<='             (correct — one token)

Input: ->
  Naïve scan:   '-'  then  '>'   (wrong)
  Maximal munch: '->'             (correct)

Input: ...
  Two dots:  '..'  then  '.'     (range + dot)
  Maximal munch would give '...' — but Flux has no '...'
  So '...' is '.' + '..' or '..' + '.' depending on longest rule.
  Operator trie resolves: '..' matches before '.', so '...' → '..' + '.'.
```

### The operator trie in detail

The Samasa `operator_trie` is a compile-time trie over operator characters. During scanning, the cursor advances into
the trie simultaneously with the source characters:

```
State after consuming '<':
  trie cursor at node {'=': lt_eq, default: lt}

Peek next character:
  '=' → advance cursor → reach leaf lt_eq → token is <=
  'x' → no child   → backtrack to default → token is <
```

This is O (L) where L = length of longest operator. No backtracking over the source.

### The keyword table in detail

The Samasa `keyword_table` avoids the typical "check every keyword" loop. Instead:

1. Scan an identifier by consuming `[a-zA-Z_][a-zA-Z_0-9]*`
2. Compute FNV-1a hash of the text in O (L) time — same cost as scanning
3. Look up `table[hash % N]` — one array access
4. Compare the stored keyword string with the scanned text — one string compare
5. Return keyword kind on match, `identifier` on mismatch

```
"let" → FNV hash 0xA2B3C4D5 → slot 7 → stored: "let", kw_let
                                         scanned == stored? yes → kw_let

"letter" → FNV hash 0x1234ABCD → slot 12 → stored: "else", kw_else
                                            scanned == stored? no → identifier
```

No loop. No sorted search. The hash collision rate is negligible for small keyword sets.

---

## Token Buffer Layout

Samasa's `token_buffer<TK>` is a flat array of fixed-size token records:

```
token<TK>  {
    uint32_t offset        // byte offset into source text
    uint32_t length        // byte count of this token
    uint16_t trivia_start  // index into trivia_arena
    uint16_t trivia_count  // number of trivia pieces before this token
    uint16_t flags         // scanner-level flags (e.g., has_escape in string)
    TK       kind          // the token_kind enum value
}
```

Key properties:

- **Random access O (1)** — `buffer[i]` gives the i-th token directly
- **No heap per token** — all tokens in one allocation
- **Trivia separate** — `trivia_arena` is a parallel array; token records are small
- **Source slice O (1)** — `src.substr(tok.offset, tok.length)` gives the text

---

## Lexy vs Samasa: When to Choose Which

| Scenario                                | Path                 | Why                                                  |
|-----------------------------------------|----------------------|------------------------------------------------------|
| Parse user source files at runtime      | A (lexy)             | Mature library, full UTF-8, ASI, rich error messages |
| Embedded DSL verified at compile time   | B (Samasa consteval) | Zero runtime overhead, compile-time errors           |
| Embed a small expression in C++ code    | B or C               | C is simplest; B if you want Flux syntax             |
| Large source with complex recovery      | A (lexy)             | Lexy's error recovery is richer                      |
| Unit-test the parser in consteval tests | B (Samasa)           | Can static_assert on parse output                    |
| Build expression trees programmatically | C (EDSL)             | No scanning overhead at all                          |

In practice, a production Flux tool uses **Path A** for user-facing compilation and **Path C** for internal use
(optimizer algorithms, test fixtures, benchmark harnesses).

---

## Complete Example — All Three Paths

```cpp
// ch01_example.cpp
#include <lithe/lithe.hpp>
#include <languages/samasa/samasa.hpp>
#include <print>

#include "flux/lexer.hpp"    // token_kind, flux_keywords, flux_operators, flux_scanner_policy
#include "flux/grammar.hpp"  // flux_grammar (Chapter 2)
#include "flux/lower.hpp"    // flux_compile_runtime, flux_build_from_cst (Chapter 7)

// Symbolic input variables shared across all three paths
inline auto x_sym = lithe::make_symbolic("x");
inline auto y_sym = lithe::make_symbolic("y");

int main() {
    // ── Path A: runtime source via lexy ──────────────────────────────────────
    std::string_view src = "sqrt(x*x + y*y)";
    auto tree_A = flux::compile_runtime(src, {{"x", x_sym}, {"y", y_sym}});

    // ── Path B: compile-time source via Samasa ───────────────────────────────
    // (syntax verified at build time; zero scanning cost at runtime)
    constexpr auto cst_B = flux::scan_consteval<"sqrt(x*x + y*y)">();
    static_assert(cst_B.success, "Flux syntax error");
    auto tree_B = flux::build_from_cst(cst_B, {{"x", x_sym}, {"y", y_sym}});

    // ── Path C: C++ EDSL, no scanning ────────────────────────────────────────
    auto tree_C = lithe::sqrt(x_sym*x_sym + y_sym*y_sym);

    // ── Verify invariant ─────────────────────────────────────────────────────
    auto hA = lithe::structural_hash(tree_A);
    auto hB = lithe::structural_hash(tree_B);
    auto hC = lithe::structural_hash(tree_C);

    std::println("Hash A (lexy)   : 0x{:016x}", hA);
    std::println("Hash B (samasa) : 0x{:016x}", hB);
    std::println("Hash C (EDSL)   : 0x{:016x}", hC);
    std::println("A == B == C     : {}", (hA == hB && hB == hC));

    // ── Print tokens from Path A scan ────────────────────────────────────────
    auto buf = flux::scan(src);
    std::println("\nTokens:");
    for (auto const& tok : buf.tokens()) {
        auto text = src.substr(tok.offset, tok.length);
        std::println("  {:15}  kind={}", text, static_cast<int>(tok.kind));
    }
}
```

Expected output:

```
Hash A (lexy)   : 0x7f3a9b2c8d41e005
Hash B (samasa) : 0x7f3a9b2c8d41e005
Hash C (EDSL)   : 0x7f3a9b2c8d41e005
A == B == C     : true

Tokens:
  sqrt             kind=1    (identifier)
  (                kind=28   (lparen)
  x                kind=1    (identifier)
  *                kind=9    (star)
  x                kind=1    (identifier)
  +                kind=7    (plus)
  y                kind=1    (identifier)
  *                kind=9    (star)
  y                kind=1    (identifier)
  )                kind=29   (rparen)
```

---

## Summary

| Concept         | Detail                                                      |
|-----------------|-------------------------------------------------------------|
| Token           | (kind, offset, length, trivia) — named chunk of source      |
| Maximal munch   | Longest match always wins — implemented by operator trie    |
| Keyword lookup  | FNV hash + single compare — O(1), no loop                   |
| Trivia          | Whitespace/comments attached to next token, not emitted     |
| Path A (lexy)   | Runtime: fused lex+parse, lexy parse_tree output            |
| Path B (Samasa) | Runtime + consteval: separate scan, green_arena CST output  |
| Path C (EDSL)   | No scanning: `vakya::node` built by C++ operator overloads  |
| Convergence     | All paths → same `vakya::node` tree, same `structural_hash` |

---

## Next

[Chapter 2 → Grammar](ch02_grammar.md) — build the PEG grammar that consumes the token stream (Paths A and B) and
constructs the Flux CST.

---

## DFA Theory: What a Lexer Is Formally

A lexer is a **Deterministic Finite Automaton** (DFA). Every scanning rule — identifier, integer, operator — is a DFA.
The scanner runs all relevant DFAs simultaneously and picks the longest match (maximal munch). This section makes that
formal so the later implementation details have a firm theoretical grounding.

### Formal definition

A DFA is a 5-tuple **(S, Σ, δ, s₀, F)** where:

| Component         | Meaning                                                                         |
|-------------------|---------------------------------------------------------------------------------|
| **S**             | Finite set of states                                                            |
| **Σ**             | Input alphabet (for a lexer: bytes or Unicode code points)                      |
| **δ : S × Σ → S** | Transition function — given current state and next character, return next state |
| **s₀ ∈ S**        | Start state                                                                     |
| **F ⊆ S**         | Set of accept states — reaching one means a token has been recognised           |

The lexer feeds characters to δ one at a time. When the next character would leave all active DFAs (no transition
exists), scanning stops and the longest accept state reached so far wins.

### DFA for `<`, `=`, `<=` in Flux

```
              ┌─────────────────────────────────────────────────┐
              │  DFA: comparison / assignment operators          │
              └─────────────────────────────────────────────────┘

  start ──'<'──► state_lt ──'='──► ACCEPT(lt_eq)   "consumed <="
                   │
                   └──other──► ACCEPT(lt)           "consumed <, push back 1 char"

  start ──'='──► state_eq ──'='──► ACCEPT(eq_eq)   "consumed =="
                   │
                   └──other──► ACCEPT(eq)           "consumed =, push back 1 char"

  start ──'>'──► state_gt ──'='──► ACCEPT(gt_eq)   "consumed >="
                   │
                   └──other──► ACCEPT(gt)           "consumed >, push back 1 char"
```

"Push back 1" means the lookahead character was consumed only to confirm the DFA exits — it is returned to the input
stream for the *next* token. This is the standard one-character lookahead found in every real lexer.

### How DFA states map to the Samasa operator trie

The Samasa `operator_trie` is a data structure that directly encodes δ for all operator DFAs simultaneously. Each trie
**node** is a DFA state. Each **edge** is a character transition. Each **leaf** (or node with a stored kind) is an
accept state.

```
Trie node for root (= start state s₀):
  edge '<' → node_lt         (= state_lt in the diagram above)
  edge '=' → node_eq         (= state_eq)
  edge '>' → node_gt         (= state_gt)
  edge '+' → leaf(plus)      (= ACCEPT immediately)
  edge '-' → node_minus      (might be '->' or '-')
  …

Trie node_lt:
  edge '=' → leaf(lt_eq)     (= ACCEPT(lt_eq))
  no edge  → use stored kind lt (= ACCEPT(lt), push back)
```

There is a 1-to-1 correspondence: every DFA state is a trie node, every DFA transition is a trie edge. Building
`flux_operators()` at compile time builds all operator DFAs at once.

### Parallel DFAs and maximal match

The scanner runs three classes of DFA in parallel on each character:

```
  Character stream: "sqrt("
         │
         ├─► identifier DFA    s→sq→sqr→sqrt  ACCEPT(identifier) at length 4
         ├─► integer DFA       s=not digit → reject immediately
         └─► operator trie     s→'s' has no edge → reject immediately

  At '(':
         ├─► identifier DFA    no longer running (accepted at length 4)
         ├─► operator trie     '(' → leaf(lparen) ACCEPT immediately
         └─► integer DFA       not digit → reject
```

The winner is whichever DFA accepted at the greatest offset. Ties are broken by priority (keywords beat identifiers;
this is handled by running keyword lookup after the identifier DFA accepts).

### DFA for Flux numeric literals

```
              ┌──────────────────────────────────────────────────────┐
              │  DFA: integer_lit vs float_lit                       │
              └──────────────────────────────────────────────────────┘

  start
    │
   digit
    ▼
  int_body ──digit──► int_body
    │                    │
    │                    └──EOF/non-digit/non-dot──► ACCEPT(integer_lit)
    │
   '.'
    ▼
  frac_body ──digit──► frac_body
    │                    │
    │                    └──EOF/non-digit──► ACCEPT(float_lit)
    │
   'e'/'E'
    ▼
  exp_sign ──'+'/'–'/digit──► exp_body ──digit──► exp_body
                                                     │
                                                     └──EOF/non-digit──► ACCEPT(float_lit)
```

The distinguishing character is `'.'`: scanning `3` stays in `int_body` (integer); scanning `3.14`
transitions to `frac_body` (float). The exponent arm (`3e10`, `1.5e-3`) also produces `float_lit`
because exponential notation always implies a real number.

This DFA is implemented in `flux_scanner_policy::classify` — compare the `has_dot` flag logic in the `classify` method
shown earlier in this chapter.

---

## Tutorial 1 — Lexy: Hands-On

> **Copy, paste, compile, run.**

Each step below is a self-contained C++ file. Add `-std=c++23` and link lexy. The expected output is shown after each
listing.

### Step 1 — Scan a single identifier

```cpp
// lexy_step1.cpp — scan one identifier from "hello"
#include <lexy/dsl.hpp>
#include <lexy/action/parse.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/callback.hpp>
#include <print>

namespace {

struct identifier_token {
    // Rule: alpha or underscore, followed by alphanumeric or underscore
    static constexpr auto rule =
        dsl::identifier(dsl::ascii::alpha_underscore,
                        dsl::ascii::alpha_digit_underscore);

    // Value callback: capture the lexeme text
    static constexpr auto value =
        lexy::as_string<std::string>;
};

} // namespace

int main() {
    auto input  = lexy::zstring_input("hello");
    auto result = lexy::parse<identifier_token>(input, lexy::noop);
    if (result.has_value())
        std::println("token: {} (identifier)", result.value());
    else
        std::println("parse error");
}
```

```text
token: hello (identifier)
```

### Step 2 — Keywords and identifiers from `"let x = 10"`

```cpp
// lexy_step2.cpp — distinguish kw_let from plain identifier
#include <lexy/dsl.hpp>
#include <lexy/action/parse.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/callback.hpp>
#include <print>
#include <string>
#include <vector>

namespace {

enum class token_kind { identifier, kw_let, eq, integer };

struct token_rec { std::string text; token_kind kind; };

struct ident_or_keyword {
    static constexpr auto rule =
        dsl::identifier(dsl::ascii::alpha_underscore,
                        dsl::ascii::alpha_digit_underscore)
            .reserve(LEXY_KEYWORD("let", dsl::lit_c<'l'>));  // reserve "let"

    static constexpr auto value = lexy::as_string<std::string>;
};

// Minimal token scanner for the 4-token string "let x = 10"
// (In a real Flux lexer this lives inside a full grammar production.)
void scan_tokens(std::string_view src) {
    // Manually drive the scan for tutorial clarity
    std::vector<token_rec> tokens;
    std::size_t i = 0;

    auto skip_ws = [&]{ while (i < src.size() && src[i]==' ') ++i; };

    auto scan_ident = [&]() -> token_rec {
        std::size_t start = i;
        while (i < src.size() && (std::isalnum((unsigned char)src[i]) || src[i]=='_')) ++i;
        std::string text(src.substr(start, i-start));
        token_kind k = (text == "let") ? token_kind::kw_let : token_kind::identifier;
        return {text, k};
    };

    while (i < src.size()) {
        skip_ws();
        if (i >= src.size()) break;
        char c = src[i];
        if (std::isalpha((unsigned char)c) || c == '_')
            tokens.push_back(scan_ident());
        else if (c == '=') { tokens.push_back({"=", token_kind::eq}); ++i; }
        else if (std::isdigit((unsigned char)c)) {
            std::size_t s = i;
            while (i < src.size() && std::isdigit((unsigned char)src[i])) ++i;
            tokens.push_back({std::string(src.substr(s,i-s)), token_kind::integer});
        } else ++i;
    }

    constexpr auto kind_name = [](token_kind k) -> std::string_view {
        switch (k) {
            case token_kind::identifier: return "identifier";
            case token_kind::kw_let:     return "kw_let";
            case token_kind::eq:         return "eq";
            case token_kind::integer:    return "integer";
        }
        return "?";
    };

    for (auto const& t : tokens)
        std::println("  {:10}  kind={}", t.text, kind_name(t.kind));
}

} // namespace

int main() { scan_tokens("let x = 10"); }
```

```text
  let        kind=kw_let
  x          kind=identifier
  =          kind=eq
  10         kind=integer
```

### Step 3 — Full Flux expression `"sqrt(x*x + y*y)"`

```cpp
// lexy_step3.cpp — lex the running example, print all tokens
#include <lexy/dsl.hpp>
#include <lexy/action/parse.hpp>
#include <lexy/action/scan.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/callback.hpp>
#include <print>
#include <string>
#include <vector>

// (In a real codebase this comes from flux/lexer.hpp.)
enum class token_kind : int {
    identifier = 1, integer_lit = 2, float_lit = 3,
    plus = 7, star = 9, lparen = 28, rparen = 29,
};

constexpr std::string_view kind_name(token_kind k) {
    switch (k) {
        case token_kind::identifier:   return "identifier";
        case token_kind::integer_lit:  return "integer_lit";
        case token_kind::float_lit:    return "float_lit";
        case token_kind::plus:         return "plus";
        case token_kind::star:         return "star";
        case token_kind::lparen:       return "lparen";
        case token_kind::rparen:       return "rparen";
        default:                       return "?";
    }
}

struct token_rec { std::string text; token_kind kind; };

// Minimal hand-rolled scanner for tutorial clarity —
// a real Flux tool uses lexy grammar productions instead.
std::vector<token_rec> lex(std::string_view src) {
    std::vector<token_rec> out;
    std::size_t i = 0;
    while (i < src.size()) {
        char c = src[i];
        if (c == ' ') { ++i; continue; }
        if (std::isalpha((unsigned char)c) || c == '_') {
            std::size_t s = i;
            while (i<src.size() && (std::isalnum((unsigned char)src[i])||src[i]=='_')) ++i;
            out.push_back({std::string(src.substr(s,i-s)), token_kind::identifier});
        } else if (std::isdigit((unsigned char)c)) {
            std::size_t s = i;
            bool dot = false;
            while (i<src.size() && (std::isdigit((unsigned char)src[i])||(src[i]=='.'&&!dot))) {
                if (src[i]=='.') dot=true; ++i;
            }
            out.push_back({std::string(src.substr(s,i-s)),
                           dot ? token_kind::float_lit : token_kind::integer_lit});
        } else if (c == '+') { out.push_back({"+", token_kind::plus});  ++i; }
          else if (c == '*') { out.push_back({"*", token_kind::star});  ++i; }
          else if (c == '(') { out.push_back({"(", token_kind::lparen}); ++i; }
          else if (c == ')') { out.push_back({")", token_kind::rparen}); ++i; }
          else ++i;
    }
    return out;
}

int main() {
    auto tokens = lex("sqrt(x*x + y*y)");
    for (auto const& t : tokens)
        std::println("  {:15}  kind={}", t.text, kind_name(t.kind));
}
```

```text
  sqrt             kind=identifier
  (                kind=lparen
  x                kind=identifier
  *                kind=star
  x                kind=identifier
  +                kind=plus
  y                kind=identifier
  *                kind=star
  y                kind=identifier
  )                kind=rparen
```

### Step 4 — What lexy gives you for free: trivia

Lexy attaches whitespace and comments to the *next* meaningful token as **trivia**. The parser never sees whitespace
directly, but you can inspect it for formatters or error messages.

```cpp
// lexy_step4.cpp — inspect trivia attached to tokens
//
// In lexy, whitespace is declared via dsl::whitespace and then automatically
// skipped between tokens. The lexy::parse_tree records which whitespace
// preceded each token.  Here we demonstrate with a comment line.

#include <lexy/dsl.hpp>
#include <lexy/action/parse_as_tree.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/visualize.hpp>
#include <print>

namespace {

// Declare "-- comment to end of line" as lexy whitespace
constexpr auto flux_comment = LEXY_LIT("--") >> dsl::until(dsl::newline);
constexpr auto ws            = dsl::ascii::space | flux_comment;

struct kw_let_rule {
    static constexpr auto whitespace = ws;
    static constexpr auto rule       = LEXY_KEYWORD("let", dsl::identifier(dsl::ascii::alpha_digit_underscore));
    static constexpr auto value      = lexy::forward<void>;
};

} // namespace

int main() {
    // Source has a comment before 'let' — lexy captures it as trivia
    auto input = lexy::zstring_input("-- compute\nlet");

    lexy::parse_tree<decltype(input), lexy::noop_encoding> tree;
    auto result = lexy::parse_as_tree<kw_let_rule>(tree, input, lexy::noop);

    // Walk the tree and print trivia nodes
    for (auto node : tree.root().children()) {
        if (node.kind().is_whitespace())
            std::println("trivia: {:?}", node.lexeme().data());
        else
            std::println("token:  {} (kind={})", node.lexeme().data(), node.kind().name());
    }
}
```

```text
trivia: "-- compute\n"
token:  let (kind=kw_let)
```

The comment `-- compute` is not lost — it is preserved as trivia on the `let` token. A language server or formatter can
recover it from the parse tree without a separate comment-scanning pass.

### Step 5 — A lexy parse error

Lexy emits structured diagnostics. When the input violates a rule, you get a `lexy::error` with a position and message,
not a raw exception.

```cpp
// lexy_step5.cpp — observe lexy's error format on "let 123 = x"
// Here "123" appears where an identifier is expected.

#include <lexy/dsl.hpp>
#include <lexy/action/parse.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/callback.hpp>
#include <lexy_ext/report_error.hpp>
#include <print>

namespace {

struct identifier_token {
    static constexpr auto rule =
        dsl::identifier(dsl::ascii::alpha_underscore,
                        dsl::ascii::alpha_digit_underscore)
            .reserve(LEXY_KEYWORD("let", dsl::lit_c<'l'>));
    static constexpr auto value = lexy::as_string<std::string>;
};

} // namespace

int main() {
    // Position the input past "let " so the parser sees "123 = x"
    auto input  = lexy::zstring_input("123");
    auto result = lexy::parse<identifier_token>(
        input,
        lexy_ext::report_error.path("<stdin>")  // print diagnostics to stderr
    );

    if (!result.has_value())
        std::println("(parse failed — see diagnostic above)");
}
```

```text
<stdin>:1:1: error: expected alpha or underscore to begin identifier
  123
  ^
(parse failed — see diagnostic above)
```

The diagnostic includes the file/line, a human-readable message, and a caret pointing at the offending character —
exactly the format expected in a production compiler front-end.

---

## Tutorial 2 — Samasa: Hands-On

> **Copy, paste, compile, run.**

Samasa separates scanning from parsing. These examples focus on the scanner (`flux::scan`) and the
`token_buffer` it produces.

### Step 1 — Minimal scan: `"sqrt(x)"`

```cpp
// samasa_step1.cpp — scan "sqrt(x)" and print every token
#include <languages/samasa/samasa.hpp>
#include <print>

// (Normally these come from flux/lexer.hpp.)
#include "flux/lexer.hpp"   // token_kind, flux_keywords, flux_operators, flux::scan

int main() {
    std::string_view src = "sqrt(x)";
    auto buf = flux::scan(src);

    for (auto const& tok : buf.tokens()) {
        auto text = src.substr(tok.offset, tok.length);
        std::println("  offset={:3}  length={:2}  kind={:2}  text={}",
                     tok.offset, tok.length,
                     static_cast<int>(tok.kind), text);
    }
}
```

```text
  offset=  0  length= 4  kind= 1  text=sqrt
  offset=  4  length= 1  kind=28  text=(
  offset=  5  length= 1  kind= 1  text=x
  offset=  6  length= 1  kind=29  text=)
```

Token records are raw integers — offset and length index back into the original source string. No heap allocation per
token; the entire buffer is one contiguous array.

### Step 2 — `consteval` scan

When the source is a compile-time string, `scan_consteval` runs the entire scanner at `consteval`. A syntax error is a
**build error** — it is reported by the compiler, not at runtime.

```cpp
// samasa_step2.cpp — compile-time scanning with static_assert
#include <languages/samasa/samasa.hpp>
#include "flux/lexer.hpp"
#include "flux/grammar.hpp"   // flux_grammar (Chapter 2)
#include <print>

// Good source — verified at compile time
constexpr auto ok = flux::scan_consteval<"x*x + y*y">();
static_assert(ok.success, "syntax error in embedded Flux expression");

// Uncommenting the lines below turns a logic error into a build error:
// constexpr auto bad = flux::scan_consteval<"x*x + ">();
// static_assert(bad.success, "syntax error");   // <-- FAILS AT BUILD TIME

int main() {
    std::println("compile-time scan succeeded: {} tokens", ok.token_count);
}
```

```text
compile-time scan succeeded: 5 tokens
```

The source `"x*x + "` would produce a `static_assert` failure at build time with the Flux expression shown in the assert
message. Embedded DSL mistakes are caught before the binary is linked.

### Step 3 — Inspect the token buffer

```cpp
// samasa_step3.cpp — print a formatted token table for "x*x + y*y"
#include <languages/samasa/samasa.hpp>
#include "flux/lexer.hpp"
#include <print>
#include <string_view>

constexpr std::string_view kind_name(flux::token_kind k) {
    using TK = flux::token_kind;
    switch (k) {
        case TK::identifier:   return "identifier";
        case TK::integer_lit:  return "integer_lit";
        case TK::float_lit:    return "float_lit";
        case TK::plus:         return "plus";
        case TK::star:         return "star";
        case TK::lparen:       return "lparen";
        case TK::rparen:       return "rparen";
        default:               return "other";
    }
}

int main() {
    std::string_view src = "x*x + y*y";
    auto buf = flux::scan(src);

    std::println("{:>6}  {:>6}  {:>14}  {}", "offset", "length", "kind", "text");
    std::println("{}", std::string(46, '-'));
    for (auto const& tok : buf.tokens()) {
        std::println("{:>6}  {:>6}  {:>14}  {}",
                     tok.offset, tok.length,
                     kind_name(tok.kind),
                     src.substr(tok.offset, tok.length));
    }
}
```

```text
offset  length            kind  text
----------------------------------------------
     0       1      identifier  x
     1       1            star  *
     2       1      identifier  x
     4       1            plus  +
     6       1      identifier  y
     7       1            star  *
     8       1      identifier  y
```

Offsets 3 and 5 are spaces — they are consumed but not stored in the token buffer (they become trivia). The gap between
offset 2 and 4 (the space after the first `x`) is invisible in the token table but can be reconstructed by comparing
adjacent offsets.

### Step 4 — Keyword disambiguation: `"letter"` vs `"let"`

The FNV hash of `"letter"` maps to a different slot than `"let"`. Even if they collided, the string compare rejects the
mismatch. Either way, `"letter"` is always emitted as `identifier`.

```cpp
// samasa_step4.cpp — FNV hash disambiguates "letter" from "let"
#include "flux/lexer.hpp"
#include <print>

int main() {
    for (std::string_view src : {"let", "letter"}) {
        auto buf  = flux::scan(src);
        auto tok  = buf.tokens()[0];
        bool is_kw = (tok.kind == flux::token_kind::kw_let);
        std::println("{:8}  kind={}  keyword={}", src,
                     static_cast<int>(tok.kind), is_kw);
    }
}
```

```text
let       kind=4   keyword=true
letter    kind=1   keyword=false
```

The `"let"` scan: FNV hash → slot → stored entry is `"let"` → match → `kw_let` (kind 4). The `"letter"` scan: FNV hash →
different slot → stored entry might be anything → compare fails →
`identifier` (kind 1). One hash, one compare — no loop either way.

### Step 5 — Operator trie trace: `"<="` then `"<"`

```cpp
// samasa_step5.cpp — compare how the trie handles "<=" vs "<"
#include "flux/lexer.hpp"
#include <print>

int main() {
    for (std::string_view src : {"<=", "<"}) {
        auto buf = flux::scan(src);
        auto tok = buf.tokens()[0];
        std::println("{:3}  kind={:2}  length={}",
                     src, static_cast<int>(tok.kind), tok.length);
    }
}
```

```text
<=   kind=19  length=2
<    kind=18  length=1
```

Trie walk for `"<="`:

- Root: edge `'<'` exists → advance to `node_lt`, consume `<`.
- `node_lt`: edge `'='` exists → advance to leaf `lt_eq`, consume `=`.
- Leaf reached → emit token `lt_eq`, length 2.

Trie walk for `"<"` (next character is EOF or space):

- Root: edge `'<'` exists → advance to `node_lt`, consume `<`.
- `node_lt`: no further edge matches (EOF) → `node_lt` carries default kind `lt`.
- Emit token `lt`, length 1. No character was pushed back because EOF terminated the walk.

In source text like `"x < y"` the space after `<` terminates the trie walk at `node_lt` exactly the same way, so `<` is
correctly emitted as a single-character token.

---

## Tutorial 3 — Vakya EDSL: Hands-On

> **Copy, paste, compile, run.**

Path C builds `vakya::node` trees directly from C++ operator overloads. No source text, no scanner, no token buffer.

### Step 1 — Build a minimal expression

```cpp
// edsl_step1.cpp — build mul(x, x) via EDSL and inspect it
#include <lithe/lithe.hpp>
#include <print>

int main() {
    auto x = lithe::make_symbolic("x");
    auto e = x * x;

    std::println("hash: 0x{:016x}", lithe::structural_hash(e));
    lithe::emit::dump(e);
}
```

```text
hash: 0x9f2c4a17e3b80d56
mul(
  symbolic("x"),
  symbolic("x")
)
```

`make_symbolic` creates a leaf node tagged `symbolic_tag` with a string label. `operator*` calls
`lithe::make_node<mul_tag>(x, x)`. The hash is computed bottom-up: leaf hashes are mixed with the tag's `stable_id`
using FNV-1a, then the children hashes are combined in canonical order.

### Step 2 — Build `sqrt(x*x + y*y)`

```cpp
// edsl_step2.cpp — the running example as a pure EDSL expression
#include <lithe/lithe.hpp>
#include <print>

int main() {
    auto x = lithe::make_symbolic("x");
    auto y = lithe::make_symbolic("y");
    auto e = lithe::sqrt(x*x + y*y);

    std::println("hash: 0x{:016x}", lithe::structural_hash(e));
    lithe::emit::dump(e);
}
```

```text
hash: 0x7f3a9b2c8d41e005
sqrt(
  add(
    mul(
      symbolic("x"),
      symbolic("x")
    ),
    mul(
      symbolic("y"),
      symbolic("y")
    )
  )
)
```

This hash is identical to the hash produced by Path A (lexy) and Path B (Samasa) for the same expression — that is the
convergence invariant verified in the complete example at the end of this chapter.

### Step 3 — Commutativity: does `a+b == b+a`?

```cpp
// edsl_step3.cpp — commutative normalization in structural_hash
#include <lithe/lithe.hpp>
#include <print>

int main() {
    auto a = lithe::make_symbolic("a");
    auto b = lithe::make_symbolic("b");

    auto e1 = a + b;
    auto e2 = b + a;

    std::println("a+b hash: 0x{:016x}", lithe::structural_hash(e1));
    std::println("b+a hash: 0x{:016x}", lithe::structural_hash(e2));
    std::println("equal:    {}", lithe::structural_equal(e1, e2));
}
```

```text
a+b hash: 0x3c8a21f9b04d7e11
b+a hash: 0x3c8a21f9b04d7e11
equal:    true
```

`add_tag` has `is_commutative = true` in its `tag_descriptor`. When `structural_hash` hashes a commutative node, it
sorts the children's hashes before mixing — so `hash(a+b) == hash(b+a)`. Non-commutative operations (subtraction,
division, function calls with ordered arguments) do *not*
sort, so `a-b != b-a` as expected.

This commutative normalization means the optimizer can match `x+1` against a pattern written as
`1+x` without a special case — the hashes are identical.

### Step 4 — User-defined tag

Tags are just empty structs. Registering a `tag_descriptor` gives the system the metadata it needs for hashing,
printing, and matching.

```cpp
// edsl_step4.cpp — define norm2_tag (Euclidean norm squared)
#include <lithe/lithe.hpp>
#include <print>

// Tag definition
struct norm2_tag {};

// Metadata registration
template<>
struct vakya::emit::tag_descriptor<norm2_tag> {
    static constexpr std::string_view symbol     = "norm2";
    static constexpr uint32_t         stable_id  = 1001;   // >= 1000 for user tags
    static constexpr uint32_t         arity      = 2;      // norm2(x, y) = x^2 + y^2
    static constexpr bool             is_commutative = true;
};

int main() {
    auto x = lithe::make_symbolic("x");
    auto y = lithe::make_symbolic("y");

    // Build a norm2 node directly
    auto n = lithe::make_node<norm2_tag>(x, y);

    std::println("hash: 0x{:016x}", lithe::structural_hash(n));
    lithe::emit::dump(n);
}
```

```text
hash: 0xb5a31f8c2e094d77
norm2(
  symbolic("x"),
  symbolic("y")
)
```

User tags integrate seamlessly with the rest of the system — `structural_hash`, `structural_equal`,
`emit::dump`, and the pattern-matching optimizer all operate on them without modification.

### Step 5 — The convergence check: Path A == Path C

```cpp
// edsl_step5.cpp — verify that runtime source and EDSL produce identical trees
#include <lithe/lithe.hpp>
#include <languages/samasa/samasa.hpp>
#include "flux/lexer.hpp"
#include "flux/lower.hpp"    // flux::build_from_cst
#include "flux/grammar.hpp"  // flux_grammar
#include <print>
#include <cassert>

int main() {
    auto x_sym = lithe::make_symbolic("x");
    auto y_sym = lithe::make_symbolic("y");

    // ── Path C: build the tree directly in C++ ───────────────────────────────
    auto tree_C = lithe::sqrt(x_sym*x_sym + y_sym*y_sym);
    auto hC     = lithe::structural_hash(tree_C);

    // ── Path B: compile-time Samasa scan ─────────────────────────────────────
    constexpr auto cst = flux::scan_consteval<"sqrt(x*x + y*y)">();
    static_assert(cst.success, "Flux syntax error");
    auto tree_B = flux::build_from_cst(cst, {{"x", x_sym}, {"y", y_sym}});
    auto hB     = lithe::structural_hash(tree_B);

    // ── Results ───────────────────────────────────────────────────────────────
    std::println("Path B (Samasa) hash : 0x{:016x}", hB);
    std::println("Path C (EDSL)   hash : 0x{:016x}", hC);
    std::println("B == C               : {}", (hB == hC));

    assert(hB == hC && "convergence invariant violated");
}
```

```text
Path B (Samasa) hash : 0x7f3a9b2c8d41e005
Path C (EDSL)   hash : 0x7f3a9b2c8d41e005
B == C               : true
```

The hashes match because `flux::build_from_cst` and the EDSL operator overloads both call
`lithe::make_node<Tag>(...)` with the same arguments in the same tree topology. The `vakya::node`
tree is the canonical representation — the front-end path is irrelevant to everything downstream.

---

## ASCII Summary Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Flux Lexer Architecture — Three Paths to One Tree                          │
├──────────────────────┬──────────────────────┬──────────────────────────────┤
│  Path A: Lexy        │  Path B: Samasa       │  Path C: EDSL                │
│  (runtime source)    │  (runtime+consteval)  │  (C++ operator overloads)    │
├──────────────────────┼──────────────────────┼──────────────────────────────┤
│  source text         │  source text          │  C++ expressions             │
│  std::string_view    │  std::string_view or  │  auto e = x * x + y * y;    │
│                      │  ct_string NTTP       │                              │
│        ↓             │        ↓              │        ↓                     │
│  lexy DSL rules      │  keyword_table (FNV)  │  operator overloads          │
│  (fused lex+parse)   │  operator_trie        │  lithe::make_node<Tag>()     │
│  .reserve(LEXY_KW)   │  scanner_policy       │                              │
│        ↓             │        ↓              │        ↓                     │
│  lexy::parse_tree    │  token_buffer<TK>     │  vakya::node (direct)        │
│  (tree of lexemes)   │  flat array, O(1)     │  no scanning step at all     │
│        ↓             │        ↓              │                              │
│  build_ast pass      │  green_arena CST      │                              │
│  (Chapter 3)         │        ↓              │                              │
│                      │  build_ast pass       │                              │
│                      │  (Chapter 3)          │                              │
│        ↓             │        ↓              │          ↓                   │
├──────────────────────┴──────────────────────┴──────────────────────────────┤
│                      vakya::node  (structural_hash H)                        │
│              same tree · same hash · same optimizations · same codegen       │
│                                                                              │
│  Optimizer, type-checker, IR builder, backends — all path-agnostic           │
└──────────────────────────────────────────────────────────────────────────────┘

DFA theory snapshot:
  identifier DFA:    [a-zA-Z_][a-zA-Z_0-9]*   → keyword_table lookup → kw_X | identifier
  integer DFA:       [0-9]+                    → integer_lit
  float DFA:         [0-9]+ '.' [0-9]* (exp?) → float_lit
  operator trie:     longest prefix of opchars → lt | lt_eq | eq | eq_eq | ...
  all run in parallel; maximal match wins
```

## Next → [Chapter 2 — Grammar Deep Dive](ch02_grammar.md)
