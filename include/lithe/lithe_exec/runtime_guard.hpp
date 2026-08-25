#pragma once

// =============================================================================
// lithe_exec/runtime_guard.hpp — Runtime versioning: guards + versioned plan
//
// Namespace: lithe::exec
//
// Provides:
//   guard_kind       — what condition a runtime guard checks
//   runtime_guard    — POD guard descriptor (kind + operands + constant)
//   execution_plan_id— opaque handle to an execution_plan (index)
//   versioned_plan   — fast path + fallback path + guard predicate set
//
// Design:
//   - All POD; operands are value ids in the HL MIR value table.
//   - constant holds thresholds: alignment width, min trip count, etc.
//   - Fallback MUST be the safe_cpu scalar plan when fallback_policy::safe_cpu.
//   - No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include <cstdint>
#include <type_traits>
#include <vector>

namespace lithe::exec {
    // =========================================================================
    // guard_kind — what runtime predicate to evaluate
    // =========================================================================

    enum class guard_kind : std::uint8_t {
        no_alias = 0, // [A..A+size) ∩ [B..B+size) = ∅
        aligned = 1, // base_addr % constant == 0
        min_trip_count = 2, // loop trip count ≥ constant
        device_available = 3, // GPU/accelerator device is present and ready
        device_resident = 4, // buffer is resident on device (no transfer needed)
        reduction_policy_ok = 5, // runtime reduction policy check (custom ops)
    };

    [[nodiscard]] inline constexpr std::string_view to_string(guard_kind k) noexcept {
        switch (k) {
        case guard_kind::no_alias: return "no_alias";
        case guard_kind::aligned: return "aligned";
        case guard_kind::min_trip_count: return "min_trip_count";
        case guard_kind::device_available: return "device_available";
        case guard_kind::device_resident: return "device_resident";
        case guard_kind::reduction_policy_ok: return "reduction_policy_ok";
        }
        return "unknown";
    }

    // =========================================================================
    // runtime_guard — one runtime check
    //
    // operand_a / operand_b: value ids in the HL MIR value table.
    //   no_alias:        a = pointer A, b = pointer B (size in constant)
    //   aligned:         a = pointer, constant = required alignment
    //   min_trip_count:  a = trip-count value, constant = threshold
    //   device_available:operand_a = device id constant
    //   device_resident: a = buffer base value id
    //   reduction_policy_ok: a = accumulator id
    // =========================================================================

    struct runtime_guard {
        guard_kind kind = guard_kind::no_alias;
        std::uint32_t operand_a = 0;
        std::uint32_t operand_b = 0;
        std::int64_t constant = 0;
    };

    static_assert(std::is_trivially_copyable_v<runtime_guard>);
    static_assert(std::is_standard_layout_v<runtime_guard>);

    // =========================================================================
    // execution_plan_id — opaque index into the auto_execution_pass results vector
    // =========================================================================

    struct execution_plan_id {
        std::uint32_t index = std::uint32_t(-1); // sentinel = invalid

        [[nodiscard]] constexpr bool valid() const noexcept {
            return index != std::uint32_t(-1);
        }

        [[nodiscard]] constexpr bool operator==(execution_plan_id o) const noexcept {
            return index == o.index;
        }
    };

    static_assert(std::is_trivially_copyable_v<execution_plan_id>);

    // =========================================================================
    // versioned_plan — fast path + fallback + guards
    //
    // Execution semantics: if all guards hold → execute fast plan,
    //                      else → execute fallback plan.
    // Fallback must always be the safe scalar plan.
    // =========================================================================

    struct versioned_plan {
        execution_plan_id fast;
        execution_plan_id fallback;
        std::vector<runtime_guard> guards;

        [[nodiscard]] bool is_guarded() const noexcept { return !guards.empty(); }

        [[nodiscard]] bool valid() const noexcept {
            return fast.valid() && fallback.valid();
        }
    };
} // namespace lithe::exec
