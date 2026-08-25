#pragma once

// =============================================================================
// lithe_execution/execution_plan.hpp — capability-driven execution plan (impl-4)
//
// Arch §6: a plan records required + preferred capabilities, selected backend +
// reason, fallback chain, memory/device requirements, sync/cancellation
// requirements, estimated cost, diagnostics, and constraining semantic policies.
//
// Provides:
//   memory_requirements     — input/output buffer placement constraints
//   device_requirements     — compute domain + async requirements
//   sync_requirements       — synchronization / barrier model
//   plan_request            — caller intent: preferred policy, allow_fallback list
//   execution_plan          — the fully-formed plan record
//   make_execution_plan()   — capability-driven planner:
//                             required caps = hard filter (planning failure if unmet)
//                             preferred caps = authorized fallback chain only
//   execute_plan()          — run plan: selected + fallback chain via impl-3 cache;
//                             recoverable failures continue; definitive (trap/
//                             deadline/cancel) return immediately without fallback.
//
// Reuses:
//   cost_vector             (lithe_cost_model.hpp)
//   semantic_policy         (lithe_ir/portable/opt/pass.hpp via execution_plan.hpp)
//   cost_based_backend_selector, selection_explanation (lithe_algorithms/selection.hpp)
//   persisted_backend_id, backend_capability_set       (foundation.hpp)
//   execution_outcome<T>    (execution_result.hpp)
//   target_profile          (target_profile.hpp)
//
// Static dispatch on hot path; erasure only at the backend_registry boundary.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "execution_result.hpp"                             // execution_outcome, execution_failure, failure_stage
#include "target_profile.hpp"                              // target_profile
#include "foundation.hpp"                                  // persisted_backend_id, backend_capability_set
#include "../lithe_cost_model.hpp"                         // cost_vector
#include "../lithe_ir/portable/opt/pass.hpp"               // semantic_policy
#include "../lithe_algorithms/selection.hpp"               // backend_capability_info, selection_explanation, …
#include "../lithe_diagnostics.hpp"                        // diag::diagnostic

namespace lithe::execution {
    // =============================================================================
    // memory_requirements — where input and output buffers must live
    // =============================================================================

    struct memory_requirements {
        memory_domain input_domain = memory_domain::host_cpu;
        memory_domain output_domain = memory_domain::host_cpu;
        std::size_t min_input_alignment = 1;
        std::size_t min_output_alignment = 1;
    };

    // =============================================================================
    // device_requirements — execution domain constraints
    // =============================================================================

    struct device_requirements {
        memory_domain preferred_domain = memory_domain::host_cpu;
        bool allow_migration = false; // may move between domains at runtime
    };

    // =============================================================================
    // sync_requirements — synchronization model
    // =============================================================================

    struct sync_requirements {
        bool need_memory_barrier_before = false;
        bool need_memory_barrier_after = false;
        bool submit_to_queue = false; // async GPU submit required
    };

    // =============================================================================
    // plan_request — caller intent for make_execution_plan()
    //
    // allow_fallback: explicit list of backends authorized for fallback, in priority
    // order (GPU → SIMD → interpreter). Empty = no fallback authorized.
    //
    // Distinguishing authorized-fallback from arbitrary-fallback is the arch §6 rule:
    // preferred capability failure causes fallback ONLY along the explicitly authorized
    // chain; required capability failure is always a hard error.
    // =============================================================================

    struct plan_request {
        algorithms::selection_policy policy = algorithms::selection_policy::balanced;
        std::vector<persisted_backend_id> allow_fallback; // authorized order; empty = none
        bool cancellable = false;
        std::optional<std::uint64_t> deadline_ns; // wall-clock ns; nullopt = no deadline
    };

    // =============================================================================
    // execution_plan — the fully-formed plan record (arch §6)
    // =============================================================================

    struct execution_plan {
        // Capability constraints (from portable_module.declared_capabilities → mapping)
        backend_capability_set required; // MUST be satisfied — hard filter
        backend_capability_set preferred; // soft preference — drives ranking

        // Selected backend + reason
        persisted_backend_id selected;
        std::string_view selected_reason; // points into static or negotiation buffer

        // Authorized fallback chain (remaining candidates in authorized order)
        std::vector<persisted_backend_id> fallback_chain;

        // Resource requirements
        memory_requirements io_mem;
        device_requirements device;
        sync_requirements sync;
        bool cancellable = false;
        std::optional<std::uint64_t> deadline_ns;

        // Cost estimate (from the selector's scoring)
        lithe::cost::cost_vector estimated_cost;

        // Diagnostics (per-backend accept/reject reasons from the planner)
        std::vector<diag::diagnostic> diagnostics;

        // Policy that constrained selection (from impl-2)
        lithe::ir::portable::opt::semantic_policy policy;

        // Selection detail (for NADI + post-mortem)
        algorithms::selection_explanation explanation;

        [[nodiscard]] bool valid() const noexcept {
            return !selected.empty();
        }
    };

    // =============================================================================
    // map_capability_bits — portable_capability_bit → backend_capability_set
    //
    // Converts the portable module's declared capabilities into the execution-layer
    // backend_capability_set that the planner uses as its required filter.
    // =============================================================================

    namespace detail {
        [[nodiscard]] inline backend_capability_set
        map_capability_bits(const ir::portable::capability_set& caps) noexcept {
            backend_capability_set out;
            // Always required: arithmetic, memory, branches, calls
            out.add(backend_feature::integer_arithmetic);
            out.add(backend_feature::floating_arithmetic);
            out.add(backend_feature::memory_operands);
            out.add(backend_feature::branches);

            if (caps.has(ir::portable::portable_capability_bit::simd_hint))
                out.add(backend_feature::tensor_arithmetic);
            // gpu_hint becomes a preferred rather than required cap in default mapping

            return out;
        }

        // Preferred caps from the module's soft hints.
        [[nodiscard]] inline backend_capability_set
        map_preferred_bits(const ir::portable::capability_set& caps) noexcept {
            backend_capability_set out;
            if (caps.has(ir::portable::portable_capability_bit::gpu_hint))
                out.add(backend_feature::tensor_arithmetic);
            return out;
        }
    } // namespace detail

    // =============================================================================
    // make_execution_plan() — capability-driven planner (arch §6, §11.M4.2)
    //
    // Template parameters:
    //   Backends — span of backend_capability_info (describes candidate backends)
    //   Selector — selector strategy (default: cost_based_backend_selector)
    //
    // Algorithm:
    //   1. Map portable module capabilities → required/preferred sets.
    //   2. Filter candidates by required caps (hard error if none remain).
    //   3. Rank by Selector under selection_policy.
    //   4. selected = best; fallback_chain = remaining candidates in authorized order
    //      (only backends present in plan_request.allow_fallback are included in chain).
    //   5. Record reasons + estimated cost + diagnostics.
    // =============================================================================

    [[nodiscard]] inline std::expected<execution_plan, execution_failure>
    make_execution_plan(
        const ir::portable::portable_module& mod,
        const target_profile& target,
        const plan_request& req,
        std::span<const algorithms::backend_capability_info> candidates,
        algorithms::cost_based_backend_selector selector = {}) {
        (void)target; // used for fingerprinting; available to selector extensions

        execution_plan plan;
        plan.policy = {}; // semantic_policy from impl-2 opt pass — default here
        plan.cancellable = req.cancellable;
        plan.deadline_ns = req.deadline_ns;

        // Step 1: map module capabilities → required/preferred
        plan.required = detail::map_capability_bits(mod.declared_capabilities);
        plan.preferred = detail::map_preferred_bits(mod.declared_capabilities);

        // Step 2: filter by required caps
        std::vector<algorithms::backend_capability_info> eligible;
        eligible.reserve(candidates.size());
        for (const auto& b : candidates) {
            if (b.caps.contains_all(plan.required))
                eligible.push_back(b);
        }

        if (eligible.empty()) {
            execution_failure f;
            f.stage = failure_stage::planning;
            f.error = execution_error{"required capabilities unmet: no eligible backend"};
            return std::unexpected(std::move(f));
        }

        // Step 3: rank eligible backends
        algorithms::negotiation_report_buffer report;
        selector.policy = req.policy;
        const auto sel = selector(
            std::span<const algorithms::backend_capability_info>{eligible},
            compile_requirements{.required = plan.required, .preferred = plan.preferred},
            report,
            &plan.explanation);

        if (!sel.has_value()) {
            execution_failure f;
            f.stage = failure_stage::planning;
            f.error = execution_error{sel.error().detail};
            return std::unexpected(std::move(f));
        }

        // Step 4: selected + authorized fallback chain
        plan.selected = persisted_backend_id{sel->backend_id};
        plan.selected_reason = sel->negotiation_report;
        plan.estimated_cost = lithe::cost::cost_vector{
            static_cast<float>(sel->score), // latency proxy
            0.0f, 0.0f, 0.0f
        };

        // Build fallback chain: backends in allow_fallback that are also eligible,
        // excluding the selected backend.
        for (const auto& fb_id : req.allow_fallback) {
            if (fb_id == plan.selected) continue;
            for (const auto& b : eligible) {
                if (persisted_backend_id{b.backend_id} == fb_id) {
                    plan.fallback_chain.push_back(fb_id);
                    break;
                }
            }
        }

        // Step 5: diagnostics from selection explanation
        for (const auto& d : plan.explanation.decisions) {
            if (!d.accepted)
                plan.diagnostics.push_back(d.diag);
        }

        return plan;
    }

    // =============================================================================
    // execute_plan() — run the plan via impl-3 cache; fallback on recoverable failure
    //
    // Template parameters:
    //   T         — the typed result the caller expects
    //   BackendFn — callable: (persisted_backend_id, ...) -> execution_outcome<T>
    //               represents get_or_compile + invoke for one backend
    //
    // Contract (arch §6, §12):
    //   • Iterates [selected] + fallback_chain in order.
    //   • Calls backend_fn(id) → execution_outcome<T> for each.
    //   • On success: attach served-by + fallback stats; return.
    //   • On failure: if is_definitive(stage) → return immediately (no fallback).
    //   • On recoverable failure: log diagnostic; try next backend.
    //   • If all exhausted: return failure{stage=invocation, diagnostics}.
    // =============================================================================

    template <class T, class BackendFn>
        requires std::invocable<BackendFn, persisted_backend_id>
        && std::same_as<std::invoke_result_t<BackendFn, persisted_backend_id>,
                        execution_outcome<T>>
    [[nodiscard]] execution_outcome<T>
    execute_plan(
        const execution_plan& plan,
        BackendFn&& backend_fn) {
        std::vector<diag::diagnostic> all_diags;
        bool fallback_occurred = false;
        std::string_view fallback_reason;

        // Build the ordered trial list: [selected, fallback_chain...]
        auto try_backend = [&](const persisted_backend_id& id, bool is_fallback)
            -> execution_outcome<T> {
            auto result = backend_fn(id);
            if (result.has_value()) {
                // Attach served-by + fallback metadata to stats.
                result->stats.served_by = id;
                result->stats.fallback_occurred = is_fallback;
                result->stats.fallback_reason = fallback_reason;
                for (auto& d : all_diags)
                    (void)d; // diagnostics available on plan; not re-attached here
                return result;
            }
            const auto& fail = result.error();
            // Definitive failures: return immediately without trying the next backend.
            if (is_definitive(fail.stage)) {
                return result;
            }
            // Recoverable: record diagnostic and allow fallback.
            if (!fail.diagnostics.empty()) {
                for (const auto& d : fail.diagnostics)
                    all_diags.push_back(d);
            }
            else {
                diag::diagnostic d;
                d.level = diag::severity::info;
                d.stage = diag::stage::backend;
                d.code = std::string{id.value};
                d.message = std::string{id.value} + ": recoverable failure, trying fallback";
                all_diags.push_back(std::move(d));
            }
            return result;
        };

        // Try selected first.
        auto r = try_backend(plan.selected, false);
        if (r.has_value() || is_definitive(r.error().stage))
            return r;

        // Try authorized fallback chain.
        for (const auto& fb_id : plan.fallback_chain) {
            fallback_occurred = true;
            fallback_reason = "primary unavailable or failed";
            auto fb_r = try_backend(fb_id, true);
            if (fb_r.has_value() || is_definitive(fb_r.error().stage))
                return fb_r;
        }
        (void)fallback_occurred; // consumed via stats.fallback_occurred inside try_backend

        // All backends exhausted.
        execution_failure f;
        f.stage = failure_stage::invocation;
        f.error = execution_error{"all backends exhausted"};
        f.diagnostics = std::move(all_diags);
        return std::unexpected(std::move(f));
    }
} // namespace lithe::execution
