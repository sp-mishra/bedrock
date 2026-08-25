#pragma once

#include "lithe_codegen_assembler.hpp"
#include "lithe_codegen_debug_text_backend.hpp"
#include "lithe_codegen_interpreter.hpp"
#include "lithe_codegen_metal.hpp"
#include "lithe_codegen_simd.hpp"

#if defined(ASMJIT_STATIC) || defined(ASMJIT_BUILD_RELEASE) || defined(ASMJIT_BUILD_DEBUG)
#  define LITHE_HAS_ASMJIT 1
#  include "lithe_codegen_asmjit.hpp"
#endif

namespace lithe::codegen::backends {
#if defined(LITHE_HAS_ASMJIT)
    using asmjit_or_stub = asmjit_backend;
#else
    // Stub used when asmjit is unavailable — satisfies CodeEmissionTarget so it
    // fits in backend_variant, but reports an error at emit time.
    struct asmjit_backend_stub {
        [[nodiscard]] static constexpr backend_capability_set capabilities() {
            return backend_capability_set{};
        }

        [[nodiscard]] static emission_target_traits traits() {
            return emission_target_traits{
                .name = "asmjit_backend",
                .input_phase = target_input_phase::physical_mir,
                .produced_artifact = artifact_kind::jit_function,
                .capabilities = capabilities(),
            };
        }

        [[nodiscard]] static compilation_artifact emit(mir::physical_mir_function const&) {
            compilation_artifact art;
            art.diagnostics.push_back("asmjit backend unavailable: not compiled with asmjit");
            return art;
        }
    };

    using asmjit_or_stub = asmjit_backend_stub;
#endif

    enum class backend_kind : std::uint8_t {
        debug_text,
        null_backend,
        interpreter,
        text_assembly,
        asmjit,
        simd,
        metal,
    };

    using backend_variant = std::variant<
        debug_text_backend,
        null_backend,
        interpreter_backend,
        text_assembly_target,
        asmjit_or_stub,
        simd_backend,
        metal_backend
    >;

    [[nodiscard]] inline std::vector<std::string_view> list_available_backends() {
        // Only list backends that are genuinely constructible and parseable by
        // make_backend(). When asmjit is not compiled in, the stub is not a real
        // backend, so it is omitted rather than advertised as "asmjit(stub)".
        // simd_backend is always available (Highway is a portable header dep).
#if defined(LITHE_HAS_ASMJIT)
        std::vector<std::string_view> names{
            "debug_text", "null_backend", "interpreter", "text_assembly", "asmjit", "simd"};
#else
        std::vector<std::string_view> names{
            "debug_text", "null_backend", "interpreter", "text_assembly", "simd"};
#endif
        if (metal_backend::available()) names.push_back("metal");
        return names;
    }

    [[nodiscard]] inline std::optional<backend_kind> backend_kind_from_string(const std::string_view name) {
        if (name == "debug" || name == "debug_text" || name == "debug_text_backend") {
            return backend_kind::debug_text;
        }
        if (name == "null" || name == "null_backend") {
            return backend_kind::null_backend;
        }
        if (name == "interp" || name == "interpreter" || name == "interpreter_backend") {
            return backend_kind::interpreter;
        }
        if (name == "asm" || name == "text_assembly" || name == "text_assembly_target") {
            return backend_kind::text_assembly;
        }
        if (name == "jit" || name == "asmjit" || name == "asmjit_backend") {
            return backend_kind::asmjit;
        }
        if (name == "simd" || name == "simd_backend" || name == "highway") {
            return backend_kind::simd;
        }
        if (name == "metal" || name == "metal_backend" || name == "native_metal") {
            return backend_kind::metal;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline backend_variant make_backend(const backend_kind kind) {
        switch (kind) {
        case backend_kind::debug_text: return debug_text_backend{};
        case backend_kind::null_backend: return null_backend{};
        case backend_kind::interpreter: return interpreter_backend{};
        case backend_kind::text_assembly: return text_assembly_target{};
        case backend_kind::asmjit: return asmjit_or_stub{};
        case backend_kind::simd: return simd_backend{};
        case backend_kind::metal: return metal_backend{};
        }
        return debug_text_backend{};
    }

    [[nodiscard]] inline std::optional<backend_variant> make_backend(const std::string_view name) {
        const auto kind = backend_kind_from_string(name);
        if (!kind.has_value()) {
            return std::nullopt;
        }
        return make_backend(*kind);
    }

    [[nodiscard]] inline backend_result emit_with_backend(
        backend_variant& backend,
        const allocated_function_ir& fn,
        backend_state state = {}
    ) {
        return std::visit([&](auto& impl) -> backend_result {
            if constexpr (MachineCodeBackend<std::remove_reference_t<decltype(impl)>>) {
                return emit_function(impl, fn, std::move(state));
            }
            else {
                return backend_result::fail(
                    std::string(impl.traits().name) + " requires physical MIR; use emit_with_physical_mir_backend");
            }
        }, backend);
    }

    [[nodiscard]] inline compilation_artifact emit_with_physical_mir_backend(
        backend_variant& backend,
        mir::physical_mir_function const& fn
    ) {
        return std::visit([&](auto& impl) -> compilation_artifact {
            if constexpr (CodeEmissionTarget<std::remove_reference_t<decltype(impl)>>) {
                return impl.emit(fn);
            }
            else {
                compilation_artifact art;
                art.diagnostics.push_back("backend does not implement CodeEmissionTarget");
                return art;
            }
        }, backend);
    }

    // Returns true iff all instructions in fn are covered by caps.
    // Delegates to validate_backend_requirements; no RTTI, no allocation on success path.
    [[nodiscard]] inline bool verify_backend_legality(
        const mir::physical_mir_function& fn,
        const backend_capability_set& caps
    ) noexcept {
        return validate_backend_requirements(fn, caps).ok();
    }

    // Policy controlling what happens when the fallback backend is itself
    // incapable of the function. attempt_anyway (default) emits regardless and
    // records the fallback trace + an "also incapable" diagnostic — preserving
    // best-effort behavior. reject refuses to emit and returns a diagnostic
    // artifact (kind == none) instead, for callers that want a hard preflight gate.
    enum class fallback_policy { attempt_anyway, reject };

    // Attempt emission via primary_backend; fall back to fallback_backend on
    // capability mismatch.  A codegen_diagnostic_event trace is appended to
    // the artifact diagnostics when fallback fires.
    [[nodiscard]] inline compilation_artifact execute_with_fallback(
        mir::physical_mir_function const& fn,
        backend_variant& primary_backend,
        backend_variant& fallback_backend,
        fallback_policy policy = fallback_policy::attempt_anyway
    ) {
        const backend_capability_set primary_caps = std::visit(
            [](const auto& impl) -> backend_capability_set {
                if constexpr (requires { impl.capabilities(); }) {
                    return impl.capabilities();
                }
                else {
                    return backend_capability_set{};
                }
            },
            primary_backend
        );

        if (verify_backend_legality(fn, primary_caps)) {
            return emit_with_physical_mir_backend(primary_backend, fn);
        }

        const auto legality = validate_backend_requirements(fn, primary_caps);

        // Preflight the fallback — query its capabilities and check them too.
        const backend_capability_set fallback_caps = std::visit(
            [](const auto& impl) -> backend_capability_set {
                if constexpr (requires { impl.capabilities(); }) {
                    return impl.capabilities();
                }
                else {
                    return backend_capability_set{};
                }
            },
            fallback_backend
        );
        const std::string fallback_name = std::visit(
            [](const auto& impl) -> std::string {
                if constexpr (requires { impl.traits(); }) {
                    return std::string(impl.traits().name);
                }
                else {
                    return "unknown";
                }
            },
            fallback_backend
        );
        const bool fallback_ok = verify_backend_legality(fn, fallback_caps);

        // Preflight gate (opt-in): only when policy == reject do we refuse to emit
        // an invalid artifact and return diagnostics instead. The default
        // attempt_anyway falls through to best-effort emission + fallback trace.
        if (!fallback_ok && policy == fallback_policy::reject) {
            compilation_artifact art;
            art.kind = artifact_kind::none;
            art.diagnostics.push_back(
                std::string("execute_with_fallback: primary backend invalid and fallback '") +
                fallback_name + "' is also incapable; emission refused (fallback_policy::reject)");
            for (const auto& diag : legality.diagnostics) {
                art.diagnostics.push_back("fallback-reason: " + diag);
            }
            const auto fallback_legality = validate_backend_requirements(fn, fallback_caps);
            for (const auto& diag : fallback_legality.diagnostics) {
                art.diagnostics.push_back("fallback-incapable: " + diag);
            }
            return art;
        }

        compilation_artifact art = emit_with_physical_mir_backend(fallback_backend, fn);

        observability::codegen_diagnostic_event ev;
        ev.stage = "execute_with_fallback";
        ev.message = std::string("Primary backend invalid; falling back to ") + std::string(fallback_name);
        ev.timestamp_ns = observability::now_ns();
        art.diagnostics.push_back(ev.stage + ": " + ev.message);

        for (const auto& diag : legality.diagnostics) {
            art.diagnostics.push_back("fallback-reason: " + diag);
        }

        if (!fallback_ok) {
            art.diagnostics.push_back(
                "execute_with_fallback: fallback backend '" + std::string(fallback_name) +
                "' also incapable of handling this function");
        }

        return art;
    }
} // namespace lithe::codegen::backends
