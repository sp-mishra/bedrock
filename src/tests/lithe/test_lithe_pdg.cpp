#include "catch_amalgamated.hpp"

#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/lithe_lowering.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers (mirror the helpers in test_lithe_mir_cfg.cpp to avoid coupling)
// ─────────────────────────────────────────────────────────────────────────────
namespace {
    using namespace lithe::codegen;
    using namespace lithe::pdg;

    allocated_instruction make_branch(std::uint32_t id, std::uint32_t target) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::branch;
        i.uses = {allocated_operand::as_block(target)};
        return i;
    }

    allocated_instruction make_ret(std::uint32_t id) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::ret;
        return i;
    }

    allocated_instruction make_def_only(std::uint32_t id, preg dst) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::load_imm;
        i.defs = {allocated_operand::as_preg(dst)};
        i.uses = {allocated_operand::as_i64(42)};
        return i;
    }

    allocated_instruction make_use_only(std::uint32_t id, preg used) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::ret;
        i.uses = {allocated_operand::as_preg(used)};
        return i;
    }

    allocated_instruction make_mov(std::uint32_t id, preg dst, preg src) {
        allocated_instruction i;
        i.id = id;
        i.op = opcode::mov;
        i.defs = {allocated_operand::as_preg(dst)};
        i.uses = {allocated_operand::as_preg(src)};
        return i;
    }

    allocated_basic_block make_block(
        std::uint32_t id,
        std::vector<allocated_instruction> instructions,
        std::vector<std::uint32_t> successors = {}) {
        allocated_basic_block b;
        b.id = id;
        b.name = "bb" + std::to_string(id);
        b.instructions = std::move(instructions);
        b.successors = std::move(successors);
        return b;
    }

    mir::physical_mir_function make_physical(
        std::vector<allocated_basic_block> blocks,
        std::uint32_t entry_block) {
        allocated_function_ir fn;
        fn.name = "pdg_test";
        fn.blocks = std::move(blocks);
        fn.cfg.entry_block = entry_block;
        for (const auto& b : fn.blocks) {
            fn.cfg.successors[b.id] = b.successors;
            for (const auto s : b.successors)
                fn.cfg.predecessors[s].push_back(b.id);
        }
        mir::physical_mir_function out;
        out.function = std::move(fn);
        out.metadata.current_phase = mir::phase::physical_mir;
        return out;
    }

    // Inject a specific edge_kind onto a typed_edge in an existing cfg_analysis_result.
    cfg_analysis_result inject_edge_kind(
        const mir::physical_mir_function& fn,
        std::uint32_t from, std::uint32_t to,
        edge_kind kind) {
        mir_pass_context ctx;
        auto cfg = get_or_compute_cfg(ctx, fn);
        for (auto& te : cfg.typed_edges)
            if (te.from == from && te.to == to)
                te.kind = kind;
        cfg.partition = partition_execution_domains(cfg);
        return cfg;
    }
} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// DependencyEdge Phase 3 classification helpers
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"DependencyEdge: is_data_dependency covers DataFlow/AntiDep/OutputDep"
,
"[lithe][pdg][dep_edge]"
)
{
    using lithe::backend::DependencyEdge;
    using lithe::backend::dependency_kind;

    DependencyEdge data;
    data.dep_type = DependencyEdge::Type::DataFlow;
    REQUIRE(data.is_data_dependency());
    REQUIRE_FALSE(data.is_control_dependency());

    DependencyEdge anti;
    anti.dep_type = DependencyEdge::Type::AntiDep;
    anti.kind = dependency_kind::data_war;
    REQUIRE(anti.is_data_dependency());
    REQUIRE_FALSE(anti.is_control_dependency());

    DependencyEdge out;
    out.dep_type = DependencyEdge::Type::OutputDep;
    out.kind = dependency_kind::data_waw;
    REQUIRE(out.is_data_dependency());
}

TEST_CASE (



"DependencyEdge: is_control_dependency covers ControlFlow and control_direct"
,
"[lithe][pdg][dep_edge]"
)
{
    using lithe::backend::DependencyEdge;
    using lithe::backend::dependency_kind;

    DependencyEdge ctrl;
    ctrl.dep_type = DependencyEdge::Type::ControlFlow;
    REQUIRE(ctrl.is_control_dependency());
    REQUIRE_FALSE(ctrl.is_data_dependency());

    DependencyEdge direct;
    direct.kind = dependency_kind::control_direct;
    REQUIRE(direct.is_control_dependency());
}

TEST_CASE (



"DependencyEdge: fine-grained PDG kinds are distinguishable"
,
"[lithe][pdg][dep_edge]"
)
{
    using lithe::backend::dependency_kind;

    REQUIRE(dependency_kind::data_raw     != dependency_kind::data_war);
    REQUIRE(dependency_kind::data_waw     != dependency_kind::data_raw);
    REQUIRE(dependency_kind::data_raw_cross != dependency_kind::data_raw);
    REQUIRE(dependency_kind::control_direct != dependency_kind::control);
}

// ─────────────────────────────────────────────────────────────────────────────
// PDG node and edge construction
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"program_dependence_graph: add_node deduplicates by instr_id"
,
"[lithe][pdg][graph]"
)
{
    program_dependence_graph g;
    const auto idx0 = g.add_node({1u, 1u, 0u, false});
    const auto idx1 = g.add_node({1u, 1u, 0u, false}); // duplicate
    REQUIRE(idx0 == idx1);
    REQUIRE(g.node_count() == 1u);
}

TEST_CASE (



"program_dependence_graph: add_edge stores outgoing and incoming arcs"
,
"[lithe][pdg][graph]"
)
{
    program_dependence_graph g;
    g.add_node({10u, 1u, 0u, false});
    g.add_node({20u, 1u, 0u, false});
    g.add_edge(make_data_edge(10u, 20u, data_dep_kind::raw, 99u));

    REQUIRE(g.out_edges(10u).size() == 1u);
    REQUIRE(g.in_edges(20u).size()  == 1u);
    REQUIRE(g.out_edges(10u)[0].to_instr   == 20u);
    REQUIRE(g.out_edges(10u)[0].value_id   == 99u);
    REQUIRE(g.out_edges(10u)[0].is_data());
    REQUIRE(g.in_edges(20u)[0].from_instr  == 10u);
}

TEST_CASE (



"program_dependence_graph: add_edge with unknown endpoints is silently ignored"
,
"[lithe][pdg][graph]"
)
{
    program_dependence_graph g;
    g.add_node({1u, 1u, 0u, false});
    g.add_edge(make_data_edge(1u, 99u, data_dep_kind::raw)); // 99 doesn't exist
    REQUIRE(g.edge_count() == 0u);
}

TEST_CASE (



"program_dependence_graph: control edge is classified correctly"
,
"[lithe][pdg][graph]"
)
{
    program_dependence_graph g;
    g.add_node({5u, 1u, 0u, false});
    g.add_node({6u, 2u, 0u, false});
    g.add_edge(make_control_edge(5u, 6u));

    const auto &e = g.out_edges(5u)[0];
    REQUIRE(e.is_control());
    REQUIRE_FALSE(e.is_data());
}

TEST_CASE (



"program_dependence_graph: data_edges() range view returns only data arcs"
,
"[lithe][pdg][graph]"
)
{
    program_dependence_graph g;
    g.add_node({1u, 1u, 0u, false});
    g.add_node({2u, 1u, 0u, false});
    g.add_node({3u, 2u, 0u, false});
    g.add_edge(make_data_edge(1u, 2u, data_dep_kind::raw));
    g.add_edge(make_control_edge(2u, 3u));

    std::size_t data_cnt = 0, ctrl_cnt = 0;
    for (const auto &e : g.data_edges())    { (void)e; ++data_cnt; }
    for (const auto &e : g.control_edges()) { (void)e; ++ctrl_cnt; }
    REQUIRE(data_cnt == 1u);
    REQUIRE(ctrl_cnt == 1u);
}

TEST_CASE (



"program_dependence_graph: nodes_in_domain filters by domain_id"
,
"[lithe][pdg][graph]"
)
{
    program_dependence_graph g;
    g.add_node({1u, 1u, 0u, false});
    g.add_node({2u, 2u, 1u, false});
    g.add_node({3u, 2u, 1u, false});

    REQUIRE(g.nodes_in_domain(0u).size() == 1u);
    REQUIRE(g.nodes_in_domain(1u).size() == 2u);

    const auto ids = g.domain_ids();
    REQUIRE(ids.size() == 2u);
    REQUIRE(ids[0] == 0u);
    REQUIRE(ids[1] == 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// build_pdg_pass: sequential function (no partitioning)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"build_pdg_pass: all instructions of a linear function become PDG nodes"
,
"[lithe][pdg][build]"
)
{
    const preg r1{1, "r1"};
    // bb1: load_imm r1 (id=1), ret r1 (id=2)
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_use_only(2, r1)})
    }, 1);

    mir_pass_context ctx;
    const auto res = build_pdg(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.graph.node_count() == 2u);
    REQUIRE(res.graph.find_node(1u) != nullptr);
    REQUIRE(res.graph.find_node(2u) != nullptr);
}

TEST_CASE (



"build_pdg_pass: RAW data edge is emitted from def to use"
,
"[lithe][pdg][build]"
)
{
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_use_only(2, r1)})
    }, 1);

    mir_pass_context ctx;
    const auto res = build_pdg(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.data_edge_count >= 1u);

    // def (inst 1) → use (inst 2) with kind RAW
    const auto out = res.graph.out_edges(1u);
    bool found = false;
    for (const auto &e : out) {
        if (e.to_instr == 2u && e.is_data() &&
            (e.data_kind == data_dep_kind::raw ||
             e.data_kind == data_dep_kind::raw_cross_domain))
            found = true;
    }
    REQUIRE(found);
}

TEST_CASE (



"build_pdg_pass: WAW edge is emitted when same preg is defined twice"
,
"[lithe][pdg][build]"
)
{
    const preg r1{1, "r1"};
    // inst 1: def r1, inst 2: def r1 again, inst 3: ret r1
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_def_only(2, r1), make_use_only(3, r1)})
    }, 1);

    mir_pass_context ctx;
    const auto res = build_pdg(fn, ctx);

    REQUIRE(res.ok());
    // At least one WAW edge 1→2.
    bool found_waw = false;
    for (const auto &e : res.graph.out_edges(1u)) {
        if (e.to_instr == 2u && e.data_kind == data_dep_kind::waw)
            found_waw = true;
    }
    REQUIRE(found_waw);
}

TEST_CASE (



"build_pdg_pass: control edge emitted from branch to each successor instruction"
,
"[lithe][pdg][build]"
)
{
    // bb1: branch_cond to bb2 and bb3
    const preg r5{5, "cond"};
    allocated_instruction brc;
    brc.id = 10; brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};

    auto fn = make_physical({
        make_block(1, {brc},                 {2, 3}),
        make_block(2, {make_ret(20)}),
        make_block(3, {make_ret(30)})
    }, 1);

    mir_pass_context ctx;
    const auto res = build_pdg(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.control_edge_count >= 2u);

    // inst 10 should have control arcs to inst 20 and inst 30.
    const auto out = res.graph.out_edges(10u);
    bool to_20 = false, to_30 = false;
    for (const auto &e : out) {
        if (e.to_instr == 20u && e.is_control()) to_20 = true;
        if (e.to_instr == 30u && e.is_control()) to_30 = true;
    }
    REQUIRE(to_20);
    REQUIRE(to_30);
}

TEST_CASE (



"build_pdg_pass: no control edges for unconditional branch (single successor)"
,
"[lithe][pdg][build]"
)
{
    // Single successor → no control-dep boundary.
    auto fn = make_physical({
        make_block(1, {make_branch(1, 2)}, {2}),
        make_block(2, {make_ret(2)})
    }, 1);

    mir_pass_context ctx;
    const auto res = build_pdg(fn, ctx);

    REQUIRE(res.ok());
    // The branch instruction (id=1) must emit no control arcs.
    for (const auto &e : res.graph.out_edges(1u)) {
        REQUIRE_FALSE(e.is_control());
    }
}

TEST_CASE (



"build_pdg_pass: all nodes in a single-domain function have domain_id == 0"
,
"[lithe][pdg][build]"
)
{
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    mir_pass_context ctx;
    const auto res = build_pdg(fn, ctx);

    REQUIRE(res.ok());
    for (const auto &n : res.graph.nodes()) {
        REQUIRE(n.domain_id == 0u);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// build_pdg_pass: cross-domain (rpc_boundary) function
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"build_pdg_pass: rpc_boundary edge produces two distinct domains in the PDG"
,
"[lithe][pdg][build][domain]"
)
{
    // bb1 -rpc_boundary-> bb2(ret)
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    const auto cfg = inject_edge_kind(fn, 1u, 2u, edge_kind::rpc_boundary);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto res = build_pdg_pass{}.run(fn, cfg, vf);

    REQUIRE(res.ok());
    const auto dom_ids = res.graph.domain_ids();
    REQUIRE(dom_ids.size() == 2u);

    // bb1 instructions are in domain 0; bb2 instructions in domain != 0.
    const auto *n1 = res.graph.find_node(1u); // def in bb1
    const auto *n3 = res.graph.find_node(3u); // use in bb2
    REQUIRE(n1 != nullptr);
    REQUIRE(n3 != nullptr);
    REQUIRE(n1->domain_id == 0u);
    REQUIRE(n3->domain_id != 0u);
}

TEST_CASE (



"build_pdg_pass: cross-domain RAW edge carries raw_cross_domain kind"
,
"[lithe][pdg][build][domain]"
)
{
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    const auto cfg = inject_edge_kind(fn, 1u, 2u, edge_kind::rpc_boundary);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto res = build_pdg_pass{}.run(fn, cfg, vf);
    REQUIRE(res.ok());

    // The data edge from inst 1 (domain 0) to inst 3 (other domain) must be
    // tagged as raw_cross_domain.
    bool found_cross = false;
    for (const auto &e : res.graph.out_edges(1u)) {
        if (e.to_instr == 3u && e.data_kind == data_dep_kind::raw_cross_domain)
            found_cross = true;
    }
    REQUIRE(found_cross);
}

TEST_CASE (



"build_pdg_pass: rpc_boundary source block is marked is_boundary"
,
"[lithe][pdg][build][domain]"
)
{
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    const auto cfg = inject_edge_kind(fn, 1u, 2u, edge_kind::rpc_boundary);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto res = build_pdg_pass{}.run(fn, cfg, vf);
    REQUIRE(res.ok());

    // All instructions in bb1 (the rpc_boundary source) must have is_boundary == true.
    const auto *n1 = res.graph.find_node(1u);
    REQUIRE(n1 != nullptr);
    REQUIRE(n1->is_boundary);

    // Instructions in bb2 (remote domain) must NOT be marked boundary.
    const auto *n3 = res.graph.find_node(3u);
    REQUIRE(n3 != nullptr);
    REQUIRE_FALSE(n3->is_boundary);
}

// ─────────────────────────────────────────────────────────────────────────────
// distribute_mir_pass: single domain (no partitioning)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"distribute_mir_pass: single-domain function returns one partition equal to input"
,
"[lithe][pdg][distribute]"
)
{
    auto fn = make_physical({
        make_block(1, {make_branch(1, 2)}, {2}),
        make_block(2, {make_ret(2)})
    }, 1);

    mir_pass_context ctx;
    const auto res = distribute_mir(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.partitions.size() == 1u);
    REQUIRE(res.partitions[0].function.blocks.size() == 2u);
    REQUIRE(res.domain_to_partition.count(0u) == 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// distribute_mir_pass: rpc_boundary splits into two partitions
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"distribute_mir_pass: rpc_boundary produces two physical_mir_functions"
,
"[lithe][pdg][distribute][rpc]"
)
{
    // bb1(domain 0) -rpc_boundary-> bb2(domain 1)
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    auto cfg = inject_edge_kind(fn, 1u, 2u, edge_kind::rpc_boundary);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto pdg_res = build_pdg_pass{}.run(fn, cfg, vf);
    REQUIRE(pdg_res.ok());

    const auto res = distribute_mir_pass{}.run(fn, pdg_res, cfg);
    REQUIRE(res.ok());
    REQUIRE(res.partitions.size() == 2u);
}

TEST_CASE (



"distribute_mir_pass: domain 0 partition contains only local blocks"
,
"[lithe][pdg][distribute][rpc]"
)
{
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    auto cfg = inject_edge_kind(fn, 1u, 2u, edge_kind::rpc_boundary);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto pdg_res = build_pdg_pass{}.run(fn, cfg, vf);
    const auto res = distribute_mir_pass{}.run(fn, pdg_res, cfg);

    REQUIRE(res.ok());
    const std::size_t part0_idx = res.domain_to_partition.at(0u);
    const auto &part0 = res.partitions[part0_idx];

    // Domain 0 contains only bb1.
    REQUIRE(part0.function.blocks.size() == 1u);
    REQUIRE(part0.function.blocks[0].id == 1u);
    REQUIRE(part0.function.cfg.entry_block == 1u);
}

TEST_CASE (



"distribute_mir_pass: remote domain partition contains only remote blocks"
,
"[lithe][pdg][distribute][rpc]"
)
{
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    auto cfg = inject_edge_kind(fn, 1u, 2u, edge_kind::rpc_boundary);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto pdg_res = build_pdg_pass{}.run(fn, cfg, vf);
    const auto res = distribute_mir_pass{}.run(fn, pdg_res, cfg);

    REQUIRE(res.ok());
    REQUIRE(res.partitions.size() == 2u);

    // Find the partition that holds bb2.
    const auto *remote = [&]() -> const lithe::codegen::mir::physical_mir_function * {
        for (const auto &p : res.partitions)
            for (const auto &b : p.function.blocks)
                if (b.id == 2u) return &p;
        return nullptr;
    }();
    REQUIRE(remote != nullptr);

    // The remote partition must not contain bb1.
    const bool has_bb1 = std::ranges::any_of(
        remote->function.blocks, [](const auto &b) { return b.id == 1u; });
    REQUIRE_FALSE(has_bb1);
}

TEST_CASE (



"distribute_mir_pass: remote function name carries domain suffix"
,
"[lithe][pdg][distribute][rpc]"
)
{
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    auto cfg = inject_edge_kind(fn, 1u, 2u, edge_kind::rpc_boundary);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto pdg_res = build_pdg_pass{}.run(fn, cfg, vf);
    const auto res = distribute_mir_pass{}.run(fn, pdg_res, cfg);

    REQUIRE(res.ok());

    // At least one non-root partition must carry a "__domain_" suffix.
    bool has_suffix = false;
    for (const auto &p : res.partitions) {
        if (p.function.name.find("__domain_") != std::string::npos)
            has_suffix = true;
    }
    REQUIRE(has_suffix);
}

TEST_CASE (



"distribute_mir_pass: cross-domain branch instruction stripped from local partition"
,
"[lithe][pdg][distribute][rpc]"
)
{
    const preg r1{1, "r1"};
    auto fn = make_physical({
        make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
        make_block(2, {make_use_only(3, r1)})
    }, 1);

    auto cfg = inject_edge_kind(fn, 1u, 2u, edge_kind::rpc_boundary);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto pdg_res = build_pdg_pass{}.run(fn, cfg, vf);
    const auto res = distribute_mir_pass{}.run(fn, pdg_res, cfg);

    REQUIRE(res.ok());
    const std::size_t part0_idx = res.domain_to_partition.at(0u);
    const auto &bb1_out = res.partitions[part0_idx].function.blocks[0];

    // The branch instruction (id=2) targeting bb2 must have been removed or replaced.
    const bool still_has_raw_branch = std::ranges::any_of(
        bb1_out.instructions, [](const auto &i) {
            return i.op == lithe::codegen::opcode::branch && i.id == 2u;
        });
    REQUIRE_FALSE(still_has_raw_branch);

    // A stub call should have been injected instead.
    const bool has_stub = std::ranges::any_of(
        bb1_out.instructions, [](const auto &i) {
            return i.op == lithe::codegen::opcode::call;
        });
    REQUIRE(has_stub);
}

// ─────────────────────────────────────────────────────────────────────────────
// distribute_mir_pass: multi-domain (async_fork produces two domains)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"distribute_mir_pass: async_fork edge produces two distinct partitions"
,
"[lithe][pdg][distribute][async]"
)
{
    const preg r5{5, "cond"};
    allocated_instruction brc;
    brc.id = 10; brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};

    auto fn = make_physical({
        make_block(1, {brc},                {2, 3}),
        make_block(2, {make_branch(20, 4)}, {4}),
        make_block(3, {make_ret(30)}),
        make_block(4, {make_ret(40)})
    }, 1);

    // Inject async_fork on 1→3.
    auto cfg = inject_edge_kind(fn, 1u, 3u, edge_kind::async_fork);

    value_flow_analysis_result vf;
    {
        mir_pass_context ctx;
        vf = get_or_compute_def_use(ctx, fn);
    }

    const auto pdg_res = build_pdg_pass{}.run(fn, cfg, vf);
    const auto res = distribute_mir_pass{}.run(fn, pdg_res, cfg);

    REQUIRE(res.ok());
    REQUIRE(res.partitions.size() == 2u);

    // bb3 must not appear in the local (domain 0) partition.
    const std::size_t p0 = res.domain_to_partition.at(0u);
    const bool root_has_bb3 = std::ranges::any_of(
        res.partitions[p0].function.blocks,
        [](const auto &b) { return b.id == 3u; });
    REQUIRE_FALSE(root_has_bb3);

    // bb3 must appear in exactly one partition.
    std::size_t bb3_count = 0;
    for (const auto &p : res.partitions)
        for (const auto &b : p.function.blocks)
            if (b.id == 3u) ++bb3_count;
    REQUIRE(bb3_count == 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Convenience API: distribute_mir() free function
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (



"distribute_mir free function: single-domain function returns one partition"
,
"[lithe][pdg][api]"
)
{
    auto fn = make_physical({
        make_block(1, {make_branch(1, 2)}, {2}),
        make_block(2, {make_ret(2)})
    }, 1);

    lithe::codegen::mir_pass_context ctx;
    const auto res = lithe::pdg::distribute_mir(fn, ctx);

    REQUIRE(res.ok());
    REQUIRE(res.partitions.size() == 1u);
}
