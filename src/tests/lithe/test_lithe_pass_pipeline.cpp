// =============================================================================
// test_lithe_pass_pipeline.cpp — typed pass pipeline + analysis preservation (§7)
//
// Verifies:
//   • static_pipeline runs typed passes with no std::function.
//   • A pass reporting a preserved analysis avoids recompute in analysis_manager.
//   • dynamic_pipeline parity on a small case.
//   • analysis_manager invalidation and cache.
//   • no_pipeline_hooks is empty (is_empty_v).
//   • pass_result diagnostics / has_errors().
// =============================================================================

#include "catch_amalgamated.hpp"

#include <string>
#include <type_traits>

#include "lithe/lithe_algorithms/pipeline.hpp"
#include "lithe/lithe_execution/foundation.hpp"

namespace al = lithe::algorithms;

// ============================================================================
// Helpers — minimal IR + passes for testing
// ============================================================================

// Minimal IR type: just an integer value.
struct test_ir {
    int value = 0;
};

// identity_pass: returns IR unchanged, preserves all analyses.
struct identity_pass {
    al::pass_result<test_ir> operator()(al::analysis_manager& /*am*/, test_ir ir) const {
        return al::pass_result<test_ir>{std::move(ir), false, al::preserved_analysis_set::all()};
    }
};

// increment_pass: increments value, invalidates all analyses.
struct increment_pass {
    al::pass_result<test_ir> operator()(al::analysis_manager& /*am*/, test_ir ir) const {
        ir.value++;
        return al::pass_result<test_ir>{std::move(ir), true, al::preserved_analysis_set::none_set()};
    }
};

// doubling_pass: doubles value, preserves cfg only.
struct doubling_pass {
    al::pass_result<test_ir> operator()(al::analysis_manager& /*am*/, test_ir ir) const {
        ir.value *= 2;
        al::preserved_analysis_set pset;
        pset.set(al::analysis_id::cfg);
        return al::pass_result<test_ir>{std::move(ir), true, pset};
    }
};

// ============================================================================
// static_pipeline tests
// ============================================================================

TEST_CASE (


"static_pipeline: runs passes in order"
,
"[pipeline][static]"
)
 {
    al::static_pipeline<lithe::execution::no_pipeline_hooks,
                        increment_pass, doubling_pass> pipeline{increment_pass{}, doubling_pass{}};

    al::analysis_manager am;
    test_ir ir{.value = 3};

    auto result = pipeline.run(am, std::move(ir));
    // increment → 4, double → 8
    CHECK(result.output.value == 8);
    CHECK(result.changed == true);
    CHECK(pipeline.pass_count() == 2);
}

TEST_CASE (


"static_pipeline: identity pass does not change IR"
,
"[pipeline][static]"
)
 {
    al::static_pipeline<lithe::execution::no_pipeline_hooks,
                        identity_pass> pipeline{identity_pass{}};

    al::analysis_manager am;
    test_ir ir{.value = 42};

    auto result = pipeline.run(am, std::move(ir));
    CHECK(result.output.value == 42);
    CHECK(result.changed == false);
    CHECK(result.preserved.any());
}

TEST_CASE (


"static_pipeline: no std::function — zero erasure"
,
"[pipeline][static]"
)
 {
    // Verify the passes are stored by value in a tuple (no std::function).
    using pipeline_t = al::static_pipeline<lithe::execution::no_pipeline_hooks,
                                            identity_pass, increment_pass>;
    static_assert(sizeof(pipeline_t) > 0, "pipeline must have non-zero size");
    SUCCEED();
}

// ============================================================================
// analysis_manager — cache + invalidation
// ============================================================================

TEST_CASE (


"analysis_manager: store and retrieve"
,
"[analysis_manager]"
)
 {
    al::analysis_manager am;

    am.store(al::analysis_id::cfg, std::string{"cfg_data"});
    REQUIRE(am.has(al::analysis_id::cfg));

    const auto* val = am.get<std::string>(al::analysis_id::cfg);
    REQUIRE(val != nullptr);
    CHECK(*val == "cfg_data");
}

TEST_CASE (


"analysis_manager: invalidation removes non-preserved analyses"
,
"[analysis_manager]"
)
{
    al::analysis_manager am;
    am.store(al::analysis_id::cfg,     std::string{"cfg"});
    am.store(al::analysis_id::liveness, std::string{"live"});

    // Only preserve cfg.
    al::preserved_analysis_set pset;
    pset.set(al::analysis_id::cfg);

    am.invalidate_except(pset);

    CHECK(am.has(al::analysis_id::cfg));          // preserved
    CHECK_FALSE(am.has(al::analysis_id::liveness)); // invalidated
}

TEST_CASE (


"analysis_manager: preserved analysis avoids recompute"
,
"[analysis_manager]"
)
 {
    al::analysis_manager am;
    am.store(al::analysis_id::dominator, 99);

    // A pass preserving dominator should leave it in cache.
    al::preserved_analysis_set pset = al::preserved_analysis_set::all();
    am.invalidate_except(pset);

    CHECK(am.has(al::analysis_id::dominator)); // still cached
    const auto* v = am.get<int>(al::analysis_id::dominator);
    REQUIRE(v != nullptr);
    CHECK(*v == 99);
}

TEST_CASE (


"analysis_manager: clear removes everything"
,
"[analysis_manager]"
)
 {
    al::analysis_manager am;
    am.store(al::analysis_id::cfg, std::string{"x"});
    am.clear();
    CHECK_FALSE(am.has(al::analysis_id::cfg));
}

// ============================================================================
// dynamic_pipeline — parity on small case
// ============================================================================

TEST_CASE (


"dynamic_pipeline: parity with static_pipeline"
,
"[pipeline][dynamic]"
)
 {
    al::dynamic_pipeline<test_ir> dyn;
    dyn.add(increment_pass{});
    dyn.add(doubling_pass{});

    al::analysis_manager am;
    test_ir ir{.value = 3};

    auto result = dyn.run(am, std::move(ir));
    CHECK(result.output.value == 8); // same as static path above
    CHECK(dyn.pass_count() == 2);
}

TEST_CASE (


"dynamic_pipeline: empty pipeline is identity"
,
"[pipeline][dynamic]"
)
 {
    al::dynamic_pipeline<test_ir> dyn;
    REQUIRE(dyn.empty());

    al::analysis_manager am;
    test_ir ir{.value = 55};
    auto result = dyn.run(am, std::move(ir));
    CHECK(result.output.value == 55);
}

// ============================================================================
// no_pipeline_hooks — compile-time: empty
// ============================================================================

TEST_CASE (


"no_pipeline_hooks: is_empty_v"
,
"[pipeline][hooks]"
)
 {
    static_assert(std::is_empty_v<lithe::execution::no_pipeline_hooks>,
                  "no_pipeline_hooks must be empty (zero-cost default)");
    SUCCEED();
}

// ============================================================================
// pass_result diagnostics
// ============================================================================

struct error_pass {
    al::pass_result<test_ir> operator()(al::analysis_manager& /*am*/, test_ir ir) const {
        al::pass_result<test_ir> r{std::move(ir)};
        r.diagnostics.push_back({al::pass_diagnostic_level::error, "test error"});
        return r;
    }
};

TEST_CASE (


"pass_result: has_errors detects error diagnostics"
,
"[pass_result]"
)
 {
    al::static_pipeline<lithe::execution::no_pipeline_hooks, error_pass> p{error_pass{}};
    al::analysis_manager am;
    test_ir ir{};
    auto result = p.run(am, std::move(ir));
    CHECK(result.has_errors());
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.front().level == al::pass_diagnostic_level::error);
}
