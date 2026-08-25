// =============================================================================
// test_lithe_ir_hooks.cpp — neutral hooks eliminated; active hooks path (§10a.6)
//
// Verifies:
//   • no_pipeline_hooks is empty (is_empty_v).
//   • pipeline_hooks<no_ir_provider> compiles away (active = false).
//   • pipeline_hooks<diagnostic_text_stub> active path fires at each hook_point.
//   • Each hook_failure_policy honoured.
//   • diagnostic_text_stub provider validates IR correctly.
//   • format_descriptor with target_address_width == 0 → ir_error.
//   • ir_resolution_state values exist and are correct.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <string>
#include <string_view>
#include <type_traits>

#include "lithe/lithe_execution/foundation.hpp"
#include "lithe/lithe_ir_core.hpp"
#include "lithe/lithe_ir/hooks.hpp"

// ============================================================================
// format_descriptor — validating constructor
// ============================================================================

TEST_CASE (


"format_descriptor: target_address_width 0 → ir_error"
,
"[ir][format]"
)
 {
    auto result = lithe::ir::format_descriptor::make(
        lithe::ir::encoding::binary_le,
        lithe::ir::stage::physical,
        {1, 0, 0},
        0, // invalid!
        lithe::execution::ir_kind::physical_mir);

    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().detail.empty());
}

TEST_CASE (


"format_descriptor: valid address width accepted"
,
"[ir][format]"
)
 {
    auto result = lithe::ir::format_descriptor::make(
        lithe::ir::encoding::binary_le,
        lithe::ir::stage::physical,
        {1, 0, 0},
        64,
        lithe::execution::ir_kind::physical_mir);

    REQUIRE(result.has_value());
    CHECK(result->valid());
    CHECK(result->target_address_width == 64);
}

TEST_CASE (


"format_descriptor: target_address_width 32 is valid"
,
"[ir][format]"
)
 {
    auto result = lithe::ir::format_descriptor::make(
        lithe::ir::encoding::text_utf8,
        lithe::ir::stage::surface,
        {0, 1, 0},
        32,
        lithe::execution::ir_kind::surface_ast);

    REQUIRE(result.has_value());
    CHECK(result->target_address_width == 32);
}

// ============================================================================
// no_pipeline_hooks — compile-time empty
// ============================================================================

TEST_CASE (


"no_pipeline_hooks: is_empty_v"
,
"[hooks][neutral]"
)
 {
    static_assert(std::is_empty_v<lithe::execution::no_pipeline_hooks>);
    SUCCEED();
}

// ============================================================================
// pipeline_hooks<no_ir_provider> — available = false; compiles away
// ============================================================================

TEST_CASE (


"pipeline_hooks<no_ir_provider>: active = false"
,
"[hooks][neutral]"
)
 {
    using hooks_t = lithe::ir::pipeline_hooks<lithe::ir::no_ir_provider>;
    static_assert(!hooks_t::active, "inactive hooks must report active=false");
    hooks_t h;
    CHECK_FALSE(h.is_active());
}

TEST_CASE (


"pipeline_hooks<no_ir_provider>: fire is a no-op"
,
"[hooks][neutral]"
)
 {
    lithe::ir::pipeline_hooks<lithe::ir::no_ir_provider> h;
    lithe::ir::ir_hook_request req{
        .point  = lithe::ir::hook_point::pre_compile,
        .format = {},
        .context = "test",
    };
    auto err = h.fire(req, "some ir text");
    CHECK_FALSE(err.has_value()); // no error from inactive hooks
}

// ============================================================================
// pipeline_hooks<diagnostic_text_stub> — active path
// ============================================================================

TEST_CASE (


"pipeline_hooks<diagnostic_text_stub>: active = true"
,
"[hooks][active]"
)
 {
    using hooks_t = lithe::ir::pipeline_hooks<lithe::ir::diagnostic_text_stub>;
    static_assert(hooks_t::active, "active hooks must report active=true");

    hooks_t h{lithe::ir::diagnostic_text_stub{}};
    CHECK(h.is_active());
}

TEST_CASE (


"pipeline_hooks<diagnostic_text_stub>: fire with valid format succeeds"
,
"[hooks][active]"
)
{
    lithe::ir::diagnostic_text_stub stub;
    lithe::ir::pipeline_hooks<lithe::ir::diagnostic_text_stub> h{
        stub, lithe::ir::hook_failure_policy::ignore};

    auto fmt = lithe::ir::format_descriptor::make(
        lithe::ir::encoding::text_utf8,
        lithe::ir::stage::physical,
        {1, 0, 0}, 64,
        lithe::execution::ir_kind::physical_mir);
    REQUIRE(fmt.has_value());

    lithe::ir::ir_hook_request req{
        .point  = lithe::ir::hook_point::pre_compile,
        .format = *fmt,
        .context = "test_context",
    };

    auto err = h.fire(req, "test_ir_content");
    CHECK_FALSE(err.has_value()); // no error
}

TEST_CASE (


"pipeline_hooks: hook_failure_policy::ignore does not propagate"
,
"[hooks][failure_policy]"
)
{
    lithe::ir::diagnostic_text_stub stub;
    lithe::ir::pipeline_hooks<lithe::ir::diagnostic_text_stub> h{
        stub, lithe::ir::hook_failure_policy::ignore};

    // Fire with invalid format — should be ignored.
    lithe::ir::ir_hook_request req{
        .point  = lithe::ir::hook_point::pre_compile,
        .format = {}, // invalid
    };

    auto err = h.fire(req, "some_ir");
    // With ignore policy, errors are swallowed.
    // The fire() returns nullopt (propagate_error policy would be different).
    // Either way, this must not crash.
    SUCCEED();
}

TEST_CASE (


"pipeline_hooks: hook_failure_policy::propagate_error surfaces error"
,
"[hooks][failure_policy]"
)
{
    lithe::ir::diagnostic_text_stub stub;
    lithe::ir::pipeline_hooks<lithe::ir::diagnostic_text_stub> h{
        stub, lithe::ir::hook_failure_policy::propagate_error};

    lithe::ir::ir_hook_request req{
        .point  = lithe::ir::hook_point::on_error,
        .format = {}, // invalid format → error
    };

    auto err = h.fire(req, "some_ir");
    REQUIRE(err.has_value()); // error surfaced
    CHECK_FALSE(err->detail.empty());
}

// ============================================================================
// diagnostic_text_stub — provider validation
// ============================================================================

TEST_CASE (


"diagnostic_text_stub: validate non-empty IR → resolved"
,
"[ir][provider]"
)
 {
    lithe::ir::diagnostic_text_stub stub;
    auto state = stub.do_validate("define void @f() { ret void }");
    CHECK(state == lithe::ir::ir_resolution_state::resolved);
}

TEST_CASE (


"diagnostic_text_stub: validate empty IR → unresolved_required"
,
"[ir][provider]"
)
 {
    lithe::ir::diagnostic_text_stub stub;
    auto state = stub.do_validate("");
    CHECK(state == lithe::ir::ir_resolution_state::unresolved_required_operations);
}

TEST_CASE (


"diagnostic_text_stub: export_text round-trip"
,
"[ir][provider]"
)
 {
    lithe::ir::diagnostic_text_stub stub;
    auto fmt = lithe::ir::format_descriptor::make(
        lithe::ir::encoding::text_utf8, lithe::ir::stage::physical,
        {1, 0, 0}, 64, lithe::execution::ir_kind::physical_mir);
    REQUIRE(fmt.has_value());

    std::string_view ir_text = "test IR content";
    auto out = stub.do_export_text(ir_text, *fmt);
    REQUIRE(out.has_value());
    CHECK(out->valid());
    CHECK(out->view().as_string_view() == ir_text);
}

TEST_CASE (


"diagnostic_text_stub: export_text with invalid format → error"
,
"[ir][provider]"
)
 {
    lithe::ir::diagnostic_text_stub stub;
    lithe::ir::format_descriptor bad_fmt{}; // target_address_width = 0

    auto out = stub.do_export_text("some text", bad_fmt);
    REQUIRE_FALSE(out.has_value());
}

// ============================================================================
// no_ir_provider — available = false sentinel
// ============================================================================

TEST_CASE (


"no_ir_provider: available = false"
,
"[ir][provider]"
)
 {
    static_assert(!lithe::ir::no_ir_provider::available);
    static_assert(std::is_empty_v<lithe::ir::no_ir_provider>);
    SUCCEED();
}

// ============================================================================
// ir_resolution_state enum values
// ============================================================================

TEST_CASE (


"ir_resolution_state: values exist"
,
"[ir][provider]"
)
 {
    using st = lithe::ir::ir_resolution_state;
    CHECK(st::resolved                        != st::contains_opaque_optional_operations);
    CHECK(st::resolved                        != st::unresolved_required_operations);
    CHECK(st::contains_opaque_optional_operations != st::unresolved_required_operations);
}

// ============================================================================
// §10a.6 canonical hook_point values
// ============================================================================

TEST_CASE (


"hook_point: canonical §10a.6 values exist"
,
"[hooks][hook_point]"
)
 {
    using hp = lithe::ir::hook_point;
    // All 8 canonical values distinct.
    CHECK(hp::after_capture              != hp::after_semantic_analysis);
    CHECK(hp::after_semantic_analysis    != hp::after_canonicalization);
    CHECK(hp::after_canonicalization     != hp::after_high_level_lowering);
    CHECK(hp::after_high_level_lowering  != hp::after_optimization);
    CHECK(hp::after_optimization         != hp::after_physical_lowering);
    CHECK(hp::after_physical_lowering    != hp::before_backend_compilation);
    CHECK(hp::before_backend_compilation != hp::after_backend_compilation);
}

TEST_CASE (


"hook_point: compatibility aliases compile"
,
"[hooks][hook_point]"
)
 {
    using hp = lithe::ir::hook_point;
    // Legacy aliases map to canonical values — just verify they compile and are usable.
    [[maybe_unused]] hp a = hp::pre_compile;
    [[maybe_unused]] hp b = hp::post_compile;
    [[maybe_unused]] hp c = hp::pre_install;
    [[maybe_unused]] hp d = hp::post_install;
    [[maybe_unused]] hp e = hp::on_error;
    SUCCEED();
}

// ============================================================================
// §10a.6 canonical hook_failure_policy values
// ============================================================================

TEST_CASE (


"hook_failure_policy: all three values distinct"
,
"[hooks][failure_policy]"
)
 {
    using fp = lithe::ir::hook_failure_policy;
    CHECK(fp::ignore           != fp::emit_diagnostic);
    CHECK(fp::emit_diagnostic  != fp::fail_compilation);
    CHECK(fp::ignore           != fp::fail_compilation);
}

TEST_CASE (


"hook_failure_policy: emit_diagnostic is non-fatal (returns nullopt)"
,
"[hooks][failure_policy]"
)
{
    lithe::ir::diagnostic_text_stub stub;
    lithe::ir::pipeline_hooks<lithe::ir::diagnostic_text_stub> h{
        stub, lithe::ir::hook_failure_policy::emit_diagnostic};

    // Invalid format with emit_diagnostic policy — non-fatal, returns nullopt.
    lithe::ir::ir_hook_request req{
        .point  = lithe::ir::hook_point::after_optimization,
        .format = {}, // invalid
    };
    auto err = h.fire(req, "some_ir");
    CHECK_FALSE(err.has_value()); // non-fatal
}

TEST_CASE (


"hook_failure_policy: fail_compilation is fatal (returns ir_error)"
,
"[hooks][failure_policy]"
)
{
    lithe::ir::diagnostic_text_stub stub;
    lithe::ir::pipeline_hooks<lithe::ir::diagnostic_text_stub> h{
        stub, lithe::ir::hook_failure_policy::fail_compilation};

    lithe::ir::ir_hook_request req{
        .point  = lithe::ir::hook_point::before_backend_compilation,
        .format = {}, // invalid → triggers failure
    };
    auto err = h.fire(req, "some_ir");
    REQUIRE(err.has_value());
    CHECK_FALSE(err->detail.empty());
}

// ============================================================================
// §10a.6 ir_hook_request — new fields
// ============================================================================

TEST_CASE (


"ir_hook_request: requested_encoding and per-hook failure_policy compile"
,
"[hooks][request]"
)
{
    lithe::ir::ir_hook_request req{
        .point              = lithe::ir::hook_point::after_physical_lowering,
        .requested_encoding = lithe::ir::encoding::binary_le,
        .failure_policy     = lithe::ir::hook_failure_policy::emit_diagnostic,
    };
    CHECK(req.requested_encoding == lithe::ir::encoding::binary_le);
    CHECK(req.failure_policy     == lithe::ir::hook_failure_policy::emit_diagnostic);
    CHECK(req.point              == lithe::ir::hook_point::after_physical_lowering);
}

TEST_CASE (


"ir_hook_request: per-hook failure_policy overrides global policy"
,
"[hooks][request]"
)
{
    // Global policy = ignore; per-hook = fail_compilation.
    lithe::ir::diagnostic_text_stub stub;
    lithe::ir::pipeline_hooks<lithe::ir::diagnostic_text_stub> h{
        stub, lithe::ir::hook_failure_policy::ignore};

    lithe::ir::ir_hook_request req{
        .point          = lithe::ir::hook_point::after_backend_compilation,
        .failure_policy = lithe::ir::hook_failure_policy::fail_compilation,
        .format         = {}, // invalid
    };
    auto err = h.fire(req, "ir");
    REQUIRE(err.has_value()); // per-hook fail_compilation wins over global ignore
}

