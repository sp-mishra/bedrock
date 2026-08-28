#pragma once

// =============================================================================
// lithe_exec/reduction.hpp — Reduction recognition and contract seam
//
// Namespace: lithe::exec
//
// Provides:
//   reduction_op      — arithmetic/bitwise/logical reduction operations
//   reduction_info    — one recognized reduction: accumulator, op, identity,
//                       associativity, FP-reorder policy
//   reduction_contract— concept: user-supplied contract for custom reductions
//   recognize_reductions(loop, pdg) — produce vector<reduction_info>
//
// Design:
//   - Associativity/identity from emit::tag_descriptor::is_commutative + a
//     constexpr lookup table keyed on stable op ids.
//   - FP reordering gated on fp_reordering_allowed flag (set by caller from
//     profile_descriptor::deterministic or auto_execution_policy::deterministic).
//   - Custom reductions: concept-based contract; no op-contract = sequential.
//   - No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include <array>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lithe::exec {
    // =========================================================================
    // reduction_op — the combining operation
    // =========================================================================

    enum class reduction_op : std::uint8_t {
        add = 0,
        mul = 1,
        min_val = 2,
        max_val = 3,
        bitwise_and = 4,
        bitwise_or = 5,
        bitwise_xor = 6,
        logical_and = 7,
        logical_or = 8,
        custom = 9, // requires a registered reduction_contract
    };

    [[nodiscard]] inline constexpr std::string_view to_string(reduction_op op) noexcept {
        switch (op) {
        case reduction_op::add: return "add";
        case reduction_op::mul: return "mul";
        case reduction_op::min_val: return "min";
        case reduction_op::max_val: return "max";
        case reduction_op::bitwise_and: return "band";
        case reduction_op::bitwise_or: return "bor";
        case reduction_op::bitwise_xor: return "bxor";
        case reduction_op::logical_and: return "land";
        case reduction_op::logical_or: return "lor";
        case reduction_op::custom: return "custom";
        }
        return "unknown";
    }

    // =========================================================================
    // reduction_info — one recognized reduction in a loop
    // =========================================================================

    struct reduction_info {
        std::uint32_t accumulator_id = 0; // preg/value id of the accumulator
        std::uint32_t identity_id = 0; // value id of the identity element
        reduction_op op = reduction_op::add;
        bool associative = false;
        bool commutative = false;
        bool deterministic = false; // result is order-independent
        bool fp_reordering_allowed = true; // false when deterministic mode
    };

    static_assert(std::is_trivially_copyable_v<reduction_info>);

    // =========================================================================
    // reduction_contract — concept for user-defined custom reductions
    //
    // A type R satisfies reduction_contract iff it provides:
    //   - R::op_id : uint32_t (stable tag id for this op)
    //   - R::associative : bool constexpr
    //   - R::deterministic : bool constexpr
    //   - R::identity_value() -> uint64_t (bit-cast to type at use site)
    // =========================================================================

    template <class R>
    concept reduction_contract =
        requires {
            { R::op_id } -> std::convertible_to<std::uint32_t>;
            { R::associative } -> std::convertible_to<bool>;
            { R::deterministic } -> std::convertible_to<bool>;
            { R::identity_value() } -> std::convertible_to<std::uint64_t>;
        };

    // =========================================================================
    // Built-in reduction trait table — constexpr lookup keyed on reduction_op
    //
    // Provides associativity, commutativity, and fp_reorder eligibility for
    // the built-in ops. Custom ops must supply a reduction_contract.
    // =========================================================================

    namespace impl {
        struct reduction_traits {
            bool associative;
            bool commutative;
            bool fp_reorder_eligible; // false for FP add/mul in deterministic mode
        };

        inline constexpr std::array<reduction_traits, 10> kReductionTraits = {
            {
                // add
                {.associative = true, .commutative = true, .fp_reorder_eligible = true},
                // mul
                {.associative = true, .commutative = true, .fp_reorder_eligible = true},
                // min
                {.associative = true, .commutative = true, .fp_reorder_eligible = false},
                // max
                {.associative = true, .commutative = true, .fp_reorder_eligible = false},
                // band
                {.associative = true, .commutative = true, .fp_reorder_eligible = false},
                // bor
                {.associative = true, .commutative = true, .fp_reorder_eligible = false},
                // bxor
                {.associative = true, .commutative = true, .fp_reorder_eligible = false},
                // land
                {.associative = true, .commutative = true, .fp_reorder_eligible = false},
                // lor
                {.associative = true, .commutative = true, .fp_reorder_eligible = false},
                // custom — unknown by default; contract must specify
                {.associative = false, .commutative = false, .fp_reorder_eligible = false},
            }
        };

        [[nodiscard]] inline constexpr reduction_traits traits_for(reduction_op op) noexcept {
            return kReductionTraits[static_cast<std::uint8_t>(op)];
        }
    } // namespace impl

    // =========================================================================
    // make_reduction_info — build reduction_info from op + policy flags
    // =========================================================================

    [[nodiscard]] inline constexpr reduction_info make_reduction_info(
        std::uint32_t accumulator_id,
        std::uint32_t identity_id,
        reduction_op op,
        bool deterministic_mode,
        bool is_integer_type = false) noexcept {
        const auto& t = impl::traits_for(op);
        const bool fp_reorder = !deterministic_mode &&
            t.fp_reorder_eligible &&
            !is_integer_type;
        return reduction_info{
            .accumulator_id = accumulator_id,
            .identity_id = identity_id,
            .op = op,
            .associative = t.associative,
            .commutative = t.commutative,
            .deterministic = deterministic_mode,
            .fp_reordering_allowed = fp_reorder || is_integer_type,
        };
    }
} // namespace lithe::exec
