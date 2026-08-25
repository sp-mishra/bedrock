// =============================================================================
// test_crank_annotation.cpp — Module 6 (Gap 1): typed annotation registry.
//
// Verifies: include/languages/crank/annotation.hpp
//           include/languages/crank/dump.hpp (dump_annotations)
//           include/languages/crank/context.hpp (register_annotation / install_extension)
//
//  1.  make_crank_annotation_registry() seeds crank.* built-in descriptors.
//  2.  @lithe.cacheline(align=64) resolves + validates (u32 arg).
//  3.  @lithe.cacheline(align="large") → CRANK-ANN-004 type mismatch.
//  4.  Unknown arg name → CRANK-ANN-003.
//  5.  Missing required arg → CRANK-ANN-005.
//  6.  Unqualified extension @foo → CRANK-ANN-001 hard diagnostic.
//  7.  Unknown namespaced under strict → CRANK-ANN-002; under preserve_unknown → kept.
//  8.  Reserved-namespace recognition for all 9 prefixes.
//  9.  optimization_hint consume → execution_hint matches map_exec_attr of equiv exec attr.
// 10.  capability_declaration / effect_declaration fold ext-band bits.
// 11.  proof_annotation kind recognized correctly.
// 12.  assumption-strength under paranoid verify policy → CRANK-ANN-007.
// 13.  Drift guard: built-in unqualified set has exactly 9 entries.
// 14.  install_extension registers a static crank_extension's annotations.
// 15.  dump_annotations round-trips to non-empty JSON.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/annotation.hpp"
#include "languages/crank/context.hpp"
#include "languages/crank/dump.hpp"

using namespace crank;

// ---- Helper: build an annotation_arg_value with a u32 value ----------------

static annotation_arg_value make_arg_u32(std::string_view name, std::uint32_t v) {
    return {containers::desc_name_hash(name), v};
}

static annotation_arg_value make_arg_str(std::string_view name, std::string s) {
    return {containers::desc_name_hash(name), std::move(s)};
}

// ===========================================================================
// 1. make_crank_annotation_registry seeds built-in descriptors
// ===========================================================================
TEST_CASE (

"make_crank_annotation_registry: built-in descriptors present"
,
"[crank][annotation][registry]"
)
 {
    auto reg = make_crank_annotation_registry();
    CHECK(reg.resolve("crank.parallel") != nullptr);
    CHECK(reg.resolve("crank.simd")     != nullptr);
    CHECK(reg.resolve("crank.gpu")      != nullptr);
    CHECK(reg.resolve("crank.pure")     != nullptr);
    CHECK(reg.resolve("crank.io")       != nullptr);
    CHECK(reg.resolve("crank.net")      != nullptr);
    CHECK(reg.resolve("crank.host")     != nullptr);
    CHECK(reg.resolve("crank.reads")    != nullptr);
    CHECK(reg.resolve("crank.writes")   != nullptr);
}

// ===========================================================================
// 2. @lithe.cacheline(align=64) resolves + validates
// ===========================================================================
TEST_CASE (

"@lithe.cacheline resolves when descriptor registered"
,
"[crank][annotation][resolve]"
)
 {
    auto reg = make_crank_annotation_registry();

    // Register the lithe.cacheline descriptor manually (id >= 1000)
    static const schema_field cacheline_schema[] = {
        { containers::desc_name_hash("align"), annotation_arg_type::u32, true },
    };
    annotation_descriptor d;
    d.name             = "lithe.cacheline";
    d.kind             = annotation_kind::optimization_hint;
    d.default_strength = annotation_strength::advisory;
    d.stable_id        = 2000;
    d.version          = 1;
    d.name_hash        = containers::desc_name_hash("lithe.cacheline");
    reg.register_desc(d, cacheline_schema);

    parsed_annotation ann;
    ann.name = "lithe.cacheline";
    ann.args.push_back(make_arg_u32("align", 64));

    annotation_resolver resolver(reg);
    auto res = resolver.resolve(ann);

    REQUIRE(res.desc != nullptr);
    CHECK(res.desc->name == "lithe.cacheline");

    // Validate args against schema
    auto diags = validate_args_with_schema(*res.desc, reg, ann);
    CHECK(diags.empty());
}

// ===========================================================================
// 3. @lithe.cacheline(align="large") → CRANK-ANN-004 type mismatch
// ===========================================================================
TEST_CASE (

"@lithe.cacheline(align=string) → CRANK-ANN-004"
,
"[crank][annotation][validation]"
)
 {
    auto reg = make_crank_annotation_registry();

    static const schema_field cacheline_schema[] = {
        { containers::desc_name_hash("align"), annotation_arg_type::u32, true },
    };
    annotation_descriptor d;
    d.name      = "lithe.cacheline";
    d.kind      = annotation_kind::optimization_hint;
    d.default_strength = annotation_strength::advisory;
    d.stable_id = 2000;
    d.name_hash = containers::desc_name_hash("lithe.cacheline");
    reg.register_desc(d, cacheline_schema);

    parsed_annotation ann;
    ann.name = "lithe.cacheline";
    ann.args.push_back(make_arg_str("align", "large"));  // wrong type

    auto* desc = reg.resolve("lithe.cacheline");
    REQUIRE(desc != nullptr);

    auto diags = validate_args_with_schema(*desc, reg, ann);
    REQUIRE_FALSE(diags.empty());
    CHECK(diags[0].kind == annotation_diag_kind::arg_type_mismatch);
    CHECK(diags[0].message.find("CRANK-ANN-004") != std::string::npos);
}

// ===========================================================================
// 4. Unknown arg name → CRANK-ANN-003
// ===========================================================================
TEST_CASE (

"unknown arg name → CRANK-ANN-003"
,
"[crank][annotation][validation]"
)
 {
    auto reg = make_crank_annotation_registry();

    static const schema_field schema[] = {
        { containers::desc_name_hash("align"), annotation_arg_type::u32, false },
    };
    annotation_descriptor d;
    d.name      = "lithe.test";
    d.kind      = annotation_kind::optimization_hint;
    d.default_strength = annotation_strength::advisory;
    d.stable_id = 2001;
    d.name_hash = containers::desc_name_hash("lithe.test");
    reg.register_desc(d, schema);

    parsed_annotation ann;
    ann.name = "lithe.test";
    ann.args.push_back(make_arg_u32("not_in_schema", 1));

    auto* desc = reg.resolve("lithe.test");
    REQUIRE(desc != nullptr);

    auto diags = validate_args_with_schema(*desc, reg, ann);
    REQUIRE_FALSE(diags.empty());
    CHECK(diags[0].kind == annotation_diag_kind::arg_name_not_in_schema);
    CHECK(diags[0].message.find("CRANK-ANN-003") != std::string::npos);
}

// ===========================================================================
// 5. Missing required arg → CRANK-ANN-005
// ===========================================================================
TEST_CASE (

"missing required arg → CRANK-ANN-005"
,
"[crank][annotation][validation]"
)
 {
    auto reg = make_crank_annotation_registry();

    static const schema_field schema[] = {
        { containers::desc_name_hash("align"), annotation_arg_type::u32, true },
    };
    annotation_descriptor d;
    d.name      = "lithe.required_test";
    d.kind      = annotation_kind::optimization_hint;
    d.default_strength = annotation_strength::advisory;
    d.stable_id = 2002;
    d.name_hash = containers::desc_name_hash("lithe.required_test");
    reg.register_desc(d, schema);

    parsed_annotation ann;
    ann.name = "lithe.required_test";
    // No args supplied — 'align' is required

    auto* desc = reg.resolve("lithe.required_test");
    REQUIRE(desc != nullptr);

    auto diags = validate_args_with_schema(*desc, reg, ann);
    REQUIRE_FALSE(diags.empty());
    CHECK(diags[0].kind == annotation_diag_kind::missing_required_arg);
    CHECK(diags[0].message.find("CRANK-ANN-005") != std::string::npos);
}

// ===========================================================================
// 6. Unqualified extension @foo → CRANK-ANN-001 hard diagnostic
// ===========================================================================
TEST_CASE (

"unqualified extension @foo → CRANK-ANN-001"
,
"[crank][annotation][namespace]"
)
 {
    auto reg = make_crank_annotation_registry();
    annotation_resolver resolver(reg);

    parsed_annotation ann;
    ann.name = "foo";  // not in built-in unqualified set

    auto res = resolver.resolve(ann);
    REQUIRE_FALSE(res.diags.empty());
    CHECK(res.diags[0].kind == annotation_diag_kind::unqualified_extension);
    CHECK(res.diags[0].message.find("CRANK-ANN-001") != std::string::npos);
    CHECK(res.diags[0].is_error);
}

// ===========================================================================
// 7. Unknown namespaced: strict → CRANK-ANN-002; preserve_unknown → kept
// ===========================================================================
TEST_CASE (

"unknown namespaced under strict → CRANK-ANN-002"
,
"[crank][annotation][namespace]"
)
 {
    auto reg = make_crank_annotation_registry();
    annotation_resolver resolver(reg, annotation_policy::strict);

    parsed_annotation ann;
    ann.name = "company.unknown_feature";

    auto res = resolver.resolve(ann);
    REQUIRE_FALSE(res.diags.empty());
    CHECK(res.diags[0].kind == annotation_diag_kind::unknown_namespaced_strict);
    CHECK(res.diags[0].message.find("CRANK-ANN-002") != std::string::npos);
}

TEST_CASE (

"unknown namespaced under preserve_unknown → kept, no error"
,
"[crank][annotation][namespace]"
)
 {
    auto reg = make_crank_annotation_registry();
    annotation_resolver resolver(reg, annotation_policy::preserve_unknown);

    parsed_annotation ann;
    ann.name = "company.unknown_feature";

    auto res = resolver.resolve(ann);
    CHECK(res.diags.empty());
    CHECK(res.preserved);
}

// ===========================================================================
// 8. Reserved-namespace recognition for all 9 prefixes
// ===========================================================================
TEST_CASE (

"is_reserved_namespace recognizes all 9 reserved prefixes"
,
"[crank][annotation][namespace]"
)
 {
    CHECK(is_reserved_namespace("crank"));
    CHECK(is_reserved_namespace("lithe"));
    CHECK(is_reserved_namespace("pravaha"));
    CHECK(is_reserved_namespace("medha"));
    CHECK(is_reserved_namespace("tarka"));
    CHECK(is_reserved_namespace("sutra"));
    CHECK(is_reserved_namespace("domain"));
    CHECK(is_reserved_namespace("company"));
    CHECK(is_reserved_namespace("user"));
    CHECK_FALSE(is_reserved_namespace("external"));
    CHECK_FALSE(is_reserved_namespace("mylib"));
}

// ===========================================================================
// 9. optimization_hint consume → execution_hint matches map_exec_attr
// ===========================================================================
TEST_CASE (

"optimization_hint consume produces execution_hint matching map_exec_attr"
,
"[crank][annotation][consume]"
)
 {
    auto reg = make_crank_annotation_registry();
    auto* parallel_desc = reg.resolve("crank.parallel");
    REQUIRE(parallel_desc != nullptr);

    auto eff = consume(*parallel_desc, {});
    REQUIRE(eff.hint.has_value());
    CHECK(eff.hint->preferred == lithe::exec::execution_kind::threaded);

    // Cross-check with map_exec_attr directly
    crank_exec_attr attr;
    attr.kind = crank_attr_kind::parallel;
    auto direct_hint = map_exec_attr(attr);
    CHECK(eff.hint->preferred == direct_hint.preferred);
}

// ===========================================================================
// 10. capability_declaration / effect_declaration fold ext-band bits
// ===========================================================================
TEST_CASE (

"capability_declaration / effect_declaration fold ext-band bits"
,
"[crank][annotation][consume]"
)
 {
    annotation_descriptor cap_desc;
    cap_desc.name         = "crank.host";
    cap_desc.kind         = annotation_kind::capability_declaration;
    cap_desc.default_strength = annotation_strength::advisory;
    cap_desc.stable_id    = 1006;
    cap_desc.capabilities = kCapMaskHost;

    auto eff = consume(cap_desc, {});
    CHECK(eff.add_caps == kCapMaskHost);

    annotation_descriptor eff_desc;
    eff_desc.name       = "crank.io";
    eff_desc.kind       = annotation_kind::effect_declaration;
    eff_desc.default_strength = annotation_strength::advisory;
    eff_desc.stable_id  = 1004;
    eff_desc.effects    = vakya::types::kEffectMaskIO;

    auto eff2 = consume(eff_desc, {});
    CHECK(eff2.add_effects == vakya::types::kEffectMaskIO);
}

// ===========================================================================
// 11. proof_annotation kind recognized in consume
// ===========================================================================
TEST_CASE (

"proof_annotation kind recognized by consume"
,
"[crank][annotation][consume]"
)
 {
    annotation_descriptor d;
    d.name             = "crank.assume_pure";
    d.kind             = annotation_kind::proof_annotation;
    d.default_strength = annotation_strength::assumption;
    d.stable_id        = 1010;

    auto eff = consume(d, {});
    REQUIRE_FALSE(eff.proof_obligations.empty());
    CHECK(eff.proof_obligations[0].find("crank.assume_pure") != std::string::npos);
}

// ===========================================================================
// 12. assumption-strength under paranoid verify policy → CRANK-ANN-007
// ===========================================================================
TEST_CASE (

"assumption-strength under paranoid policy → CRANK-ANN-007"
,
"[crank][annotation][policy]"
)
 {
    annotation_descriptor d;
    d.name             = "crank.assume_pure";
    d.kind             = annotation_kind::proof_annotation;
    d.default_strength = annotation_strength::assumption;
    d.stable_id        = 1010;

    parsed_annotation ann;
    ann.name = "crank.assume_pure";

    auto diag = check_assumption_strength(d, ann, verify_policy::paranoid);
    REQUIRE(diag.has_value());
    CHECK(diag->kind == annotation_diag_kind::assumption_paranoid_verify);
    CHECK(diag->message.find("CRANK-ANN-007") != std::string::npos);
}

TEST_CASE (

"assumption-strength under non-paranoid policy: no diagnostic"
,
"[crank][annotation][policy]"
)
 {
    annotation_descriptor d;
    d.name             = "crank.assume_pure";
    d.kind             = annotation_kind::proof_annotation;
    d.default_strength = annotation_strength::assumption;
    d.stable_id        = 1010;

    parsed_annotation ann;
    ann.name = "crank.assume_pure";

    // verify_policy::check is a non-paranoid policy — should produce no diagnostic
    auto diag = check_assumption_strength(d, ann, verify_policy::check);
    CHECK_FALSE(diag.has_value());
}

// ===========================================================================
// 13. Drift guard: built-in unqualified set has exactly 9 entries
//     (3 exec attrs + 6 effect attrs — must stay in sync with exec_hint/effects)
// ===========================================================================
TEST_CASE (

"built-in unqualified set has 9 entries (drift guard)"
,
"[crank][annotation][drift]"
)
 {
    // The closed set: parallel, simd, gpu (exec_hint) + pure, reads, writes, io, net, host (effects)
    CHECK(kBuiltinUnqualifiedAnnotations.size() == 9u);

    // All crank_attr_kind spellings must be in the built-in set
    CHECK(is_builtin_unqualified("parallel"));
    CHECK(is_builtin_unqualified("simd"));
    CHECK(is_builtin_unqualified("gpu"));

    // All fn_attribute_set effect spellings must be in the built-in set
    CHECK(is_builtin_unqualified("pure"));
    CHECK(is_builtin_unqualified("reads"));
    CHECK(is_builtin_unqualified("writes"));
    CHECK(is_builtin_unqualified("io"));
    CHECK(is_builtin_unqualified("net"));
    CHECK(is_builtin_unqualified("host"));

    // Non-builtins must NOT be in the set
    CHECK_FALSE(is_builtin_unqualified("cacheline"));
    CHECK_FALSE(is_builtin_unqualified("foo"));
}

// ===========================================================================
// 14. install_extension registers a static crank_extension's annotations
// ===========================================================================

struct MyExtension {
    static constexpr std::uint32_t id = 5000;
    static constexpr std::uint32_t version = 1;

    void register_annotations(annotation_registry& reg) {
        annotation_descriptor d;
        d.name = "company.my_hint";
        d.kind = annotation_kind::optimization_hint;
        d.default_strength = annotation_strength::advisory;
        d.stable_id = 5001;
        d.version = 1;
        d.name_hash = containers::desc_name_hash("company.my_hint");
        reg.register_desc(d, {});
    }
};

TEST_CASE (

"install_extension registers annotations from static plugin"
,
"[crank][annotation][extension]"
)
 {
    crank::context ctx;
    ctx.install_extension(MyExtension{});

    auto* d = ctx.annotations().resolve("company.my_hint");
    REQUIRE(d != nullptr);
    CHECK(d->stable_id == 5001u);
}

// ===========================================================================
// 15. dump_annotations round-trips to non-empty JSON
// ===========================================================================
TEST_CASE (

"dump_annotations round-trips to non-empty JSON"
,
"[crank][annotation][dump]"
)
 {
    auto reg = make_crank_annotation_registry();
    annotation_resolver resolver(reg);

    // Resolve a built-in: @parallel
    parsed_annotation ann;
    ann.name = "parallel";  // built-in unqualified

    std::vector<parsed_annotation>    anns    = {ann};
    std::vector<annotation_resolution> resols = {resolver.resolve(ann)};

    auto records = make_annotation_records(anns, resols);
    REQUIRE_FALSE(records.empty());

    const auto json = dump_annotations(records);
    CHECK_FALSE(json.empty());
    CHECK(json.find("fq_name")  != std::string::npos);
    CHECK(json.find("kind")     != std::string::npos);
    CHECK(json.find("strength") != std::string::npos);
}
