#pragma once

// =============================================================================
// lithe_execution/artifact.hpp — payload-parameterised compiled artifacts
//
// An artifact is the OUTPUT of a compile step.  It is distinct from a resource
// (the output of an install step) and from an entry (a callable handle).
//
// Core design decisions (, , ):
//   • basic_compiled_artifact<Payload, Metadata> owns Payload — a backend-specific
//     typed value, NOT a raw byte span.  Metadata defaults to compilation_metadata.
//   • Metadata is split by lifecycle stage so backends that omit native code
//     (interpreter, debug_text) never carry empty native fields.
//   • any_compiled_artifact erase() provides the dynamic boundary — move-only;
//     the static path never allocates an erased artifact.
//
// Aliases shipped for the five backends:
//   interpreter_artifact   payload = interpreter_program (bytecode plan)
//   asmjit_artifact        payload = jit_function_handle
//   object_artifact        payload = object_bytes  (relocatable binary)
//   debug_text_artifact    payload = std::string   (human-readable listing)
//   text_asm_artifact      payload = std::string   (text assembly)
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <any>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "foundation.hpp"   // ir_kind, artifact_class, compilation errors

namespace lithe::execution {
    // =========================================================================
    //  Artifact manifest — backend-neutral provenance
    // =========================================================================

    struct artifact_manifest {
        ir_kind produced_from = ir_kind::physical_mir;
        artifact_class role = artifact_class::none;
        std::string_view backend_id = {}; // points to static storage
        std::uint64_t version = 0; // monotonic, 0 = unknown

        [[nodiscard]] constexpr bool valid() const noexcept {
            return role != artifact_class::none;
        }
    };

    // =========================================================================
    //  Metadata lifecycle stages
    // =========================================================================

    // compilation_metadata — known immediately after compile(), before install().
    struct compilation_metadata {
        std::string_view source_name = {};
        std::uint64_t ir_hash = 0;
        std::optional<std::string> diagnostics_text;
        bool has_debug_info = false;
    };

    // installation_metadata — filled in by install(); not available on artifact.
    // Stored in resource, not in artifact (lifecycle split ).
    struct installation_metadata {
        std::uint64_t install_cookie = 0; // backend-defined stable id
        std::size_t code_size_bytes = 0;
        bool w_xor_x = false;
    };

    // code_version_metadata — assembled in stages (compile + install + runtime).
    // The one shared metadata system referenced by GC, deopt, AOT, tiering ().
    // This is the FULL bundle; partial slices are compilation_metadata /
    // installation_metadata above.
    struct code_version_metadata {
        compilation_metadata compile;
        installation_metadata install;
        std::uint64_t version_id = 0;
        bool is_aot_ready = false;
    };

    // =========================================================================
    //  basic_compiled_artifact<Payload, Metadata>
    //
    // Payload is backend-specific (interpreter_program, jit_function_handle, …).
    // Metadata defaults to compilation_metadata (the compile-stage slice).
    // Move-only when Payload is move-only.
    // =========================================================================

    template <class Payload, class Metadata = compilation_metadata>
    struct basic_compiled_artifact {
        artifact_manifest manifest;
        Payload payload;
        Metadata metadata;

        basic_compiled_artifact() = default;

        explicit basic_compiled_artifact(artifact_manifest m, Payload p, Metadata meta = {})
            : manifest(std::move(m)), payload(std::move(p)), metadata(std::move(meta)) {}

        // Convenience: check if the artifact carries real content.
        [[nodiscard]] bool valid() const noexcept { return manifest.valid(); }
    };

    static_assert(std::is_move_constructible_v<basic_compiled_artifact<std::string>>);

    // =========================================================================
    //  any_compiled_artifact — erased dynamic boundary
    //
    // The static path uses concrete artifact types; the erased boundary is used
    // only for dynamic registry / plugin / reflection paths and AOT serialisation.
    //
    // erase() moves a concrete artifact into any_compiled_artifact.
    // typed_artifact<A>() extracts it back (returns nullptr on type mismatch).
    // =========================================================================

    class any_compiled_artifact {
    public:
        any_compiled_artifact() = default;

        // Only movable — copying erased artifact is expensive and rarely correct.
        any_compiled_artifact(const any_compiled_artifact&) = delete;
        any_compiled_artifact& operator=(const any_compiled_artifact&) = delete;
        any_compiled_artifact(any_compiled_artifact&&) noexcept = default;
        any_compiled_artifact& operator=(any_compiled_artifact&&) noexcept = default;

        [[nodiscard]] bool valid() const noexcept { return manifest_.valid() && payload_.has_value(); }

        [[nodiscard]] const artifact_manifest& manifest() const noexcept { return manifest_; }

        // Typed extraction — returns pointer into storage (valid until this is moved/destroyed).
        template <class A>
        [[nodiscard]] A* get() noexcept {
            return std::any_cast<A>(&payload_);
        }

        template <class A>
        [[nodiscard]] const A* get() const noexcept {
            return std::any_cast<const A>(&payload_);
        }

        // Move a concrete artifact into the erased boundary.
        template <class Payload, class Metadata>
        [[nodiscard]] static any_compiled_artifact
        erase(basic_compiled_artifact<Payload, Metadata>&& art) {
            any_compiled_artifact out;
            out.manifest_ = art.manifest;
            out.payload_ = std::move(art);
            return out;
        }

    private:
        artifact_manifest manifest_;
        std::any payload_;
    };

    // =========================================================================
    //  Payload types for the five backends
    // =========================================================================

    // Interpreter: the program object produced by the interpreter's compile step.
    // This is a plan structure that can be installed (bound to a resource) and
    // later invoked.  The interpreter backend owns the definition in its header;
    // here we declare a forward-compatible placeholder type alias pattern.
    //
    // Each backend defines its own payload type in its own header; we provide the
    // canonical alias names here so code that includes only artifact.hpp can use
    // the type-alias names symbolically (the full type is complete in the adapter).

    // Forward-declare a minimal interpreter payload struct that the adapter fills in.
    // The adapter header (lithe_codegen_interpreter_facet.hpp) provides the real definition.
    struct interpreter_program; // defined in adapter header

    using interpreter_artifact = basic_compiled_artifact<interpreter_program>;

    // Object artifact: relocatable binary bytes.
    struct object_bytes {
        std::vector<std::uint8_t> data;
        std::string section_name;
    };

    using object_artifact = basic_compiled_artifact<object_bytes>;

    // Text-based artifacts (debug_text, text_assembly) share a string payload.
    using debug_text_artifact = basic_compiled_artifact<std::string>;
    using text_asm_artifact = basic_compiled_artifact<std::string>;

    // AsmJIT artifact: wraps the jit_function_handle (defined in asmjit backend header).
    // Forward reference only — full type in lithe_codegen_asmjit_facet.hpp.
    struct jit_compiled_payload; // defined in adapter header
    using asmjit_artifact = basic_compiled_artifact<jit_compiled_payload>;
} // namespace lithe::execution
