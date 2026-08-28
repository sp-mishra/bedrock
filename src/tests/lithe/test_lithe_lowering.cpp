#include "catch_amalgamated.hpp"

#include "lithe/lithe.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/lithe_lowering.hpp"

#include <string>
#include <type_traits>
#include <unordered_map>

namespace {
    lithe::codegen::spill_slot test_spill_slot(std::uint32_t id) {
        lithe::codegen::spill_slot slot;
        slot.id = id;
        slot.size = 8;
        slot.alignment = 8;
        slot.frame_offset = -static_cast<std::int64_t>(id * 8);
        return slot;
    }

    lithe::codegen::mir::physical_mir_function make_test_physical(
        std::string name,
        std::vector<lithe::codegen::allocated_instruction> instructions
    ) {
        using namespace lithe::codegen;

        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = 1;

        allocated_basic_block block;
        block.id = 1;
        block.name = "entry";
        block.instructions = std::move(instructions);
        fn.blocks.push_back(std::move(block));

        mir::physical_mir_function physical;
        physical.function = std::move(fn);
        physical.metadata.current_phase = mir::phase::physical_mir;
        return physical;
    }

    using expr_t = decltype(lithe::make_node<lithe::add_tag>(1, 2));

    struct mock_dataflow_backend {
        using handle_type = std::size_t;
        mutable std::size_t next = 1;

        void begin_module(lithe::lowering::lowering_context&) const {
            next = 1;
        }

        template <class T>
        handle_type emit_terminal(T&&) const {
            return next++;
        }

        handle_type emit_operation(std::string_view) const {
            return next++;
        }

        void connect(handle_type, handle_type) const {}

        void end_module() const {}

        template <class Expr>
        auto lower(Expr&&, lithe::lowering::lowering_context&) const {
            using graph_t = litegraph::Graph<int, int, litegraph::Directed>;
            graph_t g;
            const auto a = g.add_node(1);
            const auto b = g.add_node(2);
            g.add_edge(a, b, 0);
            return g;
        }
    };

    struct mock_codegen_backend {
        using handle_type = std::size_t;
        mutable std::size_t next = 1;

        void begin_module(lithe::lowering::lowering_context&) const {
            next = 1;
        }

        template <class T>
        handle_type emit_terminal(T&&) const {
            return next++;
        }

        handle_type emit_operation(std::string_view) const {
            return next++;
        }

        void connect(handle_type, handle_type) const {}

        void end_module() const {}

        template <class Expr>
        std::string lower(Expr&&, lithe::lowering::lowering_context&) const {
            return "mock-ir";
        }
    };

    struct raw_frontend_adapter {
        static constexpr lithe::lowering::frontend_kind kind = lithe::lowering::frontend_kind::handwritten;

        auto parse(std::string_view) const {
            return lithe::make_node<lithe::add_tag>(1, 2);
        }
    };

    struct diagnostic_frontend_adapter {
        static constexpr lithe::lowering::frontend_kind kind = lithe::lowering::frontend_kind::json_ast;

        auto parse(std::string_view) const {
            lithe::lowering::frontend_result<expr_t> result;
            result.ir = lithe::make_node<lithe::add_tag>(2, 3);
            result.span = lithe::lowering::source_span{10, 4, 3, 2};
            result.diagnostics.push_back(lithe::lowering::frontend_diagnostic{
                lithe::lowering::frontend_diagnostic::level::info,
                "json adapter ok",
                result.span
            });
            return result;
        }
    };

    struct normalizing_frontend_adapter {
        static constexpr lithe::lowering::frontend_kind kind = lithe::lowering::frontend_kind::external_dsl;

        auto normalize_to_lithe_ir(std::string_view input) const {
            return std::string(input) + "::normalized";
        }

        auto parse(const std::string& normalized) const {
            lithe::lowering::frontend_result<std::string> result;
            result.ir = normalized;
            return result;
        }
    };

    struct mapped_expr_frontend_adapter {
        static constexpr lithe::lowering::frontend_kind kind = lithe::lowering::frontend_kind::json_ast;

        auto normalize_to_lithe_ir(std::string_view input, lithe::lowering::frontend_context& ctx) const {
            const auto file_id = ctx.add_source_file("inline.json", std::string(input));
            (void)file_id;
            return input;
        }

        auto parse(std::string_view) const {
            lithe::lowering::frontend_result<expr_t> result;
            result.ir = lithe::make_node<lithe::add_tag>(7, 8);
            result.span = lithe::lowering::source_span{1, 3, 1, 2};
            result.diagnostics.push_back(lithe::lowering::frontend_diagnostic{
                lithe::lowering::frontend_diagnostic::level::warning,
                "mapped expr",
                result.span
            });
            return result;
        }
    };
} // namespace

TEST_CASE (



"Lithe lowering backend concepts classify built-in backends"
,
"[lithe][lowering]"
)
 {
    STATIC_REQUIRE(lithe::lowering::BackendModuleProtocol<lithe::lowering::graph_backend>);
    STATIC_REQUIRE(lithe::lowering::BackendModuleProtocol<lithe::lowering::tree_backend>);

    STATIC_REQUIRE(lithe::lowering::ExpressionLoweringBackend<lithe::lowering::graph_backend, expr_t>);
    STATIC_REQUIRE(lithe::lowering::ExpressionLoweringBackend<lithe::lowering::tree_backend, expr_t>);

    STATIC_REQUIRE(lithe::lowering::GraphLoweringBackend<lithe::lowering::graph_backend, expr_t>);
    STATIC_REQUIRE(lithe::lowering::TreeLoweringBackend<lithe::lowering::tree_backend, expr_t>);

    STATIC_REQUIRE(lithe::lowering::LoweringBackend<lithe::lowering::graph_backend, expr_t>);
    STATIC_REQUIRE(lithe::lowering::LoweringBackend<lithe::lowering::tree_backend, expr_t>);
}

TEST_CASE (



"Lithe lowering extended backend concepts accept dataflow and codegen adapters"
,
"[lithe][lowering]"
)
 {
    STATIC_REQUIRE(lithe::lowering::BackendModuleProtocol<mock_dataflow_backend>);
    STATIC_REQUIRE(lithe::lowering::DataflowLoweringBackend<mock_dataflow_backend, expr_t>);

    STATIC_REQUIRE(lithe::lowering::BackendModuleProtocol<mock_codegen_backend>);
    STATIC_REQUIRE(lithe::lowering::CodegenBackend<mock_codegen_backend, expr_t>);
}

TEST_CASE (



"Lithe lowering common protocol methods are callable"
,
"[lithe][lowering]"
)
 {
    lithe::lowering::lowering_context ctx;
    lithe::lowering::graph_backend graph;
    lithe::lowering::tree_backend tree;

    graph.begin_module(ctx);
    const auto g_term = graph.emit_terminal(7);
    const auto g_op = graph.emit_operation("add");
    graph.connect(g_op, g_term);
    graph.end_module();

    tree.begin_module(ctx);
    const auto t_term = tree.emit_terminal(3);
    const auto t_op = tree.emit_operation("mul");
    tree.connect(t_op, t_term);
    tree.end_module();

    REQUIRE(g_term > 0);
    REQUIRE(g_op > 0);
    REQUIRE(t_term > 0);
    REQUIRE(t_op > 0);
}

TEST_CASE (



"Lithe graph/tree backends still lower expressions with cleaned concepts"
,
"[lithe][lowering]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(1, 2);
    lithe::lowering::lowering_context ctx;

    lithe::lowering::graph_backend graph_backend;
    auto graph_lowered = graph_backend.lower(expr, ctx);
    REQUIRE(lithe::structural_hash(graph_lowered) != 0);

    lithe::lowering::tree_backend tree_backend;
    auto tree_lowered = tree_backend.lower(expr, ctx);
    REQUIRE(!tree_lowered.empty());
}

TEST_CASE (



"Lithe symbolic DAG lowering is recursive and stores structural hashes"
,
"[lithe][lowering][dag]"
)
 {
    auto left = lithe::make_node<lithe::mul_tag>(2, 3);
    auto right = lithe::make_node<lithe::sub_tag>(4, 1);
    auto expr = lithe::make_node<lithe::add_tag>(left, right);

    lithe::backend::symbolic_dag_builder builder;
    auto root_id = lithe::visit(expr, builder);

    REQUIRE(builder.dag.node_count() == 7);
    REQUIRE(builder.dag.edge_count() == 6);

    const auto h_left = lithe::structural_hash(left);
    const auto h_right = lithe::structural_hash(right);
    const auto h_root = lithe::structural_hash(expr);

    REQUIRE(builder.hash_to_vertex.contains(h_left));
    REQUIRE(builder.hash_to_vertex.contains(h_right));
    REQUIRE(builder.hash_to_vertex.contains(h_root));

    const auto &left_node = builder.dag.node_data(builder.hash_to_vertex.at(h_left));
    const auto &right_node = builder.dag.node_data(builder.hash_to_vertex.at(h_right));
    const auto &root_node = builder.dag.node_data(root_id);

    REQUIRE(left_node.name == "mul");
    REQUIRE(right_node.name == "sub");
    REQUIRE(root_node.name == "add");
    REQUIRE(left_node.structural_hash == h_left);
    REQUIRE(right_node.structural_hash == h_right);
    REQUIRE(root_node.structural_hash == h_root);
}

TEST_CASE (



"Lithe symbolic DAG lowering reuses nodes for repeated subexpressions"
,
"[lithe][lowering][dag]"
)
 {
    auto repeated = lithe::make_node<lithe::mul_tag>(2, 3);
    auto expr = lithe::make_node<lithe::add_tag>(repeated, repeated);

    lithe::backend::symbolic_dag_builder builder;
    auto root_id = lithe::visit(expr, builder);

    REQUIRE(builder.dag.node_count() == 4);
    REQUIRE(builder.dag.edge_count() == 4);

    const auto h_repeated = lithe::structural_hash(repeated);
    const auto h_root = lithe::structural_hash(expr);
    REQUIRE(builder.hash_to_vertex.contains(h_repeated));
    REQUIRE(builder.hash_to_vertex.contains(h_root));

    const auto mul_id = builder.hash_to_vertex.at(h_repeated);
    const auto &mul_node = builder.dag.node_data(mul_id);
    const auto &root_node = builder.dag.node_data(root_id);

    REQUIRE(mul_node.name == "mul");
    REQUIRE(mul_node.structural_hash == h_repeated);
    REQUIRE(root_node.name == "add");
    REQUIRE(root_node.structural_hash == h_root);
}

TEST_CASE (



"Lithe graph IR analysis reports sharing and canonical order"
,
"[lithe][lowering][graph-ir]"
)
 {
    auto repeated = lithe::make_node<lithe::mul_tag>(2, 3);
    auto expr = lithe::make_node<lithe::add_tag>(repeated, repeated);

    lithe::backend::symbolic_dag_builder builder;
    const auto root = lithe::visit(expr, builder);

    const auto analysis = lithe::lowering::analyze_graph_ir(builder.dag, std::vector{root});
    REQUIRE(analysis.node_count == builder.dag.node_count());
    REQUIRE(analysis.reusable_node_count >= 1);
    REQUIRE(analysis.shared_subtree_count >= 1);
    REQUIRE_FALSE(analysis.canonical_order.empty());
}

TEST_CASE (



"Lithe graph IR optimization removes dead nodes and duplicate dependencies"
,
"[lithe][lowering][graph-ir]"
)
 {
    lithe::backend::SymbolicDAG dag;

    lithe::backend::SymbolicExpression lhs{};
    lhs.type = lithe::backend::SymbolicExpression::Type::Constant;
    lhs.name = "const";
    lhs.expr_id = 1;
    lhs.structural_hash = 11;
    lhs.category = lithe::backend::operation_category::terminal;

    lithe::backend::SymbolicExpression rhs = lhs;
    rhs.expr_id = 2;
    rhs.structural_hash = 12;

    lithe::backend::SymbolicExpression root_expr{};
    root_expr.type = lithe::backend::SymbolicExpression::Type::Operation;
    root_expr.name = "add";
    root_expr.expr_id = 3;
    root_expr.structural_hash = 33;
    root_expr.category = lithe::backend::operation_category::arithmetic;

    lithe::backend::SymbolicExpression dead_expr = lhs;
    dead_expr.expr_id = 4;
    dead_expr.structural_hash = 44;

    const auto n0 = dag.add_node(lhs);
    const auto n1 = dag.add_node(rhs);
    const auto n2 = dag.add_node(root_expr);
    dag.add_node(dead_expr);

    lithe::backend::DependencyEdge dep{};
    dep.dep_type = lithe::backend::DependencyEdge::Type::DataFlow;
    dep.kind = lithe::backend::dependency_kind::data;
    dep.structural_hash = 101;

    dag.add_edge(n0, n2, dep);
    dag.add_edge(n0, n2, dep); // duplicate edge to be simplified
    dag.add_edge(n1, n2, dep);

    const auto optimized = lithe::lowering::optimize_graph_ir(std::move(dag), std::vector{n2});
    REQUIRE(optimized.removed_dead_nodes >= 1);
    REQUIRE(optimized.simplified_dependencies >= 1);
    REQUIRE(optimized.after.node_count <= optimized.before.node_count);
}

TEST_CASE (



"Lithe lowering accepts preset v2 objects"
,
"[lithe][lowering][preset]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(0, 5);

    auto graph_result = lithe::lowering::compile_to_graph(expr, lithe::preset::O1{});
    REQUIRE(graph_result.ok());
    REQUIRE(graph_result.output.has_value());

    auto composed = lithe::preset::compose(lithe::preset::O1{}).with(lithe::passes::canonicalize_commutative_pass{});
    auto tree_result = lithe::lowering::compile_to_tree(expr, composed);
    REQUIRE(tree_result.ok());
    REQUIRE(tree_result.output.has_value());
    REQUIRE(tree_result.output->size() >= 1);
}

TEST_CASE (



"Lithe lowering pipeline v2 compile returns lowering artifact"
,
"[lithe][lowering][pipeline]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(4, 5);
    auto pipeline = lithe::lowering::make_pipeline(
        lithe::compiler::opt_level::O1,
        lithe::passes::canonicalize_commutative_pass{}
    );

    auto compiled = lithe::lowering::compile(expr, pipeline);
    REQUIRE(compiled.ok());
    REQUIRE(compiled.output.has_value());
    REQUIRE(compiled.output->ir_hash != 0);
    REQUIRE(compiled.output->context.input_hash != 0);
    REQUIRE(!compiled.context.stage_trace.empty());
}

TEST_CASE (



"Lithe lowering pipeline v2 supports compile_to_dataflow"
,
"[lithe][lowering][pipeline]"
)
 {
    auto expr = lithe::make_node<lithe::mul_tag>(3, 7);

    auto dataflow_result = lithe::lowering::compile_to_dataflow(expr, lithe::compiler::opt_level::O1);
    REQUIRE(dataflow_result.ok());
    REQUIRE(dataflow_result.output.has_value());
    REQUIRE(dataflow_result.output->node_count() >= 1);

    auto default_dataflow = lithe::lowering::compile_to_dataflow(expr);
    REQUIRE(default_dataflow.ok());
    REQUIRE(default_dataflow.output.has_value());
}

TEST_CASE (



"Lithe lowering pipeline v2 adds default compile_to_graph and compile_to_tree overloads"
,
"[lithe][lowering][pipeline]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(1, 9);

    auto graph_result = lithe::lowering::compile_to_graph(expr);
    REQUIRE(graph_result.ok());
    REQUIRE(graph_result.output.has_value());

    auto tree_result = lithe::lowering::compile_to_tree(expr);
    REQUIRE(tree_result.ok());
    REQUIRE(tree_result.output.has_value());
}

TEST_CASE (



"Lithe lowering graph IR optimized compile pipeline"
,
"[lithe][lowering][graph-ir]"
)
 {
    auto repeated = lithe::make_node<lithe::mul_tag>(2, 3);
    auto expr = lithe::make_node<lithe::add_tag>(repeated, repeated);

    auto optimized = lithe::lowering::compile_to_graph_optimized(expr, lithe::compiler::opt_level::O1);
    REQUIRE(optimized.ok());
    REQUIRE(optimized.output.has_value());
    REQUIRE(optimized.output->graph.node_count() >= 1);
    REQUIRE(optimized.output->before.node_count >= optimized.output->after.node_count);
    REQUIRE(optimized.output->applied_passes.size() == 5);
}

TEST_CASE (



"Lithe symbolic dataflow engine tracks dependencies and builds schedule"
,
"[lithe][lowering][dataflow-ir]"
)
 {
    auto repeated = lithe::make_node<lithe::mul_tag>(2, 3);
    auto expr = lithe::make_node<lithe::add_tag>(repeated, repeated);

    lithe::backend::symbolic_dag_builder builder;
    const auto root = lithe::visit(expr, builder);

    using engine_t = lithe::lowering::symbolic_dataflow_engine<lithe::backend::SymbolicDAG>;
    const auto deps = engine_t::track_dependencies(builder.dag);
    REQUIRE(deps.dependencies.contains(root));
    REQUIRE(deps.dependencies.at(root).size() >= 1);

    const auto schedule = engine_t::build_schedule(builder.dag);
    REQUIRE_FALSE(schedule.empty());
    REQUIRE(schedule.size() == builder.dag.node_count());

    const auto ir = engine_t::build_ir(builder.dag, std::vector{root});
    REQUIRE(ir.analysis.node_count == builder.dag.node_count());
    REQUIRE_FALSE(ir.region.exit_nodes.empty());
}

TEST_CASE (



"Lithe symbolic dataflow engine supports lazy evaluation and dependency pruning"
,
"[lithe][lowering][dataflow-ir]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::mul_tag>(2, 3),
        lithe::make_node<lithe::sub_tag>(8, 1)
    );

    lithe::backend::symbolic_dag_builder builder;
    const auto root = lithe::visit(expr, builder);

    using graph_t = lithe::backend::SymbolicDAG;
    using engine_t = lithe::lowering::symbolic_dataflow_engine<graph_t>;

    std::unordered_map<litegraph::NodeId, int> cache;
    int eval_calls = 0;
    auto evaluator = [&](const graph_t &g, litegraph::NodeId node, const std::vector<int> &inputs) {
        ++eval_calls;
        const auto &payload = g.node_data(node);
        if (payload.name == "const") {
            if (std::holds_alternative<int>(payload.value)) {
                return std::get<int>(payload.value);
            }
            return static_cast<int>(std::get<double>(payload.value));
        }
        if (payload.name == "add") return inputs[0] + inputs[1];
        if (payload.name == "sub") return inputs[0] - inputs[1];
        if (payload.name == "mul") return inputs[0] * inputs[1];
        if (payload.name == "div") return inputs[0] / inputs[1];
        return 0;
    };

    const int first = engine_t::lazy_evaluate<int>(builder.dag, root, evaluator, cache);
    const int second = engine_t::lazy_evaluate<int>(builder.dag, root, evaluator, cache);
    REQUIRE(first == 13);
    REQUIRE(second == 13);
    REQUIRE(eval_calls == static_cast<int>(builder.dag.node_count()));

    auto dag_copy = builder.dag;
    const auto pruned = engine_t::prune_dependencies(dag_copy, [](litegraph::NodeId from, litegraph::NodeId to, const auto &) {
        return from.value != to.value && from.value % 2 == 0;
    });
    REQUIRE(pruned >= 1);
}

TEST_CASE (



"Lithe compile_to_symbolic_dataflow returns reusable dataflow IR"
,
"[lithe][lowering][dataflow-ir]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(
        lithe::make_node<lithe::mul_tag>(2, 3),
        lithe::make_node<lithe::sub_tag>(9, 4)
    );

    auto out = lithe::lowering::compile_to_symbolic_dataflow(expr, lithe::compiler::opt_level::O1);
    REQUIRE(out.ok());
    REQUIRE(out.output.has_value());
    REQUIRE(out.output->analysis.node_count >= 1);
    REQUIRE_FALSE(out.output->schedule.empty());
    REQUIRE_FALSE(out.output->region.nodes.empty());
}

TEST_CASE (



"Lithe observability hooks collect compile trace and pass timings"
,
"[lithe][lowering][observability]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(3, 4);
    auto pipeline = lithe::lowering::make_pipeline(
        lithe::compiler::opt_level::O1,
        lithe::passes::canonicalize_commutative_pass{}
    );

    lithe::lowering::observability::trace_observer observer;
    auto out = lithe::lowering::compile_observed<true>(expr, pipeline, lithe::lowering::graph_backend{}, observer);

    REQUIRE(out.ok());
    REQUIRE(!observer.trace.compilation_events.empty());
    REQUIRE(!observer.trace.pass_events.empty());
    REQUIRE(!observer.trace.pass_timings.empty());
    REQUIRE(!observer.trace.lowering_events.empty());
    REQUIRE(observer.trace.hash_stats.input_hash != 0);
}

TEST_CASE (



"Lithe observability hooks compile away cleanly when disabled"
,
"[lithe][lowering][observability]"
)
 {
    auto expr = lithe::make_node<lithe::mul_tag>(5, 6);
    auto pipeline = lithe::lowering::make_pipeline(lithe::compiler::opt_level::O1);

    lithe::lowering::observability::trace_observer observer;
    auto out = lithe::lowering::compile_observed<false>(expr, pipeline, lithe::lowering::graph_backend{}, observer);

    REQUIRE(out.ok());
    REQUIRE(observer.trace.compilation_events.empty());
    REQUIRE(observer.trace.pass_events.empty());
    REQUIRE(observer.trace.rewrite_events.empty());
    REQUIRE(observer.trace.diagnostic_events.empty());
}

TEST_CASE (



"Lithe observability hooks collect rewrite statistics for graph optimization"
,
"[lithe][lowering][observability]"
)
 {
    auto repeated = lithe::make_node<lithe::mul_tag>(2, 3);
    auto expr = lithe::make_node<lithe::add_tag>(repeated, repeated);

    lithe::lowering::observability::trace_observer observer;
    auto out = lithe::lowering::compile_to_graph_optimized_observed<true>(
        expr,
        lithe::compiler::opt_level::O1,
        observer
    );

    REQUIRE(out.ok());
    REQUIRE(!observer.trace.rewrite_events.empty());
    REQUIRE(observer.trace.rewrite_stats.rewrites_attempted >= observer.trace.rewrite_stats.rewrites_applied);
}

TEST_CASE (



"Lithe frontend adapter v2 wraps raw parser output into frontend_result"
,
"[lithe][lowering][frontend]"
)
 {
    STATIC_REQUIRE(lithe::lowering::FrontendAdapter<raw_frontend_adapter, std::string_view>);

    auto out = lithe::lowering::parse_to_lithe(raw_frontend_adapter{}, std::string_view{"ignored"});
    REQUIRE(out.kind == lithe::lowering::frontend_kind::handwritten);
    REQUIRE(out.ok());
    REQUIRE(out.ir.has_value());
    REQUIRE(lithe::structural_equal(*out.ir, lithe::make_node<lithe::add_tag>(1, 2)));
}

TEST_CASE (



"Lithe frontend adapter v2 propagates diagnostics and source spans"
,
"[lithe][lowering][frontend]"
)
 {
    STATIC_REQUIRE(lithe::lowering::FrontendAdapter<diagnostic_frontend_adapter, const char *>);

    auto out = lithe::lowering::parse_to_lithe(diagnostic_frontend_adapter{}, "{\"op\":\"mul\"}");
    REQUIRE(out.kind == lithe::lowering::frontend_kind::json_ast);
    REQUIRE(out.ok());
    REQUIRE(out.ir.has_value());
    REQUIRE(out.span.has_value());
    REQUIRE(out.span->line == 3);
    REQUIRE(out.span->column == 2);
    REQUIRE(out.diagnostics.size() == 1);
}

TEST_CASE (



"Lithe frontend adapter v2 normalize_to_lithe_ir uses adapter hooks"
,
"[lithe][lowering][frontend]"
)
 {
    auto out = lithe::lowering::parse_to_lithe(normalizing_frontend_adapter{}, std::string_view{"dsl-input"});
    REQUIRE(out.kind == lithe::lowering::frontend_kind::external_dsl);
    REQUIRE(out.ok());
    REQUIRE(out.ir.has_value());
    REQUIRE(*out.ir == "dsl-input::normalized");
}

TEST_CASE (



"Lithe frontend framework v2 propagates context diagnostics and source-to-expression mappings"
,
"[lithe][lowering][frontend]"
)
 {
    lithe::lowering::frontend_context ctx;
    auto out = lithe::lowering::parse_to_lithe(mapped_expr_frontend_adapter{}, std::string_view{"{\"op\":\"add\"}"}, ctx);

    REQUIRE(out.ok());
    REQUIRE(out.ir.has_value());
    REQUIRE(ctx.kind == lithe::lowering::frontend_kind::json_ast);
    REQUIRE(ctx.files.size() == 1);
    REQUIRE(ctx.diagnostics.size() == 1);

    const auto key = lithe::structural_key(*out.ir);
    const auto mapping = ctx.mapping_for_expression(key);
    REQUIRE(mapping.has_value());
    REQUIRE(mapping->span.offset == 1);
    REQUIRE(mapping->span.length == 3);
}

TEST_CASE (



"Lithe frontend framework v2 provides parser-independent AST normalization and transforms"
,
"[lithe][lowering][frontend]"
)
 {
    lithe::lowering::frontend_ast ast;
    ast.node_kind = "binary_op";
    ast.span = lithe::lowering::source_span{4, 2, 1, 5};
    ast.children.push_back(lithe::lowering::frontend_ast{"literal", "", {}, {}, lithe::lowering::source_span{4, 1, 1, 5}});

    auto normalized = lithe::lowering::normalize_frontend_ast(std::move(ast));
    REQUIRE_FALSE(normalized.node_id.empty());
    REQUIRE(normalized.children.size() == 1);
    REQUIRE_FALSE(normalized.children.front().node_id.empty());

    lithe::lowering::frontend_context ctx;
    lithe::lowering::frontend_transform<> annotate_transform{
        "annotate-kind",
        [](const lithe::lowering::frontend_ast &in, lithe::lowering::frontend_context &context) {
            auto out = in;
            context.add_diagnostic(lithe::lowering::frontend_diagnostic{
                lithe::lowering::frontend_diagnostic::level::info,
                "transform:" + in.node_kind,
                in.span
            });
            return out;
        }
    };

    auto transformed = annotate_transform(normalized, ctx);
    REQUIRE(transformed.node_kind == "binary_op");
    REQUIRE(ctx.diagnostics.size() == 1);
}

TEST_CASE (



"Lithe compatibility lowering header graph/tree compile"
,
"[lithe][compat][lowering]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(1, 2);

    auto graph_result = lithe::lowering::compile_to_graph(expr, lithe::preset::O0{});
    REQUIRE(graph_result.ok());
    REQUIRE(graph_result.output.has_value());

    auto tree_result = lithe::lowering::compile_to_tree(expr, lithe::preset::O0{});
    REQUIRE(tree_result.ok());
    REQUIRE(tree_result.output.has_value());
    REQUIRE(!tree_result.output->empty());
}

TEST_CASE (



"Lithe compatibility umbrella header old API"
,
"[lithe][compat][umbrella]"
)
 {
    auto expr = lithe::make_node<lithe::add_tag>(0, 5);

    auto out = lithe::compile(expr, lithe::passes::simplify_add_zero_pass{});
    REQUIRE(lithe::structural_equal(out, 5));

    auto preset_out = lithe::preset::O1{}(expr);
    REQUIRE(lithe::structural_hash(preset_out) != 0);

    lithe::semantic::annotate(expr, lithe::semantic::annotation{
        lithe::semantic::semantic_key::domain,
        lithe::semantic::domain_type::arithmetic
    });
    REQUIRE(lithe::semantic::get_semantics(expr).has_value());
}

TEST_CASE (



"Codegen machine IR dump optionally includes register pressure"
,
"[lithe][lowering][codegen]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "pressure_dump_toggle";
    auto &entry = fn.create_block("entry");
    const auto vr = fn.make_vreg();

    instruction li;
    li.op = opcode::load_imm;
    li.defs = {operand::as_vreg(vr)};
    li.uses = {operand::as_i64(7)};
    (void) fn.emit(entry.id, std::move(li));

    instruction ret;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(vr)};
    (void) fn.emit(entry.id, std::move(ret));

    const auto dump_default = dump_machine_ir(fn);
    const auto dump_with_pressure = dump_machine_ir(fn, true);

    REQUIRE(dump_default.find("register-pressure") == std::string::npos);
    REQUIRE(dump_with_pressure.find("register-pressure") != std::string::npos);
}

TEST_CASE (



"Codegen scheduling metadata placeholders are available and updatable"
,
"[lithe][lowering][codegen]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "scheduling_metadata_placeholders";
    auto &entry = fn.create_block("entry");
    const auto v0 = fn.make_vreg();
    const auto v1 = fn.make_vreg();

    instruction li;
    li.op = opcode::load_imm;
    li.defs = {operand::as_vreg(v0)};
    li.uses = {operand::as_i64(3)};
    const auto li_id = fn.emit(entry.id, std::move(li)).id;

    instruction add;
    add.op = opcode::add;
    add.defs = {operand::as_vreg(v1)};
    add.uses = {operand::as_vreg(v0), operand::as_i64(9)};
    const auto add_id = fn.emit(entry.id, std::move(add)).id;

    instruction ret;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(v1)};
    const auto ret_id = fn.emit(entry.id, std::move(ret)).id;

    const auto computed = compute_scheduling_metadata(fn);
    REQUIRE(computed.instruction_metadata.size() == 3);
    REQUIRE(computed.statistics.contains("instruction_count"));
    REQUIRE(computed.statistics.contains("dependency_edge_count"));

    const auto dump = dump_scheduling_metadata(computed);
    REQUIRE(dump.find("scheduling-metadata") != std::string::npos);
    REQUIRE(dump.find("dependency_edges=") != std::string::npos);

    const auto queried_before = query_instruction_scheduling_metadata(fn, add_id);
    REQUIRE(queried_before.has_value());

    instruction_scheduling_metadata custom;
    custom.latency = 6;
    custom.throughput = 0.5;
    custom.scheduling_class = "test_custom";
    custom.scheduling_groups = {"group_a"};
    custom.scheduling_priority = 42;
    custom.hazard_flags = {scheduling_hazard_flag::read_after_write};
    custom.scheduling_constraints = {scheduling_constraint{"must_follow", "li"}};
    REQUIRE(update_instruction_scheduling_metadata(fn, add_id, custom));
    REQUIRE(annotate_instruction_scheduling_metadata(fn, add_id, "note", "manual"));

    const auto queried_after = query_instruction_scheduling_metadata(fn, add_id);
    REQUIRE(queried_after.has_value());
    REQUIRE(queried_after->latency == 6);
    REQUIRE(queried_after->annotations.contains("note"));

    const auto updated = update_scheduling_metadata(fn);
    REQUIRE_FALSE(updated.update_log.empty());

    const auto validated = validate_scheduling_metadata(fn);
    REQUIRE(validated.ok());
    REQUIRE(validated.diagnostics.empty());

    const auto virtual_fn = mir::virtual_mir_function{fn};
    REQUIRE(dump_scheduling_metadata(virtual_fn).find("scheduling-metadata") != std::string::npos);
    REQUIRE(validate_scheduling_metadata(virtual_fn).ok());

    REQUIRE(li_id != 0);
    REQUIRE(ret_id != 0);
}

TEST_CASE (



"Codegen compile_to_physical_mir keeps peephole disabled by default"
,
"[lithe][lowering][codegen][peephole]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);
    const auto compiled = compile_to_physical_mir(expr);

    REQUIRE(compiled.ok());
    REQUIRE_FALSE(compiled.peephole.has_value());
    REQUIRE(verify_physical_mir(compiled.physical_mir).ok());
}

TEST_CASE (



"Codegen compile_to_physical_mir stores peephole result when enabled"
,
"[lithe][lowering][codegen][peephole]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<mul_tag>(
        make_node<add_tag>(1, 2),
        3
    );

    codegen_options options;
    options.enable_peephole = true;

    const auto compiled = compile_to_physical_mir(expr, options);
    REQUIRE(compiled.ok());
    REQUIRE(compiled.peephole.has_value());
    REQUIRE(compiled.peephole->ok());
    REQUIRE(verify_physical_mir(compiled.physical_mir).ok());

    const auto baseline = compile_to_physical_mir(expr);
    REQUIRE(baseline.ok());
    if (compiled.peephole->changed) {
        REQUIRE(dump_physical_mir(compiled.physical_mir) != dump_physical_mir(baseline.physical_mir));
    }
}

TEST_CASE (



"Codegen MIR diagnostics report unresolved spill in normal instruction"
,
"[lithe][lowering][codegen][mir][negative]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction inst;
    inst.id = 9001;
    inst.op = opcode::add;
    inst.defs = {allocated_operand::as_spill(test_spill_slot(1))};
    inst.uses = {allocated_operand::as_spill(test_spill_slot(1)), allocated_operand::as_preg({1, "r1"})};

    const auto physical = make_test_physical("diag_unresolved_spill", {inst});
    const auto verification = verify_physical_mir(physical);

    REQUIRE_FALSE(verification.ok());
    REQUIRE_FALSE(verification.diagnostics.empty());
}

TEST_CASE (



"Codegen MIR diagnostics report duplicate instruction ids"
,
"[lithe][lowering][codegen][mir][negative]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction li;
    li.id = 9002;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(7)};

    allocated_instruction ret;
    ret.id = 9002;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    const auto physical = make_test_physical("diag_duplicate_inst_ids", {li, ret});
    const auto verification = verify_physical_mir(physical);

    REQUIRE_FALSE(verification.ok());
    REQUIRE_FALSE(verification.diagnostics.empty());
}

TEST_CASE (



"Codegen MIR diagnostics report invalid branch target"
,
"[lithe][lowering][codegen][mir][negative]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction branch;
    branch.id = 9003;
    branch.op = opcode::branch;
    branch.uses = {allocated_operand::as_block(999)};

    auto physical = make_test_physical("diag_invalid_branch_target", {branch});
    physical.function.blocks[0].successors = {999};

    const auto verification = verify_physical_mir(physical);
    REQUIRE_FALSE(verification.ok());
    REQUIRE_FALSE(verification.diagnostics.empty());
}

TEST_CASE (



"Codegen MIR diagnostics report invalid load_spill operand shape"
,
"[lithe][lowering][codegen][mir][negative]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction inst;
    inst.id = 9004;
    inst.op = opcode::load_spill;
    inst.defs = {allocated_operand::as_memory(make_memory_operand_for_spill_slot(test_spill_slot(2)))};
    inst.uses = {allocated_operand::as_i64(0)};

    const auto physical = make_test_physical("diag_bad_load_spill_shape", {inst});
    const auto verification = verify_physical_mir(physical);

    REQUIRE_FALSE(verification.ok());
    REQUIRE_FALSE(verification.diagnostics.empty());
}

TEST_CASE (



"Codegen MIR diagnostics report invalid store_spill operand shape"
,
"[lithe][lowering][codegen][mir][negative]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction inst;
    inst.id = 9005;
    inst.op = opcode::store_spill;
    inst.defs = {allocated_operand::as_preg({5, "r5"})};
    inst.uses = {allocated_operand::as_i64(1)};

    const auto physical = make_test_physical("diag_bad_store_spill_shape", {inst});
    const auto verification = verify_physical_mir(physical);

    REQUIRE_FALSE(verification.ok());
    REQUIRE_FALSE(verification.diagnostics.empty());
}

TEST_CASE (



"Codegen MIR diagnostics report invalid load_arg index"
,
"[lithe][lowering][codegen][mir][negative]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction arg;
    arg.id = 9006;
    arg.op = opcode::load_arg;
    arg.defs = {allocated_operand::as_preg({2, "a2"})};
    arg.uses = {allocated_operand::as_argument_index(2)};

    allocated_instruction ret;
    ret.id = 9007;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({2, "a2"})};

    auto physical = make_test_physical("diag_invalid_load_arg_index", {arg, ret});
    function_signature signature;
    signature.name = "diag_invalid_load_arg_index";
    signature.arguments = {argument_descriptor{"only0"}};
    physical.signature = signature;

    const auto convention_check = validate_calling_convention(physical, signature);
    REQUIRE_FALSE(convention_check.ok());
    REQUIRE_FALSE(convention_check.diagnostics.empty());
}

TEST_CASE (



"Codegen compile_and_emit returns failure when codegen has diagnostics"
,
"[lithe][lowering][codegen][mir][negative]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    struct passthrough_backend {
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state &state) {
            return backend_result::success_result(state);
        }
    };

    // Use a wide n-ary expression to increase register pressure, then disable spill rewrite
    // so unresolved spill operands are surfaced as diagnostics before emission.
    const auto expr = make_node<add_tag>(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);

    codegen_options options;
    options.enable_spill_rewrite = false;

    passthrough_backend backend;
    const auto result = compile_and_emit(expr, backend, options);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
}

TEST_CASE (



"Codegen backend capability inference marks required MIR features"
,
"[lithe][lowering][codegen][capabilities]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction li;
    li.id = 9101;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({1, "r1"})};
    li.uses = {allocated_operand::as_i64(1)};

    allocated_instruction load_mem;
    load_mem.id = 9102;
    load_mem.op = opcode::load;
    load_mem.defs = {allocated_operand::as_preg({2, "r2"})};
    load_mem.uses = {allocated_operand::as_memory(make_memory_operand_for_spill_slot(test_spill_slot(3)))};

    allocated_instruction call;
    call.id = 9103;
    call.op = opcode::call;
    call.uses = {allocated_operand::as_preg({2, "r2"})};

    allocated_instruction branch;
    branch.id = 9104;
    branch.op = opcode::branch;
    branch.uses = {allocated_operand::as_block(2)};

    auto physical = make_test_physical("capability_inference", {li, load_mem, call, branch});
    allocated_basic_block target;
    target.id = 2;
    target.name = "exit";
    physical.function.blocks.push_back(target);

    const auto required = required_backend_features(physical);
    REQUIRE(required.has(backend_feature::integer_arithmetic));
    REQUIRE(required.has(backend_feature::memory_operands));
    REQUIRE(required.has(backend_feature::spill_load_store));
    REQUIRE(required.has(backend_feature::branches));
    REQUIRE(required.has(backend_feature::calls));
    REQUIRE(required.has(backend_feature::stack_frame));
}

TEST_CASE (



"Codegen emission validates declared backend capabilities"
,
"[lithe][lowering][codegen][capabilities]"
)
 {
    using namespace lithe::codegen;

    struct limited_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({backend_feature::integer_arithmetic});
        }

        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state &state) {
            return backend_result::success_result(state);
        }
    };

    allocated_instruction load_mem;
    load_mem.id = 9201;
    load_mem.op = opcode::load;
    load_mem.defs = {allocated_operand::as_preg({1, "r1"})};
    load_mem.uses = {allocated_operand::as_memory(make_memory_operand_for_spill_slot(test_spill_slot(1)))};

    allocated_instruction ret;
    ret.id = 9202;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({1, "r1"})};

    const auto physical = make_test_physical("capability_emit_validation", {load_mem, ret});
    const auto validation = validate_backend_capabilities(physical, limited_backend::capabilities());
    REQUIRE_FALSE(validation.ok());
    REQUIRE_FALSE(validation.diagnostics.empty());

    limited_backend backend;
    const auto result = emit_function(backend, physical);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
}

TEST_CASE (



"Codegen interpreter backend rejects unsupported features via capability diagnostics"
,
"[lithe][lowering][codegen][capabilities]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction call;
    call.id = 9301;
    call.op = opcode::call;
    call.uses = {allocated_operand::as_preg({1, "r1"})};

    const auto physical = make_test_physical("interpreter_capability_reject", {call});

    backends::interpreter_backend backend;
    const auto result = emit_function(backend, physical);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    REQUIRE(result.errors.front().message.find("backend capability missing: feature=calls") != std::string::npos);
}

// T5: ast_builder::on_node must attach child subtrees, not just copy leaf data
TEST_CASE (



"ast_builder builds tree with correct child and grandchild structure"
,
"[lithe][lowering][ast_builder]"
)
 {
    using namespace lithe;
    using namespace lithe::backend;

    // Build add(mul(1, 2), 3) through ast_builder via visit.
    // Expected tree:
    //   add
    //   ├── mul
    //   │   ├── terminal(1)
    //   │   └── terminal(2)
    //   └── terminal(3)
    const auto expr = make_node<add_tag>(make_node<mul_tag>(1, 2), 3);

    ast_builder builder;
    visit(expr, builder);
    builder.finalize();

    const ASTTree &tree = builder.tree;
    ASTNode *root = tree.get_root();

    REQUIRE(root != nullptr);
    REQUIRE(root->data.operation == "add");
    REQUIRE(root->child_count() == 2);

    ASTNode *mul_child = root->children[0].get();
    ASTNode *term3_child = root->children[1].get();

    REQUIRE(mul_child != nullptr);
    REQUIRE(mul_child->data.operation == "mul");
    REQUIRE(mul_child->child_count() == 2);

    REQUIRE(term3_child != nullptr);
    REQUIRE(term3_child->data.operation == "terminal");
    REQUIRE(term3_child->child_count() == 0);

    ASTNode *term1 = mul_child->children[0].get();
    ASTNode *term2 = mul_child->children[1].get();
    REQUIRE(term1 != nullptr);
    REQUIRE(term1->data.operation == "terminal");
    REQUIRE(term2 != nullptr);
    REQUIRE(term2->data.operation == "terminal");
}


// ============================================================================
// Finding 9: dependency_simplification keeps distinct dependencies
// ============================================================================

TEST_CASE (


"dependency_simplification keeps distinct dependencies"
,
"[lithe][lowering]"
)
 {
    using namespace lithe::backend;
    using namespace lithe::lowering;
    namespace lg = litegraph;

    // Use the canonical SymbolicDAG type used by lithe_lowering.
    using graph_t = SymbolicDAG;

    graph_t g;
    SymbolicExpression sa; sa.name = "a"; sa.expr_id = 0;
    sa.type = SymbolicExpression::Type::Variable;
    SymbolicExpression sb; sb.name = "b"; sb.expr_id = 1;
    sb.type = SymbolicExpression::Type::Variable;
    const auto na = g.add_node(sa);
    const auto nb = g.add_node(sb);

    // Two edges with different dependency_kind — both are structurally distinct.
    DependencyEdge ep1; ep1.kind = dependency_kind::data_raw;
    DependencyEdge ep2; ep2.kind = dependency_kind::data_war;
    g.add_edge(na, nb, ep1);
    g.add_edge(na, nb, ep2);

    // Neither must be removed — they differ.
    std::size_t removed = dependency_simplification(g);
    REQUIRE(removed == 0);

    std::size_t edge_count = 0;
    for ([[maybe_unused]] const auto& [eid, e] : g.edges()) ++edge_count;
    REQUIRE(edge_count == 2);

    // Add a truly duplicate edge (same from, to, same DependencyEdge fields).
    DependencyEdge ep3; ep3.kind = dependency_kind::data_raw; // identical to ep1
    g.add_edge(na, nb, ep3);

    std::size_t removed2 = dependency_simplification(g);
    REQUIRE(removed2 == 1);

    std::size_t edge_count2 = 0;
    for ([[maybe_unused]] const auto& [eid, e] : g.edges()) ++edge_count2;
    REQUIRE(edge_count2 == 2); // only 2 distinct edges remain
}
