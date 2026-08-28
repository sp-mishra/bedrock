#include "catch_amalgamated.hpp"

#include "lithe/lithe.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"

// ---------------------------------------------------------------------------
// Prompt 15 test 1 — instrumentation_trace records events
// ---------------------------------------------------------------------------
TEST_CASE (



"instrumentation_trace records compile and pass events"
,
"[lithe][instrumentation][trace]"
)
 {
    using namespace lithe::codegen;

    instrumentation_trace trace;

    instrumentation_event compile_begin_ev;
    compile_begin_ev.kind = instrumentation_event_kind::compile_begin;
    compile_begin_ev.pass_or_stage_name = "compile";
    trace.add(compile_begin_ev);

    instrumentation_event pass_begin_ev;
    pass_begin_ev.kind = instrumentation_event_kind::pass_begin;
    pass_begin_ev.pass_or_stage_name = "dead_def_elimination";
    trace.add(pass_begin_ev);

    instrumentation_event pass_end_ev;
    pass_end_ev.kind = instrumentation_event_kind::pass_end;
    pass_end_ev.pass_or_stage_name = "dead_def_elimination";
    trace.add(pass_end_ev);

    instrumentation_event compile_end_ev;
    compile_end_ev.kind = instrumentation_event_kind::compile_end;
    compile_end_ev.pass_or_stage_name = "compile";
    trace.add(compile_end_ev);

    REQUIRE(trace.events.size() == 4);
    CHECK(trace.events[0].kind == instrumentation_event_kind::compile_begin);
    CHECK(trace.events[1].kind == instrumentation_event_kind::pass_begin);
    CHECK(trace.events[1].pass_or_stage_name == "dead_def_elimination");
    CHECK(trace.events[2].kind == instrumentation_event_kind::pass_end);
    CHECK(trace.events[3].kind == instrumentation_event_kind::compile_end);
}

// ---------------------------------------------------------------------------
// Prompt 15 test 2 — build_feedback_profile creates hot block counts
// ---------------------------------------------------------------------------
TEST_CASE (



"build_feedback_profile creates hot block counts from block_executed events"
,
"[lithe][instrumentation][profile]"
)
 {
    using namespace lithe::codegen;

    instrumentation_trace trace;

    // Block 1 executes 100 times — hot.
    for (int i = 0; i < 100; ++i) {
        instrumentation_event ev;
        ev.kind     = instrumentation_event_kind::block_executed;
        ev.block_id = 1;
        ev.value    = 1;
        trace.add(ev);
    }

    // Block 2 executes 2 times — cold (< 10% of max).
    for (int i = 0; i < 2; ++i) {
        instrumentation_event ev;
        ev.kind     = instrumentation_event_kind::block_executed;
        ev.block_id = 2;
        ev.value    = 1;
        trace.add(ev);
    }

    const auto profile = build_feedback_profile(trace);

    REQUIRE(profile.blocks.size() == 2);

    const block_profile *hot_block  = nullptr;
    const block_profile *cold_block = nullptr;
    for (const auto &b : profile.blocks) {
        if (b.block_id == 1) hot_block  = &b;
        if (b.block_id == 2) cold_block = &b;
    }

    REQUIRE(hot_block  != nullptr);
    REQUIRE(cold_block != nullptr);

    CHECK(hot_block->execution_count  == 100);
    CHECK(hot_block->is_hot           == true);
    CHECK(cold_block->execution_count == 2);
    CHECK(cold_block->is_hot          == false);

    const auto hot_ids = profile.hot_blocks();
    REQUIRE(hot_ids.size() == 1);
    CHECK(hot_ids[0] == 1);
}

// ---------------------------------------------------------------------------
// Prompt 15 test 3 — merge_feedback_profiles combines counts
// ---------------------------------------------------------------------------
TEST_CASE (



"merge_feedback_profiles combines block execution counts"
,
"[lithe][instrumentation][merge]"
)
 {
    using namespace lithe::codegen;

    // Profile A: block 1 = 50, block 2 = 10
    instrumentation_trace trace_a;
    for (int i = 0; i < 50; ++i) {
        instrumentation_event ev;
        ev.kind = instrumentation_event_kind::block_executed;
        ev.block_id = 1; ev.value = 1;
        trace_a.add(ev);
    }
    for (int i = 0; i < 10; ++i) {
        instrumentation_event ev;
        ev.kind = instrumentation_event_kind::block_executed;
        ev.block_id = 2; ev.value = 1;
        trace_a.add(ev);
    }

    // Profile B: block 1 = 30, block 3 = 20 (new block)
    instrumentation_trace trace_b;
    for (int i = 0; i < 30; ++i) {
        instrumentation_event ev;
        ev.kind = instrumentation_event_kind::block_executed;
        ev.block_id = 1; ev.value = 1;
        trace_b.add(ev);
    }
    for (int i = 0; i < 20; ++i) {
        instrumentation_event ev;
        ev.kind = instrumentation_event_kind::block_executed;
        ev.block_id = 3; ev.value = 1;
        trace_b.add(ev);
    }

    const auto pa = build_feedback_profile(trace_a);
    const auto pb = build_feedback_profile(trace_b);
    const auto merged = merge_feedback_profiles(pa, pb);

    // Merged must have blocks 1, 2, 3.
    REQUIRE(merged.blocks.size() == 3);

    std::uint64_t count1 = 0, count2 = 0, count3 = 0;
    for (const auto &b : merged.blocks) {
        if (b.block_id == 1) count1 = b.execution_count;
        if (b.block_id == 2) count2 = b.execution_count;
        if (b.block_id == 3) count3 = b.execution_count;
    }

    CHECK(count1 == 80);
    CHECK(count2 == 10);
    CHECK(count3 == 20);
}

// ---------------------------------------------------------------------------
// Prompt 15 test 4 — feedback_compile with one iteration returns a result
// ---------------------------------------------------------------------------
TEST_CASE (



"feedback_compile with one iteration returns a valid result"
,
"[lithe][feedback][compile]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    feedback_compile_options opts;
    opts.max_iterations = 1;

    const auto result = feedback_compile(expr, std::move(opts));

    REQUIRE(result.ok());
    CHECK(result.iterations_run() == 1);
    CHECK(result.best_iteration  == 0);
    CHECK(result.history.size()  == 1);
    CHECK(result.history[0].iteration == 0);
    // The single compile must have produced a valid physical MIR.
    CHECK(result.best_result.ok());
    CHECK_FALSE(result.best_result.physical_mir.function.blocks.empty());
}

// ---------------------------------------------------------------------------
// Prompt 15 test 5 — feedback_compile without ML advisor uses deterministic baseline
// ---------------------------------------------------------------------------
TEST_CASE (



"feedback_compile without ML advisor uses deterministic baseline"
,
"[lithe][feedback][no_advisor]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(3, 4);

    // Two iterations, no advisor, no trace_fn, no score_fn.
    feedback_compile_options opts;
    opts.max_iterations = 2;
    // advisor left default (empty — no ML)

    const auto result = feedback_compile(expr, std::move(opts));

    REQUIRE(result.ok());
    CHECK(result.iterations_run() == 2);

    // Without a scorer every iteration scores 0.0 — the last is kept as best.
    CHECK(result.best_score == 0.0);

    // Both iterations must have compiled successfully.
    for (const auto &rec : result.history) {
        CHECK(rec.compile_result.ok());
    }

    // Without an advisor, decisions must be empty (no pipeline changes requested).
    for (const auto &rec : result.history) {
        CHECK(rec.decision.pass_order.empty());
        CHECK(rec.decision.enable_passes.empty());
        CHECK(rec.decision.disable_passes.empty());
        CHECK_FALSE(rec.decision.opt_level_override.has_value());
    }
}

// ---------------------------------------------------------------------------
// feedback_compile with score_fn selects the best-scoring iteration
// ---------------------------------------------------------------------------
TEST_CASE (



"feedback_compile score_fn selects best iteration"
,
"[lithe][feedback][scoring]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    int call_count = 0;
    feedback_compile_options opts;
    opts.max_iterations = 3;
    opts.score_fn = [&](const codegen_result &, const optimization_feedback_profile &) -> double {
        // Return increasing scores: 1.0, 2.0, 3.0
        return static_cast<double>(++call_count);
    };

    const auto result = feedback_compile(expr, std::move(opts));

    REQUIRE(result.ok());
    CHECK(result.iterations_run() == 3);
    // Last iteration scores highest.
    CHECK(result.best_score     == 3.0);
    CHECK(result.best_iteration == 2);
}

// ---------------------------------------------------------------------------
// feedback_compile on_iteration callback fires once per iteration
// ---------------------------------------------------------------------------
TEST_CASE (



"feedback_compile on_iteration callback fires per iteration"
,
"[lithe][feedback][callback]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    std::vector<std::uint32_t> seen_iterations;
    feedback_compile_options opts;
    opts.max_iterations = 3;
    opts.on_iteration = [&](const feedback_iteration_record &rec) {
        seen_iterations.push_back(rec.iteration);
    };

    const auto result = feedback_compile(expr, std::move(opts));

    REQUIRE(result.ok());
    REQUIRE(seen_iterations.size() == 3);
    CHECK(seen_iterations[0] == 0);
    CHECK(seen_iterations[1] == 1);
    CHECK(seen_iterations[2] == 2);
}
