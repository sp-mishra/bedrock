#pragma once

#include "lithe_core.hpp"
#include "lithe_semantic.hpp"
#include "lithe_passes.hpp"

#include "containers/tree/NAryTree.hpp"
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"

#include <array>
#include <any>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <queue>

#ifndef LITHE_ENABLE_OBSERVABILITY
#define LITHE_ENABLE_OBSERVABILITY 0
#endif

// LITHE_HAS_NADI, LITHE_HAS_THREAD_LOCAL_SINK and LITHE_HAS_PROFILER are defined transitively via lithe_passes.hpp.

namespace lithe::backend {
    struct BasicBlock;
    struct ControlEdge;
    struct SymbolicExpression;
    struct DependencyEdge;
}

namespace std {
    template <>
    struct hash<lithe::backend::BasicBlock> {
        std::size_t operator()(const lithe::backend::BasicBlock& block) const noexcept;
    };

    template <>
    struct hash<lithe::backend::ControlEdge> {
        std::size_t operator()(const lithe::backend::ControlEdge& edge) const noexcept;
    };

    template <>
    struct hash<lithe::backend::SymbolicExpression> {
        std::size_t operator()(const lithe::backend::SymbolicExpression& expr) const noexcept;
    };

    template <>
    struct hash<lithe::backend::DependencyEdge> {
        std::size_t operator()(const lithe::backend::DependencyEdge& edge) const noexcept;
    };
}

namespace lithe { namespace backend {
        enum class operation_category : std::uint8_t {
            unknown,
            terminal,
            arithmetic,
            logical,
            comparison,
            control_flow,
            dataflow,
            custom
        };

        enum class dependency_kind : std::uint8_t {
            // Coarse-grained (legacy / SymbolicDAG builder).
            data,
            control,
            anti,
            output,
            order,
            custom,
            // Fine-grained PDG variants (Phase 3).
            data_raw, // Read-After-Write  (true dependence)
            data_war, // Write-After-Read  (anti dependence)
            data_waw, // Write-After-Write (output dependence)
            data_raw_cross, // RAW crossing an rpc_boundary or async_fork edge
            control_direct, // Direct control dependence (branch → dominated successor)
        };

        struct ASTNodeData {
            std::string operation;
            std::variant<double, int, std::string> value;
            std::size_t node_id;

            constexpr explicit ASTNodeData(std::string op = "", const std::size_t id = 0)
                : operation(std::move(op)), node_id(id) {}

            constexpr bool operator==(const ASTNodeData& other) const {
                return operation == other.operation && value == other.value;
            }
        };

        using ASTTree = NAryTree<ASTNodeData, EmptyMetadata>;
        using ASTNode = ASTTree::TreeNode;

        struct ast_builder {
            mutable ASTTree tree;
            mutable std::size_t next_id = 1;
            mutable std::unordered_map<ASTNode*, ASTTree> subtrees_;

            template <class T>
            ASTNode* on_terminal(T&& t) const {
                ASTNodeData data{"terminal", next_id++};

                if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
                    if constexpr (std::is_integral_v<std::decay_t<T>>) {
                        data.value = static_cast<int>(t);
                    }
                    else {
                        data.value = static_cast<double>(t);
                    }
                }
                else {
                    data.value = std::string("var");
                }

                ASTTree t_tree;
                ASTNode* node = t_tree.insert(nullptr, std::move(data));
                subtrees_.emplace(node, std::move(t_tree));
                return node;
            }

            template <class Tag, class... Children>
            ASTNode* on_node(Tag, Children&&... children) const {
                ASTNodeData data{get_operation_name<Tag>(), next_id++};

                ASTTree node_tree;
                ASTNode* node = node_tree.insert(nullptr, std::move(data));

                ([&](auto&& child) {
                    if constexpr (std::is_pointer_v<std::decay_t<decltype(child)>>) {
                        if (child) {
                            auto it = subtrees_.find(child);
                            if (it != subtrees_.end()) {
                                node_tree.graft(node, std::move(it->second));
                                subtrees_.erase(it);
                            }
                        }
                    }
                }(std::forward<Children>(children)), ...);

                subtrees_.emplace(node, std::move(node_tree));
                return node;
            }

            // Call after visit() to retrieve the assembled tree.
            // Exactly one subtree remains in the map after a complete visit — the root.
            void finalize() const {
                if (subtrees_.size() == 1) {
                    tree = std::move(subtrees_.begin()->second);
                    subtrees_.clear();
                }
            }

            void finalize(ASTNode* root) const {
                auto it = subtrees_.find(root);
                if (it != subtrees_.end()) {
                    tree = std::move(it->second);
                    subtrees_.erase(it);
                }
            }

            template <class Tag>
            std::string get_operation_name() const {
                if constexpr (std::is_same_v<Tag, add_tag>) return "add";
                else if constexpr (std::is_same_v<Tag, sub_tag>) return "sub";
                else if constexpr (std::is_same_v<Tag, mul_tag>) return "mul";
                else if constexpr (std::is_same_v<Tag, div_tag>) return "div";
                else if constexpr (std::is_same_v<Tag, neg_tag>) return "neg";
                else return "unknown";
            }
        };

        struct BasicBlock {
            std::size_t block_id;
            std::vector<std::string> instructions;

            constexpr bool operator==(const BasicBlock& other) const {
                return block_id == other.block_id;
            }

            constexpr auto operator<=>(const BasicBlock& other) const = default;
        };

        struct ControlEdge {
            enum class Type { Fallthrough, Branch, Jump, Return };

            Type edge_type = Type::Fallthrough;
            std::optional<std::string> condition;

            constexpr bool operator==(const ControlEdge& other) const {
                return edge_type == other.edge_type && condition == other.condition;
            }

            constexpr auto operator<=>(const ControlEdge& other) const = default;
        };

        using CFG = litegraph::Graph<BasicBlock, ControlEdge>;

        struct cfg_builder {
            mutable std::vector<BasicBlock> blocks;
            mutable std::size_t next_block_id = 1;

            template <class T>
            std::size_t on_terminal(T&&) const {
                blocks.emplace_back(BasicBlock{next_block_id++, {"terminal"}});
                return blocks.size() - 1;
            }

            template <class Tag, class... Args>
            std::size_t on_node(Tag, Args&&... args) const {
                blocks.emplace_back(BasicBlock{next_block_id++, {get_operation_name<Tag>()}});
                return blocks.size() - 1;
            }

            template <class Tag>
            std::string get_operation_name() const {
                if constexpr (std::is_same_v<Tag, add_tag>) return "add";
                else if constexpr (std::is_same_v<Tag, sub_tag>) return "sub";
                else if constexpr (std::is_same_v<Tag, mul_tag>) return "mul";
                else if constexpr (std::is_same_v<Tag, seq_tag>) return "sequence";
                else return "operation";
            }
        };

        struct SymbolicExpression {
            enum class Type { Variable, Constant, Operation };

            Type type;
            std::string name;
            std::variant<double, int, std::string> value;
            std::size_t expr_id{};
            operation_category category = operation_category::unknown;
            structural_hash_t structural_hash = 0;
            std::optional<structural_hash_t> semantic_fingerprint;

            constexpr bool operator==(const SymbolicExpression& other) const {
                return expr_id == other.expr_id && type == other.type && name == other.name &&
                    category == other.category && structural_hash == other.structural_hash;
            }
        };

        struct DependencyEdge {
            enum class Type { DataFlow, ControlFlow, AntiDep, OutputDep };

            Type dep_type = Type::DataFlow;
            dependency_kind kind = dependency_kind::data;
            structural_hash_t structural_hash = 0;
            double weight = 1.0;
            std::optional<structural_hash_t> semantic_fingerprint;

            // Phase 3: fine-grained PDG classification helpers.
            [[nodiscard]] constexpr bool is_data_dependency() const noexcept {
                if (dep_type == Type::ControlFlow) return false;
                return dep_type == Type::DataFlow ||
                    dep_type == Type::AntiDep ||
                    dep_type == Type::OutputDep ||
                    kind == dependency_kind::data ||
                    kind == dependency_kind::data_raw ||
                    kind == dependency_kind::data_war ||
                    kind == dependency_kind::data_waw ||
                    kind == dependency_kind::data_raw_cross;
            }

            [[nodiscard]] constexpr bool is_control_dependency() const noexcept {
                return dep_type == Type::ControlFlow ||
                    kind == dependency_kind::control ||
                    kind == dependency_kind::control_direct;
            }

            constexpr bool operator==(const DependencyEdge& other) const {
                return dep_type == other.dep_type && kind == other.kind &&
                    structural_hash == other.structural_hash && weight == other.weight;
            }
        };

        using SymbolicDAG = litegraph::Graph<SymbolicExpression, DependencyEdge>;

        struct symbolic_dag_builder {
            mutable SymbolicDAG dag;
            mutable std::size_t next_expr_id = 1;
            mutable std::unordered_map<std::size_t, litegraph::NodeId> hash_to_vertex;

            template <class Child>
            std::pair<litegraph::NodeId, structural_hash_t> materialize_child(Child&& child) const {
                using child_t = std::decay_t<Child>;
                if constexpr (std::is_same_v<child_t, litegraph::NodeId>) {
                    const auto& child_payload = dag.node_data(child);
                    return {child, child_payload.structural_hash};
                }
                else {
                    auto child_id = on_terminal(std::forward<Child>(child));
                    const auto& child_payload = dag.node_data(child_id);
                    return {child_id, child_payload.structural_hash};
                }
            }

            template <class T>
            litegraph::NodeId on_terminal(T&& t) const {
                auto hash = emit::structural_hash(t);
                if (auto it = hash_to_vertex.find(hash); it != hash_to_vertex.end()) {
                    return it->second;
                }

                SymbolicExpression expr;
                expr.expr_id = next_expr_id++;
                if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
                    expr.category = operation_category::terminal;
                }
                else {
                    expr.category = operation_category::dataflow;
                }

                if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
                    expr.type = SymbolicExpression::Type::Constant;
                    expr.name = "const";
                    if constexpr (std::is_integral_v<std::decay_t<T>>) {
                        expr.value = static_cast<int>(t);
                    }
                    else {
                        expr.value = static_cast<double>(t);
                    }
                }
                else {
                    expr.type = SymbolicExpression::Type::Variable;
                    expr.name = "var";
                    expr.value = std::string("unknown");
                }
                expr.structural_hash = hash;
                expr.semantic_fingerprint = semantic_fingerprint(semantic::infer_semantics(t));

                auto vertex_id = dag.add_node(std::move(expr));
                hash_to_vertex[hash] = vertex_id;
                return vertex_id;
            }

            template <class Tag, class... Children>
            litegraph::NodeId on_node(Tag, Children&&... children) const {
                std::size_t op_hash = emit::tag_id<Tag>::value;

                auto child_nodes = std::array{materialize_child(std::forward<Children>(children))...};
                for (const auto& [child_id, child_hash] : child_nodes) {
                    (void)child_id;
                    op_hash = emit::hash_combine(op_hash, child_hash);
                }

                if (auto it = hash_to_vertex.find(op_hash); it != hash_to_vertex.end()) {
                    return it->second;
                }

                SymbolicExpression expr;
                expr.type = SymbolicExpression::Type::Operation;
                expr.name = get_operation_name<Tag>();
                expr.expr_id = next_expr_id++;
                expr.category = get_operation_category<Tag>();
                expr.structural_hash = op_hash;

                semantic::semantic_info merged_semantics;
                for (const auto& [child_id, child_hash] : child_nodes) {
                    (void)child_hash;
                    const auto& child_payload = dag.node_data(child_id);
                    if (child_payload.semantic_fingerprint.has_value()) {
                        const auto child_sem = semantic::get_semantics(
                            semantic::semantic_node::from_key(*child_payload.semantic_fingerprint));
                        if (child_sem.has_value()) {
                            merged_semantics.merge_overlay(*child_sem);
                        }
                    }
                }
                merged_semantics.normalize();
                if (const auto merged_hash = semantic_fingerprint(merged_semantics); merged_hash != 0) {
                    expr.semantic_fingerprint = merged_hash;
                    semantic::registry().merge(op_hash, merged_semantics);
                }

                auto vertex_id = dag.add_node(std::move(expr));
                hash_to_vertex[op_hash] = vertex_id;
                for (const auto& [child_id, child_hash] : child_nodes) {
                    (void)child_hash;
                    add_dependency(vertex_id, child_id);
                }
                return vertex_id;
            }

            template <class Child>
            void add_dependency(litegraph::NodeId parent, Child&& child) const {
                auto child_id = materialize_child(std::forward<Child>(child)).first;
                const auto& source = dag.node_data(child_id);
                const auto& target = dag.node_data(parent);
                DependencyEdge edge{};
                edge.dep_type = DependencyEdge::Type::DataFlow;
                edge.kind = dependency_kind::data;
                edge.structural_hash = emit::hash_combine(source.structural_hash, target.structural_hash);
                edge.semantic_fingerprint = target.semantic_fingerprint;
                dag.add_edge(child_id, parent, edge);
            }

            template <class Tag>
            std::string get_operation_name() const {
                if constexpr (std::is_same_v<Tag, add_tag>) return "add";
                else if constexpr (std::is_same_v<Tag, sub_tag>) return "sub";
                else if constexpr (std::is_same_v<Tag, mul_tag>) return "mul";
                else if constexpr (std::is_same_v<Tag, div_tag>) return "div";
                else return "op";
            }

            template <class Tag>
            operation_category get_operation_category() const {
                if constexpr (std::is_same_v<Tag, add_tag> ||
                    std::is_same_v<Tag, sub_tag> ||
                    std::is_same_v<Tag, mul_tag> ||
                    std::is_same_v<Tag, div_tag> ||
                    std::is_same_v<Tag, neg_tag>) {
                    return operation_category::arithmetic;
                }
                return operation_category::custom;
            }

            static structural_hash_t semantic_fingerprint(const semantic::semantic_info& info) {
                std::size_t seed = std::hash<int>{}(static_cast<int>(info.effect));
                seed = emit::hash_combine(seed, std::hash<int>{}(static_cast<int>(info.domain)));
                seed = emit::hash_combine(seed, std::hash<int>{}(static_cast<int>(info.purity_level)));
                seed = emit::hash_combine(seed, std::hash<int>{}(static_cast<int>(info.evaluation)));
                return seed;
            }
        };

        struct unified_backend {
            ast_builder ast;
            cfg_builder cfg;
            symbolic_dag_builder symbolic;

            template <class Expr>
            auto analyze_expression(const Expr& expr) const {
                struct analysis_result {
                    ASTTree ast_tree;
                    CFG control_flow;
                    SymbolicDAG symbolic_dag;

                    [[nodiscard]] std::size_t complexity_score() const {
                        return ast_tree.size() + control_flow.node_count() + symbolic_dag.node_count();
                    }

                    [[nodiscard]] bool has_control_flow() const {
                        return control_flow.node_count() > 1;
                    }

                    [[nodiscard]] double cse_benefit() const {
                        auto total_nodes = symbolic_dag.node_count();
                        return total_nodes > 0 ? 0.3 : 0.0;
                    }
                };

                visit(expr, ast);
                ast.finalize();
                visit(expr, cfg);
                visit(expr, symbolic);

                return analysis_result{
                    std::move(ast.tree),
                    CFG{},
                    std::move(symbolic.dag)
                };
            }
        };
    } // namespace backend

    namespace lowering {
        enum class frontend_kind : std::uint8_t {
            unknown,
            lithe_edsl,
            lexy,
            handwritten,
            json_ast,
            external_dsl,
            custom
        };

        struct source_span {
            std::size_t offset = 0;
            std::size_t length = 0;
            std::size_t line = 0;
            std::size_t column = 0;
        };

        struct frontend_diagnostic {
            enum class level {
                info,
                warning,
                error
            };

            level severity = level::info;
            std::string message;
            std::optional<source_span> span;
        };

        struct source_file {
            std::size_t file_id = 0;
            std::string name;
            std::string content;
        };

        struct source_mapping {
            std::size_t file_id = 0;
            source_span span{};
            structural_hash_t expression_key = 0;
            std::string ast_node_id;
        };

        struct frontend_ast {
            std::string node_kind;
            std::string node_id;
            // Payload: variant over common types to avoid heap allocation for small payloads.
            std::variant<std::monostate,
                         std::unordered_map<std::string, std::string>,
                         std::any> payload;
            std::vector<frontend_ast> children;
            std::optional<source_span> span;
        };

        struct frontend_context {
            frontend_kind kind = frontend_kind::unknown;
            std::vector<source_file> files;
            std::vector<source_mapping> mappings;
            std::vector<frontend_diagnostic> diagnostics;
            std::vector<frontend_ast> ast_roots;
            std::unordered_map<structural_hash_t, source_mapping> expression_source_map;

            [[nodiscard]] std::size_t add_source_file(std::string name, std::string content) {
                const std::size_t id = files.size() + 1;
                files.push_back(source_file{id, std::move(name), std::move(content)});
                return id;
            }

            void add_ast(frontend_ast ast) {
                ast_roots.push_back(std::move(ast));
            }

            void add_diagnostic(frontend_diagnostic diagnostic) {
                diagnostics.push_back(std::move(diagnostic));
            }

            void add_mapping(source_mapping mapping) {
                mappings.push_back(mapping);
                if (mapping.expression_key != 0) {
                    expression_source_map[mapping.expression_key] = std::move(mapping);
                }
            }

            template <class Expr>
            void map_expression(const Expr& expr, const source_span span, const std::size_t file_id = 0,
                                std::string ast_node_id = {}) {
                if constexpr (requires { lithe::structural_key(expr); }) {
                    add_mapping(source_mapping{
                        file_id,
                        span,
                        lithe::structural_key(expr),
                        std::move(ast_node_id)
                    });
                }
            }

            [[nodiscard]] std::optional<source_mapping> mapping_for_expression(
                const structural_hash_t expression_key) const {
                if (auto it = expression_source_map.find(expression_key); it != expression_source_map.end()) {
                    return it->second;
                }
                return std::nullopt;
            }
        };

        template <class T>
        struct parsed_expr {
            std::optional<T> expr;
            std::vector<frontend_diagnostic> diagnostics;
            std::optional<source_span> span;

            [[nodiscard]] bool ok() const {
                return expr.has_value() &&
                    std::none_of(diagnostics.begin(), diagnostics.end(),
                                 [](const frontend_diagnostic& d) {
                                     return d.severity == frontend_diagnostic::level::error;
                                 });
            }
        };

        template <class T>
        struct frontend_result {
            std::optional<T> ir;
            std::vector<frontend_diagnostic> diagnostics;
            std::optional<source_span> span;
            frontend_kind kind = frontend_kind::unknown;

            [[nodiscard]] bool ok() const {
                return ir.has_value() &&
                    std::none_of(diagnostics.begin(), diagnostics.end(),
                                 [](const frontend_diagnostic& d) {
                                     return d.severity == frontend_diagnostic::level::error;
                                 });
            }
        };

        template <class InputT = frontend_ast, class OutputT = frontend_ast>
        struct frontend_transform {
            std::string name;
            std::function<OutputT(const InputT&, frontend_context&)> apply;

            [[nodiscard]] OutputT operator()(const InputT& input, frontend_context& ctx) const {
                return apply(input, ctx);
            }
        };

        inline frontend_ast normalize_frontend_ast(frontend_ast ast) {
            if (ast.node_id.empty()) {
                if (ast.span.has_value()) {
                    ast.node_id = "span:" + std::to_string(ast.span->offset) + ":" + std::to_string(ast.span->length);
                }
                else {
                    ast.node_id = "node:" + ast.node_kind;
                }
            }
            for (auto& child : ast.children) {
                child = normalize_frontend_ast(std::move(child));
            }
            return ast;
        }

        constexpr std::string_view normalize_frontend_input(const std::string_view input) {
            return input;
        }

        constexpr std::string_view normalize_frontend_input(const std::string& input) {
            return std::string_view{input};
        }

        constexpr std::string_view normalize_frontend_input(const char* input) {
            return input ? std::string_view{input} : std::string_view{};
        }

        template <class Input>
        constexpr decltype(auto) normalize_frontend_input(Input&& input) {
            return std::forward<Input>(input);
        }

        template <class Adapter, class Input, class = void>
        struct frontend_adapter_traits {
            static constexpr frontend_kind kind = frontend_kind::custom;

            static constexpr decltype(auto) normalize(Input&& input) {
                return normalize_frontend_input(std::forward<Input>(input));
            }

            static constexpr decltype(auto) normalize(Input&& input, frontend_context&) {
                return normalize_frontend_input(std::forward<Input>(input));
            }
        };

        template <class Adapter, class Input>
        struct frontend_adapter_traits<Adapter, Input, std::void_t<decltype(Adapter::kind)>> {
            static constexpr frontend_kind kind = Adapter::kind;

            static constexpr decltype(auto) normalize(Input&& input) {
                return normalize_frontend_input(std::forward<Input>(input));
            }

            static constexpr decltype(auto) normalize(Input&& input, frontend_context&) {
                return normalize_frontend_input(std::forward<Input>(input));
            }
        };

        template <class Adapter, class Input, class = void>
        struct adapter_traits : frontend_adapter_traits<Adapter, Input> {};

        template <class Adapter, class Input>
        constexpr auto normalize_to_lithe_ir(Adapter&& adapter, Input&& input) {
            auto normalized = frontend_adapter_traits<
                std::remove_cvref_t<Adapter>,
                Input
            >::normalize(std::forward<Input>(input));

            if constexpr (requires { adapter.normalize_to_lithe_ir(normalized); }) {
                return adapter.normalize_to_lithe_ir(normalized);
            }
            else {
                return normalized;
            }
        }

        template <class Adapter, class Input>
        constexpr auto normalize_to_lithe_ir(Adapter&& adapter, Input&& input, frontend_context& ctx) {
            auto normalized = frontend_adapter_traits<
                std::remove_cvref_t<Adapter>,
                Input
            >::normalize(std::forward<Input>(input), ctx);

            if constexpr (requires { adapter.normalize_to_lithe_ir(normalized, ctx); }) {
                return adapter.normalize_to_lithe_ir(normalized, ctx);
            }
            else if constexpr (requires { adapter.normalize_to_lithe_ir(normalized); }) {
                return adapter.normalize_to_lithe_ir(normalized);
            }
            else {
                return normalized;
            }
        }

        template <class Adapter, class Input>
        concept FrontendAdapter = requires(Adapter adapter, Input&& input) {
            adapter.parse(normalize_to_lithe_ir(adapter, std::forward<Input>(input)));
        };

        template <class Adapter, class Input>
            requires FrontendAdapter<Adapter, Input>
        constexpr auto parse_to_lithe(Adapter&& adapter, Input&& input, frontend_context& ctx);

        template <class Adapter, class Input>
            requires FrontendAdapter<Adapter, Input>
        constexpr auto parse_to_lithe(Adapter&& adapter, Input&& input) {
            frontend_context unused_ctx;
            return parse_to_lithe(std::forward<Adapter>(adapter), std::forward<Input>(input), unused_ctx);
        }

        template <class Result>
        void propagate_frontend_diagnostics(frontend_context& ctx, const Result& result) {
            if constexpr (requires { result.diagnostics; }) {
                ctx.diagnostics.insert(ctx.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
            }
        }

        template <class Adapter, class Input>
            requires FrontendAdapter<Adapter, Input>
        constexpr auto parse_to_lithe(Adapter&& adapter, Input&& input, frontend_context& ctx) {
            auto lithe_ir_input = normalize_to_lithe_ir(adapter, std::forward<Input>(input), ctx);
            auto result = std::forward<Adapter>(adapter).parse(lithe_ir_input);
            constexpr auto kind = frontend_adapter_traits<std::remove_cvref_t<Adapter>, Input>::kind;
            ctx.kind = kind;

            if constexpr (requires { result.ir; result.diagnostics; }) {
                auto wrapped = std::move(result);
                if constexpr (requires { wrapped.kind; }) {
                    if (wrapped.kind == frontend_kind::unknown) {
                        wrapped.kind = kind;
                    }
                }
                propagate_frontend_diagnostics(ctx, wrapped);
                if (wrapped.span.has_value() && wrapped.ir.has_value()) {
                    ctx.map_expression(*wrapped.ir, *wrapped.span);
                }
                return wrapped;
            }
            else if constexpr (requires { result.expr; result.diagnostics; }) {
                using expr_opt_t = std::decay_t<decltype(result.expr)>;
                using expr_t = expr_opt_t::value_type;
                auto wrapped = frontend_result<expr_t>{
                    std::move(result.expr),
                    std::move(result.diagnostics),
                    std::move(result.span),
                    kind
                };
                propagate_frontend_diagnostics(ctx, wrapped);
                if (wrapped.span.has_value() && wrapped.ir.has_value()) {
                    ctx.map_expression(*wrapped.ir, *wrapped.span);
                }
                return wrapped;
            }
            else {
                using ir_t = std::decay_t<decltype(result)>;
                auto wrapped = frontend_result<ir_t>{
                    std::optional<ir_t>{std::move(result)},
                    {},
                    std::nullopt,
                    kind
                };
                propagate_frontend_diagnostics(ctx, wrapped);
                return wrapped;
            }
        }

        struct lowering_context {
            structural_hash_t input_hash = 0;
            structural_hash_t output_hash = 0;
            bool equivalent = false;
            std::size_t passes_applied = 0;
            std::vector<std::string> pass_names;
        };

        enum class lowering_stage : std::uint8_t {
            capture,
            semantic_propagation,
            normalization,
            optimization,
            graph_lowering,
            backend_lowering,
            codegen_preparation
        };

        struct compiler_context {
            lowering_stage current_stage = lowering_stage::capture;
            lowering_context legacy;
            std::vector<lowering_stage> stage_trace;
            std::vector<std::string> stage_notes;
        };

        struct graph_lowering_context {
            lowering_context lowering{};
            semantic::semantic_context semantics{};
            std::vector<litegraph::NodeId> roots;
            bool enable_cse = true;
            bool enable_canonicalization = true;
            bool preserve_semantics = true;
        };

        using shared_expr = passes::shared_expr;
        using dag_node = passes::dag_node;
        using dag_region = passes::dag_region;
        using dag_expr = passes::dag_expr;
        using structural_intern_table = passes::structural_intern_table;

        template <class Expr>
        [[nodiscard]] dag_expr canonical_dag(
            const Expr& expr,
            const semantic::semantic_registry* semantic_registry = nullptr,
            std::string region_name = "root"
        ) {
            return passes::to_canonical_dag(expr, semantic_registry, std::move(region_name));
        }

        template <class Expr>
        [[nodiscard]] dag_expr canonical_dag(const Expr& expr, const graph_lowering_context& ctx,
                                             std::string region_name = "root") {
            return passes::to_canonical_dag(expr, std::addressof(ctx.semantics.store()), std::move(region_name));
        }

        template <class CanonicalExpr>
        [[nodiscard]] dag_expr canonical_dag_from_canonical(
            const lithe::canonical_expr<CanonicalExpr>& canonical,
            const semantic::semantic_registry* semantic_registry = nullptr,
            std::string region_name = "root"
        ) {
            return passes::to_dag_expr(canonical, semantic_registry, std::move(region_name));
        }

        template <class CanonicalExpr>
        [[nodiscard]] dag_expr canonical_dag_from_canonical(
            const lithe::canonical_expr<CanonicalExpr>& canonical,
            const graph_lowering_context& ctx,
            std::string region_name = "root"
        ) {
            return passes::to_dag_expr(canonical, std::addressof(ctx.semantics.store()), std::move(region_name));
        }

        template <litegraph::LiteGraphModel GraphT>
        struct graph_analysis_result_t {
            std::size_t node_count = 0;
            std::size_t edge_count = 0;
            std::size_t reusable_node_count = 0;
            std::size_t shared_subtree_count = 0;
            std::size_t duplicate_dependency_count = 0;
            bool is_dag = true;
            std::vector<litegraph::NodeId> topological_order;
            std::vector<litegraph::NodeId> canonical_order;
        };

        template <litegraph::LiteGraphModel GraphT>
        struct graph_optimization_result_t {
            GraphT graph;
            graph_analysis_result_t<GraphT> before;
            graph_analysis_result_t<GraphT> after;
            std::vector<std::string> applied_passes;
            std::size_t removed_dead_nodes = 0;
            std::size_t merged_shared_subtrees = 0;
            std::size_t simplified_dependencies = 0;
            std::size_t dominator_simplifications = 0;
        };

        using graph_analysis_result = graph_analysis_result_t<backend::SymbolicDAG>;
        using graph_optimization_result = graph_optimization_result_t<backend::SymbolicDAG>;

        struct symbolic_value {
            std::size_t value_id = 0;
            structural_hash_t structural_hash = 0;
            std::variant<std::monostate, bool, int, double, std::string> payload;
            std::optional<structural_hash_t> semantic_fingerprint;
            bool is_lazy = true;
        };

        struct symbolic_operation {
            std::size_t operation_id = 0;
            std::string opcode;
            backend::operation_category category = backend::operation_category::unknown;
            structural_hash_t structural_hash = 0;
            bool is_pure = true;
        };

        struct symbolic_dependency {
            litegraph::NodeId source;
            litegraph::NodeId target;
            backend::dependency_kind kind = backend::dependency_kind::data;
            double weight = 1.0;
            bool is_prunable = false;
        };

        struct symbolic_region {
            std::size_t region_id = 0;
            std::string name = "root";
            std::vector<litegraph::NodeId> nodes;
            std::vector<litegraph::NodeId> entry_nodes;
            std::vector<litegraph::NodeId> exit_nodes;
        };

        struct symbolic_schedule_step {
            litegraph::NodeId node;
            std::size_t stage = 0;
            bool lazy = true;
        };

        struct symbolic_schedule {
            std::vector<symbolic_schedule_step> steps;
            std::vector<std::vector<litegraph::NodeId>> stages;
            bool is_topological = true;

            [[nodiscard]] bool empty() const { return steps.empty(); }
            [[nodiscard]] std::size_t size() const { return steps.size(); }
        };

        template <litegraph::LiteGraphModel GraphT>
        struct symbolic_dataflow_ir {
            GraphT graph;
            symbolic_region region;
            symbolic_schedule schedule;
            graph_analysis_result_t<GraphT> analysis;
        };

        template <litegraph::LiteGraphModel GraphT>
        struct symbolic_simplification_result {
            GraphT graph;
            std::size_t removed_nodes = 0;
            std::size_t removed_dependencies = 0;
            std::size_t merged_subtrees = 0;
            std::vector<std::string> passes;
        };

        template <litegraph::LiteGraphModel GraphT>
        struct dependency_tracking_result {
            std::unordered_map<litegraph::NodeId, std::vector<litegraph::NodeId>> dependencies;
            std::unordered_map<litegraph::NodeId, std::vector<litegraph::NodeId>> users;
            std::vector<symbolic_dependency> edges;
        };

        template <litegraph::LiteGraphModel GraphT>
        graph_analysis_result_t<GraphT> analyze_graph_ir(
            const GraphT& graph,
            const std::vector<litegraph::NodeId>& roots = {}
        );

        template <litegraph::LiteGraphModel GraphT>
        std::size_t dead_node_elimination(GraphT& graph, const std::vector<litegraph::NodeId>& roots = {});

        template <litegraph::LiteGraphModel GraphT>
        std::size_t shared_subtree_extraction(GraphT & graph);

        template <litegraph::LiteGraphModel GraphT>
        std::size_t dependency_simplification(GraphT & graph);

        template <litegraph::LiteGraphModel GraphT>
        struct symbolic_dataflow_engine {
            static dependency_tracking_result<GraphT> track_dependencies(const GraphT& graph) {
                dependency_tracking_result<GraphT> out;
                for (const auto& [nid, node] : graph.nodes()) {
                    out.dependencies[litegraph::NodeId{nid}] = {};
                    out.users[litegraph::NodeId{nid}] = {};
                }

                for (const auto& [eid, edge] : graph.edges()) {
                    out.dependencies[edge.to].push_back(edge.from);
                    out.users[edge.from].push_back(edge.to);

                    symbolic_dependency dep;
                    dep.source = edge.from;
                    dep.target = edge.to;
                    if constexpr (requires { edge.data.kind; }) {
                        dep.kind = edge.data.kind;
                    }
                    if constexpr (requires { edge.data.weight; }) {
                        dep.weight = edge.data.weight;
                    }
                    out.edges.push_back(dep);
                }
                return out;
            }

            static symbolic_schedule build_schedule(const GraphT& graph) {
                symbolic_schedule schedule;
                auto analysis = analyze_graph_ir(graph);
                schedule.is_topological = analysis.is_dag;

                std::unordered_map<litegraph::NodeId, std::size_t> indegree;
                std::queue<litegraph::NodeId> ready;
                for (const auto& [nid, node] : graph.nodes()) {
                    const auto id = litegraph::NodeId{nid};
                    indegree[id] = 0;
                }
                for (const auto& [eid, edge] : graph.edges()) {
                    ++indegree[edge.to];
                }

                for (const auto& [nid, node] : graph.nodes()) {
                    const auto id = litegraph::NodeId{nid};
                    if (indegree[id] == 0) {
                        ready.push(id);
                    }
                }

                std::size_t stage = 0;
                while (!ready.empty()) {
                    const std::size_t width = ready.size();
                    schedule.stages.emplace_back();
                    for (std::size_t i = 0; i < width; ++i) {
                        const auto u = ready.front();
                        ready.pop();
                        schedule.stages.back().push_back(u);
                        schedule.steps.push_back(symbolic_schedule_step{u, stage, true});
                        for (auto eid : graph.out_edges(u)) {
                            const auto& edge = graph.get_edge(eid);
                            auto v = edge.to;
                            if (indegree[v] > 0) {
                                --indegree[v];
                                if (indegree[v] == 0) {
                                    ready.push(v);
                                }
                            }
                        }
                    }
                    ++stage;
                }

                if (schedule.steps.size() != graph.node_count()) {
                    schedule.is_topological = false;
                    schedule.steps.clear();
                    schedule.stages.clear();
                    std::size_t fallback_stage = 0;
                    for (auto node : analysis.canonical_order) {
                        schedule.steps.push_back(symbolic_schedule_step{node, fallback_stage, true});
                        schedule.stages.push_back({node});
                        ++fallback_stage;
                    }
                }

                return schedule;
            }

            template <class Predicate>
            static std::size_t prune_dependencies(GraphT& graph, Predicate&& should_prune) {
                std::vector<litegraph::EdgeId> to_remove;
                for (const auto& [eid, edge] : graph.edges()) {
                    if (should_prune(edge.from, edge.to, edge.data)) {
                        to_remove.push_back(litegraph::EdgeId{eid});
                    }
                }
                for (auto eid : to_remove) {
                    graph.remove_edge(eid);
                }
                if (!to_remove.empty()) {
                    graph.compact();
                }
                return to_remove.size();
            }

            static symbolic_simplification_result<GraphT> simplify(GraphT graph,
                                                                   const std::vector<litegraph::NodeId>& roots = {}) {
                symbolic_simplification_result<GraphT> out{std::move(graph)};
                out.removed_nodes = dead_node_elimination(out.graph, roots);
                out.passes.push_back("dead_node_elimination");

                out.merged_subtrees = shared_subtree_extraction(out.graph);
                out.passes.push_back("shared_subtree_extraction");

                out.removed_dependencies = dependency_simplification(out.graph);
                out.passes.push_back("dependency_simplification");
                return out;
            }

            template <class ResultT, class Evaluator>
            static ResultT lazy_evaluate(
                const GraphT& graph,
                litegraph::NodeId root,
                Evaluator&& evaluator,
                std::unordered_map<litegraph::NodeId, ResultT>& cache
            ) {
                if (auto it = cache.find(root); it != cache.end()) {
                    return it->second;
                }

                std::vector<ResultT> inputs;
                if constexpr (std::is_same_v<typename GraphT::directed_tag, litegraph::Directed>) {
                    for (auto eid : graph.in_edges(root)) {
                        const auto& edge = graph.get_edge(eid);
                        inputs.push_back(lazy_evaluate<ResultT>(graph, edge.from, evaluator, cache));
                    }
                }

                ResultT value = evaluator(graph, root, inputs);
                cache.emplace(root, value);
                return value;
            }

            template <class ResultT, class Evaluator>
            static ResultT lazy_evaluate(
                const GraphT& graph,
                litegraph::NodeId root,
                Evaluator&& evaluator
            ) {
                std::unordered_map<litegraph::NodeId, ResultT> cache;
                return lazy_evaluate<ResultT>(graph, root, std::forward<Evaluator>(evaluator), cache);
            }

            static symbolic_dataflow_ir<GraphT>
            build_ir(GraphT graph, const std::vector<litegraph::NodeId>& roots = {}) {
                symbolic_dataflow_ir<GraphT> out;
                out.graph = std::move(graph);
                out.analysis = analyze_graph_ir(out.graph, roots);
                out.schedule = build_schedule(out.graph);

                out.region.region_id = 1;
                out.region.nodes = out.analysis.canonical_order;
                out.region.entry_nodes.clear();
                out.region.exit_nodes.clear();

                for (const auto& [nid, node] : out.graph.nodes()) {
                    const auto id = litegraph::NodeId{nid};
                    if constexpr (std::is_same_v<typename GraphT::directed_tag, litegraph::Directed>) {
                        if (out.graph.in_degree(id) == 0) {
                            out.region.entry_nodes.push_back(id);
                        }
                        if (out.graph.out_degree(id) == 0) {
                            out.region.exit_nodes.push_back(id);
                        }
                    }
                }

                if (!roots.empty()) {
                    out.region.exit_nodes = roots;
                }
                return out;
            }
        };

        namespace detail {
            template <class NodePayload>
            structural_hash_t node_structural_hash(const NodePayload& payload) {
                if constexpr (requires { payload.structural_hash; }) {
                    return payload.structural_hash;
                }
                else {
                    return std::hash<NodePayload>{}(payload);
                }
            }

            template <class EdgePayload>
            structural_hash_t edge_structural_hash(const EdgePayload& payload) {
                if constexpr (requires { payload.structural_hash; }) {
                    return payload.structural_hash;
                }
                else {
                    return std::hash<EdgePayload>{}(payload);
                }
            }

            template <litegraph::LiteGraphModel GraphT>
            std::vector<litegraph::NodeId> infer_roots(const GraphT& graph) {
                std::vector<litegraph::NodeId> roots;
                if constexpr (std::is_same_v<typename GraphT::directed_tag, litegraph::Directed>) {
                    for (const auto& [nid, node] : graph.nodes()) {
                        auto node_id = litegraph::NodeId{nid};
                        if (graph.out_degree(node_id) == 0) {
                            roots.push_back(node_id);
                        }
                    }
                }
                if (roots.empty()) {
                    for (const auto& [nid, node] : graph.nodes()) {
                        roots.push_back(litegraph::NodeId{nid});
                    }
                }
                return roots;
            }
        } // namespace detail

        template <litegraph::LiteGraphModel GraphT>
        graph_analysis_result_t<GraphT> analyze_graph_ir(
            const GraphT& graph,
            const std::vector<litegraph::NodeId>& roots
        ) {
            graph_analysis_result_t<GraphT> out;
            out.node_count = graph.node_count();
            out.edge_count = graph.edge_count();

            std::unordered_map<structural_hash_t, std::size_t> node_hash_counts;
            for (const auto& [nid, node] : graph.nodes()) {
                const auto hash = detail::node_structural_hash(node.data);
                ++node_hash_counts[hash];
                const auto node_id = litegraph::NodeId{nid};
                if (graph.out_degree(node_id) > 1) {
                    ++out.reusable_node_count;
                    out.shared_subtree_count += graph.out_degree(node_id) - 1;
                }
            }
            for (const auto& [hash, count] : node_hash_counts) {
                if (count > 1) {
                    out.reusable_node_count += 1;
                    out.shared_subtree_count += count - 1;
                }
            }

            std::unordered_set<std::uint64_t> seen_dependencies;
            for (const auto& [eid, edge] : graph.edges()) {
                const auto dep_hash = detail::edge_structural_hash(edge.data);
                const std::uint64_t key = (std::uint64_t(edge.from.value) << 32) |
                    (std::uint64_t(edge.to.value) & 0xFFFFFFFFULL) ^
                    (dep_hash * 0x9e3779b97f4a7c15ULL);
                if (!seen_dependencies.insert(key).second) {
                    ++out.duplicate_dependency_count;
                }
            }

            if constexpr (std::is_same_v<typename GraphT::directed_tag, litegraph::Directed>) {
                std::vector<std::size_t> indegree(graph.node_capacity(), 0);
                for (const auto& [eid, edge] : graph.edges()) {
                    if (graph.valid_node(edge.to)) {
                        ++indegree[edge.to.value];
                    }
                }

                std::queue<litegraph::NodeId> ready;
                for (const auto& [nid, node] : graph.nodes()) {
                    if (indegree[nid] == 0) {
                        ready.push(litegraph::NodeId{nid});
                    }
                }

                while (!ready.empty()) {
                    auto u = ready.front();
                    ready.pop();
                    out.topological_order.push_back(u);
                    for (auto eid : graph.out_edges(u)) {
                        const auto& edge = graph.get_edge(eid);
                        auto v = edge.to;
                        if (indegree[v.value] > 0) {
                            --indegree[v.value];
                            if (indegree[v.value] == 0) {
                                ready.push(v);
                            }
                        }
                    }
                }

                out.is_dag = (out.topological_order.size() == graph.node_count());
            }
            else {
                for (const auto& [nid, node] : graph.nodes()) {
                    out.topological_order.push_back(litegraph::NodeId{nid});
                }
            }

            if (out.topological_order.empty()) {
                for (const auto& [nid, node] : graph.nodes()) {
                    out.topological_order.push_back(litegraph::NodeId{nid});
                }
            }

            out.canonical_order = out.topological_order;
            std::ranges::sort(out.canonical_order, [&](litegraph::NodeId a, litegraph::NodeId b) {
                const auto& lhs = graph.node_data(a);
                const auto& rhs = graph.node_data(b);
                const auto lhs_hash = detail::node_structural_hash(lhs);
                const auto rhs_hash = detail::node_structural_hash(rhs);
                if (lhs_hash != rhs_hash) {
                    return lhs_hash < rhs_hash;
                }
                return a.value < b.value;
            });

            (void)roots;
            return out;
        }

        template <litegraph::LiteGraphModel GraphT>
        std::size_t dead_node_elimination(GraphT& graph, const std::vector<litegraph::NodeId>& roots) {
            std::unordered_set<std::size_t> live;
            std::queue<litegraph::NodeId> work;

            auto effective_roots = roots.empty() ? detail::infer_roots(graph) : roots;
            for (auto root : effective_roots) {
                if (graph.valid_node(root) && live.insert(root.value).second) {
                    work.push(root);
                }
            }

            while (!work.empty()) {
                auto node = work.front();
                work.pop();
                if constexpr (std::is_same_v<typename GraphT::directed_tag, litegraph::Directed>) {
                    for (auto eid : graph.in_edges(node)) {
                        const auto& edge = graph.get_edge(eid);
                        if (graph.valid_node(edge.from) && live.insert(edge.from.value).second) {
                            work.push(edge.from);
                        }
                    }
                }
                else {
                    for (auto neighbor : graph.neighbors(node)) {
                        if (graph.valid_node(neighbor) && live.insert(neighbor.value).second) {
                            work.push(neighbor);
                        }
                    }
                }
            }

            std::size_t removed = 0;
            std::vector<litegraph::NodeId> to_remove;
            for (const auto& [nid, node] : graph.nodes()) {
                if (!live.contains(nid)) {
                    to_remove.push_back(litegraph::NodeId{nid});
                }
            }
            for (auto nid : to_remove) {
                graph.remove_node(nid);
                ++removed;
            }
            graph.compact();
            return removed;
        }

        template <litegraph::LiteGraphModel GraphT>
        std::size_t dependency_simplification(GraphT& graph) {
            // Exact structural identity: the key stores the endpoints AND the full edge
            // payload, comparing payloads with operator==. The structural hash is used
            // only for bucket selection, never as identity — so two distinct payloads
            // that collide on structural_hash stay separate edges.
            using edge_payload_t = std::remove_cvref_t<typename GraphT::edge_type>;
            struct edge_key {
                std::uint32_t from;
                std::uint32_t to;
                edge_payload_t payload;

                bool operator==(const edge_key& o) const {
                    return from == o.from && to == o.to && payload == o.payload;
                }
            };
            struct edge_key_hash {
                std::size_t operator()(const edge_key& k) const noexcept {
                    std::size_t h = std::hash<std::uint32_t>{}(k.from);
                    h ^= std::hash<std::uint32_t>{}(k.to) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                    const auto ph = static_cast<std::size_t>(detail::edge_structural_hash(k.payload));
                    h ^= ph + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                    return h;
                }
            };
            std::unordered_set<edge_key, edge_key_hash> seen;
            std::vector<litegraph::EdgeId> duplicate_edges;

            for (const auto& [eid, edge] : graph.edges()) {
                const edge_key key{
                    static_cast<std::uint32_t>(edge.from.value),
                    static_cast<std::uint32_t>(edge.to.value),
                    edge.data
                };
                if (!seen.insert(key).second) {
                    duplicate_edges.push_back(litegraph::EdgeId{eid});
                }
            }

            for (auto eid : duplicate_edges) {
                graph.remove_edge(eid);
            }
            if (!duplicate_edges.empty()) {
                graph.compact();
            }
            return duplicate_edges.size();
        }

        template <litegraph::LiteGraphModel GraphT>
        std::size_t dominator_aware_simplification(GraphT& graph, const std::vector<litegraph::NodeId>& roots = {}) {
            if constexpr (!std::is_same_v<typename GraphT::directed_tag, litegraph::Directed>) {
                (void)graph;
                (void)roots;
                return 0;
            }
            else {
                std::size_t removed = 0;
                for (const auto& [nid, node] : graph.nodes()) {
                    const auto target = litegraph::NodeId{nid};
                    std::unordered_map<structural_hash_t, std::size_t> representative_pred;
                    std::vector<litegraph::EdgeId> redundant;
                    for (auto eid : graph.in_edges(target)) {
                        const auto& edge = graph.get_edge(eid);
                        const auto pred_hash = detail::node_structural_hash(graph.node_data(edge.from));
                        if (auto it = representative_pred.find(pred_hash); it == representative_pred.end()) {
                            representative_pred.emplace(pred_hash, edge.from.value);
                        }
                        else if (it->second != edge.from.value) {
                            redundant.push_back(eid);
                        }
                        else {
                            // Same predecessor and same hash: leave this to dependency_simplification.
                        }
                    }
                    for (auto eid : redundant) {
                        graph.remove_edge(eid);
                        ++removed;
                    }
                }
                if (removed > 0) {
                    graph.compact();
                }
                (void)roots;
                return removed;
            }
        }

        template <litegraph::LiteGraphModel GraphT>
        std::size_t shared_subtree_extraction(GraphT& graph) {
            if constexpr (!std::is_same_v<typename GraphT::directed_tag, litegraph::Directed>) {
                return 0;
            }
            else {
                std::unordered_map<structural_hash_t, litegraph::NodeId> canonical;
                std::vector<std::pair<litegraph::NodeId, litegraph::NodeId>> rewrites;

                for (const auto& [nid, node] : graph.nodes()) {
                    const auto id = litegraph::NodeId{nid};
                    const auto hash = detail::node_structural_hash(node.data);
                    if (auto it = canonical.find(hash); it != canonical.end()) {
                        rewrites.emplace_back(id, it->second);
                    }
                    else {
                        canonical.emplace(hash, id);
                    }
                }

                std::size_t merged = 0;
                for (const auto& [duplicate, keep] : rewrites) {
                    if (!graph.valid_node(duplicate) || !graph.valid_node(keep) || duplicate.value == keep.value) {
                        continue;
                    }

                    for (auto eid : graph.out_edge_ids(duplicate)) {
                        const auto& edge = graph.get_edge(eid);
                        if (graph.valid_node(edge.to) && edge.to.value != keep.value) {
                            graph.add_edge(keep, edge.to, edge.data);
                        }
                    }
                    for (auto eid : graph.in_edge_ids(duplicate)) {
                        const auto& edge = graph.get_edge(eid);
                        if (graph.valid_node(edge.from) && edge.from.value != keep.value) {
                            graph.add_edge(edge.from, keep, edge.data);
                        }
                    }

                    graph.remove_node(duplicate);
                    ++merged;
                }

                if (merged > 0) {
                    dependency_simplification(graph);
                    graph.compact();
                }
                return merged;
            }
        }

        template <litegraph::LiteGraphModel GraphT>
        std::vector<litegraph::NodeId> topological_normalization(const GraphT& graph) {
            auto analysis = analyze_graph_ir(graph);
            if (!analysis.is_dag) {
                return analysis.canonical_order;
            }

            auto normalized = analysis.topological_order;
            std::ranges::stable_sort(normalized, [&](litegraph::NodeId a, litegraph::NodeId b) {
                const auto& lhs = graph.node_data(a);
                const auto& rhs = graph.node_data(b);
                const auto lhs_hash = detail::node_structural_hash(lhs);
                const auto rhs_hash = detail::node_structural_hash(rhs);
                if (lhs_hash != rhs_hash) {
                    return lhs_hash < rhs_hash;
                }
                return a.value < b.value;
            });
            return normalized;
        }

        template <litegraph::LiteGraphModel GraphT>
        graph_optimization_result_t<GraphT> optimize_graph_ir(
            GraphT graph,
            const std::vector<litegraph::NodeId>& roots = {}
        ) {
            graph_optimization_result_t<GraphT> out{std::move(graph)};
            out.before = analyze_graph_ir(out.graph, roots);

            out.removed_dead_nodes = dead_node_elimination(out.graph, roots);
            out.applied_passes.push_back("dead_node_elimination");

            auto normalized = topological_normalization(out.graph);
            out.applied_passes.push_back("topological_normalization");

            out.dominator_simplifications = dominator_aware_simplification(out.graph, roots);
            out.applied_passes.push_back("dominator_aware_simplification");

            out.merged_shared_subtrees = shared_subtree_extraction(out.graph);
            out.applied_passes.push_back("shared_subtree_extraction");

            out.simplified_dependencies = dependency_simplification(out.graph);
            out.applied_passes.push_back("dependency_simplification");

            out.after = analyze_graph_ir(out.graph, roots);
            if (!normalized.empty()) {
                out.after.canonical_order = std::move(normalized);
            }

            return out;
        }

        template <class Expr>
        struct compilation_unit {
            Expr captured;
            semantic::semantic_info semantics;
            structural_hash_t initial_hash = 0;
            frontend_kind frontend = frontend_kind::lithe_edsl;
        };

        template <class Fn>
        struct lowering_pass {
            std::string name;
            lowering_stage stage = lowering_stage::optimization;
            bool enabled = true;
            Fn fn;

            template <class Expr>
            constexpr auto operator()(Expr&& expr) const {
                return fn(std::forward<Expr>(expr));
            }
        };

        template <class Fn>
        constexpr auto make_lowering_pass(std::string name, lowering_stage stage, Fn fn, bool enabled = true) {
            return lowering_pass<Fn>{std::move(name), stage, enabled, std::move(fn)};
        }

        template <class IR>
        struct lowering_artifact {
            IR ir;
            semantic::semantic_info semantics;
            lowering_context context;
            structural_hash_t ir_hash = 0;
        };

        struct lowering_diagnostic {
            enum class level {
                info,
                warning,
                error
            };

            level severity = level::info;
            std::string stage;
            std::string message;
        };

        struct lowering_error {
            std::string stage;
            std::string message;

            [[nodiscard]] lowering_diagnostic to_diagnostic() const {
                return {lowering_diagnostic::level::error, stage, message};
            }
        };

        template <class T>
        struct lowering_result {
            std::optional<T> output;
            lowering_context context;
            std::vector<lowering_diagnostic> diagnostics;

            [[nodiscard]] bool ok() const {
                return output.has_value() &&
                    std::none_of(diagnostics.begin(), diagnostics.end(),
                                 [](const lowering_diagnostic& d) {
                                     return d.severity == lowering_diagnostic::level::error;
                                 });
            }
        };

        template <class Artifact>
        struct backend_result {
            std::optional<Artifact> output;
            compiler_context context;
            std::vector<lowering_diagnostic> diagnostics;

            [[nodiscard]] bool ok() const {
                return output.has_value() &&
                    std::none_of(diagnostics.begin(), diagnostics.end(),
                                 [](const lowering_diagnostic& d) {
                                     return d.severity == lowering_diagnostic::level::error;
                                 });
            }
        };

        inline semantic::backend_profile make_sql_like_profile(std::string backend_name = "sql") {
            semantic::backend_profile profile;
            profile.backend_name = std::move(backend_name);
            profile.allow_mutable_tensor_ops = false;
            profile.allow_filesystem_effects = false;
            profile.allow_high_level_query_ops = true;
            profile.allow_symbolic_only_nodes = true;
            return profile;
        }

        inline semantic::backend_profile make_asmjit_like_profile(std::string backend_name = "asmjit") {
            semantic::backend_profile profile;
            profile.backend_name = std::move(backend_name);
            profile.allow_symbolic_only_nodes = false;
            profile.allow_high_level_query_ops = false;
            profile.allow_mutable_tensor_ops = true;
            profile.allow_filesystem_effects = true;
            return profile;
        }

        namespace detail {
            template <class Tag>
            [[nodiscard]] std::string operation_name() {
                if constexpr (std::is_same_v<Tag, add_tag>) return "add";
                else if constexpr (std::is_same_v<Tag, sub_tag>) return "sub";
                else if constexpr (std::is_same_v<Tag, mul_tag>) return "mul";
                else if constexpr (std::is_same_v<Tag, div_tag>) return "div";
                else if constexpr (std::is_same_v<Tag, mod_tag>) return "mod";
                else if constexpr (std::is_same_v<Tag, neg_tag>) return "neg";
                else if constexpr (std::is_same_v<Tag, call_tag>) return "call";
                else if constexpr (std::is_same_v<Tag, if_tag>) return "if";
                else if constexpr (std::is_same_v<Tag, while_tag>) return "while";
                else if constexpr (std::is_same_v<Tag, for_tag>) return "for";
                else if constexpr (std::is_same_v<Tag, seq_tag>) return "seq";
                else return typeid(Tag).name();
            }

            struct lowering_legality_validator {
                const semantic::capability_registry* registry = nullptr;
                std::string backend_name;
                semantic::backend_profile profile;
                mutable semantic::lowering_legality_result legality;

                template <class T>
                semantic::semantic_info on_terminal(T&& terminal) const {
                    const auto semantic_info = semantic::infer_semantics(terminal);
                    const auto type = semantic::infer_type_descriptor(semantic_info);
                    legality.merge(registry->validate_lowering(
                        backend_name,
                        "terminal",
                        semantic_info,
                        type,
                        profile));
                    return semantic_info;
                }

                template <class Tag, class... ChildInfo>
                semantic::semantic_info on_node(Tag, ChildInfo&&... children) const {
                    auto semantic_info = semantic::detail::semantic_analyzer{}.on_node(Tag{}, children...);
                    const auto type = semantic::infer_type_descriptor(semantic_info);
                    legality.merge(registry->validate_lowering(
                        backend_name,
                        operation_name<Tag>(),
                        semantic_info,
                        type,
                        profile));
                    return semantic_info;
                }
            };
        } // namespace detail

        template <class Expr>
        [[nodiscard]] semantic::lowering_legality_result validate_lowering(
            const Expr& expr,
            const semantic::capability_registry& registry,
            const std::string_view backend_name,
            semantic::backend_profile profile = {}
        ) {
            if (profile.backend_name.empty()) {
                profile.backend_name = std::string{backend_name};
            }
            detail::lowering_legality_validator validator{
                std::addressof(registry),
                std::string{backend_name},
                std::move(profile),
                semantic::lowering_legality_result{true, std::string{backend_name}, {}}
            };
            lithe::visit(expr, validator);
            return validator.legality;
        }

        namespace observability {
            inline constexpr bool enabled_by_default = false;

            struct compilation_event {
                enum class kind : std::uint8_t {
                    started,
                    finished,
                    failed
                };

                kind type = kind::started;
                std::string phase;
                std::uint64_t timestamp_ns = 0;
            };

            struct pass_event {
                std::string pass_name;
                std::size_t pass_index = 0;
                std::uint64_t start_ns = 0;
                std::uint64_t end_ns = 0;
                bool changed = true;
                std::string ir_before_dump;
                std::string ir_after_dump;
                std::string ir_diff;
                structural_hash_t ir_before_hash = 0;
                structural_hash_t ir_after_hash = 0;
                structural_hash_t ir_before_structural_hash = 0;
                structural_hash_t ir_after_structural_hash = 0;
            };

            struct rewrite_event {
                std::string pass_name;
                std::size_t rewrites_attempted = 0;
                std::size_t rewrites_applied = 0;
                std::uint64_t timestamp_ns = 0;
            };

            struct semantic_event {
                std::string stage;
                structural_hash_t expression_hash = 0;
                structural_hash_t semantic_hash = 0;
                std::uint64_t timestamp_ns = 0;
            };

            struct lowering_event {
                lowering_stage stage = lowering_stage::capture;
                std::string detail;
                structural_hash_t input_hash = 0;
                structural_hash_t output_hash = 0;
                std::uint64_t start_ns = 0;
                std::uint64_t end_ns = 0;
            };

            struct structural_hash_event {
                std::string label;
                structural_hash_t expression_hash = 0;
                structural_hash_t structural_hash = 0;
                std::uint64_t timestamp_ns = 0;
            };

            struct backend_lowering_event {
                std::string backend_name;
                bool legal = true;
                std::vector<std::string> reasons;
                std::uint64_t timestamp_ns = 0;
            };

            struct codegen_event {
                std::string stage;
                structural_hash_t ir_hash = 0;
                std::uint64_t timestamp_ns = 0;
            };

            struct codegen_diagnostic_event {
                lowering_diagnostic::level severity = lowering_diagnostic::level::info;
                std::string stage;
                std::string message;
                std::uint64_t timestamp_ns = 0;
            };

            using diagnostic_event = codegen_diagnostic_event;

            struct pass_timing_hook {
                std::string pass_name;
                std::uint64_t duration_ns = 0;
            };

            struct structural_hash_statistics {
                structural_hash_t input_hash = 0;
                structural_hash_t output_hash = 0;
                bool equivalent = false;
            };

            struct rewrite_statistics {
                std::size_t rewrites_attempted = 0;
                std::size_t rewrites_applied = 0;
            };

            struct compile_trace {
                std::vector<compilation_event> compilation_events;
                std::vector<pass_event> pass_events;
                std::vector<rewrite_event> rewrite_events;
                std::vector<semantic_event> semantic_events;
                std::vector<lowering_event> lowering_events;
                std::vector<structural_hash_event> structural_hash_events;
                std::vector<backend_lowering_event> backend_lowering_events;
                std::vector<codegen_event> codegen_events;
                std::vector<codegen_diagnostic_event> diagnostic_events;
                std::vector<pass_timing_hook> pass_timings;
                structural_hash_statistics hash_stats{};
                rewrite_statistics rewrite_stats{};
            };

            [[nodiscard]] inline std::uint64_t now_ns() noexcept {
#if LITHE_HAS_NADI
                return utils::nadi::SteadyClockPolicy::now();
#else
                return static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
            }

            struct null_observer {
                template <class Event>
                constexpr void on_event(const Event&) const noexcept {}
            };

            struct trace_observer {
                compile_trace trace;

                void on_event(const compilation_event& event) {
                    trace.compilation_events.push_back(event);
                }

                void on_event(const pass_event& event) {
                    trace.pass_events.push_back(event);
                    trace.pass_timings.push_back(pass_timing_hook{
                        event.pass_name,
                        event.end_ns >= event.start_ns ? event.end_ns - event.start_ns : 0
                    });
                }

                void on_event(const rewrite_event& event) {
                    trace.rewrite_events.push_back(event);
                    trace.rewrite_stats.rewrites_attempted += event.rewrites_attempted;
                    trace.rewrite_stats.rewrites_applied += event.rewrites_applied;
                }

                void on_event(const semantic_event& event) {
                    trace.semantic_events.push_back(event);
                }

                void on_event(const lowering_event& event) {
                    trace.lowering_events.push_back(event);
                    trace.hash_stats.input_hash = event.input_hash;
                    trace.hash_stats.output_hash = event.output_hash;
                    trace.hash_stats.equivalent = (event.input_hash == event.output_hash);
                }

                void on_event(const structural_hash_event& event) {
                    trace.structural_hash_events.push_back(event);
                }

                void on_event(const backend_lowering_event& event) {
                    trace.backend_lowering_events.push_back(event);
                }

                void on_event(const codegen_event& event) {
                    trace.codegen_events.push_back(event);
                }

                void on_event(const codegen_diagnostic_event& event) {
                    trace.diagnostic_events.push_back(event);
                }
            };

            [[nodiscard]] inline std::string diff_ir(const std::string_view before, const std::string_view after) {
                if (before == after) {
                    return "no-change";
                }
                return "changed bytes=" + std::to_string(before.size()) + "->" + std::to_string(after.size());
            }

            [[nodiscard]] inline structural_hash_t hash_text(const std::string_view text) {
                return std::hash<std::string_view>{}(text);
            }

            template <class Expr>
            [[nodiscard]] std::string dump_ir(const Expr& expr) {
                if constexpr (requires { emit::dump(expr); }) {
                    return emit::dump(expr);
                }
                else {
                    return "<ir-dump-unavailable>";
                }
            }

            template <bool Enabled, class Observer, class Event>
            constexpr void emit(Observer& observer, Event event) {
                if constexpr (Enabled) {
                    if constexpr (requires(Observer obs, Event e) { obs.on_event(e); }) {
                        observer.on_event(std::move(event));
                    }
                }
                else {
                    (void)observer;
                    (void)event;
                }
            }
        } // namespace observability

        template <class... Passes>
        struct lowering_pipeline {
            std::tuple<Passes...> pass_bundle;
            compiler::opt_level preset = compiler::opt_level::O0;
            bool use_preset = true;
            bool use_semantic_propagation = true;

            template <class Expr>
            constexpr auto run(Expr&& expr, lowering_context& ctx) const {
                auto input = std::forward<Expr>(expr);

                ctx.pass_names.emplace_back("capture");
                if (use_semantic_propagation) {
                    [[maybe_unused]] auto propagated = semantic::propagate_semantics(input);
                    ctx.pass_names.emplace_back("semantic_propagation");
                }

                auto apply_passes = [&](auto&& normalized) {
                    if constexpr (sizeof...(Passes) > 0) {
                        auto out = std::apply(
                            [&](const auto&... passes) {
                                return compiler::compile(std::forward<decltype(normalized)>(normalized), passes...);
                            },
                            pass_bundle);
                        ctx.pass_names.emplace_back("optimization");
                        ctx.passes_applied = sizeof...(Passes);
                        return out;
                    }
                    else {
                        ctx.passes_applied = 0;
                        return std::forward<decltype(normalized)>(normalized);
                    }
                };

                if (use_preset) {
                    ctx.pass_names.emplace_back("normalization");
                }
                auto normalized = compiler::optimize_preset(std::move(input),
                                                            use_preset ? preset : compiler::opt_level::O0);
                return apply_passes(std::move(normalized));
            }

            template <class Expr>
            constexpr auto run(Expr&& expr, lowering_context& ctx, compiler::pass_context& pass_ctx) const {
                auto input = std::forward<Expr>(expr);

                pass_ctx.begin_pass("capture");
                ctx.pass_names.emplace_back("capture");
                pass_ctx.end_pass(false);

                if (use_semantic_propagation) {
                    pass_ctx.begin_pass("semantic_propagation");
                    [[maybe_unused]] auto propagated = semantic::propagate_semantics(input);
                    ctx.pass_names.emplace_back("semantic_propagation");
                    pass_ctx.end_pass(true);
                }

                auto apply_passes = [&](auto&& normalized) {
                    if constexpr (sizeof...(Passes) > 0) {
                        pass_ctx.begin_pass("optimization");
                        auto out = std::apply(
                            [&](const auto&... passes) {
                                return compiler::compile(std::forward<decltype(normalized)>(normalized), pass_ctx,
                                                         passes...);
                            },
                            pass_bundle);
                        ctx.pass_names.emplace_back("optimization");
                        pass_ctx.end_pass(true);
                        ctx.passes_applied = sizeof...(Passes);
                        return out;
                    }
                    else {
                        ctx.passes_applied = 0;
                        return std::forward<decltype(normalized)>(normalized);
                    }
                };

                if (use_preset) {
                    pass_ctx.begin_pass("normalization");
                    ctx.pass_names.emplace_back("normalization");
                    pass_ctx.end_pass(false);
                }
                auto normalized = compiler::optimize_preset(std::move(input),
                                                            use_preset ? preset : compiler::opt_level::O0);
                return apply_passes(std::move(normalized));
            }

            template <bool ObservabilityEnabled = observability::enabled_by_default,
                      class Observer = observability::null_observer,
                      class Expr>
            constexpr auto run_observed(Expr&& expr, lowering_context& ctx, Observer& observer) const {
#if LITHE_HAS_THREAD_LOCAL_SINK
                utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.lower.run"> _nadi_run{};
#endif
                auto input = std::forward<Expr>(expr);

                auto emit_pass = [&](std::string name,
                                     const std::size_t index,
                                     const std::uint64_t start_ns,
                                     const std::uint64_t end_ns,
                                     const bool changed,
                                     std::string before_dump,
                                     std::string after_dump,
                                     const structural_hash_t before_structural,
                                     const structural_hash_t after_structural) {
                    const auto ir_diff = observability::diff_ir(before_dump, after_dump);
                    const auto before_hash = observability::hash_text(before_dump);
                    const auto after_hash = observability::hash_text(after_dump);
                    observability::emit<ObservabilityEnabled>(observer, observability::pass_event{
                                                                  std::move(name),
                                                                  index,
                                                                  start_ns,
                                                                  end_ns,
                                                                  changed,
                                                                  std::move(before_dump),
                                                                  std::move(after_dump),
                                                                  ir_diff,
                                                                  before_hash,
                                                                  after_hash,
                                                                  before_structural,
                                                                  after_structural
                                                              });
                };

                auto semantic_hash = [](const semantic::semantic_info& info) {
                    std::size_t seed = std::hash<int>{}(static_cast<int>(info.effect));
                    seed = emit::hash_combine(seed, std::hash<int>{}(static_cast<int>(info.domain)));
                    seed = emit::hash_combine(seed, std::hash<int>{}(static_cast<int>(info.purity_level)));
                    seed = emit::hash_combine(seed, std::hash<int>{}(static_cast<int>(info.evaluation)));
                    return seed;
                };

                std::size_t pass_idx = 0;
                const auto capture_start = observability::now_ns();
                ctx.pass_names.emplace_back("capture");
                const auto capture_end = observability::now_ns();
                emit_pass(
                    "capture",
                    pass_idx++,
                    capture_start,
                    capture_end,
                    false,
                    observability::dump_ir(input),
                    observability::dump_ir(input),
                    structural_hash(input),
                    structural_hash(input));

                if (use_semantic_propagation) {
                    const auto sem_start = observability::now_ns();
                    auto propagated = semantic::propagate_semantics(input);
                    const auto sem_end = observability::now_ns();
                    ctx.pass_names.emplace_back("semantic_propagation");
                    emit_pass(
                        "semantic_propagation",
                        pass_idx++,
                        sem_start,
                        sem_end,
                        false,
                        observability::dump_ir(input),
                        observability::dump_ir(input),
                        structural_hash(input),
                        structural_hash(input));
                    observability::emit<ObservabilityEnabled>(observer, observability::semantic_event{
                                                                  "semantic_propagation",
                                                                  structural_hash(input),
                                                                  semantic_hash(propagated),
                                                                  sem_end
                                                              });
                }

                auto apply_passes_observed = [&](auto&& normalized) {
                    if constexpr (sizeof...(Passes) > 0) {
                        const auto opt_start = observability::now_ns();
#if LITHE_HAS_PROFILER
                        profiler::ScopedProfiler _opt_prof{"lithe.lower.optimization"};
#endif
                        auto out = std::apply(
                            [&](const auto&... passes) {
                                return compiler::compile(std::forward<decltype(normalized)>(normalized), passes...);
                            },
                            pass_bundle);
                        const auto opt_end = observability::now_ns();
                        ctx.pass_names.emplace_back("optimization");
                        emit_pass(
                            "optimization",
                            pass_idx++,
                            opt_start,
                            opt_end,
                            true,
                            observability::dump_ir(normalized),
                            observability::dump_ir(out),
                            structural_hash(normalized),
                            structural_hash(out));
                        observability::emit<ObservabilityEnabled>(observer, observability::structural_hash_event{
                                                                      "optimization",
                                                                      observability::hash_text(
                                                                          observability::dump_ir(out)),
                                                                      structural_hash(out),
                                                                      opt_end
                                                                  });
                        ctx.passes_applied = sizeof...(Passes);
                        return out;
                    }
                    else {
                        ctx.passes_applied = 0;
                        return std::forward<decltype(normalized)>(normalized);
                    }
                };

                if (use_preset) {
                    const auto norm_start = observability::now_ns();
                    const auto before_norm_ir = observability::dump_ir(input);
                    const auto before_norm_hash = structural_hash(input);
                    auto normalized = compiler::optimize_preset(std::move(input), preset);
                    ctx.pass_names.emplace_back("normalization");
                    const auto norm_end = observability::now_ns();
                    emit_pass(
                        "normalization",
                        pass_idx++,
                        norm_start,
                        norm_end,
                        true,
                        before_norm_ir,
                        observability::dump_ir(normalized),
                        before_norm_hash,
                        structural_hash(normalized));
                    return apply_passes_observed(std::move(normalized));
                }
                auto pass_input = compiler::optimize_preset(std::move(input), compiler::opt_level::O0);
                return apply_passes_observed(std::move(pass_input));
            }
        };

        template <class... Passes>
        constexpr auto make_pipeline(compiler::opt_level preset, Passes... passes) {
            return lowering_pipeline<Passes...>{
                std::tuple < Passes...>{std::move(passes)...},
                preset,
                true,
                true
            };
        }

        template <class... Passes>
        constexpr auto make_pipeline_no_preset(Passes... passes) {
            return lowering_pipeline<Passes...>{
                std::tuple < Passes...>{std::move(passes)...},
                compiler::opt_level::O0,
                false,
                true
            };
        }

        template <class Backend>
        concept BackendModuleProtocol = requires(Backend backend, lowering_context& ctx, std::string_view op_name) {
            backend.begin_module(ctx);
            { backend.emit_terminal(0) };
            { backend.emit_operation(op_name) } -> std::same_as<decltype(backend.emit_terminal(0))>;
            backend.connect(backend.emit_operation(op_name), backend.emit_terminal(0));
            backend.end_module();
        };

        template <class Backend, class Expr>
        concept ExpressionLoweringBackend = BackendModuleProtocol<Backend> && requires(
            Backend backend,
            Expr&& expr,
            lowering_context& ctx) {
                backend.lower(std::forward<Expr>(expr), ctx);
            };

        template <class Backend, class Expr>
        concept GraphLoweringBackend = ExpressionLoweringBackend<Backend, Expr>;

        template <class Backend, class Expr>
        concept TreeLoweringBackend = ExpressionLoweringBackend<Backend, Expr> && requires(
            Backend backend,
            Expr&& expr,
            lowering_context& ctx) {
                {
                    backend.lower(std::forward<Expr>(expr), ctx).size()
                } -> std::convertible_to<std::size_t>;
                backend.lower(std::forward<Expr>(expr), ctx).get_root();
            };

        template <class Backend, class Expr>
        concept DataflowLoweringBackend = ExpressionLoweringBackend<Backend, Expr>;

        template <class Artifact>
        concept CodegenArtifact =
            std::constructible_from<std::string_view, Artifact> ||
            requires(Artifact artifact) {
                artifact.data();
                { artifact.size() } -> std::convertible_to<std::size_t>;
            };

        template <class Backend, class Expr>
        concept CodegenBackend = ExpressionLoweringBackend<Backend, Expr> && requires(
            Backend backend,
            Expr&& expr,
            lowering_context& ctx) {
                requires CodegenArtifact<std::decay_t<decltype(backend.lower(
                    std::forward<Expr>(expr), ctx))>>;
            };

        template <class Backend, class Expr>
        concept LoweringBackend = ExpressionLoweringBackend<Backend, Expr>;

        struct backend_protocol_state {
            std::size_t next_handle = 1;

            void reset() {
                next_handle = 1;
            }

            std::size_t allocate() {
                return next_handle++;
            }
        };

        struct graph_backend {
            using handle_type = std::size_t;
            mutable backend_protocol_state protocol_state;

            void begin_module(lowering_context&) const {
                protocol_state.reset();
            }

            template <class T>
            handle_type emit_terminal(T&&) const {
                return protocol_state.allocate();
            }

            handle_type emit_operation(std::string_view) const {
                return protocol_state.allocate();
            }

            void connect(handle_type, handle_type) const {}

            void end_module() const {}

            template <class Expr>
            auto lower(Expr&& expr, lowering_context& ctx) const {
                begin_module(ctx);
                auto out = graph::build_dag(std::forward<Expr>(expr));
                end_module();
                return out;
            }
        };

        struct tree_backend {
            using handle_type = std::size_t;
            mutable backend_protocol_state protocol_state;

            void begin_module(lowering_context&) const {
                protocol_state.reset();
            }

            template <class T>
            handle_type emit_terminal(T&&) const {
                return protocol_state.allocate();
            }

            handle_type emit_operation(std::string_view) const {
                return protocol_state.allocate();
            }

            void connect(handle_type, handle_type) const {}

            void end_module() const {}

            template <class Expr>
            auto lower(Expr&& expr, lowering_context& ctx) const {
                begin_module(ctx);
                backend::ast_builder builder;
                visit(std::forward<Expr>(expr), builder);
                builder.finalize();
                auto out = std::move(builder.tree);
                end_module();
                return out;
            }
        };

        struct dataflow_backend {
            using handle_type = std::size_t;
            mutable backend_protocol_state protocol_state;

            void begin_module(lowering_context&) const {
                protocol_state.reset();
            }

            template <class T>
            handle_type emit_terminal(T&&) const {
                return protocol_state.allocate();
            }

            handle_type emit_operation(std::string_view) const {
                return protocol_state.allocate();
            }

            void connect(handle_type, handle_type) const {}

            void end_module() const {}

            template <class Expr>
            auto lower(Expr&& expr, lowering_context& ctx) const {
                begin_module(ctx);
                backend::symbolic_dag_builder builder;
                visit(std::forward<Expr>(expr), builder);
                auto out = std::move(builder.dag);
                end_module();
                return out;
            }
        };

        template <class Expr, class Pipeline, class Backend>
            requires requires(Pipeline pipeline, Expr&& e, lowering_context& ctx) {
                pipeline.run(std::forward<Expr>(e), ctx);
            } && ExpressionLoweringBackend<Backend, Expr>
        constexpr auto compile(Expr&& expr, const Pipeline& pipeline, Backend backend) {
            lowering_context ctx;
            ctx.input_hash = structural_hash(expr);
            ctx.pass_names.emplace_back("capture");

            using lowered_input_t = decltype(pipeline.run(std::forward<Expr>(expr), ctx));
            using output_t = decltype(backend.lower(std::declval<lowered_input_t&>(), ctx));
            lowering_result<output_t> result;

            auto do_lower = [&]() -> std::expected<output_t, lowering_error> {
                try {
                    auto lowered = pipeline.run(std::forward<Expr>(expr), ctx);
                    ctx.output_hash = structural_hash(lowered);
                    ctx.equivalent = (ctx.input_hash == ctx.output_hash);
                    ctx.pass_names.emplace_back("graph_lowering");
                    ctx.pass_names.emplace_back("backend_lowering");
                    ctx.pass_names.emplace_back("codegen_preparation");
                    return backend.lower(lowered, ctx);
                }
                catch (const std::exception& ex) {
                    return std::unexpected(lowering_error{"lowering", ex.what()});
                }
                catch (...) {
                    return std::unexpected(lowering_error{"lowering", "unknown lowering failure"});
                }
            };

            if (auto lowered = do_lower()) {
                result.output = std::move(*lowered);
                result.context = std::move(ctx);
            }
            else {
                result.context = std::move(ctx);
                result.diagnostics.push_back(lowered.error().to_diagnostic());
            }

            return result;
        }

        template <class Expr, class Pipeline, class Backend>
            requires requires(Pipeline pipeline, Expr&& e, lowering_context& ctx) {
                pipeline.run(std::forward<Expr>(e), ctx);
            } && ExpressionLoweringBackend<Backend, Expr>
        constexpr auto compile(Expr&& expr, const Pipeline& pipeline, Backend backend,
                               compiler::pass_context& pass_ctx) {
            lowering_context ctx;
            ctx.input_hash = structural_hash(expr);
            ctx.pass_names.emplace_back("capture");

            using lowered_input_t = decltype(pipeline.run(std::forward<Expr>(expr), ctx));
            using output_t = decltype(backend.lower(std::declval<lowered_input_t&>(), ctx));
            lowering_result<output_t> result;

            auto do_lower = [&]() -> std::expected<output_t, lowering_error> {
                try {
                    auto lowered = [&]() {
                        if constexpr (requires { pipeline.run(std::forward<Expr>(expr), ctx, pass_ctx); }) {
                            return pipeline.run(std::forward<Expr>(expr), ctx, pass_ctx);
                        }
                        else {
                            return pipeline.run(std::forward<Expr>(expr), ctx);
                        }
                    }();
                    ctx.output_hash = structural_hash(lowered);
                    ctx.equivalent = (ctx.input_hash == ctx.output_hash);

                    pass_ctx.begin_pass("backend_lowering");
                    ctx.pass_names.emplace_back("graph_lowering");
                    ctx.pass_names.emplace_back("backend_lowering");
                    ctx.pass_names.emplace_back("codegen_preparation");
                    auto out = backend.lower(lowered, ctx);
                    pass_ctx.end_pass(true);
                    return out;
                }
                catch (const std::exception& ex) {
                    return std::unexpected(lowering_error{"lowering", ex.what()});
                }
                catch (...) {
                    return std::unexpected(lowering_error{"lowering", "unknown lowering failure"});
                }
            };

            if (auto lowered = do_lower()) {
                result.output = std::move(*lowered);
                result.context = std::move(ctx);
            }
            else {
                result.context = std::move(ctx);
                result.diagnostics.push_back(lowered.error().to_diagnostic());
                pass_ctx.emit_diagnostic(
                    compiler::diagnostic_level::error,
                    compiler::diagnostic_code::lowering_failed,
                    lowered.error().message);
            }

            return result;
        }

        template <class Expr, class Pipeline>
            requires requires(Pipeline pipeline, Expr&& e, lowering_context& ctx) {
                pipeline.run(std::forward<Expr>(e), ctx);
            }
        constexpr auto compile(Expr&& expr, const Pipeline& pipeline) {
            compiler_context ctx;

            using lowered_t = decltype(pipeline.run(std::forward<Expr>(expr), ctx.legacy));
            using artifact_t = lowering_artifact<std::decay_t<lowered_t>>;
            backend_result<artifact_t> result;

            ctx.current_stage = lowering_stage::capture;
            ctx.stage_trace.push_back(ctx.current_stage);
            ctx.legacy.input_hash = structural_hash(expr);

            try {
                auto lowered = pipeline.run(std::forward<Expr>(expr), ctx.legacy);
                ctx.current_stage = lowering_stage::codegen_preparation;
                ctx.stage_trace.push_back(ctx.current_stage);
                ctx.legacy.output_hash = structural_hash(lowered);
                ctx.legacy.equivalent = (ctx.legacy.input_hash == ctx.legacy.output_hash);

                artifact_t artifact{
                    std::move(lowered),
                    semantic::infer_semantics(expr),
                    ctx.legacy,
                    ctx.legacy.output_hash
                };
                result.output = std::move(artifact);
                result.context = std::move(ctx);
            }
            catch (const std::exception& ex) {
                result.context = std::move(ctx);
                result.diagnostics.push_back(lowering_error{"compile", ex.what()}.to_diagnostic());
            }
            catch (...) {
                result.context = std::move(ctx);
                result.diagnostics.push_back(
                    lowering_error{"compile", "unknown compile pipeline failure"}.to_diagnostic());
            }

            return result;
        }

        template <class Expr, class Pipeline>
            requires requires(Pipeline pipeline, Expr&& e, lowering_context& ctx) {
                pipeline.run(std::forward<Expr>(e), ctx);
            }
        constexpr auto compile(Expr&& expr, const Pipeline& pipeline, compiler::pass_context& pass_ctx) {
            compiler_context ctx;

            using lowered_t = decltype(pipeline.run(std::forward<Expr>(expr), ctx.legacy));
            using artifact_t = lowering_artifact<std::decay_t<lowered_t>>;
            backend_result<artifact_t> result;

            ctx.current_stage = lowering_stage::capture;
            ctx.stage_trace.push_back(ctx.current_stage);
            ctx.legacy.input_hash = structural_hash(expr);

            try {
                auto lowered = [&]() {
                    if constexpr (requires { pipeline.run(std::forward<Expr>(expr), ctx.legacy, pass_ctx); }) {
                        return pipeline.run(std::forward<Expr>(expr), ctx.legacy, pass_ctx);
                    }
                    else {
                        return pipeline.run(std::forward<Expr>(expr), ctx.legacy);
                    }
                }();

                ctx.current_stage = lowering_stage::codegen_preparation;
                ctx.stage_trace.push_back(ctx.current_stage);
                ctx.legacy.output_hash = structural_hash(lowered);
                ctx.legacy.equivalent = (ctx.legacy.input_hash == ctx.legacy.output_hash);

                auto inferred = semantic::infer_semantics(expr);
                if constexpr (requires { lithe::structural_key(expr); }) {
                    pass_ctx.semantic_registry().merge(lithe::structural_key(expr), inferred);
                }

                artifact_t artifact{
                    std::move(lowered),
                    std::move(inferred),
                    ctx.legacy,
                    ctx.legacy.output_hash
                };
                result.output = std::move(artifact);
                result.context = std::move(ctx);
            }
            catch (const std::exception& ex) {
                const lowering_error err{"compile", ex.what()};
                result.context = std::move(ctx);
                result.diagnostics.push_back(err.to_diagnostic());
                pass_ctx.emit_diagnostic(
                    compiler::diagnostic_level::error,
                    compiler::diagnostic_code::lowering_failed,
                    err.message);
            }
            catch (...) {
                const lowering_error err{"compile", "unknown compile pipeline failure"};
                result.context = std::move(ctx);
                result.diagnostics.push_back(err.to_diagnostic());
                pass_ctx.emit_diagnostic(
                    compiler::diagnostic_level::error,
                    compiler::diagnostic_code::lowering_failed,
                    err.message);
            }

            return result;
        }

        template <bool ObservabilityEnabled = observability::enabled_by_default, class Expr, class Pipeline, class
                  Backend,
                  class Observer = observability::null_observer>
            requires requires(Pipeline pipeline, Expr&& e, lowering_context& ctx) {
                pipeline.run(std::forward<Expr>(e), ctx);
            } && ExpressionLoweringBackend<Backend, Expr>
        constexpr auto compile_observed(Expr&& expr, const Pipeline& pipeline, Backend backend, Observer& observer) {
            lowering_context ctx;
#if LITHE_HAS_THREAD_LOCAL_SINK
            utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.lower.compile"> _nadi_compile{};
#endif
            ctx.input_hash = structural_hash(expr);
            ctx.pass_names.emplace_back("capture");

            observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                          observability::compilation_event::kind::started,
                                                          "compile",
                                                          observability::now_ns()
                                                      });

            using lowered_input_t = decltype(pipeline.run(std::forward<Expr>(expr), ctx));
            using output_t = decltype(backend.lower(std::declval<lowered_input_t&>(), ctx));
            lowering_result<output_t> result;

            const auto stage_start = observability::now_ns();
            try {
                auto lowered = pipeline.template run_observed<ObservabilityEnabled>(
                    std::forward<Expr>(expr), ctx, observer);
                ctx.output_hash = structural_hash(lowered);
                ctx.equivalent = (ctx.input_hash == ctx.output_hash);

                ctx.pass_names.emplace_back("graph_lowering");
                ctx.pass_names.emplace_back("backend_lowering");
                ctx.pass_names.emplace_back("codegen_preparation");
                result.output = backend.lower(lowered, ctx);
                result.context = std::move(ctx);

                observability::emit<ObservabilityEnabled>(observer, observability::lowering_event{
                                                              lowering_stage::codegen_preparation,
                                                              "lowering_backend_complete",
                                                              result.context.input_hash,
                                                              result.context.output_hash,
                                                              stage_start,
                                                              observability::now_ns()
                                                          });
                observability::emit<ObservabilityEnabled>(observer, observability::backend_lowering_event{
                                                              "backend",
                                                              true,
                                                              {},
                                                              observability::now_ns()
                                                          });
                observability::emit<ObservabilityEnabled>(observer, observability::codegen_event{
                                                              "codegen_preparation",
                                                              observability::hash_text(
                                                                  observability::dump_ir(result.output.value())),
                                                              observability::now_ns()
                                                          });

                observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                              observability::compilation_event::kind::finished,
                                                              "compile",
                                                              observability::now_ns()
                                                          });
            }
            catch (const std::exception& ex) {
                result.context = std::move(ctx);
                result.diagnostics.push_back(lowering_error{"lowering", ex.what()}.to_diagnostic());
                observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                              observability::compilation_event::kind::failed,
                                                              "compile",
                                                              observability::now_ns()
                                                          });
            }
            catch (...) {
                result.context = std::move(ctx);
                result.diagnostics.push_back(lowering_error{"lowering", "unknown lowering failure"}.to_diagnostic());
                observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                              observability::compilation_event::kind::failed,
                                                              "compile",
                                                              observability::now_ns()
                                                          });
            }

            if constexpr (ObservabilityEnabled) {
                for (const auto& diagnostic : result.diagnostics) {
                    observability::emit<ObservabilityEnabled>(observer, observability::codegen_diagnostic_event{
                                                                  diagnostic.severity,
                                                                  diagnostic.stage,
                                                                  diagnostic.message,
                                                                  observability::now_ns()
                                                              });
                }
            }

            return result;
        }

        template <bool ObservabilityEnabled = observability::enabled_by_default, class Expr, class Pipeline,
                  class Observer = observability::null_observer>
            requires requires(Pipeline pipeline, Expr&& e, lowering_context& ctx) {
                pipeline.run(std::forward<Expr>(e), ctx);
            }
        constexpr auto compile_observed(Expr&& expr, const Pipeline& pipeline, Observer& observer) {
            compiler_context ctx;
#if LITHE_HAS_THREAD_LOCAL_SINK
            utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.lower.compile"> _nadi_compile{};
#endif
            observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                          observability::compilation_event::kind::started,
                                                          "compile_artifact",
                                                          observability::now_ns()
                                                      });

            using lowered_t = decltype(pipeline.run(std::forward<Expr>(expr), ctx.legacy));
            using artifact_t = lowering_artifact<std::decay_t<lowered_t>>;
            backend_result<artifact_t> result;

            const auto lower_start = observability::now_ns();
            try {
                ctx.current_stage = lowering_stage::capture;
                ctx.stage_trace.push_back(ctx.current_stage);
                ctx.legacy.input_hash = structural_hash(expr);

                auto lowered = pipeline.template run_observed<ObservabilityEnabled>(
                    std::forward<Expr>(expr), ctx.legacy, observer);

                ctx.current_stage = lowering_stage::codegen_preparation;
                ctx.stage_trace.push_back(ctx.current_stage);
                ctx.legacy.output_hash = structural_hash(lowered);
                ctx.legacy.equivalent = (ctx.legacy.input_hash == ctx.legacy.output_hash);

                artifact_t artifact{
                    std::move(lowered),
                    semantic::infer_semantics(expr),
                    ctx.legacy,
                    ctx.legacy.output_hash
                };
                result.output = std::move(artifact);
                result.context = std::move(ctx);

                observability::emit<ObservabilityEnabled>(observer, observability::lowering_event{
                                                              lowering_stage::codegen_preparation,
                                                              "artifact_ready",
                                                              result.context.legacy.input_hash,
                                                              result.context.legacy.output_hash,
                                                              lower_start,
                                                              observability::now_ns()
                                                          });
                observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                              observability::compilation_event::kind::finished,
                                                              "compile_artifact",
                                                              observability::now_ns()
                                                          });
            }
            catch (const std::exception& ex) {
                result.context = std::move(ctx);
                result.diagnostics.push_back(lowering_error{"compile", ex.what()}.to_diagnostic());
                observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                              observability::compilation_event::kind::failed,
                                                              "compile_artifact",
                                                              observability::now_ns()
                                                          });
            }
            return result;
        }

        template <class Expr>
        constexpr auto compile_to_graph(Expr&& expr) {
            lowering_pipeline<> pipeline{{}, compiler::opt_level::O1, true, true};
            return lowering::compile(std::forward<Expr>(expr), pipeline, graph_backend{});
        }

        template <class Expr>
        constexpr auto compile_to_graph(Expr&& expr, const compiler::opt_level preset) {
            lowering_pipeline<> pipeline{{}, preset, true, true};
            return lowering::compile(std::forward<Expr>(expr), pipeline, graph_backend{});
        }

        template <litegraph::LiteGraphModel GraphT>
        graph_analysis_result_t<GraphT> analyze_graph_ir(const GraphT& graph, const graph_lowering_context& ctx) {
            return analyze_graph_ir(graph, ctx.roots);
        }

        template <litegraph::LiteGraphModel GraphT>
        graph_optimization_result_t<GraphT> optimize_graph_ir(GraphT graph, const graph_lowering_context& ctx) {
            return optimize_graph_ir(std::move(graph), ctx.roots);
        }

        template <class Expr>
        lowering_result<graph_optimization_result> compile_to_graph_optimized(
            Expr&& expr,
            compiler::opt_level preset = compiler::opt_level::O1,
            graph_lowering_context graph_ctx = {}
        ) {
            lowering_result<graph_optimization_result> result;
            graph_ctx.lowering.input_hash = structural_hash(expr);

            lowering_pipeline<> pipeline{{}, preset, true, true};
            try {
                auto lowered_expr = pipeline.run(std::forward<Expr>(expr), graph_ctx.lowering);
                graph_ctx.lowering.output_hash = structural_hash(lowered_expr);
                graph_ctx.lowering.equivalent = (graph_ctx.lowering.input_hash == graph_ctx.lowering.output_hash);

                backend::symbolic_dag_builder builder;
                const auto root = visit(lowered_expr, builder);
                if (graph_ctx.roots.empty()) {
                    graph_ctx.roots.push_back(root);
                }

                auto optimized = optimize_graph_ir(std::move(builder.dag), graph_ctx.roots);
                result.output = std::move(optimized);
                result.context = graph_ctx.lowering;
                result.context.pass_names.emplace_back("graph_ir_analysis");
                result.context.pass_names.emplace_back("graph_ir_optimization");
                result.context.passes_applied += 5;
            }
            catch (const std::exception& ex) {
                result.context = graph_ctx.lowering;
                result.diagnostics.push_back(lowering_error{"graph-ir", ex.what()}.to_diagnostic());
            }
            catch (...) {
                result.context = graph_ctx.lowering;
                result.diagnostics.push_back(
                    lowering_error{"graph-ir", "unknown graph optimization failure"}.to_diagnostic());
            }

            return result;
        }

        template <bool ObservabilityEnabled = false, class Expr, class Observer = observability::null_observer>
        lowering_result<graph_optimization_result> compile_to_graph_optimized_observed(
            Expr&& expr,
            compiler::opt_level preset,
            Observer& observer
        ) {
            auto result = compile_to_graph_optimized(std::forward<Expr>(expr), preset, graph_lowering_context{});
            if constexpr (ObservabilityEnabled) {
                if (result.output.has_value()) {
                    observability::emit<ObservabilityEnabled>(observer, observability::rewrite_event{
                                                                  "graph_ir_optimization",
                                                                  result.output->before.edge_count + result.output->
                                                                  before.node_count,
                                                                  result.output->removed_dead_nodes +
                                                                  result.output->merged_shared_subtrees +
                                                                  result.output->simplified_dependencies +
                                                                  result.output->dominator_simplifications,
                                                                  observability::now_ns()
                                                              });
                }
                for (const auto& diagnostic : result.diagnostics) {
                    observability::emit<ObservabilityEnabled>(observer, observability::diagnostic_event{
                                                                  diagnostic.severity,
                                                                  diagnostic.stage,
                                                                  diagnostic.message,
                                                                  observability::now_ns()
                                                              });
                }
            }
            return result;
        }

        template <class Preset>
        struct preset_pipeline {
            Preset preset;

            template <class Expr>
            constexpr auto run(Expr&& expr, lowering_context& ctx) const {
                auto current = preset(std::forward<Expr>(expr));
                ctx.passes_applied = 1;
                ctx.pass_names = {"preset"};
                return current;
            }
        };

        template <class Expr, class Preset>
            requires std::invocable<Preset, Expr>
        constexpr auto compile_to_graph(Expr&& expr, Preset preset) {
            return lowering::compile(
                std::forward<Expr>(expr),
                preset_pipeline<std::decay_t<Preset>>{std::move(preset)},
                graph_backend{}
            );
        }

        template <class Expr>
        constexpr auto compile_to_tree(Expr&& expr, const compiler::opt_level preset) {
            lowering_pipeline<> pipeline{{}, preset, true, true};
            return lowering::compile(std::forward<Expr>(expr), pipeline, tree_backend{});
        }

        template <class Expr>
        constexpr auto compile_to_tree(Expr&& expr) {
            lowering_pipeline<> pipeline{{}, compiler::opt_level::O1, true, true};
            return lowering::compile(std::forward<Expr>(expr), pipeline, tree_backend{});
        }

        template <class Expr>
        constexpr auto compile_to_dataflow(Expr&& expr, const compiler::opt_level preset) {
            lowering_pipeline<> pipeline{{}, preset, true, true};
            return lowering::compile(std::forward<Expr>(expr), pipeline, dataflow_backend{});
        }

        template <class Expr, class Preset>
            requires std::invocable<Preset, Expr>
        constexpr auto compile_to_dataflow(Expr&& expr, Preset preset) {
            return lowering::compile(
                std::forward<Expr>(expr),
                preset_pipeline<std::decay_t<Preset>>{std::move(preset)},
                dataflow_backend{}
            );
        }

        template <class Expr>
        constexpr auto compile_to_dataflow(Expr&& expr) {
            return compile_to_dataflow(std::forward<Expr>(expr), compiler::opt_level::O1);
        }

        template <class Expr>
        lowering_result<symbolic_dataflow_ir<backend::SymbolicDAG>> compile_to_symbolic_dataflow(
            Expr&& expr,
            compiler::opt_level preset = compiler::opt_level::O1,
            const std::vector<litegraph::NodeId>& roots = {},
            bool simplify_before_schedule = true
        ) {
            lowering_result<symbolic_dataflow_ir<backend::SymbolicDAG>> result;
            lowering_pipeline<> pipeline{{}, preset, true, true};

            lowering_context ctx;
            ctx.input_hash = structural_hash(expr);
            try {
                auto lowered_expr = pipeline.run(std::forward<Expr>(expr), ctx);
                backend::symbolic_dag_builder builder;
                const auto root = visit(lowered_expr, builder);

                std::vector<litegraph::NodeId> effective_roots = roots;
                if (effective_roots.empty()) {
                    effective_roots.push_back(root);
                }

                backend::SymbolicDAG graph = std::move(builder.dag);
                if (simplify_before_schedule) {
                    auto simplified = symbolic_dataflow_engine<backend::SymbolicDAG>::simplify(
                        std::move(graph), effective_roots);
                    graph = std::move(simplified.graph);
                    ctx.pass_names.insert(ctx.pass_names.end(), simplified.passes.begin(), simplified.passes.end());
                }

                result.output = symbolic_dataflow_engine<backend::SymbolicDAG>::build_ir(
                    std::move(graph), effective_roots);
                ctx.output_hash = structural_hash(lowered_expr);
                ctx.equivalent = (ctx.input_hash == ctx.output_hash);
                result.context = std::move(ctx);
            }
            catch (const std::exception& ex) {
                result.context = std::move(ctx);
                result.diagnostics.push_back(lowering_error{"symbolic-dataflow", ex.what()}.to_diagnostic());
            }

            return result;
        }

        template <bool ObservabilityEnabled = false, class Expr, class Observer = observability::null_observer>
        lowering_result<symbolic_dataflow_ir<backend::SymbolicDAG>> compile_to_symbolic_dataflow_observed(
            Expr&& expr,
            compiler::opt_level preset,
            const std::vector<litegraph::NodeId>& roots,
            bool simplify_before_schedule,
            Observer& observer
        ) {
            auto result = compile_to_symbolic_dataflow(
                std::forward<Expr>(expr),
                preset,
                roots,
                simplify_before_schedule);

            if constexpr (ObservabilityEnabled) {
                if (result.output.has_value()) {
                    observability::emit<ObservabilityEnabled>(observer, observability::lowering_event{
                                                                  lowering_stage::graph_lowering,
                                                                  "symbolic_dataflow_built",
                                                                  result.context.input_hash,
                                                                  result.context.output_hash,
                                                                  observability::now_ns(),
                                                                  observability::now_ns()
                                                              });
                }
                for (const auto& diagnostic : result.diagnostics) {
                    observability::emit<ObservabilityEnabled>(observer, observability::diagnostic_event{
                                                                  diagnostic.severity,
                                                                  diagnostic.stage,
                                                                  diagnostic.message,
                                                                  observability::now_ns()
                                                              });
                }
            }
            return result;
        }

        template <class Expr, class Preset>
            requires std::invocable<Preset, Expr>
        constexpr auto compile_to_tree(Expr&& expr, Preset preset) {
            return lowering::compile(
                std::forward<Expr>(expr),
                preset_pipeline<std::decay_t<Preset>>{std::move(preset)},
                tree_backend{}
            );
        }
    } // namespace lowering

    namespace optimization {
        struct dependency_aware_scheduler {
            struct PassInfo {
                std::string name;
                std::size_t pass_id;
                std::unordered_set<std::size_t> prerequisites;
                bool is_analysis = false;

                constexpr bool operator==(const PassInfo& other) const {
                    return pass_id == other.pass_id;
                }

                constexpr bool operator<(const PassInfo& other) const {
                    return pass_id < other.pass_id;
                }
            };

            struct DependencyEdge {
                enum class Type { Required, Optional, Invalidates };

                Type dep_type = Type::Required;

                constexpr bool operator==(const DependencyEdge& other) const {
                    return dep_type == other.dep_type;
                }
            };

            mutable std::vector<PassInfo> passes;
            mutable std::size_t next_pass_id = 1;

            template <class PassType>
            std::size_t register_pass(const std::string& name,
                                      const std::vector<std::string>& dependencies = {},
                                      const bool is_analysis = false) const {
                PassInfo info{name, next_pass_id++, {}, is_analysis};
                passes.push_back(std::move(info));
                return passes.size() - 1;
            }

            std::vector<std::string> get_optimal_ordering() const {
                std::vector<std::string> ordering;
                ordering.reserve(passes.size());
                for (const auto& pass : passes) {
                    ordering.push_back(pass.name);
                }
                return ordering;
            }

            static bool has_circular_dependencies() {
                return false;
            }
        };

        struct pattern_matcher {
            backend::ASTTree pattern_tree;

            template <class Pattern>
            explicit pattern_matcher(const Pattern& pattern) {
                backend::ast_builder builder;
                visit(pattern, builder);
                builder.finalize();
                pattern_tree = std::move(builder.tree);
            }

            template <class Expr>
            bool matches(const Expr& expr) const {
                backend::ast_builder builder;
                visit(expr, builder);
                builder.finalize();
                return pattern_tree.structural_equal(builder.tree);
            }

            template <class Expr>
            std::vector<std::size_t> find_matches(const Expr& expr) const {
                std::vector<std::size_t> matches;
                return matches;
            }
        };

        template <class Rewriter>
        struct graph_rewrite_rule {
            pattern_matcher pattern;
            Rewriter rewriter;

            template <class Pattern, class R>
            graph_rewrite_rule(Pattern&& pat, R&& rw)
                : pattern(std::forward<Pattern>(pat))
                  , rewriter(std::forward<R>(rw)) {}

            template <class Expr>
            bool can_apply(const Expr& expr) const {
                return pattern.matches(expr);
            }

            template <class Expr>
            decltype(auto) apply(Expr&& expr) const {
                if (can_apply(expr)) {
                    return rewriter(std::forward<Expr>(expr));
                }
                return std::forward<Expr>(expr);
            }
        };

        template <class Pattern, class Rewriter>
        graph_rewrite_rule(Pattern&&, Rewriter&&) -> graph_rewrite_rule<std::decay_t<Rewriter>>;
    } // namespace optimization

    namespace integration {
        template <class Expr>
        struct enhanced_compilation_result {
            using original_type = std::decay_t<Expr>;

            Expr optimized_expr;
            std::string backend_analysis;
            analysis::analysis_results<Expr> frontend_analysis;

            [[nodiscard]] bool should_use_cse() const {
                return frontend_analysis.dag_sharing_ratio > 0.2;
            }

            [[nodiscard]] bool has_optimization_opportunities() const {
                return frontend_analysis.is_optimization_worthwhile();
            }

            [[nodiscard]] std::vector<std::string> get_optimization_recommendations() const {
                std::vector<std::string> recommendations;

                if (should_use_cse()) {
                    recommendations.emplace_back("Enable common subexpression elimination");
                }
                if (!frontend_analysis.common_subexprs.empty()) {
                    recommendations.emplace_back("Consider control flow optimizations");
                }
                if (frontend_analysis.total_cost > 20) {
                    recommendations.emplace_back("Apply strength reduction");
                }

                return recommendations;
            }
        };

        template <class Expr, class... Passes>
        auto compile_with_analysis(Expr&& expr, Passes&&... passes) {
            auto frontend_analysis = analysis::analyze(expr);
            backend::unified_backend backend;
            auto backend_analysis = backend.analyze_expression(expr);
            auto optimized = compiler::compile(std::forward<Expr>(expr), std::forward<Passes>(passes)...);

            return enhanced_compilation_result<std::decay_t<Expr>>{
                std::move(optimized),
                std::move(backend_analysis),
                std::move(frontend_analysis)
            };
        }

        template <class Expr>
        auto smart_optimize(Expr&& expr) {
            auto analysis_result = analysis::analyze(expr);

            if (analysis_result.total_cost < 5) {
                return compiler::optimize_preset(std::forward<Expr>(expr), compiler::opt_level::O1);
            }
            else if (analysis_result.is_optimization_worthwhile()) {
                return compiler::optimize_preset(std::forward<Expr>(expr), compiler::opt_level::O2);
            }
            else {
                return compiler::optimize_preset(std::forward<Expr>(expr), compiler::opt_level::O0);
            }
        }

        template <class Expr, class Pattern, class Replacement>
        auto optimize_with_pattern(Expr&& expr, Pattern&& pattern, Replacement&& replacement) {
            optimization::pattern_matcher matcher(std::forward<Pattern>(pattern));

            if (matcher.matches(expr)) {
                return std::forward<Replacement>(replacement);
            }
            else {
                return std::forward<Expr>(expr);
            }
        }
    } // namespace integration

    // =========================================================================
    // External frontend import model
    // Allows tree-sitter / custom parsers to feed Lithe without Lithe owning
    // parsing.  Pure adapter/data model — no parser implementation.
    // =========================================================================
    namespace frontend {
        // Stable numeric tag for an operation carried by an imported node.
        // Values 0–255 are reserved for well-known Lithe operations; callers
        // may allocate IDs >= 256 for language-specific extensions.
        using operation_id = std::uint32_t;

        namespace op {
            inline constexpr operation_id unknown = 0u;
            inline constexpr operation_id add = 1u;
            inline constexpr operation_id sub = 2u;
            inline constexpr operation_id mul = 3u;
            inline constexpr operation_id div = 4u;
            inline constexpr operation_id mod = 5u;
            inline constexpr operation_id neg = 6u;
            inline constexpr operation_id eq = 10u;
            inline constexpr operation_id ne = 11u;
            inline constexpr operation_id lt = 12u;
            inline constexpr operation_id le = 13u;
            inline constexpr operation_id gt = 14u;
            inline constexpr operation_id ge = 15u;
            inline constexpr operation_id logical_and = 20u;
            inline constexpr operation_id logical_or = 21u;
            inline constexpr operation_id logical_not = 22u;
            inline constexpr operation_id sequence = 30u;
            inline constexpr operation_id call = 31u;
            inline constexpr operation_id ret = 32u;
            inline constexpr operation_id branch = 33u;
            inline constexpr operation_id literal = 40u;
            inline constexpr operation_id variable = 41u;
            inline constexpr operation_id assign = 42u;
            inline constexpr operation_id function_def = 50u;
            inline constexpr operation_id module_def = 51u;
        } // namespace op

        // Position within a source text identified by a file handle.
        struct source_location {
            std::size_t file_id = 0;
            std::size_t line = 0; // 1-based
            std::size_t column = 0; // 1-based
            std::size_t offset = 0; // byte offset from file start
        };

        // Half-open byte range [begin, end) within a single source file.
        struct source_span {
            source_location begin{};
            source_location end{};

            [[nodiscard]] constexpr bool valid() const noexcept {
                return begin.file_id == end.file_id && begin.offset <= end.offset;
            }

            [[nodiscard]] constexpr std::size_t length() const noexcept {
                return end.offset - begin.offset;
            }
        };

        // Span plus a human-readable label used in error messages / IDE hovers.
        struct diagnostic_span {
            source_span span{};
            std::string label; // e.g. "expected ';' here"
            std::string note; // optional secondary detail
        };

        // A single node produced by an external parser.
        struct imported_node {
            operation_id operation = op::unknown;
            std::vector<imported_node> children;
            std::unordered_map<std::string, std::string> attributes;
            source_span span{};
        };

        // A top-level function as provided by an external parser.
        struct imported_function {
            std::string name;
            std::vector<std::string> parameter_names;
            imported_node body;
            source_span span{};
            std::unordered_map<std::string, std::string> attributes;
        };

        // A module (compilation unit) supplied by an external parser.
        struct imported_module {
            std::string name;
            std::string source_path;
            std::vector<imported_function> functions;
            std::vector<imported_node> top_level_nodes;
            source_span span{};
        };

        // Outcome of a single import operation.
        struct frontend_import_result {
            std::optional<imported_module> module;
            std::vector<diagnostic_span> diagnostics;

            [[nodiscard]] bool ok() const noexcept {
                return module.has_value();
            }

            [[nodiscard]] bool has_errors() const noexcept {
                return !module.has_value();
            }
        };

        // =====================================================================
        // ExternalFrontendAdapter concept
        // An external parser plugs into Lithe by satisfying this concept.
        // No grammar, no parser dependency — pure structural requirement.
        // =====================================================================

        // The concept requires three callable surfaces on the adapter:
        //   parse(source_text)         → frontend_import_result
        //   import_module(path)        → frontend_import_result
        //   diagnostics()             → range of diagnostic_span
        template <class A>
        concept ExternalFrontendAdapter =
            requires(A& adapter,
                     std::string_view source,
                     std::string_view path) {
                // parse a source text and return an import result
                {
                    adapter.parse(source)
                }
                -> std::convertible_to<frontend_import_result>;

                // import a module by path (file system or virtual)
                {
                    adapter.import_module(path)
                }
                -> std::convertible_to<frontend_import_result>;

                // retrieve accumulated diagnostics
                { adapter.diagnostics() };
            };

        // =====================================================================
        // Lowering helpers
        // Map imported nodes / functions / modules into Lithe IR structures.
        // Source spans are preserved; unknown operations produce diagnostics.
        // =====================================================================

        // Result of lowering a single imported_node.
        struct lowered_node_result {
            lowering::frontend_ast ast;
            std::vector<diagnostic_span> diagnostics;

            [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
        };

        // Map a single imported_node into a frontend_ast.
        // Unknown operation IDs produce a warning diagnostic; the node is
        // still emitted (as "unknown") so downstream passes can inspect it.
        inline lowered_node_result lower_imported_node(
            const imported_node& node,
            lowering::frontend_context& ctx) {
            lowered_node_result result;

            // Build a human-readable opcode string from the operation_id.
            // Well-known IDs get canonical names; everything else is "op:<id>".
            std::string opcode;
            switch (node.operation) {
            case op::unknown: opcode = "unknown";
                break;
            case op::add: opcode = "add";
                break;
            case op::sub: opcode = "sub";
                break;
            case op::mul: opcode = "mul";
                break;
            case op::div: opcode = "div";
                break;
            case op::mod: opcode = "mod";
                break;
            case op::neg: opcode = "neg";
                break;
            case op::eq: opcode = "eq";
                break;
            case op::ne: opcode = "ne";
                break;
            case op::lt: opcode = "lt";
                break;
            case op::le: opcode = "le";
                break;
            case op::gt: opcode = "gt";
                break;
            case op::ge: opcode = "ge";
                break;
            case op::logical_and: opcode = "and";
                break;
            case op::logical_or: opcode = "or";
                break;
            case op::logical_not: opcode = "not";
                break;
            case op::sequence: opcode = "sequence";
                break;
            case op::call: opcode = "call";
                break;
            case op::ret: opcode = "ret";
                break;
            case op::branch: opcode = "branch";
                break;
            case op::literal: opcode = "literal";
                break;
            case op::variable: opcode = "variable";
                break;
            case op::assign: opcode = "assign";
                break;
            case op::function_def: opcode = "function_def";
                break;
            case op::module_def: opcode = "module_def";
                break;
            default:
                opcode = "op:" + std::to_string(node.operation);
                // Unknown operations: emit a warning diagnostic that
                // preserves the source span for IDE integration.
                result.diagnostics.push_back(diagnostic_span{
                    node.span,
                    "unknown operation id " + std::to_string(node.operation),
                    "node will be emitted as 'unknown' in the Lithe IR"
                });
                ctx.add_diagnostic(lowering::frontend_diagnostic{
                    lowering::frontend_diagnostic::level::warning,
                    "unknown external operation id " + std::to_string(node.operation),
                    lowering::source_span{
                        node.span.begin.offset,
                        node.span.length(),
                        node.span.begin.line,
                        node.span.begin.column
                    }
                });
                break;
            }

            // Build the frontend_ast node.
            lowering::frontend_ast ast_node;
            ast_node.node_kind = opcode;
            ast_node.span = lowering::source_span{
                node.span.begin.offset,
                node.span.length(),
                node.span.begin.line,
                node.span.begin.column
            };

            // Copy attributes into payload (as a string map via std::any).
            if (!node.attributes.empty()) {
                ast_node.payload = node.attributes;
            }

            // Recursively lower children.
            for (const auto& child : node.children) {
                auto child_result = lower_imported_node(child, ctx);
                ast_node.children.push_back(std::move(child_result.ast));
                for (auto& d : child_result.diagnostics) {
                    result.diagnostics.push_back(std::move(d));
                }
            }

            result.ast = std::move(ast_node);
            return result;
        }

        // Convenience overload that allocates a temporary frontend_context.
        inline lowered_node_result lower_imported_node(const imported_node& node) {
            lowering::frontend_context ctx;
            return lower_imported_node(node, ctx);
        }

        // Result of lowering an imported_function.
        struct lowered_function_result {
            lowering::frontend_ast ast;
            std::vector<diagnostic_span> diagnostics;

            [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
        };

        // Map an imported_function into a frontend_ast.
        inline lowered_function_result lower_imported_function(
            const imported_function& fn,
            lowering::frontend_context& ctx) {
            lowered_function_result result;

            lowering::frontend_ast fn_ast;
            fn_ast.node_kind = "function_def";
            fn_ast.node_id = fn.name;
            fn_ast.span = lowering::source_span{
                fn.span.begin.offset,
                fn.span.length(),
                fn.span.begin.line,
                fn.span.begin.column
            };

            // Emit parameter names as attribute children.
            for (const auto& param : fn.parameter_names) {
                lowering::frontend_ast param_ast;
                param_ast.node_kind = "parameter";
                param_ast.node_id = param;
                fn_ast.children.push_back(std::move(param_ast));
            }

            // Carry function attributes.
            if (!fn.attributes.empty()) {
                fn_ast.payload = fn.attributes;
            }

            // Lower the body node.
            auto body_result = lower_imported_node(fn.body, ctx);
            fn_ast.children.push_back(std::move(body_result.ast));
            for (auto& d : body_result.diagnostics) {
                result.diagnostics.push_back(std::move(d));
            }

            result.ast = std::move(fn_ast);
            return result;
        }

        inline lowered_function_result lower_imported_function(const imported_function& fn) {
            lowering::frontend_context ctx;
            return lower_imported_function(fn, ctx);
        }

        // Result of lowering a complete imported_module.
        struct lowered_module_result {
            std::vector<lowering::frontend_ast> function_asts;
            std::vector<lowering::frontend_ast> top_level_asts;
            std::vector<diagnostic_span> diagnostics;

            [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
        };

        // Map an imported_module into its Lithe IR representation, populating
        // the provided frontend_context with span mappings and diagnostics.
        inline lowered_module_result lower_imported_module(
            const imported_module& mod,
            lowering::frontend_context& ctx) {
            lowered_module_result result;

            for (const auto& fn : mod.functions) {
                auto fn_result = lower_imported_function(fn, ctx);
                result.function_asts.push_back(std::move(fn_result.ast));
                for (auto& d : fn_result.diagnostics) {
                    result.diagnostics.push_back(std::move(d));
                }
            }

            for (const auto& node : mod.top_level_nodes) {
                auto node_result = lower_imported_node(node, ctx);
                result.top_level_asts.push_back(std::move(node_result.ast));
                for (auto& d : node_result.diagnostics) {
                    result.diagnostics.push_back(std::move(d));
                }
            }

            return result;
        }

        inline lowered_module_result lower_imported_module(const imported_module& mod) {
            lowering::frontend_context ctx;
            return lower_imported_module(mod, ctx);
        }

        // =====================================================================
        // Adapter-driven entry point
        // Given any ExternalFrontendAdapter and a source path, import and lower
        // a module in one call.
        // =====================================================================
        template <ExternalFrontendAdapter Adapter>
        [[nodiscard]] lowered_module_result import_and_lower(
            Adapter& adapter,
            std::string_view path,
            lowering::frontend_context& ctx) {
            auto import_result = adapter.import_module(path);
            if (!import_result.ok()) {
                lowered_module_result fail;
                fail.diagnostics = std::move(import_result.diagnostics);
                return fail;
            }
            return lower_imported_module(*import_result.module, ctx);
        }

        template <ExternalFrontendAdapter Adapter>
        [[nodiscard]] lowered_module_result import_and_lower(
            Adapter& adapter,
            std::string_view path) {
            lowering::frontend_context ctx;
            return import_and_lower(adapter, path, ctx);
        }
    } // namespace frontend

    // =========================================================================
    // Coercion lowering
    //
    // abstract_coercion_op   : a backend-agnostic node that records a required
    //                          coercion between two types.  It carries no machine-
    //                          code sequence; backends decode it in their own pass.
    //
    // coercion_insertion_context : per-lowering state tracking which coercions
    //                              were inserted and where.
    //
    // coercion_lowering_pass : a lowering::lowering_pass-compatible functor that
    //                          walks symbolic IR and inserts abstract_coercion_op
    //                          nodes wherever the semantic_type_rule_engine finds
    //                          a required coercion.
    // =========================================================================
    namespace coercion {
        // ---------------------------------------------------------------------
        // abstract_coercion_op
        //
        // Represents a single, abstract "convert value of source_type to
        // target_type using the specified coercion_kind".  No backend details.
        // ---------------------------------------------------------------------
        struct abstract_coercion_op {
            // Identity of this coercion node within the IR.
            std::size_t node_id = 0;
            structural_hash_t operand_hash = 0; // hash of the value being coerced

            // Type information.
            semantic::types::type_id source_type = semantic::types::invalid_type_id;
            semantic::types::type_id target_type = semantic::types::invalid_type_id;

            // The resolved plan — may contain multiple steps.
            semantic::type_rules::coercion_plan plan;

            // Convenience: the primary coercion kind of the first step.
            [[nodiscard]] semantic::type_rules::coercion_kind primary_kind() const noexcept {
                if (plan.steps.empty()) return semantic::type_rules::coercion_kind::identity;
                return plan.steps.front().kind;
            }

            [[nodiscard]] bool is_identity() const noexcept {
                return plan.steps.empty() ||
                (plan.steps.size() == 1 &&
                    plan.steps[0].kind == semantic::type_rules::coercion_kind::identity);
            }

            [[nodiscard]] bool is_lossless() const noexcept {
                return plan.is_lossless;
            }

            [[nodiscard]] std::string describe() const {
                return plan.describe();
            }
        };

        // ---------------------------------------------------------------------
        // coercion_site
        //
        // Records one location in the IR where a coercion was inserted or
        // identified as needed.
        // ---------------------------------------------------------------------
        struct coercion_site {
            structural_hash_t expression_key = 0;
            std::string operation_name;
            std::size_t operand_index = 0;
            abstract_coercion_op coercion;
            lowering::lowering_diagnostic::level severity
                = lowering::lowering_diagnostic::level::info;
        };

        // ---------------------------------------------------------------------
        // coercion_insertion_context
        //
        // Accumulates all coercions found or inserted during a lowering pass.
        // Passed by reference through the coercion_lowering_pass.
        // ---------------------------------------------------------------------
        struct coercion_insertion_context {
            std::vector<coercion_site> sites;
            std::vector<lowering::lowering_diagnostic> diagnostics;
            std::size_t next_node_id = 1;
            bool has_errors = false;

            [[nodiscard]] std::size_t allocate_node_id() {
                return next_node_id++;
            }

            void record_site(coercion_site site) {
                if (!site.coercion.is_identity()) {
                    sites.push_back(std::move(site));
                }
            }

            void add_diagnostic(const lowering::lowering_diagnostic::level level,
                                std::string stage, std::string message) {
                if (level == lowering::lowering_diagnostic::level::error) {
                    has_errors = true;
                }
                diagnostics.push_back({level, std::move(stage), std::move(message)});
            }

            [[nodiscard]] bool ok() const { return !has_errors; }

            // Produce a coercion_plan for a given (from → to) transition.
            [[nodiscard]] semantic::type_rules::coercion_plan plan(
                const semantic::types::type_id from,
                const semantic::types::type_id to,
                const semantic::types::semantic_type_registry* reg = &semantic::types::type_registry()) const {
                return semantic::type_rules::plan_coercion(from, to, reg);
            }
        };

        // ---------------------------------------------------------------------
        // coercion_analysis_result
        //
        // Returned by coercion_lowering_pass::analyze() to describe all required
        // coercions without mutating IR.
        // ---------------------------------------------------------------------
        struct coercion_analysis_result {
            std::vector<coercion_site> required_coercions;
            std::vector<lowering::lowering_diagnostic> diagnostics;
            std::size_t coercion_count = 0;
            std::size_t lossless_count = 0;
            std::size_t lossy_count = 0;
            bool feasible = true;

            [[nodiscard]] bool has_lossy() const { return lossy_count > 0; }
            [[nodiscard]] bool needs_coercion() const { return coercion_count > 0; }
        };

        // ---------------------------------------------------------------------
        // coercion_lowering_pass
        //
        // A pass that integrates with semantic_type_rule_engine to:
        //   1. Walk each node in the SymbolicDAG.
        //   2. For each operand edge, ask the rule engine if a coercion is needed.
        //   3. Record an abstract_coercion_op and emit it into the context.
        //
        // Coercions are abstract operations only — no backend code is emitted.
        // This pass is meant to run at lowering_stage::semantic_propagation,
        // before graph_lowering.
        // ---------------------------------------------------------------------
        class coercion_lowering_pass {
        public:
            explicit coercion_lowering_pass(
                semantic::type_rules::semantic_type_rule_engine* engine
                    = &semantic::type_rules::type_rule_engine(),
                semantic::types::semantic_type_registry* type_reg
                    = &semantic::types::type_registry())
                : engine_(engine), type_reg_(type_reg) {}

            // ------------------------------------------------------------------
            // analyze — inspect a SymbolicDAG for required coercions without
            //           mutating anything.
            // ------------------------------------------------------------------
            [[nodiscard]] coercion_analysis_result analyze(
                const backend::SymbolicDAG& dag) const {
                coercion_analysis_result out;
                coercion_insertion_context ctx;

                for (const auto& [nid, node] : dag.nodes()) {
                    analyze_node_(dag, litegraph::NodeId{nid}, ctx);
                }

                out.required_coercions = std::move(ctx.sites);
                out.diagnostics = std::move(ctx.diagnostics);
                out.coercion_count = out.required_coercions.size();
                for (const auto& site : out.required_coercions) {
                    if (site.coercion.is_lossless()) {
                        ++out.lossless_count;
                    }
                    else {
                        ++out.lossy_count;
                    }
                }
                out.feasible = ctx.ok();
                return out;
            }

            // ------------------------------------------------------------------
            // apply — walk a SymbolicDAG and populate the coercion_insertion_context
            //         with all required abstract_coercion_op nodes.
            //
            // The DAG itself is not mutated: backends receive both the original
            // DAG and the context, and perform actual insertion themselves.
            // ------------------------------------------------------------------
            void apply(
                const backend::SymbolicDAG& dag,
                coercion_insertion_context& ctx) const {
                for (const auto& [nid, node] : dag.nodes()) {
                    analyze_node_(dag, litegraph::NodeId{nid}, ctx);
                }
            }

            // ------------------------------------------------------------------
            // check_operand — ask the rule engine whether a coercion is needed
            //                 for one (actual_type → expected_type) operand slot.
            //                 Returns a resolved coercion_plan (possibly identity).
            // ------------------------------------------------------------------
            [[nodiscard]] semantic::type_rules::coercion_plan check_operand(
                const semantic::types::type_id actual_type,
                const semantic::types::type_id expected_type) const {
                if (actual_type == expected_type) {
                    // Fast path: types are identical, no coercion needed.
                    semantic::type_rules::coercion_plan identity;
                    identity.source_type = actual_type;
                    identity.target_type = expected_type;
                    identity.feasible = true;
                    identity.is_lossless = true;
                    identity.steps.push_back({
                        actual_type, expected_type,
                        semantic::type_rules::coercion_kind::identity,
                        true, "identity", "same type"
                    });
                    return identity;
                }

                // Delegate to the planner (uses rule registry + structural registry).
                return semantic::type_rules::plan_coercion(actual_type, expected_type, type_reg_);
            }

            // ------------------------------------------------------------------
            // plan_for_assignment — convenience wrapper for assignment-site use.
            //   Returns the plan and adds a diagnostic warning for lossy coercions.
            // ------------------------------------------------------------------
            [[nodiscard]] semantic::type_rules::coercion_plan plan_for_assignment(
                const semantic::types::type_id rhs_type,
                const semantic::types::type_id lhs_type,
                coercion_insertion_context& ctx,
                const std::string_view location_hint = "") const {
                auto plan = check_operand(rhs_type, lhs_type);
                if (!plan.feasible) {
                    ctx.add_diagnostic(
                        lowering::lowering_diagnostic::level::error,
                        "coercion_lowering",
                        "no coercion path: " + plan.failure_reason +
                        (location_hint.empty() ? "" : " at " + std::string{location_hint}));
                }
                else if (!plan.is_lossless) {
                    ctx.add_diagnostic(
                        lowering::lowering_diagnostic::level::warning,
                        "coercion_lowering",
                        "lossy coercion inserted: " + plan.describe() +
                        (location_hint.empty() ? "" : " at " + std::string{location_hint}));
                }
                return plan;
            }

        private:
            // Walk one node's incoming edges and check each operand type.
            void analyze_node_(
                const backend::SymbolicDAG& dag,
                const litegraph::NodeId node_id,
                coercion_insertion_context& ctx) const {
                const auto& node_payload = dag.node_data(node_id);

                // Infer the expected type for this node from its semantic fingerprint.
                semantic::types::type_id expected_type = semantic::types::invalid_type_id;
                if (node_payload.semantic_fingerprint.has_value() && type_reg_) {
                    // Use the structural fingerprint to look up a registered type.
                    // In practice this would map through a semantic→type bridge;
                    // here we record what's available from the DAG node.
                    (void)expected_type; // resolved per-edge below
                }

                std::size_t operand_idx = 0;
                for (auto eid : dag.in_edges(node_id)) {
                    const auto& edge = dag.get_edge(eid);
                    const auto& src_payload = dag.node_data(edge.from);

                    // Determine the type of the source operand.
                    semantic::types::type_id src_type = infer_node_type_(src_payload);
                    semantic::types::type_id dst_type = infer_node_type_(node_payload);

                    if (src_type == semantic::types::invalid_type_id ||
                        dst_type == semantic::types::invalid_type_id) {
                        ++operand_idx;
                        continue; // not enough type information to plan
                    }

                    auto plan = check_operand(src_type, dst_type);

                    if (!plan.feasible) {
                        ctx.add_diagnostic(
                            lowering::lowering_diagnostic::level::error,
                            "coercion_lowering",
                            "infeasible coercion at operand " +
                            std::to_string(operand_idx) +
                            " of node " + node_payload.name +
                            ": " + plan.failure_reason);
                    }
                    else if (!plan.trivial()) {
                        abstract_coercion_op op;
                        op.node_id = ctx.allocate_node_id();
                        op.operand_hash = src_payload.structural_hash;
                        op.source_type = src_type;
                        op.target_type = dst_type;
                        op.plan = std::move(plan);

                        ctx.record_site(coercion_site{
                            src_payload.structural_hash,
                            node_payload.name,
                            operand_idx,
                            std::move(op),
                            plan.is_lossless
                                ? lowering::lowering_diagnostic::level::info
                                : lowering::lowering_diagnostic::level::warning
                        });
                    }
                    ++operand_idx;
                }
            }

            // Infer a type_id from a SymbolicExpression using its semantic fingerprint.
            // When no fingerprint is available, returns invalid_type_id.
            [[nodiscard]] semantic::types::type_id infer_node_type_(
                const backend::SymbolicExpression& expr) const noexcept {
                if (!type_reg_) return semantic::types::invalid_type_id;
                if (!expr.semantic_fingerprint.has_value()) {
                    return semantic::types::invalid_type_id;
                }

                // Look up cached semantic info and convert to a type_id.
                auto info = semantic::registry().get(*expr.semantic_fingerprint);
                if (!info.has_value()) return semantic::types::invalid_type_id;

                const auto& sem = *info;

                // Map semantic domain/primitive info to a registered type.
                using namespace semantic::types;
                if (semantic::has_domain(sem.domain, semantic::domain_type::arithmetic)) {
                    if (sem.mutability_kind == semantic::mutability::immutable &&
                        sem.purity_level == semantic::purity::pure) {
                        return type_reg_->make_float_type(64);
                    }
                    return type_reg_->make_float_type(32);
                }
                if (semantic::has_domain(sem.domain, semantic::domain_type::tensor)) {
                    auto elem = type_reg_->make_float_type(32);
                    return type_reg_->make_tensor_type(elem, {});
                }
                return invalid_type_id;
            }

            semantic::type_rules::semantic_type_rule_engine* engine_;
            semantic::types::semantic_type_registry* type_reg_;
        };

        // ------------------------------------------------------------------
        // make_coercion_pass — factory for use in lowering pipelines.
        // ------------------------------------------------------------------
        [[nodiscard]] inline coercion_lowering_pass make_coercion_pass(
            semantic::type_rules::semantic_type_rule_engine * engine
            = &semantic::type_rules::type_rule_engine(),
            semantic::types::semantic_type_registry * type_reg
            = &semantic::types::type_registry()) {
            return coercion_lowering_pass{engine, type_reg};
        }
    } // namespace coercion

    // =========================================================================
    // MIR typing bridge
    //
    // Extends MIR-level lowering structures with optional semantic type metadata
    // so that type information from the semantic IR survives into backend code
    // generation.  Existing untyped lowering APIs are untouched.
    // Nested inside lithe::lowering to avoid collision with lithe::codegen::mir.
    // =========================================================================
    namespace lowering { namespace typed_mir {
            // -----------------------------------------------------------------
            // mir_type_metadata — optional type decoration for a single MIR value
            // -----------------------------------------------------------------
            struct mir_type_metadata {
                semantic::types::type_id type_id = semantic::types::invalid_type_id;

                // Source semantic info (effect, domain, purity, …) from typed IR.
                std::optional<semantic::semantic_info> semantic;

                // Original source span carried forward for diagnostics.
                std::optional<semantic::type_rules::source_span> source_span;

                [[nodiscard]] bool has_type() const noexcept {
                    return type_id != semantic::types::invalid_type_id;
                }
            };

            // -----------------------------------------------------------------
            // mir_operand — a typed operand reference in a MIR instruction
            // -----------------------------------------------------------------
            struct mir_operand {
                std::size_t value_id = 0; // virtual register / SSA value
                structural_hash_t structural_hash = 0;
                mir_type_metadata type_meta; // optional type decoration

                [[nodiscard]] bool is_typed() const noexcept { return type_meta.has_type(); }
            };

            // -----------------------------------------------------------------
            // mir_instruction — a single MIR instruction with optional type metadata
            // -----------------------------------------------------------------
            struct mir_instruction {
                std::string opcode;
                std::vector<mir_operand> operands;

                // Result type metadata (one per result value; usually one entry).
                std::vector<mir_type_metadata> result_types;

                // Source span carried from the typed expression.
                std::optional<semantic::type_rules::source_span> source_span;

                // Semantic info of the operation (effect, domain, coercions, …).
                std::optional<semantic::semantic_info> semantic;

                [[nodiscard]] bool has_result_type() const noexcept {
                    return !result_types.empty() && result_types.front().has_type();
                }

                [[nodiscard]] semantic::types::type_id primary_result_type() const noexcept {
                    return result_types.empty()
                               ? semantic::types::invalid_type_id
                               : result_types.front().type_id;
                }
            };

            // -----------------------------------------------------------------
            // mir_basic_block — a sequence of MIR instructions
            // -----------------------------------------------------------------
            struct mir_basic_block {
                std::size_t block_id = 0;
                std::string label;
                std::vector<mir_instruction> instructions;
            };

            // -----------------------------------------------------------------
            // mir_function — a MIR-level function with typed parameters/return
            // -----------------------------------------------------------------
            struct mir_function {
                std::string name;
                std::vector<mir_type_metadata> parameter_types;
                mir_type_metadata return_type;
                std::vector<mir_basic_block> blocks;

                // Aggregate semantic effect of the function body.
                std::optional<semantic::semantic_info> aggregate_effect;

                [[nodiscard]] bool has_return_type() const noexcept {
                    return return_type.has_type();
                }
            };

            // -----------------------------------------------------------------
            // mir_module — top-level MIR container
            // -----------------------------------------------------------------
            struct mir_module {
                std::string name;
                std::vector<mir_function> functions;
                std::vector<mir_instruction> globals;

                // Module-level semantic metadata.
                std::optional<semantic::semantic_info> module_effects;
            };
        } // namespace typed_mir

        // =========================================================================
        // Typed lowering — functions that consume typed semantic IR and
        // emit MIR with preserved type metadata and coercion insertions.
        //
        // The untyped lowering_pipeline and compile(…) APIs remain unchanged.
        // =========================================================================
        // (typed_lowering_context and lower_typed_* live in the same lowering namespace)

        // -----------------------------------------------------------------
        // typed_lowering_context — aggregates state for a single typed lowering
        // -----------------------------------------------------------------
        struct typed_lowering_context {
            semantic::types::semantic_type_registry* type_registry
                = &semantic::types::type_registry();
            semantic::type_rules::semantic_type_rule_engine* rule_engine
                = &semantic::type_rules::type_rule_engine();

            // Diagnostics emitted during typed lowering.
            std::vector<lowering_diagnostic> diagnostics;

            // Whether to insert abstract coercion nodes before MIR generation.
            bool insert_coercions = true;

            // Whether to preserve source spans into MIR diagnostic fields.
            bool preserve_source_spans = true;

            void emit_diagnostic(const lowering_diagnostic::level sev,
                                 std::string stage, std::string msg) {
                diagnostics.push_back({sev, std::move(stage), std::move(msg)});
            }

            [[nodiscard]] bool ok() const {
                return std::none_of(diagnostics.begin(), diagnostics.end(),
                                    [](const lowering_diagnostic& d) {
                                        return d.severity == lowering_diagnostic::level::error;
                                    });
            }
        };

        // -----------------------------------------------------------------
        // lower_typed_expression
        //
        // Converts a typed_ir::typed_expression into a mir_instruction that
        // carries preserved type metadata and (if needed) a coercion prefix.
        // -----------------------------------------------------------------
        [[nodiscard]] inline typed_mir::mir_instruction lower_typed_expression(
            const semantic::typed_ir::typed_expression& texpr,
            typed_lowering_context& ctx) {
            typed_mir::mir_instruction instr;

            // Populate result type from the inferred type.
            typed_mir::mir_type_metadata res_meta;
            res_meta.type_id = texpr.inferred_type;
            res_meta.semantic = texpr.effect_metadata;
            if (ctx.preserve_source_spans) {
                res_meta.source_span = texpr.span;
                instr.source_span = texpr.span;
            }
            instr.result_types.push_back(std::move(res_meta));

            // Carry semantic info.
            instr.semantic = texpr.effect_metadata;

            // If the expression carries a required coercion, insert it.
            if (ctx.insert_coercions && texpr.needs_coercion()) {
                instr.opcode = std::string{
                    semantic::type_rules::coercion_kind_name(
                        texpr.coercion.steps.empty()
                            ? semantic::type_rules::coercion_kind::identity
                            : texpr.coercion.steps.front().kind)
                };

                // Emit a diagnostic for lossy coercions.
                if (!texpr.coercion.is_lossless) {
                    ctx.emit_diagnostic(
                        lowering_diagnostic::level::warning,
                        "typed_lowering",
                        "lossy coercion applied: " + texpr.coercion.describe());
                }
            }
            else {
                instr.opcode = "value";
            }

            return instr;
        }

        // -----------------------------------------------------------------
        // lower_typed_function
        //
        // Lowers a typed_ir::typed_function into a mir_function, inserting
        // coercions and preserving source spans.
        // -----------------------------------------------------------------
        [[nodiscard]] inline typed_mir::mir_function lower_typed_function(
            const semantic::typed_ir::typed_function& fn,
            typed_lowering_context& ctx) {
            typed_mir::mir_function out;
            out.name = fn.name;
            out.aggregate_effect = fn.aggregate_effect;

            // Populate parameter types.
            for (const auto& pt : fn.parameter_types) {
                typed_mir::mir_type_metadata meta;
                meta.type_id = pt;
                out.parameter_types.push_back(std::move(meta));
            }

            // Return type.
            out.return_type.type_id = fn.return_type;

            // Lower body expressions into a single basic block.
            typed_mir::mir_basic_block block;
            block.block_id = 1;
            block.label = fn.name + ".entry";
            for (const auto& expr : fn.body) {
                block.instructions.push_back(lower_typed_expression(expr, ctx));
            }

            // Lower operations.
            for (const auto& op : fn.operations) {
                typed_mir::mir_instruction instr;
                instr.opcode = op.operation_id;
                instr.semantic = op.effect_metadata;

                // Operand types.
                for (std::size_t i = 0; i < op.operand_type_ids.size(); ++i) {
                    typed_mir::mir_operand operand;
                    operand.value_id = i + 1;
                    operand.type_meta.type_id = op.operand_type_ids[i];
                    instr.operands.push_back(std::move(operand));
                }

                // Result types.
                for (const auto& rt : op.result_type_ids) {
                    typed_mir::mir_type_metadata meta;
                    meta.type_id = rt;
                    meta.semantic = op.effect_metadata;
                    instr.result_types.push_back(std::move(meta));
                }

                block.instructions.push_back(std::move(instr));
            }

            out.blocks.push_back(std::move(block));

            if (ctx.preserve_source_spans) {
                // Attach function-level span to the first block if available.
                if (!fn.span.valid() && !out.blocks.empty()) {
                    (void)fn.span; // no-op: span not yet propagated
                }
            }

            return out;
        }

        // -----------------------------------------------------------------
        // lower_typed_module
        //
        // Lowers a typed_ir::typed_module into a mir_module.
        // -----------------------------------------------------------------
        [[nodiscard]] inline typed_mir::mir_module lower_typed_module(
            const semantic::typed_ir::typed_module& mod,
            typed_lowering_context& ctx) {
            typed_mir::mir_module out;
            out.name = mod.name;
            out.module_effects = mod.module_effects;

            for (const auto& fn : mod.functions) {
                out.functions.push_back(lower_typed_function(fn, ctx));
            }

            // Lower global constant expressions.
            for (const auto& g : mod.globals) {
                out.globals.push_back(lower_typed_expression(g, ctx));
            }

            return out;
        }

        // -----------------------------------------------------------------
        // Semantic pre-lowering pass
        //
        // Runs the semantic canonicalization pipeline over a semantic_registry
        // before any typed lowering begins.  This ensures MIR generation sees
        // the canonical, coercion-eliminated, effect-simplified semantic_info
        // for every node.
        //
        // -----------------------------------------------------------------

        // Result of running the semantic pre-lowering pass.
        struct semantic_pre_lowering_result {
            // Optimization report from the canonicalization pipeline.
            semantic::semantic_optimization_report report;

            // Diagnostics for nodes that could not be canonicalized.
            std::vector<lowering_diagnostic> diagnostics;

            [[nodiscard]] bool ok() const {
                return std::none_of(diagnostics.begin(), diagnostics.end(),
                                    [](const lowering_diagnostic& d) {
                                        return d.severity == lowering_diagnostic::level::error;
                                    });
            }
        };

        // Run the canonicalization pipeline over all node ids collected from
        // `mod`, using the semantic_registry embedded in `ctx`.
        //
        // Call this before lower_typed_module to ensure canonicalized semantics.
        template <class Observer = compiler::observability::null_observer>
        [[nodiscard]] semantic_pre_lowering_result
        run_semantic_pre_lowering(
            const semantic::typed_ir::typed_module& mod,
            semantic::semantic_registry& reg,
            const semantic::semantic_optimization_pipeline& pipeline,
            Observer& observer = compiler::observability::null_observer{}) {
            semantic_pre_lowering_result out;

            // Collect structural hashes for all typed expressions in the module.
            std::vector<structural_hash_t> node_ids;
            node_ids.reserve(mod.functions.size() * 8 + mod.globals.size());

            for (const auto& fn : mod.functions) {
                for (const auto& expr : fn.body) {
                    if (expr.expression_key != 0)
                        node_ids.push_back(expr.expression_key);
                }
            }
            for (const auto& g : mod.globals) {
                if (g.expression_key != 0)
                    node_ids.push_back(g.expression_key);
            }

            // Deduplicate.
            std::ranges::sort(node_ids);
            node_ids.erase(std::unique(node_ids.begin(), node_ids.end()),
                           node_ids.end());

            // Run the semantic canonicalization pipeline with observability.
            out.report = semantic_pass::run_semantic_canonicalization_pass(
                reg, node_ids, pipeline, observer,
                "semantic_pre_lowering[" + mod.name + "]");

            return out;
        }

        // Convenience overload: uses the default canonicalization pipeline and
        // no observer.
        [[nodiscard]] inline semantic_pre_lowering_result
        run_semantic_pre_lowering(
            const semantic::typed_ir::typed_module& mod,
            semantic::semantic_registry& reg) {
            const auto pipeline = semantic::make_default_semantic_pipeline();
            compiler::observability::null_observer noop;
            return run_semantic_pre_lowering(mod, reg, pipeline, noop);
        }
    } // namespace lowering
} // namespace lithe

namespace std {
    inline std::size_t hash<lithe::backend::BasicBlock>::operator()(
        const lithe::backend::BasicBlock& block) const noexcept {
        return std::hash<std::size_t>{}(block.block_id);
    }

    inline std::size_t hash<lithe::backend::ControlEdge>::operator()(
        const lithe::backend::ControlEdge& edge) const noexcept {
        std::size_t h1 = std::hash<int>{}(static_cast<int>(edge.edge_type));
        std::size_t h2 = edge.condition ? std::hash<std::string>{}(*edge.condition) : 0;
        return h1 ^ (h2 << 1);
    }

    inline std::size_t hash<lithe::backend::SymbolicExpression>::operator()(
        const lithe::backend::SymbolicExpression& expr) const noexcept {
        std::size_t h1 = std::hash<std::size_t>{}(expr.expr_id);
        std::size_t h2 = std::hash<std::string>{}(expr.name);
        std::size_t h3 = std::hash<int>{}(static_cast<int>(expr.type));
        std::size_t h4 = std::hash<int>{}(static_cast<int>(expr.category));
        std::size_t h5 = std::hash<std::size_t>{}(expr.structural_hash);
        std::size_t h6 = expr.semantic_fingerprint.has_value()
                             ? std::hash<std::size_t>{}(*expr.semantic_fingerprint)
                             : 0;
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4) ^ (h6 << 5);
    }

    inline std::size_t hash<lithe::backend::DependencyEdge>::operator()(
        const lithe::backend::DependencyEdge& edge) const noexcept {
        std::size_t h1 = std::hash<int>{}(static_cast<int>(edge.dep_type));
        std::size_t h2 = std::hash<int>{}(static_cast<int>(edge.kind));
        std::size_t h3 = std::hash<std::size_t>{}(edge.structural_hash);
        std::size_t h4 = std::hash<double>{}(edge.weight);
        std::size_t h5 = edge.semantic_fingerprint.has_value()
                             ? std::hash<std::size_t>{}(*edge.semantic_fingerprint)
                             : 0;
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
    }
}
