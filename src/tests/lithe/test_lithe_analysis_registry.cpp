// =============================================================================
// test_lithe_analysis_registry.cpp — analysis registry: dual-index, require,
//   extension invalidation, pass_result::invalidated  (imp-2)
//
// Verifies:
//   1. Built-in fast path: require<A> computes once, second call served cached.
//   2. invalidate_except after a pass drops non-preserved built-ins (bitset).
//   3. Extension analysis: require computes, caches, re-serves.
//   4. Extension invalidation via explicit pass_result::invalidated.
//   5. analysis_key equality / structural usability (static_assert).
// =============================================================================

#include "catch_amalgamated.hpp"

#include <type_traits>

#include "lithe/lithe_algorithms/pipeline.hpp"

namespace al = lithe::algorithms;

// ============================================================================
// Minimal test IR
// ============================================================================

struct reg_ir {
    int value = 0;
};

// ============================================================================
// 1. Built-in analysis via analysis_descriptor — CfgAnalysis
// ============================================================================

struct CfgAnalysis {
    int cfg_data = 0;
};

template <>
struct al::analysis_descriptor<CfgAnalysis> {
    using result_t = CfgAnalysis;
    static constexpr al::analysis_id id = al::analysis_id::cfg;

    static CfgAnalysis compute(const reg_ir& ir, al::analysis_manager& /*am*/) {
        return CfgAnalysis{ir.value * 10};
    }
};

TEST_CASE (


"require<builtin>: computes once, serves cached"
,
"[analysis_registry][builtin]"
)
 {
    al::analysis_manager am;
    reg_ir ir{.value = 5};

    const auto& r1 = am.require<CfgAnalysis>(ir);
    CHECK(r1.cfg_data == 50);

    // Mutate ir so a second compute would differ — but require must not recompute.
    ir.value = 99;
    const auto& r2 = am.require<CfgAnalysis>(ir);
    CHECK(r2.cfg_data == 50);   // still 50: served from cache

    // Both references must point to the same cached object.
    CHECK(&r1 == &r2);
}

TEST_CASE (


"invalidate_except: drops non-preserved builtin, keeps preserved"
,
"[analysis_registry][builtin]"
)
{
    al::analysis_manager am;
    reg_ir ir{.value = 3};

    [[maybe_unused]] const auto& _ = am.require<CfgAnalysis>(ir);
    REQUIRE(am.has(al::analysis_id::cfg));

    // Also store dominator.
    am.store(al::analysis_id::dominator, 42);
    REQUIRE(am.has(al::analysis_id::dominator));

    // Preserve cfg only → dominator should be dropped.
    al::preserved_analysis_set pset;
    pset.set(al::analysis_id::cfg);
    am.invalidate_except(pset);

    CHECK(am.has(al::analysis_id::cfg));
    CHECK_FALSE(am.has(al::analysis_id::dominator));
}

// ============================================================================
// 3. Extension analysis — TensorShape
// ============================================================================

struct TensorShape {
    int rank = 0;
    int elems = 0;
};

namespace {
    // Extension key: stable_id in the >= 1000 band.
    inline constexpr al::analysis_key kTensorShapeKey = []() consteval {
        al::analysis_key k;
        // Fill domain: "tensor\0..."
        const char dom[] = "tensor";
        for (std::size_t i = 0; i < sizeof(dom); ++i) k.domain[i] = dom[i];
        const char nm[] = "shape";
        for (std::size_t i = 0; i < sizeof(nm); ++i) k.name[i] = nm[i];
        k.stable_id = 1000;
        return k;
    }();
}

template <>
struct al::analysis_descriptor<TensorShape> {
    using result_t = TensorShape;
    static constexpr al::analysis_key key = kTensorShapeKey;

    static TensorShape compute(const reg_ir& ir, al::analysis_manager& /*am*/) {
        return TensorShape{ir.value, ir.value * ir.value};
    }
};

TEST_CASE (


"require<extension>: computes, caches, re-serves"
,
"[analysis_registry][extension]"
)
 {
    al::analysis_manager am;
    reg_ir ir{.value = 4};

    const auto& s1 = am.require<TensorShape>(ir);
    CHECK(s1.rank  == 4);
    CHECK(s1.elems == 16);

    ir.value = 99;  // would change result if recomputed
    const auto& s2 = am.require<TensorShape>(ir);
    CHECK(s2.rank  == 4);   // cached
    CHECK(&s1 == &s2);
}

// ============================================================================
// 4. Extension invalidation via pass_result::invalidated
// ============================================================================

struct ext_dirty_pass {
    al::pass_result<reg_ir> operator()(al::analysis_manager& /*am*/, reg_ir ir) const {
        ir.value++;
        al::pass_result<reg_ir> r{std::move(ir), true, al::preserved_analysis_set::all()};
        r.invalidated.push_back(1000); // explicitly dirty TensorShape
        return r;
    }
};

struct ext_clean_pass {
    al::pass_result<reg_ir> operator()(al::analysis_manager& /*am*/, reg_ir ir) const {
        return al::pass_result<reg_ir>{std::move(ir), false, al::preserved_analysis_set::all()};
    }
};

TEST_CASE (


"pass_result::invalidated: explicitly drops extension analysis"
,
"[analysis_registry][extension]"
)
{
    al::analysis_manager am;
    reg_ir ir{.value = 2};

    [[maybe_unused]] const auto& _s1 = am.require<TensorShape>(ir);
    REQUIRE(am.has_ext(al::analysis_descriptor<TensorShape>::key));

    // ext_dirty_pass signals stable_id 1000 dirtied.
    al::static_pipeline<lithe::execution::no_pipeline_hooks, ext_dirty_pass> pipe{ext_dirty_pass{}};
    [[maybe_unused]] auto result = pipe.run(am, std::move(ir));

    CHECK_FALSE(am.has_ext(al::analysis_descriptor<TensorShape>::key));
}

TEST_CASE (


"pass_result::invalidated: preserving pass keeps extension"
,
"[analysis_registry][extension]"
)
{
    al::analysis_manager am;
    reg_ir ir{.value = 3};

    [[maybe_unused]] const auto& _s2 = am.require<TensorShape>(ir);
    REQUIRE(am.has_ext(al::analysis_descriptor<TensorShape>::key));

    // ext_clean_pass does NOT list stable_id 1000 → extension stays cached.
    al::static_pipeline<lithe::execution::no_pipeline_hooks, ext_clean_pass> pipe{ext_clean_pass{}};
    [[maybe_unused]] auto r2 = pipe.run(am, std::move(ir));

    CHECK(am.has_ext(al::analysis_descriptor<TensorShape>::key));
}

// ============================================================================
// 5. analysis_key equality / structural usability
// ============================================================================

TEST_CASE (


"analysis_key: equality and structural properties"
,
"[analysis_registry][key]"
)
 {
    constexpr al::analysis_key k1 = kTensorShapeKey;
    constexpr al::analysis_key k2 = kTensorShapeKey;

    static_assert(k1 == k2, "analysis_key equality must be constexpr");
    static_assert(std::is_aggregate_v<al::analysis_key>, "analysis_key must be aggregate");
    static_assert(std::is_trivially_copyable_v<al::analysis_key>, "analysis_key must be trivially copyable");

    CHECK(k1 == k2);
    CHECK(k1.stable_id == 1000u);
    CHECK(k1.domain_view() == "tensor");
    CHECK(k1.name_view()   == "shape");
}
