# Chapter 3 — AST: CST → Flux AST

## Theory

The **CST** (Concrete Syntax Tree) is source-faithful but noisy: it contains every keyword, comma, brace, and whitespace
position. The **AST** (Abstract Syntax Tree) strips syntactic noise and gives semantic structure.

### Why a separate AST?

1. **Type annotation** — AST nodes carry type metadata added by inference passes.
2. **Simplified structure** — `if` in the CST has tokens `if`, `{`, `}`, `else`, `{`, `}`. In the AST it is
   `if_expr { cond, then_body, else_body }`.
3. **Decoupled passes** — name resolution, type inference, and lowering all work on the AST, not the CST. Passes never
   re-parse.
4. **Arena allocation** — flat index-based storage; no pointer chasing; cache-friendly.

### Flat Arena

`lang::ast_arena<Node>` stores nodes as a contiguous vector. Each node is a `std::variant` of all possible node types.
Children are stored as indices into the same arena.

```
arena[0] = program_node { decls=[1,3], expr=5 }
arena[1] = input_decl_node { name="x", type=flux_type::f32 }
arena[2] = input_decl_node { name="y", type=flux_type::f32 }
arena[3] = let_decl_node { name="distance", rhs=4 }
arena[4] = call_expr_node { callee="sqrt", args=[5] }
arena[5] = binary_expr_node { op="+", lhs=6, rhs=8 }
...
```

---

## Flux Type Representation

```cpp
// include/languages/flux/types.hpp
#pragma once
#include <cstdint>
#include <variant>
#include <vector>
#include <string>

namespace flux {

// Primitive types
enum class prim_type : uint8_t {
    i32, i64, u32, u64, f32, f64, bool_, string_
};

// Tensor shape: compile-time dims
struct tensor_shape {
    std::vector<std::size_t> dims;  // empty = scalar
};

// Flux type: a tagged sum
struct flux_type {
    struct prim   { prim_type kind; };
    struct vec    { std::shared_ptr<flux_type> element; };
    struct mat    { std::shared_ptr<flux_type> element; std::size_t rows, cols; };
    struct tensor { std::shared_ptr<flux_type> element; tensor_shape shape; };
    struct tup    { std::vector<flux_type> elements; };
    struct fn     { std::vector<flux_type> params; std::shared_ptr<flux_type> ret; };
    struct var    { uint32_t id; };     // unification variable (during inference)
    struct named  { std::string name; };

    using variant = std::variant<prim, vec, mat, tensor, tup, fn, var, named>;
    variant data;

    // Convenience constructors
    static flux_type f32()  { return { prim{ prim_type::f32 } }; }
    static flux_type f64()  { return { prim{ prim_type::f64 } }; }
    static flux_type i32()  { return { prim{ prim_type::i32 } }; }
    static flux_type bool_() { return { prim{ prim_type::bool_ } }; }
    static flux_type fresh_var(uint32_t id) { return { var{id} }; }
};

} // namespace flux
```

---

## AST Node Definitions

```cpp
// include/languages/flux/ast.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <variant>
#include "types.hpp"

namespace flux {

using node_idx = uint32_t;
constexpr node_idx null_node = ~uint32_t{0};

// ── Expression nodes ──────────────────────────────────────────────────────────

struct integer_literal_node {
    int64_t     value;
    flux_type   type;
};

struct float_literal_node {
    double      value;
    flux_type   type;
};

struct bool_literal_node {
    bool        value;
    flux_type   type = flux_type::bool_();
};

struct string_literal_node {
    std::string value;
};

struct identifier_node {
    std::string name;
    flux_type   type;
    node_idx    decl;  // resolved declaration index (null_node = unresolved)
};

struct binary_expr_node {
    std::string op;   // "+", "-", "*", "/", "<", "==", …
    node_idx    lhs;
    node_idx    rhs;
    flux_type   type;
};

struct unary_expr_node {
    std::string op;   // "-", "not"
    node_idx    operand;
    flux_type   type;
};

struct call_expr_node {
    std::string           callee;
    std::vector<node_idx> args;
    flux_type             type;
};

struct method_call_expr_node {
    node_idx              receiver;
    std::string           method;
    std::vector<node_idx> args;
    flux_type             type;
};

struct if_expr_node {
    node_idx  cond;
    node_idx  then_body;
    node_idx  else_body;
    flux_type type;
};

struct array_expr_node {
    std::vector<node_idx> elements;
    flux_type             type;
};

struct range_expr_node {
    node_idx  start;
    node_idx  end;
    flux_type type;
};

struct lambda_node {
    std::vector<std::string>          params;
    std::vector<std::optional<flux_type>> param_types;
    node_idx                          body;
    flux_type                         type;
};

struct block_node {
    std::vector<node_idx> stmts;   // declarations or expressions
    node_idx              result;  // last expression (null_node = unit)
    flux_type             type;
};

// ── Declaration nodes ─────────────────────────────────────────────────────────

struct input_decl_node {
    std::string name;
    flux_type   type;
};

struct let_decl_node {
    std::string name;
    node_idx    rhs;
    flux_type   type;
};

struct param_node {
    std::string           name;
    std::optional<flux_type> type;
};

struct fn_decl_node {
    std::string              name;
    std::vector<param_node>  params;
    std::optional<flux_type> return_type;
    node_idx                 body;
    bool                     is_pure;
    flux_type                type;
};

struct import_node {
    std::string module_name;
};

// ── Program ───────────────────────────────────────────────────────────────────

struct program_node {
    std::vector<node_idx> imports;
    std::vector<node_idx> decls;
    node_idx              expr;   // optional top-level expression
};

// ── Variant ───────────────────────────────────────────────────────────────────

using flux_node = std::variant<
    program_node,
    import_node,
    input_decl_node,
    let_decl_node,
    fn_decl_node,
    block_node,
    integer_literal_node,
    float_literal_node,
    bool_literal_node,
    string_literal_node,
    identifier_node,
    binary_expr_node,
    unary_expr_node,
    call_expr_node,
    method_call_expr_node,
    if_expr_node,
    array_expr_node,
    range_expr_node,
    lambda_node,
    param_node
>;

} // namespace flux
```

---

## AST Arena

```cpp
// include/languages/flux/ast_arena.hpp
#pragma once
#include "ast.hpp"
#include <vector>

namespace flux {

class ast_arena {
public:
    node_idx add(flux_node node) {
        auto idx = static_cast<node_idx>(nodes_.size());
        nodes_.push_back(std::move(node));
        return idx;
    }

    flux_node const& at(node_idx idx) const { return nodes_.at(idx); }
    flux_node&       at(node_idx idx)       { return nodes_.at(idx); }

    std::size_t size() const noexcept { return nodes_.size(); }

    // Visit a node by index
    template<typename Visitor>
    decltype(auto) visit(node_idx idx, Visitor&& v) const {
        return std::visit(std::forward<Visitor>(v), at(idx));
    }

private:
    std::vector<flux_node> nodes_;
};

} // namespace flux
```

---

## CST → AST Builder

The builder walks the CST green tree and constructs AST nodes.

```cpp
// include/languages/flux/build_ast.hpp
#pragma once
#include "ast_arena.hpp"
#include "grammar.hpp"
#include <languages/generic/generic.hpp>

namespace flux {

class ast_builder {
public:
    explicit ast_builder(lang::green_arena<flux_kind> const& cst,
                         std::string_view src)
        : cst_(cst), src_(src) {}

    // Top-level entry: build the whole program
    node_idx build_program();

private:
    node_idx build_declaration(lang::green_node<flux_kind> const& node);
    node_idx build_expression(lang::green_node<flux_kind> const& node);
    node_idx build_let_decl(lang::green_node<flux_kind> const& node);
    node_idx build_fn_decl(lang::green_node<flux_kind> const& node);
    node_idx build_input_decl(lang::green_node<flux_kind> const& node);
    node_idx build_block(lang::green_node<flux_kind> const& node);
    node_idx build_call_expr(lang::green_node<flux_kind> const& node);
    node_idx build_binary_expr(lang::green_node<flux_kind> const& node);
    node_idx build_if_expr(lang::green_node<flux_kind> const& node);
    node_idx build_array_expr(lang::green_node<flux_kind> const& node);
    node_idx build_range_expr(lang::green_node<flux_kind> const& node);
    node_idx build_lambda(lang::green_node<flux_kind> const& node);

    flux_type build_type(lang::green_node<flux_kind> const& node);

    std::string token_text(lang::green_node<flux_kind> const& node) const;

    lang::green_arena<flux_kind> const& cst_;
    std::string_view src_;
    ast_arena arena_;
    uint32_t next_var_id_ = 0;

    flux_type fresh_var() { return flux_type::fresh_var(next_var_id_++); }
};

// Build from parse output
ast_arena build_ast(lang::samasa::parse_output<flux_kind, token_kind> const& parse,
                    std::string_view src);

} // namespace flux
```

### Key Builder Methods

```cpp
// build_expression dispatch — matches on CST node kind
node_idx ast_builder::build_expression(lang::green_node<flux_kind> const& node) {
    switch (node.kind()) {
        case flux_kind::binary_expr:   return build_binary_expr(node);
        case flux_kind::call_expr:     return build_call_expr(node);
        case flux_kind::if_expr:       return build_if_expr(node);
        case flux_kind::array_expr:    return build_array_expr(node);
        case flux_kind::lambda_expr:   return build_lambda(node);
        case flux_kind::range_expr:    return build_range_expr(node);
        case flux_kind::identifier:
            return arena_.add(identifier_node{
                .name = token_text(node),
                .type = fresh_var(),   // filled by type inference
                .decl = null_node      // filled by name resolution
            });
        case flux_kind::integer_literal:
            return arena_.add(integer_literal_node{
                .value = std::stoll(token_text(node)),
                .type  = flux_type::i64()
            });
        case flux_kind::float_literal:
            return arena_.add(float_literal_node{
                .value = std::stod(token_text(node)),
                .type  = fresh_var()
            });
        default:
            // Should not happen after a successful parse
            return null_node;
    }
}
```

---

## Complete Example

```cpp
// ch03_example.cpp
#include <lithe/lithe.hpp>
#include <languages/samasa/samasa.hpp>
#include <print>

#include "grammar.hpp"
#include "build_ast.hpp"

int main() {
    std::string_view src = R"(
        input x : f32
        input y : f32
        let distance = sqrt(x*x + y*y)
    )";

    auto parse_result = flux::parse(src);
    if (!parse_result.success) { std::println("parse failed"); return 1; }

    auto arena = flux::build_ast(parse_result, src);

    // arena[0] is the program node
    auto const& prog = std::get<flux::program_node>(arena.at(0));
    std::println("Program: {} decls", prog.decls.size());

    for (auto idx : prog.decls) {
        arena.visit(idx, [](auto const& node) {
            if constexpr (std::is_same_v<std::decay_t<decltype(node)>, flux::let_decl_node>)
                std::println("  let {} = <expr>", node.name);
            else if constexpr (std::is_same_v<std::decay_t<decltype(node)>, flux::input_decl_node>)
                std::println("  input {} : <type>", node.name);
        });
    }
}
```

---

## What We Have

- `flux_type` — tagged sum: primitives, vectors, matrices, tensors, tuples, functions, unification vars
- `flux_node` — 20-variant AST node covering all Flux constructs
- `ast_arena` — flat index-based node store
- `ast_builder` — CST → AST builder

---

## Next

[Chapter 4 → Name Resolution](ch04_name_resolution.md) — bind every identifier to its declaration.
