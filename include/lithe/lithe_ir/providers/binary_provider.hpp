#pragma once

// =============================================================================
// lithe_ir/providers/binary_provider.hpp — binary Lithe IR provider
//
//
// Satisfies:
//   binary_importer_for<binary_provider<DV,SV>, lithe_binary_ir_doc>
//   binary_exporter_for<binary_provider<DV,SV>, lithe_binary_ir_doc>
//   ir_validator_for<binary_provider<DV,SV>, lithe_binary_ir_doc>
//
// Security ordering (hard-enforced):
//   1. Structural validation BEFORE large allocation
//      (magic / address_width / payload_size / section directory bounds)
//   2. Integrity digest verification BEFORE decode
//   3. Authenticity signature verification BEFORE the IR is trusted or compiled
//
// Wire rules:
//   • No serialised pointers; no size_t on the wire; fixed-width ints only.
//   • Bounds-check BEFORE allocation.
//   • Checked offset/size arithmetic (no integer overflow).
//   • Unknown REQUIRED section → reject.
//   • Unknown OPTIONAL section → preserve as opaque (→ contains_opaque_optional_operations).
//   • Major mismatch → reject; minor compat descriptor-driven.
//
// Operation identity wiring ( item 4):
//   Each op in the binary IR carries a serialised_operation_identity
//   {domain_id, operation_id, schema_version}.  The decode path routes through:
//     • known + compatible schema  → decode
//     • unknown optional           → preserve opaque (if limits allow)
//     • unknown required           → reject
//     • known + incompatible schema → reject OR invoke registered upgrade_ir
//
// Template parameters:
//   DigestVerifier  — satisfies digest_verifier concept (default: no_digest_verifier)
//   SigVerifier     — satisfies signature_verifier concept (default: no_signature_verifier)
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <any>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../lithe_execution/foundation.hpp"   // ir_error
#include "../format.hpp"                          // format_descriptor, etc.
#include "../provider.hpp"                        // ir_resolution_state, cpo tags
#include "../integration.hpp"                     // diagnostic_list
#include "../upgrade.hpp"                         // upgrade_registry, upgrade_key
#include "../security_envelope.hpp"               // envelope types

namespace lithe::ir {
    // =========================================================================
    //a.8 serialised_operation_identity — per-op wire identity
    //
    // Carried by each op record in the binary format.  Drives upgrade routing.
    // =========================================================================

    struct serialised_operation_identity {
        std::string stable_domain_id; // e.g. "lithe.core", "lithe.hl"
        std::string stable_operation_id; // e.g. "add", "structured_for"
        schema_version op_schema_version; // {major, minor, patch}

        [[nodiscard]] bool operator==(const serialised_operation_identity& o) const noexcept {
            return stable_domain_id == o.stable_domain_id &&
                stable_operation_id == o.stable_operation_id &&
                op_schema_version == o.op_schema_version;
        }
    };

    // =========================================================================
    //a.8 lithe_binary_ir_doc — the binary IR object type
    //
    // A generic, stage-independent decoded document.  Stage adapters convert
    // to/from their concrete forms (graph_ir, hl_mir_ir, physical_mir_ir).
    // =========================================================================

    struct binary_ir_op_record {
        serialised_operation_identity identity;
        std::vector<std::uint32_t> operand_ids;
        std::vector<std::uint32_t> result_ids;
        std::uint32_t block_id = 0;

        // Attribute payload (raw bytes — typed attributes decoded by stage adapters)
        std::vector<std::uint8_t> attribute_bytes;

        // Opaque-preservation: if the op is unknown-optional, bytes are preserved.
        bool is_opaque_optional = false;
        std::vector<std::uint8_t> opaque_bytes; // preserved if opaque
    };

    struct binary_ir_block_record {
        std::uint32_t id = 0;
        std::vector<std::uint32_t> op_ids;
        std::vector<std::uint32_t> arg_value_ids;
        std::vector<std::uint32_t> succ_block_ids;
    };

    struct binary_ir_value_record {
        std::uint32_t id = 0;
        std::uint8_t kind = 0; // wire_value_kind enum
        std::uint8_t bit_width = 64;
    };

    struct binary_ir_string_record {
        std::uint32_t id = 0;
        std::string text;
    };

    // An opaque-optional section preserved for round-trip
    struct binary_opaque_section {
        std::string name;
        bool required = false;
        std::vector<std::uint8_t> data;
    };

    struct lithe_binary_ir_doc {
        format_descriptor doc_format{};

        std::vector<binary_ir_value_record> values;
        std::vector<binary_ir_op_record> ops;
        std::vector<binary_ir_block_record> blocks;
        std::vector<binary_ir_string_record> strings;
        std::vector<binary_opaque_section> opaque_sections;

        bool has_unknown_required = false;
        bool has_unknown_optional = false;

        [[nodiscard]] bool valid() const noexcept {
            return doc_format.valid();
        }
    };

    // =========================================================================
    //a.8 binary_encode_options — export configuration
    // =========================================================================

    struct binary_encode_options {
        wire_endian wire_endian_tag = wire_endian::little;
        digest_algorithm digest_alg = digest_algorithm::none;
        signature_algorithm sig_alg = signature_algorithm::none;
        compression_algorithm comp_alg = compression_algorithm::none;
    };

    // =========================================================================
    //a.8 binary_provider<DigestVerifier, SigVerifier>
    //
    // Template parameters allow pluggable digest and signature verification
    // without baking in any particular crypto scheme.
    // =========================================================================

    template <
        class DigestVerifier = no_digest_verifier,
        class SigVerifier = no_signature_verifier>
        requires digest_verifier<DigestVerifier> && signature_verifier<SigVerifier>
    class binary_provider {
    public:
        static constexpr bool available = true;
        static constexpr std::string_view id = "lithe.ir.binary";

        explicit binary_provider(envelope_limits limits = {},
                                 binary_encode_options encode_opts = {},
                                 DigestVerifier dv = {},
                                 SigVerifier sv = {})
            : limits_(limits)
              , encode_opts_(encode_opts)
              , digest_verifier_(std::move(dv))
              , sig_verifier_(std::move(sv)) {}

        // ====================================================================
        // import_binary — decode a binary_ir_view into a lithe_binary_ir_doc
        //
        // Security ordering enforced:
        //   1. Structural validation (magic / addr_width / sizes / sections)
        //   2. Integrity digest check
        //   3. Authenticity signature check
        //   4. Decode payload into lithe_binary_ir_doc
        // ====================================================================

        [[nodiscard]] std::expected<lithe_binary_ir_doc, ::lithe::execution::ir_error>
        do_import_binary(const binary_ir_view& view,
                         diagnostic_list& diags,
                         const upgrade_registry* upgrades = nullptr) const {
            if (!view.valid())
                return std::unexpected(::lithe::execution::ir_error{
                    "binary_provider: invalid binary_ir_view"
                });

            const std::span<const std::uint8_t> raw = view.data;

            // -------- STEP 1: Structural validation (before large alloc) ----
            const auto sv_res = validate_envelope_structural(raw, limits_);
            if (!sv_res.ok)
                return std::unexpected(::lithe::execution::ir_error{sv_res.error_detail});

            // -------- STEP 2: Integrity digest (before decode) ---------------
            {
                // Signed range is the envelope + payload (before the digest field)
                // In practice, integrity check covers everything after the digest bytes.
                // We use the full span as the payload for simplicity (provider-specific).
                const std::span<const std::uint8_t> payload_span =
                    raw.subspan(sizeof(binary_ir_envelope));

                // Extract expected digest from envelope
                binary_ir_envelope env_hdr;
                std::memcpy(&env_hdr, raw.data(), sizeof(binary_ir_envelope));
                const std::span<const std::uint8_t> expected_digest{
                    env_hdr.digest_bytes.data(),
                    static_cast<std::size_t>(env_hdr.digest_len)
                };

                const auto dig_res = digest_verifier_(
                    payload_span, sv_res.dig_alg, expected_digest);
                if (!dig_res.ok)
                    return std::unexpected(::lithe::execution::ir_error{
                        "binary_provider: integrity digest mismatch"
                    });
            }

            // -------- STEP 3: Authenticity (before IR is trusted/compiled) ---
            {
                binary_ir_envelope env_hdr;
                std::memcpy(&env_hdr, raw.data(), sizeof(binary_ir_envelope));

                const bool sig_present = (sv_res.sig_alg_val != signature_algorithm::none);
                if (sig_present || !limits_.allow_no_signature) {
                    const std::span<const std::uint8_t> signed_range = raw;
                    const std::span<const std::uint8_t> key_id_span{
                        env_hdr.sig_key_id.data(),
                        static_cast<std::size_t>(env_hdr.sig_key_id_len)
                    };
                    const std::span<const std::uint8_t> sig_span{
                        env_hdr.sig_bytes.data(),
                        static_cast<std::size_t>(env_hdr.sig_len)
                    };

                    const auto auth_res = sig_verifier_(
                        signed_range, sv_res.sig_alg_val, key_id_span, sig_span);
                    if (!auth_res.ok)
                        return std::unexpected(::lithe::execution::ir_error{
                            "binary_provider: signature verification failed"
                        });
                }
            }

            // -------- STEP 4: Decode payload --------------------------------
            // Only reach here if structural validation + digest + signature passed.
            return decode_payload_(raw, sv_res, view.format, diags, upgrades);
        }

        // ====================================================================
        // export_binary — serialise a lithe_binary_ir_doc to owned_binary_ir
        // ====================================================================

        [[nodiscard]] std::expected<owned_binary_ir, ::lithe::execution::ir_error>
        do_export_binary(const lithe_binary_ir_doc& doc,
                         const format_descriptor& fmt) const {
            if (!fmt.valid())
                return std::unexpected(::lithe::execution::ir_error{
                    "binary_provider: invalid format_descriptor for export"
                });
            if (!doc.valid())
                return std::unexpected(::lithe::execution::ir_error{
                    "binary_provider: invalid doc for export"
                });

            return encode_document_(doc, fmt);
        }

        // ====================================================================
        // validate_ir — classify the doc's resolution state
        // ====================================================================

        [[nodiscard]] ir_resolution_state
        do_validate(const lithe_binary_ir_doc& doc) const noexcept {
            if (doc.has_unknown_required)
                return ir_resolution_state::unresolved_required_operations;
            if (doc.has_unknown_optional)
                return ir_resolution_state::contains_opaque_optional_operations;
            return ir_resolution_state::resolved;
        }

        // ====================================================================
        // CPO tag_invoke friends (ADL)
        // ====================================================================

        friend std::expected<lithe_binary_ir_doc, ::lithe::execution::ir_error>
        tag_invoke(cpo::import_binary_t,
                   binary_provider& self,
                   binary_ir_view view) {
            diagnostic_list diags;
            return self.do_import_binary(view, diags, nullptr);
        }

        friend std::expected<lithe_binary_ir_doc, ::lithe::execution::ir_error>
        tag_invoke(cpo::import_binary_t,
                   const binary_provider& self,
                   binary_ir_view view) {
            diagnostic_list diags;
            return self.do_import_binary(view, diags, nullptr);
        }

        friend std::expected<owned_binary_ir, ::lithe::execution::ir_error>
        tag_invoke(cpo::export_binary_t,
                   const binary_provider& self,
                   const lithe_binary_ir_doc& doc,
                   format_descriptor fmt) {
            return self.do_export_binary(doc, fmt);
        }

        friend ir_resolution_state
        tag_invoke(cpo::validate_ir_t,
                   const binary_provider& self,
                   const lithe_binary_ir_doc& doc) noexcept {
            return self.do_validate(doc);
        }

        // ====================================================================
        // validate_erased — type-erased validation for ir_provider_registry
        // Satisfies erased_validator concept.
        // ====================================================================

        [[nodiscard]] ir_resolution_state
        validate_erased(const std::any& ir) const noexcept {
            const auto* doc = std::any_cast<lithe_binary_ir_doc>(&ir);
            if (!doc) return ir_resolution_state::unresolved_required_operations;
            return do_validate(*doc);
        }

        // ====================================================================
        // Extended import with upgrade registry (for  item 4 wiring)
        // ====================================================================

        [[nodiscard]] std::expected<lithe_binary_ir_doc, ::lithe::execution::ir_error>
        import_with_upgrades(const binary_ir_view& view,
                             diagnostic_list& diags,
                             const upgrade_registry& upgrades) const {
            return do_import_binary(view, diags, &upgrades);
        }

    private:
        envelope_limits limits_;
        binary_encode_options encode_opts_;
        [[no_unique_address]] DigestVerifier digest_verifier_;
        [[no_unique_address]] SigVerifier sig_verifier_;

        // ====================================================================
        // Payload decoder
        // ====================================================================

        [[nodiscard]] std::expected<lithe_binary_ir_doc, ::lithe::execution::ir_error>
        decode_payload_(const std::span<const std::uint8_t> raw,
                        const envelope_validation_result& sv_res,
                        const format_descriptor& hint_fmt,
                        diagnostic_list& diags,
                        const upgrade_registry* upgrades) const {
            lithe_binary_ir_doc doc;

            // Build format_descriptor from envelope fields.
            // Use sv_res.dialect_id_val (decoded from envelope) for dialect — not a
            // caller-supplied hint (G11/G2 fix).
            // Use sv_res.ir_kind_val decoded from the envelope ir_kind_tag field (G12 fix).
            const encoding wire_enc = (sv_res.endian == wire_endian::little)
                                          ? encoding::binary_le
                                          : encoding::binary_be;
            auto fmt_result = format_descriptor::make(
                wire_enc,
                sv_res.ir_stage_val,
                sv_res.schema_ver,
                sv_res.addr_width,
                sv_res.ir_kind_val,
                sv_res.dialect_id_val,
                sv_res.endian);
            if (!fmt_result)
                return std::unexpected(fmt_result.error());
            doc.doc_format = *fmt_result;

            if (raw.size() < sizeof(binary_ir_envelope))
                return std::unexpected(::lithe::execution::ir_error{
                    "binary_provider: data too short for envelope"
                });

            // Read section directory.
            // All section_dir_offset and section data_offset values are relative to
            // the start of the payload (post-envelope).  Add the envelope size to
            // convert to absolute offsets into raw.
            binary_ir_envelope env_hdr;
            std::memcpy(&env_hdr, raw.data(), sizeof(binary_ir_envelope));

            constexpr std::size_t env_sz = sizeof(binary_ir_envelope);
            const std::size_t dir_off_abs =
                env_sz + static_cast<std::size_t>(env_hdr.section_dir_offset);
            const std::size_t dir_size = env_hdr.section_count * sizeof(section_entry);
            if (dir_off_abs > raw.size() || dir_size > raw.size() - dir_off_abs)
                return std::unexpected(::lithe::execution::ir_error{
                    "binary_provider: section directory out of bounds"
                });

            std::vector<section_entry> sections(env_hdr.section_count);
            std::memcpy(sections.data(),
                        raw.data() + dir_off_abs,
                        dir_size);

            // Validate and decode each section
            for (const auto& sec : sections) {
                const std::string_view sec_name = sec.name_view();
                const bool is_req = sec.required();

                // Bounds check section data offset/size BEFORE allocation.
                // data_offset is relative to payload start; convert to absolute.
                const std::size_t sec_off_abs =
                    env_sz + static_cast<std::size_t>(sec.data_offset);
                const std::size_t sec_size = static_cast<std::size_t>(sec.data_size);

                // Overflow-safe arithmetic
                if (sec_off_abs > raw.size() || sec_size > raw.size() - sec_off_abs) {
                    if (is_req) {
                        return std::unexpected(::lithe::execution::ir_error{
                            "binary_provider: section data out of bounds"
                        });
                    }
                    // optional section with bad offset → skip and mark opaque
                    doc.has_unknown_optional = true;
                    continue;
                }

                const std::span<const std::uint8_t> sec_data =
                    raw.subspan(sec_off_abs, sec_size);

                // Route to known section decoders
                std::string section_err;
                if (!decode_section_(sec_name, is_req, sec_data, doc,
                                     diags, upgrades, section_err)) {
                    const std::string msg = section_err.empty()
                                                ? "binary_provider: required section failed to decode"
                                                : section_err;
                    return std::unexpected(::lithe::execution::ir_error{msg});
                }
            }

            return doc;
        }

        // Section router: returns false on hard reject (unknown required section
        // or decode failure of a required section).
        bool decode_section_(const std::string_view name,
                             const bool is_required,
                             const std::span<const std::uint8_t> data,
                             lithe_binary_ir_doc& doc,
                             diagnostic_list& /* diags */,
                             const upgrade_registry* upgrades,
                             std::string& err_out) const {
            // Known section names
            if (name == "lithe.ir.values") {
                return decode_values_section_(data, doc);
            }
            if (name == "lithe.ir.ops") {
                return decode_ops_section_(data, doc, upgrades, err_out);
            }
            if (name == "lithe.ir.blocks") {
                return decode_blocks_section_(data, doc);
            }
            if (name == "lithe.ir.strings") {
                return decode_strings_section_(data, doc);
            }

            // Unknown section
            if (is_required) {
                doc.has_unknown_required = true;
                return false; // reject: unknown required section
            }
            // Unknown optional: preserve for round-trip
            if (limits_.preserve_opaque_optional) {
                doc.has_unknown_optional = true;
                binary_opaque_section opaque;
                opaque.name = std::string{name};
                opaque.required = false;
                opaque.data.assign(data.begin(), data.end());
                doc.opaque_sections.push_back(std::move(opaque));
            }
            return true;
        }

        // ---- Minimal section decoders (wire-safe: all fixed-width reads) ----

        // Values section: [count:u32] [id:u32, kind:u8, bit_width:u8, pad:u2]*
        bool decode_values_section_(const std::span<const std::uint8_t> data,
                                    lithe_binary_ir_doc& doc) const {
            if (data.size() < 4) return false;
            std::uint32_t count = 0;
            std::memcpy(&count, data.data(), 4);
            if (count > limits_.max_value_count) return false;

            const std::size_t record_size = 8; // id(4) + kind(1) + bw(1) + pad(2)
            if (data.size() < 4 + static_cast<std::size_t>(count) * record_size) return false;

            doc.values.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint8_t* p = data.data() + 4 + i * record_size;
                binary_ir_value_record v;
                std::memcpy(&v.id, p, 4);
                v.kind = p[4];
                v.bit_width = p[5];
                doc.values.push_back(v);
            }
            return true;
        }

        // Strings section: [count:u32] [len:u32, bytes...]*
        bool decode_strings_section_(const std::span<const std::uint8_t> data,
                                     lithe_binary_ir_doc& doc) const {
            if (data.size() < 4) return false;
            std::uint32_t count = 0;
            std::memcpy(&count, data.data(), 4);

            std::size_t off = 4;
            doc.strings.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                if (off + 8 > data.size()) return false;
                std::uint32_t id = 0, len = 0;
                std::memcpy(&id, data.data() + off, 4);
                std::memcpy(&len, data.data() + off + 4, 4);
                off += 8;
                if (len > limits_.max_value_count) return false; // reuse limit
                if (off + len > data.size()) return false;
                binary_ir_string_record sr;
                sr.id = id;
                sr.text = std::string{reinterpret_cast<const char*>(data.data() + off), len};
                off += len;
                doc.strings.push_back(std::move(sr));
            }
            return true;
        }

        // Ops section: [count:u32] [per-op records...]*
        // Per-op record: [domain_len:u16][domain...][name_len:u16][name...][schema:3xu16]
        //                [required:u8][n_operands:u32][operand_ids...][n_results:u32][result_ids...]
        //                [block_id:u32][attr_len:u32][attr_bytes...]
        bool decode_ops_section_(const std::span<const std::uint8_t> data,
                                 lithe_binary_ir_doc& doc,
                                 const upgrade_registry* upgrades,
                                 std::string& err_out) const {
            if (data.size() < 4) return false;
            std::uint32_t count = 0;
            std::memcpy(&count, data.data(), 4);
            if (count > limits_.max_op_count) return false;

            std::size_t off = 4;
            doc.ops.reserve(count);

            for (std::uint32_t i = 0; i < count; ++i) {
                binary_ir_op_record op;
                serialised_operation_identity& id = op.identity;

                // domain string
                if (!read_len_prefixed_string_(data, off, id.stable_domain_id)) return false;
                // name string
                if (!read_len_prefixed_string_(data, off, id.stable_operation_id)) return false;

                // schema version: [major:u16][minor:u16][patch:u16]
                if (off + 6 > data.size()) return false;
                std::memcpy(&id.op_schema_version.major, data.data() + off, 2);
                std::memcpy(&id.op_schema_version.minor, data.data() + off + 2, 2);
                std::memcpy(&id.op_schema_version.patch, data.data() + off + 4, 2);
                off += 6;

                // required flag
                if (off + 1 > data.size()) return false;
                const bool op_required = (data[off] != 0);
                ++off;

                // operands
                if (!read_u32_array_(data, off, op.operand_ids)) return false;
                // results
                if (!read_u32_array_(data, off, op.result_ids)) return false;

                // block_id
                if (off + 4 > data.size()) return false;
                std::memcpy(&op.block_id, data.data() + off, 4);
                off += 4;

                // attribute bytes
                if (!read_byte_array_(data, off, op.attribute_bytes)) return false;

                // ---- Upgrade routing ( item 4, G14 fix) ----------------
                // Fire upgrade for ALL ops (known or unknown) if registered.
                bool upgrade_applied = false;
                if (upgrades) {
                    const auto* upgrade_fn = upgrades->find(
                        doc.doc_format.ir_kind_tag,
                        id.op_schema_version);
                    if (upgrade_fn) {
                        std::any erased_op{op};
                        auto up_result = (*upgrade_fn)(std::move(erased_op));
                        if (!up_result) {
                            err_out = "binary_provider: upgrade transform failed for op '" +
                                id.stable_domain_id + '.' + id.stable_operation_id + '\'';
                            return false;
                        }
                        if (auto* upgraded = std::any_cast<binary_ir_op_record>(&*up_result))
                            op = std::move(*upgraded);
                        upgrade_applied = true;
                    }
                }

                // For KNOWN ops: reject if schema is incompatible and no upgrade resolved it.
                if (is_known_op_(id.stable_domain_id, id.stable_operation_id)) {
                    if (!is_compatible_schema_(id) && !upgrade_applied) {
                        err_out = "binary_provider: known op '" + id.stable_domain_id
                            + '.' + id.stable_operation_id
                            + "' has incompatible schema major="
                            + std::to_string(id.op_schema_version.major);
                        return false;
                    }
                }

                // Unknown op handling
                if (!is_known_op_(id.stable_domain_id, id.stable_operation_id)) {
                    if (op_required) {
                        if (!upgrades || !upgrades->has(
                            doc.doc_format.ir_kind_tag,
                            id.op_schema_version)) {
                            doc.has_unknown_required = true;
                            return false; // reject: unknown required op with no upgrade
                        }
                        // Upgrade registered but not yet applied at op level — treated
                        // as resolved if the upgrade_fn was invoked above; otherwise reject.
                        doc.has_unknown_required = true;
                        return false;
                    }
                    // unknown optional
                    if (limits_.preserve_opaque_optional) {
                        op.is_opaque_optional = true;
                        op.opaque_bytes = op.attribute_bytes;
                        doc.has_unknown_optional = true;
                    }
                }

                doc.ops.push_back(std::move(op));
            }
            return true;
        }

        // Blocks section: [count:u32] [id:u32][n_ops:u32][op_ids:u32...][n_args:u32][arg_ids...]
        //                             [n_succs:u32][succ_ids...]
        bool decode_blocks_section_(const std::span<const std::uint8_t> data,
                                    lithe_binary_ir_doc& doc) const {
            if (data.size() < 4) return false;
            std::uint32_t count = 0;
            std::memcpy(&count, data.data(), 4);
            if (count > limits_.max_block_count) return false;

            std::size_t off = 4;
            doc.blocks.reserve(count);

            for (std::uint32_t i = 0; i < count; ++i) {
                if (off + 4 > data.size()) return false;
                binary_ir_block_record blk;
                std::memcpy(&blk.id, data.data() + off, 4);
                off += 4;

                if (!read_u32_array_(data, off, blk.op_ids)) return false;
                if (!read_u32_array_(data, off, blk.arg_value_ids)) return false;
                if (!read_u32_array_(data, off, blk.succ_block_ids)) return false;

                doc.blocks.push_back(std::move(blk));
            }
            return true;
        }

        // ---- Wire-safe helper reads ----------------------------------------

        static bool read_len_prefixed_string_(const std::span<const std::uint8_t> data,
                                              std::size_t& off,
                                              std::string& out) {
            if (off + 2 > data.size()) return false;
            std::uint16_t len = 0;
            std::memcpy(&len, data.data() + off, 2);
            off += 2;
            if (off + len > data.size()) return false;
            out.assign(reinterpret_cast<const char*>(data.data() + off), len);
            off += len;
            return true;
        }

        static bool read_u32_array_(const std::span<const std::uint8_t> data,
                                    std::size_t& off,
                                    std::vector<std::uint32_t>& out) {
            if (off + 4 > data.size()) return false;
            std::uint32_t n = 0;
            std::memcpy(&n, data.data() + off, 4);
            off += 4;
            if (off + static_cast<std::size_t>(n) * 4 > data.size()) return false;
            out.resize(n);
            for (std::uint32_t i = 0; i < n; ++i) {
                std::memcpy(&out[i], data.data() + off + i * 4, 4);
            }
            off += static_cast<std::size_t>(n) * 4;
            return true;
        }

        static bool read_byte_array_(const std::span<const std::uint8_t> data,
                                     std::size_t& off,
                                     std::vector<std::uint8_t>& out) {
            if (off + 4 > data.size()) return false;
            std::uint32_t len = 0;
            std::memcpy(&len, data.data() + off, 4);
            off += 4;
            if (off + len > data.size()) return false;
            out.assign(data.data() + off, data.data() + off + len);
            off += len;
            return true;
        }

        // Minimal known-op check.  A real implementation would consult the
        // operation registry.  Here we recognise a small core set so tests
        // can exercise the unknown-op path without the full registry.
        static bool is_known_op_(const std::string_view domain,
                                 const std::string_view name) noexcept {
            // Recognise the lithe core and hl domains as "known".
            if (domain == "lithe.core" || domain == "lithe.mir" ||
                domain == "lithe.hl" || domain == "lithe")
                return true;
            if (domain.empty() && !name.empty())
                return true; // bare name from core ops
            return false;
        }

        // Known ops with schema major != 1 and major != 0 are incompatible.
        // Extend per-op as the operation registry matures.
        static bool is_compatible_schema_(const serialised_operation_identity& id) noexcept {
            return id.op_schema_version.major == 1 || id.op_schema_version.major == 0;
        }

        // ====================================================================
        // Encoder — minimal round-trip encoder for export
        // ====================================================================

        [[nodiscard]] std::expected<owned_binary_ir, ::lithe::execution::ir_error>
        encode_document_(const lithe_binary_ir_doc& doc,
                         const format_descriptor& fmt) const {
            // Build a minimal valid binary document.
            // Section layout:
            //   [envelope header]
            //   [section directory]
            //   [values section]
            //   [strings section]
            //   [ops section]
            //   [blocks section]
            //   [opaque sections]

            std::vector<std::uint8_t> payload;
            payload.reserve(1024);

            auto write_u8 = [&](std::uint8_t v) { payload.push_back(v); };
            auto write_u16 = [&](std::uint16_t v) {
                payload.push_back(static_cast<std::uint8_t>(v & 0xFF));
                payload.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
            };
            auto write_u32 = [&](std::uint32_t v) {
                for (int i = 0; i < 4; ++i)
                    payload.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
            };
            auto write_u64 = [&](std::uint64_t v) {
                for (int i = 0; i < 8; ++i)
                    payload.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
            };
            auto write_bytes = [&](const void* p, std::size_t n) {
                const auto* b = static_cast<const std::uint8_t*>(p);
                payload.insert(payload.end(), b, b + n);
            };
            auto write_str16 = [&](const std::string& s) {
                write_u16(static_cast<std::uint16_t>(s.size() & 0xFFFF));
                write_bytes(s.data(), s.size());
            };

            // Placeholder for envelope header (filled in last)
            const std::size_t env_offset = 0;
            payload.resize(sizeof(binary_ir_envelope), std::uint8_t{0});

            // --- Encode sections ---

            struct SectionBounds {
                std::uint32_t name_offset = 0; // not used here; name stored separately
                std::string name;
                bool required = true;
                std::uint64_t data_offset = 0;
                std::uint64_t data_size = 0;
            };
            std::vector<SectionBounds> sec_bounds;

            // All offsets are relative to the start of the payload (post-envelope).
            auto start_section = [&](const std::string& name, bool req) -> std::size_t {
                SectionBounds b;
                b.name = name;
                b.required = req;
                b.data_offset = payload.size() - sizeof(binary_ir_envelope);
                sec_bounds.push_back(b);
                return sec_bounds.size() - 1;
            };
            auto end_section = [&](std::size_t idx) {
                sec_bounds[idx].data_size =
                    (payload.size() - sizeof(binary_ir_envelope)) - sec_bounds[idx].data_offset;
            };

            // Values section
            {
                std::size_t si = start_section("lithe.ir.values", true);
                write_u32(static_cast<std::uint32_t>(doc.values.size()));
                for (const auto& v : doc.values) {
                    write_u32(v.id);
                    write_u8(v.kind);
                    write_u8(v.bit_width);
                    write_u8(0);
                    write_u8(0); // padding
                }
                end_section(si);
            }

            // Strings section
            {
                std::size_t si = start_section("lithe.ir.strings", true);
                write_u32(static_cast<std::uint32_t>(doc.strings.size()));
                for (const auto& sr : doc.strings) {
                    write_u32(sr.id);
                    write_u32(static_cast<std::uint32_t>(sr.text.size()));
                    write_bytes(sr.text.data(), sr.text.size());
                }
                end_section(si);
            }

            // Ops section
            {
                std::size_t si = start_section("lithe.ir.ops", true);
                write_u32(static_cast<std::uint32_t>(doc.ops.size()));
                for (const auto& op : doc.ops) {
                    write_str16(op.identity.stable_domain_id);
                    write_str16(op.identity.stable_operation_id);
                    write_u16(op.identity.op_schema_version.major);
                    write_u16(op.identity.op_schema_version.minor);
                    write_u16(op.identity.op_schema_version.patch);
                    write_u8(op.is_opaque_optional ? 0 : 1); // required flag
                    // operands
                    write_u32(static_cast<std::uint32_t>(op.operand_ids.size()));
                    for (const auto id : op.operand_ids) write_u32(id);
                    // results
                    write_u32(static_cast<std::uint32_t>(op.result_ids.size()));
                    for (const auto id : op.result_ids) write_u32(id);
                    write_u32(op.block_id);
                    // attribute bytes
                    write_u32(static_cast<std::uint32_t>(op.attribute_bytes.size()));
                    write_bytes(op.attribute_bytes.data(), op.attribute_bytes.size());
                }
                end_section(si);
            }

            // Blocks section
            {
                std::size_t si = start_section("lithe.ir.blocks", true);
                write_u32(static_cast<std::uint32_t>(doc.blocks.size()));
                for (const auto& blk : doc.blocks) {
                    write_u32(blk.id);
                    write_u32(static_cast<std::uint32_t>(blk.op_ids.size()));
                    for (const auto id : blk.op_ids) write_u32(id);
                    write_u32(static_cast<std::uint32_t>(blk.arg_value_ids.size()));
                    for (const auto id : blk.arg_value_ids) write_u32(id);
                    write_u32(static_cast<std::uint32_t>(blk.succ_block_ids.size()));
                    for (const auto id : blk.succ_block_ids) write_u32(id);
                }
                end_section(si);
            }

            // Opaque sections
            for (const auto& opq : doc.opaque_sections) {
                std::size_t si = start_section(opq.name, opq.required);
                write_bytes(opq.data.data(), opq.data.size());
                end_section(si);
            }

            // --- Section directory ---
            // dir_offset is relative to the start of the payload (post-envelope).
            const std::uint64_t dir_offset =
                payload.size() - sizeof(binary_ir_envelope);
            for (const auto& sb : sec_bounds) {
                section_entry se;
                const std::size_t nm_len = std::min(sb.name.size(), std::size_t{63});
                std::memcpy(se.name_bytes.data(), sb.name.data(), nm_len);
                se.name_len = static_cast<std::uint32_t>(nm_len);
                se.is_required = sb.required ? 1 : 0;
                se.data_offset = sb.data_offset;
                se.data_size = sb.data_size;
                write_bytes(&se, sizeof(section_entry));
            }

            // --- Fill in envelope header ---
            binary_ir_envelope env{};
            env.magic = k_binary_ir_magic;
            env.format_major = 1;
            env.format_minor = 0;
            env.wire_endian_tag = static_cast<std::uint8_t>(encode_opts_.wire_endian_tag);
            env.target_address_width = fmt.target_address_width;
            env.ir_stage_tag = static_cast<std::uint8_t>(fmt.ir_stage);
            env.ir_kind_tag = static_cast<std::uint8_t>(fmt.ir_kind_tag);
            // Write dialect identity from format_descriptor::dialect into envelope (G13 fix).
            env.set_dialect(fmt.dialect);
            env.schema_major = fmt.version.major;
            env.schema_minor = fmt.version.minor;
            env.schema_patch = fmt.version.patch;
            env.payload_size = static_cast<std::uint64_t>(
                payload.size() - sizeof(binary_ir_envelope));
            env.maximum_decoded_size = env.payload_size;
            env.compression_alg = static_cast<std::uint8_t>(encode_opts_.comp_alg);
            env.digest_alg = static_cast<std::uint8_t>(encode_opts_.digest_alg);
            env.sig_alg = static_cast<std::uint8_t>(encode_opts_.sig_alg);
            env.section_count = static_cast<std::uint32_t>(sec_bounds.size());
            env.section_dir_offset = static_cast<std::uint32_t>(dir_offset);

            std::memcpy(payload.data() + env_offset, &env, sizeof(binary_ir_envelope));

            owned_binary_ir out;
            out.format = fmt;
            out.data = std::move(payload);
            return out;
        }

        // (encode helpers for lambdas captured by value above are not needed here)
    }; // class binary_provider

    // =========================================================================
    // Static concept checks for the default instantiation
    // =========================================================================

    using default_binary_provider = binary_provider<>;
    static_assert(default_binary_provider::available);
    static_assert(binary_importer_for<default_binary_provider, lithe_binary_ir_doc>,
                  "binary_provider must satisfy binary_importer_for");
    static_assert(binary_exporter_for<default_binary_provider, lithe_binary_ir_doc>,
                  "binary_provider must satisfy binary_exporter_for");
    static_assert(ir_validator_for<default_binary_provider, lithe_binary_ir_doc>,
                  "binary_provider must satisfy ir_validator_for");
} // namespace lithe::ir
