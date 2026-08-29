#include "catch_amalgamated.hpp"
#include <memory>
#include <string>
#include <vector>
#include "../../../examples/languages/flux/flux_frontend.hpp"
#include "../../../examples/languages/flux/flux_lowering.hpp"

// Lowering tests for Flux example
// Tests AST → Vakya → Lithe MIR pipeline
// Integration tests of example, NOT lithe internals

namespace flux::tests::lowering {

TEST_CASE("flux_example: vakya lowering", "[flux][example][lowering][vakya]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::lowering::vakya_lowerer::lower_to_vakya(expr);

  REQUIRE(result.vakya_tree != nullptr);
  REQUIRE(result.type_annotation == "f64");
  REQUIRE(result.shape_annotation == "[]");
  REQUIRE(result.node_count == 5);
}

TEST_CASE("flux_example: MIR generation", "[flux][example][lowering][mir]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::lowering::mir_generator::generate_mir(expr);

  REQUIRE(result.instruction_count == 6);
  REQUIRE(result.ssa_form == "strict_ssa");
  REQUIRE(!result.instructions.empty());
}

TEST_CASE("flux_example: MIR instructions correct", "[flux][example][lowering][mir-ops]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::lowering::mir_generator::generate_mir(expr);

  std::vector<std::string> expected_ops = {"load", "load", "load", "fmul", "fadd", "ret"};
  for (size_t i = 0; i < expected_ops.size() && i < result.instructions.size(); ++i) {
    REQUIRE(result.instructions[i].op == expected_ops[i]);
  }
}

TEST_CASE("flux_example: full lowering pipeline", "[flux][example][lowering][pipeline]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto summary = flux::lowering::lowering_pipeline::lower(expr);

  REQUIRE(summary.vakya_result.vakya_tree != nullptr);
  REQUIRE(summary.mir_result.instruction_count > 0);
}

}  // namespace flux::tests::lowering
