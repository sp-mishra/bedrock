#include "catch_amalgamated.hpp"

#include "lithe/lithe_semantic.hpp"

#include <string>

// ---------------------------------------------------------------------------
// Test 1 — register primitive integer / float / bool types
// ---------------------------------------------------------------------------
TEST_CASE (



"semantic_type_registry registers integer, float, and bool primitives"
,
"[lithe][semantic][types]"
)
 {
    using namespace lithe::semantic::types;

    semantic_type_registry reg;

    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto f64 = reg.make_float_type(64, "f64");

    // Boolean is an integer of width 1.
    const auto b1 = reg.make_integer_type(1, false, "bool");

    REQUIRE(i32 != invalid_type_id);
    REQUIRE(f64 != invalid_type_id);
    REQUIRE(b1  != invalid_type_id);

    const auto d_i32 = reg.find_type(i32);
    const auto d_f64 = reg.find_type(f64);
    const auto d_b1  = reg.find_type(b1);

    REQUIRE(d_i32.has_value());
    REQUIRE(d_i32->kind      == type_kind::integer);
    REQUIRE(d_i32->bit_width == 32);
    REQUIRE(d_i32->name      == "i32");

    REQUIRE(d_f64.has_value());
    REQUIRE(d_f64->kind      == type_kind::floating);
    REQUIRE(d_f64->bit_width == 64);

    REQUIRE(d_b1.has_value());
    REQUIRE(d_b1->kind      == type_kind::integer);
    REQUIRE(d_b1->bit_width == 1);
    REQUIRE(d_b1->name      == "bool");
}

// ---------------------------------------------------------------------------
// Test 2 — canonicalize returns the same id for structurally equivalent types
// ---------------------------------------------------------------------------
TEST_CASE (



"semantic_type_registry canonicalizes structurally equivalent types"
,
"[lithe][semantic][types]"
)
 {
    using namespace lithe::semantic::types;

    semantic_type_registry reg;

    // Register the same integer shape twice through two different paths.
    const auto id_a = reg.make_integer_type(32, true, "int32");
    const auto id_b = reg.make_integer_type(32, true, "int32");

    // Both calls must resolve to the same canonical id.
    REQUIRE(id_a == id_b);
    REQUIRE(reg.equivalent(id_a, id_b));
}

// ---------------------------------------------------------------------------
// Test 3 — subtype_of works for simple numeric widening
// ---------------------------------------------------------------------------
TEST_CASE (



"subtype_of returns true for numeric widening (i16 < i32 < i64, f32 < f64)"
,
"[lithe][semantic][types]"
)
 {
    using namespace lithe::semantic::types;

    semantic_type_registry reg;

    const auto i16 = reg.make_integer_type(16, true, "i16");
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    const auto f32 = reg.make_float_type(32, "f32");
    const auto f64 = reg.make_float_type(64, "f64");

    // i16 is a subtype of i32 and i64.
    REQUIRE(reg.subtype_of(i16, i32));
    REQUIRE(reg.subtype_of(i16, i64));
    REQUIRE(reg.subtype_of(i32, i64));

    // f32 is a subtype of f64.
    REQUIRE(reg.subtype_of(f32, f64));

    // An integer is a subtype of any float (promotion rule).
    REQUIRE(reg.subtype_of(i32, f64));

    // Inverse direction is not a subtype.
    REQUIRE_FALSE(reg.subtype_of(i64, i32));
    REQUIRE_FALSE(reg.subtype_of(f64, f32));
}

// ---------------------------------------------------------------------------
// Test 4 — operation contract validates add(int32, int32)
// ---------------------------------------------------------------------------
TEST_CASE (



"operation contract validates add(int32, int32) without errors"
,
"[lithe][semantic][type_rules]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::type_rules;

    types::semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");

    semantic_type_rule_engine engine{&reg};
    const auto result = engine.check_operation_contract("add", {i32, i32});

    REQUIRE(result.ok);
    // No error-level diagnostics.
    for (const auto &d : result.diagnostics) {
        REQUIRE(d.severity != type_rule_severity::error);
    }
}

// ---------------------------------------------------------------------------
// Test 5 — operation contract rejects add(tensor, query) — incompatible kinds
// ---------------------------------------------------------------------------
TEST_CASE (



"operation contract rejects add(tensor, query) as incompatible types"
,
"[lithe][semantic][type_rules]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::type_rules;

    types::semantic_type_registry reg;

    // Tensor element type.
    const auto elem = reg.make_float_type(32, "f32");
    const auto tensor_t = reg.make_tensor_type(elem, {4, 4}, "tensor_4x4_f32");

    // Query type (dynamic / structural placeholder).
    const auto query_t = reg.make_dynamic_type("query");

    semantic_type_rule_engine engine{&reg};
    const auto result = engine.check_operation_contract("add", {tensor_t, query_t});

    // tensor + query cannot be resolved by any built-in widening rule.
    // The engine must report a non-ok result or at minimum leave it unsupported.
    // Either an error diagnostic or the result being infeasible is acceptable.
    const bool has_error = !result.ok ||
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [](const type_rule_diagnostic &d) {
                        return d.severity == type_rule_severity::error ||
                               d.severity == type_rule_severity::warning;
                    });
    REQUIRE(has_error);
}

// ---------------------------------------------------------------------------
// Test 6 — coercion plan inserts numeric_widen for i32 → i64
// ---------------------------------------------------------------------------
TEST_CASE (



"coercion_planner produces a numeric_widen plan for i32 to i64"
,
"[lithe][semantic][coercion]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::type_rules;

    types::semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    coercion_planner planner{&reg};
    const auto plan = planner.plan(i32, i64);

    REQUIRE(plan.feasible);
    REQUIRE(plan.is_lossless);
    REQUIRE_FALSE(plan.steps.empty());

    // The first (and only) step must be a numeric widening.
    REQUIRE(plan.steps.front().kind == coercion_kind::numeric_widen);
    REQUIRE(plan.steps.front().from_type == i32);
    REQUIRE(plan.steps.front().to_type   == i64);
}

// ---------------------------------------------------------------------------
// Test 7 — EasyRules-backed diagnostic explanation is non-empty for a type error
// ---------------------------------------------------------------------------
TEST_CASE (



"semantic_type_rule_engine explain_type_error returns non-empty string"
,
"[lithe][semantic][type_rules][easy_rules]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::type_rules;

    types::semantic_type_registry reg;
    const auto elem = reg.make_float_type(32, "f32");
    const auto tensor_t = reg.make_tensor_type(elem, {4}, "tensor_f32");
    const auto query_t  = reg.make_dynamic_type("query_type");

    semantic_type_rule_engine engine{&reg};

    // Force an incompatible assignment check to generate an error result.
    const auto result = engine.check_assignment(tensor_t, query_t);

    if (!result.ok) {
        const auto explanation = engine.explain_type_error(result);
        REQUIRE_FALSE(explanation.empty());
        REQUIRE(explanation != "no error");
    } else {
        // If the engine accepted this (e.g. dynamic accepts all), just verify
        // explain_type_error on a manually-constructed error is non-empty.
        type_rule_result err;
        err.add_error("test_rule", "synthetic mismatch", tensor_t, query_t);
        const auto fallback = engine.explain_type_error(err);
        REQUIRE_FALSE(fallback.empty());
        REQUIRE(fallback != "no error");
    }
}

// ---------------------------------------------------------------------------
// Test 8 — integer_widening rule fires and appears in audit trail
// ---------------------------------------------------------------------------
TEST_CASE (



"type_rule_audit records integer_widening rule for i32 + i64 operands"
,
"[lithe][semantic][type_rules][easy_rules][audit]"
)
{
    using namespace lithe::semantic;
    using namespace lithe::semantic::type_rules;
    using easy_rules::AuditEvent;

    types::semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    semantic_type_rule_engine engine{&reg};
    (void) engine.check_operation_contract("add", {i32, i64});

    const auto &history = engine.type_rule_audit().get_history();

    const bool integer_widening_fired = std::any_of(
        history.begin(), history.end(),
        [](const AuditEvent &e) {
            return e.type == AuditEvent::Type::RuleFired &&
                   e.rule_name == "integer_widening";
        });
    REQUIRE(integer_widening_fired);
}

// ---------------------------------------------------------------------------
// Test 9 — int_float_promotion rule fires when int meets float
// ---------------------------------------------------------------------------
TEST_CASE (



"type_rule_audit records int_float_promotion rule for i32 + f64 operands"
,
"[lithe][semantic][type_rules][easy_rules][audit]"
)
{
    using namespace lithe::semantic;
    using namespace lithe::semantic::type_rules;
    using easy_rules::AuditEvent;

    types::semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto f64 = reg.make_float_type(64, "f64");

    semantic_type_rule_engine engine{&reg};
    const auto result = engine.check_operation_contract("mul", {i32, f64});

    // Promotion to float — result must be ok (no error, widening is legal).
    REQUIRE(result.ok);

    const auto &history = engine.type_rule_audit().get_history();
    const bool promotion_fired = std::any_of(
        history.begin(), history.end(),
        [](const AuditEvent &e) {
            return e.type == AuditEvent::Type::RuleFired &&
                   e.rule_name == "int_float_promotion";
        });
    REQUIRE(promotion_fired);
}

// ---------------------------------------------------------------------------
// Test 10 — homogeneous_type rule fires when both operands are identical
// ---------------------------------------------------------------------------
TEST_CASE (



"type_rule_audit records homogeneous_type rule for i32 + i32 operands"
,
"[lithe][semantic][type_rules][easy_rules][audit]"
)
{
    using namespace lithe::semantic;
    using namespace lithe::semantic::type_rules;
    using easy_rules::AuditEvent;

    types::semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");

    semantic_type_rule_engine engine{&reg};
    const auto result = engine.check_operation_contract("add", {i32, i32});

    REQUIRE(result.ok);

    const auto &history = engine.type_rule_audit().get_history();
    const bool homogeneous_fired = std::any_of(
        history.begin(), history.end(),
        [](const AuditEvent &e) {
            return e.type == AuditEvent::Type::RuleFired &&
                   e.rule_name == "homogeneous_type";
        });
    REQUIRE(homogeneous_fired);
}

// ---------------------------------------------------------------------------
// Test 11 — audit trail is non-empty after any rule engine run
// ---------------------------------------------------------------------------
TEST_CASE (



"type_rule_audit is non-empty after check_operation_contract"
,
"[lithe][semantic][type_rules][easy_rules][audit]"
)
{
    using namespace lithe::semantic;
    using namespace lithe::semantic::type_rules;

    types::semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    semantic_type_rule_engine engine{&reg};
    (void) engine.check_operation_contract("add", {i32, i64});

    REQUIRE_FALSE(engine.type_rule_audit().get_history().empty());
}

// Helper: build a simple host_type_descriptor for i32 to use in backend_capability.
static lithe::semantic::type_descriptor make_i32_host_desc() {
    lithe::semantic::type_descriptor d;
    d.primitive = lithe::semantic::primitive_type::signed_integer;
    d.numeric = lithe::semantic::numeric_type_info{true, true, 32};
    d.debug_name = "i32";
    return d;
}

// Helper: build a simple backend_capability covering arithmetic/pure/add/i32.
static lithe::semantic::backend_capability make_arith_backend(std::string name) {
    lithe::semantic::backend_capability cap;
    cap.backend_name = std::move(name);
    cap.operations.insert("add");
    cap.domains.push_back(lithe::semantic::domain_type::arithmetic);
    cap.effects.push_back(lithe::semantic::effect_type::pure);
    cap.types.push_back(make_i32_host_desc());
    return cap;
}

// ---------------------------------------------------------------------------
// Test 12 — capability_registry::validate_lowering accepts a registered op
// ---------------------------------------------------------------------------
TEST_CASE (



"capability_registry validate_lowering accepts a supported operation"
,
"[lithe][semantic][capability][easy_rules]"
)
{
    using namespace lithe::semantic;

    capability_registry registry;
    registry.register_backend(make_arith_backend("test_backend"));

    semantic_info sem;
    sem.domain = domain_type::arithmetic;
    sem.effect = effect_type::pure;

    const auto result = registry.validate_lowering(
        "test_backend", "add", sem, make_i32_host_desc());
    REQUIRE(result.legal);
    REQUIRE(result.reasons.empty());
}

// ---------------------------------------------------------------------------
// Test 13 — capability_registry::validate_lowering rejects an unsupported op
// ---------------------------------------------------------------------------
TEST_CASE (



"capability_registry validate_lowering rejects an unsupported operation"
,
"[lithe][semantic][capability][easy_rules]"
)
{
    using namespace lithe::semantic;

    // Register backend with no operations listed.
    backend_capability cap;
    cap.backend_name = "restricted_backend";
    cap.domains.push_back(domain_type::arithmetic);
    cap.effects.push_back(effect_type::pure);
    cap.types.push_back(make_i32_host_desc());

    capability_registry registry;
    registry.register_backend(std::move(cap));

    // Profile requires declared operations — backend has none.
    backend_profile prof;
    prof.require_declared_operations = true;

    semantic_info sem;
    sem.domain = domain_type::arithmetic;
    sem.effect = effect_type::pure;

    const auto result = registry.validate_lowering(
        "restricted_backend", "add", sem, make_i32_host_desc(), prof);
    REQUIRE_FALSE(result.legal);
    REQUIRE_FALSE(result.reasons.empty());
}

// ---------------------------------------------------------------------------
// Test 14 — validate_lowering rejects an unregistered backend
// ---------------------------------------------------------------------------
TEST_CASE (



"capability_registry validate_lowering rejects unknown backend"
,
"[lithe][semantic][capability][easy_rules]"
)
{
    using namespace lithe::semantic;

    capability_registry registry;

    semantic_info sem;
    const auto result = registry.validate_lowering("nonexistent", "add", sem, type_descriptor{});
    REQUIRE_FALSE(result.legal);
    REQUIRE_FALSE(result.reasons.empty());
}

// ---------------------------------------------------------------------------
// Test 15 — cap_audit() records events after validate_lowering runs
// ---------------------------------------------------------------------------
TEST_CASE (



"cap_audit records audit events after validate_lowering"
,
"[lithe][semantic][capability][easy_rules][audit]"
)
{
    using namespace lithe::semantic;

    capability_registry registry;
    registry.register_backend(make_arith_backend("audit_backend"));

    semantic_info sem;
    sem.domain = domain_type::arithmetic;
    sem.effect = effect_type::pure;

    (void) registry.validate_lowering("audit_backend", "add", sem, make_i32_host_desc());

    // At least one audit event must have been recorded.
    REQUIRE_FALSE(registry.cap_audit().get_history().empty());
}

// ---------------------------------------------------------------------------
// Test 16 — validate_lowering deny-list: denied operation is rejected
// ---------------------------------------------------------------------------
TEST_CASE (



"capability_registry validate_lowering rejects denied operation via profile"
,
"[lithe][semantic][capability][easy_rules]"
)
{
    using namespace lithe::semantic;

    capability_registry registry;
    registry.register_backend(make_arith_backend("denylist_backend"));

    backend_profile prof;
    prof.denied_operations.insert("add");

    semantic_info sem;
    sem.domain = domain_type::arithmetic;
    sem.effect = effect_type::pure;

    const auto result = registry.validate_lowering(
        "denylist_backend", "add", sem, make_i32_host_desc(), prof);
    REQUIRE_FALSE(result.legal);
}

// ---------------------------------------------------------------------------
// Test 17 — validate_lowering deny-list: denied effect is rejected
// ---------------------------------------------------------------------------
TEST_CASE (



"capability_registry validate_lowering rejects denied effect via profile"
,
"[lithe][semantic][capability][easy_rules]"
)
{
    using namespace lithe::semantic;

    backend_capability cap;
    cap.backend_name = "effect_backend";
    cap.operations.insert("read_file");
    cap.domains.push_back(domain_type::arithmetic);
    cap.effects.push_back(effect_type::io_operation);
    cap.types.push_back(make_i32_host_desc());

    capability_registry registry;
    registry.register_backend(std::move(cap));

    backend_profile prof;
    prof.denied_effects.push_back(effect_type::io_operation);

    semantic_info sem;
    sem.domain = domain_type::arithmetic;
    sem.effect = effect_type::io_operation;

    const auto result = registry.validate_lowering(
        "effect_backend", "read_file", sem, make_i32_host_desc(), prof);
    REQUIRE_FALSE(result.legal);
}
