#include "catch_amalgamated.hpp"
#include <memory>
#include <string>
#include "../../../examples/languages/flux/flux_frontend.hpp"
#include "../../../examples/languages/flux/flux_cost_model.hpp"

// Cost modeling tests for Flux example
// Tests backend selection decision engine
// Integration tests of example, NOT lithe cost model

namespace flux::tests::cost_model {

TEST_CASE("flux_example: CPU SIMD backend cost", "[flux][example][cost][cpu]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto cost = flux::cost_model::backend_cost_estimator::estimate(
      expr, flux::cost_model::backend_kind::cpu_simd, 1024);

  REQUIRE(cost.compute_ops == 1024);
  REQUIRE(cost.memory_bytes > 0);
  REQUIRE(cost.latency_ms > 0.0f);
  REQUIRE(cost.power_mw == 15);
}

TEST_CASE("flux_example: GPU Metal backend cost", "[flux][example][cost][metal]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto cost = flux::cost_model::backend_cost_estimator::estimate(
      expr, flux::cost_model::backend_kind::gpu_metal, 1024);

  REQUIRE(cost.compute_ops == 1024);
  REQUIRE(cost.memory_bytes > 0);
  REQUIRE(cost.latency_ms > 0.0f);
  REQUIRE(cost.power_mw == 8);
}

TEST_CASE("flux_example: GPU Vulkan backend cost", "[flux][example][cost][vulkan]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto cost = flux::cost_model::backend_cost_estimator::estimate(
      expr, flux::cost_model::backend_kind::gpu_vulkan, 1024);

  REQUIRE(cost.compute_ops == 1024);
  REQUIRE(cost.memory_bytes > 0);
  REQUIRE(cost.latency_ms > 0.0f);
}

TEST_CASE("flux_example: decision engine small workload", "[flux][example][cost][small]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto decision = flux::cost_model::decision_engine::select_backend(expr, 1024);

  REQUIRE(decision.selected_backend == flux::cost_model::backend_kind::cpu_simd);
}

TEST_CASE("flux_example: decision engine large workload", "[flux][example][cost][large]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto decision = flux::cost_model::decision_engine::select_backend(expr, 1000000);

  REQUIRE(decision.selected_backend == flux::cost_model::backend_kind::gpu_metal);
}

TEST_CASE("flux_example: admission control admit", "[flux][example][cost][admit]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result =
      flux::cost_model::execution_admission_control::check_admission(expr, 32 * 1024 * 1024);

  REQUIRE(result.admitted);
}

TEST_CASE("flux_example: admission control reject", "[flux][example][cost][reject]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::cost_model::execution_admission_control::check_admission(
      expr, 200 * 1024 * 1024);

  REQUIRE(!result.admitted);
}

}  // namespace flux::tests::cost_model
