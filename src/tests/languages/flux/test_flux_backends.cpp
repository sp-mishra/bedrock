#include "catch_amalgamated.hpp"
#include <memory>
#include <string>
#include "../../../examples/languages/flux/flux_frontend.hpp"
#include "../../../examples/languages/flux/flux_semantic.hpp"
#include "../../../examples/languages/flux/flux_optimization.hpp"
#include "../../../examples/languages/flux/flux_lowering.hpp"
#include "../../../examples/languages/flux/flux_cost_model.hpp"
#include "../../../examples/languages/flux/flux_gpu_execution.hpp"

// Cross-backend validation for Flux example
// Tests full pipeline: frontend → semantic → lowering → cost → execution
// Integration tests of example modules

namespace flux::tests::backends {

TEST_CASE("flux_example: three paths converge", "[flux][example][backends][3-path]") {
  auto path_a = flux::frontend::runtime_path::parse("x + y * z");
  auto path_b = flux::frontend::consteval_path::parse();
  auto path_c = flux::frontend::edsl_path::construct();

  auto hash_a = path_a->structural_hash();
  auto hash_b = path_b->structural_hash();
  auto hash_c = path_c->structural_hash();

  REQUIRE(hash_a == hash_b);
  REQUIRE(hash_b == hash_c);
}

TEST_CASE("flux_example: CPU backend for small workload", "[flux][example][backends][cpu-small]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto decision = flux::cost_model::decision_engine::select_backend(expr, 256);

  REQUIRE(decision.selected_backend == flux::cost_model::backend_kind::cpu_simd);
}

TEST_CASE("flux_example: GPU backend for large workload", "[flux][example][backends][gpu-large]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto decision = flux::cost_model::decision_engine::select_backend(expr, 10000000);

  REQUIRE(decision.selected_backend == flux::cost_model::backend_kind::gpu_metal);
}

TEST_CASE("flux_example: full pipeline CPU path",
          "[flux][example][backends][pipeline-cpu]") {
  auto expr = flux::frontend::edsl_path::construct();

  // Semantic analysis
  auto semantic = flux::semantic::semantic_pipeline::analyze(expr);
  REQUIRE(semantic.effects.is_pure);

  // Optimization
  auto opt_passes = flux::optimization::egraph_optimizer::optimize(expr);
  REQUIRE(!opt_passes.empty());

  // Lowering
  auto lower = flux::lowering::lowering_pipeline::lower(expr);
  REQUIRE(lower.mir_result.instruction_count > 0);

  // Cost decision
  auto decision = flux::cost_model::decision_engine::select_backend(expr, 1024);
  REQUIRE(decision.selected_backend == flux::cost_model::backend_kind::cpu_simd);
}

TEST_CASE("flux_example: full pipeline GPU path",
          "[flux][example][backends][pipeline-gpu]") {
  auto expr = flux::frontend::edsl_path::construct();

  // Semantic analysis
  auto semantic = flux::semantic::semantic_pipeline::analyze(expr);
  REQUIRE(semantic.effects.is_pure);

  // Optimization
  auto opt_passes = flux::optimization::egraph_optimizer::optimize(expr);
  REQUIRE(!opt_passes.empty());

  // Lowering
  auto lower = flux::lowering::lowering_pipeline::lower(expr);
  REQUIRE(lower.mir_result.instruction_count > 0);

  // Cost decision
  auto decision = flux::cost_model::decision_engine::select_backend(expr, 1000000);
  REQUIRE(decision.selected_backend == flux::cost_model::backend_kind::gpu_metal);

  // GPU compilation & execution
  auto metal = flux::gpu_execution::metal_backend::compile(expr);
  REQUIRE(metal.compilation_success);

  auto dispatch = flux::gpu_execution::gpu_dispatcher::execute(expr, "metal");
  REQUIRE(dispatch.total_ms > 0.0f);
}

}  // namespace flux::tests::backends
