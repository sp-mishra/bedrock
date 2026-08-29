#include "catch_amalgamated.hpp"
#include <memory>
#include <string>
#include <vector>
#include "../../../examples/languages/flux/flux_frontend.hpp"
#include "../../../examples/languages/flux/flux_gpu_execution.hpp"

// GPU execution tests for Flux example
// Tests Metal and Vulkan code generation and dispatch
// Integration tests of example, NOT lithe backends

namespace flux::tests::gpu_execution {

TEST_CASE("flux_example: Metal backend compile", "[flux][example][gpu][metal]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::gpu_execution::metal_backend::compile(expr);

  REQUIRE(result.compilation_success);
  REQUIRE(!result.msl_kernel.empty());
  REQUIRE(result.compilation_time_ms > 0.0f);
}

TEST_CASE("flux_example: Metal backend dispatch", "[flux][example][gpu][metal-exec]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto disp = flux::gpu_execution::metal_backend::dispatch_gpu(expr, 1024);

  REQUIRE(disp.upload_ms > 0.0f);
  REQUIRE(disp.launch_ms > 0.0f);
  REQUIRE(disp.compute_ms > 0.0f);
  REQUIRE(disp.download_ms > 0.0f);
  REQUIRE(disp.total_ms() > 0.0f);
}

TEST_CASE("flux_example: Vulkan backend compile", "[flux][example][gpu][vulkan]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::gpu_execution::vulkan_backend::compile(expr);

  REQUIRE(result.compilation_success);
  REQUIRE(!result.spirv_bytecode.empty());
  REQUIRE(result.compilation_time_ms > 0.0f);
}

TEST_CASE("flux_example: unified dispatcher Metal", "[flux][example][gpu][dispatcher]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::gpu_execution::gpu_dispatcher::execute(expr, "metal");

  REQUIRE(result.backend_name == "metal");
  REQUIRE(result.total_ms > 0.0f);
}

TEST_CASE("flux_example: SPIR-V magic number", "[flux][example][gpu][spirv-magic]") {
  auto expr = flux::frontend::edsl_path::construct();
  auto result = flux::gpu_execution::vulkan_backend::compile(expr);

  REQUIRE(!result.spirv_bytecode.empty());
  // SPIR-V magic: 0x07230203
  REQUIRE(result.spirv_bytecode[0] == 0x07230203);
}

}  // namespace flux::tests::gpu_execution
