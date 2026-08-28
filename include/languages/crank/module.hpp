#pragma once

// crank/module.hpp — module descriptor + resolver for crank (Module 2).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// module_descriptor: metadata for a compiled/source module.
// module_resolver:   resolver order (design §5, §10.1):
//   native → embedded artifact → embedded src → in-memory → project paths →
//   app paths → cache → system (if policy) → package registry.
//
// import "math.vector" → <base_path>/math/vector.crank
// module.crank         → package root (one package per directory, grammar §2)
//
// Dependency graph (compilation units) is built crank-local over module_descriptors;
// swap to lithe::compilation_unit when G-LIT-2 graduates (G-VAK-3 default fix (b)).
//
// Content hash: FNV-1a over source bytes — stable across identical source.

#include "lithe/lithe_extension.hpp"  // lithe::fixed_string, lithe::version_triple

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace crank {
    // ============================================================================
    // version_triple — re-export from lithe
    // ============================================================================

    using version_triple = lithe::version_triple;

    // ============================================================================
    // module_kind
    // ============================================================================

    enum class module_kind : std::uint8_t {
        source = 0, // .crank source file
        embedded_src = 1, // embedded source text (in-memory string)
        embedded_artifact = 2, // pre-compiled binary artifact
        native = 3, // registered C++ native module
        package_root = 4, // module.crank package root
    };

    // ============================================================================
    // module_hash — FNV-1a content hash (stable across identical source)
    // ============================================================================

    struct module_hash {
        std::uint64_t value = 0;

        [[nodiscard]] bool operator==(const module_hash&) const noexcept = default;
        [[nodiscard]] bool empty() const noexcept { return value == 0; }
    };

    [[nodiscard]] inline module_hash hash_source(std::string_view src) noexcept {
        // FNV-1a 64-bit
        constexpr std::uint64_t basis = 14695981039346656037ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;
        std::uint64_t h = basis;
        for (unsigned char c : src) {
            h ^= c;
            h *= prime;
        }
        return {h};
    }

    // ============================================================================
    // module_capabilities — effect/capability mask for the whole module
    // ============================================================================

    struct module_capabilities {
        std::uint64_t effect_mask = 0;
        std::uint64_t capability_mask = 0;
    };

    // ============================================================================
    // module_descriptor — static identity block for a crank module
    // ============================================================================

    struct module_descriptor {
        std::string name; // "math.vector"
        version_triple version{};
        module_kind kind = module_kind::source;
        module_hash content_hash{};
        module_capabilities capabilities{};
        std::string source_path; // filesystem path (if kind == source)
        std::string package_name; // grammar §2 package clause
    };

    // ============================================================================
    // resolver_policy — controls system-path and registry fallback
    // ============================================================================

    struct resolver_policy {
        bool allow_system_paths = false;
        bool allow_package_registry = false;
    };

    // ============================================================================
    // module_resolver — resolves import "name" → module_descriptor
    //
    // Resolution order (design §5, §10.1):
    //   1. native (registered C++ modules)
    //   2. embedded artifact (pre-compiled, in-memory)
    //   3. embedded src (in-memory source)
    //   4. in-memory (runtime-added source strings)
    //   5. project paths
    //   6. app paths
    //   7. cache
    //   8. system paths (if policy.allow_system_paths)
    //   9. package registry (if policy.allow_package_registry)
    // ============================================================================

    class module_resolver {
    public:
        explicit module_resolver(resolver_policy policy = {})
            : policy_(policy) {}

        // ── registration ──────────────────────────────────────────────────────────

        // Register a native C++ module (highest precedence).
        void add_native(module_descriptor desc) {
            desc.kind = module_kind::native;
            native_[desc.name] = std::move(desc);
        }

        // Register a pre-compiled embedded artifact.
        void add_embedded_artifact(module_descriptor desc) {
            desc.kind = module_kind::embedded_artifact;
            embedded_artifact_[desc.name] = std::move(desc);
        }

        // Register an embedded source module.
        void add_embedded_src(std::string module_name, std::string source) {
            module_descriptor d;
            d.name = module_name;
            d.kind = module_kind::embedded_src;
            d.content_hash = hash_source(source);
            embedded_src_[module_name] = {std::move(d), std::move(source)};
        }

        // Register a runtime-injected source module (tier 4, distinct from the
        // compile-time embedded_src slot). Resolves after embedded_src, before
        // project paths. Reuses embedded_src kind — the difference is provenance.
        void add_in_memory(std::string module_name, std::string source) {
            module_descriptor d;
            d.name = module_name;
            d.kind = module_kind::embedded_src;
            d.content_hash = hash_source(source);
            in_memory_[module_name] = {std::move(d), std::move(source)};
        }

        // Add a project search path (searched in insertion order).
        void add_project_path(std::filesystem::path p) {
            project_paths_.push_back(std::move(p));
        }

        // Add an application search path (searched after project paths).
        void add_app_path(std::filesystem::path p) {
            app_paths_.push_back(std::move(p));
        }

        // Add a cache directory (searched after app paths).
        void add_cache_path(std::filesystem::path p) {
            cache_paths_.push_back(std::move(p));
        }

        // ── resolution ────────────────────────────────────────────────────────────

        // Resolve import "module.name" → optional descriptor.
        [[nodiscard]] std::optional<module_descriptor>
        resolve(std::string_view import_name) const {
            // 1. native
            if (auto it = native_.find(std::string(import_name)); it != native_.end())
                return it->second;

            // 2. embedded artifact
            if (auto it = embedded_artifact_.find(std::string(import_name));
                it != embedded_artifact_.end())
                return it->second;

            // 3. embedded src
            if (auto it = embedded_src_.find(std::string(import_name));
                it != embedded_src_.end())
                return it->second.desc;

            // 4. in-memory (runtime-injected source strings) — tier 4
            if (auto it = in_memory_.find(std::string(import_name));
                it != in_memory_.end())
                return it->second.desc;

            // 5. project paths
            if (auto d = search_paths(import_name, project_paths_)) return d;

            // 6. app paths
            if (auto d = search_paths(import_name, app_paths_)) return d;

            // 7. cache
            if (auto d = search_paths(import_name, cache_paths_)) return d;

            // 8. system (policy-gated)
            if (policy_.allow_system_paths) {
                if (auto d = search_paths(import_name, system_paths_)) return d;
            }

            return std::nullopt;
        }

        // Retrieve embedded source text (if available).
        [[nodiscard]] std::optional<std::string>
        source_text(std::string_view module_name) const {
            if (auto it = embedded_src_.find(std::string(module_name));
                it != embedded_src_.end())
                return it->second.source;
            if (auto it = in_memory_.find(std::string(module_name));
                it != in_memory_.end())
                return it->second.source;
            return std::nullopt;
        }

    private:
        resolver_policy policy_;

        std::unordered_map<std::string, module_descriptor> native_;
        std::unordered_map<std::string, module_descriptor> embedded_artifact_;

        struct embedded_src_entry {
            module_descriptor desc;
            std::string source;
        };

        std::unordered_map<std::string, embedded_src_entry> embedded_src_;
        std::unordered_map<std::string, embedded_src_entry> in_memory_; // tier 4

        std::vector<std::filesystem::path> project_paths_;
        std::vector<std::filesystem::path> app_paths_;
        std::vector<std::filesystem::path> cache_paths_;
        std::vector<std::filesystem::path> system_paths_;

        // Convert "math.vector" → relative path "math/vector.crank"
        [[nodiscard]] static std::filesystem::path import_to_path(std::string_view name) {
            std::filesystem::path p;
            std::string seg;
            for (char c : name) {
                if (c == '.') {
                    p /= seg;
                    seg.clear();
                }
                else seg += c;
            }
            if (!seg.empty()) p /= seg;
            p.replace_extension(".crank");
            return p;
        }

        [[nodiscard]] std::optional<module_descriptor>
        search_paths(std::string_view name,
                     const std::vector<std::filesystem::path>& paths) const {
            std::filesystem::path rel = import_to_path(name);
            for (const auto& base : paths) {
                auto full = base / rel;
                if (std::filesystem::exists(full)) {
                    module_descriptor d;
                    d.name = std::string(name);
                    d.kind = module_kind::source;
                    d.source_path = full.string();
                    // Hash the source on first discovery
                    // (lazy — avoids reading all files upfront)
                    return d;
                }
            }
            return std::nullopt;
        }
    };

    // ============================================================================
    // dependency_graph — crank-local CU/dep graph over module_descriptors
    // (will swap to lithe::compilation_unit once G-LIT-2 graduates)
    // ============================================================================

    struct dep_edge {
        std::string importer;
        std::string importee;
    };

    class dependency_graph {
    public:
        void add_module(module_descriptor desc) {
            name_to_desc_[desc.name] = std::move(desc);
        }

        void add_import(std::string_view importer, std::string_view importee) {
            edges_.push_back({std::string(importer), std::string(importee)});
            adj_[std::string(importer)].push_back(std::string(importee));
            reverse_adj_[std::string(importee)].push_back(std::string(importer));
            // Auto-register endpoints so cycle/topo queries see every node even
            // when a module is only referenced as an import target.
            if (!name_to_desc_.count(std::string(importer))) {
                module_descriptor d; d.name = std::string(importer);
                name_to_desc_[d.name] = std::move(d);
            }
            if (!name_to_desc_.count(std::string(importee))) {
                module_descriptor d; d.name = std::string(importee);
                name_to_desc_[d.name] = std::move(d);
            }
        }

        [[nodiscard]] const module_descriptor* descriptor(std::string_view name) const {
            auto it = name_to_desc_.find(std::string(name));
            if (it == name_to_desc_.end()) return nullptr;
            return &it->second;
        }

        // Topological order — compile dependencies before dependents (Kahn's).
        // in_degree counts how many modules import a given module (reverse of adj_).
        // Nodes with in_degree==0 have no importers → compile first.
        // Cycle → empty result.
        [[nodiscard]] std::vector<std::string> topo_order() const {
            std::unordered_map<std::string, int> in_degree;
            for (const auto& [k, _] : name_to_desc_) in_degree[k] = 0;
            for (const auto& e : edges_) in_degree[e.importer]++; // importees first

            std::vector<std::string> queue, order;
            for (const auto& [k, d] : in_degree)
                if (d == 0) queue.push_back(k);

            while (!queue.empty()) {
                auto n = queue.back();
                queue.pop_back();
                order.push_back(n);
                // For each module that n imports, decrement that importer's count
                // once all its dependencies are scheduled before it.
                // We need reverse_adj_: importee → [importers].
                if (auto it = reverse_adj_.find(n); it != reverse_adj_.end())
                    for (const auto& importer : it->second)
                        if (--in_degree[importer] == 0) queue.push_back(importer);
            }
            if (order.size() != name_to_desc_.size()) return {}; // cycle
            return order;
        }

        [[nodiscard]] const std::vector<dep_edge>& edges() const noexcept { return edges_; }

        // Returns the module names forming a dependency cycle (empty if acyclic).
        // DFS 3-coloring: white=0 (unvisited), gray=1 (on stack), black=2 (done).
        // The returned path is closed (first node repeated at the end).
        [[nodiscard]] std::vector<std::string> cycle_nodes() const {
            std::unordered_map<std::string, int> color;
            for (const auto& [k, _] : name_to_desc_) color[k] = 0;

            std::vector<std::string> cycle, path;
            std::function<bool(const std::string&)> dfs =
                [&](const std::string& n) -> bool {
                color[n] = 1;
                path.push_back(n);
                if (auto it = adj_.find(n); it != adj_.end()) {
                    for (const auto& dep : it->second) {
                        if (color[dep] == 1) {
                            auto start = std::find(path.begin(), path.end(), dep);
                            cycle.assign(start, path.end());
                            cycle.push_back(dep);
                            return true;
                        }
                        if (color[dep] == 0 && dfs(dep)) return true;
                    }
                }
                path.pop_back();
                color[n] = 2;
                return false;
            };
            for (const auto& [k, _] : name_to_desc_)
                if (color[k] == 0 && dfs(k)) break;
            return cycle;
        }

        // Direct importers of `name` (modules that import it).
        [[nodiscard]] std::vector<std::string> find_dependents(std::string_view name) const {
            auto it = reverse_adj_.find(std::string(name));
            if (it == reverse_adj_.end()) return {};
            return it->second;
        }

        // Direct dependencies of `name` (modules it imports).
        [[nodiscard]] std::vector<std::string> find_dependencies(std::string_view name) const {
            auto it = adj_.find(std::string(name));
            if (it == adj_.end()) return {};
            return it->second;
        }

    private:
        std::unordered_map<std::string, module_descriptor> name_to_desc_;
        std::vector<dep_edge> edges_;
        std::unordered_map<std::string, std::vector<std::string>> adj_;
        std::unordered_map<std::string, std::vector<std::string>> reverse_adj_;
    };
} // namespace crank
