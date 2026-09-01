# Chapter 13 — Debugging & REPL

## Why Debugging a Compiler Is Different

Debugging an ordinary program: "my variable has the wrong value."
Debugging a compiler: three separate failure layers, each needing different tools.

```
Layer 1: Source bugs
  Flux source → error diagnostic with line/col + message + suggestion

Layer 2: Compiler bugs
  Flux source → compile → dump vakya tree + type annotations + hashes
  Compare against expected EDSL tree

Layer 3: Backend bugs
  Run on CPU (reference) + SIMD + GPU → compare results
  verify_backends() detects numerical divergence
```

**Layer 1 — Source bugs**: wrong type, wrong shape, undefined name, `pure fn` calling effectful code. The frontend
catches these and emits a diagnostic. User fixes the code.

**Layer 2 — Compiler bugs**: wrong type inferred, shape constraint not generated,
`vakya_lowerer` emits wrong tag, `structural_hash` diverges between Path A and Path C. Needs per-pass inspection: token
stream, CST, typed AST, lowered vakya tree.

**Layer 3 — Backend bugs**: vakya tree is correct but emitted code is wrong. SIMD produces different numerical result
than CPU reference. Needs cross-backend comparison.

---

## The Generic REPL Framework

`include/languages/repl/repl.hpp` provides a complete, language-neutral REPL framework. Flux binds to it by implementing
the `repl::repl_engine` concept.

### Framework components

```
include/languages/repl/
  repl.hpp           — umbrella include (pulls in everything below)
  engine_concept.hpp — the binding contract (what a language must implement)
  session.hpp        — session<Engine>: state machine, def accumulation, history
  command.hpp        — command_registry: ':' meta-command dispatch
  transcript.hpp     — transcript: scrollback log of all interactions
  line_classify.hpp  — classify(): bracket balancer for multi-line detection
```

### Architecture

```
stdin/UI
   |
   v
session<flux_engine>                        ← session.hpp
   |
   |-- classify(pending_)                   ← line_classify.hpp
   |     complete? → submit_buffer()
   |     incomplete? → show continuation prompt
   |
   |-- assemble(defs_ + fragment)
   |
   |-- engine_.submit(program)              ← flux_engine implements repl_engine
   |     parse → type_infer → shape_infer → lower → analyze → run
   |     returns eval_outcome
   |
   |-- transcript_.push(entry)             ← transcript.hpp
   |
   |-- command_registry.dispatch()          ← command.hpp (for ':' lines)
   v
stdout / transcript render
```

---

## The `repl_engine` Concept

Any type satisfying this concept can drive `session<Engine>`:

```cpp
// engine_concept.hpp (verbatim from turbo_twig)
template <class E>
concept repl_engine = requires(E e, const E ce,
                               std::string_view src,
                               const std::filesystem::path& dir) {
    { e.submit(src) }               -> std::same_as<repl::eval_outcome>;
    { ce.classify_definition(src) } -> std::same_as<bool>;
    { e.set_module_path(dir) };
    { e.reset() };
};
```

`eval_outcome` — what the engine returns per submit:

```cpp
struct eval_outcome {
    bool                     ok = false;
    std::string              display;       // pre-rendered result text
    std::vector<std::string> diagnostics;  // non-empty ⇒ !ok
    std::vector<std::string> notes;        // timing, fallback notices
    std::int64_t             elapsed_ns = 0;
    bool                     is_definition = false;
};
```

---

## Building the Flux Engine Binding

### `flux_engine` — implementing `repl_engine`

```cpp
// include/languages/flux/repl_engine.hpp
#pragma once
#include <languages/repl/repl.hpp>
#include <vakya/vakya_types.hpp>
#include <lithe/lithe.hpp>

// Flux pipeline headers (see ch03–ch07)
#include "grammar.hpp"
#include "build_ast.hpp"
#include "resolve.hpp"
#include "type_inference.hpp"
#include "shape_inference.hpp"
#include "lower_vakya.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace flux {

class flux_engine {
public:
    flux_engine() {
        // Pre-install builtin type schemes once — reused across all submits
        install_builtin_types(type_env_, tara_, gen_);
    }

    // ── repl_engine contract ────────────────────────────────────────────────

    repl::eval_outcome submit(std::string_view program);

    bool classify_definition(std::string_view src) const noexcept {
        // A fragment that starts with 'fn', 'let', 'input', or 'pure fn'
        // is a definition — the session should persist it.
        auto s = trim_sv(src);
        return s.starts_with("fn ")
            || s.starts_with("let ")
            || s.starts_with("input ")
            || s.starts_with("pure fn ")
            || s.starts_with("extern fn ");
    }

    void set_module_path(const std::filesystem::path& /*dir*/) noexcept {
        // Module loading deferred to future work
    }

    void reset() noexcept {
        // Drop accumulated type env entries that came from user definitions
        type_env_.clear();
        install_builtin_types(type_env_, tara_, gen_);
    }

    // ── Flux-specific accessors (not part of repl_engine contract) ──────────

    // Enable per-pass debug tracing to stderr
    void set_debug(bool on) noexcept { debug_ = on; }

    // Return the last lowered vakya tree (for :vakya command)
    std::optional<lithe::shared_expr> last_expr() const noexcept { return last_expr_; }

    // Return the analysis record for the last expression (for :type, :effects)
    std::optional<vakya::types::analysis_record> last_record() const noexcept {
        return last_record_;
    }

    // Return the structural hash of the last expression (for :hash)
    std::optional<uint64_t> last_hash() const noexcept {
        if (!last_expr_) return std::nullopt;
        return lithe::structural_hash(*last_expr_);
    }

private:
    static std::string_view trim_sv(std::string_view s) noexcept {
        while (!s.empty() && (s.front()==' '||s.front()=='\t')) s.remove_prefix(1);
        return s;
    }

    // Persistent across submits (definitions accumulate here)
    vakya::types::type_arena           tara_;
    vakya::types::type_var_generator   gen_;
    vakya::types::substitution         subst_;
    std::unordered_map<std::string, vakya::types::type_ref> type_env_;
    vakya::types::analysis_store       astore_;
    lithe::dag_builder                 dag_;

    // Last evaluated expression (commands can inspect it)
    std::optional<lithe::shared_expr>                    last_expr_;
    std::optional<vakya::types::analysis_record>         last_record_;

    bool debug_ = false;
};

} // namespace flux
```

### `flux_engine::submit` — the full pipeline

```cpp
repl::eval_outcome flux_engine::submit(std::string_view program) {
    const auto t0 = std::chrono::steady_clock::now();
    repl::eval_outcome out;

    // ── 1. Parse ──────────────────────────────────────────────────────────
    auto parse_result = flux::parse(program);
    if (!parse_result.success) {
        for (auto const& d : parse_result.diagnostics())
            out.diagnostics.push_back(
                std::format("  error at {}: {}", d.offset, d.message()));
        return out;
    }

    // ── 2. Build AST ──────────────────────────────────────────────────────
    auto arena = flux::build_ast(parse_result, program);

    // ── 3. Name resolution ────────────────────────────────────────────────
    flux::resolver resolver{arena};
    auto resolve_errs = resolver.resolve(0);
    if (!resolve_errs.empty()) {
        for (auto const& e : resolve_errs)
            out.diagnostics.push_back("  error: " + e.message);
        return out;
    }

    // ── 4. Type inference ─────────────────────────────────────────────────
    flux::type_inferrer inferrer{arena, type_env_, tara_, gen_, subst_};
    auto type_errs = inferrer.infer(0);
    if (!type_errs.empty()) {
        for (auto const& e : type_errs)
            out.diagnostics.push_back(
                std::format("  type error: {} ({})", e.message, e.lhs_type));
        return out;
    }

    // ── 5. Shape inference ────────────────────────────────────────────────
    flux::shape_inferrer shape_inf{arena, tara_, subst_};
    auto shape_errs = shape_inf.infer(0);
    if (!shape_errs.empty()) {
        for (auto const& e : shape_errs)
            out.diagnostics.push_back("  shape error: " + e.message);
        return out;
    }

    // ── 6. Vakya lowering ─────────────────────────────────────────────────
    flux::vakya_lowerer lowerer{arena, dag_};
    auto expr = lowerer.lower(0);
    last_expr_ = expr;

    // ── 7. Analysis (effects, capabilities) ──────────────────────────────
    vakya::types::analyze_options opts{ .emit_effects = true, .emit_caps = true };
    vakya::types::analyze(expr, type_env_, /*solver*/{}, tara_, gen_, subst_,
                          astore_, opts);

    if (auto* rec = astore_.find_for(expr)) {
        last_record_ = *rec;
        // Render type info as display string
        out.display = std::format("{}", inferrer.type_name(rec->type));
    }

    // ── 8. Execute on CPU ─────────────────────────────────────────────────
    // (expression-only submits; definitions just store the type)
    if (!classify_definition(program)) {
        auto result = lithe::run<lithe::BackendKind::CPU>(expr);
        out.display = render_value(result);
    }

    const auto t1 = std::chrono::steady_clock::now();
    out.elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    out.ok = true;
    return out;
}
```

---

## Building the REPL: `flux_repl`

### Session setup with command registry

```cpp
// include/languages/flux/flux_repl.hpp
#pragma once
#include <languages/repl/repl.hpp>
#include "repl_engine.hpp"
#include <iostream>
#include <string>

namespace flux {

// flux_repl: wraps session<flux_engine> + command_registry + headless I/O loop.
class flux_repl {
public:
    flux_repl() : session_(flux_engine{}) {
        register_commands();
    }

    // Run interactive loop until EOF or :quit
    void run(std::istream& in  = std::cin,
             std::ostream& out = std::cout);

    // Run a script file (no prompts)
    void run_script(std::string_view source, std::ostream& out = std::cout);

private:
    void register_commands();
    void print_prompt(std::ostream& out, bool continuation);
    void render_outcome(repl::eval_outcome const& o, std::ostream& out);

    repl::session<flux_engine>  session_;
    repl::command_registry      commands_;
};

} // namespace flux
```

### `register_commands` — binding `:` commands

```cpp
void flux_repl::register_commands() {
    // :help — list all commands
    commands_.add("help", [&](auto const& /*args*/) -> repl::command_result {
        std::string msg = "Available commands:\n";
        for (auto const& [name, spec] : commands_.commands())
            msg += std::format("  :{:<12}  {}\n", name, spec.help);
        return { .handled=true, .message=msg };
    }, ":help", "show this help");

    // :quit / :q — exit
    auto quit_fn = [](auto const&) -> repl::command_result {
        return { .handled=true, .quit=true, .message="bye" };
    };
    commands_.add("quit", quit_fn, ":quit", "exit the REPL");
    commands_.add("q",    quit_fn, ":q",    "exit the REPL");

    // :reset — drop accumulated definitions, restart engine
    commands_.add("reset", [&](auto const&) -> repl::command_result {
        session_.reset();
        return { .handled=true, .message="session reset" };
    }, ":reset", "clear all accumulated definitions");

    // :type — show type of last expression
    commands_.add("type", [&](auto const& /*args*/) -> repl::command_result {
        auto rec = session_.engine().last_record();
        if (!rec) return { .handled=true, .is_error=true,
                           .message="no expression evaluated yet" };
        return { .handled=true,
                 .message=std::format("type: {}", type_name_for(*rec)) };
    }, ":type", "show type of last expression");

    // :vakya — dump vakya tree of last expression
    commands_.add("vakya", [&](auto const& /*args*/) -> repl::command_result {
        auto expr = session_.engine().last_expr();
        if (!expr) return { .handled=true, .is_error=true,
                            .message="no expression evaluated yet" };
        return { .handled=true, .message=dump_vakya_str(*expr) };
    }, ":vakya", "dump vakya::node tree of last expression");

    // :effects — show effects of last expression
    commands_.add("effects", [&](auto const& /*args*/) -> repl::command_result {
        auto rec = session_.engine().last_record();
        if (!rec) return { .handled=true, .is_error=true,
                           .message="no expression evaluated yet" };
        auto msg = rec->effects.bits == 0
            ? "pure (no effects)"
            : effects_str(rec->effects);
        return { .handled=true, .message=msg };
    }, ":effects", "show effects of last expression");

    // :hash — structural hash of last expression
    commands_.add("hash", [&](auto const& /*args*/) -> repl::command_result {
        auto h = session_.engine().last_hash();
        if (!h) return { .handled=true, .is_error=true,
                         .message="no expression evaluated yet" };
        return { .handled=true,
                 .message=std::format("structural_hash: 0x{:016x}", *h) };
    }, ":hash", "structural hash of last expression");

    // :debug — toggle per-pass debug tracing
    commands_.add("debug", [&](auto const& /*args*/) -> repl::command_result {
        // toggle
        auto& eng = session_.engine();
        static bool on = false;
        on = !on;
        eng.set_debug(on);
        return { .handled=true,
                 .message=std::format("debug tracing: {}", on ? "ON" : "OFF") };
    }, ":debug", "toggle per-pass debug tracing to stderr");

    // :defs — list all accumulated top-level definitions
    commands_.add("defs", [&](auto const& /*args*/) -> repl::command_result {
        auto const& defs = session_.definitions();
        if (defs.empty())
            return { .handled=true, .message="(no definitions)" };
        std::string msg;
        for (auto const& d : defs) {
            msg += "  ";
            // Print first 60 chars of each definition
            msg += d.substr(0, std::min<std::size_t>(d.size(), 60));
            if (d.size() > 60) msg += "...";
            msg += '\n';
        }
        return { .handled=true, .message=msg };
    }, ":defs", "list accumulated top-level definitions");
}
```

### Main loop

```cpp
void flux_repl::run(std::istream& in, std::ostream& out) {
    out << "Flux REPL  (type :help for commands, :quit to exit)\n\n";

    std::string line;
    while (true) {
        print_prompt(out, session_.has_pending());
        if (!std::getline(in, line)) break;  // EOF

        // ── Meta-command? (:help, :reset, ...) ──────────────────────────
        if (commands_.is_command(line)) {
            auto cr = commands_.dispatch(line);
            if (!cr.message.empty())
                out << cr.message << '\n';
            session_.record_command(line, cr.message, cr.is_error);
            if (cr.quit) break;
            continue;
        }

        // ── Flux line — feed to session ──────────────────────────────────
        auto status = session_.feed_line(line);

        switch (status) {
        case repl::submit_status::skipped:
            break;  // blank line

        case repl::submit_status::needs_more:
            // open brace/paren detected — show continuation prompt next iteration
            break;

        case repl::submit_status::evaluated: {
            auto const* entry = session_.transcript().last();
            if (!entry) break;
            render_outcome(entry->outcome, out);
            break;
        }

        case repl::submit_status::command:
            break;  // handled above (shouldn't reach here)
        }
    }

    out << "\nbye\n";
}

void flux_repl::print_prompt(std::ostream& out, bool continuation) {
    out << (continuation ? "  ... " : "flux> ");
    out.flush();
}

void flux_repl::render_outcome(repl::eval_outcome const& o, std::ostream& out) {
    if (!o.ok) {
        for (auto const& d : o.diagnostics)
            out << d << '\n';
        return;
    }
    if (!o.display.empty())
        out << o.display << '\n';
    if (!o.notes.empty())
        for (auto const& n : o.notes)
            out << "  -- " << n << '\n';
}
```

### Script runner

```cpp
void flux_repl::run_script(std::string_view source, std::ostream& out) {
    // Split source on newlines, feed each line to session
    std::size_t pos = 0;
    while (pos <= source.size()) {
        auto nl = source.find('\n', pos);
        auto line = nl == std::string_view::npos
            ? source.substr(pos)
            : source.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos ? source.size() + 1 : nl + 1);

        if (commands_.is_command(line)) {
            auto cr = commands_.dispatch(line);
            if (!cr.message.empty()) out << cr.message << '\n';
            if (cr.quit) return;
            continue;
        }

        auto status = session_.feed_line(line);
        if (status == repl::submit_status::evaluated) {
            auto const* e = session_.transcript().last();
            if (e) render_outcome(e->outcome, out);
        }
    }
}
```

---

## Multi-Line Input: How `line_classify` Works

`repl::classify()` (from `line_classify.hpp`) determines whether the accumulated buffer is a complete construct or needs
more input. It counts bracket depth and respects string literals and line comments:

```cpp
// classify() returns:
//   input_state::complete   — balanced, safe to evaluate
//   input_state::incomplete — open bracket / unterminated string — show '...'
//   input_state::empty      — only whitespace
```

Flux uses `--` for comments, but `line_classify` tracks `//` line comments. Since Flux comments are `--`, they do NOT
interact with the bracket counter — that's fine, they contain no brackets. For Flux, provide a thin wrapper that strips
`--` comments before classifying:

```cpp
// flux_classify: wraps repl::classify, pre-strips -- comments
repl::input_state flux_classify(std::string_view src) noexcept {
    // Build a view with -- comment spans replaced by spaces
    // (cheap: just scan for '--' outside strings, treat rest of line as spaces)
    // For most Flux input this is a no-op — bracket balancing is unaffected.
    // Use repl::classify directly since '--' has no brackets:
    return repl::classify(src);
}
```

Session uses this via `session<flux_engine>` which calls `repl::classify(pending_)`.

Multi-line example trace:

```
User types: "fn add(x : f32, y : f32) -> f32 {"
                                                 ↑ depth=1 (open brace)
  → classify returns incomplete → show '...' prompt

User types: "    x + y"
                    ↑ depth still 1 (no new brackets)
  → classify returns incomplete → show '...' prompt

User types: "}"
             ↑ depth=0 (close brace)
  → classify returns complete → session.submit_buffer() called
```

---

## Diagnostic Structures

These live in `include/languages/generic/core/`:

### `source_location` (from `generic/core/source_location.hpp`)

```cpp
struct source_location {
    uint32_t         line;    // 1-based
    uint32_t         column;  // 1-based
    uint32_t         offset;  // byte offset from source start
    uint32_t         length;  // token length in bytes
    std::string_view file;    // filename or "<repl>"
};
```

### `rich_diagnostic` (from `generic/core/rich_diagnostic.hpp`)

The generic diagnostics infrastructure provides rustc-style rendering:

```cpp
#include <languages/generic/core/rich_diagnostic.hpp>

// Emit a type mismatch error
lang::rich_diagnostic diag{
    .severity = lang::diagnostic_severity::error,
    .code     = "E0010",
    .message  = "type mismatch",
    .loc      = { .line=3, .column=9, .length=9, .file="<repl>" },
    .label    = "expected f32, found bool",
    .help     = "use a numeric expression instead of bool",
};

// Render to stderr (rustc-style underline + caret)
diag.render(source_text, std::cerr);
```

Output:

```
error[E0010]: type mismatch
  --> <repl>:3:9
   |
 3 |     let x = 1.0 + true
   |             ^^^^^^^^^  expected f32, found bool
   |
   = help: use a numeric expression instead of bool
```

---

## Debug Tracing in Each Compiler Pass

When `debug_` is true, `flux_engine::submit` enables tracing at each pass. This uses the `debug_context` struct threaded
through the pipeline:

```cpp
struct debug_context {
    bool         enabled          = false;
    bool         trace_tokens     = false;
    bool         trace_types      = false;
    bool         trace_shapes     = false;
    bool         trace_lowering   = false;
    bool         trace_analysis   = false;
    std::ostream* out             = &std::cerr;
};
```

Sample output with `:debug` enabled:

```
[tokens]  let distance = sqrt ( x * x + y * y )
           kw_let ident eq ident lparen ident star ident plus ident star ident rparen

[types]   x      : f32  (from annotation)
          x*x    : f32  (mul: f32 × f32 → f32)
          y*y    : f32
          x*x+y*y: f32  (add: f32 + f32 → f32)
          sqrt(.): f32  (sqrt : f32→f32, inst from ∀α.α→α)
          distance: f32

[shapes]  (no tensors — all scalars)

[lower]   sqrt_tag
            add_tag
              mul_tag(sym(x), sym(x))  [hash: 0x1234abcd]
              mul_tag(sym(y), sym(y))  [hash: 0x5678ef01]
          → shared_expr hash: 0x7f3a9b2c8d41e005

[analysis] effects: pure (0x0000000000000000)
           caps:    none (0x0000000000000000)
```

---

## Introspection Commands in Action

### `:vakya` — dump the vakya tree

The command calls `dump_vakya_str()` which walks the `lithe::shared_expr` tree:

```cpp
std::string dump_vakya_str(lithe::shared_expr const& expr, int depth = 0) {
    std::string result;
    std::string indent(depth * 2, ' ');
    result += std::format("{}{}\n", indent, lithe::tag_symbol(expr));
    for (auto const& child : lithe::children(expr))
        result += dump_vakya_str(child, depth + 1);
    return result;
}
```

Example:

```
flux> let C = matmul(A, B)
C : tensor<f32>  shape=[4,16]

flux> :vakya
matmul
  input A  [tensor<f32>  shape=[4,8]]
  input B  [tensor<f32>  shape=[8,16]]
```

### `:type` — full type + shape

```
flux> :type
tensor<f32>
  shape:  [4, 16]
  rank:   2
```

### `:effects` — effect set

```
flux> let d = sqrt(x*x + y*y)
d : f32

flux> :effects
pure (no effects)

flux> fn report(x : f32) { print(x) }
report : fn(f32) -> unit

flux> :effects
effects: IO
```

### `:hash` — structural fingerprint

```
flux> :hash
structural_hash: 0x7f3a9b2c8d41e005
```

### `:defs` — what the session remembers

```
flux> :defs
  fn add(x : f32, y : f32) -> f32 { x + y }
  let pi = 3.14159
  input A : tensor<f32>[4,8]
```

---

## Complete Copy-Paste REPL Program

### `flux_repl_main.cpp`

```cpp
// flux_repl_main.cpp — build and run the Flux REPL
//
// Build:
//   clang++ -std=c++23 -I<turbo_twig>/include flux_repl_main.cpp -o flux_repl
//
// Run (interactive):
//   ./flux_repl
//
// Run (script):
//   ./flux_repl my_program.flux

#include <languages/repl/repl.hpp>    // generic REPL framework
#include <lithe/lithe.hpp>
#include <languages/samasa/samasa.hpp>
#include <print>
#include <fstream>
#include <iostream>
#include <sstream>

// Flux pipeline headers
#include "flux/grammar.hpp"
#include "flux/build_ast.hpp"
#include "flux/resolve.hpp"
#include "flux/type_inference.hpp"
#include "flux/shape_inference.hpp"
#include "flux/lower_vakya.hpp"
#include "flux/repl_engine.hpp"
#include "flux/flux_repl.hpp"

int main(int argc, char* argv[]) {
    flux::flux_repl repl;

    if (argc > 1) {
        // Script mode: load file, run without prompts
        std::ifstream file(argv[1]);
        if (!file.is_open()) {
            std::println(std::cerr, "error: cannot open '{}'", argv[1]);
            return 1;
        }
        std::ostringstream oss;
        oss << file.rdbuf();
        repl.run_script(oss.str(), std::cout);
    } else {
        // Interactive mode
        repl.run(std::cin, std::cout);
    }
}
```

### Expected interactive session

```text
Flux REPL  (type :help for commands, :quit to exit)

flux> 1 + 2
3 : i32

flux> 3.14 * 2.0
6.28 : f64

flux> let x = 10.0
(definition)

flux> x * x
100.0 : f64

flux> fn identity(a) { a }
(definition)

flux> identity(42)
42 : i64

flux> identity(3.14)
3.14 : f64

flux> :type
f64

flux> input A : tensor<f32>[4,8]
(definition)

flux> input B : tensor<f32>[8,16]
(definition)

flux> let C = matmul(A, B)
(definition)

flux> :type
tensor<f32>  shape=[4,16]

flux> :vakya
matmul
  input A  [tensor<f32>  shape=[4,8]]
  input B  [tensor<f32>  shape=[8,16]]

flux> :effects
pure (no effects)

flux> :hash
structural_hash: 0x7f3a9b2c8d41e005

flux> :defs
  input A : tensor<f32>[4,8]
  input B : tensor<f32>[8,16]
  let C = matmul(A, B)
  fn identity(a) { a }

flux> 1.0 + true
  type error: type mismatch in operator + (f32 vs bool)

flux> pure fn square(x : f32) -> f32 { x * x }
(definition)

flux> :effects
pure (no effects)

flux> fn multi_line(x : f32) {
  ...     x * x + 1.0
  ... }
(definition)

flux> :reset
session reset

flux> :defs
(no definitions)

flux> :help
Available commands:
  :help          show this help
  :quit          exit the REPL
  :q             exit the REPL
  :reset         clear all accumulated definitions
  :type          show type of last expression
  :vakya         dump vakya::node tree of last expression
  :effects       show effects of last expression
  :hash          structural hash of last expression
  :debug         toggle per-pass debug tracing to stderr
  :defs          list accumulated top-level definitions

flux> :quit
bye
```

---

## Script Mode Example

Save as `demo.flux`:

```flux
-- demo.flux: basic Flux REPL script
fn square(x : f32) -> f32 { x * x }
fn distance(x : f32, y : f32) -> f32 { sqrt(x*x + y*y) }

square(3.0)
distance(3.0, 4.0)
```

Run:

```bash
./flux_repl demo.flux
```

Output:

```text
9.0 : f32
5.0 : f32
```

---

## How the Session Accumulates Definitions

`session<flux_engine>` implements the **accumulated-source model**:

1. User submits `fn add(x, y) { x + y }` → `classify_definition` returns `true`
2. Session calls `engine_.submit("fn add(x, y) { x + y }")` → `ok`, `is_definition=true`
3. Session appends the fragment to `defs_`
4. User submits `add(1.0, 2.0)`
5. Session calls `engine_.submit("fn add(x, y) { x + y }\nadd(1.0, 2.0)")`
   — full program including all prior definitions
6. Engine compiles and runs the complete program
7. Result: `3.0 : f32`

This means every `submit()` call receives a **complete, self-contained program**. The engine never needs to maintain
incremental state — it always compiles from scratch with the full definition context. Simple, correct, and matches how
Flux's pipeline works.

```
defs_:  ["fn add(x,y){x+y}", "let pi=3.14159"]
new input: "add(pi, pi)"

assembled program passed to submit():
  fn add(x,y){x+y}
  let pi=3.14159
  add(pi, pi)
```

---

## Hash Invariant Checker

After each submit, verify Path A == Path C to catch lowering bugs:

```cpp
void flux_engine::check_invariant(std::string_view flux_src,
                                   lithe::shared_expr const& edsl_expr) {
    auto result = submit(flux_src);
    if (!result.ok || !last_expr_) return;

    auto hA = lithe::structural_hash(*last_expr_);
    auto hC = lithe::structural_hash(edsl_expr);

    if (hA != hC) {
        std::println(std::cerr, "INVARIANT VIOLATION:");
        std::println(std::cerr, "  Flux source: {}", flux_src);
        std::println(std::cerr, "  Flux hash:   0x{:016x}", hA);
        std::println(std::cerr, "  EDSL hash:   0x{:016x}", hC);
        // Dump both trees for comparison
        std::println(std::cerr, "Flux tree:");
        lithe::emit::dump(*last_expr_, std::cerr);
        std::println(std::cerr, "EDSL tree:");
        lithe::emit::dump(edsl_expr, std::cerr);
    }
}
```

Usage in tests:

```cpp
auto x = lithe::make_symbolic("x");
auto y = lithe::make_symbolic("y");
auto edsl = lithe::sqrt(x*x + y*y);

flux_engine eng;
eng.check_invariant("sqrt(x*x + y*y)", edsl);
// No output → invariant holds
```

---

## Source Locations and Error Codes

Each compiler pass emits diagnostics with source locations. The error code scheme:

| Range       | Pass             | Example                                  |
|-------------|------------------|------------------------------------------|
| E0001–E0009 | Lexer / scanner  | E0001: unexpected character              |
| E0010–E0019 | Type mismatch    | E0010: cannot unify f32 with bool        |
| E0020–E0029 | Shape mismatch   | E0020: matmul inner dim mismatch         |
| E0030–E0039 | Effect violation | E0030: pure fn has IO effect             |
| E0040–E0049 | Name resolution  | E0040: undefined name                    |
| E0050–E0059 | FFI              | E0050: unknown extern fn                 |
| E0100+      | Backend          | E0100: unsupported capability on backend |

---

## What We Have

| Component                     | Source                    | Purpose                                  |
|-------------------------------|---------------------------|------------------------------------------|
| `repl::repl_engine` concept   | `repl/engine_concept.hpp` | Binding contract                         |
| `repl::session<E>`            | `repl/session.hpp`        | State machine, def accumulation, history |
| `repl::command_registry`      | `repl/command.hpp`        | `:` meta-command dispatch                |
| `repl::transcript`            | `repl/transcript.hpp`     | Scrollback log                           |
| `repl::classify()`            | `repl/line_classify.hpp`  | Multi-line bracket balancing             |
| `repl::eval_outcome`          | `repl/engine_concept.hpp` | Per-submit result                        |
| `flux::flux_engine`           | `flux/repl_engine.hpp`    | Flux binding (implements `repl_engine`)  |
| `flux::flux_repl`             | `flux/flux_repl.hpp`      | Full REPL with commands + I/O loop       |
| `:type/:vakya/:effects/:hash` | `flux_repl.hpp`           | Introspection commands                   |
| `:debug`                      | `flux_repl.hpp`           | Per-pass trace toggle                    |
| `check_invariant()`           | `flux/repl_engine.hpp`    | Path A == Path C verifier                |

## Next

Return to [Chapter 0 → Overview](ch00_overview.md) for the full picture, or explore
[Chapter 12 → FFI](ch_ffi.md) to call C++ functions from Flux.
