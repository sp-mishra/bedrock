#include <catch_amalgamated.hpp>

#include "lithe/lithe_execution_admission.hpp"

namespace {
    using namespace lithe::codegen::hl;

    TEST_CASE("execution admission updates only its matching selector candidate",
              "[lithe][execution][admission]") {
        execution_selection_inputs inputs;
        inputs.vector_legal = true;
        inputs.candidates = {{
            {planned_execution_kind::interpreter, true, 0, 10},
            {planned_execution_kind::jit, true, 0, 5},
            {planned_execution_kind::simd, true, 0, 1},
            {planned_execution_kind::metal, true, 0, 1},
            {planned_execution_kind::vulkan, false, 0, 1},
        }};
        const execution_backend_admission rejected{
            .kind = planned_execution_kind::simd,
            .plan_admitted = false,
            .provider_available = true,
            .reason = execution_admission_reason::plan_rejected,
        };

        REQUIRE(apply_execution_admission(inputs, rejected));
        CHECK_FALSE(inputs.candidates[2].available);
        CHECK(inputs.candidates[3].available);
    }

    TEST_CASE("execution fallback chain follows the explicit macOS preference order",
              "[lithe][execution][admission]") {
        execution_selection_inputs inputs;
        inputs.vector_legal = true;
        inputs.accelerator_legal = true;
        inputs.candidates = {{
            {planned_execution_kind::interpreter, true, 0, 1},
            {planned_execution_kind::jit, true, 0, 1},
            {planned_execution_kind::simd, true, 0, 1},
            {planned_execution_kind::metal, true, 0, 1},
            {planned_execution_kind::vulkan, true, 0, 1},
        }};

        const auto chain = make_execution_fallback_chain(
            inputs, {}, planned_execution_kind::metal);

        REQUIRE(chain.size == 4);
        CHECK(chain.kinds[0] == planned_execution_kind::vulkan);
        CHECK(chain.kinds[1] == planned_execution_kind::simd);
        CHECK(chain.kinds[2] == planned_execution_kind::jit);
        CHECK(chain.kinds[3] == planned_execution_kind::interpreter);
    }

    TEST_CASE("required forced provider is reported as unsatisfied without fallback",
              "[lithe][execution][admission]") {
        execution_selection_inputs inputs;
        inputs.candidates = {{
            {planned_execution_kind::interpreter, true, 0, 1},
            {planned_execution_kind::jit, false, 0, 0},
            {planned_execution_kind::simd, false, 0, 0},
            {planned_execution_kind::metal, false, 0, 0},
            {planned_execution_kind::vulkan, false, 0, 0},
        }};
        const execution_selection_policy policy{
            .force = planned_execution_kind::metal,
            .force_requires_success = true,
        };

        const auto selection = select_execution_plan(inputs, policy);

        CHECK(selection.selected == planned_execution_kind::metal);
        CHECK(selection.force_unsatisfied);
        CHECK_FALSE(selection.fell_back);
    }
} // namespace
