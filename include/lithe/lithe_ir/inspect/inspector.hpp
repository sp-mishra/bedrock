#pragma once

// =============================================================================
// lithe_ir/inspect/inspector.hpp — ir_inspector: one facade for a portable module
//
// Namespace: lithe::ir::inspect
//
// ir_inspector answers: structure (functions/imports/exports/globals/capabilities),
// views (hl_mir_view per function), dump (canonical/binary/pretty), digest,
// canonical bytes, verify, opcode signatures, and optional provenance.
//
// Borrows; never owns the portable_module (no lifetime surprise, no accidental
// mutation).  dump/dump_module in canonical/binary delegate to impl-1 providers
// so the exposed bytes are byte-identical to what impl-3 stores and impl-1
// verifies.  verify() re-runs impl-1 verify_portable (idempotent, read-only).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "../portable/module.hpp"   // portable_module, portable_*
#include "../portable/verify.hpp"   // verify_portable, verify_policy, verify_report, opcode_signature_entry, k_opcode_signatures

#include "../portable/digest.hpp"   // canonical_encode, semantic_digest
#include "handles.hpp"
#include "view.hpp"
#include "provenance_view.hpp"

namespace lithe::ir::inspect {
    // =============================================================================
    // ir_inspector — main facade (borrows const portable_module&)
    // =============================================================================

    class ir_inspector {
    public:
        explicit ir_inspector(const portable::portable_module& m) noexcept : mod_(&m) {}

        // -------------------------------------------------------------------------
        // Structure
        // -------------------------------------------------------------------------

        [[nodiscard]] std::size_t function_count() const noexcept {
            return mod_->functions.size();
        }

        [[nodiscard]] std::string_view function_name(std::uint32_t idx) const noexcept {
            if (idx < mod_->functions.size())
                return mod_->functions[idx].function_name;
            return {};
        }

        [[nodiscard]] std::span<const portable::portable_import>
        imports() const noexcept { return mod_->imports; }

        [[nodiscard]] std::span<const portable::portable_export>
        exports() const noexcept { return mod_->exports; }

        [[nodiscard]] std::span<const portable::portable_global>
        globals() const noexcept { return mod_->globals; }

        [[nodiscard]] portable::capability_set declared_capabilities() const noexcept {
            return mod_->declared_capabilities;
        }

        // -------------------------------------------------------------------------
        // Views (zero-erasure)
        // -------------------------------------------------------------------------

        // Returns a read-only hl_mir_view of the lowered HL MIR for function idx.
        [[nodiscard]] hl_mir_view function_view(std::uint32_t idx) const noexcept {
            // Caller is responsible for range-checking idx before calling.
            // If out-of-range, returns a view over a dangling reference — prevent
            // by asserting idx < function_count() at the call site.
            return hl_mir_view{mod_->functions[idx]};
        }

        // Returns the stage_key for the given stage if available.
        [[nodiscard]] std::optional<stage_key>
        available_stage(lithe::ir::stage s) const noexcept {
            // This inspector only holds HL MIR (portable_module carries hl_mir_ir).
            if (s == lithe::ir::stage::lowered || s == lithe::ir::stage::optimized) {
                return stage_key{s, mod_->schema, ir_family::hl_mir};
            }
            return std::nullopt;
        }

        // -------------------------------------------------------------------------
        // Serialize / dump
        //
        // dump(fn_idx, binary)        — canonical_encode bytes for function fn_idx
        //                               (the canonical preimage; byte-identical to
        //                               what impl-1 verify/digest use)
        // dump(fn_idx, canonical_text)— hex-encoded canonical bytes (stable, round-
        //                               trippable via re-parse)
        // dump(fn_idx, human_pretty) — human-readable text; NON-NORMATIVE
        // dump_module(binary)         — canonical_encode bytes for the full module
        // dump_module(canonical_text) — hex-encoded module canonical bytes
        // -------------------------------------------------------------------------

        [[nodiscard]] std::expected<std::vector<std::uint8_t>, inspect_error>
        dump(std::uint32_t fn_idx, ir_dump_format fmt,
             const ir_text_options& = {}) const {
            if (fn_idx >= mod_->functions.size())
                return std::unexpected(
                    inspect_error{
                        inspect_error::code::unknown_function,
                        "function index out of range"
                    });

            // Build a single-function portable_module for encoding.
            portable::portable_module single;
            single.functions.push_back(mod_->functions[fn_idx]);
            single.schema = mod_->schema;

            return dump_module_impl(single, fmt);
        }

        [[nodiscard]] std::expected<std::vector<std::uint8_t>, inspect_error>
        dump_module(ir_dump_format fmt, const ir_text_options& = {}) const {
            return dump_module_impl(*mod_, fmt);
        }

        // -------------------------------------------------------------------------
        // Identity / integrity
        // -------------------------------------------------------------------------

        // semantic_digest — delegates to impl-1 (no second implementation).
        [[nodiscard]] std::array<std::uint8_t, 64> semantic_digest() const {
            return portable::semantic_digest(*mod_);
        }

        // canonical_bytes — the digest preimage; byte-identical to what impl-1 uses.
        [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const {
            return portable::canonical_encode(*mod_);
        }

        // -------------------------------------------------------------------------
        // Verification (read-only re-run of impl-1 verify_portable)
        // -------------------------------------------------------------------------

        [[nodiscard]] portable::verify_report
        verify(const portable::verify_policy& policy = {}) const {
            return portable::verify_portable(*mod_, policy);
        }

        // -------------------------------------------------------------------------
        // Metadata discovery
        // -------------------------------------------------------------------------

        // opcode_signatures — the impl-1 canonical opcode signature table.
        [[nodiscard]] std::span<const portable::opcode_signature_entry>
        opcode_signatures() const noexcept {
            return portable::k_opcode_signatures;
        }

        // -------------------------------------------------------------------------
        // Optional provenance (attach if present)
        // -------------------------------------------------------------------------

        void attach_provenance(provenance_view pv) noexcept {
            provenance_ = std::move(pv);
        }

        [[nodiscard]] std::optional<provenance_view> provenance() const noexcept {
            return provenance_;
        }

    private:
        const portable::portable_module* mod_;
        std::optional<provenance_view> provenance_;

        // -------------------------------------------------------------------------
        // dump_module_impl — shared implementation
        //
        // binary:         canonical_encode bytes (the impl-1 canonical form)
        // canonical_text: hex-encoded canonical bytes (stable, round-trippable via
        //                 re-encoding of decoded bytes)
        // human_pretty:   indented text dump (NON-NORMATIVE; never feed to decoder)
        // -------------------------------------------------------------------------
        [[nodiscard]] static std::expected<std::vector<std::uint8_t>, inspect_error>
        dump_module_impl(const portable::portable_module& m, ir_dump_format fmt) {
            const auto canonical = portable::canonical_encode(m);

            switch (fmt) {
            case ir_dump_format::binary:
                return canonical;

            case ir_dump_format::canonical_text: {
                // Emit canonical bytes as lowercase hex, one byte per two chars.
                std::vector<std::uint8_t> out;
                out.reserve(canonical.size() * 2);
                static constexpr char hex[] = "0123456789abcdef";
                for (std::uint8_t b : canonical) {
                    out.push_back(static_cast<std::uint8_t>(hex[(b >> 4) & 0xF]));
                    out.push_back(static_cast<std::uint8_t>(hex[b & 0xF]));
                }
                return out;
            }

            case ir_dump_format::human_pretty: {
                // Non-normative human text: function list + op counts.
                // NOT round-trippable; never decode this.
                std::string txt;
                txt.reserve(256);
                txt += "; lithe portable module (human_pretty — non-normative)\n";
                txt += "; schema " + std::to_string(m.schema.major) + "."
                    + std::to_string(m.schema.minor) + "\n";
                for (std::size_t i = 0; i < m.functions.size(); ++i) {
                    txt += "fn[" + std::to_string(i) + "] "
                        + m.functions[i].function_name
                        + "  ops=" + std::to_string(m.functions[i].ops.size()) + "\n";
                }
                std::vector<std::uint8_t> out(txt.begin(), txt.end());
                return out;
            }
            }
            return std::unexpected(
                inspect_error{inspect_error::code::dump_failed, "unknown format"});
        }
    };
} // namespace lithe::ir::inspect
