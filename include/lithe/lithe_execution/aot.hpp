#pragma once

// =============================================================================
// lithe_execution/aot.hpp — AOT artifact serialization/deserialization
//
// AOT is a DISTINCT subsystem from IR interchange:
//   • Separate format (binary artifact blob, not IR text/binary interchange).
//   • Separate trust policy (artifact signature ≠ IR security envelope).
//   • Forces the materialized-artifact split path (compile→serialize; deserialize→install).
//   • AOT export errors are NOT routed through ir_error.
//
// Provides:
//   aot_header          — fixed-size binary prefix for all AOT blobs.
//   aot_checksum        — CRC/hash of the artifact body bytes.
//   aot_error           — error type distinct from ir_error / compile_error.
//   aot_buffer          — owning byte vector for serialized AOT data.
//   aot_view            — non-owning view into an AOT blob (Setu-mapped load path).
//   aot_validation_result — outcome of checksum + signature verification.
//
//   cpo::serialize_aot  — write artifact + header + checksum into aot_buffer.
//   cpo::deserialize_aot— read aot_view → expected<Artifact, aot_error>.
//
//   aot_signature_provider — separate provider concept for AOT signature/validation.
//     Distinct from the IR security envelope (different trust boundary).
//
//   serializer_aot<B,Artifact>   — concept: B supports AOT serialize_aot.
//   deserializer_aot<B,Artifact> — concept: B supports AOT deserialize_aot.
//
// Setu-mapped load: aot_view wraps a read-only byte span and DOES NOT copy.
//   Only deserialize_aot may read it — no mutation is permitted through the view.
//
// Guarded: #if defined(LITHE_HAS_AOT)
//   The entire implementation is feature-gated so builds without AOT incur
//   zero compile cost and zero link cost.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#if defined(LITHE_HAS_AOT)

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "foundation.hpp"   // compile_error, ir_error, ir_kind, execution_mode
#include "artifact.hpp"     // basic_compiled_artifact, artifact_manifest

namespace lithe::execution {
    // =========================================================================
    // P11  aot_error — error type for the AOT subsystem
    //
    // Intentionally distinct from ir_error and compile_error.
    // AOT errors are NOT routed through the IR error table.
    // =========================================================================

    struct aot_error {
        std::string_view detail;

        constexpr explicit aot_error(const std::string_view d = {}) noexcept
            : detail(d) {}
    };

    static_assert(!std::is_same_v<aot_error, ir_error>);
    static_assert(!std::is_same_v<aot_error, compile_error>);

    // =========================================================================
    // P11  aot_header — fixed-size binary prefix (magic + version + metadata)
    //
    // All multi-byte fields are stored little-endian.
    // The header is followed immediately by the serialized artifact body.
    // =========================================================================

    struct aot_header {
        static constexpr std::array<std::uint8_t, 4> kMagic = {'L', 'A', 'O', 'T'};
        static constexpr std::uint16_t kFormatVersion = 1;

        std::array<std::uint8_t, 4> magic = kMagic;
        std::uint16_t format_version = kFormatVersion;
        std::uint16_t flags = 0; // reserved, must be 0
        ir_kind produced_from = ir_kind::unknown;
        execution_mode target_mode = execution_mode::interpret;
        std::uint64_t backend_id_hash = 0; // FNV-1a of backend id string
        std::uint64_t code_version = 0; // matches artifact_manifest::version
        std::uint64_t body_size_bytes = 0; // size of artifact body after header
        std::uint64_t checksum = 0; // see aot_checksum

        [[nodiscard]] constexpr bool magic_ok() const noexcept {
            return magic == kMagic;
        }

        [[nodiscard]] constexpr bool version_ok() const noexcept {
            return format_version == kFormatVersion;
        }
    };

    static_assert(std::is_trivially_copyable_v<aot_header>);

    // =========================================================================
    // P11  aot_checksum — FNV-1a 64-bit hash of artifact body bytes
    //
    // Detects accidental corruption.  NOT a cryptographic MAC — use the
    // aot_signature_provider for security-critical integrity.
    // =========================================================================

    struct aot_checksum {
        std::uint64_t value = 0;

        [[nodiscard]] static constexpr std::uint64_t
        compute(const std::uint8_t* data, const std::size_t len) noexcept {
            constexpr std::uint64_t kOffset = 14695981039346656037ULL;
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            std::uint64_t h = kOffset;
            for (std::size_t i = 0; i < len; ++i) {
                h ^= static_cast<std::uint64_t>(data[i]);
                h *= kPrime;
            }
            return h;
        }

        [[nodiscard]] static aot_checksum
        of(const std::span<const std::uint8_t> body) noexcept {
            return {compute(body.data(), body.size())};
        }

        [[nodiscard]] constexpr bool operator==(const aot_checksum&) const noexcept = default;
    };

    // =========================================================================
    // P11  fnv1a_hash — inline helper for backend id string → uint64
    // =========================================================================

    [[nodiscard]] inline constexpr std::uint64_t
    fnv1a_hash(const std::string_view s) noexcept {
        constexpr std::uint64_t kOffset = 14695981039346656037ULL;
        constexpr std::uint64_t kPrime = 1099511628211ULL;
        std::uint64_t h = kOffset;
        for (const char c : s) {
            h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
            h *= kPrime;
        }
        return h;
    }

    // =========================================================================
    // P11  aot_buffer — owning byte vector for a complete serialized blob
    //
    // Layout: [aot_header][artifact body bytes]
    // The header embeds the checksum of the body.
    // =========================================================================

    struct aot_buffer {
        std::vector<std::uint8_t> bytes;

        [[nodiscard]] bool valid() const noexcept {
            return bytes.size() >= sizeof(aot_header);
        }

        [[nodiscard]] const aot_header* header() const noexcept {
            if (!valid()) return nullptr;
            return reinterpret_cast<const aot_header*>(bytes.data());
        }

        [[nodiscard]] std::span<const std::uint8_t> body() const noexcept {
            if (!valid()) return {};
            return {
                bytes.data() + sizeof(aot_header),
                bytes.size() - sizeof(aot_header)
            };
        }
    };

    // =========================================================================
    // P11  aot_view — non-owning read-only view into an AOT blob
    //
    // Setu-mapped load: the underlying bytes are NOT copied.  Only
    // deserialize_aot may read through this view.  Mutating the bytes
    // through the underlying pointer is undefined behaviour.
    // =========================================================================

    struct aot_view {
        std::span<const std::uint8_t> bytes;

        [[nodiscard]] bool valid() const noexcept {
            return bytes.size() >= sizeof(aot_header);
        }

        [[nodiscard]] const aot_header* header() const noexcept {
            if (!valid()) return nullptr;
            return reinterpret_cast<const aot_header*>(bytes.data());
        }

        [[nodiscard]] std::span<const std::uint8_t> body() const noexcept {
            if (!valid()) return {};
            return bytes.subspan(sizeof(aot_header));
        }

        // Construct a view over an aot_buffer (zero-copy).
        [[nodiscard]] static aot_view from(const aot_buffer& buf) noexcept {
            return {std::span<const std::uint8_t>{buf.bytes.data(), buf.bytes.size()}};
        }
    };

    // =========================================================================
    // P11  aot_validation_result — outcome of checksum + signature check
    // =========================================================================

    enum class aot_validation_status : std::uint8_t {
        ok = 0,
        bad_magic = 1,
        bad_version = 2,
        size_mismatch = 3,
        checksum_failed = 4,
        signature_failed = 5,
    };

    struct aot_validation_result {
        aot_validation_status status = aot_validation_status::ok;

        [[nodiscard]] constexpr bool ok() const noexcept {
            return status == aot_validation_status::ok;
        }

        [[nodiscard]] constexpr std::string_view message() const noexcept {
            switch (status) {
            case aot_validation_status::ok: return "ok";
            case aot_validation_status::bad_magic: return "bad magic bytes";
            case aot_validation_status::bad_version: return "unsupported format version";
            case aot_validation_status::size_mismatch: return "body size mismatch";
            case aot_validation_status::checksum_failed: return "checksum mismatch";
            case aot_validation_status::signature_failed: return "signature verification failed";
            }
            return "unknown";
        }
    };

    // =========================================================================
    // P11  validate_aot_view — structural + checksum validation
    //
    // Does NOT require a signature provider — only checks the structural header
    // and the FNV-1a checksum.  For security-critical integrity call the
    // aot_signature_provider separately.
    // =========================================================================

    [[nodiscard]] inline aot_validation_result
    validate_aot_view(const aot_view& view) noexcept {
        if (!view.valid())
            return {aot_validation_status::bad_magic};

        const aot_header* hdr = view.header();
        if (!hdr->magic_ok())
            return {aot_validation_status::bad_magic};
        if (!hdr->version_ok())
            return {aot_validation_status::bad_version};

        const auto body = view.body();
        if (body.size() != hdr->body_size_bytes)
            return {aot_validation_status::size_mismatch};

        const auto computed = aot_checksum::of(body);
        if (computed.value != hdr->checksum)
            return {aot_validation_status::checksum_failed};

        return {aot_validation_status::ok};
    }

    // =========================================================================
    // P11  aot_signature_provider concept
    //
    // Separate from the IR security envelope — different trust boundary.
    //   sign(Provider&, aot_buffer&)  → bool   (append/embed signature)
    //   verify(Provider const&, aot_view) → bool (verify embedded signature)
    //
    // Structural concept — no inheritance required.
    // =========================================================================

    template <class P>
    concept aot_signature_provider =
        requires {
            { P::provider_id() } -> std::convertible_to<std::string_view>;
        } &&
        requires(P& p, aot_buffer& buf) {
            { p.sign(buf) } -> std::same_as<bool>;
        } &&
        requires(const P& p, const aot_view view) {
            { p.verify(view) } -> std::same_as<bool>;
        };

    // =========================================================================
    // P11  no_aot_signature — zero-cost sentinel: always accepts
    //
    // Default when no security policy is configured.  sign() does nothing;
    // verify() always returns true (no check performed).
    // =========================================================================

    struct no_aot_signature {
        [[nodiscard]] static constexpr std::string_view provider_id() noexcept {
            return "lithe.aot.no_signature";
        }

        [[nodiscard]] constexpr bool sign(aot_buffer&) const noexcept { return true; }
        [[nodiscard]] constexpr bool verify(const aot_view) const noexcept { return true; }
    };

    static_assert(aot_signature_provider<no_aot_signature>);
    static_assert(std::is_empty_v<no_aot_signature>);

    // =========================================================================
    // P11  CPO: cpo::serialize_aot
    //
    // Writes the artifact body bytes into an aot_buffer with a prepended
    // aot_header including FNV-1a checksum.
    //
    // tag_invoke(serialize_aot_t{}, B const&, Artifact const&, aot_buffer&)
    //   → expected<void, aot_error>
    //
    // Backend tag_invoke is responsible for writing the artifact body bytes
    // into buffer.bytes (starting after the header placeholder).  The CPO
    // then computes and stamps the header.
    //
    // Sequence:
    //   1. Reserve header space (zeroed placeholder).
    //   2. Call tag_invoke → backend appends body bytes.
    //   3. CPO fills in checksum + body_size_bytes into the placeholder.
    // =========================================================================

    namespace cpo {
        struct serialize_aot_t {
            template <class B, class Artifact>
            [[nodiscard]] constexpr auto
            operator()(B&& b, Artifact&& art, aot_buffer& out) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b),
                                             std::forward<Artifact>(art), out)))
                -> decltype(tag_invoke(*this, std::forward<B>(b),
                                       std::forward<Artifact>(art), out)) {
                return tag_invoke(*this, std::forward<B>(b),
                                  std::forward<Artifact>(art), out);
            }
        };

        inline constexpr serialize_aot_t serialize_aot{};

        // --------------------------------------------------------------------
        // cpo::deserialize_aot
        //
        // Reads an aot_view (Setu-mapped, zero-copy) → Artifact.
        //
        // tag_invoke(deserialize_aot_t{}, B&, aot_view)
        //   → expected<Artifact, aot_error>
        //
        // The view MUST pass validate_aot_view() before deserialization.
        // The backend reads the body bytes to reconstruct the artifact in memory.
        // --------------------------------------------------------------------

        struct deserialize_aot_t {
            template <class B>
            [[nodiscard]] constexpr auto
            operator()(B&& b, const aot_view view) const
                noexcept(noexcept(tag_invoke(*this, std::forward<B>(b), view)))
                -> decltype(tag_invoke(*this, std::forward<B>(b), view)) {
                return tag_invoke(*this, std::forward<B>(b), view);
            }
        };

        inline constexpr deserialize_aot_t deserialize_aot{};
    } // namespace cpo

    // =========================================================================
    // P11  Detection helpers for AOT facet concepts
    // =========================================================================

    namespace detail {
        template <class B, class Artifact>
        concept has_serialize_aot = requires(const B& b, const Artifact& art, aot_buffer& out) {
            cpo::serialize_aot(b, art, out);
        };

        template <class B>
        concept has_deserialize_aot = requires(B& b, aot_view view) {
            cpo::deserialize_aot(b, view);
        };
    } // namespace detail

    // =========================================================================
    // P11  Facet concepts
    // =========================================================================

    // serializer_aot<B,Artifact>: B can serialize artifacts to AOT blobs.
    template <class B, class Artifact>
    concept serializer_aot = detail::has_serialize_aot<B, artifact_value_t<Artifact>>;

    // deserializer_aot<B>: B can deserialize aot_view → Artifact.
    template <class B>
    concept deserializer_aot = detail::has_deserialize_aot<B>;

    // =========================================================================
    // P11  make_aot_buffer — high-level serialize + header stamp helper
    //
    // Orchestrates the full serialize→header→checksum sequence.
    // The caller supplies the artifact manifest to populate header metadata.
    // An optional signature_provider is applied after stamping.
    //
    // Returns: expected<aot_buffer, aot_error>
    //
    // Not a CPO — a free function that delegates to the cpo::serialize_aot CPO.
    // =========================================================================

    template <class B, class Artifact,
              class SigProvider = no_aot_signature>
        requires serializer_aot<std::remove_cvref_t<B>, std::remove_cvref_t<Artifact>>
    [[nodiscard]] std::expected<aot_buffer, aot_error>
    make_aot_buffer(B&& backend,
                    const Artifact& artifact,
                    const artifact_manifest& manifest,
                    SigProvider&& sig = {}) {
        aot_buffer out;

        // Reserve space for the header (zero-filled placeholder).
        out.bytes.resize(sizeof(aot_header), std::uint8_t{0});
        const std::size_t body_start = out.bytes.size();

        // Ask the backend to append body bytes.
        auto result = cpo::serialize_aot(std::forward<B>(backend), artifact, out);
        if (!result)
            return std::unexpected(result.error());

        const std::size_t body_size = out.bytes.size() - body_start;

        // Compute checksum over the body bytes only.
        const auto body_span = std::span<const std::uint8_t>{
            out.bytes.data() + body_start, body_size
        };
        const auto cksum = aot_checksum::of(body_span);

        // Stamp the header in place.
        aot_header hdr;
        hdr.produced_from = manifest.produced_from;
        hdr.target_mode = execution_mode::interpret; // default; backend may override
        hdr.backend_id_hash = fnv1a_hash(manifest.backend_id);
        hdr.code_version = manifest.version;
        hdr.body_size_bytes = body_size;
        hdr.checksum = cksum.value;

        static_assert(sizeof(aot_header) <= 64,
                      "aot_header unexpectedly large — check alignment/padding");
        std::memcpy(out.bytes.data(), &hdr, sizeof(aot_header));

        // Apply signature provider (may append or embed a signature).
        if (!sig.sign(out))
            return std::unexpected(aot_error{"aot: signature provider sign() failed"});

        return out;
    }

    // =========================================================================
    // P11  load_aot — Setu-mapped load + validate + deserialize
    //
    // Zero-copy path: the bytes pointed to by `view` must outlive the returned
    // Artifact (or be copied into it by the backend).
    //
    // Steps:
    //   1. Structural + checksum validation (validate_aot_view).
    //   2. Optional signature verification via the SigProvider.
    //   3. Delegate to cpo::deserialize_aot → Artifact.
    //
    // The returned Artifact owns its data; the view may be released afterward.
    // =========================================================================

    template <class B,
              class SigProvider = no_aot_signature>
        requires deserializer_aot<std::remove_cvref_t<B>>
    [[nodiscard]] auto
    load_aot(B&& backend,
             const aot_view view,
             const SigProvider& sig = {})
        -> std::expected<
            typename decltype(
                cpo::deserialize_aot(std::declval<B>(), aot_view{}))::value_type,
            aot_error> {
        // Step 1: structural + checksum validation.
        const auto vresult = validate_aot_view(view);
        if (!vresult.ok())
            return std::unexpected(aot_error{vresult.message()});

        // Step 2: signature check (separate trust boundary from IR security envelope).
        if (!sig.verify(view))
            return std::unexpected(aot_error{"aot: signature verification failed"});

        // Step 3: deserialize via the backend CPO.
        return cpo::deserialize_aot(std::forward<B>(backend), view);
    }
} // namespace lithe::execution

#endif // defined(LITHE_HAS_AOT)
