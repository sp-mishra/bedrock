#include <catch_amalgamated.hpp>

#include "lithe/lithe_execution_admission.hpp"

namespace {
    using namespace lithe::codegen::hl;

    TEST_CASE("execution admission updates only its matching selector candidate",
              "[lithe][execution][admission]") {
        execution_selection_inputs inputs;
        inputs.vector_legal = true;
        inputs.candidates = {{
            {.kind = planned_execution_kind::interpreter, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 10},
            {.kind = planned_execution_kind::jit, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 5},
            {.kind = planned_execution_kind::simd, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
            {.kind = planned_execution_kind::metal, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
            {.kind = planned_execution_kind::vulkan, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 1},
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
            {.kind = planned_execution_kind::interpreter, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
            {.kind = planned_execution_kind::jit, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
            {.kind = planned_execution_kind::simd, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
            {.kind = planned_execution_kind::metal, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
            {.kind = planned_execution_kind::vulkan, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
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
            {.kind = planned_execution_kind::interpreter, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
            {.kind = planned_execution_kind::jit, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 0},
            {.kind = planned_execution_kind::simd, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 0},
            {.kind = planned_execution_kind::metal, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 0},
            {.kind = planned_execution_kind::vulkan, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 0},
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

    TEST_CASE("execution admission stores reason code on unavailable candidates",
              "[lithe][execution][admission]") {
        execution_candidate_cost candidate{
            .kind = planned_execution_kind::metal,
            .available = true,
        };
        const execution_backend_admission unavailable{
            .kind = planned_execution_kind::metal,
            .plan_admitted = true,
            .provider_available = false,
            .reason = execution_admission_reason::provider_unavailable,
        };

        const auto updated = apply_execution_admission(candidate, unavailable);
        CHECK_FALSE(updated.available);
        CHECK(updated.unavailable_reason == "provider_unavailable");
    }

    TEST_CASE("forced selection fallback exposes adapter reason",
              "[lithe][execution][admission]") {
        execution_selection_inputs inputs;
        inputs.candidates = {{
            {.kind = planned_execution_kind::interpreter, .available = true, .setup_cost_ns = 0, .work_item_cost_ns = 1},
            {.kind = planned_execution_kind::jit, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 0, .unavailable_reason = "provider_unavailable"},
            {.kind = planned_execution_kind::simd, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 0},
            {.kind = planned_execution_kind::metal, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 0, .unavailable_reason = "plan_rejected"},
            {.kind = planned_execution_kind::vulkan, .available = false, .setup_cost_ns = 0, .work_item_cost_ns = 0},
        }};
        const execution_selection_policy policy{
            .force = planned_execution_kind::metal,
            .force_requires_success = false,
        };

        const auto selection = select_execution_plan(inputs, policy);

        CHECK(selection.fell_back);
        CHECK(selection.selected == planned_execution_kind::interpreter);
        CHECK(selection.fallback_reason == "plan_rejected");
    }
} // namespace
