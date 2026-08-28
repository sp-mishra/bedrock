#pragma once

// crank/execute.hpp — Scalar physical MIR + interpreter execution (Module 4).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// crank_execute_result: output of interpreter execution.
//   return_value: the i64 result (or nullopt for void / trap)
//   diagnostics: runtime diagnostics + fallback trace
//   fallback_fired: true if primary was ineligible and interpreter was used
//   stats: timing + instruction counters
//
// execute_via_interpreter(lower_hl_result, args)
//   Calls coordinate_lowering_pass (HL→physical MIR), then drives the
//   interpreter_backend.  Integer ops are wrapping two's-complement by default;
//   @overflow(checked) is tracked via the overflow_checked flag and checked
//   against the safety policy at runtime.
//
// execute_with_auto_fallback(lower_hl_result, primary_kind, args)
//   Wraps execute_with_fallback: tries the named primary backend (GPU/SIMD),
//   falls back to interpreter on capability mismatch, emits NADI pulse.
//
// execute_planned(lower_hl_result, args, opts, hints)  — defined in plan.hpp
//   Single crank execution entry point (L-1 W1). Resurrects the crank planner:
//   construct_plan → execute_plan with a lithe-native run closure (compile+invoke).
//   Interpreter enters only as the planner's scalar fallback candidate.
//
// Design §3.6. No framework API edits. Consumes lower_hl_result from lower_hl.hpp.

#include "lithe/lithe_codegen.hpp"
#include "lithe/lithe_codegen_hl_passes.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/backends/lithe_codegen_backend_registry.hpp"
#include "lithe/lithe_execution/compile.hpp"
#include "lithe/lithe_exec/exec_hint.hpp"
#include "languages/crank/lower_hl.hpp"
#include "languages/crank/safety.hpp"
#include "languages/crank/exec_result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace crank {
    // ============================================================================
    // select_backend_name — §v2.8 device/SIMD routing hint.
    //
    // Maps a region's execution affinity to a backend name understood by
    // backend_kind_from_string(). The crank planner computes simd_eligible / GPU
    // affinity from generic_capability_summary (§v2.6) and calls this to fill
    // execute_options::primary_backend_name; execute_with_auto_fallback then routes
    // to the real backend and falls back to the interpreter (NADI pulse) if the
    // backend is ineligible for the concrete MIR. Kept enum-free and dependency-free
    // so execute.hpp need not include monomorphize.hpp.
    // ============================================================================

    enum class exec_affinity : std::uint8_t { cpu, simd, gpu };

    [[nodiscard]] constexpr std::string_view
    select_backend_name(exec_affinity a) noexcept {
        switch (a) {
        case exec_affinity::simd: return "simd";
        case exec_affinity::gpu: return "gpu";
        case exec_affinity::cpu:
#if defined(LITHE_HAS_ASMJIT)
            return "asmjit";
#else
            return "interpreter";
#endif
        }
        return "interpreter";
    }

    // ============================================================================
    // execution_status — now defined in exec_result.hpp (design §3.1).
    //
    // The 4 legacy enumerators (ok / unsupported_control_flow / lowering_failed /
    // runtime_error) are retained there as source-compatible aliases of the 7
    // canonical states, so the crank_execute_result.status field and its callers
    // keep working unchanged. See exec_result.hpp.
    // ============================================================================

    // ============================================================================
    // execute_stats — per-invocation timing + counters
    // ============================================================================

    struct execute_stats {
        std::int64_t lower_ns = 0; // coordinate_lowering_pass wall time
        std::int64_t execute_ns = 0; // interpreter run wall time
        std::uint32_t instr_count = 0; // instructions emitted into physical MIR
        std::uint32_t branch_count = 0; // branch/branch_cond ops in physical MIR
        std::uint32_t block_count = 0; // physical MIR basic blocks
        bool fallback_used = false;
    };

    // ============================================================================
    // crank_execute_result — output of execution
    // ============================================================================

    struct crank_execute_result {
        std::optional<std::int64_t> return_value; // nullopt = void / trap / skipped
        std::vector<std::string> diagnostics; // fatal runtime errors only
        std::vector<std::string> notes; // non-fatal runtime notes (e.g. interpreter control-flow limits)
        bool fallback_fired = false;
        bool overflow_trapped = false;
        // Typed execution status. Prefer this over checking execution_skipped_reason.
        execution_status status = execution_status::ok;
        // Legacy string reason — kept for source compatibility. Mirrors status field.
        // Check status instead; execution_skipped_reason will be removed in a future version.
        std::optional<std::string_view> execution_skipped_reason;
        execute_stats stats;

        // ok() reflects fatal diagnostics only. Non-fatal runtime notes (interpreter
        // control-flow limits) never flip this. Also true for unsupported_control_flow.
        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // ============================================================================
    // execute_options — per-call knobs
    // ============================================================================

    struct execute_options {
        enum class execution_path : std::uint8_t {
            auto_select, // choose by MIR shape (default)
            jit_preferred, // prefer native JIT; safe fallback to interpreter
            interpreter_only, // force interpreter_backend
            native_only // force lithe::execution::compile + invoke
        };

        struct hotness_thresholds {
            std::uint32_t instruction_count = 24u;
            std::uint32_t block_count = 4u;
            std::uint32_t branch_count = 1u;
        } hotness{};

        // If true, @overflow(checked) scope: trap on integer overflow.
        // Default false = wrapping two's-complement.
        bool overflow_checked = false;
        // Safety policy used when a bounds guard fires at runtime.
        safety_failure safety_policy = safety_failure::trap;
        // Preferred primary backend name (empty = auto via plan()).
        // Ignored by execute_physical_native (which always calls compile() via plan).
        std::string primary_backend_name;
        // Execution hint from @parallel/@simd/@gpu attributes on the function.
        // Threaded into lithe::execution::compile_request when using native path.
        lithe::exec::execution_hint hint;
        // Path selection for execute_physical / execute_with_auto_fallback when
        // the caller does not force a specific backend name.
        execution_path path = execution_path::auto_select;
    };

    // ============================================================================
    // lower_phase_result — output of the lowering phase (HL MIR → physical MIR)
    //
    // Separates lowering from execution so callers can lower once and interpret
    // many times (e.g. benchmarks measuring execute-only cost). The physical MIR
    // is owned by the source lower_hl_result::cached_phys; phys points into it and
    // stays valid for that result's lifetime.
    // ============================================================================

    struct lower_phase_result {
        const lithe::codegen::mir::physical_mir_function* phys = nullptr; // borrowed
        std::vector<std::string> diagnostics;
        std::int64_t lower_ns = 0; // 0 on cache hit
        lithe::exec::execution_hint hint; // from hl_res.exec_hint

        [[nodiscard]] bool ok() const noexcept {
            return phys != nullptr && diagnostics.empty();
        }
    };

    // ============================================================================
    // detail helpers
    // ============================================================================

    namespace detail {
        inline constexpr std::uint32_t k_auto_native_instr_threshold = 24u;
        inline constexpr std::uint32_t k_auto_native_block_threshold = 4u;
        inline constexpr std::uint32_t k_auto_native_branch_threshold = 1u;

        // Substring marker for the interpreter's documented control-flow limitation
        // (see lithe_codegen_interpreter.hpp: "opcode is unsupported by interpreter
        // backend"). Functions lowered to structured_for hit this; it is non-fatal —
        // the fn is lowered + verified but produces no scalar value. See execution.md §3.
        inline constexpr std::string_view k_interp_cfg_limit = "unsupported by interpreter backend";
        // Emitted by execute_with_fallback when the primary backend is invalid and the
        // interpreter fallback fires. These are execution-trace notes, not fatal errors.
        inline constexpr std::string_view k_fallback_trace_pfx = "execute_with_fallback:";
        inline constexpr std::string_view k_fallback_reason_pfx = "fallback-reason:";

        // A diagnostic is a non-fatal note (not a fatal error) if it reports the
        // interpreter's control-flow limitation or a backend fallback trace.
        [[nodiscard]] inline bool is_nonfatal_interp_note(std::string_view diag) noexcept {
            return diag.find(k_interp_cfg_limit) != std::string_view::npos
                || diag.starts_with(k_fallback_trace_pfx)
                || diag.starts_with(k_fallback_reason_pfx);
        }

        // Returns true if the physical MIR contains branch/branch_cond opcodes.
        // Such functions have CFG control flow that the interpreter cannot follow;
        // they should skip interpreter execution and return ok() with no scalar value.
        [[nodiscard]] inline bool has_control_flow(
            const lithe::codegen::mir::physical_mir_function& fn) noexcept {
            for (const auto& blk : fn.function.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (inst.op == lithe::codegen::opcode::branch ||
                        inst.op == lithe::codegen::opcode::branch_cond)
                        return true;
                }
            }
            return false;
        }

        // Count instructions across all blocks of a physical MIR function.
        [[nodiscard]] inline std::uint32_t
        count_instrs(const lithe::codegen::mir::physical_mir_function& fn) noexcept {
            std::uint32_t n = 0;
            for (const auto& blk : fn.function.blocks)
                n += static_cast<std::uint32_t>(blk.instructions.size());
            return n;
        }

        // Count branch/branch_cond opcodes — a structural CFG-complexity signal for
        // debuggers and editors (how much control flow a function contains).
        [[nodiscard]] inline std::uint32_t
        count_branches(const lithe::codegen::mir::physical_mir_function& fn) noexcept {
            std::uint32_t n = 0;
            for (const auto& blk : fn.function.blocks)
                for (const auto& inst : blk.instructions)
                    if (inst.op == lithe::codegen::opcode::branch ||
                        inst.op == lithe::codegen::opcode::branch_cond)
                        ++n;
            return n;
        }

        // Count physical MIR basic blocks.
        [[nodiscard]] inline std::uint32_t
        count_blocks(const lithe::codegen::mir::physical_mir_function& fn) noexcept {
            return static_cast<std::uint32_t>(fn.function.blocks.size());
        }

        [[nodiscard]] inline std::uint64_t exec_fnv1a_u64(std::uint64_t seed, std::uint64_t v) noexcept {
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            for (int i = 0; i < 8; ++i) {
                seed ^= (v >> (i * 8)) & 0xFFu;
                seed *= kPrime;
            }
            return seed;
        }

        [[nodiscard]] inline std::uint64_t exec_fnv1a_i64(std::uint64_t seed, std::int64_t v) noexcept {
            return exec_fnv1a_u64(seed, static_cast<std::uint64_t>(v));
        }

        [[nodiscard]] inline std::uint64_t
        fingerprint_physical_mir(const lithe::codegen::mir::physical_mir_function& fn) noexcept {
            constexpr std::uint64_t kOffset = 14695981039346656037ULL;
            std::uint64_t h = kOffset;
            h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(fn.function.cfg.entry_block));
            h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(fn.function.blocks.size()));

            for (const auto& blk : fn.function.blocks) {
                h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(blk.id));
                h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(blk.instructions.size()));
                for (const auto& inst : blk.instructions) {
                    h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(inst.id));
                    h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(inst.op));
                    h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(inst.defs.size()));
                    for (const auto& d : inst.defs) {
                        h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(d.type));
                        if (d.type == lithe::codegen::allocated_operand::kind::preg) {
                            if (const auto* p = std::get_if<lithe::codegen::preg>(&d.value))
                                h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(p->id));
                        }
                    }

                    h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(inst.uses.size()));
                    for (const auto& u : inst.uses) {
                        h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(u.type));
                        switch (u.type) {
                        case lithe::codegen::allocated_operand::kind::preg:
                            if (const auto* p = std::get_if<lithe::codegen::preg>(&u.value))
                                h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(p->id));
                            break;
                        case lithe::codegen::allocated_operand::kind::immediate_i64:
                            if (const auto* i = std::get_if<std::int64_t>(&u.value))
                                h = exec_fnv1a_i64(h, *i);
                            break;
                        case lithe::codegen::allocated_operand::kind::block:
                            if (const auto* b = std::get_if<std::uint32_t>(&u.value))
                                h = exec_fnv1a_u64(h, static_cast<std::uint64_t>(*b));
                            break;
                        default:
                            break;
                        }
                    }
                }
            }

            return h;
        }

        // Run the interpreter, capture return value.
        [[nodiscard]] inline crank_execute_result
        run_interpreter(const lithe::codegen::mir::physical_mir_function& phys_fn,
                        const std::vector<std::int64_t>& args,
                        const execute_options& opts) {
            using namespace lithe::codegen;
            using namespace lithe::codegen::backends;

            crank_execute_result res;
            res.stats.instr_count = detail::count_instrs(phys_fn);
            res.stats.branch_count = detail::count_branches(phys_fn);
            res.stats.block_count = detail::count_blocks(phys_fn);

            auto t0 = std::chrono::steady_clock::now();

            interpreter_backend interp;
            interp.arguments = args;
            interp.reset_runtime_state();

            auto art = interp.emit(phys_fn);

            auto t1 = std::chrono::steady_clock::now();
            res.stats.execute_ns = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
            );

            // Propagate diagnostics — route the interpreter's non-fatal control-flow
            // limitation into notes so it does not flip res.ok(). See execution.md §3.
            for (const auto& d : art.diagnostics) {
                if (is_nonfatal_interp_note(d)) res.notes.push_back(d);
                else res.diagnostics.push_back(d);
            }
            for (const auto& rd : interp.runtime_diagnostics) {
                if (is_nonfatal_interp_note(rd)) res.notes.push_back(rd);
                else res.diagnostics.push_back(rd);
            }

            if (interp.return_value)
                res.return_value = *interp.return_value;

            // Overflow trap check: if overflow_checked and a halted state recorded, mark.
            if (opts.overflow_checked && interp.halted) {
                res.overflow_trapped = true;
                if (opts.safety_policy == safety_failure::trap
                    || opts.safety_policy == safety_failure::terminate) {
                    res.return_value = std::nullopt;
                }
            }

            return res;
        }

        [[nodiscard]] bool
        should_use_native(const lithe::codegen::mir::physical_mir_function& phys,
                          const execute_options& opts) noexcept;
    } // namespace detail

    // ============================================================================
    // lower_to_physical — phase 1: HL MIR → physical MIR (cached)
    //
    // Runs coordinate_lowering_pass on first call and caches the physical MIR in
    // hl_res.cached_phys; subsequent calls return the cached function with
    // lower_ns == 0. Callers wanting execute-only timing lower once here, then feed
    // *result.phys into execute_physical repeatedly. See §3.6.
    // ============================================================================

    [[nodiscard]] inline lower_phase_result
    lower_to_physical(const lower_hl_result& hl_res) {
        using namespace lithe::codegen::hl;

        lower_phase_result res;
        res.hint = hl_res.exec_hint; // propagate hint for callers

        if (!hl_res.ok()) {
            res.diagnostics = hl_res.diagnostics;
            return res;
        }

        if (!hl_res.cached_phys.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            coordinate_lowering_pass lower_pass;
            auto lower_result = lower_pass.run(hl_res.hl_fn);
            lower_result.fn.metadata.current_phase = lithe::codegen::mir::phase::physical_mir;
            auto t1 = std::chrono::steady_clock::now();
            res.lower_ns = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
            );
            for (const auto& d : lower_result.diagnostics)
                res.diagnostics.push_back(d);
            if (!res.diagnostics.empty()) return res;
            // Verify once here; set verified=true so subsequent execute_physical
            // calls skip redundant re-verification (C-3 fix).
            const auto vr = lithe::codegen::verify_physical_mir(lower_result.fn);
            for (const auto& d : vr.diagnostics)
                res.diagnostics.push_back(d);
            if (!res.diagnostics.empty()) return res;
            lower_result.fn.verified = true;
            hl_res.cached_phys = std::move(lower_result.fn);
        }

        res.phys = &*hl_res.cached_phys;
        return res;
    }

    template <class Observer = ::lang::telemetry::phase_observer<>>
    [[nodiscard]] inline lower_phase_result
    lower_to_physical_observed(const lower_hl_result& hl_res,
                               const std::uint64_t unit_id = 0) {
        ::lang::telemetry::phase_scope<Observer> scope{{
            .unit_id = unit_id,
            .stage = ::lang::telemetry::phase::physical_lower,
        }};
        auto result = lower_to_physical(hl_res);
        scope.set_outcome(result.ok() ? ::lang::telemetry::phase_outcome::success
                                      : ::lang::telemetry::phase_outcome::failed);
        return result;
    }

    // ============================================================================
    // execute_physical — phase 2: run an already-lowered physical MIR
    //
    // Measures execute time only (stats.lower_ns stays 0). Path is selected by
    // execute_options::path:
    //   - interpreter_only: run interpreter backend
    //   - native_only: run compile()+invoke path
    //   - auto_select (default): prefer native for CFG-heavy / larger MIR, otherwise
    //     keep interpreter for tiny straight-line fragments.
    // Reusable across many calls on one phys fn.
    // ============================================================================

    [[nodiscard]] inline crank_execute_result
    execute_physical(const lithe::codegen::mir::physical_mir_function& phys,
                     const std::vector<std::int64_t>& args = {},
                     execute_options opts = {});

    // ============================================================================
    // execute_physical_native — native JIT execution of an already-lowered MIR
    //
    // Calls lithe::execution::compile (asmjit primary, interpreter fallback) then
    // lithe::execution::invoke.  Correct for both straight-line and CFG functions
    // — the native path follows branches so counted loops return a scalar value.
    //
    // fallback_fired=true in the result means asmjit was unavailable and the
    // interpreter ran instead.  The return value is valid either way.
    //
    // Existing execute_physical (interpreter) is unchanged for callers that want
    // the explicit interpreter path.
    // ============================================================================

    // Prepared execution retains a compiled artifact across calls.  It is the
    // hot-path API: preparation owns planning, fingerprinting, and cache lookup;
    // invocation is a direct typed call when native code is available.
    class prepared_native_execution {
    public:
        using native_i64_entry = lithe::execution::prepared_execution::native_i64_entry;

        prepared_native_execution(lithe::execution::prepared_execution prepared,
                                  execute_stats preparation_stats) noexcept
            : prepared_(std::move(prepared)), preparation_stats_(preparation_stats) {}

        [[nodiscard]] bool is_native() const noexcept { return prepared_.is_native(); }
        [[nodiscard]] bool fallback_fired() const noexcept { return prepared_.used_fallback(); }
        [[nodiscard]] native_i64_entry native_entry() const noexcept {
            return prepared_.native_entry();
        }
        [[nodiscard]] std::optional<std::int64_t>
        invoke(const std::span<const std::int64_t> args = {}) const noexcept {
            return prepared_.invoke(args);
        }
        template <class Observer = ::lang::telemetry::phase_observer<>>
        [[nodiscard]] std::optional<std::int64_t>
        invoke_observed(const std::span<const std::int64_t> args = {},
                        const std::uint64_t unit_id = 0) const noexcept {
            return prepared_.template invoke_observed<Observer>(args, unit_id);
        }
        [[nodiscard]] const execute_stats& preparation_stats() const noexcept {
            return preparation_stats_;
        }
        [[nodiscard]] const lithe::execution::compile_result& compilation() const noexcept {
            return prepared_.result();
        }

    private:
        lithe::execution::prepared_execution prepared_;
        execute_stats preparation_stats_;
    };

    template <class Observer = ::lang::telemetry::phase_observer<>>
    [[nodiscard]] inline prepared_native_execution
    prepare_physical_native(const lithe::codegen::mir::physical_mir_function& phys,
                            execute_options opts = {},
                            const std::uint64_t unit_id = 0) {
        execute_stats stats;
        stats.instr_count = detail::count_instrs(phys);
        stats.branch_count = detail::count_branches(phys);
        stats.block_count = detail::count_blocks(phys);

        lithe::execution::compile_request req;
        req.hint = opts.hint;
        req.policy = {}; // default auto_execution_policy

        // Compile cache key: stable hash of physical MIR shape + backend target.
        lithe::execution::aot_cache_key key;
        key.module_name = phys.function.name.empty() ? std::string{"crank_anon"} : phys.function.name;
        key.source_hash = detail::fingerprint_physical_mir(phys);
        key.backend_id = "asmjit";
        key.opt_profile_id = "crank.execute_physical_native";
        req.cache_key = key;

        static lithe::execution::artifact_store s_native_store;

        auto t0 = std::chrono::steady_clock::now();
        auto prepared = lithe::execution::prepare_observed<Observer>(
            phys, req, &s_native_store, unit_id);
        auto t1 = std::chrono::steady_clock::now();
        stats.execute_ns = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
        );

        return prepared_native_execution{std::move(prepared), stats};
    }

    [[nodiscard]] inline crank_execute_result
    execute_physical_native(const lithe::codegen::mir::physical_mir_function& phys,
                            const std::vector<std::int64_t>& args = {},
                            execute_options opts = {}) {
        auto prepared = prepare_physical_native(phys, opts);
        crank_execute_result res;
        res.stats = prepared.preparation_stats();
        res.fallback_fired = prepared.fallback_fired();
        res.stats.fallback_used = res.fallback_fired;

        for (const auto& d : prepared.compilation().diagnostics) {
            if (detail::is_nonfatal_interp_note(d)) res.notes.push_back(d);
            else res.diagnostics.push_back(d);
        }

        const std::span<const std::int64_t> arg_span{args};
        res.return_value = prepared.invoke(arg_span);

        return res;
    }

    namespace detail {
        // Heuristic: loops/CFG and larger MIR bodies are usually dispatch-bound in the
        // interpreter, so prefer native codegen when the caller leaves path=auto_select.
        [[nodiscard]] inline bool
        should_use_native(const lithe::codegen::mir::physical_mir_function& phys,
                          const execute_options& opts) noexcept {
            if (opts.path == execute_options::execution_path::native_only) return true;
            if (opts.path == execute_options::execution_path::jit_preferred) return true;
            if (opts.path == execute_options::execution_path::interpreter_only) return false;
            if (count_branches(phys) >= opts.hotness.branch_count) return true;
            if (count_blocks(phys) >= opts.hotness.block_count) return true;
            return count_instrs(phys) >= opts.hotness.instruction_count;
        }
    } // namespace detail

    [[nodiscard]] inline crank_execute_result
    execute_physical(const lithe::codegen::mir::physical_mir_function& phys,
                     const std::vector<std::int64_t>& args,
                     execute_options opts) {
        if (detail::should_use_native(phys, opts)) {
            return execute_physical_native(phys, args, opts);
        }
        return detail::run_interpreter(phys, args, opts);
    }

    // ============================================================================
    // execute_via_interpreter — HL MIR → physical MIR → selected local backend
    //
    // Convenience wrapper: lower_to_physical (phase 1) then execute_physical
    // (phase 2), merging both phases' timing into one crank_execute_result. The
    // default auto_select policy retains interpreter behavior for tiny fragments
    // but chooses native JIT for hot or CFG-heavy MIR. interpreter_only remains
    // available for verification and explicit fallback probes.
    // ============================================================================

    [[nodiscard]] inline crank_execute_result
    execute_via_interpreter(const lower_hl_result& hl_res,
                            const std::vector<std::int64_t>& args = {},
                            execute_options opts = {}) {
        crank_execute_result res;

        auto lp = lower_to_physical(hl_res);
        res.stats.lower_ns = lp.lower_ns;
        if (!lp.ok()) {
            res.diagnostics = std::move(lp.diagnostics);
            return res;
        }

        auto selected_res = execute_physical(*lp.phys, args, opts);

        res.return_value = selected_res.return_value;
        res.overflow_trapped = selected_res.overflow_trapped;
        res.fallback_fired = selected_res.fallback_fired;
        res.stats.execute_ns = selected_res.stats.execute_ns;
        res.stats.instr_count = selected_res.stats.instr_count;
        res.stats.branch_count = selected_res.stats.branch_count;
        res.stats.block_count = selected_res.stats.block_count;
        res.stats.fallback_used = selected_res.stats.fallback_used;
        for (auto& d : selected_res.diagnostics)
            res.diagnostics.push_back(std::move(d));
        for (auto& n : selected_res.notes)
            res.notes.push_back(std::move(n));

        return res;
    }

    // ============================================================================
    // execute_with_auto_fallback — try a named primary, fall back to interpreter
    //
    // If primary_backend_name is empty or unknown, goes directly to interpreter.
    // When fallback fires: emits a diagnostic note (NADI pulse on real NADI).
    // ============================================================================

    [[nodiscard]] inline crank_execute_result
    execute_with_auto_fallback(const lower_hl_result& hl_res,
                               const std::vector<std::int64_t>& args = {},
                               execute_options opts = {}) {
        using namespace lithe::codegen;
        using namespace lithe::codegen::backends;

        crank_execute_result res;
        if (!hl_res.ok()) {
            res.diagnostics = hl_res.diagnostics;
            return res;
        }

        // Lower HL → physical (cached, phase 1).
        auto lp = lower_to_physical(hl_res);
        res.stats.lower_ns = lp.lower_ns;
        for (auto& d : lp.diagnostics)
            res.diagnostics.push_back(std::move(d));
        if (!lp.ok()) return res;

        const auto& phys_fn = *lp.phys;
        res.stats.branch_count = detail::count_branches(phys_fn);
        res.stats.block_count = detail::count_blocks(phys_fn);

        // §v2.7 — CFG functions now execute through the CFG-aware interpreter
        // (branch/branch_cond are followed). No execution paths are skipped; the
        // branch/block stats above remain informative. The interpreter backend
        // advertises branches support, so execute_with_fallback below is legal for
        // CFG functions too.
        res.stats.instr_count = detail::count_instrs(phys_fn);

        if (opts.primary_backend_name.empty()) {
            // Auto path: choose interpreter vs native based on execute_options::path
            // and MIR shape. Native may still fallback internally if unavailable.
            auto picked = detail::should_use_native(phys_fn, opts)
                              ? execute_physical_native(phys_fn, args, opts)
                              : detail::run_interpreter(phys_fn, args, opts);

            res.return_value = picked.return_value;
            res.overflow_trapped = picked.overflow_trapped;
            res.stats.execute_ns = picked.stats.execute_ns;
            res.stats.instr_count = picked.stats.instr_count;
            res.fallback_fired = picked.fallback_fired;
            res.stats.fallback_used = picked.stats.fallback_used;
            for (auto& d : picked.diagnostics) res.diagnostics.push_back(std::move(d));
            for (auto& n : picked.notes) res.notes.push_back(std::move(n));
            return res;
        }

        // Try primary, fallback = interpreter
        auto primary_opt = backend_kind_from_string(opts.primary_backend_name);
        if (!primary_opt) {
            // Unknown primary — fall back silently (note recorded via fallback_fired flag)
            res.fallback_fired = true;
            auto ir = detail::run_interpreter(phys_fn, args, opts);
            res.return_value = ir.return_value;
            res.overflow_trapped = ir.overflow_trapped;
            res.stats.execute_ns = ir.stats.execute_ns;
            res.stats.instr_count = ir.stats.instr_count;
            res.stats.fallback_used = true;
            for (auto& d : ir.diagnostics) res.diagnostics.push_back(std::move(d));
            for (auto& n : ir.notes) res.notes.push_back(std::move(n));
            return res;
        }

        backend_variant primary = make_backend(*primary_opt);
        backend_variant fallback = make_backend(backend_kind::interpreter);

        auto t2 = std::chrono::steady_clock::now();
        auto art = lithe::codegen::backends::execute_with_fallback(phys_fn, primary, fallback);
        auto t3 = std::chrono::steady_clock::now();
        res.stats.execute_ns = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count()
        );
        res.stats.instr_count = detail::count_instrs(phys_fn);

        // Check if fallback was used (fallback fires emit a diagnostic with stage tag).
        // Route interpreter CFG-limit messages to notes — they are non-fatal.
        for (const auto& d : art.diagnostics) {
            if (d.find("execute_with_fallback") != std::string::npos) {
                res.fallback_fired = true;
                res.stats.fallback_used = true;
            }
            if (detail::is_nonfatal_interp_note(d)) res.notes.push_back(d);
            else res.diagnostics.push_back(d);
        }

        // Extract return value from artifact.
        // For native artifacts (jit_function): call the handle directly — this is correct
        // for both straight-line and CFG functions (native follows branches).
        // For interpreter fallback: read from artifact metadata.
        // Previously this block re-ran the interpreter on straight-line fns and skipped
        // CFG fns entirely (C-4 fix: counted loops now return a scalar via native path).
        if (art.ok()) {
            const std::span<const std::int64_t> arg_span{args};
            // Build a minimal compile_result wrapping the existing artifact so invoke()
            // can dispatch correctly via is_native() / metadata fallback.
            lithe::execution::compile_result cr;
            cr.artifact = std::move(art);
            cr.fallback_fired = res.fallback_fired;
            res.return_value = lithe::execution::invoke(cr, arg_span);
            // If native invoke didn't return a value but a scalar is in the metadata,
            // invoke() already handled that. Overflow check is interpreter-only.
        }

        return res;
    }
} // namespace crank
