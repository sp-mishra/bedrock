// =============================================================================
// test_lithe_exec_legality.cpp — Unit tests for lithe_exec/legality.hpp
//
// Cases:
//   1.  scalar always proven_legal
//   2.  GPU rejected on host_call effect
//   3.  GPU rejected on io effect
//   4.  GPU rejected when transaction region
//   5.  SIMD rejected on non-contiguous layout
//   6.  SIMD rejected when non-affine loop
//   7.  SIMD rejected when insufficient alignment
//   8.  threaded rejected on threaded_legal == false (host_call)
//   9.  threaded unknown when has_unknown_aliasing
//  10.  threaded unknown when loop-carried dep + unknown dep
//  11.  GPU unknown when layout.space == unknown
//  12.  GPU rejected when no device
//  13.  threaded proven_legal for independent affine loop
//  14.  policy.allow_gpu=false → proven_illegal for GPU
//  15.  hint forbid_gpu → proven_illegal for GPU (policy path)
// =============================================================================

#include "catch_amalgamated.hpp"
#include "lithe/lithe_exec/legality.hpp"

using namespace lithe::exec;

namespace {
    target_capabilities make_full_target() {
        target_capabilities t;
        t.vector_width_bytes = 32; // AVX2
        t.max_threads = 8;
        t.gpu_device_present = true;
        t.shared_memory_bytes = 48 * 1024;
        return t;
    }

    auto_execution_policy make_policy(bool gpu = true, bool threads = true, bool simd = true) {
        auto_execution_policy p;
        p.allow_gpu = gpu;
        p.allow_threads = threads;
        p.allow_simd = simd;
        return p;
    }

    effect_summary pure_rw_effects() {
        effect_summary e;
        e.add(effect_kind::reads_memory);
        e.add(effect_kind::writes_memory);
        return e;
    }

    layout_summary contiguous_aligned(std::uint32_t align = 32) {
        layout_summary l;
        l.rank = 1;
        l.dims[0] = 1024;
        l.strides[0] = 1;
        l.alignment = align;
        l.contiguous = true;
        l.space = address_space::host;
        return l;
    }

    loop_info_view known_loop(std::int64_t trip = 1024) {
        return {
            .has_loop = true, .trip_count_known = true,
            .trip_count = trip, .is_affine = true, .depth = 1
        };
    }

    region_context simple_region(region_class cls = region_class::independent_loop) {
        region_context r;
        r.cls = cls;
        return r;
    }

    memory_summary clean_memory() {
        memory_summary m;
        m.aliases.has_unknown_aliasing = false;
        return m;
    }
} // namespace

TEST_CASE (

"legality: scalar always proven_legal"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    auto loop = known_loop();
    auto region = simple_region();
    auto target = make_full_target();
    auto policy = make_policy();

    auto outcome = check_legality(execution_kind::scalar, region, effects, mem,
                                   loop, layout, target, policy);
    CHECK(outcome == analysis_outcome::proven_legal);
}

TEST_CASE (

"legality: GPU rejected on host_call effect"
,
"[exec][legality]"
)
 {
    effect_summary effects;
    effects.add(effect_kind::host_call);
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    layout.space = address_space::device;
    layout.device_resident = true;
    auto loop = known_loop();
    auto region = simple_region();
    auto target = make_full_target();
    auto policy = make_policy();

    auto outcome = check_legality(execution_kind::gpu, region, effects, mem,
                                   loop, layout, target, policy);
    CHECK(outcome == analysis_outcome::proven_illegal);
}

TEST_CASE (

"legality: GPU rejected on io effect"
,
"[exec][legality]"
)
 {
    effect_summary effects;
    effects.add(effect_kind::io);
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    layout.space = address_space::device;
    auto loop = known_loop();
    auto region = simple_region();
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::gpu, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_illegal);
}

TEST_CASE (

"legality: GPU rejected in transaction region"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    effects.add(effect_kind::transaction);
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    layout.space = address_space::device;
    auto loop = known_loop();
    region_context region;
    region.cls = region_class::transaction_region;
    region.in_transaction = true;
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::gpu, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_illegal);
}

TEST_CASE (

"legality: SIMD rejected on non-contiguous layout"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    layout_summary layout;
    layout.rank = 1; layout.strides[0] = 2; layout.contiguous = false;
    layout.alignment = 32;
    auto loop = known_loop();
    loop.is_affine = true;
    auto region = simple_region();
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::simd, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_illegal);
}

TEST_CASE (

"legality: SIMD rejected for non-affine loop"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    loop_info_view loop;
    loop.has_loop = true; loop.is_affine = false; loop.trip_count_known = true;
    auto region = simple_region();
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::simd, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_illegal);
}

TEST_CASE (

"legality: SIMD rejected for insufficient alignment"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    auto layout = contiguous_aligned(4); // only 4-byte aligned, AVX needs 32
    layout.strides[0] = 1;
    auto loop = known_loop();
    loop.is_affine = true;
    auto region = simple_region();
    auto target = make_full_target(); // vector_width=32
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::simd, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_illegal);
}

TEST_CASE (

"legality: threaded rejected for host_call effect"
,
"[exec][legality]"
)
 {
    effect_summary effects;
    effects.add(effect_kind::host_call);
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    auto loop = known_loop();
    auto region = simple_region();
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::threaded, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_illegal);
}

TEST_CASE (

"legality: threaded unknown when has_unknown_aliasing"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    memory_summary mem;
    mem.aliases.has_unknown_aliasing = true;
    auto layout = contiguous_aligned();
    auto loop = known_loop();
    auto region = simple_region();
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::threaded, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::unknown);
}

TEST_CASE (

"legality: threaded unknown when dep.has_unknown_dep"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    auto loop = known_loop();
    dependency_summary deps;
    deps.has_unknown_dep = true;
    region_context region;
    region.cls  = region_class::independent_loop;
    region.deps = &deps;
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::threaded, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::unknown);
}

TEST_CASE (

"legality: GPU unknown when address_space==unknown"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    layout_summary layout = contiguous_aligned();
    layout.space = address_space::unknown;
    auto loop = known_loop();
    auto region = simple_region();
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::gpu, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::unknown);
}

TEST_CASE (

"legality: GPU rejected when no device present"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    layout.space = address_space::device;
    auto loop = known_loop();
    auto region = simple_region();
    target_capabilities target = make_full_target();
    target.gpu_device_present = false;
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::gpu, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_illegal);
}

TEST_CASE (

"legality: threaded proven_legal for independent affine loop"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    auto loop = known_loop();
    auto region = simple_region(region_class::independent_loop);
    auto target = make_full_target();
    auto policy = make_policy();

    CHECK(check_legality(execution_kind::threaded, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_legal);
}

TEST_CASE (

"legality: policy.allow_gpu=false → proven_illegal for GPU"
,
"[exec][legality]"
)
 {
    auto effects = pure_rw_effects();
    auto mem = clean_memory();
    auto layout = contiguous_aligned();
    layout.space = address_space::device;
    layout.device_resident = true;
    auto loop = known_loop();
    auto region = simple_region();
    auto target = make_full_target();
    auto_execution_policy policy = make_policy(/*gpu=*/false);

    CHECK(check_legality(execution_kind::gpu, region, effects, mem,
                          loop, layout, target, policy) == analysis_outcome::proven_illegal);
}
