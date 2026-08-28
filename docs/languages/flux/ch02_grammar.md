# Chapter 2 — Grammar: Tokens → Concrete Syntax Tree

## What Problem Are We Solving?

The lexer from Chapter 1 gives us a flat token stream — a sequence of typed, positioned chunks with no
relationships between them. The **parser** must find the structure: which tokens group together into
expressions, which expressions group into statements, which statements compose into functions. That
structure is the **Concrete Syntax Tree (CST)**.

```
Tokens: IDENT("sqrt") LPAREN IDENT("x") STAR IDENT("x") PLUS IDENT("y") STAR IDENT("y") RPAREN
                              | parser
                              v
CST:   call_expr
         +-- callee: identifier "sqrt"
         +-- arg_list
               +-- binary_expr  (+)
                     +-- binary_expr  (*)
                     |     +-- identifier "x"
                     |     +-- identifier "x"
                     +-- binary_expr  (*)
                           +-- identifier "y"
                           +-- identifier "y"
```

The CST is not simplified. It preserves every token including parentheses, commas, and keywords. A
later pass (Chapter 3) lowers it to a typed AST. This two-phase approach makes the parser itself
simpler and composable, and gives the diagnostic layer access to exact source positions for every
token in the tree.

### The Three Entry Paths

As Chapter 1 established, Flux has three entry paths that all converge at `vakya::node`. Parsing is
used by two of them:

```
Path A — Runtime text (lexy)
  source text
    --> lexy scanner     --> lexy parse_tree
    --> build_ast        --> vakya::node

Path B — Compile-time string (Samasa consteval)
  constexpr string literal
    --> Samasa parse_static<G, Src>()  [consteval]
    --> green_arena
    --> build_ast         [consteval]
    --> vakya::node

Path C — C++ EDSL (lithe direct)
  C++ operator expressions
    --> vakya::node directly (no parsing)
```

This chapter covers the **Samasa parser** used in both Path A (runtime parse) and Path B (consteval
parse). Path A feeds the same grammar but runs at runtime with full diagnostics. Path B runs the
identical grammar at compile time via `consteval`, so parse errors become compiler errors with a
`static_assert`.

---

## Grammar Formalism: Three Competing Approaches

Before writing any Flux parser code, it helps to understand the design space. Three formalisms exist
for expressing context-free syntax: classical CFG (used by yacc/bison), PEG (used by Samasa), and
hand-written recursive descent. They make different tradeoffs.

### Context-Free Grammars and LALR

A **Context-Free Grammar** is a 4-tuple G = (N, Σ, P, S) where N is a set of nonterminals, Σ is the
terminal alphabet (tokens), P is a set of production rules, and S is the start symbol. Production
rules are written in BNF:

```
expr   ::= expr '+' term | term
term   ::= term '*' factor | factor
factor ::= '(' expr ')' | NUMBER
```

**LALR(1) parsers** (yacc, bison, menhir) build a finite-state machine from the grammar at compile
time. The parser is a table: given the current state and the lookahead token, look up either SHIFT
(push the token and go to a new state) or REDUCE (apply a production rule and pop states).

The problem is ambiguity. Consider the classic dangling-else:

```
if_stmt ::= 'if' expr 'then' stmt
          | 'if' expr 'then' stmt 'else' stmt
```

After parsing `if E then S`, the next token is `else`. The LALR automaton is in a state where two
actions are valid:

```
Dangling else: if E then S else S
  After parsing "if E then S", next token is "else":

  Parser can either:
    SHIFT  -- attach 'else' to the inner 'if' (standard C behaviour)
    REDUCE -- close the outer if_stmt without an else

  --> SHIFT/REDUCE CONFLICT.
  yacc resolves by preferring SHIFT (a hack, not a grammar property).
  Bison will warn; menhir will refuse to compile without an explicit %prec.
```

LALR grammars also require separate lexer and parser passes, which causes the classic
lexer-hack problem: some tokens need parser context to be recognised correctly (C's typedef
names, for example). LALR is mature and fast but grammar conflicts are a constant source of
maintenance burden.

### PEG Grammars — No Ambiguity by Design

A **Parsing Expression Grammar** replaces the nondeterministic union `A | B` with an ordered
choice `A / B`. The operator `/` means: try A; if A succeeds, commit (do not try B); only try
B if A fails. This makes PEG grammars deterministic by construction. There are no shift/reduce
conflicts because there is no table — the grammar is directly interpreted.

PEG operators:

```
e1 e2       sequence: e1 then e2 (both must succeed)
e1 / e2     ordered choice: try e1; only try e2 if e1 fails
e?          optional: e or empty (always succeeds)
e*          zero or more e (greedy: consume as many as possible)
e+          one or more e
!e          negative lookahead: succeed iff e would FAIL (consumes nothing)
&e          positive lookahead: succeed iff e would SUCCEED (consumes nothing)
```

The dangling-else disappears:

```
if_expr := 'if' expr block ('else' block)?

  Ordered choice:  ('else' block)?
  On seeing 'else', the ? grabs it greedily -- exactly the C behaviour,
  no conflict, no grammar hack, no table entry, no warning.
```

PEG grammars can express all LALR(1) grammars plus many LL(k) grammars. They cannot directly
express left-recursive rules (the PEG sequence `expr := expr '+' term` would loop), but Pratt
parsing (covered below) handles operator precedence correctly without left recursion.

### Samasa's `cut` Combinator

Pure PEG backtracking can produce poor error messages. Consider:

```
rule_let_decl := seq(kw_let, identifier, eq, expression)
```

If the source is `let 123 = x`, a pure PEG parser sees `kw_let` match, then fails on `identifier`
(got `integer_literal`). It backtracks and tries the next alternative. The error message reports
the *outermost* failure location, not the point of confusion.

`cut` solves this:

```
rule_let_decl := seq(kw_let, cut, identifier, eq, expression)
                              ^^^
                              After 'let' is matched, any subsequent
                              failure is an ERROR, not a backtrack.
                              Parsing does NOT try other alternatives.
```

Once the parser has committed past `cut`, the diagnostic is precise:

```
error at 4: expected identifier, got integer_literal '123'
```

Without `cut`, the parser would backtrack to the top-level `choice` between `let_decl`, `fn_decl`,
and `input_decl`, and produce a message like "expected declaration", which is useless.

### Recursive Descent

Hand-written **recursive descent** is simply a manual PEG implementation. Each grammar rule becomes
a function that returns success/failure and a node. Recursive descent is easy to debug and profile,
but writing it by hand is verbose and the combinator structure is buried in control flow.

Samasa **generates** recursive descent from the grammar type: each `rule_*` struct is compiled into
an inlined parse function. The grammar type is the source of truth; the generated code is what runs.
This gives the readability of hand-written recursive descent with the correctness guarantees of a
formal grammar definition.

---

## Pratt Parsing: Expression Precedence

This is the most important section of the chapter. Operator expressions — `x + y * z`, `-x^2`,
`a == b and c != d` — appear in almost every Flux program. Getting their structure right (which
operator binds more tightly, which is left- vs right-associative) requires a parsing strategy that
handles precedence without encoding it as grammar rewrites.

### Why Naive Recursive Descent Fails for Expressions

The natural BNF for arithmetic is left-recursive:

```
expr := expr '+' term | term    -- LEFT RECURSIVE: would loop forever!
term := term '*' factor | factor
```

A recursive descent parser that naively calls `parse_expr()` inside `parse_expr()` as the first
thing it does will recurse infinitely before consuming a single token.

The classic fix is to stratify the grammar by precedence level:

```
expr     := term ('+' term)*
term     := power ('*' power)*
power    := primary ('^' power)?   -- right-associative via self-tail
primary  := NUMBER | '(' expr ')'
```

This works, but adding a new operator at a new precedence level requires inserting a new function
in the chain. Adding `and` and `or` requires two more levels. The grammar becomes a ladder of
single-purpose rules, and every operator must know its exact position in the ladder at the time
the grammar is written. Pratt parsing eliminates this ladder entirely.

### Pratt's Insight: Binding Power

**Binding power** (bp) is a number associated with an operator that encodes how tightly it holds
onto its operands. Each infix operator has two binding powers:

- `left_bp` — how hard it pulls on the expression to its left
- `right_bp` — how hard it pulls on the expression to its right

For left-associative operators, `right_bp = left_bp + 1`. The asymmetry ensures that a second
occurrence of the same operator attaches to the right, not the left (which would create left
recursion). For right-associative operators, `right_bp = left_bp` — identical binding power
forces the recursion to go rightward.

Flux operator table:

```
operator    left_bp   right_bp   associativity   notes
--------    -------   --------   -------------   -----
or              10         11   left
and             20         21   left
== !=           30         31   left
< <= > >=       40         41   left
+ -             50         51   left
* / %           60         61   left
^ (power)       70         70   right            same bp both sides
- (unary)       --         65   prefix           no left_bp
not (unary)     --         65   prefix
. (method)      80         --   postfix          no right_bp
```

### Pratt Algorithm

```
parse_expr(min_bp = 0):
  lhs = parse_primary()
  loop:
    op = peek()
    if op is end-of-input or left_bp(op) < min_bp:
      return lhs
    consume(op)
    rhs = parse_expr(right_bp(op))     <-- recurse with higher threshold
    lhs = make_binary(op, lhs, rhs)
  return lhs
```

The key insight: `min_bp` is the "minimum binding power that is allowed to grab my left side". An
operator only gets to consume `lhs` if its `left_bp` is at least `min_bp`. When we recurse for the
right side, we pass `right_bp(op)` as the new minimum, so only operators that bind *more tightly*
than `op` can grab tokens to the right.

### Trace: `x + y * z`

```
parse_expr(min_bp=0):
  lhs = parse_primary() --> identifier "x"
  peek: +   left_bp(+)=50 >= 0  YES
  consume +
  rhs = parse_expr(right_bp(+) = 51):
    lhs = parse_primary() --> identifier "y"
    peek: *   left_bp(*)=60 >= 51  YES
    consume *
    rhs = parse_expr(right_bp(*) = 61):
      lhs = parse_primary() --> identifier "z"
      peek: eof  left_bp=0 < 61  NO
      return identifier "z"
    lhs = binary_expr(*, y, z)
    peek: eof  left_bp=0 < 51  NO
    return binary_expr(*, y, z)
  rhs = binary_expr(*, y, z)
  lhs = binary_expr(+, x, binary_expr(*, y, z))
  peek: eof  0 < 0 is false... eof exits loop
  return binary_expr(+, x, binary_expr(*, y, z))
```

Result: `(x + (y * z))` — correct, `*` binds tighter.

### Trace: `2 ^ 3 ^ 4` (right-associative)

```
parse_expr(min_bp=0):
  lhs = integer_literal 2
  peek: ^   left_bp(^)=70 >= 0  YES
  consume ^
  rhs = parse_expr(right_bp(^) = 70):     <-- SAME as left_bp
    lhs = integer_literal 3
    peek: ^   left_bp(^)=70 >= 70  YES    <-- still passes!
    consume ^
    rhs = parse_expr(right_bp(^) = 70):
      lhs = integer_literal 4
      peek: eof  exit
      return integer_literal 4
    lhs = binary_expr(^, 3, 4)
    peek: eof  exit
    return binary_expr(^, 3, 4)
  rhs = binary_expr(^, 3, 4)
  lhs = binary_expr(^, 2, binary_expr(^, 3, 4))
  return binary_expr(^, 2, binary_expr(^, 3, 4))
```

Result: `2^(3^4)` — correct right-associativity. The trick is that for right-associative operators,
`right_bp == left_bp`, so the recursive call can still see another `^` and consume it. For
left-associative operators, `right_bp = left_bp + 1` means the recursive call will *not* consume
another operator at the same level — forcing left grouping.

### Trace: `-x * y` (prefix unary)

Prefix operators are handled in `parse_primary`, not in the main loop. When `parse_primary` sees
a token that acts as a prefix operator, it reads the operator, then calls `parse_expr(right_bp)`
with the unary operator's right binding power:

```
parse_primary():
  token = peek()
  if token == '-' (unary):
    consume '-'
    operand = parse_expr(right_bp(-unary) = 65)
      --> parse_primary() --> identifier "x"
      --> peek: *   left_bp(*)=60 < 65  NO   <-- * does NOT grab x from under unary
      --> return identifier "x"
    return unary_expr(neg, x)

parse_expr(min_bp=0):
  lhs = parse_primary() --> unary_expr(neg, x)
  peek: *   left_bp(*)=60 >= 0  YES
  consume *
  rhs = parse_expr(61) --> identifier "y"
  lhs = binary_expr(*, unary_expr(neg, x), y)
  return binary_expr(*, unary_expr(neg, x), y)
```

Result: `((-x) * y)` — correct. Unary minus binds tighter than `*` for its operand (`65 > 60`),
so `*` cannot steal `x` away from `neg`. But the overall `*` expression still forms correctly
because `left_bp(*) = 60` passes the outer `min_bp = 0`.

### Postfix and Method Calls

The `.` operator for method calls is postfix. When the main loop peeks at `.`, it consumes the dot,
reads the method name and argument list, and wraps the current `lhs` in a `method_call_expr`:

```
parse_expr(min_bp=0):
  lhs = parse_primary() --> identifier "d"
  peek: .   left_bp(.)=80 >= 0  YES
  consume .
  -- postfix handler reads method name + '(' args ')'
  method_name = identifier "show_vakya"
  '('   ')' -- no args
  lhs = method_call_expr(object=d, method=show_vakya, args=[])
  peek: eof  exit
  return method_call_expr(...)
```

The `.` operator has `left_bp = 80`, the highest in Flux, so method calls bind more tightly than
any arithmetic. `a + b.f()` is `a + (b.f())`, not `(a + b).f()`.

---

## Samasa Grammar API in Depth

Samasa grammars are C++23 types. Each rule is a struct with a `static constexpr auto pattern`
member. Patterns are composed from combinators.

### Combinator Reference

```
seq(a, b, c)           sequence: a then b then c, all must match
choice(a, b, c)        ordered choice: try a; fail -> try b; fail -> c
opt(a)                 a or empty (always succeeds)
many(a)                zero or more a (greedy)
many1(a)               one or more a (fails if zero matches)
sep_by(a, sep)         a, sep, a, sep, ...  zero or more
sep_by1(a, sep)        at least one a, separated by sep
cut                    commit: failures past here are errors, not backtracks
lookahead(a)           succeed iff a would succeed, consume nothing
node_t<Kind, p>        emit a CST node of Kind wrapping pattern p
rule_ref<R>{}          lazy reference to rule R (resolves mutual recursion)
pratt_expression<T, P> Pratt expression parser using op-table T, primary P
recover_with<P, R>     on failure in P, apply recovery strategy R
skip_until_sync<...>   skip tokens until one of the sync tokens is seen
```

The most important design note: `rule_ref<R>{}` is how mutually recursive rules refer to each
other. Without it, `rule_expression` would need `rule_primary` to be complete before it can be
declared, and `rule_primary` needs `rule_expression` for parenthesised sub-expressions. The
indirection via `rule_ref` breaks the cycle by deferring resolution to instantiation time.

### `tok<TK>::of(kind)` versus `tok<TK>::not_of(kind)`

Token matchers follow a consistent pattern:

```cpp
tok<TK>::of(TK::kw_let)       // match exactly token_kind::kw_let
tok<TK>::of(TK::identifier)   // match any identifier token
tok<TK>::not_of(TK::rbrace)   // match any token except '}'
```

The type parameter `TK` is always `token_kind` (aliased as `TK` inside the grammar namespace).
This is verbose but unambiguous: every token reference in the grammar is fully qualified.

### Annotated Grammar Rules

**`rule_let_decl`** — variable binding:

```cpp
struct rule_let_decl {
    // let_decl := 'let' identifier '=' expression
    //
    // cut after 'let': once we see the keyword, any failure is a hard error.
    // Without cut, "let 123 = x" would silently backtrack to rule_fn_decl,
    // producing "expected declaration" instead of "expected identifier".
    static constexpr auto pattern = node_t<flux_kind::let_decl,
        seq(
            tok<TK>::of(TK::kw_let),      // keyword 'let'
            cut,                           // committed from here
            tok<TK>::of(TK::identifier),   // binding name
            tok<TK>::of(TK::eq),           // '='
            rule_ref<rule_expression>{}    // right-hand side value
        )
    >{};
};
```

**`rule_fn_decl`** — function definition, with optional pure annotation and return type:

```cpp
struct rule_fn_decl {
    // fn_decl := ('pure')? 'fn' identifier '(' params ')' ('->' type)? block
    //
    // 'pure' is optional: marks a function as having no side effects.
    // The '->' return type annotation is optional: inferred if absent.
    static constexpr auto pattern = node_t<flux_kind::fn_decl,
        seq(
            opt(tok<TK>::of(TK::kw_pure)),   // optional 'pure' qualifier
            tok<TK>::of(TK::kw_fn),          // keyword 'fn'
            cut,                              // committed from here
            tok<TK>::of(TK::identifier),      // function name
            tok<TK>::of(TK::lparen),          // '('
            rule_ref<rule_param_list>{},       // zero or more params
            tok<TK>::of(TK::rparen),          // ')'
            opt(seq(                           // optional return type
                tok<TK>::of(TK::arrow),        // '->'
                rule_ref<rule_type>{}          // return type expression
            )),
            rule_ref<rule_block>{}             // function body
        )
    >{};
};
```

**`rule_param`** — single function parameter:

```cpp
struct rule_param {
    // param := identifier (':' type)?
    //
    // Type annotation is optional at the grammar level.
    // Type inference (Chapter 5) fills in unannotated parameters.
    static constexpr auto pattern = node_t<flux_kind::param,
        seq(
            tok<TK>::of(TK::identifier),
            opt(seq(
                tok<TK>::of(TK::colon),
                rule_ref<rule_type>{}
            ))
        )
    >{};
};
```

**`rule_block`** — brace-delimited sequence of declarations and expressions:

```cpp
struct rule_block {
    // block := '{' (let_decl | fn_decl | expression)* '}'
    //
    // cut after '{': once we're inside a block, failures are errors.
    // The last item before '}' is the block's return value (like Rust).
    static constexpr auto pattern = node_t<flux_kind::block,
        seq(
            tok<TK>::of(TK::lbrace),
            cut,
            many(choice(
                rule_ref<rule_let_decl>{},
                rule_ref<rule_fn_decl>{},
                rule_ref<rule_expression>{}
            )),
            tok<TK>::of(TK::rbrace)
        )
    >{};
};
```

**`rule_input_decl`** — top-level input declaration (declares a free variable for external binding):

```cpp
struct rule_input_decl {
    // input_decl := 'input' identifier ':' type
    //
    // Inputs are declared at the top of a Flux program.
    // They bind to external data sources when the program is executed.
    // Type annotation is required (no inference for external interfaces).
    static constexpr auto pattern = node_t<flux_kind::input_decl,
        seq(
            tok<TK>::of(TK::kw_input),
            cut,
            tok<TK>::of(TK::identifier),
            tok<TK>::of(TK::colon),
            rule_ref<rule_type>{}
        )
    >{};
};
```

**`rule_type`** — type expressions including composite types:

```cpp
struct rule_type {
    // type := primitive_type
    //       | 'vec' '<' type '>'
    //       | 'mat' '<' type ',' INT ',' INT '>'
    //       | 'tensor' '<' type '>' '[' INT (',' INT)* ']'
    //       | identifier   (named/opaque type)
    //
    // Ordered choice: vec/mat/tensor are tried first so the keyword
    // is consumed before the named-type fallback can grab it.
    static constexpr auto pattern = node_t<flux_kind::primitive_type,
        choice(
            seq(tok<TK>::of(TK::kw_vec),
                tok<TK>::of(TK::lt),
                cut,
                rule_ref<rule_type>{},
                tok<TK>::of(TK::gt)),
            seq(tok<TK>::of(TK::kw_mat),
                tok<TK>::of(TK::lt),
                cut,
                rule_ref<rule_type>{},
                tok<TK>::of(TK::comma),
                tok<TK>::of(TK::integer_lit),
                tok<TK>::of(TK::comma),
                tok<TK>::of(TK::integer_lit),
                tok<TK>::of(TK::gt)),
            seq(tok<TK>::of(TK::kw_tensor),
                tok<TK>::of(TK::lt),
                cut,
                rule_ref<rule_type>{},
                tok<TK>::of(TK::gt),
                tok<TK>::of(TK::lbracket),
                sep_by(tok<TK>::of(TK::integer_lit), tok<TK>::of(TK::comma)),
                tok<TK>::of(TK::rbracket)),
            rule_ref<rule_primitive_type>{},
            node_t<flux_kind::named_type, tok<TK>::of(TK::identifier)>{}
        )
    >{};
};
```

**`rule_expression`** — hands off to the Pratt engine:

```cpp
struct rule_expression {
    static constexpr auto table = flux_op_table();
    static constexpr auto pattern =
        pratt_expression<decltype(table), rule_primary>{};
};
```

The Pratt engine is provided by Samasa. `flux_op_table()` is a constexpr function that returns
the operator table (all `infix`, `prefix`, and `postfix` entries). `rule_primary` is the rule for
atoms — identifiers, literals, parenthesised expressions, `if` expressions, lambdas.

**`rule_program`** — the root rule:

```cpp
struct rule_program {
    // program := import* declaration* expression?
    //
    // The trailing expression? is the program's "result expression":
    // a Flux program can be a top-level computation that returns a value.
    static constexpr auto pattern = node_t<flux_kind::program,
        seq(
            many(rule_ref<rule_import>{}),
            many(choice(
                rule_ref<rule_input_decl>{},
                rule_ref<rule_fn_decl>{},
                rule_ref<rule_let_decl>{}
            )),
            opt(rule_ref<rule_expression>{})
        )
    >{};
};
```

---

## The Green Tree: CST Data Structure

Samasa's output is a **green tree** — a flat, immutable, index-based arena of CST nodes. The term
comes from the Roslyn compiler (C#) where the green tree is the source-faithful layer that never
loses a token.

### Why a Green Tree?

A traditional AST allocates one heap object per node. For a program with thousands of expressions,
that is thousands of separate allocations, each with its own pointer header, poor cache locality,
and individual destructor calls. A green tree uses a single contiguous arena: all nodes are stored
as flat entries in a `std::vector`-like structure, referenced by integer index.

Properties:

```
Source-faithful  Every token has an offset and length into the original source.
                 Error messages can cite exact character positions.

Immutable        Once built, nodes are never modified.
                 The tree can be shared safely between threads or cached.

Cheap            No per-node heap allocation. All nodes in one arena.
                 Child access is O(1) index lookup.

Comparable       Two green trees with the same content compare equal by structure.
                 Used by the incremental re-parse cache.
```

### Arena Layout

For `let x = 5 + 3`:

```
green_arena layout (flat, index-based):

  idx   kind               span          children
  ----  -----------------  ----------    --------
  0     let_decl           [0..12]       [1, 2]
  1     identifier         [4..5]        []       leaf: "x"
  2     binary_expr        [8..12]       [3, 4]
  3     integer_literal    [8..9]        []       leaf: "5"
  4     integer_literal    [12..13]      []       leaf: "3"
```

Children are stored as indices into the same arena — no pointers. To walk the tree:

```cpp
// depth-first walk via index stack
void walk(green_arena const& a, green_node_idx root) {
    std::stack<green_node_idx> stack;
    stack.push(root);
    while (!stack.empty()) {
        auto idx = stack.top(); stack.pop();
        auto const& node = a[idx];
        process(node.kind, node.span);
        // push children in reverse order so first child is processed first
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
            stack.push(*it);
    }
}
```

### Trivia

The green tree preserves **trivia** — whitespace and comments — as child nodes of the surrounding
tokens. This is what makes the CST "source faithful": given the green tree, you can reconstruct the
original source exactly. The AST (Chapter 3) strips trivia; it only needs the semantic structure.

---

## Error Recovery

A parser that stops at the first error is useless for development. Real compilers report as many
errors as possible in a single pass. This requires **error recovery**: when parsing fails, skip
forward to a known-good position and resume.

### Synchronisation Points

The standard technique is **panic mode**: on error, skip tokens until a **synchronisation token**
is found — a token that is very likely to start or end a syntactic unit. For Flux:

```
Synchronisation tokens: '{', '}', ';', 'let', 'fn', 'input'
```

These are chosen because they are rare inside expressions and almost always appear at statement or
block boundaries. After skipping to a sync token, the parser has a good chance of re-aligning with
the structure of the remaining source.

### `recover_with` in Samasa

```cpp
struct rule_safe_statement {
    // On error inside any statement, skip to the next sync token.
    // The error is recorded as an error_node in the CST.
    // Parsing resumes after the sync token.
    static constexpr auto pattern =
        recover_with<
            choice(
                rule_ref<rule_let_decl>{},
                rule_ref<rule_fn_decl>{},
                rule_ref<rule_expression>{}
            ),
            skip_until_sync<
                tok<TK>::of(TK::semicolon),
                tok<TK>::of(TK::lbrace),
                tok<TK>::of(TK::rbrace)
            >
        >{};
};
```

When the inner `choice` fails (and `cut` has not been hit), Samasa invokes the recovery strategy.
`skip_until_sync` consumes tokens until it sees one of the listed sync tokens, then emits an
`error_node` containing the skipped span. Parsing continues with the next statement.

### Recovery Example

```flux
let x = 5
let 123 = "bad"   -- error: expected identifier after 'let'
let y = 10        -- parsing continues here after recovery
```

The parser produces:

```
program
  +-- let_decl           "let x = 5"
  +-- error_node         "let 123 = \"bad\""   (span 10..26)
  +-- let_decl           "let y = 10"
```

Two diagnostics are emitted, one for each let_decl with an error. The program CST contains an
`error_node` placeholder where the bad statement was. Downstream passes (Chapter 3 AST builder,
Chapter 4 name resolution) check for `error_node` children and skip them, so errors do not
cascade into the semantic analysis.

### `cut` + Recovery Interaction

When `cut` has fired, recovery is triggered at the cut point, not at the choice point. For:

```
rule_let_decl := seq(kw_let, cut, identifier, eq, expression)
```

The sequence `let 123 = x` fires `cut` after `kw_let`. The failure on `identifier` (got
`integer_literal`) triggers recovery immediately — no backtracking to try `rule_fn_decl`. The
recovery skips to the next sync token and records a precise error at the failing position.

Without `cut`, the parser would backtrack to the top `choice` in `rule_safe_statement` and try
`rule_fn_decl` (which also fails, at position 0 since there is no `fn`), then `rule_expression`
(which would parse `let` as an identifier — wrong). The first error message would be wrong and
potentially misleading.

---

## Full Flux Grammar Reference

Complete grammar in EBNF. This is the canonical reference for all three entry paths.

```
program    := import* decl* expression?

import     := 'import' IDENT

decl       := let_decl | fn_decl | input_decl

let_decl   := 'let' IDENT '=' expr

fn_decl    := 'pure'? 'fn' IDENT '(' params ')' ('->' type)? block

input_decl := 'input' IDENT ':' type

block      := '{' (decl | expr)* expr? '}'

params     := (param (',' param)*)?

param      := IDENT (':' type)?

expr       := pratt_expr

primary    := INT
            | FLOAT
            | BOOL
            | STRING
            | IDENT ('(' args ')')?
            | '(' expr ')'
            | 'if' expr block 'else' block
            | '[' args ']'
            | 'fn' '(' params ')' block

args       := (expr (',' expr)*)?

type       := 'i32' | 'i64' | 'u32' | 'u64' | 'f32' | 'f64' | 'bool' | 'string'
            | 'vec' '<' type '>'
            | 'mat' '<' type ',' INT ',' INT '>'
            | 'tensor' '<' type '>' '[' INT (',' INT)* ']'
            | 'tuple' '<' type (',' type)* '>'
            | IDENT

operators (highest binding power first):

  .           method/field access (postfix, bp=80)
  ^ (power)   right-associative, bp=70
  * / %       left-associative,  bp=60
  + -         left-associative,  bp=50
  < <= > >=   left-associative,  bp=40
  == !=       left-associative,  bp=30
  and         left-associative,  bp=20
  or          left-associative,  bp=10

  prefix:
  - (negate)  right_bp=65
  not         right_bp=65
```

---

## Tutorial: Grammar in Action

The following examples can be compiled and run against the actual Flux grammar headers.

### Try It 1 — Parse `let distance = sqrt(x*x + y*y)`

```cpp
// ch02_try1.cpp
#include <lithe/lithe.hpp>
#include <languages/samasa/samasa.hpp>
#include <print>
#include "flux/grammar.hpp"

int main() {
    std::string_view src = "let distance = sqrt(x*x + y*y)";

    auto result = flux::parse(src);
    std::println("success: {}", result.success);
    std::println("cst nodes: {}", result.green_tree().size());

    // Walk depth-first, print kind + source span + text
    result.green_tree().walk([&](auto kind, int depth, auto span) {
        std::string indent(depth * 2, ' ');
        auto text = src.substr(span.begin, span.end - span.begin);
        std::println("{}{} [{:3}..{:3}] '{}'",
            indent, flux::kind_name(kind), span.begin, span.end, text);
    });
}
```

Expected output:

```text
success: true
cst nodes: 12
let_decl [  0.. 30] 'let distance = sqrt(x*x + y*y)'
  identifier [  4.. 12] 'distance'
  call_expr [ 15.. 30] 'sqrt(x*x + y*y)'
    identifier [ 15.. 19] 'sqrt'
    arg_list [ 20.. 29] 'x*x + y*y'
      binary_expr [ 20.. 29] 'x*x + y*y'
        binary_expr [ 20.. 23] 'x*x'
          identifier [ 20.. 21] 'x'
          identifier [ 22.. 23] 'x'
        binary_expr [ 26.. 29] 'y*y'
          identifier [ 26.. 27] 'y'
          identifier [ 28.. 29] 'y'
```

Notice that the `+` and `*` operators are represented by the parent `binary_expr` node, not as
separate leaf nodes. The operator token kind is stored in the `binary_expr`'s own metadata field,
recoverable as `node.op_kind()`. This keeps the tree compact — operators are attributes of their
parent expression node rather than separate structural children.

### Try It 2 — Intentional Parse Error

```cpp
// ch02_try2.cpp
#include "flux/grammar.hpp"
#include <print>

int main() {
    std::string_view bad = "let 123 = x";

    auto result = flux::parse(bad);
    std::println("success: {}", result.success);

    for (auto const& diag : result.diagnostics())
        std::println("error at {}: {}", diag.offset, diag.message());
}
```

Expected output:

```text
success: false
error at 4: expected identifier, got integer_literal '123'
```

The error offset is 4 — the character position of `123` in `"let 123 = x"`. The message names
both what was expected (`identifier`) and what was found (`integer_literal '123'`). This is the
diagnostic quality that `cut` enables.

### Try It 3 — `consteval` Grammar Check (Path B)

```cpp
// ch02_try3.cpp
#include "flux/grammar.hpp"

// Parse at compile time. If the source has a syntax error,
// the static_assert fires and compilation fails with the
// diagnostic message from the Samasa grammar engine.
constexpr auto r = flux::scan_consteval<
    "pure fn add(x : f32, y : f32) -> f32 { x + y }"
>();
static_assert(r.success, "compile-time parse error in Flux source");

int main() {
    // r is a fully formed green_arena. Its size is a compile-time constant.
    static_assert(r.green_tree().size() > 0);
}
```

If the string is changed to `"pure fn add(x : f32 y : f32) -> f32 { x + y }"` (missing comma
after `f32`), compilation produces:

```text
ch02_try3.cpp:5: error: static assertion failed: compile-time parse error in Flux source
  compile-time parse error at 20: expected ',' or ')', got identifier 'y'
```

This is Path B in action: the grammar, the parser, and the diagnostic are all executed at
`consteval` time. Shipping a Flux source literal in C++ code that fails to parse is a
**compile error**, not a runtime failure.

### Try It 4 — Function with Return Type

```cpp
// ch02_try4.cpp
#include "flux/grammar.hpp"
#include <print>

int main() {
    std::string_view src =
        "pure fn distance(x : f32, y : f32) -> f32 {\n"
        "    let dx2 = x * x\n"
        "    let dy2 = y * y\n"
        "    sqrt(dx2 + dy2)\n"
        "}";

    auto result = flux::parse(src);
    std::println("success: {}", result.success);

    // Check top-level node is fn_decl with kw_pure
    auto root = result.green_tree().root();
    std::println("root kind: {}", flux::kind_name(root.kind));
    std::println("is pure: {}", root.has_child(flux_kind::kw_pure));
}
```

Expected output:

```text
success: true
root kind: program
is pure: true
```

### Try It 5 — Method Call Syntax

Flux supports `expr.method(args)` — the `.` operator parses as a postfix method call with
binding power 80:

```flux
let d = sqrt(x*x + y*y)
d.show_vakya()
d.run(cpu)
```

CST for `d.show_vakya()`:

```
method_call_expr
  +-- object: identifier "d"
  +-- method: identifier "show_vakya"
  +-- arg_list (empty)
```

CST for `a + b.f()`:

```
binary_expr (+)
  +-- identifier "a"
  +-- method_call_expr
        +-- object: identifier "b"
        +-- method: identifier "f"
        +-- arg_list (empty)
```

The `+` has `left_bp = 50`. When the Pratt loop processes `b`, it peeks at `.` with `left_bp = 80`,
which is greater than the current `min_bp` of 51 (we are inside `parse_expr(51)` for the right side
of `+`). So `.` fires and wraps `b` into a `method_call_expr` before the `+` node is closed. The
result is `a + (b.f())`, not `(a + b).f()`.

---

## ASCII Diagram: Full Parse Pipeline

```
Source text:
  "fn distance(x:f32, y:f32) -> f32 { sqrt(x*x + y*y) }"

                  | lexer (Chapter 1)
                  v

Token stream:
  KW_FN  IDENT("distance")  LPAREN
  IDENT("x")  COLON  KW_F32  COMMA
  IDENT("y")  COLON  KW_F32  RPAREN
  ARROW  KW_F32
  LBRACE
    IDENT("sqrt")  LPAREN
      IDENT("x")  STAR  IDENT("x")
      PLUS
      IDENT("y")  STAR  IDENT("y")
    RPAREN
  RBRACE

                  | Samasa parser (this chapter)
                  v

CST (green_arena, flat index-based):

  fn_decl
    +-- identifier "distance"
    +-- param_list
    |     +-- param
    |     |     +-- identifier "x"
    |     |     +-- primitive_type f32
    |     +-- param
    |           +-- identifier "y"
    |           +-- primitive_type f32
    +-- primitive_type f32         (return type)
    +-- block
          +-- call_expr
                +-- identifier "sqrt"
                +-- arg_list
                      +-- binary_expr (+)
                            +-- binary_expr (*)
                            |     +-- identifier "x"
                            |     +-- identifier "x"
                            +-- binary_expr (*)
                                  +-- identifier "y"
                                  +-- identifier "y"

                  | build_ast (Chapter 3)
                  v

Flux AST (ast_arena, typed nodes):

  fn_decl_node {
    name     = "distance",
    pure     = false,
    params   = [
      param_node { name="x", type=f32 },
      param_node { name="y", type=f32 }
    ],
    ret_type = f32,
    body     = call_node {
      callee = "sqrt",
      args   = [
        add_node {
          lhs = mul_node { lhs=var("x"), rhs=var("x") },
          rhs = mul_node { lhs=var("y"), rhs=var("y") }
        }
      ]
    }
  }

                  | vakya lowering (Chapter 7)
                  v

  vakya::node (structural hash, backend-agnostic)
```

---

## Compile-Time Grammar Validation

The grammar type is validated at translation time via `grammar_valid<>`:

```cpp
// At the bottom of grammar.hpp
using flux_grammar = lang::samasa::grammar<
    flux_kind,
    token_kind,
    rule_program,
    rule_import,
    rule_let_decl,
    rule_fn_decl,
    rule_input_decl,
    rule_param,
    rule_param_list,
    rule_block,
    rule_expression,
    rule_primitive_type,
    rule_type
>;

static_assert(lang::samasa::grammar_valid<flux_grammar>(),
    "Flux grammar has conflicts — run grammar_diag_code for details");
```

`grammar_valid<G>()` checks:

```
FIRST conflict     Two alternatives in a choice() share the same first token.
                   (Would cause non-determinism in LL parsing; Samasa uses
                    PEG ordered choice, so it is a warning, not a hard error,
                    unless the ambiguity produces different trees.)

FOLLOW conflict    A token that follows a rule is also a valid start of the
                   rule itself (opt() / many() boundary ambiguity).

Left recursion     A rule directly or indirectly calls itself as its first
                   token consumer. Fatal: would loop forever.
```

Left recursion is the only hard failure. FIRST/FOLLOW conflicts are reported as warnings with the
rule name and conflicting token. In practice, Flux's `cut` annotations resolve almost all FOLLOW
ambiguities by making the parser commit before the ambiguous point is reached.

---

## `flux::parse` Entry Point

```cpp
// include/languages/flux/parser.hpp
#pragma once
#include <languages/samasa/samasa.hpp>
#include "grammar.hpp"

namespace flux {

// Runtime parse. Returns a parse_output containing the green CST and diagnostics.
// src must outlive the returned result (spans are offsets into src).
inline auto parse(std::string_view src)
    -> lang::samasa::parse_output<flux_kind, token_kind>
{
    auto tokens = scan(src);          // Chapter 1 lexer
    return lang::samasa::parse<flux_grammar>(tokens.view());
}

// Consteval parse. src must be a string_view of a string literal.
// Returns a constexpr parse_output. Use static_assert(result.success).
template <std::string_view const& src>
consteval auto scan_consteval()
{
    auto tokens = scan_consteval_impl<src>();
    return lang::samasa::parse_static<flux_grammar, src, tokens>();
}

} // namespace flux
```

The split between `parse()` (runtime) and `scan_consteval()` (compile time) is the only place
where Path A and Path B diverge. After this call, both produce a `parse_output<flux_kind,
token_kind>` and the AST builder (Chapter 3) handles both identically.

---

## What We Have

| Artifact | Description |
|----------|-------------|
| `flux_kind` | Enum of 30+ CST node kinds (declarations, expressions, types, error) |
| `flux_grammar` | PEG grammar validated at compile time; Pratt operator table |
| `flux::parse(src)` | Runtime entry point — returns green CST + diagnostics |
| `flux::scan_consteval<src>()` | Compile-time entry point — parse errors = compiler errors |
| `green_arena` | Flat, index-based, source-faithful immutable CST |
| Error recovery | `recover_with` + `skip_until_sync` — multiple errors per parse pass |

The grammar is the authoritative source of truth for Flux syntax. The EBNF reference above, the
Samasa rule structs, and the PEG operator table are all derived from the same type definitions.
Changing the grammar means changing the structs; the parser, the validator, and the compile-time
check all update automatically.

---

## Next

[Chapter 3 → AST](ch03_ast.md) — lower the green CST into a typed Flux AST using a flat arena.
The AST discards trivia, resolves operator tokens into typed expression nodes, and establishes the
form that name resolution (Chapter 4) and type inference (Chapter 5) operate on.
