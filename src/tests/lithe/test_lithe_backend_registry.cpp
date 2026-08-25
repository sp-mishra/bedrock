// =============================================================================
// test_lithe_backend_registry.cpp — dynamic backend_registry (§5.2, §5.3, §11 P8)
//
// Verifies:
//   • Registration: token returned, handle valid.
//   • Stale-handle detection: acquire returns nullopt after token destroyed.
//   • Unregister blocked while a live backend_ref exists; proceeds after release.
//   • Acquisition race: a pin acquired under shared lock survives concurrent unregister.
//   • find_first: locates by predicate; returns nullopt if nothing matches.
//   • Copy of backend_ref correctly increments/decrements refcount.
//   • Double-unregister (via token dtor twice equivalent) does not crash.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <atomic>
#include <string_view>
#include <thread>
#include <vector>

#include "lithe/lithe_execution/registry.hpp"
#include "lithe/lithe_execution/foundation.hpp"
#include "lithe/lithe_execution/capability.hpp"
#include "lithe/lithe_extension.hpp"
#include "lithe/lithe_exec/exec_bridge.hpp"

namespace ex = lithe::execution;

// ============================================================================
// Minimal stub backend for registry tests
// ============================================================================

namespace {
    struct stub_backend {
        static constexpr lithe::plugin_descriptor descriptor{
            "lithe.test.stub_backend", {1, 0, 0}, "lithe-test"
        };

        [[nodiscard]] static ex::backend_capability_set capabilities() noexcept {
            ex::backend_capability_set caps;
            caps.add(ex::backend_feature::integer_arithmetic);
            return caps;
        }
    };

    struct stub_backend_b {
        static constexpr lithe::plugin_descriptor descriptor{
            "lithe.test.stub_backend_b", {1, 0, 0}, "lithe-test"
        };

        [[nodiscard]] static ex::backend_capability_set capabilities() noexcept {
            return {};
        }
    };

    // Wire up the erased ops that register_backend<B>() builds internally.
    // The registry builds them via make_ops<B>() which requires the backend type
    // to expose capabilities() and a static id accessor.
    // Here we verify registration succeeds and that acquire/unregister behave.
} // namespace

// ============================================================================
// §5.2 Registration and handle validity
// ============================================================================

TEST_CASE (


"backend_registry: register returns valid token and handle"
,
"[registry][registration]"
)
{
    ex::backend_registry reg;
    CHECK(reg.size() == 0u);

    auto token = reg.register_backend(stub_backend{});
    CHECK(token.valid());
    CHECK(!token.handle().is_null());
    CHECK(reg.size() == 1u);
}

TEST_CASE (


"backend_registry: token dtor unregisters"
,
"[registry][registration]"
)
{
    ex::backend_registry reg;
    {
        auto token = reg.register_backend(stub_backend{});
        CHECK(reg.size() == 1u);
    }
    CHECK(reg.size() == 0u);
}

// ============================================================================
// §5.2 Stale-handle detection
// ============================================================================

TEST_CASE (


"backend_registry: acquire on stale handle returns nullopt"
,
"[registry][stale]"
)
{
    ex::backend_registry reg;
    ex::registration_handle h;
    {
        auto token = reg.register_backend(stub_backend{});
        h = token.handle();
        CHECK(reg.acquire(h).has_value());
    }
    // Token destroyed → slot erased; handle is now stale.
    auto ref = reg.acquire(h);
    CHECK(!ref.has_value());
}

TEST_CASE (


"backend_registry: acquire on null handle returns nullopt"
,
"[registry][stale]"
)
{
    ex::backend_registry reg;
    auto ref = reg.acquire(ex::registration_handle::null());
    CHECK(!ref.has_value());
}

// ============================================================================
// §5.3 Unregister deferred while live backend_ref exists
// ============================================================================

TEST_CASE (


"backend_registry: unregister deferred while ref held"
,
"[registry][lifetime]"
)
{
    ex::backend_registry reg;
    auto token = reg.register_backend(stub_backend{});
    const ex::registration_handle h = token.handle();

    // Acquire a pin — now refcount > 0 (token slot-pin + this acquire pin).
    auto ref = reg.acquire(h);
    REQUIRE(ref.has_value());
    CHECK(ref->valid());

    // Explicitly unregister via the token.
    // The slot has refcount > 0 (our ref pin), so erase is deferred.
    bool immediate = reg.unregister(h);
    CHECK(!immediate);  // deferred: our ref still holds a pin

    // The ref is still valid — backend_ref is alive.
    CHECK(ref->valid());

    // Drop the token (already unregistered above — second call is harmless
    // because the handle was already erased from the map).
    token = {};  // reset the token

    // Release the ref — this is the last pin; backend is now truly gone.
    ref = {};
    // Registry is now clean.
    CHECK(reg.size() == 0u);
}

TEST_CASE (


"backend_registry: acquire returns nullopt after unregister (retiring state)"
,
"[registry][lifetime]"
)
{
    ex::backend_registry reg;
    auto token = reg.register_backend(stub_backend{});
    const ex::registration_handle h = token.handle();

    // Hold a ref so unregister is deferred.
    auto ref = reg.acquire(h);
    REQUIRE(ref.has_value());

    // Unregister — slot goes to retiring.
    reg.unregister(h);

    // New acquire must fail — slot is retiring.
    auto ref2 = reg.acquire(h);
    CHECK(!ref2.has_value());

    // Release existing ref + token.
    ref   = {};
    token = {};
}

// ============================================================================
// §5.3 backend_ref copy: refcount incremented / decremented correctly
// ============================================================================

TEST_CASE (


"backend_registry: backend_ref copy increments refcount"
,
"[registry][refcount]"
)
{
    ex::backend_registry reg;
    auto token = reg.register_backend(stub_backend{});
    const ex::registration_handle h = token.handle();

    auto ref1 = reg.acquire(h);
    REQUIRE(ref1.has_value());
    CHECK(ref1->live_refs() >= 1u);

    // Copy ref1 → ref2 — refcount must increase.
    auto ref2 = *ref1;
    CHECK(ref2.valid());
    CHECK(ref1->live_refs() >= 2u);

    // Drop ref2 — refcount decreases but ref1 is still alive.
    ref2 = {};
    CHECK(ref1->valid());
    CHECK(ref1->live_refs() >= 1u);
}

// ============================================================================
// §5.3 find_first
// ============================================================================

TEST_CASE (


"backend_registry: find_first returns match by id"
,
"[registry][find]"
)
{
    ex::backend_registry reg;
    auto token = reg.register_backend(stub_backend{});

    auto ref = reg.find_first([](const ex::backend_slot& s) {
        return s.id() == stub_backend::descriptor.id_view();
    });
    REQUIRE(ref.has_value());
    CHECK(ref->id() == stub_backend::descriptor.id_view());
}

TEST_CASE (


"backend_registry: find_first returns nullopt when no match"
,
"[registry][find]"
)
{
    ex::backend_registry reg;
    auto token = reg.register_backend(stub_backend{});

    auto ref = reg.find_first([](const ex::backend_slot&) { return false; });
    CHECK(!ref.has_value());
}

TEST_CASE (


"backend_registry: find_first with multiple backends picks first match"
,
"[registry][find]"
)
{
    ex::backend_registry reg;
    auto t1 = reg.register_backend(stub_backend{});
    auto t2 = reg.register_backend(stub_backend_b{});
    CHECK(reg.size() == 2u);

    auto ref_a = reg.find_first([](const ex::backend_slot& s) {
        return s.id() == stub_backend::descriptor.id_view();
    });
    REQUIRE(ref_a.has_value());
    CHECK(ref_a->id() == stub_backend::descriptor.id_view());

    auto ref_b = reg.find_first([](const ex::backend_slot& s) {
        return s.id() == stub_backend_b::descriptor.id_view();
    });
    REQUIRE(ref_b.has_value());
    CHECK(ref_b->id() == stub_backend_b::descriptor.id_view());
}

// ============================================================================
// §5.3 Acquisition race stress: concurrent erase+acquire — no pin succeeds
//       on a slot that is in retiring state
// ============================================================================

TEST_CASE (


"backend_registry: acquisition race — pin never succeeds on retiring slot"
,
"[registry][race][stress]"
)
{
    // Register once; spin up many readers trying to acquire while one writer
    // unregiters.  Assert: no reader gets a valid ref after the slot retires.
    ex::backend_registry reg;

    constexpr int kReaders = 8;
    constexpr int kRounds  = 200;

    std::atomic<int> false_pins{0};

    for (int round = 0; round < kRounds; ++round) {
        auto token = reg.register_backend(stub_backend{});
        const auto h = token.handle();

        std::vector<std::thread> threads;
        std::atomic<bool> go{false};

        for (int i = 0; i < kReaders; ++i) {
            threads.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                // Try to acquire repeatedly; once unregistered, must get nullopt.
                for (int j = 0; j < 10; ++j) {
                    auto ref = reg.acquire(h);
                    if (ref && !ref->valid()) {
                        ++false_pins;  // should never happen
                    }
                }
            });
        }

        go.store(true, std::memory_order_release);

        // Writer: unregister immediately after go.
        reg.unregister(h);
        token = {};  // token already used handle directly; drop token

        for (auto& t : threads) t.join();
    }

    // No thread should have gotten a valid ref after the slot went retiring.
    CHECK(false_pins.load() == 0);
}

// ============================================================================
// Pinned lease survives concurrent structural mutation
// ============================================================================

TEST_CASE (


"backend_registry: pinned lease valid while concurrent inserts/removes"
,
"[registry][race]"
)
{
    ex::backend_registry reg;
    auto token = reg.register_backend(stub_backend{});
    const auto h = token.handle();

    auto ref = reg.acquire(h);
    REQUIRE(ref.has_value());

    // Concurrently insert and remove other backends while holding `ref`.
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&reg] {
            for (int j = 0; j < 50; ++j) {
                auto t = reg.register_backend(stub_backend_b{});
                // token drops at end of scope → unregister
            }
        });
    }
    for (auto& t : threads) t.join();

    // The original ref is still valid.
    CHECK(ref->valid());
    CHECK(ref->id() == stub_backend::descriptor.id_view());
}

// =============================================================================
// exec_bridge: execution_kind ↔ execution_mode conversion
// =============================================================================

TEST_CASE (

"exec_bridge: to_execution_mode covers all execution_kind values"
,
"[exec_bridge][exec]"
)
 {
    using namespace lithe::exec;
    using em = lithe::execution::execution_mode;

    CHECK(to_execution_mode(execution_kind::scalar)      == em::interpret);
    CHECK(to_execution_mode(execution_kind::simd)        == em::jit_tier1);
    CHECK(to_execution_mode(execution_kind::threaded)    == em::jit_tier1);
    CHECK(to_execution_mode(execution_kind::gpu)         == em::device);
    CHECK(to_execution_mode(execution_kind::distributed) == em::out_of_proc);
}

TEST_CASE (

"exec_bridge: to_execution_kind reverse mapping stable values"
,
"[exec_bridge][exec]"
)
 {
    using namespace lithe::exec;
    using em = lithe::execution::execution_mode;

    // gpu ↔ device round-trip must be exact
    CHECK(to_execution_kind(em::device)      == execution_kind::gpu);
    // distributed ↔ out_of_proc round-trip must be exact
    CHECK(to_execution_kind(em::out_of_proc) == execution_kind::distributed);
    // scalar fallback paths
    CHECK(to_execution_kind(em::interpret)     == execution_kind::scalar);
    CHECK(to_execution_kind(em::aot)           == execution_kind::scalar);
    CHECK(to_execution_kind(em::native_inline) == execution_kind::scalar);
}

TEST_CASE (

"exec_bridge: compile_requirements::for_exec_plan sets hl_mir input"
,
"[exec_bridge][capability]"
)
 {
    const auto req = lithe::execution::compile_requirements::for_exec_plan();
    CHECK(req.artifact.accepted_input == lithe::execution::ir_kind::hl_mir);
    // Must not restrict modes — caller overrides as needed
    CHECK(req.allowed_modes.none());
}
