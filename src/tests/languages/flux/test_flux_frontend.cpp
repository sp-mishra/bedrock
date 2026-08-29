#include "catch_amalgamated.hpp"
#include <memory>
#include <string>
#include "../../../examples/languages/flux/flux_frontend.hpp"

// Frontend tests for Flux example (src/examples/languages/flux/)
// Tests the three-path architecture: lexy, Samasa, EDSL
// These are integration tests of the example, NOT unit tests of lithe

namespace flux::tests::frontend {

TEST_CASE("flux_example: three-path convergence invariant", "[flux][example][3-path]") {
  // Path A: Runtime lexy parser (simulated)
  auto path_a_tree = flux::frontend::runtime_path::parse("x + y * z");
  REQUIRE(path_a_tree != nullptr);

  // Path B: Samasa consteval (simulated)
  auto path_b_tree = flux::frontend::consteval_path::parse();
  REQUIRE(path_b_tree != nullptr);

  // Path C: Vakya EDSL
  auto path_c_tree = flux::frontend::edsl_path::construct();
  REQUIRE(path_c_tree != nullptr);

  // All paths should produce identical structural hashes
  auto hash_a = path_a_tree->structural_hash();
  auto hash_b = path_b_tree->structural_hash();
  auto hash_c = path_c_tree->structural_hash();

  REQUIRE(hash_a == hash_b);
  REQUIRE(hash_b == hash_c);
  REQUIRE(hash_a != 0);
}

TEST_CASE("flux_example: runtime path produces valid tree", "[flux][example][path-a]") {
  auto tree = flux::frontend::runtime_path::parse("x + y * z");
  REQUIRE(tree != nullptr);
  REQUIRE(tree->structural_hash() != 0);
}

TEST_CASE("flux_example: consteval path produces valid tree", "[flux][example][path-b]") {
  auto tree = flux::frontend::consteval_path::parse();
  REQUIRE(tree != nullptr);
  REQUIRE(tree->structural_hash() != 0);
}

TEST_CASE("flux_example: EDSL path produces valid tree", "[flux][example][path-c]") {
  auto tree = flux::frontend::edsl_path::construct();
  REQUIRE(tree != nullptr);
  REQUIRE(tree->structural_hash() != 0);
}

}  // namespace flux::tests::frontend
