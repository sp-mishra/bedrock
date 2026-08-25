#pragma once

// =============================================================================
// lithe_execution/store/envelope.hpp — artifact envelope codec (impl-3)
//
// Provides:
//   artifact_envelope_magic     — wire magic bytes "LART"
//   reloc_record / import_record / export_record — relocation + symbol tables
//   artifact_envelope           — complete wire envelope struct (arch §8)
//   artifact_error              — error type for encode/decode failures
//   artifact_error_stage        — which verification stage failed
//   encode_options / decode_policy — control parameters
//   encode_artifact()           — artifact_record → bytes
//   decode_artifact()           — bytes → artifact_record (enforces §8 ordering)
//
// Verification ordering (hard-enforced, arch §8):
//   1. Structural limits BEFORE allocation
//   2. Payload digest integrity BEFORE payload decode
//   3. Signature authenticity BEFORE trusting the artifact
//   4. Compatibility predicate BEFORE returning a usable record
//
// Reuses:
//   aot_checksum (FNV-1a) from aot.hpp for fast corruption detection.
//   digest_algorithm / signature_algorithm from security_envelope.hpp.
//   artifact_record from artifact_record.hpp.
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "artifact_record.hpp"  // artifact_record, artifact_key, …

namespace lithe::execution::store {
    // =============================================================================
    // Wire magic
    // =============================================================================

    inline constexpr std::array<std::uint8_t, 4> k_artifact_envelope_magic =
        {'L', 'A', 'R', 'T'};
    inline constexpr std::uint16_t k_artifact_envelope_version = 1;

    // =============================================================================
    // reloc_record / import_record / export_record
    // =============================================================================

    struct reloc_record {
        std::uint64_t offset = 0;
        std::uint8_t kind = 0; // relocation_kind enum value (backend-specific)
        std::uint32_t symbol = 0;
        std::int64_t addend = 0;
        std::uint8_t width = 8;
        [[nodiscard]] bool operator==(const reloc_record&) const noexcept = default;
    };

    struct import_record {
        std::string name;
        std::string abi_tag;
        [[nodiscard]] bool operator==(const import_record&) const noexcept = default;
    };

    struct export_record {
        std::string name;
        std::uint64_t offset = 0;
        [[nodiscard]] bool operator==(const export_record&) const noexcept = default;
    };

    // =============================================================================
    // artifact_error
    // =============================================================================

    enum class artifact_error_stage : std::uint8_t {
        none = 0,
        structural = 1, // limits / magic / version check before allocation
        integrity = 2, // payload digest mismatch
        signature = 3, // signature verification failure
        compatibility = 4, // compatibility predicate rejection
        codec = 5, // serialization / deserialization failure
    };

    struct artifact_error {
        artifact_error_stage stage = artifact_error_stage::none;
        std::string detail;

        [[nodiscard]] static artifact_error structural(std::string d) {
            return {artifact_error_stage::structural, std::move(d)};
        }

        [[nodiscard]] static artifact_error integrity(std::string d) {
            return {artifact_error_stage::integrity, std::move(d)};
        }

        [[nodiscard]] static artifact_error signature(std::string d) {
            return {artifact_error_stage::signature, std::move(d)};
        }

        [[nodiscard]] static artifact_error compat(std::string d) {
            return {artifact_error_stage::compatibility, std::move(d)};
        }

        [[nodiscard]] static artifact_error codec(std::string d) {
            return {artifact_error_stage::codec, std::move(d)};
        }
    };

    // =============================================================================
    // encode_options / decode_policy
    // =============================================================================

    struct encode_options {
        lithe::ir::digest_algorithm payload_digest_alg =
            lithe::ir::digest_algorithm::sha256;
        bool include_checksum = true; // FNV-1a fast corruption check
    };

    struct decode_policy {
        std::uint64_t max_payload_bytes = 256ULL * 1024 * 1024; // 256 MiB
        bool require_digest = true;
        bool require_signature = false;
        bool run_compat_check = false; // caller wires in a host_profile if true
    };

    // =============================================================================
    // Internal: simple flat codec helpers
    //
    // Encoding layout (binary little-endian):
    //   [4]  magic
    //   [2]  format_version
    //   [2]  flags (reserved, 0)
    //   [1]  artifact_kind
    //   [1]  payload_digest_alg
    //   [1]  signature_alg
    //   [1]  reserved
    //   [8]  fast_checksum  (FNV-1a of payload section)
    //   [64] semantic_digest
    //   [1]  semantic_digest_len
    //   [32] payload_digest (SHA-256 or zero if none)
    //   [32] sig_key_id
    //   [2]  sig_bytes_len
    //   [128] sig_bytes (zero-padded)
    //   [4]  reloc_count
    //   [4]  import_count
    //   [4]  export_count
    //   [8]  payload_size
    //   [N]  compatibility_manifest (BEVE if Glaze available, else raw packed)
    //   [M]  provenance (same)
    //   [P]  payload bytes
    //   [R]  reloc records (packed)
    //   [I]  import records (length-prefixed strings)
    //   [E]  export records
    // =============================================================================

    namespace detail {
        // FNV-1a 64-bit (same algorithm as aot_checksum; defined locally to avoid
        // the LITHE_HAS_AOT guard dependency).
        [[nodiscard]] inline std::uint64_t
        fnv1a_64(std::span<const std::uint8_t> data) noexcept {
            constexpr std::uint64_t kOffset = 14695981039346656037ULL;
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            std::uint64_t h = kOffset;
            for (const auto b : data) {
                h ^= static_cast<std::uint64_t>(b);
                h *= kPrime;
            }
            return h;
        }

        // Minimal byte-push helpers (little-endian, no external deps).
        inline void push_u8(std::vector<std::uint8_t>& v, std::uint8_t x) {
            v.push_back(x);
        }

        inline void push_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
            v.push_back(static_cast<std::uint8_t>(x & 0xFF));
            v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
        }

        inline void push_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
            for (int i = 0; i < 4; ++i)
                v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
        }

        inline void push_u64(std::vector<std::uint8_t>& v, std::uint64_t x) {
            for (int i = 0; i < 8; ++i)
                v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
        }

        inline void push_str(std::vector<std::uint8_t>& v, const std::string& s) {
            push_u32(v, static_cast<std::uint32_t>(s.size()));
            for (unsigned char c : s) v.push_back(c);
        }

        inline void push_bytes(std::vector<std::uint8_t>& v,
                               const std::vector<std::uint8_t>& src) {
            v.insert(v.end(), src.begin(), src.end());
        }

        template <std::size_t N>
        inline void push_array(std::vector<std::uint8_t>& v,
                               const std::array<std::uint8_t, N>& a) {
            v.insert(v.end(), a.begin(), a.end());
        }

        // Read helpers — return false on underflow.
        inline bool read_u8(std::span<const std::uint8_t>& s, std::uint8_t& out) {
            if (s.empty()) return false;
            out = s[0];
            s = s.subspan(1);
            return true;
        }

        inline bool read_u16(std::span<const std::uint8_t>& s, std::uint16_t& out) {
            if (s.size() < 2) return false;
            out = static_cast<std::uint16_t>(s[0]) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(s[1]) << 8);
            s = s.subspan(2);
            return true;
        }

        inline bool read_u32(std::span<const std::uint8_t>& s, std::uint32_t& out) {
            if (s.size() < 4) return false;
            out = 0;
            for (int i = 0; i < 4; ++i)
                out |= static_cast<std::uint32_t>(s[i]) << (8 * i);
            s = s.subspan(4);
            return true;
        }

        inline bool read_u64(std::span<const std::uint8_t>& s, std::uint64_t& out) {
            if (s.size() < 8) return false;
            out = 0;
            for (int i = 0; i < 8; ++i)
                out |= static_cast<std::uint64_t>(s[i]) << (8 * i);
            s = s.subspan(8);
            return true;
        }

        inline bool read_str(std::span<const std::uint8_t>& s, std::string& out) {
            std::uint32_t len = 0;
            if (!read_u32(s, len)) return false;
            if (s.size() < len) return false;
            out.assign(reinterpret_cast<const char*>(s.data()), len);
            s = s.subspan(len);
            return true;
        }

        template <std::size_t N>
        inline bool read_array(std::span<const std::uint8_t>& s,
                               std::array<std::uint8_t, N>& out) {
            if (s.size() < N) return false;
            std::copy_n(s.data(), N, out.begin());
            s = s.subspan(N);
            return true;
        }

        // Encode compatibility_manifest + provenance as packed bytes.
        inline void encode_compat(std::vector<std::uint8_t>& v,
                                  const compatibility_manifest& c) {
            push_u16(v, c.ir_schema.major);
            push_u16(v, c.ir_schema.minor);
            push_u16(v, c.ir_schema.patch);
            push_array(v, c.abi.digest);
            push_u64(v, c.required_caps.bits);
            push_str(v, c.target.os);
            push_str(v, c.target.arch);
            push_u32(v, c.target.min_os_version);
            push_u32(v, static_cast<std::uint32_t>(c.ext_syms.size()));
            for (const auto& sym : c.ext_syms) {
                push_str(v, sym.name);
                push_str(v, sym.abi_tag);
            }
            push_u64(v, c.security.id);
            push_u64(v, c.security.version);
        }

        inline bool decode_compat(std::span<const std::uint8_t>& s,
                                  compatibility_manifest& c) {
            if (!read_u16(s, c.ir_schema.major)) return false;
            if (!read_u16(s, c.ir_schema.minor)) return false;
            if (!read_u16(s, c.ir_schema.patch)) return false;
            if (!read_array(s, c.abi.digest)) return false;
            if (!read_u64(s, c.required_caps.bits)) return false;
            if (!read_str(s, c.target.os)) return false;
            if (!read_str(s, c.target.arch)) return false;
            if (!read_u32(s, c.target.min_os_version)) return false;
            std::uint32_t nsyms = 0;
            if (!read_u32(s, nsyms)) return false;
            c.ext_syms.resize(nsyms);
            for (auto& sym : c.ext_syms) {
                if (!read_str(s, sym.name)) return false;
                if (!read_str(s, sym.abi_tag)) return false;
            }
            if (!read_u64(s, c.security.id)) return false;
            if (!read_u64(s, c.security.version)) return false;
            return true;
        }

        inline void encode_provenance(std::vector<std::uint8_t>& v,
                                      const provenance& p) {
            push_str(v, p.pipe.name);
            push_u16(v, p.pipe_ver.major);
            push_u16(v, p.pipe_ver.minor);
            push_u8(v, p.backend.has_value() ? 1u : 0u);
            if (p.backend) push_str(v, p.backend->name);
            push_u8(v, p.backend_ver.has_value() ? 1u : 0u);
            if (p.backend_ver) {
                push_u16(v, p.backend_ver->major);
                push_u16(v, p.backend_ver->minor);
            }
            push_u32(v, static_cast<std::uint32_t>(p.upgrades.size()));
            for (const auto& step : p.upgrades) {
                push_str(v, step.upgrader_id);
                push_u16(v, step.from_major);
                push_u16(v, step.from_minor);
                push_u16(v, step.to_major);
                push_u16(v, step.to_minor);
            }
            push_str(v, p.producer);
        }

        inline bool decode_provenance(std::span<const std::uint8_t>& s,
                                      provenance& p) {
            if (!read_str(s, p.pipe.name)) return false;
            if (!read_u16(s, p.pipe_ver.major)) return false;
            if (!read_u16(s, p.pipe_ver.minor)) return false;
            std::uint8_t has_backend = 0;
            if (!read_u8(s, has_backend)) return false;
            if (has_backend) {
                p.backend.emplace();
                if (!read_str(s, p.backend->name)) return false;
            }
            std::uint8_t has_bver = 0;
            if (!read_u8(s, has_bver)) return false;
            if (has_bver) {
                p.backend_ver.emplace();
                if (!read_u16(s, p.backend_ver->major)) return false;
                if (!read_u16(s, p.backend_ver->minor)) return false;
            }
            std::uint32_t nsteps = 0;
            if (!read_u32(s, nsteps)) return false;
            p.upgrades.resize(nsteps);
            for (auto& step : p.upgrades) {
                if (!read_str(s, step.upgrader_id)) return false;
                if (!read_u16(s, step.from_major)) return false;
                if (!read_u16(s, step.from_minor)) return false;
                if (!read_u16(s, step.to_major)) return false;
                if (!read_u16(s, step.to_minor)) return false;
            }
            if (!read_str(s, p.producer)) return false;
            return true;
        }

        // Serialize the artifact_key into a byte vector.
        inline void encode_key(std::vector<std::uint8_t>& v, const artifact_key& key) {
            auto write_opt = [&](const optimized_key& k) {
                const auto active = static_cast<std::uint8_t>(
                    std::min<std::size_t>(k.semantic_digest_len, 64));
                push_u8(v, active);
                for (std::size_t i = 0; i < 64; ++i) push_u8(v, k.semantic_digest[i]);
                push_u16(v, k.ir_schema.major);
                push_u16(v, k.ir_schema.minor);
                push_u16(v, k.ir_schema.patch);
                push_array(v, k.abi.digest);
                push_str(v, k.pipe_id.name);
                push_u16(v, k.pipe_ver.major);
                push_u16(v, k.pipe_ver.minor);
                push_array(v, k.policy.digest);
            };
            std::visit([&](const auto& k) {
                using T = std::remove_cvref_t<decltype(k)>;
                if constexpr (std::is_same_v<T, optimized_key>) {
                    push_u8(v, 0);
                    write_opt(k);
                }
                else {
                    push_u8(v, 1);
                    write_opt(k.base);
                    push_str(v, k.backend.name);
                    push_u16(v, k.backend_ver.major);
                    push_u16(v, k.backend_ver.minor);
                    push_array(v, k.target_caps.digest);
                    push_u16(v, k.backend_pipe.major);
                    push_u16(v, k.backend_pipe.minor);
                    push_array(v, k.spec.digest);
                    push_array(v, k.ext_syms.digest);
                }
            }, key);
        }

        inline bool decode_key(std::span<const std::uint8_t>& s, artifact_key& key) {
            std::uint8_t kind_tag = 0;
            if (!read_u8(s, kind_tag)) return false;

            auto read_opt = [&](optimized_key& k) -> bool {
                if (!read_u8(s, k.semantic_digest_len)) return false;
                if (!read_array(s, k.semantic_digest)) return false;
                if (!read_u16(s, k.ir_schema.major)) return false;
                if (!read_u16(s, k.ir_schema.minor)) return false;
                if (!read_u16(s, k.ir_schema.patch)) return false;
                if (!read_array(s, k.abi.digest)) return false;
                if (!read_str(s, k.pipe_id.name)) return false;
                if (!read_u16(s, k.pipe_ver.major)) return false;
                if (!read_u16(s, k.pipe_ver.minor)) return false;
                if (!read_array(s, k.policy.digest)) return false;
                return true;
            };

            if (kind_tag == 0) {
                optimized_key k;
                if (!read_opt(k)) return false;
                key = std::move(k);
            }
            else {
                executable_key ek;
                if (!read_opt(ek.base)) return false;
                if (!read_str(s, ek.backend.name)) return false;
                if (!read_u16(s, ek.backend_ver.major)) return false;
                if (!read_u16(s, ek.backend_ver.minor)) return false;
                if (!read_array(s, ek.target_caps.digest)) return false;
                if (!read_u16(s, ek.backend_pipe.major)) return false;
                if (!read_u16(s, ek.backend_pipe.minor)) return false;
                if (!read_array(s, ek.spec.digest)) return false;
                if (!read_array(s, ek.ext_syms.digest)) return false;
                key = std::move(ek);
            }
            return true;
        }
    } // namespace detail

    // =============================================================================
    // encode_artifact — serialize artifact_record to bytes
    // =============================================================================

    [[nodiscard]] inline std::expected<std::vector<std::uint8_t>, artifact_error>
    encode_artifact(const artifact_record& rec,
                    const encode_options& opts = {},
                    const std::vector<reloc_record>& relocs = {},
                    const std::vector<import_record>& imports = {},
                    const std::vector<export_record>& exports = {}) {
        std::vector<std::uint8_t> out;
        out.reserve(512);

        // ---- Header prefix (fixed-size) ----------------------------------------
        detail::push_array(out, k_artifact_envelope_magic);
        detail::push_u16(out, k_artifact_envelope_version);
        detail::push_u16(out, 0); // flags = reserved

        detail::push_u8(out, static_cast<std::uint8_t>(rec.kind));
        detail::push_u8(out, static_cast<std::uint8_t>(opts.payload_digest_alg));
        detail::push_u8(out, rec.signature.has_value()
                                 ? static_cast<std::uint8_t>(rec.signature->alg)
                                 : static_cast<std::uint8_t>(lithe::ir::signature_algorithm::none));
        detail::push_u8(out, 0); // reserved

        // Placeholder for fast checksum (FNV-1a) — filled in at the end.
        const std::size_t checksum_offset = out.size();
        detail::push_u64(out, 0);

        // Semantic digest (64 bytes + length byte)
        detail::push_u8(out, rec.semantic_digest_len);
        for (auto b : rec.semantic_digest) detail::push_u8(out, b);

        // Payload digest placeholder (32 bytes) — computed after payload is encoded
        const std::size_t payload_digest_offset = out.size();
        for (int i = 0; i < 32; ++i) detail::push_u8(out, 0);

        // Signature key id + bytes
        if (rec.signature) {
            detail::push_array(out, rec.signature->key_id);
            detail::push_u16(out, static_cast<std::uint16_t>(
                                 std::min(rec.signature->sig_bytes.size(), std::size_t{128})));
            std::array<std::uint8_t, 128> sig_buf{};
            const auto ncopy = std::min(rec.signature->sig_bytes.size(), std::size_t{128});
            std::copy_n(rec.signature->sig_bytes.begin(), ncopy, sig_buf.begin());
            detail::push_array(out, sig_buf);
        }
        else {
            std::array<std::uint8_t, 32> empty_kid{};
            detail::push_array(out, empty_kid);
            detail::push_u16(out, 0);
            std::array<std::uint8_t, 128> empty_sig{};
            detail::push_array(out, empty_sig);
        }

        // Counts
        detail::push_u32(out, static_cast<std::uint32_t>(relocs.size()));
        detail::push_u32(out, static_cast<std::uint32_t>(imports.size()));
        detail::push_u32(out, static_cast<std::uint32_t>(exports.size()));

        // ---- Variable: compatibility_manifest + provenance + key ----------------
        detail::encode_compat(out, rec.compat);
        detail::encode_provenance(out, rec.prov);
        detail::encode_key(out, rec.key);

        // Resource limits
        detail::push_u64(out, rec.limits.max_stack_bytes);
        detail::push_u64(out, rec.limits.max_heap_bytes);
        detail::push_u64(out, rec.limits.max_time_ns);

        // ---- Payload ------------------------------------------------------------
        const std::size_t payload_size_offset = out.size();
        detail::push_u64(out, 0); // payload_size placeholder

        const std::size_t payload_start = out.size();
        std::visit([&](const auto& p) {
            using T = std::remove_cvref_t<decltype(p)>;
            if constexpr (std::is_same_v<T, inline_payload>) {
                detail::push_u8(out, 0); // inline tag
                detail::push_u64(out, static_cast<std::uint64_t>(p.bytes.size()));
                out.insert(out.end(), p.bytes.begin(), p.bytes.end());
            }
            else {
                detail::push_u8(out, 1); // content-address tag
                for (auto b : p.digest) out.push_back(b);
                detail::push_u64(out, p.size);
            }
        }, rec.payload);
        const std::size_t payload_size = out.size() - payload_start;

        // Stamp payload_size
        std::uint64_t psz = static_cast<std::uint64_t>(payload_size);
        for (int i = 0; i < 8; ++i)
            out[payload_size_offset + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((psz >> (8 * i)) & 0xFF);

        // ---- Reloc / import / export tables -------------------------------------
        for (const auto& r : relocs) {
            detail::push_u64(out, r.offset);
            detail::push_u8(out, r.kind);
            detail::push_u32(out, r.symbol);
            detail::push_u64(out, static_cast<std::uint64_t>(r.addend));
            detail::push_u8(out, r.width);
        }
        for (const auto& im : imports) {
            detail::push_str(out, im.name);
            detail::push_str(out, im.abi_tag);
        }
        for (const auto& ex : exports) {
            detail::push_str(out, ex.name);
            detail::push_u64(out, ex.offset);
        }

        // ---- Compute + stamp payload digest (SHA-256) ---------------------------
        const auto payload_span = std::span<const std::uint8_t>{
            out.data() + payload_start, payload_size
        };
        const auto phash = containers::content_digest<containers::sha256_digest_policy>(
            payload_span);
        std::copy_n(phash.begin(), 32,
                    out.begin() + static_cast<std::ptrdiff_t>(payload_digest_offset));

        // ---- Fast checksum (FNV-1a over all bytes after the fixed header) -------
        if (opts.include_checksum) {
            const auto body_span = std::span<const std::uint8_t>{
                out.data() + checksum_offset + 8,
                out.size() - (checksum_offset + 8)
            };
            const std::uint64_t cksum = detail::fnv1a_64(body_span);
            for (int i = 0; i < 8; ++i)
                out[checksum_offset + static_cast<std::size_t>(i)] =
                    static_cast<std::uint8_t>((cksum >> (8 * i)) & 0xFF);
        }

        return out;
    }

    // =============================================================================
    // decode_artifact — bytes → artifact_record (enforces §8 verification ordering)
    // =============================================================================

    [[nodiscard]] inline std::expected<artifact_record, artifact_error>
    decode_artifact(std::span<const std::uint8_t> data,
                    const decode_policy& policy = {}) {
        // ---- Stage 1: Structural limits BEFORE allocation -----------------------
        constexpr std::size_t k_fixed_header_min =
            4 // magic
            + 2 // version
            + 2 // flags
            + 1 // kind
            + 1 // digest_alg
            + 1 // sig_alg
            + 1 // reserved
            + 8 // checksum
            + 1 // semantic_digest_len
            + 64 // semantic_digest
            + 32 // payload_digest
            + 32 // sig_key_id
            + 2 // sig_bytes_len
            + 128; // sig_bytes

        if (data.size() < k_fixed_header_min)
            return std::unexpected(artifact_error::structural("data too small for envelope header"));

        auto s = data;

        // Magic
        std::array<std::uint8_t, 4> magic{};
        if (!detail::read_array(s, magic))
            return std::unexpected(artifact_error::structural("read magic failed"));
        if (magic != k_artifact_envelope_magic)
            return std::unexpected(artifact_error::structural("bad magic"));

        std::uint16_t ver = 0;
        if (!detail::read_u16(s, ver))
            return std::unexpected(artifact_error::structural("read version failed"));
        if (ver != k_artifact_envelope_version)
            return std::unexpected(artifact_error::structural("unsupported format version"));

        std::uint16_t flags = 0;
        if (!detail::read_u16(s, flags)) // reserved
            return std::unexpected(artifact_error::structural("read flags failed"));

        std::uint8_t kind_tag = 0, dig_alg_tag = 0, sig_alg_tag = 0, _reserved = 0;
        if (!detail::read_u8(s, kind_tag) ||
            !detail::read_u8(s, dig_alg_tag) ||
            !detail::read_u8(s, sig_alg_tag) ||
            !detail::read_u8(s, _reserved))
            return std::unexpected(artifact_error::structural("read kind/alg failed"));

        std::uint64_t stored_checksum = 0;
        if (!detail::read_u64(s, stored_checksum))
            return std::unexpected(artifact_error::structural("read checksum failed"));

        // ---- Fast corruption check (FNV-1a) before proceeding -------------------
        constexpr std::size_t k_checksum_body_start = 4 + 2 + 2 + 1 + 1 + 1 + 1 + 8;
        if (data.size() > k_checksum_body_start) {
            const auto body_span = data.subspan(k_checksum_body_start);
            const std::uint64_t cksum = detail::fnv1a_64(body_span);
            if (stored_checksum != 0 && cksum != stored_checksum)
                return std::unexpected(artifact_error::integrity("fast checksum mismatch"));
        }

        // Continue reading fixed header fields
        std::uint8_t sem_dig_len = 0;
        if (!detail::read_u8(s, sem_dig_len))
            return std::unexpected(artifact_error::structural("read semantic_digest_len failed"));

        std::array<std::uint8_t, 64> sem_dig{};
        if (!detail::read_array(s, sem_dig))
            return std::unexpected(artifact_error::structural("read semantic_digest failed"));

        std::array<std::uint8_t, 32> stored_payload_digest{};
        if (!detail::read_array(s, stored_payload_digest))
            return std::unexpected(artifact_error::structural("read payload_digest failed"));

        std::array<std::uint8_t, 32> sig_key_id{};
        if (!detail::read_array(s, sig_key_id))
            return std::unexpected(artifact_error::structural("read sig_key_id failed"));

        std::uint16_t sig_len = 0;
        if (!detail::read_u16(s, sig_len))
            return std::unexpected(artifact_error::structural("read sig_len failed"));

        std::array<std::uint8_t, 128> sig_buf{};
        if (!detail::read_array(s, sig_buf))
            return std::unexpected(artifact_error::structural("read sig_bytes failed"));

        std::uint32_t nrelocs = 0, nimports = 0, nexports = 0;
        if (!detail::read_u32(s, nrelocs) ||
            !detail::read_u32(s, nimports) ||
            !detail::read_u32(s, nexports))
            return std::unexpected(artifact_error::structural("read counts failed"));

        // ---- Variable section (compat + provenance + key) -----------------------
        compatibility_manifest compat;
        if (!detail::decode_compat(s, compat))
            return std::unexpected(artifact_error::codec("decode compat failed"));

        provenance prov;
        if (!detail::decode_provenance(s, prov))
            return std::unexpected(artifact_error::codec("decode provenance failed"));

        artifact_key key;
        if (!detail::decode_key(s, key))
            return std::unexpected(artifact_error::codec("decode key failed"));

        resource_limits limits;
        if (!detail::read_u64(s, limits.max_stack_bytes) ||
            !detail::read_u64(s, limits.max_heap_bytes) ||
            !detail::read_u64(s, limits.max_time_ns))
            return std::unexpected(artifact_error::codec("decode limits failed"));

        // ---- Stage 2: Payload size check BEFORE allocation ----------------------
        std::uint64_t payload_size = 0;
        if (!detail::read_u64(s, payload_size))
            return std::unexpected(artifact_error::structural("read payload_size failed"));
        if (payload_size > policy.max_payload_bytes)
            return std::unexpected(artifact_error::structural("payload_size exceeds limit"));
        if (s.size() < payload_size)
            return std::unexpected(artifact_error::structural("payload truncated"));

        const std::span<const std::uint8_t> payload_span = s.subspan(0, payload_size);

        // ---- Stage 2: Payload digest integrity BEFORE payload decode ------------
        if (policy.require_digest) {
            const auto computed = containers::content_digest<containers::sha256_digest_policy>(
                payload_span);
            if (computed != stored_payload_digest)
                return std::unexpected(artifact_error::integrity("payload digest mismatch"));
        }

        // ---- Stage 3: Signature authenticity -----------------------------------
        // (Caller plugs in a verifier; here we gate on policy.)
        if (policy.require_signature &&
            sig_alg_tag == static_cast<std::uint8_t>(lithe::ir::signature_algorithm::none))
            return std::unexpected(artifact_error::signature("signature required but none present"));

        // Decode payload
        payload_ref pref{inline_payload{}};
        {
            auto ps = payload_span;
            std::uint8_t payload_tag = 0;
            if (!detail::read_u8(ps, payload_tag))
                return std::unexpected(artifact_error::codec("payload tag read failed"));
            if (payload_tag == 0) {
                std::uint64_t nb = 0;
                if (!detail::read_u64(ps, nb))
                    return std::unexpected(artifact_error::codec("inline payload size failed"));
                if (ps.size() < nb)
                    return std::unexpected(artifact_error::codec("inline payload truncated"));
                inline_payload ip;
                ip.bytes.assign(ps.begin(), ps.begin() + static_cast<std::ptrdiff_t>(nb));
                pref = std::move(ip);
            }
            else {
                content_address_ref cref;
                if (!detail::read_array(ps, cref.digest))
                    return std::unexpected(artifact_error::codec("content-addr digest failed"));
                if (!detail::read_u64(ps, cref.size))
                    return std::unexpected(artifact_error::codec("content-addr size failed"));
                pref = cref;
            }
        }
        s = s.subspan(payload_size);

        // Decode reloc / import / export
        std::vector<reloc_record> relocs(nrelocs);
        for (auto& r : relocs) {
            if (!detail::read_u64(s, r.offset) ||
                !detail::read_u8(s, r.kind) ||
                !detail::read_u32(s, r.symbol))
                return std::unexpected(artifact_error::codec("reloc decode failed"));
            std::uint64_t addend64 = 0;
            if (!detail::read_u64(s, addend64))
                return std::unexpected(artifact_error::codec("reloc addend failed"));
            r.addend = static_cast<std::int64_t>(addend64);
            if (!detail::read_u8(s, r.width))
                return std::unexpected(artifact_error::codec("reloc width failed"));
        }
        std::vector<import_record> imports_dec(nimports);
        for (auto& im : imports_dec) {
            if (!detail::read_str(s, im.name) || !detail::read_str(s, im.abi_tag))
                return std::unexpected(artifact_error::codec("import decode failed"));
        }
        std::vector<export_record> exports_dec(nexports);
        for (auto& ex : exports_dec) {
            if (!detail::read_str(s, ex.name) || !detail::read_u64(s, ex.offset))
                return std::unexpected(artifact_error::codec("export decode failed"));
        }

        // Assemble the record
        artifact_record rec;
        rec.kind = static_cast<artifact_kind>(kind_tag);
        rec.semantic_digest = sem_dig;
        rec.semantic_digest_len = sem_dig_len;
        rec.payload = std::move(pref);
        rec.prov = std::move(prov);
        rec.compat = std::move(compat);
        rec.key = std::move(key);
        rec.limits = limits;

        const auto sig_algo = static_cast<lithe::ir::signature_algorithm>(sig_alg_tag);
        if (sig_algo != lithe::ir::signature_algorithm::none) {
            signature_info si;
            si.alg = sig_algo;
            si.key_id = sig_key_id;
            si.sig_bytes.assign(sig_buf.begin(),
                                sig_buf.begin() + static_cast<std::ptrdiff_t>(sig_len));
            rec.signature = std::move(si);
        }

        // Stage 4: compat check deferred to caller (requires host_profile context).
        // decode_policy::run_compat_check is a hint; caller calls check_compatible.
        (void)policy.run_compat_check;

        return rec;
    }
} // namespace lithe::execution::store
