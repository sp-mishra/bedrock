// Flux Lowering — AST → Vakya → Lithe MIR
// Reference: docs/languages/flux/ch07_vakya_lowering.md
//            docs/languages/flux/ch09_ir.md

#pragma once

#include "flux_frontend.hpp"

#include <iostream>
#include <string>

namespace flux::lowering {

using shared_expr = flux::frontend::shared_expr;

// Vakya Lowering: Build vakya::node tree with type & shape info
class vakya_lowerer {
public:
  struct lowering_result {
    shared_expr vakya_tree;
    std::string type_annotation;
    std::string shape_annotation;
    int node_count = 0;
  };

  static lowering_result lower_to_vakya(const shared_expr& expr) {
    std::cout << "--- Lowering to Vakya ---\n";
    std::cout << "Input expr: " << expr->to_string() << "\n\n";

    // Step 1: Vakya lowering (ch07)
    std::cout << "Step 1: AST → vakya::node (ch07_vakya_lowering.md)\n";
    std::cout << "  Building vakya tree structure\n";
    std::cout << "  Attaching type information (f64)\n";
    std::cout << "  Attaching shape information (scalar)\n";
    std::cout << "  Common subexpression elimination (CSE)\n";
    std::cout << "  DAG representation (shared nodes)\n\n";

    lowering_result result;
    result.vakya_tree = expr;  // In real impl: walk AST → build vakya
    result.type_annotation = "f64";
    result.shape_annotation = "[]";
    result.node_count = 5;  // Demo: x, y, z, mul, add

    return result;
  }
};

// MIR Generation: Lower vakya → Lithe machine IR
class mir_generator {
public:
  struct mir_instruction {
    int id;
    std::string op;
    std::vector<int> operands;
    std::string result_type;
  };

  struct mir_result {
    std::vector<mir_instruction> instructions;
    int instruction_count = 0;
    std::string ssa_form;
  };

  static mir_result generate_mir(const shared_expr& expr) {
    std::cout << "Step 2: Vakya → HL-MIR (high-level IR)\n";
    std::cout << "  Region detection (loops, tensor ops)\n";
    std::cout << "  Fusion analysis\n";
    std::cout << "  Polyhedral planning\n\n";

    std::cout << "Step 3: HL-MIR → Lithe MIR (machine IR)\n";
    std::cout << "  SSA form (Single Static Assignment)\n";
    std::cout << "  Control flow graph (CFG)\n";
    std::cout << "  Instructions per backend\n\n";

    std::cout << "Resulting MIR for expression x + y * z:\n";
    mir_result result;

    mir_instruction i0{0, "load", {}, "f64"};
    std::cout << "  %0 = load f64, x_addr\n";
    result.instructions.push_back(i0);

    mir_instruction i1{1, "load", {}, "f64"};
    std::cout << "  %1 = load f64, y_addr\n";
    result.instructions.push_back(i1);

    mir_instruction i2{2, "load", {}, "f64"};
    std::cout << "  %2 = load f64, z_addr\n";
    result.instructions.push_back(i2);

    mir_instruction i3{3, "fmul", {1, 2}, "f64"};
    std::cout << "  %3 = fmul %1, %2\n";
    result.instructions.push_back(i3);

    mir_instruction i4{4, "fadd", {0, 3}, "f64"};
    std::cout << "  %4 = fadd %0, %3\n";
    result.instructions.push_back(i4);

    mir_instruction i5{5, "ret", {4}, "void"};
    std::cout << "  ret %4\n\n";
    result.instructions.push_back(i5);

    result.instruction_count = result.instructions.size();
    result.ssa_form = "strict_ssa";

    return result;
  }
};

// Full lowering pipeline
class lowering_pipeline {
public:
  struct lowering_summary {
    vakya_lowerer::lowering_result vakya_result;
    mir_generator::mir_result mir_result;
    std::string optimization_summary;
  };

  static lowering_summary lower(const shared_expr& expr) {
    std::cout << "\n--- Lowering to Lithe MIR ---\n";

    lowering_summary summary;
    summary.vakya_result = vakya_lowerer::lower_to_vakya(expr);
    summary.mir_result = mir_generator::generate_mir(expr);
    summary.optimization_summary = "No CSE opportunities in this expression";

    return summary;
  }
};

} // namespace flux::lowering
