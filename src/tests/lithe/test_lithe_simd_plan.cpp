#include "catch_amalgamated.hpp"

#include "lithe/backends/lithe_codegen_simd.hpp"

using namespace lithe::codegen;

TEST_CASE(
    "bind_vector_plan accepts a proven f32 elementwise plan",
    "[lithe][simd][vector-plan]"
) {
    hl::vector_plan plan;
    plan.lanes = 8;
    plan.element_bits = 32;
    plan.tail = hl::vector_tail_strategy::scalar_epilogue;
    plan.reduction = hl::vector_reduction_shape::none;
    plan.legality = hl::vector_plan_legality::proven;
    plan.schedule_materialized = true;

    const auto binding = backends::bind_vector_plan(plan);
    REQUIRE(binding.accepted());
    REQUIRE(binding.native_lanes > 0);
}

TEST_CASE(
    "bind_vector_plan retains scalar fallback for an unproven plan",
    "[lithe][simd][vector-plan]"
) {
    hl::vector_plan plan;
    plan.lanes = 8;
    plan.element_bits = 32;
    plan.tail = hl::vector_tail_strategy::scalar_epilogue;

    const auto binding = backends::bind_vector_plan(plan);
    REQUIRE_FALSE(binding.accepted());
}
