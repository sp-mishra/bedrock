# REPL Framework (`include/languages/repl`) + cranki

A generic, header-only interactive-loop framework for the languages in this tree,
plus **cranki** — the crank REPL with a Borland-style FTXUI terminal UI.

## Table of Contents

1. [Purpose](#purpose)
2. [Architecture](#architecture)
3. [Algorithms Used](#algorithms-used)
4. [The `repl_engine` seam](#the-repl_engine-seam)
4. [`session<Engine>` — accumulated-source model](#sessionengine--accumulated-source-model)
5. [Input classification](#input-classification)
6. [Meta-commands](#meta-commands)
7. [Transcript](#transcript)
8. [Binding a language (crank)](#binding-a-language-crank)
9. [cranki — the FTXUI REPL](#cranki--the-ftxui-repl)
10. [Building & running](#building--running)
11. [Testing](#testing)
12. [Extending to a new language](#extending-to-a-new-language)

## Purpose

crank has a full interpreter facade (`crank::engine`) but no interactive front-end.
This layer adds one, **without hardwiring to crank**: the loop semantics
(accumulated definitions, multi-line assembly, history, transcript, `:`-commands)
are language- and UI-agnostic. A language binds by satisfying a small concept; a
front-end (headless or FTXUI) drives a `session`.

Design invariants (repo-wide): C++23, header-only, **no virtual, no macros**,
pay-for-what-you-use (templated engine, monomorphized — no vtable), reuse of the
existing `crank::engine` pipeline (no re-implementation of parse/analyse/lower/run).

## Architecture

```
include/languages/repl/         generic core — ZERO crank & ZERO FTXUI dependency
  engine_concept.hpp            repl_engine concept + eval_outcome
  line_classify.hpp             complete / incomplete / empty input
  transcript.hpp                ordered, bounded scrollback
  command.hpp                   ':' meta-command registry
  session.hpp                   session<Engine> — the REPL state machine
  repl.hpp                      umbrella (generic core only)

src/cranki/
  crank_binding.hpp             crank::engine → repl_engine (crank-aware, no UI)
  main.cpp                      FTXUI Borland-style TUI over session<crank>
```

The core never names crank or FTXUI, and contains no language binding. The crank
binding lives with its only consumer (the cranki executable) in
`src/cranki/crank_binding.hpp`; it is crank-aware but has no UI dependency (so it
compiles headless). FTXUI lives solely in `src/cranki/main.cpp`.

```
                 drives                 satisfies
   front-end  ─────────▶  session<E>  ─────────▶  E : repl_engine
 (cranki TUI /            (generic)              (crank_repl_engine, …)
  headless)                                            │ wraps
                                                        ▼
                                                 crank::engine
```

## Algorithms Used

Concrete named algorithms in the framework, with the header they live in.

| Concern | Algorithm | Where |
|---|---|---|
| Input completeness | Language-neutral bracket/quote balancer: nesting scan over `() [] {}` skipping `"…"`/`'…'` literals and `//` comments; trailing `\` = continuation → `complete`/`incomplete`/`empty` | `line_classify.hpp` |
| Command dispatch | Registry map `:name → command_result(args)`, value dispatch via `std::function` (no virtual) | `command.hpp` |
| Session model | Accumulated-source replay: each accepted input appended, whole buffer re-evaluated | `session<Engine>` |
| Transcript | Append-only ordered log with bounded retention (ring-drop oldest) | `transcript.hpp` |

## The `repl_engine` seam

`engine_concept.hpp` defines what a binding must provide:

```cpp
struct eval_outcome {
    bool ok = false;
    std::string display;                  // rendered result ("" for unit)
    std::vector<std::string> diagnostics; // fatal (non-empty ⇒ !ok)
    std::vector<std::string> notes;       // non-fatal
    std::int64_t elapsed_ns = 0;
    bool is_definition = false;           // set by the session
};

template <class E>
concept repl_engine = requires(E e, const E ce, std::string_view src,
                               const std::filesystem::path& dir) {
    { e.submit(src) }               -> std::same_as<eval_outcome>;
    { ce.classify_definition(src) } -> std::same_as<bool>;
    { e.set_module_path(dir) };
    { e.reset() };
};
```

- `submit(program)` — evaluate a **complete program** and report the outcome.
- `classify_definition(fragment)` — does this fragment introduce a top-level
  binding (a `fn`, `type`, `import`, …) the session should persist? Pure/const.
- `set_module_path(dir)` — register a module search directory.
- `reset()` — drop accumulated engine state (fresh context).

## `session<Engine>` — accumulated-source model

crank (like most compile-then-run backends) does **not** retain definitions
between `engine.run()` calls — a `fn` defined on one line is not visible on the
next. The session bridges this: it keeps the set of successful top-level
definitions and, per submission, evaluates

```
program = join(persisted_definitions, "\n") + "\n" + new_fragment
```

Flow of `submit_buffer(fragment)`:

1. record in history;
2. ask the engine `classify_definition(fragment)`;
3. assemble `program` (all prior defs + fragment) and `engine.submit(program)`;
4. if the eval succeeded **and** the fragment was a definition, append it to the
   persisted set (a broken definition never poisons the set);
5. push a `transcript_entry`.

Multi-line input is handled by `feed_line(line)`, which accumulates physical lines
until [`classify`](#input-classification) reports the buffer is balanced:

```cpp
repl::session<cranki::crank_repl_engine> s;
s.feed_line("fn F() -> Int64 {");   // → needs_more (open brace)
s.feed_line("  return 7 }");        // → evaluated
s.submit_buffer("F()");             // expression, not persisted
```

Other members: `history_prev()/history_next()` (recall), `reset(full)` (clear
defs + engine, optionally scrollback/history/paths), `set_module_path(dir)`,
`transcript()`, `definitions()`, `record_info()/record_command()`.

`submit_status`: `evaluated`, `needs_more`, `command`, `skipped`.

## Input classification

`line_classify.hpp` — a pure, language-neutral bracket/quote balancer returning
`complete` / `incomplete` / `empty`. It tracks nesting of `() [] {}` while ignoring
brackets inside `"…"` / `'…'` literals and `//` line comments, and treats a
trailing `\` as an explicit continuation. It does **not** parse the language; a
language needing exotic quoting supplies its own classifier callable.

## Meta-commands

`command.hpp` — a registry mapping a `:name` to a handler
`command_result(const std::vector<std::string>& args)`. Dispatch is by value
(`std::function`), not virtual. Lines whose first non-space char is `:` are
commands. cranki registers: `:help :reset :clear :defs :modpath <dir> :load <file>
:quit`. `command_result` carries `{handled, quit, message, is_error}`.

## Transcript

`transcript.hpp` — append-only ordered log of `transcript_entry`
(`evaluation` / `command` / `info`) with **bounded retention** (oldest dropped past
the cap; default 2048 in a session). Pure data — front-ends render it.

## Binding a language (crank)

`src/cranki/crank_binding.hpp` provides `cranki::crank_repl_engine`, holding a
`crank::engine` behind a `unique_ptr` (so `reset()` rebuilds a fresh context):

| concept method            | crank mapping |
|---------------------------|---------------|
| `submit(program)`         | `engine.run(program)`; `run_report`/`crank_error` → `eval_outcome`; `value` rendered (int64 → decimal, unit → "") |
| `classify_definition(in)` | leading token ∈ `{fn,type,struct,enum,import,extern,const,trait,impl,package}` or starts with `@` |
| `set_module_path(dir)`    | `engine.context().modules().add_path(dir)` |
| `reset()`                 | rebuild `crank::engine` |

A `static_assert(repl_engine<crank_repl_engine>)` guards the contract.

> Note: crank's scripting path currently synthesizes a minimal program, so a
> returned scalar is not guaranteed for arbitrary source. The REPL surfaces
> whatever the engine reports; result rendering widens automatically as
> `crank::value` grows (host values, floats, …).

## cranki — the FTXUI REPL

`src/cranki/main.cpp` — the Borland/Turbo-Vision-inspired terminal UI, first
consumer of ftxui in the tree.

```
┌ cranki  Modules: /pkgs                         F2 modpath  F3 open ┐
├───────────────────────────────┬───────────────────────────────────┤
│ transcript (scrollback)        │ compiler messages                 │
│  crank> fn F() -> Int64 {…}    │  ok            123 ns             │
│  crank> F()                    │                                   │
│     => 7                       │                                   │
├───────────────────────────────┴───────────────────────────────────┤
│ crank> _                                                           │
└────────────────────────────────────────────────────────────────────┘
 F1 help · F2 modpath · F3 scripts · Ctrl-L clear · Ctrl-R reset · Esc · Ctrl-Q
```

- **Input** (`ftxui::Input`): Enter submits; when a construct spans lines the
  prompt switches to `...>` (session `needs_more`); Esc cancels a pending buffer.
- **Transcript pane**: colour-coded — results green, errors red, notes yellow.
- **Compiler-messages pane**: the last submission's diagnostics + timing.
- **F2**: modal to add a module search directory (`session.set_module_path`).
- **F3**: scans module paths for `*.crank` / `*.crk` and lists them (`:load <file>`
  evaluates one as a whole program).
- **Keys**: F1 help, Ctrl-L clear scrollback, Ctrl-R reset session, Ctrl-Q quit.

The UI holds no language logic — every keystroke that evaluates goes through the
session.

## Building & running

FTXUI is already vendored + wired in `CMakeLists.txt`. The `cranki` target builds
when `src/cranki/main.cpp` exists (guarded), mirroring the crank test target's link
set plus `ftxui::component` / `ftxui::screen`.

```sh
cmake --build <build-dir> --target cranki
./cranki /path/to/modules      # extra args = initial module search paths
```

## Testing

`src/tests/test_repl.cpp` (tag `[repl]`) covers the framework **headless and
engine-agnostic** — it drives the framework with a small `mock_engine` (no crank,
no FTXUI, no src/cranki coupling): `line_classify` cases, `command_registry`
dispatch, `transcript` ordering + retention, the `repl_engine` concept, and
session behaviour (definition-persists / expression-does-not, multi-line assembly,
`set_module_path` delegation, reset + path re-apply, history). The crank binding
itself is exercised by building the `cranki` target.

```sh
cmake --build <build-dir> --target turbo_twig_tests
./turbo_twig_tests "[repl]"
```

## Extending to a new language

To add e.g. a sutra REPL, mirror cranki:

1. Write `src/sutrai/sutra_binding.hpp` with a `sutra_repl_engine` satisfying
   `repl_engine` (wrap the sutra evaluator; implement the four methods).
2. `static_assert(repl::repl_engine<sutra_repl_engine>);`
3. Drive it: `repl::session<sutra_repl_engine> s;` — headless, or add an
   `src/sutrai/main.cpp` reusing the same FTXUI shell.

No change to the core is required; `line_classify`, `command`, `transcript`, and
`session` are already language-neutral. The binding stays with its consumer, never
inside `include/languages/repl/`.
