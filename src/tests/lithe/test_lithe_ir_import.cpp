// =============================================================================
// test_lithe_ir_import.cpp — IR import→compile contract + nested error (§8.2)
//
// Verifies (using fake/stub provider — real codecs land in impl-5):
//   • import_text_ir / import_binary_ir return imported_ir<IR> with all four
//     fields populated: value, resolution, diagnostics, source_format.
//   • target_address_width == 0 in format_descriptor → ir_error before any bytes read.
//   • Decode failure → ir_error returned.
//   • validate_ir fires after decode; ir_resolution_state propagated.
//   • unresolved_required_operations → ir_resolution_error via resolution gate.
//   • contains_opaque_optional_operations → accepted by gate (opaque-optional allowed).
//   • compile_text/compile_binary: full contract (decode→validate→gate→compile_best).
//   • ir_compile_error discriminates stage: ir_error / ir_resolution_error /
//     ir_stage_engine_compile_error; helpers is_decode_error etc. work.
//   • compile_best (engine core) returns engine_compile_error — NO ir_error alternative.
//   • Separate functions per encoding (text vs binary tested independently).
// =============================================================================

#include "catch_amalgamated.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

// IR integration layer
#include "lithe/lithe_ir/integration.hpp"
#include "lithe/lithe_ir/format.hpp"
#include "lithe/lithe_ir/provider.hpp"

// Engine + interpreter backend (for compile_text round-trip).
#include "lithe/lithe_engine.hpp"
#include "lithe/backends/lithe_codegen_interpreter_facet.hpp"
#include "lithe/backends/lithe_execution_backends.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"

namespace irns = lithe::ir;
namespace ex = lithe::execution;
namespace cg = lithe::codegen;
namespace mir = lithe::codegen::mir;

// ============================================================================
// Minimal fake IR type
// ============================================================================

namespace {
    using fake_ir_t = mir::physical_mir_function;

    mir::physical_mir_function make_add_fn() {
        cg::allocated_function_ir fn;
        fn.name = "ir_import_test_add";
        fn.cfg.entry_block = 1;

        cg::allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";

        cg::allocated_instruction load0;
        load0.id = 1;
        load0.op = cg::opcode::load_arg;
        load0.defs = {cg::allocated_operand::as_preg({0, "r0"})};
        load0.uses = {cg::allocated_operand::as_argument_index(0)};
        bb.instructions.push_back(load0);

        cg::allocated_instruction load1;
        load1.id = 2;
        load1.op = cg::opcode::load_arg;
        load1.defs = {cg::allocated_operand::as_preg({1, "r1"})};
        load1.uses = {cg::allocated_operand::as_argument_index(1)};
        bb.instructions.push_back(load1);

        cg::allocated_instruction add;
        add.id = 3;
        add.op = cg::opcode::add;
        add.defs = {cg::allocated_operand::as_preg({2, "r2"})};
        add.uses = {
            cg::allocated_operand::as_preg({0, "r0"}),
            cg::allocated_operand::as_preg({1, "r1"})
        };
        bb.instructions.push_back(add);

        cg::allocated_instruction ret;
        ret.id = 4;
        ret.op = cg::opcode::ret;
        ret.uses = {cg::allocated_operand::as_preg({2, "r2"})};
        bb.instructions.push_back(ret);

        fn.blocks.push_back(std::move(bb));

        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }

    // Fake text bytes (provider ignores the actual content).
    constexpr char kFakeTextBytes[] = "fake_text_ir_bytes";
    constexpr std::uint8_t kFakeBinaryBytes[] = {0x42, 0x43, 0x44, 0x45};

    irns::format_descriptor valid_text_fmt() {
        return irns::format_descriptor{
            irns::encoding::text_utf8,
            irns::stage::physical,
            {1, 0, 0},
            64,
            ex::ir_kind::physical_mir
        };
    }

    irns::format_descriptor valid_binary_fmt() {
        return irns::format_descriptor{
            irns::encoding::binary_le,
            irns::stage::physical,
            {1, 0, 0},
            64,
            ex::ir_kind::physical_mir
        };
    }

    // =========================================================================
    // fake_text_provider
    // =========================================================================

    struct fake_text_provider {
        bool fail_decode = false;
        irns::ir_resolution_state resolution_override
            = irns::ir_resolution_state::resolved;

        friend std::expected<fake_ir_t, ex::ir_error>
        tag_invoke(irns::cpo::import_text_t,
                   fake_text_provider& p,
                   irns::text_ir_view /*view*/) {
            if (p.fail_decode)
                return std::unexpected(ex::ir_error{"fake_text_provider: decode failed"});
            return make_add_fn();
        }

        friend irns::ir_resolution_state
        tag_invoke(irns::cpo::validate_ir_t,
                   const fake_text_provider& p,
                   const fake_ir_t& /*ir*/) {
            return p.resolution_override;
        }
    };

    static_assert(irns::text_importer_for<fake_text_provider, fake_ir_t>);
    static_assert(irns::ir_validator_for<fake_text_provider, fake_ir_t>);

    // =========================================================================
    // fake_binary_provider
    // =========================================================================

    struct fake_binary_provider {
        bool fail_decode = false;
        irns::ir_resolution_state resolution_override
            = irns::ir_resolution_state::resolved;

        friend std::expected<fake_ir_t, ex::ir_error>
        tag_invoke(irns::cpo::import_binary_t,
                   fake_binary_provider& p,
                   irns::binary_ir_view /*view*/) {
            if (p.fail_decode)
                return std::unexpected(ex::ir_error{"fake_binary_provider: decode failed"});
            return make_add_fn();
        }

        friend irns::ir_resolution_state
        tag_invoke(irns::cpo::validate_ir_t,
                   const fake_binary_provider& p,
                   const fake_ir_t& /*ir*/) {
            return p.resolution_override;
        }
    };

    static_assert(irns::binary_importer_for<fake_binary_provider, fake_ir_t>);
    static_assert(irns::ir_validator_for<fake_binary_provider, fake_ir_t>);

    // =========================================================================
    // Engine setup
    // =========================================================================

    using test_sig_t = std::int64_t(


    std::int64_t
    ,
    std::int64_t
    );
    using test_back_t = cg::backends::interpreter_backend;

    auto make_engine() {
        test_back_t backend;
        auto bset = std::tuple<test_back_t>(std::move(backend));
        return lithe::basic_lithe_engine<decltype(bset)>(std::move(bset));
    }
} // namespace

// ============================================================================
// Compile-time: ir_compile_error stages are distinct
// ============================================================================

static_assert(!std::is_same_v<irns::ir_resolution_error, ex::ir_error>);
static_assert(!std::is_same_v<irns::ir_stage_engine_compile_error, ex::ir_error>);
static_assert(!std::is_same_v<irns::ir_stage_engine_compile_error, irns::ir_resolution_error>);

// ============================================================================
// §8.2 import_text_ir — happy path
// ============================================================================

TEST_CASE (


"import_text_ir: success populates all four fields"
,
"[ir][import][text]"
)
{
    fake_text_provider prov;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        valid_text_fmt()};

    auto result = irns::import_text_ir<fake_ir_t>(prov, view);
    REQUIRE(result.has_value());

    CHECK(!result->value.function.name.empty());
    CHECK(result->resolution == irns::ir_resolution_state::resolved);
    CHECK(result->is_resolved());
    CHECK(result->source_format.wire_encoding == irns::encoding::text_utf8);
    CHECK(result->source_format.target_address_width == 64);
}

// ============================================================================
// §8.2 target_address_width == 0 rejected before decode
// ============================================================================

TEST_CASE (


"import_text_ir: target_address_width==0 → ir_error before decode"
,
"[ir][import][text][format]"
)
{
    fake_text_provider prov;
    irns::format_descriptor bad = valid_text_fmt();
    bad.target_address_width = 0;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        bad};

    auto result = irns::import_text_ir<fake_ir_t>(prov, view);
    CHECK(!result.has_value());
}

// ============================================================================
// §8.2 decode failure propagates ir_error
// ============================================================================

TEST_CASE (


"import_text_ir: decode failure → ir_error"
,
"[ir][import][text]"
)
{
    fake_text_provider prov;
    prov.fail_decode = true;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        valid_text_fmt()};

    auto result = irns::import_text_ir<fake_ir_t>(prov, view);
    CHECK(!result.has_value());
}

// ============================================================================
// §8.2 opaque-optional resolution accepted (not rejected)
// ============================================================================

TEST_CASE (


"import_text_ir: opaque-optional resolution accepted"
,
"[ir][import][text][resolution]"
)
{
    fake_text_provider prov;
    prov.resolution_override = irns::ir_resolution_state::contains_opaque_optional_operations;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        valid_text_fmt()};

    auto result = irns::import_text_ir<fake_ir_t>(prov, view);
    REQUIRE(result.has_value());
    CHECK(result->has_opaque_optional());
    CHECK(!result->is_resolved());
}

// ============================================================================
// §8.2 import_binary_ir — happy path
// ============================================================================

TEST_CASE (


"import_binary_ir: success populates all four fields"
,
"[ir][import][binary]"
)
{
    fake_binary_provider prov;
    irns::binary_ir_view view{
        std::span<const std::uint8_t>{kFakeBinaryBytes, sizeof(kFakeBinaryBytes)},
        valid_binary_fmt()};

    auto result = irns::import_binary_ir<fake_ir_t>(prov, view);
    REQUIRE(result.has_value());
    CHECK(result->source_format.wire_encoding == irns::encoding::binary_le);
    CHECK(result->resolution == irns::ir_resolution_state::resolved);
}

// ============================================================================
// §8.2 resolution gate
// ============================================================================

TEST_CASE (


"check_resolution_gate: unresolved_required_operations → error"
,
"[ir][resolution]"
)
{
    auto err = irns::check_resolution_gate(
        irns::ir_resolution_state::unresolved_required_operations);
    REQUIRE(err.has_value());
    CHECK(err->state == irns::ir_resolution_state::unresolved_required_operations);
}

TEST_CASE (


"check_resolution_gate: resolved → nullopt"
,
"[ir][resolution]"
)
{
    CHECK(!irns::check_resolution_gate(irns::ir_resolution_state::resolved).has_value());
}

TEST_CASE (


"check_resolution_gate: contains_opaque_optional → passes gate [G3]"
,
"[ir][resolution]"
)
{
    // contains_opaque_optional_operations is allowed by the gate: optional sections
    // are preserved/skipped at compile time; only unresolved_required_operations is refused.
    auto e = irns::check_resolution_gate(
        irns::ir_resolution_state::contains_opaque_optional_operations);
    CHECK(!e.has_value()); // gate passes for opaque-optional
}

// ============================================================================
// §8.2 ir_compile_error — discriminant helpers
// ============================================================================

TEST_CASE (


"ir_compile_error: discriminant helpers work correctly"
,
"[ir][compile_error]"
)
{
    irns::ir_compile_error decode_err{ex::ir_error{"decode fail"}};
    CHECK(irns::is_decode_error(decode_err));
    CHECK(!irns::is_resolution_error(decode_err));
    CHECK(!irns::is_engine_error(decode_err));

    irns::ir_compile_error res_err{irns::ir_resolution_error{
        irns::ir_resolution_state::unresolved_required_operations, "gate blocked"}};
    CHECK(!irns::is_decode_error(res_err));
    CHECK(irns::is_resolution_error(res_err));
    CHECK(!irns::is_engine_error(res_err));

    irns::ir_compile_error eng_err{irns::ir_stage_engine_compile_error{"backend rejected"}};
    CHECK(!irns::is_decode_error(eng_err));
    CHECK(!irns::is_resolution_error(eng_err));
    CHECK(irns::is_engine_error(eng_err));
}

// ============================================================================
// §8.2 compile_text — full contract
// ============================================================================

TEST_CASE (


"compile_text: happy path → entry compiles"
,
"[ir][compile_text]"
)
{
    auto engine = make_engine();
    fake_text_provider prov;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        valid_text_fmt()};

    auto result = irns::compile_text<fake_ir_t, test_sig_t>(engine, prov, view);
    REQUIRE(result.has_value());
    std::visit([](auto& se) { CHECK(se.valid()); }, *result);
}

TEST_CASE (


"compile_text: invalid format → ir_compile_error (decode stage)"
,
"[ir][compile_text][format]"
)
{
    auto engine = make_engine();
    fake_text_provider prov;
    irns::format_descriptor bad = valid_text_fmt();
    bad.target_address_width = 0;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        bad};

    auto result = irns::compile_text<fake_ir_t, test_sig_t>(engine, prov, view);
    REQUIRE(!result.has_value());
    CHECK(irns::is_decode_error(result.error()));
}

TEST_CASE (


"compile_text: decode failure → ir_compile_error (decode stage)"
,
"[ir][compile_text]"
)
{
    auto engine = make_engine();
    fake_text_provider prov;
    prov.fail_decode = true;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        valid_text_fmt()};

    auto result = irns::compile_text<fake_ir_t, test_sig_t>(engine, prov, view);
    REQUIRE(!result.has_value());
    CHECK(irns::is_decode_error(result.error()));
}

TEST_CASE (


"compile_text: unresolved ops → ir_compile_error (resolution stage)"
,
"[ir][compile_text][resolution]"
)
{
    auto engine = make_engine();
    fake_text_provider prov;
    prov.resolution_override = irns::ir_resolution_state::unresolved_required_operations;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        valid_text_fmt()};

    auto result = irns::compile_text<fake_ir_t, test_sig_t>(engine, prov, view);
    REQUIRE(!result.has_value());
    CHECK(irns::is_resolution_error(result.error()));
}

TEST_CASE (


"compile_text: opaque-optional refused (§10a.11) [G3]"
,
"[ir][compile_text][resolution]"
)
{
    auto engine = make_engine();
    fake_text_provider prov;
    prov.resolution_override = irns::ir_resolution_state::contains_opaque_optional_operations;
    irns::text_ir_view view{
        std::span<const char>{kFakeTextBytes, sizeof(kFakeTextBytes)-1},
        valid_text_fmt()};

    // Import still surfaces opaque-optional (printable/storable/forwardable).
    auto imported = irns::import_text_ir<fake_ir_t>(prov, view);
    REQUIRE(imported.has_value());
    CHECK(imported->has_opaque_optional());

    // But compile refuses it at the resolution gate.
    auto result = irns::compile_text<fake_ir_t, test_sig_t>(engine, prov, view);
    REQUIRE(!result.has_value());
    CHECK(irns::is_resolution_error(result.error()));
}

// ============================================================================
// §8.2 compile_binary — full contract
// ============================================================================

TEST_CASE (


"compile_binary: happy path → entry compiles"
,
"[ir][compile_binary]"
)
{
    auto engine = make_engine();
    fake_binary_provider prov;
    irns::binary_ir_view view{
        std::span<const std::uint8_t>{kFakeBinaryBytes, sizeof(kFakeBinaryBytes)},
        valid_binary_fmt()};

    auto result = irns::compile_binary<fake_ir_t, test_sig_t>(engine, prov, view);
    REQUIRE(result.has_value());
    std::visit([](auto& se) { CHECK(se.valid()); }, *result);
}

TEST_CASE (


"compile_binary: unresolved ops → ir_resolution_error"
,
"[ir][compile_binary][resolution]"
)
{
    auto engine = make_engine();
    fake_binary_provider prov;
    prov.resolution_override = irns::ir_resolution_state::unresolved_required_operations;
    irns::binary_ir_view view{
        std::span<const std::uint8_t>{kFakeBinaryBytes, sizeof(kFakeBinaryBytes)},
        valid_binary_fmt()};

    auto result = irns::compile_binary<fake_ir_t, test_sig_t>(engine, prov, view);
    REQUIRE(!result.has_value());
    CHECK(irns::is_resolution_error(result.error()));
}

// ============================================================================
// §8.2 Engine core: compile_best error type must NOT be ir_error
// ============================================================================

TEST_CASE (


"engine compile_best: error type is engine_compile_error (no ir_error)"
,
"[ir][engine][error_type]"
)
{
    static_assert(!std::is_same_v<lithe::engine_compile_error, ex::ir_error>,
        "engine_compile_error must not be ir_error");

    using bset_t   = std::tuple<test_back_t>;
    using engine_t = lithe::basic_lithe_engine<bset_t>;
    using best_t   = decltype(std::declval<engine_t>()
                                   .template compile_best<test_sig_t>(
                                       std::declval<fake_ir_t>()));
    using error_t  = typename best_t::error_type;

    static_assert(std::is_same_v<error_t, lithe::engine_compile_error>,
        "compile_best error must be engine_compile_error");
    static_assert(!std::is_same_v<error_t, ex::ir_error>,
        "compile_best must not return ir_error");

    SUCCEED("compile_best error type contract verified at compile time");
}

// ============================================================================
// [G3] resolution-gate coverage (impl-1): opaque-optional REFUSED before compile
// ============================================================================

TEST_CASE (


"check_resolution_gate: resolved → nullopt [G3]"
,
"[ir][resolution]"
)
{
    CHECK(!irns::check_resolution_gate(
        irns::ir_resolution_state::resolved).has_value());
}

TEST_CASE (


"check_resolution_gate: unresolved_required → ir_resolution_error [G3]"
,
"[ir][resolution]"
)
{
    auto e = irns::check_resolution_gate(
        irns::ir_resolution_state::unresolved_required_operations);
    REQUIRE(e.has_value());
    CHECK(e->state == irns::ir_resolution_state::unresolved_required_operations);
}
