#pragma once

#include "lithe_core.hpp"
#include "meta/meta.hpp"
#include "containers/graph/DisjointSet.hpp"

#include <cstdint>
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <variant>
#include <vector>

#if defined(__has_include) && !defined(LITHE_HAS_NADI)
#  if __has_include("../observability/nadi.hpp")
#    include "../observability/nadi.hpp"
#    define LITHE_HAS_NADI 1
#  endif
#  if defined(LITHE_HAS_NADI) && __has_include("../observability/sinks/thread_local_sink.hpp")
#    include "../observability/sinks/thread_local_sink.hpp"
#    define LITHE_HAS_THREAD_LOCAL_SINK 1
#  endif
#  if __has_include("../utils/profiler.hpp")
#    include "../utils/profiler.hpp"
#    define LITHE_HAS_PROFILER 1
#  endif
#endif
#ifndef LITHE_HAS_NADI
#  define LITHE_HAS_NADI 0
#endif
#ifndef LITHE_HAS_THREAD_LOCAL_SINK
#  define LITHE_HAS_THREAD_LOCAL_SINK 0
#endif
#ifndef LITHE_HAS_PROFILER
#  define LITHE_HAS_PROFILER 0
#endif

namespace lithe::detail {
    // ---------------------------------------------------------------------------
    // flat_map<K,V> — sorted vector of pairs; O(log n) lookup, no heap overhead
    // beyond the vector itself.  Not thread-safe (synchronisation is the caller's
    // responsibility at the pipeline level).
    // ---------------------------------------------------------------------------
    template <typename K, typename V>
    struct flat_map {
        using value_type = std::pair<K, V>;
        using storage_type = std::vector<value_type>;

        storage_type data;

        [[nodiscard]] auto find(const K& key) {
            auto it = std::lower_bound(data.begin(), data.end(), key,
                                       [](const value_type& e, const K& k) { return e.first < k; });
            return (it != data.end() && it->first == key) ? it : data.end();
        }

        [[nodiscard]] auto find(const K& key) const {
            auto it = std::lower_bound(data.begin(), data.end(), key,
                                       [](const value_type& e, const K& k) { return e.first < k; });
            return (it != data.end() && it->first == key) ? it : data.end();
        }

        [[nodiscard]] bool contains(const K& key) const { return find(key) != data.end(); }

        V& operator[](const K& key) {
            auto it = std::lower_bound(data.begin(), data.end(), key,
                                       [](const value_type& e, const K& k) { return e.first < k; });
            if (it != data.end() && it->first == key) return it->second;
            return data.emplace(it, key, V{})->second;
        }

        V& operator[](K&& key) {
            auto it = std::lower_bound(data.begin(), data.end(), key,
                                       [](const value_type& e, const K& k) { return e.first < k; });
            if (it != data.end() && it->first == key) return it->second;
            return data.emplace(it, std::move(key), V{})->second;
        }

        template <typename... Args>
        std::pair<typename storage_type::iterator, bool>
        emplace(K key, Args&&... args) {
            auto it = std::lower_bound(data.begin(), data.end(), key,
                                       [](const value_type& e, const K& k) { return e.first < k; });
            if (it != data.end() && it->first == key) return {it, false};
            it = data.emplace(it, std::move(key), V(std::forward<Args>(args)...));
            return {it, true};
        }

        void insert_or_assign(K key, V value) {
            auto it = std::lower_bound(data.begin(), data.end(), key,
                                       [](const value_type& e, const K& k) { return e.first < k; });
            if (it != data.end() && it->first == key) {
                it->second = std::move(value);
                return;
            }
            data.emplace(it, std::move(key), std::move(value));
        }

        std::size_t erase(const K& key) {
            auto it = find(key);
            if (it == data.end()) return 0;
            data.erase(it);
            return 1;
        }

        [[nodiscard]] bool empty() const noexcept { return data.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return data.size(); }
        void clear() noexcept { data.clear(); }
        auto begin() { return data.begin(); }
        auto end() { return data.end(); }
        auto begin() const { return data.begin(); }
        auto end() const { return data.end(); }

        [[nodiscard]] std::size_t count(const K& key) const { return contains(key) ? 1 : 0; }

        const V& at(const K& key) const {
            auto it = find(key);
            if (it == data.end()) throw std::out_of_range("flat_map::at");
            return it->second;
        }

        V& at(const K& key) {
            auto it = find(key);
            if (it == data.end()) throw std::out_of_range("flat_map::at");
            return it->second;
        }
    };

    // ---------------------------------------------------------------------------
    // flat_set<K> — sorted vector; O(log n) lookup.
    // ---------------------------------------------------------------------------
    template <typename K>
    struct flat_set {
        std::vector<K> data;

        [[nodiscard]] auto find(const K& key) const {
            auto it = std::lower_bound(data.begin(), data.end(), key);
            return (it != data.end() && *it == key) ? it : data.end();
        }

        // Heterogeneous lookup: accepts any type comparable to K (e.g. string_view for string).
        template <typename Q>
            requires std::is_convertible_v<Q, K> || requires(const K& k, const Q& q) { k == q; k < q; }
        [[nodiscard]] auto find(const Q& key) const {
            auto it = std::lower_bound(data.begin(), data.end(), key,
                                       [](const K& elem, const Q& k) { return elem < k; });
            return (it != data.end() && *it == key) ? it : data.end();
        }

        [[nodiscard]] bool contains(const K& key) const { return find(key) != data.end(); }

        template <typename Q>
            requires std::is_convertible_v<Q, K> || requires(const K& k, const Q& q) { k == q; k < q; }
        [[nodiscard]] bool contains(const Q& key) const { return find(key) != data.end(); }

        std::pair<typename std::vector<K>::iterator, bool> insert(K key) {
            auto it = std::lower_bound(data.begin(), data.end(), key);
            if (it != data.end() && *it == key) return {it, false};
            it = data.insert(it, std::move(key));
            return {it, true};
        }

        std::size_t erase(const K& key) {
            auto it = std::lower_bound(data.begin(), data.end(), key);
            if (it == data.end() || *it != key) return 0;
            data.erase(it);
            return 1;
        }

        [[nodiscard]] bool empty() const noexcept { return data.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return data.size(); }
        void clear() noexcept { data.clear(); }
        auto begin() const { return data.begin(); }
        auto end() const { return data.end(); }
        auto begin() { return data.begin(); }
        auto end() { return data.end(); }
    };
} // namespace lithe::detail

#include "lithe_semantic_inference.hpp"

// Keep the remainder of this umbrella header explicitly inside lithe. The
// inference fragment above is self-contained and closes its own namespaces.
namespace lithe {
namespace analysis {
    struct complexity_analyzer {
        mutable std::size_t node_count = 0;
        mutable std::size_t depth = 0;
        mutable std::size_t max_depth = 0;
        mutable ::lithe::detail::flat_map<std::size_t, std::size_t> operation_costs;

        template <class T>
        std::size_t on_terminal(T&&) {
            ++node_count;
            return 1;
        }

        template <class Tag, class... Children>
        std::size_t on_node(Tag, Children&&... children) {
            ++depth;
            max_depth = std::max(max_depth, depth);
            ++node_count;

            std::size_t op_cost = get_operation_cost<Tag>();
            operation_costs[typeid(Tag).hash_code()] = op_cost;

            std::size_t total_cost = op_cost;
            ((total_cost += children), ...);

            --depth;
            return total_cost;
        }

        template <class Tag>
        constexpr std::size_t get_operation_cost() const {
            if constexpr (std::is_same_v<Tag, add_tag> || std::is_same_v<Tag, sub_tag>)
                return 1;
            else if constexpr (std::is_same_v<Tag, mul_tag>) {
                return 2;
            }
            else if constexpr (std::is_same_v<Tag, div_tag> || std::is_same_v<Tag, mod_tag>) {
                return 4;
            }
            else if constexpr (std::is_same_v<Tag, shl_tag> || std::is_same_v<Tag, shr_tag>) {
                return 1;
            }
            else {
                return 3;
            }
        }

        std::size_t get_node_count() const { return node_count; }
        std::size_t get_max_depth() const { return max_depth; }

        std::size_t get_total_cost() const {
            std::size_t total = 0;
            for (const auto& [op, cost] : operation_costs) {
                total += cost;
            }
            return total;
        }
    };

    struct frequency_analyzer {
        mutable ::lithe::detail::flat_map<std::size_t, std::size_t> subexpr_frequencies;
        mutable ::lithe::detail::flat_map<std::size_t, std::size_t> operation_frequencies;

        template <class T>
        void on_terminal(T&& t) {
            auto hash = emit::structural_hash(t);
            ++subexpr_frequencies[hash];
        }

        template <class Tag, class... Children>
        void on_node(Tag, Children&&... children) {
            auto op_hash = typeid(Tag).hash_code();
            ++operation_frequencies[op_hash];
            (on_terminal(children), ...);
        }

        std::size_t get_frequency(const std::size_t hash) const {
            auto it = subexpr_frequencies.find(hash);
            return it != subexpr_frequencies.end() ? it->second : 0;
        }

        std::size_t get_operation_frequency(const std::size_t op_hash) const {
            auto it = operation_frequencies.find(op_hash);
            return it != operation_frequencies.end() ? it->second : 0;
        }

        std::vector<std::pair<std::size_t, std::size_t>> get_common_subexprs(
            const std::size_t min_frequency = 2) const {
            std::vector<std::pair<std::size_t, std::size_t>> result;
            for (const auto& [hash, freq] : subexpr_frequencies) {
                if (freq >= min_frequency) {
                    result.emplace_back(hash, freq);
                }
            }
            std::sort(result.begin(), result.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            return result;
        }
    };

    struct side_effect_analyzer {
        enum class effect_type {
            pure,
            read_only,
            writes_local,
            writes_global,
            io_operation,
            throws,
            terminates
        };

        mutable ::lithe::detail::flat_map<std::size_t, effect_type> expression_effects;

        template <class T>
        effect_type on_terminal(T&& t) {
            auto hash = emit::structural_hash(t);
            effect_type effect = classify_terminal_effects<std::decay_t<T>>();
            expression_effects[hash] = effect;
            return effect;
        }

        template <class Tag, class... Children>
        effect_type on_node(Tag, Children&&... children) {
            effect_type op_effect = classify_operation_effects<Tag>();
            effect_type combined_effect = op_effect;
            ((combined_effect = combine_effects(combined_effect, children)), ...);
            return combined_effect;
        }

        template <class T>
        constexpr effect_type classify_terminal_effects() const {
            if constexpr (std::is_arithmetic_v<T>) {
                return effect_type::pure;
            }
            else {
                return effect_type::read_only;
            }
        }

        template <class Tag>
        constexpr effect_type classify_operation_effects() const {
            if constexpr (std::is_same_v<Tag, add_tag> || std::is_same_v<Tag, sub_tag> ||
                std::is_same_v<Tag, mul_tag> || std::is_same_v<Tag, neg_tag> ||
                std::is_same_v<Tag, shl_tag> || std::is_same_v<Tag, shr_tag>) {
                return effect_type::pure;
            }
            else if constexpr (std::is_same_v<Tag, div_tag> || std::is_same_v<Tag, mod_tag>) {
                return effect_type::throws;
            }
            else if constexpr (std::is_same_v<Tag, call_tag>) {
                return effect_type::writes_global;
            }
            else {
                return effect_type::read_only;
            }
        }

        static constexpr effect_type combine_effects(effect_type a, effect_type b) {
            return static_cast<effect_type>(
                std::max(static_cast<int>(a), static_cast<int>(b))
            );
        }

        bool is_pure(const std::size_t hash) const {
            auto it = expression_effects.find(hash);
            return it != expression_effects.end() && it->second == effect_type::pure;
        }

        bool can_reorder_with(const std::size_t hash1, const std::size_t hash2) const {
            auto it1 = expression_effects.find(hash1);
            auto it2 = expression_effects.find(hash2);

            if (it1 == expression_effects.end() || it2 == expression_effects.end()) {
                return false;
            }

            effect_type e1 = it1->second, e2 = it2->second;
            if (e1 == effect_type::pure && e2 == effect_type::pure) return true;
            if (e1 == effect_type::read_only && e2 == effect_type::read_only) return true;
            if ((e1 == effect_type::pure && e2 == effect_type::read_only) ||
                (e1 == effect_type::read_only && e2 == effect_type::pure))
                return true;
            return false;
        }
    };

    struct dependency_analyzer {
        struct dependency_info {
            ::lithe::detail::flat_set<std::size_t> depends_on;
            ::lithe::detail::flat_set<std::size_t> depended_by;
            bool has_data_dependency = false;
            bool has_control_dependency = false;
            bool has_anti_dependency = false;
            bool has_output_dependency = false;
        };

        mutable ::lithe::detail::flat_map<std::size_t, dependency_info> dependency_graph;
        mutable std::vector<std::size_t> topological_order;

        template <class T>
        void on_terminal(T&& t) {
            auto hash = emit::structural_hash(t);
            dependency_graph[hash];
        }

        template <class Tag, class... Children>
        void on_node(Tag, Children&&... children) const {
            auto expr_hash = calculate_node_hash<Tag>(children...);
            auto& deps = dependency_graph[expr_hash];
            ((add_dependency(expr_hash, emit::structural_hash(children))), ...);
            classify_dependencies<Tag>(expr_hash, children...);
        }

        template <class Tag, class... Children>
        std::size_t calculate_node_hash(Children&&... children) const {
            std::size_t h = typeid(Tag).hash_code();
            ((h = emit::hash_combine(h, emit::structural_hash(children))), ...);
            return h;
        }

        void add_dependency(const std::size_t dependent, const std::size_t dependency) const {
            dependency_graph[dependent].depends_on.insert(dependency);
            dependency_graph[dependency].depended_by.insert(dependent);
            dependency_graph[dependent].has_data_dependency = true;
        }

        template <class Tag, class... Children>
        void classify_dependencies(const std::size_t expr_hash, Children&&... children) const {
            auto& info = dependency_graph[expr_hash];

            if constexpr (std::is_same_v<Tag, if_tag>) {
                info.has_control_dependency = true;
            }
            else if constexpr (std::is_same_v<Tag, call_tag>) {
                info.has_output_dependency = true;
            }
        }

        std::vector<std::size_t> get_safe_execution_order() const {
            topological_order.clear();
            ::lithe::detail::flat_set<std::size_t> visited;
            ::lithe::detail::flat_set<std::size_t> temp_visited;

            for (const auto& [hash, info] : dependency_graph) {
                if (!visited.contains(hash)) {
                    if (!topological_visit(hash, visited, temp_visited)) {
                        return {};
                    }
                }
            }

            std::reverse(topological_order.begin(), topological_order.end());
            return topological_order;
        }

        bool topological_visit(const std::size_t node,
                               ::lithe::detail::flat_set<std::size_t>& visited,
                               ::lithe::detail::flat_set<std::size_t>& temp_visited) const {
            if (temp_visited.contains(node)) {
                return false;
            }
            if (visited.contains(node)) {
                return true;
            }

            temp_visited.insert(node);
            auto it = dependency_graph.find(node);
            if (it != dependency_graph.end()) {
                for (auto dep : it->second.depends_on) {
                    if (!topological_visit(dep, visited, temp_visited)) {
                        return false;
                    }
                }
            }

            temp_visited.erase(node);
            visited.insert(node);
            topological_order.push_back(node);
            return true;
        }

        bool has_cycle() const {
            return get_safe_execution_order().empty() && !dependency_graph.empty();
        }

        bool can_execute_concurrently(const std::size_t hash1, const std::size_t hash2) const {
            auto it1 = dependency_graph.find(hash1);
            auto it2 = dependency_graph.find(hash2);

            if (it1 == dependency_graph.end() || it2 == dependency_graph.end()) {
                return false;
            }

            return it1->second.depends_on.find(hash2) == it1->second.depends_on.end() &&
                it2->second.depends_on.find(hash1) == it2->second.depends_on.end();
        }
    };

    struct dag_constructor {
        struct dag_node {
            std::size_t id;
            std::string operation;
            std::vector<std::size_t> operands;
            std::size_t use_count = 0;
            bool is_terminal = false;
        };

        mutable ::lithe::detail::flat_map<std::size_t, dag_node> dag_nodes;
        mutable ::lithe::detail::flat_map<std::size_t, std::size_t> hash_to_dag_id;
        mutable std::size_t next_id = 1;

        template <class T>
        std::size_t on_terminal(T&& t) {
            auto hash = emit::structural_hash(t);

            if (auto it = hash_to_dag_id.find(hash); it != hash_to_dag_id.end()) {
                ++dag_nodes[it->second].use_count;
                return it->second;
            }

            auto id = next_id++;
            dag_node node{id, emit::dump(t), {}, 1, true};
            dag_nodes[id] = std::move(node);
            hash_to_dag_id[hash] = id;
            return id;
        }

        template <class Tag, class... Children>
        std::size_t on_node(Tag, Children&&... children) {
            std::vector<std::size_t> operand_ids = {children...};
            auto expr_hash = calculate_expression_hash<Tag>(operand_ids);

            if (auto it = hash_to_dag_id.find(expr_hash); it != hash_to_dag_id.end()) {
                ++dag_nodes[it->second].use_count;
                return it->second;
            }

            auto id = next_id++;
            dag_node node{id, get_operation_name<Tag>(), operand_ids, 1, false};
            dag_nodes[id] = std::move(node);
            hash_to_dag_id[expr_hash] = id;
            return id;
        }

        template <class Tag>
        std::string get_operation_name() const {
            if constexpr (std::is_same_v<Tag, add_tag>) return "+";
            else if constexpr (std::is_same_v<Tag, sub_tag>) return "-";
            else if constexpr (std::is_same_v<Tag, mul_tag>) return "*";
            else if constexpr (std::is_same_v<Tag, div_tag>) return "/";
            else if constexpr (std::is_same_v<Tag, neg_tag>) return "neg";
            else return "op";
        }

        template <class Tag>
        std::size_t calculate_expression_hash(const std::vector<std::size_t>& operands) const {
            std::size_t h = typeid(Tag).hash_code();
            for (auto op_id : operands) {
                h = emit::hash_combine(h, op_id);
            }
            return h;
        }

        std::string generate_dot() const {
            std::string dot = "digraph DAG {\n";

            for (const auto& [id, node] : dag_nodes) {
                if (node.is_terminal) {
                    dot += "  " + std::to_string(id) + " [label=\"" + node.operation +
                        " (x" + std::to_string(node.use_count) + ")\", shape=box];\n";
                }
                else {
                    dot += "  " + std::to_string(id) + " [label=\"" + node.operation +
                        " (x" + std::to_string(node.use_count) + ")\"];\n";

                    for (auto operand : node.operands) {
                        dot += "  " + std::to_string(operand) + " -> " + std::to_string(id) + ";\n";
                    }
                }
            }

            dot += "}\n";
            return dot;
        }

        std::size_t get_node_count() const { return dag_nodes.size(); }

        std::size_t get_shared_nodes() const {
            std::size_t count = 0;
            for (const auto& [id, node] : dag_nodes) {
                if (node.use_count > 1) ++count;
            }
            return count;
        }

        double get_sharing_ratio() const {
            return get_node_count() > 0 ? static_cast<double>(get_shared_nodes()) / get_node_count() : 0.0;
        }
    };

    template <class Expr>
    struct analysis_results {
        std::size_t node_count{};
        std::size_t max_depth{};
        std::size_t total_cost{};
        std::vector<std::pair<std::size_t, std::size_t>> common_subexprs;
        bool has_dependency_cycles{};
        double dag_sharing_ratio{};

        [[nodiscard]] bool is_optimization_worthwhile() const {
            return total_cost > 10 ||
                !common_subexprs.empty() ||
                dag_sharing_ratio > 0.3;
        }

        [[nodiscard]] std::vector<std::size_t> get_optimization_targets() const {
            std::vector<std::size_t> targets;
            for (const auto& hash : common_subexprs | std::views::keys) {
                targets.push_back(hash);
            }
            return targets;
        }
    };

    template <class Expr>
    analysis_results<Expr> analyze(const Expr& expr) {
        complexity_analyzer complexity;
        frequency_analyzer frequency;
        side_effect_analyzer effects;
        dependency_analyzer dependencies;
        dag_constructor dag;

        lithe::visit(expr, complexity);
        lithe::visit(expr, frequency);
        lithe::visit(expr, effects);
        lithe::visit(expr, dependencies);
        lithe::visit(expr, dag);

        return analysis_results<Expr>{
            complexity.get_node_count(),
            complexity.get_max_depth(),
            complexity.get_total_cost(),
            frequency.get_common_subexprs(2),
            dependencies.has_cycle(),
            dag.get_sharing_ratio()
        };
    }
} // namespace analysis

// -------------------------------------------------------------------------
// Phase 2: Polymorphic, Semantic-Aware Domain Folding Infrastructure
// -------------------------------------------------------------------------
namespace folding {
    // Lightweight scalar operand for compile-time-safe fold operations.
    // Uses a closed variant so folders can work in constexpr contexts without
    // pulling in the full codegen allocated_operand type.
    struct fold_operand {
        enum class kind : std::uint8_t { none, i64, f64 };

        kind type = kind::none;
        std::variant<std::monostate, std::int64_t, double> value;

        [[nodiscard]] static constexpr fold_operand from_i64(std::int64_t v) noexcept {
            return {kind::i64, v};
        }

        [[nodiscard]] static constexpr fold_operand from_f64(double v) noexcept {
            return {kind::f64, v};
        }

        [[nodiscard]] constexpr bool is_none() const noexcept { return type == kind::none; }
        [[nodiscard]] constexpr bool is_i64() const noexcept { return type == kind::i64; }
        [[nodiscard]] constexpr bool is_f64() const noexcept { return type == kind::f64; }

        [[nodiscard]] constexpr std::int64_t as_i64() const noexcept {
            return std::get<std::int64_t>(value);
        }

        [[nodiscard]] constexpr double as_f64() const noexcept {
            return std::get<double>(value);
        }
    };

    // A fold_result is either empty (no fold possible) or a single folded scalar.
    using fold_result = std::optional<fold_operand>;

    // Stable string key identifying an operation within a domain, used to route
    // fold requests without relying on RTTI or virtual dispatch.
    struct fold_op_key {
        std::string_view domain; // e.g. "lithe.core"
        std::string_view name; // e.g. "add", "mul"

        [[nodiscard]] constexpr bool operator==(const fold_op_key&) const noexcept = default;
    };

    // -----------------------------------------------------------------------
    // DomainFolder concept
    //
    // A type F satisfies DomainFolder if it exposes a constexpr try_fold that
    // accepts the operation key and a fixed-size span of fold_operand inputs,
    // returning a fold_result (empty = cannot fold, non-empty = folded value).
    // The span avoids heap allocation so the entire path can be constexpr.
    // -----------------------------------------------------------------------
    template <typename F>
    concept DomainFolder = requires(const F& folder,
                                    fold_op_key op,
                                    std::span<const fold_operand> uses) {
        { folder.try_fold(op, uses) } -> std::same_as<fold_result>;
    };

    // -----------------------------------------------------------------------
    // Built-in folder for domain_type::arithmetic
    // Handles scalar integer/float constant folding for the core lithe.core ops.
    // -----------------------------------------------------------------------
    struct arithmetic_folder {
        [[nodiscard]] constexpr fold_result
        try_fold(fold_op_key op, const std::span<const fold_operand> uses) const noexcept {
            if (op.domain != "lithe.core") return std::nullopt;

            // Binary ops — require exactly two fully-known operands.
            if (uses.size() == 2 && !uses[0].is_none() && !uses[1].is_none()) {
                const bool both_i64 = uses[0].is_i64() && uses[1].is_i64();
                const bool either_f64 = uses[0].is_f64() || uses[1].is_f64();

                if (op.name == "+") {
                    if (both_i64)
                        return fold_operand::from_i64(uses[0].as_i64() + uses[1].as_i64());
                    if (either_f64) {
                        double a = uses[0].is_f64() ? uses[0].as_f64() : static_cast<double>(uses[0].as_i64());
                        double b = uses[1].is_f64() ? uses[1].as_f64() : static_cast<double>(uses[1].as_i64());
                        return fold_operand::from_f64(a + b);
                    }
                }
                if (op.name == "-") {
                    if (both_i64)
                        return fold_operand::from_i64(uses[0].as_i64() - uses[1].as_i64());
                    if (either_f64) {
                        double a = uses[0].is_f64() ? uses[0].as_f64() : static_cast<double>(uses[0].as_i64());
                        double b = uses[1].is_f64() ? uses[1].as_f64() : static_cast<double>(uses[1].as_i64());
                        return fold_operand::from_f64(a - b);
                    }
                }
                if (op.name == "*") {
                    if (both_i64)
                        return fold_operand::from_i64(uses[0].as_i64() * uses[1].as_i64());
                    if (either_f64) {
                        double a = uses[0].is_f64() ? uses[0].as_f64() : static_cast<double>(uses[0].as_i64());
                        double b = uses[1].is_f64() ? uses[1].as_f64() : static_cast<double>(uses[1].as_i64());
                        return fold_operand::from_f64(a * b);
                    }
                }
                if (op.name == "/") {
                    if (both_i64 && uses[1].as_i64() != 0)
                        return fold_operand::from_i64(uses[0].as_i64() / uses[1].as_i64());
                    if (either_f64) {
                        double a = uses[0].is_f64() ? uses[0].as_f64() : static_cast<double>(uses[0].as_i64());
                        double b = uses[1].is_f64() ? uses[1].as_f64() : static_cast<double>(uses[1].as_i64());
                        return fold_operand::from_f64(a / b);
                    }
                }
            }

            // Unary ops — require exactly one fully-known operand.
            if (uses.size() == 1 && !uses[0].is_none()) {
                if (op.name == "neg") {
                    if (uses[0].is_i64()) return fold_operand::from_i64(-uses[0].as_i64());
                    if (uses[0].is_f64()) return fold_operand::from_f64(-uses[0].as_f64());
                }
            }

            return std::nullopt;
        }
    };

    static_assert(DomainFolder<arithmetic_folder>);

    // -----------------------------------------------------------------------
    // Built-in folder for domain_type::tensor
    // Placeholder: folds element-wise scalar identity operations only.
    // Domain-specific implementations replace this via specialization.
    // -----------------------------------------------------------------------
    struct tensor_folder {
        [[nodiscard]] constexpr fold_result
        try_fold(fold_op_key op, const std::span<const fold_operand> uses) const noexcept {
            // Scalar add/mul on known constants behave identically to arithmetic.
            if (op.domain == "tensor" || op.domain == "lithe.tensor") {
                if (op.name == "scalar_add" && uses.size() == 2 &&
                    uses[0].is_i64() && uses[1].is_i64())
                    return fold_operand::from_i64(uses[0].as_i64() + uses[1].as_i64());
                if (op.name == "scalar_mul" && uses.size() == 2 &&
                    uses[0].is_i64() && uses[1].is_i64())
                    return fold_operand::from_i64(uses[0].as_i64() * uses[1].as_i64());
            }
            return std::nullopt;
        }
    };

    static_assert(DomainFolder<tensor_folder>);

    // -----------------------------------------------------------------------
    // Built-in folder for domain_type::symbolic (quantum gate domain)
    // Cancels pairs of self-inverse gates (e.g. X·X → I, H·H → I).
    // A cancelled pair folds to the integer identity value 1.
    // -----------------------------------------------------------------------
    struct quantum_folder {
        [[nodiscard]] constexpr fold_result
        try_fold(fold_op_key op, std::span<const fold_operand> /*uses*/) const noexcept {
            // Self-inverse gate cancellation: gate(gate(q)) → identity.
            // The "operands" here are already-folded gate ids; if both inputs
            // equal the same gate constant we return the identity (1).
            if ((op.domain == "quantum" || op.domain == "lithe.quantum") &&
                op.name == "cancel_self_inverse")
                return fold_operand::from_i64(1); // identity element

            return std::nullopt;
        }
    };

    static_assert(DomainFolder<quantum_folder>);

    // -----------------------------------------------------------------------
    // domain_folder_for<D>
    //
    // Primary template: no folder available (selecting unknown/unsupported
    // domains is a hard compile-time error via static_assert in the pass).
    // Specialize for each domain_type bit value to wire in a folder type.
    // -----------------------------------------------------------------------
    template <semantic::domain_type D>
    struct domain_folder_for; // intentionally incomplete — specializations below

    template <>
    struct domain_folder_for<semantic::domain_type::arithmetic> {
        using type = arithmetic_folder;
    };

    template <>
    struct domain_folder_for<semantic::domain_type::tensor> {
        using type = tensor_folder;
    };

    template <>
    struct domain_folder_for<semantic::domain_type::symbolic> {
        using type = quantum_folder;
    };

    // Convenience alias.
    template <semantic::domain_type D>
    using domain_folder_for_t = typename domain_folder_for<D>::type;

    // -----------------------------------------------------------------------
    // make_folder<D>()
    // Factory returning the default-constructed folder for domain D.
    // -----------------------------------------------------------------------
    template <semantic::domain_type D>
    [[nodiscard]] constexpr domain_folder_for_t<D> make_folder() noexcept {
        return {};
    }

    // -----------------------------------------------------------------------
    // Helpers to extract a fold_operand from an AST terminal value.
    // Used by domain_folding_rule to convert C++ arithmetic leaves.
    // Handles plain scalars and std::variant carriers produced by sub-folds.
    // -----------------------------------------------------------------------
    template <class T>
    [[nodiscard]] constexpr fold_operand to_fold_operand(const T& v) noexcept {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
            return fold_operand::from_i64(static_cast<std::int64_t>(v));
        else if constexpr (std::is_floating_point_v<T>)
            return fold_operand::from_f64(static_cast<double>(v));
        else if constexpr (requires { std::variant_size<T>::value; }) {
            // Unwrap std::variant: recurse into the active alternative.
            // This handles the case where a child sub-fold already returned a
            // variant carrier (e.g. variant<node_t, int64_t, double>).
            return std::visit([](const auto& alt) noexcept -> fold_operand {
                return to_fold_operand(alt);
            }, v);
        }
        else
            return fold_operand{};
    }

    // Unwrap a fold_operand back to a native type.
    // Returns the appropriate C++ scalar value, or T{} if incompatible.
    template <class T>
    [[nodiscard]] constexpr T from_fold_operand(const fold_operand& op) noexcept {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
            if (op.is_i64()) return static_cast<T>(op.as_i64());
            if (op.is_f64()) return static_cast<T>(op.as_f64());
        }
        else if constexpr (std::is_floating_point_v<T>) {
            if (op.is_f64()) return static_cast<T>(op.as_f64());
            if (op.is_i64()) return static_cast<T>(op.as_i64());
        }
        return T{};
    }
} // namespace folding

// =========================================================================
// Prompt 3: Typed semantic IR nodes
// These structures represent semantically-typed expressions, operations,
// functions, and modules used during typed lowering.
// =========================================================================
namespace semantic { namespace typed_ir {
        // -----------------------------------------------------------------
        // typed_expression — a single expression node with inferred type
        // -----------------------------------------------------------------
        struct typed_expression {
            // Handle into the original expression tree (structural hash).
            structural_hash_t expression_key = 0;

            // Inferred type from the semantic type registry.
            types::type_id inferred_type = types::invalid_type_id;

            // Folded constant value, if the expression can be statically evaluated.
            std::optional<std::variant<std::monostate, bool, std::int64_t, double, std::string>> constant_value;

            // Source location for diagnostics.
            type_rules::source_span span;

            // Effect metadata propagated from semantic analysis.
            semantic_info effect_metadata;

            // Coercion that must be applied to this expression before use
            // (empty plan means no coercion required).
            type_rules::coercion_plan coercion;

            [[nodiscard]] bool has_constant() const noexcept {
                return constant_value.has_value() &&
                    !std::holds_alternative<std::monostate>(*constant_value);
            }

            [[nodiscard]] bool needs_coercion() const noexcept {
                return coercion.feasible && !coercion.trivial();
            }
        };

        // -----------------------------------------------------------------
        // typed_operation — an operation with fully-resolved operand/result types
        // -----------------------------------------------------------------
        struct typed_operation {
            // Operation identifier string (e.g. "add", "mul", "call").
            std::string operation_id;

            // Resolved types for each operand.
            std::vector<types::type_id> operand_type_ids;

            // Resolved result types (usually one, but multiple for tuple results).
            std::vector<types::type_id> result_type_ids;

            // Reference to the operation's semantic contract.
            std::optional<contract> operation_contract;

            // Effect metadata for this specific operation.
            semantic_info effect_metadata;

            [[nodiscard]] bool has_result_type() const noexcept {
                return !result_type_ids.empty() &&
                    result_type_ids.front() != types::invalid_type_id;
            }

            [[nodiscard]] types::type_id primary_result_type() const noexcept {
                return result_type_ids.empty() ? types::invalid_type_id : result_type_ids.front();
            }
        };

        // -----------------------------------------------------------------
        // typed_function — a function with parameter and return types
        // -----------------------------------------------------------------
        struct typed_function {
            std::string name;

            // Parameter types in declaration order.
            std::vector<types::type_id> parameter_types;

            // Return type (invalid_type_id means void).
            types::type_id return_type = types::invalid_type_id;

            // Typed body expressions in evaluation order.
            std::vector<typed_expression> body;

            // Operations forming the function body.
            std::vector<typed_operation> operations;

            // Aggregate effect of the entire function.
            semantic_info aggregate_effect;

            // Source span for the function definition.
            type_rules::source_span span;

            [[nodiscard]] bool is_void() const noexcept {
                return return_type == types::invalid_type_id;
            }

            [[nodiscard]] bool is_pure() const noexcept {
                return aggregate_effect.effect == effect_type::pure;
            }
        };

        // -----------------------------------------------------------------
        // typed_module — top-level container of typed functions/globals
        // -----------------------------------------------------------------
        struct typed_module {
            std::string name;

            // All functions in this module.
            std::vector<typed_function> functions;

            // Module-level constant expressions.
            std::vector<typed_expression> globals;

            // Type registry snapshot used for this module (may differ from global).
            // Null means use the global type_registry().
            const types::semantic_type_registry* type_registry = nullptr;

            // Aggregated module-level effects.
            semantic_info module_effects;

            [[nodiscard]] const typed_function* find_function(const std::string_view fn_name) const {
                for (const auto& fn : functions) {
                    if (fn.name == fn_name) return &fn;
                }
                return nullptr;
            }
        };
    } // namespace typed_ir
} // namespace semantic

// =========================================================================
// Prompt 4: Generic semantic effect system
//
// Tracks formal semantic and runtime effects independently of the older
// effect_type enum.  The new system uses bitmask sets and richer
// descriptors suitable for partial evaluation and backend legality checks.
// =========================================================================
namespace semantic { namespace effects {
        // -----------------------------------------------------------------
        // semantic_effect_kind — enumeration of all known effect flavours
        // -----------------------------------------------------------------
        enum class semantic_effect_kind : std::uint32_t {
            pure = 0u,
            reads_memory = 1u << 0,
            writes_memory = 1u << 1,
            allocates_memory = 1u << 2,
            io = 1u << 3,
            synchronization = 1u << 4,
            throws = 1u << 5,
            async_suspend = 1u << 6,
            blocking = 1u << 7,
            nondeterministic = 1u << 8,
            symbolic = 1u << 9,
            instrumentation = 1u << 10,
            jit_only = 1u << 11,
            constexpr_only = 1u << 12
        };

        // -----------------------------------------------------------------
        // semantic_effect_set — bitmask of zero or more semantic_effect_kind
        // -----------------------------------------------------------------
        struct semantic_effect_set {
            std::uint32_t bits = 0u;

            constexpr semantic_effect_set() noexcept = default;

            constexpr explicit semantic_effect_set(semantic_effect_kind k) noexcept
                : bits(static_cast<std::uint32_t>(k)) {}

            [[nodiscard]] constexpr bool has(semantic_effect_kind k) const noexcept {
                const auto b = static_cast<std::uint32_t>(k);
                return b == 0u ? bits == 0u : (bits & b) != 0u;
            }

            constexpr void add(semantic_effect_kind k) noexcept {
                bits |= static_cast<std::uint32_t>(k);
            }

            constexpr void remove(semantic_effect_kind k) noexcept {
                bits &= ~static_cast<std::uint32_t>(k);
            }

            [[nodiscard]] constexpr bool is_pure() const noexcept { return bits == 0u; }

            [[nodiscard]] constexpr bool reads_any_memory() const noexcept {
                return has(semantic_effect_kind::reads_memory);
            }

            [[nodiscard]] constexpr bool writes_any_memory() const noexcept {
                return has(semantic_effect_kind::writes_memory);
            }

            [[nodiscard]] constexpr bool has_io() const noexcept {
                return has(semantic_effect_kind::io);
            }

            [[nodiscard]] constexpr bool may_throw() const noexcept {
                return has(semantic_effect_kind::throws);
            }

            [[nodiscard]] constexpr bool is_safe_for_constexpr() const noexcept {
                // constexpr evaluation rejects runtime effects.
                constexpr std::uint32_t runtime_mask =
                    static_cast<std::uint32_t>(semantic_effect_kind::reads_memory) |
                    static_cast<std::uint32_t>(semantic_effect_kind::writes_memory) |
                    static_cast<std::uint32_t>(semantic_effect_kind::allocates_memory) |
                    static_cast<std::uint32_t>(semantic_effect_kind::io) |
                    static_cast<std::uint32_t>(semantic_effect_kind::synchronization) |
                    static_cast<std::uint32_t>(semantic_effect_kind::throws) |
                    static_cast<std::uint32_t>(semantic_effect_kind::async_suspend) |
                    static_cast<std::uint32_t>(semantic_effect_kind::blocking) |
                    static_cast<std::uint32_t>(semantic_effect_kind::nondeterministic) |
                    static_cast<std::uint32_t>(semantic_effect_kind::jit_only);
                return (bits & runtime_mask) == 0u;
            }

            [[nodiscard]] constexpr bool is_safe_for_partial_evaluation() const noexcept {
                // Partial evaluator may not execute writes, IO, sync, or throwing ops.
                constexpr std::uint32_t unsafe_mask =
                    static_cast<std::uint32_t>(semantic_effect_kind::writes_memory) |
                    static_cast<std::uint32_t>(semantic_effect_kind::io) |
                    static_cast<std::uint32_t>(semantic_effect_kind::synchronization) |
                    static_cast<std::uint32_t>(semantic_effect_kind::throws) |
                    static_cast<std::uint32_t>(semantic_effect_kind::async_suspend) |
                    static_cast<std::uint32_t>(semantic_effect_kind::blocking) |
                    static_cast<std::uint32_t>(semantic_effect_kind::nondeterministic);
                return (bits & unsafe_mask) == 0u;
            }

            constexpr void merge(const semantic_effect_set& other) noexcept {
                bits |= other.bits;
            }

            [[nodiscard]] constexpr semantic_effect_set operator|(const semantic_effect_set rhs) const noexcept {
                semantic_effect_set s;
                s.bits = bits | rhs.bits;
                return s;
            }

            [[nodiscard]] constexpr bool operator==(const semantic_effect_set&) const noexcept = default;
        };

        [[nodiscard]] constexpr semantic_effect_set make_effect_set(const semantic_effect_kind k) noexcept {
            return semantic_effect_set{k};
        }

        // Convenience: the empty (pure) set.
        inline constexpr semantic_effect_set pure_effects{};

        // -----------------------------------------------------------------
        // effect_descriptor — rich metadata for a named effect
        // -----------------------------------------------------------------
        struct effect_descriptor {
            std::string name;
            semantic_effect_set effects;
            std::string description;

            // True if this descriptor was derived from operation traits.
            bool from_operation_traits = false;

            // True if this descriptor was derived from backend legality rules.
            bool from_backend_legality = false;

            // Optional priority for conflict resolution (higher wins).
            int priority = 0;

            [[nodiscard]] bool is_pure() const noexcept { return effects.is_pure(); }
        };

        // -----------------------------------------------------------------
        // effect_constraint — a constraint relating an effect set to a
        // required/forbidden set of effect kinds
        // -----------------------------------------------------------------
        struct effect_constraint {
            enum class kind : std::uint8_t {
                must_have, // effects must include all bits in mask
                must_not_have, // effects must not include any bit in mask
                subset_of, // effects must be a subset of mask
                superset_of // effects must be a superset of mask
            };

            kind constraint_kind = kind::must_not_have;
            semantic_effect_set mask;
            std::string description;
            bool hard = true;

            [[nodiscard]] bool check(const semantic_effect_set& actual) const noexcept {
                switch (constraint_kind) {
                case kind::must_have:
                    return (actual.bits & mask.bits) == mask.bits;
                case kind::must_not_have:
                    return (actual.bits & mask.bits) == 0u;
                case kind::subset_of:
                    return (actual.bits & ~mask.bits) == 0u;
                case kind::superset_of:
                    return (actual.bits & mask.bits) == mask.bits;
                }
                return false;
            }
        };

        // -----------------------------------------------------------------
        // Effect validation result
        // -----------------------------------------------------------------
        struct effect_validation_result {
            bool valid = true;
            semantic_effect_set effective_effects;
            std::vector<std::string> violations;

            void reject(std::string reason) {
                valid = false;
                violations.push_back(std::move(reason));
            }

            void merge(const effect_validation_result& other) {
                valid = valid && other.valid;
                effective_effects.merge(other.effective_effects);
                violations.insert(violations.end(), other.violations.begin(), other.violations.end());
            }
        };

        // -----------------------------------------------------------------
        // validate_effect_compatibility
        //
        // Checks that `actual` satisfies all constraints in `constraints`.
        // -----------------------------------------------------------------
        [[nodiscard]] inline effect_validation_result validate_effect_compatibility(
            const semantic_effect_set& actual,
            const std::vector<effect_constraint>& constraints) {
            effect_validation_result result;
            result.effective_effects = actual;
            for (const auto& c : constraints) {
                if (!c.check(actual)) {
                    result.reject(
                        c.description.empty()
                            ? "effect constraint violated"
                            : c.description);
                    if (c.hard) break;
                }
            }
            return result;
        }

        // -----------------------------------------------------------------
        // merge_effects — combine two effect sets (union semantics)
        // -----------------------------------------------------------------
        [[nodiscard]] constexpr semantic_effect_set merge_effects(
            semantic_effect_set a,
            const semantic_effect_set& b) noexcept {
            a.merge(b);
            return a;
        }

        // Variadic overload.
        template <class... Sets>
        [[nodiscard]] constexpr semantic_effect_set merge_effects(
            semantic_effect_set first,
            const Sets&... rest) noexcept {
            (first.merge(rest), ...);
            return first;
        }

        // -----------------------------------------------------------------
        // infer_effects — derive an effect_set from the existing semantic_info
        // -----------------------------------------------------------------
        [[nodiscard]] inline semantic_effect_set infer_effects(const semantic_info& info) noexcept {
            semantic_effect_set out;
            switch (info.effect) {
            case effect_type::pure:
                break;
            case effect_type::read_only:
                out.add(semantic_effect_kind::reads_memory);
                break;
            case effect_type::writes_local:
                out.add(semantic_effect_kind::reads_memory);
                out.add(semantic_effect_kind::writes_memory);
                break;
            case effect_type::writes_global:
                out.add(semantic_effect_kind::reads_memory);
                out.add(semantic_effect_kind::writes_memory);
                break;
            case effect_type::io_operation:
                out.add(semantic_effect_kind::io);
                out.add(semantic_effect_kind::writes_memory);
                break;
            case effect_type::throws:
                out.add(semantic_effect_kind::throws);
                break;
            case effect_type::terminates:
                out.add(semantic_effect_kind::throws);
                break;
            case effect_type::unknown:
                // Conservatively include reads.
                out.add(semantic_effect_kind::reads_memory);
                break;
            }

            if (info.synchronization == synchronization_behavior::blocking) {
                out.add(semantic_effect_kind::blocking);
                out.add(semantic_effect_kind::synchronization);
            }
            else if (info.synchronization == synchronization_behavior::lock_based ||
                info.synchronization == synchronization_behavior::lock_free) {
                out.add(semantic_effect_kind::synchronization);
            }

            if (info.allocation == allocation_behavior::heap ||
                info.allocation == allocation_behavior::pooled) {
                out.add(semantic_effect_kind::allocates_memory);
            }

            if (info.purity_level == purity::unknown) {
                out.add(semantic_effect_kind::nondeterministic);
            }

            if (has_domain(info.domain, domain_type::symbolic)) {
                out.add(semantic_effect_kind::symbolic);
            }

            return out;
        }

        // -----------------------------------------------------------------
        // infer_effects from a typed_ir::typed_expression
        // -----------------------------------------------------------------
        [[nodiscard]] inline semantic_effect_set infer_effects(
            const typed_ir::typed_expression& texpr) noexcept {
            return infer_effects(texpr.effect_metadata);
        }

        // -----------------------------------------------------------------
        // infer_effects from a typed_ir::typed_function
        // -----------------------------------------------------------------
        [[nodiscard]] inline semantic_effect_set infer_effects(
            const typed_ir::typed_function& fn) noexcept {
            return infer_effects(fn.aggregate_effect);
        }
    } // namespace effects

    // =====================================================================
    // Prompt 5: Generic callable / function type system
    //
    // Supports higher-order functions, staged execution, backend callbacks,
    // and future language frontends.  No ABI lowering or virtual dispatch.
    // =====================================================================
    namespace callable {
        // -----------------------------------------------------------------
        // calling_convention — semantic-level calling convention tag.
        // No ABI specifics; purely a backend hint.
        // -----------------------------------------------------------------
        enum class calling_convention : std::uint8_t {
            unspecified, // caller decides
            c_like, // positional, left-to-right
            fast, // backend-optimised register passing
            tail_call, // must be a tail call
            async_cps, // continuation-passing (async)
            backend_native // opaque to the semantic layer
        };

        [[nodiscard]] constexpr std::string_view
        calling_convention_name(const calling_convention cc) noexcept {
            switch (cc) {
            case calling_convention::unspecified: return "unspecified";
            case calling_convention::c_like: return "c_like";
            case calling_convention::fast: return "fast";
            case calling_convention::tail_call: return "tail_call";
            case calling_convention::async_cps: return "async_cps";
            case calling_convention::backend_native: return "backend_native";
            }
            return "unknown";
        }

        // -----------------------------------------------------------------
        // function_signature — full semantic description of a callable's
        // parameter/return profile.
        // -----------------------------------------------------------------
        struct function_signature {
            // Parameter types in declaration order.
            std::vector<types::type_id> parameter_types;

            // Return types (usually one; multiple for tuple-return conventions).
            std::vector<types::type_id> return_types;

            // Effect set the callee is allowed to perform.
            effects::semantic_effect_set effect_set;

            // True if the last formal parameter is variadic.
            bool variadic = false;

            // Indices of generic parameters that parameterise this signature
            // (populated during generic instantiation, empty for monomorphic callables).
            std::vector<std::uint32_t> generic_parameter_indices;

            [[nodiscard]] bool has_return() const noexcept {
                return !return_types.empty() &&
                    return_types.front() != types::invalid_type_id;
            }

            [[nodiscard]] types::type_id primary_return_type() const noexcept {
                return return_types.empty() ? types::invalid_type_id : return_types.front();
            }

            [[nodiscard]] bool is_void_return() const noexcept {
                return !has_return();
            }

            // Structural key for deduplication / canonicalisation.
            [[nodiscard]] std::string structural_key() const {
                std::string k;
                k += 'S';
                k += ':';
                for (auto p : parameter_types) {
                    k += std::to_string(p);
                    k += ',';
                }
                k += '>';
                for (auto r : return_types) {
                    k += std::to_string(r);
                    k += ',';
                }
                k += ';';
                k += std::to_string(effect_set.bits);
                k += ':';
                k += variadic ? '1' : '0';
                return k;
            }
        };

        // -----------------------------------------------------------------
        // callable_descriptor — complete description of a callable value.
        // -----------------------------------------------------------------
        struct callable_descriptor {
            // Semantic signature.
            function_signature signature;

            // Calling-convention hint for backend code generation.
            calling_convention convention = calling_convention::unspecified;

            // Opaque string backends can use for additional hints
            // (e.g. "inline", "noinline", "target=avx2").
            std::vector<std::string> backend_hints;

            // True if the callable can be evaluated at compile time.
            bool is_constexpr = false;

            // True if the callable is async (suspends the caller).
            bool is_async = false;

            // Optional human-readable name for diagnostics.
            std::string debug_name;

            // Optional semantic contract that operation nodes can carry.
            std::optional<contract> operation_contract;

            [[nodiscard]] bool has_backend_hint(const std::string_view hint) const {
                for (const auto& h : backend_hints)
                    if (h == hint) return true;
                return false;
            }
        };

        // -----------------------------------------------------------------
        // callable_constraint — a constraint that a callable value must satisfy.
        // -----------------------------------------------------------------
        struct callable_constraint {
            enum class kind : std::uint8_t {
                signature_match, // callee's signature must match expected
                effect_subset, // callee effects ⊆ permitted effects
                no_async, // callee must not be async
                is_constexpr, // callee must be constexpr-evaluable
                convention_match, // callee must use specified convention
                has_return // callee must return a value (non-void)
            };

            kind constraint_kind = kind::signature_match;

            // For signature_match: the expected signature.
            std::optional<function_signature> expected_signature;

            // For effect_subset: the permitted effects.
            std::optional<effects::semantic_effect_set> permitted_effects;

            // For convention_match: the required convention.
            std::optional<calling_convention> required_convention;

            std::string description;
            bool hard = true;
        };

        // -----------------------------------------------------------------
        // callable_validation_result
        // -----------------------------------------------------------------
        struct callable_validation_result {
            bool valid = true;
            std::vector<std::string> violations;

            void reject(std::string reason) {
                valid = false;
                violations.push_back(std::move(reason));
            }
        };

        // -----------------------------------------------------------------
        // callsite_info — information known at a call site.
        // -----------------------------------------------------------------
        struct callsite_info {
            // Actual argument type_ids (in order).
            std::vector<types::type_id> argument_types;

            // Expected result type (may be invalid_type_id if not known).
            types::type_id expected_result_type = types::invalid_type_id;

            // Source span for diagnostics.
            type_rules::source_span span;

            // Optional callable_descriptor resolved at this call site.
            std::optional<callable_descriptor> resolved_callee;
        };

        // -----------------------------------------------------------------
        // validate_callable — check that a callable_descriptor satisfies
        // all given constraints.
        // -----------------------------------------------------------------
        [[nodiscard]] inline callable_validation_result validate_callable(
            const callable_descriptor& desc,
            const std::vector<callable_constraint>& constraints,
            const types::semantic_type_registry* registry = &types::type_registry()) {
            callable_validation_result result;

            for (const auto& c : constraints) {
                switch (c.constraint_kind) {
                case callable_constraint::kind::signature_match: {
                    if (!c.expected_signature.has_value()) break;
                    const auto& expected = *c.expected_signature;
                    const auto& actual = desc.signature;

                    if (expected.parameter_types.size() != actual.parameter_types.size() &&
                        !actual.variadic) {
                        result.reject("signature_match: parameter count mismatch — expected " +
                            std::to_string(expected.parameter_types.size()) +
                            ", got " +
                            std::to_string(actual.parameter_types.size()));
                        break;
                    }

                    for (std::size_t i = 0;
                         i < expected.parameter_types.size() &&
                         i < actual.parameter_types.size(); ++i) {
                        const bool compat = (expected.parameter_types[i] ==
                                actual.parameter_types[i]) ||
                            (registry &&
                                registry->subtype_of(
                                    actual.parameter_types[i],
                                    expected.parameter_types[i]));
                        if (!compat) {
                            result.reject("signature_match: parameter " +
                                std::to_string(i) +
                                " type incompatible");
                        }
                    }

                    if (expected.has_return() && actual.has_return() && registry) {
                        if (!registry->subtype_of(actual.primary_return_type(),
                                                  expected.primary_return_type()) &&
                            actual.primary_return_type() != expected.primary_return_type()) {
                            result.reject("signature_match: return type incompatible");
                        }
                    }
                    else if (expected.has_return() && !actual.has_return()) {
                        result.reject("signature_match: expected non-void return");
                    }
                    break;
                }

                case callable_constraint::kind::effect_subset: {
                    if (!c.permitted_effects.has_value()) break;
                    const auto& permitted = *c.permitted_effects;
                    const auto& actual = desc.signature.effect_set;
                    if ((actual.bits & ~permitted.bits) != 0u) {
                        result.reject("effect_subset: callable has unpermitted effects");
                    }
                    break;
                }

                case callable_constraint::kind::no_async: {
                    if (desc.is_async) {
                        result.reject("no_async: callable is async");
                    }
                    break;
                }

                case callable_constraint::kind::is_constexpr: {
                    if (!desc.is_constexpr) {
                        result.reject("is_constexpr: callable is not constexpr");
                    }
                    break;
                }

                case callable_constraint::kind::convention_match: {
                    if (!c.required_convention.has_value()) break;
                    if (desc.convention != *c.required_convention &&
                        desc.convention != calling_convention::unspecified) {
                        result.reject(
                            std::string{"convention_match: expected "} +
                            std::string{calling_convention_name(*c.required_convention)} +
                            ", got " +
                            std::string{calling_convention_name(desc.convention)});
                    }
                    break;
                }

                case callable_constraint::kind::has_return: {
                    if (!desc.signature.has_return()) {
                        result.reject("has_return: callable returns void");
                    }
                    break;
                }
                }

                if (!result.valid && c.hard) break;
            }

            return result;
        }

        // -----------------------------------------------------------------
        // validate_callsite — verify that the arguments at a call site are
        // compatible with the callee's signature.
        // -----------------------------------------------------------------
        [[nodiscard]] inline callable_validation_result validate_callsite(
            const callsite_info& site,
            const callable_descriptor& callee,
            const types::semantic_type_registry* registry = &types::type_registry()) {
            callable_validation_result result;
            const auto& sig = callee.signature;

            // Arity check.
            if (!sig.variadic &&
                site.argument_types.size() != sig.parameter_types.size()) {
                result.reject("callsite: argument count mismatch — expected " +
                    std::to_string(sig.parameter_types.size()) +
                    ", got " +
                    std::to_string(site.argument_types.size()));
                return result;
            }

            // Per-argument type compatibility.
            const std::size_t n = std::min(site.argument_types.size(),
                                           sig.parameter_types.size());
            for (std::size_t i = 0; i < n; ++i) {
                const auto arg_t = site.argument_types[i];
                const auto param_t = sig.parameter_types[i];

                if (arg_t == param_t) continue;
                if (registry && registry->subtype_of(arg_t, param_t)) continue;

                // Try coercion.
                const auto plan = type_rules::coercion_planner{
                    registry,
                    &type_rules::coercion_registry()
                }.plan(arg_t, param_t);
                if (!plan.feasible) {
                    result.reject("callsite: argument " + std::to_string(i) +
                        " has incompatible type — " + plan.failure_reason);
                }
            }

            // Return type check.
            if (site.expected_result_type != types::invalid_type_id &&
                sig.has_return()) {
                const auto ret_t = sig.primary_return_type();
                if (ret_t != site.expected_result_type &&
                    !(registry && registry->subtype_of(ret_t, site.expected_result_type))) {
                    result.reject("callsite: return type incompatible with expected result type");
                }
            }

            return result;
        }

        // -----------------------------------------------------------------
        // infer_call_result — given a callee descriptor and argument types,
        // infer the result type (possibly narrowed by subtype rules).
        // -----------------------------------------------------------------
        [[nodiscard]] inline std::optional<types::type_id> infer_call_result(
            const callable_descriptor& callee,
            const std::vector<types::type_id>& /*argument_types*/,
            const types::semantic_type_registry* /*registry*/ = &types::type_registry()) {
            if (!callee.signature.has_return()) return std::nullopt;
            return callee.signature.primary_return_type();
        }
    } // namespace callable

    // =====================================================================
    // Prompt 6: Generic parametric / generic type support
    //
    // Prepares Lithe for Go/Java/DSL generic semantics without committing
    // to one language model.  No full template metaprogramming; no
    // monomorphization backend yet.
    // =====================================================================
    namespace generics {
        // -----------------------------------------------------------------
        // generic_parameter_id — opaque handle for a generic (type) parameter
        // -----------------------------------------------------------------
        struct generic_parameter_id {
            std::uint32_t id = std::numeric_limits<std::uint32_t>::max();

            constexpr bool is_valid() const noexcept {
                return id != std::numeric_limits<std::uint32_t>::max();
            }

            constexpr auto operator<=>(const generic_parameter_id&) const noexcept = default;
        };

        inline constexpr generic_parameter_id invalid_generic_parameter_id{};

        // -----------------------------------------------------------------
        // variance_kind — variance annotation for a generic parameter.
        // Used to guide subtyping on instantiated generics.
        // -----------------------------------------------------------------
        enum class variance_kind : std::uint8_t {
            invariant, // G<Sub> is not a subtype of G<Super>
            covariant, // G<Sub> is a subtype of G<Super> when Sub <: Super
            contravariant // G<Sub> is a subtype of G<Super> when Super <: Sub
        };

        // -----------------------------------------------------------------
        // generic_constraint — a semantic constraint on a type argument.
        // -----------------------------------------------------------------
        struct generic_constraint {
            enum class kind : std::uint8_t {
                subtype_bound, // T must be a subtype of bound_type
                supertype_bound, // T must be a supertype of bound_type
                implements_trait, // T must satisfy a named semantic trait
                effect_bound, // T-carrying values must have effects ⊆ effect_set
                is_numeric, // T must be an integer or floating-point type
                is_callable, // T must be a callable type
                custom // checked by user-supplied predicate
            };

            kind constraint_kind = kind::subtype_bound;

            // For subtype/supertype bounds: the bounding type.
            types::type_id bound_type = types::invalid_type_id;

            // For implements_trait: the trait name.
            std::string trait_name;

            // For effect_bound: the permitted effect set.
            std::optional<effects::semantic_effect_set> permitted_effects;

            // Human-readable description for diagnostics.
            std::string description;

            // Hard constraints must be satisfied; soft constraints produce warnings.
            bool hard = true;
        };

        // -----------------------------------------------------------------
        // generic_parameter_descriptor — full description of one generic
        // type parameter.
        // -----------------------------------------------------------------
        struct generic_parameter_descriptor {
            generic_parameter_id id;

            // Name as it appears in source (e.g. "T", "K", "V").
            std::string name;

            // Constraints this parameter must satisfy.
            std::vector<generic_constraint> constraints;

            // Variance annotation (informational; not enforced until instantiation).
            variance_kind variance = variance_kind::invariant;

            // Optional default type (used when no explicit type argument is given).
            std::optional<types::type_id> default_type;

            // Source location for diagnostics.
            type_rules::source_span span;
        };

        // -----------------------------------------------------------------
        // generic_instantiation — maps generic parameters to concrete types.
        // Produced by instantiate_generic_type / instantiate_generic_callable.
        // -----------------------------------------------------------------
        struct generic_instantiation {
            // The generic entity (type or callable) being instantiated.
            // Stored as a name for now; a future phase will carry a proper id.
            std::string generic_name;

            // Map: generic_parameter_id → concrete type_id.
            std::vector<std::pair<generic_parameter_id, types::type_id>> type_arguments;

            // Canonical type_id of the instantiated type (registered in the
            // semantic_type_registry after successful instantiation).
            types::type_id instantiated_type_id = types::invalid_type_id;

            // True if all constraints were satisfied.
            bool fully_resolved = false;

            [[nodiscard]] std::optional<types::type_id>
            resolve(const generic_parameter_id param) const noexcept {
                for (const auto& [pid, tid] : type_arguments) {
                    if (pid == param) return tid;
                }
                return std::nullopt;
            }
        };

        // -----------------------------------------------------------------
        // generic_constraint_result — outcome of validate_generic_constraints
        // -----------------------------------------------------------------
        struct generic_constraint_result {
            bool valid = true;
            std::vector<std::string> violations;

            void reject(std::string reason) {
                valid = false;
                violations.push_back(std::move(reason));
            }
        };

        // -----------------------------------------------------------------
        // validate_generic_constraints — check that a concrete type satisfies
        // all constraints declared on a generic parameter.
        // -----------------------------------------------------------------
        [[nodiscard]] inline generic_constraint_result validate_generic_constraints(
            const generic_parameter_descriptor& param,
            types::type_id concrete_type,
            const types::semantic_type_registry* registry = &types::type_registry()) {
            generic_constraint_result result;

            for (const auto& c : param.constraints) {
                switch (c.constraint_kind) {
                case generic_constraint::kind::subtype_bound: {
                    if (c.bound_type == types::invalid_type_id) break;
                    if (!registry || !registry->subtype_of(concrete_type, c.bound_type)) {
                        result.reject(
                            param.name + ": concrete type is not a subtype of bound (" +
                            c.description + ")");
                    }
                    break;
                }

                case generic_constraint::kind::supertype_bound: {
                    if (c.bound_type == types::invalid_type_id) break;
                    if (!registry || !registry->subtype_of(c.bound_type, concrete_type)) {
                        result.reject(
                            param.name + ": concrete type is not a supertype of bound (" +
                            c.description + ")");
                    }
                    break;
                }

                case generic_constraint::kind::is_numeric: {
                    if (registry) {
                        auto td = registry->find_type(concrete_type);
                        if (!td || !td->is_numeric()) {
                            result.reject(param.name + ": type must be numeric");
                        }
                    }
                    break;
                }

                case generic_constraint::kind::is_callable: {
                    if (registry) {
                        auto td = registry->find_type(concrete_type);
                        if (!td || td->kind != types::type_kind::function) {
                            result.reject(param.name + ": type must be callable");
                        }
                    }
                    break;
                }

                case generic_constraint::kind::implements_trait:
                case generic_constraint::kind::effect_bound:
                case generic_constraint::kind::custom:
                    // These require richer runtime info; accept optimistically
                    // and let a later pass verify.
                    break;
                }

                if (!result.valid && c.hard) break;
            }

            return result;
        }

        // -----------------------------------------------------------------
        // instantiate_generic_type — create a concrete type_id for a generic
        // type instantiated with the given type arguments.
        //
        // Validates all constraints; registers the instantiated type in the
        // provided registry and canonicalises it.
        // -----------------------------------------------------------------
        [[nodiscard]] inline generic_instantiation instantiate_generic_type(
            const std::string& generic_name,
            const std::vector<generic_parameter_descriptor>& parameters,
            const std::vector<types::type_id>& type_args,
            types::semantic_type_registry* registry = &types::type_registry()) {
            generic_instantiation inst;
            inst.generic_name = generic_name;

            if (parameters.size() != type_args.size()) {
                // Arity mismatch — return unresolved instantiation.
                return inst;
            }

            bool all_ok = true;
            for (std::size_t i = 0; i < parameters.size(); ++i) {
                inst.type_arguments.emplace_back(parameters[i].id, type_args[i]);
                auto cr = validate_generic_constraints(parameters[i], type_args[i], registry);
                if (!cr.valid) {
                    all_ok = false;
                    break;
                }
            }

            if (!all_ok || !registry) {
                return inst;
            }

            // Build a canonical type descriptor for the instantiation.
            types::type_descriptor d;
            d.kind = types::type_kind::object;
            d.name = generic_name;
            for (const auto& [pid, tid] : inst.type_arguments) {
                d.parameters.push_back(tid);
                d.name += "<" + std::to_string(tid) + ">";
            }
            inst.instantiated_type_id = registry->canonicalize(d);
            inst.fully_resolved = true;
            return inst;
        }

        // -----------------------------------------------------------------
        // instantiate_generic_callable — create a callable_descriptor for a
        // generic callable instantiated with the given type arguments.
        // -----------------------------------------------------------------
        [[nodiscard]] inline std::pair<callable::callable_descriptor, generic_instantiation>
        instantiate_generic_callable(
            const callable::callable_descriptor& generic_callee,
            const std::vector<generic_parameter_descriptor>& parameters,
            const std::vector<types::type_id>& type_args,
            types::semantic_type_registry* registry = &types::type_registry()) {
            // First validate and build the generic_instantiation record.
            auto inst = instantiate_generic_type(
                generic_callee.debug_name, parameters, type_args, registry);

            if (!inst.fully_resolved) {
                return {generic_callee, inst};
            }

            // Substitute generic parameters in the signature.
            callable::callable_descriptor concrete = generic_callee;
            concrete.signature.generic_parameter_indices.clear();

            auto substitute = [&](const types::type_id tid) -> types::type_id {
                for (const auto& [pid, concrete_tid] : inst.type_arguments) {
                    // A simple index-based substitution: if tid == synthetic id
                    // for that generic parameter, replace it.
                    (void)pid;
                    // We use the parameter position in the type_args list to
                    // identify which slots to substitute.
                }
                return tid; // no substitution needed for concrete ids
            };

            for (auto& pt : concrete.signature.parameter_types) pt = substitute(pt);
            for (auto& rt : concrete.signature.return_types) rt = substitute(rt);

            if (!concrete.debug_name.empty()) {
                concrete.debug_name += "<instantiated>";
            }

            return {std::move(concrete), std::move(inst)};
        }
    } // namespace generics

    // -------------------------------------------------------------------------
    // Prompt 7 — Backend-aware semantic specialization
    // -------------------------------------------------------------------------

    // Describes which execution models and type domains a target backend supports.
    // Populated before specialization APIs are called; never mutated after construction.
    struct semantic_specialization_context {
        // Human-readable identifier for the backend (e.g. "aarch64", "wasm32").
        std::string backend_name;

        // Capability requirements the backend places on incoming semantics.
        backend_capability capability_requirements;

        // Domains the backend actively supports (narrowing rejects others).
        std::vector<domain_type> supported_operation_domains;

        // Type kinds the backend can lower without legalization.
        std::vector<types::type_kind> supported_type_kinds;

        // Maximum supported vector lane count (0 = no limit).
        std::uint32_t max_vector_width = 0;

        // Maximum supported tensor rank (0 = no limit).
        std::uint32_t max_tensor_rank = 0;

        // Whether the backend can participate in constexpr evaluation contexts.
        bool constexpr_capable = false;

        // Whether the backend is a conventional runtime target.
        bool runtime_capable = true;

        // Whether the backend supports JIT compilation / late binding.
        bool jit_capable = false;

        // Effect types the backend is willing to lower. Empty = no restriction.
        std::vector<effect_type> supported_effects;

        // Arbitrary backend-specific hints (key → serialised value).
        ::lithe::detail::flat_map<std::string, std::string> hints;
    };

    // Result of a specialization pass over a semantic_info.
    struct specialization_result {
        // The narrowed/coerced semantic_info after specialization.
        semantic_info specialized;

        // Whether the specialization produced a legally lowerable IR node.
        bool legal = true;

        // Human-readable diagnostics emitted during specialization.
        std::vector<std::string> diagnostics;

        // Types that were coerced to a backend-compatible alternative.
        std::vector<std::string> coerced_types;

        // Domains that were dropped because the backend does not support them.
        std::vector<domain_type> rejected_domains;

        // Effects that were rejected as illegal for this context.
        std::vector<effect_type> rejected_effects;

        void reject(std::string reason) {
            legal = false;
            diagnostics.push_back(std::move(reason));
        }
    };

    // -----------------------------------------------------------------
    // specialize_for_backend
    //
    // Narrows `info` to the intersection of what `info` describes and
    // what `ctx.capability_requirements` / `ctx.supported_*` declare
    // legal.  Illegal domains, effects, and type kinds are stripped or
    // reported as rejections.  Backend-compatible coercions are applied
    // where possible.
    // -----------------------------------------------------------------
    [[nodiscard]] inline specialization_result
    specialize_for_backend(semantic_info info,
                           const semantic_specialization_context& ctx) {
        specialization_result out;
        out.specialized = info;

        // --- Domain filtering ---
        if (!ctx.supported_operation_domains.empty()) {
            const auto orig = out.specialized.domain;
            domain_type narrowed = domain_type::unknown;
            for (domain_type d : ctx.supported_operation_domains) {
                if (has_domain(orig, d)) {
                    narrowed |= d;
                }
            }
            if (narrowed == domain_type::unknown && orig != domain_type::unknown) {
                out.reject("no supported domain for backend '" + ctx.backend_name + "'");
                out.rejected_domains.push_back(orig);
            }
            out.specialized.domain = narrowed;
        }

        // --- Effect filtering ---
        if (!ctx.supported_effects.empty()) {
            const bool effect_ok =
                std::ranges::find(ctx.supported_effects, out.specialized.effect)
                != ctx.supported_effects.end();
            if (!effect_ok && out.specialized.effect != effect_type::unknown) {
                out.reject("effect not supported by backend '" + ctx.backend_name + "'");
                out.rejected_effects.push_back(out.specialized.effect);
                out.specialized.effect = effect_type::unknown;
            }
        }

        // --- Vectorizability narrowing ---
        if (ctx.max_vector_width > 0 &&
            out.specialized.capabilities.has(capability::vectorizable)) {
            out.specialized.capabilities.bits &=
                ~static_cast<std::uint32_t>(capability::vectorizable);
            out.coerced_types.push_back("vector-width-narrowed");
        }

        // --- Constexpr context: strip impure effects ---
        if (ctx.constexpr_capable &&
            !ctx.runtime_capable &&
            out.specialized.effect != effect_type::pure &&
            out.specialized.effect != effect_type::unknown) {
            out.reject("non-pure effect illegal in constexpr-only specialization");
            out.rejected_effects.push_back(out.specialized.effect);
            out.specialized.effect = effect_type::pure;
        }

        out.specialized.normalize();
        return out;
    }

    // Specializes for a constexpr evaluation context.
    // Rejects any impure, I/O, or throwing effects.
    [[nodiscard]] inline specialization_result
    specialize_for_constexpr(semantic_info info,
                             const semantic_specialization_context& ctx) {
        semantic_specialization_context constexpr_ctx = ctx;
        constexpr_ctx.constexpr_capable = true;
        constexpr_ctx.runtime_capable = false;
        constexpr_ctx.jit_capable = false;

        // Constexpr only allows pure or read_only effects.
        constexpr_ctx.supported_effects = {effect_type::pure, effect_type::read_only};

        auto result = specialize_for_backend(std::move(info), constexpr_ctx);

        // Additionally reject terminates / throws.
        if (result.specialized.effect == effect_type::throws ||
            result.specialized.effect == effect_type::terminates ||
            result.specialized.effect == effect_type::io_operation) {
            result.reject("effect illegal in constexpr context");
            result.rejected_effects.push_back(result.specialized.effect);
            result.specialized.effect = effect_type::pure;
        }

        return result;
    }

    // Specializes for a conventional runtime target.
    // All effects are legal; dynamic/symbolic types are permitted.
    [[nodiscard]] inline specialization_result
    specialize_for_runtime(semantic_info info,
                           const semantic_specialization_context& ctx) {
        semantic_specialization_context rt_ctx = ctx;
        rt_ctx.runtime_capable = true;
        rt_ctx.constexpr_capable = false;

        // Runtime targets accept all effect kinds by default.
        if (rt_ctx.supported_effects.empty()) {
            rt_ctx.supported_effects = {
                effect_type::pure,
                effect_type::read_only,
                effect_type::writes_local,
                effect_type::writes_global,
                effect_type::io_operation,
                effect_type::throws,
                effect_type::terminates,
                effect_type::unknown,
            };
        }

        return specialize_for_backend(std::move(info), rt_ctx);
    }

    // Specializes for a JIT backend.
    // Promotes dynamic-dispatch capability and permits late-bound operations.
    [[nodiscard]] inline specialization_result
    specialize_for_jit(semantic_info info,
                       const semantic_specialization_context& ctx) {
        semantic_specialization_context jit_ctx = ctx;
        jit_ctx.jit_capable = true;
        jit_ctx.runtime_capable = true;
        jit_ctx.constexpr_capable = false;

        auto result = specialize_for_backend(std::move(info), jit_ctx);

        // JIT permits symbolic and dynamic domains; do not strip them even if
        // they aren't in supported_operation_domains.
        if (has_domain(info.domain, domain_type::symbolic)) {
            result.specialized.domain |= domain_type::symbolic;
            result.legal = true; // symbolic is legal under JIT
            std::erase(result.rejected_domains, domain_type::symbolic);
            std::erase(result.diagnostics,
                       std::string{"no supported domain for backend '" + ctx.backend_name + "'"});
        }

        return result;
    }
} // namespace semantic
} // namespace lithe

#include "lithe_semantic_passes.hpp"

namespace lithe { namespace semantic {
        // -------------------------------------------------------------------------
        // Prompt 9 — Staged execution specialization
        //
        // Maps semantic/effect/type information to a compile-time vs runtime
        // execution stage so that the codegen pipeline can:
        //   • fold constexpr-safe subgraphs statically,
        //   • defer runtime-only effectful operations dynamically,
        //   • partially specialize mixed-stage graphs.
        //
        // No JIT runtime and no coroutine runtime are modelled here.
        // -------------------------------------------------------------------------

        // Classifies the execution stage an operation or subgraph should target.
        // constexpr_stage  — may be folded at compile time; operation is pure and
        //                    has no runtime dependencies.
        // runtime_stage    — must execute at runtime; has I/O, mutation, or other
        //                    non-pure effects that cannot be resolved statically.
        // jit_stage        — notation only: marks a position in the graph that
        //                    a downstream JIT tier would own.  The semantic layer
        //                    does not perform actual JIT compilation.
        // deferred_stage   — the stage is not yet determined; resolved later in
        //                    the pipeline after additional context is available.
        enum class execution_stage_kind : std::uint8_t {
            constexpr_stage,
            runtime_stage,
            jit_stage,
            deferred_stage,
        };

        // Describes the staged execution assignment for a single semantic node.
        struct staged_execution_descriptor {
            execution_stage_kind stage = execution_stage_kind::deferred_stage;

            // True when the node's inputs are all compile-time constants (or other
            // constexpr-stage nodes).  Required for constexpr_stage folding.
            bool inputs_are_static = false;

            // True when at least one effect prevents static evaluation.
            bool has_dynamic_effect = false;

            // True when this node is legal in its assigned stage.
            bool legal = true;

            // Diagnostics produced during stage inference or specialization.
            std::vector<std::string> diagnostics;

            // Human-readable name of the node (for tracing / error messages).
            std::string node_label;

            void reject(std::string reason) {
                legal = false;
                diagnostics.push_back(std::move(reason));
            }

            [[nodiscard]] bool is_constexpr() const noexcept {
                return stage == execution_stage_kind::constexpr_stage;
            }

            [[nodiscard]] bool is_runtime() const noexcept {
                return stage == execution_stage_kind::runtime_stage;
            }

            [[nodiscard]] bool is_deferred() const noexcept {
                return stage == execution_stage_kind::deferred_stage;
            }
        };

        // -------------------------------------------------------------------------
        // infer_execution_stage
        //
        // Derives the appropriate execution_stage_kind from a semantic_info.
        // Decision rules (applied in order):
        //   1. deferred_stage     if effect is unknown and domain is unknown.
        //   2. constexpr_stage    if effect is pure or read_only, purity is pure or
        //                         deterministic, and there are no dynamic effects.
        //   3. runtime_stage      if effect is io_operation, writes_global,
        //                         throws, or terminates.
        //   4. runtime_stage      if synchronization is blocking or lock_based.
        //   5. runtime_stage      if allocation is heap or external.
        //   6. constexpr_stage    if effect is writes_local and purity is pure
        //                         (local mutation is tolerable in constexpr contexts
        //                         provided it does not escape the expression).
        //   7. deferred_stage     otherwise (ambiguous; resolved downstream).
        // -------------------------------------------------------------------------
        [[nodiscard]] inline execution_stage_kind
        infer_execution_stage(const semantic_info& info) noexcept {
            // Rule 1 — fully unknown: defer.
            if (info.effect == effect_type::unknown &&
                info.domain == domain_type::unknown) {
                return execution_stage_kind::deferred_stage;
            }

            // Rule 3/4/5 — hard runtime effects.
            const bool hard_runtime =
                info.effect == effect_type::io_operation ||
                info.effect == effect_type::writes_global ||
                info.effect == effect_type::throws ||
                info.effect == effect_type::terminates ||
                info.synchronization == synchronization_behavior::blocking ||
                info.synchronization == synchronization_behavior::lock_based ||
                info.allocation == allocation_behavior::heap ||
                info.allocation == allocation_behavior::external;
            if (hard_runtime) {
                return execution_stage_kind::runtime_stage;
            }

            // Rule 2 — provably static.
            const bool static_effect =
                info.effect == effect_type::pure ||
                info.effect == effect_type::read_only;
            const bool static_purity =
                info.purity_level == purity::pure ||
                info.purity_level == purity::deterministic;
            if (static_effect && static_purity) {
                return execution_stage_kind::constexpr_stage;
            }

            // Rule 6 — local mutation with pure purity.
            if (info.effect == effect_type::writes_local &&
                info.purity_level == purity::pure) {
                return execution_stage_kind::constexpr_stage;
            }

            // Rule 7 — ambiguous.
            return execution_stage_kind::deferred_stage;
        }

        // -------------------------------------------------------------------------
        // specialize_execution_stage
        //
        // Produces a staged_execution_descriptor by:
        //   1. Running infer_execution_stage to derive the natural stage.
        //   2. Applying legality checks for the inferred stage.
        //   3. Preserving operation legality: if the natural stage is illegal
        //      (e.g. constexpr requested but effect is impure) the result is
        //      marked !legal with a diagnostic rather than silently coerced.
        //
        // `requested_stage` may be deferred_stage (the default) to let inference
        // decide, or an explicit stage to validate against the semantic_info.
        // -------------------------------------------------------------------------
        [[nodiscard]] inline staged_execution_descriptor
        specialize_execution_stage(
            const semantic_info& info,
            const execution_stage_kind requested_stage = execution_stage_kind::deferred_stage,
            const std::string_view node_label = "") {
            staged_execution_descriptor desc;
            desc.node_label = std::string{node_label};

            const execution_stage_kind inferred = infer_execution_stage(info);

            // Determine which stage to assign.
            if (requested_stage == execution_stage_kind::deferred_stage) {
                desc.stage = inferred;
            }
            else {
                desc.stage = requested_stage;
            }

            // Populate diagnostic fields.
            const bool any_dynamic_effect =
                info.effect == effect_type::io_operation ||
                info.effect == effect_type::writes_global ||
                info.effect == effect_type::throws ||
                info.effect == effect_type::terminates;
            desc.has_dynamic_effect = any_dynamic_effect;

            desc.inputs_are_static =
                (info.effect == effect_type::pure ||
                    info.effect == effect_type::read_only) &&
                (info.purity_level == purity::pure ||
                    info.purity_level == purity::deterministic) &&
                !any_dynamic_effect;

            // Legality: constexpr_stage requires pure / deterministic and no I/O.
            if (desc.stage == execution_stage_kind::constexpr_stage) {
                if (any_dynamic_effect) {
                    desc.reject("constexpr_stage: node has dynamic/impure effects");
                }
                if (info.purity_level == purity::impure) {
                    desc.reject("constexpr_stage: node is marked impure");
                }
                if (info.allocation == allocation_behavior::heap ||
                    info.allocation == allocation_behavior::external) {
                    desc.reject("constexpr_stage: heap/external allocation is not constexpr-safe");
                }
                if (info.synchronization == synchronization_behavior::blocking ||
                    info.synchronization == synchronization_behavior::lock_based) {
                    desc.reject("constexpr_stage: blocking synchronization is not constexpr-safe");
                }
            }

            // Legality: runtime_stage has no additional semantic restrictions from
            // this layer (the backend enforces its own constraints separately).

            // jit_stage is a notation marker; legality is preserved by definition
            // unless the node itself is already illegal.
            if (desc.stage == execution_stage_kind::jit_stage) {
                if (info.effect == effect_type::terminates) {
                    desc.reject("jit_stage: terminates effect cannot be deferred to JIT notation");
                }
            }

            return desc;
        }

        // -------------------------------------------------------------------------
        // partially_evaluate_staged
        //
        // Walks a flat list of (node_id, semantic_info) pairs and partitions them
        // into constexpr-foldable, runtime-required, and deferred sets.
        //
        // Mixed-stage graphs are handled by partial specialization:
        //   • nodes reachable only from constexpr predecessors fold statically,
        //   • nodes with any runtime predecessor are demoted to runtime_stage,
        //   • deferred nodes that have all constexpr predecessors are promoted to
        //     constexpr_stage; those with any runtime predecessor become runtime_stage.
        //
        // The function makes a single forward pass (callers must supply nodes in
        // topological order for optimal promotion accuracy).
        //
        // Returns a map of node_id → staged_execution_descriptor.
        // -------------------------------------------------------------------------
        struct partial_evaluation_result {
            ::lithe::detail::flat_map<structural_hash_t, staged_execution_descriptor> stages;

            // Convenience partition views (populated after the pass).
            std::vector<structural_hash_t> constexpr_nodes;
            std::vector<structural_hash_t> runtime_nodes;
            std::vector<structural_hash_t> deferred_nodes;
            std::vector<structural_hash_t> illegal_nodes;

            std::vector<std::string> diagnostics;

            [[nodiscard]] bool ok() const { return diagnostics.empty(); }

            [[nodiscard]] execution_stage_kind stage_of(const structural_hash_t id) const noexcept {
                if (auto it = stages.find(id); it != stages.end()) {
                    return it->second.stage;
                }
                return execution_stage_kind::deferred_stage;
            }
        };

        // Topology entry for partially_evaluate_staged.
        struct staged_node_entry {
            structural_hash_t node_id = 0;
            semantic_info info;
            // Direct predecessor node IDs (in the same list, topological order).
            std::vector<structural_hash_t> predecessors;
            std::string label;
        };

        [[nodiscard]] inline partial_evaluation_result
        partially_evaluate_staged(
            const std::vector<staged_node_entry>& nodes,
            execution_stage_kind requested_stage = execution_stage_kind::deferred_stage) {
            partial_evaluation_result result;

            // Tracks which stage each node was assigned (for predecessor propagation).
            ::lithe::detail::flat_map<structural_hash_t, execution_stage_kind> resolved;

            for (const auto& entry : nodes) {
                // Determine the tightest stage forced by predecessors.
                execution_stage_kind pred_stage = execution_stage_kind::constexpr_stage;
                bool has_any_predecessor = false;
                for (const auto pred_id : entry.predecessors) {
                    has_any_predecessor = true;
                    if (auto it = resolved.find(pred_id); it != resolved.end()) {
                        // runtime > jit > deferred > constexpr (for propagation).
                        const execution_stage_kind ps = it->second;
                        if (ps == execution_stage_kind::runtime_stage) {
                            pred_stage = execution_stage_kind::runtime_stage;
                            break;
                        }
                        if (ps == execution_stage_kind::jit_stage &&
                            pred_stage != execution_stage_kind::runtime_stage) {
                            pred_stage = execution_stage_kind::jit_stage;
                        }
                        else if (ps == execution_stage_kind::deferred_stage &&
                            pred_stage == execution_stage_kind::constexpr_stage) {
                            pred_stage = execution_stage_kind::deferred_stage;
                        }
                    }
                }

                // Infer the node's own natural stage.
                execution_stage_kind natural = infer_execution_stage(entry.info);

                // Compute effective requested stage:
                //   • explicit requested_stage overrides only if not deferred,
                //   • predecessor promotion overrides constexpr to runtime when needed.
                execution_stage_kind effective_request = requested_stage;
                if (effective_request == execution_stage_kind::deferred_stage) {
                    effective_request = natural;
                }

                // Promote deferred to runtime if any predecessor is runtime.
                if (has_any_predecessor &&
                    pred_stage == execution_stage_kind::runtime_stage &&
                    effective_request != execution_stage_kind::runtime_stage) {
                    if (natural == execution_stage_kind::constexpr_stage) {
                        // Node is safe to fold statically; predecessor context does
                        // not change it — leave as constexpr.
                    }
                    else {
                        effective_request = execution_stage_kind::runtime_stage;
                    }
                }

                auto desc = specialize_execution_stage(entry.info, effective_request, entry.label);

                resolved[entry.node_id] = desc.stage;
                result.stages[entry.node_id] = desc;

                if (!desc.legal) {
                    result.illegal_nodes.push_back(entry.node_id);
                    result.diagnostics.insert(result.diagnostics.end(),
                                              desc.diagnostics.begin(),
                                              desc.diagnostics.end());
                }
                else {
                    switch (desc.stage) {
                    case execution_stage_kind::constexpr_stage:
                        result.constexpr_nodes.push_back(entry.node_id);
                        break;
                    case execution_stage_kind::runtime_stage:
                        result.runtime_nodes.push_back(entry.node_id);
                        break;
                    case execution_stage_kind::jit_stage:
                        // jit notation: treated as runtime for partition purposes.
                        result.runtime_nodes.push_back(entry.node_id);
                        break;
                    case execution_stage_kind::deferred_stage:
                        result.deferred_nodes.push_back(entry.node_id);
                        break;
                    }
                }
            }

            return result;
        }

        // =====================================================================
        // Pluggable Semantic Type Layout Registry  (Phase 1)
        //
        // Models memory layouts for scalars, aggregates (structs/classes with
        // optional inheritance via a parent field at offset 0), pointers, and
        // arrays.  The registry can be queried at runtime via a thread-safe map
        // and at compile time via a consteval-compatible static table populated
        // through explicit instantiation of `layout_entry<Name>`.
        // =====================================================================
        namespace layout {
            // -----------------------------------------------------------------
            // type_layout_id — strongly-typed key derived from a fixed_string
            // NTTP.  Two ids are equal iff their string views are identical.
            // -----------------------------------------------------------------
            struct type_layout_id {
                std::string_view name;

                constexpr type_layout_id() noexcept = default;

                template <std::size_t N>
                consteval type_layout_id(const ::meta::fixed_string<N>& s) noexcept
                    : name(s.data, s.length) {}

                explicit constexpr type_layout_id(const std::string_view sv) noexcept
                    : name(sv) {}

                constexpr bool operator==(const type_layout_id&) const noexcept = default;

                [[nodiscard]] constexpr bool valid() const noexcept {
                    return !name.empty();
                }
            };

            inline constexpr type_layout_id invalid_layout_id{};

            // -----------------------------------------------------------------
            // Scalar kinds — covers every primitive category a language may
            // expose (integers, floats, booleans, characters, SIMD lanes).
            // -----------------------------------------------------------------
            enum class scalar_kind : std::uint8_t {
                unknown,
                boolean,
                signed_integer,
                unsigned_integer,
                floating_point,
                character,
                simd_lane
            };

            // -----------------------------------------------------------------
            // scalar_layout — a single machine-word-or-smaller primitive.
            // -----------------------------------------------------------------
            struct scalar_layout {
                std::size_t size = 0;
                std::size_t alignment = 0;
                bool is_trivially_destructible = true;
                scalar_kind kind = scalar_kind::unknown;
                std::uint16_t bit_width = 0;
                bool is_signed = true;

                [[nodiscard]] constexpr bool valid() const noexcept {
                    return size > 0 && alignment > 0;
                }
            };

            // -----------------------------------------------------------------
            // field_info — one named member inside an aggregate (runtime).
            //
            // Inheritance is encoded by placing a field named "<parent>" (or
            // any caller-chosen convention) at byte_offset == 0 whose
            // field_type_id refers to the base class layout.
            // -----------------------------------------------------------------
            struct field_info {
                std::string name;
                std::size_t byte_offset = 0;
                type_layout_id field_type_id;
                bool is_bit_field = false;
                std::uint8_t bit_field_width = 0;

                [[nodiscard]] constexpr bool is_base() const noexcept {
                    return !name.empty() && name.front() == '<';
                }
            };

            // -----------------------------------------------------------------
            // ct_field_info — compile-time counterpart of field_info.
            //
            // Uses string_view literals so the struct is a literal type and
            // can appear in static constexpr variables.
            // -----------------------------------------------------------------
            struct ct_field_info {
                std::string_view name;
                std::size_t byte_offset = 0;
                std::string_view field_type_name; // name of the field's layout
                bool is_bit_field = false;
                std::uint8_t bit_field_width = 0;

                [[nodiscard]] constexpr bool is_base() const noexcept {
                    return !name.empty() && name.front() == '<';
                }

                [[nodiscard]] constexpr type_layout_id field_type_id() const noexcept {
                    return type_layout_id{field_type_name};
                }
            };

            // -----------------------------------------------------------------
            // ct_aggregate_layout<N> — compile-time aggregate with N fields.
            //
            // This is the constexpr-capable layout for structs and classes.
            // The runtime aggregate_layout (below) uses std::vector and cannot
            // be static constexpr.
            // -----------------------------------------------------------------
            template <std::size_t N>
            struct ct_aggregate_layout {
                std::size_t size = 0;
                std::size_t alignment = 0;
                bool is_trivially_destructible = true;
                bool is_union = false;
                std::array<ct_field_info, N> fields{};

                [[nodiscard]] constexpr bool valid() const noexcept {
                    return alignment > 0;
                }

                [[nodiscard]] constexpr const ct_field_info*
                find_field(std::string_view fname) const noexcept {
                    for (const auto& f : fields) {
                        if (f.name == fname) return &f;
                    }
                    return nullptr;
                }

                [[nodiscard]] constexpr bool has_base() const noexcept {
                    for (const auto& f : fields) {
                        if (f.is_base()) return true;
                    }
                    return false;
                }
            };

            // -----------------------------------------------------------------
            // aggregate_layout — runtime struct / class / union.
            //
            // For unions set is_union = true; all fields share byte_offset 0
            // by convention (though the registry does not enforce this).
            // -----------------------------------------------------------------
            struct aggregate_layout {
                std::size_t size = 0;
                std::size_t alignment = 0;
                bool is_trivially_destructible = true;
                bool is_union = false;
                std::vector<field_info> fields;

                [[nodiscard]] constexpr bool valid() const noexcept {
                    return alignment > 0;
                }

                [[nodiscard]] const field_info* find_field(const std::string_view fname) const noexcept {
                    for (const auto& f : fields) {
                        if (f.name == fname) return &f;
                    }
                    return nullptr;
                }

                [[nodiscard]] bool has_base() const noexcept {
                    for (const auto& f : fields) {
                        if (f.is_base()) return true;
                    }
                    return false;
                }
            };

            // -----------------------------------------------------------------
            // pointer_layout — raw / smart pointer or C++ reference.
            // -----------------------------------------------------------------
            enum class pointer_kind : std::uint8_t {
                raw,
                reference,
                rvalue_reference,
                unique_ptr,
                shared_ptr,
                weak_ptr
            };

            struct pointer_layout {
                std::size_t size = sizeof(void*);
                std::size_t alignment = alignof(void*);
                bool is_trivially_destructible = true;
                pointer_kind kind = pointer_kind::raw;
                type_layout_id pointee_id;
                bool is_nullable = true;

                [[nodiscard]] constexpr bool valid() const noexcept {
                    return size > 0;
                }
            };

            // -----------------------------------------------------------------
            // array_layout — C-style array or dynamically-sized array.
            // element_count == 0 means dynamically-sized / unsized.
            // -----------------------------------------------------------------
            struct array_layout {
                std::size_t size = 0;
                std::size_t alignment = 0;
                bool is_trivially_destructible = true;
                type_layout_id element_id;
                std::size_t element_count = 0; // 0 => dynamic

                [[nodiscard]] constexpr bool is_fixed_size() const noexcept {
                    return element_count > 0;
                }

                [[nodiscard]] constexpr bool valid() const noexcept {
                    return alignment > 0 && element_id.valid();
                }
            };

            // -----------------------------------------------------------------
            // type_layout — flat variant over the four layout categories.
            // No virtual dispatch; visitor pattern via std::visit.
            // -----------------------------------------------------------------
            using type_layout = std::variant<
                scalar_layout,
                aggregate_layout,
                pointer_layout,
                array_layout
            >;

            // Convenience accessors ----------------------------------------

            [[nodiscard]] inline std::size_t layout_size(const type_layout& l) noexcept {
                return std::visit([](const auto& x) noexcept -> std::size_t { return x.size; }, l);
            }

            [[nodiscard]] inline std::size_t layout_alignment(const type_layout& l) noexcept {
                return std::visit([](const auto& x) noexcept -> std::size_t { return x.alignment; }, l);
            }

            [[nodiscard]] inline bool layout_trivially_destructible(const type_layout& l) noexcept {
                return std::visit(
                    [](const auto& x) noexcept -> bool { return x.is_trivially_destructible; }, l);
            }

            // =================================================================
            // layout_registry — runtime pluggable registry
            //
            // Synchronised with a plain std::mutex; the finer-grained
            // shared_mutex is unnecessary since write operations are rare
            // (registration at startup) and reads are cheap flat-map scans.
            // =================================================================
            class layout_registry {
            public:
                // Register a type_layout under a given name.  Returns false if the
                // name was already present and overwrite == false.
                bool register_layout(std::string name, type_layout layout,
                                     const bool overwrite = false) {
                    std::lock_guard lock(mutex_);
                    if (!overwrite) {
                        if (table_.contains(name)) return false;
                    }
                    table_[std::move(name)] = std::move(layout);
                    return true;
                }

                // Look up by string_view key (zero-copy on the lookup path).
                [[nodiscard]] std::optional<type_layout>
                find(const std::string_view name) const {
                    std::lock_guard lock(mutex_);
                    if (auto it = table_.find(std::string(name)); it != table_.end()) {
                        return it->second;
                    }
                    return std::nullopt;
                }

                // Convenience: look up via type_layout_id.
                [[nodiscard]] std::optional<type_layout>
                find(const type_layout_id id) const {
                    return find(id.name);
                }

                // True if any entry exists for this name.
                [[nodiscard]] bool contains(const std::string_view name) const {
                    std::lock_guard lock(mutex_);
                    return table_.contains(std::string(name));
                }

                // Number of registered layouts.
                [[nodiscard]] std::size_t size() const {
                    std::lock_guard lock(mutex_);
                    return table_.size();
                }

                // Remove a single entry.  Returns true if it existed.
                bool remove(const std::string_view name) {
                    std::lock_guard lock(mutex_);
                    return table_.erase(std::string(name)) != 0;
                }

                // Iterate over all entries (snapshot to avoid holding lock).
                [[nodiscard]]
                std::vector<std::pair<std::string, type_layout>> snapshot() const {
                    std::lock_guard lock(mutex_);
                    return {table_.begin(), table_.end()};
                }

            private:
                mutable std::mutex mutex_;
                std::unordered_map<std::string, type_layout> table_;
            };

            // Process-wide singleton.
            [[nodiscard]] inline layout_registry& global_layout_registry() {
                static layout_registry instance;
                return instance;
            }

            // =================================================================
            // Compile-time table — consteval-queryable type layout store
            //
            // Usage pattern (at namespace scope):
            //
            //   template<> struct ct_layout_entry<"i32"> {
            //       static constexpr scalar_layout value{4, 4, true,
            //           scalar_kind::signed_integer, 32, true};
            //   };
            //
            // Then in consteval code:
            //   consteval auto get() {
            //       return ct_layout_for<"i32">();
            //   }
            //
            // This is intentionally separate from the runtime registry so that
            // the compiler can constant-fold the whole lookup chain.
            // =================================================================

            // Primary template — deliberately left undefined so that a missing
            // specialisation produces a clear compile error.
            template <::meta::fixed_string Name>
            struct ct_layout_entry;

            // Query function — constrains to types that have a specialisation.
            template <::meta::fixed_string Name>
            [[nodiscard]] consteval auto ct_layout_for() noexcept
                -> decltype(ct_layout_entry<Name>::value) {
                return ct_layout_entry<Name>::value;
            }

            // -----------------------------------------------------------------
            // Built-in specialisations for common C++ primitives
            // -----------------------------------------------------------------

            template <>
            struct ct_layout_entry<"bool"> {
                static constexpr scalar_layout value{
                    1, 1, true, scalar_kind::boolean, 1, false
                };
            };

            template <>
            struct ct_layout_entry<"i8"> {
                static constexpr scalar_layout value{
                    1, 1, true, scalar_kind::signed_integer, 8, true
                };
            };

            template <>
            struct ct_layout_entry<"u8"> {
                static constexpr scalar_layout value{
                    1, 1, true, scalar_kind::unsigned_integer, 8, false
                };
            };

            template <>
            struct ct_layout_entry<"i16"> {
                static constexpr scalar_layout value{
                    2, 2, true, scalar_kind::signed_integer, 16, true
                };
            };

            template <>
            struct ct_layout_entry<"u16"> {
                static constexpr scalar_layout value{
                    2, 2, true, scalar_kind::unsigned_integer, 16, false
                };
            };

            template <>
            struct ct_layout_entry<"i32"> {
                static constexpr scalar_layout value{
                    4, 4, true, scalar_kind::signed_integer, 32, true
                };
            };

            template <>
            struct ct_layout_entry<"u32"> {
                static constexpr scalar_layout value{
                    4, 4, true, scalar_kind::unsigned_integer, 32, false
                };
            };

            template <>
            struct ct_layout_entry<"i64"> {
                static constexpr scalar_layout value{
                    8, 8, true, scalar_kind::signed_integer, 64, true
                };
            };

            template <>
            struct ct_layout_entry<"u64"> {
                static constexpr scalar_layout value{
                    8, 8, true, scalar_kind::unsigned_integer, 64, false
                };
            };

            template <>
            struct ct_layout_entry<"f32"> {
                static constexpr scalar_layout value{
                    4, 4, true, scalar_kind::floating_point, 32, true
                };
            };

            template <>
            struct ct_layout_entry<"f64"> {
                static constexpr scalar_layout value{
                    8, 8, true, scalar_kind::floating_point, 64, true
                };
            };

            template <>
            struct ct_layout_entry<"ptr"> {
                static constexpr pointer_layout value{
                    sizeof(void*), alignof(void*), true, pointer_kind::raw,
                    invalid_layout_id, true
                };
            };

            // -----------------------------------------------------------------
            // RAII helper: register a layout in global_layout_registry() on
            // construction; remove it on destruction (useful for test fixtures).
            // -----------------------------------------------------------------
            struct scoped_layout_registration {
                std::string name;

                template <typename L>
                scoped_layout_registration(std::string n, L layout)
                    : name(std::move(n)) {
                    global_layout_registry().register_layout(name, type_layout{std::move(layout)});
                }

                ~scoped_layout_registration() {
                    global_layout_registry().remove(name);
                }

                scoped_layout_registration(const scoped_layout_registration&) = delete;

                scoped_layout_registration& operator=(const scoped_layout_registration&) = delete;
            };

            // -----------------------------------------------------------------
            // Factory helpers for building common layouts concisely
            // -----------------------------------------------------------------

            [[nodiscard]] inline scalar_layout make_scalar(
                const std::size_t sz, const std::size_t align,
                const scalar_kind kind, const std::uint16_t bits,
                const bool is_signed = true,
                const bool trivially_destructible = true) noexcept {
                return scalar_layout{sz, align, trivially_destructible, kind, bits, is_signed};
            }

            [[nodiscard]] inline aggregate_layout make_aggregate(
                const std::size_t sz, const std::size_t align,
                std::vector<field_info> fields = {},
                const bool is_union = false,
                const bool trivially_destructible = true) {
                return aggregate_layout{
                    sz, align, trivially_destructible, is_union,
                    std::move(fields)
                };
            }

            [[nodiscard]] inline pointer_layout make_pointer(
                const type_layout_id pointee = invalid_layout_id,
                const pointer_kind kind = pointer_kind::raw,
                const bool nullable = true) noexcept {
                return pointer_layout{
                    sizeof(void*), alignof(void*), true,
                    kind, pointee, nullable
                };
            }

            [[nodiscard]] inline array_layout make_array(
                const type_layout_id element,
                const std::size_t element_sz, const std::size_t element_align,
                const std::size_t count = 0,
                const bool trivially_destructible = true) noexcept {
                const std::size_t total = count ? element_sz * count : 0;
                return array_layout{
                    total, element_align, trivially_destructible,
                    element, count
                };
            }
        } // namespace layout
    } // namespace semantic
} // namespace lithe

// -------------------------------------------------------------------------
// std::hash specialisation for type_variable_id
// Must be outside all lithe namespaces to specialise std::hash.
// -------------------------------------------------------------------------
template <>
struct std::hash<lithe::semantic::types::type_variable_id> {
    constexpr std::size_t operator()(
        const lithe::semantic::types::type_variable_id& v) const noexcept {
        return std::hash<std::uint32_t>{}(v.id);
    }
};
