#include <catch_amalgamated.hpp>

#include "lithe/backends/lithe_codegen_metal.hpp"

namespace {
    using namespace lithe::codegen;

    TEST_CASE("Metal binds only proven vector plans supported by its runtime contract",
              "[lithe][metal][plan]") {
        hl::vector_plan plan{
            .lanes = 8,
            .element_bits = 32,
            .tail = hl::vector_tail_strategy::masked,
            .reduction = hl::vector_reduction_shape::none,
            .legality = hl::vector_plan_legality::proven,
            .schedule_materialized = true,
        };

        const auto binding = backends::bind_vector_plan_for_metal(plan);

        CHECK(binding.lanes == 8);
        CHECK(binding.tail == hl::vector_tail_strategy::masked);
        CHECK(binding.accepted() == backends::metal_backend::available());
    }

    TEST_CASE("Metal rejects unproven vector plans without probing the device",
              "[lithe][metal][plan]") {
        hl::vector_plan plan{
            .lanes = 8,
            .element_bits = 32,
            .tail = hl::vector_tail_strategy::none,
            .reduction = hl::vector_reduction_shape::none,
            .legality = hl::vector_plan_legality::unknown,
            .schedule_materialized = true,
        };

        const auto binding = backends::bind_vector_plan_for_metal(plan);

        CHECK_FALSE(binding.accepted());
        CHECK(binding.disposition == backends::metal_plan_disposition::scalar_fallback);
    }
} // namespace
