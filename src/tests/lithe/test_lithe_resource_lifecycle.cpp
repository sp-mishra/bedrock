// =============================================================================
// test_lithe_resource_lifecycle.cpp — eviction≠retirement sequence (§8.1, §11 P10)
//
// Verifies:
//   • Full retirement state machine: live→evicted→retiring→draining→unregistering→released.
//   • Active-frame pinning blocks release until frame_counter reaches 0.
//   • An outstanding execution_event counts like a live frame (outstanding_events).
//   • tick() advances records through the state machine.
//   • drain_all(timeout) returns true when all records retire; false on timeout.
//   • Kosha eviction (enqueue) sets state to evicted; driver owns steps 2–5.
//   • call_count_tiering_policy tiers correctly at threshold.
//   • tiering_driver::tick() returns tier_requests for versions over threshold.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "lithe/lithe_algorithms/lifecycle.hpp"
#include "lithe/lithe_execution/entry.hpp"   // frame_counter_ref, make_frame_counter

namespace alg = lithe::algorithms;
namespace ex = lithe::execution;

// ============================================================================
// §11 P10 retirement_record construction helpers
// ============================================================================

namespace {
    std::shared_ptr<alg::retirement_record> make_record(
        const std::uint64_t version_id = 1,
        ex::frame_counter_ref fc = nullptr,
        std::function<void()> unreg = nullptr,
        std::function<void()> release = nullptr) {
        auto rec = std::make_shared<alg::retirement_record>();
        rec->version_id = version_id;
        rec->frame_counter = std::move(fc);
        rec->unregister_metadata_fn = std::move(unreg);
        rec->release_resource_fn = std::move(release);
        return rec;
    }
} // namespace

// ============================================================================
// §11 P10 State machine: tick() drives records live→released
// ============================================================================

TEST_CASE (


"retirement_driver: tick advances evicted→retired with no active frames"
,
"[lifecycle][retirement]"
)
{
    alg::retirement_driver driver;
    CHECK(driver.idle());

    bool unreg_called   = false;
    bool release_called = false;

    auto rec = make_record(1, nullptr,
        [&] { unreg_called   = true; },
        [&] { release_called = true; });

    driver.enqueue(rec);
    CHECK(!driver.idle());
    CHECK(rec->state.load(std::memory_order_acquire)
          == alg::retirement_state::evicted);

    // tick 1: evicted → retiring
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::retiring);

    // tick 2: retiring → draining
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::draining);

    // tick 3: draining (no frames) → unregistering
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::unregistering);

    // tick 4: unregistering → released (calls both callbacks; removes from queue)
    std::size_t released = driver.tick();
    CHECK(released == 1u);
    CHECK(driver.idle());
    CHECK(unreg_called);
    CHECK(release_called);
    CHECK(rec->state.load() == alg::retirement_state::released);
}

TEST_CASE (


"retirement_driver: draining blocks while active frames > 0"
,
"[lifecycle][retirement][drain]"
)
{
    auto fc = ex::make_frame_counter();
    fc->store(2, std::memory_order_relaxed);  // 2 active frames

    auto rec = make_record(1, fc);
    alg::retirement_driver driver;
    driver.enqueue(rec);

    // Advance to draining state.
    driver.tick(); // evicted → retiring
    driver.tick(); // retiring → draining

    // Tick while frames are still live: stays in draining.
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::draining);
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::draining);

    // Release one frame — still 1 active.
    fc->fetch_sub(1, std::memory_order_relaxed);
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::draining);

    // Release last frame — drain completes.
    fc->fetch_sub(1, std::memory_order_relaxed);
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::unregistering);
}

// ============================================================================
// §4.3 outstanding execution_events count as live frames
// ============================================================================

TEST_CASE (


"retirement_driver: outstanding execution_events count as live frames"
,
"[lifecycle][retirement][events]"
)
{
    auto fc = ex::make_frame_counter();
    fc->store(0, std::memory_order_relaxed);   // no active stack frames

    auto rec = make_record(1, fc);
    rec->outstanding_events = 3;   // 3 outstanding async events

    alg::retirement_driver driver;
    driver.enqueue(rec);
    driver.tick(); // evicted → retiring
    driver.tick(); // retiring → draining

    // live_frame_count = fc(0) + events(3) = 3 → stays draining.
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::draining);

    // Simulate 2 events completing.
    rec->outstanding_events = 1;
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::draining);

    // Last event completes.
    rec->outstanding_events = 0;
    driver.tick();
    CHECK(rec->state.load() == alg::retirement_state::unregistering);
}

TEST_CASE (


"retirement_record::live_frame_count combines frame_counter and events"
,
"[lifecycle][retirement][live_frame_count]"
)
{
    auto fc = ex::make_frame_counter();
    fc->store(5, std::memory_order_relaxed);

    auto rec = make_record(1, fc);
    rec->outstanding_events = 3;

    CHECK(rec->live_frame_count() == 8u);
    CHECK(!rec->can_proceed_past_drain());

    fc->store(0, std::memory_order_relaxed);
    rec->outstanding_events = 0;
    CHECK(rec->live_frame_count() == 0u);
    CHECK(rec->can_proceed_past_drain());
}

// ============================================================================
// §11 P10 drain_all: completes when all records retire
// ============================================================================

TEST_CASE (


"retirement_driver: drain_all returns true when all records retire"
,
"[lifecycle][retirement][drain_all]"
)
{
    alg::retirement_driver driver;

    for (int i = 0; i < 5; ++i) {
        driver.enqueue(make_record(static_cast<std::uint64_t>(i)));
    }
    CHECK(driver.pending_count() == 5u);

    bool ok = driver.drain_all(std::chrono::milliseconds{500});
    CHECK(ok);
    CHECK(driver.idle());
}

TEST_CASE (


"retirement_driver: drain_all times out while frames are active"
,
"[lifecycle][retirement][drain_all][timeout]"
)
{
    auto fc = ex::make_frame_counter();
    fc->store(99, std::memory_order_relaxed);  // never drains

    auto rec = make_record(1, fc);
    alg::retirement_driver driver;
    driver.enqueue(rec);

    // Short timeout — should return false.
    bool ok = driver.drain_all(std::chrono::milliseconds{10});
    CHECK(!ok);
    CHECK(!driver.idle());
    CHECK(driver.pending_count() >= 1u);
}

// ============================================================================
// §11 P10 Multiple records: different drain timings
// ============================================================================

TEST_CASE (


"retirement_driver: multiple records with mixed drain states"
,
"[lifecycle][retirement]"
)
{
    auto fc1 = ex::make_frame_counter();
    fc1->store(0, std::memory_order_relaxed);  // will drain immediately
    auto fc2 = ex::make_frame_counter();
    fc2->store(1, std::memory_order_relaxed);  // blocks until manually released

    int rel1 = 0, rel2 = 0;
    auto rec1 = make_record(1, fc1, nullptr, [&] { ++rel1; });
    auto rec2 = make_record(2, fc2, nullptr, [&] { ++rel2; });

    alg::retirement_driver driver;
    driver.enqueue(rec1);
    driver.enqueue(rec2);
    CHECK(driver.pending_count() == 2u);

    // Run several ticks — rec1 should complete; rec2 stays draining.
    for (int i = 0; i < 8; ++i) driver.tick();
    CHECK(rel1 == 1);
    CHECK(rel2 == 0);

    // Now release rec2's frame.
    fc2->store(0, std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) driver.tick();
    CHECK(rel2 == 1);
    CHECK(driver.idle());
}

// ============================================================================
// §8.1 call_count_tiering_policy
// ============================================================================

TEST_CASE (


"call_count_tiering_policy: tiers interpret→jit_tier1 at threshold"
,
"[lifecycle][tiering]"
)
{
    alg::call_count_tiering_policy policy;
    policy.tier1_threshold = 50;
    policy.tier2_threshold = 500;

    using em = ex::execution_mode;

    CHECK(!policy.should_tier(1, em::interpret, 0));
    CHECK(!policy.should_tier(1, em::interpret, 49));
    CHECK( policy.should_tier(1, em::interpret, 50));
    CHECK( policy.should_tier(1, em::interpret, 100));

    CHECK(!policy.should_tier(1, em::jit_tier1, 0));
    CHECK(!policy.should_tier(1, em::jit_tier1, 499));
    CHECK( policy.should_tier(1, em::jit_tier1, 500));

    // jit_tier2 never tiers further.
    CHECK(!policy.should_tier(1, em::jit_tier2, 999999));
}

TEST_CASE (


"call_count_tiering_policy: target_tier selects correct next mode"
,
"[lifecycle][tiering]"
)
{
    alg::call_count_tiering_policy policy;
    using em = ex::execution_mode;

    CHECK(policy.target_tier(em::interpret)  == em::jit_tier1);
    CHECK(policy.target_tier(em::jit_tier1)  == em::jit_tier2);
    CHECK(policy.target_tier(em::jit_tier2)  == em::jit_tier2);
}

// ============================================================================
// §11 P10 tiering_driver
// ============================================================================

TEST_CASE (


"tiering_driver: tick() returns tier_requests above threshold"
,
"[lifecycle][tiering_driver]"
)
{
    alg::call_count_tiering_policy policy;
    policy.tier1_threshold = 10;
    alg::tiering_driver driver{policy};

    auto counter = std::make_shared<alg::profiling_counter>(
        1, ex::execution_mode::interpret);
    driver.register_counter(counter);
    CHECK(driver.counter_count() == 1u);

    // Below threshold: no requests.
    for (int i = 0; i < 9; ++i) counter->record_call();
    CHECK(driver.tick().empty());

    // At threshold: one request.
    counter->record_call();
    auto reqs = driver.tick();
    REQUIRE(reqs.size() == 1u);
    CHECK(reqs[0].version_id  == 1u);
    CHECK(reqs[0].target_mode == ex::execution_mode::jit_tier1);
}

TEST_CASE (


"tiering_driver: no_op_tiering_policy never tiers"
,
"[lifecycle][tiering_driver]"
)
{
    alg::tiering_driver<alg::no_op_tiering_policy> driver;
    auto counter = std::make_shared<alg::profiling_counter>(
        1, ex::execution_mode::interpret);
    driver.register_counter(counter);

    for (int i = 0; i < 10000; ++i) counter->record_call();
    CHECK(driver.tick().empty());
}

// ============================================================================
// §8.1 Lifecycle policy concept assertions
// ============================================================================

static_assert(alg::tiering_policy<alg::no_op_tiering_policy>);
static_assert(alg::tiering_policy<alg::call_count_tiering_policy>);
static_assert(alg::eviction_policy<alg::no_op_eviction_policy>);
static_assert(alg::retirement_policy<alg::no_op_retirement_policy>);
static_assert(std::is_empty_v<alg::default_lifecycle_policies>);
