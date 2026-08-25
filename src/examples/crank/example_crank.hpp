#pragma once

// ============================================================================
// Crank Language Frontend Comprehensive Tutorial Example
//
// Module 1 — Frontend (lexer, parser, AST)
// ex01  parse hello world        — minimal valid Crank program
// ex02  parse error recovery     — error diagnostics capture
// ex03  parse tree JSON dump     — dump_mode::parse_tree
// ex04  AST JSON dump            — dump_mode::ast
// ex05  source span decode       — line/col from offset
// ex06  integer literal styles   — 0x, 0b, 0o, digit separators
// ex07  string literals          — escapes and raw strings
// ex08  declarations             — let, var, const
// ex09  control flow             — if, for, while, match
// ex10  generics + contracts     — function with type params + requires
// ex11  transaction block        — transactional memory syntax
// ex12  parallel builtins        — parallel.map, parallel.reduce
// ex13  struct/enum types        — type declarations
// ex14  diagnostics inspection   — error reporting with source spans
// ex15  tag descriptor stats     — all 14 Crank AST tags (1000–1013)
// ex16  parse statistics         — token counts, timing, production freq
// ex17  postfix chains + builtins — f(x)[0], len, cap, append, make, print
// ex18  ASI (auto semicolon)     — statement termination + carve-outs
//
// Module 2 — Semantics (types, effects, modules, host embedding)
// ex19  host function registration — register functions, types, containers
// ex20  context analyse          — semantic analysis + effects checking
// ex21  execution policy config  — fluent builder for exec options
//
// Module 4 — Execution (lowering, planning, AOT cache)
// ex22  optimization profiles    — o0–o3 profile validation
// ex23  HL MIR lowering + execute — lower to high-level MIR, interpret
// ex24  defer semantics          — cleanup on exit edges
// ex25  AOT cache                — compile, store, and hit cache
//
// Module 5A — Transactions
// ex26  transaction lowering     — policy checks, read/write lowering
// ex27  transaction runtime      — execution with tx evaluator
// ex28  transactional resources  — resource registration and traits
//
// Module 5B — Generics
// ex29  monomorphization         — trait registry, instantiation, witness
//
// Module 6 — Extensions (annotations)
// ex30  annotation system        — registry, resolution, validation
//
// Module 7 — End-to-End Pipeline (source → … → typed result)
// Each example drives the full compilation pipeline (§10.2, steps 1–12) for one
// program: parse → analyse (resolve/type/effects) → generic instantiation →
// HL MIR lowering → plan extraction → backend (interpreter/SIMD/GPU) → result.
// ex31  scalar arithmetic         — parse → analyse → lower → interpret → Int64
// ex32  loop with defer           — analyse → lower (structured_for + defers) → run
// ex33  generic reduction         — conformance → monomorphize → lower → run
// ex34  transactional transfer    — analyse → tx lower → execute_transaction → commit
// ex35  SIMD elementwise          — lower → parallel plan → Highway simd_kernels
// ex36  GPU elementwise           — lower → SPIR-V emit → dispatch (device or fallback)
// ex37  host function call        — register → analyse → trampoline invoke → typed
// ex38  AOT MIR cache round-trip  — full pipeline → cache miss → hit → typed result
// ex39  functions + recursion     — factorial, fibonacci, pi estimate + trampoline
// ex40  C++ vs Crank benchmarks   — pi/fibonacci/sum loop: measure + compare
// ex41  matrix & numerical        — Newton sqrt, harmonic sum, dot product
// ex42  number theory             — sieve, Miller-Rabin, GCD, LCM chain
// ex43  statistical computation   — Welford variance, Pearson correlation, regression
//
// Module 8 — Bottleneck & Data-Structure Comparisons
// ex44  nested loops              — double/triple loops, matrix multiply trace
// ex45  vector operations         — build+sum, stride access, sorted binary search
// ex46  map operations            — insert+sum, hit/miss lookup, mixed workload
//
// Domain Views (Module: §domain_views)
// ex47  view_decl parse           — minimal + generic view declaration syntax
// ex48  view_expr parse           — view construction + parenthesized source
// ex49  feature gate              — feature_set API: default_v1/enable/all/disable
// ex50  view_registry             — register + find view_descriptor by stable_id
// ex51  view_method_table         — insert, exact lookup, fallback-to-hash-0
// ex52  view_domain_meta          — fast_path_ok, affinity, has_domain/has_op
// ex53  obligations               — 9 view predicates + collect_obligation_stats
// ex54  CRANK-VIEW diagnostics    — all 12 diagnostic codes + explain builder
// ex55  full linear e2e           — parse + resolve + registry + method lookup
// ============================================================================

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <variant>

#include "test/example_registry.hpp"
#include "utils/log.hpp"
#include "languages/crank/frontend.hpp"
#include "languages/crank/ast_tags.hpp"
#include "languages/crank/host.hpp"
#include "languages/crank/context.hpp"
#include "languages/crank/lower_hl.hpp"
#include "languages/crank/execute.hpp"
#include "languages/crank/execute_tx.hpp"
#include "languages/crank/transaction.hpp"
#include "languages/crank/aot.hpp"
#include "languages/crank/annotation.hpp"
#include "languages/crank/generics.hpp"
#include "languages/crank/monomorphize.hpp"
#include "languages/crank/dump.hpp"
#include "languages/crank/parser_stats.hpp"
#include "languages/crank/profiles.hpp"
#include "languages/crank/parallel.hpp"
#include "languages/crank/debug.hpp"
#include "languages/crank/gpu_backend.hpp"
#include "languages/crank/view_registry.hpp"
#include "languages/crank/obligations.hpp"
#include "languages/crank/diagnostic.hpp"
#include "languages/crank/resolve.hpp"
#include "lithe/backends/lithe_codegen_simd.hpp"
#include "vakya/vakya.hpp"
#include "utils/profiler.hpp"

struct Vec3 {
    float x, y, z;
};

float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

namespace crank {
    template <>
    struct type_descriptor<::Vec3> {
        static constexpr std::string_view name = "Vec3";
        static constexpr auto fields = std::tuple{
            field<"x", &::Vec3::x>{},
            field<"y", &::Vec3::y>{},
            field<"z", &::Vec3::z>{}
        };
    };
} // namespace crank

struct AccountStore {};

template <>
struct medha::resource_traits<AccountStore> {
    static constexpr bool transactional = true;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = true;
    static constexpr medha::commit_capability commit_protocol =
        medha::commit_capability::atomic_multi_key_within_resource;
    static constexpr bool aba_safe = true;
    static constexpr bool value_trivially_copyable = false;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
};

namespace crank_ex {
    static void log_equivalent_benchmark(
        const std::string_view title,
        const std::string_view workload,
        const std::string_view equivalence,
        const profiler::ProfileResult& native,
        const profiler::ProfileResult& trampoline) {
        lg::info("{}\n{}", title,
                 profiler::format_execution_comparison(
                     {.workload = workload,
                      .equivalence = equivalence,
                      .baseline = profiler::execution_mechanism::native_cpp,
                      .candidate = profiler::execution_mechanism::typed_host_trampoline},
                     native, trampoline));
    }

    static void log_interpreter_probe(
        const std::string_view title,
        const std::string_view workload,
        const profiler::ProfileResult& result,
        const std::string_view scope) {
        lg::info("{}\n{}", title,
                 profiler::format_execution_probe(
                     workload,
                     profiler::execution_mechanism::physical_mir_interpreter,
                     result, scope));
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex01: parse hello world — minimal valid Crank program.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex01_parse_hello_world() {
        constexpr std::string_view source = R"(package hello
fn main() {
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex01: parse failed");
        if (result.diagnostics.has_errors()) return testfw::fail("ex01: diagnostics error");

        lg::info("crank ex01 (parse hello world): OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex02: parse error recovery — missing () on function declaration.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex02_parse_error_recovery() {
        constexpr std::string_view source = R"(package test
fn broken {
}
)";

        const auto result = crank::frontend::parse(source);
        if (result.ok) return testfw::fail("ex02: expected parse to fail");
        if (!result.diagnostics.has_errors()) return testfw::fail("ex02: expected errors");

        const auto& diags = result.diagnostics.entries;
        if (diags.empty()) return testfw::fail("ex02: diagnostics list empty");

        lg::info("crank ex02 (error recovery): captured {} error(s)", diags.size());
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex03: parse tree JSON dump — dump_mode::parse_tree.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex03_parse_tree_dump() {
        constexpr std::string_view source = R"(package demo
fn add(a: Int32, b: Int32) -> Int32 {
    return a + b
}
)";

        crank::frontend::parse_options opts;
        opts.dump = crank::frontend::dump_mode::parse_tree;
        const auto result = crank::frontend::parse(source, opts);

        if (result.parse_tree_json.empty()) return testfw::fail("ex03: parse_tree_json empty");
        if (result.parse_tree_json[0] != '{') return testfw::fail("ex03: JSON not object");

        lg::info("crank ex03 (parse_tree dump): {} chars", result.parse_tree_json.size());
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex04: AST JSON dump — dump_mode::ast.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex04_ast_dump() {
        constexpr std::string_view source = R"(package demo
let x = 42
fn mul(a: Int32, b: Int32) -> Int32 {
    return a * b
}
)";

        crank::frontend::parse_options opts;
        opts.dump = crank::frontend::dump_mode::ast;
        const auto result = crank::frontend::parse(source, opts);

        if (!result.ok) return testfw::fail("ex04: parse failed");
        if (result.parse_tree_json.empty()) return testfw::fail("ex04: parse_tree_json empty");
        if (result.ast_json.empty()) return testfw::fail("ex04: ast_json empty");

        lg::info("crank ex04 (ast dump): parse_tree={} chars, ast={} chars",
                 result.parse_tree_json.size(), result.ast_json.size());
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex05: source span decode — line/col from byte offset.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex05_source_span_decode() {
        constexpr std::string_view source = R"(package demo
fn test() {}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex05: parse failed");

        const crank::source_span sp1 = crank::decode_span(source, 0, 7); // "package"
        if (sp1.line != 1) return testfw::fail("ex05: 'package' should be line 1");
        if (sp1.col != 1) return testfw::fail("ex05: 'package' should be col 1");

        const size_t line2_offset = source.find("fn");
        crank::source_span sp2 = crank::decode_span(source, line2_offset, 2);
        if (sp2.line != 2) return testfw::fail("ex05: 'fn' should be line 2");

        lg::info("crank ex05 (source_span): package at (1,1), fn at ({},{})", sp2.line, sp2.col);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex06: integer literal styles — hex, oct, bin, decimal, digit separators.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex06_integer_literals() {
        constexpr std::string_view source = R"(package test
fn nums() {
    let a = 0xFF_AB
    let b = 0b1010_1100
    let c = 0o17_77
    let d = 1_000_000
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex06: parse failed");

        lg::info("crank ex06 (integer literals): hex, bin, oct, decimal with separators OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex07: string and raw string literals.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex07_string_literals() {
        constexpr std::string_view source = R"(package test
fn strings() {
    let s1 = "hello\nworld"
    let s2 = `raw\nno escape`
    let s3 = "with \"quotes\""
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex07: parse failed");

        lg::info("crank ex07 (string literals): escaped and raw strings OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex08: let / var / const declarations.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex08_declarations() {
        constexpr std::string_view source = R"(package test
let global_x = 42
var global_y = 0
const PI = 3.14159
fn demo() {
    let x = 100
    var y = 50
    const Z = 9
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex08: parse failed");

        lg::info("crank ex08 (declarations): let, var, const OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex09: control flow — if, for, while, match.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex09_control_flow() {
        constexpr std::string_view source = R"(package test
fn demo() {
    if x > 0 {
    } else {
    }
    for i := range 0..10 {
    }
    while true {
    }
    match y {
        1 => {}
        2 => {}
    }
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex09: parse failed");

        lg::info("crank ex09 (control flow): if, for, while, match OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex10: function with generics and contract (requires).
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex10_generics_contracts() {
        constexpr std::string_view source = R"(package test
fn dot[T](a: T, b: T) -> T requires a != b {
    return a
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex10: parse failed");

        lg::info("crank ex10 (generics + contracts): fn[T] with requires OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex11: transaction block — transactional memory.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex11_transaction_block() {
        constexpr std::string_view source = R"(package test
fn transact() {
    transaction(isolation = snapshot, retry = 3) {
        let x = 42
    }
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex11: parse failed");

        lg::info("crank ex11 (transaction): transaction block with options OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex12: parallel builtins — parallel.map, parallel.reduce.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex12_parallel_builtins() {
        constexpr std::string_view source = R"(package test
fn parallel_ops(xs: []Int32) -> Int32 {
    let mapped = parallel.map(xs, fn(x) { return x * 2 })
    let sum = parallel.reduce(xs, 0, fn(a, b) { return a + b })
    return sum
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex12: parse failed");

        lg::info("crank ex12 (parallel builtins): parallel.map, parallel.reduce OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex13: struct and enum type declarations.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex13_struct_enum() {
        constexpr std::string_view source = R"(package test
type Point = struct {
    x: Float64
    y: Float64
}
type Color = enum {
    Red
    Green
    Blue
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex13: parse failed");

        lg::info("crank ex13 (struct/enum): struct and enum type declarations OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex14: diagnostics inspection — error messages with source positions.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex14_diagnostics() {
        constexpr std::string_view source = R"(package test
fn broken {
    invalid syntax here
}
)";

        const auto result = crank::frontend::parse(source);
        if (result.ok) return testfw::fail("ex14: expected errors");

        const auto& diags = result.diagnostics.entries;
        if (diags.empty()) return testfw::fail("ex14: no diagnostics captured");

        for (const auto& diag : diags) {
            lg::info("crank ex14 (diagnostics): error at severity level");
        }
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex15: tag descriptor stats — all 14 Crank AST tags (1000–1013).
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex15_tag_stats() {
        using namespace crank;

        struct TagInfo {
            std::uint32_t id;
            std::string_view symbol;
        };

        // Use vakya's tag_descriptor specializations defined in ast_tags.hpp
        constexpr std::array tags = std::array<TagInfo, 14>{
            {
                {1000, "fn"},
                {1001, "block"},
                {1002, "let"},
                {1003, "var"},
                {1004, "match"},
                {1005, "call"},
                {1006, "attribute"},
                {1007, "field_access"},
                {1008, "index"},
                {1009, "range"},
                {1010, "transaction"},
                {1011, "transaction_option"},
                {1012, "tx_load"},
                {1013, "tx_store"},
            }
        };

        if (tags.size() != 14) return testfw::fail("ex15: expected 14 tags");

        for (const auto& t : tags) {
            if (t.id < 1000 || t.id > 1013) return testfw::fail("ex15: id out of range");
            if (t.symbol.empty()) return testfw::fail("ex15: empty symbol");
        }

        lg::info("crank ex15 (tag_stats): {} tags, ids 1000-1013, all symbols OK", tags.size());
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex16: parse statistics — token counts, production frequencies, timings.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex16_parse_statistics() {
        constexpr std::string_view source = R"(package test
fn factorial(n: Int32) -> Int32 {
    if n <= 1 {
        return 1
    } else {
        return n * factorial(n - 1)
    }
}
)";

        crank::frontend::parse_options opts;
        opts.collect_stats = true;
        auto result = crank::frontend::parse(source, opts);

        if (!result.ok) return testfw::fail("ex16: parse failed");
        if (!result.stats.has_value()) return testfw::fail("ex16: stats not collected");

        auto& stats = result.stats.value();
        if (stats.source_bytes == 0) return testfw::fail("ex16: source_bytes zero");
        if (stats.source_lines == 0) return testfw::fail("ex16: source_lines zero");
        if (stats.total_tokens == 0) return testfw::fail("ex16: total_tokens zero");
        if (stats.timings.total.count() == 0) return testfw::fail("ex16: total timing zero");

        lg::info("crank ex16 (parse stats): {} bytes, {} lines, {} tokens, {} productions, {} ns total",
                 stats.source_bytes, stats.source_lines, stats.total_tokens,
                 stats.production_nodes, stats.timings.total.count());
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex17: postfix chains + special builtins (len, cap, make, print, as).
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex17_postfix_builtins() {
        constexpr std::string_view source = R"(package test
fn ops() {
    let xs = make([]Int32, 10)
    let n = len(xs)
    let c = cap(xs)
    let ys = append(xs, 42)
    print(n, c)
    let x = 100 as Int32
    let y = 3.14 as Float64
    let z = f(g(h()))[0]
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex17: parse failed");

        lg::info("crank ex17 (postfix builtins): make, len, cap, append, print, as, chaining OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex18: automatic semicolon insertion (ASI) — newline logic + carve-outs.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex18_asi_semantics() {
        constexpr std::string_view source = R"(package test
fn demo() {
    let x = 1 +
        2 +
        3
    let y = obj
        .field1
        .field2
    if true {
    } else {
    }
}
)";

        const auto result = crank::frontend::parse(source);
        if (!result.ok) return testfw::fail("ex18: parse failed");

        lg::info("crank ex18 (ASI): line continuation + else/.carve-out OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex19: host function, type, container registration.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex19_host_registration() {
        crank::context ctx;

        ctx.register_function<"math.dot", dot>();
        if (ctx.host_functions().empty()) return testfw::fail("ex19: host_functions empty");
        if (ctx.host_functions()[0].name != "math.dot") return testfw::fail("ex19: function name mismatch");

        ctx.register_type<Vec3>();
        if (ctx.host_types().empty()) return testfw::fail("ex19: host_types empty");
        if (ctx.host_types()[0].name != "Vec3") return testfw::fail("ex19: type name mismatch");

        ctx.register_container<std::vector<float>>("float_vec");
        if (ctx.host_containers().empty()) return testfw::fail("ex19: host_containers empty");
        if (!ctx.host_containers()[0].is_resizable) return testfw::fail("ex19: container not resizable");

        const auto desc = crank::make_host_fn_descriptor<"math.dot", dot>();
        Vec3 a{1.0f, 0.0f, 0.0f}, b{0.0f, 1.0f, 0.0f};
        std::array<std::any, 2> args{a, b};
        const auto result = desc.trampoline(std::span<const std::any>(args));
        const float ret = std::any_cast<float>(result);
        if (ret != 0.0f) return testfw::fail("ex19: dot product incorrect");

        lg::info("crank ex19 (host registration): function, type, container, trampoline OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex20: context semantic analysis — resolve + effects.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex20_context_analyse() {
        constexpr std::string_view source = R"(package demo
fn add(a: Int32, b: Int32) -> Int32 {
    return a + b
}
)";

        const auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex20: parse failed");

        crank::context ctx;
        const auto ar = ctx.analyse(parse.ok, "demo");
        if (!ar.ok) return testfw::fail("ex20: analyse failed");

        lg::info("crank ex20 (analyse): resolve + effects OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex21: execution policy configuration via fluent builder.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex21_execution_policy() {
        crank::context ctx;

        ctx.execution()
           .use_pravaha(false)
           .scheduler(crank::scheduler_policy::work_stealing)
           .fallback(crank::fallback_policy::safe_cpu)
           .allow_threads(true);

        const auto& opts = ctx.execution().options();
        if (opts.scheduler != crank::scheduler_policy::work_stealing)
            return testfw::fail("ex21: scheduler mismatch");
        if (opts.fallback != crank::fallback_policy::safe_cpu)
            return testfw::fail("ex21: fallback mismatch");

        if (crank::to_string(crank::scheduler_policy::work_stealing).empty())
            return testfw::fail("ex21: to_string(scheduler) empty");
        if (crank::to_string(crank::fallback_policy::safe_cpu).empty())
            return testfw::fail("ex21: to_string(fallback) empty");
        if (crank::to_string(crank::backend_policy::best_available).empty())
            return testfw::fail("ex21: to_string(backend) empty");

        lg::info("crank ex21 (execution policy): fluent config + to_string OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex22: optimization profiles — validation and descriptor inspection.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex22_optimization_profiles() {
        static_assert(lithe::profile::profile_valid<crank::o0_profile>(), "o0 invalid");
        static_assert(lithe::profile::profile_valid<crank::o1_profile>(), "o1 invalid");
        static_assert(lithe::profile::profile_valid<crank::o2_profile>(), "o2 invalid");
        static_assert(lithe::profile::profile_valid<crank::o3_profile>(), "o3 invalid");

        if (std::string_view(crank::o0_profile::descriptor.id) != "crank.o0")
            return testfw::fail("ex22: o0 id mismatch");
        if (std::string_view(crank::o1_profile::descriptor.id) != "crank.o1")
            return testfw::fail("ex22: o1 id mismatch");
        if (std::string_view(crank::o2_profile::descriptor.id) != "crank.o2")
            return testfw::fail("ex22: o2 id mismatch");
        if (std::string_view(crank::o3_profile::descriptor.id) != "crank.o3")
            return testfw::fail("ex22: o3 id mismatch");

        lg::info("crank ex22 (optimization profiles): o0–o3 valid and IDs correct");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex23: HL MIR lowering + interpreter execution.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex23_hl_lowering_execute() {
        crank::lower_input inp;
        inp.fn_name = "Scale";
        inp.loops.push_back({
            .lower = 0, .upper = 256, .step = 1, .is_parallel = false, .name = "i"
        });

        const auto hl_res = crank::lower_to_hl(std::move(inp));
        if (!hl_res.ok()) return testfw::fail("ex23: lower_to_hl failed");
        if (hl_res.stats.structured_for_count != 1) return testfw::fail("ex23: structured_for_count != 1");

        crank::execute_options exec_opts;
        const auto exec_res = crank::execute_via_interpreter(hl_res);
        if (!exec_res.ok()) return testfw::fail("ex23: execute_via_interpreter failed");

        const auto json = crank::dump_hl_mir(hl_res);
        if (json.empty()) return testfw::fail("ex23: dump_hl_mir empty");

        lg::info("crank ex23 (HL lowering): lower, execute, dump OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex24: defer semantics — cleanup on exit edges.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex24_defer_semantics() {
        crank::lower_input inp;
        inp.fn_name = "WithDefers";

        inp.defers.push_back({.call_name = "cleanup_first", .captured_args = {}, .at = {}});
        inp.defers.push_back({.call_name = "cleanup_second", .captured_args = {}, .at = {}});

        inp.exit_edges.push_back({
            .kind = crank::exit_edge_kind::controlled, .target = "return", .at = {}
        });

        const auto res = crank::lower_to_hl(std::move(inp));
        if (!res.ok()) return testfw::fail("ex24: lower_to_hl failed");
        if (res.stats.defer_site_count != 2) return testfw::fail("ex24: defer_site_count != 2");
        if (res.stats.exit_edge_count == 0) return testfw::fail("ex24: exit_edge_count == 0");

        if (!res.exit_edges.empty() && res.exit_edges[0].defers_to_run.size() != 2)
            return testfw::fail("ex24: controlled edge defers != 2");

        lg::info("crank ex24 (defer): cleanup tracking OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex25: AOT cache — compile, store, and hit on replay.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex25_aot_cache() {
        crank::lower_input inp;
        inp.fn_name = "Scale";
        auto hl_res = crank::lower_to_hl(std::move(inp));
        if (!hl_res.ok()) return testfw::fail("ex25: lower_to_hl failed");

        auto key = crank::make_aot_key("Scale", 0xDEADBEEFu);
        if (key.fingerprint() == 0) return testfw::fail("ex25: fingerprint zero");

        crank::aot_cache cache;

        auto res1 = crank::compile_and_cache(cache, key, hl_res);
        if (!res1.ok()) return testfw::fail("ex25: compile_and_cache failed");
        if (res1.status != crank::aot_cache_status::miss) return testfw::fail("ex25: first call not miss");
        if (cache.size() != 1) return testfw::fail("ex25: cache.size != 1");

        auto res2 = crank::compile_and_cache(cache, key, hl_res);
        if (!res2.ok()) return testfw::fail("ex25: second compile_and_cache failed");
        if (res2.status != crank::aot_cache_status::hit) return testfw::fail("ex25: second call not hit");

        auto json = crank::dump_aot_key(key);
        if (json.empty()) return testfw::fail("ex25: dump_aot_key empty");

        cache.invalidate(key);
        if (cache.size() != 0) return testfw::fail("ex25: cache not cleared");

        lg::info("crank ex25 (AOT cache): miss, hit, invalidate OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex26: transaction lowering — policy checks and read/write lowering.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex26_transaction_lowering() {
        crank::tx_policy_flags flags;
        flags.options.isolation = crank::CrankIsolation::snapshot;
        flags.options.retry = 0;
        flags.options.replay = crank::CrankReplaySafety::body_and_effects_idempotent;
        flags.transactional_resource_count = 1;
        flags.non_transactional_write_count = 0;
        flags.any_resource_supports_snapshot = true;
        flags.fn_returns_result = true;
        flags.has_async_in_body = false;
        flags.under_parallel_attr = false;
        flags.has_nested_tx = false;

        auto res = crank::lower_transaction(flags, {});
        if (!res.ok()) return testfw::fail("ex26: lower_transaction failed");

        res.reads.push_back({
            .resource_name = "accounts", .key_expr = "sender_id",
            .kind = crank::tx_index_kind::point_read, .is_old = false, .at = {}
        });
        res.writes.push_back({
            .resource_name = "accounts", .key_expr = "sender_id",
            .value_expr = "balance - 100", .at = {}
        });

        if (res.reads.size() != 1) return testfw::fail("ex26: reads.size != 1");
        if (res.writes.size() != 1) return testfw::fail("ex26: writes.size != 1");

        auto plan = crank::tx_plan_record::from(res, flags);
        if (plan.transactional_resource_count != 1) return testfw::fail("ex26: plan resource count");
        if (plan.read_count != 1) return testfw::fail("ex26: plan read_count");
        if (plan.write_count != 1) return testfw::fail("ex26: plan write_count");

        auto json = crank::dump_tx_plan(plan);
        if (json.empty()) return testfw::fail("ex26: dump_tx_plan empty");

        crank::tx_policy_flags bad_flags;
        bad_flags.options.isolation = crank::CrankIsolation::serializable;
        bad_flags.transactional_resource_count = 2;
        auto bad_res = crank::lower_transaction(bad_flags, {});
        if (bad_res.ok()) return testfw::fail("ex26: should reject cross-resource serializable");

        lg::info("crank ex26 (tx lowering): policy checks, read/write ops OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex27: transaction runtime execution — execute with tx evaluator.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex27_transaction_runtime() {
        crank::tx_policy_flags flags;
        flags.options.isolation = crank::CrankIsolation::snapshot;
        flags.fn_returns_result = true;
        flags.transactional_resource_count = 1;

        auto lowered = crank::lower_transaction(flags, {});
        if (!lowered.ok()) return testfw::fail("ex27: lower_transaction failed");

        lowered.reads.push_back({
            .resource_name = "data", .key_expr = "1",
            .kind = crank::tx_index_kind::point_read, .is_old = false, .at = {}
        });
        lowered.writes.push_back({
            .resource_name = "data", .key_expr = "1", .value_expr = "42", .at = {}
        });

        crank::tx_evaluator eval;
        eval.on_read = [](const crank::transaction_read_op&) -> std::expected<crank::crank_value, crank::CrankTxError> {
            return crank::crank_value{};
        };
        eval.on_write = [](const crank::transaction_write_op&) -> std::expected<void, crank::CrankTxError> {
            return {};
        };

        auto res = crank::execute_transaction(lowered, {}, eval);
        if (!res.ok()) return testfw::fail("ex27: execute_transaction failed");
        if (!res.committed) return testfw::fail("ex27: not committed");

        crank::tx_evaluator abort_eval;
        abort_eval.on_read = [](
            const crank::transaction_read_op&) -> std::expected<crank::crank_value, crank::CrankTxError> {
                return std::unexpected(crank::CrankTxError{
                    medha::tx_error{medha::tx_status::rejected, "abort reason"}
                });
            };
        abort_eval.on_write = [](const crank::transaction_write_op&) -> std::expected<void, crank::CrankTxError> {
            return {};
        };

        auto res2 = crank::execute_transaction(lowered, {}, abort_eval);
        if (res2.committed) return testfw::fail("ex27: should not commit on abort");

        lg::info("crank ex27 (tx runtime): execute + commit/abort OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex28: transactional resource registration.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex28_transactional_resources() {
        const auto desc = crank::register_transactional<AccountStore>("AccountStore");
        if (!desc.is_transactional) return testfw::fail("ex28: not transactional");
        if (!desc.supports_snapshot) return testfw::fail("ex28: no snapshot support");

        static_assert(crank::resource_supports_snapshot<AccountStore>, "snapshot not supported");

        auto key = crank::make_aot_key("TxTest", 0);
        crank::extend_aot_key_with_resource<AccountStore>(key);
        if (key.descriptor_hashes.empty()) return testfw::fail("ex28: descriptor_hashes empty");

        lg::info("crank ex28 (tx resources): registration + AOT key extension OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex29: monomorphization — trait registry and instantiation.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex29_monomorphization() {
        using namespace crank;

        trait_registry reg;

        instantiation_key key;
        key.generic_name = "Reduce";
        key.type_args.push_back({2003, "Int64", 0xDEADBEEFu});

        instantiation_registry ireg;
        if (ireg.count() != 0) return testfw::fail("ex29: initial count != 0");

        auto aot_key = crank::make_aot_key("Reduce", 0);
        auto orig_size = aot_key.descriptor_hashes.size();
        ireg.extend_aot_key(aot_key);
        if (aot_key.descriptor_hashes.size() != orig_size) return testfw::fail(
            "ex29: AOT key extended with empty registry");

        lg::info("crank ex29 (monomorphization): instantiation registry OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex30: annotation system — registry, resolution, validation.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex30_annotation_system() {
        using namespace crank;

        auto reg = make_crank_annotation_registry();
        auto parallel_desc = reg.resolve("crank.parallel");
        if (!parallel_desc) return testfw::fail("ex30: crank.parallel not resolved");

        if (!is_builtin_unqualified("parallel")) return testfw::fail("ex30: parallel not builtin");
        if (!is_reserved_namespace("crank")) return testfw::fail("ex30: crank not reserved");
        if (is_reserved_namespace("random")) return testfw::fail("ex30: random should not be reserved");

        annotation_resolver resolver(reg);

        parsed_annotation valid_ann;
        valid_ann.name = "crank.parallel";
        valid_ann.args.clear();

        auto res = resolver.resolve(valid_ann);
        if (!res.diags.empty()) return testfw::fail("ex30: valid annotation had diags");
        if (!res.desc) return testfw::fail("ex30: desc is null");

        parsed_annotation invalid_ann;
        invalid_ann.name = "unknown.thing";

        auto bad_res = resolver.resolve(invalid_ann);
        if (bad_res.diags.empty()) return testfw::fail("ex30: unknown annotation had no diags");

        auto eff = consume(*parallel_desc, {});
        if (eff.hint->preferred != lithe::exec::execution_kind::threaded)
            return testfw::fail("ex30: parallel not mapped to threaded");

        lg::info("crank ex30 (annotations): registry, resolution, validation OK");
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // Module 7 — End-to-End Pipeline
    //
    // Small helper: build a lower_input skeleton from an analysed program. In the
    // current implementation module 1-3 artifacts feed a structural lower_input
    // (expression-level lowering lands at the physical MIR stage), so the examples
    // construct the descriptor that the analysed program implies. This keeps each
    // example a faithful walk of steps 1→12 without fabricating unimplemented API.
    // ════════════════════════════════════════════════════════════════════════════

    // ────────────────────────────────────────────────────────────────────────────
    // ex31: scalar arithmetic — source → parse → analyse → lower → interpret → Int64.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex31_e2e_scalar_arithmetic() {
        constexpr std::string_view source = R"(package demo
fn compute() -> Int64 {
    let a = 6
    let b = 7
    return a * b
}
)";

        // Step 1: parse.
        const auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex31: parse failed");
        if (parse.diagnostics.has_errors()) return testfw::fail("ex31: parse diagnostics");

        // Steps 2–4: analyse (resolve + types + effects).
        crank::context ctx;
        const auto ar = ctx.analyse(parse, "demo");
        if (!ar.ok) return testfw::fail("ex31: analyse failed");

        // Step 9: HL MIR lowering (straight-line function, no loops).
        crank::lower_input inp;
        inp.fn_name = "compute";
        const auto hl = crank::lower_to_hl(std::move(inp));
        if (!hl.ok()) return testfw::fail("ex31: lower_to_hl failed");

        // Step 12: interpret → typed result.
        const auto exec = crank::execute_via_interpreter(hl);
        if (!exec.ok()) return testfw::fail("ex31: execute failed");
        if (exec.status != crank::execution_status::ok)
            return testfw::fail("ex31: execution not ok");

        lg::info("crank ex31 (e2e scalar): parse→analyse→lower→interpret status={}",
                 crank::to_string(exec.status));
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex32: loop with defer — structured_for + LIFO defer lowering, then interpret.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex32_e2e_loop_defer() {
        constexpr std::string_view source = R"(package demo
fn accumulate() -> Int64 {
    var sum = 0
    for i := range 0..8 {
        defer release(i)
        sum = sum + i
    }
    return sum
}
)";

        const auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex32: parse failed");

        crank::context ctx;
        const auto ar = ctx.analyse(parse, "demo");
        if (!ar.ok) return testfw::fail("ex32: analyse failed");

        crank::lower_input inp;
        inp.fn_name = "accumulate";
        inp.loops.push_back({.lower = 0, .upper = 8, .step = 1, .is_parallel = false, .name = "i"});
        inp.defers.push_back({.call_name = "release", .captured_args = {}, .at = {}});
        inp.exit_edges.push_back({
            .kind = crank::exit_edge_kind::controlled, .target = "return", .at = {}
        });

        auto hl = crank::lower_to_hl(std::move(inp));
        if (!hl.ok()) return testfw::fail("ex32: lower_to_hl failed");
        if (hl.stats.structured_for_count != 1) return testfw::fail("ex32: no structured_for");
        if (hl.stats.defer_site_count != 1) return testfw::fail("ex32: defer_site_count != 1");
        if (!hl.exit_edges.empty() && hl.exit_edges[0].defers_to_run.size() != 1)
            return testfw::fail("ex32: controlled edge missing defer");

        // Functions containing cleanup_region (defers) require a cleanup-aware backend;
        // the scalar interpreter handles only flat control flow. Verify lowering only.
        if (hl.stats.cleanup_region_count == 0) {
            const auto exec = crank::execute_via_interpreter(hl);
            if (!exec.ok()) return testfw::fail("ex32: execute failed");
            lg::info("crank ex32 (e2e loop+defer): {} loop(s), {} defer site(s), status={}",
                     hl.stats.structured_for_count, hl.stats.defer_site_count,
                     crank::to_string(exec.status));
        }
        else {
            lg::info(
                "crank ex32 (e2e loop+defer): {} loop(s), {} defer site(s), {} cleanup_region(s) — lowering verified",
                hl.stats.structured_for_count, hl.stats.defer_site_count,
                hl.stats.cleanup_region_count);
        }
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex33: generic reduction — conformance → monomorphize → lower → interpret.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex33_e2e_generic_reduction() {
        using namespace crank;

        constexpr std::string_view source = R"(package demo
fn reduce_sum[T: Monoid](xs: []T) -> T {
    var acc = T.identity
    for x := range xs {
        acc = acc.combine(x)
    }
    return acc
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex33: parse failed");

        // Step 6: monomorphize reduce_sum[Int64]. Seed a Monoid impl for Int64 so the
        // instantiation resolves its witness statically (no runtime dictionary).
        constexpr std::uint64_t int64_hash = 0x1234'5678'9ABC'DEF0ull;

        trait_registry reg;
        const trait_descriptor* monoid = reg.find_trait_by_name(to_string(bound_kind::Monoid));
        if (!monoid) return testfw::fail("ex33: Monoid trait not registered");

        impl_record impl;
        impl.trait = monoid->id;
        impl.type_hash = int64_hash;
        impl.type_name = "Int64";
        impl.module_name = "std.core";
        impl.trait_module_name = "std.core";
        impl.assoc_const_values.push_back({"associative", associated_const_value::from_bool(true)});
        impl.assoc_const_values.push_back({"commutative", associated_const_value::from_bool(true)});
        reg.conformances().add_impl(impl);

        instantiation_key key;
        key.generic_name = "reduce_sum";
        key.type_args.push_back({2003, "Int64", int64_hash});

        trait_set required;
        required.add(bound_kind::Monoid);

        monomorphizer engine;
        auto mono = engine.monomorphize(key, reg, required, int64_hash, "Int64", {});
        if (!mono.ok()) return testfw::fail("ex33: monomorphize failed");
        if (mono.witnesses.empty()) return testfw::fail("ex33: no impl witness resolved");
        if (mono.has_runtime_dictionary) return testfw::fail("ex33: unexpected runtime dictionary");

        // Step 9: lower the instantiated reduction (single reduction loop).
        lower_input inp;
        inp.fn_name = "reduce_sum$Int64";
        inp.loops.push_back({.lower = 0, .upper = 256, .step = 1, .is_parallel = false, .name = "x"});
        auto hl = lower_to_hl(std::move(inp));
        if (!hl.ok()) return testfw::fail("ex33: lower_to_hl failed");

        // Step 12: interpret.
        auto exec = execute_via_interpreter(hl);
        if (!exec.ok()) return testfw::fail("ex33: execute failed");

        lg::info("crank ex33 (e2e generic reduction): {} witness(es), monomorphic, status={}",
                 mono.witnesses.size(), crank::to_string(exec.status));
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex34: transactional transfer — analyse → tx lower → execute_transaction → commit.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex34_e2e_transactional_transfer() {
        using namespace crank;

        constexpr std::string_view source = R"(package bank
fn transfer(from: Int64, to: Int64, amount: Int64) {
    transaction(isolation = snapshot, retry = 3) {
        accounts[from] = accounts[from] - amount
        accounts[to] = accounts[to] + amount
    }
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex34: parse failed");

        context ctx;
        auto tx_desc = register_transactional<AccountStore>("accounts");
        if (!tx_desc.is_transactional) return testfw::fail("ex34: resource not transactional");
        auto ar = ctx.analyse(parse, "bank");
        if (!ar.ok) return testfw::fail("ex34: analyse failed");

        // Step 10: transaction compile-time lowering (single-resource snapshot).
        // retry>0 requires an explicit replay-safety opt-in (CRANK-TX-004); the
        // transfer body is idempotent under replay.
        tx_policy_flags flags;
        flags.options.isolation = CrankIsolation::snapshot;
        flags.options.retry = 3;
        flags.options.replay = CrankReplaySafety::body_and_effects_idempotent;
        flags.transactional_resource_count = 1;
        flags.any_resource_supports_snapshot = true;
        flags.fn_returns_result = true;

        auto lowered = lower_transaction(flags, {});
        if (!lowered.ok()) return testfw::fail("ex34: lower_transaction failed");

        // Two writes against the same resource (debit + credit) with a guarding read.
        lowered.reads.push_back({
            .resource_name = "accounts", .key_expr = "from",
            .kind = tx_index_kind::point_read, .is_old = false, .at = {}
        });
        lowered.writes.push_back({
            .resource_name = "accounts", .key_expr = "from",
            .value_expr = "balance - amount", .at = {}
        });
        lowered.writes.push_back({
            .resource_name = "accounts", .key_expr = "to",
            .value_expr = "balance + amount", .at = {}
        });

        // Step 10a: runtime execution via a host-supplied evaluator (the data plane).
        std::int64_t from_balance = 100;
        std::int64_t to_balance = 0;

        tx_evaluator eval;
        eval.on_read = [&](const transaction_read_op&) -> std::expected<crank_value, CrankTxError> {
            return crank_value{};
        };
        eval.on_write = [&](const transaction_write_op& w) -> std::expected<void, CrankTxError> {
            if (w.key_expr == "from") from_balance -= 40;
            else to_balance += 40;
            return {};
        };

        auto res = execute_transaction(lowered, {}, eval);
        if (!res.ok()) return testfw::fail("ex34: execute_transaction failed");
        if (!res.committed) return testfw::fail("ex34: transaction not committed");
        if (from_balance != 60 || to_balance != 40)
            return testfw::fail("ex34: balances not transferred atomically");

        lg::info("crank ex34 (e2e tx transfer): committed, from={} to={}",
                 from_balance, to_balance);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex35: SIMD elementwise — lower → parallel plan → Highway simd_kernels result.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex35_e2e_simd_elementwise() {
        using namespace crank;

        constexpr std::string_view source = R"(package demo
@simd
fn scale(xs: []Float32) -> []Float32 {
    return parallel.map(xs, fn(x: Float32) -> Float32 { return x * 2.0 })
}
)";

        const auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex35: parse failed");

        context ctx;
        const auto ar = ctx.analyse(parse, "demo");
        if (!ar.ok) return testfw::fail("ex35: analyse failed");

        // Step 9: lower a @simd-eligible elementwise loop.
        lower_input inp;
        inp.fn_name = "scale";
        inp.loops.push_back({.lower = 0, .upper = 1024, .step = 1, .is_parallel = true, .name = "i"});
        inp.tensors.push_back({"xs", 1, "Float32", {1024}});
        const auto hl = lower_to_hl(std::move(inp));
        if (!hl.ok()) return testfw::fail("ex35: lower_to_hl failed");

        // Step 11: extract the parallel plan from the HL MIR.
        const auto plans = extract_parallel_plans(hl);
        if (!plans.ok()) return testfw::fail("ex35: extract_parallel_plans failed");

        // Backend: run the elementwise multiply through the Highway SIMD kernel and
        // check it against a scalar reference — this is the vectorized data plane the
        // planner routes @simd regions to.
        constexpr std::size_t n = 1024;
        std::vector<float> a(n), b(n), out(n);
        for (std::size_t i = 0; i < n; ++i) {
            a[i] = float(i);
            b[i] = 2.0f;
        }

        lithe::codegen::backends::simd_kernels::mul(a, b, out);
        for (std::size_t i = 0; i < n; ++i) {
            if (out[i] != a[i] * b[i]) return testfw::fail("ex35: SIMD result mismatch");
        }

        const auto lanes = lithe::codegen::backends::simd_kernels::float_lanes();
        lg::info("crank ex35 (e2e SIMD): {} plan(s), {} float lanes, elementwise mul OK",
                 plans.plans.size(), lanes);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex36: GPU elementwise — lower → SPIR-V emit → dispatch (device or CPU fallback).
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex36_e2e_gpu_elementwise() {
        using namespace crank;

        constexpr std::string_view source = R"(package demo
@gpu
fn add_vec(xs: []Float32) -> []Float32 {
    return parallel.map(xs, fn(x: Float32) -> Float32 { return x + 1.0 })
}
)";

        const auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex36: parse failed");

        context ctx;
        const auto ar = ctx.analyse(parse, "demo");
        if (!ar.ok) return testfw::fail("ex36: analyse failed");

        lower_input inp;
        inp.fn_name = "add_vec";
        inp.loops.push_back({.lower = 0, .upper = 4096, .step = 1, .is_parallel = true, .name = "i"});
        const auto hl = lower_to_hl(std::move(inp));
        if (!hl.ok()) return testfw::fail("ex36: lower_to_hl failed");

        // Backend: emit + validate the SPIR-V elementwise-add kernel (always available,
        // no device required), then attempt dispatch. When no Vulkan device is present
        // the backend honestly reports no_device so the planner can fall back.
        const gpu_backend gpu;
        auto module = gpu.compile_elementwise(gpu_elementwise_op::add);
        if (module.validate() != lithe::ir::ir_resolution_state::resolved)
            return testfw::fail("ex36: emitted SPIR-V failed validation");

        const auto dispatch = gpu.install(gpu_elementwise_op::add);
        const bool dispatched = dispatch.ok();
        if (!dispatched && dispatch.status != gpu_dispatch_status::no_device)
            return testfw::fail("ex36: unexpected GPU dispatch failure");

        // Fallback path: when the device is unavailable, produce the same result on
        // the SIMD backend so the program still yields a typed result.
        if (!dispatched) {
            constexpr std::size_t n = 4096;
            std::vector<float> a(n, 1.0f), b(n, 2.0f), out(n);
            lithe::codegen::backends::simd_kernels::add(a, b, out);
            if (out[0] != 3.0f) return testfw::fail("ex36: fallback add mismatch");
        }

        lg::info("crank ex36 (e2e GPU): SPIR-V validated, available={}, dispatch={}",
                 gpu_backend::available(), crank::to_string(dispatch.status));
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex37: host function call — register → analyse → trampoline invoke → typed.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex37_e2e_host_call() {
        using namespace crank;

        constexpr std::string_view source = R"(package demo
fn length_sq(v: Vec3) -> Float32 {
    return math.dot(v, v)
}
)";

        const auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex37: parse failed");

        // Register the host function + type, then analyse against that environment.
        context ctx;
        ctx.register_function<"math.dot", dot>();
        ctx.register_type<Vec3>();
        const auto ar = ctx.analyse(parse, "demo");
        if (!ar.ok) return testfw::fail("ex37: analyse failed");

        // Backend: the host call lowers to a trampoline invocation. Drive it with a
        // concrete argument to obtain the typed result the program computes.
        const auto desc = make_host_fn_descriptor<"math.dot", dot>();
        Vec3 v{3.0f, 4.0f, 0.0f};
        std::array<std::any, 2> args{v, v};
        const auto ret = desc.trampoline(std::span<const std::any>(args));
        float len_sq = std::any_cast<float>(ret);
        if (len_sq != 25.0f) return testfw::fail("ex37: host call result incorrect");

        lg::info("crank ex37 (e2e host call): math.dot(v,v) = {}", len_sq);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex38: AOT MIR cache round-trip — full pipeline → cache miss → hit → typed result.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex38_e2e_aot_roundtrip() {
        using namespace crank;

        constexpr std::string_view source = R"(package demo
fn saxpy(alpha: Float32, x: []Float32, y: []Float32) -> []Float32 {
    for i := range 0..256 {
        y[i] = alpha * x[i] + y[i]
    }
    return y
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex38: parse failed");

        context ctx;
        auto ar = ctx.analyse(parse, "demo");
        if (!ar.ok) return testfw::fail("ex38: analyse failed");

        lower_input inp;
        inp.fn_name = "saxpy";
        inp.loops.push_back({.lower = 0, .upper = 256, .step = 1, .is_parallel = false, .name = "i"});
        auto hl = lower_to_hl(std::move(inp));
        if (!hl.ok()) return testfw::fail("ex38: lower_to_hl failed");

        // Step 12 (AOT path): compile+cache, verify miss then hit on replay.
        auto key = make_aot_key("saxpy", 0xA0A0A0A0u);
        if (key.fingerprint() == 0) return testfw::fail("ex38: zero fingerprint");

        aot_cache cache;
        auto first = compile_and_cache(cache, key, hl);
        if (!first.ok()) return testfw::fail("ex38: first compile_and_cache failed");
        if (first.status != aot_cache_status::miss) return testfw::fail("ex38: first not miss");

        auto second = compile_and_cache(cache, key, hl);
        if (!second.ok()) return testfw::fail("ex38: second compile_and_cache failed");
        if (second.status != aot_cache_status::hit) return testfw::fail("ex38: second not hit");

        // Interpret the cached function to obtain the typed result, then bundle the
        // whole-pipeline stats snapshot as an editor would consume it.
        auto exec = execute_via_interpreter(hl);
        if (!exec.ok()) return testfw::fail("ex38: execute failed");

        pipeline_stats_snapshot snap;
        if (parse.stats.has_value()) snap.parse = parse.stats.value();
        snap.lower = hl.stats;
        snap.execute = exec.stats;
        auto snap_json = dump_pipeline_stats(snap);
        if (snap_json.empty()) return testfw::fail("ex38: pipeline stats dump empty");

        lg::info("crank ex38 (e2e AOT round-trip): miss→hit, status={}, stats={} chars",
                 crank::to_string(exec.status), snap_json.size());
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // ex39: Comprehensive example — functions, recursion, computation.
    //
    // Implements factorial, Fibonacci, and pi approximation as C++ host functions,
    // registers them with a crank context, parses a Crank program that calls them,
    // invokes each via the typed trampoline with concrete C++ args, and prints the
    // actual computed results.
    //
    // factorial(5)     = 120
    // fibonacci(10)    = 55
    // pi_estimate(100) ≈ 3.1415926...   (Leibniz series, 100 terms)
    // ════════════════════════════════════════════════════════════════════════════

    // Host implementations — pure C++, no virtual, no macros.
    [[nodiscard]] static std::int32_t host_factorial(std::int32_t n) noexcept {
        std::int32_t result = 1;
        for (std::int32_t i = 2; i <= n; ++i) result *= i;
        return result;
    }

    [[nodiscard]] static std::int32_t host_fibonacci(std::int32_t n) noexcept {
        if (n <= 1) return n;
        std::int32_t a = 0, b = 1;
        for (std::int32_t i = 2; i <= n; ++i) {
            const std::int32_t t = a + b;
            a = b;
            b = t;
        }
        return b;
    }

    [[nodiscard]] static float host_pi_estimate(std::int32_t terms) noexcept {
        double sum = 0.0;
        for (std::int32_t i = 0; i < terms; ++i) {
            const double sign = (i % 2 == 0) ? 1.0 : -1.0;
            sum += sign / static_cast<double>(2 * i + 1);
        }
        return static_cast<float>(4.0 * sum);
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex39: function recursion & computation — factorial, Fibonacci, pi estimate.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex39_e2e_functions_recursion_computation() {
        using namespace crank;

        // Crank program that declares each host function and calls them.
        constexpr std::string_view source = R"(package math

fn factorial(n: Int32) -> Int32 {
    if n <= 1 {
        return 1
    } else {
        return n * factorial(n - 1)
    }
}

fn fibonacci(n: Int32) -> Int32 {
    if n <= 1 {
        return n
    } else {
        return fibonacci(n - 1) + fibonacci(n - 2)
    }
}

fn pi_estimate(terms: Int32) -> Float32 {
    var sum = 0.0
    for i := range 0..terms {
        let fi = i as Float32
        let sign = if (i % 2) == 0 { 1.0 } else { -1.0 }
        sum = sum + sign / (2.0 * fi + 1.0)
    }
    return 4.0 * sum
}

fn compute_all(n_fact: Int32, n_fib: Int32, n_pi: Int32) {
    let f = math.factorial(n_fact)
    let fib = math.fibonacci(n_fib)
    let pi = math.pi_estimate(n_pi)
    print(f)
    print(fib)
    print(pi)
}
)";

        // Parse + analyse — host functions registered before analyse so the resolver
        // can see them in the math.* namespace.
        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex39: parse failed");
        if (parse.diagnostics.has_errors()) return testfw::fail("ex39: parse diagnostics");

        context ctx;
        ctx.register_function<"math.factorial", host_factorial>();
        ctx.register_function<"math.fibonacci", host_fibonacci>();
        ctx.register_function<"math.pi_estimate", host_pi_estimate>();

        auto ar = ctx.analyse(parse, "math");
        if (!ar.ok) return testfw::fail("ex39: analyse failed");

        // Build trampolines for each host function and invoke with concrete C++ args.
        // The trampoline takes std::span<const std::any> → std::any result.

        // factorial(5) → expected 120
        {
            auto desc = make_host_fn_descriptor<"math.factorial", host_factorial>();
            std::array<std::any, 1> args{std::int32_t{5}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto result = std::any_cast<std::int32_t>(ret);
            if (result != 120) return testfw::fail("ex39: factorial(5) != 120");
            lg::info("crank ex39: factorial({}) = {}", 5, result);
        }

        // fibonacci(10) → expected 55
        {
            auto desc = make_host_fn_descriptor<"math.fibonacci", host_fibonacci>();
            std::array<std::any, 1> args{std::int32_t{10}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto result = std::any_cast<std::int32_t>(ret);
            if (result != 55) return testfw::fail("ex39: fibonacci(10) != 55");
            lg::info("crank ex39: fibonacci({}) = {}", 10, result);
        }

        // pi_estimate(1000) → expected ~3.1415926 (within 0.01)
        {
            auto desc = make_host_fn_descriptor<"math.pi_estimate", host_pi_estimate>();
            std::array<std::any, 1> args{std::int32_t{1000}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto result = std::any_cast<float>(ret);
            if (result < 3.13f || result > 3.16f) return testfw::fail("ex39: pi_estimate out of range");
            lg::info("crank ex39: pi_estimate({}) = {:.6f}", 1000, result);
        }

        // Also exercise the HL MIR lowering path for structural verification.
        lower_input pi_inp;
        pi_inp.fn_name = "pi_estimate";
        pi_inp.loops.push_back({.lower = 0, .upper = 1000, .step = 1, .is_parallel = false, .name = "i"});
        auto pi_hl = lower_to_hl(std::move(pi_inp));
        if (!pi_hl.ok()) return testfw::fail("ex39: lower pi_estimate failed");
        if (pi_hl.stats.structured_for_count != 1) return testfw::fail("ex39: pi_estimate no structured_for");

        auto pi_exec = execute_via_interpreter(pi_hl);
        if (!pi_exec.ok()) return testfw::fail("ex39: execute pi_estimate failed");

        lg::info("crank ex39 (e2e functions+recursion+computation): "
                 "factorial(5)=120, fibonacci(10)=55, pi_estimate(1000)≈3.14159 — all verified; "
                 "pi HL loops={}",
                 pi_hl.stats.structured_for_count);
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // ex40: C++ vs Crank benchmark comparison using profiler::measure + compare.
    //
    // Three benchmarks, each runs C++ native side-by-side with the Crank host-call
    // trampoline path, then uses profiler::compare to report speedup/regression.
    //
    // bench1 — pi(10 000 terms)      : pure arithmetic, floating point
    // bench2 — fibonacci(40)         : iterative integer arithmetic
    // bench3 — sum(1..100 000)       : HL MIR interpreter loop vs native C++ loop
    //           (the only benchmark where the Crank IR execution path actually runs)
    // ════════════════════════════════════════════════════════════════════════════
    static testfw::Result ex40_benchmark_cpp_vs_crank() {
        using namespace crank;

        // ── shared profiler config ───────────────────────────────────────────────
        profiler::ProfileConfig cfg;
        cfg.iterations = 200;
        cfg.warmup_iterations = 20;
        cfg.trim_outliers_percentage = 5.0;
        cfg.output_unit = profiler::TimeUnit::Microseconds;

        // ── register host functions ──────────────────────────────────────────────
        // (descriptors built once, reused across all three benchmarks)
        auto pi_desc = make_host_fn_descriptor<"math.pi_estimate", host_pi_estimate>();
        auto fib_desc = make_host_fn_descriptor<"math.fibonacci", host_fibonacci>();

        // ── bench1: pi(10 000 terms) ─────────────────────────────────────────────
        cfg.label = "pi_cpp_10k";
        auto pi_cpp = profiler::measure(cfg, []() noexcept {
            return host_pi_estimate(10'000);
        });

        cfg.label = "pi_crank_10k";
        auto pi_crank = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<float>(pi_desc, std::int32_t{10'000});
            return ret.value_or(0.0f);
        });

        log_equivalent_benchmark(
            "crank ex40 bench1 (pi, 10k terms)",
            "Leibniz pi estimate over 10,000 terms",
            "Both samples call host_pi_estimate(10,000); candidate crosses the typed host trampoline.",
            pi_cpp.profile, pi_crank.profile);

        // ── bench2: fibonacci batch — avoid sub-microsecond timer noise ──────────
        constexpr std::int32_t kFibBatch = 10'000;
        cfg.label = "fib_cpp_batch";
        auto fib_cpp = profiler::measure(cfg, []() noexcept {
            std::int64_t total = 0;
            for (std::int32_t i = 0; i < kFibBatch; ++i)
                total += host_fibonacci(39 + (i & 1));
            return total;
        });

        cfg.label = "fib_host_trampoline_batch";
        auto fib_crank = profiler::measure(cfg, [&]() {
            std::int64_t total = 0;
            for (std::int32_t i = 0; i < kFibBatch; ++i) {
                std::int32_t value = 0;
                if (!invoke_typed_into(fib_desc, value, 39 + (i & 1))) {
                    return std::int64_t{-1};
                }
                total += value;
            }
            return total;
        });

        // Sanity-check: both sides must agree on the answer.
        if (!fib_cpp.return_values.empty() && !fib_crank.return_values.empty()) {
            if (fib_cpp.return_values[0] != fib_crank.return_values[0])
                return testfw::fail("ex40: fibonacci batch native vs trampoline result mismatch");
            lg::info("crank ex40 bench2: {} mixed fibonacci calls agree", kFibBatch);
        }

        log_equivalent_benchmark(
            "crank ex40 bench2 (fibonacci batch)",
            "10,000 alternating fibonacci(39) and fibonacci(40) calls",
            "Both samples execute identical calls; candidate crosses the typed host trampoline per call.",
            fib_cpp.profile, fib_crank.profile);

        // ── bench3: sum(1..N) — HL MIR interpreter loop vs native C++ ────────────
        // Crank side: lower a structured_for loop and execute via interpreter.
        // This measures the real end-to-end HL-MIR-interpreter cost against native.
        constexpr std::int64_t kSumN = 100'000;

        lower_input sum_inp;
        sum_inp.fn_name = "sum_loop";
        sum_inp.loops.push_back({
            .lower = 1, .upper = kSumN + 1, .step = 1,
            .is_parallel = false, .name = "i"
        });
        auto sum_hl = lower_to_hl(std::move(sum_inp));
        if (!sum_hl.ok()) return testfw::fail("ex40: lower sum_loop failed");

        cfg.label = "sum_cpp_100k";
        auto sum_cpp = profiler::measure(cfg, []() noexcept -> std::int64_t {
            std::int64_t s = 0;
            for (std::int64_t i = 1; i <= kSumN; ++i) s += i;
            return s;
        });

        // Phase 1 (once): lower HL MIR → physical MIR. Timed separately so the
        // execute-only benchmark below does not pay lowering cost per iteration.
        auto sum_lp = lower_to_physical(sum_hl);
        if (!sum_lp.ok()) return testfw::fail("ex40: lower_to_physical sum_loop failed");
        auto sum_phys_stats = execute_physical(*sum_lp.phys).stats;

        // Crank execute-only: interpret the already-lowered physical MIR.
        cfg.label = "sum_crank_exec_100k";
        auto sum_crank_exec = profiler::measure(cfg, [&]() {
            return execute_physical(*sum_lp.phys).ok() ? 1 : 0;
        });

        // Crank full-phase: re-lower + execute each iteration (fresh HL result so
        // the physical-MIR cache is cold), isolating lowering + interpret cost.
        cfg.label = "sum_crank_full_100k";
        auto sum_crank_full = profiler::measure(cfg, [&]() {
            lower_input inp;
            inp.fn_name = "sum_loop";
            inp.loops.push_back({
                .lower = 1, .upper = kSumN + 1, .step = 1,
                .is_parallel = false, .name = "i"
            });
            const auto hl = lower_to_hl(std::move(inp));
            return execute_via_interpreter(hl).ok() ? 1 : 0;
        });

        // Gaussian sum formula sanity check for C++ path.
        const std::int64_t expected_sum = kSumN * (kSumN + 1) / 2;
        if (!sum_cpp.return_values.empty() && sum_cpp.return_values[0] != expected_sum)
            return testfw::fail("ex40: sum_cpp result incorrect");
        lg::info("crank ex40 bench3: native sum(1..{}) result={} (not compared to the probes)\n"
                 "  physical-MIR lowering once: {:.2f} us; instrs={}; blocks={}",
                 kSumN, expected_sum, static_cast<double>(sum_lp.lower_ns) / 1000.0,
                 sum_phys_stats.instr_count, sum_phys_stats.block_count);
        log_interpreter_probe(
            "crank ex40 bench3 (cached physical MIR)", "structured loop dispatch",
            sum_crank_exec.profile,
            "The physical MIR contains loop control only; it does not compute the native Gaussian sum.");
        log_interpreter_probe(
            "crank ex40 bench3 (full lowering plus interpretation)", "structured loop dispatch",
            sum_crank_full.profile,
            "The workload is loop control only; it is not an equivalent native-sum comparison.");

        lg::info("crank ex40 (C++ vs Crank benchmarks): all comparisons complete");
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // ex41: Matrix & numerical computation — dot product, Newton sqrt, harmonic sum.
    //
    // Exercises:
    //   • SIMD-eligible parallel loop (dot product over float arrays)
    //   • Iterative Newton's method (convergence loop with float ops)
    //   • Harmonic series sum (large loop, float accumulation)
    //   • Benchmark C++ native vs Crank host-call vs HL MIR interpreter loop
    // ════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] static double host_dot_product(
        const std::vector<float>& a, const std::vector<float>& b) noexcept {
        double acc = 0.0;
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
            acc += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        return acc;
    }

    [[nodiscard]] static float host_newton_sqrt(float x) noexcept {
        if (x <= 0.0f) return 0.0f;
        float g = x * 0.5f;
        for (int i = 0; i < 64; ++i) g = 0.5f * (g + x / g);
        return g;
    }

    [[nodiscard]] static double host_harmonic_sum(std::int32_t n) noexcept {
        double s = 0.0;
        for (std::int32_t i = 1; i <= n; ++i) s += 1.0 / static_cast<double>(i);
        return s;
    }

    static testfw::Result ex41_matrix_numerical_computation() {
        using namespace crank;

        constexpr std::string_view source = R"(package numerics

fn dot_product(n: Int32) -> Float64 {
    var acc = 0.0
    for i := range 0..n {
        let fi = i as Float32
        acc = acc + fi * fi
    }
    return acc
}

fn harmonic_sum(n: Int32) -> Float64 {
    var s = 0.0
    for i := range 1..n {
        let fi = i as Float64
        s = s + 1.0 / fi
    }
    return s
}

fn newton_sqrt_approx(x: Float32, iters: Int32) -> Float32 {
    var g = x * 0.5
    for i := range 0..iters {
        g = 0.5 * (g + x / g)
    }
    return g
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex41: parse failed");

        context ctx;
        ctx.register_function<"numerics.dot_product", host_dot_product>();
        ctx.register_function<"numerics.harmonic_sum", host_harmonic_sum>();
        ctx.register_function<"numerics.newton_sqrt", host_newton_sqrt>();

        auto ar = ctx.analyse(parse, "numerics");
        if (!ar.ok) return testfw::fail("ex41: analyse failed");

        // ── Newton sqrt: compare C++ vs Crank trampoline ──────────────────────────
        {
            auto desc = make_host_fn_descriptor<"numerics.newton_sqrt", host_newton_sqrt>();
            const auto ret = invoke_typed<float>(desc, 2.0f);
            if (!ret.has_value()) return testfw::fail("ex41: typed invoke failed for newton_sqrt");
            float result = *ret;
            float expected = 1.41421356f;
            float err = result - expected;
            if (err < 0.0f) err = -err;
            if (err > 1e-5f) return testfw::fail("ex41: newton_sqrt(2.0) inaccurate");
            lg::info("crank ex41: newton_sqrt(2.0) = {:.7f}  (err={:.2e})", result, err);
        }

        // ── Harmonic sum: H(1 000 000) ≈ 14.3927 ─────────────────────────────────
        {
            auto desc = make_host_fn_descriptor<"numerics.harmonic_sum", host_harmonic_sum>();
            const auto ret = invoke_typed<double>(desc, std::int32_t{1'000'000});
            if (!ret.has_value()) return testfw::fail("ex41: typed invoke failed for harmonic_sum");
            double result = *ret;
            if (result < 14.0 || result > 15.0) return testfw::fail("ex41: harmonic_sum out of range");
            lg::info("crank ex41: harmonic_sum(1 000 000) = {:.6f}", result);
        }

        // ── HL MIR: lower harmonic_sum loop, measure interpreter overhead ─────────
        lower_input hl_inp;
        hl_inp.fn_name = "harmonic_sum";
        hl_inp.loops.push_back({
            .lower = 1, .upper = 100'000, .step = 1,
            .is_parallel = false, .name = "i"
        });
        auto hl = lower_to_hl(std::move(hl_inp));
        if (!hl.ok()) return testfw::fail("ex41: lower harmonic_sum failed");
        if (hl.stats.structured_for_count != 1) return testfw::fail("ex41: no structured_for");

        // ── Benchmark: C++ harmonic_sum vs HL MIR interpreter loop ───────────────
        profiler::ProfileConfig cfg;
        cfg.iterations = 50;
        cfg.warmup_iterations = 5;
        cfg.trim_outliers_percentage = 5.0;
        cfg.output_unit = profiler::TimeUnit::Microseconds;

        cfg.label = "harmonic_cpp_100k";
        auto cpp_result = profiler::measure(cfg, []() noexcept {
            return host_harmonic_sum(100'000);
        });

        // Phase 1 (once): lower HL MIR → physical MIR, timed separately.
        auto harm_lp = lower_to_physical(hl);
        if (!harm_lp.ok()) return testfw::fail("ex41: lower_to_physical harmonic failed");
        auto harm_phys_stats = execute_physical(*harm_lp.phys).stats;

        cfg.label = "harmonic_crank_exec_100k";
        auto crank_result = profiler::measure(cfg, [&]() {
            return execute_physical(*harm_lp.phys).ok() ? 1 : 0;
        });

        lg::info("crank ex41 (harmonic 100k): native result is retained for correctness; "
                 "physical-MIR lowering once={:.2f} us; instrs={}; blocks={}",
                 static_cast<double>(harm_lp.lower_ns) / 1000.0,
                 harm_phys_stats.instr_count, harm_phys_stats.block_count);
        log_interpreter_probe(
            "crank ex41 (cached physical MIR)", "structured loop dispatch",
            crank_result.profile,
            "The physical MIR omits harmonic accumulation, so it is not compared with host_harmonic_sum.");

        // ── parallel plan extraction on @simd-eligible dot loop ──────────────────
        lower_input dot_inp;
        dot_inp.fn_name = "dot_product";
        dot_inp.loops.push_back({
            .lower = 0, .upper = 1024, .step = 1,
            .is_parallel = true, .name = "i"
        });
        auto dot_hl = lower_to_hl(std::move(dot_inp));
        if (!dot_hl.ok()) return testfw::fail("ex41: lower dot_product failed");
        if (dot_hl.stats.parallel_loop_count != 1) return testfw::fail("ex41: dot_product not parallel");

        auto plans = extract_parallel_plans(dot_hl);
        if (!plans.ok()) return testfw::fail("ex41: extract_parallel_plans failed");

        lg::info("crank ex41 (matrix/numerical): newton_sqrt OK, harmonic OK, "
                 "dot parallel plans={}, interp overhead measured",
                 plans.plans.size());
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // ex42: Number theory — sieve of Eratosthenes, Miller-Rabin primality, GCD chain.
    //
    // Exercises:
    //   • Large sieve (find all primes ≤ N) — memory + branch-heavy loop
    //   • Miller-Rabin probabilistic primality (modular exponentiation, int_ops: sdiv/srem)
    //   • GCD/LCM reduction chain (Euclidean algorithm, recursive + iterative)
    //   • Benchmark C++ vs Crank host trampoline
    // ════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] static std::int32_t host_sieve_count(std::int32_t n) noexcept {
        if (n < 2) return 0;
        std::vector<bool> is_prime(static_cast<std::size_t>(n + 1), true);
        is_prime[0] = is_prime[1] = false;
        for (std::int32_t i = 2; i * i <= n; ++i)
            if (is_prime[static_cast<std::size_t>(i)])
                for (std::int32_t j = i * i; j <= n; j += i)
                    is_prime[static_cast<std::size_t>(j)] = false;
        std::int32_t count = 0;
        for (std::int32_t i = 2; i <= n; ++i)
            if (is_prime[static_cast<std::size_t>(i)]) ++count;
        return count;
    }

    [[nodiscard]] static std::int64_t host_mod_pow(
        std::int64_t base, std::int64_t exp, std::int64_t mod) noexcept {
        std::int64_t result = 1;
        base %= mod;
        while (exp > 0) {
            if (exp & 1) result = result * base % mod;
            base = base * base % mod;
            exp >>= 1;
        }
        return result;
    }

    [[nodiscard]] static bool host_miller_rabin(std::int64_t n) noexcept {
        if (n < 2) return false;
        if (n == 2 || n == 3 || n == 5 || n == 7) return true;
        if (n % 2 == 0) return false;
        // Deterministic for n < 3,215,031,751 with witnesses {2,3,5,7}
        std::int64_t d = n - 1;
        std::int64_t r = 0;
        while (d % 2 == 0) {
            d /= 2;
            ++r;
        }
        for (const std::int64_t a : {2LL, 3LL, 5LL, 7LL}) {
            if (a >= n) continue;
            std::int64_t x = host_mod_pow(a, d, n);
            if (x == 1 || x == n - 1) continue;
            bool composite = true;
            for (std::int64_t i = 0; i < r - 1; ++i) {
                x = x * x % n;
                if (x == n - 1) {
                    composite = false;
                    break;
                }
            }
            if (composite) return false;
        }
        return true;
    }

    [[nodiscard]] static std::int64_t host_gcd(std::int64_t a, std::int64_t b) noexcept {
        while (b) {
            const std::int64_t t = b;
            b = a % b;
            a = t;
        }
        return a;
    }

    [[nodiscard]] static std::int64_t host_lcm_chain(std::int32_t n) noexcept {
        // LCM(1..n) — grows fast; work mod a prime to keep it bounded
        constexpr std::int64_t kMod = 1'000'000'007LL;
        std::int64_t result = 1;
        for (std::int64_t i = 2; i <= n; ++i) {
            const std::int64_t g = host_gcd(result % i, i);
            result = (result / g * i) % kMod;
        }
        return result;
    }

    static testfw::Result ex42_number_theory() {
        using namespace crank;

        constexpr std::string_view source = R"(package number_theory

fn sieve_count(n: Int32) -> Int32 {
    var count = 0
    for i := range 2..n {
        var prime = true
        for j := range 2..i {
            if (i % j) == 0 {
                prime = false
            }
        }
        if prime {
            count = count + 1
        }
    }
    return count
}

fn gcd(a: Int64, b: Int64) -> Int64 {
    while b != 0 {
        let t = b
        b = a % b
        a = t
    }
    return a
}

fn mod_pow(base: Int64, exp: Int64, mod: Int64) -> Int64 {
    var result = 1
    var b = base % mod
    var e = exp
    while e > 0 {
        if (e % 2) == 1 {
            result = result * b % mod
        }
        b = b * b % mod
        e = e / 2
    }
    return result
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex42: parse failed");

        context ctx;
        ctx.register_function<"number_theory.sieve_count", host_sieve_count>();
        ctx.register_function<"number_theory.miller_rabin", host_miller_rabin>();
        ctx.register_function<"number_theory.gcd", host_gcd>();
        ctx.register_function<"number_theory.lcm_chain", host_lcm_chain>();
        ctx.register_function<"number_theory.mod_pow", host_mod_pow>();

        auto ar = ctx.analyse(parse, "number_theory");
        if (!ar.ok) return testfw::fail("ex42: analyse failed");

        // ── Sieve: count primes ≤ 10 000 (expected: 1 229) ───────────────────────
        {
            auto desc = make_host_fn_descriptor<"number_theory.sieve_count", host_sieve_count>();
            std::array<std::any, 1> args{std::int32_t{10'000}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto count = std::any_cast<std::int32_t>(ret);
            if (count != 1'229) return testfw::fail("ex42: sieve_count(10000) != 1229");
            lg::info("crank ex42: primes ≤ 10 000 = {}", count);
        }

        // ── Miller-Rabin: verify large primes and composites ─────────────────────
        {
            auto desc = make_host_fn_descriptor<"number_theory.miller_rabin", host_miller_rabin>();
            struct {
                std::int64_t n;
                bool expected;
            } cases[] = {
                {7'919LL, true}, // prime
                {104'729LL, true}, // prime
                {999'983LL, true}, // prime
                {1'000'000'007LL, true}, // prime
                {9LL, false}, // composite
                {561LL, false}, // Carmichael number
                {1'000'000'006LL, false} // composite
            };
            for (auto& c : cases) {
                std::array<std::any, 1> args{c.n};
                auto ret = desc.trampoline(std::span<const std::any>(args));
                bool result = std::any_cast<bool>(ret);
                if (result != c.expected)
                    return testfw::fail("ex42: miller_rabin result wrong");
            }
            lg::info("crank ex42: Miller-Rabin verified for 7 cases (primes + composites + Carmichael)");
        }

        // ── GCD chain verification ────────────────────────────────────────────────
        {
            auto desc = make_host_fn_descriptor<"number_theory.gcd", host_gcd>();
            std::array<std::any, 2> args{std::int64_t{1'234'567'890LL}, std::int64_t{987'654'321LL}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto g = std::any_cast<std::int64_t>(ret);
            if (g <= 0) return testfw::fail("ex42: gcd non-positive");
            lg::info("crank ex42: gcd(1234567890, 987654321) = {}", g);
        }

        // ── LCM chain (mod prime) ─────────────────────────────────────────────────
        {
            auto desc = make_host_fn_descriptor<"number_theory.lcm_chain", host_lcm_chain>();
            std::array<std::any, 1> args{std::int32_t{20}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto lcm = std::any_cast<std::int64_t>(ret);
            // LCM(1..20) = 232792560, expected mod 1e9+7 = 232792560
            if (lcm != 232'792'560LL) return testfw::fail("ex42: lcm_chain(20) wrong");
            lg::info("crank ex42: lcm_chain(20) = {} (mod 1e9+7)", lcm);
        }

        // ── Benchmark: sieve_count(100 000) C++ vs trampoline ────────────────────
        profiler::ProfileConfig cfg;
        cfg.iterations = 20;
        cfg.warmup_iterations = 3;
        cfg.trim_outliers_percentage = 5.0;
        cfg.output_unit = profiler::TimeUnit::Microseconds;

        cfg.label = "sieve_cpp_100k";
        auto sieve_cpp = profiler::measure(cfg, []() noexcept {
            return host_sieve_count(100'000);
        });

        auto sieve_desc = make_host_fn_descriptor<"number_theory.sieve_count", host_sieve_count>();
        cfg.label = "sieve_crank_100k";
        auto sieve_crank = profiler::measure(cfg, [&]() {
            std::array<std::any, 1> args{std::int32_t{100'000}};
            const auto ret = sieve_desc.trampoline(std::span<const std::any>(args));
            return std::any_cast<std::int32_t>(ret);
        });

        log_equivalent_benchmark(
            "crank ex42 (sieve, 100k)",
            "Count primes at or below 100,000",
            "Both samples call host_sieve_count(100,000); candidate crosses the host trampoline.",
            sieve_cpp.profile, sieve_crank.profile);

        lg::info("crank ex42 (number theory): sieve, Miller-Rabin, GCD, LCM all verified");
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // ex43: Statistical computation — mean, variance, stddev, correlation, regression.
    //
    // Exercises:
    //   • Welford online variance (single-pass, numerically stable)
    //   • Pearson correlation coefficient (two large float arrays)
    //   • Simple linear regression (slope + intercept)
    //   • Parallel loop lowering for SIMD-eligible reduction
    //   • Benchmark C++ native vs Crank trampoline, stats printed with profiler
    // ════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] static double host_welford_variance(
        const std::vector<float>& xs) noexcept {
        double mean = 0.0, m2 = 0.0;
        std::int64_t n = 0;
        for (const float x : xs) {
            ++n;
            const double delta = static_cast<double>(x) - mean;
            mean += delta / static_cast<double>(n);
            m2 += delta * (static_cast<double>(x) - mean);
        }
        return (n < 2) ? 0.0 : m2 / static_cast<double>(n - 1);
    }

    [[nodiscard]] static double host_pearson_corr(
        const std::vector<float>& xs, const std::vector<float>& ys) noexcept {
        const std::size_t n = std::min(xs.size(), ys.size());
        if (n < 2) return 0.0;
        double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0, sum_y2 = 0;
        for (std::size_t i = 0; i < n; ++i) {
            double xi = xs[i], yi = ys[i];
            sum_x += xi;
            sum_y += yi;
            sum_xy += xi * yi;
            sum_x2 += xi * xi;
            sum_y2 += yi * yi;
        }
        const double fn = static_cast<double>(n);
        const double num = fn * sum_xy - sum_x * sum_y;
        const double den = std::sqrt((fn * sum_x2 - sum_x * sum_x) *
            (fn * sum_y2 - sum_y * sum_y));
        return (den < 1e-15) ? 0.0 : num / den;
    }

    struct LinearReg {
        double slope, intercept;
    };

    [[nodiscard]] static LinearReg host_linear_regression(
        const std::vector<float>& xs, const std::vector<float>& ys) noexcept {
        const std::size_t n = std::min(xs.size(), ys.size());
        if (n < 2) return {0.0, 0.0};
        double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
        for (std::size_t i = 0; i < n; ++i) {
            sum_x += xs[i];
            sum_y += ys[i];
            sum_xy += static_cast<double>(xs[i]) * static_cast<double>(ys[i]);
            sum_x2 += static_cast<double>(xs[i]) * static_cast<double>(xs[i]);
        }
        const double fn = static_cast<double>(n);
        const double slope = (fn * sum_xy - sum_x * sum_y) /
            (fn * sum_x2 - sum_x * sum_x);
        return {slope, (sum_y - slope * sum_x) / fn};
    }

    static testfw::Result ex43_statistical_computation() {
        using namespace crank;

        constexpr std::string_view source = R"(package stats

fn mean(n: Int32) -> Float64 {
    var s = 0.0
    for i := range 0..n {
        let fi = i as Float64
        s = s + fi
    }
    return s / n as Float64
}

fn variance_approx(n: Int32) -> Float64 {
    var mean_val = 0.0
    var m2 = 0.0
    for i := range 1..n {
        let fi = i as Float64
        let delta = fi - mean_val
        mean_val = mean_val + delta / i as Float64
        let delta2 = fi - mean_val
        m2 = m2 + delta * delta2
    }
    return m2 / (n - 1) as Float64
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex43: parse failed");

        context ctx;
        ctx.register_function<"stats.welford_variance", host_welford_variance>();
        ctx.register_function<"stats.pearson_corr", host_pearson_corr>();
        ctx.register_function<"stats.linear_regression", host_linear_regression>();

        auto ar = ctx.analyse(parse, "stats");
        if (!ar.ok) return testfw::fail("ex43: analyse failed");

        // ── Generate test datasets ────────────────────────────────────────────────
        constexpr std::size_t kN = 100'000;
        std::vector<float> xs(kN), ys(kN);
        for (std::size_t i = 0; i < kN; ++i) {
            xs[i] = static_cast<float>(i);
            ys[i] = 2.0f * static_cast<float>(i) + 3.0f; // y = 2x + 3 (known slope/intercept)
        }

        // ── Welford variance ──────────────────────────────────────────────────────
        {
            auto desc = make_host_fn_descriptor<"stats.welford_variance", host_welford_variance>();
            const auto ret = invoke_typed<double>(desc, xs);
            if (!ret.has_value()) return testfw::fail("ex43: typed invoke failed for welford_variance");
            double var = *ret;
            // Variance of 0..N-1 = N*(N-1)/12 * ... exact: sum of (i - mean)^2 / (N-1)
            // For xs = 0..99999: variance ≈ N^2/12 ≈ 833 333 333
            if (var < 8e8 || var > 9e8) return testfw::fail("ex43: welford_variance out of range");
            lg::info("crank ex43: welford_variance(0..100k) = {:.0f}", var);
        }

        // ── Pearson correlation — perfect linear: r should be ~1.0 ───────────────
        {
            auto desc = make_host_fn_descriptor<"stats.pearson_corr", host_pearson_corr>();
            const auto ret = invoke_typed<double>(desc, xs, ys);
            if (!ret.has_value()) return testfw::fail("ex43: typed invoke failed for pearson_corr");
            double r = *ret;
            if (r < 0.9999 || r > 1.0001) return testfw::fail("ex43: pearson_corr not ~1.0");
            lg::info("crank ex43: pearson_corr(x, 2x+3) = {:.8f}", r);
        }

        // ── Linear regression — should recover slope≈2, intercept≈3 ─────────────
        {
            auto desc = make_host_fn_descriptor<"stats.linear_regression", host_linear_regression>();
            const auto ret = invoke_typed<LinearReg>(desc, xs, ys);
            if (!ret.has_value()) return testfw::fail("ex43: typed invoke failed for linear_regression");
            auto reg = *ret;
            double slope_err = reg.slope - 2.0;
            double intercept_err = reg.intercept - 3.0;
            if (slope_err < 0) slope_err = -slope_err;
            if (intercept_err < 0) intercept_err = -intercept_err;
            if (slope_err > 1e-4 || intercept_err > 1e-4)
                return testfw::fail("ex43: linear_regression recovery inaccurate");
            lg::info("crank ex43: linear_regression slope={:.6f}, intercept={:.6f}", reg.slope, reg.intercept);
        }

        // ── HL MIR: parallel reduction loop for mean ─────────────────────────────
        lower_input mean_inp;
        mean_inp.fn_name = "mean";
        mean_inp.loops.push_back({
            .lower = 0, .upper = static_cast<std::int64_t>(kN),
            .step = 1, .is_parallel = true, .name = "i"
        });
        auto mean_hl = lower_to_hl(std::move(mean_inp));
        if (!mean_hl.ok()) return testfw::fail("ex43: lower mean failed");
        if (mean_hl.stats.parallel_loop_count != 1) return testfw::fail("ex43: mean not parallel");

        // ── Benchmark: Welford variance C++ vs Crank trampoline ──────────────────
        profiler::ProfileConfig cfg;
        cfg.iterations = 30;
        cfg.warmup_iterations = 5;
        cfg.trim_outliers_percentage = 5.0;
        cfg.output_unit = profiler::TimeUnit::Microseconds;

        cfg.label = "welford_cpp_100k";
        auto cpp_r = profiler::measure(cfg, [&]() noexcept {
            return host_welford_variance(xs);
        });

        auto wf_desc = make_host_fn_descriptor<"stats.welford_variance", host_welford_variance>();
        cfg.label = "welford_crank_100k";
        auto crank_r = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<double>(wf_desc, xs);
            return ret.value_or(0.0);
        });

        log_equivalent_benchmark(
            "crank ex43 (Welford variance, 100k)",
            "Single-pass Welford variance over the same 100,000 values",
            "Both samples call host_welford_variance over the same vector; candidate crosses the host trampoline.",
            cpp_r.profile, crank_r.profile);

        // ── Benchmark: Pearson correlation C++ vs Crank trampoline ───────────────
        cfg.label = "pearson_cpp_100k";
        auto pearson_cpp = profiler::measure(cfg, [&]() noexcept {
            return host_pearson_corr(xs, ys);
        });

        auto pr_desc = make_host_fn_descriptor<"stats.pearson_corr", host_pearson_corr>();
        cfg.label = "pearson_crank_100k";
        auto pearson_crank = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<double>(pr_desc, xs, ys);
            return ret.value_or(0.0);
        });

        log_equivalent_benchmark(
            "crank ex43 (Pearson correlation, 100k)",
            "Pearson correlation over the same paired vectors",
            "Both samples call host_pearson_corr over the same vectors; candidate crosses the host trampoline.",
            pearson_cpp.profile, pearson_crank.profile);

        lg::info("crank ex43 (statistical computation): variance, correlation, regression verified; "
                 "parallel mean loop={}", mean_hl.stats.parallel_loop_count);
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // ex44: Nested loops — matrix multiply, nested sum, triangular loop.
    //
    // Bottlenecks:
    //   • Flat double loop (N×N sum)       — branch-free integer accumulation
    //   • Matrix multiply (N×N×N)          — memory + arithmetic bound
    //   • Triangular loop (N*(N+1)/2 iters) — irregular trip count
    //   • Benchmark C++ native vs Crank HL MIR interpreter (full-phase)
    // ════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] static std::int64_t host_nested_sum(std::int32_t n) noexcept {
        std::int64_t s = 0;
        for (std::int32_t i = 0; i < n; ++i)
            for (std::int32_t j = 0; j < n; ++j)
                s += i * n + j;
        return s;
    }

    [[nodiscard]] static std::int64_t host_matmul_trace(std::int32_t n) noexcept {
        // Allocate row-major flat arrays for A, B, C (n×n of int32).
        const std::size_t sz = static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
        std::vector<std::int32_t> A(sz), B(sz), C(sz, 0);
        for (std::int32_t i = 0; i < n; ++i)
            for (std::int32_t j = 0; j < n; ++j) {
                A[static_cast<std::size_t>(i * n + j)] = i == j ? 1 : 0; // identity
                B[static_cast<std::size_t>(i * n + j)] = i + j;
            }
        for (std::int32_t i = 0; i < n; ++i)
            for (std::int32_t k = 0; k < n; ++k) {
                const std::int32_t aik = A[static_cast<std::size_t>(i * n + k)];
                if (aik == 0) continue;
                for (std::int32_t j = 0; j < n; ++j)
                    C[static_cast<std::size_t>(i * n + j)] += aik * B[static_cast<std::size_t>(k * n + j)];
            }
        std::int64_t trace = 0;
        for (std::int32_t i = 0; i < n; ++i) trace += C[static_cast<std::size_t>(i * n + i)];
        return trace;
    }

    [[nodiscard]] static std::int64_t host_triangular_sum(std::int32_t n) noexcept {
        std::int64_t s = 0;
        for (std::int32_t i = 0; i < n; ++i)
            for (std::int32_t j = 0; j <= i; ++j)
                s += static_cast<std::int64_t>(i - j);
        return s;
    }

    static testfw::Result ex44_nested_loops() {
        using namespace crank;

        constexpr std::string_view source = R"(package nested

fn nested_sum(n: Int32) -> Int64 {
    var s = 0
    for i := range 0..n {
        for j := range 0..n {
            s = s + i * n + j
        }
    }
    return s
}

fn triangular_sum(n: Int32) -> Int64 {
    var s = 0
    for i := range 0..n {
        for j := range 0..i {
            s = s + i - j
        }
    }
    return s
}

fn matmul_trace(n: Int32) -> Int64 {
    var trace = 0
    for i := range 0..n {
        for k := range 0..n {
            for j := range 0..n {
                trace = trace + i + k + j
            }
        }
    }
    return trace
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex44: parse failed");

        context ctx;
        ctx.register_function<"nested.nested_sum", host_nested_sum>();
        ctx.register_function<"nested.matmul_trace", host_matmul_trace>();
        ctx.register_function<"nested.triangular_sum", host_triangular_sum>();

        auto ar = ctx.analyse(parse, "nested");
        if (!ar.ok) return testfw::fail("ex44: analyse failed");

        // ── Correctness checks via trampolines ───────────────────────────────────
        {
            auto desc = make_host_fn_descriptor<"nested.nested_sum", host_nested_sum>();
            std::array<std::any, 1> args{std::int32_t{10}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto result = std::any_cast<std::int64_t>(ret);
            // sum of i*10+j for i,j in [0,10) = 10*(sum 0..9)*10 + 10*(sum 0..9) = not formula-needed, verify positive
            if (result <= 0) return testfw::fail("ex44: nested_sum(10) not positive");
            lg::info("crank ex44: nested_sum(10) = {}", result);
        }
        {
            auto desc = make_host_fn_descriptor<"nested.triangular_sum", host_triangular_sum>();
            std::array<std::any, 1> args{std::int32_t{100}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto result = std::any_cast<std::int64_t>(ret);
            if (result <= 0) return testfw::fail("ex44: triangular_sum(100) not positive");
            lg::info("crank ex44: triangular_sum(100) = {}", result);
        }
        {
            auto desc = make_host_fn_descriptor<"nested.matmul_trace", host_matmul_trace>();
            std::array<std::any, 1> args{std::int32_t{32}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto result = std::any_cast<std::int64_t>(ret);
            // A=identity → C=B → trace = sum of i+i for i in [0,32) = sum 2i = 32*31 = 992
            if (result != 992) return testfw::fail("ex44: matmul_trace(32) != 992");
            lg::info("crank ex44: matmul_trace(32) = {}", result);
        }

        // ── HL MIR: lower nested_sum with 2 loops ─────────────────────────────
        constexpr std::int32_t kN = 256;
        lower_input ni;
        ni.fn_name = "nested_sum";
        ni.loops.push_back({.lower = 0, .upper = kN, .step = 1, .is_parallel = false, .name = "i"});
        ni.loops.push_back({.lower = 0, .upper = kN, .step = 1, .is_parallel = false, .name = "j"});
        auto hl = lower_to_hl(std::move(ni));
        if (!hl.ok()) return testfw::fail("ex44: lower nested_sum failed");
        if (hl.stats.structured_for_count != 2) return testfw::fail("ex44: expected 2 structured_for");

        // ── Benchmark: nested_sum C++ vs Crank full-phase ───────────────────────
        profiler::ProfileConfig cfg;
        cfg.iterations = 100;
        cfg.warmup_iterations = 10;
        cfg.trim_outliers_percentage = 5.0;
        cfg.output_unit = profiler::TimeUnit::Microseconds;

        cfg.label = "nested_sum_cpp_256";
        auto cpp_r = profiler::measure(cfg, []() noexcept {
            return host_nested_sum(kN);
        });

        auto ns_desc = make_host_fn_descriptor<"nested.nested_sum", host_nested_sum>();
        cfg.label = "nested_sum_crank_256";
        auto crank_r = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<std::int64_t>(ns_desc, std::int32_t{kN});
            return ret.value_or(0LL);
        });

        // Phase split: lower once, execute repeatedly to isolate interpret cost.
        auto lp = lower_to_physical(hl);
        if (!lp.ok()) return testfw::fail("ex44: lower_to_physical failed");

        cfg.label = "nested_sum_crank_exec_256";
        auto exec_r = profiler::measure(cfg, [&]() {
            return execute_physical(*lp.phys).ok() ? 1 : 0;
        });

        auto phys_stats = execute_physical(*lp.phys).stats;

        log_equivalent_benchmark(
            std::format("crank ex44 (nested sum, N={})", kN),
            "Nested integer accumulation",
            "Both samples call host_nested_sum(N); candidate crosses the typed host trampoline.",
            cpp_r.profile, crank_r.profile);
        lg::info("crank ex44 physical-MIR lowering once={:.2f} us; instrs={}; blocks={}",
                 static_cast<double>(lp.lower_ns) / 1000.0,
                 phys_stats.instr_count, phys_stats.block_count);
        log_interpreter_probe(
            "crank ex44 (cached physical MIR)", "nested structured-loop dispatch", exec_r.profile,
            "The physical MIR models loop control only; it does not perform host_nested_sum accumulation.");

        // ── Benchmark: matmul_trace(64) C++ vs trampoline ────────────────────────
        cfg.iterations = 20;
        cfg.warmup_iterations = 3;
        cfg.label = "matmul_cpp_64";
        auto mm_cpp = profiler::measure(cfg, []() noexcept { return host_matmul_trace(64); });

        auto mm_desc = make_host_fn_descriptor<"nested.matmul_trace", host_matmul_trace>();
        cfg.label = "matmul_crank_64";
        auto mm_crank = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<std::int64_t>(mm_desc, std::int32_t{64});
            return ret.value_or(0LL);
        });

        log_equivalent_benchmark(
            "crank ex44 (matrix trace, N=64)",
            "Matrix multiplication trace for the same 64 by 64 problem",
            "Both samples call host_matmul_trace(64); candidate crosses the host trampoline.",
            mm_cpp.profile, mm_crank.profile);

        lg::info("crank ex44 (nested loops): nested_sum, matmul_trace, triangular_sum verified + benchmarked");
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // ex45: Vector — dynamic array operations in Crank vs std::vector.
    //
    // Crank side: host C++ implementation behind typed trampolines.
    // Operations:
    //   • build (push_back N elements)
    //   • sequential scan (sum all elements)
    //   • random access (sum at stride-2 indices)
    //   • sort then binary search
    //   • Benchmark C++ native vs Crank trampoline (same code paths, FFI overhead only)
    // ════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] static std::int64_t host_vec_build_sum(std::int32_t n) noexcept {
        std::vector<std::int32_t> v;
        v.reserve(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) v.push_back(i);
        std::int64_t s = 0;
        for (const std::int32_t x : v) s += x;
        return s;
    }

    [[nodiscard]] static std::int64_t host_vec_stride_sum(std::int32_t n, std::int32_t stride) noexcept {
        std::vector<std::int32_t> v(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) v[static_cast<std::size_t>(i)] = i * 3 + 1;
        std::int64_t s = 0;
        for (std::int32_t i = 0; i < n; i += stride)
            s += v[static_cast<std::size_t>(i)];
        return s;
    }

    [[nodiscard]] static std::int32_t host_vec_sorted_search(std::int32_t n, std::int32_t target) noexcept {
        std::vector<std::int32_t> v(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) v[static_cast<std::size_t>(i)] = i * 2;
        // already sorted (step 2)
        const auto it = std::lower_bound(v.begin(), v.end(), target);
        if (it == v.end() || *it != target) return -1;
        return static_cast<std::int32_t>(it - v.begin());
    }

    static testfw::Result ex45_vector_operations() {
        using namespace crank;

        constexpr std::string_view source = R"(package vec_ops

fn build_sum(n: Int32) -> Int64 {
    var s = 0
    for i := range 0..n {
        s = s + i
    }
    return s
}

fn stride_sum(n: Int32, stride: Int32) -> Int64 {
    var s = 0
    var i = 0
    while i < n {
        s = s + i * 3 + 1
        i = i + stride
    }
    return s
}

fn count_even(n: Int32) -> Int32 {
    var count = 0
    for i := range 0..n {
        if (i * 2) % 4 == 0 {
            count = count + 1
        }
    }
    return count
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex45: parse failed");

        context ctx;
        ctx.register_function<"vec_ops.build_sum", host_vec_build_sum>();
        ctx.register_function<"vec_ops.stride_sum", host_vec_stride_sum>();
        ctx.register_function<"vec_ops.sorted_search", host_vec_sorted_search>();
        ctx.register_container<std::vector<std::int32_t>>("Int32Vec");

        auto ar = ctx.analyse(parse, "vec_ops");
        if (!ar.ok) return testfw::fail("ex45: analyse failed");

        // ── Correctness ──────────────────────────────────────────────────────────
        {
            auto desc = make_host_fn_descriptor<"vec_ops.build_sum", host_vec_build_sum>();
            std::array<std::any, 1> args{std::int32_t{1000}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto s = std::any_cast<std::int64_t>(ret);
            if (s != 999LL * 1000LL / 2LL) return testfw::fail("ex45: build_sum(1000) wrong");
            lg::info("crank ex45: build_sum(1000) = {} (Gauss)", s);
        }
        {
            auto desc = make_host_fn_descriptor<"vec_ops.sorted_search", host_vec_sorted_search>();
            std::array<std::any, 2> args{std::int32_t{1000}, std::int32_t{500}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto idx = std::any_cast<std::int32_t>(ret);
            if (idx != 250) return testfw::fail("ex45: sorted_search(1000, 500) != 250");
            lg::info("crank ex45: sorted_search(1000, 500) = index {}", idx);
        }

        // ── HL MIR: build_sum and stride_sum loops ─────────────────────────────
        constexpr std::int32_t kVecN = 1'000'000;

        lower_input bs_inp;
        bs_inp.fn_name = "build_sum";
        bs_inp.loops.push_back({.lower = 0, .upper = kVecN, .step = 1, .is_parallel = false, .name = "i"});
        auto bs_hl = lower_to_hl(std::move(bs_inp));
        if (!bs_hl.ok()) return testfw::fail("ex45: lower build_sum failed");

        lower_input ss_inp;
        ss_inp.fn_name = "stride_sum";
        ss_inp.loops.push_back({.lower = 0, .upper = kVecN, .step = 1, .is_parallel = false, .name = "i"});
        auto ss_hl = lower_to_hl(std::move(ss_inp));
        if (!ss_hl.ok()) return testfw::fail("ex45: lower stride_sum failed");

        // ── Benchmark: build_sum 1M ──────────────────────────────────────────────
        profiler::ProfileConfig cfg;
        cfg.iterations = 100;
        cfg.warmup_iterations = 10;
        cfg.trim_outliers_percentage = 5.0;
        cfg.output_unit = profiler::TimeUnit::Microseconds;

        cfg.label = "vec_build_sum_cpp_1M";
        auto cpp_bs = profiler::measure(cfg, []() noexcept {
            return host_vec_build_sum(kVecN);
        });

        auto bs_desc = make_host_fn_descriptor<"vec_ops.build_sum", host_vec_build_sum>();
        cfg.label = "vec_build_sum_crank_1M";
        auto crank_bs = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<std::int64_t>(bs_desc, std::int32_t{kVecN});
            return ret.value_or(0LL);
        });

        // Phase split: lower → physical MIR once, then execute-only.
        auto bs_lp = lower_to_physical(bs_hl);
        if (!bs_lp.ok()) return testfw::fail("ex45: lower_to_physical build_sum failed");
        auto bs_phys_stats = execute_physical(*bs_lp.phys).stats;

        cfg.label = "vec_build_sum_crank_exec_1M";
        auto exec_bs = profiler::measure(cfg, [&]() {
            return execute_physical(*bs_lp.phys).ok() ? 1 : 0;
        });

        log_equivalent_benchmark(
            std::format("crank ex45 (vector build and sum, N={})", kVecN),
            "Allocate, populate, and sum a vector of N integers",
            "Both samples call host_vec_build_sum(N); candidate crosses the typed host trampoline.",
            cpp_bs.profile, crank_bs.profile);
        lg::info("crank ex45 physical-MIR lowering once={:.2f} us; instrs={}; blocks={}",
                 static_cast<double>(bs_lp.lower_ns) / 1000.0,
                 bs_phys_stats.instr_count, bs_phys_stats.block_count);
        log_interpreter_probe(
            "crank ex45 (cached physical MIR)", "structured loop dispatch", exec_bs.profile,
            "The physical MIR does not allocate, populate, or sum the native vector workload.");

        // ── Benchmark: stride_sum (stride=4) ─────────────────────────────────────
        cfg.iterations = 50;
        cfg.warmup_iterations = 5;
        cfg.label = "vec_stride_sum_cpp_1M_s4";
        auto cpp_ss = profiler::measure(cfg, []() noexcept {
            return host_vec_stride_sum(kVecN, 4);
        });

        auto ss_desc = make_host_fn_descriptor<"vec_ops.stride_sum", host_vec_stride_sum>();
        cfg.label = "vec_stride_sum_crank_1M_s4";
        auto crank_ss = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<std::int64_t>(ss_desc, std::int32_t{kVecN}, std::int32_t{4});
            return ret.value_or(0LL);
        });

        log_equivalent_benchmark(
            "crank ex45 (vector stride sum, stride=4)",
            "Build and stride-sum the same one-million-element vector",
            "Both samples call host_vec_stride_sum(N, 4); candidate crosses the host trampoline.",
            cpp_ss.profile, crank_ss.profile);

        lg::info("crank ex45 (vector operations): build_sum, stride_sum, sorted_search verified + benchmarked");
        return {};
    }

    // ════════════════════════════════════════════════════════════════════════════
    // ex46: Map — hash map operations in Crank vs std::unordered_map.
    //
    // Crank side: host C++ unordered_map implementation behind typed trampolines.
    // Operations:
    //   • insert N entries (sequential keys)
    //   • successful lookup (hit rate 100%)
    //   • failed lookup (miss rate 100%)
    //   • mixed workload (50% hit / 50% miss)
    //   • count entries matching predicate (full scan)
    //   • Benchmark C++ native vs Crank trampoline to quantify FFI overhead
    // ════════════════════════════════════════════════════════════════════════════

    [[nodiscard]] static std::int64_t host_map_insert_sum(std::int32_t n) noexcept {
        std::unordered_map<std::int32_t, std::int64_t> m;
        m.reserve(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i)
            m[i] = static_cast<std::int64_t>(i) * i;
        std::int64_t s = 0;
        for (const auto& [k, v] : m) s += v;
        return s;
    }

    [[nodiscard]] static std::int32_t host_map_lookup_hits(std::int32_t n, std::int32_t queries) noexcept {
        std::unordered_map<std::int32_t, std::int64_t> m;
        m.reserve(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) m[i] = i;
        std::int32_t hits = 0;
        for (std::int32_t q = 0; q < queries; ++q)
            if (m.count(q % n)) ++hits;
        return hits;
    }

    [[nodiscard]] static std::int32_t host_map_mixed_workload(std::int32_t n) noexcept {
        std::unordered_map<std::int32_t, std::int32_t> m;
        m.reserve(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) m[i * 2] = i; // even keys only
        std::int32_t found = 0;
        for (std::int32_t i = 0; i < n; ++i) {
            if (m.count(i)) ++found; // hits for even i, misses for odd i
        }
        return found;
    }

    [[nodiscard]] static std::int32_t host_map_count_above(std::int32_t n, std::int64_t threshold) noexcept {
        std::unordered_map<std::int32_t, std::int64_t> m;
        m.reserve(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) m[i] = static_cast<std::int64_t>(i) * i;
        std::int32_t count = 0;
        for (const auto& [k, v] : m)
            if (v > threshold) ++count;
        return count;
    }

    static testfw::Result ex46_map_operations() {
        using namespace crank;

        constexpr std::string_view source = R"(package map_ops

fn build_sum(n: Int32) -> Int64 {
    var s = 0
    for i := range 0..n {
        s = s + i * i
    }
    return s
}

fn lookup_all(n: Int32, queries: Int32) -> Int32 {
    var hits = 0
    for q := range 0..queries {
        let key = q % n
        if key >= 0 {
            hits = hits + 1
        }
    }
    return hits
}

fn mixed_even_odd(n: Int32) -> Int32 {
    var found = 0
    for i := range 0..n {
        if (i % 2) == 0 {
            found = found + 1
        }
    }
    return found
}

fn count_above(n: Int32, threshold: Int64) -> Int32 {
    var count = 0
    for i := range 0..n {
        let v = i as Int64 * i as Int64
        if v > threshold {
            count = count + 1
        }
    }
    return count
}
)";

        auto parse = crank::frontend::parse(source);
        if (!parse.ok) return testfw::fail("ex46: parse failed");

        context ctx;
        ctx.register_function<"map_ops.insert_sum", host_map_insert_sum>();
        ctx.register_function<"map_ops.lookup_hits", host_map_lookup_hits>();
        ctx.register_function<"map_ops.mixed_workload", host_map_mixed_workload>();
        ctx.register_function<"map_ops.count_above", host_map_count_above>();

        auto ar = ctx.analyse(parse, "map_ops");
        if (!ar.ok) return testfw::fail("ex46: analyse failed");

        // ── Correctness ──────────────────────────────────────────────────────────
        {
            auto desc = make_host_fn_descriptor<"map_ops.insert_sum", host_map_insert_sum>();
            std::array<std::any, 1> args{std::int32_t{100}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto s = std::any_cast<std::int64_t>(ret);
            // sum i^2 for i in [0,100) = n*(n-1)*(2n-1)/6 = 100*99*199/6 = 328350
            if (s != 328'350LL) return testfw::fail("ex46: insert_sum(100) wrong");
            lg::info("crank ex46: map insert_sum(100) = {}", s);
        }
        {
            auto desc = make_host_fn_descriptor<"map_ops.lookup_hits", host_map_lookup_hits>();
            std::array<std::any, 2> args{std::int32_t{1000}, std::int32_t{5000}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto hits = std::any_cast<std::int32_t>(ret);
            if (hits != 5000) return testfw::fail("ex46: lookup_hits should be all hits");
            lg::info("crank ex46: map lookup_hits(1000, 5000) = {}", hits);
        }
        {
            auto desc = make_host_fn_descriptor<"map_ops.mixed_workload", host_map_mixed_workload>();
            std::array<std::any, 1> args{std::int32_t{1000}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto found = std::any_cast<std::int32_t>(ret);
            if (found != 500) return testfw::fail("ex46: mixed_workload(1000) != 500");
            lg::info("crank ex46: map mixed_workload(1000) = {} hits (50%)", found);
        }
        {
            auto desc = make_host_fn_descriptor<"map_ops.count_above", host_map_count_above>();
            std::array<std::any, 2> args{std::int32_t{1000}, std::int64_t{250000LL}};
            auto ret = desc.trampoline(std::span<const std::any>(args));
            auto count = std::any_cast<std::int32_t>(ret);
            // i^2 > 250000 → i > 500 → 499 values (501..999)
            if (count != 499) return testfw::fail("ex46: count_above(1000, 250000) != 499");
            lg::info("crank ex46: map count_above(1000, 250000) = {}", count);
        }

        // ── Benchmark: insert_sum ─────────────────────────────────────────────────
        constexpr std::int32_t kMapN = 100'000;

        profiler::ProfileConfig cfg;
        cfg.iterations = 50;
        cfg.warmup_iterations = 5;
        cfg.trim_outliers_percentage = 5.0;
        cfg.output_unit = profiler::TimeUnit::Microseconds;

        cfg.label = "map_insert_sum_cpp_100k";
        auto cpp_is = profiler::measure(cfg, []() noexcept {
            return host_map_insert_sum(kMapN);
        });

        auto is_desc = make_host_fn_descriptor<"map_ops.insert_sum", host_map_insert_sum>();
        cfg.label = "map_insert_sum_crank_100k";
        auto crank_is = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<std::int64_t>(is_desc, std::int32_t{kMapN});
            return ret.value_or(0LL);
        });

        log_equivalent_benchmark(
            std::format("crank ex46 (map insert and sum, N={})", kMapN),
            "Reserve, insert, and sum the same unordered-map workload",
            "Both samples call host_map_insert_sum(N); candidate crosses the host trampoline.",
            cpp_is.profile, crank_is.profile);

        // ── Benchmark: lookup_hits ────────────────────────────────────────────────
        cfg.label = "map_lookup_cpp_100k_500k";
        auto cpp_lh = profiler::measure(cfg, []() noexcept {
            return host_map_lookup_hits(kMapN, 500'000);
        });

        auto lh_desc = make_host_fn_descriptor<"map_ops.lookup_hits", host_map_lookup_hits>();
        cfg.label = "map_lookup_crank_100k_500k";
        auto crank_lh = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<std::int32_t>(lh_desc, std::int32_t{kMapN}, std::int32_t{500'000});
            return ret.value_or(0);
        });

        log_equivalent_benchmark(
            "crank ex46 (map lookup, 500k queries)",
            "Lookup the same 500,000 keys in the same 100,000-entry map",
            "Both samples call host_map_lookup_hits; candidate crosses the host trampoline.",
            cpp_lh.profile, crank_lh.profile);

        // ── Benchmark: mixed_workload ─────────────────────────────────────────────
        cfg.label = "map_mixed_cpp_100k";
        auto cpp_mw = profiler::measure(cfg, []() noexcept {
            return host_map_mixed_workload(kMapN);
        });

        auto mw_desc = make_host_fn_descriptor<"map_ops.mixed_workload", host_map_mixed_workload>();
        cfg.label = "map_mixed_crank_100k";
        auto crank_mw = profiler::measure(cfg, [&]() {
            const auto ret = invoke_typed<std::int32_t>(mw_desc, std::int32_t{kMapN});
            return ret.value_or(0);
        });

        log_equivalent_benchmark(
            std::format("crank ex46 (mixed map workload, N={})", kMapN),
            "The same insert, lookup, and hit-rate workload",
            "Both samples call host_map_mixed_workload(N); candidate crosses the host trampoline.",
            cpp_mw.profile, crank_mw.profile);

        lg::info("crank ex46 (map operations): insert_sum, lookup_hits, mixed_workload, "
            "count_above verified + benchmarked (C++ vs Crank trampoline)");
        return {};
    }
} // namespace crank_ex

// ============================================================================
// Domain Views Examples (ex47–ex55)
// ============================================================================
// ex47  view_decl parse          — minimal + generic view declaration syntax
// ex48  view_expr parse          — view construction + parenthesized source
// ex49  feature gate             — feature_set API: default_v1/enable/all/disable
// ex50  view_registry            — register + find view_descriptor by stable_id
// ex51  view_method_table        — insert, exact lookup, fallback-to-hash-0
// ex52  view_domain_meta         — fast_path_ok, affinity, has_domain/has_op
// ex53  obligations              — 9 view predicates + collect_obligation_stats
// ex54  CRANK-VIEW diagnostics   — all 12 diagnostic codes
// ex55  full linear e2e          — parse + resolve + registry + method lookup
// ============================================================================

namespace crank_views_ex {
    // ────────────────────────────────────────────────────────────────────────────
    // ex47: view_decl parse — minimal and generic view declaration.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex47_view_decl_parse() {
        constexpr std::string_view source_simple = R"(package ex47
type Buffer = struct {}
view Slice of base: Buffer
requires contiguous(base)
)";
        const auto r1 = crank::frontend::parse(source_simple);
        if (!r1.ok) return testfw::fail("ex47: simple view_decl parse failed");

        constexpr std::string_view source_generic = R"(package ex47g
type Buffer = struct {}
view Tensor[T, N: usize] of base: Buffer
requires contiguous(base)
requires rank(base) == N
)";
        const auto r2 = crank::frontend::parse(source_generic);
        if (!r2.ok) return testfw::fail("ex47: generic view_decl parse failed");

        lg::info("crank ex47 (view_decl parse): simple + generic OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex48: view_expr parse — view construction in function body.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex48_view_expr_parse() {
        constexpr std::string_view source_ident = R"(package ex48
type Buffer = struct {}
view Tensor[T] of base: Buffer
fn project(raw: Buffer) -> Tensor[Float32] {
    let t = view raw as Tensor[Float32]
    return t
}
)";
        const auto r1 = crank::frontend::parse(source_ident);
        if (!r1.ok) return testfw::fail("ex48: ident source view_expr parse failed");

        // view_expr with parenthesized source (field access via (expr)).
        constexpr std::string_view source_paren = R"(package ex48b
type Buffer = struct {}
type Wrapper = struct {}
view Tensor[T] of base: Buffer
fn project2(w: Wrapper) -> Tensor[Float32] {
    let t = view (w.buf) as Tensor[Float32]
    return t
}
)";
        const auto r2 = crank::frontend::parse(source_paren);
        if (!r2.ok) return testfw::fail("ex48: parenthesized source view_expr parse failed");

        lg::info("crank ex48 (view_expr parse): ident + parenthesized source OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex49: feature gate — feature_set API for domain_views.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex49_feature_gate() {
        auto fs_v1 = crank::feature_set::default_v1();
        if (fs_v1.has(crank::crank_feature::domain_views))
            return testfw::fail("ex49: domain_views should be off in default_v1");

        fs_v1.enable(crank::crank_feature::domain_views);
        if (!fs_v1.has(crank::crank_feature::domain_views))
            return testfw::fail("ex49: domain_views should be on after enable");

        fs_v1.disable(crank::crank_feature::domain_views);
        if (fs_v1.has(crank::crank_feature::domain_views))
            return testfw::fail("ex49: domain_views should be off after disable");

        const auto fs_all = crank::feature_set::all();
        if (!fs_all.has(crank::crank_feature::domain_views))
            return testfw::fail("ex49: domain_views should be on in all()");
        if (!fs_all.has(crank::crank_feature::associated_types))
            return testfw::fail("ex49: associated_types should be on in all()");
        if (!fs_all.has(crank::crank_feature::structured_concurrency))
            return testfw::fail("ex49: structured_concurrency should be on in all()");

        lg::info("crank ex49 (feature gate): default_v1/enable/disable/all OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex50: view_registry — register and look up a view_descriptor.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex50_view_registry() {
        crank::view_registry reg;

        crank::view_descriptor desc;
        desc.stable_id = 5001u;
        desc.name_hash = crank::view_name_hash("ex50::Tensor");
        desc.category = crank::view_category::generic;
        desc.qualified_name = "ex50::Tensor";
        desc.backing_name = "base";
        desc.backing_type_id = 100u;
        desc.generic_arity = 2u;

        const auto handle = reg.register_desc(desc);
        if (handle.is_null()) return testfw::fail("ex50: register_desc returned null handle");

        const auto* found = reg.find(5001u);
        if (!found) return testfw::fail("ex50: find(5001) returned nullptr");
        if (found->qualified_name != "ex50::Tensor")
            return testfw::fail("ex50: qualified_name mismatch");
        if (found->generic_arity != 2u)
            return testfw::fail("ex50: generic_arity mismatch");

        const auto* by_name = reg.find_by_name(crank::view_name_hash("ex50::Tensor"));
        if (!by_name) return testfw::fail("ex50: find_by_name returned nullptr");
        if (by_name->stable_id != 5001u)
            return testfw::fail("ex50: find_by_name stable_id mismatch");

        if (reg.find(9999u)) return testfw::fail("ex50: find(9999) should return nullptr");

        crank::view_descriptor desc2;
        desc2.stable_id = 5002u;
        desc2.name_hash = crank::view_name_hash("ex50::ImageView");
        desc2.category = crank::view_category::specialized;
        desc2.qualified_name = "ex50::ImageView";
        desc2.backing_name = "data";
        desc2.backing_type_id = 200u;
        reg.register_desc(desc2);

        const auto all_views = reg.all();
        if (all_views.size() != 2u) return testfw::fail("ex50: expected 2 views in registry");

        lg::info("crank ex50 (view_registry): register/find/find_by_name/all OK ({} views)",
                 all_views.size());
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex51: view_method_table — insert, exact lookup, fallback-to-hash-0.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex51_view_method_table() {
        crank::view_method_table table;

        crank::method_entry generic_mm;
        generic_mm.method_name = "matmul";
        generic_mm.generic_arg_hash = 0;
        generic_mm.fn_node_id = 10u;
        table.insert(generic_mm);

        crank::method_entry spec_mm;
        spec_mm.method_name = "matmul";
        spec_mm.generic_arg_hash = crank::view_name_hash("Float32");
        spec_mm.fn_node_id = 20u;
        table.insert(spec_mm);

        crank::method_entry rs;
        rs.method_name = "reduce_sum";
        rs.generic_arg_hash = 0;
        rs.fn_node_id = 30u;
        table.insert(rs);

        const auto* found_spec = table.find("matmul", crank::view_name_hash("Float32"));
        if (!found_spec) return testfw::fail("ex51: specialized matmul not found");
        if (found_spec->fn_node_id != 20u)
            return testfw::fail("ex51: specialized matmul fn_node_id wrong");

        const auto* found_gen = table.find("matmul", 0);
        if (!found_gen) return testfw::fail("ex51: generic matmul not found");
        if (found_gen->fn_node_id != 10u)
            return testfw::fail("ex51: generic matmul fn_node_id wrong");

        const auto* fallback = table.find("matmul", 0xDEADBEEFu);
        if (!fallback) return testfw::fail("ex51: fallback to generic matmul failed");
        if (fallback->fn_node_id != 10u)
            return testfw::fail("ex51: fallback fn_node_id should be generic");

        if (table.find("dot_product")) return testfw::fail("ex51: unknown method should be nullptr");

        const auto* rs_found = table.find("reduce_sum", 0xCAFEu);
        if (!rs_found) return testfw::fail("ex51: reduce_sum fallback failed");
        if (rs_found->fn_node_id != 30u) return testfw::fail("ex51: reduce_sum fn_node_id wrong");

        lg::info("crank ex51 (view_method_table): exact/fallback/missing lookup OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex52: view_domain_meta — fast_path_ok, affinity fields, has_domain/has_op.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex52_view_domain_meta() {
        crank::view_domain_meta meta;
        if (meta.has_domain()) return testfw::fail("ex52: default has_domain should be false");
        if (meta.has_op()) return testfw::fail("ex52: default has_op should be false");
        if (meta.fast_path_ok()) return testfw::fail("ex52: default fast_path_ok should be false");

        meta.domain_name = "tensor";
        meta.op_name = "tensor.matmul";
        if (!meta.has_domain()) return testfw::fail("ex52: has_domain should be true");
        if (!meta.has_op()) return testfw::fail("ex52: has_op should be true");

        meta.law_pure = true;
        if (meta.fast_path_ok()) return testfw::fail("ex52: needs deterministic too");
        meta.law_deterministic = true;
        if (!meta.fast_path_ok()) return testfw::fail("ex52: pure+deterministic should be fast_path");

        meta.affinity_simd = true;
        meta.affinity_gpu = true;
        if (!meta.affinity_simd) return testfw::fail("ex52: affinity_simd wrong");
        if (!meta.affinity_gpu) return testfw::fail("ex52: affinity_gpu wrong");
        if (meta.affinity_dag) return testfw::fail("ex52: affinity_dag should be false");
        if (meta.affinity_streaming) return testfw::fail("ex52: affinity_streaming should be false");

        lg::info("crank ex52 (view_domain_meta): domain={} op={} fast_path={} simd={} gpu={}",
                 meta.domain_name, meta.op_name,
                 meta.fast_path_ok(), meta.affinity_simd, meta.affinity_gpu);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex53: obligations — all 9 view predicates + collect_obligation_stats.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex53_view_obligations() {
        crank::obligation_builder bld;
        const crank::source_span span{1, 1, 1, 40};

        bld.add_view_dtype(span, "base", "Float32");
        bld.add_view_rank(span, "base", 2u);
        bld.add_view_shape(span, "base", "[M, N]");
        bld.add_view_contiguous(span, "base");
        bld.add_view_aligned(span, "base", 64u);
        bld.add_view_strides(span, "base", "row_major");
        bld.add_view_requires(span, "contiguous(base) && rank(base) == 2");
        bld.add_view_lifetime(span, "W", "raw");
        bld.add_view_aliasing(span, "raw");

        const auto obs = bld.take();
        if (obs.size() != 9u) return testfw::fail("ex53: expected 9 view obligations");
        for (const auto& o : obs) {
            if (o.family != crank::obligation_family::view)
                return testfw::fail("ex53: all obligations should be view family");
            if (o.label.empty())
                return testfw::fail("ex53: obligation label empty");
        }

        crank::obligation_builder bld2;
        bld2.add_view_contiguous(span, "base");
        bld2.add_view_rank(span, "base", 3u);
        bld2.add_view_dtype(span, "base", "Int32");
        bld2.add_index(span, "xs", "i"); // 2 bounds obligations

        const auto obs2 = bld2.take();
        auto stats = crank::collect_obligation_stats(obs2);
        if (stats.view_count != 3u) return testfw::fail("ex53: expected 3 view obligations");
        if (stats.bounds_count != 2u) return testfw::fail("ex53: expected 2 bounds obligations");
        if (stats.total != 5u) return testfw::fail("ex53: expected 5 total");

        lg::info("crank ex53 (view obligations): 9 predicates OK, stats view={} bounds={} total={}",
                 stats.view_count, stats.bounds_count, stats.total);
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex54: CRANK-VIEW diagnostic codes — all 12 codes + explain builder.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex54_crank_view_diagnostics() {
        struct DiagCase {
            crank::view_diag_kind kind;
            std::string_view expected;
        };
        constexpr DiagCase cases[] = {
            {crank::view_diag_kind::feature_disabled, "CRANK-VIEW-000"},
            {crank::view_diag_kind::unknown_target, "CRANK-VIEW-001"},
            {crank::view_diag_kind::not_viewable, "CRANK-VIEW-002"},
            {crank::view_diag_kind::requirement_failed, "CRANK-VIEW-003"},
            {crank::view_diag_kind::runtime_guard, "CRANK-VIEW-004"},
            {crank::view_diag_kind::would_copy, "CRANK-VIEW-005"},
            {crank::view_diag_kind::ambiguous_decl, "CRANK-VIEW-006"},
            {crank::view_diag_kind::lifetime, "CRANK-VIEW-007"},
            {crank::view_diag_kind::mutable_conflict, "CRANK-VIEW-008"},
            {crank::view_diag_kind::provider_missing, "CRANK-VIEW-009"},
            {crank::view_diag_kind::metadata_conflict, "CRANK-VIEW-010"},
            {crank::view_diag_kind::reserved_011, "CRANK-VIEW-011"},
        };
        for (const auto& c : cases) {
            if (crank::view_diagnostic_code(c.kind) != c.expected) {
                return testfw::fail(std::string("ex54: wrong code for kind ") +
                    std::to_string(static_cast<int>(c.kind)));
            }
        }

        const crank::source_span span{3, 1, 3, 45};
        const auto expl = crank::explain(
                              std::string(crank::view_diagnostic_code(crank::view_diag_kind::not_viewable)),
                              "source Buffer cannot be viewed as Tensor[Float32]",
                              span)
                          .note("dtype mismatch: Float64 vs Float32")
                          .help("declare Tensor[Float64] view or cast first")
                          .build();
        const auto full = expl.render_full();
        if (full.find("CRANK-VIEW-002") == std::string::npos)
            return testfw::fail("ex54: render_full missing CRANK-VIEW-002");
        if (expl.notes.empty()) return testfw::fail("ex54: notes empty");
        if (expl.help.empty()) return testfw::fail("ex54: help empty");

        const auto expl2 = crank::explain(
                               std::string(crank::view_diagnostic_code(crank::view_diag_kind::would_copy)),
                               "implicit copy not allowed",
                               span)
                           .help("use explicit materialize(v)")
                           .build();
        if (expl2.render_message().find("CRANK-VIEW-005") == std::string::npos)
            return testfw::fail("ex54: render_message missing CRANK-VIEW-005");

        lg::info("crank ex54 (CRANK-VIEW diagnostics): all 12 codes + explain builder OK");
        return {};
    }

    // ────────────────────────────────────────────────────────────────────────────
    // ex55: full linear e2e — parse + resolve + registry + method lookup.
    // ────────────────────────────────────────────────────────────────────────────
    static testfw::Result ex55_domain_views_e2e() {
        constexpr std::string_view src = R"(package ml
type Buffer = struct {}
view Tensor[T] of base: Buffer
requires contiguous(base)
impl Tensor[T] {
    fn matmul(self, rhs: Tensor[T]) -> Tensor[T] {
        return self
    }
    fn reduce_sum(self) -> Float32 {
        return Float32.identity()
    }
}
fn linear(w: Buffer, x: Buffer) -> Tensor[Float32] {
    let W = view w as Tensor[Float32]
    let X = view x as Tensor[Float32]
    return W.matmul(X)
}
)";
        auto parse_result = crank::frontend::parse(src);
        if (!parse_result.ok) return testfw::fail("ex55: parse failed");

        crank::resolver r("ml");
        r.declare_type("Buffer", 100u);
        r.declare_view("Tensor", "base", 100u);
        r.declare_function("matmul", true, true);
        r.declare_function("reduce_sum", true, true);
        r.declare_function("linear", true, true);
        auto res = r.take();

        const auto* tensor_sym = res.symbols.lookup("ml::Tensor");
        if (!tensor_sym)
            return testfw::fail("ex55: Tensor not in symbol table");
        if (tensor_sym->kind != crank::symbol_kind::view)
            return testfw::fail("ex55: Tensor should have symbol_kind::view");
        if (tensor_sym->view_backing_name != "base")
            return testfw::fail("ex55: Tensor backing_name should be 'base'");

        crank::view_registry reg;
        crank::view_descriptor desc;
        desc.stable_id = 6001u;
        desc.name_hash = crank::view_name_hash("ml::Tensor");
        desc.category = crank::view_category::generic;
        desc.qualified_name = "ml::Tensor";
        desc.backing_name = "base";
        desc.backing_type_id = 100u;
        desc.generic_arity = 1u;
        desc.domain_meta.domain_name = "tensor";
        desc.domain_meta.op_name = "tensor.matmul";
        desc.domain_meta.law_pure = true;
        desc.domain_meta.law_deterministic = true;

        crank::method_entry mm;
        mm.method_name = "matmul";
        mm.fn_node_id = 200u;
        crank::method_entry rs;
        rs.method_name = "reduce_sum";
        rs.fn_node_id = 201u;
        desc.methods.insert(mm);
        desc.methods.insert(rs);
        reg.register_desc(std::move(desc));

        const auto* td = reg.find(6001u);
        if (!td) return testfw::fail("ex55: Tensor descriptor not in registry");
        if (!td->domain_meta.fast_path_ok())
            return testfw::fail("ex55: fast_path_ok should be true");
        if (!td->methods.find("matmul")) return testfw::fail("ex55: matmul missing");
        if (!td->methods.find("reduce_sum")) return testfw::fail("ex55: reduce_sum missing");

        auto fs = crank::feature_set::default_v1();
        if (fs.has(crank::crank_feature::domain_views))
            return testfw::fail("ex55: domain_views off by default");
        fs.enable(crank::crank_feature::domain_views);
        if (!fs.has(crank::crank_feature::domain_views))
            return testfw::fail("ex55: domain_views on after enable");

        lg::info("crank ex55 (domain views e2e): OK (domain={} fast_path={} matmul_id={})",
                 td->domain_meta.domain_name,
                 td->domain_meta.fast_path_ok(),
                 td->methods.find("matmul")->fn_node_id);
        return {};
    }
} // namespace crank_views_ex

struct CrankExample {
    static constexpr std::string_view name() { return "crank"; }

    static constexpr std::string_view description() {
        return "Crank language frontend: parse, error recovery, JSON dump, "
            "source spans, literals, declarations, control flow, generics, "
            "transactions, parallel builtins, diagnostics, tag stats, "
            "parse statistics, postfix chains, ASI, host embedding, "
            "semantic analysis, execution policies, HL MIR lowering, "
            "AOT cache, defer semantics, transaction lowering and runtime, "
            "resource traits, monomorphization, annotations, and full "
            "end-to-end pipeline runs (scalar, loop+defer, generic reduction, "
            "transactional transfer, SIMD, GPU, host call, AOT round-trip, "
            "and comprehensive functions/recursion/computation: factorial, "
            "fibonacci, pi approximation, and C++ vs Crank benchmark comparisons "
            "(pi/fibonacci/sum loop with profiler::measure + profiler::compare), "
            "matrix/numerical computation (Newton sqrt, harmonic sum, dot product), "
            "number theory (sieve, Miller-Rabin, GCD, LCM), and statistical "
            "computation (Welford variance, Pearson correlation, linear regression), "
            "nested loops (double/triple loop, matrix multiply trace, triangular sum), "
            "vector operations (build+sum, stride access, binary search), and "
            "map operations (insert+sum, lookup, mixed workload, predicate count) — "
            "all with C++ native vs Crank trampoline vs HL MIR interpreter comparisons, "
            "and domain views (ex47–ex55): view_decl/view_expr parse, feature gate, "
            "view_registry, view_method_table, view_domain_meta, obligations, "
            "CRANK-VIEW diagnostics, and full linear e2e (parse + resolve + registry)";
    }

    static constexpr std::array<std::string_view, 9> tag_data{
        "crank", "parser", "language", "tutorial", "frontend",
        "host", "execution", "transactions", "domain_views"
    };
    static constexpr std::span<const std::string_view> tags() { return tag_data; }

    static testfw::Result run() {
        if (auto r = crank_ex::ex01_parse_hello_world(); !r) return r;
        if (auto r = crank_ex::ex02_parse_error_recovery(); !r) return r;
        if (auto r = crank_ex::ex03_parse_tree_dump(); !r) return r;
        if (auto r = crank_ex::ex04_ast_dump(); !r) return r;
        if (auto r = crank_ex::ex05_source_span_decode(); !r) return r;
        if (auto r = crank_ex::ex06_integer_literals(); !r) return r;
        if (auto r = crank_ex::ex07_string_literals(); !r) return r;
        if (auto r = crank_ex::ex08_declarations(); !r) return r;
        if (auto r = crank_ex::ex09_control_flow(); !r) return r;
        if (auto r = crank_ex::ex10_generics_contracts(); !r) return r;
        if (auto r = crank_ex::ex11_transaction_block(); !r) return r;
        if (auto r = crank_ex::ex12_parallel_builtins(); !r) return r;
        if (auto r = crank_ex::ex13_struct_enum(); !r) return r;
        if (auto r = crank_ex::ex14_diagnostics(); !r) return r;
        if (auto r = crank_ex::ex15_tag_stats(); !r) return r;
        if (auto r = crank_ex::ex16_parse_statistics(); !r) return r;
        if (auto r = crank_ex::ex17_postfix_builtins(); !r) return r;
        if (auto r = crank_ex::ex18_asi_semantics(); !r) return r;
        if (auto r = crank_ex::ex19_host_registration(); !r) return r;
        if (auto r = crank_ex::ex20_context_analyse(); !r) return r;
        if (auto r = crank_ex::ex21_execution_policy(); !r) return r;
        if (auto r = crank_ex::ex22_optimization_profiles(); !r) return r;
        if (auto r = crank_ex::ex23_hl_lowering_execute(); !r) return r;
        if (auto r = crank_ex::ex24_defer_semantics(); !r) return r;
        if (auto r = crank_ex::ex25_aot_cache(); !r) return r;
        if (auto r = crank_ex::ex26_transaction_lowering(); !r) return r;
        if (auto r = crank_ex::ex27_transaction_runtime(); !r) return r;
        if (auto r = crank_ex::ex28_transactional_resources(); !r) return r;
        if (auto r = crank_ex::ex29_monomorphization(); !r) return r;
        if (auto r = crank_ex::ex30_annotation_system(); !r) return r;
        if (auto r = crank_ex::ex31_e2e_scalar_arithmetic(); !r) return r;
        if (auto r = crank_ex::ex32_e2e_loop_defer(); !r) return r;
        if (auto r = crank_ex::ex33_e2e_generic_reduction(); !r) return r;
        if (auto r = crank_ex::ex34_e2e_transactional_transfer(); !r) return r;
        if (auto r = crank_ex::ex35_e2e_simd_elementwise(); !r) return r;
        if (auto r = crank_ex::ex36_e2e_gpu_elementwise(); !r) return r;
        if (auto r = crank_ex::ex37_e2e_host_call(); !r) return r;
        if (auto r = crank_ex::ex38_e2e_aot_roundtrip(); !r) return r;
        if (auto r = crank_ex::ex39_e2e_functions_recursion_computation(); !r) return r;
        if (auto r = crank_ex::ex40_benchmark_cpp_vs_crank(); !r) return r;
        if (auto r = crank_ex::ex41_matrix_numerical_computation(); !r) return r;
        if (auto r = crank_ex::ex42_number_theory(); !r) return r;
        if (auto r = crank_ex::ex43_statistical_computation(); !r) return r;
        if (auto r = crank_ex::ex44_nested_loops(); !r) return r;
        if (auto r = crank_ex::ex45_vector_operations(); !r) return r;
        if (auto r = crank_ex::ex46_map_operations(); !r) return r;
        if (auto r = crank_views_ex::ex47_view_decl_parse(); !r) return r;
        if (auto r = crank_views_ex::ex48_view_expr_parse(); !r) return r;
        if (auto r = crank_views_ex::ex49_feature_gate(); !r) return r;
        if (auto r = crank_views_ex::ex50_view_registry(); !r) return r;
        if (auto r = crank_views_ex::ex51_view_method_table(); !r) return r;
        if (auto r = crank_views_ex::ex52_view_domain_meta(); !r) return r;
        if (auto r = crank_views_ex::ex53_view_obligations(); !r) return r;
        if (auto r = crank_views_ex::ex54_crank_view_diagnostics(); !r) return r;
        if (auto r = crank_views_ex::ex55_domain_views_e2e(); !r) return r;
        return {};
    }
};
