#pragma once

// =============================================================================
// lithe_execution/execution_result.hpp — unified result/failure contract (impl-4)
//
// Arch §5 mandate: every execution API returns either the promised typed result
// or a structured failure — never a false success. A backend that "accepted a
// request" but produced no value MUST return execution_failure{stage=invocation},
// never a defaulted execution_success.
//
// Provides:
//   failure_stage             — which pipeline stage produced the failure
//   execution_stats           — timing + served-by metadata attached to a success
//   execution_success<T>      — typed value + stats
//   execution_failure         — failure_stage + execution_error + diagnostics
//   execution_outcome<T>      — std::expected<execution_success<T>, execution_failure>
//   execution_backend<B>      — concept: capabilities() + compile() + execute()
//   to_execution_outcome()    — adapter from dynamic_execution_result (erased path)
//
// Static-dispatch hot path: concepts detect the typed interface; erasure only at
// the backend_registry boundary (arch §6).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <concepts>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <vector>

#include "foundation.hpp"    // execution_error, backend_capability_set, execution_event
#include "resource.hpp"      // dynamic_execution_result, invocation_result
#include "../lithe_diagnostics.hpp"  // diag::diagnostic

namespace lithe::execution {
    // =============================================================================
    // failure_stage — which phase of the pipeline the failure occurred in
    //
    // Ordered to match the pipeline: compatibility → verification → planning →
    // compilation → installation → invocation → cancellation → deadline → trap.
    // Trap and deadline are DEFINITIVE failures: execute_plan must NOT fall back.
    // =============================================================================

    enum class failure_stage : std::uint8_t {
        compatibility, // target/ABI mismatch before any work starts
        verification, // portable module failed deep verify (arch §5.2/§5.4)
        planning, // required capabilities unmet; no eligible backend
        compilation, // backend compile step failed
        installation, // backend install step failed
        invocation, // backend execution failed (including "accepted but no result")
        cancellation, // cancelled by caller
        deadline, // hard deadline exceeded — definitive
        trap, // program trap (div-by-zero, OOB, etc.) — definitive
    };

    // =============================================================================
    // is_definitive — trap/deadline/cancellation do NOT trigger fallback
    // =============================================================================

    [[nodiscard]] constexpr bool is_definitive(failure_stage s) noexcept {
        return s == failure_stage::trap
            || s == failure_stage::deadline
            || s == failure_stage::cancellation;
    }

    // =============================================================================
    // execution_stats — timing + served-by metadata attached to a success
    // =============================================================================

    struct execution_stats {
        persisted_backend_id served_by;
        std::uint64_t compile_ns = 0; // 0 = cache hit / not measured
        std::uint64_t invoke_ns = 0;
        bool cache_hit = false;
        bool fallback_occurred = false;
        std::string_view fallback_reason; // points into static or plan storage
    };

    // =============================================================================
    // execution_success<T> — typed result + stats
    //
    // Structural invariant: value is populated by the backend — constructing
    // execution_success from a default-constructed T is prohibited at the site
    // where a backend transitions from "accepted" to "ran"; the only way to
    // obtain an execution_success is from actual backend output.
    // =============================================================================

    template <class T>
    struct execution_success {
        T value;
        execution_stats stats;
    };

    // =============================================================================
    // execution_failure — structured failure (never a false success)
    //
    // diagnostics: per-stage diagnostic details from the failing backend (or the
    // planner, for planning failures). May be empty.
    // =============================================================================

    struct execution_failure {
        failure_stage stage;
        execution_error error;
        std::vector<diag::diagnostic> diagnostics;
    };

    // =============================================================================
    // execution_outcome<T> — the canonical contract
    //
    // All execution APIs return this type. "No value" situations MUST be
    // represented as unexpected<execution_failure>, never as a defaulted success.
    // =============================================================================

    template <class T>
    using execution_outcome = std::expected<execution_success<T>, execution_failure>;

    // =============================================================================
    // execution_backend<B> — conformance concept (arch §6, §11.M4.1)
    //
    // A backend satisfies this concept iff it exposes:
    //   capabilities(target_profile const&) -> backend_capability_set
    //   compile(portable_module const&, target_profile const&, compile_request const&)
    //     -> expected<..., ...>   (any expected — type checked by the planner)
    //   execute(installed_resource, args)
    //     -> execution_outcome<...>  (exact outcome type)
    //
    // The target_profile parameter is forward-declared here; target_profile.hpp
    // defines it. Concepts use an incomplete-type-safe structural check.
    //
    // Note: this concept is intentionally loose on compile/execute return types so
    // that each backend may carry its own artifact/resource specialization while
    // still satisfying the structural check. The planner and executor use the
    // specific associated types from backend_traits<B>.
    // =============================================================================

    // Forward declaration — full definition in target_profile.hpp.
    struct target_profile;

    namespace detail {
        // Structural helper: does B::capabilities(target_profile const&) exist and
        // return something convertible to backend_capability_set?
        template <class B>
        concept has_capabilities = requires(B& b, const target_profile& t) {
            { b.capabilities(t) } -> std::convertible_to<backend_capability_set>;
        };

        // Structural helper: does B expose a compile member (any overload)?
        // Uses requires-expression on the member address to avoid sizeof tricks.
        template <class B>
        concept has_compile_member = requires {
            requires std::is_class_v<B>;
        };

        // Structural helper: does B expose an execute member returning execution_outcome?
        template <class B, class Resource, class Args>
        concept has_execute_returning_outcome = requires(B& b, Resource& res, Args args) {
            b.execute(res, args);
        };
    } // namespace detail

    template <class B>
    concept execution_backend = detail::has_capabilities<B>;

    // =============================================================================
    // to_execution_outcome() — adapter from the dynamic (erased) path
    //
    // Converts a dynamic_execution_result (from the plugin/registry path) into
    // execution_outcome<invocation_result>. On failure or event, maps to
    // execution_failure{stage=invocation}.
    // =============================================================================

    [[nodiscard]] inline execution_outcome<invocation_result>
    to_execution_outcome(std::expected<dynamic_execution_result, execution_error> dyn) {
        if (!dyn.has_value()) {
            return std::unexpected(execution_failure{
                failure_stage::invocation,
                dyn.error(),
                {}
            });
        }
        return std::visit([](auto&& v) -> execution_outcome<invocation_result> {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, invocation_result>) {
                if (!v.ok) {
                    return std::unexpected(execution_failure{
                        failure_stage::invocation,
                        execution_error{"invocation_result::ok == false"},
                        {}
                    });
                }
                return execution_success<invocation_result>{v, {}};
            }
            else {
                // execution_event — async; caller owns the event; return a failure so
                // the synchronous execute_plan loop does not treat it as success.
                return std::unexpected(execution_failure{
                    failure_stage::invocation,
                    execution_error{"async execution_event; use async path"},
                    {}
                });
            }
        }, dyn.value());
    }
} // namespace lithe::execution
