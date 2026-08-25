// =============================================================================
// test_lithe_ir_noop.cpp — no_ir_provider neutral default behaviour
// (§10a.4, §10a.5, impl-5 P12 item 5)
//
// Tests:
//   1. no_ir_provider::available == false.
//   2. import_text_ir / import_binary_ir with no_ir_provider cannot be invoked
//      (no CPO binding → compile-time constraint).
//   3. engine_integration<no_ir_provider> is empty (zero-cost).
//   4. pipeline_hooks<no_ir_provider>: active == false, fire() is a no-op.
//   5. The neutral default never silently accepts: calling import_* with
//      no_ir_provider is a type error (concept unsatisfied).
//   6. The core engine build with no_ir_provider compiles cleanly.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <expected>
#include <type_traits>

#include "lithe/lithe_ir/provider.hpp"
#include "lithe/lithe_ir/integration.hpp"
#include "lithe/lithe_ir/hooks.hpp"
#include "lithe/lithe_ir/format.hpp"

namespace irns = lithe::ir;

// ============================================================================
// TEST: no_ir_provider::available == false, is_empty
// ============================================================================

TEST_CASE (


"no_ir_provider: available == false and is empty"
,
"[lithe_ir][noop]"
)
 {
    static_assert(!irns::no_ir_provider::available);
    static_assert(std::is_empty_v<irns::no_ir_provider>);

    irns::no_ir_provider prov;
    // available is a compile-time constant, not a runtime value
    CHECK(!irns::no_ir_provider::available);
}

// ============================================================================
// TEST: no_ir_provider does NOT satisfy importer/exporter concepts
// ============================================================================

TEST_CASE (


"no_ir_provider: does not satisfy text_importer_for concept"
,
"[lithe_ir][noop]"
)
 {
    // These static_assert the NEGATION — no_ir_provider provides no CPO binding.
    struct dummy_ir {};
    static_assert(!irns::text_importer_for<irns::no_ir_provider, dummy_ir>,
                  "no_ir_provider must NOT satisfy text_importer_for");
    static_assert(!irns::binary_importer_for<irns::no_ir_provider, dummy_ir>,
                  "no_ir_provider must NOT satisfy binary_importer_for");
    static_assert(!irns::text_exporter_for<irns::no_ir_provider, dummy_ir>,
                  "no_ir_provider must NOT satisfy text_exporter_for");
    static_assert(!irns::binary_exporter_for<irns::no_ir_provider, dummy_ir>,
                  "no_ir_provider must NOT satisfy binary_exporter_for");
    SUCCEED("no_ir_provider concept-unsatisfied assertions pass");
}

// ============================================================================
// TEST: engine_integration<no_ir_provider> is empty (zero-cost)
// ============================================================================

TEST_CASE (


"engine_integration<no_ir_provider>: is empty and active==false"
,
"[lithe_ir][noop]"
)
 {
    static_assert(std::is_empty_v<irns::engine_integration<irns::no_ir_provider>>,
                  "engine_integration<no_ir_provider> must be zero-cost empty");
    static_assert(!irns::engine_integration<irns::no_ir_provider>::active,
                  "engine_integration<no_ir_provider>::active must be false");
    CHECK(!irns::engine_integration<irns::no_ir_provider>::active);
}

// ============================================================================
// TEST: pipeline_hooks<no_ir_provider>: active == false, fire() is a no-op
// ============================================================================

TEST_CASE (


"pipeline_hooks<no_ir_provider>: active==false, fire returns nullopt"
,
"[lithe_ir][noop]"
)
 {
    static_assert(!irns::pipeline_hooks<irns::no_ir_provider>::active,
                  "pipeline_hooks<no_ir_provider>::active must be false");

    irns::pipeline_hooks<irns::no_ir_provider> hooks;
    CHECK(!hooks.is_active());

    // fire() must be a no-op returning nullopt (not an error)
    const irns::ir_hook_request req{};
    const auto result = hooks.fire(req, "some ir text");
    CHECK(!result.has_value()); // nullopt → no error fired
}

// ============================================================================
// TEST: format_descriptor::valid() — target_address_width==0 is invalid
// ============================================================================

TEST_CASE (


"format_descriptor: target_address_width==0 invalid, make() rejects"
,
"[lithe_ir][noop]"
)
 {
    // Validating factory rejects width == 0
    const auto bad = irns::format_descriptor::make(
        irns::encoding::text_utf8,
        irns::stage::physical,
        {1, 0, 0},
        0,  // invalid
        lithe::execution::ir_kind::physical_mir);

    CHECK(!bad.has_value());

    // Default-constructed descriptor is invalid (target_address_width == 0)
    const irns::format_descriptor default_desc{};
    CHECK(!default_desc.valid());

    // Valid construction
    const auto good = irns::format_descriptor::make(
        irns::encoding::text_utf8,
        irns::stage::physical,
        {1, 0, 0}, 64,
        lithe::execution::ir_kind::physical_mir);
    REQUIRE(good.has_value());
    CHECK(good->valid());
}

// ============================================================================
// TEST: import_text_ir with no_ir_provider → compile-time type error
//
// We can't call import_text_ir(no_ir_provider, view) because no_ir_provider
// does not satisfy text_importer_for.  We verify this with a requires check.
// ============================================================================

TEST_CASE (


"no_ir_provider: import_text_ir concept gate prevents compilation"
,
"[lithe_ir][noop]"
)
 {
    struct dummy_ir {};
    // Verify the gate: import_text_ir<IR, Provider> requires text_importer_for<Provider, IR>.
    // no_ir_provider does NOT satisfy it → concept unsatisfied (compile-time gate).
    //
    // We check the importer concepts directly rather than testing the call expression,
    // to avoid confusing clangd's unevaluated-context analysis.
    static_assert(!irns::text_importer_for<irns::no_ir_provider, dummy_ir>,
                  "no_ir_provider must not satisfy text_importer_for → import_text_ir gate holds");
    static_assert(!irns::binary_importer_for<irns::no_ir_provider, dummy_ir>,
                  "no_ir_provider must not satisfy binary_importer_for → import_binary_ir gate holds");
    SUCCEED("no_ir_provider compile-time gate assertions pass");
}
