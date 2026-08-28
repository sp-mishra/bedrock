// =============================================================================
// test_lithe_typed_entry.cpp — entry_lease vs invocation_guard (§4.6)
//
// Verifies:
//   • get_entry on interpreter produces a valid typed_entry.
//   • Entry lease keeps storage alive but does NOT count a frame.
//   • invocation_guard is raised only during the call; frame count returns to 0.
//   • The typed path constructs no invocation_request.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <atomic>
#include <functional>
#include <type_traits>

#include "lithe/lithe_execution/entry.hpp"

namespace ex = lithe::execution;

// ============================================================================
// entry_lease — storage pin without frame count increment
// ============================================================================

TEST_CASE (


"entry_lease: valid / invalid"
,
"[entry][lease]"
)
 {
    auto counter = ex::make_frame_counter();
    ex::entry_lease lease{counter, 1};

    REQUIRE(lease.valid());
    CHECK(lease.version() == 1);
    CHECK(counter->load() == 0);  // lease does NOT increment frame counter

    ex::entry_lease empty;
    REQUIRE_FALSE(empty.valid());
}

TEST_CASE (


"entry_lease: raise_guard increments, guard dtor decrements"
,
"[entry][lease]"
)
 {
    auto counter = ex::make_frame_counter();
    ex::entry_lease lease{counter};

    CHECK(counter->load() == 0);
    {
        auto guard = lease.raise_guard();
        CHECK(counter->load() == 1);
    }
    // After guard destruction, counter must be 0.
    CHECK(counter->load() == 0);

    // Lease is still valid after guard is gone.
    REQUIRE(lease.valid());
}

TEST_CASE (


"entry_lease: multiple sequential guards"
,
"[entry][lease]"
)
 {
    auto counter = ex::make_frame_counter();
    ex::entry_lease lease{counter};

    for (int i = 0; i < 5; ++i) {
        {
            auto g = lease.raise_guard();
            CHECK(counter->load() == 1);
        }
        CHECK(counter->load() == 0);
    }
}

// ============================================================================
// invocation_guard directly
// ============================================================================

TEST_CASE (


"invocation_guard: direct construction from atomic"
,
"[entry][guard]"
)
 {
    std::atomic<std::uint64_t> raw{0};
    {
        ex::invocation_guard g{raw};
        CHECK(raw.load() == 1);
        CHECK(g.active_frames() == 1);
    }
    CHECK(raw.load() == 0);
}

TEST_CASE (


"invocation_guard: construction from frame_counter_ref"
,
"[entry][guard]"
)
 {
    auto cref = ex::make_frame_counter();
    {
        ex::invocation_guard g{cref};
        CHECK(cref->load() == 1);
    }
    CHECK(cref->load() == 0);
}

// ============================================================================
// typed_entry<Sig> — direct typed call
// ============================================================================

TEST_CASE (


"typed_entry: valid and callable"
,
"[entry][typed_entry]"
)
 {
    auto counter = ex::make_frame_counter();
    ex::entry_lease lease{counter};

    bool called = false;
    std::function<std::int64_t(std::int64_t, std::int64_t)> fn =
        [&](std::int64_t a, std::int64_t b) -> std::int64_t {
            called = true;
            return a + b;
        };

    ex::typed_entry<std::int64_t(std::int64_t, std::int64_t)> entry{
        std::move(lease), std::move(fn)};

    REQUIRE(entry.valid());

    // Before call: frame count is 0.
    CHECK(entry.active_frames() == 0);

    auto result = entry(3, 4);
    CHECK(result == 7);
    CHECK(called);

    // After call: frame count returns to 0.
    CHECK(entry.active_frames() == 0);
}

TEST_CASE (


"typed_entry: guard raised only during call"
,
"[entry][typed_entry]"
)
 {
    auto counter = ex::make_frame_counter();
    ex::entry_lease lease{counter};

    std::uint64_t frames_during_call = 0;
    std::function<int(int)> fn = [&](int x) -> int {
        frames_during_call = counter->load();
        return x * 2;
    };

    ex::typed_entry<int(int)> entry{std::move(lease),
        std::function<int(int)>{std::move(fn)}};

    CHECK(counter->load() == 0);
    auto r = entry(5);
    CHECK(r == 10);
    CHECK(frames_during_call == 1);  // guard was 1 inside the call
    CHECK(counter->load() == 0);     // guard released after call
}

// ============================================================================
// Compile-time guard: typed path does NOT use invocation_request
// ============================================================================

// invocation_request lives in resource.hpp; typed_entry<Sig>::operator() signature
// takes Args... directly, never packing them into invocation_request.
// Verify by confirming operator() is callable with Args, not with invocation_request.
TEST_CASE (


"typed_entry: operator() takes typed args, not invocation_request"
,
"[entry][typed_entry]"
)
 {
    // compile-time: if this test file compiles, the typed path uses Args..., not invocation_request.
    // The lambda body is the callable; no invocation_request is ever constructed here.
    ex::entry_lease lease{ex::make_frame_counter()};
    ex::typed_entry<double(double, double)> entry{
        std::move(lease),
        std::function<double(double, double)>{[](double a, double b) { return a * b; }}
    };
    CHECK(entry(2.5, 4.0) == Catch::Approx(10.0));
}
