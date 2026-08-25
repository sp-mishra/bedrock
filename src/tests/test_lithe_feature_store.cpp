// =============================================================================
// test_lithe_feature_store.cpp — Unit Tests: Feature Store
//
// Verifies: include/edsl/lithe_feature_store.hpp
//
// Cases:
//   1.  feature_source to_string: all enum values produce non-empty strings.
//   2.  feature_snapshot::make: hash, dims, source, timestamp_ns populated.
//   3.  feature_snapshot::valid: returns false for default-constructed snapshot.
//   4.  feature_snapshot::valid: returns true for snapshot with hash + dims > 0.
//   5.  feature_store: put + get round-trip preserves hash and dims.
//   6.  feature_store: get on missing key returns nullopt.
//   7.  feature_store: put replaces existing entry for same hash.
//   8.  feature_store: evict removes an entry; subsequent get returns nullopt.
//   9.  feature_store: size() reflects inserted entries (approx).
//   10. feature_store: put(hash, fv, source) convenience overload works.
//   11. feature_store: global() returns the same instance on repeated calls.
//   12. feature_store: snapshot source field preserved through round-trip.
//   13. feature_store: feature_vector data preserved through round-trip.
//   14. feature_store: timestamp_ns is non-zero after make().
//   15. feature_store: multiple distinct hashes stored independently.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_feature_store.hpp"

namespace lf = lithe::features;

TEST_CASE (


"feature_source to_string returns non-empty for all values"
,
"[feature_store][feature_source]"
)
{
    REQUIRE(!lf::to_string(lf::feature_source::graph).empty());
    REQUIRE(!lf::to_string(lf::feature_source::expression).empty());
    REQUIRE(!lf::to_string(lf::feature_source::mir).empty());
    REQUIRE(!lf::to_string(lf::feature_source::runtime).empty());
    REQUIRE(!lf::to_string(lf::feature_source::combined).empty());
    REQUIRE(!lf::to_string(lf::feature_source::custom).empty());
}

TEST_CASE (


"feature_snapshot::make populates all fields"
,
"[feature_store][feature_snapshot]"
)
{
    lf::feature_vector fv;
    fv.append(1.0f);
    fv.append(2.0f);
    fv.append(3.0f);

    const std::uint64_t h = 0xDEADBEEF12345678ULL;
    auto snap = lf::feature_snapshot::make(h, fv, lf::feature_source::expression);

    REQUIRE(snap.hash == h);
    REQUIRE(snap.dims == 3u);
    REQUIRE(snap.source == lf::feature_source::expression);
    REQUIRE(snap.timestamp_ns != 0);
}

TEST_CASE (


"feature_snapshot::valid returns false for default-constructed"
,
"[feature_store][feature_snapshot]"
)
{
    lf::feature_snapshot snap{};
    REQUIRE(!snap.valid());
}

TEST_CASE (


"feature_snapshot::valid returns true when hash and dims set"
,
"[feature_store][feature_snapshot]"
)
{
    lf::feature_vector fv;
    fv.append(0.5f);
    auto snap = lf::feature_snapshot::make(42ULL, fv, lf::feature_source::graph);
    REQUIRE(snap.valid());
}

TEST_CASE (


"feature_store put + get round-trip preserves hash and dims"
,
"[feature_store]"
)
{
    lf::feature_store store{32};

    lf::feature_vector fv;
    fv.append(7.0f);
    fv.append(8.0f);

    const std::uint64_t h = 999ULL;
    store.put(h, lf::feature_snapshot::make(h, fv, lf::feature_source::graph));

    auto result = store.get(h);
    REQUIRE(result.has_value());
    REQUIRE(result->hash == h);
    REQUIRE(result->dims == 2u);
}

TEST_CASE (


"feature_store get on missing key returns nullopt"
,
"[feature_store]"
)
{
    lf::feature_store store{32};
    auto result = store.get(0xFFFFFFFFFFFFFFFFULL);
    REQUIRE(!result.has_value());
}

TEST_CASE (


"feature_store put replaces existing entry for same hash"
,
"[feature_store]"
)
{
    lf::feature_store store{32};

    const std::uint64_t h = 100ULL;

    lf::feature_vector fv1;
    fv1.append(1.0f);
    store.put(h, lf::feature_snapshot::make(h, fv1, lf::feature_source::mir));

    lf::feature_vector fv2;
    fv2.append(2.0f);
    fv2.append(3.0f);
    store.put(h, lf::feature_snapshot::make(h, fv2, lf::feature_source::runtime));

    auto result = store.get(h);
    REQUIRE(result.has_value());
    REQUIRE(result->dims == 2u);
    REQUIRE(result->source == lf::feature_source::runtime);
}

TEST_CASE (


"feature_store evict removes an entry"
,
"[feature_store]"
)
{
    lf::feature_store store{32};

    const std::uint64_t h = 555ULL;
    lf::feature_vector fv;
    fv.append(1.0f);
    store.put(h, fv, lf::feature_source::graph);

    REQUIRE(store.get(h).has_value());
    store.evict(h);
    REQUIRE(!store.get(h).has_value());
}

TEST_CASE (


"feature_store size reflects inserted entries"
,
"[feature_store]"
)
{
    lf::feature_store store{64};
    REQUIRE(store.size() == 0u);

    lf::feature_vector fv;
    fv.append(1.0f);
    store.put(1ULL, fv, lf::feature_source::graph);
    store.put(2ULL, fv, lf::feature_source::expression);

    REQUIRE(store.size() >= 1u); // at least one shard has entries
}

TEST_CASE (


"feature_store put(hash, fv, source) convenience overload stores snapshot"
,
"[feature_store]"
)
{
    lf::feature_store store{32};

    lf::feature_vector fv;
    fv.append(10.0f);
    const std::uint64_t h = 77ULL;
    store.put(h, fv, lf::feature_source::combined);

    auto result = store.get(h);
    REQUIRE(result.has_value());
    REQUIRE(result->source == lf::feature_source::combined);
    REQUIRE(result->dims == 1u);
}

TEST_CASE (


"feature_store global() returns same instance on repeated calls"
,
"[feature_store]"
)
{
    lf::feature_store& a = lf::feature_store::global();
    lf::feature_store& b = lf::feature_store::global();
    REQUIRE(&a == &b);
}

TEST_CASE (


"feature_store snapshot source preserved through round-trip"
,
"[feature_store]"
)
{
    lf::feature_store store{32};

    lf::feature_vector fv;
    fv.append(3.0f);
    const std::uint64_t h = 123ULL;
    store.put(h, fv, lf::feature_source::mir);

    auto result = store.get(h);
    REQUIRE(result.has_value());
    REQUIRE(result->source == lf::feature_source::mir);
}

TEST_CASE (


"feature_store feature_vector data preserved through round-trip"
,
"[feature_store]"
)
{
    lf::feature_store store{32};

    lf::feature_vector fv;
    for (int i = 0; i < 5; ++i) fv.append(static_cast<float>(i) * 0.5f);

    const std::uint64_t h = 456ULL;
    store.put(h, fv, lf::feature_source::expression);

    auto result = store.get(h);
    REQUIRE(result.has_value());
    REQUIRE(result->fv.size() == 5u);
    for (int i = 0; i < 5; ++i)
        REQUIRE(result->fv[static_cast<std::size_t>(i)] ==
                Catch::Approx(static_cast<float>(i) * 0.5f));
}

TEST_CASE (


"feature_snapshot timestamp_ns is non-zero after make"
,
"[feature_store][feature_snapshot]"
)
{
    lf::feature_vector fv;
    fv.append(1.0f);
    auto snap = lf::feature_snapshot::make(1ULL, fv, lf::feature_source::graph);
    REQUIRE(snap.timestamp_ns != 0);
}

TEST_CASE (


"feature_store multiple distinct hashes stored independently"
,
"[feature_store]"
)
{
    lf::feature_store store{64};

    lf::feature_vector fva;
    fva.append(1.0f);
    lf::feature_vector fvb;
    fvb.append(2.0f);
    fvb.append(3.0f);

    store.put(10ULL, fva, lf::feature_source::graph);
    store.put(20ULL, fvb, lf::feature_source::expression);

    auto ra = store.get(10ULL);
    auto rb = store.get(20ULL);
    REQUIRE(ra.has_value());
    REQUIRE(rb.has_value());
    REQUIRE(ra->dims == 1u);
    REQUIRE(rb->dims == 2u);
}
