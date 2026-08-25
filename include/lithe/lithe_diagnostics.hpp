#pragma once

// =============================================================================
// lithe_diagnostics.hpp — unified diagnostic value type + pluggable sinks
//
// Namespace: lithe::diag
//
// Design:
//   • Single diagnostic value type shared across all pipeline stages.
//   • diagnostic_sink concept — pluggable, zero-cost when empty.
//   • collecting_sink  — gathers into a vector (tests / tools).
//   • nadi_sink        — routes each diagnostic to the NADI event bus (default).
//   • null_sink        — no-op (zero bytes, [[no_unique_address]] friendly).
//   • multiplex_sink   — fan-out to a variadic pack of sinks.
//   • lithe_passes.hpp aliases passes::diagnostic* → diag::* for back-compat.
//   • algorithms::pass_diagnostic adapts to this type via instr_id field.
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#if defined(__has_include)
#  if __has_include("../observability/nadi.hpp")
#    include "../observability/nadi.hpp"
#    ifndef LITHE_DIAG_HAS_NADI
#      define LITHE_DIAG_HAS_NADI 1
#    endif
#  endif
#endif
#ifndef LITHE_DIAG_HAS_NADI
#  define LITHE_DIAG_HAS_NADI 0
#endif

namespace lithe::diag {
    // =========================================================================
    // stage — which compilation stage produced this diagnostic
    // =========================================================================

    enum class stage : std::uint8_t {
        semantic,
        optimization,
        lowering,
        ir,
        backend,
        runtime
    };

    // =========================================================================
    // severity — diagnostic severity level
    // =========================================================================

    enum class severity : std::uint8_t {
        note,
        info,
        warning,
        error,
        fatal
    };

    // =========================================================================
    // source_span — location within source/IR
    // =========================================================================

    struct source_span {
        std::size_t file_id = 0;
        std::size_t offset = 0;
        std::size_t length = 0;
        std::size_t line = 0;
        std::size_t column = 0;
    };

    // =========================================================================
    // diagnostic — unified diagnostic value type
    //
    // code: stable string identifier (extensible — backends add codes without
    //       editing a core enum).  Built-in codes live in diag::codes::*.
    // stage: which pipeline stage emitted this.
    // notes: attached note sub-diagnostics (back-compat with passes::diagnostic).
    // related: nested child diagnostics (notes, related locations).
    // =========================================================================

    struct diagnostic {
        severity level = severity::info;
        diag::stage stage = diag::stage::optimization;
        std::string code = "unknown";
        std::string message;
        std::optional<source_span> span;
        std::vector<diagnostic> notes; // back-compat with passes::diagnostic.notes
        std::vector<diagnostic> related;
    };

    // diagnostic_note is a type alias — a note is just a diagnostic.
    using diagnostic_note = diagnostic;

    // =========================================================================
    // Built-in stable code strings (mirrors the old diagnostic_code enum)
    // =========================================================================

    namespace codes {
        inline constexpr const char* unknown = "unknown";
        inline constexpr const char* pass_failed = "pass_failed";
        inline constexpr const char* invalid_rewrite = "invalid_rewrite";
        inline constexpr const char* optimization_budget_exhausted = "optimization_budget_exhausted";
        inline constexpr const char* backend_capability_mismatch = "backend_capability_mismatch";
        inline constexpr const char* semantic_conflict = "semantic_conflict";
        inline constexpr const char* lowering_failed = "lowering_failed";

        // ── lithe::exec automatic execution analysis codes (LITHE-EXEC-* / LITHE-RED-*) ──
        // String codes are open-extension; no enum edit needed.
        namespace exec {
            // LITHE-EXEC-021: @gpu(required=true) region proven illegal — no legal GPU plan
            inline constexpr const char* gpu_required_illegal = "gpu_required_illegal";
            // LITHE-EXEC-034: region cannot be proven legal statically → runtime-versioned plan
            inline constexpr const char* runtime_versioned = "runtime_versioned";
            // LITHE-EXEC-041: parallel hint rejected due to dependence
            inline constexpr const char* parallel_rejected_dep = "parallel_rejected_dep";
            // LITHE-RED-012: deterministic mode — FP reduction reordering disabled
            inline constexpr const char* deterministic_reduction_disabled = "deterministic_reduction_disabled";
        } // namespace exec
    } // namespace codes

    // Backward-compat enum (passes:: users still use diagnostic_code::unknown etc.)
    // These map to the string codes above via to_code_string().
    enum class diagnostic_code : std::uint16_t {
        unknown = 0,
        pass_failed,
        invalid_rewrite,
        optimization_budget_exhausted,
        backend_capability_mismatch,
        semantic_conflict,
        lowering_failed
    };

    [[nodiscard]] inline constexpr const char* to_code_string(diagnostic_code c) noexcept {
        switch (c) {
        case diagnostic_code::pass_failed: return codes::pass_failed;
        case diagnostic_code::invalid_rewrite: return codes::invalid_rewrite;
        case diagnostic_code::optimization_budget_exhausted: return codes::optimization_budget_exhausted;
        case diagnostic_code::backend_capability_mismatch: return codes::backend_capability_mismatch;
        case diagnostic_code::semantic_conflict: return codes::semantic_conflict;
        case diagnostic_code::lowering_failed: return codes::lowering_failed;
        default: return codes::unknown;
        }
    }

    // Backward-compat alias: diagnostic_level → severity
    using diagnostic_level = severity;

    // =========================================================================
    // diagnostic_sink concept
    //
    // A sink must provide: void on_diagnostic(const diagnostic&)
    // The default (null_sink) is zero bytes and zero cost.
    // =========================================================================

    template <class S>
    concept diagnostic_sink =
        requires(S& s, const diagnostic& d) {
            { s.on_diagnostic(d) };
        };

    // =========================================================================
    // null_sink — no-op, zero bytes
    // =========================================================================

    struct null_sink {
        void on_diagnostic(const diagnostic& /*d*/) noexcept {}
    };

    static_assert(sizeof(null_sink) == 1); // may be elided via [[no_unique_address]]
    static_assert(diagnostic_sink<null_sink>);

    // =========================================================================
    // collecting_sink — collects diagnostics into a vector (tests / tools)
    // =========================================================================

    struct collecting_sink {
        std::vector<diagnostic> entries;

        void on_diagnostic(const diagnostic& d) {
            entries.push_back(d);
        }

        // Legacy emit() name used by pass_context.
        void emit(diagnostic d) { entries.push_back(std::move(d)); }

        [[nodiscard]] bool has_errors() const noexcept {
            for (const auto& d : entries)
                if (d.level == severity::error || d.level == severity::fatal)
                    return true;
            return false;
        }

        void clear() noexcept { entries.clear(); }
    };

    static_assert(diagnostic_sink<collecting_sink>);

    // =========================================================================
    // nadi_sink — routes each diagnostic to the NADI event bus
    //
    // When NADI is not present, falls back silently to collecting_sink.
    // Zero-cost when the NADI pulse is optimised away by the compiler.
    // =========================================================================

#if LITHE_DIAG_HAS_NADI

    struct nadi_sink {
        void on_diagnostic(const diagnostic& d) noexcept {
            using utils::nadi::Pulse;
            using utils::nadi::PulsePhase;

            struct payload_t {
                std::uint8_t severity_val;
                std::uint8_t stage_val;
                std::string_view code;
                std::string_view message;
            };

            auto pulse = Pulse < "lithe.diag", payload_t
            >
            {};
            pulse.phase = PulsePhase::Instant;
            pulse.id = utils::nadi::generate_event_id();
            pulse.payload = payload_t{
                static_cast<std::uint8_t>(d.level),
                static_cast<std::uint8_t>(d.stage),
                std::string_view{d.code},
                std::string_view{d.message}
            };
    // Route through thread-local default sink if available.
    // Suppressed entirely by the optimiser when no sink listens.
#  if defined(LITHE_HAS_THREAD_LOCAL_SINK)
    utils::nadi::route_pulse<utils::nadi::ThreadLocalSink> (pulse);
#  else
    utils::nadi::route_pulse<utils::nadi::NoSink> (pulse);
#  endif
    }
    };

#else // fallback when NADI absent

    struct nadi_sink {
        void on_diagnostic(const diagnostic& /*d*/) noexcept {}
    };

#endif // LITHE_DIAG_HAS_NADI

    static_assert(diagnostic_sink<nadi_sink>);

    // =========================================================================
    // multiplex_sink<Sinks...> — fan-out to all sinks
    //
    // Each Sink stored with [[no_unique_address]] — zero extra bytes for empty
    // sinks like null_sink or stateless nadi_sink.
    // =========================================================================

    template <diagnostic_sink... Sinks>
    struct multiplex_sink {
        // Store each sink. For empty sinks [[no_unique_address]] elides storage.
        std::tuple<Sinks...> sinks;

        explicit multiplex_sink() = default;
        explicit multiplex_sink(Sinks... s) : sinks(std::move(s)...) {}

        void on_diagnostic(const diagnostic& d) {
            std::apply([&d](auto&... s) {
                (s.on_diagnostic(d), ...);
            }, sinks);
        }

        template <std::size_t I>
        [[nodiscard]] auto& get() noexcept { return std::get < I > (sinks); }

        template <std::size_t I>
        [[nodiscard]] const auto& get() const noexcept { return std::get < I > (sinks); }
    };

    // Spot-check: a concrete instantiation satisfies the concept.
    static_assert(diagnostic_sink<multiplex_sink<collecting_sink, null_sink>>);

    // =========================================================================
    // collecting_sink is the alias used by diagnostic_engine (back-compat name)
    // =========================================================================
    using diagnostic_engine = collecting_sink;
} // namespace lithe::diag
