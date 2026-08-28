#pragma once

// lithe/lithe_import.hpp — Import/Export framework: adapt ANY frontend structure
// into a neutral in-memory model Lithe can optimize, and reconstruct a target.
//
// Opt-in via:  #include "lithe/lithe_import.hpp"   (NOT pulled by lithe.hpp)
// Namespace:   lithe  (semantic_model concept, import, export_to)
//
// Motivation: make Lithe a UNIVERSAL optimization engine rather than a compiler
// tied to Vākya structures. A frontend (Vākya tree, custom AST, Pravaha task
// graph) exposes a small CPO surface; import(frontend) yields a neutral model
// that downstream passes consume; export_to<Target>(model) reconstructs the
// target grammar.
//
// DISTINCT from lithe_ir: lithe_ir (de)serializes IR to text/binary wire form;
// this framework adapts LIVE in-memory structures. No overlap.
//
// Design:
//   - semantic_model is a CONCEPT + CPO surface, not a base class. A frontend
//     opts in by providing four ADL tag_invoke overloads:
//       model_root(F)              -> root node handle
//       model_children(F, handle)  -> range of child handles
//       model_op(F, handle)        -> stable op id (std::size_t)
//       model_arity(F, handle)     -> child count
//     Zero erasure on the static path — the model is a thin non-owning view.
//   - The NEUTRAL model reuses vakya::graph (dag_view / shared_expr) — the
//     existing shared-DAG carrier — so no new traversal engine is introduced.
//     import() interns a frontend into that carrier; export_to<Target>()
//     walks it and calls a symmetric build CPO on the target.
//   - Custom-AST support requires NO core edit: a user writes the CPO overloads
//     for their type (mirrors the structural_unwrap / tag_descriptor seam).
//   - Properties (vakya/property.hpp) ride the model as the attrs() channel —
//     one metadata model, not two (opt-in; only if that header is included).
//
// Constraints: C++23, header-only, no virtual, no macros, pay-for-what-you-use.

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace lithe {
    // -----------------------------------------------------------------------
    // 1) CPO surface. Each is a niebloid dispatching to a frontend-provided
    //    tag_invoke overload found by ADL.
    // -----------------------------------------------------------------------
    namespace import_cpo {
        struct model_root_t {
            template <class F>
            [[nodiscard]] constexpr auto operator()(F&& f) const
                noexcept(noexcept(tag_invoke(*this, std::forward<F>(f))))
                -> decltype(tag_invoke(*this, std::forward<F>(f))) {
                return tag_invoke(*this, std::forward<F>(f));
            }
        };

        struct model_children_t {
            template <class F, class H>
            [[nodiscard]] constexpr auto operator()(F&& f, const H& h) const
                noexcept(noexcept(tag_invoke(*this, std::forward<F>(f), h)))
                -> decltype(tag_invoke(*this, std::forward<F>(f), h)) {
                return tag_invoke(*this, std::forward<F>(f), h);
            }
        };

        struct model_op_t {
            template <class F, class H>
            [[nodiscard]] constexpr std::size_t operator()(F&& f, const H& h) const {
                return tag_invoke(*this, std::forward<F>(f), h);
            }
        };

        struct model_arity_t {
            template <class F, class H>
            [[nodiscard]] constexpr std::size_t operator()(F&& f, const H& h) const {
                return tag_invoke(*this, std::forward<F>(f), h);
            }
        };
    } // namespace import_cpo

    inline constexpr import_cpo::model_root_t model_root{};
    inline constexpr import_cpo::model_children_t model_children{};
    inline constexpr import_cpo::model_op_t model_op{};
    inline constexpr import_cpo::model_arity_t model_arity{};

    // -----------------------------------------------------------------------
    // 2) semantic_model concept — a frontend that supplies the four CPOs.
    // -----------------------------------------------------------------------
    template <class F>
    concept semantic_model = requires(F f) {
        { model_root(f) };
        { model_op(f, model_root(f)) } -> std::convertible_to<std::size_t>;
        { model_arity(f, model_root(f)) } -> std::convertible_to<std::size_t>;
        { model_children(f, model_root(f)) };
    };

    // -----------------------------------------------------------------------
    // 3) Neutral in-memory model. Reuses the vakya shared-DAG carrier so passes
    //    and exporters walk a single, framework-agnostic structure.
    //    model_node stores op id + child ids only (topology + op); payloads and
    //    metadata attach through the Property System overlay, keyed by node id.
    // -----------------------------------------------------------------------
    struct model_node {
        std::size_t op{};
        std::vector<std::size_t> children;
    };

    struct neutral_model {
        std::vector<model_node> nodes; // index == node id
        std::size_t root{};

        [[nodiscard]] std::size_t node_count() const noexcept { return nodes.size(); }
        [[nodiscard]] const model_node& at(std::size_t id) const { return nodes[id]; }
        [[nodiscard]] const model_node& root_node() const { return nodes[root]; }
    };

    // -----------------------------------------------------------------------
    // 4) import(frontend) — walk a semantic_model into a neutral_model.
    //    Post-order intern so children precede parents (topological, exporter-
    //    friendly). No structural interning here — that is Lithe's egraph/CSE
    //    job downstream; import stays a faithful structural copy.
    // -----------------------------------------------------------------------
    namespace import_detail {
        template <semantic_model F, class H>
        std::size_t import_node(const F& f, const H& h, neutral_model& out) {
            model_node n;
            n.op = model_op(f, h);

            auto children = model_children(f, h);
            for (auto&& child : children) {
                n.children.push_back(import_node(f, child, out));
            }

            const std::size_t id = out.nodes.size();
            out.nodes.push_back(std::move(n));
            return id;
        }
    } // namespace import_detail

    template <semantic_model F>
    [[nodiscard]] neutral_model import(const F& f) {
        neutral_model out;
        out.root = import_detail::import_node(f, model_root(f), out);
        return out;
    }

    // -----------------------------------------------------------------------
    // 5) export_to<Target>(model, builder) — reconstruct a target structure.
    //    Builder is a caller object supplying:
    //        using result_type = <target node type>;
    //        result_type make(op_id, std::vector<result_type> child_results);
    //    Walk is post-order so children are built before parents. Fully generic
    //    over the target type; no target grammar is named here.
    // -----------------------------------------------------------------------
    template <class Builder>
    [[nodiscard]] auto export_to(const neutral_model& m, Builder&& builder) {
        using result_t = typename std::decay_t<Builder>::result_type;

        std::vector<result_t> built(m.node_count());
        std::vector<bool> done(m.node_count(), false);

        auto build = [&](auto& self, std::size_t id) -> void {
            if (done[id]) return;
            const model_node& n = m.at(id);
            std::vector<result_t> child_results;
            child_results.reserve(n.children.size());
            for (std::size_t c : n.children) {
                self(self, c);
                child_results.push_back(built[c]);
            }
            built[id] = builder.make(n.op, std::move(child_results));
            done[id] = true;
        };
        build(build, m.root);
        return built[m.root];
    }
} // namespace lithe
