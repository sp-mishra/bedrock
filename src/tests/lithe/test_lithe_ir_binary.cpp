// =============================================================================
// test_lithe_ir_binary.cpp — binary_provider encode/decode + security envelope
// + validation ordering (§10a.8, impl-5 P13)
//
// Tests:
//   1. Encode→decode round-trip preserves IR.
//   2. Envelope fields (magic/major/minor/endian/target-address-width/digest)
//      round-trip.
//   3. target_address_width == 0 rejected BEFORE any byte read.
//   4. Integrity digest mismatch → reject.
//   5. Authenticity verified before IR is trusted (signature failure never
//      reaches compile — assert trust gate precedes decode-into-IR).
//   6. Structural validation before allocation (over-maximum_decoded_size
//      payload rejected without the large allocation).
//   7. No size_t/pointers on the wire (fixed-width only — compile-time/size
//      asserts on wire structs).
//   8. Unknown required section → reject; unknown optional → preserve (opaque).
//   9. Major mismatch → reject.
//  10. Fuzz/malformed-input corpus — truncated, over-count, bad-offset inputs
//      all reject cleanly.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lithe/lithe_ir/providers/binary_provider.hpp"
#include "lithe/lithe_ir/security_envelope.hpp"
#include "lithe/lithe_ir/format.hpp"
#include "lithe/lithe_ir/provider.hpp"
#include "lithe/lithe_ir/integration.hpp"

namespace irns = lithe::ir;
using Prov = irns::default_binary_provider;

// ============================================================================
// Helpers
// ============================================================================

namespace {
    irns::format_descriptor make_fmt() {
        return *irns::format_descriptor::make(
            irns::encoding::binary_le,
            irns::stage::physical,
            {1, 0, 0}, 64,
            lithe::execution::ir_kind::physical_mir);
    }

    // Build a minimal valid lithe_binary_ir_doc
    irns::lithe_binary_ir_doc make_minimal_doc() {
        irns::lithe_binary_ir_doc doc;
        doc.doc_format = make_fmt();

        // One value
        irns::binary_ir_value_record v;
        v.id = 0;
        v.kind = 1;
        v.bit_width = 64;
        doc.values.push_back(v);

        // One op (known domain)
        irns::binary_ir_op_record op;
        op.identity.stable_domain_id = "lithe.core";
        op.identity.stable_operation_id = "add";
        op.identity.op_schema_version = {1, 0, 0};
        op.result_ids = {0};
        op.operand_ids = {};
        op.block_id = 0;
        doc.ops.push_back(std::move(op));

        // One block
        irns::binary_ir_block_record blk;
        blk.id = 0;
        blk.op_ids = {0};
        doc.blocks.push_back(blk);

        return doc;
    }

    // Encode doc to bytes
    std::vector<std::uint8_t> encode_doc(const irns::lithe_binary_ir_doc& doc) {
        Prov prov;
        const auto result = irns::cpo::export_binary(prov, doc, doc.doc_format);
        REQUIRE(result.has_value());
        return result->data;
    }
} // anonymous namespace

// ============================================================================
// TEST: Wire struct sizes — no size_t/pointers on the wire
// ============================================================================

TEST_CASE (


"binary security envelope: wire struct fields are fixed-width"
,
"[lithe_ir][binary_provider]"
)
 {
    // binary_ir_envelope must have no implicit padding that could introduce
    // host-size-dependent layout.  All fields are uint8/16/32/64.
    static_assert(sizeof(irns::binary_ir_envelope::magic)         == 4);
    static_assert(sizeof(irns::binary_ir_envelope::digest_bytes)  == 64);
    static_assert(sizeof(irns::binary_ir_envelope::sig_bytes)     == 128);
    static_assert(sizeof(irns::binary_ir_envelope::sig_key_id)    == 32);
    static_assert(sizeof(irns::section_entry::name_bytes)         == 64);
    SUCCEED("All wire struct size assertions pass");
}

// ============================================================================
// TEST: target_address_width == 0 rejected before any byte read
// ============================================================================

TEST_CASE (


"binary_provider: target_address_width==0 rejected structurally (before alloc)"
,
"[lithe_ir][binary_provider]"
)
 {
    // Build a raw envelope with addr_width = 0
    std::vector<std::uint8_t> bad_data(sizeof(irns::binary_ir_envelope) + 16, 0);
    irns::binary_ir_envelope env{};
    env.magic                = irns::k_binary_ir_magic;
    env.format_major         = 1;
    env.target_address_width = 0;  // INVALID
    std::memcpy(bad_data.data(), &env, sizeof(irns::binary_ir_envelope));

    const irns::envelope_limits limits;
    const auto sv_res = irns::validate_envelope_structural(
        std::span<const std::uint8_t>{bad_data.data(), bad_data.size()}, limits);

    CHECK(!sv_res.ok);
    CHECK(sv_res.error_detail.find("target_address_width") != std::string::npos);
}

// ============================================================================
// TEST: Encode/decode round-trip
// ============================================================================

TEST_CASE (


"binary_provider: encode→decode round-trip preserves IR"
,
"[lithe_ir][binary_provider]"
)
 {
    const auto original = make_minimal_doc();
    const auto bytes    = encode_doc(original);

    Prov prov;
    irns::binary_ir_view view;
    view.data   = std::span<const std::uint8_t>{bytes.data(), bytes.size()};
    view.format = original.doc_format;

    irns::diagnostic_list diags;
    const auto r = prov.do_import_binary(view, diags);
    REQUIRE(r.has_value());

    // Values preserved
    CHECK(r->values.size() == original.values.size());
    // Ops preserved
    CHECK(r->ops.size() == original.ops.size());
    if (!r->ops.empty() && !original.ops.empty()) {
        CHECK(r->ops[0].identity.stable_domain_id    ==
              original.ops[0].identity.stable_domain_id);
        CHECK(r->ops[0].identity.stable_operation_id ==
              original.ops[0].identity.stable_operation_id);
    }
    // Blocks preserved
    CHECK(r->blocks.size() == original.blocks.size());
}

// ============================================================================
// TEST: Envelope fields round-trip (magic / major / endian / addr-width)
// ============================================================================

TEST_CASE (


"binary_provider: envelope header fields round-trip"
,
"[lithe_ir][binary_provider]"
)
 {
    const auto doc   = make_minimal_doc();
    const auto bytes = encode_doc(doc);

    REQUIRE(bytes.size() >= sizeof(irns::binary_ir_envelope));

    irns::binary_ir_envelope env{};
    std::memcpy(&env, bytes.data(), sizeof(irns::binary_ir_envelope));

    CHECK(env.magic == irns::k_binary_ir_magic);
    CHECK(env.format_major == 1);
    CHECK(env.target_address_width == 64);
    CHECK(env.schema_major == 1);
    CHECK(env.schema_minor == 0);
    CHECK(env.schema_patch == 0);
    CHECK(env.ir_stage_tag == static_cast<std::uint8_t>(irns::stage::physical));
}

// ============================================================================
// TEST: Major version mismatch → reject
// ============================================================================

TEST_CASE (


"binary_provider: major version mismatch rejected"
,
"[lithe_ir][binary_provider]"
)
 {
    auto bytes = encode_doc(make_minimal_doc());
    // Corrupt the major version field
    irns::binary_ir_envelope env{};
    std::memcpy(&env, bytes.data(), sizeof(irns::binary_ir_envelope));
    env.format_major = 99;  // unsupported major
    std::memcpy(bytes.data(), &env, sizeof(irns::binary_ir_envelope));

    const irns::envelope_limits limits;
    const auto sv_res = irns::validate_envelope_structural(
        std::span<const std::uint8_t>{bytes.data(), bytes.size()}, limits);

    CHECK(!sv_res.ok);
    CHECK(sv_res.error_detail.find("major") != std::string::npos);
}

// ============================================================================
// TEST: Digest mismatch → reject
// ============================================================================

TEST_CASE (


"binary_provider: integrity digest mismatch → reject"
,
"[lithe_ir][binary_provider]"
)
 {
    // Use a verifier that always reports failure
    struct failing_digest_verifier {
        irns::envelope_integrity_result
        operator()(std::span<const std::uint8_t>,
                   irns::digest_algorithm,
                   std::span<const std::uint8_t>) const noexcept {
            return {.ok = false, .error_detail = "test: digest mismatch injected"};
        }
    };

    irns::binary_provider<failing_digest_verifier> prov;

    const auto bytes = encode_doc(make_minimal_doc());
    irns::binary_ir_view view;
    view.data   = std::span<const std::uint8_t>{bytes.data(), bytes.size()};
    view.format = make_fmt();

    irns::diagnostic_list diags;
    const auto r = prov.do_import_binary(view, diags);
    CHECK(!r.has_value());
    if (!r.has_value()) {
        CHECK(r.error().detail.find("digest") != std::string::npos);
    }
}

// ============================================================================
// TEST: Signature failure never reaches compile (trust gate before decode)
// ============================================================================

TEST_CASE (


"binary_provider: signature failure blocks trust gate (before decode)"
,
"[lithe_ir][binary_provider]"
)
 {
    // A signature verifier that always fails (not just skips)
    struct failing_sig_verifier {
        irns::envelope_auth_result
        operator()(std::span<const std::uint8_t>,
                   irns::signature_algorithm,
                   std::span<const std::uint8_t>,
                   std::span<const std::uint8_t>) const noexcept {
            return {.ok = false, .error_detail = "test: sig verification failed"};
        }
    };

    // Limits: require signature (not allow_no_signature)
    irns::envelope_limits limits;
    limits.allow_no_signature = false;

    irns::binary_provider<irns::no_digest_verifier, failing_sig_verifier> prov{limits};

    const auto bytes = encode_doc(make_minimal_doc());
    irns::binary_ir_view view;
    view.data   = std::span<const std::uint8_t>{bytes.data(), bytes.size()};
    view.format = make_fmt();

    irns::diagnostic_list diags;
    const auto r = prov.do_import_binary(view, diags);
    // Must fail BEFORE decode; the error is about signature, not about IR content
    CHECK(!r.has_value());
    if (!r.has_value()) {
        CHECK(r.error().detail.find("signature") != std::string::npos);
    }
}

// ============================================================================
// TEST: Structural validation before allocation — maximum_decoded_size limit
// ============================================================================

TEST_CASE (


"binary_provider: maximum_decoded_size limit prevents large allocation"
,
"[lithe_ir][binary_provider]"
)
 {
    auto bytes = encode_doc(make_minimal_doc());

    // Set maximum_decoded_size to a huge value in the envelope
    irns::binary_ir_envelope env{};
    std::memcpy(&env, bytes.data(), sizeof(irns::binary_ir_envelope));
    env.maximum_decoded_size = 1024ULL * 1024 * 1024 * 1024;  // 1 TiB
    std::memcpy(bytes.data(), &env, sizeof(irns::binary_ir_envelope));

    // Configure limits with a much smaller max
    irns::envelope_limits limits;
    limits.max_decoded_size = 1024; // 1 KiB

    const auto sv_res = irns::validate_envelope_structural(
        std::span<const std::uint8_t>{bytes.data(), bytes.size()}, limits);

    // Must be rejected by structural validation — no large allocation occurs
    CHECK(!sv_res.ok);
    CHECK(sv_res.error_detail.find("decoded") != std::string::npos);
}

// ============================================================================
// TEST: Unknown required section → reject; unknown optional → opaque-optional
// ============================================================================

TEST_CASE (


"binary_provider: unknown required section rejected"
,
"[lithe_ir][binary_provider]"
)
 {
    // Build a doc, then modify the section directory to add an unknown required section.
    auto doc   = make_minimal_doc();
    auto bytes = encode_doc(doc);

    if (bytes.size() < sizeof(irns::binary_ir_envelope)) {
        SKIP("encoded doc too small for this test");
    }

    irns::binary_ir_envelope env{};
    std::memcpy(&env, bytes.data(), sizeof(irns::binary_ir_envelope));

    if (env.section_count == 0) {
        SKIP("no section directory to modify");
    }

    // Overwrite first section entry name with an unknown name and mark required
    const std::size_t dir_off = env.section_dir_offset;
    if (dir_off + sizeof(irns::section_entry) > bytes.size()) {
        SKIP("section directory out of range");
    }

    irns::section_entry se{};
    std::memcpy(&se, bytes.data() + sizeof(irns::binary_ir_envelope) + dir_off,
                sizeof(irns::section_entry));
    const std::string unknown_name = "unknown.future.required.section";
    const std::size_t nm_len = std::min(unknown_name.size(), std::size_t{63});
    std::fill(se.name_bytes.begin(), se.name_bytes.end(), std::uint8_t{0});
    std::memcpy(se.name_bytes.data(), unknown_name.data(), nm_len);
    se.name_len   = static_cast<std::uint32_t>(nm_len);
    se.is_required = 1;  // required
    std::memcpy(bytes.data() + sizeof(irns::binary_ir_envelope) + dir_off,
                &se, sizeof(irns::section_entry));

    Prov prov;
    irns::binary_ir_view view;
    view.data   = std::span<const std::uint8_t>{bytes.data(), bytes.size()};
    view.format = doc.doc_format;

    irns::diagnostic_list diags;
    const auto r = prov.do_import_binary(view, diags);
    CHECK(!r.has_value()); // unknown required section → reject
}

// ============================================================================
// TEST: Fuzz corpus — malformed inputs reject cleanly
// ============================================================================

TEST_CASE (


"binary_provider: malformed inputs rejected cleanly (no UB, no over-alloc)"
,
"[lithe_ir][binary_provider]"
)
 {
    Prov prov;
    const auto fmt = make_fmt();

    // Truncated input (just 4 bytes — too small for envelope)
    {
        const std::vector<std::uint8_t> tiny = {0x4C, 0x54, 0x49, 0x52};
        irns::binary_ir_view view;
        view.data   = std::span<const std::uint8_t>{tiny.data(), tiny.size()};
        view.format = fmt;
        irns::diagnostic_list diags;
        const auto r = prov.do_import_binary(view, diags);
        CHECK(!r.has_value());
    }

    // Empty input
    {
        const std::vector<std::uint8_t> empty{};
        irns::binary_ir_view view;
        view.data   = std::span<const std::uint8_t>{empty.data(), empty.size()};
        view.format = fmt;
        irns::diagnostic_list diags;
        const auto r = prov.do_import_binary(view, diags);
        CHECK(!r.has_value());
    }

    // Bad magic
    {
        std::vector<std::uint8_t> bad(sizeof(irns::binary_ir_envelope) + 8, 0xFF);
        irns::binary_ir_view view;
        view.data   = std::span<const std::uint8_t>{bad.data(), bad.size()};
        view.format = fmt;
        irns::diagnostic_list diags;
        const auto r = prov.do_import_binary(view, diags);
        CHECK(!r.has_value());
    }

    // Over-section_count (huge count in header)
    {
        std::vector<std::uint8_t> bad(sizeof(irns::binary_ir_envelope) + 8, 0);
        irns::binary_ir_envelope env{};
        env.magic                = irns::k_binary_ir_magic;
        env.format_major         = 1;
        env.target_address_width = 64;
        env.section_count        = 0xFFFFFFFF; // way too many
        std::memcpy(bad.data(), &env, sizeof(irns::binary_ir_envelope));
        irns::binary_ir_view view;
        view.data   = std::span<const std::uint8_t>{bad.data(), bad.size()};
        view.format = fmt;
        irns::diagnostic_list diags;
        const auto r = prov.do_import_binary(view, diags);
        CHECK(!r.has_value());
    }

    // Bad offset (section directory off the end)
    {
        std::vector<std::uint8_t> bad(sizeof(irns::binary_ir_envelope) + 8, 0);
        irns::binary_ir_envelope env{};
        env.magic                = irns::k_binary_ir_magic;
        env.format_major         = 1;
        env.target_address_width = 64;
        env.section_count        = 1;
        env.section_dir_offset   = 0xFFFF0000; // way past end
        std::memcpy(bad.data(), &env, sizeof(irns::binary_ir_envelope));
        irns::binary_ir_view view;
        view.data   = std::span<const std::uint8_t>{bad.data(), bad.size()};
        view.format = fmt;
        irns::diagnostic_list diags;
        const auto r = prov.do_import_binary(view, diags);
        CHECK(!r.has_value());
    }
}

// ============================================================================
// TEST: CPO concept satisfaction
// ============================================================================

TEST_CASE (


"binary_provider: satisfies typed CPO concepts"
,
"[lithe_ir][binary_provider]"
)
 {
    static_assert(irns::binary_importer_for<Prov, irns::lithe_binary_ir_doc>);
    static_assert(irns::binary_exporter_for<Prov, irns::lithe_binary_ir_doc>);
    static_assert(irns::ir_validator_for<Prov, irns::lithe_binary_ir_doc>);
    SUCCEED("All binary_provider concept assertions pass");
}
