#pragma once

// =============================================================================
// lithe_execution/engine_execute.hpp — portable-first unified execution entry
//
// Arch §3 end-to-end flow:
//   portable module → verify_portable (arch §5.2/§5.4)
//                   → make_execution_plan (capability-driven planner, arch §6)
//                   → execute_plan (impl-3 cache + fallback chain, arch §12)
//                   → execution_outcome<T>  (typed result or typed failure)
//
// Provides:
//   run_request         — per-invocation options layered over plan_request
//   run()               — the portable-first end-to-end entry point
//
// Architecture rules:
//   • Boundary verification (verify_portable) is MANDATORY (arch §5).
//   • Target-local optimization stays inside each backend's compile step.
//   • Static dispatch on the hot path; erasure only at the backend_registry
//     boundary (arch §6).
//   • run() is layered OVER basic_lithe_engine — it does not replace the typed
//     compile/invoke path; it adds the portable-artifact-first flow.
//
// Template parameters:
//   T         — the typed result the caller expects from execution
//   BackendFn — callable: (persisted_backend_id) -> execution_outcome<T>
//               represents get_or_compile + invoke for one backend (from impl-3)
//   Sink      — NADI sink for exec_profiler (default: exec_default_sink = NoSink)
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <expected>
#include <span>
#include <type_traits>
#include <vector>

#include "execution_result.hpp"                    // execution_outcome, execution_failure
#include "execution_plan.hpp"                      // execution_plan, make_execution_plan, execute_plan
#include "target_profile.hpp"                      // target_profile, discover_target_profile
#include "exec_profiling.hpp"                      // exec_profiler, exec_default_sink
#include "../lithe_ir/portable/verify.hpp"         // verify_portable, verify_report
#include "../lithe_ir/portable/module.hpp"         // portable_module
#include "../lithe_algorithms/selection.hpp"       // backend_capability_info, cost_based_backend_selector

namespace lithe::execution {
    // =============================================================================
    // run_request — per-invocation options
    //
    // Extends plan_request with the backend candidates list and an optional
    // custom target_profile (default: discover_target_profile()).
    // =============================================================================

    struct run_request {
        plan_request plan; // capability policy + allow_fallback
        std::span<const algorithms::backend_capability_info> candidates;
        const target_profile* target = nullptr; // null = use host profile
        ir::portable::verify_policy verify = {}; // verification strictness
    };

    // =============================================================================
    // run() — the portable-first end-to-end execution entry (arch §3)
    //
    // Flow:
    //   1. verify_portable(module, req.verify) — hard boundary check (arch §5.2/§5.4)
    //   2. make_execution_plan(...)            — capability-driven planner
    //   3. execute_plan(plan, backend_fn)      — impl-3 cache + fallback
    //
    // Returns execution_outcome<T>:
    //   success → execution_success<T>{value, stats}
    //   failure → execution_failure{stage, error, diagnostics}
    //
    // Profiler events emitted at each stage (zero-cost when Sink::enabled == false).
    // =============================================================================

    template <class T,
              class BackendFn,
              class Sink = exec_default_sink>
        requires std::invocable<BackendFn, persisted_backend_id>
        && std::same_as<std::invoke_result_t<BackendFn, persisted_backend_id>,
                        execution_outcome<T>>
    [[nodiscard]] execution_outcome<T>
    run(const ir::portable::portable_module& module,
        const run_request& req,
        BackendFn&& backend_fn,
        algorithms::cost_based_backend_selector selector = {}) {
        exec_profiler<Sink> profiler;

        // ---- Step 1: boundary verification (arch §5.2/§5.4) ----------------------
        {
            const auto report = ir::portable::verify_portable(module, req.verify);
            if (!report.ok) {
                execution_failure f;
                f.stage = failure_stage::verification;
                f.error = execution_error{"portable module failed deep verification"};
                for (const auto& d : report.diagnostics)
                    f.diagnostics.push_back(d);
                profiler.on_failure(f.stage, f.error.detail);
                return std::unexpected(std::move(f));
            }
        }

        // ---- Step 2: capability-driven plan --------------------------------------
        const target_profile& target = req.target
                                           ? *req.target
                                           : discover_target_profile();

        auto plan_result = make_execution_plan(module, target, req.plan,
                                               req.candidates, selector);
        if (!plan_result.has_value()) {
            profiler.on_failure(plan_result.error().stage,
                                plan_result.error().error.detail);
            return std::unexpected(std::move(plan_result.error()));
        }
        const execution_plan& plan = plan_result.value();
        profiler.on_plan_built(plan);

        // ---- Step 3: execute via impl-3 cache + fallback chain -------------------
        auto outcome = execute_plan<T>(plan, std::forward<BackendFn>(backend_fn));
        if (!outcome.has_value()) {
            profiler.on_failure(outcome.error().stage, outcome.error().error.detail);
        }
        else {
            profiler.on_invoke_end(outcome->stats.served_by,
                                   outcome->stats.invoke_ns,
                                   outcome->stats.fallback_occurred);
        }
        return outcome;
    }
} // namespace lithe::execution
