#pragma once

// taranga/source_span.hpp — Source location carrier for the Taranga frontend.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// source_span: alias for lang::source_span (generic layer).
// span_key:    property_store key for attaching spans to Taranga nodes.
// decode_span: thin forwarder to lang::decode_span.
//
// Taranga carries TWO position spaces:
//   - text (WAT) spans decode line/col from source bytes via lang::decode_span.
//   - binary (.wasm) spans have offset == byte position in the module image,
//     length == encoded field width; line/col are left at their defaults (1,1)
//     because a binary image has no textual lines. Diagnostics on binary input
//     therefore report byte offsets, not line/col — this is intentional.
//
// Diagnostics: Taranga errors map to vakya::diag::diagnostic via
//   make_diagnostic() and are collected in a vakya::diag::collecting_sink.
// Diagnostic code convention (stable, greppable):
//   TARANGA-PARSE-###   frontend / WAT grammar
//   TARANGA-BIN-###     binary decoder
//   TARANGA-VAL-###     Wasm validation
//   TARANGA-SSA-###     stack→SSA construction
//   TARANGA-LOWER-###   HL MIR lowering + freeze/verify
//   TARANGA-EXEC-###    engine / execution

#include "languages/generic/core/source_location.hpp"
#include "languages/generic/tree/spans.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "vakya/diagnostics.hpp"
#include "vakya/property.hpp"

// ── Glaze JSON opt-in (guarded) ────────────────────────────────────────────
#if __has_include("glaze/glaze.hpp")
#  include "glaze/glaze.hpp"
#  define TARANGA_HAS_GLAZE 1
#endif

namespace taranga {

    // source_span / byte_span / decode_span — re-exported from the generic layer.
    using source_span = lang::source_span;
    using byte_span   = lang::byte_span;
    using lang::decode_span;

    // ============================================================================
    // span_key — property_store key for attaching source_span to Taranga nodes
    // ============================================================================

    using span_key = vakya::property_key<source_span, "taranga.source_span">;

    // ============================================================================
    // Diagnostic helpers — mirror crank::make_diagnostic / make_error.
    // ============================================================================

    [[nodiscard]] inline vakya::diag::diagnostic
    make_diagnostic(vakya::diag::severity sev,
                    std::string_view code,
                    std::string_view message,
                    std::optional<source_span> span = {}) {
        std::optional<vakya::diag::source_span> vspan;
        if (span) {
            vspan = vakya::diag::source_span{
                .file = {},
                .line = span->line,
                .column = span->col,
            };
        }
        return vakya::diag::diagnostic{sev, std::string(code), std::string(message), vspan};
    }

    [[nodiscard]] inline vakya::diag::diagnostic
    make_error(std::string_view code, std::string_view message,
               std::optional<source_span> span = {}) {
        return make_diagnostic(vakya::diag::severity::error, code, message, span);
    }

    [[nodiscard]] inline vakya::diag::diagnostic
    make_warning(std::string_view code, std::string_view message,
                 std::optional<source_span> span = {}) {
        return make_diagnostic(vakya::diag::severity::warning, code, message, span);
    }

    // byte_span → source_span for binary-image positions: offset/length carry the
    // module byte range; line/col stay 1 (no textual lines in a binary image).
    [[nodiscard]] inline source_span
    span_from_bytes(byte_span b) noexcept {
        return source_span{b.offset, b.length, 1u, 1u};
    }

} // namespace taranga

// ============================================================================
// Glaze metadata (serialisation in frontend dumps)
// ============================================================================

#ifdef TARANGA_HAS_GLAZE
template <>
struct glz::meta<taranga::source_span> {
    using T = taranga::source_span;
    static constexpr auto value = glz::object(
        "offset", &T::offset,
        "length", &T::length,
        "line", &T::line,
        "col", &T::col
    );
};
#endif
