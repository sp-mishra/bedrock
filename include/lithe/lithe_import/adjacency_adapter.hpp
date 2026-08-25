#pragma once

// lithe/lithe_import/adjacency_adapter.hpp — a reference runtime frontend that
// satisfies the semantic_model concept via the tag_invoke CPO surface.
//
// This demonstrates the frontend-agnostic path: any runtime structure (custom
// AST, query plan, or a task graph such as Pravaha's TaskIR) opts into
// lithe::import by exposing the four CPOs — no core edit, no coupling to a
// specific grammar. adjacency_model is the minimal such structure: a flat node
// list with explicit op ids and child-index lists.
//
// Opt-in via:  #include "lithe/lithe_import/adjacency_adapter.hpp"
// Namespace:   lithe

#include "../lithe_import.hpp"

#include <cstddef>
#include <vector>

namespace lithe {
    // A trivial runtime DAG/tree: node i has op ops[i] and children adj[i].
    struct adjacency_model {
        std::vector<std::size_t> ops;
        std::vector<std::vector<std::size_t>> adj;
        std::size_t root_id{};
    };

    // Handle type is just the node index.
    // The four CPO overloads found by ADL (adjacency_model lives in lithe, so
    // the CPO structs — also in lithe — see these as hidden friends via ADL).

    [[nodiscard]] inline std::size_t
    tag_invoke(import_cpo::model_root_t, const adjacency_model& m) {
        return m.root_id;
    }

    [[nodiscard]] inline const std::vector<std::size_t>&
    tag_invoke(import_cpo::model_children_t, const adjacency_model& m, const std::size_t& h) {
        return m.adj[h];
    }

    [[nodiscard]] inline std::size_t
    tag_invoke(import_cpo::model_op_t, const adjacency_model& m, const std::size_t& h) {
        return m.ops[h];
    }

    [[nodiscard]] inline std::size_t
    tag_invoke(import_cpo::model_arity_t, const adjacency_model& m, const std::size_t& h) {
        return m.adj[h].size();
    }

    static_assert(semantic_model<adjacency_model>,
                  "adjacency_model must satisfy the semantic_model concept");
} // namespace lithe
