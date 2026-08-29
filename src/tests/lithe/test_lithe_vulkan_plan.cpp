#include <catch_amalgamated.hpp>

#include "lithe/backends/lithe_codegen_vulkan_spirv_ir.hpp"

namespace {
    using namespace lithe::codegen;
    using namespace lithe::ir;

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

    // =========================================================================
    // GAP-10: SPIR-V result-id uniqueness validation
    // =========================================================================

    TEST_CASE("spirv_module::validate passes for a well-formed binary elementwise module",
              "[lithe][vulkan][spirv][gap10]") {
        auto mod = backends::emit_spirv_binary_elementwise(
            backends::spirv_binary_operation::add, 64);
        REQUIRE(mod.valid());
        const auto state = mod.validate();
        CHECK(state == ir_resolution_state::resolved);
    }

    TEST_CASE("spirv_module::validate rejects a module with bad magic word",
              "[lithe][vulkan][spirv][gap10]") {
        auto mod = backends::emit_spirv_binary_elementwise(
            backends::spirv_binary_operation::multiply, 64);
        REQUIRE(!mod.words.empty());
        mod.words[0] = 0xDEADBEEFu; // corrupt magic
        const auto state = mod.validate();
        CHECK(state == ir_resolution_state::unresolved_required_operations);
    }

    TEST_CASE("spirv_module::validate rejects a module with duplicate result IDs",
              "[lithe][vulkan][spirv][gap10]") {
        auto mod = backends::emit_spirv_binary_elementwise(
            backends::spirv_binary_operation::add, 64);
        REQUIRE(mod.valid());
        // Inject a duplicate OpFAdd instruction with the same result ID as an
        // existing one.  OpFAdd = opcode 129, word count = 5,
        // layout: [inst] [float_t] [result_id] [va] [vb].
        // We use result ID 30 (va) which is already defined as a load result.
        const std::uint32_t inst_word = (5u << 16) | 129u; // wcount=5, opcode=129
        mod.words.push_back(inst_word);
        mod.words.push_back(5u);  // float_t result type (id 5 = float)
        mod.words.push_back(30u); // duplicate result ID 30 (= va, already defined)
        mod.words.push_back(19u); // va operand
        mod.words.push_back(20u); // vb operand
        const auto state = mod.validate();
        CHECK(state == ir_resolution_state::unresolved_required_operations);
    }

    TEST_CASE("spirv_module::validate rejects a truncated module",
              "[lithe][vulkan][spirv][gap10]") {
        auto mod = backends::emit_spirv_binary_elementwise(
            backends::spirv_binary_operation::add, 64);
        // Truncate to just the header — no instructions, no OpEntryPoint.
        mod.words.resize(backends::k_spirv_header_words);
        const auto state = mod.validate();
        CHECK(state == ir_resolution_state::unresolved_required_operations);
    }

    TEST_CASE("spirv_opcode_has_result recognises value-producing opcodes",
              "[lithe][vulkan][spirv][gap10]") {
        CHECK(backends::spirv_opcode_has_result(129)); // OpFAdd
        CHECK(backends::spirv_opcode_has_result(61));  // OpLoad
        CHECK(backends::spirv_opcode_has_result(65));  // OpAccessChain
        CHECK_FALSE(backends::spirv_opcode_has_result(71));  // OpDecorate — no result
        CHECK_FALSE(backends::spirv_opcode_has_result(253)); // OpReturn — no result
    }

    TEST_CASE("spirv_opcode_is_type recognises type-definition opcodes",
              "[lithe][vulkan][spirv][gap10]") {
        CHECK(backends::spirv_opcode_is_type(19));  // OpTypeVoid
        CHECK(backends::spirv_opcode_is_type(22));  // OpTypeFloat
        CHECK(backends::spirv_opcode_is_type(21));  // OpTypeInt
        CHECK_FALSE(backends::spirv_opcode_is_type(129)); // OpFAdd — not a type
        CHECK_FALSE(backends::spirv_opcode_is_type(15));  // OpEntryPoint — not a type
    }
} // namespace
