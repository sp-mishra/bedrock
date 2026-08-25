#pragma once

// lithe/lithe_import/vakya_adapter.hpp — import a Vākya expression tree into the
// neutral_model. Vākya nodes are compile-time trees (children in a tuple), so
// this adapter does a compile-time post-order walk rather than the runtime CPO
// path (which suits runtime frontends — see adjacency_adapter.hpp).
//
// Opt-in via:  #include "lithe/lithe_import/vakya_adapter.hpp"
// Namespace:   lithe

#include "../lithe_import.hpp"
#include "vakya/vakya.hpp"

#include <cstddef>
#include <tuple>
#include <utility>

namespace lithe { namespace import_detail {
        // Op id for a vakya node = its tag's stable_id (single source of truth).
        template <class E>
        std::size_t import_vakya(const E& e, neutral_model& out) {
            if constexpr (vakya::Expression<E>) {
                using tag_t = typename std::decay_t<E>::tag_type;

                model_node n;
                n.op = vakya::emit::tag_descriptor<tag_t>::stable_id;

                std::apply([&](auto const&... ch) {
                    ((n.children.push_back(import_vakya(ch, out))), ...);
                }, e.children);

                const std::size_t id = out.nodes.size();
                out.nodes.push_back(std::move(n));
                return id;
            }
            else {
                // Terminal / wrapper: leaf node, op id from structural hash low
                // bits (stable per distinct terminal value).
                model_node n;
                n.op = static_cast<std::size_t>(vakya::structural_hash(e));
                const std::size_t id = out.nodes.size();
                out.nodes.push_back(std::move(n));
                return id;
            }
        }
    } // namespace import_detail

    // import(vakya_expr) — faithful structural copy into neutral_model.
    template <class E>
        requires vakya::Expression<E>
    [[nodiscard]] neutral_model import_vakya(const E& e) {
        neutral_model out;
        out.root = import_detail::import_vakya(e, out);
        return out;
    }
} // namespace lithe
