#pragma once

// =============================================================================
// lithe_ir/security_envelope.hpp — binary Lithe IR security envelope
//
//
// The security envelope is the OUTER structure of every binary Lithe IR document.
// It must be validated in this order (hard-enforced by binary_provider):
//
//   1. Structural validation BEFORE large allocation
//      – magic / major-version / target_address_width != 0 checked immediately
//      – payload size vs maximum_decoded_size limit checked before allocating
//      – section offsets and sizes bounds-checked
//      – max nesting / block / value / section limits enforced
//
//   2. Integrity verification (digest) BEFORE any decode
//
//   3. Authenticity verification (signature) BEFORE the IR is trusted or compiled
//
// This is distinct from the AOT envelope (lithe_execution/aot.hpp):
//   • AOT uses a separate format (aot_header / FNV-1a checksum).
//   • The IR security envelope is for *interchange* IR (not AOT artifacts).
//   • Different trust policies and error paths.
//
// Wire format rules (enforced by static_assert):
//   • All fields are fixed-width integers (uint8/16/32/64).
//   • No size_t, no pointers, no host-native types on the wire.
//   • Wire endian: always little-endian (binary_le) for persisted artifacts.
//
// All size limits are in the envelope descriptor — no hardcoded constants.
// Signature verification is a pluggable algorithm id slot, not a baked-in scheme.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "format.hpp"                          // format_descriptor, schema_version, stage, stable_ir_id

namespace lithe::ir {
    // =========================================================================
    //a.8 Wire magic
    // =========================================================================

    // Four-byte magic: "LTIR" (Lithe IR)
    inline constexpr std::array<std::uint8_t, 4> k_binary_ir_magic = {0x4C, 0x54, 0x49, 0x52};

    // wire_endian is defined in format.hpp — imported here (G3).
    // Authoritative definition: lithe::ir::wire_endian (format.hppa.2).

    // =========================================================================
    //a.8 Digest algorithm identifier
    // =========================================================================

    enum class digest_algorithm : std::uint8_t {
        none = 0, // no integrity protection (testing only)
        sha256 = 1,
        sha3_256 = 2,
        blake3 = 3,
    };

    inline constexpr std::uint8_t digest_size_bytes(const digest_algorithm alg) noexcept {
        switch (alg) {
        case digest_algorithm::sha256: return 32;
        case digest_algorithm::sha3_256: return 32;
        case digest_algorithm::blake3: return 32;
        default: return 0;
        }
    }

    // =========================================================================
    //a.8 Signature algorithm identifier
    // =========================================================================

    enum class signature_algorithm : std::uint8_t {
        none = 0, // no signature
        ed25519 = 1,
        hmac_sha256 = 2,
    };

    inline constexpr std::uint8_t signature_size_bytes(const signature_algorithm alg) noexcept {
        switch (alg) {
        case signature_algorithm::ed25519: return 64;
        case signature_algorithm::hmac_sha256: return 32;
        default: return 0;
        }
    }

    // =========================================================================
    //a.8 Compression algorithm identifier
    // =========================================================================

    enum class compression_algorithm : std::uint8_t {
        none = 0,
        lz4 = 1,
        zstd = 2,
    };

    // =========================================================================
    //a.8 binary_ir_envelope — the full wire envelope struct
    //
    // Every field is fixed-width (uint8/16/32/64).  No size_t on the wire.
    // target_address_width == 0 is rejected immediately (before any byte read).
    // =========================================================================

    struct binary_ir_envelope {
        // ------ Identity (always first; structural validation gate) ----------
        std::array<std::uint8_t, 4> magic = k_binary_ir_magic;
        std::uint8_t format_major = 1; // major version: mismatch → reject
        std::uint8_t format_minor = 0; // minor version: descriptor-driven compat
        std::uint8_t wire_endian_tag = 0; // wire_endian enum value
        std::uint8_t target_address_width = 0; // MUST be != 0; validated first

        // ------ Dialect / schema identity ------------------------------------
        std::uint8_t ir_stage_tag = 0; // stage enum value
        std::uint8_t ir_kind_tag = 0; // ir_kind enum value
        // dialect_bytes/dialect_len: stable_ir_id serialised inline (,a.2).
        // Replaces the former uint16_t dialect_id — that field could not carry the
        // full cross-process stable_ir_id.  Max 63 printable ASCII chars + NUL.
        std::uint8_t dialect_len = 0; // actual length of dialect string
        std::uint8_t _pad_dialect[1] = {}; // align to even byte boundary
        std::uint16_t schema_major = 1;
        std::uint16_t schema_minor = 0;
        std::uint16_t schema_patch = 0;
        std::array<std::uint8_t, 65> dialect_bytes = {}; // dialect id string bytes (null-term)

        // ------ Size limits (checked before any allocation) ------------------
        std::uint64_t payload_size = 0; // compressed payload size in bytes
        std::uint64_t maximum_decoded_size = 0; // 0 = unlimited (not recommended)
        std::uint64_t decompression_limit = 0; // max decompressed size (0 = payload_size)

        // ------ Compression --------------------------------------------------
        std::uint8_t compression_alg = 0; // compression_algorithm enum value
        std::uint8_t _pad0[7] = {}; // explicit padding — no implicit padding

        // ------ Integrity (verified before decode) ---------------------------
        std::uint8_t digest_alg = 0; // digest_algorithm enum value
        std::uint8_t digest_len = 0; // actual digest length in bytes (≤ 64)
        std::uint8_t _pad1[6] = {}; // explicit padding
        std::array<std::uint8_t, 64> digest_bytes = {}; // digest payload

        // ------ Signature (verified before the IR is trusted) ----------------
        std::uint8_t sig_alg = 0; // signature_algorithm enum value
        std::uint8_t sig_len = 0; // actual signature length in bytes (≤ 128)
        std::uint8_t sig_key_id_len = 0; // length of signing key id in key_id bytes
        std::uint8_t _pad2[5] = {};
        std::array<std::uint8_t, 32> sig_key_id = {}; // signing key id (stable id)
        std::array<std::uint8_t, 128> sig_bytes = {}; // signature payload

        // ------ Required feature set (bitfield) ------------------------------
        // Bit N set = feature N is required; unknown required feature → reject.
        std::uint64_t required_features = 0;

        // ------ Section directory --------------------------------------------
        std::uint32_t section_count = 0;
        std::uint32_t section_dir_offset = 0; // byte offset to section directory

        [[nodiscard]] constexpr bool magic_valid() const noexcept {
            return magic == k_binary_ir_magic;
        }

        [[nodiscard]] constexpr bool address_width_valid() const noexcept {
            return target_address_width != 0;
        }

        // Returns the dialect as a stable_ir_id decoded from dialect_bytes/dialect_len.
        [[nodiscard]] constexpr stable_ir_id dialect_ir_id() const noexcept {
            stable_ir_id s;
            s.len = dialect_len;
            const std::uint8_t copy_len =
                (dialect_len < static_cast<std::uint8_t>(stable_ir_id::max_len))
                    ? dialect_len
                    : static_cast<std::uint8_t>(stable_ir_id::max_len);
            for (std::uint8_t i = 0; i < copy_len; ++i)
                s.data[i] = static_cast<char>(dialect_bytes[i]);
            return s;
        }

        // Writes a stable_ir_id into dialect_bytes/dialect_len.
        void set_dialect(const stable_ir_id& d) noexcept {
            dialect_len = d.len;
            const std::uint8_t copy_len =
                (d.len < static_cast<std::uint8_t>(dialect_bytes.size()))
                    ? d.len
                    : static_cast<std::uint8_t>(dialect_bytes.size() - 1);
            for (std::uint8_t i = 0; i < copy_len; ++i)
                dialect_bytes[i] = static_cast<std::uint8_t>(d.data[i]);
        }

        [[nodiscard]] constexpr wire_endian endian() const noexcept {
            return static_cast<wire_endian>(wire_endian_tag);
        }

        [[nodiscard]] constexpr stage ir_stage() const noexcept {
            return static_cast<stage>(ir_stage_tag);
        }

        [[nodiscard]] constexpr digest_algorithm digest_alg_enum() const noexcept {
            return static_cast<digest_algorithm>(digest_alg);
        }

        [[nodiscard]] constexpr signature_algorithm sig_alg_enum() const noexcept {
            return static_cast<signature_algorithm>(sig_alg);
        }

        [[nodiscard]] constexpr compression_algorithm compression_alg_enum() const noexcept {
            return static_cast<compression_algorithm>(compression_alg);
        }

        [[nodiscard]] constexpr schema_version schema() const noexcept {
            return {schema_major, schema_minor, schema_patch};
        }

        // Returns the effective decompression limit (falls back to payload_size).
        [[nodiscard]] constexpr std::uint64_t effective_decompression_limit() const noexcept {
            return (decompression_limit > 0) ? decompression_limit : payload_size;
        }
    };

    // Enforce no implicit padding and fixed-width-only fields.
    static_assert(sizeof(binary_ir_envelope::magic) == 4);
    static_assert(sizeof(binary_ir_envelope::digest_bytes) == 64);
    static_assert(sizeof(binary_ir_envelope::sig_bytes) == 128);
    static_assert(sizeof(binary_ir_envelope::sig_key_id) == 32);

    // =========================================================================
    //a.8 section_entry — one entry in the section directory
    //
    // All offsets and sizes are relative to the start of the payload.
    // No absolute pointers — validated via bounds check before allocation.
    // =========================================================================

    struct section_entry {
        std::array<std::uint8_t, 64> name_bytes = {}; // section name (null-terminated)
        std::uint32_t name_len = 0; // length of name in name_bytes
        std::uint8_t is_required = 1; // 1 = required, 0 = optional
        std::uint8_t _pad[3] = {};
        std::uint64_t data_offset = 0; // byte offset from payload start
        std::uint64_t data_size = 0; // byte size of section data

        [[nodiscard]] std::string_view name_view() const noexcept {
            const std::size_t len = (name_len < 64) ? name_len : 63u;
            return {reinterpret_cast<const char*>(name_bytes.data()), len};
        }

        [[nodiscard]] bool required() const noexcept { return is_required != 0; }
    };

    static_assert(sizeof(section_entry::name_bytes) == 64);

    // =========================================================================
    //a.8 envelope_limits — configurable operational limits
    //
    // All limits are fields of this descriptor, NOT hardcoded constants.
    // Set by the caller and passed to binary_provider as configuration.
    // =========================================================================

    struct envelope_limits {
        std::uint32_t max_section_count = 256;
        std::uint32_t max_nesting = 64;
        std::uint32_t max_block_count = 100'000;
        std::uint32_t max_value_count = 1'000'000;
        std::uint32_t max_op_count = 1'000'000;
        std::uint64_t max_decoded_size = 512ULL * 1024 * 1024; // 512 MiB default
        std::uint64_t max_payload_size = 256ULL * 1024 * 1024; // 256 MiB default
        bool allow_no_digest = true; // true = allow digest_algorithm::none
        bool allow_no_signature = true; // true  = don't require signature
        bool preserve_opaque_optional = true; // false = reject unknown optional
    };

    // =========================================================================
    //a.8 envelope_validation_result — structural validation outcome
    //
    // Returned by validate_envelope_structural() before any allocation.
    // On success, carries the decoded envelope + section directory offsets.
    // On failure, carries the error string.
    // =========================================================================

    struct envelope_validation_result {
        bool ok = false;
        std::string error_detail;

        // Decoded values for the provider (only valid if ok == true)
        wire_endian endian = wire_endian::little;
        std::uint8_t addr_width = 64;
        stage ir_stage_val = stage::physical;
        schema_version schema_ver = {1, 0, 0};
        digest_algorithm dig_alg = digest_algorithm::none;
        signature_algorithm sig_alg_val = signature_algorithm::none;
        compression_algorithm comp_alg = compression_algorithm::none;
        std::uint64_t payload_bytes = 0;
        std::uint64_t decoded_limit = 0;
        std::uint32_t section_count = 0;
        std::uint32_t section_dir_off = 0;
        // Dialect identity decoded from dialect_bytes/dialect_len (G2 fix).
        stable_ir_id dialect_id_val;
        // ir_kind decoded from envelope ir_kind_tag field (G12 fix).
        ::lithe::execution::ir_kind ir_kind_val = ::lithe::execution::ir_kind::physical_mir;
    };

    // =========================================================================
    //a.8 validate_envelope_structural — step 1 gate
    //
    // Checks magic, major version, target_address_width, payload size vs limits,
    // section directory bounds.  Does NOT allocate the payload.
    //
    // Returns envelope_validation_result.ok == true iff the document is
    // structurally valid under the given limits.
    // =========================================================================

    [[nodiscard]] inline envelope_validation_result
    validate_envelope_structural(const std::span<const std::uint8_t> data,
                                 const envelope_limits& limits) noexcept {
        envelope_validation_result res;

        // Minimum size: must hold at least the envelope header
        if (data.size() < sizeof(binary_ir_envelope)) {
            res.error_detail = "envelope: data too small to hold envelope header";
            return res;
        }

        // Overlay the envelope header (read-only; no allocation)
        const binary_ir_envelope* env = nullptr;
        // Use memcpy-safe read (std::memcpy — no __builtin_memcpy, cross-platform C++23)
        binary_ir_envelope tmp;
        std::memcpy(&tmp, data.data(), sizeof(binary_ir_envelope));
        env = &tmp;

        // 1. Magic
        if (!env->magic_valid()) {
            res.error_detail = "envelope: bad magic";
            return res;
        }

        // 2. Major version
        if (env->format_major != 1) {
            res.error_detail = "envelope: unsupported major version";
            return res;
        }

        // 3. target_address_width != 0 — BEFORE any byte read from payload
        if (!env->address_width_valid()) {
            res.error_detail = "envelope: target_address_width == 0";
            return res;
        }

        // 4. Payload size vs configured limits
        if (env->payload_size > limits.max_payload_size) {
            res.error_detail = "envelope: payload_size exceeds max_payload_size limit";
            return res;
        }

        // 5. maximum_decoded_size vs configured limit
        const std::uint64_t max_dec = (env->maximum_decoded_size > 0)
                                          ? env->maximum_decoded_size
                                          : env->payload_size;
        if (max_dec > limits.max_decoded_size) {
            res.error_detail = "envelope: maximum_decoded_size exceeds limit";
            return res;
        }

        // 6. Digest algorithm: reject none if required
        const auto dig = env->digest_alg_enum();
        if (!limits.allow_no_digest && dig == digest_algorithm::none) {
            res.error_detail = "envelope: digest required but digest_algorithm == none";
            return res;
        }

        // 7. Section count limit
        if (env->section_count > limits.max_section_count) {
            res.error_detail = "envelope: section_count exceeds max_section_count";
            return res;
        }

        // 8. Section directory bounds (checked before allocation).
        // section_dir_offset is relative to the payload start (post-envelope).
        const std::uint64_t dir_size =
            static_cast<std::uint64_t>(env->section_count) * sizeof(section_entry);
        const std::uint64_t dir_abs_end =
            static_cast<std::uint64_t>(sizeof(binary_ir_envelope)) +
            static_cast<std::uint64_t>(env->section_dir_offset) + dir_size;
        if (dir_abs_end > data.size()) {
            res.error_detail = "envelope: section directory out of bounds";
            return res;
        }

        // All structural checks passed — populate result
        res.ok = true;
        res.endian = env->endian();
        res.addr_width = env->target_address_width;
        res.ir_stage_val = env->ir_stage();
        res.schema_ver = env->schema();
        res.dig_alg = dig;
        res.sig_alg_val = env->sig_alg_enum();
        res.comp_alg = env->compression_alg_enum();
        res.payload_bytes = env->payload_size;
        res.decoded_limit = max_dec;
        res.section_count = env->section_count;
        res.section_dir_off = env->section_dir_offset;
        // Decode dialect identity and ir_kind from envelope fields (G2, G12 fixes).
        res.dialect_id_val = env->dialect_ir_id();
        res.ir_kind_val = static_cast<::lithe::execution::ir_kind>(env->ir_kind_tag);

        return res;
    }

    // =========================================================================
    //a.8 envelope_integrity_result — step 2 gate (integrity/digest check)
    //
    // NOTE: This library provides the hook point and protocol; the actual
    // cryptographic verification is plugged in by the caller via a digest_verifier
    // concept (see binary_provider.hpp).  This avoids pulling crypto headers here.
    // =========================================================================

    struct envelope_integrity_result {
        bool ok = false;
        std::string error_detail;
    };

    // =========================================================================
    //a.8 envelope_auth_result — step 3 gate (signature/authenticity check)
    //
    // Same design: hook point only; crypto plugged in by caller.
    // A doc failing signature NEVER reaches compile.
    // =========================================================================

    struct envelope_auth_result {
        bool ok = false;
        std::string error_detail;
        bool was_skipped = false; // true if signature_algorithm::none and allowed
    };

    // =========================================================================
    //a.8 Concepts for pluggable digest and signature verifiers
    // =========================================================================

    // digest_verifier<DV>: callable that verifies the integrity digest.
    // signature: (span<const uint8_t> payload, digest_algorithm, span<const uint8_t> expected)
    //            → envelope_integrity_result
    template <class DV>
    concept digest_verifier =
        requires(DV& dv,
                 std::span<const std::uint8_t> payload,
                 digest_algorithm alg,
                 std::span<const std::uint8_t> expected_digest) {
            {
                dv(payload, alg, expected_digest)
            }
            -> std::same_as<envelope_integrity_result>;
        };

    // signature_verifier<SV>: callable that verifies the authenticity signature.
    // signature: (span<const uint8_t> signed_range, signature_algorithm,
    //             span<const uint8_t> key_id, span<const uint8_t> sig)
    //            → envelope_auth_result
    template <class SV>
    concept signature_verifier =
        requires(SV& sv,
                 std::span<const std::uint8_t> signed_range,
                 signature_algorithm alg,
                 std::span<const std::uint8_t> key_id,
                 std::span<const std::uint8_t> sig) {
            {
                sv(signed_range, alg, key_id, sig)
            }
            -> std::same_as<envelope_auth_result>;
        };

    // =========================================================================
    //a.8 no_digest_verifier — zero-cost default (skips integrity check)
    // Use only when digest_algorithm == none and limits.allow_no_digest == true.
    // =========================================================================

    struct no_digest_verifier {
        [[nodiscard]] envelope_integrity_result
        operator()(std::span<const std::uint8_t>,
                   digest_algorithm,
                   std::span<const std::uint8_t>) const noexcept {
            return {.ok = true};
        }
    };

    static_assert(digest_verifier<no_digest_verifier>,
                  "no_digest_verifier must satisfy digest_verifier");
    static_assert(std::is_empty_v<no_digest_verifier>);

    // =========================================================================
    //a.8 no_signature_verifier — zero-cost default (skips signature)
    // Use only when signature_algorithm == none and limits.allow_no_signature == true.
    // =========================================================================

    struct no_signature_verifier {
        [[nodiscard]] envelope_auth_result
        operator()(std::span<const std::uint8_t>,
                   signature_algorithm,
                   std::span<const std::uint8_t>,
                   std::span<const std::uint8_t>) const noexcept {
            return {.ok = true, .was_skipped = true};
        }
    };

    static_assert(signature_verifier<no_signature_verifier>,
                  "no_signature_verifier must satisfy signature_verifier");
    static_assert(std::is_empty_v<no_signature_verifier>);
} // namespace lithe::ir
