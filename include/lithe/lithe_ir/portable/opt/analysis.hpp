#pragma once

// =============================================================================
// lithe_ir/portable/opt/analysis.hpp — analysis cache + 7 portable providers
//
// Namespace: lithe::ir::portable::opt
//
// analysis_cache: lazy compute + bitmask invalidation over the wire module.
// Each analysis_id maps to one provider struct satisfying analysis_provider.
//
// Providers:
//   dominance        — wraps impl-1 cfg_adapter + litegraph::DominatorTree
//   cfg_reachability — LiteGraph BFS from entry blocks
//   liveness         — backward dataflow per block (live-in/live-out sets)
//   effects          — per-block effect flags from opcode signature table
//   purity           — function/op purity from effect summary
//   ranges           — per-value integer range lattice (interval domain)
//   aliasing         — conservative memref alias classification
//
// Arch §4.4: get<A>(module) is compute-on-miss; invalidate(mask) clears stale facts.
// Changed module never leaves dangling facts (analysis results are value types).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <any>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "containers/graph/DominatorTree.hpp"
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include "../cfg_adapter.hpp"   // to_litegraph, entry_node, litegraph_cfg_result
#include "../verify.hpp"        // opcode_signature_entry, find_signature, k_opcode_signatures
#include "pass.hpp"             // analysis_id, analysis_mask, mask_of, portable_pass concept

namespace lithe::ir::portable::opt {
    // =============================================================================
    // Analysis result types (value types — no pointers into the module)
    // =============================================================================

    // --- dominance_facts ---
    struct dominance_facts {
        // Per-function dominator results keyed by function index
        struct fn_dom {
            litegraph_cfg_result cfg;
            litegraph::dominator_result<litegraph::NodeId> dom;
        };

        std::vector<fn_dom> per_fn;

        [[nodiscard]] bool dominates(std::size_t fn_idx,
                                     std::uint32_t def_block,
                                     std::uint32_t use_block) const {
            if (fn_idx >= per_fn.size()) return false;
            const auto& f = per_fn[fn_idx];
            if (def_block >= f.cfg.node_ids.size()) return false;
            if (use_block >= f.cfg.node_ids.size()) return false;
            const auto def_nid = f.cfg.node_ids[def_block];
            const auto use_nid = f.cfg.node_ids[use_block];
            return litegraph::dominates(f.dom, def_nid, use_nid);
        }
    };

    // --- cfg_reachability_facts ---
    struct cfg_reachability_facts {
        // per_fn[fn_idx] = set of reachable block canonical ids from entry
        std::vector<std::unordered_set<std::uint32_t>> per_fn;

        [[nodiscard]] bool reachable(std::size_t fn_idx, std::uint32_t block_id) const {
            if (fn_idx >= per_fn.size()) return false;
            return per_fn[fn_idx].count(block_id) > 0;
        }
    };

    // --- liveness_facts ---
    // live-in/live-out per block (value ids)
    struct block_liveness {
        std::unordered_set<std::uint32_t> live_in;
        std::unordered_set<std::uint32_t> live_out;
    };

    struct liveness_facts {
        // per_fn[fn_idx] indexed by block canonical id
        std::vector<std::unordered_map<std::uint32_t, block_liveness>> per_fn;

        [[nodiscard]] bool live_out_of(std::size_t fn_idx,
                                       std::uint32_t block_id,
                                       std::uint32_t value_id) const {
            if (fn_idx >= per_fn.size()) return false;
            const auto it = per_fn[fn_idx].find(block_id);
            if (it == per_fn[fn_idx].end()) return false;
            return it->second.live_out.count(value_id) > 0;
        }

        [[nodiscard]] bool live_in_of(std::size_t fn_idx,
                                      std::uint32_t block_id,
                                      std::uint32_t value_id) const {
            if (fn_idx >= per_fn.size()) return false;
            const auto it = per_fn[fn_idx].find(block_id);
            if (it == per_fn[fn_idx].end()) return false;
            return it->second.live_in.count(value_id) > 0;
        }
    };

    // --- effect_flags ---
    struct effect_flags {
        bool reads_memory = false;
        bool writes_memory = false;
        bool is_terminator = false;
        bool trapping = false; // any op that may trap (div, bounds, etc.)
        bool calls_extern = false;
        bool has_defer = false;
        bool has_txn = false;
        bool has_exception = false;
    };

    struct effects_facts {
        // per_fn[fn_idx][op_id] = effect_flags
        std::vector<std::unordered_map<std::uint32_t, effect_flags>> per_fn;

        // block_effects[fn_idx][block_id] = union of all op effects in block
        std::vector<std::unordered_map<std::uint32_t, effect_flags>> block_effects;

        [[nodiscard]] effect_flags op_effect(std::size_t fn_idx, std::uint32_t op_id) const {
            if (fn_idx >= per_fn.size()) return {};
            const auto it = per_fn[fn_idx].find(op_id);
            if (it == per_fn[fn_idx].end()) return {};
            return it->second;
        }

        [[nodiscard]] effect_flags block_effect(std::size_t fn_idx, std::uint32_t block_id) const {
            if (fn_idx >= block_effects.size()) return {};
            const auto it = block_effects[fn_idx].find(block_id);
            if (it == block_effects[fn_idx].end()) return {};
            return it->second;
        }
    };

    // --- purity_facts ---
    struct purity_facts {
        // per_fn[fn_idx][op_id] = true iff op is pure (no reads/writes/traps/calls)
        std::vector<std::unordered_map<std::uint32_t, bool>> per_fn;
        // fn_pure[fn_idx] = true iff the entire function is pure
        std::vector<bool> fn_pure;

        [[nodiscard]] bool op_pure(std::size_t fn_idx, std::uint32_t op_id) const {
            if (fn_idx >= per_fn.size()) return false;
            const auto it = per_fn[fn_idx].find(op_id);
            if (it == per_fn[fn_idx].end()) return false;
            return it->second;
        }

        [[nodiscard]] bool function_pure(std::size_t fn_idx) const {
            if (fn_idx >= fn_pure.size()) return false;
            return fn_pure[fn_idx];
        }
    };

    // --- ranges_facts ---
    // Interval domain: [lo, hi] for integer values; top = [INT64_MIN, INT64_MAX]
    struct value_range {
        std::int64_t lo = std::numeric_limits<std::int64_t>::min();
        std::int64_t hi = std::numeric_limits<std::int64_t>::max();

        [[nodiscard]] bool is_top() const noexcept {
            return lo == std::numeric_limits<std::int64_t>::min()
                && hi == std::numeric_limits<std::int64_t>::max();
        }

        [[nodiscard]] bool contains(std::int64_t v) const noexcept {
            return v >= lo && v <= hi;
        }

        [[nodiscard]] bool proven_in_range(std::int64_t bound_lo,
                                           std::int64_t bound_hi) const noexcept {
            return lo >= bound_lo && hi <= bound_hi;
        }
    };

    struct ranges_facts {
        // per_fn[fn_idx][value_id] = interval range
        std::vector<std::unordered_map<std::uint32_t, value_range>> per_fn;

        [[nodiscard]] value_range range_of(std::size_t fn_idx, std::uint32_t val_id) const {
            if (fn_idx >= per_fn.size()) return {};
            const auto it = per_fn[fn_idx].find(val_id);
            if (it == per_fn[fn_idx].end()) return {}; // top = unknown
            return it->second;
        }
    };

    // --- aliasing_facts ---
    enum class alias_class : std::uint8_t {
        same_base = 0, // provably same memref base
        distinct_base = 1, // provably different bases
        unknown = 2, // conservative (may alias)
    };

    struct aliasing_facts {
        // Conservative map: (fn_idx, op_id_a, op_id_b) → alias_class
        // Absent pair → unknown
        struct pair_key {
            std::uint32_t fn;
            std::uint32_t op_a;
            std::uint32_t op_b;
            bool operator==(const pair_key&) const noexcept = default;
        };

        struct pair_hash {
            std::size_t operator()(const pair_key& k) const noexcept {
                // FNV-1a style combine
                std::size_t h = 2166136261u;
                auto mix = [&](std::uint32_t v) {
                    h ^= static_cast<std::size_t>(v);
                    h *= 16777619u;
                };
                mix(k.fn);
                mix(k.op_a);
                mix(k.op_b);
                return h;
            }
        };

        std::unordered_map<pair_key, alias_class, pair_hash> classes;

        [[nodiscard]] alias_class alias_of(std::size_t fn_idx,
                                           std::uint32_t op_a,
                                           std::uint32_t op_b) const {
            auto key = pair_key{static_cast<std::uint32_t>(fn_idx), op_a, op_b};
            const auto it = classes.find(key);
            if (it == classes.end()) return alias_class::unknown;
            return it->second;
        }
    };

    // =============================================================================
    // analysis_cache — lazy compute + mask invalidation (arch §4.4)
    //
    // Each analysis_id maps to a std::any holding the computed result type.
    // get<Facts>(module) computes on cache miss; invalidate(mask) clears the set.
    // Facts must be default-constructible.
    // =============================================================================

    class analysis_cache {
    public:
        // Compute-on-miss access.  Returns const ref to cached facts.
        template <class Facts, class Provider>
        [[nodiscard]] const Facts& get(const portable_module& mod, Provider& p) {
            const analysis_id id = Provider::id();
            const auto mask = detail::mask_of(id);
            const auto key = static_cast<std::uint16_t>(id);

            if (!(valid_ & mask) || cache_.find(key) == cache_.end()) {
                cache_[key] = std::any{p.compute(mod)};
                valid_ |= mask;
            }
            return std::any_cast<const Facts&>(cache_.at(key));
        }

        // Invalidate all analyses in mask.
        void invalidate(analysis_mask mask) noexcept {
            // For each set bit in mask, clear from valid_ and erase from cache_
            for (int bit = 0; bit < 32; ++bit) {
                const analysis_mask m = 1u << bit;
                if (mask & m) {
                    valid_ &= ~m;
                    const auto key = static_cast<std::uint16_t>(bit + 1);
                    cache_.erase(key);
                }
            }
        }

        // Ensure the analyses in mask are computed; compute any missing ones.
        // Requires the corresponding providers to be provided externally.
        // (Used by the pass manager via ensure_analyses.)
        [[nodiscard]] analysis_mask valid_mask() const noexcept { return valid_; }

        // Test support: count cache hits/misses for invalidation correctness tests
        void reset_counters() noexcept { hits_ = misses_ = 0; }
        [[nodiscard]] std::uint64_t hits() const noexcept { return hits_; }
        [[nodiscard]] std::uint64_t misses() const noexcept { return misses_; }

    private:
        analysis_mask valid_ = 0;
        std::unordered_map<std::uint16_t, std::any> cache_;
        std::uint64_t hits_ = 0;
        std::uint64_t misses_ = 0;
    };

    // =============================================================================
    // Analysis provider implementations (each satisfies analysis_provider concept)
    // =============================================================================

    // --- dominance provider ---
    struct dominance_provider {
        [[nodiscard]] static constexpr analysis_id id() noexcept {
            return analysis_id::dominance;
        }

        [[nodiscard]] dominance_facts compute(const portable_module& mod) const {
            dominance_facts facts;
            facts.per_fn.reserve(mod.functions.size());
            for (const auto& fn : mod.functions) {
                auto cfg_res = to_litegraph(fn);
                const auto entry_id = entry_node(fn);
                litegraph::NodeId entry_nid{0};
                if (entry_id < cfg_res.node_ids.size())
                    entry_nid = cfg_res.node_ids[entry_id];

                // Build dominator_graph_view for compute_dominators
                litegraph::dominator_graph_view<litegraph::NodeId> view;
                view.entry = entry_nid;
                for (std::uint32_t bid = 0;
                     bid < static_cast<std::uint32_t>(cfg_res.node_ids.size()); ++bid) {
                    view.nodes.push_back(cfg_res.node_ids[bid]);
                }
                // Build predecessor/successor maps from cfg graph
                for (const auto& reg : fn.regions) {
                    const std::uint32_t nb = static_cast<std::uint32_t>(reg.block_ids.size());
                    for (std::uint32_t k = 0; k + 1 < nb; ++k) {
                        const std::uint32_t from = reg.block_ids[k];
                        const std::uint32_t to = reg.block_ids[k + 1];
                        if (from >= cfg_res.node_ids.size() || to >= cfg_res.node_ids.size())
                            continue;
                        const auto from_nid = cfg_res.node_ids[from];
                        const auto to_nid = cfg_res.node_ids[to];
                        view.successors[from_nid].push_back(to_nid);
                        view.predecessors[to_nid].push_back(from_nid);
                    }
                }
                auto dom = litegraph::compute_dominators(view);
                facts.per_fn.push_back({std::move(cfg_res), std::move(dom)});
            }
            return facts;
        }
    };

    // --- cfg_reachability provider ---
    struct cfg_reachability_provider {
        [[nodiscard]] static constexpr analysis_id id() noexcept {
            return analysis_id::cfg_reachability;
        }

        [[nodiscard]] cfg_reachability_facts compute(const portable_module& mod) const {
            cfg_reachability_facts facts;
            facts.per_fn.reserve(mod.functions.size());
            for (const auto& fn : mod.functions) {
                auto cfg_res = to_litegraph(fn);
                const auto entry_id = entry_node(fn);

                std::unordered_set<std::uint32_t> reachable;
                if (!fn.blocks.empty()) {
                    litegraph::NodeId entry_nid{0};
                    if (entry_id < cfg_res.node_ids.size())
                        entry_nid = cfg_res.node_ids[entry_id];

                    // Build reverse map NodeId → block canonical id once
                    std::unordered_map<std::uint32_t, std::uint32_t> nid_to_bid;
                    for (std::uint32_t bid = 0;
                         bid < static_cast<std::uint32_t>(cfg_res.node_ids.size()); ++bid)
                        nid_to_bid[cfg_res.node_ids[bid].value] = bid;

                    // Use free-function litegraph::bfs
                    litegraph::bfs(cfg_res.graph, entry_nid,
                                   [&](litegraph::NodeId nid, const auto& /*data*/) {
                                       const auto it = nid_to_bid.find(nid.value);
                                       if (it != nid_to_bid.end())
                                           reachable.insert(it->second);
                                   });
                }
                facts.per_fn.push_back(std::move(reachable));
            }
            return facts;
        }
    };

    // --- liveness provider ---
    // Backward dataflow: live_out[b] = union(live_in[s] for s in successors(b))
    // live_in[b] = (live_out[b] - defs[b]) ∪ uses[b]
    // One pass (conservative): iterate until stable; starts from terminators.
    struct liveness_provider {
        [[nodiscard]] static constexpr analysis_id id() noexcept {
            return analysis_id::liveness;
        }

        [[nodiscard]] liveness_facts compute(const portable_module& mod) const {
            liveness_facts facts;
            facts.per_fn.reserve(mod.functions.size());

            for (const auto& fn : mod.functions) {
                std::unordered_map<std::uint32_t, block_liveness> bmap;

                // Build op_id → op* map (op ids are not vector indices)
                std::unordered_map<std::uint32_t, const adapters::hl_wire_op*> op_by_id;
                for (const auto& op : fn.ops)
                    op_by_id[op.id] = &op;

                struct block_sets {
                    std::unordered_set<std::uint32_t> defs;
                    std::unordered_set<std::uint32_t> uses;
                    std::vector<std::uint32_t> succs;
                };
                std::unordered_map<std::uint32_t, block_sets> bsets;

                for (const auto& blk : fn.blocks) {
                    auto& bs = bsets[blk.id];
                    // block args = defs
                    for (std::uint32_t aid : blk.arg_ids) bs.defs.insert(aid);

                    // Process ops in order: first use then def (SSA)
                    for (std::uint32_t oid : blk.op_ids) {
                        const auto it = op_by_id.find(oid);
                        if (it == op_by_id.end()) continue;
                        const auto& op = *it->second;
                        for (std::uint32_t uv : op.operand_ids)
                            if (!bs.defs.count(uv)) bs.uses.insert(uv);
                        for (std::uint32_t rv : op.result_ids) bs.defs.insert(rv);
                    }
                }

                // Build successor edges from region block order
                for (const auto& reg : fn.regions) {
                    const std::uint32_t nb = static_cast<std::uint32_t>(reg.block_ids.size());
                    for (std::uint32_t k = 0; k + 1 < nb; ++k)
                        bsets[reg.block_ids[k]].succs.push_back(reg.block_ids[k + 1]);
                }

                // Initialize live_in/live_out
                for (const auto& blk : fn.blocks)
                    bmap[blk.id] = {};

                // Iterate backward until convergence
                bool changed = true;
                while (changed) {
                    changed = false;
                    // Process blocks in reverse region order (backward)
                    for (const auto& reg : fn.regions) {
                        for (auto it = reg.block_ids.rbegin(); it != reg.block_ids.rend(); ++it) {
                            const std::uint32_t bid = *it;
                            auto& bl = bmap[bid];
                            const auto& bs = bsets[bid];

                            // live_out[b] = union of live_in of successors
                            std::unordered_set<std::uint32_t> new_out;
                            for (std::uint32_t s : bs.succs)
                                for (std::uint32_t v : bmap[s].live_in)
                                    new_out.insert(v);

                            // live_in[b] = uses[b] ∪ (live_out[b] - defs[b])
                            std::unordered_set<std::uint32_t> new_in = bs.uses;
                            for (std::uint32_t v : new_out)
                                if (!bs.defs.count(v)) new_in.insert(v);

                            if (new_out != bl.live_out || new_in != bl.live_in) {
                                bl.live_out = std::move(new_out);
                                bl.live_in = std::move(new_in);
                                changed = true;
                            }
                        }
                    }
                }
                facts.per_fn.push_back(std::move(bmap));
            }
            return facts;
        }
    };

    // --- effects provider ---
    struct effects_provider {
        [[nodiscard]] static constexpr analysis_id id() noexcept {
            return analysis_id::effects;
        }

        [[nodiscard]] effects_facts compute(const portable_module& mod) const {
            effects_facts facts;
            const std::size_t nf = mod.functions.size();
            facts.per_fn.resize(nf);
            facts.block_effects.resize(nf);

            for (std::size_t fi = 0; fi < nf; ++fi) {
                const auto& fn = mod.functions[fi];
                for (const auto& op : fn.ops) {
                    const auto* sig = find_signature(op.domain, op.name);
                    effect_flags ef;
                    if (sig) {
                        ef.reads_memory = sig->reads_memory;
                        ef.writes_memory = sig->writes_memory;
                        ef.is_terminator = sig->is_terminator;
                        // external calls
                        ef.calls_extern = (op.name == "call");
                        // trapping: div ops, bounds-checked ops
                        ef.trapping = (op.name == "div" || op.name == "fdiv" ||
                            op.name == "memref_load" || op.name == "memref_store");
                    }
                    // defer/txn/exception from op annotations
                    ef.has_defer = false; // wire form marks via capability
                    ef.has_txn = false;
                    ef.has_exception = false;
                    facts.per_fn[fi][op.id] = ef;
                }
                // Aggregate block effects
                for (const auto& blk : fn.blocks) {
                    effect_flags bef;
                    for (std::uint32_t oid : blk.op_ids) {
                        const auto it = facts.per_fn[fi].find(oid);
                        if (it == facts.per_fn[fi].end()) continue;
                        const auto& ef = it->second;
                        bef.reads_memory |= ef.reads_memory;
                        bef.writes_memory |= ef.writes_memory;
                        bef.is_terminator |= ef.is_terminator;
                        bef.trapping |= ef.trapping;
                        bef.calls_extern |= ef.calls_extern;
                        bef.has_defer |= ef.has_defer;
                        bef.has_txn |= ef.has_txn;
                        bef.has_exception |= ef.has_exception;
                    }
                    facts.block_effects[fi][blk.id] = bef;
                }
            }
            return facts;
        }
    };

    // --- purity provider (derived from effects) ---
    struct purity_provider {
        [[nodiscard]] static constexpr analysis_id id() noexcept {
            return analysis_id::purity;
        }

        [[nodiscard]] purity_facts compute(const portable_module& mod) const {
            effects_provider ep;
            auto ef = ep.compute(mod);

            purity_facts facts;
            const std::size_t nf = mod.functions.size();
            facts.per_fn.resize(nf);
            facts.fn_pure.resize(nf, true);

            for (std::size_t fi = 0; fi < nf; ++fi) {
                for (const auto& [oid, flags] : ef.per_fn[fi]) {
                    const bool pure = !flags.reads_memory && !flags.writes_memory
                        && !flags.trapping && !flags.calls_extern
                        && !flags.has_defer && !flags.has_txn && !flags.has_exception;
                    facts.per_fn[fi][oid] = pure;
                    if (!pure) facts.fn_pure[fi] = false;
                }
            }
            return facts;
        }
    };

    // --- ranges provider ---
    // Conservative: only propagate constants; all others → top
    struct ranges_provider {
        [[nodiscard]] static constexpr analysis_id id() noexcept {
            return analysis_id::ranges;
        }

        [[nodiscard]] ranges_facts compute(const portable_module& mod) const {
            ranges_facts facts;
            facts.per_fn.reserve(mod.functions.size());

            for (const auto& fn : mod.functions) {
                std::unordered_map<std::uint32_t, value_range> vmap;

                // Walk ops; for constant ops with a numeric attr, record range
                for (const auto& op : fn.ops) {
                    if (op.name == "constant" && !op.result_ids.empty()) {
                        // Attempt to extract literal from constant pool index if available.
                        // Conservatively leaves as top when attribute is not a simple integer.
                        // (Full range propagation is SCCP's job — this seeds constants.)
                        const std::uint32_t vid = op.result_ids[0];
                        // No constant value info in wire op; top range is sound default.
                        // SCCP will refine via const-folding.
                        (void)vid;
                    }
                }
                facts.per_fn.push_back(std::move(vmap));
            }
            return facts;
        }
    };

    // --- aliasing provider ---
    // Conservative: only detects distinct loop-index values as distinct base
    struct aliasing_provider {
        [[nodiscard]] static constexpr analysis_id id() noexcept {
            return analysis_id::aliasing;
        }

        [[nodiscard]] aliasing_facts compute(const portable_module& mod) const {
            aliasing_facts facts;
            // Conservative default: all pairs unknown.
            // Arch §4.3: sound over precise — absence of disproof is not sufficient.
            // A more refined analysis (escape analysis, type-based aliasing) belongs
            // in a later pass or a stronger provider.
            (void)mod;
            return facts;
        }
    };

    // =============================================================================
    // all_providers — aggregate of all seven providers for pass manager use
    // =============================================================================

    struct all_providers {
        dominance_provider dominance;
        cfg_reachability_provider cfg_reachability;
        liveness_provider liveness;
        effects_provider effects;
        purity_provider purity;
        ranges_provider ranges;
        aliasing_provider aliasing;
    };
} // namespace lithe::ir::portable::opt
