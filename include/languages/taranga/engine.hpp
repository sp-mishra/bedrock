#pragma once

// taranga/engine.hpp — One-call facade over the taranga pipeline.
//
// C++23, header-only, no virtual, no macros. Namespace: taranga
//
// The embedder surface. Everything upstream (frontend → validate → build_ssa →
// lower_to_hl) produces a live HL MIR function per Wasm function; this band takes
// that HL MIR the rest of the way to a callable result by driving the SAME Lithe
// primitives crank drives — coordinate_lowering_pass → verify_physical_mir →
// lithe::execution::compile → lithe::execution::invoke. No stage is reimplemented:
// the engine is orchestration + diagnostics collation + an ergonomic result type.
//
// Why the free compile/invoke path (and not basic_lithe_engine or freeze): crank's
// execution — the precedent this mirrors — lowers a live hl_mir_function to a
// physical_mir_function with coordinate_lowering_pass, verifies it, then calls the
// free lithe::execution::compile/invoke on that physical MIR. freeze_function is an
// interchange/AOT-key path (see aot.hpp), NOT the execution path. taranga's
// lower_result carries only the live hl_fn, so the engine runs those same
// primitives directly per function.
//
// Primary types:
//   engine          — one-call entry: eval / compile
//   engine_options  — execution-hint / diagnostics knobs
//   program         — compiled, reusable handle (validate+lower once, invoke many)
//   value           — host-facing typed result wrapper (i64 domain in v1)
//   run_report      — value + diagnostics + which function ran
//   taranga_error   — failure with stage + diagnostics
//
// Quick Start:
//   taranga::engine e;
//   auto r = e.eval("(module (func (export \"f\") (result i32) i32.const 42))");
//   if (r) { std::int64_t v = r->value.as_i64(); }
//
// Staged path:
//   auto prog = e.compile(src);
//   if (prog) { auto r = prog->invoke("f", {}); }
//
// Design: the module (and thus its validated_module token) is owned by program, so
// the proof token stays valid for the program's whole lifetime. The lowered HL MIR
// is also held by program; each invoke lowers-to-physical + compiles + invokes the
// selected function. v1 is straight-line, integer-domain (Lithe's invoke ABI is
// span<const int64>), matching the frontend/ssa/lower coverage tiers.

#include "languages/taranga/frontend.hpp"
#include "languages/taranga/lower_hl.hpp"
#include "languages/taranga/module_view.hpp"
#include "languages/taranga/ssa_build.hpp"
#include "languages/taranga/validate.hpp"

#include "lithe/lithe_codegen.hpp"            // verify_physical_mir, mir::phase
#include "lithe/lithe_codegen_hl_passes.hpp"  // coordinate_lowering_pass
#include "lithe/lithe_execution/compile.hpp"  // execution::compile / invoke

#include "vakya/diagnostics.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taranga {

    // =========================================================================
    // engine_options — execution + diagnostics knobs.
    //
    // These are policy, not feature toggles: the execution_hint biases Lithe's
    // backend planner (asmjit-native preferred, interpreter fallback) exactly as
    // the free compile() consults it; collect_warnings decides whether non-fatal
    // upstream diagnostics (e.g. deferred prelude expansions) surface in reports.
    // =========================================================================

    struct engine_options {
        lithe::exec::execution_hint hint{};       // backend bias for compile()
        lithe::exec::auto_execution_policy policy{}; // planner policy
        bool collect_warnings = true;             // carry warnings into reports
    };

    // =========================================================================
    // value — host-facing result. v1 executes in Lithe's int64 invoke ABI, so a
    // produced result is an i64 (nullopt for void / trap / non-native fail). The
    // wrapper keeps the door open for wider typing without changing call sites.
    // =========================================================================

    struct value {
        std::optional<std::int64_t> bits;

        [[nodiscard]] bool has_value() const noexcept { return bits.has_value(); }
        [[nodiscard]] std::int64_t as_i64() const noexcept { return bits.value_or(0); }
        [[nodiscard]] static value none() noexcept { return {}; }
        [[nodiscard]] static value of(std::int64_t v) noexcept { return {v}; }
    };

    // Which pipeline stage a failure originated in — lets a caller (and a test)
    // discriminate a parse error from a validation error from a lowering/verify
    // error without string-matching diagnostic codes.
    enum class stage : std::uint8_t {
        frontend, validate, ssa, lower, coordinate_lowering, verify, compile, invoke,
        lookup, // requested export/function not found
    };

    [[nodiscard]] constexpr std::string_view to_string(stage s) noexcept {
        switch (s) {
        case stage::frontend:            return "frontend";
        case stage::validate:            return "validate";
        case stage::ssa:                 return "ssa";
        case stage::lower:               return "lower";
        case stage::coordinate_lowering: return "coordinate_lowering";
        case stage::verify:              return "verify";
        case stage::compile:             return "compile";
        case stage::invoke:              return "invoke";
        case stage::lookup:              return "lookup";
        }
        return "frontend";
    }

    // A failure: the stage it happened in plus the collected diagnostics. Held in
    // the error channel of the std::expected the engine returns.
    struct taranga_error {
        stage where = stage::frontend;
        vakya::diag::collecting_sink diagnostics;
    };

    // A successful run: the produced value, the function that ran, and any
    // non-fatal diagnostics accumulated along the way.
    struct run_report {
        value result;
        std::string function;
        vakya::diag::collecting_sink diagnostics;
        [[nodiscard]] bool has_value() const noexcept { return result.has_value(); }
    };

    namespace detail {

        // Copy every entry of `from` into `into` — used to collate diagnostics from
        // the per-stage sinks into a single report/error sink.
        inline void merge_diagnostics(vakya::diag::collecting_sink& into,
                                      const vakya::diag::collecting_sink& from) {
            for (const auto& d : from.entries) into.on_diagnostic(d);
        }

        // Drive one lowered HL function the rest of the way to a value, running the
        // same Lithe primitives crank runs. Returns the invoke result (nullopt for
        // void/trap) on success; on a lowering/verify/compile failure returns the
        // failing stage and appends its diagnostics to `sink`.
        [[nodiscard]] inline std::expected<std::optional<std::int64_t>, stage>
        execute_lowered(const lowered_function& lf,
                        std::span<const std::int64_t> args,
                        const engine_options& opts,
                        vakya::diag::collecting_sink& sink) {
            namespace hl = lithe::codegen::hl;
            namespace cg = lithe::codegen;

            // 1. Live HL MIR → physical MIR via the coordinate-lowering pass.
            hl::coordinate_lowering_pass lower_pass;
            auto lr = lower_pass.run(lf.hl_fn);
            if (!lr.ok()) {
                for (const auto& msg : lr.diagnostics)
                    sink.on_diagnostic(vakya::diag::make_error(
                        "TARANGA-ENGINE-010",
                        "coordinate lowering of '" + lf.name + "': " + msg));
                return std::unexpected(stage::coordinate_lowering);
            }

            // 2. Stamp the phase and verify the physical MIR (Lithe's structural
            //    single-source-of-truth check over CFG/SSA/type/region).
            lr.fn.metadata.current_phase = cg::mir::phase::physical_mir;
            auto ver = cg::verify_physical_mir(lr.fn);
            if (!ver.ok()) {
                for (const auto& msg : ver.diagnostics)
                    sink.on_diagnostic(vakya::diag::make_error(
                        "TARANGA-ENGINE-020",
                        "verify '" + lf.name + "': " + msg));
                return std::unexpected(stage::verify);
            }
            lr.fn.verified = true;

            // 3. Compile through the free execution path (asmjit-native preferred,
            //    interpreter fallback — the planner decides from the hint/policy).
            lithe::execution::compile_request req;
            req.hint = opts.hint;
            req.policy = opts.policy;
            auto cr = lithe::execution::compile(lr.fn, req);
            if (!cr.diagnostics.empty())
                for (const auto& msg : cr.diagnostics)
                    sink.on_diagnostic(vakya::diag::make_warning(
                        "TARANGA-ENGINE-030",
                        "compile '" + lf.name + "': " + msg));

            // 4. Invoke. nullopt is a legitimate outcome (void return or trap); it
            //    is not an error channel — the report simply carries value::none().
            auto rv = lithe::execution::invoke(cr, args);
            return rv;
        }

    } // namespace detail

    // =========================================================================
    // program — a compiled, reusable handle. Owns the module (keeping the
    // validated_module token alive) and the lowered HL MIR. invoke() picks a
    // function by exported name (or by index) and runs it with the given args.
    // =========================================================================

    class program {
    public:
        // Non-copyable (owns move-only HL MIR + a validated_module bound to our
        // module storage); movable so engine::compile can hand it back by value.
        program(const program&) = delete;
        program& operator=(const program&) = delete;
        program(program&&) noexcept = default;
        program& operator=(program&&) noexcept = default;

        // Run an exported function by its export name. Returns a run_report on
        // success, or a taranga_error naming the failing stage.
        [[nodiscard]] std::expected<run_report, taranga_error>
        invoke(std::string_view export_name,
               std::span<const std::int64_t> args = {}) const {
            auto idx = function_index_for_export(export_name);
            if (!idx) {
                taranga_error err;
                err.where = stage::lookup;
                err.diagnostics.on_diagnostic(vakya::diag::make_error(
                    "TARANGA-ENGINE-001",
                    "no exported function named '" + std::string(export_name) + "'"));
                return std::unexpected(std::move(err));
            }
            return invoke_index(*idx, args);
        }

        // Run a lowered function by its position in the lowering result. Useful for
        // modules with a single anonymous function or when driving by index.
        [[nodiscard]] std::expected<run_report, taranga_error>
        invoke_index(std::size_t index,
                     std::span<const std::int64_t> args = {}) const {
            if (index >= lowered_.functions.size()) {
                taranga_error err;
                err.where = stage::lookup;
                err.diagnostics.on_diagnostic(vakya::diag::make_error(
                    "TARANGA-ENGINE-002",
                    "function index " + std::to_string(index) + " out of range"));
                return std::unexpected(std::move(err));
            }
            const auto& lf = lowered_.functions[index];

            run_report rep;
            rep.function = lf.name;
            // Carry forward any non-fatal upstream diagnostics (e.g. deferred
            // prelude expansions) so a caller sees the full picture.
            if (opts_.collect_warnings)
                detail::merge_diagnostics(rep.diagnostics, lowered_.diagnostics);

            auto res = detail::execute_lowered(lf, args, opts_, rep.diagnostics);
            if (!res) {
                taranga_error err;
                err.where = res.error();
                detail::merge_diagnostics(err.diagnostics, rep.diagnostics);
                return std::unexpected(std::move(err));
            }
            rep.result = value{*res};
            return rep;
        }

        // The functions available to invoke, in lowering order.
        [[nodiscard]] std::size_t function_count() const noexcept {
            return lowered_.functions.size();
        }
        [[nodiscard]] std::string_view function_name(std::size_t i) const noexcept {
            return i < lowered_.functions.size()
                       ? std::string_view(lowered_.functions[i].name)
                       : std::string_view{};
        }

    private:
        friend class engine;

        // Constructed only by engine::compile once the pipeline has succeeded. Owns
        // the module storage so the validated_module token (a pointer into it) and
        // the export table stay valid for the program's lifetime.
        program(std::unique_ptr<taranga_module> mod, lower_result lowered,
                engine_options opts)
            : module_(std::move(mod)),
              lowered_(std::move(lowered)),
              opts_(std::move(opts)) {}

        // Resolve an export name to the index of the matching lowered function.
        // Exports name a funcidx in the Wasm index space (imports precede defined
        // functions); the lowered vector is in defined-function order, so we map
        // the export's funcidx back to a defined-function position.
        //
        // The funcidx read here (export node ext.immediate) is authoritative for
        // the binary frontend. In v1 WAT intake the top-level (export "f" (func …))
        // funcidx is not yet resolved upstream, so a named WAT export may not
        // resolve — a caller drives WAT modules by index (eval with no name → the
        // first defined function). This fails closed (nullopt → lookup error),
        // never a wrong-function miscompile.
        [[nodiscard]] std::optional<std::size_t>
        function_index_for_export(std::string_view export_name) const {
            module_view view(*module_);
            const std::uint32_t imported = view.imported_function_count();
            for (auto eid : view.exports()) {
                const auto& ex = view.node(eid);
                if (ex.ext.head != "func") continue;
                if (std::string_view(ex.ext.text) != export_name) continue;
                const std::uint32_t funcidx = ex.ext.immediate;
                if (funcidx < imported) return std::nullopt; // imported: no body
                const std::size_t defined = funcidx - imported;
                if (defined < lowered_.functions.size()) return defined;
                return std::nullopt;
            }
            return std::nullopt;
        }

        std::unique_ptr<taranga_module> module_;
        lower_result lowered_;
        engine_options opts_;
    };

    // =========================================================================
    // engine — the one-call entry. Holds options; each compile/eval runs the full
    // frontend → validate → ssa → lower pipeline and, on success, hands back a
    // program (compile) or runs a chosen function immediately (eval).
    // =========================================================================

    class engine {
    public:
        engine() = default;
        explicit engine(engine_options opts) : opts_(std::move(opts)) {}

        [[nodiscard]] const engine_options& options() const noexcept { return opts_; }
        void set_options(engine_options opts) { opts_ = std::move(opts); }

        // Compile a WAT/binary image into a reusable program. On any upstream error
        // returns a taranga_error naming the stage; the program owns its module so
        // the returned handle is fully self-contained.
        [[nodiscard]] std::expected<program, taranga_error>
        compile(std::span<const std::uint8_t> image) const {
            return compile_module(frontend::compile(image));
        }
        [[nodiscard]] std::expected<program, taranga_error>
        compile(std::string_view text) const {
            return compile_module(frontend::compile(text));
        }

        // Parse + validate + lower + run one function in a single call. Picks the
        // named export if given, else the first defined function. Returns its
        // run_report or the failing stage.
        [[nodiscard]] std::expected<run_report, taranga_error>
        eval(std::string_view text, std::string_view export_name = {},
             std::span<const std::int64_t> args = {}) const {
            auto prog = compile(text);
            if (!prog) return std::unexpected(std::move(prog.error()));
            if (!export_name.empty()) return prog->invoke(export_name, args);
            if (prog->function_count() == 0) {
                taranga_error err;
                err.where = stage::lookup;
                err.diagnostics.on_diagnostic(vakya::diag::make_error(
                    "TARANGA-ENGINE-003", "module has no defined functions to eval"));
                return std::unexpected(std::move(err));
            }
            return prog->invoke_index(0, args);
        }

    private:
        // Shared tail of both compile() overloads: take a frontend_result to a
        // program, threading diagnostics + stage tags through each gate.
        [[nodiscard]] std::expected<program, taranga_error>
        compile_module(frontend::frontend_result fr) const {
            // Own the module so the validated_module token (a pointer into it) and
            // the export table outlive this function.
            auto mod = std::make_unique<taranga_module>(std::move(fr.module));

            if (!mod->ok()) {
                taranga_error err;
                err.where = stage::frontend;
                detail::merge_diagnostics(err.diagnostics, mod->diagnostics);
                return std::unexpected(std::move(err));
            }

            auto [vr, token] = validate(*mod);
            if (!token) {
                taranga_error err;
                err.where = stage::validate;
                detail::merge_diagnostics(err.diagnostics, vr.diagnostics);
                return std::unexpected(std::move(err));
            }

            auto ssa = build_ssa(token->view());
            if (!ssa.ok()) {
                taranga_error err;
                err.where = stage::ssa;
                detail::merge_diagnostics(err.diagnostics, ssa.diagnostics);
                return std::unexpected(std::move(err));
            }

            auto lowered = lower_to_hl(*token, ssa);
            if (!lowered.ok()) {
                taranga_error err;
                err.where = stage::lower;
                detail::merge_diagnostics(err.diagnostics, lowered.diagnostics);
                return std::unexpected(std::move(err));
            }

            return program(std::move(mod), std::move(lowered), opts_);
        }

        engine_options opts_{};
    };

} // namespace taranga
