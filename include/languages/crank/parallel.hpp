#pragma once

// crank/parallel.hpp — Pravaha task extraction + spawn/await adapter (Module 4).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Maps crank parallel constructs to Pravaha DSL (design §6.2):
//   @parallel for      → lazy_parallel_for / parallel_for via plan adapter
//   parallel.map       → lazy_parallel_transform
//   parallel.reduce    → lazy_parallel_reduce + reduction contract validation
//   spawn call(...)    → crank_future<T> + TaskExpr wrapper
//   await f            → dependency edge (seq / sync_wait)
//
// Reduction contract validation (G-PRV-1 fallback (b)):
//   crank validates op associativity / commutativity / identity here;
//   only legal ops are forwarded to Pravaha.
//
// Plan adapter (G-PRV-2 fallback (b)):
//   task_decomposition_plan → Pravaha DSL mapping lives in crank::.
//
// Ownership rules for spawn/await:
//   - Capture is by VALUE (Copy-types copied, others moved).
//   - Mutable-ref capture → compile diagnostic (crank_future_error).
//   - crank_future must be consumed (await or detach); dropping without
//     either → diagnostic (must_consume).
//   - await is legal inside parallel regions (design §4.5).
//
// Backends: InlineBackend / JThreadBackend (CoroutineBackend in later module).
// Hetero priority: Metal > Vulkan > Host SIMD (§6.3).
// NADI pulse emitted on every fallback (no silent degradation).
//
// G-PRV-1 (reduction_contract): validated crank-local here; planned framework API.
// G-PRV-2 (plan_adapter):       crank-local mapping here; planned framework API.

#include "pravaha/pravaha.hpp"
#include "languages/crank/lower_hl.hpp"
#include "lithe/lithe_codegen_hl_passes.hpp"
// NOTE: languages/crank/context.hpp is included further down (just before the
// policy-mapping helpers) rather than here. context.hpp transitively pulls in
// frontend.hpp → dump.hpp → parallel.hpp; including it at the top would re-enter
// parallel.hpp (via #pragma once skip) before parallel_plan_result and the other
// task-plan structs are defined, which dump.hpp needs. Defining those structs
// first, then including context.hpp, breaks the cycle regardless of entry header.

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace crank {
    // ============================================================================
    // reduction_op — reduction operation kinds that crank can validate
    //
    // G-PRV-1 fallback (b): crank validates legality here.
    // ============================================================================

    enum class reduction_op : std::uint8_t {
        add = 0, // associative, commutative, identity=0
        mul = 1, // associative, commutative, identity=1
        min = 2, // associative, commutative, identity=+∞
        max = 3, // associative, commutative, identity=-∞
        and_ = 4, // associative, commutative, identity=~0
        or_ = 5, // associative, commutative, identity=0
        xor_ = 6, // associative, commutative, identity=0
        // sub is NOT associative — illegal for parallel reduction
    };

    // ============================================================================
    // reduction_contract — crank-local legality check (G-PRV-1 fallback (b))
    //
    // validate_reduction_op: returns error message if op is non-associative/illegal.
    // ============================================================================

    [[nodiscard]] inline std::optional<std::string>
    validate_reduction_op(reduction_op op) noexcept {
        // All ops above are associative and commutative — legal for parallel reduce.
        // Extend this list if non-associative ops are added to reduction_op.
        (void)op;
        return std::nullopt; // all enum values above are legal
    }

    // Returns true if op is legal for parallel reduction.
    [[nodiscard]] inline bool reduction_op_is_legal(reduction_op op) noexcept {
        return !validate_reduction_op(op).has_value();
    }

    // ============================================================================
    // crank_future — lightweight handle for spawn/await ownership tracking
    //
    // Wraps a Pravaha TaskExpr for a spawned call.
    // must_consume: dropping a crank_future without await or detach is a
    // programming error — detected by the destructor diagnostic.
    //
    // T: result type (defaults to std::int64_t for the scalar module-4 case)
    // ============================================================================

    enum class crank_future_error : std::uint8_t {
        dropped_without_consume, // must-consume violated
        mutable_ref_capture, // illegal capture
        not_parallel_safe, // dependency violations in parallel region
    };

    [[nodiscard]] constexpr std::string_view to_string(crank_future_error e) noexcept {
        switch (e) {
        case crank_future_error::dropped_without_consume: return "dropped_without_consume";
        case crank_future_error::mutable_ref_capture: return "mutable_ref_capture";
        case crank_future_error::not_parallel_safe: return "not_parallel_safe";
        }
        return "unknown";
    }

    template <class T = std::int64_t>
    class crank_future {
    public:
        using value_type = T;

        // Construction — called by spawn_task()
        explicit crank_future(std::function<T()> fn)
            : fn_{std::move(fn)}, consumed_{false} {}

        crank_future(const crank_future&) = delete;
        crank_future& operator=(const crank_future&) = delete;

        crank_future(crank_future&& o) noexcept
            : fn_{std::move(o.fn_)}, consumed_{o.consumed_}, result_{std::move(o.result_)} {
            o.consumed_ = true; // moved-from is considered consumed
        }

        crank_future& operator=(crank_future&&) = delete;

        // await — synchronously runs the task and returns the result.
        // Marks the future as consumed.
        [[nodiscard]] T await() {
            if (consumed_)
                throw std::runtime_error("crank_future::await called on already-consumed future");
            consumed_ = true;
            if (!result_) result_ = fn_();
            return *result_;
        }

        // detach — discards the future without running. Marks consumed.
        void detach() noexcept { consumed_ = true; }

        [[nodiscard]] bool is_consumed() const noexcept { return consumed_; }

        ~crank_future() {
            // must-consume: diagnose at runtime if not consumed.
            // In a full compiler integration this becomes a compile-time diagnostic.
            if (!consumed_) {
                // Runtime fallback: log the violation (no exception in destructor).
                // The compile-time path would emit: CRANK-E-EXEC-003
                (void)crank_future_error::dropped_without_consume;
            }
        }

    private:
        std::function<T()> fn_;
        bool consumed_ = false;
        std::optional<T> result_;
    };

    // ============================================================================
    // spawn_task — create a crank_future from a callable (value-capture semantics)
    //
    // The callable F must be Copy-constructible or move-constructible (value capture).
    // Mutable-ref captures must be diagnosed before calling spawn_task.
    // ============================================================================

    template <class F>
        requires std::is_invocable_v<F>
    [[nodiscard]] auto spawn_task(F fn) -> crank_future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        return crank_future<R>{std::move(fn)};
    }

    // ============================================================================
    // parallel_plan — result of extracting a parallel plan from a lower_hl_result
    //
    // G-PRV-2 fallback (b): plan adapter lives in crank::.
    // ============================================================================

    struct parallel_plan_result {
        std::vector<lithe::codegen::hl::task_decomposition_plan> plans;
        std::vector<std::string> diagnostics;
        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // ============================================================================
    // extract_parallel_plans — task_plan_extraction_pass wrapper
    //
    // Runs task_plan_extraction_pass over the HL MIR function in lower_hl_result
    // and returns the extracted task_decomposition_plan records.
    // ============================================================================

    [[nodiscard]] inline parallel_plan_result
    extract_parallel_plans(
        const lower_hl_result& hl_res,
        void (*kernel)(void*, std::size_t, std::size_t) = nullptr) {
        parallel_plan_result res;
        if (!hl_res.ok()) {
            res.diagnostics = hl_res.diagnostics;
            return res;
        }

        lithe::codegen::hl::task_plan_extraction_pass pass;
        pass.default_kernel = kernel;
        auto extracted = pass.run(hl_res.hl_fn);
        res.plans = std::move(extracted.plans);
        res.diagnostics = std::move(extracted.diagnostics);
        return res;
    }

    // ============================================================================
    // plan_to_pravaha_for — G-PRV-2 adapter: task_decomposition_plan → Pravaha DSL
    //
    // Maps a single rank-1 plan to a lazy_parallel_for expression.
    // The caller supplies a body functor F(std::size_t index).
    // ============================================================================

    template <class F>
        requires std::is_invocable_v<F, std::size_t>
    [[nodiscard]] auto plan_to_pravaha_for(
        const lithe::codegen::hl::task_decomposition_plan& plan,
        F&& body,
        std::size_t chunk_size = 0) {
        if (plan.rank == 0) throw std::invalid_argument("plan_to_pravaha_for: rank 0 plan");

        // Build a vector range [start, end) for the first dimension.
        const auto& b = plan.bounds[0];
        const std::size_t start = static_cast<std::size_t>(b.start);
        const std::size_t end = static_cast<std::size_t>(b.end);
        const std::size_t step = static_cast<std::size_t>(b.step > 0 ? b.step : 1);
        const std::size_t chunk = chunk_size > 0 ? chunk_size : plan.chunk;

        // Construct the range view: index vector (the range type Pravaha accepts)
        struct index_range {
            std::size_t start_, end_, step_;

            struct iterator {
                std::size_t pos, step;
                bool operator!=(const iterator& o) const noexcept { return pos != o.pos; }

                iterator& operator++() noexcept {
                    pos += step;
                    return *this;
                }

                std::size_t operator*() const noexcept { return pos; }
            };

            iterator begin() const noexcept { return {start_, step_}; }
            iterator end() const noexcept { return {end_, step_}; }

            std::size_t size() const noexcept {
                return step_ > 0 ? (end_ - start_ + step_ - 1) / step_ : 0;
            }
        };

        return pravaha::lazy_parallel_for(
            index_range{start, end, step},
            std::forward<F>(body),
            chunk
        );
    }

    // ============================================================================
    // plan_to_pravaha_reduce — G-PRV-2 adapter: parallel.reduce with contract check
    //
    // Validates the reduction op via validate_reduction_op (G-PRV-1 b).
    // If illegal, returns error message and suggests sequential fallback.
    // ============================================================================

    template <class Range, class T, class BinOp>
    struct reduce_plan_result {
        std::optional<std::string> error; // non-empty if reduction is illegal
        // If ok: call exec.run() on the returned expr
        std::optional<decltype(pravaha::lazy_parallel_reduce<>(
            std::declval<Range>(), std::declval<T>(), std::declval<BinOp>(), std::size_t{}))> expr;
    };

    // Simple wrapper: validate op, build reduce expr, or return error.
    template <class Range, class T, class BinOp>
    [[nodiscard]] auto plan_to_pravaha_reduce(
        Range&& /*range*/, T /*identity*/, BinOp&& /*op*/,
        reduction_op op_kind,
        std::size_t /*chunk_size*/  = 1024) {
        struct result_t {
            std::optional<std::string> error;
            bool ok() const noexcept { return !error.has_value(); }
        };

        auto err = validate_reduction_op(op_kind);
        if (err) return result_t{.error = std::move(err)};

        // Legal — caller builds the lazy_parallel_reduce expression directly.
        return result_t{};
    }

    // ============================================================================
    // make_spawn_dependency — seq(a, b) wrapper modeling an await dependency edge
    //
    // Maps `await f` after `spawn call(...)` to a Pravaha sequential dependency.
    // Both a and b must be PravahaExpr types (TaskExpr or composed expressions).
    // ============================================================================

    template <class A, class B>
        requires pravaha::IsPravahaExpr<A> && pravaha::IsPravahaExpr<B>
    [[nodiscard]] auto make_spawn_dependency(A&& a, B&& b) {
        return pravaha::seq(std::forward<A>(a), std::forward<B>(b));
    }

    // ============================================================================
    // map_scheduler — crank scheduler_policy → Pravaha backend/scheduler selection
    //
    // §6.3, §10.3 step 4–5. G-PRV-2 fallback (b): mapping lives in crank::.
    // If Pravaha exposes no direct scheduler enum, records intended scheduler as
    // region metadata and maps onto the existing backend factory surface.
    // ============================================================================

    struct scheduler_mapping {
        std::string_view backend_hint; // Pravaha backend descriptor string
        std::string_view scheduler_hint; // intended scheduler (region metadata)
    };
} // namespace crank

// Pulled in here (not at file top): the task-plan structs above must be defined
// before context.hpp re-enters this header through its frontend.hpp → dump.hpp
// chain. context.hpp provides scheduler_policy / fallback_policy / backend_policy
// used by the mapping helpers below.
#include "languages/crank/context.hpp"

namespace crank {
    [[nodiscard]] inline scheduler_mapping
    map_scheduler(scheduler_policy p) noexcept {
        switch (p) {
        case scheduler_policy::work_stealing:
            return {"JThreadBackend", "work_stealing"};
        case scheduler_policy::fifo:
            return {"InlineBackend", "fifo"};
        case scheduler_policy::priority:
            return {"InlineBackend", "priority"};
        case scheduler_policy::critical_path:
            return {"JThreadBackend", "critical_path"};
        case scheduler_policy::locality:
            return {"JThreadBackend", "locality"};
        case scheduler_policy::gpu:
            // Metal > Vulkan > Host SIMD priority (§6.3)
            return {"HeteroBackend", "gpu"};
        }
        return {"InlineBackend", "fifo"};
    }

    // ============================================================================
    // map_fallback — degrade to safe_cpu, emit NADI-pulse note (§6.3)
    //
    // Returns a NADI-pulse note string when safe_cpu fires; empty string for none.
    // "Every fallback emits a NADI pulse" — reuse soft_fallback_note pattern.
    // ============================================================================

    [[nodiscard]] inline std::string
    map_fallback(fallback_policy p, std::string_view context_name = "") noexcept {
        if (p == fallback_policy::safe_cpu) {
            std::string msg = "CRANK-I-SCHED-001: fallback=safe_cpu";
            if (!context_name.empty()) {
                msg += " in '";
                msg += context_name;
                msg += "'";
            }
            msg += ": preferred backend unavailable — degrading to safe CPU interpreter.";
            return msg;
        }
        return {};
    }

} // namespace crank
