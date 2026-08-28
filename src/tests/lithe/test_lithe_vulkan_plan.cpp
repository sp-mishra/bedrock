#include <catch_amalgamated.hpp>

#include "lithe/backends/lithe_codegen_vulkan_spirv_ir.hpp"

namespace {
    using namespace lithe::codegen;

    [[nodiscard]] device::kernel_plan compatible_kernel_plan(
        hl::hl_mir_function& source,
        hl::hl_operation& root) {
        device::kernel_plan plan;
        plan.source = &source;
        plan.root = &root;
        plan.operations.push_back(&root);
        plan.element_type = device::scalar_type::f32;
        plan.launch.local_x = 64;
        plan.legality.rank_one = true;
        plan.legality.canonical_counted = true;
        plan.legality.regular_stride = true;
        plan.legality.all_memrefs_contiguous = true;
        plan.legality.uniform_memory_element_type = true;

        const auto add_binding = [&plan](const std::uint64_t id,
                                         const device::binding_access access,
                                         const std::uint32_t index) {
            device::kernel_binding binding;
            binding.base_value = {.id = id};
            binding.view.elem_kind = abstract_value_kind::floating;
            binding.view.elem_bits = 32;
            binding.view.rank = 1;
            binding.view.shape[0] = 128;
            binding.view.strides[0] = 1;
            binding.view.contiguous = true;
            binding.access = access;
            binding.index = index;
            plan.bindings.push_back(binding);
        };
        add_binding(1, device::binding_access::read, 0);
        add_binding(2, device::binding_access::read, 1);
        add_binding(3, device::binding_access::write, 2);
        return plan;
    }

    [[nodiscard]] hl::vector_plan compatible_vector_plan() {
        return {
            .lanes = 8,
            .element_bits = 32,
            .tail = hl::vector_tail_strategy::masked,
            .reduction = hl::vector_reduction_shape::none,
            .legality = hl::vector_plan_legality::proven,
            .schedule_materialized = true,
        };
    }

    TEST_CASE("Vulkan binds a proven vector plan to the existing SPIR-V ABI",
              "[lithe][vulkan][plan]") {
        hl::hl_mir_function source;
        hl::hl_operation root;
        auto kernel_plan = compatible_kernel_plan(source, root);

        const auto binding = backends::bind_vector_plan_for_vulkan(
            compatible_vector_plan(), kernel_plan);

        CHECK(binding.compatible());
        CHECK(binding.planned_lanes == 8);
        CHECK(binding.local_x == 64);
        CHECK(binding.tail == hl::vector_tail_strategy::masked);
    }

    TEST_CASE("Vulkan binding rejects a vector plan without a materialized schedule",
              "[lithe][vulkan][plan]") {
        hl::hl_mir_function source;
        hl::hl_operation root;
        auto vector_plan = compatible_vector_plan();
        vector_plan.schedule_materialized = false;

        const auto binding = backends::bind_vector_plan_for_vulkan(
            vector_plan, compatible_kernel_plan(source, root));

        CHECK_FALSE(binding.compatible());
        CHECK(binding.disposition == backends::vulkan_plan_disposition::scalar_fallback);
    }
} // namespace
