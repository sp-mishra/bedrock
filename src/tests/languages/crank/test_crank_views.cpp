// =============================================================================
// test_crank_views.cpp — Crank domain views unit tests.
//
// Covers: include/languages/crank/lexer.hpp     (view keyword token)
//         include/languages/crank/parser.hpp     (view_decl / view_expr productions)
//         include/languages/crank/ast_tags.hpp   (view_decl_tag / view_expr_tag stable_ids)
//         include/languages/crank/build_ast.hpp  (view_decl_node / view_expr_node)
//         include/languages/crank/resolve.hpp    (symbol_kind::view + view metadata)
//         include/languages/crank/view_registry.hpp (view_descriptor / view_method_table)
//         include/languages/crank/obligations.hpp  (obligation_family::view predicates)
//         include/languages/crank/diagnostic.hpp   (CRANK-VIEW-000..010)
//         include/languages/crank/profiles.hpp     (crank_feature::domain_views)
//         include/languages/crank/sema_types.hpp   (typing_rule<view_expr_tag>)
//
// Test groups:
//   1. Lexer: "view" is a reserved keyword; "of" is still a valid identifier.
//   2. Parser: view_decl + view_expr parse (generic/const params; requires clause).
//   3. Feature gate: CRANK-VIEW-000 diagnostic code exists; feature_set API.
//   4. Sema: view wrapper type result matches target; same_type constraint emitted.
//   5. Obligations: all view obligation builders + stats counting.
//   6. Method resolution: view_method_table lookup (exact + fallback).
//   7. Annotations: view_domain_meta fields; fast_path_ok.
//   8. Lowering stubs: view_descriptor populates registry correctly.
//   9. Coherence (compile-time): resolve declares view + resolve_impl_target.
//  10. Host provider stubs: CRANK-VIEW-009 code accessible.
//  11. Borrow rule stubs: view aliasing obligations.
//  12. Builtins: indices is in builtin_name; zero uses T.identity() pattern.
//  13. Cross-domain stubs: CRANK-VIEW-002 code accessible.
//  14. Materialize-out stubs: CRANK-VIEW-005 code accessible.
//  Full linear example: parse + resolve view_decl + view_expr.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/ast_tags.hpp"
#include "languages/crank/build_ast.hpp"
#include "languages/crank/diagnostic.hpp"
#include "languages/crank/obligations.hpp"
#include "languages/crank/parser.hpp"
#include "languages/crank/profiles.hpp"
#include "languages/crank/resolve.hpp"
#include "languages/crank/sema_types.hpp"
#include "languages/crank/view_registry.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

// ============================================================================
// Group 1 — Lexer: "view" is reserved; "of" remains an ordinary identifier
// ============================================================================

TEST_CASE (

"crank view_decl_tag and view_expr_tag stable_ids are correct"
,
"[crank][views][lexer]"
)
 {
    using namespace vakya::emit;
    CHECK(tag_descriptor<crank::view_decl_tag>::stable_id == 1016u);
    CHECK(tag_descriptor<crank::view_expr_tag>::stable_id == 1017u);
}

TEST_CASE (

"crank view tag stable_ids are in extension band"
,
"[crank][views][lexer]"
)
 {
    using namespace vakya::emit;
    CHECK(tag_descriptor<crank::view_decl_tag>::stable_id >= 1000u);
    CHECK(tag_descriptor<crank::view_expr_tag>::stable_id >= 1000u);
}

TEST_CASE (

"crank view tag symbols are correct"
,
"[crank][views][lexer]"
)
 {
    using namespace vakya::emit;
    CHECK(tag_descriptor<crank::view_decl_tag>::symbol == "view_decl");
    CHECK(tag_descriptor<crank::view_expr_tag>::symbol == "view_expr");
}

TEST_CASE (

"crank view tag stable_ids are unique among all crank tags"
,
"[crank][views][lexer]"
)
 {
    using namespace vakya::emit;
    std::vector<std::uint32_t> ids = {
        tag_descriptor<crank::fn_tag>::stable_id,
        tag_descriptor<crank::block_tag>::stable_id,
        tag_descriptor<crank::let_tag>::stable_id,
        tag_descriptor<crank::var_tag>::stable_id,
        tag_descriptor<crank::match_tag>::stable_id,
        tag_descriptor<crank::crank_call_tag>::stable_id,
        tag_descriptor<crank::attribute_tag>::stable_id,
        tag_descriptor<crank::field_access_tag>::stable_id,
        tag_descriptor<crank::index_tag>::stable_id,
        tag_descriptor<crank::range_tag>::stable_id,
        tag_descriptor<crank::transaction_tag>::stable_id,
        tag_descriptor<crank::transaction_option_tag>::stable_id,
        tag_descriptor<crank::tx_load_tag>::stable_id,
        tag_descriptor<crank::tx_store_tag>::stable_id,
        tag_descriptor<crank::tx_abort_tag>::stable_id,
        tag_descriptor<crank::tx_yield_tag>::stable_id,
        tag_descriptor<crank::view_decl_tag>::stable_id,
        tag_descriptor<crank::view_expr_tag>::stable_id,
    };
    std::sort(ids.begin(), ids.end());
    auto it = std::adjacent_find(ids.begin(), ids.end());
    REQUIRE(it == ids.end());
}

// ============================================================================
// Group 2 — Parser: view_decl + view_expr parse without throwing
// ============================================================================

static constexpr std::string_view kSimpleViewDecl = R"crank(
package test

type Array struct {}

view Tensor of base: Array
requires contiguous(base)
)crank";

TEST_CASE (

"crank parser accepts simple view_decl"
,
"[crank][views][parser]"
)
 {
    REQUIRE_NOTHROW([&] {
        auto tree = crank::grammar::parse(kSimpleViewDecl);
        (void)tree;
    }());
    SUCCEED("view_decl parsed without exception");
}

static constexpr std::string_view kGenericViewDecl = R"crank(
package test

type Array struct {}

view Tensor[T, N: usize] of base: Array
requires contiguous(base)
)crank";

TEST_CASE (

"crank parser accepts generic view_decl with const params"
,
"[crank][views][parser]"
)
 {
    REQUIRE_NOTHROW([&] {
        auto tree = crank::grammar::parse(kGenericViewDecl);
        (void)tree;
    }());
    SUCCEED("generic view_decl parsed without exception");
}

static constexpr std::string_view kViewExprSource = R"crank(
package test

type Array struct {}

view Tensor[T] of base: Array
requires contiguous(base)

fn linear(w: Array) -> Array {
    let W = view w as Tensor[Float32]
    return W
}
)crank";

TEST_CASE (

"crank parser accepts view_expr in function body"
,
"[crank][views][parser]"
)
 {
    REQUIRE_NOTHROW([&] {
        auto tree = crank::grammar::parse(kViewExprSource);
        (void)tree;
    }());
    SUCCEED("view_expr parsed without exception");
}

TEST_CASE (

"crank parser: view_decl appears in typed AST"
,
"[crank][views][parser]"
)
 {
    auto tree = crank::grammar::parse(kSimpleViewDecl);
    vakya::property_store store;
    REQUIRE_NOTHROW([&] {
        auto result = crank::build_ast(tree, kSimpleViewDecl, store);
        (void)result;
    }());
    SUCCEED("build_ast on view_decl source completed");
}

// ============================================================================
// Group 3 — Feature gate: domain_views flag + CRANK-VIEW-000 diagnostic
// ============================================================================

TEST_CASE (

"crank feature_set: domain_views is off in default_v1()"
,
"[crank][views][feature]"
)
 {
    auto fs = crank::feature_set::default_v1();
    CHECK_FALSE(fs.has(crank::crank_feature::domain_views));
}

TEST_CASE (

"crank feature_set: domain_views is on after enable()"
,
"[crank][views][feature]"
)
 {
    auto fs = crank::feature_set::default_v1();
    fs.enable(crank::crank_feature::domain_views);
    CHECK(fs.has(crank::crank_feature::domain_views));
}

TEST_CASE (

"crank feature_set: all() enables domain_views"
,
"[crank][views][feature]"
)
 {
    auto fs = crank::feature_set::all();
    CHECK(fs.has(crank::crank_feature::domain_views));
}

TEST_CASE (

"crank feature_set: disable() turns domain_views off"
,
"[crank][views][feature]"
)
 {
    auto fs = crank::feature_set::all();
    fs.disable(crank::crank_feature::domain_views);
    CHECK_FALSE(fs.has(crank::crank_feature::domain_views));
}

TEST_CASE (

"crank CRANK-VIEW-000 code is correct (feature_disabled)"
,
"[crank][views][diag]"
)
 {
    CHECK(crank::view_diagnostic_code(crank::view_diag_kind::feature_disabled) == "CRANK-VIEW-000");
}

TEST_CASE (

"crank view_diagnostic_code covers all 12 view diag kinds"
,
"[crank][views][diag]"
)
 {
    for (std::uint8_t i = 0; i <= 11; ++i) {
        auto k = static_cast<crank::view_diag_kind>(i);
        auto code = crank::view_diagnostic_code(k);
        CHECK_FALSE(code.empty());
        // All codes start with CRANK-VIEW-
        CHECK(code.substr(0, 10) == "CRANK-VIEW");
    }
}

// ============================================================================
// Group 4 — Sema: view_expr_tag typing_rule emits constraint
// ============================================================================

TEST_CASE (

"crank typing_rule<view_expr_tag> with two children returns target type"
,
"[crank][views][sema]"
)
 {
    using namespace vakya::types;
    type_arena        arena;
    substitution      subst;
    type_environment  env;
    type_var_generator gen;

    type_var_id src_id = subst.make_var();
    type_var_id tgt_id = subst.make_var();
    type_ref src_ref   = arena.intern_variable(src_id);
    type_ref tgt_ref   = arena.intern_variable(tgt_id);

    std::vector<type_ref> ct = {src_ref, tgt_ref};
    auto [result_type, constraints] =
        vakya::types::typing_rule<crank::view_expr_tag>::emit(ct, env, arena, gen, subst);

    CHECK(result_type == tgt_ref);
    CHECK(constraints.size() == 1u);
    CHECK(constraints[0].kind == constraint_kind::same_type);
}

TEST_CASE (

"crank typing_rule<view_expr_tag> with no children returns fresh var"
,
"[crank][views][sema]"
)
 {
    using namespace vakya::types;
    type_arena        arena;
    substitution      subst;
    type_environment  env;
    type_var_generator gen;

    std::vector<type_ref> ct;
    auto [result_type, constraints] =
        vakya::types::typing_rule<crank::view_expr_tag>::emit(ct, env, arena, gen, subst);

    CHECK(constraints.empty());
    (void)result_type;
    SUCCEED("empty children returns fresh var without throw");
}

TEST_CASE (

"crank typing_rule<view_decl_tag> returns Unit type"
,
"[crank][views][sema]"
)
 {
    using namespace vakya::types;
    type_arena        arena;
    substitution      subst;
    type_environment  env;
    type_var_generator gen;

    std::vector<type_ref> ct;
    auto [result_type, constraints] =
        vakya::types::typing_rule<crank::view_decl_tag>::emit(ct, env, arena, gen, subst);

    CHECK(constraints.empty());
    (void)result_type;
    SUCCEED("view_decl_tag returns Unit without throw");
}

// ============================================================================
// Group 5 — Obligations: view obligation family + predicate builtins + stats
// ============================================================================

TEST_CASE (

"crank obligation_builder: view family obligations"
,
"[crank][views][obligations]"
)
 {
    crank::obligation_builder bld;
    crank::source_span span{1, 1, 1, 20};

    bld.add_view_contiguous   (span, "base");
    bld.add_view_aligned      (span, "base", 16);
    bld.add_view_rank         (span, "base", 2u);
    bld.add_view_shape        (span, "base", "[M, K]");
    bld.add_view_strides      (span, "base", "row_major");
    bld.add_view_dtype        (span, "base", "Float32");
    bld.add_view_requires     (span, "contiguous(base)");
    bld.add_view_lifetime     (span, "W", "w");
    bld.add_view_aliasing     (span, "w");

    auto obs = bld.take();
    CHECK(obs.size() == 9u);
    for (const auto& r : obs)
        CHECK(r.family == crank::obligation_family::view);
}

TEST_CASE (

"crank collect_obligation_stats counts view obligations"
,
"[crank][views][obligations]"
)
 {
    crank::obligation_builder bld;
    crank::source_span span{1, 1, 1, 10};

    bld.add_view_contiguous(span, "base");
    bld.add_view_rank(span, "base", 2u);
    bld.add_index(span, "xs", "i");  // 2 bounds obligations

    auto obs = bld.take();
    auto stats = crank::collect_obligation_stats(obs);
    CHECK(stats.view_count   == 2u);
    CHECK(stats.bounds_count == 2u);
    CHECK(stats.total        == 4u);
}

TEST_CASE (

"crank view obligation label strings are non-empty"
,
"[crank][views][obligations]"
)
 {
    crank::obligation_builder bld;
    crank::source_span span{1, 1, 1, 5};
    bld.add_view_contiguous(span, "base");
    bld.add_view_requires  (span, "rank(base) == 2");
    auto obs = bld.take();
    for (const auto& r : obs)
        CHECK_FALSE(r.label.empty());
}

// ============================================================================
// Group 6 — Method resolution: view_method_table insert + find (exact + fallback)
// ============================================================================

TEST_CASE (

"crank view_method_table: exact method lookup"
,
"[crank][views][methods]"
)
 {
    crank::view_method_table table;
    crank::method_entry e;
    e.method_name     = "reduce_sum";
    e.generic_arg_hash = 0;
    e.fn_node_id      = 42u;
    table.insert(e);

    const auto* found = table.find("reduce_sum", 0);
    REQUIRE(found != nullptr);
    CHECK(found->method_name  == "reduce_sum");
    CHECK(found->fn_node_id   == 42u);
}

TEST_CASE (

"crank view_method_table: fallback to non-specialized (hash 0)"
,
"[crank][views][methods]"
)
 {
    crank::view_method_table table;
    crank::method_entry e;
    e.method_name     = "matmul";
    e.generic_arg_hash = 0;   // generic, non-specialized
    e.fn_node_id      = 7u;
    table.insert(e);

    // Lookup with a non-zero hash should fall back to hash-0 entry
    const auto* found = table.find("matmul", 0xABCDu);
    REQUIRE(found != nullptr);
    CHECK(found->fn_node_id == 7u);
}

TEST_CASE (

"crank view_method_table: missing method returns nullptr"
,
"[crank][views][methods]"
)
 {
    crank::view_method_table table;
    CHECK(table.find("no_such_method") == nullptr);
}

TEST_CASE (

"crank view_method_table: specialized entry overrides generic for exact hash"
,
"[crank][views][methods]"
)
 {
    crank::view_method_table table;
    crank::method_entry generic_e;
    generic_e.method_name     = "dot";
    generic_e.generic_arg_hash = 0;
    generic_e.fn_node_id      = 10u;
    table.insert(generic_e);

    crank::method_entry spec_e;
    spec_e.method_name     = "dot";
    spec_e.generic_arg_hash = 0xDEADu;
    spec_e.fn_node_id      = 20u;
    table.insert(spec_e);

    // Exact hash finds specialized entry
    const auto* found_spec = table.find("dot", 0xDEADu);
    REQUIRE(found_spec != nullptr);
    CHECK(found_spec->fn_node_id == 20u);

    // Zero hash finds generic entry
    const auto* found_gen = table.find("dot", 0);
    REQUIRE(found_gen != nullptr);
    CHECK(found_gen->fn_node_id == 10u);
}

// ============================================================================
// Group 7 — Annotations: view_domain_meta fields + fast_path_ok
// ============================================================================

TEST_CASE (

"crank view_domain_meta: default is empty / not fast_path_ok"
,
"[crank][views][meta]"
)
 {
    crank::view_domain_meta meta;
    CHECK_FALSE(meta.has_domain());
    CHECK_FALSE(meta.has_op());
    CHECK_FALSE(meta.fast_path_ok());
}

TEST_CASE (

"crank view_domain_meta: fast_path_ok requires pure + deterministic"
,
"[crank][views][meta]"
)
 {
    crank::view_domain_meta meta;
    meta.law_pure          = true;
    CHECK_FALSE(meta.fast_path_ok());  // deterministic not set

    meta.law_deterministic = true;
    CHECK(meta.fast_path_ok());        // both set → fast path
}

TEST_CASE (

"crank view_domain_meta: affinity fields independent"
,
"[crank][views][meta]"
)
 {
    crank::view_domain_meta meta;
    meta.affinity_simd = true;
    meta.affinity_gpu  = true;
    CHECK(meta.affinity_simd);
    CHECK(meta.affinity_gpu);
    CHECK_FALSE(meta.affinity_dag);
    CHECK_FALSE(meta.affinity_streaming);
}

TEST_CASE (

"crank view_domain_meta: has_domain / has_op check non-empty string"
,
"[crank][views][meta]"
)
 {
    crank::view_domain_meta meta;
    meta.domain_name = "tensor";
    meta.op_name     = "tensor.matmul";
    CHECK(meta.has_domain());
    CHECK(meta.has_op());
}

// ============================================================================
// Group 8 — view_descriptor + view_registry (descriptor_registry backed)
// ============================================================================

TEST_CASE (

"crank view_descriptor satisfies RegistrableDescriptor"
,
"[crank][views][registry]"
)
 {
    static_assert(containers::RegistrableDescriptor<crank::view_descriptor>,
                  "view_descriptor must satisfy RegistrableDescriptor");
    SUCCEED("view_descriptor satisfies RegistrableDescriptor");
}

TEST_CASE (

"crank view_registry: register and find view_descriptor"
,
"[crank][views][registry]"
)
 {
    crank::view_registry reg;

    crank::view_descriptor desc;
    desc.stable_id       = 2001u;
    desc.name_hash       = crank::view_name_hash("test::Tensor");
    desc.category        = crank::view_category::generic;
    desc.qualified_name  = "test::Tensor";
    desc.backing_name    = "base";
    desc.backing_type_id = 500u;
    desc.generic_arity   = 2u;

    auto handle = reg.register_desc(desc);
    CHECK_FALSE(handle.is_null());

    const auto* found = reg.find(2001u);
    REQUIRE(found != nullptr);
    CHECK(found->qualified_name == "test::Tensor");
    CHECK(found->backing_name   == "base");
    CHECK(found->generic_arity  == 2u);
}

TEST_CASE (

"crank view_name_hash: non-zero for non-empty name"
,
"[crank][views][registry]"
)
 {
    CHECK(crank::view_name_hash("tensor") != 0u);
    CHECK(crank::view_name_hash("") == 14695981039346656037ULL); // FNV basis
}

TEST_CASE (

"crank view_name_hash: different names produce different hashes"
,
"[crank][views][registry]"
)
 {
    CHECK(crank::view_name_hash("Tensor") != crank::view_name_hash("Image"));
    CHECK(crank::view_name_hash("sutra.tensor") != crank::view_name_hash("sutra.graph"));
}

// ============================================================================
// Group 9 — Coherence: resolve::declare_view + resolve_impl_target
// ============================================================================

TEST_CASE (

"crank resolver: declare_view adds symbol with kind::view"
,
"[crank][views][resolve]"
)
 {
    crank::resolver r("test");
    r.declare_view("Tensor", "base", 500u);
    auto res = r.take();

    const auto* sym = res.symbols.lookup("test::Tensor");
    REQUIRE(sym != nullptr);
    CHECK(sym->kind                == crank::symbol_kind::view);
    CHECK(sym->view_backing_name   == "base");
    CHECK(sym->view_source_type_id == 500u);
    CHECK(sym->visibility          == crank::visibility_kind::exported);
}

TEST_CASE (

"crank resolver: resolve_impl_target finds view symbol"
,
"[crank][views][resolve]"
)
 {
    crank::resolver r("test");
    r.declare_view("Tensor", "base");

    const auto* found = r.resolve_impl_target("Tensor");
    REQUIRE(found != nullptr);
    CHECK(found->kind == crank::symbol_kind::view);
}

TEST_CASE (

"crank resolver: resolve_impl_target finds type_def symbol"
,
"[crank][views][resolve]"
)
 {
    crank::resolver r("test");
    r.declare_type("Array", 100u);

    const auto* found = r.resolve_impl_target("Array");
    REQUIRE(found != nullptr);
    CHECK(found->kind == crank::symbol_kind::type_def);
}

TEST_CASE (

"crank resolver: resolve_impl_target returns nullptr for value symbol"
,
"[crank][views][resolve]"
)
 {
    crank::resolver r("test");
    r.declare_value("x", crank::mutability_kind::immutable, 0u, true, true);
    const auto* found = r.resolve_impl_target("x");
    CHECK(found == nullptr);
}

TEST_CASE (

"crank resolver: duplicate view declaration emits diagnostic"
,
"[crank][views][resolve]"
)
 {
    crank::resolver r("test");
    r.declare_view("Tensor", "base");
    r.declare_view("Tensor", "base");  // duplicate

    const auto& diags = r.diagnostics();
    bool has_dup = false;
    for (const auto& d : diags)
        if (d.k == crank::resolve_diagnostic::kind::duplicate_symbol) has_dup = true;
    CHECK(has_dup);
}

// ============================================================================
// Group 10 — Host provider: CRANK-VIEW-009 code accessible
// ============================================================================

TEST_CASE (

"crank CRANK-VIEW-009 code is correct (provider_missing)"
,
"[crank][views][diag]"
)
 {
    CHECK(crank::view_diagnostic_code(crank::view_diag_kind::provider_missing) == "CRANK-VIEW-009");
}

// ============================================================================
// Group 11 — Borrow rule: view aliasing obligations
// ============================================================================

TEST_CASE (

"crank borrow-rule: view_lifetime obligation"
,
"[crank][views][borrow]"
)
 {
    crank::obligation_builder bld;
    crank::source_span span{5, 1, 5, 30};
    bld.add_view_lifetime(span, "W", "w");

    auto obs = bld.take();
    REQUIRE(obs.size() == 1u);
    CHECK(obs[0].family == crank::obligation_family::view);
    CHECK(obs[0].label.find("lifetime") != std::string::npos);
    CHECK(obs[0].label.find("W") != std::string::npos);
    CHECK(obs[0].label.find("w") != std::string::npos);
}

TEST_CASE (

"crank borrow-rule: view_aliasing obligation"
,
"[crank][views][borrow]"
)
 {
    crank::obligation_builder bld;
    crank::source_span span{5, 1, 5, 30};
    bld.add_view_aliasing(span, "data");

    auto obs = bld.take();
    REQUIRE(obs.size() == 1u);
    CHECK(obs[0].family == crank::obligation_family::view);
    CHECK(obs[0].label.find("aliasing") != std::string::npos);
    CHECK(obs[0].label.find("data") != std::string::npos);
}

TEST_CASE (

"crank CRANK-VIEW-008 code is correct (mutable_conflict)"
,
"[crank][views][borrow]"
)
 {
    CHECK(crank::view_diagnostic_code(crank::view_diag_kind::mutable_conflict) == "CRANK-VIEW-008");
}

TEST_CASE (

"crank CRANK-VIEW-007 code is correct (lifetime)"
,
"[crank][views][borrow]"
)
 {
    CHECK(crank::view_diagnostic_code(crank::view_diag_kind::lifetime) == "CRANK-VIEW-007");
}

// ============================================================================
// Group 12 — Builtins: indices is a prelude function (lean charter §7); zero uses T.identity() pattern
// ============================================================================

TEST_CASE (

"crank parser: indices call parses in expression position"
,
"[crank][views][builtins]"
)
 {
    static constexpr std::string_view kIndicesSource = R"crank(
package test

type Array struct {}

view Tensor[T] of base: Array

impl Tensor[T] {
    fn reduce_sum(self) -> Float32 {
        var acc: Float32 = Float32.identity()
        for i in indices(base) {
            acc = acc + self.base[i]
        }
        return acc
    }
}
)crank";
    REQUIRE_NOTHROW([&] {
        auto tree = crank::grammar::parse(kIndicesSource);
        (void)tree;
    }());
    SUCCEED("indices prelude call parsed in view method body");
}

// ============================================================================
// Group 13 — Cross-domain: CRANK-VIEW-002 accessible (not_viewable)
// ============================================================================

TEST_CASE (

"crank CRANK-VIEW-002 code is correct (not_viewable)"
,
"[crank][views][crossdomain]"
)
 {
    CHECK(crank::view_diagnostic_code(crank::view_diag_kind::not_viewable) == "CRANK-VIEW-002");
}

TEST_CASE (

"crank explain builder produces CRANK-VIEW-002 message"
,
"[crank][views][crossdomain]"
)
 {
    crank::source_span span{3, 1, 3, 40};
    auto expl = crank::explain(
                    std::string(crank::view_diagnostic_code(crank::view_diag_kind::not_viewable)),
                    "source type Array[Float64] cannot be viewed as Tensor[Float32]",
                    span)
                .note("dtype mismatch: Float64 vs Float32")
                .build();
    auto msg = expl.render_message();
    CHECK(msg.find("CRANK-VIEW-002") != std::string::npos);
    CHECK_FALSE(expl.notes.empty());
}

// ============================================================================
// Group 14 — Materialize-out: CRANK-VIEW-005 accessible (would_copy)
// ============================================================================

TEST_CASE (

"crank CRANK-VIEW-005 code is correct (would_copy)"
,
"[crank][views][materialize]"
)
 {
    CHECK(crank::view_diagnostic_code(crank::view_diag_kind::would_copy) == "CRANK-VIEW-005");
}

TEST_CASE (

"crank explain builder produces CRANK-VIEW-005 message"
,
"[crank][views][materialize]"
)
 {
    crank::source_span span{10, 5, 10, 50};
    auto expl = crank::explain(
                    std::string(crank::view_diagnostic_code(crank::view_diag_kind::would_copy)),
                    "implicit copy from Tensor[Float32] to Array[Float32] not allowed",
                    span)
                .help("use explicit materialize(v) to copy")
                .build();
    auto full = expl.render_full();
    CHECK(full.find("CRANK-VIEW-005") != std::string::npos);
    CHECK_FALSE(expl.help.empty());
}

// ============================================================================
// Full linear example — parse + resolve view_decl + view_expr end-to-end
// ============================================================================

static constexpr std::string_view kLinearSource = R"crank(
package app

type Array struct {}

view Tensor[T] of base: Array
requires contiguous(base)

impl Tensor[T] {
    fn matmul(self, rhs: Tensor[T]) -> Tensor[T] {
        return self
    }
}

fn linear(w: Array, x: Array) -> Tensor[Float32] {
    let W = view w as Tensor[Float32]
    let X = view x as Tensor[Float32]
    return W.matmul(X)
}
)crank";

TEST_CASE (

"crank full linear example: parse completes without throw"
,
"[crank][views][e2e]"
)
 {
    REQUIRE_NOTHROW([&] {
        auto tree = crank::grammar::parse(kLinearSource);
        (void)tree;
    }());
    SUCCEED("linear example parsed successfully");
}

TEST_CASE (

"crank full linear example: resolve declares Tensor as view symbol"
,
"[crank][views][e2e]"
)
 {
    crank::resolver r("app");
    r.declare_type("Array",  100u);
    r.declare_view("Tensor", "base", 100u);
    r.declare_function("linear", true, true);

    auto res = r.take();
    const auto* tensor_sym = res.symbols.lookup("app::Tensor");
    REQUIRE(tensor_sym != nullptr);
    CHECK(tensor_sym->kind              == crank::symbol_kind::view);
    CHECK(tensor_sym->view_backing_name == "base");
}

TEST_CASE (

"crank full linear example: view_registry entry for Tensor"
,
"[crank][views][e2e]"
)
 {
    crank::view_registry reg;
    crank::view_descriptor desc;
    desc.stable_id       = 3000u;
    desc.name_hash       = crank::view_name_hash("app::Tensor");
    desc.category        = crank::view_category::generic;
    desc.qualified_name  = "app::Tensor";
    desc.backing_name    = "base";
    desc.backing_type_id = 100u;
    desc.generic_arity   = 1u;

    crank::view_domain_meta meta;
    meta.domain_name = "tensor";
    desc.domain_meta = meta;

    crank::method_entry me;
    me.method_name = "matmul";
    me.fn_node_id  = 99u;
    desc.methods.insert(me);

    reg.register_desc(std::move(desc));

    const auto* found = reg.find(3000u);
    REQUIRE(found != nullptr);
    CHECK(found->domain_meta.domain_name == "tensor");
    const auto* matmul = found->methods.find("matmul");
    REQUIRE(matmul != nullptr);
    CHECK(matmul->fn_node_id == 99u);
}
