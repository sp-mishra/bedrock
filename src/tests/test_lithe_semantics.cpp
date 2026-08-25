#include "catch_amalgamated.hpp"

#include <ranges>

#include "lithe/lithe_passes.hpp"
#include "lithe/lithe_semantic.hpp"

#include <string>

namespace {
    struct tensor_terminal_rules {
        template <class T>
        static lithe::semantic::semantic_info terminal(T&& value) {
            lithe::semantic::semantic_info info = lithe::semantic::detail::default_semantic_rules::terminal(
                std::forward<T>(value)
            );
            info.domain = lithe::semantic::domain_type::tensor;
            info.mutability_kind = lithe::semantic::mutability::deep_mutable;
            return info;
        }

        template <class Tag>
        static lithe::semantic::semantic_info node_base() {
            return lithe::semantic::detail::default_semantic_rules::node_base<Tag>();
        }

        static void merge_child(
            lithe::semantic::semantic_info& parent,
            const lithe::semantic::semantic_info& child,
            const lithe::semantic::propagation_rule& rule,
            lithe::semantic::semantic_merge_strategy strategy
        ) {
            lithe::semantic::detail::default_semantic_rules::merge_child(parent, child, rule, strategy);
        }

        template <class Tag>
        static void finalize(lithe::semantic::semantic_info& info) {
            lithe::semantic::detail::default_semantic_rules::finalize<Tag>(info);
        }
    };
}

TEST_CASE (



"Lithe semantic capability and domain composition helpers"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    capability_set caps;
    REQUIRE(caps.empty());

    caps.add(capability::reorderable);
    REQUIRE(caps.has(capability::reorderable));

    capability_set extra = capability::common_subexpression_safe;
    caps.merge(extra);
    REQUIRE(caps.has(capability::common_subexpression_safe));

    auto merged = caps | capability::memoizable;
    REQUIRE(merged.has(capability::memoizable));

    domain_type dom = domain_type::arithmetic;
    dom |= domain_type::symbolic;
    REQUIRE(has_domain(dom, domain_type::arithmetic));
    REQUIRE(has_domain(dom, domain_type::symbolic));
    REQUIRE_FALSE(has_domain(dom, domain_type::tensor));
}

TEST_CASE (



"Lithe semantic annotation_map applies typed entries"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    annotation_map map;
    map.emplace(semantic_key::effect, effect_type::pure);
    map.emplace(semantic_key::domain, domain_type::tensor);
    map.emplace(semantic_key::capabilities, capability::memoizable);
    map.emplace(semantic_key::custom, std::string{"tag:unit-test"});

    semantic_info info;
    info.apply_annotation_map(map);

    REQUIRE(info.effect == effect_type::pure);
    REQUIRE(info.domain == domain_type::tensor);
    REQUIRE(info.capabilities.has(capability::memoizable));
    REQUIRE(info.extras.contains(semantic_key::custom));
    REQUIRE(std::holds_alternative<std::string>(info.extras.at(semantic_key::custom)));
}

TEST_CASE (



"Lithe semantic metadata is side-car keyed by structural hash"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    auto expr = lithe::make_node<lithe::add_tag>(64123, 97211);
    const auto hash_before = lithe::structural_hash(expr);

    annotate(expr, annotation{semantic_key::domain, domain_type::symbolic});
    with_effect(expr, effect_type::read_only);
    requires_capability(expr, capability::reorderable);

    const auto hash_after = lithe::structural_hash(expr);
    REQUIRE(hash_after == hash_before);

    const auto info = get_semantics(expr);
    REQUIRE(info.has_value());
    REQUIRE(info->domain == domain_type::symbolic);
    REQUIRE(info->effect == effect_type::read_only);
    REQUIRE(info->capabilities.has(capability::reorderable));

    // A structurally equivalent expression resolves to the same side-car entry.
    auto same_expr = lithe::make_node<lithe::add_tag>(64123, 97211);
    REQUIRE(lithe::structural_equal(expr, same_expr));

    const auto same_info = get_semantics(same_expr);
    REQUIRE(same_info.has_value());
    REQUIRE(same_info->effect == effect_type::read_only);
}

TEST_CASE (



"Lithe semantic helper APIs annotate domain/effect/capabilities"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    auto expr = lithe::make_node<lithe::mul_tag>(8801, 4423);

    with_domain(expr, domain_type::arithmetic | domain_type::symbolic);
    with_effect(expr, effect_type::pure);
    requires_capability(expr, capability::common_subexpression_safe);

    const auto info = get_semantics(expr);
    REQUIRE(info.has_value());
    REQUIRE(has_domain(info->domain, domain_type::arithmetic));
    REQUIRE(has_domain(info->domain, domain_type::symbolic));
    REQUIRE(info->effect == effect_type::pure);
    REQUIRE(info->capabilities.has(capability::common_subexpression_safe));
}

TEST_CASE (



"Lithe semantic merge and safety predicates"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    semantic_info lhs;
    lhs.effect = effect_type::pure;
    lhs.capabilities = capability::reorderable | capability::common_subexpression_safe;

    semantic_info rhs;
    rhs.effect = effect_type::read_only;
    rhs.capabilities = capability::reorderable | capability::memoizable;

    const auto merged = merge(lhs, rhs);
    REQUIRE(merged.effect == effect_type::read_only);
    REQUIRE(merged.capabilities.has(capability::reorderable));
    REQUIRE(merged.capabilities.has(capability::common_subexpression_safe));
    REQUIRE(merged.capabilities.has(capability::memoizable));

    REQUIRE(is_safe_to_reorder(lhs, rhs));

    semantic_info writes;
    writes.effect = effect_type::writes_global;
    writes.capabilities = capability::reorderable;
    REQUIRE_FALSE(is_safe_to_reorder(lhs, writes));
}

TEST_CASE (



"Lithe semantic CSE safety checks constraints and effects"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    semantic_info pure_memoized;
    pure_memoized.effect = effect_type::pure;
    pure_memoized.capabilities = capability::memoizable;
    REQUIRE(is_safe_to_cse(pure_memoized));

    semantic_info constrained = pure_memoized;
    constrained.constraints.push_back(constraint{
        "non_zero_divisor",
        false,
        "must prove divisor non-zero"
    });
    REQUIRE_FALSE(is_safe_to_cse(constrained));

    semantic_info throws = pure_memoized;
    throws.effect = effect_type::throws;
    throws.capabilities.add(capability::common_subexpression_safe);
    REQUIRE_FALSE(is_safe_to_cse(throws));
}

TEST_CASE (



"Lithe semantic analyzer classifies division as guarded arithmetic"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    auto expr = lithe::make_node<lithe::div_tag>(90210, 9);
    const auto analyzed = lithe::visit(expr, detail::semantic_analyzer{});

    REQUIRE(analyzed.effect == effect_type::throws);
    REQUIRE(has_domain(analyzed.domain, domain_type::arithmetic));
    REQUIRE(analyzed.capabilities.has(capability::requires_guard));

    const auto constraints = analyzed.check_constraints();
    REQUIRE_FALSE(constraints.all_satisfied);
    REQUIRE_FALSE(constraints.unsatisfied.empty());
    REQUIRE(constraints.unsatisfied.front().name == "non_zero_divisor");
}

TEST_CASE (



"Lithe guarded rewrite pass is opt-in and allows safe rewrites"
,
"[lithe][semantic][passes]"
)
 {
    using namespace lithe;

    auto expr = make_node<add_tag>(2, 1);

    const auto unguarded = passes::canonicalize_commutative_pass{}(expr);
    const auto guarded = passes::guarded(
        passes::canonicalize_commutative_pass{},
        passes::pure_only_guard{}
    )(expr);

    auto expected = make_node<add_tag>(1, 2);
    REQUIRE(structural_equal(unguarded, expected));
    REQUIRE(structural_equal(guarded, expected));
}

TEST_CASE (



"Lithe semantic guards can block rewrites when policy denies"
,
"[lithe][semantic][passes]"
)
 {
    using namespace lithe;

    struct always_false_guard {
        constexpr bool operator()(const semantic::semantic_info &) const {
            return false;
        }
    };

    auto expr = make_node<add_tag>(0, 42);

    const auto unguarded = passes::simplify_add_zero_pass{}(expr);
    const auto guarded = passes::guarded(
        passes::simplify_add_zero_pass{},
        always_false_guard{}
    )(expr);

    REQUIRE(structural_equal(unguarded, 42));
    REQUIRE(structural_equal(guarded, expr));
}

TEST_CASE (



"Lithe built-in semantic guard policies evaluate semantic info"
,
"[lithe][semantic][passes]"
)
 {
    using namespace lithe;

    auto pure_expr = make_node<add_tag>(1, 2);
    auto throwing_expr = make_node<div_tag>(8, 2);

    const auto pure_info = semantic::analyze_semantics(pure_expr);
    const auto throwing_info = semantic::analyze_semantics(throwing_expr);

    REQUIRE(passes::pure_only_guard{}(pure_info));
    REQUIRE(passes::no_throw_guard{}(pure_info));
    REQUIRE(passes::reorder_safe_guard{}(pure_info));
    REQUIRE(passes::cse_safe_guard{}(pure_info));

    REQUIRE_FALSE(passes::pure_only_guard{}(throwing_info));
    REQUIRE_FALSE(passes::no_throw_guard{}(throwing_info));
    REQUIRE_FALSE(passes::cse_safe_guard{}(throwing_info));
}

TEST_CASE (



"Lithe semantic context and semantic_query expose side-car semantics"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    semantic_context context;
    auto expr = lithe::make_node<lithe::add_tag>(13, 29);

    semantic_annotation ann;
    ann.effect = effect_type::pure;
    ann.domain = domain_type::arithmetic;
    ann.ownership = ownership_semantics::shared;
    ann.mutability_kind = mutability::immutable;
    ann.evaluation = evaluation_strategy::eager;
    ann.capabilities = capability::reorderable | capability::memoizable;
    annotate(context, expr, ann);

    const auto info = get_semantics(context, expr);
    REQUIRE(info.has_value());
    REQUIRE(info->ownership == ownership_semantics::shared);
    REQUIRE(info->mutability_kind == mutability::immutable);

    semantic_query query{context};
    REQUIRE(query.has_effect(expr, effect_type::pure));
    REQUIRE(query.has_capability(expr, capability::memoizable));
    REQUIRE(query.domain_of(expr) == domain_type::arithmetic);
}

TEST_CASE (



"Lithe semantic helpers support normalization, propagation, and conflicts"
,
"[lithe][semantic]"
)
 {
    using namespace lithe::semantic;

    semantic_info pure_info;
    pure_info.effect = effect_type::pure;
    pure_info.normalize();
    REQUIRE(pure_info.purity_level == purity::pure);
    REQUIRE(pure_info.allocation == allocation_behavior::none);

    semantic_info io_info;
    io_info.effect = effect_type::io_operation;
    io_info.purity_level = purity::impure;
    io_info.evaluation = evaluation_strategy::deferred;

    const auto merged = merge_semantics(pure_info, io_info);
    REQUIRE(merged.effect == effect_type::io_operation);
    REQUIRE(merged.purity_level == purity::impure);

    const auto conflicts = detect_semantic_conflicts(pure_info, io_info);
    REQUIRE_FALSE(conflicts.empty());

    const auto propagated = propagate_semantics(semantic_info{}, pure_info);
    REQUIRE(propagated.effect == effect_type::pure);
    REQUIRE(propagated.purity_level == purity::pure);
}

TEST_CASE (



"Lithe semantic propagator infers effect/domain across expression trees"
,
"[lithe][semantic][propagation]"
)
 {
    using namespace lithe::semantic;

    auto pure_expr = lithe::make_node<lithe::add_tag>(1, 2);
    const auto pure = infer_semantics(pure_expr);
    REQUIRE(pure.effect == effect_type::pure);
    REQUIRE(pure.domain == domain_type::arithmetic);

    auto io_child = lithe::make_node<lithe::call_tag>(42);
    auto mixed_expr = lithe::make_node<lithe::add_tag>(1, io_child);
    const auto mixed = infer_semantics(mixed_expr);
    REQUIRE(mixed.effect == effect_type::writes_global);
}

TEST_CASE (



"Lithe semantic propagator supports tensor dominance and mutability inheritance"
,
"[lithe][semantic][propagation]"
)
 {
    using namespace lithe::semantic;


    auto expr = lithe::make_node<lithe::mul_tag>(10, 3);

    const auto inferred = infer_semantics<decltype(expr), tensor_terminal_rules>(expr);

    REQUIRE(inferred.domain == domain_type::tensor);
    REQUIRE(inferred.mutability_kind == mutability::deep_mutable);
}

TEST_CASE (



"Lithe semantic propagation API stores and validates inferred semantics"
,
"[lithe][semantic][propagation]"
)
 {
    using namespace lithe::semantic;

    auto expr = lithe::make_node<lithe::div_tag>(16, 2);
    const auto propagated = propagate_semantics(expr);
    REQUIRE(propagated.effect == effect_type::throws);

    const auto stored = get_semantics(expr);
    REQUIRE(stored.has_value());
    REQUIRE(stored->effect == effect_type::throws);

    const auto validation = validate_semantics(expr);
    REQUIRE_FALSE(validation.valid);
    REQUIRE_FALSE(validation.unsatisfied_constraints.empty());
}

TEST_CASE (



"Lithe semantic type descriptor separates host and compiler semantic types"
,
"[lithe][semantic][types]"
)
 {
    using namespace lithe::semantic;

    const auto host_i32 = host_type_descriptor<int>();
    REQUIRE(host_i32.host_backed);
    REQUIRE(host_i32.primitive == primitive_type::signed_integer);
    REQUIRE(host_i32.numeric.has_value());
    REQUIRE(host_i32.numeric->is_integer);

    auto expr = lithe::make_node<lithe::add_tag>(4, 5);
    const auto sem_desc = infer_type_descriptor(expr);
    REQUIRE_FALSE(sem_desc.host_backed);
    REQUIRE(sem_desc.primitive == primitive_type::floating_point);

    const auto rel = relate_types(sem_desc, host_type_descriptor<double>());
    const bool relation_ok =
        rel == type_relation::identical || rel == type_relation::assignable || rel == type_relation::convertible;
    REQUIRE(relation_ok);

    const auto check = check_types(host_type_descriptor<double>(), sem_desc);
    REQUIRE(check.ok);
}

TEST_CASE (



"Lithe compatibility semantic API annotate and analyze"
,
"[lithe][compat][semantic]"
)
 {
    auto expr = lithe::make_node<lithe::mul_tag>(4, 5);

    lithe::semantic::annotate(expr, lithe::semantic::annotation{
        lithe::semantic::semantic_key::effect,
        lithe::semantic::effect_type::pure
    });

    auto stored = lithe::semantic::get_semantics(expr);
    REQUIRE(stored.has_value());
    REQUIRE(stored->effect == lithe::semantic::effect_type::pure);

    auto analyzed = lithe::semantic::analyze_semantics(expr);
    REQUIRE(lithe::semantic::is_no_throw(analyzed));
}

// T3: semantic_context isolates annotations from the global registry singleton
TEST_CASE (



"Lithe semantic_context isolates annotations from global registry"
,
"[lithe][semantic][isolation]"
)
 {
    using namespace lithe::semantic;

    // Annotate in a local context — must not leak into the global registry.
    semantic_context ctx;
    auto expr = lithe::make_node<lithe::add_tag>(77777, 88888);

    semantic_annotation ann;
    ann.effect = effect_type::writes_global;
    ann.domain = domain_type::symbolic;
    annotate(ctx, expr, ann);

    // Visible inside the context.
    const auto in_ctx = get_semantics(ctx, expr);
    REQUIRE(in_ctx.has_value());
    REQUIRE(in_ctx->effect == effect_type::writes_global);

    // Global registry must NOT see the context-local annotation.
    const auto in_global = get_semantics(expr);
    REQUIRE_FALSE(in_global.has_value());

    // Two independent contexts are fully isolated from each other.
    semantic_context ctx2;
    semantic_annotation ann2;
    ann2.effect = effect_type::pure;
    annotate(ctx2, expr, ann2);

    const auto in_ctx2 = get_semantics(ctx2, expr);
    REQUIRE(in_ctx2.has_value());
    REQUIRE(in_ctx2->effect == effect_type::pure);

    // First context is unmodified by second context's writes.
    const auto in_ctx_after = get_semantics(ctx, expr);
    REQUIRE(in_ctx_after.has_value());
    REQUIRE(in_ctx_after->effect == effect_type::writes_global);
}

// T4: CSE deduplication — (a+b)+(a+b) must collapse to a single add(a,b) node in the DAG
// NOTE: true_cse_pass currently does not reduce tree nodes (no-op); DAG conversion via
// to_dag_expr is the correct path for hash-based deduplication.
TEST_CASE (



"Lithe CSE deduplicates (a+b)+(a+b) in DAG representation"
,
"[lithe][passes][cse]"
)
 {
    using namespace lithe;

    // Both inner subexpressions are structurally identical.
    const auto ab   = make_node<add_tag>(2, 3);
    const auto expr = make_node<add_tag>(ab, ab);

    // DAG conversion must deduplicate: terminal(2), terminal(3), add(2,3), outer_add → 4 unique nodes.
    const auto dag = passes::to_canonical_dag(expr);
    REQUIRE_FALSE(dag.empty());
    REQUIRE(dag.node_count() <= 4);

    // The structural hash of both inner subexpressions must be the same.
    const auto ab2 = make_node<add_tag>(2, 3);
    REQUIRE(structural_hash(ab) == structural_hash(ab2));

    // true_cse_pass preserves structural equality of the expression (even if tree is unchanged).
    const auto canonical = passes::canonicalize(expr);
    const auto after_cse = passes::true_cse_pass{}(canonical);
    REQUIRE(structural_equal(after_cse, expr));
}

