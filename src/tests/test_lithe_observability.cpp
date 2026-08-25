// =============================================================================
// test_lithe_observability.cpp — §3.3 pass_event egraph/rewrite telemetry fields
//
// Verifies:
//   • pass_event carries rule_fired, iterations, nodes_before/after, pass_cost_ns,
//     egraph_enodes, egraph_eclasses (zero-default).
//   • compile_observed<true>() populates pass_cost_ns in the collected events.
//   • Unused telemetry fields default to 0.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_passes.hpp"

// ============================================================================
// Verify zero-default new telemetry fields
// ============================================================================

TEST_CASE (


"pass_event: new telemetry fields zero-default"
,
"[observability][pass_event]"
)
 {
    lithe::compiler::observability::pass_event ev;
    REQUIRE(ev.rule_fired.empty());
    REQUIRE(ev.iterations == 0);
    REQUIRE(ev.nodes_before == 0);
    REQUIRE(ev.nodes_after == 0);
    REQUIRE(ev.pass_cost_ns == 0);
    REQUIRE(ev.egraph_enodes == 0);
    REQUIRE(ev.egraph_eclasses == 0);
}

// ============================================================================
// Verify manual assignment round-trips all new fields
// ============================================================================

TEST_CASE (


"pass_event: telemetry fields assignable and readable"
,
"[observability]"
)
 {
    lithe::compiler::observability::pass_event ev;
    ev.rule_fired     = "commutativity";
    ev.iterations     = 5;
    ev.nodes_before   = 12;
    ev.nodes_after    = 8;
    ev.pass_cost_ns   = 1234;
    ev.egraph_enodes  = 100;
    ev.egraph_eclasses = 50;

    REQUIRE(ev.rule_fired     == "commutativity");
    REQUIRE(ev.iterations     == 5);
    REQUIRE(ev.nodes_before   == 12);
    REQUIRE(ev.nodes_after    == 8);
    REQUIRE(ev.pass_cost_ns   == 1234);
    REQUIRE(ev.egraph_enodes  == 100);
    REQUIRE(ev.egraph_eclasses == 50);
}

// ============================================================================
// Verify compile_observed<true> populates pass_cost_ns
// ============================================================================

TEST_CASE (


"compile_observed: pass_cost_ns populated"
,
"[observability][trace]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(0, 5);

    lithe::compiler::observability::trace_observer obs;
    lithe::compiler::pass_context ctx;

    lithe::compiler::compile_observed<true>(
        expr, ctx, obs,
        lithe::passes::simplify_add_zero_pass{});

    REQUIRE(!obs.trace.pass_events.empty());
    const auto& ev = obs.trace.pass_events.front();

    // pass_cost_ns == end_ns - start_ns (non-negative).
    const auto computed = (ev.end_ns >= ev.start_ns)
                        ? (ev.end_ns - ev.start_ns)
                        : std::uint64_t{0};
    REQUIRE(ev.pass_cost_ns == computed);

    // Classic pass: iterations == 1.
    REQUIRE(ev.iterations == 1);

    // Classic pass: egraph fields remain 0.
    REQUIRE(ev.egraph_enodes == 0);
    REQUIRE(ev.egraph_eclasses == 0);
}

// ============================================================================
// Verify compile_trace stores events with new telemetry fields intact
// ============================================================================

TEST_CASE (


"compile_trace: stores events with new fields"
,
"[observability]"
)
 {
    lithe::compiler::observability::compile_trace t;

    lithe::compiler::observability::pass_event ev1;
    ev1.pass_name    = "fold";
    ev1.pass_cost_ns = 100;
    ev1.iterations   = 1;

    lithe::compiler::observability::pass_event ev2;
    ev2.pass_name       = "egraph_optimize";
    ev2.iterations      = 7;
    ev2.egraph_enodes   = 42;
    ev2.egraph_eclasses = 15;

    t.pass_events.push_back(ev1);
    t.pass_events.push_back(ev2);

    REQUIRE(t.pass_events.size() == 2);
    REQUIRE(t.pass_events[0].pass_cost_ns == 100);
    REQUIRE(t.pass_events[1].egraph_enodes == 42);
    REQUIRE(t.pass_events[1].iterations == 7);
}
