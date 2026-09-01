#include "catch_amalgamated.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "lithe/backends/lithe_codegen_simd.hpp"

using namespace lithe::codegen;
using Catch::Approx;

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

// =============================================================================
// GAP-5: double and int32_t kernels
// =============================================================================

TEST_CASE(
    "simd_kernels::add works for double",
    "[lithe][simd][gap5][double]"
) {
    const std::array<double, 6> a{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    const std::array<double, 6> b{10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
    std::array<double, 6> out{};
    backends::simd_kernels::add(std::span{a}, std::span{b}, std::span{out});
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == Approx(a[i] + b[i]));
}

TEST_CASE(
    "simd_kernels::mul works for double",
    "[lithe][simd][gap5][double]"
) {
    const std::array<double, 4> a{1.5, 2.5, 3.5, 4.5};
    const std::array<double, 4> b{2.0, 4.0, 6.0, 8.0};
    std::array<double, 4> out{};
    backends::simd_kernels::mul(std::span{a}, std::span{b}, std::span{out});
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == Approx(a[i] * b[i]));
}

TEST_CASE(
    "simd_kernels::axpy works for double",
    "[lithe][simd][gap5][double]"
) {
    const double alpha = 3.0;
    const std::array<double, 4> x{1.0, 2.0, 3.0, 4.0};
    const std::array<double, 4> y{0.5, 0.5, 0.5, 0.5};
    std::array<double, 4> out{};
    backends::simd_kernels::axpy(alpha, std::span{x}, std::span{y}, std::span{out});
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == Approx(alpha * x[i] + y[i]));
}

TEST_CASE(
    "simd_kernels::reduce_sum works for double",
    "[lithe][simd][gap5][double]"
) {
    const std::array<double, 8> a{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    const double result = backends::simd_kernels::reduce_sum(std::span{a});
    CHECK(result == Approx(36.0));
}

TEST_CASE(
    "simd_kernels::add works for int32_t",
    "[lithe][simd][gap5][int32]"
) {
    const std::array<std::int32_t, 8> a{1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<std::int32_t, 8> b{10, 20, 30, 40, 50, 60, 70, 80};
    std::array<std::int32_t, 8> out{};
    backends::simd_kernels::add(std::span{a}, std::span{b}, std::span{out});
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == a[i] + b[i]);
}

TEST_CASE(
    "simd_kernels::mul works for int32_t",
    "[lithe][simd][gap5][int32]"
) {
    const std::array<std::int32_t, 6> a{1, 2, 3, 4, 5, 6};
    const std::array<std::int32_t, 6> b{3, 4, 5, 6, 7, 8};
    std::array<std::int32_t, 6> out{};
    backends::simd_kernels::mul(std::span{a}, std::span{b}, std::span{out});
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == a[i] * b[i]);
}

TEST_CASE(
    "bind_vector_plan accepts a proven f64 elementwise plan",
    "[lithe][simd][gap5][double][vector-plan]"
) {
    hl::vector_plan plan;
    plan.lanes = 4;
    plan.element_bits = 64;
    plan.tail = hl::vector_tail_strategy::scalar_epilogue;
    plan.reduction = hl::vector_reduction_shape::none;
    plan.legality = hl::vector_plan_legality::proven;
    plan.schedule_materialized = true;

    const auto binding = backends::bind_vector_plan(plan);
    REQUIRE(binding.accepted());
    REQUIRE(binding.native_lanes > 0);
}

TEST_CASE(
    "bind_vector_plan rejects unsupported element_bits (16-bit)",
    "[lithe][simd][vector-plan]"
) {
    hl::vector_plan plan;
    plan.lanes = 8;
    plan.element_bits = 16; // not supported
    plan.tail = hl::vector_tail_strategy::scalar_epilogue;
    plan.reduction = hl::vector_reduction_shape::none;
    plan.legality = hl::vector_plan_legality::proven;
    plan.schedule_materialized = true;

    const auto binding = backends::bind_vector_plan(plan);
    REQUIRE_FALSE(binding.accepted());
    REQUIRE(binding.native_lanes == 0);
}

TEST_CASE(
    "execute_simd_binary<double> produces correct results",
    "[lithe][simd][gap5][double]"
) {
    hl::vector_plan plan;
    plan.lanes = 4;
    plan.element_bits = 64;
    plan.tail = hl::vector_tail_strategy::scalar_epilogue;
    plan.reduction = hl::vector_reduction_shape::none;
    plan.legality = hl::vector_plan_legality::proven;
    plan.schedule_materialized = true;

    const auto lowering = backends::lower_vector_plan_for_simd(
        plan, backends::simd_binary_operation::add);

    const std::array<double, 4> lhs{1.0, 2.0, 3.0, 4.0};
    const std::array<double, 4> rhs{5.0, 6.0, 7.0, 8.0};
    std::array<double, 4> output{};
    bool fallback_called = false;

    const auto path = backends::execute_simd_binary<double>(
        lowering, std::span{lhs}, std::span{rhs}, std::span{output},
        [&fallback_called](const auto l, const auto r, auto res) {
            fallback_called = true;
            for (std::size_t i = 0; i < res.size(); ++i) res[i] = l[i] + r[i];
        });

    CHECK_FALSE(fallback_called);
    for (std::size_t i = 0; i < output.size(); ++i)
        CHECK(output[i] == Approx(lhs[i] + rhs[i]));
    // path is vectorized only if binding was accepted
    if (lowering.accepted())
        CHECK(path == backends::simd_execution_path::vectorized);
}

TEST_CASE(
    "double_lanes and int32_lanes return positive counts",
    "[lithe][simd][gap5]"
) {
    CHECK(backends::simd_kernels::double_lanes() > 0);
    CHECK(backends::simd_kernels::int32_lanes() > 0);
}

TEST_CASE(
    "simd_kernels::reduce_sum works for int32_t",
    "[lithe][simd][gap5][int32]"
) {
    const std::array<std::int32_t, 7> a{1, 2, 3, 4, 5, 6, 7};
    CHECK(backends::simd_kernels::reduce_sum(std::span{a}) == 28);
}

TEST_CASE(
    "planned SIMD reduction executes and preserves scalar fallback",
    "[lithe][simd][vector-plan][reduction]"
) {
    hl::vector_plan plan;
    plan.lanes = 8;
    plan.element_bits = 32;
    plan.tail = hl::vector_tail_strategy::scalar_epilogue;
    plan.reduction = hl::vector_reduction_shape::horizontal;
    plan.legality = hl::vector_plan_legality::proven;
    plan.schedule_materialized = true;

    const auto lowering = backends::lower_vector_reduction_plan_for_simd(plan);
    const std::array<float, 5> values{1.f, 2.f, 3.f, 4.f, 5.f};
    bool fallback_called = false;
    const auto [sum, path] = backends::execute_simd_reduction<float>(
        lowering,
        std::span{values},
        [&fallback_called](const auto input) {
            fallback_called = true;
            float total = 0.f;
            for (const auto value : input) total += value;
            return total;
        });
    CHECK(sum == Approx(15.0f));
    CHECK(path == backends::simd_execution_path::vectorized);
    CHECK_FALSE(fallback_called);

    plan.reduction = hl::vector_reduction_shape::none;
    const auto rejected = backends::lower_vector_reduction_plan_for_simd(plan);
    const auto [fallback_sum, fallback_path] = backends::execute_simd_reduction<float>(
        rejected,
        std::span{values},
        [&fallback_called](const auto input) {
            fallback_called = true;
            float total = 0.f;
            for (const auto value : input) total += value;
            return total;
        });
    CHECK(fallback_sum == Approx(15.0f));
    CHECK(fallback_path == backends::simd_execution_path::scalar_fallback);
    CHECK(fallback_called);
}

