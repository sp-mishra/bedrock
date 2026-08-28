#pragma once

// =============================================================================
// backends/lithe_execution_backends.hpp — per-backend facet adapters +
//   static_backend_set<Backends...>
//
// This header:
//   1. Provides tag_invoke facet adapters for the four remaining backends:
//      debug_text_backend, null_backend, text_assembly_target, asmjit_or_stub.
//      (interpreter adapter is in lithe_codegen_interpreter_facet.hpp)
//
//   2. Defines static_backend_set<Backends...>: a zero-erasure, fold-based
//      backend registry that dispatches compile/install/get_entry/invoke via
//      if constexpr facet checks.
//
//   3. backend_variant + execute_with_fallback remain unchanged (additive).
//
// Design (, , P4):
//   • Each adapter wraps the backend's emit() into compile / optionally install.
//   • Adapters are additive — no existing code paths are modified.
//   • static_backend_set folds over the type list; no erasure, no RTTI.
//   • AsmJIT adapter is guarded by LITHE_HAS_ASMJIT.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "../lithe_execution/facet.hpp"
#include "../lithe_execution/artifact.hpp"
#include "../lithe_execution/resource.hpp"
#include "../lithe_execution/entry.hpp"
#include "../lithe_execution/capability.hpp"         // compile_requirements, mode_gate deps
#include "lithe_codegen_interpreter_facet.hpp"       // interpreter adapter + traits
#include "lithe_codegen_backend_registry.hpp"         // backend_variant, null_backend, text_assembly, debug_text

#include <expected>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lithe::execution {
    // =========================================================================
    //  Text-based artifact resource type
    //
    // debug_text_backend and text_assembly_target produce text artifacts.
    // A "resource" for these is just the text payload — there is no
    // install step (no executable memory).  We model them as a thin wrapper.
    // =========================================================================

    struct text_resource {
        std::string text;
        frame_counter_ref counter = make_frame_counter();

        [[nodiscard]] bool valid() const noexcept { return !text.empty(); }
    };

    // =========================================================================
    //  Null resource — the null_backend produces no useful output
    // =========================================================================

    struct null_resource {
        frame_counter_ref counter = make_frame_counter();
        [[nodiscard]] bool valid() const noexcept { return true; }
    };

    // =========================================================================
    // backend_traits specialisations for text/null backends
    // =========================================================================

    using debug_text_backend_t = codegen::backends::debug_text_backend;
    using null_backend_t = codegen::backends::null_backend;
    using text_assembly_target_t = codegen::backends::text_assembly_target;

    template <>
    struct backend_traits<debug_text_backend_t> {
        template <class IR>
        using artifact = basic_compiled_artifact<std::string>;
        template <class A>
        using resource = text_resource;
        template <class R, class Sig>
        using entry = typed_entry<Sig>;
    };

    template <>
    struct backend_traits<null_backend_t> {
        template <class IR>
        using artifact = basic_compiled_artifact<std::monostate>;
        template <class A>
        using resource = null_resource;
        template <class R, class Sig>
        using entry = typed_entry<Sig>;
    };

    template <>
    struct backend_traits<text_assembly_target_t> {
        template <class IR>
        using artifact = basic_compiled_artifact<std::string>;
        template <class A>
        using resource = text_resource;
        template <class R, class Sig>
        using entry = typed_entry<Sig>;
    };

    // =========================================================================
    //  AsmJIT payload/resource types (guarded by LITHE_HAS_ASMJIT)
    //
    // These live in lithe::execution because they appear in backend_traits and
    // entry types used from the execution vocabulary side.
    // =========================================================================

#if defined(LITHE_HAS_ASMJIT)

    // jit_compiled_payload — complete definition of the forward-declared type.
    // Holds the original compilation_artifact (via its shared handle) rather than
    // moving the jit_function_handle out — the handle is tied to the JitRuntime
    // stored in the artifact's shared_ptr.
    struct jit_compiled_payload {
        codegen::compilation_artifact stored_art;

        [[nodiscard]] bool valid() const noexcept {
            auto* h = codegen::backends::asmjit_backend::get_handle(stored_art);
            return h != nullptr && h->valid();
        }

        [[nodiscard]] std::int64_t call(std::int64_t a, std::int64_t b) const {
            auto* h = codegen::backends::asmjit_backend::get_handle(stored_art);
            return h ? h->call(a, b) : 0;
        }

        [[nodiscard]] double call_f64(std::int64_t a, std::int64_t b) const {
            auto* h = codegen::backends::asmjit_backend::get_handle(stored_art);
            return h ? h->call_f64(a, b) : 0.0;
        }
    };

    struct jit_resource {
        std::shared_ptr<jit_compiled_payload> payload;
        frame_counter_ref counter = make_frame_counter();

        [[nodiscard]] bool valid() const noexcept {
            return payload != nullptr && payload->valid();
        }
    };

    template <>
    struct backend_traits<codegen::backends::asmjit_backend> {
        template <class IR>
        using artifact = basic_compiled_artifact<jit_compiled_payload>;
        template <class A>
        using resource = jit_resource;
        template <class R, class Sig>
        using entry = typed_entry<Sig>;
    };

#endif // LITHE_HAS_ASMJIT

    // =========================================================================
    //  mode_gate — trivial gating predicate ()
    //
    // Templated on CapSet to accept both lithe::execution::backend_capability_set
    // and lithe::codegen::backend_capability_set (same layout, different types).
    // =========================================================================

    template <class CapSet>
    [[nodiscard]] constexpr bool
    mode_gate(const compile_requirements& reqs,
              const CapSet& caps,
              const execution_mode mode_hint) noexcept {
        // Capability check: required caps must be present.
        if (!reqs.satisfies_required(backend_capability_set{caps.bits})) return false;
        // Mode check: the given mode must be allowed.
        if (!reqs.mode_allowed(mode_hint)) return false;
        return true;
    }

    // =========================================================================
    //  static_backend_set<Backends...>
    //
    // A zero-erasure, compile-time-ordered backend set.
    // Provides:
    //   visit_all(fn)     — calls fn(backend) for each backend in order.
    //   select_first(reqs, mode)
    //                    — returns pointer to first backend satisfying reqs+mode.
    //   compile_first(reqs, mode, ir)
    //                    — compiles via the first capable backend; returns the
    //                       erased artifact (any_compiled_artifact) or error.
    //
    // Fold + if constexpr: no erasure, no runtime dispatch table, no RTTI.
    // backend_variant stays for execute_with_fallback callers.
    // =========================================================================

    template <class... Backends>
    class static_backend_set {
    public:
        // Construct from lvalue references to externally-owned backends.
        explicit static_backend_set(Backends&... backends)
            : backends_(std::forward_as_tuple(backends...)) {}

        // visit_all(fn) — fn(b) called for each backend; b is B&.
        template <class Fn>
        void visit_all(Fn&& fn) {
            std::apply([&](Backends&... bs) {
                (fn(bs), ...);
            }, backends_);
        }

        // visit_all const overload
        template <class Fn>
        void visit_all(Fn&& fn) const {
            std::apply([&](const Backends&... bs) {
                (fn(bs), ...);
            }, backends_);
        }

        // select_first — returns void* to first backend that satisfies reqs+mode.
        // Caller casts via type_token_for<B>().
        [[nodiscard]] void*
        select_first(const compile_requirements& reqs,
                     const execution_mode mode_hint) noexcept {
            void* result = nullptr;
            std::apply([&](Backends&... bs) {
                ((result == nullptr && try_select(result, bs, reqs, mode_hint)), ...);
            }, backends_);
            return result;
        }

    private:
        template <class B>
        static bool try_select(void*& out, B& b,
                               const compile_requirements& reqs,
                               const execution_mode mode) noexcept {
            if constexpr (requires { B::capabilities(); }) {
                if (mode_gate(reqs, B::capabilities(), mode)) {
                    out = static_cast<void*>(&b);
                    return true;
                }
            }
            return false;
        }

        std::tuple<Backends&...> backends_;
    };

    // Deduction guide for lvalue refs.
    template <class... Backends>
    static_backend_set(Backends&...) -> static_backend_set<Backends...>;
} // namespace lithe::execution

// =============================================================================
// tag_invoke customisations for text/null/asmjit backends — in
// lithe::codegen::backends so ADL finds them via the backend argument.
// =============================================================================

namespace lithe::codegen::backends {
    // =========================================================================
    //  Facet adapters — debug_text_backend
    // =========================================================================

    [[nodiscard]] inline
    std::expected<
        lithe::execution::basic_compiled_artifact<std::string>,
        lithe::execution::compile_error>
    tag_invoke(lithe::execution::cpo::compile_t,
               debug_text_backend& backend,
               mir::physical_mir_function&& fn) {
        auto art = backend.emit(fn);
        if (art.kind == codegen::artifact_kind::none)
            return std::unexpected(lithe::execution::compile_error{"debug_text emit failed"});

        lithe::execution::artifact_manifest manifest;
        manifest.produced_from = lithe::execution::ir_kind::physical_mir;
        manifest.role = lithe::execution::artifact_class::text_report;
        manifest.backend_id = "lithe.backend.debug_text";

        return lithe::execution::basic_compiled_artifact<std::string>{
            std::move(manifest), art.text_payload, {}
        };
    }

    [[nodiscard]] inline
    std::expected<lithe::execution::text_resource, lithe::execution::install_error>
    tag_invoke(lithe::execution::cpo::install_t,
               debug_text_backend& /*backend*/,
               lithe::execution::basic_compiled_artifact<std::string>&& art) {
        return lithe::execution::text_resource{
            std::move(art.payload), lithe::execution::make_frame_counter()
        };
    }

    // =========================================================================
    //  Facet adapters — null_backend
    // =========================================================================

    [[nodiscard]] inline
    std::expected<
        lithe::execution::basic_compiled_artifact<std::monostate>,
        lithe::execution::compile_error>
    tag_invoke(lithe::execution::cpo::compile_t,
               null_backend& /*backend*/,
               mir::physical_mir_function&& /*fn*/) {
        lithe::execution::artifact_manifest manifest;
        manifest.produced_from = lithe::execution::ir_kind::physical_mir;
        manifest.role = lithe::execution::artifact_class::metadata_only;
        manifest.backend_id = "lithe.backend.null";

        return lithe::execution::basic_compiled_artifact<std::monostate>{
            std::move(manifest), {}, {}
        };
    }

    [[nodiscard]] inline
    std::expected<lithe::execution::null_resource, lithe::execution::install_error>
    tag_invoke(lithe::execution::cpo::install_t,
               null_backend& /*backend*/,
               lithe::execution::basic_compiled_artifact<std::monostate>&& /*art*/) {
        return lithe::execution::null_resource{lithe::execution::make_frame_counter()};
    }

    // =========================================================================
    //  Facet adapters — text_assembly_target
    // =========================================================================

    [[nodiscard]] inline
    std::expected<
        lithe::execution::basic_compiled_artifact<std::string>,
        lithe::execution::compile_error>
    tag_invoke(lithe::execution::cpo::compile_t,
               text_assembly_target& backend,
               mir::physical_mir_function&& fn) {
        auto art = backend.emit(fn);
        if (art.kind == codegen::artifact_kind::none)
            return std::unexpected(lithe::execution::compile_error{"text_assembly emit failed"});

        lithe::execution::artifact_manifest manifest;
        manifest.produced_from = lithe::execution::ir_kind::physical_mir;
        manifest.role = lithe::execution::artifact_class::text_report;
        manifest.backend_id = "lithe.backend.text_assembly";

        return lithe::execution::basic_compiled_artifact<std::string>{
            std::move(manifest), art.text_payload, {}
        };
    }

    [[nodiscard]] inline
    std::expected<lithe::execution::text_resource, lithe::execution::install_error>
    tag_invoke(lithe::execution::cpo::install_t,
               text_assembly_target& /*backend*/,
               lithe::execution::basic_compiled_artifact<std::string>&& art) {
        return lithe::execution::text_resource{
            std::move(art.payload), lithe::execution::make_frame_counter()
        };
    }

    // =========================================================================
    //  AsmJIT backend adapter (guarded by LITHE_HAS_ASMJIT)
    // =========================================================================

#if defined(LITHE_HAS_ASMJIT)

    [[nodiscard]] inline
    std::expected<
        lithe::execution::basic_compiled_artifact<lithe::execution::jit_compiled_payload>,
        lithe::execution::compile_error>
    tag_invoke(lithe::execution::cpo::compile_t,
               asmjit_backend& backend,
               mir::physical_mir_function&& fn) {
        auto art = backend.emit(fn);
        auto* jit_hdl = asmjit_backend::get_handle(art);
        if (!jit_hdl || !jit_hdl->valid())
            return std::unexpected(
                lithe::execution::compile_error{"asmjit: no valid JIT handle produced"});

        lithe::execution::artifact_manifest manifest;
        manifest.produced_from = lithe::execution::ir_kind::physical_mir;
        manifest.role = lithe::execution::artifact_class::native_code;
        manifest.backend_id = "lithe.backend.asmjit";

        lithe::execution::jit_compiled_payload payload;
        payload.stored_art = std::move(art);

        return lithe::execution::basic_compiled_artifact<lithe::execution::jit_compiled_payload>{
            std::move(manifest), std::move(payload), {}
        };
    }

    [[nodiscard]] inline
    std::expected<lithe::execution::jit_resource, lithe::execution::install_error>
    tag_invoke(lithe::execution::cpo::install_t,
               asmjit_backend& /*backend*/,
               lithe::execution::basic_compiled_artifact<
                   lithe::execution::jit_compiled_payload>&& art) {
        if (!art.valid() || !art.payload.valid())
            return std::unexpected(lithe::execution::install_error{"invalid asmjit artifact"});

        lithe::execution::jit_resource res;
        res.payload = std::make_shared<lithe::execution::jit_compiled_payload>(
            std::move(art.payload));
        return res;
    }

    [[nodiscard]] inline
    std::expected<
        lithe::execution::typed_entry < std::int64_t(std::int64_t, std::int64_t)>,
    lithe::execution::execution_error>
    tag_invoke(lithe::execution::cpo::get_entry_t,
               asmjit_backend& /*backend*/,
               lithe::execution::jit_resource& res,
               lithe::execution::type_tag<std::int64_t(std::int64_t, std::int64_t)>) {
        if (!res.valid())
            return std::unexpected(lithe::execution::execution_error{"jit_resource invalid"});

        lithe::execution::entry_lease lease{res.counter};
        auto fn = [payload = res.payload](std::int64_t a, std::int64_t b) -> std::int64_t {
            return payload->call(a, b);
        };
        return lithe::execution::typed_entry < std::int64_t(std::int64_t, std::int64_t) >
        {
            std::move(lease),
                std::function < std::int64_t(std::int64_t, std::int64_t) >
            {
                std::move(fn)
            }
        };
    }

    [[nodiscard]] inline
    std::expected<
        lithe::execution::typed_entry < double(std::int64_t, std::int64_t)>,
    lithe::execution::execution_error>
    tag_invoke(lithe::execution::cpo::get_entry_t,
               asmjit_backend& /*backend*/,
               lithe::execution::jit_resource& res,
               lithe::execution::type_tag<double(std::int64_t, std::int64_t)>) {
        if (!res.valid())
            return std::unexpected(lithe::execution::execution_error{"jit_resource invalid"});

        lithe::execution::entry_lease lease{res.counter};
        auto fn = [payload = res.payload](std::int64_t a, std::int64_t b) -> double {
            return payload->call_f64(a, b);
        };
        return lithe::execution::typed_entry < double(std::int64_t, std::int64_t) >
        {
            std::move(lease),
                std::function < double(std::int64_t, std::int64_t) >
            {
                std::move(fn)
            }
        };
    }

    // =========================================================================
    // , P9 AsmJIT compile_and_install — fused path
    //
    // AsmJIT's JitRuntime::add() has no natural compile/install boundary.
    // This fused tag_invoke emits and installs in one step, returning a
    // jit_resource (with a real native entry).
    //
    // The engine uses compile_and_install (fused) when no artifact cache or AOT
    // serialization is needed.  Split compile+install is used only for:
    //   artifact cache / AOT / inspection / serialization.
    //
    // Returns compile_install_error (distinct) when AsmJIT cannot attribute
    // the failure to a stage.
    //
    // Guarded by LITHE_HAS_ASMJIT.
    // =========================================================================

    [[nodiscard]] inline
    std::expected<lithe::execution::jit_resource, lithe::execution::compile_install_error>
    tag_invoke(lithe::execution::cpo::compile_and_install_t,
               asmjit_backend& backend,
               mir::physical_mir_function&& fn) {
        // AsmJIT emit() handles both compile and install internally.
        auto art = backend.emit(fn);
        auto* jit_hdl = asmjit_backend::get_handle(art);
        if (!jit_hdl || !jit_hdl->valid())
            return std::unexpected(lithe::execution::compile_install_error{
                "asmjit: compile_and_install produced no valid JIT handle"
            });

        lithe::execution::jit_compiled_payload payload;
        payload.stored_art = std::move(art);

        lithe::execution::jit_resource res;
        res.payload = std::make_shared<lithe::execution::jit_compiled_payload>(
            std::move(payload));
        return res;
    }

#endif // LITHE_HAS_ASMJIT
} // namespace lithe::codegen::backends
