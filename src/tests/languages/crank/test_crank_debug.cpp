// =============================================================================
// test_crank_debug.cpp — Debugger + introspection facilities (debug_info / debug).
//
// Covers:
//   1. build_debug_info: module root + function scopes + globals/locals.
//   2. append_line_table: controlled exit edges become stmt-boundary rows.
//   3. resolve_breakpoint: snaps a requested line to nearest stmt boundary.
//   4. dump_* serializers emit non-empty (parseable) JSON.
//   5. New stats fields populate (execute_stats.branch_count for a loop fn;
//      hl_lowering_stats.block_count / max_loop_nest).
//   6. debug_hooks: no-op path + a captured std::function sink receives an event.
//   7. emit_debug_pulse compiles with/without NADI (compile-time guard).
//   8. dump_pipeline_stats bundles present stages.
//
// New file — does not modify existing tests.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/debug_info.hpp"
#include "languages/crank/debug.hpp"
#include "languages/crank/resolve.hpp"
#include "languages/crank/lower_hl.hpp"
#include "languages/crank/execute.hpp"

// ============================================================================
// Test 1 — build_debug_info: scopes + variables from the symbol table
// ============================================================================

TEST_CASE (

"build_debug_info builds module root + function scopes"
,
"[crank][debug]"
)
 {
    crank::resolver res("mymod");
    res.declare_function("Compute", /*return_type_annotated=*/true,
                         /*params_typed=*/true, /*return_type_id=*/1);
    res.declare_value("acc", crank::mutability_kind::mutable_,
                      /*type_id=*/2, /*initialized=*/true);
    auto rr = res.take();

    auto di = crank::build_debug_info(rr, "mymod");

    REQUIRE(di.module_name == "mymod");
    REQUIRE_FALSE(di.scopes.empty());
    CHECK(di.scopes[0].kind == crank::debug_scope_kind::module);
    CHECK(di.scopes[0].name == "mymod");

    bool has_fn = false;
    for (const auto& s : di.scopes)
        if (s.kind == crank::debug_scope_kind::function && s.name == "Compute")
            has_fn = true;
    CHECK(has_fn);

    // "acc" declared at module depth 0 → global.
    bool has_global = false;
    for (const auto& g : di.globals)
        if (g.name == "acc") { has_global = true; CHECK(g.is_mutable); }
    CHECK(has_global);
}

// ============================================================================
// Test 2 — append_line_table: controlled exits → stmt-boundary rows
// ============================================================================

TEST_CASE (

"append_line_table folds exit edges into stmt-boundary rows"
,
"[crank][debug]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "loopy";
    inp.loops.push_back({ .lower = 0, .upper = 8, .step = 1,
                          .is_parallel = false, .name = "i" });
    inp.exit_edges.push_back({ .kind = crank::exit_edge_kind::controlled,
                               .target = "ret",
                               .at = crank::source_span{.offset=10,.length=4,.line=3,.col=1} });
    inp.exit_edges.push_back({ .kind = crank::exit_edge_kind::trap,
                               .target = "trap",
                               .at = crank::source_span{.offset=40,.length=4,.line=7,.col=1} });
    auto lr = crank::lower_to_hl(std::move(inp));
    REQUIRE(lr.ok());

    crank::debug_info di;
    di.module_name = "m";
    crank::append_line_table(di, lr);

    REQUIRE(di.line_table.size() == 2);
    // Rows sorted by line; the controlled edge (line 3) is a stmt boundary.
    CHECK(di.line_table[0].src.line == 3);
    CHECK(di.line_table[0].is_stmt_boundary);
    CHECK(di.line_table[0].fn_name == "loopy");
    // The trap edge (line 7) is not a snap target.
    CHECK(di.line_table[1].src.line == 7);
    CHECK_FALSE(di.line_table[1].is_stmt_boundary);
}

// ============================================================================
// Test 3 — resolve_breakpoint snaps to nearest stmt boundary
// ============================================================================

TEST_CASE (

"resolve_breakpoint snaps a requested line to nearest stmt"
,
"[crank][debug]"
)
 {
    crank::debug_info di;
    di.line_table.push_back({ .src = {.line=5,.col=1}, .fn_name="f",
                              .is_stmt_boundary=true });
    di.line_table.push_back({ .src = {.line=9,.col=1}, .fn_name="f",
                              .is_stmt_boundary=true });

    auto bp = crank::resolve_breakpoint(di, /*line=*/6);
    REQUIRE(bp.has_value());
    CHECK(bp->verified);
    CHECK(bp->resolved.line == 9);   // nearest boundary at or after 6
    CHECK(bp->fn_name == "f");

    // A line past the last boundary is unverified.
    auto miss = crank::resolve_breakpoint(di, /*line=*/20);
    REQUIRE(miss.has_value());
    CHECK_FALSE(miss->verified);
}

// ============================================================================
// Test 4 — dump_* serializers emit non-empty JSON
// ============================================================================

TEST_CASE (

"debug dump_* serializers produce non-empty JSON"
,
"[crank][debug][dump]"
)
 {
    crank::resolver res("m");
    res.declare_function("Foo", true, true, 1);
    auto rr = res.take();
    auto di = crank::build_debug_info(rr, "m");

    CHECK(crank::dump_debug_info(di).find("\"module\"") != std::string::npos);
    CHECK(crank::dump_scopes(di).front() == '[');
    CHECK(crank::dump_line_table(di).front() == '[');

    crank::breakpoint_location bp;
    bp.line = 3; bp.verified = true; bp.resolved = {.line=3,.col=1};
    std::vector<crank::breakpoint_location> bps{bp};
    CHECK(crank::dump_breakpoints(bps).find("\"verified\":true") != std::string::npos);

    crank::debug_event ev;
    ev.kind = crank::debug_event_kind::breakpoint_hit;
    ev.at = {.line=3,.col=1};
    ev.detail = "hit";
    CHECK(crank::dump_debug_event(ev).find("breakpoint_hit") != std::string::npos);
}

// ============================================================================
// Test 5 — new stats fields populate
// ============================================================================

TEST_CASE (

"execute_stats.branch_count > 0 for a loop function"
,
"[crank][debug][stats]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "loop_fn";
    inp.loops.push_back({ .lower = 0, .upper = 4, .step = 1,
                          .is_parallel = false, .name = "i" });
    auto lr = crank::lower_to_hl(std::move(inp));
    REQUIRE(lr.ok());
    // hl_lowering_stats new fields.
    CHECK(lr.stats.block_count >= 2);      // entry + loop body
    CHECK(lr.stats.max_loop_nest >= 1);

    auto xr = crank::execute_via_interpreter(lr, {}, {});
    // A loop lowers to CFG branches; interpreter skips but stats are populated.
    CHECK(xr.stats.branch_count > 0);
    CHECK(xr.stats.block_count > 0);
}

// ============================================================================
// Test 6 — debug_hooks: no-op default + captured std::function sink
// ============================================================================

TEST_CASE (

"debug_hooks no-op default and installed callback dispatch"
,
"[crank][debug][hooks]"
)
 {
    // Default (null) hooks: nothing installed, dispatch is a safe no-op.
    CHECK_FALSE(crank::null_debug_hooks.any_installed());
    crank::debug_event ev;
    ev.kind = crank::debug_event_kind::breakpoint_hit;
    crank::null_debug_hooks.dispatch(ev);   // must not crash

    // Installed callback receives the event.
    int hits = 0;
    crank::debug_hooks hooks;
    hooks.on_breakpoint = [&](const crank::debug_event&) { ++hits; };
    CHECK(hooks.any_installed());
    hooks.dispatch(ev);
    CHECK(hits == 1);

    // A concept-satisfying sink type compiles + receives on_event.
    struct CountingSink {
        int n = 0;
        void on_event(const crank::debug_event&) { ++n; }
    };
    static_assert(crank::DebugEventSink<CountingSink>);
    CountingSink sink;
    sink.on_event(ev);
    CHECK(sink.n == 1);
}

// ============================================================================
// Test 7 — emit_debug_pulse compiles regardless of NADI presence
// ============================================================================

TEST_CASE (

"emit_debug_pulse compiles (NADI guard)"
,
"[crank][debug][nadi]"
)
 {
    crank::debug_event ev;
    ev.kind = crank::debug_event_kind::trap;
    ev.at = {.line=1,.col=1};
    // Default sink is a no-op (NoSink when NADI present, ignored when absent).
    crank::emit_debug_pulse(ev);
    SUCCEED("emit_debug_pulse instantiated");
}

// ============================================================================
// Test 8 — dump_pipeline_stats bundles present stages
// ============================================================================

TEST_CASE (

"dump_pipeline_stats emits only present stages"
,
"[crank][debug][stats]"
)
 {
    crank::pipeline_stats_snapshot snap;
    crank::hl_lowering_stats l;
    l.block_count = 3;
    l.max_loop_nest = 1;
    snap.lower = l;

    auto js = crank::dump_pipeline_stats(snap);
    CHECK(js.find("\"lower\"") != std::string::npos);
    CHECK(js.find("\"block_count\":3") != std::string::npos);
    CHECK(js.find("\"parse\"") == std::string::npos);    // absent stage omitted
    CHECK(js.find("\"execute\"") == std::string::npos);
}
