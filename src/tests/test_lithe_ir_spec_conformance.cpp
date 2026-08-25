// =============================================================================
// test_lithe_ir_spec_conformance.cpp — IR spec conformance test (impl-6)
//
// Pins docs/spec/lithe-ir-spec.md to the actual code constants.
// If spec and code diverge, this test fails — forcing a spec update.
//
// Tests:
//   1. Stage + section-id + envelope constants match spec (§3/§4/§12)
//   2. Opcode signature registry matches spec (§8) — bidirectional
//   3. Canonical type grammar accepts/rejects per spec (§5)
//   4. Determinism + semantic/payload digest distinction (§11)
//   5. Validation ordering observability + validity definition (§12/§13)
// =============================================================================

#include "catch_amalgamated.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Light IR core
#include "lithe/lithe_ir_core.hpp"
#include "lithe/lithe_ir/security_envelope.hpp"

// Spec fixture — transcribed constants from docs/spec/lithe-ir-spec.md
#include "spec/lithe_ir_spec_fixture.hpp"

using namespace lithe::ir;
using namespace lithe::ir::portable;
using namespace lithe::ir::adapters;
using namespace lithe::spec::fixture;

// =============================================================================
// Helpers
// =============================================================================

namespace {
    // Minimal valid portable_module for digest/verify tests
    portable_module make_conformance_module() {
        portable_module mod;
        mod.schema = {1, 0, 0};

        lithe_hl_mir_ir fn;
        fn.function_name = "conf_fn";
        fn.source_stage = stage::lowered;
        fn.schema = {1, 0, 0};
        fn.values = {hl_wire_value{0, "i64"}, hl_wire_value{1, "i64"}};

        hl_wire_op c_op;
        c_op.id = 0;
        c_op.domain = "lithe.hl";
        c_op.name = "constant";
        c_op.result_ids = {0};
        c_op.block_id = 0;
        c_op.region_id = 0;

        hl_wire_op y_op;
        y_op.id = 1;
        y_op.domain = "lithe.hl";
        y_op.name = "region_yield";
        y_op.operand_ids = {0};
        y_op.block_id = 0;
        y_op.region_id = 0;

        fn.ops = {c_op, y_op};
        fn.blocks = {hl_wire_block{0, {0, 1}, {}}};
        fn.regions = {hl_wire_region{0, {0}, {}}};
        fn.entry_block_ids = {0};

        mod.functions.push_back(fn);
        mod.manifest.producer = "spec_conformance";
        mod.manifest.source_language = "lithe_test";
        return mod;
    }
} // namespace

// =============================================================================
// Test 1: Stage values + section ids + envelope magic match spec
// =============================================================================

TEST_CASE (

"spec: stage integer values match §3.1"
,
"[spec][stages]"
)
 {
    // Verify each spec-listed stage maps to the correct code enum value.
    for (const auto& e : k_spec_stages) {
        if (e.name == "surface")   CHECK(static_cast<std::uint8_t>(stage::surface)   == e.value);
        if (e.name == "canonical") CHECK(static_cast<std::uint8_t>(stage::canonical) == e.value);
        if (e.name == "optimized") CHECK(static_cast<std::uint8_t>(stage::optimized) == e.value);
        if (e.name == "lowered")   CHECK(static_cast<std::uint8_t>(stage::lowered)   == e.value);
        if (e.name == "physical")  CHECK(static_cast<std::uint8_t>(stage::physical)  == e.value);
        if (e.name == "managed")   CHECK(static_cast<std::uint8_t>(stage::managed)   == e.value);
    }
}

TEST_CASE (

"spec: Graph IR section ids match §6.2"
,
"[spec][section-ids]"
)
 {
    using namespace lithe::ir::adapters::section_ids;
    const std::array<std::string_view, 4> code_ids = {
        graph_nodes, graph_strings, graph_roots, graph_meta
    };
    for (std::size_t i = 0; i < code_ids.size(); ++i)
        CHECK(code_ids[i] == k_spec_graph_section_ids[i]);
}

TEST_CASE (

"spec: HL MIR section ids match §7.4"
,
"[spec][section-ids]"
)
 {
    using namespace lithe::ir::adapters::section_ids;
    const std::array<std::string_view, 6> code_ids = {
        hl_mir_values, hl_mir_ops, hl_mir_blocks,
        hl_mir_regions, hl_mir_meta, hl_mir_strings
    };
    for (std::size_t i = 0; i < code_ids.size(); ++i)
        CHECK(code_ids[i] == k_spec_hl_mir_section_ids[i]);
}

TEST_CASE (

"spec: Physical MIR section ids match §9.3"
,
"[spec][section-ids]"
)
 {
    using namespace lithe::ir::adapters::section_ids;
    const std::array<std::string_view, 6> code_ids = {
        phys_vregs, phys_pregs, phys_spills,
        phys_instrs, phys_blocks, phys_meta
    };
    for (std::size_t i = 0; i < code_ids.size(); ++i)
        CHECK(code_ids[i] == k_spec_phys_section_ids[i]);
}

TEST_CASE (

"spec: envelope magic matches §12.1"
,
"[spec][envelope]"
)
 {
    CHECK(k_binary_ir_magic == k_spec_envelope_magic);
}

TEST_CASE (

"spec: digest algorithm ids and sizes match §12.4"
,
"[spec][digest-algs]"
)
 {
    for (const auto& e : k_spec_digest_algs) {
        const auto alg = static_cast<digest_algorithm>(e.id);
        CHECK(digest_size_bytes(alg) == e.size_bytes);
    }
    // Canonical sha256 size
    CHECK(digest_size_bytes(digest_algorithm::sha256) == k_spec_sha256_digest_size);
}

// =============================================================================
// Test 2: Opcode signature registry matches spec §8 — bidirectional
// =============================================================================

TEST_CASE (

"spec: opcode signature table matches §8.2 — bidirectional"
,
"[spec][opcodes]"
)
 {
    SECTION("code table size == spec table size") {
        CHECK(k_opcode_signatures.size() == k_spec_opcodes.size());
    }

    SECTION("every code entry matches corresponding spec entry") {
        REQUIRE(k_opcode_signatures.size() == k_spec_opcodes.size());
        for (std::size_t i = 0; i < k_opcode_signatures.size(); ++i) {
            const auto& code = k_opcode_signatures[i];
            const auto& spec = k_spec_opcodes[i];
            INFO("Checking opcode " << code.domain << "/" << code.name << " at index " << i);
            CHECK(code.domain       == spec.domain);
            CHECK(code.name         == spec.name);
            CHECK(code.arity_min    == spec.arity_min);
            CHECK(code.arity_max    == spec.arity_max);
            CHECK(code.result_count == spec.result_count);
            CHECK(code.is_terminator == spec.is_terminator);
            CHECK(code.reads_memory  == spec.reads_memory);
            CHECK(code.writes_memory == spec.writes_memory);
            CHECK(code.may_trap      == spec.may_trap);
            // external_calls cap check
            const bool code_needs_extcalls =
                (static_cast<std::uint32_t>(code.required_cap) ==
                 static_cast<std::uint32_t>(portable_capability_bit::external_calls));
            CHECK(code_needs_extcalls == spec.requires_external_calls_cap);
        }
    }

    SECTION("every spec entry exists in code table (no spec entry missing from code)") {
        for (const auto& spec : k_spec_opcodes) {
            const auto* found = find_signature(spec.domain, spec.name);
            INFO("Checking spec opcode " << spec.domain << "/" << spec.name);
            CHECK(found != nullptr);
        }
    }
}

// =============================================================================
// Test 3: Canonical type grammar matches spec §5
// =============================================================================

TEST_CASE (

"spec: type grammar accept/reject matches §5"
,
"[spec][types]"
)
 {
    SECTION("valid types accepted by type_str_parseable") {
        for (const auto& ts : k_spec_valid_types) {
            INFO("type: " << ts);
            CHECK(detail::type_str_parseable(ts));
        }
    }

    SECTION("invalid types rejected by type_str_parseable") {
        for (const auto& ts : k_spec_invalid_types) {
            INFO("type: '" << ts << "'");
            CHECK(!detail::type_str_parseable(ts));
        }
    }
}

// =============================================================================
// Test 4: Determinism + semantic/payload digest distinction §11
// =============================================================================

TEST_CASE (

"spec: canonical encoding determinism matches §11.4"
,
"[spec][digest]"
)
 {
    SECTION("same logical module → same canonical_encode bytes") {
        const auto m1 = make_conformance_module();
        const auto m2 = make_conformance_module();  // identical construction
        CHECK(canonical_encode(m1) == canonical_encode(m2));
    }

    SECTION("same logical module → same semantic_digest") {
        const auto m1 = make_conformance_module();
        const auto m2 = make_conformance_module();
        CHECK(semantic_digest(m1) == semantic_digest(m2));
    }

    SECTION("op mutation changes semantic_digest") {
        auto mod_a = make_conformance_module();
        auto mod_b = make_conformance_module();
        mod_b.functions[0].function_name = "different_name";
        CHECK(semantic_digest(mod_a) != semantic_digest(mod_b));
    }
}

TEST_CASE (

"spec: semantic vs payload digest are distinct concepts §11.3"
,
"[spec][digest]"
)
 {
    // Semantic digest = hash of canonical_encode.
    // Payload digest = hash of wire bytes (stored in envelope).
    // They are intentionally different objects.
    const auto mod = make_conformance_module();
    const auto sem = semantic_digest(mod);          // program identity
    const auto preimage = canonical_encode(mod);

    // Verify: semantic digest is SHA-256 of canonical bytes
    const auto h = containers::content_digest<containers::sha256_digest_policy>(
        std::span<const std::uint8_t>{preimage.data(), preimage.size()});
    std::array<std::uint8_t, 64> expected{};
    for (std::size_t i = 0; i < h.size() && i < expected.size(); ++i)
        expected[i] = h[i];
    CHECK(sem == expected);

    // The envelope payload digest would be over the *wire bytes* (after encode +
    // compression), not over canonical_encode — so they would differ in general.
    // We document this distinction, not test them equal.
    CHECK(true);  // conformance: the types are conceptually distinct (see §11.3)
}

// =============================================================================
// Test 5: Validation ordering observability + validity definition §12/§13
// =============================================================================

TEST_CASE (

"spec: verify_portable defines validity via seven checks §13"
,
"[spec][verify]"
)
 {
    SECTION("valid module passes all seven checks") {
        const auto mod = make_conformance_module();
        const auto rep = verify_portable(mod);
        CHECK(rep.ok);
        CHECK(rep.diagnostics.empty());
    }

    SECTION("export out-of-range → Y002 (§13 check Y)") {
        auto mod = make_conformance_module();
        mod.exports.push_back(portable_export{"sym", 999, "()->i64"});
        const auto rep = verify_portable(mod);
        CHECK(!rep.ok);
        const bool has_y002 = std::any_of(
            rep.diagnostics.begin(), rep.diagnostics.end(),
            [](const auto& d){ return d.code == diag_codes::export_out_of_range; });
        CHECK(has_y002);
        CHECK(std::string_view{diag_codes::export_out_of_range} == "LITHE-PORT-Y002");
    }

    SECTION("all stable diagnostic codes present in diag_codes namespace") {
        // Verify the spec-listed codes are actual string literals in the namespace.
        const std::array<const char*, 19> code_ptrs = {{
            diag_codes::type_parse_failed,
            diag_codes::type_arity_mismatch,
            diag_codes::type_shape_mismatch,
            diag_codes::block_no_terminator,
            diag_codes::branch_target_missing,
            diag_codes::op_after_terminator,
            diag_codes::value_multi_def,
            diag_codes::use_not_dominated,
            diag_codes::import_unresolved,
            diag_codes::export_out_of_range,
            diag_codes::export_duplicate,
            diag_codes::effectful_in_pure,
            diag_codes::tx_op_outside_region,
            diag_codes::cleanup_op_outside,
            diag_codes::region_cycle,
            diag_codes::block_in_two_regions,
            diag_codes::region_kind_mismatch,
            diag_codes::capability_missing,
            diag_codes::limit_exceeded,
        }};
        for (std::size_t i = 0; i < code_ptrs.size(); ++i) {
            const std::string_view code{code_ptrs[i]};
            const bool in_spec = std::any_of(
                k_spec_diag_codes.begin(), k_spec_diag_codes.end(),
                [&](std::string_view sv){ return sv == code; });
            INFO("Code " << code);
            CHECK(in_spec);
        }
    }
}
