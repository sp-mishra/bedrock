#pragma once

// =============================================================================
// lithe_ir/provider.hpp — generic IR provider CPO declarations
//
// Provides the import/export/validate CPO declarations consumed by the pipeline
// hook seam and 's import→compile path.
//
// NO concrete IR (no parser, no serializer, no lithe_ir.hpp dependency).
//
// Includes:
//   • import/export CPO declarations (text + binary paths)
//   • typed concepts (text_importer_for, binary_importer_for, …)
//   • no_ir_provider sentinel (available = false; safe default)
//   • ir_resolution_state — classifies IR after import
//   • diagnostic_text_stub — a debug text-export stub (NOT the canonical codec)
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <concepts>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../lithe_execution/foundation.hpp"   // ir_error, ir_kind
#include "format.hpp"                          // text_ir_view, binary_ir_view, etc.

namespace lithe::ir {
    // =========================================================================
    //a.11 ir_resolution_state — classifies IR after import
    // =========================================================================

    enum class ir_resolution_state : std::uint8_t {
        resolved = 0, // all operations recognised
        contains_opaque_optional_operations = 1, // unknown but optional ops present
        unresolved_required_operations = 2, // unknown required ops → cannot compile
    };

    // =========================================================================
    //a.3 CPO tags for generic IR provider operations
    //
    // Pattern: CPO tag struct + constexpr inline object in namespace lithe::ir::cpo.
    // Customisation via tag_invoke ADL only.
    // =========================================================================

    namespace cpo {
        // ---- import_text ----------------------------------------------------
        // tag_invoke(import_text_t{}, Provider&, text_ir_view)
        //   → expected<IR, ir_error>
        struct import_text_t {
            template <class Prov, class IR = decltype(
                          tag_invoke(std::declval<import_text_t>(),
                                     std::declval<Prov&>(),
                                     std::declval<text_ir_view>()))>
            [[nodiscard]] auto operator()(Prov&& p, text_ir_view view) const
                noexcept(noexcept(tag_invoke(*this, std::forward<Prov>(p), view)))
                -> decltype(tag_invoke(*this, std::forward<Prov>(p), view)) {
                return tag_invoke(*this, std::forward<Prov>(p), view);
            }
        };

        inline constexpr import_text_t import_text{};

        // ---- import_binary --------------------------------------------------
        // tag_invoke(import_binary_t{}, Provider&, binary_ir_view)
        //   → expected<IR, ir_error>
        struct import_binary_t {
            template <class Prov>
            [[nodiscard]] auto operator()(Prov&& p, binary_ir_view view) const
                noexcept(noexcept(tag_invoke(*this, std::forward<Prov>(p), view)))
                -> decltype(tag_invoke(*this, std::forward<Prov>(p), view)) {
                return tag_invoke(*this, std::forward<Prov>(p), view);
            }
        };

        inline constexpr import_binary_t import_binary{};

        // ---- export_text ----------------------------------------------------
        // tag_invoke(export_text_t{}, Provider const&, IR const&, format_descriptor)
        //   → expected<owned_text_ir, ir_error>
        struct export_text_t {
            template <class Prov, class IR>
            [[nodiscard]] auto operator()(Prov&& p, const IR& ir, format_descriptor fmt) const
                noexcept(noexcept(tag_invoke(*this, std::forward<Prov>(p), ir, fmt)))
                -> decltype(tag_invoke(*this, std::forward<Prov>(p), ir, fmt)) {
                return tag_invoke(*this, std::forward<Prov>(p), ir, fmt);
            }
        };

        inline constexpr export_text_t export_text{};

        // ---- export_binary --------------------------------------------------
        // tag_invoke(export_binary_t{}, Provider const&, IR const&, format_descriptor)
        //   → expected<owned_binary_ir, ir_error>
        struct export_binary_t {
            template <class Prov, class IR>
            [[nodiscard]] auto operator()(Prov&& p, const IR& ir, format_descriptor fmt) const
                noexcept(noexcept(tag_invoke(*this, std::forward<Prov>(p), ir, fmt)))
                -> decltype(tag_invoke(*this, std::forward<Prov>(p), ir, fmt)) {
                return tag_invoke(*this, std::forward<Prov>(p), ir, fmt);
            }
        };

        inline constexpr export_binary_t export_binary{};

        // ---- validate_ir ----------------------------------------------------
        // tag_invoke(validate_ir_t{}, Provider const&, IR const&)
        //   → ir_resolution_state
        struct validate_ir_t {
            template <class Prov, class IR>
            [[nodiscard]] auto operator()(Prov&& p, const IR& ir) const
                noexcept(noexcept(tag_invoke(*this, std::forward<Prov>(p), ir)))
                -> decltype(tag_invoke(*this, std::forward<Prov>(p), ir)) {
                return tag_invoke(*this, std::forward<Prov>(p), ir);
            }
        };

        inline constexpr validate_ir_t validate_ir{};
    } // namespace cpo

    // =========================================================================
    //a.3 Typed provider concepts
    // =========================================================================

    // text_importer_for<Prov, IR>: Prov can import text IR to produce IR objects.
    template <class Prov, class IR>
    concept text_importer_for = requires(Prov& p, text_ir_view view) {
        {
            cpo::import_text(p, view)
        }
        -> std::same_as<std::expected<IR, ::lithe::execution::ir_error>>;
    };

    // binary_importer_for<Prov, IR>: Prov can import binary IR.
    template <class Prov, class IR>
    concept binary_importer_for = requires(Prov& p, binary_ir_view view) {
        {
            cpo::import_binary(p, view)
        }
        -> std::same_as<std::expected<IR, ::lithe::execution::ir_error>>;
    };

    // text_exporter_for<Prov, IR>: Prov can export IR to text.
    template <class Prov, class IR>
    concept text_exporter_for = requires(Prov const& p, const IR& ir, format_descriptor fmt) {
        {
            cpo::export_text(p, ir, fmt)
        }
        -> std::same_as<std::expected<owned_text_ir, ::lithe::execution::ir_error>>;
    };

    // binary_exporter_for<Prov, IR>: Prov can export IR to binary.
    template <class Prov, class IR>
    concept binary_exporter_for = requires(Prov const& p, const IR& ir, format_descriptor fmt) {
        {
            cpo::export_binary(p, ir, fmt)
        }
        -> std::same_as<std::expected<owned_binary_ir, ::lithe::execution::ir_error>>;
    };

    // ir_validator_for<Prov, IR>: Prov can validate IR.
    template <class Prov, class IR>
    concept ir_validator_for = requires(Prov const& p, const IR& ir) {
        { cpo::validate_ir(p, ir) } -> std::same_as<ir_resolution_state>;
    };

    // =========================================================================
    //a.3 no_ir_provider — sentinel: available = false
    //
    // Used as the default IrIntegration parameter in lithe_engine.hpp so that
    // including the engine header does NOT pull in any IR codec.
    // =========================================================================

    struct no_ir_provider {
        static constexpr bool available = false;
    };

    static_assert(std::is_empty_v<no_ir_provider>);
    static_assert(!no_ir_provider::available);

    // =========================================================================
    //a.3 diagnostic_text_stub — debug-only text export provider
    //
    // NOT the canonical text codec (that is).  This stub outputs a
    // minimal human-readable representation for diagnostics and tests.
    // It satisfies text_exporter_for for std::string_view IR (diagnostic paths).
    // =========================================================================

    struct diagnostic_text_stub {
        static constexpr bool available = true;
        static constexpr std::string_view id = "lithe.ir.diagnostic_text_stub";

        // Export a string_view IR (e.g., the debug_text backend output) as text.
        [[nodiscard]] std::expected<owned_text_ir, ::lithe::execution::ir_error>
        do_export_text(std::string_view ir_text, format_descriptor fmt) const {
            if (!fmt.valid())
                return std::unexpected(::lithe::execution::ir_error{
                    "diagnostic_text_stub: invalid format_descriptor"
                });
            owned_text_ir out;
            out.format = fmt;
            out.data.assign(ir_text.begin(), ir_text.end());
            return out;
        }

        // Validate: a non-empty string is trivially "resolved".
        [[nodiscard]] ir_resolution_state
        do_validate(std::string_view ir_text) const noexcept {
            return ir_text.empty()
                       ? ir_resolution_state::unresolved_required_operations
                       : ir_resolution_state::resolved;
        }
    };
} // namespace lithe::ir
