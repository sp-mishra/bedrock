#pragma once

// crank/safety.hpp — Safety failure policy + SafetyError runtime record (Module 3).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Models what a *failed runtime guard* does (design §7b).
// Guards exist only for `unknown` obligations — a proven program has no SafetyError path.
//
// safety_kind  — what type of violation occurred
// SafetyError  — POD structured record (no allocation on the failure path)
// safety_failure — what action to take when a guard fires
// FromSafetyError — concept for error types that can wrap SafetyError
//
// Policy resolution order (§7b):
//   1. function @on_safety_failure(policy) attribute
//   2. module safety_policy declaration
//   3. context default (return_result where expressible, else trap)
//
// Non-Result fn + return_result + live guard → compile diagnostic (§7b.3).
// No sentinel value invented; no silent fallback.
//
// defer interaction (§7b.4):
//   return_result / host_handler → scope unwinds (defers run)
//   trap / terminate             → defers do NOT run

#include "languages/crank/source_span.hpp"

#include <cstdint>
#include <string_view>
#include <string>

namespace crank {
    // ============================================================================
    // safety_kind — what category of violation occurred
    // ============================================================================

    enum class safety_kind : std::uint8_t {
        bounds_violation, // array/slice index out of range
        div_by_zero, // integer division or modulo by zero
        range_conversion, // narrowing `as` overflows target type
        assert_failed, // runtime `assert` guard fired
        overflow_checked, // checked arithmetic overflowed
        tx_failed, // transaction commit guard failed
    };

    [[nodiscard]] constexpr std::string_view to_string(safety_kind k) noexcept {
        switch (k) {
        case safety_kind::bounds_violation: return "BoundsViolation";
        case safety_kind::div_by_zero: return "DivByZero";
        case safety_kind::range_conversion: return "RangeConversion";
        case safety_kind::assert_failed: return "AssertFailed";
        case safety_kind::overflow_checked: return "OverflowChecked";
        case safety_kind::tx_failed: return "TxFailed";
        }
        return "Unknown";
    }

    // ============================================================================
    // SafetyError — POD record; no heap allocation on the failure path
    // ============================================================================

    struct SafetyError {
        safety_kind kind;
        source_span at; // source location of the guard
    };

    static_assert(std::is_trivially_copyable_v<SafetyError>);

    // ============================================================================
    // safety_failure — what to do when a runtime guard fires
    // ============================================================================

    enum class safety_failure : std::uint8_t {
        return_result, // return Result.Err(E::from(SafetyError)) — fn must return Result<T,E>
        trap, // immediate process trap / abort (defers do NOT run)
        terminate, // std::terminate equivalent (defers do NOT run)
        host_handler, // call a registered host callback; scope unwinds (defers run)
    };

    [[nodiscard]] constexpr std::string_view to_string(safety_failure p) noexcept {
        switch (p) {
        case safety_failure::return_result: return "return_result";
        case safety_failure::trap: return "trap";
        case safety_failure::terminate: return "terminate";
        case safety_failure::host_handler: return "host_handler";
        }
        return "unknown";
    }

    // ============================================================================
    // FromSafetyError concept — error types that can wrap SafetyError
    //
    // Satisfied by TxError / String / user error types.
    // Required when safety_failure::return_result is in use.
    // ============================================================================

    template <class E>
    concept FromSafetyError = requires(const SafetyError& se) {
        { E::from(se) } -> std::convertible_to<E>;
    };

    // ============================================================================
    // safety_policy_record — per-scope effective policy with source provenance
    // ============================================================================

    enum class policy_source : std::uint8_t {
        function_attribute, // @on_safety_failure(...)
        module_declaration, // module-level policy statement
        context_default, // fallback default
    };

    struct safety_policy_record {
        safety_failure policy = safety_failure::trap; // context default
        policy_source source = policy_source::context_default;
        std::string_view fn_name; // non-owning; lifetime tied to the AST
    };

    // ============================================================================
    // safety_policy_resolver
    //
    // Resolves the effective safety_failure for a given function/scope.
    // Resolution order: function attribute > module declaration > context default.
    // ============================================================================

    class safety_policy_resolver {
    public:
        safety_policy_resolver() = default;

        void set_module_policy(safety_failure p) noexcept {
            module_policy_ = p;
            has_module_ = true;
        }

        void set_context_default(safety_failure p) noexcept {
            context_default_ = p;
        }

        [[nodiscard]] safety_policy_record resolve(
            std::string_view fn_name,
            bool fn_has_attr,
            safety_failure fn_attr_policy) const noexcept {
            if (fn_has_attr)
                return {fn_attr_policy, policy_source::function_attribute, fn_name};
            if (has_module_)
                return {module_policy_, policy_source::module_declaration, fn_name};
            return {context_default_, policy_source::context_default, fn_name};
        }

    private:
        safety_failure module_policy_ = safety_failure::trap;
        safety_failure context_default_ = safety_failure::trap;
        bool has_module_ = false;
    };

    // ============================================================================
    // safety_diagnostic_kind — compile-time safety diagnostic codes
    // ============================================================================

    enum class safety_diagnostic_kind : std::uint8_t {
        non_result_return_result, // §7b.3: non-Result fn + return_result + live guard
    };

    [[nodiscard]] constexpr std::string_view
    safety_diagnostic_code(safety_diagnostic_kind k) noexcept {
        switch (k) {
        case safety_diagnostic_kind::non_result_return_result:
            return "CRANK-SAFE-001";
        }
        return "CRANK-SAFE-000";
    }

    struct safety_compile_diagnostic {
        safety_diagnostic_kind kind;
        source_span at;
        std::string message;

        [[nodiscard]] bool is_error() const noexcept { return true; } // all are errors

        [[nodiscard]] constexpr std::string_view code() const noexcept {
            return safety_diagnostic_code(kind);
        }
    };

    // ============================================================================
    // non_result_guard_check — §7b.3 compile-time rule
    //
    // Returns a diagnostic if:
    //   - a function does NOT return Result<T,E>
    //   - its safety_failure policy is return_result
    //   - it has at least one live guard (unknown obligation)
    //
    // Callers should check this after the obligation discharge pass.
    // ============================================================================

    [[nodiscard]] inline std::optional<safety_compile_diagnostic>
    check_non_result_return_result(
        std::string_view fn_name,
        bool fn_returns_result,
        safety_failure fn_policy,
        bool has_live_guard,
        source_span fn_span) {
        if (!fn_returns_result
            && fn_policy == safety_failure::return_result
            && has_live_guard) {
            return safety_compile_diagnostic{
                safety_diagnostic_kind::non_result_return_result,
                fn_span,
                "function '" + std::string(fn_name)
                + "' uses @on_safety_failure(return_result) but its return type is not "
                "Result<T,E>; either change the return type, pick trap/terminate/"
                "host_handler, or strengthen the contract to eliminate the live guard"
            };
        }
        return std::nullopt;
    }
} // namespace crank
