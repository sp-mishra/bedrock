// =============================================================================
// test_lithe_profiles.cpp — Optimization Profile Framework (imp-4)
//
// Verifies:
//   1. std_o3 profile output equals preset::O3{} output (alias parity).
//   2. profile_valid<std_o3>() true; invalid profile (missing dep) fails.
//   3. profile_inherit extends bundle + dedupes duplicate descriptor.
//   4. profile_descriptor id namespacing: "std.o3" distinct from "tensor.o3".
//   5. export_profile → profile_record → import_profile → dynamic_profile;
//      descriptor metadata preserved; pass_count matches.
//   6. Zero-overhead: sizeof(std_o3{}) == 1 (empty type).
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_passes.hpp"

#include <string_view>

namespace pf = lithe::profile;
namespace p = lithe::passes;

// ============================================================================
// Test 1 — alias parity: std_o3 output matches preset::O3{} on same expr
// ============================================================================

TEST_CASE(

    "profile std_o3 output matches preset::O3 (alias parity)",
    "[lithe][profile][alias]") {
  // add(0, mul(2, 1)) — constant-foldable with strength-reduction candidate
  auto expr1 = lithe::make_node<lithe::add_tag>(
      0, lithe::make_node<lithe::mul_tag>(2, 1));
  auto expr2 = lithe::make_node<lithe::add_tag>(
      0, lithe::make_node<lithe::mul_tag>(2, 1));

  auto out_preset = lithe::preset::O3{}(expr1);
  auto out_profile = pf::std_o3{}(expr2);

  REQUIRE(lithe::structural_hash(out_preset) ==
          lithe::structural_hash(out_profile));
}

TEST_CASE(

    "profile std_o1 output matches preset::O1 (alias parity)",
    "[lithe][profile][alias]") {
  auto expr1 = lithe::make_node<lithe::add_tag>(0, 7);
  auto expr2 = lithe::make_node<lithe::add_tag>(0, 7);

  auto out_preset = lithe::preset::O1{}(expr1);
  auto out_profile = pf::std_o1{}(expr2);

  REQUIRE(lithe::structural_hash(out_preset) ==
          lithe::structural_hash(out_profile));
}

// ============================================================================
// Test 2 — profile_valid: valid profiles pass, invalid (missing dep) fails
// ============================================================================

TEST_CASE(

    "profile_valid returns true for std built-in profiles",
    "[lithe][profile][valid]") {
  STATIC_REQUIRE(pf::profile_valid<pf::std_o1>());
  STATIC_REQUIRE(pf::profile_valid<pf::std_o2>());
  STATIC_REQUIRE(pf::profile_valid<pf::std_o3>());
}

// Profile with missing dependency — all_deps_present fails.
namespace {
using broken_bundle = p::pass_bundle<pf::desc_constant_fold>;

struct broken_profile {
  static constexpr pf::profile_descriptor descriptor{"test.broken"};
  using bundle = broken_bundle;
  using ordered = p::order_pass_bundle_t<broken_bundle>;
};
} // namespace

TEST_CASE(

    "profile_valid returns false for bundle with missing dependency",
    "[lithe][profile][valid]") {
  // desc_constant_fold depends on desc_simplify_add_zero and
  // desc_simplify_mul_identity which are absent from broken_bundle →
  // all_deps_present fails.
  STATIC_REQUIRE_FALSE(pf::profile_valid<broken_profile>());
}

// ============================================================================
// Test 3 — profile_inherit: extends base bundle, dedupes duplicates
// ============================================================================

namespace {
// Extra pass descriptor for inheritance test (standalone, no deps).
struct test_extra_pass {
  template <class E> constexpr auto operator()(E &&e) const {
    return std::forward<E>(e);
  }
};

using desc_test_extra = p::pass_descriptor<99, test_extra_pass>;

using extra_bundle = p::pass_bundle<desc_test_extra>;

// Inherited profile: std_o1 + extra_bundle
using extended_o1 = pf::profile_inherit<pf::std_o1, extra_bundle>;

// Duplicate test: inherit std_o1 again from extended_o1 —
// desc_simplify_add_zero and desc_simplify_mul_identity should not be
// duplicated.
using deduped_o1 =
    pf::profile_inherit<extended_o1, pf::o1_bundle, pf::k_std_o1_desc>;
} // namespace

TEST_CASE(

    "profile_inherit adds extra pass to bundle", "[lithe][profile][inherit]") {
  STATIC_REQUIRE(
      p::contains_pass_v<extended_o1::bundle, pf::desc_simplify_add_zero>);
  STATIC_REQUIRE(p::contains_pass_v<extended_o1::bundle, desc_test_extra>);

  // Bundle size = o1_bundle(2) + extra_bundle(1) = 3
  using ordered = extended_o1::ordered;
  constexpr auto sz = p::detail::bundle_size_v<ordered>::value;
  STATIC_REQUIRE(sz == 3);
}

TEST_CASE(

    "profile_inherit deduplicates overlapping descriptors",
    "[lithe][profile][inherit]") {
  // deduped_o1 inherits extended_o1 (3 descriptors) + o1_bundle (2 descriptors,
  // both already present) → should still be 3 total.
  using ordered = deduped_o1::ordered;
  constexpr auto sz = p::detail::bundle_size_v<ordered>::value;
  STATIC_REQUIRE(sz == 3);
}

TEST_CASE(

    "profile_inherit runs base passes then extra pass",
    "[lithe][profile][inherit]") {
  // extended_o1 should run simplify passes + the extra no-op pass.
  // Result should equal std_o1 applied to same expr (extra pass is identity).
  auto expr1 = lithe::make_node<lithe::add_tag>(0, 5);
  auto expr2 = lithe::make_node<lithe::add_tag>(0, 5);

  auto out_base = pf::std_o1{}(expr1);
  auto out_extended = extended_o1{}(expr2);

  REQUIRE(lithe::structural_hash(out_base) ==
          lithe::structural_hash(out_extended));
}

// ============================================================================
// Test 4 — id namespacing: "std.o3" and "tensor.o3" are distinct descriptors
// ============================================================================

TEST_CASE(

    "profile_descriptor id namespacing keeps domains distinct",
    "[lithe][profile][descriptor]") {
  constexpr pf::profile_descriptor std_d{"std.o3"};
  constexpr pf::profile_descriptor tensor_d{"tensor.o3"};

  // Different ids → not equal
  STATIC_REQUIRE(!(std_d == tensor_d));

  // Same ids → equal
  constexpr pf::profile_descriptor std_d2{"std.o3"};
  STATIC_REQUIRE(std_d == std_d2);

  // id_view() reflects correct string
  REQUIRE(std_d.id_view() == "std.o3");
  REQUIRE(tensor_d.id_view() == "tensor.o3");
}

// ============================================================================
// Test 5 — export → profile_record → import → dynamic_profile round-trip
// ============================================================================

TEST_CASE(

    "export_profile + import_profile descriptor round-trip",
    "[lithe][profile][export_import]") {
  auto rec = pf::export_profile<pf::std_o3>();

  REQUIRE(rec.profile_id == "std.o3");
  REQUIRE(rec.max_iters == 8);
  REQUIRE(rec.version.major == 1);
  CHECK(
      std::none_of(rec.passes.begin(), rec.passes.end(), [](const auto &pass) {
        return pass.effect == lithe::passes::pass_effect_kind::placeholder ||
               pass.effect == lithe::passes::pass_effect_kind::analyzes;
      }));
  REQUIRE(rec.version.minor == 0);
  REQUIRE(rec.version.patch == 0);
  REQUIRE(!rec.trace);
  REQUIRE(rec.deterministic);

  // Standard optimizing profiles contain only transforming tree passes.
  using ordered = pf::std_o3::ordered;
  constexpr auto expected_count = p::detail::bundle_size_v<ordered>::value;
  REQUIRE(rec.passes.size() == expected_count);
}

TEST_CASE(

    "import_profile builds dynamic_profile with correct pass_count",
    "[lithe][profile][export_import]") {
  auto rec = pf::export_profile<pf::std_o3>();

  // Resolver that returns a no-op erased pass for any stable_id.
  pf::pass_resolver resolver = [](std::size_t,
                                  int) -> pf::dynamic_profile::erased_pass {
    return [](std::any &) {};
  };

  auto dp = pf::import_profile(rec, resolver);

  REQUIRE(dp.id() == "std.o3");
  REQUIRE(dp.pass_count() == rec.passes.size());
  REQUIRE(!dp.empty());
}

TEST_CASE(

    "import_profile without resolver produces descriptor-only dynamic_profile",
    "[lithe][profile][export_import]") {
  auto rec = pf::export_profile<pf::std_o2>();
  auto dp = pf::import_profile(rec);

  REQUIRE(dp.id() == "std.o2");
  REQUIRE(dp.empty());
  REQUIRE(dp.record.max_iters == 6);
}

// ============================================================================
// Test 6 — zero-overhead: profile<> is an empty type (sizeof == 1)
// ============================================================================

TEST_CASE(

    "profile types are empty (zero-overhead guarantee)",
    "[lithe][profile][sizeof]") {
  STATIC_REQUIRE(sizeof(pf::std_o0{}) == 1);
  STATIC_REQUIRE(sizeof(pf::std_o1{}) == 1);
  STATIC_REQUIRE(sizeof(pf::std_o2{}) == 1);
  STATIC_REQUIRE(sizeof(pf::std_o3{}) == 1);
  STATIC_REQUIRE(sizeof(pf::std_debug{}) == 1);
  STATIC_REQUIRE(sizeof(pf::std_semantic_safe{}) == 1);
}
