#pragma once

// taranga/frontend.hpp — Frontend façade: source (WAT text or .wasm bytes) → AST.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga::frontend
//
// Single intake surface over the two frontends. Both converge on the same
// taranga_module (build_ast.hpp), so callers downstream (validate → ssa → lower →
// engine) never branch on surface syntax:
//
//   taranga::frontend::compile_wat(text)    -> frontend_result
//   taranga::frontend::compile_binary(bytes)-> frontend_result
//   taranga::frontend::compile(input)       -> frontend_result   (auto-detect)
//
// Auto-detect keys off the 4-byte \0asm magic: an image that begins with
// {00 61 73 6D} is binary, everything else is treated as WAT text. This mirrors
// wabt/wasm-tools sniffing and lets a single `compile()` accept either surface.
//
// frontend_result wraps the built taranga_module plus a definitive `ok` flag; the
// module already carries its own collecting_sink, so diagnostics flow through
// unchanged. No exceptions: a malformed input yields ok == false with diagnostics.

#include "languages/taranga/build_ast.hpp"
#include "languages/taranga/decoder_bin.hpp"
#include "languages/taranga/parser_wat.hpp"

#include "vakya/diagnostics.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace taranga::frontend {

    // The surface the input was parsed from — recorded so later bands can key
    // capability decisions (e.g. name-section availability) off the origin.
    enum class surface : std::uint8_t { wat, binary };

    struct frontend_result {
        taranga_module module;
        surface origin = surface::wat;
        [[nodiscard]] bool ok() const noexcept { return module.ok(); }
    };

    // -------------------------------------------------------------------------
    // WAT text → AST. Parses with the lexy grammar, then folds the parse_tree.
    // A parse that produces an empty tree surfaces as a TARANGA-PARSE diagnostic
    // inside build_from_wat.
    // -------------------------------------------------------------------------
    [[nodiscard]] inline frontend_result compile_wat(std::string_view text) {
        frontend_result out;
        out.origin = surface::wat;
        auto tree = grammar::parse_wat(text);
        out.module = build_from_wat(tree, text);
        return out;
    }

    // -------------------------------------------------------------------------
    // Binary .wasm → AST. Decodes the section stream, then projects raw_module
    // structurally. Decoder diagnostics (TARANGA-BIN-###) are lifted into the
    // module's sink so a single result carries the full error set.
    // -------------------------------------------------------------------------
    [[nodiscard]] inline frontend_result
    compile_binary(std::span<const std::uint8_t> image) {
        frontend_result out;
        out.origin = surface::binary;
        auto decoded = binary_decoder::decode(image);
        if (!decoded.ok()) {
            // Lift decoder diagnostics into the module sink so the caller sees them.
            for (const auto& d : decoded.diagnostics.entries)
                out.module.diagnostics.on_diagnostic(d);
            return out;
        }
        out.module = build_from_binary(*decoded.module);
        // Preserve any decode-time warnings (e.g. skipped custom sections).
        for (const auto& d : decoded.diagnostics.entries)
            out.module.diagnostics.on_diagnostic(d);
        return out;
    }

    // -------------------------------------------------------------------------
    // Auto-detecting entry: sniff the \0asm magic to pick the surface.
    // -------------------------------------------------------------------------
    [[nodiscard]] inline bool looks_binary(std::span<const std::uint8_t> image) noexcept {
        return image.size() >= 4 && image[0] == 0x00 && image[1] == 0x61 &&
               image[2] == 0x73 && image[3] == 0x6D;
    }

    [[nodiscard]] inline frontend_result
    compile(std::span<const std::uint8_t> image) {
        if (looks_binary(image)) return compile_binary(image);
        std::string_view text(reinterpret_cast<const char*>(image.data()), image.size());
        return compile_wat(text);
    }

    [[nodiscard]] inline frontend_result compile(std::string_view text) {
        std::span<const std::uint8_t> bytes(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        if (looks_binary(bytes)) return compile_binary(bytes);
        return compile_wat(text);
    }

} // namespace taranga::frontend
