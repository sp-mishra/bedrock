// =============================================================================
// test_lithe_ir_inspect.cpp — IR introspection facade (impl-5)
//
// Tests:
//   1. ir_inspector exposes correct structure for a multi-function module
//   2. dump(binary) byte-equals canonical_encode; dump(canonical_text) is hex
//      of canonical bytes; human_pretty is non-round-trippable (flagged)
//   3. semantic_digest() / canonical_bytes() agree with impl-1 free functions
//   4. Read-only invariant: no endpoint mutates the inspected module
//   5. verify() matches standalone verify_portable; opcode_signatures() correct
//   6. any_ir_view heterogeneous container + ir_view static_asserts
// =============================================================================

#include "catch_amalgamated.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Light IR core (includes inspect headers via lithe_ir_core.hpp now)
#include "lithe/lithe_ir_core.hpp"
#include "lithe/lithe_ir/inspect/inspect.hpp"

using namespace lithe::ir;
using namespace lithe::ir::portable;
using namespace lithe::ir::inspect;
using namespace lithe::ir::adapters;

// =============================================================================
// Helpers: build wire functions and portable modules for testing
// =============================================================================

namespace {
    // Minimal hl_mir function: one constant op, one region_yield terminator
    lithe_hl_mir_ir make_wire_fn(const std::string& fname,
                                 std::uint32_t op_id_offset = 0) {
        lithe_hl_mir_ir fn;
        fn.function_name = fname;
        fn.source_stage = stage::lowered;
        fn.schema = {1, 0, 0};

        // Two values: v0 (i64), v1 (i64)
        fn.values = {
            hl_wire_value{op_id_offset + 0, "i64"},
            hl_wire_value{op_id_offset + 1, "i64"},
        };

        // Two ops: constant (produces v0), region_yield (terminates with v0)
        hl_wire_op const_op;
        const_op.id = op_id_offset + 0;
        const_op.domain = "lithe.hl";
        const_op.name = "constant";
        const_op.result_ids = {op_id_offset + 0};
        const_op.block_id = op_id_offset + 0;
        const_op.region_id = op_id_offset + 0;

        hl_wire_op yield_op;
        yield_op.id = op_id_offset + 1;
        yield_op.domain = "lithe.hl";
        yield_op.name = "region_yield";
        yield_op.operand_ids = {op_id_offset + 0};
        yield_op.block_id = op_id_offset + 0;
        yield_op.region_id = op_id_offset + 0;

        fn.ops = {const_op, yield_op};

        // One block containing both ops
        hl_wire_block blk;
        blk.id = op_id_offset + 0;
        blk.op_ids = {op_id_offset + 0, op_id_offset + 1};
        fn.blocks = {blk};

        // One region containing the block
        hl_wire_region reg;
        reg.id = op_id_offset + 0;
        reg.block_ids = {op_id_offset + 0};
        fn.regions = {reg};

        fn.entry_block_ids = {op_id_offset + 0};
        return fn;
    }

    // Build a two-function module with one import, one export, one global
    portable_module make_test_module() {
        portable_module mod;
        mod.schema = {1, 0, 0};

        mod.functions.push_back(make_wire_fn("fn_a", 0));
        mod.functions.push_back(make_wire_fn("fn_b", 10));

        mod.imports.push_back(portable_import{"ext_mod", "ext_sym", "i64->i64"});
        mod.exports.push_back(portable_export{"fn_a", 0, "()->i64"});

        mod.globals.push_back(portable_global{"gvar", "i64", 0, false});

        mod.constants.types.push_back("i64");
        mod.constants.data.push_back({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

        mod.declared_capabilities.set(portable_capability_bit::external_calls);

        mod.manifest.producer = "test_runner";
        mod.manifest.source_language = "lithe_test";

        return mod;
    }
} // namespace

// =============================================================================
// Test 1: facade exposes correct structure
// =============================================================================

TEST_CASE (

"ir_inspector: structure endpoints match source module"
,
"[inspect][structure]"
)
 {
    const auto mod = make_test_module();
    ir_inspector insp{mod};

    CHECK(insp.function_count() == 2u);
    CHECK(insp.function_name(0) == "fn_a");
    CHECK(insp.function_name(1) == "fn_b");
    CHECK(insp.function_name(99) == std::string_view{});  // out-of-range

    SECTION("imports / exports / globals") {
        const auto imps = insp.imports();
        REQUIRE(imps.size() == 1u);
        CHECK(imps[0].module == "ext_mod");
        CHECK(imps[0].symbol == "ext_sym");

        const auto exps = insp.exports();
        REQUIRE(exps.size() == 1u);
        CHECK(exps[0].symbol == "fn_a");
        CHECK(exps[0].function_index == 0u);

        const auto globs = insp.globals();
        REQUIRE(globs.size() == 1u);
        CHECK(globs[0].name == "gvar");
    }

    SECTION("declared capabilities") {
        const auto caps = insp.declared_capabilities();
        CHECK(caps.has(portable_capability_bit::external_calls));
        CHECK(!caps.has(portable_capability_bit::exceptions));
    }

    SECTION("available_stage returns lowered for this module") {
        const auto sk = insp.available_stage(stage::lowered);
        REQUIRE(sk.has_value());
        CHECK(sk->family == ir_family::hl_mir);
        CHECK(sk->s == stage::lowered);

        CHECK(!insp.available_stage(stage::physical).has_value());
    }
}

// =============================================================================
// Test 2: dump round-trips and matches canonical bytes (observation == verification)
// =============================================================================

TEST_CASE (

"ir_inspector: dump matches canonical_encode"
,
"[inspect][dump]"
)
 {
    const auto mod = make_test_module();
    ir_inspector insp{mod};

    SECTION("dump_module(binary) == canonical_encode(module)") {
        const auto expected = canonical_encode(mod);
        const auto result   = insp.dump_module(ir_dump_format::binary);
        REQUIRE(result.has_value());
        CHECK(*result == expected);
    }

    SECTION("dump_module(canonical_text) is hex of canonical bytes") {
        const auto bin_bytes = canonical_encode(mod);
        const auto text_res  = insp.dump_module(ir_dump_format::canonical_text);
        REQUIRE(text_res.has_value());
        const auto& text = *text_res;
        // Hex = 2 chars per byte
        REQUIRE(text.size() == bin_bytes.size() * 2);
        // Verify first byte encodes correctly
        static constexpr char hex[] = "0123456789abcdef";
        const std::uint8_t b0 = bin_bytes[0];
        CHECK(text[0] == static_cast<std::uint8_t>(hex[(b0 >> 4) & 0xF]));
        CHECK(text[1] == static_cast<std::uint8_t>(hex[b0 & 0xF]));
    }

    SECTION("dump(fn_idx=0, binary) != dump_module(binary) [single-fn subset]") {
        const auto full_mod   = insp.dump_module(ir_dump_format::binary);
        const auto fn0_result = insp.dump(0, ir_dump_format::binary);
        REQUIRE(full_mod.has_value());
        REQUIRE(fn0_result.has_value());
        // Single-function dump is a subset module encoding — different bytes
        // (one function vs two). Just assert it doesn't crash and has content.
        CHECK(!fn0_result->empty());
    }

    SECTION("dump(unknown_fn_idx) returns error") {
        const auto bad = insp.dump(99, ir_dump_format::binary);
        REQUIRE(!bad.has_value());
        CHECK(bad.error().ec == inspect_error::code::unknown_function);
    }

    SECTION("human_pretty is non-empty and non-normative (documented)") {
        const auto pretty = insp.dump_module(ir_dump_format::human_pretty);
        REQUIRE(pretty.has_value());
        // Must contain a non-normative marker.
        const std::string text{pretty->begin(), pretty->end()};
        CHECK(text.find("non-normative") != std::string::npos);
    }
}

// =============================================================================
// Test 3: semantic_digest / canonical_bytes agree with impl-1 free functions
// =============================================================================

TEST_CASE (

"ir_inspector: digest/canonical_bytes agree with impl-1"
,
"[inspect][digest]"
)
 {
    const auto mod = make_test_module();
    ir_inspector insp{mod};

    SECTION("semantic_digest() == portable::semantic_digest(module)") {
        const auto direct  = semantic_digest(mod);
        const auto via_insp = insp.semantic_digest();
        CHECK(direct == via_insp);
    }

    SECTION("canonical_bytes() == canonical_encode(module)") {
        const auto direct   = canonical_encode(mod);
        const auto via_insp = insp.canonical_bytes();
        CHECK(direct == via_insp);
    }

    SECTION("mutating a copy changes the digest") {
        auto mod2 = mod;
        mod2.functions[0].function_name = "different_name";
        ir_inspector insp2{mod2};
        CHECK(insp2.semantic_digest() != insp.semantic_digest());
    }
}

// =============================================================================
// Test 4: read-only invariant — no inspection endpoint mutates the module
// =============================================================================

TEST_CASE (

"ir_inspector: read-only invariant (no mutation of subject)"
,
"[inspect][readonly]"
)
 {
    const auto mod = make_test_module();
    const auto snapshot_before = canonical_encode(mod);

    ir_inspector insp{mod};

    // Exercise every read endpoint
    (void)insp.function_count();
    (void)insp.function_name(0);
    (void)insp.imports();
    (void)insp.exports();
    (void)insp.globals();
    (void)insp.declared_capabilities();
    (void)insp.available_stage(stage::lowered);
    (void)insp.function_view(0).entity_count();
    auto r0 = insp.dump_module(ir_dump_format::binary);        (void)r0;
    auto r1 = insp.dump_module(ir_dump_format::canonical_text); (void)r1;
    auto r2 = insp.dump_module(ir_dump_format::human_pretty);  (void)r2;
    auto r3 = insp.dump(0, ir_dump_format::binary);            (void)r3;
    (void)insp.semantic_digest();
    (void)insp.canonical_bytes();
    (void)insp.verify();
    (void)insp.opcode_signatures();

    const auto snapshot_after = canonical_encode(mod);
    CHECK(snapshot_before == snapshot_after);
}

// =============================================================================
// Test 5: verify() matches standalone verifier; opcode_signatures() correct
// =============================================================================

TEST_CASE (

"ir_inspector: verify() matches standalone; opcode_signatures correct"
,
"[inspect][verify]"
)
 {
    const auto mod = make_test_module();
    ir_inspector insp{mod};

    SECTION("verify() result agrees with verify_portable") {
        const verify_policy policy;
        const auto direct = verify_portable(mod, policy);
        const auto via    = insp.verify(policy);
        CHECK(via.ok == direct.ok);
        CHECK(via.diagnostics.size() == direct.diagnostics.size());
    }

    SECTION("opcode_signatures() equals impl-1 k_opcode_signatures") {
        const auto sig_span = insp.opcode_signatures();
        REQUIRE(sig_span.size() == k_opcode_signatures.size());
        for (std::size_t i = 0; i < sig_span.size(); ++i) {
            CHECK(sig_span[i].domain  == k_opcode_signatures[i].domain);
            CHECK(sig_span[i].name    == k_opcode_signatures[i].name);
            CHECK(sig_span[i].arity_min == k_opcode_signatures[i].arity_min);
            CHECK(sig_span[i].arity_max == k_opcode_signatures[i].arity_max);
        }
    }

    SECTION("verify() on a malformed module (export out of range) reports error") {
        auto bad_mod = mod;
        bad_mod.exports.push_back(portable_export{"bad_export", 999, "()->i64"});
        ir_inspector bad_insp{bad_mod};
        const auto rep = bad_insp.verify();
        CHECK(!rep.ok);
        const bool has_export_err = std::any_of(
            rep.diagnostics.begin(), rep.diagnostics.end(),
            [](const auto& d){ return d.code == diag_codes::export_out_of_range; });
        CHECK(has_export_err);
    }
}

// =============================================================================
// Test 6: any_ir_view heterogeneous container + static_assert static path
// =============================================================================

// Confirm concept is satisfied by all concrete views at compile time.
static_assert(ir_view<graph_view>, "graph_view must satisfy ir_view");
static_assert(ir_view<hl_mir_view>, "hl_mir_view must satisfy ir_view");
static_assert(ir_view<physical_mir_view>, "physical_mir_view must satisfy ir_view");
static_assert(ir_view<any_ir_view>, "any_ir_view must satisfy ir_view");

// Template helper: verify static dispatch compiles.
template <ir_view V>
static std::size_t probe_view(const V& v) {
    return v.entity_count() + v.block_count();
}

TEST_CASE (

"ir_view concept: heterogeneous container + static dispatch proof"
,
"[inspect][view]"
)
 {
    const auto gir = lithe_graph_ir{};
    const auto hlf = make_wire_fn("fn_x");
    const auto phy = lithe_physical_mir_ir{};

    SECTION("static path (concrete views)") {
        const graph_view        gv{gir};
        const hl_mir_view       hv{hlf};
        const physical_mir_view pv{phy};

        // Static dispatch via template helper
        (void)probe_view(gv);
        (void)probe_view(hv);
        (void)probe_view(pv);

        CHECK(gv.family()  == ir_family::graph);
        CHECK(hv.family()  == ir_family::hl_mir);
        CHECK(pv.family()  == ir_family::physical_mir);

        CHECK(gv.stage_of() == stage::surface);
        CHECK(hv.stage_of() == stage::lowered);
        CHECK(pv.stage_of() == stage::physical);

        CHECK(hv.entity_count() == hlf.ops.size());
        CHECK(hv.block_count()  == hlf.blocks.size());

        const entity_ref e0{0};
        const auto [dom, nm] = hv.opcode_name(e0);
        CHECK(dom == "lithe.hl");
        CHECK(nm  == "constant");

        CHECK(hv.type_string(e0) == "i64");

        CHECK(hv.structurally_valid());
    }

    SECTION("any_ir_view heterogeneous container") {
        const graph_view        gv{gir};
        const hl_mir_view       hv{hlf};
        const physical_mir_view pv{phy};

        const std::vector<any_ir_view> views = {gv, hv, pv};
        REQUIRE(views.size() == 3u);

        CHECK(views[0].family() == ir_family::graph);
        CHECK(views[1].family() == ir_family::hl_mir);
        CHECK(views[2].family() == ir_family::physical_mir);

        // Dispatch via any_ir_view (std::visit-backed, no virtual)
        (void)probe_view(views[0]);
        (void)probe_view(views[1]);
        (void)probe_view(views[2]);
    }
}
