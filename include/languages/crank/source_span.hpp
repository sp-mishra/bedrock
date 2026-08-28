#pragma once

// crank/source_span.hpp — Source location carrier for the crank frontend.
//
// C++23, header-only, no virtual, no macros.
// Namespace: crank
//
// source_span: alias for lang::source_span (generic layer).
// span_key:    property_store key for attaching spans to Vakya nodes.
// decode_span: thin forwarder to lang::decode_span.
//
// Diagnostics: crank parse errors map to vakya::diag::diagnostic via
//   make_diagnostic() and are collected in a vakya::diag::collecting_sink.

#include "languages/generic/core/source_location.hpp"

#include <optional>

#include "vakya/diagnostics.hpp"
#include "vakya/property.hpp"

// ── Glaze JSON opt-in (guarded) ────────────────────────────────────────────
#if __has_include("glaze/glaze.hpp")
#  include "glaze/glaze.hpp"
#  define CRANK_HAS_GLAZE 1
#endif

namespace crank {

    // source_span / decode_span — re-exported from the generic layer.
    using source_span = lang::source_span;
    using lang::decode_span;

    // ============================================================================
    // span_key — property_store key for attaching source_span to Vakya nodes
    // ============================================================================

    using span_key = vakya::property_key<source_span, "crank.source_span">;

    // ============================================================================
    // Diagnostic helpers
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
} // namespace crank

// ============================================================================
// Glaze metadata (serialisation in dump.hpp)
// ============================================================================

#ifdef CRANK_HAS_GLAZE
template <>
struct glz::meta<crank::source_span> {
    using T = crank::source_span;
    static constexpr auto value = glz::object(
        "offset", &T::offset,
        "length", &T::length,
        "line", &T::line,
        "col", &T::col
    );
};
#endif
