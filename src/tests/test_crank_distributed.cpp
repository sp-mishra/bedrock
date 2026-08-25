// =============================================================================
// test_crank_distributed.cpp — §v2.10 distributed execution.
//
// Verifies:
//   1.  placement factories + is_local / to_string.
//   2.  spawn_remote (no adapter): local honored; non-local relaxes to local
//       with CRANK-DIST-001 and awaits to the real value.
//   3.  remote_future consume discipline: await consumes; detach suppresses drop.
//   4.  serialization_boundary: trivially-copyable is boundary-safe; a non-POD
//       payload is flagged boundary_unsafe but still runs local.
//   5.  spawn_remote (adapter): can_place true ⇒ honored; false ⇒ relaxed.
//   6.  retry_policy: retry>0 + non-replay-safe conflicts; replay-safe does not.
//   7.  resolve_tx_distribution: local always ok; shard/replicated need adapter.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/distributed.hpp"

#include <string>

using namespace crank;

// ============================================================================
// Test 1 — placement factories + is_local + to_string
// ============================================================================

TEST_CASE (

"v2.10 placement factories and predicates"
,
"[crank][distributed][v2]"
)
 {
    auto here = placement::local_here();
    CHECK(here.is_local());
    CHECK(here.kind == placement_kind::local);

    auto n = placement::on_node("node-3");
    CHECK_FALSE(n.is_local());
    CHECK(n.kind == placement_kind::node);
    CHECK(n.name == "node-3");

    auto g = placement::on_group("shard-a");
    CHECK(g.kind == placement_kind::group);
    CHECK(g.name == "shard-a");

    CHECK(std::string(to_string(placement_kind::local)) == "local");
    CHECK(std::string(to_string(placement_kind::node))  == "node");
    CHECK(std::string(to_string(placement_kind::group)) == "group");
}

// ============================================================================
// Test 2 — spawn_remote without adapter: local honored, non-local relaxes
// ============================================================================

TEST_CASE (

"v2.10 spawn_remote local is honored"
,
"[crank][distributed][v2]"
)
 {
    auto f = spawn_remote(placement::local_here(), [] { return 42; });
    CHECK(f.placement_honored());
    CHECK_FALSE(f.relaxed());
    CHECK(f.diag() == distributed_diag::ok);
    auto r = f.await();
    REQUIRE(r.has_value());
    CHECK(*r == 42);
}

TEST_CASE (

"v2.10 spawn_remote non-local relaxes to local with pulse"
,
"[crank][distributed][v2]"
)
 {
    auto f = spawn_remote(placement::on_node("gpu-box"), [] { return 7; });
    CHECK_FALSE(f.placement_honored());
    CHECK(f.relaxed());
    CHECK(f.diag() == distributed_diag::relaxed_to_local);
    CHECK(std::string(to_string(f.diag())) == "CRANK-DIST-001");
    CHECK(f.placement_resolved().is_local());
    auto r = f.await();
    REQUIRE(r.has_value());
    CHECK(*r == 7);
}

// ============================================================================
// Test 3 — consume discipline
// ============================================================================

TEST_CASE (

"v2.10 remote_future await consumes; detach suppresses drop"
,
"[crank][distributed][v2]"
)
 {
    {
        auto f = spawn_remote(placement::local_here(), [] { return 1; });
        auto consumed = f.await();
        CHECK(consumed.has_value());
        CHECK(f.is_consumed());
        CHECK_FALSE(f.was_dropped());
    }
    {
        auto f = spawn_remote(placement::local_here(), [] { return 2; });
        f.detach();
        CHECK(f.is_consumed());
        CHECK_FALSE(f.was_dropped());
    }
}

// ============================================================================
// Test 4 — serialization_boundary
// ============================================================================

namespace {
    struct NonPod {
        std::string s; // not trivially copyable
    };
}

TEST_CASE (

"v2.10 serialization_boundary flags non-POD payloads"
,
"[crank][distributed][v2]"
)
 {
    CHECK(is_boundary_safe<int>);
    CHECK(is_boundary_safe<double>);
    CHECK_FALSE(is_boundary_safe<NonPod>);

    // A trivially-copyable payload requested local: ok.
    auto ok = spawn_remote(placement::local_here(), [] { return 3; });
    CHECK(ok.diag() == distributed_diag::ok);

    // A non-POD payload requested local: runs local but flagged boundary_unsafe.
    auto flagged = spawn_remote(placement::local_here(), [] { return NonPod{"x"}; });
    CHECK(flagged.diag() == distributed_diag::boundary_unsafe);
    CHECK(std::string(to_string(flagged.diag())) == "CRANK-DIST-002");
    auto r = flagged.await();
    REQUIRE(r.has_value());
    CHECK(r->s == "x");
}

// ============================================================================
// Test 5 — spawn_remote with an adapter
// ============================================================================

namespace {
    // Adapter that only accepts a single named node.
    struct OneNodeAdapter {
        std::string ok_node;

        [[nodiscard]] bool can_place(const placement& p) const {
            return p.kind == placement_kind::node && p.name == ok_node;
        }
    };
}

TEST_CASE (

"v2.10 adapter honors placeable requests, relaxes the rest"
,
"[crank][distributed][v2]"
)
 {
    static_assert(distributed_adapter<OneNodeAdapter>);
    OneNodeAdapter a{"node-3"};

    auto honored = spawn_remote(a, placement::on_node("node-3"), [] { return 10; });
    CHECK(honored.placement_honored());
    CHECK_FALSE(honored.relaxed());
    CHECK(honored.diag() == distributed_diag::ok);

    auto relaxed = spawn_remote(a, placement::on_node("node-9"), [] { return 20; });
    CHECK_FALSE(relaxed.placement_honored());
    CHECK(relaxed.relaxed());
    CHECK(relaxed.placement_resolved().is_local());

    // Adapter present but payload not boundary-safe ⇒ kept local, flagged.
    auto unsafe = spawn_remote(a, placement::on_node("node-3"), [] { return NonPod{"y"}; });
    CHECK(unsafe.diag() == distributed_diag::boundary_unsafe);
    CHECK(unsafe.placement_resolved().is_local());
}

// ============================================================================
// Test 6 — retry_policy
// ============================================================================

TEST_CASE (

"v2.10 retry_policy conflicts when non-replay-safe"
,
"[crank][distributed][v2]"
)
 {
    retry_policy none{};
    CHECK_FALSE(none.wants_retry());
    CHECK_FALSE(none.conflicts());

    retry_policy unsafe{3, false};
    CHECK(unsafe.wants_retry());
    CHECK(unsafe.conflicts());

    retry_policy safe{3, true};
    CHECK(safe.wants_retry());
    CHECK_FALSE(safe.conflicts());
}

// ============================================================================
// Test 7 — resolve_tx_distribution
// ============================================================================

TEST_CASE (

"v2.10 tx distribution gating"
,
"[crank][distributed][v2]"
)
 {
    auto loc = resolve_tx_distribution(tx_distribution::local, /*adapter=*/false);
    CHECK(loc.allowed);
    CHECK(loc.diag == distributed_diag::ok);

    auto shard_no = resolve_tx_distribution(tx_distribution::shard, false);
    CHECK_FALSE(shard_no.allowed);
    CHECK(shard_no.diag == distributed_diag::adapter_unavailable);
    CHECK(std::string(to_string(shard_no.diag)) == "CRANK-DIST-010");

    auto shard_yes = resolve_tx_distribution(tx_distribution::shard, true);
    CHECK(shard_yes.allowed);

    auto repl_yes = resolve_tx_distribution(tx_distribution::replicated, true);
    CHECK(repl_yes.allowed);

    CHECK(std::string(to_string(tx_distribution::local))      == "local");
    CHECK(std::string(to_string(tx_distribution::shard))      == "shard");
    CHECK(std::string(to_string(tx_distribution::replicated)) == "replicated");
}

// ============================================================================
// Test — hard vs soft placement (required/preferred, CRANK-DIST-003)
// ============================================================================

TEST_CASE (

"v2.10 required placement with no adapter is an error, never runs local"
,
"[crank][distributed][v2]"
)
 {
    auto f = spawn_remote(placement::on_node("gpu-box"), [] { return 99; },
                          placement_mode::required);
    CHECK(f.required_unmet());
    CHECK_FALSE(f.relaxed());
    CHECK(f.diag() == distributed_diag::required_unsatisfiable);
    CHECK(std::string(to_string(f.diag())) == "CRANK-DIST-003");
    // Required miss keeps the requested placement (does NOT downgrade to local).
    CHECK_FALSE(f.placement_resolved().is_local());
    auto r = f.await();
    CHECK_FALSE(r.has_value());   // no local fallback result
}

TEST_CASE (

"v2.10 preferred placement still relaxes to local (default mode)"
,
"[crank][distributed][v2]"
)
 {
    auto f = spawn_remote(placement::on_node("gpu-box"), [] { return 5; },
                          placement_mode::preferred);
    CHECK(f.relaxed());
    CHECK_FALSE(f.required_unmet());
    CHECK(f.diag() == distributed_diag::relaxed_to_local);
    auto r = f.await();
    REQUIRE(r.has_value());
    CHECK(*r == 5);
}

TEST_CASE (

"v2.10 required placement honored when adapter can place it"
,
"[crank][distributed][v2]"
)
 {
    struct YesAdapter {
        [[nodiscard]] bool can_place(const placement&) const noexcept { return true; }
    };
    YesAdapter a;
    auto f = spawn_remote(a, placement::on_node("node-3"), [] { return 3; },
                          placement_mode::required);
    CHECK_FALSE(f.required_unmet());
    CHECK(f.placement_honored());
    CHECK(f.diag() == distributed_diag::ok);
    auto r = f.await();
    REQUIRE(r.has_value());
    CHECK(*r == 3);
}

TEST_CASE (

"v2.10 required placement errors when adapter cannot place it"
,
"[crank][distributed][v2]"
)
 {
    struct NoAdapter {
        [[nodiscard]] bool can_place(const placement&) const noexcept { return false; }
    };
    NoAdapter a;
    auto f = spawn_remote(a, placement::on_node("node-3"), [] { return 3; },
                          placement_mode::required);
    CHECK(f.required_unmet());
    CHECK(std::string(to_string(f.diag())) == "CRANK-DIST-003");
    auto r = f.await();
    CHECK_FALSE(r.has_value());
}

TEST_CASE (

"v2.10 placement_mode to_string"
,
"[crank][distributed][v2]"
)
 {
    CHECK(std::string(to_string(placement_mode::preferred)) == "preferred");
    CHECK(std::string(to_string(placement_mode::required))  == "required");
}
