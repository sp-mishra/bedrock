// =============================================================================
// test_lithe_schedule_bridge.cpp — Unit Tests: Schedule Bridge
//
// Verifies: include/edsl/lithe_schedule_bridge.hpp
//
// Cases:
//   1.  choose_schedule: empty mir_features + generic ctx → priority (safe default).
//   2.  choose_schedule: deep critical path ratio → critical_path.
//   3.  choose_schedule: high block/instruction ratio → work_stealing.
//   4.  choose_schedule: GPU backend id + loop_depth ≥ threshold → gpu.
//   5.  choose_schedule: vulkan backend id + loop_depth ≥ threshold → gpu.
//   6.  choose_schedule: non-GPU backend → not gpu.
//   7.  choose_schedule: GPU backend + loop_depth < threshold → not gpu.
//   8.  to_string: all schedule_policy_id values have non-empty string.
//   9.  choose_schedule returns a valid schedule_policy_id (no UB enum cast).
//   10. choose_schedule: zero instruction_count → safe default (no division by zero).
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_schedule_bridge.hpp"

namespace li = lithe::intelligence;
using spid = li::schedule_policy_id;

// ---------------------------------------------------------------------------
TEST_CASE (


"choose_schedule: empty features + generic ctx → priority or valid"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    lithe::cost::cost_context ctx;
    auto policy = li::choose_schedule(mf, ctx);
    // With no specific signals, expect priority as safe default
    REQUIRE(policy == spid::priority);
}

TEST_CASE (


"choose_schedule: deep critical path ratio → critical_path"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    mf.instruction_count = 100;
    mf.critical_path_len = 80;  // 80% > kCritPathRatio(0.5)
    lithe::cost::cost_context ctx;
    auto policy = li::choose_schedule(mf, ctx);
    REQUIRE(policy == spid::critical_path);
}

TEST_CASE (


"choose_schedule: high block ratio → work_stealing"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    mf.instruction_count = 100;
    mf.block_count       = 40;  // 40% > kParallelRatio(0.15)
    mf.critical_path_len = 0;   // no critical path pressure
    lithe::cost::cost_context ctx;
    auto policy = li::choose_schedule(mf, ctx);
    // work_stealing should win over priority here
    REQUIRE(policy == spid::work_stealing);
}

TEST_CASE (


"choose_schedule: GPU backend + loop_depth ≥ 2 → gpu"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    mf.loop_depth = 2;
    lithe::cost::cost_context ctx;
    ctx.backend_id = "lithe.gpu.vulkan";
    auto policy = li::choose_schedule(mf, ctx);
    REQUIRE(policy == spid::gpu);
}

TEST_CASE (


"choose_schedule: vulkan backend id → gpu"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    mf.loop_depth = 3;
    lithe::cost::cost_context ctx;
    ctx.backend_id = "lithe.vulkan";
    auto policy = li::choose_schedule(mf, ctx);
    REQUIRE(policy == spid::gpu);
}

TEST_CASE (


"choose_schedule: non-GPU backend → not gpu (unless other signals)"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    mf.loop_depth = 5;
    lithe::cost::cost_context ctx;
    ctx.backend_id = "lithe.interp";
    auto policy = li::choose_schedule(mf, ctx);
    REQUIRE(policy != spid::gpu);
}

TEST_CASE (


"choose_schedule: GPU backend + loop_depth < threshold → not gpu"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    mf.loop_depth = 1;  // < kGpuLoopDepth(2)
    lithe::cost::cost_context ctx;
    ctx.backend_id = "lithe.gpu";
    auto policy = li::choose_schedule(mf, ctx);
    REQUIRE(policy != spid::gpu);
}

TEST_CASE (


"to_string: all policy ids have non-empty string"
,
"[schedule_bridge]"
)
 {
    for (std::uint8_t i = 0; i <= 5; ++i) {
        auto s = li::to_string(static_cast<spid>(i));
        REQUIRE(!s.empty());
        REQUIRE(s != "unknown");
    }
}

TEST_CASE (


"choose_schedule: returns valid policy id"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    lithe::cost::cost_context ctx;
    auto p = li::choose_schedule(mf, ctx);
    const auto v = static_cast<std::uint8_t>(p);
    REQUIRE(v <= 5);
}

TEST_CASE (


"choose_schedule: zero instruction_count → safe default (no div by zero)"
,
"[schedule_bridge]"
)
 {
    lithe::features::mir_features mf{};
    mf.instruction_count = 0;
    mf.critical_path_len = 10;
    mf.block_count = 10;
    lithe::cost::cost_context ctx;
    REQUIRE_NOTHROW(li::choose_schedule(mf, ctx));
}
