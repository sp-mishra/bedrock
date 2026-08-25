#pragma once

// crank/context.hpp — semantic umbrella + host context for crank (Module 2).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// crank::context — top-level entry point for the crank semantic layer:
//   .modules()     — module resolution (module.hpp)
//   .register_function<"name", fn>()   — host fn binding (host.hpp)
//   .register_type<T>()               — host type reflection
//   .register_container<C>("name")    — host container
//   .execution()   — execution policy (placeholder for module 4)
//   .import(name)  — resolve + seed dependency graph
//   .semantics()   — access sema_context for type inference
//   .effects()     — access effect checker
//
// Usage:
//   crank::context ctx;
//   ctx.modules().add_project_path("/my/project");
//   ctx.register_function<"math.dot", dot>();
//   ctx.register_type<Vec3>();
//   ctx.register_container<std::vector<float>>("float_vec");
//   auto parse = crank::frontend::parse(source);
//   ctx.analyse(parse);  // runs resolve + sema_types + effects

#include "languages/crank/host.hpp"
#include "languages/crank/module.hpp"
#include "languages/crank/resolve.hpp"
#include "languages/generic/module/import_resolver.hpp"  // lang::import_graph (§10b)
#include "languages/crank/sema_types.hpp"
#include "languages/crank/effects.hpp"
#include "languages/crank/std_types.hpp"
#include "languages/crank/annotation.hpp"
#include "languages/crank/debug_info.hpp"
#include "languages/crank/ffi_module.hpp"

#include <concepts>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

// Forward-declare parse_result to break the circular include chain:
// context.hpp ← parallel.hpp ← dump.hpp ← frontend.hpp
// When context.hpp is first processed via parallel.hpp, frontend.hpp is not yet
// parsed. The analyse(parse_result) overload only needs the type by reference.
namespace crank::frontend {
    struct parse_result;
}

// Forward-declare extern_fn_node so the analyse() template body can use
// std::get_if<extern_fn_node> during phase-1 lookup.  The full definition is
// in build_ast.hpp (included via frontend.hpp at the bottom of this file), which
// is complete at every template instantiation site.
namespace crank {
    struct extern_fn_node;
}

namespace crank {
    // ============================================================================
    // Import-pipeline boundary adapters (§10b, §8b) — crank ⇄ lang.
    //
    // crank keeps its own symbol_table / symbol_entry / module_descriptor (span +
    // HM-type-var fields, FNV fingerprints) to avoid regressing fingerprints and
    // consumers. These free functions convert only at the import boundary so
    // lang::import_graph can run version / capability / circular checks and flow
    // exported symbols, without aliasing crank's core types.
    // ============================================================================

    // lithe::version_triple → lang::version_triple (field-identical {maj,min,pat};
    // narrows uint32→uint16, safe for realistic version numbers).
    [[nodiscard]] inline lang::version_triple
    to_lang_version(const version_triple& v) noexcept {
        return {static_cast<std::uint16_t>(v.major),
                static_cast<std::uint16_t>(v.minor),
                static_cast<std::uint16_t>(v.patch)};
    }

    // crank::module_descriptor → lang::module_descriptor. Kind maps 1:1 (both
    // enums share the same 0..4 ordering).
    [[nodiscard]] inline lang::module_descriptor
    to_lang_descriptor(const module_descriptor& d) {
        lang::module_descriptor o;
        o.name         = d.name;
        o.version      = to_lang_version(d.version);
        o.kind         = static_cast<lang::module_kind>(static_cast<std::uint8_t>(d.kind));
        o.content_hash = lang::module_hash{d.content_hash.value};
        o.capabilities = lang::module_capabilities{d.capabilities.effect_mask,
                                                   d.capabilities.capability_mask};
        o.source_path  = d.source_path;
        o.package_name = d.package_name;
        return o;
    }

    // crank::symbol_entry → lang::symbol_entry, for the import symbol_provider.
    // Only the export-relevant fields cross the boundary; crank-only fields
    // (span, type-var binding, view metadata) stay on the crank side.
    [[nodiscard]] inline lang::symbol_entry
    to_lang_symbol(const symbol_entry& e, std::string_view origin) {
        lang::symbol_entry o;
        o.name          = e.local_name.empty() ? e.qualified_name : e.local_name;
        o.visibility    = (e.visibility == visibility_kind::exported)
                              ? lang::sym_visibility::exported
                              : lang::sym_visibility::module_local;
        o.module_origin = std::string(origin);
        o.scope_depth   = e.scope_depth;
        switch (e.kind) {
        case symbol_kind::function: o.kind = lang::sym_kind::function;   break;
        case symbol_kind::type_def: o.kind = lang::sym_kind::type_alias; break;
        case symbol_kind::constant: o.kind = lang::sym_kind::constant;   break;
        case symbol_kind::view:     o.kind = lang::sym_kind::type_alias; break;
        default:                    o.kind = lang::sym_kind::variable;   break;
        }
        return o;
    }



    enum class scheduler_policy : std::uint8_t {
        fifo = 0, // first-in first-out (InlineBackend ordered)
        priority = 1, // priority queue (InlineBackend ordered)
        work_stealing = 2, // work-stealing (JThreadBackend default)
        critical_path = 3, // critical-path scheduling (future planner)
        locality = 4, // NUMA-aware locality scheduling
        gpu = 5, // hetero/GPU scheduler overlay (§6.3)
    };

    [[nodiscard]] constexpr std::string_view to_string(scheduler_policy p) noexcept {
        switch (p) {
        case scheduler_policy::fifo: return "fifo";
        case scheduler_policy::priority: return "priority";
        case scheduler_policy::work_stealing: return "work_stealing";
        case scheduler_policy::critical_path: return "critical_path";
        case scheduler_policy::locality: return "locality";
        case scheduler_policy::gpu: return "gpu";
        }
        return "unknown";
    }

    // ============================================================================
    // fallback_policy — what happens when the primary backend is unavailable (§6.3)
    // ============================================================================

    enum class fallback_policy : std::uint8_t {
        safe_cpu = 0, // fall back to interpreter/inline CPU; emit NADI pulse
        none = 1, // no fallback — fail hard if primary unavailable
    };

    [[nodiscard]] constexpr std::string_view to_string(fallback_policy p) noexcept {
        switch (p) {
        case fallback_policy::safe_cpu: return "safe_cpu";
        case fallback_policy::none: return "none";
        }
        return "unknown";
    }

    // ============================================================================
    // backend_policy — which execution backend to prefer (§6.3)
    // ============================================================================

    enum class backend_policy : std::uint8_t {
        best_available = 0, // pick best available (GPU > SIMD > threaded > inline)
        inline_only = 1, // interpreter/inline only
        threaded_only = 2, // JThreadBackend only
    };

    [[nodiscard]] constexpr std::string_view to_string(backend_policy p) noexcept {
        switch (p) {
        case backend_policy::best_available: return "best_available";
        case backend_policy::inline_only: return "inline_only";
        case backend_policy::threaded_only: return "threaded_only";
        }
        return "unknown";
    }

    // ============================================================================
    // execution_options — per-context execution configuration (§9.4, §6.3)
    // ============================================================================

    struct execution_options {
        bool use_pravaha = false;
        bool allow_simd = true;
        bool allow_gpu = false;
        bool allow_threads = true; // §6.3
        bool allow_async = true; // §6.3
        bool allow_distributed = false; // §6.3 (v1: false)
        scheduler_policy scheduler = scheduler_policy::work_stealing; // §9.4
        fallback_policy fallback = fallback_policy::safe_cpu; // §6.3
        backend_policy backend = backend_policy::best_available; // §6.3
    };

    // ============================================================================
    // execution_config — fluent builder for execution_options
    // ============================================================================

    class execution_config {
    public:
        execution_config& use_pravaha(bool v = true) {
            opts_.use_pravaha = v;
            return *this;
        }

        execution_config& allow_simd(bool v = true) {
            opts_.allow_simd = v;
            return *this;
        }

        execution_config& allow_gpu(bool v = true) {
            opts_.allow_gpu = v;
            return *this;
        }

        execution_config& allow_threads(bool v = true) {
            opts_.allow_threads = v;
            return *this;
        }

        execution_config& allow_async(bool v = true) {
            opts_.allow_async = v;
            return *this;
        }

        execution_config& allow_distributed(bool v = true) {
            opts_.allow_distributed = v;
            return *this;
        }

        execution_config& scheduler(scheduler_policy p) {
            opts_.scheduler = p;
            return *this;
        }

        execution_config& fallback(fallback_policy p) {
            opts_.fallback = p;
            return *this;
        }

        execution_config& backend(backend_policy p) {
            opts_.backend = p;
            return *this;
        }

        [[nodiscard]] const execution_options& options() const noexcept { return opts_; }

    private:
        execution_options opts_;
    };

    // ============================================================================
    // debug_config — fluent builder holding debugger hooks + a collect flag
    //
    // Mirrors execution_config. Purely opt-in: when untouched, hooks are empty and
    // collect_debug_info is false, so a non-debug session costs nothing. The stored
    // debug_hooks (debug_info.hpp) are consumed by the future CFG-aware interpreter;
    // collect_debug_info signals that debug.hpp builders should run in the pipeline.
    // ============================================================================

    class debug_config {
    public:
        debug_config& collect(bool v = true) {
            collect_ = v;
            return *this;
        }

        debug_config& on_breakpoint(std::function<void(const debug_event &)> cb) {
            hooks_.on_breakpoint = std::move(cb);
            return *this;
        }

        debug_config& on_step(std::function<void(const debug_event &)> cb) {
            hooks_.on_step = std::move(cb);
            return *this;
        }

        debug_config& on_watch(std::function<void(const debug_event &)> cb) {
            hooks_.on_watch = std::move(cb);
            return *this;
        }

        debug_config& add_breakpoint(breakpoint_location bp) {
            hooks_.breakpoints.push_back(std::move(bp));
            return *this;
        }

        debug_config& watch(std::string var) {
            hooks_.watched_vars.push_back(std::move(var));
            return *this;
        }

        [[nodiscard]] bool collect_debug_info() const noexcept { return collect_; }
        [[nodiscard]] const debug_hooks& hooks() const noexcept { return hooks_; }
        [[nodiscard]] debug_hooks& hooks() noexcept { return hooks_; }

    private:
        debug_hooks hooks_;
        bool collect_ = false;
    };

    // ============================================================================
    // module_config — fluent builder over module_resolver
    // ============================================================================
    class module_config {
    public:
        module_config& add_path(std::filesystem::path p) {
            resolver_.add_project_path(std::move(p));
            return *this;
        }

        module_config& add_cache(std::filesystem::path p) {
            resolver_.add_cache_path(std::move(p));
            return *this;
        }

        module_config& add_native(module_descriptor d) {
            resolver_.add_native(std::move(d));
            return *this;
        }

        [[nodiscard]] module_resolver& resolver() noexcept { return resolver_; }
        [[nodiscard]] const module_resolver& resolver() const noexcept { return resolver_; }

    private:
        module_resolver resolver_;
    };

    // ============================================================================
    // analyse_result — output of context::analyse()
    // ============================================================================

    struct analyse_result {
        resolve_result resolve;
        effects_result effects;
        bool ok = false;
    };

    // ============================================================================
    // context — top-level crank semantic context
    // ============================================================================

    class context {
    public:
        context()
            : type_registry_(make_crank_type_registry())
              , effect_registry_(make_crank_effects_registry())
              , cap_registry_(make_crank_caps_registry())
              , annotation_registry_(make_crank_annotation_registry()) {}

        // ── module configuration ─────────────────────────────────────────────────

        [[nodiscard]] module_config& modules() noexcept { return modules_; }

        // ── host function registration ───────────────────────────────────────────

        template <lithe::fixed_string Name, auto Fn>
        void register_function() {
            host_fns_.push_back(make_host_fn_descriptor<Name, Fn>());
        }

        // Register a capturing callable as a named host function.
        void register_function(std::string fn_name, std::size_t arity,
                               std::function<std::any(std::span<const std::any>)> trampoline) {
            host_fn_descriptor d;
            d.name = std::move(fn_name);
            d.arity = arity;
            d.trampoline = std::move(trampoline);
            host_fns_.push_back(std::move(d));
        }

        // Register a prebuilt function_descriptor (typed thunk + options).
        // Lets callers set effects/capabilities/flags that the templated
        // register_function<> overload leaves at their defaults — used by the
        // standard library to attach effect/capability metadata per function.
        void register_function_descriptor(function_descriptor d) {
            host_fns_.push_back(std::move(d));
        }

        // ── host type registration ────────────────────────────────────────────────

        template <class T>
            requires has_type_descriptor<T>
        void register_type() {
            host_types_.push_back(make_host_type_descriptor<T>());
        }

        // ── host container registration ──────────────────────────────────────────

        template <class C>
            requires has_container_traits<C>
        void register_container(std::string name) {
            host_containers_.push_back(make_host_container_descriptor<C>(std::move(name)));
        }

        // ── execution policy ─────────────────────────────────────────────────────

        [[nodiscard]] execution_config& execution() noexcept { return exec_; }

        // ── structured concurrency (§v2.9) ─────────────────────────────────────────
        //
        // task_scope / deadline_scope / join_group are a standalone C++ API in
        // task_scope.hpp (which depends on future.hpp). They are intentionally NOT
        // pulled into the context include graph — future.hpp's crank_future_error
        // enum is distinct from parallel.hpp's same-named v1 enum, and dragging both
        // into one TU would be an ODR clash. Include "languages/crank/task_scope.hpp"
        // directly where structured concurrency is used; scopes are backend-agnostic
        // and honor allow_threads via the Pravaha adapter at execution time.

        // ── multi-resource transaction coordinators (§v2.11) ───────────────────────

        // Register a named 2PC/coordinator. The transaction policy checker consults
        // this set to validate `transaction(..., coordinator = name)` (CRANK-TX-010
        // when a referenced coordinator name is not registered).
        void register_coordinator(std::string name) {
            coordinators_.insert(std::move(name));
        }

        [[nodiscard]] bool has_coordinator(std::string_view name) const {
            return coordinators_.find(std::string(name)) != coordinators_.end();
        }

        [[nodiscard]] const std::unordered_set<std::string>&
        coordinators() const noexcept { return coordinators_; }

        // ── debug configuration ──────────────────────────────────────────────────

        // Opt-in debugger hooks + debug-info collection flag. Zero cost when unused.
        [[nodiscard]] debug_config& debug() noexcept { return debug_; }
        [[nodiscard]] const debug_config& debug() const noexcept { return debug_; }

        // ── annotation registry ──────────────────────────────────────────────────

        [[nodiscard]] annotation_registry& annotations() noexcept { return annotation_registry_; }

        [[nodiscard]] const annotation_registry& annotations() const noexcept {
            return annotation_registry_;
        }

        // Register a typed annotation from a compile-time schema (§5b.4).
        // Usage: ctx.register_annotation<"lithe.cacheline",
        //            annotation_schema<arg<"align", annotation_arg_type::u32>>>(
        //            annotation_kind::optimization_hint, annotation_strength::advisory, 2000);
        template <lithe::fixed_string Name, class Schema>
        void register_annotation(annotation_kind kind,
                                 annotation_strength strength,
                                 std::uint32_t stable_id,
                                 effect_mask effects = 0,
                                 capability_mask caps = 0) {
            constexpr auto fields = detail::flatten_schema<Schema>::make();
            annotation_descriptor d;
            d.name = std::string_view{Name.data(), Name.size() - 1};
            d.kind = kind;
            d.default_strength = strength;
            d.effects = effects;
            d.capabilities = caps;
            d.stable_id = stable_id;
            d.version = 1;
            d.name_hash = containers::desc_name_hash(d.name);
            annotation_registry_.register_desc(d, std::span<const schema_field>{fields});
        }

        // Install a static extension plugin (§5b.9).
        template <CrankExtension E>
        void install_extension(E&& ext) {
            crank::install_extension(annotation_registry_, std::forward < E > (ext));
        }

        // Resolve a batch of parsed annotations (thin caller over annotation_resolver).
        [[nodiscard]] std::vector<annotation_resolution>
        resolve_annotations(std::span<const parsed_annotation> anns,
                            annotation_policy policy = annotation_policy::strict) const {
            annotation_resolver resolver(annotation_registry_, policy);
            return resolver.resolve_all(anns);
        }

        // ── semantic analysis ────────────────────────────────────────────────────

        // Run full semantic analysis (resolve + type inference + effects).
        // Call with the parse result's ok flag.  Pass the full parse_result via
        // analyse(parse) only when frontend.hpp is also included by the caller.
        [[nodiscard]] analyse_result analyse(bool parse_ok,
                                             std::string module_name = "") {
            analyse_result ar;

            // 1. Name resolution
            resolver res(std::move(module_name));
            ar.resolve = res.take();

            // 2. Effects
            effect_checker eff_chk(effect_registry_, cap_registry_);
            ar.effects = eff_chk.take();

            ar.ok = ar.resolve.ok() && ar.effects.ok() && parse_ok;
            return ar;
        }

        // Typed overload — accepts parse_result directly; threads the AST through
        // the resolver and effect checker. Prefer this over analyse(bool) in new code.
        //
        // Templated so name lookup of pr.ok is deferred to instantiation (call site),
        // where parse_result is complete. context.hpp only forward-declares
        // parse_result to break the parallel.hpp → dump.hpp → frontend.hpp include
        // cycle, so an ordinary member body here would see an incomplete type.
        //
        // Also walks pr.typed_ast for extern_fn_node entries and verifies each
        // against registered host functions (CRANK-EXT-010/011).
        // extern_fn_node is forward-declared above; complete at every instantiation site.
        template <class ParseResult>
            requires std::same_as<std::remove_cvref_t<ParseResult>, frontend::parse_result>
        [[nodiscard]] analyse_result analyse(const ParseResult& pr,
                                             std::string module_name = "") {
            analyse_result ar = analyse(pr.ok, std::move(module_name));
            if (!pr.typed_ast) return ar;
            for (const auto& node : pr.typed_ast->top_level) {
                const auto* efn = std::get_if<extern_fn_node>(&node);
                if (!efn || efn->host_link.empty()) continue;
                const host_fn_descriptor* found = nullptr;
                for (const auto& fn : host_fns_) {
                    if (fn.name == efn->host_link) { found = &fn; break; }
                }
                if (!found) {
                    resolve_diagnostic d;
                    d.k       = resolve_diagnostic::kind::extern_unknown_host;
                    d.symbol  = efn->host_link;
                    d.message = "extern fn '" + efn->name +
                                "': unknown host symbol '" + efn->host_link + "'";
                    ar.resolve.diagnostics.push_back(std::move(d));
                    ar.ok = false;
                } else if (found->arity != efn->param_names.size()) {
                    resolve_diagnostic d;
                    d.k       = resolve_diagnostic::kind::extern_arity_mismatch;
                    d.symbol  = efn->name;
                    d.message = "extern fn '" + efn->name +
                                "': arity mismatch (declared " +
                                std::to_string(efn->param_names.size()) +
                                ", registered " + std::to_string(found->arity) + ")";
                    ar.resolve.diagnostics.push_back(std::move(d));
                    ar.ok = false;
                }
            }
            return ar;
        }

        // ── type system access ────────────────────────────────────────────────────

        [[nodiscard]] sema_context& semantics() noexcept { return sema_; }

        [[nodiscard]] const vakya::types::type_registry& type_registry() const noexcept {
            return type_registry_;
        }

        // ── registration queries ──────────────────────────────────────────────────

        [[nodiscard]] const std::vector<host_fn_descriptor>& host_functions() const noexcept {
            return host_fns_;
        }

        [[nodiscard]] const std::vector<host_type_descriptor>& host_types() const noexcept {
            return host_types_;
        }

        [[nodiscard]] const std::vector<host_container_descriptor>& host_containers() const noexcept {
            return host_containers_;
        }

        // ── module dependency graph ───────────────────────────────────────────────

        // Resolve an import string and add it to the dependency graph.
        [[nodiscard]] std::optional<module_descriptor>
        import(std::string_view name) {
            auto desc = modules_.resolver().resolve(name);
            if (desc) dep_graph_.add_module(*desc);
            return desc;
        }

        [[nodiscard]] dependency_graph& dep_graph() noexcept { return dep_graph_; }

        // Provider of a module's exported crank symbols, keyed by module name.
        // Supplied by the resolve pass; returns the importee's exported symbols
        // (uppercase / pub) so they can flow into the importer at the boundary.
        using crank_symbol_provider =
            std::function<std::vector<symbol_entry>(std::string_view)>;

        // Run the full file-module import pipeline (§10b) over captured imports.
        //
        // Seeds the crank dependency_graph (edges importer→importee, preserving
        // existing topo behavior), then routes through lang::import_graph to run
        // circular (LANG-IMP-003), version (004) and capability (005) checks plus
        // exported-symbol flow. A lang::module_resolver mirror is built from crank
        // resolutions so the generic pipeline sees version/kind without aliasing
        // crank's resolver. sym_provider (optional) yields each importee's crank
        // exports, converted to lang::symbol_entry at the boundary.
        [[nodiscard]] lang::import_graph::resolve_result
        resolve_imports(std::string_view importer,
                        const std::vector<std::string>& imports,
                        const lang::module_capabilities_map& caps = {},
                        const crank_symbol_provider& sym_provider = {}) {
            lang::import_graph graph;
            lang::module_resolver mirror; // native-tier mirror for the pipeline

            std::vector<lang::import_spec> specs;
            specs.reserve(imports.size());
            for (const auto& name : imports) {
                dep_graph_.add_import(importer, name);
                if (auto desc = modules_.resolver().resolve(name)) {
                    dep_graph_.add_module(*desc);
                    mirror.add_native(to_lang_descriptor(*desc)); // carry version/kind
                }
                specs.push_back(lang::import_spec{.module_name = name});
            }
            graph.declare_imports(importer, std::move(specs));

            lang::symbol_provider bridge;
            if (sym_provider) {
                bridge = [&sym_provider](std::string_view mod)
                    -> std::vector<lang::symbol_entry> {
                    std::vector<lang::symbol_entry> out;
                    for (const auto& e : sym_provider(mod))
                        if (e.visibility == visibility_kind::exported)
                            out.push_back(to_lang_symbol(e, mod));
                    return out;
                };
            }
            return graph.resolve(mirror, caps, bridge);
        }

        // ── FFI module registry ───────────────────────────────────────────────────

        // Register an FFI module grouping host symbols under a module name.
        // Seeds the module_resolver's native tier so `import "name"` resolves.
        void register_ffi_module(ffi_module_descriptor desc) {
            ffi_modules_.register_module(std::move(desc), &modules_.resolver());
        }

        [[nodiscard]] const ffi_module_registry& ffi_modules() const noexcept {
            return ffi_modules_;
        }

    private:
        module_config modules_;
        execution_config exec_;
        debug_config debug_;
        sema_context sema_;
        vakya::types::type_registry type_registry_;
        vakya::types::effect_registry effect_registry_;
        vakya::types::capability_registry cap_registry_;
        annotation_registry annotation_registry_;
        dependency_graph dep_graph_;

        std::vector<host_fn_descriptor> host_fns_;
        std::vector<host_type_descriptor> host_types_;
        std::vector<host_container_descriptor> host_containers_;
        std::unordered_set<std::string> coordinators_; // §v2.11
        ffi_module_registry ffi_modules_;
    };
} // namespace crank

// Included last: frontend.hpp pulls in dump.hpp → parallel.hpp, which reference
// crank's execution policy enums (scheduler_policy, fallback_policy, backend_policy).
// Those enums must be fully defined above before this include is processed,
// otherwise the parallel.hpp → dump.hpp → frontend.hpp cycle re-enters context.hpp
// (skipped by #pragma once) with the enums not yet visible.
#include "languages/crank/frontend.hpp"
