#pragma once

// crank/std/containers.hpp — std.containers module: internal container
// algorithms projected into Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Where std.collections exposes STL container storage, std.containers exposes
// our internal container *algorithms* as pure, value-semantic free functions.
// The typed-thunk boundary is copy-in/copy-out over registered value types, so
// graphs are modeled as flat edge lists (two parallel VecInt endpoint arrays
// plus a node count) rather than a stateful graph object — the same shape the
// rest of the stdlib uses. Connected-components reuses containers::union_find
// directly; traversal builds a local CSR adjacency. Every function is pure and
// side-effect free (inputs by value, new outputs), so all carry kPure.

#include "languages/crank/std/detail/register.hpp"

#include "containers/union_find.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace crank::stdlib {
    namespace ctr_fns {
        using ivec = std::vector<std::int64_t>;

        namespace detail {
            // Build a directed CSR adjacency over [0, n) from parallel endpoint
            // arrays. Out-of-range or negative endpoints are skipped (no UB);
            // mismatched array lengths use the shorter length.
            struct csr {
                std::vector<std::size_t> offset;   // size n+1
                std::vector<std::size_t> target;   // size = edge count
            };

            [[nodiscard]] inline std::size_t node_count(std::int64_t n) noexcept {
                return n <= 0 ? std::size_t{0} : static_cast<std::size_t>(n);
            }

            [[nodiscard]] inline bool in_range(std::int64_t v, std::size_t n) noexcept {
                return v >= 0 && static_cast<std::size_t>(v) < n;
            }

            [[nodiscard]] inline csr build_csr(const ivec& from, const ivec& to, std::size_t n) {
                const std::size_t m = std::min(from.size(), to.size());
                std::vector<std::size_t> degree(n, 0);
                for (std::size_t i = 0; i < m; ++i)
                    if (in_range(from[i], n) && in_range(to[i], n))
                        ++degree[static_cast<std::size_t>(from[i])];

                csr g;
                g.offset.assign(n + 1, 0);
                for (std::size_t v = 0; v < n; ++v) g.offset[v + 1] = g.offset[v] + degree[v];
                g.target.resize(g.offset[n]);

                std::vector<std::size_t> cursor(g.offset.begin(), g.offset.end() - 1);
                for (std::size_t i = 0; i < m; ++i) {
                    if (!in_range(from[i], n) || !in_range(to[i], n)) continue;
                    const auto u = static_cast<std::size_t>(from[i]);
                    g.target[cursor[u]++] = static_cast<std::size_t>(to[i]);
                }
                return g;
            }

            // In-degree per node over valid edges (used by Kahn topo sort).
            [[nodiscard]] inline std::vector<std::size_t> indegrees(const ivec& from, const ivec& to, std::size_t n) {
                const std::size_t m = std::min(from.size(), to.size());
                std::vector<std::size_t> deg(n, 0);
                for (std::size_t i = 0; i < m; ++i)
                    if (in_range(from[i], n) && in_range(to[i], n))
                        ++deg[static_cast<std::size_t>(to[i])];
                return deg;
            }
        } // namespace detail

        // ── union_find-backed connected components (undirected) ─────────────
        //
        // Returns a component label per node in [0, n): the label is the
        // smallest node id belonging to that component, so labels are stable and
        // independent of edge order.
        [[nodiscard]] inline ivec connected_components(ivec from, ivec to, std::int64_t n) {
            const std::size_t nn = detail::node_count(n);
            containers::union_find<std::uint32_t> uf;
            uf.reserve(nn);
            for (std::size_t i = 0; i < nn; ++i) uf.make_set();

            const std::size_t m = std::min(from.size(), to.size());
            for (std::size_t i = 0; i < m; ++i)
                if (detail::in_range(from[i], nn) && detail::in_range(to[i], nn))
                    uf.unite(static_cast<std::uint32_t>(from[i]), static_cast<std::uint32_t>(to[i]));

            // Map each root to the smallest node id in its set.
            std::vector<std::int64_t> label(nn, 0);
            for (std::size_t v = 0; v < nn; ++v) label[v] = static_cast<std::int64_t>(v);
            ivec out(nn, 0);
            std::vector<std::int64_t> root_min(nn, -1);
            for (std::size_t v = 0; v < nn; ++v) {
                const auto r = uf.find(static_cast<std::uint32_t>(v));
                if (root_min[r] < 0 || static_cast<std::int64_t>(v) < root_min[r])
                    root_min[r] = static_cast<std::int64_t>(v);
            }
            for (std::size_t v = 0; v < nn; ++v)
                out[v] = root_min[uf.find(static_cast<std::uint32_t>(v))];
            return out;
        }

        [[nodiscard]] inline std::int64_t component_count(ivec from, ivec to, std::int64_t n) {
            const std::size_t nn = detail::node_count(n);
            containers::union_find<std::uint32_t> uf;
            uf.reserve(nn);
            for (std::size_t i = 0; i < nn; ++i) uf.make_set();
            const std::size_t m = std::min(from.size(), to.size());
            for (std::size_t i = 0; i < m; ++i)
                if (detail::in_range(from[i], nn) && detail::in_range(to[i], nn))
                    uf.unite(static_cast<std::uint32_t>(from[i]), static_cast<std::uint32_t>(to[i]));
            std::int64_t roots = 0;
            for (std::size_t v = 0; v < nn; ++v)
                if (uf.find(static_cast<std::uint32_t>(v)) == static_cast<std::uint32_t>(v)) ++roots;
            return roots;
        }

        [[nodiscard]] inline bool same_component(ivec from, ivec to, std::int64_t n, std::int64_t a, std::int64_t b) {
            const std::size_t nn = detail::node_count(n);
            if (!detail::in_range(a, nn) || !detail::in_range(b, nn)) return false;
            containers::union_find<std::uint32_t> uf;
            uf.reserve(nn);
            for (std::size_t i = 0; i < nn; ++i) uf.make_set();
            const std::size_t m = std::min(from.size(), to.size());
            for (std::size_t i = 0; i < m; ++i)
                if (detail::in_range(from[i], nn) && detail::in_range(to[i], nn))
                    uf.unite(static_cast<std::uint32_t>(from[i]), static_cast<std::uint32_t>(to[i]));
            return uf.connected(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
        }

        // ── directed traversal over CSR adjacency ───────────────────────────
        [[nodiscard]] inline ivec bfs_order(ivec from, ivec to, std::int64_t n, std::int64_t start) {
            const std::size_t nn = detail::node_count(n);
            if (!detail::in_range(start, nn)) return {};
            const auto g = detail::build_csr(from, to, nn);
            std::vector<char> seen(nn, 0);
            ivec order;
            order.reserve(nn);
            std::vector<std::size_t> queue;
            queue.reserve(nn);
            std::size_t head = 0;
            const auto s = static_cast<std::size_t>(start);
            seen[s] = 1;
            queue.push_back(s);
            while (head < queue.size()) {
                const std::size_t u = queue[head++];
                order.push_back(static_cast<std::int64_t>(u));
                for (std::size_t e = g.offset[u]; e < g.offset[u + 1]; ++e) {
                    const std::size_t w = g.target[e];
                    if (!seen[w]) { seen[w] = 1; queue.push_back(w); }
                }
            }
            return order;
        }

        [[nodiscard]] inline ivec dfs_order(ivec from, ivec to, std::int64_t n, std::int64_t start) {
            const std::size_t nn = detail::node_count(n);
            if (!detail::in_range(start, nn)) return {};
            const auto g = detail::build_csr(from, to, nn);
            std::vector<char> seen(nn, 0);
            ivec order;
            order.reserve(nn);
            std::vector<std::size_t> stack;
            stack.push_back(static_cast<std::size_t>(start));
            while (!stack.empty()) {
                const std::size_t u = stack.back();
                stack.pop_back();
                if (seen[u]) continue;
                seen[u] = 1;
                order.push_back(static_cast<std::int64_t>(u));
                // Push neighbours in reverse so the lowest target is visited first.
                for (std::size_t e = g.offset[u + 1]; e > g.offset[u]; --e) {
                    const std::size_t w = g.target[e - 1];
                    if (!seen[w]) stack.push_back(w);
                }
            }
            return order;
        }

        [[nodiscard]] inline std::int64_t reachable_count(ivec from, ivec to, std::int64_t n, std::int64_t start) {
            return static_cast<std::int64_t>(bfs_order(std::move(from), std::move(to), n, start).size());
        }

        // Kahn topological order over a directed graph. Returns an empty vector
        // if the graph has a cycle (the caller distinguishes via HasCycle).
        [[nodiscard]] inline ivec topo_order(ivec from, ivec to, std::int64_t n) {
            const std::size_t nn = detail::node_count(n);
            const auto g = detail::build_csr(from, to, nn);
            auto indeg = detail::indegrees(from, to, nn);
            std::vector<std::size_t> ready;
            for (std::size_t v = 0; v < nn; ++v) if (indeg[v] == 0) ready.push_back(v);
            std::sort(ready.begin(), ready.end());   // deterministic order

            ivec order;
            order.reserve(nn);
            std::size_t head = 0;
            while (head < ready.size()) {
                const std::size_t u = ready[head++];
                order.push_back(static_cast<std::int64_t>(u));
                for (std::size_t e = g.offset[u]; e < g.offset[u + 1]; ++e) {
                    const std::size_t w = g.target[e];
                    if (--indeg[w] == 0) ready.push_back(w);
                }
            }
            if (order.size() != nn) return {};   // cycle: not all nodes emitted
            return order;
        }

        [[nodiscard]] inline bool has_cycle(ivec from, ivec to, std::int64_t n) {
            const std::size_t nn = detail::node_count(n);
            return topo_order(std::move(from), std::move(to), n).size() != nn;
        }

        // ── vector algorithms (fill the value-utility gap) ──────────────────
        [[nodiscard]] inline ivec vec_sort(ivec v) { std::sort(v.begin(), v.end()); return v; }
        [[nodiscard]] inline ivec vec_unique(ivec v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            return v;
        }
        [[nodiscard]] inline ivec vec_reverse(ivec v) { std::reverse(v.begin(), v.end()); return v; }
        [[nodiscard]] inline std::int64_t vec_sum(ivec v) noexcept {
            std::int64_t total = 0;
            for (auto x : v) total += x;
            return total;
        }
        [[nodiscard]] inline ivec vec_concat(ivec a, ivec b) {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        }
    } // namespace ctr_fns

    // install_std_containers — register the std.containers module into ctx +
    // resolver. Always-on: pure STL + containers::union_find, no optional deps.
    inline void install_std_containers(crank::context& ctx) {
        namespace c = ctr_fns;
        ffi_module_builder mod{"std.containers"};
        const function_options pure{.flags = kPure};

        ctx.register_container<c::ivec>("std.containers.VecInt");
        mod.type("std.containers.VecInt", "VecInt");

        detail::add_fn<"std.containers.connected_components", &c::connected_components>(mod, ctx, "ConnectedComponents", pure);
        detail::add_fn<"std.containers.component_count", &c::component_count>(mod, ctx, "ComponentCount", pure);
        detail::add_fn<"std.containers.same_component", &c::same_component>(mod, ctx, "SameComponent", pure);

        detail::add_fn<"std.containers.bfs_order", &c::bfs_order>(mod, ctx, "BfsOrder", pure);
        detail::add_fn<"std.containers.dfs_order", &c::dfs_order>(mod, ctx, "DfsOrder", pure);
        detail::add_fn<"std.containers.reachable_count", &c::reachable_count>(mod, ctx, "ReachableCount", pure);
        detail::add_fn<"std.containers.topo_order", &c::topo_order>(mod, ctx, "TopoOrder", pure);
        detail::add_fn<"std.containers.has_cycle", &c::has_cycle>(mod, ctx, "HasCycle", pure);

        detail::add_fn<"std.containers.vec_sort", &c::vec_sort>(mod, ctx, "VecIntSort", pure);
        detail::add_fn<"std.containers.vec_unique", &c::vec_unique>(mod, ctx, "VecIntUnique", pure);
        detail::add_fn<"std.containers.vec_reverse", &c::vec_reverse>(mod, ctx, "VecIntReverse", pure);
        detail::add_fn<"std.containers.vec_sum", &c::vec_sum>(mod, ctx, "VecIntSum", pure);
        detail::add_fn<"std.containers.vec_concat", &c::vec_concat>(mod, ctx, "VecIntConcat", pure);

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
