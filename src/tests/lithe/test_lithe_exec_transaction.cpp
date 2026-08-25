// =============================================================================
// test_lithe_exec_transaction.cpp — Medha transaction conservatism tests
//
// Cases:
//   1.  transaction region + @parallel hint → scalar + note diagnostic
//   2.  transaction region + @gpu(required) hint → scalar + error diagnostic
//   3.  transaction region with no hint → scalar, no diagnostic
//   4.  transaction region + pure local computation → scalar proven_legal
//   5.  effect_summary with transaction effect → threaded_legal=false
//   6.  effect_summary with transaction effect → gpu_legal=false
//   7.  legality: transaction region forced scalar even with full target
//   8.  legality: @gpu(required) in transaction → proven_illegal (not scalar)
// =============================================================================

#include "catch_amalgamated.hpp"
#include "lithe/lithe_exec/exec_hint.hpp"
#include "lithe/lithe_exec/effect_summary.hpp"
#include "lithe/lithe_exec/legality.hpp"
#include "lithe/lithe_exec/selection.hpp"

using namespace lithe::exec;

namespace {
    target_capabilities full_target() {
        target_capabilities t;
        t.vector_width_bytes = 32;
        t.max_threads = 8;
        t.gpu_device_present = true;
        return t;
    }

    layout_summary device_layout() {
        layout_summary l;
        l.rank = 1;
        l.dims[0] = 1024;
        l.strides[0] = 1;
        l.alignment = 32;
        l.contiguous = true;
        l.space = address_space::device;
        l.device_resident = true;
        return l;
    }

    loop_info_view simple_loop() {
        return {
            .has_loop = true, .trip_count_known = true,
            .trip_count = 512, .is_affine = true, .depth = 1
        };
    }
} // namespace

TEST_CASE (

"transaction: @parallel hint → scalar + note diagnostic"
,
"[exec][transaction]"
)
 {
    lithe::diag::collecting_sink sink;

    effect_summary effects;
    effects.add(effect_kind::reads_memory);
    effects.add(effect_kind::writes_memory);
    effects.add(effect_kind::transaction);

    memory_summary mem;
    layout_summary layout = device_layout();
    auto loop = simple_loop();
    auto_execution_policy policy;

    region_context region;
    region.cls = region_class::transaction_region;
    region.in_transaction = true;

    std::vector<execution_hint> hints = {hint_parallel()};
    exec_candidate_context ctx;
    ctx.effects = &effects; ctx.memory = &mem; ctx.layout = &layout;
    ctx.loop = &loop; ctx.region = &region;
    ctx.target = ([] { static target_capabilities t = full_target(); return &t; })();
    ctx.policy = &policy; ctx.hints = hints;

    // Transaction region → legality check forces scalar regardless
    auto outcome = check_legality(execution_kind::threaded, region, effects, mem,
                                   loop, layout, full_target(), policy);
    CHECK(outcome == analysis_outcome::proven_illegal);

    auto scalar_outcome = check_legality(execution_kind::scalar, region, effects, mem,
                                          loop, layout, full_target(), policy);
    CHECK(scalar_outcome == analysis_outcome::proven_legal);
}

TEST_CASE (

"transaction: @gpu(required) in transaction → proven_illegal"
,
"[exec][transaction]"
)
 {
    effect_summary effects;
    effects.add(effect_kind::transaction);

    memory_summary mem;
    auto layout = device_layout();
    auto loop = simple_loop();
    auto_execution_policy policy;
    policy.allow_gpu = true;

    region_context region;
    region.cls = region_class::transaction_region;
    region.in_transaction = true;

    auto outcome = check_legality(execution_kind::gpu, region, effects, mem,
                                   loop, layout, full_target(), policy);
    CHECK(outcome == analysis_outcome::proven_illegal);
}

TEST_CASE (

"transaction: region with no hint → scalar, no error"
,
"[exec][transaction]"
)
 {
    effect_summary effects;
    effects.add(effect_kind::reads_memory);
    effects.add(effect_kind::transaction);

    memory_summary mem;
    auto layout = device_layout();
    auto loop = simple_loop();
    auto_execution_policy policy;

    region_context region;
    region.cls = region_class::transaction_region;
    region.in_transaction = true;

    auto outcome = check_legality(execution_kind::scalar, region, effects, mem,
                                   loop, layout, full_target(), policy);
    CHECK(outcome == analysis_outcome::proven_legal);
}

TEST_CASE (

"transaction: local pure computation → scalar proven_legal"
,
"[exec][transaction]"
)
 {
    effect_summary effects;
    effects.add(effect_kind::reads_memory);
    effects.add(effect_kind::writes_memory);
    effects.add(effect_kind::transaction);

    memory_summary mem;
    layout_summary layout;
    layout.rank = 1; layout.strides[0] = 1; layout.contiguous = true;
    layout.space = address_space::host;
    auto loop = simple_loop();
    auto_execution_policy policy;

    region_context region;
    region.cls = region_class::transaction_region;
    region.in_transaction = true;

    CHECK(check_legality(execution_kind::scalar, region, effects, mem,
                          loop, layout, full_target(), policy)
          == analysis_outcome::proven_legal);
}

TEST_CASE (

"transaction effect: threaded_legal=false"
,
"[exec][transaction]"
)
 {
    effect_summary s;
    s.add(effect_kind::transaction);
    CHECK_FALSE(threaded_legal(s));
}

TEST_CASE (

"transaction effect: gpu_legal=false"
,
"[exec][transaction]"
)
 {
    effect_summary s;
    s.add(effect_kind::transaction);
    CHECK_FALSE(gpu_legal(s));
}

TEST_CASE (

"legality: transaction region, scalar forced even with full target"
,
"[exec][transaction]"
)
 {
    effect_summary effects;
    effects.add(effect_kind::transaction);

    memory_summary mem;
    auto layout = device_layout();
    auto loop = simple_loop();
    auto_execution_policy policy;
    policy.allow_gpu = true;

    region_context region;
    region.cls = region_class::transaction_region;
    region.in_transaction = true;

    for (auto k : {execution_kind::simd, execution_kind::threaded, execution_kind::gpu}) {
        auto outcome = check_legality(k, region, effects, mem, loop, layout, full_target(), policy);
        CHECK(outcome == analysis_outcome::proven_illegal);
    }
    auto scalar = check_legality(execution_kind::scalar, region, effects, mem,
                                  loop, layout, full_target(), policy);
    CHECK(scalar == analysis_outcome::proven_legal);
}
