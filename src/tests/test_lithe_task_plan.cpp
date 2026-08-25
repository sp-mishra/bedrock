#include "catch_amalgamated.hpp"

#include "lithe/lithe_codegen_pipeline.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// task_decomposition_plan — POD contract, ABI stability, extraction
// ─────────────────────────────────────────────────────────────────────────────

using namespace lithe::codegen;
using namespace lithe::codegen::hl;

// ─────────────────────────────────────────────────────────────────────────────
// 1. Static ABI contract
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"task_decomposition_plan: is trivially copyable"
,
"[lithe][task_plan][abi]"
)
 {
    static_assert(std::is_trivially_copyable_v<task_decomposition_plan>);
    SUCCEED();
}

TEST_CASE (


"task_decomposition_plan: is standard layout"
,
"[lithe][task_plan][abi]"
)
 {
    static_assert(std::is_standard_layout_v<task_decomposition_plan>);
    SUCCEED();
}

TEST_CASE (


"loop_range: is trivially copyable"
,
"[lithe][task_plan][abi]"
)
 {
    static_assert(std::is_trivially_copyable_v<loop_range>);
    SUCCEED();
}

TEST_CASE (


"task_decomposition_plan: max_rank == 8"
,
"[lithe][task_plan][abi]"
)
 {
    static_assert(task_decomposition_plan::max_rank == 8);
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Zero-state defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"task_decomposition_plan: default-constructed has zero rank"
,
"[lithe][task_plan]"
)
 {
    task_decomposition_plan plan{};
    REQUIRE(plan.rank  == 0u);
    REQUIRE(plan.chunk == 1u);
    REQUIRE(plan.kernel    == nullptr);
    REQUIRE(plan.user_data == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. task_plan_extraction_pass — rank-1 parallel loop
// ─────────────────────────────────────────────────────────────────────────────

namespace {
    hl_mir_function make_parallel_loop(std::uint8_t rank = 1,
                                       int lower = 0, int upper = 256, int step = 1) {
        hl_mir_function fn{1u << 20};
        fn.name = "parallel_kernel";

        auto* blk = fn.make_block();
        fn.body_region.blocks.push_back(blk);
        blk->parent_region = &fn.body_region;

        auto* for_op = fn.make_op(hl_opcode::structured_for);
        structured_for_attr sf;
        sf.rank = rank;
        sf.is_parallel = true;
        for (std::uint8_t d = 0; d < rank; ++d)
            sf.bounds[d] = {lower, upper, step, true, true, true};
        for_op->attr = sf;

        auto* body = fn.make_region();
        auto rspan = fn.alloc_span<hl_region*>(1);
        rspan[0] = body;
        for_op->regions = rspan;
        body->parent_op = for_op;
        blk->ops.push_back(for_op);
        return fn;
    }
} // anonymous namespace

TEST_CASE (


"task_plan_extraction_pass: detects rank-1 parallel loop"
,
"[lithe][task_plan]"
)
 {
    auto fn = make_parallel_loop(1, 0, 256, 1);

    task_plan_extraction_pass pass;
    const auto result = pass.run(fn);

    REQUIRE(result.plans.size() == 1u);
    const auto& plan = result.plans[0];
    REQUIRE(plan.rank == 1u);
    REQUIRE(plan.bounds[0].start == 0);
    REQUIRE(plan.bounds[0].end   == 256);
    REQUIRE(plan.bounds[0].step  == 1);
}

TEST_CASE (


"task_plan_extraction_pass: rank-2 parallel loop"
,
"[lithe][task_plan]"
)
 {
    auto fn = make_parallel_loop(2, 0, 64, 2);

    task_plan_extraction_pass pass;
    const auto result = pass.run(fn);

    REQUIRE(result.plans.size() == 1u);
    const auto& plan = result.plans[0];
    REQUIRE(plan.rank == 2u);
    REQUIRE(plan.bounds[0].start == 0);
    REQUIRE(plan.bounds[0].end   == 64);
    REQUIRE(plan.bounds[0].step  == 2);
    REQUIRE(plan.bounds[1].start == 0);
    REQUIRE(plan.bounds[1].end   == 64);
    REQUIRE(plan.bounds[1].step  == 2);
}

TEST_CASE (


"task_plan_extraction_pass: non-parallel loop produces no plan"
,
"[lithe][task_plan]"
)
 {
    hl_mir_function fn{1u << 20};
    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto* for_op = fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank        = 1;
    sf.is_parallel = false; // NOT parallel
    sf.bounds[0]   = { 0, 128, 1, true, true, true };
    for_op->attr   = sf;

    auto* body = fn.make_region();
    auto rspan = fn.alloc_span<hl_region*>(1);
    rspan[0] = body;
    for_op->regions = rspan;
    blk->ops.push_back(for_op);

    task_plan_extraction_pass pass;
    const auto result = pass.run(fn);
    REQUIRE(result.plans.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. kernel pointer round-trip
// ─────────────────────────────────────────────────────────────────────────────

static void test_kernel(void*, std::size_t, std::size_t) noexcept {}

TEST_CASE (


"task_plan_extraction_pass: default_kernel propagated"
,
"[lithe][task_plan]"
)
 {
    auto fn = make_parallel_loop();

    task_plan_extraction_pass pass;
    pass.default_kernel = test_kernel;
    const auto result = pass.run(fn);

    REQUIRE(result.plans.size() == 1u);
    REQUIRE(result.plans[0].kernel == test_kernel);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Multiple parallel loops in one function
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"task_plan_extraction_pass: two parallel loops → two plans"
,
"[lithe][task_plan]"
)
 {
    hl_mir_function fn{1u << 20};
    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto make_for = [&](int upper) {
        auto* op = fn.make_op(hl_opcode::structured_for);
        structured_for_attr sf;
        sf.rank = 1;
        sf.is_parallel = true;
        sf.bounds[0] = { 0, upper, 1, true, true, true };
        op->attr = sf;
        auto* body = fn.make_region();
        auto rspan = fn.alloc_span<hl_region*>(1);
        rspan[0] = body;
        op->regions = rspan;
        blk->ops.push_back(op);
    };

    make_for(128);
    make_for(512);

    task_plan_extraction_pass pass;
    const auto result = pass.run(fn);

    REQUIRE(result.plans.size() == 2u);
    REQUIRE(result.plans[0].bounds[0].end == 128);
    REQUIRE(result.plans[1].bounds[0].end == 512);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. loop_range field semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"loop_range: fields populate correctly"
,
"[lithe][task_plan][loop_range]"
)
 {
    loop_range r{4, 256, 8};
    REQUIRE(r.start == 4);
    REQUIRE(r.end   == 256);
    REQUIRE(r.step  == 8);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Zero Pravaha/Sutra headers in scope (no circular dep)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"task_decomposition_plan: no Pravaha types required to compile"
,
"[lithe][task_plan][isolation]"
)
 {
    // This test compiles successfully iff task_decomposition_plan pulls in
    // no Pravaha/Sutra headers.  The very fact this TU compiles is the test.
    task_decomposition_plan plan{};
    (void)plan;
    SUCCEED();
}
