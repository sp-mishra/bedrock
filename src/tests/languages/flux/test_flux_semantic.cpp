#include "catch_amalgamated.hpp"
#include <memory>
#include <string>
#include "../../../examples/languages/flux/flux_frontend.hpp"
#include "../../../examples/languages/flux/flux_semantic.hpp"

// Semantic analysis tests for Flux example
// Tests name resolution, type inference, shape inference
// Integration tests of example modules, NOT lithe unit tests

namespace flux::tests::semantic {

TEST_CASE("flux_example: name resolution", "[flux][example][semantic][names]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto symtab = flux::semantic::name_resolver::resolve(expr);

  REQUIRE(symtab.size() == 3);
  REQUIRE(symtab.count("x") == 1);
  REQUIRE(symtab.count("y") == 1);
  REQUIRE(symtab.count("z") == 1);
}

TEST_CASE("flux_example: type inference", "[flux][example][semantic][types]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto symtab = flux::semantic::name_resolver::resolve(expr);
  auto type_info = flux::semantic::type_inferencer::infer(expr, symtab);

  REQUIRE(type_info.type_name == "f64");
  REQUIRE(!type_info.is_polymorphic);
}

TEST_CASE("flux_example: shape inference scalar", "[flux][example][semantic][shapes]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto symtab = flux::semantic::name_resolver::resolve(expr);
  auto type_info = flux::semantic::type_inferencer::infer(expr, symtab);
  auto shape_info = flux::semantic::shape_inferencer::infer(expr, type_info);

  REQUIRE(shape_info.is_scalar);
  REQUIRE(shape_info.rank == 0);
}

TEST_CASE("flux_example: effect analysis pure", "[flux][example][semantic][effects]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto symtab = flux::semantic::name_resolver::resolve(expr);
  auto type_info = flux::semantic::type_inferencer::infer(expr, symtab);
  auto shape_info = flux::semantic::shape_inferencer::infer(expr, type_info);
  auto effects = flux::semantic::effect_analyzer::analyze(expr, shape_info);

  REQUIRE(effects.is_pure);
  REQUIRE(!effects.has_io);
  REQUIRE(!effects.has_mutation);
}

TEST_CASE("flux_example: full semantic pipeline", "[flux][example][semantic][pipeline]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::semantic::semantic_pipeline::analyze(expr);

  REQUIRE(result.symbols.size() == 3);
  REQUIRE(result.type_info.type_name == "f64");
  REQUIRE(result.shape_info.is_scalar);
  REQUIRE(result.effects.is_pure);
}

}  // namespace flux::tests::semantic
