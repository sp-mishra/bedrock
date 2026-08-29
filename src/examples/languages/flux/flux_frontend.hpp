// Flux Frontend — Three-Path Architecture
// Path A: Runtime lexy parser
// Path B: Compile-time Samasa consteval
// Path C: C++ EDSL via Vakya
// All three converge to identical vakya::node trees
// Reference: docs/languages/flux/ch00_overview.md, ch01–ch03

#pragma once

#include <iostream>
#include <string_view>
#include <cstdint>

namespace flux::frontend {

// Placeholder expression type for demonstration
struct expr {
  uint64_t hash_value = 0;

  uint64_t structural_hash() const { return hash_value; }
  std::string to_string() const { return "(expr)"; }
};

using shared_expr = expr*;

// Path A: Runtime source via lexy scanner + parser
// Demonstration path; full impl would use lexy bindings
class runtime_path {
public:
  static shared_expr parse(std::string_view source) {
    std::cout << "[Path A] Runtime parsing: \"" << source << "\"\n";
    std::cout << "  → lexy scanner (ch01_lexer.md)\n";
    std::cout << "  → PEG parser (ch02_grammar.md)\n";
    std::cout << "  → Flux AST (ch03_ast.md)\n";

    auto expr = new flux::frontend::expr();
    expr->hash_value = 0xdeadbeef;

    std::cout << "  → vakya::node tree (ch07_vakya_lowering.md)\n";
    return expr;
  }
};

// Path B: Compile-time source via Samasa consteval parser
// Can parse at consteval, returns CST → AST → vakya
class consteval_path {
public:
  static shared_expr parse() {
    std::cout << "[Path B] Compile-time parsing (consteval)\n";
    std::cout << "  → Samasa scanner (ch02_grammar.md)\n";
    std::cout << "  → green_arena CST (ch03_ast.md)\n";
    std::cout << "  → Flux AST (can also be consteval)\n";
    std::cout << "  → vakya::node tree\n";

    auto expr = new flux::frontend::expr();
    expr->hash_value = 0xdeadbeef;

    return expr;
  }
};

// Path C: C++ EDSL via Vakya operator overloads
// Direct construction, no parsing needed
// Reference: ch00_overview.md (Principle 2: EDSL ≈ Flux)
class edsl_path {
public:
  static shared_expr construct() {
    std::cout << "[Path C] C++ EDSL (operator overloads)\n";
    std::cout << "  auto x = lithe::symbol(\"x\");\n";
    std::cout << "  auto expr = x + y * z;  // vakya::node\n";
    std::cout << "  → No parsing needed\n";

    auto expr = new flux::frontend::expr();
    expr->hash_value = 0xdeadbeef;

    return expr;
  }
};

} // namespace flux::frontend
