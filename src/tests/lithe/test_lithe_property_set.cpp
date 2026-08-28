// =============================================================================
// test_lithe_property_set.cpp — Unit Tests: Property Set
//
// Verifies: include/edsl/lithe_property_set.hpp
//
// Cases:
//   1.  property_set: empty() before first set().
//   2.  property_set: size() == 0 before first set().
//   3.  property_set: set/get round-trip for int64_t.
//   4.  property_set: set/get round-trip for double.
//   5.  property_set: set/get round-trip for string_view.
//   6.  property_set: set/get round-trip for feature_vector*.
//   7.  property_set: get returns nullopt for absent key.
//   8.  property_set: get returns nullopt for type mismatch.
//   9.  property_set: overwrite existing key via second set().
//   10. property_set: erase removes key; has() returns false.
//   11. property_set: spill beyond kInlineSlots into overflow.
//   12. property_set: size() reflects both inline + overflow.
//   13. property_set: set/get round-trip in overflow region.
//   14. propagate_forward copies src into dst.
//   15. propagate_forward: src key overwrites dst key (src wins).
//   16. merge: a properties then b; b wins on conflict.
//   17. merge: dst cleared before merge.
//   18. property_set: has() true for present key, false for absent.
//   19. property_set: different domains, same id → independent keys.
//   20. property_set: for_each visits all slots.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_property_set.hpp"

namespace li = lithe::intelligence;
using pk = li::property_key;
using pd = li::property_domain;

// ---------------------------------------------------------------------------
TEST_CASE (


"property_set: empty() before first set"
,
"[property_set]"
)
 {
    li::property_set ps;
    REQUIRE(ps.empty());
}

TEST_CASE (


"property_set: size() == 0 before first set"
,
"[property_set]"
)
 {
    li::property_set ps;
    REQUIRE(ps.size() == 0);
}

TEST_CASE (


"property_set: set/get round-trip int64_t"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk key{pd::source, 1};
    ps.set(key, std::int64_t{42});
    auto v = ps.get<std::int64_t>(key);
    REQUIRE(v.has_value());
    REQUIRE(*v == 42);
    REQUIRE_FALSE(ps.empty());
}

TEST_CASE (


"property_set: set/get round-trip double"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk key{pd::tensor, 2};
    ps.set(key, 3.14);
    auto v = ps.get<double>(key);
    REQUIRE(v.has_value());
    REQUIRE(*v == Catch::Approx(3.14));
}

TEST_CASE (


"property_set: set/get round-trip string_view"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk key{pd::optimization, 10};
    constexpr std::string_view sv = "hello";
    ps.set(key, sv);
    auto v = ps.get<std::string_view>(key);
    REQUIRE(v.has_value());
    REQUIRE(*v == "hello");
}

TEST_CASE (


"property_set: set/get round-trip feature_vector*"
,
"[property_set]"
)
 {
    li::property_set ps;
    lithe::features::feature_vector fv;
    fv.append(1.0f);
    lithe::features::feature_vector* ptr = &fv;
    pk key{pd::ml, 100};
    ps.set(key, ptr);
    auto v = ps.get<lithe::features::feature_vector*>(key);
    REQUIRE(v.has_value());
    REQUIRE(*v == ptr);
}

TEST_CASE (


"property_set: get returns nullopt for absent key"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk key{pd::source, 99};
    auto v = ps.get<std::int64_t>(key);
    REQUIRE_FALSE(v.has_value());
}

TEST_CASE (


"property_set: get returns nullopt for type mismatch"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk key{pd::source, 5};
    ps.set(key, std::int64_t{1});
    auto v = ps.get<double>(key);  // wrong type
    REQUIRE_FALSE(v.has_value());
}

TEST_CASE (


"property_set: overwrite existing key"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk key{pd::source, 3};
    ps.set(key, std::int64_t{10});
    ps.set(key, std::int64_t{20});
    auto v = ps.get<std::int64_t>(key);
    REQUIRE(*v == 20);
    REQUIRE(ps.size() == 1);
}

TEST_CASE (


"property_set: erase removes key"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk key{pd::source, 7};
    ps.set(key, std::int64_t{5});
    REQUIRE(ps.has(key));
    ps.erase(key);
    REQUIRE_FALSE(ps.has(key));
    REQUIRE(ps.empty());
}

TEST_CASE (


"property_set: spill beyond kInlineSlots"
,
"[property_set]"
)
 {
    li::property_set ps;
    for (std::uint32_t i = 0; i < li::property_set::kInlineSlots + 2; ++i)
        ps.set(pk{pd::optimization, i}, std::int64_t{static_cast<long long>(i)});

    REQUIRE(ps.size() == li::property_set::kInlineSlots + 2);
}

TEST_CASE (


"property_set: size() reflects inline + overflow"
,
"[property_set]"
)
 {
    li::property_set ps;
    const std::size_t N = li::property_set::kInlineSlots + 4;
    for (std::uint32_t i = 0; i < N; ++i)
        ps.set(pk{pd::tensor, i}, std::int64_t{i});
    REQUIRE(ps.size() == N);
}

TEST_CASE (


"property_set: set/get round-trip in overflow region"
,
"[property_set]"
)
 {
    li::property_set ps;
    // Fill inline
    for (std::uint32_t i = 0; i < li::property_set::kInlineSlots; ++i)
        ps.set(pk{pd::source, i}, std::int64_t{0});
    // Now in overflow
    pk overflow_key{pd::source, 999};
    ps.set(overflow_key, std::int64_t{77});
    auto v = ps.get<std::int64_t>(overflow_key);
    REQUIRE(v.has_value());
    REQUIRE(*v == 77);
}

TEST_CASE (


"propagate_forward copies src into dst"
,
"[property_set]"
)
 {
    li::property_set src, dst;
    pk k1{pd::source, 1}, k2{pd::tensor, 2};
    src.set(k1, std::int64_t{10});
    src.set(k2, 2.5);

    li::propagate_forward(src, dst);
    REQUIRE(dst.get<std::int64_t>(k1) == std::optional<std::int64_t>{10});
    REQUIRE(dst.get<double>(k2).has_value());
}

TEST_CASE (


"propagate_forward: src overwrites dst on conflict"
,
"[property_set]"
)
 {
    li::property_set src, dst;
    pk key{pd::source, 1};
    dst.set(key, std::int64_t{1});
    src.set(key, std::int64_t{99});
    li::propagate_forward(src, dst);
    REQUIRE(*dst.get<std::int64_t>(key) == 99);
}

TEST_CASE (


"merge: b wins on key conflict"
,
"[property_set]"
)
 {
    li::property_set a, b, dst;
    pk key{pd::source, 5};
    a.set(key, std::int64_t{1});
    b.set(key, std::int64_t{2});
    li::merge(a, b, dst);
    REQUIRE(*dst.get<std::int64_t>(key) == 2);
}

TEST_CASE (


"merge: dst cleared before merge"
,
"[property_set]"
)
 {
    li::property_set a, b, dst;
    pk old_key{pd::optimization, 1};
    dst.set(old_key, std::int64_t{99});
    li::merge(a, b, dst);
    REQUIRE(dst.empty());
}

TEST_CASE (


"property_set: has() true/false"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk key{pd::ml, 42};
    REQUIRE_FALSE(ps.has(key));
    ps.set(key, 1.0);
    REQUIRE(ps.has(key));
}

TEST_CASE (


"property_set: different domains same id → independent"
,
"[property_set]"
)
 {
    li::property_set ps;
    pk k1{pd::source, 1}, k2{pd::tensor, 1};
    ps.set(k1, std::int64_t{10});
    ps.set(k2, std::int64_t{20});
    REQUIRE(*ps.get<std::int64_t>(k1) == 10);
    REQUIRE(*ps.get<std::int64_t>(k2) == 20);
    REQUIRE(ps.size() == 2);
}

TEST_CASE (


"property_set: for_each visits all slots"
,
"[property_set]"
)
 {
    li::property_set ps;
    const std::size_t N = 5;
    for (std::uint32_t i = 0; i < N; ++i)
        ps.set(pk{pd::source, i}, std::int64_t{i});

    std::size_t count = 0;
    ps.for_each([&](li::property_key, const li::property_value&) { ++count; });
    REQUIRE(count == N);
}
