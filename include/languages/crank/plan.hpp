#pragma once

// crank/plan.hpp — Execution plan construction, selection, fallback (design §7, §15).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Ties the execution layer together. Given a verified_mir, a set of discovered
// backends (capability.hpp), and a policy (execution_options + optional exec
// hints), construct_plan produces an execution_plan: a ranked list of legal
// backend candidates with a scalar fallback attached. execute_plan then runs the
// selected candidate and, on failure, walks the fallback chain — UNLESS the
// failing candidate already produced visible device effects, in which case retry
// is refused (unsafe_fallback_after_effects, design §7.4 / §11.4).
//
// execute_planned(lower_hl_result, args, opts, hints) — single crank entry (L-1 W1):
//   lower_to_physical → verify_crank_mir → construct_plan → execute_plan with a
//   lithe-native run closure (lithe::execution::compile + invoke). Interpreter is
//   the planner's scalar fallback candidate, not the primary path.
//
// Conflict resolution (design §7.2): required > preferred > advisory; within a
// strength tier, lowest cost wins; ties broken by determinism. Two conflicting
// `required` constraints are unsatisfiable → invalid_plan.
//
// execution_plan_record is the serializable mirror for dump.hpp (§15.1); it is
// named distinctly from any existing execution_plan_json to avoid a clash.

#include "languages/crank/execute.hpp"   // must be first: resolves the dump.hpp cycle
#include "languages/crank/capability.hpp"
#include "languages/crank/verify_mir.hpp"
#include "languages/crank/gpu_memory.hpp"
#include "languages/crank/cancellation.hpp"
#include "languages/crank/exec_hint.hpp"
#include "languages/crank/exec_result.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crank {
    // ============================================================================
    // legality / requirement_strength (design §7.1)
    // ============================================================================

    enum class legality : std::uint8_t {
        legal, // usable as-is
        requires_guard, // usable only under a runtime alias/bounds guard
        illegal, // cannot be used for this MIR
    };

    [[nodiscard]] constexpr std::string_view to_string(legality l) noexcept {
        switch (l) {
        case legality::legal: return "legal";
        case legality::requires_guard: return "requires_guard";
        case legality::illegal: return "illegal";
        }
        return "unknown";
    }

    enum class requirement_strength : std::uint8_t {
        advisory, // hint only — dropped if illegal/unavailable
        preferred, // bias selection strongly, but keep fallback
        required, // must be honored, or the plan is invalid
    };

    [[nodiscard]] constexpr std::string_view to_string(requirement_strength s) noexcept {
        switch (s) {
        case requirement_strength::advisory: return "advisory";
        case requirement_strength::preferred: return "preferred";
        case requirement_strength::required: return "required";
        }
        return "unknown";
    }

    // Bridge exec_hint's execution_preference + required flag onto a strength.
    // (`required` → required; `strong` → preferred; else advisory.) Does NOT
    // redefine execution_preference — it reads it.
    [[nodiscard]] constexpr requirement_strength
    from_preference(execution_preference p, bool required) noexcept {
        if (required) return requirement_strength::required;
        if (p == execution_preference::strong) return requirement_strength::preferred;
        return requirement_strength::advisory;
    }

    // ============================================================================
    // exec_pref — a resolved backend preference (design §7.1)
    //
    // Suffixed `exec_pref` (not `execution_preference`) so both this and exec_hint's
    // enum can coexist when a translation unit (e.g. dump.hpp) includes both.
    // ============================================================================

    struct exec_pref {
        execution_kind kind = execution_kind::scalar;
        requirement_strength strength = requirement_strength::advisory;
        bool deterministic = false;
    };

    // Map a crank exec attribute kind onto the coarse execution_kind family.
    [[nodiscard]] constexpr execution_kind
    kind_of(crank_attr_kind k) noexcept {
        switch (k) {
        case crank_attr_kind::parallel: return execution_kind::threaded;
        case crank_attr_kind::simd: return execution_kind::simd;
        case crank_attr_kind::gpu: return execution_kind::gpu;
        }
        return execution_kind::scalar;
    }

    [[nodiscard]] inline exec_pref pref_from_attr(const crank_exec_attr& a) noexcept {
        return exec_pref{
            kind_of(a.kind),
            from_preference(a.preference, a.required),
            a.deterministic
        };
    }

    // ============================================================================
    // backend_candidate — one option the planner ranks (design §7.1)
    // ============================================================================

    struct backend_candidate {
        backend_id id = 0;
        execution_kind kind = execution_kind::scalar;
        legality legal = legality::legal;
        std::uint64_t cost = 0; // lower is better (relative model)
        exec_pref requirement; // the preference that produced it
    };

    // ============================================================================
    // execution_plan — the constructed plan (design §7.3, §15.1)
    // ============================================================================

    struct execution_plan {
        std::uint64_t plan_id = 0;
        verified_mir fn;
        std::vector<backend_candidate> candidates;
        std::uint32_t selected = 0; // index into candidates
        std::optional<std::uint32_t> fallback; // index into candidates
        std::vector<std::string> guards; // runtime guards to emit
        transfer_plan transfers; // GPU residency plan (may be empty)
        std::string selection_reason;

        [[nodiscard]] const backend_candidate* selected_candidate() const noexcept {
            return selected < candidates.size() ? &candidates[selected] : nullptr;
        }
    };

    // ============================================================================
    // execution_plan_record — serializable mirror for dump.hpp (design §15.1)
    // ============================================================================

    struct execution_plan_candidate_record {
        backend_id id;
        std::string kind;
        std::string legality;
        std::uint64_t cost;
        std::string strength;
        bool deterministic;
    };

    struct execution_plan_record {
        std::uint64_t plan_id = 0;
        std::string fn_name;
        std::vector<execution_plan_candidate_record> candidates;
        std::uint32_t selected = 0;
        std::optional<std::uint32_t> fallback;
        std::vector<std::string> guards;
        std::string selection_reason;
    };

    [[nodiscard]] inline execution_plan_record
    to_record(const execution_plan& p) {
        execution_plan_record r;
        r.plan_id = p.plan_id;
        if (p.fn.function()) r.fn_name = p.fn.function()->function.name;
        r.selected = p.selected;
        r.fallback = p.fallback;
        r.guards = p.guards;
        r.selection_reason = p.selection_reason;
        for (const auto& c : p.candidates) {
            r.candidates.push_back(execution_plan_candidate_record{
                c.id, std::string(to_string(c.kind)), std::string(to_string(c.legal)),
                c.cost, std::string(to_string(c.requirement.strength)),
                c.requirement.deterministic
            });
        }
        return r;
    }

    // ============================================================================
    // detail — candidate generation + ranking
    // ============================================================================

    namespace detail {
        // Relative cost model (design §7.2): device dispatch is cheap per-element but
        // carries transfer latency; threaded amortizes over cores; simd is cheapest for
        // vectorizable straight-line; scalar is the always-legal baseline.
        [[nodiscard]] constexpr std::uint64_t base_cost(execution_kind k) noexcept {
            switch (k) {
            case execution_kind::simd: return 10;
            case execution_kind::threaded: return 20;
            case execution_kind::gpu: return 30; // + transfer, added by caller
            case execution_kind::async: return 25;
            case execution_kind::host: return 40;
            case execution_kind::scalar: return 100; // baseline / fallback
            }
            return 100;
        }

        // Compare two legal candidates: required > preferred > advisory, then lower cost,
        // then deterministic wins. Returns true if `a` should rank before `b`.
        [[nodiscard]] inline bool ranks_before(const backend_candidate& a,
                                               const backend_candidate& b) noexcept {
            const auto sa = static_cast<std::uint8_t>(a.requirement.strength);
            const auto sb = static_cast<std::uint8_t>(b.requirement.strength);
            if (sa != sb) return sa > sb; // stronger first
            if (a.cost != b.cost) return a.cost < b.cost; // cheaper first
            return a.requirement.deterministic && !b.requirement.deterministic;
        }
    } // namespace detail

    // ============================================================================
    // construct_plan — discover, generate, filter, rank, attach fallback (§7.3)
    //
    // verified_mir must outlive the returned plan (verified_mir is a non-owning
    // view). `hints` carries the user's @simd/@gpu/@parallel constraints; empty =
    // no preference (scalar-first). `transfers` is the GPU residency plan when a GPU
    // candidate is present (pass an empty one otherwise).
    //
    // Returns invalid_plan if two conflicting `required` hints cannot both hold, or
    // if a required backend is illegal/unavailable for this MIR.
    // ============================================================================

    [[nodiscard]] inline execution_result<execution_plan>
    construct_plan(const verified_mir& fn,
                   const execution_options& opts,
                   const std::vector<crank_exec_attr>& hints = {},
                   transfer_plan transfers = {}) {
        if (!fn.valid()) {
            return make_failed<execution_plan>(make_error(
                execution_error_kind::plan_construction_failed,
                "cannot plan an invalid verified_mir"));
        }

        const capability_set& caps = discover_backends(opts);
        const std::string fn_name = fn.function() ? fn.function()->function.name : std::string{};

        // Resolve hints into required/preferred strengths per kind.
        std::vector<exec_pref> prefs;
        prefs.reserve(hints.size());
        for (const auto& h : hints) prefs.push_back(pref_from_attr(h));

        // Detect conflicting requireds: two different kinds both marked required.
        std::optional<execution_kind> required_kind;
        for (const auto& p : prefs) {
            if (p.strength == requirement_strength::required) {
                if (required_kind && *required_kind != p.kind) {
                    return make_error_result<execution_plan>(make_error(
                        execution_error_kind::plan_construction_failed,
                        "conflicting required backends: " +
                        std::string(to_string(*required_kind)) + " vs " +
                        std::string(to_string(p.kind)),
                        fn_name));
                }
                required_kind = p.kind;
            }
        }

        // A required backend that was not discovered is fatal (no silent fallback).
        if (required_kind && !caps.has(*required_kind)) {
            return make_error_result<execution_plan>(make_error(
                execution_error_kind::required_backend_illegal,
                "required backend " + std::string(to_string(*required_kind)) +
                " is unavailable under this policy",
                fn_name));
        }

        execution_plan plan;
        plan.plan_id = detail::policy_fingerprint(opts);
        plan.fn = fn;
        plan.transfers = std::move(transfers);

        // Strength lookup for a discovered backend's kind.
        auto strength_for = [&](execution_kind k) -> requirement_strength {
            requirement_strength best = requirement_strength::advisory;
            bool seen = false;
            for (const auto& p : prefs) {
                if (p.kind != k) continue;
                seen = true;
                if (static_cast<std::uint8_t>(p.strength) >
                    static_cast<std::uint8_t>(best))
                    best = p.strength;
            }
            (void)seen;
            return best;
        };
        auto deterministic_for = [&](execution_kind k) -> bool {
            for (const auto& p : prefs) if (p.kind == k && p.deterministic) return true;
            return false;
        };

        // Generate one candidate per discovered backend.
        for (const auto& desc : caps.backends) {
            backend_candidate c;
            c.id = desc.id;
            c.kind = desc.kind;
            c.legal = legality::legal;
            c.cost = detail::base_cost(desc.kind);
            c.requirement = exec_pref{
                desc.kind, strength_for(desc.kind),
                deterministic_for(desc.kind)
            };
            // GPU carries the transfer cost of its residency plan.
            if (desc.kind == execution_kind::gpu)
                c.cost += static_cast<std::uint64_t>(plan.transfers.nodes.size()) * 5;
            plan.candidates.push_back(c);
        }

        if (plan.candidates.empty()) {
            return make_error_result<execution_plan>(make_error(
                execution_error_kind::backend_unavailable,
                "no backends discovered under this policy", fn_name));
        }

        // Rank: strongest requirement, then cheapest, then deterministic.
        std::sort(plan.candidates.begin(), plan.candidates.end(),
                  detail::ranks_before);

        plan.selected = 0;
        const auto& top = plan.candidates.front();
        plan.selection_reason =
            "selected " + std::string(to_string(top.kind)) +
            " (strength=" + std::string(to_string(top.requirement.strength)) +
            ", cost=" + std::to_string(top.cost) + ")";

        // Attach scalar fallback (always legal) unless the top pick IS scalar, or a
        // required non-scalar backend forbids falling back.
        const bool required_forbids_fallback =
            required_kind && *required_kind != execution_kind::scalar;
        if (!required_forbids_fallback) {
            for (std::uint32_t i = 0; i < plan.candidates.size(); ++i) {
                if (plan.candidates[i].kind == execution_kind::scalar && i != plan.selected) {
                    plan.fallback = i;
                    break;
                }
            }
        }

        return make_completed(std::move(plan));
    }

    // ============================================================================
    // backend_executor — the caller-supplied run function
    //
    // Runs a single candidate against the plan's verified_mir, returning the typed
    // result. The planner owns the fallback logic; the executor owns the actual
    // backend invocation (interpreter/simd/gpu). Modelled as a concept so execute_plan
    // stays vtable-free (design §7.4).
    // ============================================================================

    template <class F, class T>
    concept CandidateExecutor = requires(F f, const execution_plan& p,
                                         const backend_candidate& c) {
        { f(p, c) } -> std::same_as<execution_result<T>>;
    };

    // ============================================================================
    // execute_plan — run selected, fall back on failure (design §7.4)
    //
    // Fallback loop honoring "no unsafe retry after visible effects": if the failing
    // candidate produced observable device writes (plan.transfers.visible_device_writes
    // and the failing candidate was the GPU one), we do NOT retry — replaying would
    // double-apply effects. We return unsafe_fallback_after_effects instead.
    //
    // Cancellation/deadline are polled before each attempt.
    // ============================================================================

    template <class T, class Exec>
        requires CandidateExecutor<Exec, T>
    [[nodiscard]] execution_result<T>
    execute_plan(execution_plan& plan, Exec&& run,
                 const cancellation_token& token = {},
                 std::optional<deadline_point> deadline = std::nullopt) {
        const std::string fn_name =
            plan.fn.function() ? plan.fn.function()->function.name : std::string{};

        if (auto intr = check_interruption(token, deadline))
            return interrupt_result<T>(*intr, fn_name);

        const auto* sel = plan.selected_candidate();
        if (!sel) {
            return make_error_result<T>(make_error(
                execution_error_kind::plan_construction_failed,
                "plan has no selected candidate", fn_name));
        }

        // Attempt the selected candidate.
        execution_result<T> primary = run(plan, *sel);
        if (primary.completed()) return primary;

        // If the failed candidate committed visible device writes, refuse to retry.
        if (sel->kind == execution_kind::gpu && plan.transfers.visible_device_writes) {
            return make_failed<T>(make_error(
                execution_error_kind::unsafe_fallback_after_effects,
                "gpu candidate failed after visible device writes; fallback refused",
                fn_name, std::to_string(sel->id)));
        }

        // No fallback available → propagate the primary error.
        if (!plan.fallback) return primary;

        if (auto intr = check_interruption(token, deadline))
            return interrupt_result<T>(*intr, fn_name);

        const backend_candidate& fb = plan.candidates[*plan.fallback];
        execution_result<T> fallback = run(plan, fb);
        if (fallback.completed()) {
            fallback.trace.notes.push_back(
                "fell back from " + std::string(to_string(sel->kind)) +
                " to " + std::string(to_string(fb.kind)));
            return fallback;
        }
        return fallback;
    }

    // ============================================================================
    // execute_planned — single crank execution entry point (L-1 W1, design §7.3/§7.4)
    //
    // Resurrects the dead planner: construct_plan + execute_plan with a lithe-native
    // run closure.  The closure calls lithe::execution::compile then invoke for
    // each candidate; the interpreter is the planner's scalar fallback, not the
    // primary path.  CFG functions (counted loops) return a scalar via native JIT.
    //
    // hints: merged crank_exec_attr list from @parallel/@simd/@gpu on the function.
    //        Fed to construct_plan directly — no re-parsing.
    //
    // Falls back to the interpreter when:
    //   - lower_to_physical fails; or
    //   - construct_plan fails (no backends); or
    //   - the planner selects the scalar fallback candidate.
    // ============================================================================

    [[nodiscard]] inline crank_execute_result
    execute_planned(const lower_hl_result& hl_res,
                    const std::vector<std::int64_t>& args = {},
                    execute_options opts = {},
                    const std::vector<crank_exec_attr>& hints = {}) {
        crank_execute_result res;
        if (!hl_res.ok()) {
            res.diagnostics = hl_res.diagnostics;
            return res;
        }

        // Phase 1: HL → physical MIR (cached).
        auto lp = lower_to_physical(hl_res);
        res.stats.lower_ns = lp.lower_ns;
        for (auto& d : lp.diagnostics) res.diagnostics.push_back(std::move(d));
        if (!lp.ok()) return res;

        const auto& phys = *lp.phys;
        res.stats.instr_count = detail::count_instrs(phys);
        res.stats.branch_count = detail::count_branches(phys);
        res.stats.block_count = detail::count_blocks(phys);

        // Phase 2: construct the execution plan.
        execution_options plan_opts;
        plan_opts.allow_simd = true;
        plan_opts.allow_gpu = (opts.primary_backend_name == "gpu");
        plan_opts.allow_threads = true;

        // Build attr hints: caller-supplied attrs take precedence; opts.hint as advisory.
        std::vector<crank_exec_attr> effective_hints = hints;
        if (effective_hints.empty() && opts.hint.preferred.has_value()) {
            crank_exec_attr synthetic;
            switch (*opts.hint.preferred) {
            case lithe::exec::execution_kind::simd:
                synthetic.kind = crank_attr_kind::simd;
                break;
            case lithe::exec::execution_kind::gpu:
                synthetic.kind = crank_attr_kind::gpu;
                break;
            default:
                synthetic.kind = crank_attr_kind::parallel;
                break;
            }
            synthetic.required = opts.hint.required;
            synthetic.deterministic = opts.hint.deterministic;
            effective_hints.push_back(synthetic);
        }

        auto vm_res = verify_crank_mir(phys, /*expects_value=*/false);
        if (!vm_res.completed()) {
            res.fallback_fired = true;
            auto ir = detail::run_interpreter(phys, args, opts);
            res.return_value = ir.return_value;
            res.overflow_trapped = ir.overflow_trapped;
            res.stats.execute_ns = ir.stats.execute_ns;
            for (auto& d : ir.diagnostics) res.diagnostics.push_back(std::move(d));
            for (auto& n : ir.notes) res.notes.push_back(std::move(n));
            if (vm_res.error) res.diagnostics.push_back("verify: " + vm_res.error->message);
            return res;
        }

        auto plan_res = construct_plan(std::move(vm_res).unwrap(), plan_opts, effective_hints);
        if (!plan_res.completed()) {
            res.fallback_fired = true;
            res.stats.fallback_used = true;
            auto ir = detail::run_interpreter(phys, args, opts);
            res.return_value = ir.return_value;
            res.overflow_trapped = ir.overflow_trapped;
            res.stats.execute_ns = ir.stats.execute_ns;
            for (auto& d : ir.diagnostics) res.diagnostics.push_back(std::move(d));
            for (auto& n : ir.notes) res.notes.push_back(std::move(n));
            if (plan_res.error) res.diagnostics.push_back("plan: " + plan_res.error->message);
            return res;
        }

        auto plan = std::move(plan_res).unwrap();

        // Phase 3: execute via the lithe-native run closure.
        const lithe::exec::execution_hint base_hint =
            !effective_hints.empty() ? merge_exec_hints(effective_hints) : opts.hint;

        auto t0 = std::chrono::steady_clock::now();

        auto run_closure = [&](const execution_plan&, const backend_candidate& cand)
            -> execution_result<crank_execute_result> {
            lithe::execution::compile_request req;
            req.hint = base_hint;
            switch (cand.kind) {
            case execution_kind::simd:
                req.hint.preferred = lithe::exec::execution_kind::simd;
                break;
            case execution_kind::gpu:
                req.hint.preferred = lithe::exec::execution_kind::gpu;
                break;
            case execution_kind::threaded:
                req.hint.preferred = lithe::exec::execution_kind::threaded;
                break;
            default:
                req.hint.preferred = lithe::exec::execution_kind::scalar;
                break;
            }
            req.policy = {};

            auto cr = lithe::execution::compile(phys, req);
            crank_execute_result sub;
            sub.fallback_fired = cr.fallback_fired;
            sub.stats.fallback_used = cr.fallback_fired;
            for (const auto& d : cr.diagnostics) {
                if (detail::is_nonfatal_interp_note(d)) sub.notes.push_back(d);
                else sub.diagnostics.push_back(d);
            }

            if (!cr.ok() && !sub.diagnostics.empty()) {
                return make_failed<crank_execute_result>(make_error(
                    execution_error_kind::lowering_failed, sub.diagnostics.front()));
            }

            const std::span<const std::int64_t> arg_span{args};
            sub.return_value = lithe::execution::invoke(cr, arg_span);

            return make_completed(std::move(sub));
        };

        auto exec_res = execute_plan<crank_execute_result>(plan, run_closure);

        auto t1 = std::chrono::steady_clock::now();
        res.stats.execute_ns = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

        if (exec_res.completed()) {
            crank_execute_result& sub = *exec_res.value;
            res.return_value = sub.return_value;
            res.fallback_fired = sub.fallback_fired;
            res.overflow_trapped = sub.overflow_trapped;
            res.stats.fallback_used = sub.stats.fallback_used;
            for (auto& d : sub.diagnostics) res.diagnostics.push_back(std::move(d));
            for (auto& n : sub.notes) res.notes.push_back(std::move(n));
            for (auto& n : exec_res.trace.notes) res.notes.push_back(std::move(n));
        }
        else {
            if (exec_res.error) res.diagnostics.push_back(exec_res.error->message);
        }

        return res;
    }
} // namespace crank
