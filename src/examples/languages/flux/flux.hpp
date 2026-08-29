// Flux Language — Modular Compiler Architecture
// Professional end-to-end example with three-path frontend
// Uses Lithe + Vakya + Language Commons
//
// Reference: docs/languages/flux/ch00_overview.md
//
// Modules:
//   flux_frontend.hpp      — Three-path convergence (lexy/Samasa/EDSL)
//   flux_semantic.hpp      — Name resolution, type & shape inference
//   flux_optimization.hpp  — E-graphs, rewrites, semantic passes
//   flux_lowering.hpp      — AST → Vakya → Lithe MIR
//   flux_cost_model.hpp    — CPU vs GPU decision engine
//   flux_gpu_execution.hpp — Metal/Vulkan code generation
//   flux_observability.hpp — Phase events, tracing

#pragma once

#include <lithe/lithe.hpp>
#include <lithe/lithe_cost_model.hpp>
#include <lithe/lithe_execution_admission.hpp>
#include <lithe/backends/lithe_codegen_metal.hpp>
#include <lithe/backends/lithe_codegen_vulkan_spirv_ir.hpp>
#include <vakya/vakya.hpp>
#include <vakya/vakya_types.hpp>

// Include modular components
#include "flux_frontend.hpp"
#include "flux_semantic.hpp"
#include "flux_optimization.hpp"
#include "flux_lowering.hpp"
#include "flux_cost_model.hpp"
#include "flux_gpu_execution.hpp"
#include "flux_observability.hpp"

#include <iostream>
#include <iomanip>
#include <cassert>

namespace flux {

// Main orchestration: run complete example through all pipeline phases
void run_complete_example();

// Main orchestration: run complete example through all pipeline phases
void run_complete_example() {
  std::cout << "╔════════════════════════════════════════════════════════════╗\n";
  std::cout << "║  Flux Language — Modular Compiler Example                ║\n";
  std::cout << "║  Three-path frontend → unified pipeline → GPU execution   ║\n";
  std::cout << "╚════════════════════════════════════════════════════════════╝\n\n";

  // 1. Three-path frontend (modules: flux_frontend.hpp)
  std::cout << "══════════════════════════════════════════════════════════════\n";
  std::cout << "1. THREE-PATH FRONTEND (flux_frontend.hpp)\n";
  std::cout << "══════════════════════════════════════════════════════════════\n\n";

  auto path_a = frontend::runtime_path::parse("x + y * z");
  auto path_b = frontend::consteval_path::parse();
  auto path_c = frontend::edsl_path::construct();

  auto hash_a = path_a->structural_hash();
  auto hash_b = path_b->structural_hash();
  auto hash_c = path_c->structural_hash();

  if (hash_a == hash_b && hash_b == hash_c) {
    std::cout << "✓ Three paths converge: hash=" << std::hex << hash_a << std::dec << "\n\n";
  }

  // 2. Semantic analysis (module: flux_semantic.hpp)
  std::cout << "══════════════════════════════════════════════════════════════\n";
  std::cout << "2. SEMANTIC ANALYSIS (flux_semantic.hpp)\n";
  std::cout << "══════════════════════════════════════════════════════════════\n";
  auto semantic_result = semantic::semantic_pipeline::analyze(path_c);

  // 3. Optimization (module: flux_optimization.hpp)
  std::cout << "══════════════════════════════════════════════════════════════\n";
  std::cout << "3. OPTIMIZATION (flux_optimization.hpp)\n";
  std::cout << "══════════════════════════════════════════════════════════════\n";
  auto opt_passes = optimization::egraph_optimizer::optimize(path_c);

  // 4. Lowering (module: flux_lowering.hpp)
  std::cout << "══════════════════════════════════════════════════════════════\n";
  std::cout << "4. LOWERING TO LITHE MIR (flux_lowering.hpp)\n";
  std::cout << "══════════════════════════════════════════════════════════════\n";
  auto lower_result = lowering::lowering_pipeline::lower(path_c);

  // 5. Cost modeling (module: flux_cost_model.hpp)
  std::cout << "══════════════════════════════════════════════════════════════\n";
  std::cout << "5. COST MODELING & BACKEND SELECTION (flux_cost_model.hpp)\n";
  std::cout << "══════════════════════════════════════════════════════════════\n";
  auto decision_small = cost_model::decision_engine::select_backend(path_c, 1024);
  auto decision_large = cost_model::decision_engine::select_backend(path_c, 1000000);

  // 6. GPU execution (module: flux_gpu_execution.hpp)
  std::cout << "══════════════════════════════════════════════════════════════\n";
  std::cout << "6. GPU EXECUTION (flux_gpu_execution.hpp)\n";
  std::cout << "══════════════════════════════════════════════════════════════\n";
  auto metal_result = gpu_execution::metal_backend::compile(path_c);
  auto metal_exec = gpu_execution::gpu_dispatcher::execute(path_c, "metal");

  // 7. Observability (module: flux_observability.hpp)
  std::cout << "══════════════════════════════════════════════════════════════\n";
  std::cout << "7. OBSERVABILITY (flux_observability.hpp)\n";
  std::cout << "══════════════════════════════════════════════════════════════\n";
  observability::observability_pipeline::demonstrate();

  // Summary
  std::cout << "══════════════════════════════════════════════════════════════\n";
  std::cout << "ARCHITECTURE SUMMARY\n";
  std::cout << "══════════════════════════════════════════════════════════════\n\n";
  std::cout << "Modular design:\n";
  std::cout << "  ✓ flux_frontend.hpp      — Three-path convergence\n";
  std::cout << "  ✓ flux_semantic.hpp      — Type & shape inference\n";
  std::cout << "  ✓ flux_optimization.hpp  — E-graphs, rewrites\n";
  std::cout << "  ✓ flux_lowering.hpp      — Vakya → MIR\n";
  std::cout << "  ✓ flux_cost_model.hpp    — Backend decision engine\n";
  std::cout << "  ✓ flux_gpu_execution.hpp — Metal/Vulkan codegen\n";
  std::cout << "  ✓ flux_observability.hpp — Phase events, tracing\n\n";

  std::cout << "Unit tests (src/tests/languages/flux/):\n";
  std::cout << "  • test_flux_frontend.cpp      — Three-path invariant\n";
  std::cout << "  • test_flux_semantic.cpp      — Type & shape analysis\n";
  std::cout << "  • test_flux_optimization.cpp  — Rewrite rules\n";
  std::cout << "  • test_flux_lowering.cpp      — MIR generation\n";
  std::cout << "  • test_flux_cost_model.cpp    — Backend selection\n";
  std::cout << "  • test_flux_gpu.cpp           — GPU codegen\n";
  std::cout << "  • test_flux_backends.cpp      — CPU/SIMD/GPU validation\n\n";

  std::cout << "Reference: docs/languages/flux/ch00_overview.md\n";
  std::cout << "═════════════════════════════════════════════════════════════\n";
}

} // namespace flux
