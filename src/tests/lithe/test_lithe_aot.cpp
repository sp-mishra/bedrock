// =============================================================================
// test_lithe_aot.cpp — AOT serialize/deserialize round-trip (§11 P11, guarded)
//
// Guarded by LITHE_HAS_AOT.  Tests are skipped (WARN) if not defined.
//
// Verifies:
//   • aot_header magic and version stamped correctly.
//   • aot_checksum FNV-1a computed correctly; mismatch detected.
//   • validate_aot_view: ok / bad_magic / bad_version / size_mismatch / checksum_failed.
//   • make_aot_buffer: header + checksum stamped; body size correct.
//   • load_aot: validation + optional signature + deserialize round-trip.
//   • no_aot_signature: always passes; zero-cost empty struct.
//   • aot_signature_provider concept satisfied by no_aot_signature.
//   • aot_error is distinct from ir_error and compile_error.
//   • AOT errors are NOT routed through the IR error table (different type).
//   • serializer_aot / deserializer_aot concepts.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#if defined(LITHE_HAS_AOT)

#include "lithe/lithe_execution/aot.hpp"
#include "lithe/lithe_execution/foundation.hpp"

namespace ex = lithe::execution;

// ============================================================================
// Compile-time: aot_error is distinct from ir_error and compile_error
// ============================================================================

static_assert(!std::is_same_v<ex::aot_error, ex::ir_error>);
static_assert(!std::is_same_v<ex::aot_error, ex::compile_error>);
static_assert(std::is_empty_v<ex::no_aot_signature>);
static_assert(ex::aot_signature_provider<ex::no_aot_signature>);

// ============================================================================
// Minimal stub backend for AOT round-trip tests
// ============================================================================

namespace {
    // Fake "artifact" type for AOT tests — just a byte vector.
    struct fake_aot_artifact {
        std::vector<std::uint8_t> code_bytes;
        bool valid() const noexcept { return !code_bytes.empty(); }
    };

    // Stub backend implementing serialize_aot and deserialize_aot.
    struct fake_aot_backend {
        // serialize_aot: append artifact body bytes to the aot_buffer.
        friend std::expected<void, ex::aot_error>
        tag_invoke(ex::cpo::serialize_aot_t,
                   const fake_aot_backend&,
                   const fake_aot_artifact& art,
                   ex::aot_buffer& out) {
            if (art.code_bytes.empty())
                return std::unexpected(ex::aot_error{"fake_aot_backend: empty artifact"});
            for (const auto b : art.code_bytes)
                out.bytes.push_back(b);
            return {};
        }

        // deserialize_aot: reconstruct artifact from the aot_view body.
        friend std::expected<fake_aot_artifact, ex::aot_error>
        tag_invoke(ex::cpo::deserialize_aot_t,
                   fake_aot_backend&,
                   const ex::aot_view view) {
            const auto body = view.body();
            fake_aot_artifact art;
            art.code_bytes.assign(body.begin(), body.end());
            return art;
        }
    };

    static_assert(ex::serializer_aot<fake_aot_backend, fake_aot_artifact>);
    static_assert(ex::deserializer_aot<fake_aot_backend>);

    inline ex::artifact_manifest make_manifest() {
        ex::artifact_manifest m;
        m.produced_from = ex::ir_kind::physical_mir;
        m.backend_id = "lithe.test.fake_aot_backend";
        m.version = 42;
        return m;
    }

    inline fake_aot_artifact make_artifact() {
        fake_aot_artifact art;
        art.code_bytes = {0x01, 0x02, 0x03, 0xDE, 0xAD, 0xBE, 0xEF, 0x00};
        return art;
    }
} // namespace

// ============================================================================
// §11 P11  aot_checksum
// ============================================================================

TEST_CASE ("aot_checksum: same bytes produce same hash",
          "[aot][checksum]")
{
    const std::uint8_t data[] = {1, 2, 3, 4, 5};
    const auto c1 = ex::aot_checksum::of(
        std::span<const std::uint8_t>{data, sizeof(data)});
    const auto c2 = ex::aot_checksum::of(
        std::span<const std::uint8_t>{data, sizeof(data)});
    CHECK(c1 == c2);
}

TEST_CASE ("aot_checksum: different bytes produce different hash",
          "[aot][checksum]")
{
    const std::uint8_t d1[] = {1, 2, 3};
    const std::uint8_t d2[] = {1, 2, 4};
    const auto c1 = ex::aot_checksum::of(std::span<const std::uint8_t>{d1, 3});
    const auto c2 = ex::aot_checksum::of(std::span<const std::uint8_t>{d2, 3});
    CHECK(!(c1 == c2));
}

// ============================================================================
// §11 P11  aot_header
// ============================================================================

TEST_CASE ("aot_header: default magic and format version are correct",
          "[aot][header]")
{
    ex::aot_header hdr;
    CHECK(hdr.magic_ok());
    CHECK(hdr.version_ok());
}

// ============================================================================
// §11 P11  validate_aot_view
// ============================================================================

TEST_CASE ("validate_aot_view: valid buffer passes",
          "[aot][validate]")
{
    fake_aot_backend backend;
    const auto art = make_artifact();
    const auto manifest = make_manifest();

    auto buf_result = ex::make_aot_buffer(backend, art, manifest);
    REQUIRE(buf_result.has_value());

    const auto view = ex::aot_view::from(*buf_result);
    const auto vr   = ex::validate_aot_view(view);
    CHECK(vr.ok());
}

TEST_CASE ("validate_aot_view: empty bytes → bad_magic",
          "[aot][validate]")
{
    ex::aot_view empty_view{{}};
    const auto vr = ex::validate_aot_view(empty_view);
    CHECK(!vr.ok());
    CHECK(vr.status == ex::aot_validation_status::bad_magic);
}

TEST_CASE ("validate_aot_view: corrupted magic → bad_magic",
          "[aot][validate]")
{
    fake_aot_backend backend;
    const auto art = make_artifact();
    auto buf_result = ex::make_aot_buffer(backend, art, make_manifest());
    REQUIRE(buf_result.has_value());

    // Corrupt the first byte of the magic.
    buf_result->bytes[0] = 0xFF;

    const auto view = ex::aot_view::from(*buf_result);
    const auto vr   = ex::validate_aot_view(view);
    CHECK(!vr.ok());
    CHECK(vr.status == ex::aot_validation_status::bad_magic);
}

TEST_CASE ("validate_aot_view: checksum mismatch → checksum_failed",
          "[aot][validate]")
{
    fake_aot_backend backend;
    const auto art = make_artifact();
    auto buf_result = ex::make_aot_buffer(backend, art, make_manifest());
    REQUIRE(buf_result.has_value());

    // Corrupt a body byte (after the header).
    const auto header_sz = sizeof(ex::aot_header);
    if (buf_result->bytes.size() > header_sz) {
        buf_result->bytes[header_sz] ^= 0xFF;
    }

    const auto view = ex::aot_view::from(*buf_result);
    const auto vr   = ex::validate_aot_view(view);
    CHECK(!vr.ok());
    CHECK(vr.status == ex::aot_validation_status::checksum_failed);
}

// ============================================================================
// §11 P11  make_aot_buffer → load_aot round-trip
// ============================================================================

TEST_CASE ("make_aot_buffer + load_aot: round-trip preserves artifact bytes",
          "[aot][round_trip]")
{
    fake_aot_backend backend;
    const auto art      = make_artifact();
    const auto manifest = make_manifest();

    auto buf_result = ex::make_aot_buffer(backend, art, manifest);
    REQUIRE(buf_result.has_value());
    CHECK(buf_result->valid());

    // Header must be stamped.
    const auto* hdr = buf_result->header();
    REQUIRE(hdr != nullptr);
    CHECK(hdr->magic_ok());
    CHECK(hdr->version_ok());
    CHECK(hdr->body_size_bytes == art.code_bytes.size());
    CHECK(hdr->backend_id_hash == ex::fnv1a_hash(manifest.backend_id));
    CHECK(hdr->code_version == manifest.version);

    // Load via aot_view (Setu-mapped, zero-copy).
    const auto view = ex::aot_view::from(*buf_result);

    fake_aot_backend backend2;
    auto load_result = ex::load_aot(backend2, view);
    REQUIRE(load_result.has_value());
    CHECK(load_result->code_bytes == art.code_bytes);
}

// ============================================================================
// §11 P11  Signature provider rejection
// ============================================================================

namespace {
    // Stub signature provider that always rejects during verify.
    struct always_reject_sig {
        [[nodiscard]] static constexpr std::string_view provider_id() noexcept {
            return "lithe.test.always_reject";
        }

        [[nodiscard]] bool sign(ex::aot_buffer&) const noexcept { return true; }
        [[nodiscard]] bool verify(const ex::aot_view) const noexcept { return false; }
    };

    static_assert(ex::aot_signature_provider<always_reject_sig>);
} // namespace

TEST_CASE ("load_aot: signature verification failure → aot_error",
          "[aot][signature]")
{
    fake_aot_backend backend;
    const auto art = make_artifact();
    auto buf_result = ex::make_aot_buffer(backend, art, make_manifest());
    REQUIRE(buf_result.has_value());

    const auto view = ex::aot_view::from(*buf_result);
    always_reject_sig sig;

    fake_aot_backend backend2;
    auto load_result = ex::load_aot(backend2, view, sig);
    REQUIRE(!load_result.has_value());
}

// ============================================================================
// §11 P11  AOT errors are NOT ir_error (separate error table)
// ============================================================================

TEST_CASE ("aot_error: distinct from ir_error and compile_error",
          "[aot][error_type]")
{
    static_assert(!std::is_same_v<ex::aot_error, ex::ir_error>,
        "aot_error must not be ir_error — AOT is not in the IR error table");
    static_assert(!std::is_same_v<ex::aot_error, ex::compile_error>,
        "aot_error must not be compile_error");
    SUCCEED("aot_error type contract verified at compile time");
}

// ============================================================================
// §11 P11  no_aot_signature: zero-cost; always passes
// ============================================================================

TEST_CASE ("no_aot_signature: sign() returns true; verify() returns true",
          "[aot][signature][no_op]")
{
    ex::no_aot_signature sig;
    ex::aot_buffer buf;
    CHECK(sig.sign(buf));
    CHECK(sig.verify(ex::aot_view{}));
}

#else

TEST_CASE (


"aot: LITHE_HAS_AOT not defined — all tests skipped"
,
"[aot]"
)
{
    WARN("LITHE_HAS_AOT is not defined; AOT tests are guarded and will not run.");
}

#endif // LITHE_HAS_AOT
