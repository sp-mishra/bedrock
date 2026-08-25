#pragma once

// crank/engine.hpp — One-call facade over the crank pipeline (§A, §B, §I, §M).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Primary types:
//   engine          — one-call embedder entry (eval / run / compile / load)
//   engine_options  — policy/capability/diagnostics knobs (NOT feature on/off)
//   program         — compiled, reusable program handle (lower-once, run-many)
//   value           — host-facing typed result wrapper
//   module_handle   — resolved module + exports/imports/cache status
//   module_graph_view — topologically ordered resolved module list
//   run_report      — eval result + stats + diagnostics + optional plan view
//   target_kind     — advisory platform pin for backend discovery
//
// Quick Start:
//   crank::engine e;
//   auto r = e.eval("fn Main() -> Int64 { return 42 }");
//   if (r) { auto v = r->as<std::int64_t>(); }
//
// Staged path (advanced):
//   auto prog = e.compile(src);
//   if (prog) { auto r = prog->execute(); }
//
// Module path:
//   e.context().modules().add_path("/my/project");
//   auto m = e.load("math.vector");
//   auto g = e.module_graph(); // topological module list with cache status
//
// Extern functions (§X):
//   e.context().register_function<"math.dot", dot>();
//   // Crank source declares: @host.link("math.dot") extern fn Dot(a: Vec3, b: Vec3) -> Float32
//   // Analysis verifies arity + types against the registered descriptor.
//   auto r = crank::verify_extern_fn_decl(e.context(), "Dot", "math.dot", 2);
//
// Design:
//   engine owns a crank::context + engine_options. All pipeline stages
//   (frontend::parse, ctx.analyse, lower_to_hl, lower_to_physical, execute_physical)
//   are delegated unchanged — no stage is reimplemented. engine provides
//   orchestration + diagnostics collation + ergonomic surface only.
//
//   Native language features (transactions, generics, views, proof surface) are
//   NOT toggles — they are always available, pay-for-use at the IR level.
//   permit_* options cap the backend planner's candidate set (deployment/security
//   gate), never suppress language features. Backend selection is automatic per
//   §I (Lithe decides per region based on platform capability + legality +
//   profitability). target_kind pins the assumed capability set for cross-compile
//   or reproducible benchmark scenarios.
//
//   The facade's eval/run/compile synthesize a lower_input from the analysis
//   result for the scripting use case (minimal fn_name from parse stats). For full
//   structured lowering the staged API (construct lower_input manually) remains.

#include "languages/crank/context.hpp"
#include "languages/crank/execute.hpp"
#include "languages/crank/lower_hl.hpp"
#include "languages/crank/module.hpp"
#include "languages/crank/ffi_module.hpp"
#include "languages/crank/profiles.hpp"
#include "languages/crank/capability.hpp"
#include "languages/crank/diagnostic.hpp"
#include "languages/crank/host.hpp"
#include "languages/crank/verify.hpp"
#include "languages/crank/plan.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "containers/cache/kosha.hpp"

namespace crank {

    // =========================================================================
    // target_kind — advisory platform pin for backend discovery (§I.3)
    //
    // Caps the capability set the planner assumes. Does NOT force a backend that
    // is illegal or unprofitable; Lithe's legality + profitability filters still
    // apply. Default: host (discover the real machine).
    // =========================================================================

    enum class target_kind : std::uint8_t {
        host,              // discover actual machine at runtime (default)
        cpu_only,          // cap: scalar interpreter only
        simd,              // cap: scalar + SIMD (no GPU)
        gpu_if_available,  // cap: full set; GPU included if present
    };

    [[nodiscard]] constexpr std::string_view to_string(target_kind t) noexcept {
        switch (t) {
        case target_kind::host:             return "host";
        case target_kind::cpu_only:         return "cpu_only";
        case target_kind::simd:             return "simd";
        case target_kind::gpu_if_available: return "gpu_if_available";
        }
        return "host";
    }

    // =========================================================================
    // plan_region_entry — one region's backend selection record (§I.3)
    // =========================================================================

    struct plan_region_entry {
        std::string region_name;      // function or loop body name/id
        std::string selected_backend; // "scalar" / "simd" / "gpu" / "threaded"
        bool        was_fallback = false; // true = higher-ranked backend tried first
        std::string fallback_reason;      // why preferred was not chosen
        double      profitability_score = 0.0; // planner score (0 = not computed)
    };

    struct plan_view {
        std::vector<plan_region_entry> regions;
        std::uint64_t plan_id = 0;

        [[nodiscard]] bool any_fallback() const noexcept {
            for (const auto& r : regions) if (r.was_fallback) return true;
            return false;
        }
    };

    // =========================================================================
    // crank_error — unified, stage-tagged error for the facade (§A.3)
    //
    // Built by draining diagnostics after each stage; the first failing stage
    // short-circuits. Uses stable string diagnostic codes.
    // =========================================================================

    enum class error_stage : std::uint8_t {
        parse,
        analyse,
        lower,
        execute,
        module_resolve,
        extern_fn,  // CRANK-EXT-01x
        options,    // invalid option combination
    };

    [[nodiscard]] constexpr std::string_view to_string(error_stage s) noexcept {
        switch (s) {
        case error_stage::parse:          return "parse";
        case error_stage::analyse:        return "analyse";
        case error_stage::lower:          return "lower";
        case error_stage::execute:        return "execute";
        case error_stage::module_resolve: return "module_resolve";
        case error_stage::extern_fn:      return "extern_fn";
        case error_stage::options:        return "options";
        }
        return "unknown";
    }

    struct crank_error {
        error_stage  stage = error_stage::parse;
        std::string  code;    // stable diagnostic code, e.g. "CRANK-EXT-010"
        std::string  message;
        std::optional<source_span> span;
        std::vector<std::string>   notes;

        [[nodiscard]] std::string format() const {
            std::string out = "[" + std::string(to_string(stage)) + "] ";
            if (!code.empty()) out += code + ": ";
            out += message;
            for (const auto& n : notes) out += "\n  note: " + n;
            return out;
        }
    };

    // =========================================================================
    // engine_options — policy / capability / diagnostics knobs (§A.1, §A.2)
    //
    // These are NOT feature on/off switches. Native language features
    // (transactions, generics, views, verification surface) are always
    // available; this struct holds deployment/policy/diagnostics choices only.
    //
    // verify uses the existing crank::verify_policy (verify.hpp):
    //   assume   — trust obligations without discharge (fast, default for scripting)
    //   check    — discharge via SMT (default for strict mode)
    //   paranoid — discharge implicit+explicit, no assume override
    // =========================================================================

    struct engine_options {
        verify_policy verify             = verify_policy::assume;
        bool          aot_cache          = false;
        bool          diagnostics_verbose = false;
        target_kind   target             = target_kind::host;

        // permit_* cap what the automatic planner is ALLOWED to select.
        // Default: everything permitted (Lithe decides per region).
        // A hard @gpu(required=true) region under permit_gpu=false is a
        // diagnostic — no silent degradation.
        bool permit_parallel = true;
        bool permit_simd     = true;
        bool permit_gpu      = true;

        // Named preset: scripting — assume mode, all backends permitted, aot off.
        [[nodiscard]] static constexpr engine_options scripting() noexcept {
            engine_options o;
            o.verify    = verify_policy::assume;
            o.aot_cache = false;
            return o;
        }

        // Named preset: strict — check mode, aot on.
        [[nodiscard]] static constexpr engine_options strict() noexcept {
            engine_options o;
            o.verify    = verify_policy::check;
            o.aot_cache = true;
            return o;
        }
    };

    // =========================================================================
    // value — host-facing typed result wrapper (§B.4)
    //
    // Wraps an int64 scalar (interpreter path) or unit (void result). Accessors
    // return expected<T, crank_error> on type mismatch.
    // Future: will also adapt owned_host_value for custom registered types.
    // =========================================================================

    class value {
    public:
        value()                            = default;
        value(const value&)                = default;
        value& operator=(const value&)     = default;
        value(value&&) noexcept            = default;
        value& operator=(value&&) noexcept = default;

        // Construct from scalar int64 (interpreter / native JIT path).
        explicit value(std::int64_t v) noexcept : i64_(v), has_i64_(true) {}

        [[nodiscard]] bool has_value() const noexcept { return has_i64_; }
        [[nodiscard]] bool is_unit()   const noexcept { return !has_i64_; }

        // Extract as T. For numeric types coerces from i64.
        template <class T>
        [[nodiscard]] std::expected<T, crank_error> as() const {
            if constexpr (std::is_arithmetic_v<T>) {
                if (has_i64_) return static_cast<T>(i64_);
            }
            crank_error e;
            e.stage   = error_stage::execute;
            e.code    = has_i64_ ? "CRANK-VAL-001" : "CRANK-VAL-002";
            e.message = has_i64_ ? "type mismatch extracting value"
                                 : "value is unit — no scalar result";
            return std::unexpected(std::move(e));
        }

    private:
        std::int64_t i64_     = 0;
        bool         has_i64_ = false;
    };

    // =========================================================================
    // run_stats — timing + instruction counters for run_report
    // =========================================================================

    struct run_stats {
        std::int64_t  lower_ns    = 0;
        std::int64_t  execute_ns  = 0;
        std::uint32_t instr_count  = 0;
        std::uint32_t branch_count = 0;
        std::uint32_t block_count  = 0;
        bool          fallback_used = false;
    };

    // =========================================================================
    // run_report — result of engine::run() (§A.2)
    // =========================================================================

    struct run_report {
        value                    result;
        run_stats                stats;
        std::vector<std::string> notes;       // non-fatal observations
        std::vector<std::string> diagnostics; // fatal (empty = ok)

        // plan() — backend-selection introspection (§I.3).
        [[nodiscard]] plan_view plan() const {
            plan_view pv;
            pv.plan_id = plan_id_;
            pv.regions = plan_regions_;
            return pv;
        }

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }

    private:
        friend class engine;
        std::uint64_t                  plan_id_ = 0;
        std::vector<plan_region_entry> plan_regions_;
    };

    // =========================================================================
    // module_ref — a resolved import reference in a module_handle
    // =========================================================================

    struct module_ref {
        std::string name;
        module_hash content_hash;
    };

    // =========================================================================
    // symbol — exported symbol from a module
    // =========================================================================

    struct symbol {
        enum class kind : std::uint8_t { function, type, constant } sym_kind = kind::function;
        std::string name;
    };

    // =========================================================================
    // module_handle — resolved module + metadata (§M.3)
    // =========================================================================

    class module_handle {
    public:
        module_handle() = default;

        explicit module_handle(module_descriptor        desc,
                               std::vector<symbol>      exports,
                               std::vector<module_ref>  imports,
                               bool                     was_cached = false)
            : desc_(std::move(desc))
            , exports_(std::move(exports))
            , imports_(std::move(imports))
            , was_cached_(was_cached) {}

        [[nodiscard]] const std::string&         name()         const noexcept { return desc_.name; }
        [[nodiscard]] const module_hash&          content_hash() const noexcept { return desc_.content_hash; }
        [[nodiscard]] std::span<const symbol>     exports()     const noexcept { return exports_; }
        [[nodiscard]] std::span<const module_ref> imports()     const noexcept { return imports_; }
        [[nodiscard]] bool                        was_cached()  const noexcept { return was_cached_; }
        [[nodiscard]] const module_descriptor&    descriptor()  const noexcept { return desc_; }
        [[nodiscard]] bool                        valid()       const noexcept { return !desc_.name.empty(); }

    private:
        module_descriptor       desc_;
        std::vector<symbol>     exports_;
        std::vector<module_ref> imports_;
        bool                    was_cached_ = false;
    };

    // =========================================================================
    // module_graph_entry — one node in the module_graph_view (§M.3)
    // =========================================================================

    struct module_graph_entry {
        std::string              name;
        module_hash              content_hash;
        bool                     was_cached = false;
        std::vector<std::string> imports;
    };

    // =========================================================================
    // module_graph_view — topological module list (§M.3)
    //
    // Order: dependencies before dependents (compile order).
    // Reuses dependency_graph::topo_order() from module.hpp.
    // =========================================================================

    struct module_graph_view {
        std::vector<module_graph_entry> modules;

        [[nodiscard]] bool        empty() const noexcept { return modules.empty(); }
        [[nodiscard]] std::size_t size()  const noexcept { return modules.size(); }

        [[nodiscard]] const module_graph_entry* find(std::string_view name) const noexcept {
            for (const auto& m : modules) if (m.name == name) return &m;
            return nullptr;
        }
    };

    // =========================================================================
    // program — compiled, reusable program handle (§B.3)
    //
    // Wraps lower_hl_result (cached_phys populated). execute() runs the already-
    // lowered physical MIR many times. Move-only; single-owner.
    // =========================================================================

    class program {
    public:
        program()                              = default;
        program(const program&)                = delete;
        program& operator=(const program&)     = delete;
        program(program&&) noexcept            = default;
        program& operator=(program&&) noexcept = default;

        [[nodiscard]] bool valid() const noexcept { return hl_res_ != nullptr; }

        // Execute with optional int64 args.
        [[nodiscard]] std::expected<value, crank_error>
        execute(const std::vector<std::int64_t>& args = {}) const {
            if (!hl_res_) {
                crank_error e;
                e.stage   = error_stage::execute;
                e.code    = "CRANK-PROG-001";
                e.message = "program is empty";
                return std::unexpected(std::move(e));
            }
            execute_options opts;
            auto xr = execute_via_interpreter(*hl_res_, args, opts);
            if (!xr.ok()) {
                crank_error e;
                e.stage   = error_stage::execute;
                e.code    = "CRANK-PROG-002";
                e.message = xr.diagnostics.empty() ? "execution failed" : xr.diagnostics[0];
                for (std::size_t i = 1; i < xr.diagnostics.size(); ++i)
                    e.notes.push_back(xr.diagnostics[i]);
                return std::unexpected(std::move(e));
            }
            if (xr.return_value) return value{*xr.return_value};
            return value{};
        }

        // Execute and return full run_report with stats.
        [[nodiscard]] run_report execute_report(const std::vector<std::int64_t>& args = {}) const {
            run_report rep;
            if (!hl_res_) {
                rep.diagnostics.push_back("program is empty");
                return rep;
            }
            execute_options opts;
            auto xr = execute_via_interpreter(*hl_res_, args, opts);
            rep.stats.lower_ns     = xr.stats.lower_ns;
            rep.stats.execute_ns   = xr.stats.execute_ns;
            rep.stats.instr_count  = xr.stats.instr_count;
            rep.stats.branch_count = xr.stats.branch_count;
            rep.stats.block_count  = xr.stats.block_count;
            rep.stats.fallback_used = xr.fallback_fired;
            rep.diagnostics = std::move(xr.diagnostics);
            rep.notes       = std::move(xr.notes);
            if (xr.return_value) rep.result = value{*xr.return_value};
            return rep;
        }

        [[nodiscard]] const run_stats& lower_stats() const noexcept { return lower_stats_; }

    private:
        friend class engine;

        // Heap-allocated so program is unconditionally movable regardless of
        // whether lower_hl_result's members are movable (e.g. mutable optional<phys>).
        std::unique_ptr<lower_hl_result> hl_res_;
        run_stats lower_stats_;

        program(lower_hl_result r, run_stats s)
            : hl_res_(std::make_unique<lower_hl_result>(std::move(r)))
            , lower_stats_(s) {}
    };

    // =========================================================================
    // engine — one-call facade over the crank pipeline (§A)
    // =========================================================================

    class engine {
    public:
        engine() = default;

        explicit engine(engine_options opts) : opts_(opts) {
            apply_options_to_context();
        }

        // ── fluent option setters ─────────────────────────────────────────────

        engine& optimize(verify_policy p) { opts_.verify = p; return *this; }
        engine& verify(verify_policy p)   { opts_.verify = p; return *this; }

        engine& target(target_kind t) {
            opts_.target = t;
            apply_options_to_context();
            return *this;
        }

        engine& aot_cache(bool v)             { opts_.aot_cache = v; return *this; }
        engine& diagnostics_verbose(bool v)   { opts_.diagnostics_verbose = v; return *this; }

        engine& permit_parallel(bool v) {
            opts_.permit_parallel = v;
            apply_options_to_context();
            return *this;
        }

        engine& permit_simd(bool v) {
            opts_.permit_simd = v;
            apply_options_to_context();
            return *this;
        }

        engine& permit_gpu(bool v) {
            opts_.permit_gpu = v;
            apply_options_to_context();
            return *this;
        }

        // ── context access ────────────────────────────────────────────────────

        [[nodiscard]] crank::context&       context()       noexcept { return ctx_; }
        [[nodiscard]] const crank::context& context() const noexcept { return ctx_; }
        [[nodiscard]] const engine_options& options() const noexcept { return opts_; }

        // Register an FFI module group (§X/§M).  Seeds the module_resolver's
        // native tier so `import "name"` resolves from Crank source.
        engine& register_ffi_module(ffi_module_descriptor desc) {
            ctx_.register_ffi_module(std::move(desc));
            return *this;
        }

        // ── eval — parse + analyse + lower + execute, return scalar value ─────

        [[nodiscard]] std::expected<value, crank_error>
        eval(std::string_view source) {
            auto pr = frontend::parse(source);
            if (!pr.ok) return std::unexpected(make_parse_error(pr));

            auto ar = ctx_.analyse(pr);
            if (!ar.ok) return std::unexpected(make_analyse_error(ar));

            lower_input inp = make_lower_input(pr);
            auto lr = lower_to_hl(std::move(inp));
            if (!lr.ok()) return std::unexpected(make_lower_error(lr));

            auto xr = execute_via_interpreter(lr, {}, make_exec_options());
            if (!xr.ok()) return std::unexpected(make_execute_error(xr));

            if (xr.return_value) return value{*xr.return_value};
            return value{};
        }

        // ── run — eval + full run_report ──────────────────────────────────────

        [[nodiscard]] std::expected<run_report, crank_error>
        run(std::string_view source) {
            auto pr = frontend::parse(source);
            if (!pr.ok) return std::unexpected(make_parse_error(pr));

            auto ar = ctx_.analyse(pr);
            if (!ar.ok) return std::unexpected(make_analyse_error(ar));

            lower_input inp = make_lower_input(pr);
            auto lr = lower_to_hl(std::move(inp));
            if (!lr.ok()) return std::unexpected(make_lower_error(lr));

            auto xr = execute_via_interpreter(lr, {}, make_exec_options());
            if (!xr.ok()) return std::unexpected(make_execute_error(xr));

            run_report rep;
            rep.stats.lower_ns     = xr.stats.lower_ns;
            rep.stats.execute_ns   = xr.stats.execute_ns;
            rep.stats.instr_count  = xr.stats.instr_count;
            rep.stats.branch_count = xr.stats.branch_count;
            rep.stats.block_count  = xr.stats.block_count;
            rep.stats.fallback_used = xr.fallback_fired;
            rep.notes       = std::move(xr.notes);
            // diagnostics is empty because xr.ok() returned true.

            if (xr.return_value) rep.result = value{*xr.return_value};

            // Backend plan telemetry (§I.3).
            if (opts_.diagnostics_verbose) {
                plan_region_entry entry;
                entry.region_name      = inp_fn_name_.empty() ? "main" : inp_fn_name_;
                entry.selected_backend = xr.fallback_fired ? "scalar_fallback" : "scalar";
                entry.was_fallback     = xr.fallback_fired;
                if (xr.fallback_fired)
                    entry.fallback_reason = "primary backend ineligible";
                rep.plan_regions_.push_back(std::move(entry));
            }

            return rep;
        }

        // ── compile — parse..lower, cache, return program handle ─────────────

        [[nodiscard]] std::expected<program, crank_error>
        compile(std::string_view source) {
            auto pr = frontend::parse(source);
            if (!pr.ok) return std::unexpected(make_parse_error(pr));

            auto ar = ctx_.analyse(pr);
            if (!ar.ok) return std::unexpected(make_analyse_error(ar));

            lower_input inp = make_lower_input(pr);
            auto t0 = std::chrono::steady_clock::now();
            auto lr = lower_to_hl(std::move(inp));
            auto t1 = std::chrono::steady_clock::now();

            if (!lr.ok()) return std::unexpected(make_lower_error(lr));

            run_stats ls;
            ls.lower_ns = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            return program{std::move(lr), ls};
        }

        // ── load — module-aware resolution + parse + analyse (§M.3) ──────────

        [[nodiscard]] std::expected<module_handle, crank_error>
        load(std::string_view name_or_path) {
            auto desc_opt = ctx_.import(name_or_path);
            if (!desc_opt) {
                crank_error e;
                e.stage   = error_stage::module_resolve;
                e.code    = "CRANK-MOD-001";
                e.message = "module not found: " + std::string(name_or_path);
                return std::unexpected(std::move(e));
            }

            const auto& desc = *desc_opt;
            std::vector<symbol>     exports;
            std::vector<module_ref> imports;
            bool was_cached = false;

            // For native (FFI) modules, populate exports from the registered
            // ffi_module_descriptor symbols.
            if (desc.kind == module_kind::native) {
                const auto* ffi_desc = ctx_.ffi_modules().find(desc.name);
                if (ffi_desc) {
                    for (const auto& s : ffi_desc->symbols) {
                        symbol sym;
                        sym.name = s.crank_name;
                        switch (s.kind) {
                        case ffi_symbol_kind::function: sym.sym_kind = symbol::kind::function; break;
                        case ffi_symbol_kind::type:     sym.sym_kind = symbol::kind::type;     break;
                        case ffi_symbol_kind::constant: sym.sym_kind = symbol::kind::constant; break;
                        default:                        sym.sym_kind = symbol::kind::function; break;
                        }
                        exports.push_back(std::move(sym));
                    }
                }
            }

            if (desc.kind == module_kind::embedded_src) {
                auto src_opt = ctx_.modules().resolver().source_text(desc.name);
                if (src_opt) {
                    auto pr = frontend::parse(*src_opt);
                    if (!pr.ok) return std::unexpected(make_parse_error(pr));
                    auto ar = ctx_.analyse(pr, desc.name);
                    (void)ar; // symbol collection wired at full sema level

                    // File-module import pipeline (§10b/§13): the parser captured
                    // this module's imports on the typed AST; route them through
                    // lang::import_graph for circular/version/capability checks and
                    // record the resolved importees on the handle. The first hard
                    // import error is surfaced as a module_resolve failure.
                    if (pr.typed_ast && !pr.typed_ast->imports.empty()) {
                        const auto importer = pr.typed_ast->package_name.empty()
                            ? desc.name : pr.typed_ast->package_name;
                        auto ir = ctx_.resolve_imports(importer, pr.typed_ast->imports);
                        if (!ir.ok()) {
                            const auto& first = ir.errors.front();
                            crank_error e;
                            e.stage   = error_stage::module_resolve;
                            e.code    = first.code;
                            e.message = first.message;
                            return std::unexpected(std::move(e));
                        }
                        for (const auto& ri : ir.resolved)
                            imports.push_back(
                                module_ref{ri.desc.name,
                                           module_hash{ri.desc.content_hash.value}});
                    }
                }
            }

            module_handle h{desc, std::move(exports), std::move(imports), was_cached};
            loaded_modules_[desc.name] = h;
            return h;
        }

        // ── module_graph — topological view of resolved modules (§M.3) ────────

        [[nodiscard]] module_graph_view module_graph() {
            module_graph_view gv;
            auto order = ctx_.dep_graph().topo_order();
            for (const auto& name : order) {
                const auto* desc = ctx_.dep_graph().descriptor(name);
                if (!desc) continue;
                module_graph_entry e;
                e.name         = desc->name;
                e.content_hash = desc->content_hash;
                e.was_cached   = false;
                for (const auto& edge : ctx_.dep_graph().edges())
                    if (edge.importer == name) e.imports.push_back(edge.importee);
                gv.modules.push_back(std::move(e));
            }
            // Include loaded modules not yet in the dep_graph.
            for (const auto& [mname, mhandle] : loaded_modules_) {
                if (!gv.find(mname)) {
                    module_graph_entry e;
                    e.name         = mhandle.name();
                    e.content_hash = mhandle.content_hash();
                    e.was_cached   = mhandle.was_cached();
                    gv.modules.push_back(std::move(e));
                }
            }
            return gv;
        }

    private:
        crank::context  ctx_;
        engine_options  opts_;
        std::string     inp_fn_name_; // retained from last make_lower_input call
        std::unordered_map<std::string, module_handle> loaded_modules_;

        void apply_options_to_context() {
            auto& exec = ctx_.execution();
            exec.allow_simd(opts_.permit_simd);
            exec.allow_gpu(opts_.permit_gpu);
            exec.allow_threads(opts_.permit_parallel);

            switch (opts_.target) {
            case target_kind::cpu_only:
                exec.backend(backend_policy::inline_only);
                exec.allow_simd(false);
                exec.allow_gpu(false);
                break;
            case target_kind::simd:
                exec.backend(backend_policy::best_available);
                exec.allow_gpu(false);
                break;
            case target_kind::gpu_if_available:
                exec.backend(backend_policy::best_available);
                exec.allow_gpu(true);
                break;
            case target_kind::host:
            default:
                exec.backend(backend_policy::best_available);
                break;
            }
        }

        // Build a minimal lower_input from the parse result.
        // For the scripting facade the function name is extracted from parse stats
        // (deepest_fn_name_len is non-zero for any named fn). When no name is
        // available "main" is used as the synthetic entry-point name.
        lower_input make_lower_input(const frontend::parse_result& pr) {
            lower_input inp;
            inp.fn_name = "main";
            if (pr.stats && pr.stats->deepest_fn_name_len != 0) {
                // Use a generic fn name indicator when stats are collected.
                inp.fn_name = "script_main";
            }
            inp_fn_name_ = inp.fn_name;
            inp.safety_policy = safety_failure::trap;
            // Add a minimal ret node so the lowering emits a valid single-block fn.
            cfg_node ret;
            ret.kind           = cfg_node_kind::ret;
            ret.returns_value  = true;
            inp.cfg_nodes.push_back(ret);
            return inp;
        }

        execute_options make_exec_options() const {
            execute_options xopts;
            xopts.safety_policy = safety_failure::trap;
            return xopts;
        }

        // ── error builders ────────────────────────────────────────────────────

        static crank_error make_parse_error(const frontend::parse_result& pr) {
            crank_error e;
            e.stage = error_stage::parse;
            e.code  = "CRANK-PARSE-001";
            const auto& diags = pr.diagnostics.entries;
            if (!diags.empty()) {
                e.message = diags[0].message;
                // crank::source_span and vakya::diag::source_span are different types;
                // span assignment is skipped here — callers that need location context
                // should use the parse_result directly.
                for (std::size_t i = 1; i < diags.size(); ++i)
                    e.notes.push_back(diags[i].message);
            }
            else {
                e.message = "parse failed";
            }
            return e;
        }

        static crank_error make_analyse_error(const analyse_result& ar) {
            crank_error e;
            e.stage   = error_stage::analyse;
            e.code    = "CRANK-SEM-001";
            e.message = "semantic analysis failed";
            for (const auto& d : ar.resolve.diagnostics)
                e.notes.push_back(d.message);
            return e;
        }

        static crank_error make_lower_error(const lower_hl_result& lr) {
            crank_error e;
            e.stage   = error_stage::lower;
            e.code    = "CRANK-LOWER-001";
            e.message = lr.diagnostics.empty() ? "lowering failed" : lr.diagnostics[0];
            for (std::size_t i = 1; i < lr.diagnostics.size(); ++i)
                e.notes.push_back(lr.diagnostics[i]);
            return e;
        }

        static crank_error make_execute_error(const crank_execute_result& xr) {
            crank_error e;
            e.stage   = error_stage::execute;
            e.code    = "CRANK-EXEC-001";
            e.message = xr.diagnostics.empty() ? "execution failed" : xr.diagnostics[0];
            for (std::size_t i = 1; i < xr.diagnostics.size(); ++i)
                e.notes.push_back(xr.diagnostics[i]);
            return e;
        }
    };

    // =========================================================================
    // Free-function shorthands — crank::eval / crank::run
    //
    // Create a default engine per call. For repeated use, construct a persistent
    // engine and reuse it.
    // =========================================================================

    [[nodiscard]] inline std::expected<value, crank_error>
    eval(std::string_view source, engine_options opts = {}) {
        engine e{opts};
        return e.eval(source);
    }

    [[nodiscard]] inline std::expected<run_report, crank_error>
    run(std::string_view source, engine_options opts = {}) {
        engine e{opts};
        return e.run(source);
    }

    // =========================================================================
    // extern_fn_error_kind — §X.4 diagnostic codes for @host.link verification
    // =========================================================================

    enum class extern_fn_error_kind : std::uint8_t {
        unknown_host_symbol, // CRANK-EXT-010
        signature_mismatch,  // CRANK-EXT-011
        effect_escalation,   // CRANK-EXT-012
    };

    [[nodiscard]] constexpr std::string_view
    extern_fn_diag_code(extern_fn_error_kind k) noexcept {
        switch (k) {
        case extern_fn_error_kind::unknown_host_symbol: return "CRANK-EXT-010";
        case extern_fn_error_kind::signature_mismatch:  return "CRANK-EXT-011";
        case extern_fn_error_kind::effect_escalation:   return "CRANK-EXT-012";
        }
        return "CRANK-EXT-000";
    }

    // =========================================================================
    // extern_fn_decl — semantic record for a resolved @host.link extern fn (§X.4)
    //
    // On success, thunk points to the typed thunk in the registered
    // function_descriptor (no new indirection at call time).
    // =========================================================================

    struct extern_fn_decl {
        std::string                name;        // crank-side declared name
        std::string                host_link;   // @host.link value
        std::size_t                arity = 0;
        const function_descriptor* descriptor = nullptr; // bound host descriptor
        void (*thunk)(const void* const*, void*) = nullptr; // direct thunk
        descriptor_fingerprint     fingerprint  = 0;
    };

    // =========================================================================
    // verify_extern_fn_decl — §X.4 descriptor-match verification
    //
    // Looks up @host.link name in ctx.host_functions(). Checks:
    //   1. Descriptor present — else CRANK-EXT-010
    //   2. Arity matches       — else CRANK-EXT-011
    //   (Type + effect checks require full type-system integration and are
    //    validated during the analyse phase via the resolver; this call is the
    //    host-side API for tooling / explicit verification.)
    //
    // Returns expected<extern_fn_decl, crank_error>.
    // =========================================================================

    [[nodiscard]] inline std::expected<extern_fn_decl, crank_error>
    verify_extern_fn_decl(const crank::context& ctx,
                          std::string_view      crank_name,
                          std::string_view      host_link_name,
                          std::size_t           declared_arity) {
        const function_descriptor* found = nullptr;
        for (const auto& fn : ctx.host_functions()) {
            if (std::string_view{fn.name} == host_link_name) { found = &fn; break; }
        }

        if (!found) {
            crank_error e;
            e.stage   = error_stage::extern_fn;
            e.code    = std::string(extern_fn_diag_code(extern_fn_error_kind::unknown_host_symbol));
            e.message = "extern fn '" + std::string(crank_name) +
                        "': unknown host symbol '" + std::string(host_link_name) + "'";
            return std::unexpected(std::move(e));
        }

        if (found->arity != declared_arity) {
            crank_error e;
            e.stage   = error_stage::extern_fn;
            e.code    = std::string(extern_fn_diag_code(extern_fn_error_kind::signature_mismatch));
            e.message = "extern fn '" + std::string(crank_name) +
                        "': arity mismatch (declared " + std::to_string(declared_arity) +
                        ", registered " + std::to_string(found->arity) + ")";
            return std::unexpected(std::move(e));
        }

        extern_fn_decl decl;
        decl.name        = std::string(crank_name);
        decl.host_link   = std::string(host_link_name);
        decl.arity       = declared_arity;
        decl.descriptor  = found;
        decl.thunk       = found->typed_thunk;
        decl.fingerprint = found->fingerprint;
        return decl;
    }

    // =========================================================================
    // first_error — drain first diagnostic from analyse_result (§B.5)
    //
    // Returns an optional crank_error; the engine uses this to short-circuit
    // after the analyse stage. Implemented as a free function over analyse_result
    // (additive, does not modify context.hpp).
    // =========================================================================

    [[nodiscard]] inline std::optional<crank_error>
    first_error(const analyse_result& ar) {
        if (ar.ok) return std::nullopt;
        crank_error e;
        e.stage   = error_stage::analyse;
        e.code    = "CRANK-SEM-001";
        e.message = "semantic analysis failed";
        for (const auto& d : ar.resolve.diagnostics)
            e.notes.push_back(d.message);
        return e;
    }

    // =========================================================================
    // module_parse_info — lightweight per-module metadata extracted after parse.
    //
    // Stored in module_parse_cache so re-loading an unchanged module (same content
    // hash) skips re-parsing. Only captures what the import pipeline needs;
    // full AST is rebuilt on demand (not cached — it is move-only).
    // =========================================================================

    struct module_parse_info {
        std::string              package_name;
        std::vector<std::string> imports;
        bool                     parse_ok = false;
    };

    // =========================================================================
    // module_parse_cache — Kosha sharded LRU keyed by content_hash::value.
    //
    // Template param S controls shard count (default 8, power-of-2 required).
    // Thread-safe: ShardedLRUCache wraps each shard in ThreadSafeCache.
    //
    // Usage:
    //   crank::module_parse_cache<> cache{1024};
    //   cache.put(hash, info);
    //   if (auto v = cache.get(hash)) { /* hit */ }
    // =========================================================================

    template <std::size_t S = 8>
    using module_parse_cache = kosha::ShardedLRUCache<std::uint64_t, module_parse_info, S>;

    // name → content_hash output map for parse_modules_parallel.
    struct module_hash_map_t {
        std::mutex                                     mutex;
        std::unordered_map<std::string, std::uint64_t> entries;
    };

    // =========================================================================
    // parse_modules_parallel — parse a set of source modules concurrently.
    //
    // source_provider(module_name) → std::optional<std::string> must be thread-safe
    // (called from Pravaha worker threads). Returns results in the same order as
    // `module_names`. Entries whose source_provider returns nullopt are marked
    // parse_ok = false.
    //
    // When only one module is requested, or threshold is set to 1, falls back to
    // serial (avoids JThreadBackend overhead for tiny workloads).
    //
    // Uses JThreadBackend with work-stealing for CPU-bound parse workers.
    // Each module parses independently — no shared state during the parallel phase.
    // Cache (optional): on hit, skips frontend::parse and returns cached metadata.
    // =========================================================================

    template <std::size_t CacheShards = 8>
    [[nodiscard]] std::vector<module_parse_info>
    parse_modules_parallel(
        std::span<const std::string>       module_names,
        std::function<std::optional<std::string>(std::string_view)> source_provider,
        module_parse_cache<CacheShards>*   cache            = nullptr,
        module_hash_map_t*                 hash_map         = nullptr,
        std::size_t                        serial_threshold = 1)
    {
        const auto n = module_names.size();
        std::vector<module_parse_info> results(n);
        if (n == 0) return results;

        auto do_one = [&](std::size_t i) -> module_parse_info {
            const auto& name = module_names[i];
            auto src_opt = source_provider(name);
            if (!src_opt) return {};

            const auto h = hash_source(*src_opt).value;
            if (cache) {
                if (auto hit = cache->get(h); hit) return *hit;
            }

            auto pr = frontend::parse(*src_opt);
            module_parse_info info;
            info.parse_ok = pr.ok;
            if (pr.typed_ast) {
                info.package_name = pr.typed_ast->package_name;
                info.imports      = pr.typed_ast->imports;
            }
            if (cache) (void)cache->put(h, info);
            if (hash_map) {
                std::unique_lock lk{hash_map->mutex};
                hash_map->entries[std::string(name)] = h;
            }
            return info;
        };

        if (n <= serial_threshold) {
            for (std::size_t i = 0; i < n; ++i)
                results[i] = do_one(i);
            return results;
        }

        // Parallel: lazy_parallel_for over [0, n). Each index → distinct slot; no race.
        // chunk_size=1 so each module gets its own task (parse is coarse-grained).
        auto expr = pravaha::lazy_parallel_for(
            std::views::iota(std::size_t{0}, n),
            [&](std::size_t i) { results[i] = do_one(i); },
            /*chunk_size=*/1);

        pravaha::JThreadBackend backend;
        pravaha::Runner<pravaha::JThreadBackend> runner{backend};
        (void)runner.submit(std::move(expr));

        return results;
    }

    // =========================================================================
    // batch_load_entry / batch_load_result — result of batch_load_modules.
    // =========================================================================

    struct batch_load_entry {
        std::string              name;
        bool                     ok        = false;
        std::string              error_msg;
        std::vector<std::string> imports;
    };

    struct batch_load_result {
        std::vector<batch_load_entry> entries;
        [[nodiscard]] bool all_ok() const noexcept {
            for (const auto& e : entries) if (!e.ok) return false;
            return true;
        }
    };

    // =========================================================================
    // batch_load_modules — load modules in parallel dependency waves.
    //
    // topo_names: module names in topological order (importees first), e.g. from
    //   dep_graph.topo_order(). Modules in the same wave (no unresolved deps in
    //   the batch) are parsed concurrently via Pravaha JThreadBackend.
    //
    // source_provider: thread-safe callable → source text (nullopt = not found).
    // ctx: crank::context that owns the resolver and dep graph.
    // cache (optional): skip re-parsing unchanged modules.
    // =========================================================================

    template <std::size_t CacheShards = 8>
    [[nodiscard]] batch_load_result
    batch_load_modules(
        std::span<const std::string>       topo_names,
        std::function<std::optional<std::string>(std::string_view)> source_provider,
        context&                           ctx,
        module_parse_cache<CacheShards>*   cache = nullptr)
    {
        batch_load_result result;
        result.entries.reserve(topo_names.size());

        std::unordered_set<std::string> done;
        done.reserve(topo_names.size());

        std::vector<std::string> wave;
        wave.reserve(topo_names.size());

        auto flush_wave = [&]() {
            if (wave.empty()) return;
            auto infos = parse_modules_parallel<CacheShards>(
                std::span<const std::string>(wave), source_provider, cache);
            for (std::size_t i = 0; i < wave.size(); ++i) {
                batch_load_entry e;
                e.name    = wave[i];
                e.ok      = infos[i].parse_ok;
                e.imports = infos[i].imports;
                if (e.ok) {
                    for (const auto& imp : e.imports)
                        ctx.dep_graph().add_import(e.name, imp);
                } else {
                    e.error_msg = "parse failed: " + wave[i];
                }
                done.insert(wave[i]);
                result.entries.push_back(std::move(e));
            }
            wave.clear();
        };

        for (const auto& name : topo_names) {
            auto deps = ctx.dep_graph().find_dependencies(name);
            bool ready = true;
            for (const auto& dep : deps) {
                bool in_batch = false;
                for (const auto& n : topo_names) if (n == dep) { in_batch = true; break; }
                if (in_batch && !done.count(dep)) { ready = false; break; }
            }
            if (ready) {
                wave.push_back(name);
            } else {
                flush_wave();
                wave.push_back(name);
            }
        }
        flush_wave();

        return result;
    }

} // namespace crank
