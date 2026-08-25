// =============================================================================
// test_lithe_pass_registry.cpp — pass_type_traits, bundle introspection,
//                                 and runtime pass_registry (imp-1, §2.2)
//
// Verifies:
//   1. pass_type_traits<> specializations for built-in passes.
//   2. stages_monotone<Bundle>() — true for valid ordering, false for inverted.
//   3. no_conflicts<Bundle>() — detects hand-built conflicting pair.
//   4. bundle_has_category<Bundle, C>() — true/false queries.
//   5. Runtime pass_registry: register, find by id, passes_in_category,
//      conflict rejection, stable_id band enforcement.
//   6. pass_registration_token RAII — unregisters on destruction.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <cstring>
#include <string_view>

#include "lithe/lithe_passes.hpp"
#include "lithe/lithe_execution/registry.hpp"

namespace p = lithe::passes;
namespace ex = lithe::execution;
namespace alg = lithe::algorithms;

// ============================================================================
//  Section 1 — pass_type_traits specializations (compile-time static_asserts)
// ============================================================================

TEST_CASE (


"pass_type_traits — built-in pass specializations"
,
"[lithe][pass_registry][traits]"
)
 {
    // constant_fold: canonical → optimized, category optimization
    static_assert(p::pass_type_traits<p::constant_fold_arith_pass>::category
                  == p::pass_category::optimization);
    static_assert(p::pass_type_traits<p::constant_fold_arith_pass>::in_stage
                  == p::ir_stage::canonical);
    static_assert(p::pass_type_traits<p::constant_fold_arith_pass>::out_stage
                  == p::ir_stage::optimized);
    static_assert(p::pass_type_traits<p::constant_fold_arith_pass>::stable_id == 12);

    // simplify_add_zero: canonical → canonical
    static_assert(p::pass_type_traits<p::simplify_add_zero_pass>::in_stage
                  == p::ir_stage::canonical);
    static_assert(p::pass_type_traits<p::simplify_add_zero_pass>::out_stage
                  == p::ir_stage::canonical);

    // dead_subtree: optimized → optimized
    static_assert(p::pass_type_traits<p::dead_subtree_elimination_pass>::in_stage
                  == p::ir_stage::optimized);
    static_assert(p::pass_type_traits<p::dead_subtree_elimination_pass>::out_stage
                  == p::ir_stage::optimized);

    // canonicalize_commutative: surface → canonical, category canonicalization
    static_assert(p::pass_type_traits<p::canonicalize_commutative_pass>::category
                  == p::pass_category::canonicalization);
    static_assert(p::pass_type_traits<p::canonicalize_commutative_pass>::in_stage
                  == p::ir_stage::surface);
    static_assert(p::pass_type_traits<p::canonicalize_commutative_pass>::out_stage
                  == p::ir_stage::canonical);

    // Default (unspecialized) type has stable_id == 0
    struct untagged_pass {};
    static_assert(p::pass_type_traits<untagged_pass>::stable_id == 0);

    SUCCEED("all static_asserts passed");
}

// ============================================================================
//  Section 2 — stages_monotone
// ============================================================================

// Passes at file scope (not anonymous namespace) so traits can be specialized.
struct my_canon_pass {
    template <class E>
    constexpr auto operator()(E&& e) const { return e; }
};

struct my_opt_pass {
    template <class E>
    constexpr auto operator()(E&& e) const { return e; }
};

// Regresses: optimized → canonical (invalid, out < in)
struct my_regress_pass {
    template <class E>
    constexpr auto operator()(E&& e) const { return e; }
};

template <>
struct p::pass_type_traits<my_canon_pass> : p::pass_type_traits_base {
    static constexpr auto id = lithe::fixed_string{"test.my_canon"};
    static constexpr p::ir_stage in_stage = p::ir_stage::canonical;
    static constexpr p::ir_stage out_stage = p::ir_stage::optimized;
    static constexpr std::size_t stable_id = 1001;
};

template <>
struct p::pass_type_traits<my_opt_pass> : p::pass_type_traits_base {
    static constexpr auto id = lithe::fixed_string{"test.my_opt"};
    static constexpr p::ir_stage in_stage = p::ir_stage::optimized;
    static constexpr p::ir_stage out_stage = p::ir_stage::optimized;
    static constexpr std::size_t stable_id = 1002;
};

template <>
struct p::pass_type_traits<my_regress_pass> : p::pass_type_traits_base {
    static constexpr auto id = lithe::fixed_string{"test.my_regress"};
    static constexpr p::ir_stage in_stage = p::ir_stage::optimized;
    static constexpr p::ir_stage out_stage = p::ir_stage::canonical; // regresses
    static constexpr std::size_t stable_id = 1003;
};

TEST_CASE (


"stages_monotone — valid and invalid orderings"
,
"[lithe][pass_registry][bundle]"
)
 {
    using d_canon   = p::pass_descriptor<100, my_canon_pass>;
    using d_opt     = p::pass_descriptor<101, my_opt_pass>;
    using d_regress = p::pass_descriptor<102, my_regress_pass>;

    // Valid: canonical→optimized then optimized→optimized
    using valid_bundle = p::pass_bundle<d_canon, d_opt>;
    static_assert(p::stages_monotone<valid_bundle>());

    // Invalid: regress pass outputs canonical from optimized input — regression
    using invalid_bundle = p::pass_bundle<d_regress, d_opt>;
    static_assert(!p::stages_monotone<invalid_bundle>());

    SUCCEED("stages_monotone checks passed");
}

// ============================================================================
//  Section 3 — no_conflicts
// ============================================================================

struct pass_a {};

struct pass_b {};

template <>
struct p::pass_type_traits<pass_a> : p::pass_type_traits_base {
    static constexpr auto id = lithe::fixed_string{"test.pass_a"};
    static constexpr std::size_t stable_id = 1010;
    // pass_a conflicts with pass_b (stable_id 1011)
    static constexpr std::array<std::size_t, 1> conflicts{1011};
};

template <>
struct p::pass_type_traits<pass_b> : p::pass_type_traits_base {
    static constexpr auto id = lithe::fixed_string{"test.pass_b"};
    static constexpr std::size_t stable_id = 1011;
    static constexpr std::array<std::size_t, 0> conflicts{};
};

TEST_CASE (


"no_conflicts — detects conflicting pair"
,
"[lithe][pass_registry][bundle]"
)
 {
    // Check: conflict bundle uses integer ids
    using da = p::pass_descriptor<200, pass_a>;
    using db = p::pass_descriptor<201, pass_b>;

    using conflict_bundle = p::pass_bundle<da, db>;
    static_assert(!p::no_conflicts<conflict_bundle>());

    using single_bundle = p::pass_bundle<da>;
    static_assert(p::no_conflicts<single_bundle>());

    SUCCEED("no_conflicts checks passed");
}

// ============================================================================
//  Section 4 — bundle_has_category
// ============================================================================

TEST_CASE (


"bundle_has_category — true/false queries"
,
"[lithe][pass_registry][bundle]"
)
 {
    using d_fold = p::pass_descriptor<300, p::constant_fold_arith_pass>;
    using d_dce  = p::pass_descriptor<301, p::dead_subtree_elimination_pass>;

    using opt_bundle = p::pass_bundle<d_fold, d_dce>;

    static_assert(p::bundle_has_category<opt_bundle, p::pass_category::optimization>());
    static_assert(!p::bundle_has_category<opt_bundle, p::pass_category::canonicalization>());
    static_assert(!p::bundle_has_category<opt_bundle, p::pass_category::analysis>());

    using d_comm = p::pass_descriptor<302, p::canonicalize_commutative_pass>;
    using mixed_bundle = p::pass_bundle<d_comm, d_fold>;

    static_assert(p::bundle_has_category<mixed_bundle, p::pass_category::canonicalization>());
    static_assert(p::bundle_has_category<mixed_bundle, p::pass_category::optimization>());

    SUCCEED("bundle_has_category checks passed");
}

// ============================================================================
//  Section 5 — runtime pass_registry
// ============================================================================

namespace {
    // Minimal dummy IR type for erasure tests
    struct dummy_ir {
        int value = 0;
    };

    // Passes must satisfy pass_for<P, dummy_ir>: p(analysis_manager&, dummy_ir&&) -> pass_result<dummy_ir>
    struct rt_pass_alpha {
        alg::pass_result<dummy_ir> operator()(alg::analysis_manager&, dummy_ir ir) const {
            return alg::pass_result<dummy_ir>(dummy_ir{ir.value + 1}, true);
        }
    };

    struct rt_pass_beta {
        alg::pass_result<dummy_ir> operator()(alg::analysis_manager&, dummy_ir ir) const {
            return alg::pass_result<dummy_ir>(dummy_ir{ir.value * 2}, true);
        }
    };
} // namespace

TEST_CASE (


"pass_registry — register, find, category query"
,
"[lithe][pass_registry][runtime]"
)
 {
    ex::pass_registry<dummy_ir> reg;
    REQUIRE(reg.empty());

    ex::pass_descriptor_runtime meta_a{};
    std::strncpy(meta_a.id, "test.rt.alpha", sizeof(meta_a.id) - 1);
    meta_a.stable_id   = 1100;
    meta_a.category_id = static_cast<std::uint8_t>(p::pass_category::optimization);

    auto tok_a = reg.register_pass(meta_a, alg::any_pass<dummy_ir>{rt_pass_alpha{}});
    REQUIRE(tok_a.has_value());
    REQUIRE(tok_a->valid());
    REQUIRE(reg.size() == 1);

    ex::pass_descriptor_runtime meta_b{};
    std::strncpy(meta_b.id, "test.rt.beta", sizeof(meta_b.id) - 1);
    meta_b.stable_id   = 1101;
    meta_b.category_id = static_cast<std::uint8_t>(p::pass_category::analysis);

    auto tok_b = reg.register_pass(meta_b, alg::any_pass<dummy_ir>{rt_pass_beta{}});
    REQUIRE(tok_b.has_value());
    REQUIRE(reg.size() == 2);

    // find by id
    auto found = reg.find("test.rt.alpha");
    REQUIRE(found.has_value());
    REQUIRE(found->meta->id_view() == "test.rt.alpha");

    // passes_in_category
    auto opts = reg.passes_in_category(
        static_cast<std::uint8_t>(p::pass_category::optimization));
    REQUIRE(opts.size() == 1);
    REQUIRE(opts[0].meta->id_view() == "test.rt.alpha");

    auto analyses = reg.passes_in_category(
        static_cast<std::uint8_t>(p::pass_category::analysis));
    REQUIRE(analyses.size() == 1);
}

TEST_CASE (


"pass_registry — conflict rejection"
,
"[lithe][pass_registry][runtime]"
)
 {
    ex::pass_registry<dummy_ir> reg;

    ex::pass_descriptor_runtime meta_x{};
    std::strncpy(meta_x.id, "test.rt.x", sizeof(meta_x.id) - 1);
    meta_x.stable_id     = 1200;
    meta_x.category_id   = 0;
    meta_x.conflicts[0]  = 1201;
    meta_x.conflict_count = 1;

    auto tok_x = reg.register_pass(meta_x, alg::any_pass<dummy_ir>{rt_pass_alpha{}});
    REQUIRE(tok_x.has_value());

    ex::pass_descriptor_runtime meta_y{};
    std::strncpy(meta_y.id, "test.rt.y", sizeof(meta_y.id) - 1);
    meta_y.stable_id   = 1201;
    meta_y.category_id = 0;

    // y's stable_id (1201) is in x's conflicts list → rejected
    auto tok_y = reg.register_pass(meta_y, alg::any_pass<dummy_ir>{rt_pass_beta{}});
    REQUIRE(!tok_y.has_value());
    REQUIRE(tok_y.error() == ex::pass_registry_error::conflict_detected);
}

TEST_CASE (


"pass_registry — stable_id band enforcement"
,
"[lithe][pass_registry][runtime]"
)
 {
    ex::pass_registry<dummy_ir> reg;

    ex::pass_descriptor_runtime meta{};
    std::strncpy(meta.id, "test.builtin_id", sizeof(meta.id) - 1);
    meta.stable_id = 42;  // in builtin band [0, 1000)

    auto tok = reg.register_pass(meta, alg::any_pass<dummy_ir>{rt_pass_alpha{}});
    REQUIRE(!tok.has_value());
    REQUIRE(tok.error() == ex::pass_registry_error::id_in_builtin_band);
}

TEST_CASE (


"pass_registration_token — RAII unregisters on destruction"
,
"[lithe][pass_registry][runtime]"
)
 {
    ex::pass_registry<dummy_ir> reg;

    ex::pass_descriptor_runtime meta{};
    std::strncpy(meta.id, "test.rt.raii", sizeof(meta.id) - 1);
    meta.stable_id = 1300;

    {
        auto tok = reg.register_pass(meta, alg::any_pass<dummy_ir>{rt_pass_alpha{}});
        REQUIRE(tok.has_value());
        REQUIRE(reg.size() == 1);

        auto found = reg.find("test.rt.raii");
        REQUIRE(found.has_value());
    }
    // Token destroyed — pass should be unregistered.
    REQUIRE(reg.empty());
    REQUIRE(!reg.find("test.rt.raii").has_value());
}

TEST_CASE (


"pass_registry — duplicate id rejection"
,
"[lithe][pass_registry][runtime]"
)
 {
    ex::pass_registry<dummy_ir> reg;

    ex::pass_descriptor_runtime meta{};
    std::strncpy(meta.id, "test.rt.dup", sizeof(meta.id) - 1);
    meta.stable_id = 1400;

    auto tok1 = reg.register_pass(meta, alg::any_pass<dummy_ir>{rt_pass_alpha{}});
    REQUIRE(tok1.has_value());

    ex::pass_descriptor_runtime meta2 = meta;
    meta2.stable_id = 1401;  // different stable_id but same string id
    auto tok2 = reg.register_pass(meta2, alg::any_pass<dummy_ir>{rt_pass_beta{}});
    REQUIRE(!tok2.has_value());
    REQUIRE(tok2.error() == ex::pass_registry_error::id_already_registered);
}
