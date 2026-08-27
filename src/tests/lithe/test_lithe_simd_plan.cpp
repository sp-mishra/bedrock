#include "catch_amalgamated.hpp"

#include <array>
#include <cstddef>

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

TEST_CASE(
    "planned SIMD lowering executes typed add and preserves scalar fallback",
    "[lithe][simd][vector-plan]"
) {
    hl::vector_plan plan;
    plan.lanes = 8;
    plan.element_bits = 32;
    plan.tail = hl::vector_tail_strategy::scalar_epilogue;
    plan.reduction = hl::vector_reduction_shape::none;
    plan.legality = hl::vector_plan_legality::proven;
    plan.schedule_materialized = true;
    const auto lowering = backends::lower_vector_plan_for_simd(
        plan, backends::simd_binary_operation::add);
    const std::array<float, 5> lhs{1, 2, 3, 4, 5};
    const std::array<float, 5> rhs{6, 7, 8, 9, 10};
    std::array<float, 5> output{};
    bool fallback_called = false;

    const auto path = backends::execute_simd_binary(
        lowering, std::span{lhs}, std::span{rhs}, std::span{output},
        [&fallback_called](const auto left, const auto right, auto result) {
            fallback_called = true;
            for (std::size_t index = 0; index < result.size(); ++index)
                result[index] = left[index] + right[index];
        });

    CHECK(path == backends::simd_execution_path::vectorized);
    CHECK_FALSE(fallback_called);
    CHECK(output == std::array<float, 5>{7, 9, 11, 13, 15});

    plan.legality = hl::vector_plan_legality::unknown;
    const auto rejected = backends::lower_vector_plan_for_simd(
        plan, backends::simd_binary_operation::multiply);
    fallback_called = false;
    const auto fallback_path = backends::execute_simd_binary(
        rejected, std::span{lhs}, std::span{rhs}, std::span{output},
        [&fallback_called](const auto, const auto, auto) { fallback_called = true; });
    CHECK(fallback_path == backends::simd_execution_path::scalar_fallback);
    CHECK(fallback_called);
}
