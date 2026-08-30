#pragma once

// Pure execution-admission values shared by optional backend adapters.

#include "lithe_codegen_hl_passes.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace lithe::codegen::hl {
    enum class execution_admission_reason : std::uint8_t {
        admitted,
        plan_rejected,
        provider_unavailable,
        installation_failed,
        dispatch_failed,
    };

    [[nodiscard]] constexpr std::string_view to_reason_code(
        const execution_admission_reason reason) noexcept {
        switch (reason) {
        case execution_admission_reason::admitted: return "admitted";
        case execution_admission_reason::plan_rejected: return "plan_rejected";
        case execution_admission_reason::provider_unavailable: return "provider_unavailable";
        case execution_admission_reason::installation_failed: return "installation_failed";
        case execution_admission_reason::dispatch_failed: return "dispatch_failed";
        }
        return "unknown";
    }


    struct execution_backend_admission {
        planned_execution_kind kind = planned_execution_kind::interpreter;
        bool plan_admitted = false;
        bool provider_available = false;
        execution_admission_reason reason = execution_admission_reason::plan_rejected;

        [[nodiscard]] constexpr bool usable() const noexcept {
            return plan_admitted && provider_available;
        }
    };

    [[nodiscard]] constexpr execution_candidate_cost apply_execution_admission(
        execution_candidate_cost candidate,
        const execution_backend_admission admission) noexcept {
        if (candidate.kind == admission.kind) {
            candidate.available = admission.usable();
            candidate.unavailable_reason = admission.usable() ? std::string_view{}
                : to_reason_code(admission.reason);
        }
        return candidate;
    }

    [[nodiscard]] constexpr bool apply_execution_admission(
        execution_selection_inputs& inputs,
        const execution_backend_admission admission) noexcept {
        for (auto& candidate : inputs.candidates) {
            if (candidate.kind != admission.kind) continue;
            candidate.available = admission.usable();
            candidate.unavailable_reason = admission.usable() ? std::string_view{}
                : to_reason_code(admission.reason);
            return true;
        }
        return false;
    }

    inline constexpr std::array macos_execution_fallback_order{
        planned_execution_kind::metal,
        planned_execution_kind::vulkan,
        planned_execution_kind::simd,
        planned_execution_kind::jit,
        planned_execution_kind::interpreter,
    };

    struct execution_fallback_chain {
        std::array<planned_execution_kind, 5> kinds{};
        std::uint8_t size = 0;

        [[nodiscard]] constexpr bool empty() const noexcept { return size == 0; }
    };

    [[nodiscard]] constexpr execution_fallback_chain make_execution_fallback_chain(
        const execution_selection_inputs& inputs,
        const execution_selection_policy& policy,
        const planned_execution_kind selected) noexcept {
        execution_fallback_chain out;
        bool after_selected = false;
        for (const auto kind : macos_execution_fallback_order) {
            if (!after_selected) {
                after_selected = kind == selected;
                continue;
            }
            for (const auto& candidate : inputs.candidates) {
                if (candidate.kind != kind || !candidate.available) continue;
                if (execution_candidate_legal(kind, inputs, policy))
                    out.kinds[out.size++] = kind;
                break;
            }
        }
        return out;
    }
} // namespace lithe::codegen::hl
