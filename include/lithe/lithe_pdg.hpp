#pragma once
// Internal fragment — include via lithe_codegen_pipeline.hpp (umbrella).
// Depends on: lithe::codegen types from codegen part 1.
#include <algorithm>
#include <cassert>
#include <ranges>
#include <unordered_set>

// =============================================================================
// Phase 3: Program Dependence Graph (PDG)
// Namespace: lithe::pdg
//
// Merges cfg_analysis_result + SSA def-use into a flat PDG for aggressive code
// motion and distributed graph splitting across execution domains.
//
//  • pdg_node              — instruction annotated with block / domain / boundary
//  • pdg_edge              — typed arc: data (RAW/WAW/cross) or control
//  • program_dependence_graph — flat std::vector adjacency lists; no heap-per-node
//  • build_pdg_pass        — analysis pass producing pdg_build_result
//  • distribute_mir_pass   — transformation pass producing one physical_mir_function
//                            per execution domain, splitting at rpc_boundary edges
// =============================================================================

namespace lithe::pdg {
    // -------------------------------------------------------------------------
    // 1. Dependency edge types
    // -------------------------------------------------------------------------

    enum class data_dep_kind : std::uint8_t {
        raw, // Read-After-Write  (true dependence)
        war, // Write-After-Read  (anti dependence)
        waw, // Write-After-Write (output dependence)
        raw_cross_domain, // RAW crossing an rpc_boundary or async_fork edge
    };

    enum class pdg_edge_kind : std::uint8_t {
        data_dependency, // carries a data_dep_kind
        control_dependency, // due to conditional branching / dominator relation
    };

    struct pdg_edge {
        std::uint32_t from_instr = 0;
        std::uint32_t to_instr = 0;
        pdg_edge_kind kind = pdg_edge_kind::data_dependency;
        data_dep_kind data_kind = data_dep_kind::raw;
        std::uint32_t value_id = 0;

        [[nodiscard]] bool is_data() const noexcept { return kind == pdg_edge_kind::data_dependency; }
        [[nodiscard]] bool is_control() const noexcept { return kind == pdg_edge_kind::control_dependency; }
    };

    [[nodiscard]] inline pdg_edge make_data_edge(
        const std::uint32_t from, const std::uint32_t to,
        const data_dep_kind dk, const std::uint32_t vid = 0) noexcept {
        return pdg_edge{from, to, pdg_edge_kind::data_dependency, dk, vid};
    }

    [[nodiscard]] inline pdg_edge make_control_edge(
        const std::uint32_t from, const std::uint32_t to) noexcept {
        return pdg_edge{from, to, pdg_edge_kind::control_dependency, data_dep_kind::raw, 0};
    }

    // -------------------------------------------------------------------------
    // 2. PDG node
    // -------------------------------------------------------------------------

    struct pdg_node {
        std::uint32_t instr_id = 0;
        std::uint32_t block_id = 0;
        std::uint32_t domain_id = 0;
        bool is_boundary = false;

        [[nodiscard]] bool operator==(const pdg_node& o) const noexcept {
            return instr_id == o.instr_id;
        }
    };

    // -------------------------------------------------------------------------
    // 3. Flat Program Dependence Graph
    // -------------------------------------------------------------------------

    class program_dependence_graph {
    public:
        program_dependence_graph() = default;

        std::size_t add_node(pdg_node node) {
            auto it = id_to_idx_.find(node.instr_id);
            if (it != id_to_idx_.end()) return it->second;
            const std::size_t idx = nodes_.size();
            id_to_idx_.emplace(node.instr_id, idx);
            nodes_.push_back(node);
            out_edges_.emplace_back();
            in_edges_.emplace_back();
            return idx;
        }

        void add_edge(const pdg_edge edge) {
            auto fi = id_to_idx_.find(edge.from_instr);
            auto ti = id_to_idx_.find(edge.to_instr);
            if (fi == id_to_idx_.end() || ti == id_to_idx_.end()) return;
            out_edges_[fi->second].push_back(edge);
            in_edges_[ti->second].push_back(edge);
        }

        [[nodiscard]] const std::vector<pdg_node>& nodes() const noexcept { return nodes_; }

        [[nodiscard]] std::span<const pdg_edge> out_edges(const std::uint32_t instr_id) const noexcept {
            auto it = id_to_idx_.find(instr_id);
            if (it == id_to_idx_.end()) return {};
            return out_edges_[it->second];
        }

        [[nodiscard]] std::span<const pdg_edge> in_edges(const std::uint32_t instr_id) const noexcept {
            auto it = id_to_idx_.find(instr_id);
            if (it == id_to_idx_.end()) return {};
            return in_edges_[it->second];
        }

        [[nodiscard]] const pdg_node* find_node(const std::uint32_t instr_id) const noexcept {
            auto it = id_to_idx_.find(instr_id);
            if (it == id_to_idx_.end()) return nullptr;
            return &nodes_[it->second];
        }

        [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }

        [[nodiscard]] std::size_t edge_count() const noexcept {
            std::size_t total = 0;
            for (const auto& v : out_edges_) total += v.size();
            return total;
        }

        [[nodiscard]] auto data_edges() const {
            return nodes_
                | std::views::transform([this](const pdg_node& n) { return out_edges(n.instr_id); })
                | std::views::join
                | std::views::filter([](const pdg_edge& e) { return e.is_data(); });
        }

        [[nodiscard]] auto control_edges() const {
            return nodes_
                | std::views::transform([this](const pdg_node& n) { return out_edges(n.instr_id); })
                | std::views::join
                | std::views::filter([](const pdg_edge& e) { return e.is_control(); });
        }

        [[nodiscard]] std::vector<const pdg_node*> nodes_in_domain(const std::uint32_t domain_id) const {
            std::vector<const pdg_node*> result;
            result.reserve(nodes_.size());
            for (const auto& n : nodes_)
                if (n.domain_id == domain_id) result.push_back(&n);
            return result;
        }

        [[nodiscard]] std::vector<std::uint32_t> domain_ids() const {
            std::vector<std::uint32_t> ids;
            for (const auto& n : nodes_)
                if (std::ranges::find(ids, n.domain_id) == ids.end())
                    ids.push_back(n.domain_id);
            std::ranges::sort(ids);
            return ids;
        }

    private:
        std::vector<pdg_node> nodes_;
        std::unordered_map<std::uint32_t,
                           std::size_t> id_to_idx_;
        std::vector<std::vector<pdg_edge>> out_edges_;
        std::vector<std::vector<pdg_edge>> in_edges_;
    };

    // -------------------------------------------------------------------------
    // 4. PDG build result
    // -------------------------------------------------------------------------

    struct pdg_build_result {
        program_dependence_graph graph;
        std::size_t data_edge_count = 0;
        std::size_t control_edge_count = 0;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // -------------------------------------------------------------------------
    // 5. build_pdg_pass
    // -------------------------------------------------------------------------

    struct build_pdg_pass {
        [[nodiscard]] pdg_build_result run(
            const codegen::mir::physical_mir_function& fn,
            const codegen::cfg_analysis_result& cfg,
            const codegen::value_flow_analysis_result& vf
        ) const {
            pdg_build_result result;
            auto& g = result.graph;

            // (a) Insert nodes
            std::unordered_map<std::uint32_t, std::uint32_t> block_to_domain;
            if (cfg.partition.has_value()) {
                block_to_domain = cfg.partition->block_to_domain;
            }
            else {
                for (const auto bid : cfg.reachable_blocks)
                    block_to_domain[bid] = 0u;
            }

            std::unordered_set<std::uint32_t> rpc_source_blocks;
            for (const auto& te : cfg.typed_edges)
                if (te.kind == codegen::edge_kind::rpc_boundary)
                    rpc_source_blocks.insert(te.from);

            for (const auto& block : fn.function.blocks) {
                const std::uint32_t dom = [&]() -> std::uint32_t {
                    auto it = block_to_domain.find(block.id);
                    return it != block_to_domain.end() ? it->second : 0u;
                }();
                const bool is_boundary = rpc_source_blocks.contains(block.id);
                for (const auto& instr : block.instructions)
                    g.add_node(pdg_node{instr.id, block.id, dom, is_boundary});
            }

            // (b) Data dependency edges (RAW from def-use chains)
            std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> instr_domain;
            for (const auto& n : g.nodes())
                instr_domain[n.instr_id] = {n.block_id, n.domain_id};

            for (const auto& [val_id, chain] : vf.def_use_chains) {
                const std::uint32_t def_instr = chain.definition.instruction_id;
                if (g.find_node(def_instr) == nullptr) continue;
                const auto def_it = instr_domain.find(def_instr);
                const std::uint32_t def_domain = (def_it != instr_domain.end())
                                                     ? def_it->second.second
                                                     : 0u;
                for (const auto& use : chain.uses) {
                    const std::uint32_t use_instr = use.instruction_id;
                    if (g.find_node(use_instr) == nullptr) continue;
                    const auto use_it = instr_domain.find(use_instr);
                    const std::uint32_t use_domain = (use_it != instr_domain.end())
                                                         ? use_it->second.second
                                                         : 0u;
                    const data_dep_kind dk = (def_domain != use_domain)
                                                 ? data_dep_kind::raw_cross_domain
                                                 : data_dep_kind::raw;
                    g.add_edge(make_data_edge(def_instr, use_instr, dk, val_id));
                    ++result.data_edge_count;
                }
            }

            // WAW edges: scan instruction defs directly (def_use_chains has one
            // entry per preg so cannot detect multiple writers from there).
            {
                std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> defs_by_val;
                for (const auto& block : fn.function.blocks) {
                    for (const auto& instr : block.instructions) {
                        for (const auto& def_op : instr.defs) {
                            if (def_op.type != codegen::allocated_operand::kind::preg) continue;
                            const auto& pr = std::get<codegen::preg>(def_op.value);
                            defs_by_val[pr.id].push_back(instr.id);
                        }
                    }
                }
                for (const auto& [vid, def_instrs] : defs_by_val) {
                    if (def_instrs.size() < 2) continue;
                    for (std::size_t i = 0; i < def_instrs.size(); ++i) {
                        for (std::size_t j = i + 1; j < def_instrs.size(); ++j) {
                            const std::uint32_t di = def_instrs[i];
                            const std::uint32_t dj = def_instrs[j];
                            if (g.find_node(di) == nullptr || g.find_node(dj) == nullptr) continue;
                            g.add_edge(make_data_edge(di, dj, data_dep_kind::waw, vid));
                            ++result.data_edge_count;
                        }
                    }
                }
            }

            // (c) Control dependency edges (conservative: branch terminator →
            //     all instructions in each successor of a multi-successor block).
            {
                std::unordered_map<std::uint32_t, std::uint32_t> block_terminator;
                for (const auto& block : fn.function.blocks) {
                    if (block.instructions.empty()) continue;
                    block_terminator[block.id] = block.instructions.back().id;
                }
                for (const auto& block : fn.function.blocks) {
                    const auto succs_it = fn.function.cfg.successors.find(block.id);
                    if (succs_it == fn.function.cfg.successors.end()) continue;
                    const auto& succs = succs_it->second;
                    if (succs.size() < 2) continue;
                    const auto term_it = block_terminator.find(block.id);
                    if (term_it == block_terminator.end()) continue;
                    const std::uint32_t pred_instr = term_it->second;
                    if (g.find_node(pred_instr) == nullptr) continue;
                    for (const auto succ_id : succs) {
                        const auto* succ_block = [&]() -> const codegen::allocated_basic_block* {
                            for (const auto& b : fn.function.blocks)
                                if (b.id == succ_id) return &b;
                            return nullptr;
                        }();
                        if (succ_block == nullptr) continue;
                        for (const auto& succ_instr : succ_block->instructions) {
                            if (g.find_node(succ_instr.id) == nullptr) continue;
                            g.add_edge(make_control_edge(pred_instr, succ_instr.id));
                            ++result.control_edge_count;
                        }
                    }
                }
            }

            return result;
        }
    };

    // -------------------------------------------------------------------------
    // 6. distribute_mir_pass
    // -------------------------------------------------------------------------

    struct distribute_mir_result {
        std::vector<codegen::mir::physical_mir_function> partitions;
        std::unordered_map<std::uint32_t, std::size_t> domain_to_partition;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    struct distribute_mir_pass {
        [[nodiscard]] distribute_mir_result run(
            const codegen::mir::physical_mir_function& fn,
            const pdg_build_result& pdg_result,
            const codegen::cfg_analysis_result& cfg
        ) const {
            distribute_mir_result result;
            const auto& g = pdg_result.graph;
            const auto domain_ids = g.domain_ids();

            if (domain_ids.empty() ||
                (domain_ids.size() == 1 && domain_ids[0] == 0)) {
                result.partitions.push_back(fn);
                result.domain_to_partition[0] = 0;
                return result;
            }

            // block_id → domain_id
            std::unordered_map<std::uint32_t, std::uint32_t> block_domain;
            for (const auto& n : g.nodes())
                block_domain.try_emplace(n.block_id, n.domain_id);

            // domain root blocks
            std::unordered_map<std::uint32_t, std::uint32_t> domain_root;
            domain_root[0] = fn.function.cfg.entry_block;
            if (cfg.partition.has_value()) {
                for (const auto& dom : cfg.partition->domains)
                    if (dom.domain_id != 0)
                        domain_root[dom.domain_id] = dom.root_block;
            }

            std::unordered_set<std::uint32_t> rpc_sources;
            for (const auto& te : cfg.typed_edges)
                if (te.kind == codegen::edge_kind::rpc_boundary)
                    rpc_sources.insert(te.from);

            result.partitions.reserve(domain_ids.size());
            for (const auto dom_id : domain_ids) {
                codegen::mir::physical_mir_function out_fn;
                out_fn.metadata = fn.metadata;
                out_fn.function.name = fn.function.name +
                    (dom_id == 0 ? "" : ("__domain_" + std::to_string(dom_id)));
                {
                    auto it = domain_root.find(dom_id);
                    out_fn.function.cfg.entry_block = (it != domain_root.end())
                                                          ? it->second
                                                          : fn.function.cfg.entry_block;
                }

                for (const auto& block : fn.function.blocks) {
                    const auto it = block_domain.find(block.id);
                    if (it == block_domain.end() || it->second != dom_id) continue;

                    codegen::allocated_basic_block new_block = block;

                    if (rpc_sources.contains(block.id)) {
                        new_block.instructions.erase(
                            std::remove_if(
                                new_block.instructions.begin(),
                                new_block.instructions.end(),
                                [&](const codegen::allocated_instruction& instr) {
                                    if (instr.op != codegen::opcode::branch &&
                                        instr.op != codegen::opcode::branch_cond)
                                        return false;
                                    for (const auto& op : instr.uses) {
                                        if (op.type == codegen::allocated_operand::kind::block) {
                                            const auto succ_bid = std::get<std::uint32_t>(op.value);
                                            const auto sit = block_domain.find(succ_bid);
                                            if (sit != block_domain.end() && sit->second != dom_id)
                                                return true;
                                        }
                                    }
                                    return false;
                                }),
                            new_block.instructions.end());

                        codegen::allocated_instruction stub;
                        stub.op = codegen::opcode::call;
                        stub.id = next_stub_id_++;
                        new_block.instructions.push_back(std::move(stub));
                    }

                    new_block.successors.clear();
                    const auto succs_it = fn.function.cfg.successors.find(block.id);
                    if (succs_it != fn.function.cfg.successors.end()) {
                        for (const auto succ : succs_it->second) {
                            const auto sit = block_domain.find(succ);
                            if (sit != block_domain.end() && sit->second == dom_id)
                                new_block.successors.push_back(succ);
                        }
                    }
                    out_fn.function.blocks.push_back(std::move(new_block));
                }

                out_fn.function.cfg.successors.clear();
                out_fn.function.cfg.predecessors.clear();
                for (const auto& b : out_fn.function.blocks) {
                    out_fn.function.cfg.successors[b.id] = b.successors;
                    for (const auto succ : b.successors)
                        out_fn.function.cfg.predecessors[succ].push_back(b.id);
                }

                result.domain_to_partition[dom_id] = result.partitions.size();
                result.partitions.push_back(std::move(out_fn));
            }
            return result;
        }

    private:
        mutable std::uint32_t next_stub_id_ = 0xffff'0000u;
    };

    // -------------------------------------------------------------------------
    // 7. Convenience free functions
    // -------------------------------------------------------------------------

    [[nodiscard]] inline pdg_build_result build_pdg(
        const codegen::mir::physical_mir_function& fn,
        codegen::mir_pass_context& ctx) {
        const auto& cfg = codegen::get_or_compute_cfg(ctx, fn);
        const auto& vf = codegen::get_or_compute_def_use(ctx, fn);
        return build_pdg_pass{}.run(fn, cfg, vf);
    }

    [[nodiscard]] inline distribute_mir_result distribute_mir(
        const codegen::mir::physical_mir_function& fn,
        codegen::mir_pass_context& ctx) {
        const auto& cfg = codegen::get_or_compute_cfg(ctx, fn);
        const auto pdg = build_pdg(fn, ctx);
        return distribute_mir_pass{}.run(fn, pdg, cfg);
    }
} // namespace lithe::pdg
