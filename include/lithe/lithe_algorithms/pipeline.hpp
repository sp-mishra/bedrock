#pragma once

// =============================================================================
// lithe_algorithms/pipeline.hpp — typed pass pipeline, analysis_manager
//
// Design:
//   • static_pipeline<IrHooks, Passes...>: zero std::function, passes run in
//     declaration order; [[no_unique_address]] IrHooks compiled away by default.
//     Optional [[no_unique_address]] Sink (default diag::null_sink) set via
//     set_sink() — routes pass diagnostics to any diagnostic_sink.
//   • dynamic_pipeline<IR>: erased pass list via any_pass<IR>.
//   • pass_result<IR>: output + change flag + preserved analysis set +
//     extension invalidation set + diagnostics.
//   • pass_diagnostic: thin adapter — wraps diag::diagnostic + instr_id.
//     to_diagnostic() accessor; implicit level/message access preserved.
//   • analysis_manager: dual-index cache — built-ins on enum+bitset (hot),
//     extension analyses on analysis_key (string-keyed, stable_id >= 1000).
//     analysis_descriptor<A> trait drives compute-or-serve via require<A>.
//
// The pipeline hook seam is the neutral no_pipeline_hooks from foundation.hpp;
// the active provider-backed hooks live in lithe_ir/hooks.hpp.
//
// This header MUST NOT include lithe_ir.hpp (additive, no forced IR dep).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <any>
#include <array>
#include <bitset>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../lithe_execution/foundation.hpp"   // no_pipeline_hooks, compile_error
#include "../lithe_diagnostics.hpp"             // lithe::diag unified diagnostic model

namespace lithe::algorithms {
    // =========================================================================
    // analysis_id — typed enum for well-known analyses
    //
    // Corresponds to the analysis set referenced by lithe_codegen.hpp /
    // lithe_lowering.hpp (CFG, def-use, value-flow).  Extend as new analyses land.
    // =========================================================================

    enum class analysis_id : std::uint8_t {
        cfg = 0, // control-flow graph
        dominator = 1, // dominator tree
        loop_info = 2, // loop structure
        liveness = 3, // liveness analysis
        alias = 4, // alias analysis
        managed_ref = 5, // managed reference tracking
        def_use = 6, // def-use chains
        value_flow = 7, // value-flow analysis
        count_ = 8, // sentinel
    };

    inline constexpr std::size_t analysis_count =
        static_cast<std::size_t>(analysis_id::count_);

    // =========================================================================
    // analysis_key — structural key for extension analyses (stable_id >= 1000)
    //
    // Structural type → usable as NTTP.  domain/name are null-terminated char
    // arrays so the struct satisfies C++20 structural type requirements without
    // depending on fixed_string's consteval-only constructor.
    //
    // Extensions register in their own headers by specialising
    // analysis_descriptor<A> — never edit this header.
    // =========================================================================

    inline constexpr std::size_t kAnalysisExtIdBase = 1000u; // mirrors kExtensionIdBase

    struct analysis_key {
        char domain[32]{};
        char name[32]{};
        std::size_t stable_id = kAnalysisExtIdBase; // extension band: >= kAnalysisExtIdBase

        [[nodiscard]] constexpr std::string_view domain_view() const noexcept {
            return std::string_view{domain};
        }

        [[nodiscard]] constexpr std::string_view name_view() const noexcept {
            return std::string_view{name};
        }

        constexpr bool operator==(const analysis_key&) const noexcept = default;
    };

    // =========================================================================
    // analysis_descriptor<A> — trait that registers an extension analysis.
    //
    // A conforming specialization declares:
    //   using result_t = /* concrete result type */;
    //   static constexpr analysis_key key;
    //   static Result compute(const IR&, analysis_manager&);  // pure, no virtual
    //
    // Built-in analyses (those with an analysis_id enumerator) need no
    // specialization; use analysis_manager::store/get directly.
    // =========================================================================

    template <class A>
    struct analysis_descriptor; // undefined primary — must specialize for extensions

    // =========================================================================
    // builtin_analysis_trait<A> — detects whether A is a built-in (has ::id).
    //
    // Built-in helper: specialise analysis_descriptor<A> and add:
    //   static constexpr analysis_id id = analysis_id::cfg; // etc.
    // Then builtin_analysis_trait selects the fast array path in require<A>.
    // =========================================================================

    namespace detail {
        template <class A, class = void>
        inline constexpr bool is_builtin_analysis = false;

        template <class A>
        inline constexpr bool is_builtin_analysis<A,
                                                  std::void_t<decltype(analysis_descriptor<A>::id)>> = true;
    }

    // =========================================================================
    // preserved_analysis_set — bitmask of built-in analyses a pass preserves
    // =========================================================================

    struct preserved_analysis_set {
        std::bitset<analysis_count> bits;

        constexpr preserved_analysis_set() noexcept = default;

        constexpr void set(const analysis_id a) noexcept {
            bits.set(static_cast<std::size_t>(a));
        }

        [[nodiscard]] constexpr bool test(const analysis_id a) const noexcept {
            return bits.test(static_cast<std::size_t>(a));
        }

        [[nodiscard]] constexpr bool none() const noexcept { return bits.none(); }
        [[nodiscard]] constexpr bool any() const noexcept { return bits.any(); }

        [[nodiscard]] static constexpr preserved_analysis_set all() noexcept {
            preserved_analysis_set s;
            s.bits.set();
            return s;
        }

        [[nodiscard]] static constexpr preserved_analysis_set none_set() noexcept {
            return {};
        }
    };

    // =========================================================================
    // pass_diagnostic — adapter: keeps original field layout; adds to_diagnostic()
    //
    // Fields preserved: level (severity), message (string_view), instr_id.
    // to_diagnostic() converts to diag::diagnostic for sink routing.
    // Existing construction sites unchanged: {level, message, instr_id}.
    // =========================================================================

    struct pass_diagnostic {
        lithe::diag::severity level = lithe::diag::severity::note;
        std::string_view message = {};
        std::uint32_t instr_id = 0; // MIR instruction id (0 = none)

        // Produce a full diag::diagnostic from this entry.
        [[nodiscard]] lithe::diag::diagnostic to_diagnostic() const {
            return lithe::diag::diagnostic{
                .level = level,
                .stage = lithe::diag::stage::optimization,
                .code = lithe::diag::codes::unknown,
                .message = std::string{message}
            };
        }
    };

    // Backward-compat enum alias.
    using pass_diagnostic_level = lithe::diag::severity;

    // =========================================================================
    // pass_result<IR> — output of running one pass
    //
    // invalidated: extension stable_ids this pass explicitly dirtied (beyond
    // what category-driven invalidation covers).  Default empty → zero cost.
    // =========================================================================

    template <class IR>
    struct pass_result {
        IR output;
        bool changed = false;
        preserved_analysis_set preserved = preserved_analysis_set::all();
        std::vector<std::size_t> invalidated; // extension stable_ids explicitly dirtied
        std::vector<pass_diagnostic> diagnostics;

        pass_result() = default;

        explicit pass_result(IR ir, bool changed_ = false,
                             preserved_analysis_set pset = preserved_analysis_set::all())
            : output(std::move(ir)), changed(changed_), preserved(pset) {}

        [[nodiscard]] bool has_errors() const noexcept {
            for (const auto& d : diagnostics)
                if (d.level == lithe::diag::severity::error ||
                    d.level == lithe::diag::severity::fatal)
                    return true;
            return false;
        }
    };

    // =========================================================================
    // analysis_manager
    //
    // Dual-index cache:
    //   • Built-ins: std::array<std::any, analysis_count> — O(1), no allocation.
    //   • Extensions: std::unordered_map<stable_id, std::any> — keyed on
    //     analysis_key.stable_id (>= kExtensionIdBase).
    //
    // require<A>(ir): compute-once / serve-cached for any analysis with a
    //   valid analysis_descriptor<A> specialization.
    //
    // invalidate_except(preserved, explicit_ids): built-in bitset path unchanged;
    //   extension entries dropped when their stable_id appears in explicit_ids
    //   (category-driven invalidation is caller's responsibility via explicit_ids).
    // =========================================================================

    class analysis_manager {
    public:
        analysis_manager() = default;

        // --- Built-in interface (enum key) -----------------------------------

        template <class T>
        void store(const analysis_id id, T&& result) {
            cache_[static_cast<std::size_t>(id)] = std::any(std::forward<T>(result));
        }

        template <class T>
        [[nodiscard]] const T* get(const analysis_id id) const noexcept {
            const auto& slot = cache_[static_cast<std::size_t>(id)];
            return std::any_cast<T>(&slot);
        }

        [[nodiscard]] bool has(const analysis_id id) const noexcept {
            return cache_[static_cast<std::size_t>(id)].has_value();
        }

        // --- Extension interface (analysis_key) ------------------------------

        template <class T>
        void store_ext(const analysis_key& key, T&& result) {
            ext_cache_[key.stable_id] = std::any(std::forward<T>(result));
        }

        template <class T>
        [[nodiscard]] const T* get_ext(const analysis_key& key) const noexcept {
            if (auto it = ext_cache_.find(key.stable_id); it != ext_cache_.end())
                return std::any_cast<T>(&it->second);
            return nullptr;
        }

        [[nodiscard]] bool has_ext(const analysis_key& key) const noexcept {
            return ext_cache_.contains(key.stable_id);
        }

        // --- require<A>(ir) — compute-or-serve for any registered analysis ---
        //
        // Built-in path: A must have analysis_descriptor<A>::id and ::compute.
        // Extension path: uses analysis_descriptor<A>::key and ::compute.

        template <class A, class IR>
        [[nodiscard]] const typename analysis_descriptor<A>::result_t&
        require(const IR& ir) {
            if constexpr (detail::is_builtin_analysis<A>) {
                constexpr auto aid = analysis_descriptor<A>::id;
                using R = typename analysis_descriptor<A>::result_t;
                if (auto* cached = get<R>(aid)) return *cached;
                store(aid, analysis_descriptor<A>::compute(ir, *this));
                return *get<R>(aid);
            }
            else {
                constexpr auto& key = analysis_descriptor<A>::key;
                using R = typename analysis_descriptor<A>::result_t;
                if (auto* cached = get_ext<R>(key)) return *cached;
                store_ext(key, analysis_descriptor<A>::compute(ir, *this));
                return *get_ext<R>(key);
            }
        }

        // --- Invalidation ----------------------------------------------------

        // Built-in analyses NOT in preserved are dropped.
        // Extension analyses whose stable_id appears in explicit_ids are dropped.
        void invalidate_except(const preserved_analysis_set& preserved,
                               std::span<const std::size_t> explicit_ids = {}) noexcept {
            for (std::size_t i = 0; i < analysis_count; ++i) {
                const auto aid = static_cast<analysis_id>(i);
                if (!preserved.test(aid))
                    cache_[i].reset();
            }
            for (const std::size_t id : explicit_ids)
                ext_cache_.erase(id);
        }

        // Clear all cached analyses (built-ins + extensions).
        void clear() noexcept {
            for (auto& slot : cache_) slot.reset();
            ext_cache_.clear();
        }

    private:
        std::array<std::any, analysis_count> cache_{};
        std::unordered_map<std::size_t, std::any> ext_cache_;
    };

    // =========================================================================
    // pass concept — duck-typed
    //
    // A type P satisfies pass_for<IR> iff it is callable as:
    //   p(analysis_manager&, IR&&) → pass_result<IR>
    // =========================================================================

    template <class P, class IR>
    concept pass_for = requires(P& p, analysis_manager& am, IR ir) {
        { p(am, std::move(ir)) } -> std::same_as<pass_result<IR>>;
    };

    // =========================================================================
    // static_pipeline<IrHooks, Passes...>
    //
    // Runs Passes... left-to-right over an IR value, threading analysis_manager.
    // [[no_unique_address]] IrHooks is compiled away when it is no_pipeline_hooks.
    //
    // No std::function on the pass path: passes are stored by value in a tuple.
    //
    // Sink: optional diagnostic_sink set post-construction via set_sink<S>(s).
    // Stored as std::function (type-erased) to avoid adding a Sink template param
    // that would break existing static_pipeline<IrHooks,Passes...> declarations.
    // Default (no sink set): diagnostics stay in pass_result.diagnostics only.
    // =========================================================================

    template <class IrHooks = lithe::execution::no_pipeline_hooks, class... Passes>
    class static_pipeline {
    public:
        static_assert(std::is_empty_v<IrHooks>
                      || requires(IrHooks& h) { h; }, // placeholder for real hook interface
                      "IrHooks must be empty (no_pipeline_hooks) or satisfy the hook contract");

        explicit static_pipeline(Passes... passes)
            : passes_(std::move(passes)...) {}

        // Run all passes in order; returns the final pass_result.
        // IR must match what all passes accept.
        template <class IR>
            requires (pass_for<Passes, IR> && ...)
        [[nodiscard]] pass_result<IR>
        run(analysis_manager& am, IR ir) {
            return run_impl<IR>(am, std::move(ir), std::index_sequence_for < Passes...>{});
        }

        [[nodiscard]] constexpr std::size_t pass_count() const noexcept {
            return sizeof...(Passes);
        }

        template <lithe::diag::diagnostic_sink S>
        void set_sink(S s) {
            sink_ = [s = std::move(s)](const lithe::diag::diagnostic& d) mutable {
                s.on_diagnostic(d);
            };
        }

    private:
        template <class IR, std::size_t... Is>
        [[nodiscard]] pass_result<IR>
        run_impl(analysis_manager& am, IR ir, std::index_sequence<Is...>) {
            pass_result<IR> result(std::move(ir));
            ((result = run_one<Is>(am, std::move(result.output))), ...);
            return result;
        }

        template <std::size_t I, class IR>
        [[nodiscard]] pass_result<IR>
        run_one(analysis_manager& am, IR ir) {
            auto& pass = std::get < I > (passes_);
            auto r = pass(am, std::move(ir));
            am.invalidate_except(r.preserved, r.invalidated);
            if (sink_)
                for (const auto& pd : r.diagnostics)
                    sink_(pd.to_diagnostic());
            return r;
        }

        [[no_unique_address]] IrHooks hooks_{};
        std::function<void(const lithe::diag::diagnostic &)> sink_;
        std::tuple<Passes...> passes_;
    };

    // Note: IrHooks cannot be deduced from constructor arguments alone;
    // use explicit template arguments: static_pipeline<MyHooks, Pass1, Pass2>{p1, p2}.

    // =========================================================================
    // any_pass<IR> — erased pass for dynamic_pipeline
    //
    // Type-erases a pass_for<IR> callable.  Uses std::function internally because
    // dynamic_pipeline is the erasure path — the static path uses static_pipeline.
    // =========================================================================

    template <class IR>
    struct any_pass {
        using fn_type = std::function<pass_result<IR>(analysis_manager&, IR&&)>;
        fn_type fn;

        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(fn); }

        pass_result<IR> operator()(analysis_manager& am, IR ir) const {
            return fn(am, std::move(ir));
        }

        // Construct from anything satisfying pass_for<IR>.
        template <class P>
            requires pass_for<P, IR>
        explicit any_pass(P&& p)
            : fn([pass = std::forward<P>(p)](analysis_manager& am, IR ir) mutable {
                return pass(am, std::move(ir));
            }) {}
    };

    // =========================================================================
    // dynamic_pipeline<IR>
    //
    // A sequence of any_pass<IR>; grows at runtime.  For static paths prefer
    // static_pipeline which avoids std::function and type erasure entirely.
    //
    // Sink (default: no-op).  set_sink<S>(s) stores any diagnostic_sink via
    // std::function since the pipeline is already type-erased on IR.
    // =========================================================================

    template <class IR>
    class dynamic_pipeline {
    public:
        dynamic_pipeline() = default;

        void add(any_pass<IR> pass) { passes_.push_back(std::move(pass)); }

        template <class P>
            requires pass_for<P, IR>
        void add(P&& pass) { passes_.emplace_back(std::forward<P>(pass)); }

        template <lithe::diag::diagnostic_sink S>
        void set_sink(S s) {
            sink_ = [s = std::move(s)](const lithe::diag::diagnostic& d) mutable {
                s.on_diagnostic(d);
            };
        }

        [[nodiscard]] pass_result<IR>
        run(analysis_manager& am, IR ir) const {
            pass_result<IR> result(std::move(ir));
            for (const auto& p : passes_) {
                result = p(am, std::move(result.output));
                am.invalidate_except(result.preserved, result.invalidated);
                if (sink_)
                    for (const auto& pd : result.diagnostics)
                        sink_(pd.to_diagnostic());
            }
            return result;
        }

        [[nodiscard]] std::size_t pass_count() const noexcept { return passes_.size(); }
        [[nodiscard]] bool empty() const noexcept { return passes_.empty(); }

    private:
        std::vector<any_pass<IR>> passes_;
        std::function<void(const lithe::diag::diagnostic &)> sink_;
    };

    // =========================================================================
    // Compile-time assertions
    // =========================================================================

    // no_pipeline_hooks must be empty (zero-cost default verified at inclusion).
    static_assert(std::is_empty_v<lithe::execution::no_pipeline_hooks>);
} // namespace lithe::algorithms
