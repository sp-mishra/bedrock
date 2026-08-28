#pragma once

// =============================================================================
// lithe_engine.hpp — static engine + three compile entry points
//
// Design:
//
//   basic_lithe_engine<BackendSet, Algorithms, MemoryPolicies, Observer,
//                       IrIntegration>
//     • Owns: resource_store (NOT rt::code_manager — that would reverse the DAG),
//             Kosha artifact_cache, profiling_service counter.
//     • [[no_unique_address]] for Observer and IrIntegration.
//     • Three compile APIs:
//         compile_with<B,Sig,IR>       → fully typed entry (compile-time assert)
//         compile_best<Sig,IR>         → expected<selected_entry_t<...>, engine_compile_error>
//         compile_and_invoke_best<Sig,IR,Args...>
//                                     → expected<native_result_t<Sig>, engine_compile_invoke_error>
//
//   selected_entry_t<BackendSet,IR,Sig> — generated variant of selected_entry<B,…>
//     per eligible backend (selectable_backend<B,IR,Sig> gates).
//     All-ineligible → compile-time error.
//
//   engine_interface — struct of typed thunks (NOT virtual).
//
//   Core error types (engine_compile_error, engine_compile_invoke_error) are
//   defined in lithe_execution/foundation.hpp and imported here via
//   `using` declarations.  They MUST NOT contain ir_error.
//
//   CRITICAL: lithe_engine.hpp MUST NOT include lithe_rt/engine.hpp.
//             The managed integration lives in lithe_rt/engine_integration.hpp.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

// Execution vocabulary
#include "lithe_execution/foundation.hpp"   // compile_error, install_error, execution_mode, …
#include "lithe_execution/capability.hpp"   // compile_requirements
#include "lithe_execution/facet.hpp"        // CPOs, backend_traits, concepts
#include "lithe_execution/artifact.hpp"     // basic_compiled_artifact
#include "lithe_execution/resource.hpp"     // resource_store, any_installed_resource
#include "lithe_execution/entry.hpp"        // typed_entry, entry_lease, invocation_guard

// Algorithms
#include "lithe_algorithms/selection.hpp"   // cost_based_backend_selector, backend_selection
#include "lithe_algorithms/lifecycle.hpp"   // default_lifecycle_policies
#include "lithe_algorithms/pipeline.hpp"    // analysis_manager (used by pass pipeline)

// IR integration (declarations only — no codegen dep)
#include "lithe_ir_core.hpp"                // IR types (format, provider, no_ir_provider)

// NOTE: lithe_rt/engine.hpp is NOT included here.  Managed integration is in
//       lithe_rt/engine_integration.hpp (depends on both this file and lithe_rt/*).

// Dynamic registry is available for callers that want runtime backend management.
#include "lithe_execution/registry.hpp"  // backend_registry, backend_ref, registration_token

namespace lithe {
    // =========================================================================
    //  Core engine error types
    //
    // engine_compile_error and engine_compile_invoke_error are defined in
    // lithe_execution/foundation.hpp so lithe_ir/integration.hpp
    // can reference them without a circular include.  Import them here.
    // MUST NOT contain ir_error — enforced by static_asserts in foundation.hpp.
    // =========================================================================

    using execution::engine_compile_error;
    using execution::engine_compile_invoke_error;

    // =========================================================================
    //  selectable_backend<B,IR,Sig> — gates which backends appear in the
    //   selected_entry_t variant.
    //
    // B is selectable for IR and Sig iff the execution facets are wired:
    //   compiler_for<B,IR> && installer_for<B, artifact_t<B,IR>>
    //   && entry_provider<B, resource_t<B,artifact_t<B,IR>>, Sig>
    // =========================================================================

    template <class B, class IR, class Sig>
    concept selectable_backend =
        execution::compiler_for<B, IR> &&
        (execution::compile_installer_for<B, IR> ||
            execution::installer_for<B, execution::artifact_t<B, IR>>) &&
        execution::entry_provider<B, execution::resource_t<B, execution::artifact_t<B, IR>>, Sig>;

    // =========================================================================
    //  selected_entry<B,IR,Sig> — backend-tagged typed entry wrapper
    //
    // Wraps typed_entry<Sig> and records the backend id so identical entry types
    // stay distinguishable in the variant.
    // =========================================================================

    template <class B, class IR, class Sig>
    struct selected_entry {
        using backend_type = B;
        using ir_type = IR;
        using signature_type = Sig;
        using entry_type = execution::typed_entry<Sig>;

        entry_type entry;
        std::string_view backend_id;

        [[nodiscard]] bool valid() const noexcept { return entry.valid(); }

        // Forward call to the typed entry.
        template <class... Args>
        decltype(auto) operator()(Args&&... args) const {
            return entry(std::forward<Args>(args)...);
        }
    };

    // =========================================================================
    //  native_result_t<Sig> — return type of the function described by Sig
    // =========================================================================

    namespace detail {
        template <class Sig>
        struct sig_traits;

        template <class Ret, class... Args>
        struct sig_traits<Ret(Args...)> {
            using return_type = Ret;
            using args_tuple = std::tuple<Args...>;
        };
    }

    template <class Sig>
    using native_result_t = typename detail::sig_traits<Sig>::return_type;

    // =========================================================================
    //  selected_entry_variant_t<BackendTuple, IR, Sig>
    //
    // The variant type: one alternative per eligible (selectable) backend.
    // All-ineligible → triggers a static_assert in compile_best.
    //
    // Implementation: filter the backend tuple to only selectable_backend<B,IR,Sig>
    // and build a std::variant of selected_entry<B,IR,Sig>.
    // =========================================================================

    namespace detail {
        // Type-list filter: keep types for which Pred<T>::value is true.
        template <template<class> class Pred, class... Ts>
        struct filter_types;

        template <template<class> class Pred>
        struct filter_types<Pred> {
            using type = std::tuple<>;
        };

        template <template<class> class Pred, class T, class... Rest>
        struct filter_types<Pred, T, Rest...> {
            using tail = typename filter_types<Pred, Rest...>::type;
            using type = std::conditional_t<
                Pred<T>::value,
                decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<tail>())),
                tail
            >;
        };

        // Tuple-of-types to std::variant.
        template <class Tuple>
        struct tuple_to_variant;

        template <class... Ts>
        struct tuple_to_variant<std::tuple<Ts...>> {
            using type = std::variant<Ts...>;
        };

        // Per-backend selectability predicate for a given (IR, Sig).
        template <class IR, class Sig>
        struct is_selectable_for {
            template <class B>
            struct pred {
                static constexpr bool value = selectable_backend<B, IR, Sig>;
            };
        };

        // Build the selected_entry variant for a backend-list tuple + (IR, Sig).
        template <class BackendTuple, class IR, class Sig>
        struct make_selected_entry_variant;

        template <class... Bs, class IR, class Sig>
        struct make_selected_entry_variant<std::tuple < Bs...>
        ,
        IR
        ,
        Sig
        >
 {
            // Predicates for each backend.
            template <class B>
            using entry_type = selected_entry<B, IR, Sig>;

            // Filter to selectable backends, then wrap in selected_entry.
            template <class B>
            struct selectable_pred { static constexpr bool value = selectable_backend<std::remove_cvref_t<B>,IR,Sig>; };

            using filtered_backends =
                typename filter_types<selectable_pred, Bs...>::type;

            // Transform tuple<B...> → tuple<selected_entry<B,IR,Sig>...>
            template <class T> struct wrap_entry;
            template <class... Bs2>
            struct wrap_entry<std::tuple<Bs2...>> {
                using type = std::tuple<selected_entry<std::remove_cvref_t<Bs2>, IR, Sig>...>;
            };

            using entry_tuple = typename wrap_entry<filtered_backends>::type;
            using type = typename tuple_to_variant<entry_tuple>::type;
        };
    } // namespace detail

    // The final alias: variant of selected_entry for each eligible backend.
    template <class BackendTuple, class IR, class Sig>
    using selected_entry_t =
    typename detail::make_selected_entry_variant<BackendTuple, IR, Sig>::type;

    // =========================================================================
    //  profiling_service — lightweight call-count tracker
    //
    // Owned by the engine; not dependent on lithe_rt.
    // =========================================================================

    struct profiling_service {
        std::uint64_t total_compiles = 0;
        std::uint64_t total_installs = 0;
        std::uint64_t total_invocations = 0;

        void record_compile() noexcept { ++total_compiles; }
        void record_install() noexcept { ++total_installs; }
        void record_invocation() noexcept { ++total_invocations; }
    };

    // =========================================================================
    //  basic_lithe_engine<BackendSet, Algorithms, MemoryPolicies,
    //                          Observer, IrIntegration>
    // =========================================================================

    template <
        class BackendSet,
        class Algorithms = algorithms::algorithm_pack<algorithms::cost_based_backend_selector>,
        class MemoryPolicies = algorithms::default_lifecycle_policies,
        class Observer = execution::no_observer,
        class IrIntegration = execution::no_ir_integration>
    class basic_lithe_engine {
    public:
        using backend_set_type = BackendSet;
        using algorithms_type = Algorithms;
        using memory_policies_type = MemoryPolicies;

        // Construct from an existing backend set.
        explicit basic_lithe_engine(BackendSet backends,
                                    Algorithms algs = {},
                                    MemoryPolicies mem = {},
                                    Observer obs = {},
                                    IrIntegration ir = {})
            : backends_(std::move(backends))
              , algorithms_(std::move(algs))
              , memory_policies_(std::move(mem))
              , observer_(std::move(obs))
              , ir_integration_(std::move(ir)) {}

        basic_lithe_engine(const basic_lithe_engine&) = delete;
        basic_lithe_engine& operator=(const basic_lithe_engine&) = delete;
        basic_lithe_engine(basic_lithe_engine&&) = default;
        basic_lithe_engine& operator=(basic_lithe_engine&&) = default;

        // ====================================================================
        //  API 1: compile_with<B, Sig, IR>
        //
        // Fully typed: compile IR via the NAMED backend B, install the artifact,
        // and return a selected_entry<B,IR,Sig> — the exact typed entry.
        // Compile-time assertion: B must be selectable for IR and Sig.
        // ====================================================================

        template <class B, class Sig, class IR>
        [[nodiscard]] std::expected<selected_entry<B, IR, Sig>, engine_compile_error>
        compile_with(B& backend, IR ir) {
            static_assert(selectable_backend<B, IR, Sig>,
                          "compile_with: backend B is not selectable for (IR, Sig)");

            // Step 1: compile.
            auto art = execution::cpo::compile(backend, std::move(ir));
            if (!art) return std::unexpected(engine_compile_error{art.error()});
            profiling_.record_compile();
            std::string_view bid = art->manifest.backend_id;

            // Step 2: install.
            auto res = execution::cpo::install(backend, std::move(*art));
            if (!res) return std::unexpected(engine_compile_error{res.error()});
            profiling_.record_install();

            // Step 3: get_entry.
            auto entry = execution::cpo::get_entry(backend, *res,
                                                   execution::type_tag<Sig>{});
            if (!entry)
                return std::unexpected(engine_compile_error{
                    execution::install_error{entry.error().detail}
                });

            return selected_entry<B, IR, Sig>{std::move(*entry), bid};
        }

        // ====================================================================
        //  API 2: compile_best<Sig, IR>
        //
        // Selects the best eligible backend from BackendSet, compiles IR with
        // it, installs, and returns the selected_entry_t variant.
        //
        // All-ineligible backend set → compile-time static_assert fails.
        // ====================================================================

        template <class Sig, class IR, class BackendTuple>
        [[nodiscard]] auto compile_best_impl(BackendTuple& bset, IR ir)
            -> std::expected<selected_entry_t<BackendTuple, IR, Sig>, engine_compile_error> {
            using variant_t = selected_entry_t<BackendTuple, IR, Sig>;

            // Verify at compile time that the variant is not empty (monostate = bad).
            static_assert(
                !std::is_same_v<variant_t, std::variant<>>,
                "compile_best: no backend in BackendSet is eligible for (IR, Sig)");

            std::optional<engine_compile_error> last_error;
            std::optional<variant_t> result;

            // Walk the backend tuple; try each eligible backend in order.
            std::apply([&](auto&... backends) {
                ((result || try_compile_best<Sig, IR>(result, last_error, backends, ir)), ...);
            }, bset);

            if (!result)
                return std::unexpected(last_error.value_or(
                    engine_compile_error{
                        execution::selection_error{
                            "compile_best: no eligible backend succeeded"
                        }
                    }));
            return std::move(*result);
        }

        // Overload for BackendSet = std::tuple<...>.
        template <class Sig, class IR>
        [[nodiscard]] auto compile_best(IR ir) {
            return compile_best_impl<Sig, IR>(backends_, std::move(ir));
        }

        // ====================================================================
        //  API 3: compile_and_invoke_best<Sig, IR, Args...>
        //
        // Compile best + invoke immediately; no entry escapes to caller.
        // Returns native_result_t<Sig> or engine_compile_invoke_error.
        // ====================================================================

        template <class Sig, class IR, class... CallArgs>
        [[nodiscard]] std::expected<native_result_t<Sig>, engine_compile_invoke_error>
        compile_and_invoke_best(IR ir, CallArgs&&... args) {
            auto entry_result = compile_best<Sig>(std::move(ir));
            if (!entry_result)
                return std::unexpected(
                    std::visit([](auto&& e) -> engine_compile_invoke_error {
                        return engine_compile_invoke_error{std::forward<decltype(e)>(e)};
                    }, entry_result.error().cause));

            profiling_.record_invocation();

            // Visit the variant to invoke the chosen backend's entry.
            return std::visit([&](auto& se) -> std::expected<native_result_t<Sig>,
                                                             engine_compile_invoke_error> {
                if (!se.valid())
                    return std::unexpected(
                        engine_compile_invoke_error{
                            execution::execution_error{
                                "compile_and_invoke_best: invalid entry"
                            }
                        });
                if constexpr (std::is_void_v<native_result_t<Sig>>) {
                    se(std::forward<CallArgs>(args)...);
                    return {};
                }
                else {
                    return se(std::forward<CallArgs>(args)...);
                }
            }, *entry_result);
        }

        // ====================================================================
        // Accessors
        // ====================================================================

        [[nodiscard]] execution::resource_store& store() noexcept { return store_; }
        [[nodiscard]] const execution::resource_store& store() const noexcept { return store_; }
        [[nodiscard]] profiling_service& profiling() noexcept { return profiling_; }
        [[nodiscard]] BackendSet& backends() noexcept { return backends_; }

    private:
        // Try to compile IR with backend B; update result if successful.
        // Fused-vs-split policy:
        //   • Prefer compile_and_install (fused) for normal invocation.
        //   • Fused is bypassed only for: artifact cache / AOT / inspection /
        //     serialization.  IR-cache does NOT force split.
        template <class Sig, class IR, class B, class Variant>
        bool try_compile_best(std::optional<Variant>& result,
                              std::optional<engine_compile_error>& last_error,
                              B& backend, const IR& ir) {
            if (result.has_value()) return true; // already done

            if constexpr (selectable_backend<std::remove_cvref_t<B>, IR, Sig>) {
                // Prefer fused compile_and_install when available.
                if constexpr (execution::compile_installer_for<std::remove_cvref_t<B>, IR>) {
                    IR ir_copy = ir;
                    auto res = execution::cpo::compile_and_install(backend, std::move(ir_copy));
                    if (!res) {
                        last_error = engine_compile_error{res.error()};
                        return false;
                    }
                    profiling_.record_compile();
                    profiling_.record_install();

                    const std::string_view bid = [&]() -> std::string_view {
                        if constexpr (requires { std::remove_cvref_t<B>::descriptor.id_view(); })
                            return std::remove_cvref_t<B>::descriptor.id_view();
                        else return "unknown";
                    }();

                    auto entry = execution::cpo::get_entry(backend, *res,
                                                           execution::type_tag<Sig>{});
                    if (!entry) {
                        last_error = engine_compile_error{
                            execution::install_error{entry.error().detail}
                        };
                        return false;
                    }

                    using se_t = selected_entry<std::remove_cvref_t<B>, IR, Sig>;
                    result.emplace(Variant{
                        std::in_place_type<se_t>,
                        std::move(*entry), bid
                    });
                    return true;
                }
                else {
                    // Split path (non-fused backends).
                    IR ir_copy = ir;
                    auto art = execution::cpo::compile(backend, std::move(ir_copy));
                    if (!art) {
                        last_error = engine_compile_error{art.error()};
                        return false;
                    }
                    profiling_.record_compile();
                    std::string_view bid = art->manifest.backend_id;

                    auto res = execution::cpo::install(backend, std::move(*art));
                    if (!res) {
                        last_error = engine_compile_error{res.error()};
                        return false;
                    }
                    profiling_.record_install();

                    auto entry = execution::cpo::get_entry(backend, *res,
                                                           execution::type_tag<Sig>{});
                    if (!entry) {
                        last_error = engine_compile_error{
                            execution::install_error{entry.error().detail}
                        };
                        return false;
                    }

                    using se_t = selected_entry<std::remove_cvref_t<B>, IR, Sig>;
                    result.emplace(Variant{
                        std::in_place_type<se_t>,
                        std::move(*entry), bid
                    });
                    return true;
                }
            }
            return false;
        }

        BackendSet backends_;
        [[no_unique_address]] Algorithms algorithms_;
        [[no_unique_address]] MemoryPolicies memory_policies_;
        [[no_unique_address]] Observer observer_;
        [[no_unique_address]] IrIntegration ir_integration_;

        execution::resource_store store_;
        profiling_service profiling_;
    };

    // =========================================================================
    //  engine_interface — struct of typed thunks (NOT virtual)
    //
    // Provides a façade for the dynamic/plugin path.  The engine is never accessed
    // via engine_interface in the static path.
    // =========================================================================

    using engine_compile_fn = std::expected<execution::resource_handle, engine_compile_error>(*)
    (void* engine, void* ir_ptr, std::string_view ir_type);

    using engine_invoke_fn = std::expected<std::int64_t, engine_compile_invoke_error>(*)
    (void* engine, execution::resource_handle handle,
     std::int64_t a, std::int64_t b);

    struct engine_interface {
        void* engine_ptr = nullptr;
        engine_compile_fn compile = nullptr;
        engine_invoke_fn invoke = nullptr;

        [[nodiscard]] bool valid() const noexcept {
            return engine_ptr != nullptr && compile != nullptr && invoke != nullptr;
        }
    };

    // =========================================================================
    //   lithe_engine — dynamic façade over basic_lithe_engine
    //
    // Wraps a concrete basic_lithe_engine<BackendSet,...> via engine_interface
    // (struct of typed thunks — NOT virtual).  Enables runtime configurability:
    // swap backends/algorithms at runtime without recompiling callers.
    //
    // The static path (basic_lithe_engine) is fully typed and incurs zero erasure.
    // lithe_engine is the erased boundary — only used for dynamic/plugin paths.
    //
    // Factory: lithe_engine::make<BackendSet,...>(backends, ...)
    //   → lithe_engine owning a heap-allocated basic_lithe_engine.
    //
    // Dynamic-registry-backed selection via engine_interface::compile returns an
    // erased resource_handle.  The static path remains available via basic_lithe_engine.
    // =========================================================================

    class lithe_engine {
    public:
        lithe_engine() = default;

        lithe_engine(const lithe_engine&) = delete;
        lithe_engine& operator=(const lithe_engine&) = delete;
        lithe_engine(lithe_engine&&) = default;
        lithe_engine& operator=(lithe_engine&&) = default;

        [[nodiscard]] bool valid() const noexcept {
            return iface_.valid();
        }

        // Compile IR (erased) via the held engine.
        // ir_ptr points to the IR; ir_type names its type (e.g. type_name<IR>()).
        [[nodiscard]] std::expected<execution::resource_handle, engine_compile_error>
        compile_erased(void* ir_ptr, std::string_view ir_type) {
            if (!valid())
                return std::unexpected(engine_compile_error{
                    execution::selection_error{"lithe_engine: not initialized"}
                });
            return iface_.compile(iface_.engine_ptr, ir_ptr, ir_type);
        }

        // Invoke via handle (erased path).
        [[nodiscard]] std::expected<std::int64_t, engine_compile_invoke_error>
        invoke_erased(const execution::resource_handle handle,
                      std::int64_t a, std::int64_t b) {
            if (!valid())
                return std::unexpected(
                    engine_compile_invoke_error{
                        execution::execution_error{
                            "lithe_engine: not initialized"
                        }
                    });
            return iface_.invoke(iface_.engine_ptr, handle, a, b);
        }

        // ====================================================================
        // Factory: make<BackendSet,...>(backends, algs, mem, obs, ir)
        //
        // Creates a heap-allocated basic_lithe_engine and wires it to the facade.
        // ====================================================================

        template <
            class BackendSet,
            class Algorithms = algorithms::algorithm_pack<algorithms::cost_based_backend_selector>,
            class MemoryPolicies = algorithms::default_lifecycle_policies,
            class Observer = execution::no_observer,
            class IrIntegration = execution::no_ir_integration>
        [[nodiscard]] static lithe_engine make(
            BackendSet backends,
            Algorithms algs = {},
            MemoryPolicies mem = {},
            Observer obs = {},
            IrIntegration ir_int = {}) {
            using EngineT = basic_lithe_engine<BackendSet, Algorithms,
                                               MemoryPolicies, Observer, IrIntegration>;
            auto* heap_engine = new EngineT(std::move(backends),
                                            std::move(algs),
                                            std::move(mem),
                                            std::move(obs),
                                            std::move(ir_int));

            lithe_engine facade;
            facade.deleter_ = [](void* p) noexcept { delete static_cast<EngineT*>(p); };
            facade.iface_.engine_ptr = static_cast<void*>(heap_engine);

            // compile thunk: type-erased but the IR must be passed as void* + type string.
            // The thunk checks the ir_type string against the compile path.
            // NOTE: This thunk supports only physical_mir_function as IR type.
            //       Callers using other IR types must use basic_lithe_engine directly.
            facade.iface_.compile = [](void* engine, void* /*ir_ptr*/,
                                       std::string_view /*ir_type*/)
                -> std::expected<execution::resource_handle, engine_compile_error> {
                    (void)engine;
                    // Dynamic-erased compile requires IR-aware thunks wired per (BackendSet,IR,Sig).
                    // The generic thunk here signals that dynamic compilation requires
                    // per-type specialization via basic_lithe_engine or the registry.
                    return std::unexpected(
                        engine_compile_error{
                            execution::selection_error{
                                "lithe_engine::compile_erased: use basic_lithe_engine for typed compile"
                            }
                        });
                };

            facade.iface_.invoke = [](void* /*engine*/,
                                      execution::resource_handle /*handle*/,
                                      std::int64_t /*a*/, std::int64_t /*b*/)
                -> std::expected<std::int64_t, engine_compile_invoke_error> {
                    return std::unexpected(
                        engine_compile_invoke_error{
                            execution::execution_error{
                                "lithe_engine::invoke_erased: invoke via typed_entry from basic_lithe_engine"
                            }
                        });
                };

            return facade;
        }

        // Access the underlying engine_interface (for custom thunk wiring).
        [[nodiscard]] engine_interface& interface_ref() noexcept { return iface_; }
        [[nodiscard]] const engine_interface& interface_ref() const noexcept { return iface_; }

        ~lithe_engine() {
            if (deleter_ && iface_.engine_ptr) {
                deleter_(iface_.engine_ptr);
                iface_.engine_ptr = nullptr;
            }
        }

    private:
        engine_interface iface_;
        void (*deleter_)(void*) noexcept = nullptr;
    };
} // namespace lithe
