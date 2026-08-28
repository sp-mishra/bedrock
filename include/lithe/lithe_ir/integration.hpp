#pragma once

// =============================================================================
// lithe_ir/integration.hpp — IR import→compile conveniences (, P8)
//
// Provides:
//   imported_ir<IR>        — result of import: value + resolution + diagnostics
//                           + source format.
//   ir_resolution_error    — failed resolution gate error.
//   ir_compile_error       — variant<ir_error, ir_resolution_error,
//                                     engine_compile_error> (nests core errors).
//   import_text_ir(prov, view)    → expected<imported_ir<IR>, ir_error>
//   import_binary_ir(prov, view)  → expected<imported_ir<IR>, ir_error>
//   compile_text<IR,Sig>(engine, prov, view)
//   compile_binary<IR,Sig>(engine, prov, view)
//     → expected<selected_entry_t<BackendSet,IR,Sig>, ir_compile_error>
//   engine_integration<Provider> — opt-in adapter so the engine's IrIntegration
//     slot is non-neutral only when configured.
//
// Contract (decode → validate_ir → resolution gate → engine.compile_best):
//   • Structurally-invalid IR that passes the resolution check is still rejected
//     by validate_ir → returned as ir_resolution_error.
//   • Opaque-optional IR (contains_opaque_optional_operations) is REFUSED by the
//     compile entry points; it remains printable / storable / forwardable
//     via imported_ir until a provider resolves it.
//   • Unresolved required operations → ir_resolution_error (gate before compile).
//   • target_address_width == 0 in format_descriptor → ir_error before any byte read.
//   • Separate functions per encoding (no runtime variant<text,binary,internal>).
//   • engine_compile_error nests inside ir_compile_error as engine_compile_error
//     (defined in lithe_execution/foundation.hpp, no circular include);
//     core compile_best returns engine_compile_error with NO ir_error alternative.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../lithe_execution/foundation.hpp"  // ir_error, execution_error
#include "format.hpp"                         // format_descriptor, text_ir_view, binary_ir_view
#include "provider.hpp"                       // ir_resolution_state, cpo::import_text, cpo::validate_ir

namespace lithe::ir {
    // =========================================================================
    //  diagnostic_entry — single diagnostic line from import
    // =========================================================================

    enum class diagnostic_level : std::uint8_t {
        info = 0,
        warning = 1,
        error_ = 2, // trailing underscore avoids clash with POSIX error macro
    };

    struct diagnostic_entry {
        diagnostic_level level = diagnostic_level::info;
        std::string_view source_loc; // may point into static storage or the ir text
        std::string message;

        [[nodiscard]] bool is_error() const noexcept {
            return level == diagnostic_level::error_;
        }
    };

    using diagnostic_list = std::vector<diagnostic_entry>;

    // =========================================================================
    //  imported_ir<IR> — result of a successful import
    //
    // Four fields:
    //   value       — the decoded IR object
    //   resolution  — ir_resolution_state (resolved / opaque-optional / unresolved)
    //   diagnostics — provider-emitted diagnostic messages (may be empty)
    //   source_format — the format_descriptor of the input encoding
    // =========================================================================

    template <class IR>
    struct imported_ir {
        IR value;
        ir_resolution_state resolution = ir_resolution_state::resolved;
        diagnostic_list diagnostics;
        format_descriptor source_format{};

        [[nodiscard]] bool is_resolved() const noexcept {
            return resolution == ir_resolution_state::resolved;
        }

        [[nodiscard]] bool has_opaque_optional() const noexcept {
            return resolution == ir_resolution_state::contains_opaque_optional_operations;
        }

        [[nodiscard]] bool has_errors() const noexcept {
            for (const auto& d : diagnostics) if (d.is_error()) return true;
            return false;
        }
    };

    // =========================================================================
    //  ir_resolution_error — failed resolution gate
    //
    // Distinct from ir_error (decode failure) and engine_compile_error (compile).
    // Carries the resolution state that caused the gate failure.
    // =========================================================================

    struct ir_resolution_error {
        ir_resolution_state state = ir_resolution_state::unresolved_required_operations;
        std::string_view detail;

        constexpr ir_resolution_error() = default;

        constexpr explicit ir_resolution_error(const ir_resolution_state s,
                                               const std::string_view d = {}) noexcept
            : state(s), detail(d) {}
    };

    static_assert(!std::is_same_v<ir_resolution_error, ::lithe::execution::ir_error>,
                  "ir_resolution_error must be distinct from ir_error");

    // =========================================================================
    //  ir_compile_error — variant spanning all import+compile stages
    //
    // variant<ir_error, ir_resolution_error, engine_compile_error>
    //
    // Core engine errors (engine_compile_error) NEST here; core compile_best
    // excludes ir_error — it only returns engine_compile_error.
    // =========================================================================

    // engine_compile_error is defined in lithe_execution/foundation.hpp ().
    // lithe_execution/foundation.hpp is already included above via foundation.hpp.
    // No circular include: foundation.hpp has no dependency on this file.

    // ir_stage_engine_compile_error — IR-layer wrapper for engine_compile_error.
    // Wraps execution::engine_compile_error to make it a named, distinguishable
    // alternative of ir_compile_error.  Constructible from string_view (legacy)
    // or directly from execution::engine_compile_error for lossless forwarding.
    struct ir_stage_engine_compile_error {
        ::lithe::execution::engine_compile_error cause;

        // String-view ctor for convenience / test construction.
        constexpr explicit ir_stage_engine_compile_error(const std::string_view d = {}) noexcept
            : cause(::lithe::execution::compile_error{d}) {}

        // Direct construction from engine_compile_error (lossless forwarding).
        explicit ir_stage_engine_compile_error(::lithe::execution::engine_compile_error e) noexcept
            : cause(std::move(e)) {}
    };

    using ir_compile_error = std::variant<
        ::lithe::execution::ir_error, // decode / format failure
        ir_resolution_error, // resolution gate failure
        ir_stage_engine_compile_error // compile-best failure (nested)
    >;

    // Helpers to test the ir_compile_error variant discriminant.
    [[nodiscard]] inline bool is_decode_error(const ir_compile_error& e) noexcept {
        return std::holds_alternative<::lithe::execution::ir_error>(e);
    }

    [[nodiscard]] inline bool is_resolution_error(const ir_compile_error& e) noexcept {
        return std::holds_alternative<ir_resolution_error>(e);
    }

    [[nodiscard]] inline bool is_engine_error(const ir_compile_error& e) noexcept {
        return std::holds_alternative<ir_stage_engine_compile_error>(e);
    }

    // =========================================================================
    //  import_text_ir / import_binary_ir
    //
    // Step 1: format validation (target_address_width == 0 → ir_error).
    // Step 2: decode via provider CPO.
    // Step 3: validate_ir CPO → set resolution state.
    // Step 4: collect diagnostics if provider supports them.
    //
    // Separate functions per encoding — no runtime variant.
    // =========================================================================

    template <class IR, class Provider>
        requires text_importer_for<Provider, IR>
    [[nodiscard]] std::expected<imported_ir<IR>, ::lithe::execution::ir_error>
    import_text_ir(Provider& prov, text_ir_view view) {
        // Gate: format must be valid before reading any bytes.
        if (!view.format.valid())
            return std::unexpected(::lithe::execution::ir_error{
                "import_text_ir: format_descriptor has target_address_width == 0"
            });

        auto decode_result = cpo::import_text(prov, view);
        if (!decode_result)
            return std::unexpected(decode_result.error());

        imported_ir<IR> out;
        out.value = std::move(*decode_result);
        out.source_format = view.format;

        // validate_ir if supported.
        if constexpr (ir_validator_for<Provider, IR>) {
            out.resolution = cpo::validate_ir(prov, out.value);
        }
        else {
            out.resolution = ir_resolution_state::resolved;
        }

        return out;
    }

    template <class IR, class Provider>
        requires binary_importer_for<Provider, IR>
    [[nodiscard]] std::expected<imported_ir<IR>, ::lithe::execution::ir_error>
    import_binary_ir(Provider& prov, binary_ir_view view) {
        if (!view.format.valid())
            return std::unexpected(::lithe::execution::ir_error{
                "import_binary_ir: format_descriptor has target_address_width == 0"
            });

        auto decode_result = cpo::import_binary(prov, view);
        if (!decode_result)
            return std::unexpected(decode_result.error());

        imported_ir<IR> out;
        out.value = std::move(*decode_result);
        out.source_format = view.format;

        if constexpr (ir_validator_for<Provider, IR>) {
            out.resolution = cpo::validate_ir(prov, out.value);
        }
        else {
            out.resolution = ir_resolution_state::resolved;
        }

        return out;
    }

    // =========================================================================
    //  resolution_gate — public API: query whether a resolved state is
    // acceptable for general use (print, store, forward).
    //
    // resolved                           → ok
    // contains_opaque_optional_operations → ok (optional sections preserved/skipped)
    // unresolved_required_operations      → error
    // =========================================================================

    [[nodiscard]] inline std::optional<ir_resolution_error>
    check_resolution_gate(const ir_resolution_state state) noexcept {
        if (state == ir_resolution_state::unresolved_required_operations)
            return ir_resolution_error{
                state,
                "resolution gate: unresolved required operations; cannot compile"
            };
        return std::nullopt;
    }

    //  compile_resolution_gate — strict gate used inside compile pipelines.
    //
    // Only resolved IR may be compiled: opaque-optional sections cannot be
    // lowered to machine code and must be resolved first.
    //
    // resolved                           → ok
    // contains_opaque_optional_operations → error (must resolve before compile)
    // unresolved_required_operations      → error
    // =========================================================================

    [[nodiscard]] inline std::optional<ir_resolution_error>
    compile_resolution_gate(const ir_resolution_state state) noexcept {
        if (state != ir_resolution_state::resolved)
            return ir_resolution_error{
                state,
                state == ir_resolution_state::unresolved_required_operations
                    ? "resolution gate: unresolved required operations; cannot compile"
                    : "resolution gate: contains opaque optional operations; must resolve before compile"
            };
        return std::nullopt;
    }

    // =========================================================================
    //  compile_text<IR, Sig> / compile_binary<IR, Sig>
    //
    // Full contract: decode → validate_ir → resolution gate → engine.compile_best<Sig>.
    //
    // Engine is the basic_lithe_engine (or compatible); its compile_best<Sig,IR>
    // method is called if the gate passes.
    //
    // Returns expected<Result, ir_compile_error> where Result is the engine's
    // selected_entry_t<BackendSet,IR,Sig>.
    //
    // Note: engine_compile_error from compile_best is wrapped in
    //       ir_stage_engine_compile_error (nested, not merged).
    // =========================================================================

    template <class IR, class Sig, class Engine, class Provider>
        requires text_importer_for<Provider, IR>
    [[nodiscard]] auto compile_text(Engine& engine, Provider& prov, text_ir_view view)
        -> std::expected<
            typename decltype(std::declval<Engine>().template compile_best<Sig>(std::declval<IR>()))::value_type,
            ir_compile_error> {
        // Step 1–3: import.
        auto import_result = import_text_ir<IR>(prov, view);
        if (!import_result)
            return std::unexpected(ir_compile_error{import_result.error()});

        // Step 4: strict compile-only gate (opaque-optional refused here).
        if (auto gate_err = compile_resolution_gate(import_result->resolution))
            return std::unexpected(ir_compile_error{*gate_err});

        // Step 5: compile.
        auto compile_result = engine.template compile_best<Sig>(std::move(import_result->value));
        if (!compile_result)
            return std::unexpected(ir_compile_error{
                ir_stage_engine_compile_error{compile_result.error()}
            });

        return std::move(*compile_result);
    }

    template <class IR, class Sig, class Engine, class Provider>
        requires binary_importer_for<Provider, IR>
    [[nodiscard]] auto compile_binary(Engine& engine, Provider& prov, binary_ir_view view)
        -> std::expected<
            typename decltype(std::declval<Engine>().template compile_best<Sig>(std::declval<IR>()))::value_type,
            ir_compile_error> {
        auto import_result = import_binary_ir<IR>(prov, view);
        if (!import_result)
            return std::unexpected(ir_compile_error{import_result.error()});

        if (auto gate_err = compile_resolution_gate(import_result->resolution))
            return std::unexpected(ir_compile_error{*gate_err});

        auto compile_result = engine.template compile_best<Sig>(std::move(import_result->value));
        if (!compile_result)
            return std::unexpected(ir_compile_error{
                ir_stage_engine_compile_error{compile_result.error()}
            });

        return std::move(*compile_result);
    }

    // =========================================================================
    //  engine_integration<Provider> — opt-in adapter
    //
    // Wraps a Provider instance and exposes it as the IrIntegration parameter for
    // basic_lithe_engine.  When Provider = no_ir_provider (available=false), the
    // entire infrastructure is zero-cost (empty struct).
    //
    // Usage:
    //   basic_lithe_engine<..., engine_integration<MyProvider>> engine{...,
    //       engine_integration<MyProvider>{my_provider}};
    // =========================================================================

    template <class Provider = no_ir_provider>
    class engine_integration {
    public:
        static constexpr bool active = Provider::available;

        engine_integration() = default;
        explicit engine_integration(Provider p) : provider_(std::move(p)) {}

        [[nodiscard]] Provider& provider() noexcept { return provider_; }
        [[nodiscard]] const Provider& provider() const noexcept { return provider_; }

    private:
        [[no_unique_address]] Provider provider_;
    };

    // Specialization for no_ir_provider — completely empty.
    template <>
    class engine_integration<no_ir_provider> {
    public:
        static constexpr bool active = false;
        engine_integration() = default;
    };

    static_assert(std::is_empty_v<engine_integration<no_ir_provider>>,
                  "engine_integration<no_ir_provider> must be empty");
} // namespace lithe::ir
