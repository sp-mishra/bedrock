#pragma once

// Codegen pipeline and optimizer behavior.
// Pipeline shape:
//   expression -> Virtual MIR -> Allocated MIR -> Physical MIR
//              -> MIR pass pipeline -> frame/prologue/epilogue planning
//              -> backend emission
// O-levels are compatibility presets only; users can build custom MIR
// pipelines by adding/removing/enabling/disabling passes.
// SSA and dominator support are optional adapters, not foundational requirements.

#include "lithe_codegen.hpp"
#include "lithe_runtime.hpp"
#include "lithe_semantic.hpp"
#include "lithe_extension.hpp"
#include "containers/graph/DominatorTree.hpp"
#include "lithe_execution/foundation.hpp" // canonical types moved to execution layer

#include <concepts>
#include <expected>
#include <map>
#include <set>
#include <span>
#include <typeinfo>

namespace lithe::codegen {
    // -----------------------------------------------------------------------
    // Group A — CFG analysis structs
    // -----------------------------------------------------------------------

    struct cfg_block_info {
        std::uint32_t block_id = 0;
        bool reachable = false;
        bool is_entry = false;
        bool is_exit = false;
        std::vector<std::uint32_t> predecessors;
        std::vector<std::uint32_t> successors;
    };

    // Classifies a CFG edge by its control-flow semantics.
    // sync_branch   — ordinary conditional or unconditional branch within a sequential context.
    // fallthrough   — implicit sequential fall-through to the next block (no explicit branch).
    // async_fork    — spawns a new concurrent execution context (goroutine, task, fiber, …).
    //                 The target block is treated as a new dominator-tree root.
    // sync_join     — a join point where concurrent contexts re-converge.
    // rpc_boundary  — crosses a distributed-execution boundary (e.g. remote procedure call).
    //                 Triggers subgraph partitioning between local and remote execution domains.
    // entanglement  — quantum/logical entanglement link; non-causal, informational only.
    enum class edge_kind : std::uint8_t {
        sync_branch,
        fallthrough,
        async_fork,
        sync_join,
        rpc_boundary,
        entanglement,
    };

    struct cfg_edge {
        std::uint32_t from = 0;
        std::uint32_t to = 0;
        edge_kind kind = edge_kind::sync_branch;
    };

    // A contiguous cluster of basic blocks that share the same execution domain.
    // Produced by partition_execution_domains() when async_fork or rpc_boundary
    // edges are present in the CFG.
    struct execution_domain {
        // Unique monotone identifier (0 = root / local domain).
        std::uint32_t domain_id = 0;

        // Block that is the local root of this domain:
        //   • for domain 0 it is cfg_analysis_result::entry_block,
        //   • for async_fork domains it is the fork target block,
        //   • for rpc_boundary domains it is the first remote block.
        std::uint32_t root_block = 0;

        // The edge_kind that spawned this domain (nullopt for domain 0).
        std::optional<edge_kind> spawned_by;

        // All basic blocks that belong to this domain.
        std::vector<std::uint32_t> blocks;
    };

    // Partitioning result: one entry per distinct execution domain discovered.
    // When no async_fork or rpc_boundary edges exist this contains exactly one
    // element (the root domain covering every reachable block).
    struct subgraph_partition {
        std::vector<execution_domain> domains;

        // Reverse lookup: block_id → domain_id.
        std::unordered_map<std::uint32_t, std::uint32_t> block_to_domain;

        [[nodiscard]] std::uint32_t domain_of(const std::uint32_t block_id) const noexcept {
            auto it = block_to_domain.find(block_id);
            return it != block_to_domain.end() ? it->second : 0u;
        }

        [[nodiscard]] bool is_partitioned() const noexcept { return domains.size() > 1; }
    };

    struct cfg_analysis_result {
        std::uint32_t entry_block = 0;
        std::vector<control_flow_edge> edges;
        // Typed edges (parallel to edges; produced alongside analyze_cfg).
        std::vector<cfg_edge> typed_edges;
        std::unordered_map<std::uint32_t, basic_block_info> block_info;
        std::vector<std::uint32_t> exit_blocks;
        std::vector<std::uint32_t> reachable_blocks;
        std::vector<std::uint32_t> unreachable_blocks;
        std::vector<std::string> diagnostics;

        // Populated only when async_fork or rpc_boundary edges are detected.
        std::optional<subgraph_partition> partition;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    // -----------------------------------------------------------------------
    // Group B — def-use / reaching defs structs
    // -----------------------------------------------------------------------

    struct definition_site {
        std::uint32_t block_id = 0;
        std::uint32_t instruction_id = 0;
        std::size_t operand_index = 0;

        bool operator==(const definition_site&) const = default;
    };

    struct use_site {
        std::uint32_t block_id = 0;
        std::uint32_t instruction_id = 0;
        std::size_t operand_index = 0;
    };

    struct def_use_chain {
        std::uint32_t value_id = 0;
        definition_site definition;
        std::vector<use_site> uses;
    };

    struct use_def_chain {
        use_site use;
        std::vector<definition_site> reaching_definitions;
    };

    struct value_flow_analysis_result {
        std::unordered_map<std::uint32_t, def_use_chain> def_use_chains;
        std::vector<use_def_chain> use_def_chains;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    struct reaching_definitions_block_state {
        std::uint32_t block_id = 0;
        std::unordered_map<std::uint32_t, definition_site> in;
        std::unordered_map<std::uint32_t, definition_site> out;
    };

    struct reaching_definitions_result {
        std::unordered_map<std::uint32_t, reaching_definitions_block_state> per_block;
        std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, definition_site>> before_instruction;
        std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, definition_site>> after_instruction;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    // -----------------------------------------------------------------------
    // Dominator analysis structs (must precede mir_analysis_cache)
    // -----------------------------------------------------------------------

    struct dominator_analysis_options {
        bool compute_frontier = false;
        bool compute_loop_headers = false;

        bool operator==(const dominator_analysis_options&) const = default;
    };

    // -----------------------------------------------------------------------
    // Loop analysis types
    //
    // Defined here (before the pass structs) so optimization passes can hold
    // loop_analysis_result by value.  analyze_loops() is forward-declared in
    // the forward-declarations block and defined near the end of the file.
    // -----------------------------------------------------------------------

    struct loop_info {
        std::uint32_t header = 0;

        // All blocks that are part of this natural loop (includes header).
        std::unordered_set<std::uint32_t> body;

        // Back edges that induce this loop: {from, to} where to == header.
        struct back_edge {
            std::uint32_t from = 0;
            std::uint32_t to = 0;
        };

        std::vector<back_edge> back_edges;

        // Blocks that have at least one successor outside the loop.
        std::unordered_set<std::uint32_t> exit_blocks;
    };

    struct loop_analysis_result {
        // One entry per unique loop header.  Multiple back edges into the same
        // header are merged into a single loop_info.
        std::vector<loop_info> loops;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }

        // True if any loop contains block_id.
        [[nodiscard]] bool in_any_loop(const std::uint32_t block_id) const noexcept {
            for (const auto& l : loops) {
                if (l.body.contains(block_id)) return true;
            }
            return false;
        }

        // True if block_id is a loop header.
        [[nodiscard]] bool is_loop_header(const std::uint32_t block_id) const noexcept {
            for (const auto& l : loops) {
                if (l.header == block_id) return true;
            }
            return false;
        }

        // Returns the loop whose body contains block_id, or nullptr.
        [[nodiscard]] const loop_info* loop_containing(const std::uint32_t block_id) const noexcept {
            for (const auto& l : loops) {
                if (l.body.contains(block_id)) return &l;
            }
            return nullptr;
        }

        // True if an edge from→to crosses a loop back edge (i.e. to is a loop
        // header and from is a back-edge source in that loop).
        [[nodiscard]] bool is_back_edge(const std::uint32_t from, const std::uint32_t to) const noexcept {
            for (const auto& l : loops) {
                if (l.header != to) continue;
                for (const auto& be : l.back_edges) {
                    if (be.from == from) return true;
                }
            }
            return false;
        }
    };

    struct dominator_analysis_result {
        mir::physical_mir_function function;

        // Dominator tree for the root (sequential) execution domain.
        // async_fork target blocks will have a null immediate_dominator here;
        // their dominance info lives in sub_domain_doms (indexed by domain_id).
        litegraph::dominator_result<std::uint32_t> dom;

        // Per-domain dominator trees for async_fork and rpc_boundary sub-trees.
        // Key = execution_domain::domain_id (0 is always absent — use dom above).
        // Populated only when the CFG's subgraph_partition has more than one domain.
        std::unordered_map<std::uint32_t,
                           litegraph::dominator_result<std::uint32_t>> sub_domain_doms;

        std::unordered_set<std::uint32_t> loop_header_blocks;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }

        // Query helper: true iff a dominates b, searching the correct sub-tree.
        // Requires sub_domain_doms to have been populated (compute_dominators with
        // the cfg that has a partition).
        [[nodiscard]] bool dominates_in_domain(const std::uint32_t a, const std::uint32_t b,
                                               const std::uint32_t domain_id) const noexcept {
            if (domain_id == 0) return litegraph::dominates(dom, a, b);
            auto it = sub_domain_doms.find(domain_id);
            if (it == sub_domain_doms.end()) return false;
            return litegraph::dominates(it->second, a, b);
        }
    };

    [[nodiscard]] inline dominator_analysis_result compute_dominators(
        mir::physical_mir_function const& fn,
        dominator_analysis_options options = {}
    );

    [[nodiscard]] inline bool dominates(
        dominator_analysis_result const& r,
        std::uint32_t a, std::uint32_t b
    );

    // -----------------------------------------------------------------------
    // Group C — analysis cache
    // -----------------------------------------------------------------------

    struct mir_analysis_cache {
        std::optional<cfg_analysis_result> cfg;
        std::optional<value_flow_analysis_result> def_use;
        std::optional<reaching_definitions_result> reaching_definitions;
        std::optional<register_pressure_result> register_pressure;
        std::optional<dominator_analysis_result> dominators;
        std::optional<loop_analysis_result> loops;
        // Options used when dominators was last computed.  Used to detect mismatches.
        dominator_analysis_options cached_dominator_options = {};

        void invalidate_analysis(const mir_analysis_kind kind) {
            switch (kind) {
            case mir_analysis_kind::cfg:
                cfg.reset();
                dominators.reset(); // dominators depend on CFG
                loops.reset(); // loop analysis depends on CFG
                break;
            case mir_analysis_kind::def_use: def_use.reset();
                break;
            case mir_analysis_kind::reaching_definitions: reaching_definitions.reset();
                break;
            case mir_analysis_kind::register_pressure: register_pressure.reset();
                break;
            case mir_analysis_kind::value_flow: def_use.reset();
                break;
            case mir_analysis_kind::dominators: dominators.reset();
                break;
            case mir_analysis_kind::loop_analysis: loops.reset();
                break;
            }
        }

        void invalidate_all() {
            cfg.reset();
            def_use.reset();
            reaching_definitions.reset();
            register_pressure.reset();
            dominators.reset();
            loops.reset();
        }

        [[nodiscard]] bool has_analysis(const mir_analysis_kind kind) const {
            switch (kind) {
            case mir_analysis_kind::cfg: return cfg.has_value();
            case mir_analysis_kind::def_use: return def_use.has_value();
            case mir_analysis_kind::reaching_definitions: return reaching_definitions.has_value();
            case mir_analysis_kind::register_pressure: return register_pressure.has_value();
            case mir_analysis_kind::value_flow: return def_use.has_value();
            case mir_analysis_kind::dominators: return dominators.has_value();
            case mir_analysis_kind::loop_analysis: return loops.has_value();
            }
            return false;
        }
    };

    // -----------------------------------------------------------------------
    // Group D — pass infrastructure
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // transient_arena
    //
    // Bump allocator backed by a local std::vector<std::byte>.  Intended for
    // short-lived scratch allocations inside a single pass (dominator trees,
    // PDG work-sets, etc.).  When the arena goes out of scope — or reset() is
    // called — all memory is released in O(1) without per-object destruction
    // cost.  Objects stored here must be trivially destructible or the caller
    // must destroy them explicitly before reset().
    //
    // Alignment: every allocation is aligned to alignof(std::max_align_t).
    // Overflow: if the requested size exceeds the remaining capacity the
    // arena falls back to a fresh heap allocation appended to overflow_.
    //
    // Usage inside a pass:
    //   auto *scratch = ctx.arena.alloc<std::uint32_t>(block_count);
    //   // … use scratch …
    //   ctx.arena.reset();   // or let the pass context be destroyed
    // -----------------------------------------------------------------------
    struct transient_arena {
        static constexpr std::size_t default_capacity = 256 * 1024; // 256 KiB

        explicit transient_arena(const std::size_t cap = default_capacity) {
            buf_.resize(cap);
        }

        // Allocate n objects of type T (uninitialized, trivially destructible).
        template <class T>
        [[nodiscard]] T* alloc(const std::size_t n = 1) {
            static_assert(std::is_trivially_destructible_v<T>,
                          "transient_arena: T must be trivially destructible "
                          "(no destructor will be called on reset)");
            const std::size_t bytes = sizeof(T) * n;
            const std::size_t align = alignof(T);
            // Align cursor up.
            const std::size_t aligned_cursor =
                (cursor_ + align - 1u) & ~(align - 1u);
            if (aligned_cursor + bytes <= buf_.size()) {
                T* p = reinterpret_cast<T*>(buf_.data() + aligned_cursor);
                cursor_ = aligned_cursor + bytes;
                return p;
            }
            // Overflow: fall back to heap vector owned by this arena.
            auto& slot = overflow_.emplace_back(bytes + align);
            void* raw = slot.data();
            std::size_t space = slot.size();
            void* aligned = std::align(align, bytes, raw, space);
            return reinterpret_cast<T*>(aligned);
        }

        // Reset cursor; overflow heap allocations are freed.
        void reset() noexcept {
            cursor_ = 0;
            overflow_.clear();
        }

        [[nodiscard]] std::size_t used() const noexcept { return cursor_; }
        [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

    private:
        std::vector<std::byte> buf_;
        std::size_t cursor_ = 0;
        std::vector<std::vector<std::byte>> overflow_;
    };

    struct mir_pass_statistics {
        std::size_t executed_passes = 0;
        std::size_t changed_passes = 0;
        std::size_t unchanged_passes = 0;
        std::size_t total_removed_instructions = 0;
        std::size_t total_removed_blocks = 0;
        std::size_t total_added_instructions = 0;
        std::size_t total_added_blocks = 0;
        std::size_t total_changed_instructions = 0;
        std::size_t total_changed_blocks = 0;
        std::size_t total_unchanged_instructions = 0;
        std::size_t total_unchanged_blocks = 0;
        std::vector<std::string> executed_pass_names;
        std::unordered_map<std::string, std::size_t> removed_instructions_by_pass;
        std::unordered_map<std::string, std::size_t> removed_blocks_by_pass;
    };

    enum class mir_pass_trace_event_kind : std::uint8_t {
        pass_begin,
        pass_end,
        verification_success,
        verification_failure,
        analysis_invalidated,
        diagnostic
    };

    struct mir_pass_trace_event {
        mir_pass_trace_event_kind kind = mir_pass_trace_event_kind::pass_begin;
        std::string pass_name;
        std::size_t instruction_count_before = 0;
        std::size_t instruction_count_after = 0;
        std::size_t block_count_before = 0;
        std::size_t block_count_after = 0;
        std::string message;
    };

    struct mir_pass_trace_log {
        std::vector<mir_pass_trace_event> events;

        void add(mir_pass_trace_event event) {
            events.push_back(std::move(event));
        }

        [[nodiscard]] bool empty() const { return events.empty(); }
    };

    // -----------------------------------------------------------------------
    // Group A.1 — Block-level profiling data
    //
    // Stored as a sorted flat vector so iteration is cache-linear.
    // Incrementing an existing entry is O(log N) via lower_bound; N is the
    // number of distinct basic blocks seen — typically < 64, so the constant
    // factor dominates and the flat layout wins over hash maps.
    // -----------------------------------------------------------------------

    struct profiling_data {
        struct block_counter {
            std::uint32_t block_id = 0;
            std::uint64_t execution_count = 0;

            // Sorted by block_id for binary-search increments.
            bool operator<(const block_counter& o) const noexcept {
                return block_id < o.block_id;
            }
        };

        // Sorted by block_id; maintained in order by increment().
        std::vector<block_counter> counters;

        // Minimum execution count to classify a block as "hot".
        // Callers may override; defaults to 100 (empirically good for short loops).
        std::uint64_t hot_threshold = 100;

        void increment(const std::uint32_t block_id) noexcept {
            const auto it = std::lower_bound(
                counters.begin(), counters.end(),
                block_counter{block_id, 0}
            );
            if (it != counters.end() && it->block_id == block_id) {
                ++it->execution_count;
            }
            else {
                counters.insert(it, block_counter{block_id, 1});
            }
        }

        [[nodiscard]] std::uint64_t count_of(const std::uint32_t block_id) const noexcept {
            const auto it = std::lower_bound(
                counters.begin(), counters.end(),
                block_counter{block_id, 0}
            );
            return (it != counters.end() && it->block_id == block_id)
                       ? it->execution_count
                       : 0u;
        }

        [[nodiscard]] bool is_hot(const std::uint32_t block_id) const noexcept {
            return count_of(block_id) >= hot_threshold;
        }

        [[nodiscard]] std::vector<std::uint32_t> hot_blocks() const {
            std::vector<std::uint32_t> result;
            for (const auto& c : counters) {
                if (c.execution_count >= hot_threshold) {
                    result.push_back(c.block_id);
                }
            }
            return result;
        }

        void reset() noexcept { counters.clear(); }
    };

    struct mir_pass_context {
        std::vector<std::string> diagnostics;
        mir_pass_statistics statistics;
        std::size_t optimization_budget = 0;
        bool changed = false;
        bool verify_after_each_pass = true;
        mir_analysis_cache analysis_cache;
        bool enable_trace = false;
        mir_pass_trace_log trace;

        // Live block-execution counts populated by interpreter_backend::emit
        // during Tier 1 interpretation.  Passed into tiered_compile so Tier 2
        // can identify and selectively optimize hot subgraphs.
        profiling_data profiling;

        // Scratch memory for short-lived pass-internal allocations (dominator
        // work-sets, PDG scratch buffers, etc.).  Automatically freed when the
        // context is destroyed or when a pass calls ctx.arena.reset().
        transient_arena arena;
    };

    // -----------------------------------------------------------------------
    // Monadic pass result types
    //
    // mir_pass_result   — plain struct returned by individual pass run() methods.
    //                     Preserved as-is for backward compatibility with all
    //                     existing passes and tests.
    // pass_error        — structured failure payload for the monadic pipeline.
    // mir_pass_expected — std::expected<mir_pass_result, pass_error>.
    //                     The pipeline's runner_type and and_then chain use this
    //                     type; individual passes still return mir_pass_result.
    //
    // A pass signals failure by returning a mir_pass_result whose diagnostics
    // are non-empty (existing convention, unchanged).  The pipeline's and_then
    // wrapper promotes a non-empty diagnostics list to std::unexpected so the
    // rest of the pipeline is skipped.
    //
    // For a pass that wants to hard-fail immediately, the runner_type lambda
    // can also return std::unexpected(pass_error{…}) directly.
    // -----------------------------------------------------------------------

    // Plain result struct — unchanged public API for all pass run() methods.
    struct mir_pass_result {
        mir::physical_mir_function function;
        std::size_t removed_instructions = 0;
        std::size_t removed_blocks = 0;
        std::vector<std::string> diagnostics;
        bool changed = false;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    // Error payload for the monadic pipeline layer.
    struct pass_error {
        std::string pass_name;
        std::vector<std::string> messages;

        [[nodiscard]] static pass_error from_message(const std::string_view name,
                                                     std::string msg) {
            pass_error e;
            e.pass_name = std::string{name};
            e.messages.push_back(std::move(msg));
            return e;
        }
    };

    // The monadic pipeline type.  runner_type lambdas and the and_then chain
    // use this; individual passes do NOT return this type directly.
    using mir_pass_expected = std::expected<mir_pass_result, pass_error>;

    // -----------------------------------------------------------------------
    // Forward declarations
    // -----------------------------------------------------------------------

    [[nodiscard]] inline cfg_analysis_result analyze_cfg(const mir::physical_mir_function& fn);

    // Partitions the reachable blocks in cfg into execution domains by performing a
    // DFS that opens a new domain whenever an async_fork or rpc_boundary edge is
    // crossed.  Always returns at least one domain (domain 0 = root).
    [[nodiscard]] inline subgraph_partition partition_execution_domains(
        cfg_analysis_result const& cfg);

    [[nodiscard]] inline value_flow_analysis_result analyze_def_use(const mir::physical_mir_function& fn);

    [[nodiscard]] inline reaching_definitions_result compute_reaching_definitions(const mir::physical_mir_function& fn);

    [[nodiscard]] inline mir::verification_result validate_branch_targets(const mir::physical_mir_function& fn);

    [[nodiscard]] inline mir::verification_result validate_cfg(const mir::physical_mir_function& fn);

    [[nodiscard]] inline cfg_analysis_result const& get_or_compute_cfg(
        mir_pass_context& ctx, const mir::physical_mir_function& fn);

    [[nodiscard]] inline value_flow_analysis_result const& get_or_compute_def_use(
        mir_pass_context& ctx, const mir::physical_mir_function& fn);

    [[nodiscard]] inline reaching_definitions_result const& get_or_compute_reaching_definitions(
        mir_pass_context& ctx, const mir::physical_mir_function& fn);

    [[nodiscard]] inline dominator_analysis_result const& get_or_compute_dominators(
        mir_pass_context& ctx,
        mir::physical_mir_function const& fn,
        dominator_analysis_options options = {});

    [[nodiscard]] inline std::vector<std::uint32_t> compute_reachable_blocks(const cfg_analysis_result& cfg);

    [[nodiscard]] inline std::vector<std::uint32_t> compute_exit_blocks(const cfg_analysis_result& cfg);

    [[nodiscard]] inline std::vector<std::uint32_t> topological_block_order(const cfg_analysis_result& cfg);

    [[nodiscard]] inline std::vector<std::uint32_t> reverse_postorder_block_order(const cfg_analysis_result& cfg);

    [[nodiscard]] inline loop_analysis_result analyze_loops(mir::physical_mir_function const& fn);

    [[nodiscard]] inline std::string dump_mir_pass_trace(const mir_pass_trace_log& log);

    [[nodiscard]] inline std::string dump_dominator_analysis(
        dominator_analysis_result const& result);

    struct ssa_adapter_options {
        // When true: skip actual construction and return the function unchanged.
        // Default (false): run full Cytron SSA construction (phi placement + rename).
        bool placeholder_only = false;
        // Only version integer pregs.  Spill / memory operands are never versioned.
        bool integer_pregs_only = true;
    };

    struct ssa_construction_result {
        mir::physical_mir_function function;
        // Per-block SSA state: phi nodes placed and reaching-version maps.
        std::unordered_map<std::uint32_t, ssa_block_state> block_states;
        // Global SSA value table: ssa_value_id → ssa_value descriptor.
        std::unordered_map<std::uint64_t, ssa_value> value_table;
        std::vector<std::string> diagnostics;
        // Non-fatal informational notes (counts, phase availability, etc.).
        std::vector<std::string> info;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    [[nodiscard]] inline ssa_construction_result construct_ssa(
        const mir::physical_mir_function& fn,
        const ssa_adapter_options& options = {});

    struct ssa_destruction_result {
        mir::physical_mir_function function;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    [[nodiscard]] inline ssa_destruction_result destroy_ssa(
        const ssa_construction_result& result);

    struct ssa_verification_result {
        bool valid = true;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return valid && diagnostics.empty(); }
    };

    [[nodiscard]] inline ssa_verification_result verify_ssa(
        const ssa_construction_result& result);

    // Convert a cfg_analysis_result into the backend-neutral graph view consumed
    // by litegraph::compute_dominators.
    //
    // async_fork edges are excluded: the Lengauer-Tarjan solver then cannot reach
    // the fork-target block from the entry, which means the standard idom solver
    // leaves those blocks with a null immediate dominator — exactly the semantics
    // we want for "this block is the root of its own dominator sub-tree."
    // Callers that need per-domain dominator trees should call compute_dominators,
    // which handles the multi-root case via the cfg's subgraph_partition.
    [[nodiscard]] inline litegraph::dominator_graph_view<std::uint32_t>
    to_dominator_graph_view(cfg_analysis_result const& cfg) {
        // Build a set of edges to suppress (async_fork only; rpc_boundary is kept
        // so the remote blocks remain reachable from the global root — the remote
        // domain's root will be identified separately in compute_dominators).
        std::unordered_set<std::uint64_t> suppressed; // packed (from<<32)|to
        for (const auto& te : cfg.typed_edges) {
            if (te.kind == edge_kind::async_fork) {
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(te.from) << 32) | te.to;
                suppressed.insert(key);
            }
        }

        litegraph::dominator_graph_view<std::uint32_t> view;
        view.entry = cfg.entry_block;
        view.nodes.reserve(cfg.block_info.size());
        for (auto const& [id, info] : cfg.block_info) {
            view.nodes.push_back(id);
            for (auto const succ : info.successors) {
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(id) << 32) | succ;
                if (suppressed.count(key)) continue;
                view.successors[id].push_back(succ);
                view.predecessors[succ].push_back(id);
            }
        }
        // Ensure every node has at least an empty predecessor/successor entry.
        for (auto const& [id, _] : cfg.block_info) {
            view.predecessors.try_emplace(id);
            view.successors.try_emplace(id);
        }
        return view;
    }

    struct peephole_options {
        bool remove_redundant_mov = true;
        bool remove_nop = true;
        bool fold_redundant_spill_roundtrip = true;
        bool remove_unreachable_empty_blocks = true;
    };

    struct peephole_result {
        mir::physical_mir_function function;
        std::size_t removed_instructions = 0;
        std::size_t removed_blocks = 0;
        std::vector<std::string> diagnostics;
        bool changed = false;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    [[nodiscard]] inline peephole_result run_mir_peephole(
        const mir::physical_mir_function& fn,
        const peephole_options& options = {}
    );

    struct trivial_jump_threading_pass {
        [[nodiscard]] mir_pass_result run(mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            mir_pass_result out;
            out.function = fn;

            const std::uint32_t entry = out.function.function.cfg.entry_block;

            // Build forwarding map: block_id -> final target for every non-entry
            // block whose sole instruction is an unconditional branch.
            std::unordered_map<std::uint32_t, std::uint32_t> forward_to;
            for (const auto& block : out.function.function.blocks) {
                if (block.id == entry) continue;
                if (block.instructions.size() != 1) continue;
                const auto& inst = block.instructions[0];
                if (inst.op != opcode::branch) continue;
                for (const auto& use : inst.uses) {
                    if (use.type == allocated_operand::kind::block) {
                        const auto target = std::get<std::uint32_t>(use.value);
                        if (target != block.id) {
                            // exclude self-loops
                            forward_to[block.id] = target;
                        }
                        break;
                    }
                }
            }

            if (forward_to.empty()) {
                return out;
            }

            // Resolve chains to their final destination.
            bool chain_changed = true;
            for (std::size_t iter = 0; iter < forward_to.size() && chain_changed; ++iter) {
                chain_changed = false;
                for (auto& [src, tgt] : forward_to) {
                    if (const auto it = forward_to.find(tgt); it != forward_to.end()) {
                        tgt = it->second;
                        chain_changed = true;
                    }
                }
            }

            // Rewrite only block-kind operands in branch/branch_cond instructions.
            bool any_rewritten = false;
            for (auto& block : out.function.function.blocks) {
                for (auto& inst : block.instructions) {
                    if (inst.op != opcode::branch && inst.op != opcode::branch_cond) continue;
                    for (auto& use : inst.uses) {
                        if (use.type != allocated_operand::kind::block) continue;
                        const auto orig = std::get<std::uint32_t>(use.value);
                        if (const auto it = forward_to.find(orig); it != forward_to.end()) {
                            use.value = it->second;
                            any_rewritten = true;
                        }
                    }
                }
            }

            if (!any_rewritten) {
                return out;
            }

            // Rebuild successor/predecessor lists and CFG maps from the updated
            // branch instructions so they stay consistent with the new targets.
            out.function.function.cfg.successors.clear();
            out.function.function.cfg.predecessors.clear();
            for (auto& block : out.function.function.blocks) {
                block.successors.clear();
                for (const auto& inst : block.instructions) {
                    if (inst.op != opcode::branch && inst.op != opcode::branch_cond) continue;
                    for (const auto& use : inst.uses) {
                        if (use.type != allocated_operand::kind::block) continue;
                        block.successors.push_back(std::get<std::uint32_t>(use.value));
                    }
                }
                std::ranges::sort(block.successors);
                block.successors.erase(std::unique(block.successors.begin(), block.successors.end()),
                                       block.successors.end());
                out.function.function.cfg.successors[block.id] = block.successors;
            }
            for (const auto& block : out.function.function.blocks) {
                for (const std::uint32_t succ : block.successors) {
                    out.function.function.cfg.predecessors[succ].push_back(block.id);
                }
            }
            for (auto& block : out.function.function.blocks) {
                block.predecessors = out.function.function.cfg.predecessors[block.id];
                std::ranges::sort(block.predecessors);
                block.predecessors.erase(std::unique(block.predecessors.begin(), block.predecessors.end()),
                                         block.predecessors.end());
            }

            out.changed = true;
            ctx.changed = true;

            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "trivial_jump_threading: verification failed after rewriting branch targets; "
                    "returning original physical MIR");
                out.diagnostics.insert(out.diagnostics.end(),
                                       verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.changed = false;
                ctx.changed = false;
                return out;
            }

            return out;
        }
    };

    struct empty_block_merge_pass {
        [[nodiscard]] mir_pass_result run(mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            mir_pass_result out;
            out.function = fn;

            const std::uint32_t entry = out.function.function.cfg.entry_block;

            // Build a map: block_id -> unconditional branch target, for every
            // eligible forwarding block (not entry, exactly one branch instruction).
            std::unordered_map<std::uint32_t, std::uint32_t> forward_to;
            for (const auto& block : out.function.function.blocks) {
                if (block.id == entry) continue;
                if (block.instructions.size() != 1) continue;
                const auto& inst = block.instructions[0];
                if (inst.op != opcode::branch) continue;
                // Find the single block operand.
                std::uint32_t target = 0;
                bool found = false;
                for (const auto& use : inst.uses) {
                    if (use.type == allocated_operand::kind::block) {
                        target = std::get<std::uint32_t>(use.value);
                        found = true;
                        break;
                    }
                }
                if (!found) continue;
                if (target == block.id) continue; // self-loop: not safe
                forward_to[block.id] = target;
            }

            if (forward_to.empty()) {
                return out;
            }

            // Resolve chains: if A->B and B->C both in forward_to, redirect A->C.
            // Iterate to fixed point (bounded by map size to avoid infinite loops).
            bool changed_chain = true;
            for (std::size_t iter = 0; iter < forward_to.size() && changed_chain; ++iter) {
                changed_chain = false;
                for (auto& [src, tgt] : forward_to) {
                    if (const auto it = forward_to.find(tgt); it != forward_to.end()) {
                        tgt = it->second;
                        changed_chain = true;
                    }
                }
            }

            // Patch all branch instructions in every block to skip forwarding blocks.
            for (auto& block : out.function.function.blocks) {
                for (auto& inst : block.instructions) {
                    if (inst.op != opcode::branch && inst.op != opcode::branch_cond) continue;
                    for (auto& use : inst.uses) {
                        if (use.type != allocated_operand::kind::block) continue;
                        const auto orig = std::get<std::uint32_t>(use.value);
                        if (const auto it = forward_to.find(orig); it != forward_to.end()) {
                            use.value = it->second;
                        }
                    }
                }
                // Patch successors list.
                for (auto& succ : block.successors) {
                    if (const auto it = forward_to.find(succ); it != forward_to.end()) {
                        succ = it->second;
                    }
                }
            }

            // Remove forwarding blocks that are no longer the entry and are now
            // bypassed by all their predecessors.
            const std::unordered_set<std::uint32_t> forwarding_ids = [&] {
                std::unordered_set<std::uint32_t> s;
                for (const auto& [id, _] : forward_to) s.insert(id);
                return s;
            }();

            std::vector<allocated_basic_block> kept;
            kept.reserve(out.function.function.blocks.size());
            for (const auto& block : out.function.function.blocks) {
                if (forwarding_ids.contains(block.id)) {
                    ++out.removed_blocks;
                }
                else {
                    kept.push_back(block);
                }
            }
            out.function.function.blocks = std::move(kept);

            // Rebuild CFG maps from the patched branch instructions (ground truth),
            // not from the stale successor lists which may still contain forwarding
            // block IDs that were redirected but not yet purged.
            out.function.function.cfg.successors.clear();
            out.function.function.cfg.predecessors.clear();
            for (auto& block : out.function.function.blocks) {
                block.successors.clear();
                for (const auto& inst : block.instructions) {
                    if (inst.op != opcode::branch && inst.op != opcode::branch_cond) continue;
                    for (const auto& use : inst.uses) {
                        if (use.type != allocated_operand::kind::block) continue;
                        block.successors.push_back(std::get<std::uint32_t>(use.value));
                    }
                }
                std::ranges::sort(block.successors);
                block.successors.erase(std::unique(block.successors.begin(), block.successors.end()),
                                       block.successors.end());
                block.predecessors.clear();
                out.function.function.cfg.successors[block.id] = block.successors;
            }
            for (const auto& block : out.function.function.blocks) {
                for (const std::uint32_t succ : block.successors) {
                    out.function.function.cfg.predecessors[succ].push_back(block.id);
                }
            }
            for (auto& block : out.function.function.blocks) {
                block.predecessors = out.function.function.cfg.predecessors[block.id];
                std::ranges::sort(block.predecessors);
                block.predecessors.erase(std::unique(block.predecessors.begin(), block.predecessors.end()),
                                         block.predecessors.end());
            }

            out.changed = true;
            ctx.changed = true;

            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "empty_block_merge: verification failed after merging forwarding blocks; "
                    "returning original physical MIR");
                out.diagnostics.insert(out.diagnostics.end(),
                                       verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.removed_blocks = 0;
                out.changed = false;
                ctx.changed = false;
                return out;
            }

            return out;
        }
    };

    struct unreachable_block_elimination_pass {
        [[nodiscard]] mir_pass_result run(mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            const auto cfg = analyze_cfg(fn);

            mir_pass_result out;
            out.function = fn;

            if (cfg.unreachable_blocks.empty()) {
                return out;
            }

            // Explicitly guard: never treat the entry block as dead, regardless of
            // what the CFG analysis returns (defensive against a buggy/empty CFG).
            const std::uint32_t entry = out.function.function.cfg.entry_block;
            std::unordered_set<std::uint32_t> dead;
            dead.reserve(cfg.unreachable_blocks.size());
            for (const std::uint32_t id : cfg.unreachable_blocks) {
                if (id != entry) dead.insert(id);
            }

            if (dead.empty()) {
                return out;
            }

            std::vector<allocated_basic_block> kept;
            kept.reserve(out.function.function.blocks.size());
            for (const auto& block : out.function.function.blocks) {
                if (dead.contains(block.id)) {
                    ++out.removed_blocks;
                }
                else {
                    kept.push_back(block);
                }
            }
            out.function.function.blocks = std::move(kept);

            // Rebuild CFG maps after removing blocks. Also strip branch operands
            // that still reference dead block IDs (can arise from branch_cond whose
            // true- or false-target was unreachable) so branch targets stay valid.
            out.function.function.cfg.successors.clear();
            out.function.function.cfg.predecessors.clear();
            for (auto& block : out.function.function.blocks) {
                // Strip dead targets from branch instructions.
                for (auto& inst : block.instructions) {
                    if (inst.op != opcode::branch && inst.op != opcode::branch_cond) continue;
                    inst.uses.erase(
                        std::remove_if(inst.uses.begin(), inst.uses.end(),
                                       [&dead](const allocated_operand& use) {
                                           return use.type == allocated_operand::kind::block
                                               && dead.contains(std::get<std::uint32_t>(use.value));
                                       }),
                        inst.uses.end());
                }
                block.successors.erase(
                    std::remove_if(block.successors.begin(), block.successors.end(),
                                   [&dead](const std::uint32_t t) { return dead.contains(t); }),
                    block.successors.end());
                std::ranges::sort(block.successors);
                block.successors.erase(std::unique(block.successors.begin(), block.successors.end()),
                                       block.successors.end());
                block.predecessors.clear();
                out.function.function.cfg.successors[block.id] = block.successors;
            }
            for (const auto& block : out.function.function.blocks) {
                for (const std::uint32_t succ : block.successors) {
                    out.function.function.cfg.predecessors[succ].push_back(block.id);
                }
            }
            for (auto& block : out.function.function.blocks) {
                block.predecessors = out.function.function.cfg.predecessors[block.id];
                std::ranges::sort(block.predecessors);
                block.predecessors.erase(std::unique(block.predecessors.begin(), block.predecessors.end()),
                                         block.predecessors.end());
            }

            out.changed = true;
            ctx.changed = true;

            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "unreachable_block_elimination: verification failed after removing unreachable blocks; "
                    "returning original physical MIR");
                out.diagnostics.insert(out.diagnostics.end(),
                                       verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.removed_blocks = 0;
                out.changed = false;
                ctx.changed = false;
                return out;
            }

            return out;
        }
    };

    struct dead_def_elimination_pass {
        [[nodiscard]] mir_pass_result run(mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            // ---------------------------------------------------------------
            // Phase 1: build a global used-pregs set, identical to original.
            // This handles same-block and trivially cross-block cases.
            // ---------------------------------------------------------------
            std::unordered_set<std::uint32_t> used_pregs;

            if (ctx.analysis_cache.def_use.has_value()) {
                // Extract used preg ids from the cached def-use chains.
                for (const auto& [preg_id, chain] : ctx.analysis_cache.def_use->def_use_chains) {
                    if (!chain.uses.empty()) {
                        used_pregs.insert(preg_id);
                    }
                }
                // Also scan use_def_chains to capture uses with no reaching def
                // (e.g. live-in pregs that are never explicitly defined in the fn).
                for (const auto& udc : ctx.analysis_cache.def_use->use_def_chains) {
                    const auto& block_it = std::ranges::find_if(fn.function.blocks,
                                                                [&](const auto& b) {
                                                                    return b.id == udc.use.block_id;
                                                                });
                    if (block_it == fn.function.blocks.end()) continue;
                    const auto& inst_it = std::ranges::find_if(block_it->instructions,
                                                               [&](const auto& i) {
                                                                   return i.id == udc.use.instruction_id;
                                                               });
                    if (inst_it == block_it->instructions.end()) continue;
                    if (udc.use.operand_index >= inst_it->uses.size()) continue;
                    const auto& use_op = inst_it->uses[udc.use.operand_index];
                    if (use_op.type == allocated_operand::kind::preg) {
                        used_pregs.insert(std::get<preg>(use_op.value).id);
                    }
                }
            }
            else {
                // Conservative scan: collect every preg id that appears in any use.
                for (const auto& block : fn.function.blocks) {
                    for (const auto& inst : block.instructions) {
                        for (const auto& use_op : inst.uses) {
                            if (use_op.type != allocated_operand::kind::preg) continue;
                            used_pregs.insert(std::get<preg>(use_op.value).id);
                        }
                    }
                }
            }

            // ---------------------------------------------------------------
            // Phase 2: dominance + reaching-definition guard.
            //
            // For each instruction's preg def, verify that no reachable use
            // in the function has that instruction as its sole reaching
            // definition.  Also conservatively preserve any def in a
            // loop-header block (loop-carried def guard).
            //
            // Falls back to Phase 1 only (conservative) if either analysis
            // reports diagnostics.
            // ---------------------------------------------------------------

            // dead_by_reaching_def: instruction ids that Phase 2 confirms dead
            // (i.e. no reaching-def use exists for any of their preg defs).
            // Instructions NOT in this set that appear dead by Phase 1 are
            // still eligible for removal — Phase 2 only *adds* protection,
            // it never overrides Phase 1's correct removals.
            std::unordered_set<std::uint32_t> phase2_keep; // extra instructions to preserve

            do {
                const auto dom_result = compute_dominators(fn, {.compute_loop_headers = true});
                const auto reach_result = compute_reaching_definitions(fn);
                if (!dom_result.ok() || !reach_result.ok()) break;

                // Loop-header block ids: defs in these blocks may be loop-carried.
                const auto& loop_headers_set = dom_result.loop_header_blocks;

                // Build block-id → block lookup for fast access.
                std::unordered_map<std::uint32_t, const allocated_basic_block*> block_by_id;
                for (const auto& block : fn.function.blocks) {
                    block_by_id[block.id] = &block;
                }

                // For each instruction that Phase 1 would remove (preg def,
                // not side-effecting), run the dominance/reaching-def check.
                for (const auto& block : fn.function.blocks) {
                    // Conservatively keep all defs in loop-header blocks.
                    if (loop_headers_set.contains(block.id)) {
                        for (const auto& inst : block.instructions) {
                            phase2_keep.insert(inst.id);
                        }
                        continue;
                    }

                    for (const auto& inst : block.instructions) {
                        // Only consider instructions that Phase 1 might remove:
                        // they have at least one preg def and are not side-effecting.
                        bool has_preg_def = std::ranges::any_of(inst.defs, [](const auto& d) {
                            return d.type == allocated_operand::kind::preg;
                        });
                        if (!has_preg_def) continue;

                        // For each preg def of this instruction, check whether any
                        // use in the whole function has this instruction as its
                        // reaching definition at that use site.
                        for (const auto& def_op : inst.defs) {
                            if (def_op.type != allocated_operand::kind::preg) continue;
                            const auto p_id = static_cast<std::uint32_t>(
                                std::get<preg>(def_op.value).id);

                            // Scan all instructions for uses of p_id where the
                            // reaching def at that point is this instruction.
                            for (const auto& use_block : fn.function.blocks) {
                                for (const auto& use_inst : use_block.instructions) {
                                    // Is p_id used by use_inst?
                                    bool uses_p = std::ranges::any_of(use_inst.uses, [&](const auto& u) {
                                        return u.type == allocated_operand::kind::preg
                                            && std::get<preg>(u.value).id == p_id;
                                    });
                                    if (!uses_p) continue;

                                    // What reaching def of p_id arrives before use_inst?
                                    const auto before_it =
                                        reach_result.before_instruction.find(use_inst.id);
                                    if (before_it == reach_result.before_instruction.end()) {
                                        // No reaching-def info → be conservative: keep.
                                        phase2_keep.insert(inst.id);
                                        goto next_inst;
                                    }
                                    const auto rd_it = before_it->second.find(p_id);
                                    if (rd_it == before_it->second.end()) {
                                        // preg is used but has no reaching def here
                                        // (live-in from caller?) → keep conservatively.
                                        phase2_keep.insert(inst.id);
                                        goto next_inst;
                                    }
                                    if (rd_it->second.instruction_id == inst.id) {
                                        // This def is the reaching def for a live use.
                                        phase2_keep.insert(inst.id);
                                        goto next_inst;
                                    }
                                    // reaching def is a different instruction — this
                                    // particular use does not keep our inst alive; but
                                    // the def of p_id that does reach here is a
                                    // different instruction.  Check whether the other
                                    // reaching def is in a loop header (ambiguous path):
                                    // if its defining block is a loop header keep ours
                                    // conservatively too.
                                    if (loop_headers_set.contains(rd_it->second.block_id)) {
                                        phase2_keep.insert(inst.id);
                                        goto next_inst;
                                    }
                                }
                            }
                        next_inst:;
                        }
                    }
                }
            }
            while (false);

            // ---------------------------------------------------------------
            // Build the result: remove instructions that are dead by Phase 1
            // AND not protected by Phase 2's keep set.
            // ---------------------------------------------------------------
            mir_pass_result out;
            out.function = fn;

            for (auto& block : out.function.function.blocks) {
                std::vector<allocated_instruction> kept;
                kept.reserve(block.instructions.size());
                for (const auto& inst : block.instructions) {
                    // Always preserve terminators and side-effecting instructions.
                    if (inst.op == opcode::ret
                        || inst.op == opcode::branch
                        || inst.op == opcode::branch_cond
                        || inst.op == opcode::call
                        || inst.op == opcode::store
                        || inst.op == opcode::store_spill
                        || inst.op == opcode::load_spill) {
                        kept.push_back(inst);
                        continue;
                    }
                    // Keep if it defines no preg at all (no def to eliminate).
                    bool has_preg_def = false;
                    for (const auto& def_op : inst.defs) {
                        if (def_op.type == allocated_operand::kind::preg) {
                            has_preg_def = true;
                            break;
                        }
                    }
                    if (!has_preg_def) {
                        kept.push_back(inst);
                        continue;
                    }
                    // Phase 2 guard: if dominance analysis flagged this instruction
                    // as having a live reaching use, preserve it.
                    if (phase2_keep.contains(inst.id)) {
                        kept.push_back(inst);
                        continue;
                    }
                    // Remove only if ALL preg defs are unused — keep if any def is live.
                    bool any_live = false;
                    for (const auto& def_op : inst.defs) {
                        if (def_op.type != allocated_operand::kind::preg) continue;
                        if (used_pregs.contains(std::get<preg>(def_op.value).id)) {
                            any_live = true;
                            break;
                        }
                    }
                    if (any_live) {
                        kept.push_back(inst);
                        continue;
                    }
                    ++out.removed_instructions;
                }
                block.instructions = std::move(kept);
            }

            if (out.removed_instructions == 0) {
                return out;
            }

            out.changed = true;
            ctx.changed = true;
            ctx.statistics.total_removed_instructions += out.removed_instructions;
            ctx.statistics.removed_instructions_by_pass["dead_def_elimination"] += out.removed_instructions;

            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "dead_def_elimination: verification failed after removing dead defs; "
                    "returning original physical MIR");
                out.diagnostics.insert(out.diagnostics.end(),
                                       verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.removed_instructions = 0;
                out.changed = false;
                ctx.changed = false;
                ctx.statistics.total_removed_instructions -= out.removed_instructions;
                ctx.statistics.removed_instructions_by_pass["dead_def_elimination"] -= out.removed_instructions;
                return out;
            }

            return out;
        }
    };

    struct copy_propagation_pass {
        [[nodiscard]] mir_pass_result run(mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            mir_pass_result out;
            out.function = fn;

            // ---------------------------------------------------------------
            // Phase 1: same-block copy propagation (original behaviour).
            // ---------------------------------------------------------------
            for (auto& block : out.function.function.blocks) {
                // active_copies: dst_preg_id -> src_preg_id.
                // preg::id is uint16_t, so both key and value match without narrowing.
                std::unordered_map<std::uint16_t, std::uint16_t> active_copies;
                // copy_inst_idx: instruction index of each active copy (for later removal).
                std::unordered_map<std::uint16_t, std::size_t> copy_inst_idx;
                // has_direct_use: dst pregs that were used directly (not via propagation)
                // since the copy was recorded.  A copy is removable only if absent here.
                std::unordered_set<std::uint16_t> has_direct_use;
                // removal_candidates: instruction indices confirmed safe to remove.
                // Populated when we commit state at barriers (call/terminator/block end).
                std::vector<std::size_t> removal_candidates;

                auto commit_removable = [&]() {
                    for (const auto& [dst_id, idx] : copy_inst_idx) {
                        if (!has_direct_use.contains(dst_id)) {
                            removal_candidates.push_back(idx);
                        }
                    }
                };

                auto invalidate = [&](const std::uint16_t def_id) {
                    // If this def kills a copy whose dst was never used directly,
                    // that copy is now dead and safe to remove.
                    if (copy_inst_idx.contains(def_id) && !has_direct_use.contains(def_id)) {
                        removal_candidates.push_back(copy_inst_idx[def_id]);
                    }
                    active_copies.erase(def_id);
                    copy_inst_idx.erase(def_id);
                    has_direct_use.erase(def_id);
                    // Also invalidate copies whose *source* is being redefined — the copy
                    // would now carry a stale value.  These copies are NOT removable (their
                    // dst may be live after the source redef), so just drop them.
                    std::vector<std::uint16_t> stale;
                    for (const auto& [dst, src] : active_copies) {
                        if (src == def_id) stale.push_back(dst);
                    }
                    for (const auto stale_dst : stale) {
                        active_copies.erase(stale_dst);
                        copy_inst_idx.erase(stale_dst);
                        has_direct_use.erase(stale_dst);
                    }
                };

                for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                    auto& inst = block.instructions[i];

                    // Stop propagating across calls and terminators.
                    if (inst.op == opcode::call
                        || inst.op == opcode::ret
                        || inst.op == opcode::branch
                        || inst.op == opcode::branch_cond) {
                        commit_removable();
                        active_copies.clear();
                        copy_inst_idx.clear();
                        has_direct_use.clear();
                        continue;
                    }

                    // Never touch spill instructions.
                    if (inst.op == opcode::load_spill || inst.op == opcode::store_spill) {
                        continue;
                    }

                    // Rewrite preg uses: replace active copy destinations with their source.
                    for (auto& use_op : inst.uses) {
                        if (use_op.type != allocated_operand::kind::preg) continue;
                        const auto use_id = std::get<preg>(use_op.value).id;
                        if (const auto it = active_copies.find(use_id); it != active_copies.end()) {
                            // Propagate: rewrite the use to the copy's source preg.
                            use_op.value = preg{it->second, std::get<preg>(use_op.value).name};
                        }
                        else if (copy_inst_idx.contains(use_id)) {
                            // dst is used without going through propagation — mark it live.
                            has_direct_use.insert(use_id);
                        }
                    }

                    // Detect new copy: mov with exactly one preg def and one preg use.
                    if (inst.op == opcode::mov
                        && inst.defs.size() == 1
                        && inst.uses.size() == 1
                        && inst.defs[0].type == allocated_operand::kind::preg
                        && inst.uses[0].type == allocated_operand::kind::preg) {
                        const auto dst_id = std::get<preg>(inst.defs[0].value).id;
                        const auto src_id = std::get<preg>(inst.uses[0].value).id;
                        if (dst_id != src_id) {
                            // Redefining dst invalidates any prior copy into dst and any
                            // copy sourced from dst.
                            invalidate(dst_id);
                            active_copies[dst_id] = src_id;
                            copy_inst_idx[dst_id] = i;
                        }
                        // Don't fall through to the generic def-invalidation below;
                        // invalidate() already handled dst.
                        continue;
                    }

                    // Generic def-invalidation for all other instructions.
                    for (const auto& def_op : inst.defs) {
                        if (def_op.type != allocated_operand::kind::preg) continue;
                        invalidate(std::get<preg>(def_op.value).id);
                    }
                }

                // Commit any remaining active copies that reached block end.
                commit_removable();

                // Remove confirmed-dead copy instructions in reverse index order.
                std::ranges::sort(removal_candidates, std::ranges::greater{});
                removal_candidates.erase(
                    std::unique(removal_candidates.begin(), removal_candidates.end()),
                    removal_candidates.end());
                for (const auto idx : removal_candidates) {
                    block.instructions.erase(block.instructions.begin() + static_cast<std::ptrdiff_t>(idx));
                    ++out.removed_instructions;
                }
            }

            // ---------------------------------------------------------------
            // Phase 2: cross-block copy propagation using dominance and
            // reaching definitions.  Falls back conservatively if either
            // analysis has diagnostics.
            //
            // Analyses run on the *original* fn so that copies removed by
            // Phase 1 are still visible in reaching definitions.  Rewrites
            // are applied to out.function.
            // ---------------------------------------------------------------
            bool cross_block_rewrote_uses = false;
            do {
                const auto dom_result = compute_dominators(fn, {});
                const auto reach_result = compute_reaching_definitions(fn);
                if (!dom_result.ok() || !reach_result.ok()) break;

                // Loop-awareness guard: compute loop info so we can avoid
                // propagating copies across loop back edges.
                const auto loop_result = analyze_loops(fn);
                // If loop analysis itself has diagnostics we fall back
                // conservatively by treating every block as potentially
                // loop-carried (done via loop_result.in_any_loop checks
                // returning vacuous true when the loop set is incomplete).

                // Build: copy_inst_id -> {def_block_id, dst_preg_id, src_preg}
                // Only consider mov r_dst = r_src in the (already Phase-1-rewritten) IR.
                struct cross_copy_info {
                    std::uint32_t def_block_id = 0;
                    std::uint32_t copy_inst_id = 0;
                    std::uint16_t dst_id = 0;
                    preg src_preg;
                };
                std::vector<cross_copy_info> copies;
                for (const auto& block : fn.function.blocks) {
                    for (const auto& inst : block.instructions) {
                        if (inst.op != opcode::mov
                            || inst.defs.size() != 1
                            || inst.uses.size() != 1
                            || inst.defs[0].type != allocated_operand::kind::preg
                            || inst.uses[0].type != allocated_operand::kind::preg)
                            continue;
                        const auto dst_id = std::get<preg>(inst.defs[0].value).id;
                        const auto& src = std::get<preg>(inst.uses[0].value);
                        if (dst_id == src.id) continue;
                        copies.push_back({block.id, inst.id, dst_id, src});
                    }
                }
                if (copies.empty()) break;

                // For each copy, try to propagate its dst into uses in other blocks.
                // propagated_copy_ids: copy instruction ids where all uses across all
                // blocks were covered by propagation (used to decide removal).
                std::unordered_set<std::uint32_t> cross_propagated_insts;

                // Pre-compute: which blocks contain a call instruction?
                // Used to refuse propagation when the path from copy to use crosses a call.
                std::unordered_set<std::uint32_t> blocks_with_call;
                for (const auto& b : fn.function.blocks) {
                    for (const auto& inst : b.instructions) {
                        if (inst.op == opcode::call) {
                            blocks_with_call.insert(b.id);
                            break;
                        }
                    }
                }

                // call_on_path(from, to): true if any block on a CFG path from `from`
                // to `to` (exclusive of `from`, inclusive of blocks strictly between)
                // contains a call.  Uses a simple BFS over CFG successors bounded by
                // the dominator property: we only need to visit blocks dominated by
                // `from` and that dominate or equal `to`.  Conservative: if CFG info
                // is unavailable for any block, returns true.
                auto call_on_path = [&](const std::uint32_t from_id, const std::uint32_t to_id) -> bool {
                    if (from_id == to_id) return false;
                    // BFS from `from_id` over CFG successors, stopping at `to_id`.
                    // We want to know if any intermediate block carries a call.
                    std::unordered_set<std::uint32_t> visited;
                    std::vector<std::uint32_t> worklist;
                    // Seed with direct successors of from_id.
                    for (const auto& b : fn.function.blocks) {
                        if (b.id != from_id) continue;
                        for (const auto succ : b.successors) worklist.push_back(succ);
                        break;
                    }
                    while (!worklist.empty()) {
                        const auto bid = worklist.back();
                        worklist.pop_back();
                        if (!visited.insert(bid).second) continue;
                        if (bid == to_id) continue; // reached the use block — don't look inside
                        if (blocks_with_call.contains(bid)) return true;
                        // Continue BFS through successors that are dominated by from_id
                        // (to stay on the from→to paths and avoid spiralling outside).
                        for (const auto& b : fn.function.blocks) {
                            if (b.id != bid) continue;
                            for (const auto succ : b.successors) {
                                if (!visited.contains(succ)
                                    && dominates(dom_result, from_id, succ)) {
                                    worklist.push_back(succ);
                                }
                            }
                            break;
                        }
                    }
                    return false;
                };

                bool any_rewrite = false;
                for (const auto& cp : copies) {
                    const std::uint32_t dst_key = cp.dst_id;
                    const std::uint32_t src_key = cp.src_preg.id;

                    // Reaching def of src at the copy instruction (may be absent if src
                    // is always live-in / never written in this function).
                    const auto copy_before_it = reach_result.before_instruction.find(cp.copy_inst_id);
                    const bool src_at_copy_exists = (copy_before_it != reach_result.before_instruction.end())
                        && copy_before_it->second.contains(src_key);
                    const definition_site src_at_copy = src_at_copy_exists
                                                            ? copy_before_it->second.at(src_key)
                                                            : definition_site{};

                    bool all_uses_propagated = true;

                    for (auto& use_block : out.function.function.blocks) {
                        if (use_block.id == cp.def_block_id) {
                            // Same block already handled by Phase 1.
                            // Check whether dst still appears as a use in this block; if so,
                            // it was a direct use that Phase 1 left alone → copy not fully
                            // propagated.
                            for (const auto& inst : use_block.instructions) {
                                if (inst.op == opcode::load_spill || inst.op == opcode::store_spill) continue;
                                for (const auto& use_op : inst.uses) {
                                    if (use_op.type == allocated_operand::kind::preg
                                        && std::get<preg>(use_op.value).id == cp.dst_id) {
                                        all_uses_propagated = false;
                                    }
                                }
                            }
                            continue;
                        }

                        // Only propagate into blocks dominated by the copy's block.
                        if (!dominates(dom_result, cp.def_block_id, use_block.id)) {
                            // If dst is used here we cannot propagate — copy not fully covered.
                            for (const auto& inst : use_block.instructions) {
                                for (const auto& use_op : inst.uses) {
                                    if (use_op.type == allocated_operand::kind::preg
                                        && std::get<preg>(use_op.value).id == cp.dst_id) {
                                        all_uses_propagated = false;
                                    }
                                }
                            }
                            continue;
                        }

                        // Loop guard: refuse to propagate from a loop body block into
                        // the loop header (that edge is a back edge — the header's
                        // live-in value at loop entry may differ from the back-edge
                        // iteration value).
                        if (loop_result.is_back_edge(cp.def_block_id, use_block.id)) {
                            all_uses_propagated = false;
                            continue;
                        }

                        // Call-crossing guard: refuse to propagate if any CFG path from
                        // the copy's block to the use block passes through a block that
                        // contains a call.  Calls may clobber caller-saved registers,
                        // making the copy's source register invalid at the use site.
                        if (call_on_path(cp.def_block_id, use_block.id)) {
                            // Mark all uses of dst in this block as not propagated.
                            for (const auto& inst : use_block.instructions) {
                                for (const auto& use_op : inst.uses) {
                                    if (use_op.type == allocated_operand::kind::preg
                                        && std::get<preg>(use_op.value).id == cp.dst_id) {
                                        all_uses_propagated = false;
                                    }
                                }
                            }
                            continue;
                        }

                        for (auto& inst : use_block.instructions) {
                            if (inst.op == opcode::load_spill || inst.op == opcode::store_spill) continue;

                            for (auto& use_op : inst.uses) {
                                if (use_op.type != allocated_operand::kind::preg) continue;
                                if (std::get<preg>(use_op.value).id != cp.dst_id) continue;

                                // Check that the only reaching def of dst at this use is the copy.
                                const auto use_before_it = reach_result.before_instruction.find(inst.id);
                                if (use_before_it == reach_result.before_instruction.end()) {
                                    all_uses_propagated = false;
                                    continue;
                                }
                                const auto dst_reach_it = use_before_it->second.find(dst_key);
                                if (dst_reach_it == use_before_it->second.end()
                                    || dst_reach_it->second.instruction_id != cp.copy_inst_id
                                    || dst_reach_it->second.block_id != cp.def_block_id) {
                                    all_uses_propagated = false;
                                    continue;
                                }

                                // Check that src has not been redefined between the copy and here.
                                const auto src_reach_it = use_before_it->second.find(src_key);
                                const bool src_at_use_exists = (src_reach_it != use_before_it->second.end());
                                if (src_at_copy_exists != src_at_use_exists) {
                                    // One side has a def and the other does not — path changed src.
                                    all_uses_propagated = false;
                                    continue;
                                }
                                if (src_at_copy_exists && src_at_use_exists
                                    && src_reach_it->second != src_at_copy) {
                                    // src was redefined along the path.
                                    all_uses_propagated = false;
                                    continue;
                                }

                                // All safety checks passed — rewrite the use.
                                use_op.value = preg{cp.src_preg.id, std::get<preg>(use_op.value).name};
                                any_rewrite = true;
                            }
                        }
                    }

                    if (all_uses_propagated) {
                        cross_propagated_insts.insert(cp.copy_inst_id);
                    }
                }

                if (!any_rewrite) break;

                cross_block_rewrote_uses = true;
                // Invalidate reaching-def cache — we rewrote uses, so it is stale.
                ctx.analysis_cache.invalidate_analysis(mir_analysis_kind::reaching_definitions);

                // Remove copies that are now dead (dst no longer used anywhere).
                for (auto& block : out.function.function.blocks) {
                    std::vector<std::size_t> to_remove;
                    for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                        const auto& inst = block.instructions[i];
                        if (!cross_propagated_insts.contains(inst.id)) continue;
                        // Confirm dst truly has no uses left in the whole function.
                        const auto dst_id = std::get<preg>(inst.defs[0].value).id;
                        bool dst_live = false;
                        for (const auto& chk_block : out.function.function.blocks) {
                            for (const auto& chk_inst : chk_block.instructions) {
                                for (const auto& use_op : chk_inst.uses) {
                                    if (use_op.type == allocated_operand::kind::preg
                                        && std::get<preg>(use_op.value).id == dst_id) {
                                        dst_live = true;
                                    }
                                }
                            }
                        }
                        if (!dst_live) to_remove.push_back(i);
                    }
                    std::ranges::sort(to_remove, std::ranges::greater{});
                    for (const auto idx : to_remove) {
                        block.instructions.erase(block.instructions.begin() + static_cast<std::ptrdiff_t>(idx));
                        ++out.removed_instructions;
                    }
                }
            }
            while (false);

            if (out.removed_instructions == 0 && !cross_block_rewrote_uses) {
                return out;
            }

            out.changed = true;
            ctx.changed = true;

            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "copy_propagation: verification failed after propagating copies; "
                    "returning original physical MIR");
                out.diagnostics.insert(out.diagnostics.end(),
                                       verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.removed_instructions = 0;
                out.changed = false;
                ctx.changed = false;
                return out;
            }

            return out;
        }
    };

    struct constant_propagation_pass {
        [[nodiscard]] mir_pass_result run(mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            mir_pass_result out;
            out.function = fn;

            // ---------------------------------------------------------------
            // Phase 1: same-block constant propagation (original behaviour).
            // ---------------------------------------------------------------
            for (auto& block : out.function.function.blocks) {
                // known_consts: preg_id -> integer constant value, reset per block.
                std::unordered_map<std::uint32_t, std::int64_t> known_consts;

                for (auto& inst : block.instructions) {
                    // load_imm with a single preg def and a single i64 use seeds the map.
                    if (inst.op == opcode::load_imm
                        && inst.defs.size() == 1
                        && inst.uses.size() == 1
                        && inst.defs[0].type == allocated_operand::kind::preg
                        && inst.uses[0].type == allocated_operand::kind::immediate_i64) {
                        const auto dst_id = std::get<preg>(inst.defs[0].value).id;
                        known_consts[dst_id] = std::get<std::int64_t>(inst.uses[0].value);
                        continue;
                    }

                    // mov from a known-constant preg propagates the constant to the dst.
                    if (inst.op == opcode::mov
                        && inst.defs.size() == 1
                        && inst.uses.size() == 1
                        && inst.defs[0].type == allocated_operand::kind::preg
                        && inst.uses[0].type == allocated_operand::kind::preg) {
                        const auto dst_id = std::get<preg>(inst.defs[0].value).id;
                        const auto src_id = std::get<preg>(inst.uses[0].value).id;
                        if (const auto it = known_consts.find(src_id); it != known_consts.end()) {
                            known_consts[dst_id] = it->second;
                        }
                        else {
                            known_consts.erase(dst_id);
                        }
                        continue;
                    }

                    // Fold add/sub/mul/div when both use operands are known integer constants.
                    if ((inst.op == opcode::add || inst.op == opcode::sub
                            || inst.op == opcode::mul || inst.op == opcode::div)
                        && inst.defs.size() == 1
                        && inst.uses.size() == 2
                        && inst.defs[0].type == allocated_operand::kind::preg
                        && inst.uses[0].type == allocated_operand::kind::preg
                        && inst.uses[1].type == allocated_operand::kind::preg) {
                        const auto lhs_id = std::get<preg>(inst.uses[0].value).id;
                        const auto rhs_id = std::get<preg>(inst.uses[1].value).id;
                        const auto lhs_it = known_consts.find(lhs_id);
                        const auto rhs_it = known_consts.find(rhs_id);
                        if (lhs_it != known_consts.end() && rhs_it != known_consts.end()) {
                            const std::int64_t lv = lhs_it->second;
                            const std::int64_t rv = rhs_it->second;
                            // Conservatively refuse division by zero.
                            if (inst.op == opcode::div && rv == 0) {
                                // Invalidate dst and move on.
                                known_consts.erase(std::get<preg>(inst.defs[0].value).id);
                                continue;
                            }
                            std::int64_t result = 0;
                            switch (inst.op) {
                            case opcode::add: result = lv + rv;
                                break;
                            case opcode::sub: result = lv - rv;
                                break;
                            case opcode::mul: result = lv * rv;
                                break;
                            case opcode::div: result = lv / rv;
                                break;
                            default: break;
                            }
                            const auto dst_preg = inst.defs[0];
                            const auto dst_id = std::get<preg>(dst_preg.value).id;
                            // Rewrite instruction to load_imm result into dst.
                            inst.op = opcode::load_imm;
                            inst.uses = {allocated_operand::as_i64(result)};
                            // defs unchanged — same preg, same id.
                            known_consts[dst_id] = result;
                            ++out.removed_instructions; // one fewer arithmetic inst
                            continue;
                        }
                    }

                    // Unknown instruction: invalidate any preg it defines.
                    for (const auto& def_op : inst.defs) {
                        if (def_op.type == allocated_operand::kind::preg) {
                            known_consts.erase(std::get<preg>(def_op.value).id);
                        }
                    }
                }
            }

            // ---------------------------------------------------------------
            // Phase 2: cross-block constant folding using dominance and
            // reaching definitions.  Falls back conservatively if either
            // analysis has diagnostics.
            // ---------------------------------------------------------------
            do {
                const auto& dom_result = get_or_compute_dominators(ctx, out.function, {});
                const auto& reach_result = get_or_compute_reaching_definitions(ctx, out.function);
                if (!dom_result.ok() || !reach_result.ok()) break;

                // Loop-awareness guard: compute loop info so we can avoid
                // folding constants across loop back edges.
                const auto loop_result = analyze_loops(out.function);

                // Build ambiguity guard: "preg_id@block_id" strings from reaching-def
                // diagnostics.  A preg that has an ambiguous reaching def in a block
                // must not be folded in that block.
                std::unordered_set<std::string> ambiguous_keys;
                for (const auto& diag : reach_result.diagnostics) {
                    // Diagnostic format: "ambiguous reaching def for preg N at block bbM"
                    const std::string prefix = "ambiguous reaching def for preg ";
                    if (diag.rfind(prefix, 0) == 0) {
                        const auto rest = diag.substr(prefix.size());
                        const auto at_pos = rest.find(" at block bb");
                        if (at_pos != std::string::npos) {
                            const auto preg_str = rest.substr(0, at_pos);
                            const auto block_str = rest.substr(at_pos + 12); // skip " at block bb"
                            ambiguous_keys.insert(preg_str + "@" + block_str);
                        }
                    }
                }

                // Build: load_imm instruction_id -> constant value.
                // This covers both the original load_imms and any that Phase 1 produced.
                std::unordered_map<std::uint32_t, std::int64_t> const_by_inst;
                for (const auto& block : out.function.function.blocks) {
                    for (const auto& inst : block.instructions) {
                        if (inst.op == opcode::load_imm
                            && inst.defs.size() == 1
                            && inst.uses.size() == 1
                            && inst.defs[0].type == allocated_operand::kind::preg
                            && inst.uses[0].type == allocated_operand::kind::immediate_i64) {
                            const_by_inst[inst.id] = std::get<std::int64_t>(inst.uses[0].value);
                        }
                    }
                }
                if (const_by_inst.empty()) break;

                // Helper: resolve a preg operand to a constant if safe.
                auto resolve_const = [&](const std::uint32_t use_inst_id,
                                         const std::uint16_t preg_id,
                                         const std::uint32_t use_block_id,
                                         std::int64_t& out_val) -> bool {
                    const auto key = std::to_string(preg_id) + "@" + std::to_string(use_block_id);
                    if (ambiguous_keys.contains(key)) return false;

                    const auto before_it = reach_result.before_instruction.find(use_inst_id);
                    if (before_it == reach_result.before_instruction.end()) return false;
                    const auto def_it = before_it->second.find(preg_id);
                    if (def_it == before_it->second.end()) return false;

                    const auto& ds = def_it->second;
                    // The defining block must dominate the use block.
                    if (!dominates(dom_result, ds.block_id, use_block_id)) return false;

                    // Loop guard: refuse to fold if the edge from ds.block_id to
                    // use_block_id is a loop back edge — the constant may be
                    // loop-carried and have a different value on subsequent iterations.
                    if (loop_result.is_back_edge(ds.block_id, use_block_id)) return false;

                    // Loop guard: refuse to fold a constant defined inside a loop
                    // body into the loop header — the header is the iteration merge
                    // point and the value may differ on the back-edge path.
                    if (loop_result.is_loop_header(use_block_id)
                        && loop_result.in_any_loop(ds.block_id))
                        return false;

                    const auto cv_it = const_by_inst.find(ds.instruction_id);
                    if (cv_it == const_by_inst.end()) return false;

                    out_val = cv_it->second;
                    return true;
                };

                bool any_fold = false;
                for (auto& use_block : out.function.function.blocks) {
                    for (auto& inst : use_block.instructions) {
                        if ((inst.op != opcode::add && inst.op != opcode::sub
                            && inst.op != opcode::mul && inst.op != opcode::div))
                            continue;
                        if (inst.defs.size() != 1 || inst.uses.size() != 2) continue;
                        if (inst.defs[0].type != allocated_operand::kind::preg) continue;
                        if (inst.uses[0].type != allocated_operand::kind::preg) continue;
                        if (inst.uses[1].type != allocated_operand::kind::preg) continue;

                        const auto lhs_preg_id = std::get<preg>(inst.uses[0].value).id;
                        const auto rhs_preg_id = std::get<preg>(inst.uses[1].value).id;
                        std::int64_t lv = 0, rv = 0;
                        if (!resolve_const(inst.id, lhs_preg_id, use_block.id, lv)) continue;
                        if (!resolve_const(inst.id, rhs_preg_id, use_block.id, rv)) continue;

                        if (inst.op == opcode::div && rv == 0) {
                            // Refuse division by zero — invalidate dst constant knowledge.
                            continue;
                        }

                        std::int64_t result = 0;
                        switch (inst.op) {
                        case opcode::add: result = lv + rv;
                            break;
                        case opcode::sub: result = lv - rv;
                            break;
                        case opcode::mul: result = lv * rv;
                            break;
                        case opcode::div: result = lv / rv;
                            break;
                        default: break;
                        }

                        inst.op = opcode::load_imm;
                        inst.uses = {allocated_operand::as_i64(result)};
                        // Register the new constant so subsequent instructions in the same
                        // block can fold against it.
                        const_by_inst[inst.id] = result;
                        ++out.removed_instructions;
                        any_fold = true;
                    }
                }

                if (!any_fold) break;

                // Reaching-def cache is now stale.
                ctx.analysis_cache.invalidate_analysis(mir_analysis_kind::reaching_definitions);
            }
            while (false);

            if (out.removed_instructions == 0) {
                return out;
            }

            out.changed = true;
            ctx.changed = true;

            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "constant_propagation: verification failed after folding constants; "
                    "returning original physical MIR");
                out.diagnostics.insert(out.diagnostics.end(),
                                       verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.removed_instructions = 0;
                out.changed = false;
                ctx.changed = false;
                return out;
            }

            return out;
        }
    };

    // -----------------------------------------------------------------------
    // Loop-invariant code motion
    //
    // Physical MIR does not create preheaders on its own.  This pass therefore
    // only uses an already unambiguous, single external predecessor and hoists
    // load_imm instructions: they have no operands, memory effects, traps, or
    // target-dependent floating-point state.  Broader arithmetic and memory
    // motion remains the responsibility of a future effect-aware LICM pass.
    // -----------------------------------------------------------------------

    struct loop_invariant_code_motion_pass {
        [[nodiscard]] mir_pass_result run(
            mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            mir_pass_result out;
            out.function = fn;

            const auto loops = analyze_loops(fn);
            if (!loops.ok() || loops.loops.empty()) return out;
            const auto cfg = analyze_cfg(fn);
            if (!cfg.ok()) return out;

            std::unordered_map<std::uint16_t, std::size_t> preg_def_count;
            for (const auto& block : out.function.function.blocks) {
                for (const auto& inst : block.instructions) {
                    for (const auto& def : inst.defs) {
                        if (def.type == allocated_operand::kind::preg)
                            ++preg_def_count[std::get<preg>(def.value).id];
                    }
                }
            }

            // Process inner loops first. A candidate can then be hoisted only
            // once, never by both an inner and enclosing loop.
            auto ordered_loops = loops.loops;
            std::ranges::sort(ordered_loops, {}, [](const loop_info& loop) {
                return loop.body.size();
            });

            std::size_t hoisted = 0;
            for (const auto& loop : ordered_loops) {
                auto header_it = std::ranges::find_if(out.function.function.blocks,
                    [&](const allocated_basic_block& block) { return block.id == loop.header; });
                if (header_it == out.function.function.blocks.end()) continue;

                std::vector<std::uint32_t> external_predecessors;
                for (const auto& [block_id, info] : cfg.block_info) {
                    if (loop.body.contains(block_id)) continue;
                    if (std::ranges::find(info.successors, loop.header) != info.successors.end()) {
                        external_predecessors.push_back(block_id);
                    }
                }
                if (external_predecessors.size() != 1) continue;

                auto preheader_it = std::ranges::find_if(out.function.function.blocks,
                    [&](const allocated_basic_block& block) {
                        return block.id == external_predecessors.front();
                    });
                if (preheader_it == out.function.function.blocks.end()) continue;

                std::vector<allocated_instruction> moved;
                for (auto& block : out.function.function.blocks) {
                    if (!loop.body.contains(block.id) || block.id == loop.header) continue;

                    std::vector<allocated_instruction> retained;
                    retained.reserve(block.instructions.size());
                    for (auto& inst : block.instructions) {
                        const bool is_unique_load_imm = inst.op == opcode::load_imm
                            && inst.defs.size() == 1
                            && inst.defs.front().type == allocated_operand::kind::preg
                            && inst.uses.size() == 1
                            && inst.uses.front().type == allocated_operand::kind::immediate_i64
                            && preg_def_count[std::get<preg>(inst.defs.front().value).id] == 1;
                        if (is_unique_load_imm) {
                            moved.push_back(std::move(inst));
                        }
                        else {
                            retained.push_back(std::move(inst));
                        }
                    }
                    block.instructions = std::move(retained);
                }

                if (moved.empty()) continue;

                const auto terminator = std::ranges::find_if(preheader_it->instructions,
                    [](const allocated_instruction& inst) {
                        return inst.op == opcode::branch || inst.op == opcode::branch_cond
                            || inst.op == opcode::ret;
                    });
                preheader_it->instructions.insert(terminator,
                                                   std::make_move_iterator(moved.begin()),
                                                   std::make_move_iterator(moved.end()));
                hoisted += moved.size();
            }

            if (hoisted == 0) return out;

            out.changed = true;
            ctx.changed = true;

            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "loop_invariant_code_motion: verification failed after hoisting; returning original physical MIR");
                out.diagnostics.insert(out.diagnostics.end(), verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.changed = false;
                ctx.changed = false;
            }
            return out;
        }
    };

    // -----------------------------------------------------------------------
    // Common Subexpression Elimination pass
    //
    // Replaces a pure expression with a mov from the register that holds the
    // dominating identical computation.  Conservative: any ambiguity causes
    // the expression to be left unchanged.
    // -----------------------------------------------------------------------

    struct cse_result {
        mir::physical_mir_function function;
        std::size_t replaced_expressions = 0; // number of instructions turned into mov
        std::vector<std::string> diagnostics;
        bool changed = false;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    struct cse_options {
        bool verify_after_rewrite = true;
    };

    struct common_subexpression_elimination_pass {
        [[nodiscard]] mir_pass_result run(
            mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            mir_pass_result out;
            out.function = fn;

            // We need dominator information and reaching definitions.
            // Fall back conservatively if either analysis fails.
            const auto& dom_result = get_or_compute_dominators(ctx, out.function, {});
            const auto& reach_result = get_or_compute_reaching_definitions(ctx, out.function);

            if (!dom_result.ok() || !reach_result.ok()) {
                // Conservative fallback: do nothing.
                return out;
            }

            // Loop analysis: used to prevent unsafe CSE across back edges.
            const auto loop_result = analyze_loops(fn);

            // Build ambiguity guard from reaching-def diagnostics.
            // Key format: "preg_id@block_id"
            std::unordered_set<std::string> ambiguous_keys;
            for (const auto& diag : reach_result.diagnostics) {
                const std::string prefix = "ambiguous reaching def for preg ";
                if (diag.rfind(prefix, 0) == 0) {
                    const auto rest = diag.substr(prefix.size());
                    const auto at_pos = rest.find(" at block bb");
                    if (at_pos != std::string::npos) {
                        ambiguous_keys.insert(
                            rest.substr(0, at_pos) + "@" + rest.substr(at_pos + 12));
                    }
                }
            }

            // Map from mir_expression_key → candidate definition descriptor.
            // Built incrementally in topological order so dominating defs are
            // always seen before uses.
            struct AvailEntry {
                std::uint32_t block_id = 0;
                std::uint32_t inst_id = 0; // instruction id of the defining instruction
                std::size_t inst_pos = 0; // position within block (for same-block check)
                std::uint16_t preg_id = 0;
            };
            std::unordered_map<mir_expression_key, AvailEntry> available;

            // Process blocks in topological order so that a dominating block is always
            // processed before any block it dominates.
            const auto& cfg_result = get_or_compute_cfg(ctx, out.function);
            if (!cfg_result.ok()) return out;
            const auto topo = topological_block_order(cfg_result);

            bool any_change = false;

            for (const auto block_id : topo) {
                auto* block_ptr = [&]() -> allocated_basic_block* {
                    for (auto& b : out.function.function.blocks) {
                        if (b.id == block_id) return &b;
                    }
                    return nullptr;
                }();
                if (!block_ptr) continue;

                for (std::size_t inst_idx = 0; inst_idx < block_ptr->instructions.size(); ++inst_idx) {
                    auto& inst = block_ptr->instructions[inst_idx];

                    // Invalidate available expressions whose operands reference any
                    // preg defined by this instruction.  This must happen for ALL
                    // instructions — including CSE candidates — before the candidate
                    // check below, so that a redef in the middle of a block correctly
                    // removes stale entries.
                    for (const auto& def_op : inst.defs) {
                        if (def_op.type != allocated_operand::kind::preg) continue;
                        const auto def_id = std::get<preg>(def_op.value).id;
                        std::vector<mir_expression_key> stale;
                        for (const auto& [key, entry] : available) {
                            for (const auto& ok : key.operands) {
                                if (ok.type == allocated_operand::kind::preg
                                    && ok.bits == def_id) {
                                    stale.push_back(key);
                                    break;
                                }
                            }
                        }
                        for (const auto& k : stale) available.erase(k);
                    }

                    // Do not allow CSE to cross calls — any call may clobber
                    // caller-saved registers, making the candidate result unavailable.
                    // Clear the entire available map so no pre-call expression can be
                    // reused after the call.
                    if (inst.op == opcode::call) {
                        available.clear();
                        continue;
                    }

                    // Only proceed with CSE for pure candidates.
                    if (!is_cse_candidate(inst)) continue;

                    const auto key_opt = make_expression_key(inst);
                    if (!key_opt) continue;
                    const auto& key = *key_opt;

                    const auto dst_preg_id = std::get<preg>(inst.defs[0].value).id;

                    // Check operand safety: for each preg use, verify that the
                    // reaching definition is unambiguous at this instruction.
                    // A live-in preg (no reaching def entry) is safe — it has a
                    // single definition outside the function scope and is never
                    // redefined (the available-map invalidation above would have
                    // removed any key that uses a redefined preg operand).
                    bool operands_safe = true;
                    for (const auto& use_op : inst.uses) {
                        if (use_op.type != allocated_operand::kind::preg) continue;
                        const auto uid = std::get<preg>(use_op.value).id;
                        const auto ambi_key =
                            std::to_string(uid) + "@" + std::to_string(block_id);
                        if (ambiguous_keys.contains(ambi_key)) {
                            operands_safe = false;
                            break;
                        }
                        // For cross-block uses, verify the reaching def (if any)
                        // dominates the current block.
                        const auto before_it = reach_result.before_instruction.find(inst.id);
                        if (before_it != reach_result.before_instruction.end()) {
                            const auto def_it = before_it->second.find(
                                uid);
                            if (def_it != before_it->second.end()) {
                                if (!dominates(dom_result, def_it->second.block_id, block_id)) {
                                    operands_safe = false;
                                    break;
                                }
                            }
                            // No reaching def entry: live-in preg — treat as safe.
                        }
                        // before_instruction has no entry for this instruction:
                        // this can happen for live-in pregs in simple single-block
                        // functions.  Treat as safe — the invalidation pass above
                        // ensures no stale available entry survives a redef.
                    }
                    if (!operands_safe) {
                        // Record as available for later blocks even if not safe to
                        // replace here, but only if no prior entry exists.
                        available.try_emplace(key, AvailEntry{block_id, inst.id, inst_idx, dst_preg_id});
                        continue;
                    }

                    const auto it = available.find(key);
                    if (it == available.end()) {
                        // First time we see this expression.  Don't record it as
                        // available if the current block is a loop header — an
                        // expression computed at the header is re-evaluated on each
                        // loop iteration and is not invariant across iterations.
                        if (!loop_result.is_loop_header(block_id)) {
                            available.emplace(key, AvailEntry{block_id, inst.id, inst_idx, dst_preg_id});
                        }
                        continue;
                    }

                    const auto& [def_block_id, def_inst_id, def_inst_pos, def_preg_id] = it->second;

                    // Dominance check: the candidate must dominate the current use site.
                    // For same-block reuse, the candidate must appear strictly before
                    // the current instruction (inst_idx > def_inst_pos, guaranteed by
                    // forward iteration — but checked explicitly for safety).
                    if (def_block_id == block_id) {
                        if (def_inst_pos >= inst_idx) continue; // candidate is not before use
                    }
                    else {
                        if (!dominates(dom_result, def_block_id, block_id)) continue;
                    }

                    // Loop-awareness: do not reuse an expression if the path from
                    // the definition to the current block crosses a back edge
                    // (loop-carried value may differ on each iteration).
                    if (loop_result.is_back_edge(def_block_id, block_id)) continue;
                    // Also refuse if the current block is a loop header and the def
                    // block is inside the same loop — the header's expressions are
                    // re-evaluated each iteration, so the first-iteration value
                    // recorded in `available` is not generally valid on later iterations.
                    if (loop_result.is_loop_header(block_id)
                        && loop_result.in_any_loop(def_block_id))
                        continue;

                    // Verify that the candidate result is still available at this use
                    // site via reaching definitions: the sole reaching definition of
                    // def_preg_id here must be the candidate instruction itself.
                    // This check catches cases where def_preg_id was redefined on some
                    // path not reflected in the available-map invalidation above.
                    // Skip for same-block reuse — the forward invalidation sweep already
                    // guarantees the def is the most recent one in this block.
                    if (def_block_id != block_id) {
                        const auto before_it = reach_result.before_instruction.find(inst.id);
                        if (before_it != reach_result.before_instruction.end()) {
                            const auto rd_it = before_it->second.find(
                                def_preg_id);
                            if (rd_it == before_it->second.end()
                                || rd_it->second.instruction_id != def_inst_id
                                || rd_it->second.block_id != def_block_id) {
                                // Reaching def is absent or points to a different instruction:
                                // the candidate result is not guaranteed available here.
                                continue;
                            }
                        }
                        // No before_instruction entry for inst.id: fall through conservatively
                        // only if the block-level ambiguity guard is clear.
                        else {
                            const auto ambi_key =
                                std::to_string(def_preg_id) + "@" + std::to_string(block_id);
                            if (ambiguous_keys.contains(ambi_key)) continue;
                        }
                    }

                    // Rewrite the expression to a mov from the dominating result.
                    const preg src_preg{def_preg_id, {}};
                    inst.op = opcode::mov;
                    inst.uses = {allocated_operand::as_preg(src_preg)};
                    // defs remain the same (same dst preg).
                    ++out.removed_instructions; // conceptually one fewer unique computation
                    out.changed = true;
                    any_change = true;
                }
            }

            if (any_change && ctx.verify_after_each_pass) {
                const auto verification = verify_physical_mir(out.function);
                if (!verification.ok()) {
                    // Revert to the original on verification failure.
                    out.function = fn;
                    out.removed_instructions = 0;
                    out.changed = false;
                    out.diagnostics.insert(
                        out.diagnostics.end(),
                        verification.diagnostics.begin(),
                        verification.diagnostics.end());
                }
            }

            return out;
        }
    };

    [[nodiscard]] inline peephole_result run_mir_peephole(
        const mir::physical_mir_function& fn,
        const peephole_options& options
    ) {
        peephole_result out;
        out.function = fn;

        auto spill_location_key = [](const allocated_operand& op) -> std::optional<std::string> {
            if (op.type == allocated_operand::kind::spill) {
                return std::string{"spill:"} + std::to_string(std::get<spill_slot>(op.value).id);
            }
            if (op.type != allocated_operand::kind::memory) {
                return std::nullopt;
            }
            const auto& mem = std::get<memory_operand>(op.value);
            if (mem.address.kind != memory_address_kind::spill_slot) {
                return std::nullopt;
            }
            return std::string{"spill_mem:"} + dump_memory_operand(mem);
        };

        for (auto& block : out.function.function.blocks) {
            std::vector<allocated_instruction> rewritten;
            rewritten.reserve(block.instructions.size());

            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                const auto& inst = block.instructions[i];

                if (options.remove_nop && inst.op == opcode::nop) {
                    ++out.removed_instructions;
                    continue;
                }

                if (options.remove_redundant_mov && inst.op == opcode::mov
                    && inst.defs.size() == 1
                    && inst.uses.size() == 1
                    && inst.defs[0].type == allocated_operand::kind::preg
                    && inst.uses[0].type == allocated_operand::kind::preg) {
                    const auto& dst = std::get<preg>(inst.defs[0].value);
                    const auto& src = std::get<preg>(inst.uses[0].value);
                    if (dst.id == src.id) {
                        ++out.removed_instructions;
                        continue;
                    }
                }

                if (options.fold_redundant_spill_roundtrip && i + 1 < block.instructions.size()) {
                    const auto& next = block.instructions[i + 1];
                    if (inst.op == opcode::load_spill && next.op == opcode::store_spill
                        && inst.defs.size() == 1
                        && inst.uses.size() == 1
                        && next.defs.size() == 1
                        && next.uses.size() == 1
                        && inst.defs[0].type == allocated_operand::kind::preg
                        && next.uses[0].type == allocated_operand::kind::preg) {
                        const auto& loaded_reg = std::get<preg>(inst.defs[0].value);
                        const auto& stored_reg = std::get<preg>(next.uses[0].value);
                        const auto load_key = spill_location_key(inst.uses[0]);
                        const auto store_key = spill_location_key(next.defs[0]);
                        if (loaded_reg.id == stored_reg.id
                            && load_key.has_value()
                            && store_key.has_value()
                            && *load_key == *store_key) {
                            out.removed_instructions += 2;
                            ++i;
                            continue;
                        }
                    }
                }

                rewritten.push_back(inst);
            }

            block.instructions = std::move(rewritten);
        }

        if (options.remove_unreachable_empty_blocks) {
            std::unordered_set<std::uint32_t> branch_target_ids;
            std::unordered_map<std::uint32_t, std::size_t> predecessor_reference_counts;

            for (const auto& block : out.function.function.blocks) {
                predecessor_reference_counts[block.id] += block.predecessors.size();
                for (const auto& inst : block.instructions) {
                    if (inst.op != opcode::branch && inst.op != opcode::branch_cond) {
                        continue;
                    }
                    for (const auto& use : inst.uses) {
                        if (use.type != allocated_operand::kind::block) {
                            continue;
                        }
                        const auto target = std::get<std::uint32_t>(use.value);
                        branch_target_ids.insert(target);
                    }
                }
            }

            for (const auto& [block_id, preds] : out.function.function.cfg.predecessors) {
                predecessor_reference_counts[block_id] += preds.size();
            }

            std::vector<allocated_basic_block> filtered;
            filtered.reserve(out.function.function.blocks.size());
            for (const auto& block : out.function.function.blocks) {
                const auto predecessor_refs_it = predecessor_reference_counts.find(block.id);
                const auto predecessor_refs = predecessor_refs_it == predecessor_reference_counts.end()
                                                  ? 0
                                                  : predecessor_refs_it->second;

                const bool removable = block.id != out.function.function.cfg.entry_block
                    && block.instructions.empty()
                    && predecessor_refs == 0
                    && !branch_target_ids.contains(block.id);
                if (removable) {
                    ++out.removed_blocks;
                    continue;
                }
                filtered.push_back(block);
            }
            out.function.function.blocks = std::move(filtered);
        }

        {
            std::unordered_set<std::uint32_t> block_ids;
            for (const auto& block : out.function.function.blocks) {
                block_ids.insert(block.id);
            }

            out.function.function.cfg.successors.clear();
            out.function.function.cfg.predecessors.clear();
            for (auto& block : out.function.function.blocks) {
                block.successors.erase(
                    std::remove_if(block.successors.begin(), block.successors.end(), [&](const std::uint32_t target) {
                        return !block_ids.contains(target);
                    }),
                    block.successors.end()
                );
                std::ranges::sort(block.successors);
                block.successors.erase(std::unique(block.successors.begin(), block.successors.end()),
                                       block.successors.end());
                block.predecessors.clear();
                out.function.function.cfg.successors[block.id] = block.successors;
            }

            for (const auto& block : out.function.function.blocks) {
                for (const auto succ : block.successors) {
                    out.function.function.cfg.predecessors[succ].push_back(block.id);
                }
            }

            for (auto& block : out.function.function.blocks) {
                block.predecessors = out.function.function.cfg.predecessors[block.id];
                std::ranges::sort(block.predecessors);
                block.predecessors.erase(std::unique(block.predecessors.begin(), block.predecessors.end()),
                                         block.predecessors.end());
                auto& preds = out.function.function.cfg.predecessors[block.id];
                preds = block.predecessors;
            }
        }

        out.function.instruction_ids.clear();
        out.function.referenced_spill_slots.clear();
        out.function.referenced_virtual_registers.clear();
        for (const auto& block : out.function.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.id != 0) {
                    out.function.instruction_ids.insert(inst.id);
                }
                for (const auto& def : inst.defs) {
                    if (def.type == allocated_operand::kind::spill) {
                        out.function.referenced_spill_slots.insert(std::get<spill_slot>(def.value).id);
                    }
                }
                for (const auto& use : inst.uses) {
                    if (use.type == allocated_operand::kind::spill) {
                        out.function.referenced_spill_slots.insert(std::get<spill_slot>(use.value).id);
                    }
                }
            }
        }

        out.changed = out.removed_instructions != 0 || out.removed_blocks != 0;
        if (out.changed) {
            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "peephole: verification failed after rewrites; returning original physical MIR");
                out.diagnostics.insert(out.diagnostics.end(), verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.removed_instructions = 0;
                out.removed_blocks = 0;
                out.changed = false;
                return out;
            }
        }

        return out;
    }

    // -----------------------------------------------------------------------
    // Group — compilation artifact model
    //
    // artifact_kind classifies the type of output produced by a backend pass.
    // compilation_artifact is the generic carrier for that output; it replaces
    // the ad-hoc artifact_text field that backend_result previously used.
    // backend_result is kept intact; it may contain or convert to a
    // compilation_artifact when callers are ready to migrate.
    // -----------------------------------------------------------------------

    enum class artifact_kind : std::uint8_t {
        none,
        debug_text,
        interpreter_result,
        assembly_text,
        object_file,
        aot_object,
        jit_function,
        graph_plan,
        tensor_plan,
        layout_plan,
        query_plan,
        instrumentation_report,
        optimization_report,
        device_kernel,
    };

    enum class artifact_handle_kind : std::uint8_t {
        none,
        jit_function,
        native_compute_pipeline,
    };

    // Type-erased, ref-counted carrier for backend-specific runtime objects
    // (e.g. a JIT function handle).  Copies of compilation_artifact share the
    // same underlying object via shared_ptr reference counting.
    struct artifact_handle {
        artifact_handle_kind kind = artifact_handle_kind::none;
        std::shared_ptr<void> payload;

        [[nodiscard]] bool valid() const noexcept {
            return kind != artifact_handle_kind::none && payload != nullptr;
        }

        template <typename T>
        [[nodiscard]] T* get() const noexcept {
            return static_cast<T*>(payload.get());
        }
    };

    struct compilation_artifact {
        artifact_kind kind = artifact_kind::none;
        std::string name;
        std::string text_payload;
        std::vector<std::byte> binary_payload;
        std::vector<std::string> diagnostics;
        std::unordered_map<std::string, std::string> metadata;
        std::shared_ptr<artifact_handle> handle;

        [[nodiscard]] bool ok() const { return kind != artifact_kind::none; }
    };

    struct backend_error {
        std::string message;
        std::optional<std::uint32_t> block_id;
        std::optional<std::uint32_t> instruction_id;
    };

    struct backend_state {
        std::string backend_name;
        std::size_t emitted_instructions = 0;
        std::size_t emitted_blocks = 0;
    };

    struct backend_result {
        bool success = true;
        backend_state state;
        std::vector<backend_error> errors;
        std::optional<std::string> artifact_text;

        [[nodiscard]] bool ok() const { return success && errors.empty(); }

        [[nodiscard]] static backend_result success_result(backend_state state = {}) {
            backend_result out;
            out.success = true;
            out.state = std::move(state);
            return out;
        }

        [[nodiscard]] static backend_result fail(std::string message, backend_state state = {}) {
            backend_result out;
            out.success = false;
            out.state = std::move(state);
            out.errors.push_back(backend_error{std::move(message), std::nullopt, std::nullopt});
            return out;
        }
    };

    // -----------------------------------------------------------------------
    // Group E — structured backend requirement model
    //
    // backend_requirement_kind describes the category of a single requirement.
    // backend_requirement pairs a kind with a human-readable message.
    // backend_requirement_set is a flat collection of requirements inferred
    // from the MIR or declared by a backend.
    // backend_legalization_result carries the outcome of a legality check
    // performed against a backend_requirement_set.
    //
    // These types are orthogonal to the capability-bit API below.
    // -----------------------------------------------------------------------

    enum class backend_requirement_kind : std::uint8_t {
        no_virtual_registers,
        no_unresolved_spills,
        physical_mir_verified,
        supports_integer_arithmetic,
        supports_floating_arithmetic,
        supports_memory_operands,
        supports_stack_frame,
        supports_branches,
        supports_calls,
        supports_phi_or_ssa,
        supports_loops,
        supports_load_store,
        supports_side_effects
    };

    struct backend_requirement {
        backend_requirement_kind kind;
        std::string message;
    };

    struct backend_requirement_set {
        std::vector<backend_requirement> requirements;

        void add(const backend_requirement_kind kind, std::string message = {}) {
            requirements.push_back({kind, std::move(message)});
        }

        [[nodiscard]] bool has(const backend_requirement_kind kind) const noexcept {
            for (const auto& r : requirements)
                if (r.kind == kind) return true;
            return false;
        }
    };

    struct backend_legalization_result {
        bool legal = true;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return legal && diagnostics.empty(); }
    };

    enum class backend_feature : std::uint8_t {
        integer_arithmetic,
        floating_arithmetic,
        spill_load_store,
        branches,
        calls,
        memory_operands,
        stack_frame,
        interpreter_execution,
        tensor_arithmetic, // backend can physicalize tensor/vector operations
        symbolic_arithmetic // backend can physicalize symbolic/quantum operations
    };

    struct backend_capability_set {
        std::uint64_t bits = 0;

        constexpr backend_capability_set() = default;

        constexpr explicit backend_capability_set(const std::uint64_t raw_bits) : bits(raw_bits) {}

        [[nodiscard]] static constexpr backend_capability_set from(
            const std::initializer_list<backend_feature> features) {
            backend_capability_set out;
            for (const auto feature : features) {
                out.add(feature);
            }
            return out;
        }

        constexpr void add(backend_feature feature) {
            bits |= (std::uint64_t{1} << static_cast<std::uint8_t>(feature));
        }

        [[nodiscard]] constexpr bool has(backend_feature feature) const {
            return (bits & (std::uint64_t{1} << static_cast<std::uint8_t>(feature))) != 0;
        }

        [[nodiscard]] constexpr bool contains_all(const backend_capability_set& other) const {
            return (bits & other.bits) == other.bits;
        }

        [[nodiscard]] constexpr backend_capability_set missing(const backend_capability_set& provided) const {
            return backend_capability_set{bits & ~provided.bits};
        }

        [[nodiscard]] constexpr bool empty() const { return bits == 0; }

        // Bitwise set algebra — enables mask operations in template contexts.
        [[nodiscard]] friend constexpr backend_capability_set
        operator|(const backend_capability_set a, const backend_capability_set b) noexcept {
            return backend_capability_set{a.bits | b.bits};
        }

        [[nodiscard]] friend constexpr backend_capability_set
        operator&(const backend_capability_set a, const backend_capability_set b) noexcept {
            return backend_capability_set{a.bits & b.bits};
        }

        [[nodiscard]] friend constexpr backend_capability_set
        operator~(const backend_capability_set a) noexcept {
            return backend_capability_set{~a.bits};
        }

        [[nodiscard]] friend constexpr bool
        operator==(const backend_capability_set a, const backend_capability_set b) noexcept {
            return a.bits == b.bits;
        }
    };

    struct backend_capability_validation_result {
        backend_capability_set required;
        backend_capability_set provided;
        backend_capability_set missing;
        std::vector<backend_feature> missing_features;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    // -----------------------------------------------------------------------
    // Compile-time capability domain hint
    //
    // mir_domain_hint<Caps> is a compile-time NTTP carrier that encodes a
    // known-required capability mask directly in the type.  Passing one as a
    // template argument to emit_artifact_static() lets the compiler prove at
    // instantiation time — via static_assert — that the chosen Target actually
    // provides every capability the MIR needs, rather than deferring the check
    // to runtime.
    //
    // Usage:
    //   constexpr auto float_caps =
    //       backend_capability_set::from({backend_feature::floating_arithmetic,
    //                                     backend_feature::integer_arithmetic});
    //   emit_artifact_static<my_target, mir_domain_hint<float_caps>>(target, result);
    //
    // When the required capability set is not statically knowable (e.g. it
    // depends on data loaded at runtime), use emit_artifact() instead.
    // -----------------------------------------------------------------------

    // NTTP-structural wrapper that carries a backend_capability_set value as a
    // template parameter.  backend_capability_set already has a constexpr ctor
    // from std::uint64_t, so it can serve directly as an NTTP; this alias
    // provides a named concept boundary and documents intent.
    template <backend_capability_set Required>
    struct mir_domain_hint {
        static constexpr backend_capability_set required_capabilities = Required;
    };

    // Concept: T is a mir_domain_hint specialisation.
    template <class T>
    concept IsMirDomainHint = requires {
        { T::required_capabilities } -> std::convertible_to<backend_capability_set>;
    };

    // consteval helper: given a domain_hint type, return its capability mask.
    template <IsMirDomainHint Hint>
    [[nodiscard]] consteval backend_capability_set hint_capabilities() noexcept {
        return Hint::required_capabilities;
    }

    // Convenience factory: build a domain hint from a variadic feature list.
    // Uses a fold expression over operator| to avoid initializer_list in NTTP context.
    // Example:
    //   using float_hint = make_domain_hint_t<backend_feature::floating_arithmetic,
    //                                         backend_feature::integer_arithmetic>;
    namespace detail {
        template <backend_feature... Features>
        inline constexpr backend_capability_set make_caps_from_features() noexcept {
            backend_capability_set out;
            (out.add(Features), ...);
            return out;
        }
    } // namespace detail

    template <backend_feature... Features>
    using make_domain_hint_t = mir_domain_hint<
        detail::make_caps_from_features<Features...>()
    >;

    // -----------------------------------------------------------------------
    // Backend capability refinement
    //
    // backend_capability_requirement lets a backend declare what it needs from
    // the MIR before lowering.  backend_capability_legalization_result carries
    // the outcome of validate_backend_capabilities() when given one.
    //
    // -----------------------------------------------------------------------

    // A declarative description of what a backend needs the MIR to look like.
    // Backends fill this in via a static capabilities() / requirements() method.
    struct backend_capability_requirement {
        // Capability bits that this backend provides (integer arith, branches,
        // etc.).  These are checked against the features the MIR actually uses:
        // if the MIR uses a feature the backend doesn't provide, validation fails.
        // An empty set means "don't check capability bits".
        backend_capability_set provided_capabilities = {};

        // SSA overlay: every instruction must have ssa_defs/ssa_uses populated.
        bool requires_ssa = false;

        // No back edges in the CFG (all loops must have been unrolled or
        // otherwise eliminated before this backend can lower the function).
        bool requires_no_loops = false;

        // All conditional branches must be lowered to unconditional branches
        // (branch_cond instructions are not permitted).
        bool requires_lowered_branches = false;

        // No virtual-register (vreg) operands may remain in the MIR.
        // Physical MIR normally satisfies this, but certain backends
        // (e.g. early-exit debug emitters) may need to assert it explicitly.
        bool requires_no_virtual_registers = false;

        // No unresolved spill operands may remain in the MIR.
        bool requires_no_spills = false;

        // Human-readable name for diagnostics (e.g. "aarch64", "interp").
        std::string backend_name = "unknown";

        // ----------------------------------------------------------------
        // Abstract-operation support
        //
        // These fields are additive — they do not affect any existing
        // backend_feature checks.  All three default to empty (no
        // restriction).
        // ----------------------------------------------------------------

        // Set of operation domains this backend can lower (e.g. "mylib.math").
        // An empty set means the backend does not advertise domain support.
        std::unordered_set<std::string> supported_operation_domains = {};

        // Set of specific operation_ids this backend can lower.
        // An empty set means the backend does not restrict by operation id.
        std::unordered_set<operation_id> supported_operations = {};

        // Union of operation traits this backend can handle.
        // An empty trait set means no trait-level restriction is declared.
        operation_trait_set supported_operation_traits = make_trait_set();

        // Convenience builder: add a provided capability.
        backend_capability_requirement& provide(const backend_feature f) {
            provided_capabilities.add(f);
            return *this;
        }

        // Convenience builder: declare support for an operation domain.
        backend_capability_requirement& support_domain(std::string domain) {
            supported_operation_domains.insert(std::move(domain));
            return *this;
        }

        // Convenience builder: declare support for a specific operation.
        backend_capability_requirement& support_operation(operation_id id) {
            supported_operations.insert(std::move(id));
            return *this;
        }

        // Convenience builder: declare support for an operation trait.
        backend_capability_requirement& support_trait(const operation_trait t) {
            supported_operation_traits = add_trait(supported_operation_traits, t);
            return *this;
        }

        // ----------------------------------------------------------------
        // Semantic type legality
        //
        // These fields let a backend declare which semantic type_kinds and
        // type_ids it can handle.  All default to "unconstrained" (empty /
        // unlimited) so existing backends need no changes.
        // ----------------------------------------------------------------

        // Which semantic::types::type_kind values this backend accepts.
        // Empty = accept all kinds (backwards-compatible default).
        std::unordered_set<int> supported_type_kinds = {};

        // Explicit allow-list of type_ids this backend accepts.
        // Empty = no type_id restriction.
        std::unordered_set<semantic::types::type_id> supported_type_ids = {};

        // SIMD/vector width allow-list (in bits, e.g. {128, 256, 512}).
        // Empty = no width restriction.
        std::unordered_set<std::uint32_t> supported_vector_widths = {};

        // Maximum tensor rank this backend can handle.
        // 0 = no tensor types accepted; std::numeric_limits max = unrestricted.
        std::uint32_t supported_tensor_rank_limit =
            std::numeric_limits<std::uint32_t>::max();

        // True = backend accepts semantic::types::type_kind::dynamic.
        bool supports_dynamic_types = true;

        // True = backend accepts semantic::types::type_kind::symbolic.
        bool supports_symbolic_types = true;

        // Convenience builders for the type-legality fields.
        backend_capability_requirement& accept_type_kind(semantic::types::type_kind k) {
            supported_type_kinds.insert(static_cast<int>(k));
            return *this;
        }

        backend_capability_requirement& accept_type_id(const semantic::types::type_id id) {
            supported_type_ids.insert(id);
            return *this;
        }

        backend_capability_requirement& accept_vector_width(const std::uint32_t bits) {
            supported_vector_widths.insert(bits);
            return *this;
        }

        backend_capability_requirement& set_tensor_rank_limit(const std::uint32_t max_rank) {
            supported_tensor_rank_limit = max_rank;
            return *this;
        }

        backend_capability_requirement& deny_dynamic_types() {
            supports_dynamic_types = false;
            return *this;
        }

        backend_capability_requirement& deny_symbolic_types() {
            supports_symbolic_types = false;
            return *this;
        }
    };

    // Result of validate_backend_capabilities() when given a backend_capability_requirement.
    // Carries the full set of violations so the caller can report them all at once.
    struct backend_capability_legalization_result {
        // True iff the MIR satisfies every constraint in the requirement.
        [[nodiscard]] bool ok() const { return diagnostics.empty(); }

        // One entry per violated requirement.
        std::vector<std::string> diagnostics = {};

        // Set of capability-bit violations (subset of what backend_capability_
        // _validation_result::missing contains).
        backend_capability_set missing_capabilities = {};
        std::vector<backend_feature> missing_features = {};

        // Semantic constraint violations.
        bool ssa_missing = false;
        bool loops_present = false;
        bool conditional_branches = false;
        bool virtual_registers = false;
        bool unresolved_spills = false;

        // ----------------------------------------------------------------
        // Semantic type legality violations
        // ----------------------------------------------------------------

        // type_ids that appear in metadata but are not in supported_type_ids.
        std::vector<semantic::types::type_id> unsupported_type_ids;

        // type_kinds that appear in metadata but are not in supported_type_kinds.
        std::vector<semantic::types::type_kind> unsupported_type_kinds;

        // Vector types whose width is not in supported_vector_widths.
        std::vector<std::uint32_t> unsupported_vector_widths;

        // Tensors whose rank exceeds supported_tensor_rank_limit.
        std::vector<std::uint32_t> oversized_tensor_ranks;

        // True if dynamic types were encountered and supports_dynamic_types = false.
        bool dynamic_type_rejected = false;

        // True if symbolic types were encountered and supports_symbolic_types = false.
        bool symbolic_type_rejected = false;
    };

    // Forward declaration of the requirement-based overload (implemented below,
    // after required_backend_features and the existing capability overload).
    [[nodiscard]] inline backend_capability_legalization_result validate_backend_capabilities(
        const mir::physical_mir_function& fn,
        const backend_capability_requirement& req);

    // Validate that every semantic type referenced in fn's abstract-operation
    // metadata is legal for the given backend_capability_requirement.
    // Pure validation: no rewrites, no lowering.
    [[nodiscard]] inline backend_capability_legalization_result validate_backend_type_legality(
        const mir::physical_mir_function& fn,
        const backend_capability_requirement& req,
        const operation_registry& op_reg,
        const semantic::types::semantic_type_registry& type_reg
            = semantic::types::type_registry());

    template <class Backend>
    concept MachineCodeBackend = requires(Backend backend, const allocated_instruction& inst, backend_state& state) {
        { backend.emit_instruction(inst, state) } -> std::same_as<backend_result>;
    };

    // -----------------------------------------------------------------------
    // Group — generic emission target model
    //
    // target_input_phase describes the IR level a target consumes.
    // emission_target_traits bundles static metadata about a target.
    // CodeEmissionTarget is satisfied by any type that exposes a static
    // traits() and an emit() method returning compilation_artifact.
    // -----------------------------------------------------------------------

    enum class target_input_phase : std::uint8_t {
        physical_mir,
        lowered_machine_mir,
        graph_ir,
        tensor_ir,
        layout_ir,
        query_ir,
    };

    struct emission_target_traits {
        std::string name;
        target_input_phase input_phase;
        artifact_kind produced_artifact;
        backend_capability_set capabilities;
        // Abstract operation support declared by this target.
        // Populated by each target's traits() method.
        // Passed verbatim to validate_operation_legality when a registry is
        // provided to emit_artifact.  All three sub-fields default to empty
        // (no restriction beyond what backend_capability_requirement already
        // specifies for the empty case).
        backend_capability_requirement operation_requirements;
    };

    template <class Target>
    concept CodeEmissionTarget =
        lithe::LitheExtension<Target> &&
        requires(Target target, mir::physical_mir_function const& fn) {
            { Target::traits() } -> std::same_as<emission_target_traits>;
            { Target::capabilities() } -> std::same_as<backend_capability_set>;
            { target.emit(fn) } -> std::same_as<compilation_artifact>;
        };

    // -----------------------------------------------------------------------
    // MirPass concept
    //
    // A type T models MirPass iff it:
    //   1. Satisfies LitheExtension (has a static constexpr descriptor).
    //   2. Is callable as:
    //        T{}(fn, ctx)  ->  mir_pass_result
    //      where fn  : const mir::physical_mir_function &
    //            ctx : mir_pass_context &
    //
    // This mirrors the existing run() convention used by concrete pass structs
    // (trivial_jump_threading_pass, dead_def_elimination_pass, …) but makes
    // the operator() spelling the canonical checked interface so generic
    // pass-pipeline machinery can dispatch without knowing the concrete type.
    //
    // Usage:
    //   static_assert(lithe::codegen::MirPass<my_opt_pass>);
    // -----------------------------------------------------------------------

    template <class T>
    concept MirPass =
        lithe::LitheExtension<T> &&
        requires(T pass,
                 const mir::physical_mir_function& fn,
                 mir_pass_context& ctx) {
            { pass(fn, ctx) } -> std::same_as<mir_pass_result>;
        };

    template <MachineCodeBackend Backend>
    [[nodiscard]] backend_result emit_instruction(
        Backend& backend,
        const allocated_instruction& inst,
        backend_state& state
    ) {
        return backend.emit_instruction(inst, state);
    }

    template <MachineCodeBackend Backend>
    [[nodiscard]] backend_result emit_function(
        Backend& backend,
        const allocated_function_ir& fn,
        backend_state state = {}
    ) {
        if (state.backend_name.empty()) {
            state.backend_name = typeid(Backend).name();
        }

        backend_result final_result = backend_result::success_result(state);

        if constexpr (requires(Backend b, const allocated_function_ir& f, backend_state& s) {
            { b.begin_function(f, s) } -> std::same_as<backend_result>;
        }) {
            auto begin = backend.begin_function(fn, final_result.state);
            final_result.state = std::move(begin.state);
            final_result.errors.insert(final_result.errors.end(), begin.errors.begin(), begin.errors.end());
            if (!begin.ok()) {
                final_result.success = false;
                return final_result;
            }
            if (begin.artifact_text.has_value()) {
                final_result.artifact_text = std::move(begin.artifact_text);
            }
        }

        for (const auto& block : fn.blocks) {
            ++final_result.state.emitted_blocks;
            if constexpr (requires(Backend b, const allocated_basic_block& bb, backend_state& s) {
                { b.begin_block(bb, s) } -> std::same_as<backend_result>;
            }) {
                auto begin_block = backend.begin_block(block, final_result.state);
                final_result.state = std::move(begin_block.state);
                final_result.errors.insert(final_result.errors.end(), begin_block.errors.begin(),
                                           begin_block.errors.end());
                if (!begin_block.ok()) {
                    final_result.success = false;
                    return final_result;
                }
            }

            for (const auto& inst : block.instructions) {
                auto step = emit_instruction(backend, inst, final_result.state);
                final_result.state = std::move(step.state);
                ++final_result.state.emitted_instructions;
                final_result.errors.insert(final_result.errors.end(), step.errors.begin(), step.errors.end());
                if (!step.ok()) {
                    final_result.success = false;
                    return final_result;
                }
            }

            if constexpr (requires(Backend b, const allocated_basic_block& bb, backend_state& s) {
                { b.end_block(bb, s) } -> std::same_as<backend_result>;
            }) {
                auto end_block = backend.end_block(block, final_result.state);
                final_result.state = std::move(end_block.state);
                final_result.errors.insert(final_result.errors.end(), end_block.errors.begin(), end_block.errors.end());
                if (!end_block.ok()) {
                    final_result.success = false;
                    return final_result;
                }
            }
        }

        if constexpr (requires(Backend b, const allocated_function_ir& f, backend_state& s) {
            { b.end_function(f, s) } -> std::same_as<backend_result>;
        }) {
            auto end = backend.end_function(fn, final_result.state);
            final_result.state = std::move(end.state);
            final_result.errors.insert(final_result.errors.end(), end.errors.begin(), end.errors.end());
            if (end.artifact_text.has_value()) {
                final_result.artifact_text = std::move(end.artifact_text);
            }
            if (!end.ok()) {
                final_result.success = false;
            }
        }

        return final_result;
    }

    [[nodiscard]] inline constexpr std::string_view to_string(backend_feature feature);

    [[nodiscard]] inline backend_capability_set required_backend_features(const mir::physical_mir_function& fn);

    [[nodiscard]] inline backend_requirement_set required_backend_requirements(const mir::physical_mir_function& fn);

    [[nodiscard]] inline backend_legalization_result validate_backend_requirements(
        const mir::physical_mir_function& fn,
        const backend_capability_set& capabilities
    );

    // -----------------------------------------------------------------------
    // Operation-level legality validation
    //
    // validate_operation_legality walks every instruction in a physical MIR
    // function and checks abstract operations against the registry and the
    // backend's declared capability set.  It is validation-only: the MIR is
    // never mutated.
    // -----------------------------------------------------------------------

    // Per-violation record produced by validate_operation_legality.
    struct operation_legality_violation {
        std::uint32_t block_id = 0;
        std::uint32_t inst_id = 0;
        std::string operation; // "domain/name" of the offending operation
        std::string reason; // human-readable explanation
    };

    struct operation_legality_result {
        // True iff no violations were found.
        [[nodiscard]] bool ok() const noexcept { return violations.empty(); }

        std::vector<operation_legality_violation> violations;

        // Flat diagnostic strings (one per violation), ready for logging.
        std::vector<std::string> diagnostics;
    };

    [[nodiscard]] inline operation_legality_result validate_operation_legality(
        const mir::physical_mir_function& fn,
        const operation_registry& registry,
        const backend_capability_requirement& req
    );


    [[nodiscard]] inline backend_capability_validation_result validate_backend_capabilities(
        const mir::physical_mir_function& fn,
        const backend_capability_set& capabilities
    );

    [[nodiscard]] inline bool is_physical_mir_ready(const mir::physical_mir_function& fn);

    template <MachineCodeBackend Backend>
    [[nodiscard]] backend_result emit_function(
        Backend& backend,
        const mir::physical_mir_function& fn,
        backend_state state = {}
    ) {
        if (const auto verification = verify_physical_mir(fn); !verification.ok()) {
            backend_result out = backend_result::fail("physical MIR verification failed", std::move(state));
            out.errors.clear();
            for (const auto& diag : verification.diagnostics) {
                out.errors.push_back(backend_error{diag, std::nullopt, std::nullopt});
            }
            return out;
        }

        if constexpr (requires {
            { Backend::capabilities() } -> std::convertible_to<backend_capability_set>;
        }) {
            const auto capability_validation = validate_backend_capabilities(fn, Backend::capabilities());
            if (!capability_validation.ok()) {
                backend_result out = backend_result::fail("backend capability validation failed", std::move(state));
                out.errors.clear();
                for (const auto& diag : capability_validation.diagnostics) {
                    out.errors.push_back(backend_error{diag, std::nullopt, std::nullopt});
                }
                return out;
            }

            const auto legality = validate_backend_requirements(fn, Backend::capabilities());
            if (!legality.ok()) {
                backend_result out = backend_result::fail("backend legality check failed", std::move(state));
                out.errors.clear();
                for (const auto& diag : legality.diagnostics) {
                    out.errors.push_back(backend_error{diag, std::nullopt, std::nullopt});
                }
                return out;
            }
        }

        if (state.backend_name.empty()) {
            state.backend_name = typeid(Backend).name();
        }

        backend_result final_result = backend_result::success_result(state);

        if constexpr (requires(Backend b, const mir::physical_mir_function& f, backend_state& s) {
            { b.begin_function(f, s) } -> std::same_as<backend_result>;
        }) {
            auto begin = backend.begin_function(fn, final_result.state);
            final_result.state = std::move(begin.state);
            final_result.errors.insert(final_result.errors.end(), begin.errors.begin(), begin.errors.end());
            if (!begin.ok()) {
                final_result.success = false;
                return final_result;
            }
        }

        for (const auto& block : fn.function.blocks) {
            ++final_result.state.emitted_blocks;
            if constexpr (requires(Backend b, const allocated_basic_block& bb, backend_state& s) {
                { b.begin_block(bb, s) } -> std::same_as<backend_result>;
            }) {
                auto begin_block = backend.begin_block(block, final_result.state);
                final_result.state = std::move(begin_block.state);
                final_result.errors.insert(final_result.errors.end(), begin_block.errors.begin(),
                                           begin_block.errors.end());
                if (!begin_block.ok()) {
                    final_result.success = false;
                    return final_result;
                }
            }

            for (const auto& inst : block.instructions) {
                auto step = emit_instruction(backend, inst, final_result.state);
                final_result.state = std::move(step.state);
                ++final_result.state.emitted_instructions;
                final_result.errors.insert(final_result.errors.end(), step.errors.begin(), step.errors.end());
                if (!step.ok()) {
                    final_result.success = false;
                    return final_result;
                }
            }

            if constexpr (requires(Backend b, const allocated_basic_block& bb, backend_state& s) {
                { b.end_block(bb, s) } -> std::same_as<backend_result>;
            }) {
                auto end_block = backend.end_block(block, final_result.state);
                final_result.state = std::move(end_block.state);
                final_result.errors.insert(final_result.errors.end(), end_block.errors.begin(), end_block.errors.end());
                if (!end_block.ok()) {
                    final_result.success = false;
                    return final_result;
                }
            }
        }

        if constexpr (requires(Backend b, const mir::physical_mir_function& f, backend_state& s) {
            { b.end_function(f, s) } -> std::same_as<backend_result>;
        }) {
            auto end = backend.end_function(fn, final_result.state);
            final_result.state = std::move(end.state);
            final_result.errors.insert(final_result.errors.end(), end.errors.begin(), end.errors.end());
            if (end.artifact_text.has_value()) {
                final_result.artifact_text = std::move(end.artifact_text);
            }
            if (!end.ok()) {
                final_result.success = false;
            }
            return final_result;
        }

        if constexpr (requires(Backend b, const allocated_function_ir& f, backend_state& s) {
            { b.end_function(f, s) } -> std::same_as<backend_result>;
        }) {
            auto end = backend.end_function(fn.function, final_result.state);
            final_result.state = std::move(end.state);
            final_result.errors.insert(final_result.errors.end(), end.errors.begin(), end.errors.end());
            if (end.artifact_text.has_value()) {
                final_result.artifact_text = std::move(end.artifact_text);
            }
            if (!end.ok()) {
                final_result.success = false;
            }
        }

        return final_result;
    }

    [[nodiscard]] inline constexpr std::string_view to_string(const backend_feature feature) {
        switch (feature) {
        case backend_feature::integer_arithmetic: return "integer_arithmetic";
        case backend_feature::floating_arithmetic: return "floating_arithmetic";
        case backend_feature::spill_load_store: return "spill_load_store";
        case backend_feature::branches: return "branches";
        case backend_feature::calls: return "calls";
        case backend_feature::memory_operands: return "memory_operands";
        case backend_feature::stack_frame: return "stack_frame";
        case backend_feature::interpreter_execution: return "interpreter_execution";
        case backend_feature::tensor_arithmetic: return "tensor_arithmetic";
        case backend_feature::symbolic_arithmetic: return "symbolic_arithmetic";
        }
        return "unknown";
    }

    [[nodiscard]] inline backend_capability_set required_backend_features(const mir::physical_mir_function& fn) {
        backend_capability_set out;

        auto mark_memory_feature = [&](const memory_operand& mem) {
            out.add(backend_feature::memory_operands);
            switch (mem.address.kind) {
            case memory_address_kind::stack_frame:
            case memory_address_kind::argument_slot:
            case memory_address_kind::return_slot:
                out.add(backend_feature::stack_frame);
                break;
            case memory_address_kind::spill_slot:
                out.add(backend_feature::stack_frame);
                out.add(backend_feature::spill_load_store);
                break;
            default:
                break;
            }
        };

        auto classify_operand = [&](const allocated_operand& operand) {
            switch (operand.type) {
            case allocated_operand::kind::spill:
                out.add(backend_feature::spill_load_store);
                out.add(backend_feature::stack_frame);
                break;
            case allocated_operand::kind::memory:
                mark_memory_feature(std::get<memory_operand>(operand.value));
                break;
            case allocated_operand::kind::immediate_f64:
                out.add(backend_feature::floating_arithmetic);
                break;
            default:
                break;
            }
        };

        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                switch (inst.op) {
                case opcode::add:
                case opcode::sub:
                case opcode::mul:
                case opcode::div:
                case opcode::mod:
                case opcode::neg:
                case opcode::cmp_eq:
                case opcode::cmp_ne:
                case opcode::cmp_lt:
                case opcode::cmp_le:
                case opcode::cmp_gt:
                case opcode::cmp_ge:
                case opcode::bit_and:
                case opcode::bit_or:
                case opcode::bit_xor:
                case opcode::bit_not:
                case opcode::shl:
                case opcode::shr:
                case opcode::logical_and:
                case opcode::logical_or:
                case opcode::logical_not:
                case opcode::load_imm:
                    out.add(backend_feature::integer_arithmetic);
                    break;
                case opcode::load_spill:
                case opcode::store_spill:
                    out.add(backend_feature::spill_load_store);
                    out.add(backend_feature::stack_frame);
                    break;
                case opcode::branch:
                case opcode::branch_cond:
                    out.add(backend_feature::branches);
                    break;
                case opcode::call:
                    out.add(backend_feature::calls);
                    break;
                case opcode::load:
                case opcode::store:
                    out.add(backend_feature::memory_operands);
                    break;
                case opcode::fadd:
                case opcode::fsub:
                case opcode::fmul:
                case opcode::fdiv:
                case opcode::fneg:
                case opcode::fload:
                case opcode::fstore:
                case opcode::fload_imm:
                case opcode::gpr_to_fp:
                case opcode::fp_to_gpr:
                case opcode::fcmp_eq:
                case opcode::fcmp_ne:
                case opcode::fcmp_lt:
                case opcode::fcmp_le:
                case opcode::fcmp_gt:
                case opcode::fcmp_ge:
                    out.add(backend_feature::floating_arithmetic);
                    break;
                default:
                    break;
                }

                for (const auto& def : inst.defs) {
                    classify_operand(def);
                }
                for (const auto& use : inst.uses) {
                    classify_operand(use);
                }
            }
        }

        if (fn.frame_layout.has_value() || fn.prologue.has_value() || fn.epilogue.has_value()) {
            out.add(backend_feature::stack_frame);
        }

        return out;
    }

    // Infer backend requirements from a Physical MIR function.
    // Conservative: only reports what the MIR actually uses — does not reject.
    [[nodiscard]] inline backend_requirement_set required_backend_requirements(
        const mir::physical_mir_function& fn
    ) {
        backend_requirement_set out;

        // Always required for Physical MIR before emission.
        out.add(backend_requirement_kind::no_virtual_registers,
                "physical MIR must not contain virtual registers");
        out.add(backend_requirement_kind::no_unresolved_spills,
                "physical MIR must not contain unresolved spill operands");
        out.add(backend_requirement_kind::physical_mir_verified,
                "physical MIR must pass verification before emission");

        bool needs_integer_arith = false;
        bool needs_float_arith = false;
        bool needs_memory_ops = false;
        bool needs_stack_frame = false;
        bool needs_branches = false;
        bool needs_calls = false;
        bool needs_load_store = false;
        bool needs_phi_or_ssa = false;
        bool needs_loops = false;
        bool needs_side_effects = false;

        for (const auto& blk : fn.function.blocks) {
            // phi placeholders in a block indicate phi/SSA usage.
            if (!blk.phi_placeholders.empty())
                needs_phi_or_ssa = true;

            for (const auto& inst : blk.instructions) {
                switch (inst.op) {
                // Integer arithmetic
                case opcode::add:
                case opcode::sub:
                case opcode::mul:
                case opcode::div:
                case opcode::mod:
                case opcode::neg:
                case opcode::cmp_eq:
                case opcode::cmp_ne:
                case opcode::cmp_lt:
                case opcode::cmp_le:
                case opcode::cmp_gt:
                case opcode::cmp_ge:
                case opcode::bit_and:
                case opcode::bit_or:
                case opcode::bit_xor:
                case opcode::bit_not:
                case opcode::shl:
                case opcode::shr:
                case opcode::logical_and:
                case opcode::logical_or:
                case opcode::logical_not:
                case opcode::load_imm:
                    needs_integer_arith = true;
                    break;

                // Load/store (memory)
                case opcode::load:
                case opcode::store:
                    needs_memory_ops = true;
                    needs_load_store = true;
                    break;

                // Spill load/store implies stack frame
                case opcode::load_spill:
                case opcode::store_spill:
                    needs_stack_frame = true;
                    needs_load_store = true;
                    break;

                // Branches / control flow
                case opcode::branch:
                case opcode::branch_cond:
                    needs_branches = true;
                    break;

                // Floating-point arithmetic and conversions
                case opcode::fadd:
                case opcode::fsub:
                case opcode::fmul:
                case opcode::fdiv:
                case opcode::fneg:
                case opcode::fload:
                case opcode::fstore:
                case opcode::fload_imm:
                case opcode::gpr_to_fp:
                case opcode::fp_to_gpr:
                case opcode::fcmp_eq:
                case opcode::fcmp_ne:
                case opcode::fcmp_lt:
                case opcode::fcmp_le:
                case opcode::fcmp_gt:
                case opcode::fcmp_ge:
                    needs_float_arith = true;
                    break;

                // Calls
                case opcode::call:
                    needs_calls = true;
                    needs_side_effects = true;
                    break;

                // ret has side effects
                case opcode::ret:
                    needs_side_effects = true;
                    break;

                default:
                    break;
                }

                // SSA def/use annotations → phi/SSA support.
                if (!inst.ssa_defs.empty() || !inst.ssa_uses.empty())
                    needs_phi_or_ssa = true;

                // Operand-level classification.
                for (const auto& op : {inst.defs, inst.uses}) {
                    for (const auto& operand : op) {
                        switch (operand.type) {
                        case allocated_operand::kind::immediate_f64:
                            needs_float_arith = true;
                            break;
                        case allocated_operand::kind::memory: {
                            needs_memory_ops = true;
                            const auto& mem = std::get<memory_operand>(operand.value);
                            switch (mem.address.kind) {
                            case memory_address_kind::stack_frame:
                            case memory_address_kind::argument_slot:
                            case memory_address_kind::return_slot:
                            case memory_address_kind::spill_slot:
                                needs_stack_frame = true;
                                break;
                            default:
                                break;
                            }
                            break;
                        }
                        case allocated_operand::kind::spill:
                            needs_stack_frame = true;
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
        }

        // Frame layout / prologue / epilogue → stack frame.
        if (fn.frame_layout.has_value() || fn.prologue.has_value() || fn.epilogue.has_value())
            needs_stack_frame = true;

        // Back-edges in the CFG → loop support.
        {
            const auto loops = analyze_loops(fn);
            if (!loops.loops.empty())
                needs_loops = true;
        }

        if (needs_integer_arith)
            out.add(backend_requirement_kind::supports_integer_arithmetic,
                    "MIR contains integer arithmetic opcodes");
        if (needs_float_arith)
            out.add(backend_requirement_kind::supports_floating_arithmetic,
                    "MIR contains floating-point operands or metadata");
        if (needs_memory_ops)
            out.add(backend_requirement_kind::supports_memory_operands,
                    "MIR contains memory operands");
        if (needs_stack_frame)
            out.add(backend_requirement_kind::supports_stack_frame,
                    "MIR uses frame objects or spill slots");
        if (needs_branches)
            out.add(backend_requirement_kind::supports_branches,
                    "MIR contains branch instructions");
        if (needs_calls)
            out.add(backend_requirement_kind::supports_calls,
                    "MIR contains call instructions");
        if (needs_load_store)
            out.add(backend_requirement_kind::supports_load_store,
                    "MIR contains load or store instructions");
        if (needs_phi_or_ssa)
            out.add(backend_requirement_kind::supports_phi_or_ssa,
                    "MIR contains phi placeholders or SSA annotations");
        if (needs_loops)
            out.add(backend_requirement_kind::supports_loops,
                    "MIR CFG contains back edges (loops)");
        if (needs_side_effects)
            out.add(backend_requirement_kind::supports_side_effects,
                    "MIR contains instructions with observable side effects");

        return out;
    }

    // Check whether a backend (identified by its capability_set) can legally emit fn.
    // Calls required_backend_requirements(fn) and tests each requirement against the
    // provided capability bits or structural MIR properties.  Returns a
    // backend_legalization_result with one diagnostic per unmet requirement.
    // Does not throw, emit code, or mutate MIR.
    [[nodiscard]] inline backend_legalization_result validate_backend_requirements(
        const mir::physical_mir_function& fn,
        const backend_capability_set& capabilities
    ) {
        backend_legalization_result out;
        const auto reqs = required_backend_requirements(fn);

        for (const auto& req : reqs.requirements) {
            switch (req.kind) {
            // Structural checks — not represented by capability bits.
            case backend_requirement_kind::no_virtual_registers:
                if (contains_virtual_registers(fn)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: virtual registers present in physical MIR");
                }
                break;

            case backend_requirement_kind::no_unresolved_spills:
                if (contains_unresolved_spills(fn)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: unresolved spill operands present in physical MIR");
                }
                break;

            case backend_requirement_kind::physical_mir_verified:
                if (!verify_physical_mir(fn).ok()) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: physical MIR failed verification before emission");
                }
                break;

            // Capability-bit checks.
            case backend_requirement_kind::supports_integer_arithmetic:
                if (!capabilities.has(backend_feature::integer_arithmetic)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: MIR requires integer_arithmetic but backend does not provide it");
                }
                break;

            case backend_requirement_kind::supports_floating_arithmetic:
                if (!capabilities.has(backend_feature::floating_arithmetic)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: MIR requires floating_arithmetic but backend does not provide it");
                }
                break;

            case backend_requirement_kind::supports_memory_operands:
                if (!capabilities.has(backend_feature::memory_operands)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: MIR requires memory_operands but backend does not provide it");
                }
                break;

            case backend_requirement_kind::supports_stack_frame:
                if (!capabilities.has(backend_feature::stack_frame)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: MIR requires stack_frame but backend does not provide it");
                }
                break;

            case backend_requirement_kind::supports_branches:
                if (!capabilities.has(backend_feature::branches)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: MIR requires branches but backend does not provide it");
                }
                break;

            case backend_requirement_kind::supports_calls:
                if (!capabilities.has(backend_feature::calls)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: MIR requires calls but backend does not provide it");
                }
                break;

            case backend_requirement_kind::supports_load_store:
                if (!capabilities.has(backend_feature::spill_load_store)
                    && !capabilities.has(backend_feature::memory_operands)) {
                    out.legal = false;
                    out.diagnostics.push_back(
                        "legality: MIR requires load/store but backend provides neither "
                        "spill_load_store nor memory_operands");
                }
                break;

            // No capability bits exist for phi/SSA, loops, or side effects.
            // These are informational requirements; we cannot infer from capability bits
            // alone that the backend rejects them, so we skip the check conservatively.
            case backend_requirement_kind::supports_phi_or_ssa:
            case backend_requirement_kind::supports_loops:
            case backend_requirement_kind::supports_side_effects:
                break;
            }
        }

        // ── Domain-level type legality ─────────────────────────────────────────
        // Scan abstract_operation domains on every allocated instruction and
        // reject operations whose domain requires a capability the backend has
        // not advertised.  This is a purely capability-bit check — no RTTI.
        //
        // Domain strings recognised (both short and qualified forms):
        //   tensor / lithe.tensor  → requires tensor_arithmetic
        //   symbolic / lithe.symbolic  → requires symbolic_arithmetic
        //   query / lithe.query  → requires symbolic_arithmetic (no physical repr)
        //   layout / lithe.layout  → requires symbolic_arithmetic (no physical repr)
        //
        // Additionally, if floating_arithmetic is absent we reject any instruction
        // whose operation_attributes carry type = "f64" or "f32", or whose domain
        // is "lithe.typed" with a floating result type annotation.

        const bool has_tensor = capabilities.has(backend_feature::tensor_arithmetic);
        const bool has_symbolic = capabilities.has(backend_feature::symbolic_arithmetic);
        const bool has_fp = capabilities.has(backend_feature::floating_arithmetic);

        auto is_tensor_domain = [](const std::string_view d) constexpr -> bool {
            return d == "tensor" || d == "lithe.tensor";
        };
        auto is_symbolic_domain = [](const std::string_view d) constexpr -> bool {
            return d == "symbolic" || d == "lithe.symbolic"
                || d == "query" || d == "lithe.query"
                || d == "layout" || d == "lithe.layout";
        };

        for (const auto& blk : fn.function.blocks) {
            for (const auto& inst : blk.instructions) {
                const std::string_view dom = inst.abstract_operation
                                                 ? std::string_view{inst.abstract_operation->domain}
                                                 : std::string_view{};

                if (!dom.empty()) {
                    if (!has_tensor && is_tensor_domain(dom)) {
                        out.legal = false;
                        out.diagnostics.push_back(
                            "legality: instruction in bb" + std::to_string(blk.id) +
                            " uses tensor domain '" + std::string(dom) +
                            "' but backend lacks tensor_arithmetic");
                    }
                    if (!has_symbolic && is_symbolic_domain(dom)) {
                        out.legal = false;
                        out.diagnostics.push_back(
                            "legality: instruction in bb" + std::to_string(blk.id) +
                            " uses symbolic/query/layout domain '" + std::string(dom) +
                            "' but backend lacks symbolic_arithmetic");
                    }
                }

                // Data-width check via operation_attributes["type"].
                if (!has_fp) {
                    if (const auto it = inst.operation_attributes.find("type");
                        it != inst.operation_attributes.end()) {
                        const std::string_view tv = it->second;
                        if (tv == "f64" || tv == "f32" || tv == "f16" || tv == "bf16") {
                            out.legal = false;
                            out.diagnostics.push_back(
                                "legality: instruction in bb" + std::to_string(blk.id) +
                                " operates on floating-point type '" + std::string(tv) +
                                "' but backend lacks floating_arithmetic");
                        }
                    }
                }
            }
        }

        return out;
    }

    [[nodiscard]] inline backend_capability_validation_result validate_backend_capabilities(
        const mir::physical_mir_function& fn,
        const backend_capability_set& capabilities
    ) {
        backend_capability_validation_result out;
        out.required = required_backend_features(fn);
        out.provided = capabilities;
        out.missing = out.required.missing(capabilities);

        if (out.missing.empty()) {
            return out;
        }

        constexpr std::array all_features{
            backend_feature::integer_arithmetic,
            backend_feature::floating_arithmetic,
            backend_feature::spill_load_store,
            backend_feature::branches,
            backend_feature::calls,
            backend_feature::memory_operands,
            backend_feature::stack_frame,
            backend_feature::interpreter_execution
        };

        for (const auto feature : all_features) {
            if (!out.missing.has(feature)) {
                continue;
            }
            out.missing_features.push_back(feature);
            out.diagnostics.push_back(
                std::string{"backend capability missing: feature="} + std::string{to_string(feature)});
        }

        return out;
    }

    // validate_backend_capabilities overload for backend_capability_requirement.
    // Checks all declared constraints and returns a backend_capability_legalization_result
    // that details every violation. No rewrites are performed.
    [[nodiscard]] inline backend_capability_legalization_result validate_backend_capabilities(
        const mir::physical_mir_function& fn,
        const backend_capability_requirement& req
    ) {
        backend_capability_legalization_result out;

        // --- Capability-bit check (only when the backend declares capabilities) ---
        if (!req.provided_capabilities.empty()) {
            const auto cap_result =
                validate_backend_capabilities(fn, req.provided_capabilities);
            out.missing_capabilities = cap_result.missing;
            out.missing_features = cap_result.missing_features;
            for (const auto& d : cap_result.diagnostics)
                out.diagnostics.push_back(req.backend_name + ": " + d);
        }

        // --- requires_ssa ---
        if (req.requires_ssa) {
            bool has_ssa = false;
            for (const auto& blk : fn.function.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (!inst.ssa_defs.empty() || !inst.ssa_uses.empty()) {
                        has_ssa = true;
                        break;
                    }
                }
                if (has_ssa) break;
            }
            if (!has_ssa) {
                out.ssa_missing = true;
                out.diagnostics.push_back(
                    req.backend_name + ": requires SSA but no ssa_defs/ssa_uses found");
            }
        }

        // --- requires_no_loops ---
        if (req.requires_no_loops) {
            // A loop exists iff the CFG has at least one back edge.
            const auto loop = analyze_loops(fn);
            if (!loop.loops.empty()) {
                out.loops_present = true;
                out.diagnostics.push_back(
                    req.backend_name + ": requires no loops but " +
                    std::to_string(loop.loops.size()) + " loop(s) found");
            }
        }

        // --- requires_lowered_branches ---
        if (req.requires_lowered_branches) {
            for (const auto& blk : fn.function.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (inst.op == opcode::branch_cond) {
                        out.conditional_branches = true;
                        out.diagnostics.push_back(
                            req.backend_name +
                            ": requires lowered branches but branch_cond found in bb" +
                            std::to_string(blk.id) + " inst " + std::to_string(inst.id));
                        goto done_branches;
                    }
                }
            }
        done_branches:;
        }

        // --- requires_no_virtual_registers ---
        if (req.requires_no_virtual_registers) {
            if (contains_virtual_registers(fn)) {
                out.virtual_registers = true;
                out.diagnostics.push_back(
                    req.backend_name + ": requires no virtual registers but vregs found");
            }
        }

        // --- requires_no_spills ---
        if (req.requires_no_spills) {
            if (contains_unresolved_spills(fn)) {
                out.unresolved_spills = true;
                out.diagnostics.push_back(
                    req.backend_name + ": requires no spills but unresolved spill operands found");
            }
        }

        return out;
    }

    [[nodiscard]] inline operation_legality_result validate_operation_legality(
        const mir::physical_mir_function& fn,
        const operation_registry& registry,
        const backend_capability_requirement& req
    ) {
        operation_legality_result out;

        const std::string prefix = req.backend_name.empty() ? "" : req.backend_name + ": ";

        // Fallback lookup: build id → original instruction* for instructions that
        // were not given abstract_operation metadata during allocation.
        std::unordered_map<std::uint32_t, const instruction*> orig_by_id;
        for (const auto& blk : fn.function.original_vreg_ir.blocks) {
            for (const auto& orig : blk.instructions) {
                orig_by_id.emplace(orig.id, &orig);
            }
        }

        auto emit = [&](const std::uint32_t blk_id, const std::uint32_t inst_id,
                        const std::string& op_str, std::string reason) {
            operation_legality_violation v;
            v.block_id = blk_id;
            v.inst_id = inst_id;
            v.operation = op_str;
            v.reason = std::move(reason);
            out.diagnostics.push_back(prefix + "bb" + std::to_string(blk_id) +
                " inst" + std::to_string(inst_id) +
                " [" + op_str + "]: " + v.reason);
            out.violations.push_back(std::move(v));
        };

        for (const auto& blk : fn.function.blocks) {
            for (const auto& inst : blk.instructions) {
                // ── 1. Resolve effective operation ────────────────────────
                // Prefer the metadata carried on the allocated instruction;
                // fall back to original_vreg_ir lookup when absent.
                bool is_abstract = inst.abstract_operation.has_value();
                operation_id op_id;
                if (is_abstract) {
                    op_id = *inst.abstract_operation;
                }
                else {
                    const instruction* orig = nullptr;
                    if (const auto it = orig_by_id.find(inst.id); it != orig_by_id.end()) {
                        orig = it->second;
                    }
                    is_abstract = orig && orig->abstract_operation.has_value();
                    op_id = is_abstract
                                ? *orig->abstract_operation
                                : legacy_opcode_operation_id(inst.op);
                }

                const std::string op_str = op_id.domain + "/" + op_id.name;

                // ── 2. Descriptor must exist for abstract operations ──────
                if (is_abstract && !registry.contains(op_id)) {
                    emit(blk.id, inst.id, op_str,
                         "abstract operation has no registered descriptor");
                    continue; // remaining checks require the descriptor
                }

                // ── 3. Domain support check ───────────────────────────────
                if (!req.supported_operation_domains.empty() &&
                    !req.supported_operation_domains.count(op_id.domain)) {
                    emit(blk.id, inst.id, op_str,
                         "operation domain '" + op_id.domain + "' not supported by backend");
                }

                // ── 4. Explicit operation support check ───────────────────
                if (!req.supported_operations.empty() &&
                    !req.supported_operations.count(op_id)) {
                    emit(blk.id, inst.id, op_str,
                         "operation '" + op_str + "' not in backend supported_operations set");
                }

                // ── 5. Trait / contract checks (requires a descriptor) ────
                const operation_descriptor* desc = registry.find(op_id);
                if (!desc) continue;

                const operation_trait_set actual_traits = desc->traits;

                // 5a. Backend must cover every trait the operation carries.
                if (req.supported_operation_traits != make_trait_set()) {
                    const operation_trait_set unsupported =
                        actual_traits & ~req.supported_operation_traits;
                    if (unsupported != make_trait_set()) {
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "0x%x", unsupported);
                        emit(blk.id, inst.id, op_str,
                             "operation carries traits (" + std::string{buf} +
                             ") not declared in backend supported_operation_traits");
                    }
                }

                // 5b. Contract: required_traits must be present on the operation.
                const operation_trait_set missing_req =
                    desc->contract.required_traits & ~actual_traits;
                if (missing_req != make_trait_set()) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "0x%x", missing_req);
                    emit(blk.id, inst.id, op_str,
                         "operation missing contract-required traits (" + std::string{buf} + ")");
                }

                // 5c. Contract: forbidden_traits must not appear.
                const operation_trait_set present_forbidden =
                    desc->contract.forbidden_traits & actual_traits;
                if (present_forbidden != make_trait_set()) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "0x%x", present_forbidden);
                    emit(blk.id, inst.id, op_str,
                         "operation carries contract-forbidden traits (" + std::string{buf} + ")");
                }
            }
        }

        return out;
    }

    [[nodiscard]] inline bool is_physical_mir_ready(const mir::physical_mir_function& fn) {
        if (!fn.spill_rewritten || !fn.verified || !fn.verification_diagnostics.empty()) {
            return false;
        }
        if (contains_virtual_registers(fn) || contains_unresolved_spills(fn) || has_duplicate_instruction_ids(fn)) {
            return false;
        }
        return validate_branch_targets(fn).ok();
    }

    enum class mir_opt_level : std::uint8_t {
        O0,
        O1,
        O2,
        Debug
    };

    enum class mir_pipeline_preset_kind : std::uint8_t {
        none,
        debug,
        conservative,
        aggressive,
        custom
    };

    class mir_pass_pipeline;
    struct mir_pipeline_result;
    struct constexpr_pipeline_result;
    using mir_pipeline_customizer = std::function<void(mir_pass_pipeline&)>;

    struct mir_pass_descriptor {
        using runner_type = std::function<mir_pass_result(const mir::physical_mir_function &, mir_pass_context &)>;

        std::string name;
        bool enabled = true;
        runner_type run;
    };

    class mir_pass_pipeline {
    public:
        void add_pass(mir_pass_descriptor pass) {
            if (pass.name.empty()) {
                pass.name = "unnamed_mir_pass";
            }
            passes_.push_back(std::move(pass));
        }

        template <class Pass>
            requires requires(Pass pass, const mir::physical_mir_function& fn, mir_pass_context& ctx) {
                { pass.run(fn, ctx) } -> std::same_as<mir_pass_result>;
            }
        void add_pass(std::string name, Pass pass) {
            if (name.empty()) {
                name = "unnamed_mir_pass";
            }

            mir_pass_descriptor descriptor;
            descriptor.name = std::move(name);
            descriptor.run = [pass = std::move(pass)](const mir::physical_mir_function& fn,
                                                      mir_pass_context& ctx) mutable -> mir_pass_result {
                auto result = pass.run(fn, ctx);
                if (result.function.function.blocks.empty()) {
                    result.function = fn;
                }
                return result;
            };

            add_pass(std::move(descriptor));
        }

        template <class Pass>
            requires requires(Pass pass, const mir::physical_mir_function& fn, mir_pass_context& ctx) {
                { pass.run(fn, ctx) } -> std::same_as<mir_pass_result>;
            }
        void add_pass(Pass pass) {
            add_pass(std::string{typeid(Pass).name()}, std::move(pass));
        }

        [[nodiscard]] bool remove_pass_by_name(std::string_view name) {
            const auto old_size = passes_.size();
            passes_.erase(
                std::remove_if(passes_.begin(), passes_.end(), [name](const mir_pass_descriptor& d) {
                    return d.name == name;
                }),
                passes_.end()
            );
            return passes_.size() != old_size;
        }

        bool enable_pass(const std::string_view name) {
            for (auto& d : passes_) {
                if (d.name == name) {
                    d.enabled = true;
                    return true;
                }
            }
            return false;
        }

        bool disable_pass(const std::string_view name) {
            for (auto& d : passes_) {
                if (d.name == name) {
                    d.enabled = false;
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool contains_pass(const std::string_view name) const {
            for (const auto& d : passes_) {
                if (d.name == name) return true;
            }
            return false;
        }

        [[nodiscard]] std::vector<std::string> pass_names() const {
            std::vector<std::string> names;
            names.reserve(passes_.size());
            for (const auto& d : passes_) {
                names.push_back(d.name);
            }
            return names;
        }

        void clear() { passes_.clear(); }

        [[nodiscard]] std::size_t size() const { return passes_.size(); }

        [[nodiscard]] bool empty() const { return passes_.empty(); }

        [[nodiscard]] mir_pipeline_result run(const mir::physical_mir_function& fn, mir_pass_context& context) const;

        // constexpr_run: policy-selected pass execution.
        //
        // Accepts a variadic list of statically-typed passes (each must satisfy
        // `pass.run(fn, ctx) -> mir_pass_result`).  Uses execution_storage for
        // diagnostics so the call can appear in a constexpr function.
        //
        // Passes are executed in argument order; the runtime passes_ list is
        // ignored.  No std::function dispatch, no exception handling, no
        // verify_physical_mir — those are runtime-only.
        //
        // ExecCtx must be an execution_context<Policy> instantiation (defined
        // in Group R).  The constraint is checked at the definition site.
        template <class ExecCtx, class... Passes>
        [[nodiscard]] constexpr constexpr_pipeline_result
        constexpr_run(const mir::physical_mir_function& fn,
                      ExecCtx& exec_ctx,
                      Passes&&... passes) const;

    private:
        std::vector<mir_pass_descriptor> passes_;
    };

    struct mir_pipeline_result {
        mir::physical_mir_function function;
        mir_pass_statistics statistics;
        mir_pass_trace_log trace;
        std::vector<std::string> diagnostics;
        bool changed = false;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    // mir_pass_pipeline::run — defined after mir_pipeline_result is complete.
    //
    // Monadic chaining design
    //   • Each pass is wrapped in an and_then continuation over
    //     std::expected<mir_pass_value, pass_error>.
    //   • A pass that returns std::unexpected immediately short-circuits the
    //     rest of the pipeline; no subsequent pass is executed.
    //   • try/catch is gone: exceptions from a third-party pass are caught
    //     once per pass step and converted into pass_error, which propagates
    //     as std::unexpected.
    //   • Statistics and trace bookkeeping are mutated through a captured
    //     reference to the mir_pass_context; they accumulate up to the
    //     failing pass and then stop.
    //   • The transient arena in mir_pass_context is reset after every pass so
    //     scratch allocations for dominator / PDG structures do not outlive
    //     the pass that built them.
    [[nodiscard]] inline mir_pipeline_result mir_pass_pipeline::run(
        const mir::physical_mir_function& fn, mir_pass_context& context
    ) const {
        const auto instruction_count_of = [](const mir::physical_mir_function& mir_fn) {
            std::size_t count = 0;
            for (const auto& block : mir_fn.function.blocks) {
                count += block.instructions.size();
            }
            return count;
        };

        context.changed = false;

        // Seed: the initial function wrapped in a success expected.
        mir_pass_expected chain = mir_pass_result{fn};

        for (const auto& descriptor : passes_) {
            // Short-circuit: if a previous pass already failed, skip everything.
            if (!chain) break;

            if (!descriptor.enabled) {
                if (context.enable_trace) {
                    mir_pass_trace_event ev;
                    ev.kind = mir_pass_trace_event_kind::pass_begin;
                    ev.pass_name = descriptor.name;
                    ev.message = "skipped (disabled)";
                    context.trace.add(std::move(ev));
                }
                continue;
            }

            if (!descriptor.run) {
                context.diagnostics.push_back(
                    "mir pipeline: pass '" + descriptor.name + "' has no runner");
                continue;
            }

            if (context.optimization_budget != 0
                && context.statistics.executed_passes >= context.optimization_budget) {
                context.diagnostics.push_back(
                    "mir pipeline: optimization budget reached before pass '"
                    + descriptor.name + "'");
                break;
            }

            context.statistics.executed_passes += 1;
            context.statistics.executed_pass_names.push_back(descriptor.name);

            // and_then: only entered if chain currently holds a value.
            chain = std::move(chain).and_then(
                [&](mir_pass_result current) -> mir_pass_expected {
                    const std::size_t before_inst = instruction_count_of(current.function);
                    const std::size_t before_block = current.function.function.blocks.size();

                    if (context.enable_trace) {
                        mir_pass_trace_event ev;
                        ev.kind = mir_pass_trace_event_kind::pass_begin;
                        ev.pass_name = descriptor.name;
                        ev.instruction_count_before = before_inst;
                        ev.block_count_before = before_block;
                        context.trace.add(std::move(ev));
                    }

                    // --- Invoke the pass (no try/catch escape; exceptions map to pass_error) ---
                    mir_pass_result step_result;
                    try {
                        step_result = descriptor.run(current.function, context);
                    }
                    catch (const std::exception& ex) {
                        return std::unexpected(pass_error::from_message(
                            descriptor.name,
                            "pass threw exception: " + std::string{ex.what()}));
                    }
                    catch (...) {
                        return std::unexpected(pass_error::from_message(
                            descriptor.name, "pass threw unknown exception"));
                    }

                    // --- Pass returned an error: bubble it up immediately. ---
                    if (!step_result.ok()) {
                        // Merge pass diagnostics into context before propagating.
                        for (const auto& m : step_result.diagnostics) {
                            context.diagnostics.push_back(
                                "mir pipeline: pass '" + descriptor.name + "': " + m);
                        }
                        return std::unexpected(pass_error::from_message(
                            descriptor.name,
                            step_result.diagnostics.empty()
                                ? "pass failed"
                                : step_result.diagnostics.front()));
                    }

                    // --- Trace: per-diagnostic events. ---
                    if (context.enable_trace) {
                        for (const auto& diag : step_result.diagnostics) {
                            mir_pass_trace_event ev;
                            ev.kind = mir_pass_trace_event_kind::diagnostic;
                            ev.pass_name = descriptor.name;
                            ev.message = diag;
                            context.trace.add(std::move(ev));
                        }
                    }

                    // Accumulate informational diagnostics into context.
                    context.diagnostics.insert(context.diagnostics.end(),
                                               step_result.diagnostics.begin(),
                                               step_result.diagnostics.end());

                    // --- Verify output MIR if required. ---
                    const auto& candidate = step_result.changed ? step_result.function : current.function;
                    if (context.verify_after_each_pass) {
                        const auto verification = verify_physical_mir(candidate);
                        if (!verification.ok()) {
                            context.diagnostics.push_back(
                                "mir pipeline: pass '" + descriptor.name
                                + "' produced invalid physical MIR; keeping previous MIR");
                            context.diagnostics.insert(context.diagnostics.end(),
                                                       verification.diagnostics.begin(),
                                                       verification.diagnostics.end());
                            if (context.enable_trace) {
                                mir_pass_trace_event ev;
                                ev.kind = mir_pass_trace_event_kind::verification_failure;
                                ev.pass_name = descriptor.name;
                                ev.instruction_count_before = before_inst;
                                ev.block_count_before = before_block;
                                ev.message = verification.diagnostics.empty()
                                                 ? "verification failed"
                                                 : verification.diagnostics.front();
                                context.trace.add(std::move(ev));
                            }
                            context.statistics.unchanged_passes += 1;
                            context.statistics.total_unchanged_instructions += before_inst;
                            context.statistics.total_unchanged_blocks += before_block;
                            // Restore the pre-pass function and continue.
                            context.arena.reset();
                            return mir_pass_result{current.function};
                        }
                    }

                    // --- Reset per-pass scratch memory. ---
                    context.arena.reset();

                    if (!step_result.changed) {
                        if (context.enable_trace) {
                            mir_pass_trace_event ev;
                            ev.kind = mir_pass_trace_event_kind::pass_end;
                            ev.pass_name = descriptor.name;
                            ev.instruction_count_before = before_inst;
                            ev.instruction_count_after = before_inst;
                            ev.block_count_before = before_block;
                            ev.block_count_after = before_block;
                            context.trace.add(std::move(ev));
                        }
                        context.statistics.unchanged_passes += 1;
                        context.statistics.total_unchanged_instructions += before_inst;
                        context.statistics.total_unchanged_blocks += before_block;
                        return mir_pass_result{current.function};
                    }

                    // --- Pass changed the MIR: update statistics. ---
                    const std::size_t after_inst = instruction_count_of(step_result.function);
                    const std::size_t after_block = step_result.function.function.blocks.size();

                    const std::size_t removed_inst =
                        step_result.removed_instructions > 0
                            ? step_result.removed_instructions
                            : (before_inst > after_inst ? before_inst - after_inst : 0u);
                    const std::size_t removed_blk =
                        step_result.removed_blocks > 0
                            ? step_result.removed_blocks
                            : (before_block > after_block ? before_block - after_block : 0u);

                    const std::size_t added_inst = after_inst > before_inst ? after_inst - before_inst : 0u;
                    const std::size_t added_blk = after_block > before_block ? after_block - before_block : 0u;
                    const std::size_t changed_inst = std::min(before_inst, after_inst);
                    const std::size_t changed_blk = std::min(before_block, after_block);

                    if (context.enable_trace) {
                        mir_pass_trace_event ev;
                        ev.kind = mir_pass_trace_event_kind::pass_end;
                        ev.pass_name = descriptor.name;
                        ev.instruction_count_before = before_inst;
                        ev.instruction_count_after = after_inst;
                        ev.block_count_before = before_block;
                        ev.block_count_after = after_block;
                        context.trace.add(std::move(ev));
                    }

                    context.changed = true;
                    context.statistics.changed_passes += 1;
                    context.statistics.total_removed_instructions += removed_inst;
                    context.statistics.total_removed_blocks += removed_blk;
                    context.statistics.total_added_instructions += added_inst;
                    context.statistics.total_added_blocks += added_blk;
                    context.statistics.total_changed_instructions += changed_inst;
                    context.statistics.total_changed_blocks += changed_blk;
                    context.statistics.removed_instructions_by_pass[descriptor.name] += removed_inst;
                    context.statistics.removed_blocks_by_pass[descriptor.name] += removed_blk;

                    return step_result;
                }
            );
        }

        // --- Assemble the pipeline result from the final chain state. ---
        mir_pipeline_result out;
        if (chain) {
            out.function = std::move(chain->function);
        }
        else {
            // A pass hard-failed: carry the original function and the error payload.
            out.function = fn;
            for (const auto& m : chain.error().messages) {
                out.diagnostics.push_back(
                    "pass '" + chain.error().pass_name + "' failed: " + m);
            }
        }
        out.changed = context.changed;
        out.statistics = context.statistics;
        out.trace = context.trace;
        // Merge context diagnostics (they include per-pass info) into result.
        out.diagnostics.insert(out.diagnostics.end(),
                               context.diagnostics.begin(),
                               context.diagnostics.end());
        return out;
    }

    struct mir_pipeline_preset {
        std::string name;
        mir_pipeline_preset_kind kind = mir_pipeline_preset_kind::none;
        mir_pass_pipeline pipeline;
        bool verify_after_each_pass = true;
        bool trace_enabled = false;
    };

    struct codegen_options {
        function_signature signature;
        target_abi abi;
        bool run_verification = true;
        bool enable_spill_rewrite = true;
        bool enable_frame_layout = true;
        bool enable_register_pressure_analysis = false;
        bool enable_peephole = false;
        mir_opt_level mir_optimization_level = mir_opt_level::O0;
        bool verify_after_each_mir_pass = true;
        mir_pipeline_customizer customize_mir_pipeline;
        peephole_options peephole;
        std::optional<mir_pass_pipeline> custom_mir_pipeline;
        bool use_custom_mir_pipeline = false;

        codegen_options& with_mir_pipeline(mir_pass_pipeline pipeline) {
            custom_mir_pipeline = std::move(pipeline);
            use_custom_mir_pipeline = true;
            return *this;
        }

        codegen_options& with_mir_opt_level(const mir_opt_level level) {
            mir_optimization_level = level;
            use_custom_mir_pipeline = false;
            return *this;
        }

        // Optional registry for abstract-operation validation.
        // When set, compile_to_artifact / emit_artifact will call
        // validate_operation_legality before emitting.  When null the
        // legacy behaviour (no operation checks) is preserved.
        const operation_registry* op_registry = nullptr;

        codegen_options& with_operation_registry(const operation_registry& reg) {
            op_registry = &reg;
            return *this;
        }
    };

    // =========================================================================
    // sroa_pass — Scalar Replacement of Aggregates
    //
    // Shatters non-escaping local aggregate allocations (frame_object_kind::local_slot
    // whose address is never taken by a call) into per-field scalar vregs so that
    // downstream SSA optimizations can reason about each field independently.
    //
    // Algorithm
    // ---------
    //   Phase 1 (analysis, arena-backed):
    //     • Build a map: local_slot frame-object-id → set of GEP instruction ids
    //       that reference it.
    //     • Escape check: if any GEP def-preg flows into a call/indirect_call use
    //       or is stored at an unknown address, the slot is marked "escaped" and
    //       excluded from SROA.
    //
    //   Phase 2 (validation, monadic):
    //     • Returns std::expected<sroa_analysis_result, sroa_error>; fails if a
    //       GEP carries an invalid field-index attribute.
    //
    //   Phase 3 (rewrite):
    //     • Per eligible slot, allocate one fresh vreg per distinct field index.
    //     • Replace every:
    //         load  dst  ←  gep_result        →  mov  dst  ←  field_vreg[idx]
    //         store val  →  gep_result        →  mov  field_vreg[idx]  ←  val
    //     • Remove the GEP instructions and the local-slot alloc pseudo-op.
    //
    // Constraints
    // -----------
    //   • All scratch containers use ctx.arena (trivially destructible wrappers).
    //   • The function is re-verified after a successful rewrite (follows the
    //     same pattern used by dead_def_elimination_pass et al.).
    //   • field index is carried in operation_attributes["sroa.field_index"].
    // =========================================================================

    namespace sroa_detail {
        // Trivially destructible arena-friendly pair for (frame_obj_id, preg_id).
        struct slot_gep_entry {
            std::uint32_t frame_obj_id = 0;
            std::uint32_t gep_inst_id = 0;
            std::uint32_t gep_def_preg = 0; // preg id produced by the GEP
            std::uint32_t field_index = 0; // field index attribute value
            std::uint32_t block_id = 0;
        };

        // Escape record — each GEP preg that reaches a call use.
        struct escape_entry {
            std::uint32_t frame_obj_id = 0;
        };
    } // namespace sroa_detail

    // -------------------------------------------------------------------------
    // Error / analysis types (non-arena, heap-allocated — small and few).
    // -------------------------------------------------------------------------
    struct sroa_error {
        enum class kind : std::uint8_t {
            invalid_field_index, // operation_attributes["sroa.field_index"] is non-numeric
            layout_not_found, // no layout registered for the slot's type
        };

        kind type = kind::invalid_field_index;
        std::uint32_t instruction_id = 0;
        std::string message;
    };

    struct sroa_field_slot {
        std::uint32_t field_index = 0;
        std::uint32_t scalar_vreg = 0; // newly assigned vreg id
    };

    struct sroa_slot_record {
        std::uint32_t frame_obj_id = 0;
        std::vector<sroa_field_slot> fields; // one per distinct field_index
        std::vector<std::uint32_t> gep_inst_ids; // all GEP insts for this slot
        bool escaped = false;

        [[nodiscard]] std::uint32_t vreg_for_field(const std::uint32_t idx) const noexcept {
            for (const auto& f : fields) {
                if (f.field_index == idx) return f.scalar_vreg;
            }
            return 0;
        }
    };

    struct sroa_analysis_result {
        std::vector<sroa_slot_record> eligible_slots;
        std::size_t total_gep_count = 0;
        std::size_t escaped_count = 0;
    };

    // -------------------------------------------------------------------------
    // sroa_pass
    // -------------------------------------------------------------------------
    struct sroa_pass {
        // Minimum aggregate size (bytes) below which SROA is profitable.
        // Aggregates larger than this are skipped to avoid register pressure.
        std::size_t max_aggregate_bytes = 64;

        [[nodiscard]] mir_pass_result run(mir::physical_mir_function const& fn,
                                          mir_pass_context& ctx) const {
            mir_pass_result out;
            out.function = fn;

            // -----------------------------------------------------------------
            // Phase 1: arena-backed analysis
            //
            // We need to:
            //  (a) find all GEP instructions that target a local-slot frame obj,
            //  (b) collect the preg each GEP defines,
            //  (c) determine whether that preg escapes into a call/indirect_call.
            // -----------------------------------------------------------------

            // Count total instructions for arena sizing hint.
            std::size_t total_insts = 0;
            for (const auto& blk : fn.function.blocks)
                total_insts += blk.instructions.size();

            // Arena scratch: raw arrays of slot_gep_entry and escape_entry.
            const std::size_t gep_cap = total_insts + 1;
            auto* gep_scratch = ctx.arena.alloc<sroa_detail::slot_gep_entry>(gep_cap);
            std::size_t gep_count = 0;

            // Build preg→frame_obj_id map for GEP defs (heap — map isn't trivially
            // destructible; only the per-entry scratch lives in the arena).
            std::unordered_map<std::uint32_t, std::uint32_t> gep_preg_to_frame_obj;
            std::unordered_map<std::uint32_t, std::uint32_t> gep_preg_to_field_idx;
            std::unordered_set<std::uint32_t> escaped_frame_objs;

            // Also gather local-slot frame object ids from the frame layout so we
            // can check sizes.
            std::unordered_map<std::uint32_t, std::size_t> local_slot_sizes;
            if (fn.frame_layout) {
                for (const auto& obj : fn.frame_layout->objects) {
                    if (obj.kind == frame_object_kind::local_slot) {
                        local_slot_sizes[obj.id.value] = obj.size;
                    }
                }
            }

            // Pass 1a: walk all instructions, collect GEP entries and escape info.
            for (const auto& blk : fn.function.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (inst.op == opcode::get_element_ptr) {
                        // Expect: uses[0] = base preg or frame-object, uses[1] = index.
                        // The base may be encoded as a frame_object reference via a
                        // load_imm with operation_attributes["sroa.frame_obj"].
                        // Simpler convention used here: the GEP itself carries
                        // "sroa.frame_obj_id" and "sroa.field_index" in its attributes.
                        const auto fo_it = inst.operation_attributes.find("sroa.frame_obj_id");
                        const auto fi_it = inst.operation_attributes.find("sroa.field_index");
                        if (fo_it == inst.operation_attributes.end() ||
                            fi_it == inst.operation_attributes.end()) {
                            continue; // not an SROA-eligible GEP
                        }

                        // Validate attributes are numeric.
                        std::uint32_t fo_id = 0;
                        std::uint32_t fi_idx = 0;
                        try {
                            fo_id = static_cast<std::uint32_t>(std::stoul(fo_it->second));
                            fi_idx = static_cast<std::uint32_t>(std::stoul(fi_it->second));
                        }
                        catch (...) {
                            // Defer error to Phase 2 — continue collecting valid GEPs.
                            continue;
                        }

                        // Skip if frame object not a local slot or too large.
                        if (!local_slot_sizes.contains(fo_id)) continue;
                        if (local_slot_sizes[fo_id] > max_aggregate_bytes) continue;

                        // Extract the def preg (first def).
                        if (inst.defs.empty() ||
                            inst.defs[0].type != allocated_operand::kind::preg)
                            continue;
                        const auto def_preg_id =
                            std::get<preg>(inst.defs[0].value).id;

                        // Store in arena scratch.
                        if (gep_count < gep_cap) {
                            gep_scratch[gep_count++] = {
                                fo_id, inst.id,
                                static_cast<std::uint32_t>(def_preg_id),
                                fi_idx, blk.id
                            };
                        }
                        gep_preg_to_frame_obj[def_preg_id] = fo_id;
                        gep_preg_to_field_idx[def_preg_id] = fi_idx;
                        continue;
                    }

                    // Escape check: if a GEP-produced preg is used by a call or
                    // indirect_call, the frame object escapes.
                    if (inst.op == opcode::call ||
                        inst.op == opcode::indirect_call) {
                        for (const auto& use_op : inst.uses) {
                            if (use_op.type != allocated_operand::kind::preg) continue;
                            const auto use_id = std::get<preg>(use_op.value).id;
                            if (const auto it = gep_preg_to_frame_obj.find(use_id);
                                it != gep_preg_to_frame_obj.end()) {
                                escaped_frame_objs.insert(it->second);
                            }
                        }
                    }
                }
            }

            if (gep_count == 0) {
                // Nothing to do.
                return out;
            }

            // -----------------------------------------------------------------
            // Phase 2: validation — returns expected<analysis, error>
            // -----------------------------------------------------------------
            auto validate = [&]() -> std::expected<sroa_analysis_result, sroa_error> {
                sroa_analysis_result analysis;
                analysis.total_gep_count = gep_count;
                analysis.escaped_count = escaped_frame_objs.size();

                // Group GEP entries by frame_obj_id.
                std::unordered_map<std::uint32_t, sroa_slot_record> slot_map;
                for (std::size_t i = 0; i < gep_count; ++i) {
                    const auto& e = gep_scratch[i];

                    // Re-validate attributes from the live instructions for safety.
                    const allocated_instruction* inst_ptr = nullptr;
                    for (const auto& blk : fn.function.blocks) {
                        for (const auto& inst : blk.instructions) {
                            if (inst.id == e.gep_inst_id) {
                                inst_ptr = &inst;
                                break;
                            }
                        }
                        if (inst_ptr) break;
                    }
                    if (!inst_ptr) continue;

                    const auto fi_it = inst_ptr->operation_attributes.find("sroa.field_index");
                    if (fi_it == inst_ptr->operation_attributes.end()) {
                        return std::unexpected(sroa_error{
                            sroa_error::kind::invalid_field_index,
                            e.gep_inst_id,
                            "sroa.field_index attribute missing on GEP"
                        });
                    }
                    std::uint32_t fi_idx = 0;
                    try {
                        fi_idx = static_cast<std::uint32_t>(std::stoul(fi_it->second));
                    }
                    catch (...) {
                        return std::unexpected(sroa_error{
                            sroa_error::kind::invalid_field_index,
                            e.gep_inst_id,
                            "sroa.field_index attribute is non-numeric: " + fi_it->second
                        });
                    }

                    auto& rec = slot_map[e.frame_obj_id];
                    rec.frame_obj_id = e.frame_obj_id;
                    rec.gep_inst_ids.push_back(e.gep_inst_id);

                    // Assign a vreg for this field index if not yet seen.
                    bool found = false;
                    for (const auto& fs : rec.fields) {
                        if (fs.field_index == fi_idx) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        // Next vreg id: scan all existing vregs in the function for max.
                        // We do this once per slot (amortised by outer loop).
                        std::uint32_t max_vreg = 0;
                        for (const auto& blk2 : fn.function.blocks) {
                            for (const auto& i2 : blk2.instructions) {
                                for (const auto& d : i2.defs) {
                                    if (d.type == allocated_operand::kind::preg)
                                        max_vreg = std::max(max_vreg,
                                                            static_cast<std::uint32_t>(
                                                                std::get<preg>(d.value).id));
                                }
                            }
                        }
                        // Also check existing assigned field vregs in this record.
                        for (const auto& fs : rec.fields)
                            max_vreg = std::max(max_vreg, fs.scalar_vreg);

                        rec.fields.push_back({fi_idx, max_vreg + 1u});
                    }
                }

                // Move eligible (non-escaped) slots to analysis.eligible_slots.
                for (auto& [fid, rec] : slot_map) {
                    if (escaped_frame_objs.contains(fid)) {
                        rec.escaped = true;
                        ++analysis.escaped_count;
                    }
                    else {
                        analysis.eligible_slots.push_back(std::move(rec));
                    }
                }

                return analysis;
            };

            const auto analysis_result = validate();
            if (!analysis_result) {
                out.diagnostics.push_back(
                    "sroa_pass: analysis failed — " + analysis_result.error().message);
                return out;
            }

            const auto& analysis = *analysis_result;
            if (analysis.eligible_slots.empty()) {
                return out; // nothing eligible after escape analysis
            }

            // -----------------------------------------------------------------
            // Phase 3: rewrite
            //
            // For each block, process instructions in order:
            //   GEP for eligible slot        → tombstone (removed in final sweep)
            //   load  dst ← gep_preg_use     → mov  dst ← field_vreg
            //   store val → gep_preg_use     → mov  field_vreg ← val
            //
            // "gep_preg_use" means: a use operand whose preg id maps to a known
            // eligible GEP def-preg.
            // -----------------------------------------------------------------

            // Build fast lookup: gep_def_preg_id → (field_vreg, frame_obj_id)
            struct preg_resolution {
                std::uint32_t field_vreg = 0;
                std::uint32_t frame_obj_id = 0;
                std::uint32_t field_index = 0;
            };
            std::unordered_map<std::uint32_t, preg_resolution> gep_preg_map;
            std::unordered_set<std::uint32_t> eligible_gep_inst_ids;

            for (const auto& slot : analysis.eligible_slots) {
                for (std::size_t i = 0; i < gep_count; ++i) {
                    const auto& e = gep_scratch[i];
                    if (e.frame_obj_id != slot.frame_obj_id) continue;
                    const std::uint32_t fv = slot.vreg_for_field(e.field_index);
                    if (fv == 0) continue;
                    gep_preg_map[e.gep_def_preg] = {fv, slot.frame_obj_id, e.field_index};
                    eligible_gep_inst_ids.insert(e.gep_inst_id);
                }
            }

            std::size_t removed = 0;
            for (auto& blk : out.function.function.blocks) {
                std::vector<allocated_instruction> rewritten;
                rewritten.reserve(blk.instructions.size());

                for (auto& inst : blk.instructions) {
                    // Remove GEP instructions for eligible slots.
                    if (inst.op == opcode::get_element_ptr &&
                        eligible_gep_inst_ids.contains(inst.id)) {
                        ++removed;
                        continue; // tombstone
                    }

                    // Rewrite load: load dst ← (preg that is a GEP address)
                    if (inst.op == opcode::load &&
                        !inst.uses.empty() &&
                        inst.uses[0].type == allocated_operand::kind::preg) {
                        const auto use_id = std::get<preg>(inst.uses[0].value).id;
                        if (const auto it = gep_preg_map.find(use_id);
                            it != gep_preg_map.end()) {
                            // Replace load with mov: dst ← field_vreg.
                            allocated_instruction mov_inst;
                            mov_inst.id = inst.id;
                            mov_inst.op = opcode::mov;
                            mov_inst.defs = inst.defs;
                            mov_inst.uses = {
                                allocated_operand::as_preg(
                                    preg{
                                        static_cast<std::uint16_t>(it->second.field_vreg),
                                        "sroa." + std::to_string(it->second.field_index)
                                    })
                            };
                            mov_inst.comment = "sroa: scalar load";
                            rewritten.push_back(std::move(mov_inst));
                            ++removed; // one fewer memory op
                            continue;
                        }
                    }

                    // Rewrite store: store val → (preg that is a GEP address)
                    if (inst.op == opcode::store &&
                        inst.uses.size() >= 2 &&
                        inst.uses[1].type == allocated_operand::kind::preg) {
                        // Convention: uses[0] = value to store, uses[1] = address.
                        const auto addr_id = std::get<preg>(inst.uses[1].value).id;
                        if (const auto it = gep_preg_map.find(addr_id);
                            it != gep_preg_map.end()) {
                            // Replace store with mov: field_vreg ← val.
                            allocated_instruction mov_inst;
                            mov_inst.id = inst.id;
                            mov_inst.op = opcode::mov;
                            mov_inst.defs = {
                                allocated_operand::as_preg(
                                    preg{
                                        static_cast<std::uint16_t>(it->second.field_vreg),
                                        "sroa." + std::to_string(it->second.field_index)
                                    })
                            };
                            mov_inst.uses = {inst.uses[0]}; // the value operand
                            mov_inst.comment = "sroa: scalar store";
                            rewritten.push_back(std::move(mov_inst));
                            ++removed; // one fewer memory op
                            continue;
                        }
                    }

                    rewritten.push_back(std::move(inst));
                }

                blk.instructions = std::move(rewritten);
            }

            if (removed == 0) {
                return out; // conservative: nothing actually changed
            }

            out.removed_instructions = removed;
            out.changed = true;
            ctx.changed = true;
            ctx.statistics.total_removed_instructions += removed;
            ctx.statistics.removed_instructions_by_pass["sroa_pass"] += removed;

            // Re-verify the function after rewriting (mirrors other passes).
            if (ctx.verify_after_each_pass) {
                const auto vr = verify_physical_mir(out.function);
                out.function.verified = vr.ok();
                out.function.verification_diagnostics = vr.diagnostics;
                if (!vr.ok()) {
                    for (const auto& d : vr.diagnostics)
                        out.diagnostics.push_back("sroa_pass: post-rewrite verification: " + d);
                }
            }

            ctx.arena.reset();
            return out;
        }
    };

    struct peephole_mir_pass {
        peephole_options options{};

        [[nodiscard]] mir_pass_result run(const mir::physical_mir_function& fn, mir_pass_context& context) const {
            const auto peephole = run_mir_peephole(fn, options);

            mir_pass_result out;
            out.function = peephole.function;
            out.removed_instructions = peephole.removed_instructions;
            out.removed_blocks = peephole.removed_blocks;
            out.diagnostics = peephole.diagnostics;
            out.changed = peephole.changed;

            if (out.changed) {
                context.changed = true;
            }

            return out;
        }
    };

    struct codegen_result {
        mir::virtual_mir_function virtual_mir;
        register_allocation allocation;
        mir::allocated_mir_function allocated_mir;
        mir::physical_mir_function physical_mir;
        mir_pipeline_result optimization_result;
        // Flat accessors kept for compatibility — populated from optimization_result.
        [[nodiscard]] const mir_pass_statistics& mir_optimization_statistics() const {
            return optimization_result.statistics;
        }

        [[nodiscard]] const std::vector<std::string>& executed_mir_passes() const {
            return optimization_result.statistics.executed_pass_names;
        }

        [[nodiscard]] std::size_t total_removed_instructions() const {
            return optimization_result.statistics.total_removed_instructions;
        }

        [[nodiscard]] std::size_t total_removed_blocks() const {
            return optimization_result.statistics.total_removed_blocks;
        }

        [[nodiscard]] std::size_t total_changed_passes() const {
            return optimization_result.statistics.changed_passes;
        }

        [[nodiscard]] std::size_t total_added_instructions() const {
            return optimization_result.statistics.total_added_instructions;
        }

        [[nodiscard]] std::size_t total_added_blocks() const {
            return optimization_result.statistics.total_added_blocks;
        }

        [[nodiscard]] std::size_t total_changed_instructions() const {
            return optimization_result.statistics.total_changed_instructions;
        }

        [[nodiscard]] std::size_t total_changed_blocks() const {
            return optimization_result.statistics.total_changed_blocks;
        }

        [[nodiscard]] std::size_t total_unchanged_instructions() const {
            return optimization_result.statistics.total_unchanged_instructions;
        }

        [[nodiscard]] std::size_t total_unchanged_blocks() const {
            return optimization_result.statistics.total_unchanged_blocks;
        }

        [[nodiscard]] std::size_t total_unchanged_passes() const {
            return optimization_result.statistics.unchanged_passes;
        }

        stack_frame_layout frame;
        prologue_plan prologue;
        epilogue_plan epilogue;
        std::optional<peephole_result> peephole;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    // -----------------------------------------------------------------------
    // Monadic codegen result types
    //
    // codegen_error      — structured error payload for a failed compilation.
    // codegen_expected   — std::expected<codegen_result, codegen_error>.
    //
    // compile_to_physical_mir_expected() wraps the existing function into the
    // monadic type: a successful compilation yields the codegen_result value;
    // any failure (diagnostics present after compilation) yields codegen_error.
    //
    // Callers that need monadic chaining use compile_to_physical_mir_expected;
    // the original compile_to_physical_mir is preserved for backward compat.
    // -----------------------------------------------------------------------
    struct codegen_error {
        // All diagnostics collected during the failed compilation.
        std::vector<std::string> messages;

        // The stage at which compilation halted (informational).
        std::string failed_stage;

        [[nodiscard]] static codegen_error from_result(const codegen_result& r,
                                                       std::string stage = {}) {
            codegen_error e;
            e.messages = r.diagnostics;
            e.failed_stage = std::move(stage);
            return e;
        }
    };

    // The canonical expected type for a codegen operation.
    using codegen_expected = std::expected<codegen_result, codegen_error>;

    // Convenience: wrap an existing compile_to_physical_mir call into
    // codegen_expected.  If diagnostics are present in the result, the value
    // is treated as a failure and converted to codegen_error.
    template <class Expr>
    [[nodiscard]] codegen_expected compile_to_physical_mir_expected(
        const Expr& expr,
        const codegen_options& options = {}
    ) {
        auto result = compile_to_physical_mir(expr, options);
        if (!result.ok()) {
            return std::unexpected(codegen_error::from_result(result, "compile_to_physical_mir"));
        }
        return result;
    }

    [[nodiscard]] inline peephole_mir_pass make_peephole_mir_pass(const peephole_options& options = {});

    [[nodiscard]] inline mir_pipeline_preset make_mir_pipeline_preset(
        mir_opt_level level,
        const peephole_options& options = {}
    );

    [[nodiscard]] inline mir_pass_pipeline make_mir_pipeline(
        mir_opt_level level,
        const peephole_options& options = {}
    );

    [[nodiscard]] inline mir_pipeline_result run_mir_pass_pipeline(
        const mir::physical_mir_function& fn,
        const mir_pass_pipeline& pipeline,
        mir_pass_context& context
    ) {
        return pipeline.run(fn, context);
    }

    [[nodiscard]] inline peephole_mir_pass make_peephole_mir_pass(const peephole_options& options) {
        peephole_mir_pass pass;
        pass.options = options;
        return pass;
    }

    [[nodiscard]] inline mir_pipeline_preset make_mir_pipeline_preset(
        const mir_opt_level level,
        const peephole_options& options
    ) {
        mir_pipeline_preset preset;

        switch (level) {
        case mir_opt_level::O0:
            preset.name = "none";
            preset.kind = mir_pipeline_preset_kind::none;
            break;
        case mir_opt_level::O1:
            preset.name = "conservative";
            preset.kind = mir_pipeline_preset_kind::conservative;
            preset.pipeline.add_pass("peephole_mir_pass", make_peephole_mir_pass(options));
            break;
        case mir_opt_level::O2:
            preset.name = "scalar-optimizer";
            preset.kind = mir_pipeline_preset_kind::conservative;
            preset.pipeline.add_pass("unreachable_block_elimination_pass", unreachable_block_elimination_pass{});
            preset.pipeline.add_pass("trivial_jump_threading_pass", trivial_jump_threading_pass{});
            preset.pipeline.add_pass("loop_invariant_code_motion_pass", loop_invariant_code_motion_pass{});
            preset.pipeline.add_pass("constant_propagation_pass", constant_propagation_pass{});
            preset.pipeline.add_pass("copy_propagation_pass", copy_propagation_pass{});
            preset.pipeline.add_pass("common_subexpression_elimination_pass",
                                     common_subexpression_elimination_pass{});
            preset.pipeline.add_pass("dead_def_elimination_pass", dead_def_elimination_pass{});
            preset.pipeline.add_pass("peephole_mir_pass", make_peephole_mir_pass(options));
            break;
        case mir_opt_level::Debug:
            preset.name = "debug";
            preset.kind = mir_pipeline_preset_kind::debug;
            preset.trace_enabled = true;
            preset.pipeline.add_pass(mir_pass_descriptor{
                .name = "trace_only_mir_pass",
                .run = [](const mir::physical_mir_function& fn, mir_pass_context&) {
                    mir_pass_result out;
                    out.function = fn;
                    out.diagnostics.push_back("mir debug: before trace_only_mir_pass\n" + dump_physical_mir(fn));
                    out.diagnostics.push_back("mir debug: after trace_only_mir_pass\n" + dump_physical_mir(fn));
                    out.changed = false;
                    return out;
                }
            });
            break;
        }

        return preset;
    }

    [[nodiscard]] inline mir_pass_pipeline make_mir_pipeline(
        const mir_opt_level level,
        const peephole_options& options
    ) {
        return make_mir_pipeline_preset(level, options).pipeline;
    }

    template <MachineCodeBackend Backend>
    [[nodiscard]] backend_result emit_function(
        Backend& backend,
        const codegen_result& result
    ) {
        return emit_function(backend, result, backend_state{});
    }

    template <MachineCodeBackend Backend>
    [[nodiscard]] backend_result emit_function(
        Backend& backend,
        const codegen_result& result,
        backend_state state
    ) {
        if (!result.ok()) {
            backend_result out = backend_result::fail("codegen pipeline failed", std::move(state));
            out.errors.clear();
            for (const auto& diag : result.diagnostics) {
                out.errors.push_back(backend_error{diag, std::nullopt, std::nullopt});
            }
            return out;
        }
        return emit_function(backend, result.physical_mir, std::move(state));
    }

    template <class Expr>
    [[nodiscard]] codegen_result compile_to_physical_mir(
        const Expr& expr,
        const codegen_options& options
    ) {
        codegen_result out;

        try {
            const bool has_signature = detail::has_codegen_signature(options.signature);
            target_abi effective_abi = options.abi;
            function_signature signature = options.signature;

            if (has_signature) {
                if (!detail::has_codegen_abi(effective_abi)) {
                    effective_abi = detail::default_abi_for_signature(signature);
                }
                effective_abi.convention = detail::resolved_calling_convention(signature);
                signature.abi = effective_abi;
                out.virtual_mir = build_virtual_mir(expr, signature);

                const auto lowered = lower_arguments_to_abi(out.virtual_mir, signature);
                out.virtual_mir = lowered.function;
                if (!lowered.metadata.ok()) {
                    out.diagnostics.insert(
                        out.diagnostics.end(),
                        lowered.metadata.diagnostics.begin(),
                        lowered.metadata.diagnostics.end()
                    );
                    return out;
                }
            }
            else {
                if (!detail::has_codegen_abi(effective_abi)) {
                    effective_abi = default_target_abi();
                }
                out.virtual_mir = build_virtual_mir(expr);
            }

            if (options.enable_register_pressure_analysis) {
                (void)compute_register_pressure(out.virtual_mir);
            }

            out.allocation = allocate_registers(out.virtual_mir);
            out.allocated_mir = apply_register_allocation(out.virtual_mir, out.allocation);

            auto populate_physical_mir_caches = [](mir::physical_mir_function& fn) {
                fn.instruction_ids.clear();
                fn.referenced_spill_slots.clear();
                fn.referenced_virtual_registers.clear();
                for (const auto& block : fn.function.blocks) {
                    for (const auto& inst : block.instructions) {
                        if (inst.id != 0) {
                            fn.instruction_ids.insert(inst.id);
                        }
                        for (const auto& def : inst.defs) {
                            if (def.type == allocated_operand::kind::spill) {
                                fn.referenced_spill_slots.insert(std::get<spill_slot>(def.value).id);
                            }
                        }
                        for (const auto& use : inst.uses) {
                            if (use.type == allocated_operand::kind::spill) {
                                fn.referenced_spill_slots.insert(std::get<spill_slot>(use.value).id);
                            }
                        }
                    }
                }
            };

            if (options.enable_spill_rewrite) {
                const auto rewritten = rewrite_spills(out.allocated_mir.function);
                out.physical_mir = mir::physical_mir_function{
                    rewritten.function,
                    rewritten.inserted_loads,
                    rewritten.inserted_stores,
                    rewritten.diagnostics
                };
                out.physical_mir.spill_rewritten = true;
                populate_physical_mir_caches(out.physical_mir);
                if (!rewritten.ok()) {
                    out.diagnostics.insert(
                        out.diagnostics.end(),
                        rewritten.diagnostics.begin(),
                        rewritten.diagnostics.end()
                    );
                    return out;
                }
            }
            else {
                out.physical_mir = mir::physical_mir_function{out.allocated_mir.function, 0, 0, {}};
                out.physical_mir.spill_rewritten = false;
                populate_physical_mir_caches(out.physical_mir);
            }

            const bool legacy_peephole_compat =
                options.enable_peephole && options.mir_optimization_level == mir_opt_level::O0
                && !options.use_custom_mir_pipeline;

            mir_pass_pipeline mir_pipeline;
            if (options.use_custom_mir_pipeline && options.custom_mir_pipeline) {
                mir_pipeline = *options.custom_mir_pipeline;
                if (options.enable_peephole) {
                    mir_pipeline.add_pass("peephole_mir_pass", make_peephole_mir_pass(options.peephole));
                }
            }
            else {
                mir_pipeline = make_mir_pipeline(
                    legacy_peephole_compat ? mir_opt_level::O1 : options.mir_optimization_level,
                    options.peephole
                );
            }
            if (options.customize_mir_pipeline) {
                options.customize_mir_pipeline(mir_pipeline);
            }

            if (!mir_pipeline.empty()) {
                mir_pass_context pass_context;
                pass_context.verify_after_each_pass = options.verify_after_each_mir_pass;

                auto pipeline_result = run_mir_pass_pipeline(out.physical_mir, mir_pipeline, pass_context);
                out.physical_mir = pipeline_result.function;
                out.optimization_result = std::move(pipeline_result);
                out.diagnostics.insert(
                    out.diagnostics.end(),
                    out.optimization_result.diagnostics.begin(),
                    out.optimization_result.diagnostics.end()
                );

                if (legacy_peephole_compat
                    && !options.customize_mir_pipeline
                    && out.optimization_result.statistics.executed_pass_names.size() == 1
                    && out.optimization_result.statistics.executed_pass_names.front() == "peephole_mir_pass") {
                    peephole_result compatibility_result;
                    compatibility_result.function = out.optimization_result.function;
                    compatibility_result.removed_instructions =
                        out.optimization_result.statistics.total_removed_instructions;
                    compatibility_result.removed_blocks =
                        out.optimization_result.statistics.total_removed_blocks;
                    compatibility_result.diagnostics = out.optimization_result.diagnostics;
                    compatibility_result.changed = out.optimization_result.changed;
                    out.peephole = std::move(compatibility_result);
                }
            }

            if (options.run_verification) {
                const auto verification = verify_physical_mir(out.physical_mir);
                out.physical_mir.verified = verification.ok();
                out.physical_mir.verification_diagnostics = verification.diagnostics;
                if (!verification.ok()) {
                    out.diagnostics.insert(
                        out.diagnostics.end(),
                        verification.diagnostics.begin(),
                        verification.diagnostics.end()
                    );
                    return out;
                }
            }

            if (has_signature) {
                out.physical_mir.signature = signature;
            }
            out.physical_mir.abi = effective_abi;

            if (options.enable_frame_layout) {
                out.frame = compute_stack_frame(out.physical_mir);
                out.prologue = plan_prologue(out.physical_mir, out.frame, effective_abi);
                out.epilogue = plan_epilogue(out.physical_mir, out.frame, effective_abi);
                out.physical_mir.frame_layout = out.frame;
                out.physical_mir.prologue = out.prologue;
                out.physical_mir.epilogue = out.epilogue;
            }
        }
        catch (const std::exception& ex) {
            out.diagnostics.push_back(std::string{"compile_to_physical_mir: "} + ex.what());
        }

        return out;
    }

    template <class Expr>
    [[nodiscard]] codegen_result compile_to_physical_mir(const Expr& expr) {
        return compile_to_physical_mir(expr, codegen_options{});
    }

    template <class Expr>
    [[nodiscard]] codegen_result compile_to_physical_mir(
        const Expr& expr,
        const function_signature& signature
    ) {
        codegen_options options;
        options.signature = signature;
        return compile_to_physical_mir(expr, options);
    }

    template <class Expr>
    [[nodiscard]] codegen_result compile_to_physical_mir(
        const Expr& expr,
        const function_signature& signature,
        const target_abi& abi
    ) {
        codegen_options options;
        options.signature = signature;
        options.abi = abi;
        return compile_to_physical_mir(expr, options);
    }

    template <class Expr, MachineCodeBackend Backend>
    [[nodiscard]] backend_result compile_and_emit(
        const Expr& expr,
        Backend& backend,
        const codegen_options& options = {}
    ) {
        const auto compiled = compile_to_physical_mir(expr, options);
        if (!compiled.ok()) {
            backend_result out = backend_result::fail("codegen pipeline failed");
            out.errors.clear();
            for (const auto& diag : compiled.diagnostics) {
                out.errors.push_back(backend_error{diag, std::nullopt, std::nullopt});
            }
            return out;
        }
        return emit_function(backend, compiled.physical_mir);
    }

    template <class Expr, MachineCodeBackend Backend>
    [[nodiscard]] backend_result compile_and_emit(
        const Expr& expr,
        Backend& backend,
        const function_signature& signature
    ) {
        return emit_function(backend, compile_to_physical_mir(expr, signature));
    }

    template <class Expr, MachineCodeBackend Backend>
    [[nodiscard]] backend_result compile_and_emit(
        const Expr& expr,
        Backend& backend,
        const function_signature& signature,
        const target_abi& abi
    ) {
        return emit_function(backend, compile_to_physical_mir(expr, signature, abi));
    }

    [[nodiscard]] inline mir_pass_pipeline make_noop_mir_pipeline() {
        return {};
    }

    [[nodiscard]] inline mir_pass_pipeline make_debug_mir_pipeline(const peephole_options& options = {}) {
        mir_pass_pipeline pipeline;
        pipeline.add_pass("peephole_mir_pass", make_peephole_mir_pass(options));
        return pipeline;
    }

    [[nodiscard]] inline mir_pass_pipeline make_conservative_mir_pipeline(const peephole_options& options = {}) {
        mir_pass_pipeline pipeline;
        pipeline.add_pass("trivial_jump_threading_pass", trivial_jump_threading_pass{});
        pipeline.add_pass("empty_block_merge_pass", empty_block_merge_pass{});
        pipeline.add_pass("unreachable_block_elimination_pass", unreachable_block_elimination_pass{});
        pipeline.add_pass("peephole_mir_pass", make_peephole_mir_pass(options));
        return pipeline;
    }

    [[nodiscard]] inline mir_pass_pipeline make_aggressive_mir_pipeline(const peephole_options& options = {}) {
        mir_pass_pipeline pipeline;
        pipeline.add_pass("trivial_jump_threading_pass", trivial_jump_threading_pass{});
        pipeline.add_pass("empty_block_merge_pass", empty_block_merge_pass{});
        pipeline.add_pass("unreachable_block_elimination_pass", unreachable_block_elimination_pass{});
        pipeline.add_pass("constant_propagation_pass", constant_propagation_pass{});
        pipeline.add_pass("copy_propagation_pass", copy_propagation_pass{});
        pipeline.add_pass("common_subexpression_elimination_pass", common_subexpression_elimination_pass{});
        pipeline.add_pass("dead_def_elimination_pass", dead_def_elimination_pass{});
        pipeline.add_pass("peephole_mir_pass", make_peephole_mir_pass(options));
        pipeline.add_pass("unreachable_block_elimination_pass_2", unreachable_block_elimination_pass{});
        pipeline.add_pass("peephole_mir_pass_2", make_peephole_mir_pass(options));
        return pipeline;
    }

    [[nodiscard]] inline mir_pass_pipeline make_mir_pipeline_from_opt_level(
        const mir_opt_level level,
        const peephole_options& options = {}
    ) {
        return make_mir_pipeline(level, options);
    }

    // -----------------------------------------------------------------------
    // MIR legality framework
    //
    // A backend-agnostic rule set that validates MIR before emission.
    // No legalization transforms are performed — validation only.
    // -----------------------------------------------------------------------

    // Categories of legality rules.
    enum class mir_legality_rule_kind : std::uint8_t {
        opcode_support, // the instruction's opcode is (un)supported
        operand_form, // the operand combination is illegal for this backend
        branch_legality, // a branch shape violates backend constraints
        calling_convention // a call-site doesn't match the required ABI
    };

    // One legality constraint.  Backends build a vector of these and pass it
    // via mir_legalization_context.
    struct mir_legality_rule {
        // Short identifier used in violation messages.
        std::string name;
        // Human-readable description of what this rule checks.
        std::string description;
        // Which category this rule belongs to.
        mir_legality_rule_kind kind = mir_legality_rule_kind::opcode_support;
        // Predicate: returns true iff the instruction violates this rule.
        // Receives the instruction and the block it lives in.
        std::function<bool(const allocated_instruction &,
                      const allocated_basic_block&
        )
        >
        violates;
        // Message template emitted when a violation is found.
        // The checker appends " in bb<id> inst <id>" automatically.
        std::string violation_message;
    };

    // Per-violation record produced by check_mir_legality().
    struct mir_legality_violation {
        std::string rule_name;
        std::string message;
        mir_legality_rule_kind kind = mir_legality_rule_kind::opcode_support;
        std::uint32_t block_id = 0;
        std::uint32_t instruction_id = 0;
    };

    // Input context for a legality check: the rules to apply, an optional
    // backend identifier for diagnostics, and control knobs.
    struct mir_legalization_context {
        // Rules to enforce.  Order does not affect correctness.
        std::vector<mir_legality_rule> rules;
        // Name used in violation messages.
        std::string backend_name = "unknown";
        // When true, stop after the first violation per rule (faster).
        bool stop_at_first_violation = false;
    };

    // Result of check_mir_legality(): a flat list of violations.
    struct mir_legalization_result {
        std::vector<mir_legality_violation> violations;
        // Convenience diagnostics strings suitable for printing.
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return violations.empty(); }

        [[nodiscard]] std::size_t violation_count() const noexcept {
            return violations.size();
        }

        // Violations of a specific kind.
        [[nodiscard]] std::vector<mir_legality_violation>
        violations_of_kind(const mir_legality_rule_kind k) const {
            std::vector<mir_legality_violation> out;
            for (const auto& v : violations)
                if (v.kind == k) out.push_back(v);
            return out;
        }
    };

    // Run all rules in ctx against every instruction in fn.
    // Returns a result containing all violations found.
    [[nodiscard]] inline mir_legalization_result check_mir_legality(
        const mir::physical_mir_function& fn,
        const mir_legalization_context& ctx
    ) {
        mir_legalization_result out;
        if (ctx.rules.empty()) return out;

        for (const auto& rule : ctx.rules) {
            if (!rule.violates) continue; // rule with no predicate is a no-op
            bool found_violation = false;
            for (const auto& blk : fn.function.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (!rule.violates(inst, blk)) continue;

                    mir_legality_violation v;
                    v.rule_name = rule.name;
                    v.kind = rule.kind;
                    v.block_id = blk.id;
                    v.instruction_id = inst.id;
                    v.message = ctx.backend_name + ": " + rule.violation_message
                        + " in bb" + std::to_string(blk.id)
                        + " inst " + std::to_string(inst.id);
                    out.violations.push_back(v);
                    out.diagnostics.push_back(v.message);

                    if (ctx.stop_at_first_violation) {
                        found_violation = true;
                        break;
                    }
                }
                if (found_violation && ctx.stop_at_first_violation) break;
            }
        }

        return out;
    }

    // Convenience: build an opcode-allowlist rule.
    // The predicate fires when the instruction's opcode is NOT in the allow set.
    [[nodiscard]] inline mir_legality_rule make_opcode_allowlist_rule(
        std::string name,
        std::unordered_set<opcode> allowed_opcodes,
        std::string violation_message = "unsupported opcode"
    ) {
        mir_legality_rule rule;
        rule.name = std::move(name);
        rule.description = "opcode must be in the backend's supported set";
        rule.kind = mir_legality_rule_kind::opcode_support;
        rule.violation_message = std::move(violation_message);
        rule.violates = [allowed = std::move(allowed_opcodes)]
        (const allocated_instruction& inst, const allocated_basic_block&) {
                return !allowed.contains(inst.op);
            };
        return rule;
    }

    // Convenience: build a branch-shape rule that rejects branch_cond.
    [[nodiscard]] inline mir_legality_rule make_no_conditional_branch_rule(
        std::string name = "no_branch_cond"
    ) {
        mir_legality_rule rule;
        rule.name = std::move(name);
        rule.description = "conditional branches are not permitted";
        rule.kind = mir_legality_rule_kind::branch_legality;
        rule.violation_message = "conditional branch (branch_cond) not supported";
        rule.violates = [](const allocated_instruction& inst,
                           const allocated_basic_block&) {
            return inst.op == opcode::branch_cond;
        };
        return rule;
    }

    // -----------------------------------------------------------------------
    // Backend lowering hooks
    //
    // pre_emit_lowering_pass and backend_lowering_context allow backends to
    // register lowering callbacks that run after the MIR optimization pipeline
    // but before emission.  Infrastructure only — no ISA-specific lowering.
    // -----------------------------------------------------------------------

    // Context carried through every pre-emit lowering callback.
    struct backend_lowering_context {
        // Backend identity for diagnostics.
        std::string backend_name = "unknown";
        // When true, verify_physical_mir() is called after each lowering step.
        bool verify_after_each_step = true;
        // Diagnostic messages accumulated during lowering.
        std::vector<std::string> diagnostics;
        // Arbitrary key-value annotations a backend may attach (e.g. ABI name,
        // target triple, feature flags as strings).
        std::unordered_map<std::string, std::string> annotations;

        void annotate(std::string key, std::string value) {
            annotations.insert_or_assign(std::move(key), std::move(value));
        }

        [[nodiscard]] std::optional<std::string> annotation(const std::string_view key) const {
            const auto it = annotations.find(std::string{key});
            if (it == annotations.end()) return std::nullopt;
            return it->second;
        }
    };

    // Result of running a pre_emit_lowering_pass (or the full lowering chain).
    struct backend_lowering_result {
        mir::physical_mir_function function;
        std::vector<std::string> diagnostics;
        bool changed = false;

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // A single pre-emit lowering step.  Backends create instances with a
    // callback that transforms the MIR and returns the modified function.
    // If the callback returns an unchanged function (by value), `changed`
    // is set to false in the result.
    struct pre_emit_lowering_pass {
        // Human-readable name for diagnostics and tracing.
        std::string name = "unnamed_lowering_pass";

        // The lowering callback.  Receives the current function and a mutable
        // context.  Must return a valid physical_mir_function.
        // Returning the input unchanged is legal and sets changed=false.
        using callback_type = std::function<
            mir::physical_mir_function(mir::physical_mir_function,
                                       backend_lowering_context &)>;
        callback_type lower;

        // Run the lowering step and optionally verify the result.
        [[nodiscard]] backend_lowering_result run(
            const mir::physical_mir_function& fn,
            backend_lowering_context& ctx
        ) const {
            backend_lowering_result out;
            out.function = fn;

            if (!lower) {
                // No-op pass (useful as a placeholder).
                return out;
            }

            mir::physical_mir_function result;
            try {
                result = lower(fn, ctx);
            }
            catch (const std::exception& ex) {
                ctx.diagnostics.push_back(
                    name + ": lowering callback threw: " + ex.what());
                out.diagnostics = ctx.diagnostics;
                return out;
            }
            catch (...) {
                ctx.diagnostics.push_back(
                    name + ": lowering callback threw unknown exception");
                out.diagnostics = ctx.diagnostics;
                return out;
            }

            // Detect changes by comparing instruction counts (fast proxy).
            const auto count_insts = [](const mir::physical_mir_function& f) {
                std::size_t n = 0;
                for (const auto& b : f.function.blocks) n += b.instructions.size();
                return n;
            };
            out.changed = count_insts(result) != count_insts(fn)
                || result.function.blocks.size() != fn.function.blocks.size();
            out.function = std::move(result);

            if (ctx.verify_after_each_step) {
                const auto vr = verify_physical_mir(out.function);
                if (!vr.ok()) {
                    for (const auto& d : vr.diagnostics)
                        ctx.diagnostics.push_back(name + ": verification failed: " + d);
                    out.diagnostics.insert(out.diagnostics.end(),
                                           ctx.diagnostics.begin(), ctx.diagnostics.end());
                    // Revert to the original on verification failure.
                    out.function = fn;
                    out.changed = false;
                }
            }

            return out;
        }
    };

    // Run a sequence of pre_emit_lowering_pass steps in order.
    // Each step receives the output of the previous one.
    // Stops on the first step that produces a verification failure.
    [[nodiscard]] inline backend_lowering_result run_pre_emit_lowering(
        const mir::physical_mir_function& fn,
        std::vector<pre_emit_lowering_pass> passes,
        backend_lowering_context& ctx
    ) {
        backend_lowering_result out;
        out.function = fn;

        for (auto& pass : passes) {
            const auto step = pass.run(out.function, ctx);
            if (!step.ok()) {
                // Verification failed inside the step; diagnostics already in ctx.
                out.diagnostics = ctx.diagnostics;
                return out;
            }
            if (step.changed) {
                out.function = step.function;
                out.changed = true;
            }
        }

        out.diagnostics = ctx.diagnostics;
        return out;
    }

    [[nodiscard]] inline std::string dump_mir_optimization_summary(const codegen_result& result) {
        const auto& opt = result.optimization_result;
        const auto& stats = opt.statistics;

        std::string out;
        out.reserve(256);

        // Pipeline identity
        out += "mir-optimization-summary\n";
        if (!stats.executed_pass_names.empty()) {
            out += "  passes=";
            for (std::size_t i = 0; i < stats.executed_pass_names.size(); ++i) {
                if (i > 0) out += ", ";
                out += stats.executed_pass_names[i];
            }
            out += "\n";
        }
        else {
            out += "  passes=(none)\n";
        }

        out += "  executed=" + std::to_string(stats.executed_passes);
        out += " changed=" + std::to_string(stats.changed_passes);
        out += " unchanged=" + std::to_string(stats.unchanged_passes) + "\n";

        out += "  removed_instructions=" + std::to_string(stats.total_removed_instructions);
        out += " removed_blocks=" + std::to_string(stats.total_removed_blocks) + "\n";

        out += "  diagnostics=" + std::to_string(opt.diagnostics.size()) + "\n";

        return out;
    }

    // -----------------------------------------------------------------------
    // Implementations moved from lithe_codegen.hpp
    // -----------------------------------------------------------------------

    [[nodiscard]] inline value_flow_analysis_result analyze_def_use(const mir::physical_mir_function& fn) {
        value_flow_analysis_result out;

        // last_def[preg_id] -> most recent definition_site seen while walking in order
        std::unordered_map<std::uint32_t, definition_site> last_def;

        // uses_for_current_def[preg_id] -> uses accumulated since last def of preg_id
        std::unordered_map<std::uint32_t, std::vector<use_site>> uses_for_current_def;

        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                // Process uses first so a preg that is both used and defined in the
                // same instruction (e.g. a two-operand add) reads the incoming def.
                for (std::size_t idx = 0; idx < inst.uses.size(); ++idx) {
                    const auto& use_op = inst.uses[idx];
                    if (use_op.type != allocated_operand::kind::preg) continue;

                    const auto preg_id = std::get<preg>(use_op.value).id;
                    const use_site us{block.id, inst.id, idx};

                    use_def_chain udc;
                    udc.use = us;
                    if (const auto it = last_def.find(preg_id); it != last_def.end()) {
                        udc.reaching_definitions.push_back(it->second);
                        // Record this use for the current def's chain.
                        uses_for_current_def[preg_id].push_back(us);
                    }
                    // If no definition is known, reaching_definitions stays empty.
                    out.use_def_chains.push_back(std::move(udc));
                }

                // Process defs: each preg def starts (or restarts) a def_use_chain.
                for (std::size_t idx = 0; idx < inst.defs.size(); ++idx) {
                    const auto& def_op = inst.defs[idx];
                    if (def_op.type != allocated_operand::kind::preg) continue;

                    const auto preg_id = std::get<preg>(def_op.value).id;
                    const definition_site ds{block.id, inst.id, idx};

                    // Commit the completed chain for the previous def (if any).
                    if (last_def.contains(preg_id)) {
                        auto& old_chain = out.def_use_chains[preg_id];
                        old_chain.value_id = preg_id;
                        old_chain.definition = last_def[preg_id];
                        old_chain.uses = std::move(uses_for_current_def[preg_id]);
                        uses_for_current_def.erase(preg_id);
                    }

                    last_def[preg_id] = ds;
                }
            }
        }

        // Commit chains for all pregs whose last def was never superseded.
        for (const auto& [preg_id, ds] : last_def) {
            auto& chain = out.def_use_chains[preg_id];
            chain.value_id = preg_id;
            chain.definition = ds;
            chain.uses = std::move(uses_for_current_def[preg_id]);
        }

        return out;
    }

    [[nodiscard]] inline reaching_definitions_result compute_reaching_definitions(
        const mir::physical_mir_function& fn
    ) {
        reaching_definitions_result out;

        const auto cfg = analyze_cfg(fn);

        // Build a lookup: block_id -> block reference, for fast predecessor access.
        std::unordered_map<std::uint32_t, const allocated_basic_block*> block_by_id;
        for (const auto& block : fn.function.blocks) {
            block_by_id[block.id] = &block;
        }

        // Compute local gen map for each block: last definition per preg_id.
        std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, definition_site>> local_gen;
        for (const auto& block : fn.function.blocks) {
            auto& gen = local_gen[block.id];
            for (const auto& inst : block.instructions) {
                for (std::size_t idx = 0; idx < inst.defs.size(); ++idx) {
                    const auto& def_op = inst.defs[idx];
                    if (def_op.type != allocated_operand::kind::preg) continue;
                    const auto preg_id = std::get<preg>(def_op.value).id;
                    gen[preg_id] = definition_site{block.id, inst.id, idx};
                }
            }
        }

        // Initialise per-block state with empty in/out maps.
        for (const auto& block : fn.function.blocks) {
            out.per_block[block.id].block_id = block.id;
        }

        // Track ambiguity diagnostics already emitted to avoid duplicates across
        // fixed-point iterations (key = "preg_id@block_id").
        std::unordered_set<std::string> emitted_ambiguity_diags;

        // Iterative fixed-point: repeat until no out map changes.
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& block : fn.function.blocks) {
                auto& state = out.per_block[block.id];

                // in[block] = union of predecessor out maps, with ambiguity detection.
                std::unordered_map<std::uint32_t, definition_site> new_in;
                for (const auto pred_id : block.predecessors) {
                    const auto pred_it = out.per_block.find(pred_id);
                    if (pred_it == out.per_block.end()) continue;
                    for (const auto& [preg_id, ds] : pred_it->second.out) {
                        const auto existing = new_in.find(preg_id);
                        if (existing == new_in.end()) {
                            new_in[preg_id] = ds;
                        }
                        else if (existing->second.instruction_id != ds.instruction_id
                            || existing->second.block_id != ds.block_id) {
                            // Different definitions reach this join point: keep the
                            // lower instruction_id for determinism and record a diagnostic
                            // only once per (preg, block) pair.
                            if (ds.instruction_id < existing->second.instruction_id) {
                                existing->second = ds;
                            }
                            const auto diag_key = std::to_string(preg_id) + "@"
                                + std::to_string(block.id);
                            if (emitted_ambiguity_diags.insert(diag_key).second) {
                                out.diagnostics.push_back(
                                    "ambiguous reaching def for preg " + std::to_string(preg_id)
                                    + " at block bb" + std::to_string(block.id));
                            }
                        }
                    }
                }

                // out[block] = in overwritten by local gen.
                std::unordered_map<std::uint32_t, definition_site> new_out = new_in;
                for (const auto& [preg_id, ds] : local_gen[block.id]) {
                    new_out[preg_id] = ds;
                }

                if (new_in != state.in || new_out != state.out) {
                    state.in = std::move(new_in);
                    state.out = std::move(new_out);
                    changed = true;
                }
            }
        }

        // Fill per-instruction before/after maps by simulating the block in order.
        for (const auto& block : fn.function.blocks) {
            auto current = out.per_block[block.id].in;
            for (const auto& inst : block.instructions) {
                out.before_instruction[inst.id] = current;
                for (std::size_t idx = 0; idx < inst.defs.size(); ++idx) {
                    const auto& def_op = inst.defs[idx];
                    if (def_op.type != allocated_operand::kind::preg) continue;
                    const auto preg_id = std::get<preg>(def_op.value).id;
                    current[preg_id] = definition_site{block.id, inst.id, idx};
                }
                out.after_instruction[inst.id] = current;
            }
        }

        return out;
    }

    [[nodiscard]] inline cfg_analysis_result const& get_or_compute_cfg(
        mir_pass_context& ctx, const mir::physical_mir_function& fn
    ) {
        if (!ctx.analysis_cache.cfg.has_value()) {
            ctx.analysis_cache.cfg = analyze_cfg(fn);
        }
        return *ctx.analysis_cache.cfg;
    }

    [[nodiscard]] inline value_flow_analysis_result const& get_or_compute_def_use(
        mir_pass_context& ctx, const mir::physical_mir_function& fn
    ) {
        if (!ctx.analysis_cache.def_use.has_value()) {
            ctx.analysis_cache.def_use = analyze_def_use(fn);
        }
        return *ctx.analysis_cache.def_use;
    }

    [[nodiscard]] inline reaching_definitions_result const& get_or_compute_reaching_definitions(
        mir_pass_context& ctx, const mir::physical_mir_function& fn
    ) {
        if (!ctx.analysis_cache.reaching_definitions.has_value()) {
            ctx.analysis_cache.reaching_definitions = compute_reaching_definitions(fn);
        }
        return *ctx.analysis_cache.reaching_definitions;
    }

    [[nodiscard]] inline loop_analysis_result const& get_or_compute_loop(
        mir_pass_context& ctx, const mir::physical_mir_function& fn
    ) {
        if (!ctx.analysis_cache.loops.has_value()) {
            ctx.analysis_cache.loops = analyze_loops(fn);
        }
        return *ctx.analysis_cache.loops;
    }

    [[nodiscard]] inline dominator_analysis_result const& get_or_compute_dominators(
        mir_pass_context& ctx,
        mir::physical_mir_function const& fn,
        const dominator_analysis_options options
    ) {
        if (!ctx.analysis_cache.dominators.has_value() ||
            ctx.analysis_cache.cached_dominator_options != options) {
            ctx.analysis_cache.dominators = compute_dominators(fn, options);
            ctx.analysis_cache.cached_dominator_options = options;
        }
        return *ctx.analysis_cache.dominators;
    }

    [[nodiscard]] inline cfg_analysis_result analyze_cfg(const mir::physical_mir_function& fn) {
        cfg_analysis_result out;
        out.entry_block = fn.function.cfg.entry_block;

        auto normalize_ids = [](std::vector<std::uint32_t> ids) {
            std::ranges::sort(ids);
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            return ids;
        };

        std::vector<std::uint32_t> block_order;
        block_order.reserve(fn.function.blocks.size());
        std::unordered_set<std::uint32_t> seen_block_ids;

        for (const auto& block : fn.function.blocks) {
            block_order.push_back(block.id);
            if (!seen_block_ids.insert(block.id).second) {
                out.diagnostics.push_back("physical MIR has duplicate block id bb" + std::to_string(block.id));
            }
            out.block_info[block.id] = basic_block_info{block.id, {}, {}, false, false, false};
        }

        if (out.entry_block == 0 || !out.block_info.contains(out.entry_block)) {
            out.diagnostics.emplace_back("physical MIR entry block is missing");
        }

        for (const auto& block : fn.function.blocks) {
            std::vector<std::uint32_t> successors = block.successors;
            if (const auto it = fn.function.cfg.successors.find(block.id); it != fn.function.cfg.successors.end()) {
                successors.insert(successors.end(), it->second.begin(), it->second.end());
            }

            // Classify explicit branch instructions and collect their targets.
            bool has_explicit_branch = false;
            bool has_cond_branch = false;
            for (const auto& inst : block.instructions) {
                if (inst.op == opcode::branch_cond) {
                    has_cond_branch = true;
                    has_explicit_branch = true;
                }
                else if (inst.op == opcode::branch) {
                    has_explicit_branch = true;
                }
                if (inst.op != opcode::branch && inst.op != opcode::branch_cond) {
                    continue;
                }
                for (const auto& use : inst.uses) {
                    if (use.type == allocated_operand::kind::block) {
                        successors.push_back(std::get<std::uint32_t>(use.value));
                    }
                }
            }

            successors = normalize_ids(std::move(successors));
            out.block_info[block.id].successors = successors;
            for (const auto succ : successors) {
                if (!out.block_info.contains(succ)) {
                    out.diagnostics.push_back(
                        "bb" + std::to_string(block.id) +
                        " references missing successor bb" + std::to_string(succ));
                    continue;
                }
                out.block_info[succ].predecessors.push_back(block.id);
                out.edges.push_back(control_flow_edge{block.id, succ});

                // Determine edge_kind heuristically from instruction shape.
                // async_fork / rpc_boundary / entanglement / sync_join edges must be
                // injected externally after CFG construction; default classification
                // uses only information available in the instruction stream.
                edge_kind ek;
                if (has_cond_branch) {
                    ek = edge_kind::sync_branch;
                }
                else if (!has_explicit_branch) {
                    ek = edge_kind::fallthrough;
                }
                else {
                    ek = edge_kind::sync_branch;
                }
                out.typed_edges.push_back(cfg_edge{block.id, succ, ek});
            }

            for (const auto pred : block.predecessors) {
                if (!out.block_info.contains(pred)) {
                    out.diagnostics.push_back(
                        "bb" + std::to_string(block.id) +
                        " references missing predecessor bb" + std::to_string(pred));
                }
            }
        }

        for (auto& [_, info] : out.block_info) {
            (void)_;
            info.predecessors = normalize_ids(std::move(info.predecessors));
            info.successors = normalize_ids(std::move(info.successors));
        }

        std::unordered_set<std::uint32_t> reachable_set;
        if (out.entry_block != 0 && out.block_info.contains(out.entry_block)) {
            std::vector<std::uint32_t> stack = {out.entry_block};
            while (!stack.empty()) {
                const auto current = stack.back();
                stack.pop_back();
                if (!reachable_set.insert(current).second) {
                    continue;
                }
                for (const auto succ : out.block_info[current].successors) {
                    if (out.block_info.contains(succ)) {
                        stack.push_back(succ);
                    }
                }
            }
        }

        out.reachable_blocks.reserve(block_order.size());
        out.unreachable_blocks.reserve(block_order.size());
        for (const auto block_id : block_order) {
            const bool reachable = reachable_set.contains(block_id);
            auto& info = out.block_info[block_id];
            info.is_entry = block_id == out.entry_block;
            info.reachable = reachable;
            if (reachable) {
                out.reachable_blocks.push_back(block_id);
            }
            else {
                out.unreachable_blocks.push_back(block_id);
            }
        }

        out.exit_blocks.reserve(out.reachable_blocks.size());
        for (const auto block_id : out.reachable_blocks) {
            const auto& info = out.block_info.at(block_id);
            bool is_exit = info.successors.empty();
            if (!is_exit) {
                for (const auto& block : fn.function.blocks) {
                    if (block.id != block_id) { continue; }
                    if (!block.instructions.empty() && block.instructions.back().op == opcode::ret) {
                        is_exit = true;
                    }
                    break;
                }
            }
            if (is_exit) {
                out.exit_blocks.push_back(block_id);
                out.block_info[block_id].is_exit = true;
            }
        }
        out.exit_blocks = normalize_ids(std::move(out.exit_blocks));

        // Partition into execution domains if any non-sequential edge kinds exist.
        bool needs_partition = false;
        for (const auto& te : out.typed_edges) {
            if (te.kind == edge_kind::async_fork || te.kind == edge_kind::rpc_boundary) {
                needs_partition = true;
                break;
            }
        }
        if (needs_partition) {
            out.partition = partition_execution_domains(out);
        }

        return out;
    }

    // Partitions reachable blocks into execution domains.
    //
    // Algorithm: iterative DFS from entry_block treating each async_fork or
    // rpc_boundary edge as a domain boundary.  When such an edge is crossed the
    // target block becomes the root of a new domain; the DFS recurses into it
    // without re-entering the parent domain.  sync_join edges are traversed
    // normally (both endpoints stay in the caller's domain unless they were
    // already placed into a child domain by a prior fork).
    //
    // Complexity: O(V + E) where V = reachable block count, E = typed edge count.
    [[nodiscard]] inline subgraph_partition partition_execution_domains(
        cfg_analysis_result const& cfg) {
        subgraph_partition result;
        if (cfg.entry_block == 0 || !cfg.block_info.contains(cfg.entry_block)) {
            execution_domain root;
            root.domain_id = 0;
            root.root_block = cfg.entry_block;
            result.domains.push_back(std::move(root));
            return result;
        }

        // Build a typed adjacency map from typed_edges for O(1) lookup.
        std::unordered_map<std::uint32_t, std::vector<cfg_edge>> adj;
        adj.reserve(cfg.block_info.size());
        for (const auto& te : cfg.typed_edges) {
            adj[te.from].push_back(te);
        }

        std::uint32_t next_domain_id = 0;

        // Each stack frame: (block_id, domain_id_of_this_block).
        struct frame {
            std::uint32_t block;
            std::uint32_t dom;
        };
        std::vector<frame> stack;
        stack.push_back({cfg.entry_block, 0});

        // Pre-create domain 0.
        {
            execution_domain root;
            root.domain_id = 0;
            root.root_block = cfg.entry_block;
            result.domains.push_back(std::move(root));
        }

        std::unordered_set<std::uint32_t> visited;
        visited.insert(cfg.entry_block);
        result.block_to_domain[cfg.entry_block] = 0;
        result.domains[0].blocks.push_back(cfg.entry_block);

        while (!stack.empty()) {
            const auto [cur, cur_dom] = stack.back();
            stack.pop_back();

            const auto adj_it = adj.find(cur);
            if (adj_it == adj.end()) continue;

            for (const auto& edge : adj_it->second) {
                const std::uint32_t target = edge.to;
                if (!cfg.block_info.contains(target)) continue;

                const bool boundary =
                    edge.kind == edge_kind::async_fork ||
                    edge.kind == edge_kind::rpc_boundary;

                std::uint32_t target_dom;
                if (boundary) {
                    // Open a new domain rooted at target.
                    ++next_domain_id;
                    target_dom = next_domain_id;
                    execution_domain nd;
                    nd.domain_id = target_dom;
                    nd.root_block = target;
                    nd.spawned_by = edge.kind;
                    result.domains.push_back(std::move(nd));
                }
                else {
                    target_dom = cur_dom;
                }

                if (!visited.insert(target).second) continue;

                result.block_to_domain[target] = target_dom;
                result.domains[target_dom].blocks.push_back(target);
                stack.push_back({target, target_dom});
            }
        }

        // Ensure every reachable block has an entry; any block not reached via
        // typed_edges falls back to domain 0 (conservative).
        for (const auto bid : cfg.reachable_blocks) {
            if (!result.block_to_domain.contains(bid)) {
                result.block_to_domain[bid] = 0;
                result.domains[0].blocks.push_back(bid);
            }
        }

        return result;
    }

    [[nodiscard]] inline std::vector<std::uint32_t> compute_reachable_blocks(const cfg_analysis_result& cfg) {
        std::vector<std::uint32_t> out;
        if (cfg.entry_block == 0 || !cfg.block_info.contains(cfg.entry_block)) {
            return out;
        }

        std::unordered_set<std::uint32_t> seen;
        std::vector<std::uint32_t> stack = {cfg.entry_block};
        while (!stack.empty()) {
            const auto block_id = stack.back();
            stack.pop_back();
            if (!seen.insert(block_id).second) {
                continue;
            }
            out.push_back(block_id);
            for (const auto succ : cfg.block_info.at(block_id).successors) {
                if (cfg.block_info.contains(succ)) {
                    stack.push_back(succ);
                }
            }
        }

        std::ranges::sort(out);
        return out;
    }

    [[nodiscard]] inline std::vector<std::uint32_t> compute_exit_blocks(const cfg_analysis_result& cfg) {
        std::vector<std::uint32_t> out;
        const auto reachable = compute_reachable_blocks(cfg);
        out.reserve(reachable.size());
        for (const auto block_id : reachable) {
            const auto it = cfg.block_info.find(block_id);
            if (it != cfg.block_info.end() && it->second.successors.empty()) {
                out.push_back(block_id);
            }
        }

        std::ranges::sort(out);
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    [[nodiscard]] inline std::vector<std::uint32_t> reverse_postorder_block_order(const cfg_analysis_result& cfg) {
        std::vector<std::uint32_t> postorder;
        if (cfg.entry_block == 0 || !cfg.block_info.contains(cfg.entry_block)) {
            return postorder;
        }

        // Iterative postorder DFS using an explicit worklist.
        // Each entry is (block_id, successor_index): when successor_index equals
        // the successor count the block is fully explored and appended to postorder.
        std::unordered_set<std::uint32_t> visited;
        struct frame {
            std::uint32_t block_id;
            std::size_t succ_idx;
        };
        std::vector<frame> stack;
        stack.push_back({cfg.entry_block, 0});
        visited.insert(cfg.entry_block);

        while (!stack.empty()) {
            auto& top = stack.back();
            const auto& succs = cfg.block_info.at(top.block_id).successors;
            if (top.succ_idx < succs.size()) {
                const auto succ = succs[top.succ_idx++];
                if (cfg.block_info.contains(succ) && visited.insert(succ).second) {
                    stack.push_back({succ, 0});
                }
            }
            else {
                postorder.push_back(top.block_id);
                stack.pop_back();
            }
        }

        std::ranges::reverse(postorder);
        return postorder;
    }

    [[nodiscard]] inline std::vector<std::uint32_t> topological_block_order(const cfg_analysis_result& cfg) {
        std::vector<std::uint32_t> order;
        const auto reachable = compute_reachable_blocks(cfg);
        if (reachable.empty()) {
            return order;
        }

        std::unordered_set<std::uint32_t> reachable_set(reachable.begin(), reachable.end());
        std::unordered_map<std::uint32_t, std::size_t> indegree;
        indegree.reserve(reachable.size());
        for (const auto block_id : reachable) {
            indegree[block_id] = 0;
        }
        for (const auto block_id : reachable) {
            for (const auto succ : cfg.block_info.at(block_id).successors) {
                if (reachable_set.contains(succ)) {
                    ++indegree[succ];
                }
            }
        }

        std::vector<std::uint32_t> queue;
        queue.reserve(reachable.size());
        for (const auto block_id : reachable) {
            if (indegree[block_id] == 0) {
                queue.push_back(block_id);
            }
        }

        std::size_t cursor = 0;
        while (cursor < queue.size()) {
            const auto block_id = queue[cursor++];
            order.push_back(block_id);
            for (const auto succ : cfg.block_info.at(block_id).successors) {
                if (!reachable_set.contains(succ)) {
                    continue;
                }
                auto& deg = indegree[succ];
                if (deg > 0) {
                    --deg;
                    if (deg == 0) {
                        queue.push_back(succ);
                    }
                }
            }
        }

        if (order.size() != reachable.size()) {
            const auto fallback = reverse_postorder_block_order(cfg);
            std::unordered_set<std::uint32_t> appended(order.begin(), order.end());
            for (const auto block_id : fallback) {
                if (reachable_set.contains(block_id) && !appended.contains(block_id)) {
                    order.push_back(block_id);
                }
            }
        }

        return order;
    }

    [[nodiscard]] inline mir::verification_result validate_cfg(const mir::physical_mir_function& fn) {
        mir::verification_result out;
        const auto cfg = analyze_cfg(fn);
        out.diagnostics.insert(out.diagnostics.end(), cfg.diagnostics.begin(), cfg.diagnostics.end());

        std::unordered_set<std::uint32_t> block_ids;
        for (const auto& block : fn.function.blocks) {
            block_ids.insert(block.id);
        }

        auto is_terminator = [](const opcode op) {
            return op == opcode::ret || op == opcode::branch || op == opcode::branch_cond;
        };

        for (const auto& block : fn.function.blocks) {
            std::optional<std::size_t> first_terminator_index;
            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                const auto& inst = block.instructions[i];
                if (!is_terminator(inst.op)) {
                    continue;
                }

                if (!first_terminator_index.has_value()) {
                    first_terminator_index = i;
                }

                if (inst.op == opcode::branch || inst.op == opcode::branch_cond) {
                    bool has_target = false;
                    for (const auto& use : inst.uses) {
                        if (use.type != allocated_operand::kind::block) {
                            continue;
                        }
                        has_target = true;
                        const auto target = std::get<std::uint32_t>(use.value);
                        if (!block_ids.contains(target)) {
                            out.diagnostics.push_back(
                                "invalid branch target bb" + std::to_string(target) +
                                " in i" + std::to_string(inst.id) + " (bb" + std::to_string(block.id) + ")"
                            );
                        }
                    }
                    if (!has_target) {
                        out.diagnostics.push_back(
                            "missing branch target in i" + std::to_string(inst.id) +
                            " (bb" + std::to_string(block.id) + ")"
                        );
                    }
                }
            }

            if (first_terminator_index.has_value() && *first_terminator_index + 1 != block.instructions.size()) {
                out.diagnostics.push_back("bb" + std::to_string(block.id) + " has non-final terminator");
            }
        }

        return out;
    }

    [[nodiscard]] inline mir::verification_result validate_branch_targets(const mir::physical_mir_function& fn) {
        mir::verification_result out;

        const auto cfg = analyze_cfg(fn);
        std::unordered_set<std::uint32_t> block_ids;
        block_ids.reserve(cfg.block_info.size());
        for (const auto& [block_id, _] : cfg.block_info) {
            (void)_;
            block_ids.insert(block_id);
        }

        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op != opcode::branch && inst.op != opcode::branch_cond) {
                    continue;
                }

                bool has_target = false;
                for (const auto& use : inst.uses) {
                    if (use.type != allocated_operand::kind::block) {
                        continue;
                    }
                    has_target = true;
                    const auto target = std::get<std::uint32_t>(use.value);
                    if (!block_ids.contains(target)) {
                        out.diagnostics.push_back(
                            "invalid branch target bb" + std::to_string(target) +
                            " in i" + std::to_string(inst.id) + " (bb" + std::to_string(block.id) + ")"
                        );
                    }
                }

                if (!has_target) {
                    out.diagnostics.push_back(
                        "missing branch target in i" + std::to_string(inst.id) +
                        " (bb" + std::to_string(block.id) + ")"
                    );
                }
            }
        }

        return out;
    }

    [[nodiscard]] inline std::string dump_dominator_analysis(
        dominator_analysis_result const& result
    ) {
        std::string out;
        out.reserve(256);

        out += "dominator-analysis entry=" + std::to_string(result.dom.entry) + "\n";

        if (!result.diagnostics.empty()) {
            out += "  diagnostics=" + std::to_string(result.diagnostics.size()) + "\n";
        }

        // Collect block ids in a stable order (sorted).
        std::vector<std::uint32_t> block_ids;
        block_ids.reserve(result.dom.immediate_dominator.size());
        for (const auto& [id, _] : result.dom.immediate_dominator)
            block_ids.push_back(id);
        std::ranges::sort(block_ids);

        for (const auto id : block_ids) {
            out += "  bb" + std::to_string(id);

            // Immediate dominator
            const auto idom_it = result.dom.immediate_dominator.find(id);
            if (idom_it != result.dom.immediate_dominator.end() && idom_it->second.has_value()) {
                out += " idom=bb" + std::to_string(idom_it->second.value());
            }
            else {
                out += " idom=none";
            }

            // Dominated children
            const auto ch_it = result.dom.dominated_children.find(id);
            if (ch_it != result.dom.dominated_children.end() && !ch_it->second.empty()) {
                std::vector<std::uint32_t> children(ch_it->second.begin(), ch_it->second.end());
                std::ranges::sort(children);
                out += " children=[";
                for (std::size_t i = 0; i < children.size(); ++i) {
                    if (i > 0) out += ",";
                    out += "bb" + std::to_string(children[i]);
                }
                out += "]";
            }

            // Dominance frontier
            const auto df_it = result.dom.dominance_frontier.find(id);
            if (df_it != result.dom.dominance_frontier.end() && !df_it->second.empty()) {
                std::vector<std::uint32_t> frontier(df_it->second.begin(), df_it->second.end());
                std::ranges::sort(frontier);
                out += " df=[";
                for (std::size_t i = 0; i < frontier.size(); ++i) {
                    if (i > 0) out += ",";
                    out += "bb" + std::to_string(frontier[i]);
                }
                out += "]";
            }

            // Loop-header marker
            if (result.loop_header_blocks.contains(id))
                out += " [loop-header]";

            out += "\n";
        }

        return out;
    }

    [[nodiscard]] inline std::string dump_mir_pass_trace(const mir_pass_trace_log& log) {
        static constexpr auto kind_to_str = [](const mir_pass_trace_event_kind k) -> std::string_view {
            switch (k) {
            case mir_pass_trace_event_kind::pass_begin: return "pass_begin";
            case mir_pass_trace_event_kind::pass_end: return "pass_end";
            case mir_pass_trace_event_kind::verification_success: return "verification_success";
            case mir_pass_trace_event_kind::verification_failure: return "verification_failure";
            case mir_pass_trace_event_kind::analysis_invalidated: return "analysis_invalidated";
            case mir_pass_trace_event_kind::diagnostic: return "diagnostic";
            }
            return "unknown";
        };

        std::ostringstream os;
        os << "[mir-pass-trace events=" << log.events.size() << "]\n";
        for (const auto& ev : log.events) {
            os << "  [" << kind_to_str(ev.kind) << "] pass=" << ev.pass_name;
            if (ev.kind == mir_pass_trace_event_kind::pass_begin
                || ev.kind == mir_pass_trace_event_kind::pass_end
                || ev.kind == mir_pass_trace_event_kind::verification_failure) {
                os << " insn=" << ev.instruction_count_before << "->" << ev.instruction_count_after
                    << " blk=" << ev.block_count_before << "->" << ev.block_count_after;
            }
            if (!ev.message.empty()) {
                os << " msg=" << ev.message;
            }
            os << '\n';
        }
        return os.str();
    }

    [[nodiscard]] inline mir::verification_result verify_physical_mir(const mir::physical_mir_function& fn) {
        mir::verification_result out = verify_allocated_mir(mir::allocated_mir_function{fn.function});
        if (fn.metadata.current_phase != mir::phase::physical_mir) {
            out.diagnostics.emplace_back("physical MIR metadata phase mismatch");
        }

        if (fn.function.blocks.empty()) {
            out.diagnostics.emplace_back("physical MIR has no basic blocks");
            return out;
        }

        const auto cfg_validation = validate_cfg(fn);
        out.diagnostics.insert(out.diagnostics.end(), cfg_validation.diagnostics.begin(),
                               cfg_validation.diagnostics.end());

        if (has_duplicate_instruction_ids(fn)) {
            out.diagnostics.emplace_back("physical MIR has duplicate non-zero instruction ids");
        }


        auto spill_slot_valid = [](const spill_slot& slot) {
            return slot.id != 0 && slot.size > 0 && slot.alignment > 0;
        };

        for (const auto& slot : fn.function.spill_slots) {
            if (!spill_slot_valid(slot)) {
                out.diagnostics.push_back("physical MIR has invalid spill slot spill" + std::to_string(slot.id));
            }
        }

        auto is_terminator = [](const opcode op) {
            return op == opcode::ret || op == opcode::branch || op == opcode::branch_cond;
        };

        for (const auto& block : fn.function.blocks) {
            std::size_t terminator_count = 0;

            for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                if (is_terminator(block.instructions[i].op)) {
                    ++terminator_count;
                }
            }
            if (terminator_count > 1) {
                out.diagnostics.push_back("bb" + std::to_string(block.id) + " has multiple terminators");
            }

            for (const auto& inst : block.instructions) {
                const bool spill_opcode = inst.op == opcode::load_spill || inst.op == opcode::store_spill;

                if (inst.op == opcode::ret && !inst.defs.empty()) {
                    out.diagnostics.push_back("ret must not define values in i" + std::to_string(inst.id));
                }
                if (inst.op == opcode::branch) {
                    if (inst.uses.empty() || inst.uses[0].type != allocated_operand::kind::block) {
                        out.diagnostics.push_back("branch must have a block target in i" + std::to_string(inst.id));
                    }
                }
                if (inst.op == opcode::branch_cond) {
                    bool has_condition = false;
                    bool has_target = false;
                    for (const auto& use : inst.uses) {
                        if (use.type == allocated_operand::kind::block) {
                            has_target = true;
                        }
                        else {
                            has_condition = true;
                        }
                    }
                    if (!has_condition || !has_target) {
                        out.diagnostics.push_back(
                            "branch_cond must have condition and target in i" + std::to_string(inst.id));
                    }
                }

                auto check_preg = [&](const allocated_operand& op, const std::string_view pos) {
                    if (op.type != allocated_operand::kind::preg) {
                        return;
                    }
                    const auto& reg = std::get<preg>(op.value);
                    if (reg.name.empty()) {
                        out.diagnostics.push_back(
                            "physical MIR has invalid preg name in " + std::string(pos) +
                            " of i" + std::to_string(inst.id)
                        );
                    }
                };
                auto check_spill = [&](const allocated_operand& op, const std::string_view pos) {
                    if (op.type != allocated_operand::kind::spill) {
                        return;
                    }
                    const auto& slot = std::get<spill_slot>(op.value);
                    if (!spill_slot_valid(slot)) {
                        out.diagnostics.push_back(
                            "physical MIR has invalid spill slot in " + std::string(pos) +
                            " of i" + std::to_string(inst.id)
                        );
                    }
                };

                for (const auto& def : inst.defs) {
                    check_preg(def, "def");
                    check_spill(def, "def");
                    if (def.type == allocated_operand::kind::spill && !spill_opcode) {
                        out.diagnostics.push_back(
                            "physical MIR has unresolved spill def in i" + std::to_string(inst.id) + " (bb" +
                            std::to_string(block.id) + ")"
                        );
                    }
                }
                for (const auto& use : inst.uses) {
                    check_preg(use, "use");
                    check_spill(use, "use");
                    if (use.type == allocated_operand::kind::spill && !spill_opcode) {
                        out.diagnostics.push_back(
                            "physical MIR has unresolved spill use in i" + std::to_string(inst.id) + " (bb" +
                            std::to_string(block.id) + ")"
                        );
                    }
                }

                if (inst.op == opcode::load) {
                    if (inst.defs.size() != 1 || inst.uses.size() != 1) {
                        out.diagnostics.push_back(
                            "load must have exactly one def and one use in i" + std::to_string(inst.id)
                        );
                    }
                    else {
                        if (inst.defs[0].type != allocated_operand::kind::preg) {
                            out.diagnostics.push_back("load def must be preg in i" + std::to_string(inst.id));
                        }
                        if (inst.uses[0].type != allocated_operand::kind::memory) {
                            out.diagnostics.push_back("load use must be memory operand in i" + std::to_string(inst.id));
                        }
                    }
                }

                if (inst.op == opcode::store) {
                    if (inst.defs.size() != 1 || inst.uses.size() != 1) {
                        out.diagnostics.push_back(
                            "store must have exactly one def and one use in i" + std::to_string(inst.id)
                        );
                    }
                    else {
                        if (inst.defs[0].type != allocated_operand::kind::memory) {
                            out.diagnostics.push_back(
                                "store def must be memory operand in i" + std::to_string(inst.id)
                            );
                        }
                        if (inst.uses[0].type != allocated_operand::kind::preg) {
                            out.diagnostics.push_back("store use must be preg in i" + std::to_string(inst.id));
                        }
                    }
                }

                if (inst.op == opcode::load_spill) {
                    if (inst.defs.size() != 1 || inst.uses.size() != 1) {
                        out.diagnostics.push_back(
                            "load_spill must have exactly one def and one use in i" + std::to_string(inst.id)
                        );
                    }
                    else {
                        if (inst.defs[0].type != allocated_operand::kind::preg) {
                            out.diagnostics.push_back("load_spill def must be preg in i" + std::to_string(inst.id));
                        }
                        if (inst.uses[0].type != allocated_operand::kind::memory
                            && inst.uses[0].type != allocated_operand::kind::spill) {
                            out.diagnostics.push_back(
                                "load_spill use must be memory operand or spill slot in i" + std::to_string(inst.id)
                            );
                        }
                    }
                }

                if (inst.op == opcode::store_spill) {
                    if (inst.defs.size() != 1 || inst.uses.size() != 1) {
                        out.diagnostics.push_back(
                            "store_spill must have exactly one def and one use in i" + std::to_string(inst.id)
                        );
                    }
                    else {
                        if (inst.defs[0].type != allocated_operand::kind::memory
                            && inst.defs[0].type != allocated_operand::kind::spill) {
                            out.diagnostics.push_back(
                                "store_spill def must be memory operand or spill slot in i" + std::to_string(inst.id)
                            );
                        }
                        if (inst.uses[0].type != allocated_operand::kind::preg) {
                            out.diagnostics.push_back("store_spill use must be preg in i" + std::to_string(inst.id));
                        }
                    }
                }
            }
        }

        for (const auto& diag : fn.diagnostics) {
            out.diagnostics.push_back("spill-rewrite: " + diag);
        }
        return out;
    }

    [[nodiscard]] inline ssa_construction_result construct_ssa(
        const mir::physical_mir_function& fn,
        const ssa_adapter_options& options
    ) {
        ssa_construction_result out;
        out.function = fn;

        if (options.placeholder_only) {
            out.diagnostics.push_back("SSA construction skipped (placeholder_only=true)");
            return out;
        }

        // ---------------------------------------------------------------
        // Phase 1: Compute dominators + dominance frontier.
        // ---------------------------------------------------------------
        const dominator_analysis_options dom_opts{.compute_frontier = true};
        const auto dom_result = compute_dominators(fn, dom_opts);
        if (!dom_result.ok()) {
            for (const auto& d : dom_result.diagnostics)
                out.diagnostics.push_back("ssa: " + d);
            return out;
        }
        out.info.push_back("ssa: dominance available (" +
            std::to_string(dom_result.dom.immediate_dominator.size()) + " blocks)");

        const cfg_analysis_result cfg = analyze_cfg(fn);
        if (!cfg.ok()) {
            for (const auto& d : cfg.diagnostics)
                out.diagnostics.push_back("ssa: " + d);
            return out;
        }

        // Warn if CFG has blocks unreachable from the entry block.
        {
            const std::size_t cfg_blocks = cfg.block_info.size();
            const std::size_t dom_blocks = dom_result.dom.immediate_dominator.size();
            if (cfg_blocks != dom_blocks)
                out.info.push_back(
                    "ssa: cfg has " + std::to_string(cfg_blocks) +
                    " blocks but dominator tree covers " +
                    std::to_string(dom_blocks) +
                    " — " + std::to_string(cfg_blocks > dom_blocks
                                               ? cfg_blocks - dom_blocks
                                               : dom_blocks - cfg_blocks) +
                    " block(s) may be unreachable");
        }

        // Collect all preg ids that appear as defs in at least one block.
        // Map: preg_id → set of block ids where it is defined.
        std::unordered_map<std::uint16_t, std::unordered_set<std::uint32_t>> def_blocks;
        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                for (const auto& def_op : inst.defs) {
                    if (def_op.type != allocated_operand::kind::preg) continue;
                    const auto pid = std::get<preg>(def_op.value).id;
                    def_blocks[pid].insert(block.id);
                }
            }
        }

        // ---------------------------------------------------------------
        // Phase 2: phi placement — iterated dominance frontier.
        // For each preg, place a phi at every block in DF+(def_blocks[p]).
        // ---------------------------------------------------------------

        // Initialize per-block states.
        for (const auto& block : fn.function.blocks) {
            ssa_block_state& bs = out.block_states[block.id];
            bs.block_id = block.id;
            for (const auto& inst : block.instructions)
                for (const auto& def_op : inst.defs)
                    if (def_op.type == allocated_operand::kind::preg)
                        bs.defined_here.insert(std::get<preg>(def_op.value).id);
        }

        // Mutable next_ssa_value_id counter (the function field is read-only here
        // because fn is const; we track our own counter separately).
        std::uint64_t next_val_id = 1;
        auto alloc_val = [&](const std::uint16_t pid, const std::uint32_t def_blk,
                             const std::uint32_t def_inst_id) -> ssa_value {
            ssa_value v;
            v.preg_id = pid;
            v.version = 0; // filled by rename phase
            v.value_id = ssa_value_id{next_val_id++};
            v.def_block = def_blk;
            v.def_inst = def_inst_id;
            out.value_table[v.value_id.id] = v;
            return v;
        };

        // Cytron iterated dominance frontier.
        for (auto& [pid, def_set] : def_blocks) {
            // worklist of blocks that define pid (or have a phi for pid)
            std::unordered_set<std::uint32_t> worklist(def_set.begin(), def_set.end());
            std::unordered_set<std::uint32_t> has_phi;

            while (!worklist.empty()) {
                const std::uint32_t blk = *worklist.begin();
                worklist.erase(worklist.begin());

                // For every block y in DF(blk), place a phi for pid at y.
                const auto df_it = dom_result.dom.dominance_frontier.find(blk);
                if (df_it == dom_result.dom.dominance_frontier.end()) continue;
                for (const std::uint32_t y : df_it->second) {
                    if (has_phi.contains(y)) continue;
                    has_phi.insert(y);

                    // Build the phi_node shell (incoming values filled by rename).
                    phi_node phi;
                    phi.result = alloc_val(pid, y, 0 /*phi*/);
                    phi.preg_id = pid;
                    phi.block_id = y;
                    // Reserve one incoming slot per predecessor.
                    const auto blk_it = cfg.block_info.find(y);
                    if (blk_it != cfg.block_info.end()) {
                        for (const auto pred : blk_it->second.predecessors)
                            phi.incoming.emplace_back(pred, ssa_value{});
                    }

                    out.block_states[y].phi_nodes.push_back(std::move(phi));

                    // y now also defines pid (via the phi), so add to worklist.
                    if (!def_set.contains(y)) {
                        worklist.insert(y);
                        def_set.insert(y); // prevent re-adding later
                    }
                }
            }
        }

        // Emit phi count info.
        {
            std::size_t phi_count = 0;
            for (const auto& [bid, bs] : out.block_states)
                phi_count += bs.phi_nodes.size();
            out.info.push_back("ssa: phi placement complete — " +
                std::to_string(phi_count) + " phi node(s) across " +
                std::to_string(out.block_states.size()) + " block(s)");
        }

        // ---------------------------------------------------------------
        // Phase 3: SSA rename — DFS over dominator tree.
        //
        // For each preg, maintain a stack of ssa_values.  Traverse blocks in
        // dominator-tree pre-order; for each block:
        //   a. For each phi placed at this block: allocate a new version for
        //      its result and push to the stack.
        //   b. For each instruction in order: record uses (top-of-stack for each
        //      preg operand), then for each def allocate a new version + push.
        //   c. For each CFG successor: fill in this block's contribution to
        //      any phi in the successor.
        //   d. Recurse into dominated children.
        //   e. Pop all versions pushed in this block.
        // ---------------------------------------------------------------

        // Per-preg version stack: preg_id → stack of ssa_value.
        std::unordered_map<std::uint16_t, std::vector<ssa_value>> version_stack;

        // Assign monotonically increasing version numbers per preg.
        std::unordered_map<std::uint16_t, std::uint32_t> version_counter;

        auto push_val = [&](const std::uint16_t pid, ssa_value v) -> ssa_value& {
            v.version = ++version_counter[pid];
            out.value_table[v.value_id.id].version = v.version;
            version_stack[pid].push_back(v);
            return version_stack[pid].back();
        };

        auto top_val = [&](const std::uint16_t pid) -> std::optional<ssa_value> {
            const auto it = version_stack.find(pid);
            if (it == version_stack.end() || it->second.empty()) return std::nullopt;
            return it->second.back();
        };

        // Build dominator-tree pre-order traversal list using the dominated_children map.
        // We do iterative DFS to avoid stack overflow on deep trees.
        struct rename_frame {
            std::uint32_t block_id;
            bool processed = false;
            std::unordered_map<std::uint16_t, std::size_t> pushed; // pid → count pushed
        };

        std::vector<rename_frame> dfs_stack;
        dfs_stack.push_back({cfg.entry_block, false, {}});

        while (!dfs_stack.empty()) {
            auto& frame = dfs_stack.back();
            const std::uint32_t bid = frame.block_id;

            // Only process a block the first time we see its frame (before children).
            if (!frame.processed) {
                frame.processed = true;
                // a. Process phi results placed at this block.
                auto& bs = out.block_states[bid];
                for (auto& phi : bs.phi_nodes) {
                    push_val(phi.preg_id, phi.result);
                    ++frame.pushed[phi.preg_id];
                    // Update the result version in the phi node itself.
                    phi.result = version_stack[phi.preg_id].back();
                }

                // b. Process instructions in this block.
                for (auto& blk : out.function.function.blocks) {
                    if (blk.id != bid) continue;
                    for (auto& inst : blk.instructions) {
                        // Record SSA uses (top-of-stack for each preg use).
                        inst.ssa_uses.clear();
                        for (const auto& use_op : inst.uses) {
                            if (use_op.type != allocated_operand::kind::preg) continue;
                            const auto pid = std::get<preg>(use_op.value).id;
                            if (const auto v = top_val(pid)) {
                                inst.ssa_uses.push_back(v->value_id);
                            }
                        }
                        // Allocate new versions for defs.
                        inst.ssa_defs.clear();
                        for (const auto& def_op : inst.defs) {
                            if (def_op.type != allocated_operand::kind::preg) continue;
                            const auto pid = std::get<preg>(def_op.value).id;
                            auto new_val = alloc_val(pid, bid, inst.id);
                            auto& sv = push_val(pid, new_val);
                            inst.ssa_defs.push_back(sv.value_id);
                            ++frame.pushed[pid];
                        }
                    }
                    break;
                }

                // c. Fill phi incoming edges in CFG successors.
                const auto blk_it = cfg.block_info.find(bid);
                if (blk_it != cfg.block_info.end()) {
                    for (const std::uint32_t succ : blk_it->second.successors) {
                        auto& succ_bs = out.block_states[succ];
                        for (auto& phi : succ_bs.phi_nodes) {
                            for (auto& [pred_id, incoming_val] : phi.incoming) {
                                if (pred_id != bid) continue;
                                if (const auto v = top_val(phi.preg_id))
                                    incoming_val = *v;
                            }
                        }
                    }
                }

                // d. Collect children before pushing to avoid dangling ref after realloc.
                std::vector<std::uint32_t> children;
                const auto children_it = dom_result.dom.dominated_children.find(bid);
                if (children_it != dom_result.dom.dominated_children.end())
                    children.assign(children_it->second.begin(), children_it->second.end());

                for (const std::uint32_t child : children)
                    dfs_stack.push_back({child, false, {}});
            }
            else {
                // e. Pop versions pushed during this block's processing.
                for (const auto& [pid, count] : frame.pushed)
                    for (std::size_t i = 0; i < count; ++i)
                        version_stack[pid].pop_back();
                dfs_stack.pop_back();
            }
        }

        // ---------------------------------------------------------------
        // Phase 4: Validate SSA dominance properties.
        // For every ssa_use in every instruction, verify that its defining
        // ssa_value dominates the using instruction's block.
        // ---------------------------------------------------------------
        for (const auto& block : out.function.function.blocks) {
            for (const auto& inst : block.instructions) {
                for (const auto& use_vid : inst.ssa_uses) {
                    const auto vit = out.value_table.find(use_vid.id);
                    if (vit == out.value_table.end()) continue;
                    const auto& v = vit->second;
                    if (v.def_block != 0
                        && v.def_block != block.id
                        && !dominates(dom_result, v.def_block, block.id)) {
                        out.diagnostics.push_back(
                            "ssa: dominance violation: value s" +
                            std::to_string(use_vid.id) + " (preg " +
                            std::to_string(v.preg_id) + " v" +
                            std::to_string(v.version) + " defined in bb" +
                            std::to_string(v.def_block) + ") used in bb" +
                            std::to_string(block.id) + " inst " +
                            std::to_string(inst.id) +
                            " but def does not dominate use");
                    }
                }
            }
        }

        // Emit renamed-value count info.
        out.info.push_back("ssa: rename complete — " +
            std::to_string(out.value_table.size()) + " SSA value(s) assigned across " +
            std::to_string(out.function.function.blocks.size()) + " block(s)");

        return out;
    }

    [[nodiscard]] inline ssa_destruction_result destroy_ssa(
        const ssa_construction_result& ssa
    ) {
        ssa_destruction_result out;

        // Nothing to do for placeholder results or if no phi nodes were placed.
        if (!ssa.ok() || ssa.block_states.empty()) {
            out.function = ssa.function;
            return out;
        }

        // Check if any phi nodes exist at all — if not, just strip the overlay.
        bool any_phi = false;
        for (const auto& [bid, bs] : ssa.block_states) {
            if (!bs.phi_nodes.empty()) {
                any_phi = true;
                break;
            }
        }

        // Start with the SSA function and mutate it.
        out.function = ssa.function;

        // Compute the maximum existing instruction id so we can mint fresh ones.
        std::uint32_t next_id = 1;
        for (const auto& blk : out.function.function.blocks)
            for (const auto& inst : blk.instructions)
                if (inst.id >= next_id) next_id = inst.id + 1;

        // Helper: find a mutable block by id.
        auto find_block = [&](const std::uint32_t bid) -> allocated_basic_block* {
            for (auto& b : out.function.function.blocks)
                if (b.id == bid) return &b;
            return nullptr;
        };

        if (any_phi) {
            // ------------------------------------------------------------------
            // Phase 1: Critical-edge splitting.
            //
            // A critical edge is one where the source has >1 successors AND the
            // target has >1 predecessors.  Inserting a phi copy on a critical edge
            // would place the copy inside a block shared by multiple successors,
            // corrupting the other paths.  We split every critical edge that leads
            // into a block that has phi nodes.
            // ------------------------------------------------------------------
            // Collect critical edges that end at phi-bearing blocks.
            std::vector<std::pair<std::uint32_t, std::uint32_t>> critical_edges;
            for (const auto& [bid, bs] : ssa.block_states) {
                if (bs.phi_nodes.empty()) continue;
                // bid has phi nodes; look at its predecessors.
                const auto* target = find_block(bid);
                if (!target || target->predecessors.size() <= 1) continue;
                for (const auto pred_id : target->predecessors) {
                    const auto* pred = find_block(pred_id);
                    if (pred && pred->successors.size() > 1)
                        critical_edges.emplace_back(pred_id, bid);
                }
            }

            // Split each critical edge by inserting a new forwarding block.
            for (const auto& [from_id, to_id] : critical_edges) {
                const std::uint32_t split_id =
                    static_cast<std::uint32_t>(out.function.function.blocks.size() + 1);

                // Build the split block: single unconditional branch to `to_id`.
                allocated_basic_block split_blk;
                split_blk.id = split_id;
                split_blk.name = "phi_split_" + std::to_string(from_id) +
                    "_" + std::to_string(to_id);
                split_blk.predecessors = {from_id};
                split_blk.successors = {to_id};

                allocated_instruction br_inst;
                br_inst.id = next_id++;
                br_inst.op = opcode::branch;
                br_inst.uses = {allocated_operand::as_block(to_id)};
                split_blk.instructions = {br_inst};
                out.function.function.blocks.push_back(std::move(split_blk));

                // Update the predecessor block's terminator operands and successor list.
                auto* from_blk = find_block(from_id);
                if (from_blk) {
                    // Replace to_id with split_id in successors list.
                    for (auto& s : from_blk->successors)
                        if (s == to_id) {
                            s = split_id;
                            break;
                        }
                    // Rewrite the branch target in the terminator instruction.
                    for (auto& inst : from_blk->instructions) {
                        for (auto& use_op : inst.uses) {
                            if (use_op.type == allocated_operand::kind::block
                                && std::get<std::uint32_t>(use_op.value) == to_id) {
                                use_op = allocated_operand::as_block(split_id);
                            }
                        }
                    }
                }

                // Update the target block's predecessor list.
                auto* to_blk = find_block(to_id);
                if (to_blk) {
                    for (auto& p : to_blk->predecessors)
                        if (p == from_id) {
                            p = split_id;
                            break;
                        }
                }

                // Update CFG maps.
                auto& succ_from = out.function.function.cfg.successors[from_id];
                for (auto& s : succ_from)
                    if (s == to_id) {
                        s = split_id;
                        break;
                    }
                out.function.function.cfg.successors[split_id] = {to_id};
                out.function.function.cfg.predecessors[split_id] = {from_id};
                auto& pred_to = out.function.function.cfg.predecessors[to_id];
                for (auto& p : pred_to)
                    if (p == from_id) {
                        p = split_id;
                        break;
                    }
            }

            // ------------------------------------------------------------------
            // Phase 2: Insert parallel copies.
            //
            // For every phi_node in block B with result preg R and incoming
            // (pred_id, src_ssa_value):
            //   Insert `mov R = src_ssa_value.preg_id` at the end of pred_id,
            //   before the terminator (the last instruction).
            //
            // We use a "parallel copy" model: collect all copies for each
            // predecessor block first, then serialize them.  Because we renamed
            // into distinct versions, simultaneous assignments to the same
            // destination preg within a single predecessor cannot occur (the
            // rename guarantees each phi result is fresh), so simple sequencing
            // is correct here.
            // ------------------------------------------------------------------
            for (const auto& [bid, bs] : ssa.block_states) {
                for (const auto& phi : bs.phi_nodes) {
                    if (!phi.complete()) continue;

                    const std::uint16_t dst_preg_id = phi.result.preg_id;

                    for (const auto& [pred_id, src_val] : phi.incoming) {
                        if (!src_val.valid()) continue;
                        const std::uint16_t src_preg_id = src_val.preg_id;

                        // Skip self-copies.
                        if (src_preg_id == dst_preg_id) continue;

                        // Find the (possibly split) predecessor block.
                        auto* pred_blk = find_block(pred_id);
                        if (!pred_blk) {
                            out.diagnostics.push_back(
                                "destroy_ssa: phi in block " + std::to_string(bid) +
                                ": predecessor block " + std::to_string(pred_id) +
                                " not found — copy for preg " +
                                std::to_string(dst_preg_id) + " skipped");
                            continue;
                        }

                        // Build the mov instruction.
                        allocated_instruction mov;
                        mov.id = next_id++;
                        mov.op = opcode::mov;
                        mov.defs = {
                            allocated_operand::as_preg(
                                preg{dst_preg_id, "phi_" + std::to_string(dst_preg_id)})
                        };
                        mov.uses = {
                            allocated_operand::as_preg(
                                preg{src_preg_id, "phi_src_" + std::to_string(src_preg_id)})
                        };

                        // Insert before the terminator (last instruction).
                        auto& insts = pred_blk->instructions;
                        if (insts.empty()) {
                            insts.push_back(std::move(mov));
                        }
                        else {
                            insts.insert(insts.end() - 1, std::move(mov));
                        }
                    }
                }
            }
        }

        // ------------------------------------------------------------------
        // Phase 3: Strip SSA overlay from all instructions.
        // ------------------------------------------------------------------
        for (auto& blk : out.function.function.blocks) {
            for (auto& inst : blk.instructions) {
                inst.ssa_defs.clear();
                inst.ssa_uses.clear();
            }
        }

        // ------------------------------------------------------------------
        // Phase 4: Rebuild instruction_ids cache (new movs were inserted).
        // ------------------------------------------------------------------
        out.function.instruction_ids.clear();
        for (const auto& blk : out.function.function.blocks)
            for (const auto& inst : blk.instructions)
                if (inst.id != 0)
                    out.function.instruction_ids.insert(inst.id);

        // ------------------------------------------------------------------
        // Phase 5: Verify the resulting Physical MIR.
        // ------------------------------------------------------------------
        const auto vr = verify_physical_mir(out.function);
        for (const auto& d : vr.diagnostics)
            out.diagnostics.push_back("destroy_ssa: " + d);

        return out;
    }

    [[nodiscard]] inline ssa_verification_result verify_ssa(
        const ssa_construction_result& result
    ) {
        ssa_verification_result out;

        // If SSA construction itself reported errors, propagate them.
        if (!result.ok()) {
            out.valid = false;
            for (const auto& d : result.diagnostics)
                out.diagnostics.push_back("verify_ssa: inherited error: " + d);
            return out;
        }

        // Check 1: every value in the value table has a unique value_id.
        std::unordered_set<std::uint64_t> seen_ids;
        for (const auto& [vid, val] : result.value_table) {
            if (!seen_ids.insert(vid).second) {
                out.valid = false;
                out.diagnostics.push_back(
                    "verify_ssa: duplicate ssa_value_id " + std::to_string(vid));
            }
            if (vid != val.value_id.id) {
                out.valid = false;
                out.diagnostics.push_back(
                    "verify_ssa: value_table key " + std::to_string(vid) +
                    " does not match value_id.id " + std::to_string(val.value_id.id));
            }
        }

        // Collect known block ids.
        std::unordered_set<std::uint32_t> known_blocks;
        for (const auto& blk : result.function.function.blocks)
            known_blocks.insert(blk.id);

        // Check 2: phi nodes reference existing predecessor blocks and have no
        // missing incoming values for known predecessors.
        for (const auto& [bid, bs] : result.block_states) {
            if (!known_blocks.contains(bid)) {
                out.valid = false;
                out.diagnostics.push_back(
                    "verify_ssa: block_state references unknown block " +
                    std::to_string(bid));
            }
            for (const auto& phi : bs.phi_nodes) {
                // Collect actual predecessors from the function.
                std::unordered_set<std::uint32_t> pred_set;
                for (const auto& blk : result.function.function.blocks) {
                    if (blk.id == bid) {
                        pred_set.insert(blk.predecessors.begin(), blk.predecessors.end());
                        break;
                    }
                }
                // Every incoming slot must name a known predecessor block.
                for (const auto& [pred_blk, inc_val] : phi.incoming) {
                    if (!known_blocks.contains(pred_blk)) {
                        out.valid = false;
                        out.diagnostics.push_back(
                            "verify_ssa: phi in block " + std::to_string(bid) +
                            " references unknown predecessor " +
                            std::to_string(pred_blk));
                    }
                }
                // Every known predecessor must have an incoming slot.
                for (const std::uint32_t pred : pred_set) {
                    bool found = false;
                    for (const auto& [pb, _] : phi.incoming)
                        if (pb == pred) {
                            found = true;
                            break;
                        }
                    if (!found) {
                        out.valid = false;
                        out.diagnostics.push_back(
                            "verify_ssa: phi in block " + std::to_string(bid) +
                            " for preg " + std::to_string(phi.preg_id) +
                            " missing incoming value from predecessor " +
                            std::to_string(pred));
                    }
                }
            }
        }

        return out;
    }

    [[nodiscard]] inline dominator_analysis_result compute_dominators(
        mir::physical_mir_function const& fn,
        dominator_analysis_options options
    ) {
        dominator_analysis_result out;
        out.function = fn;

        cfg_analysis_result cfg = analyze_cfg(fn);
        if (!cfg.ok()) {
            out.diagnostics.insert(out.diagnostics.end(),
                                   cfg.diagnostics.begin(), cfg.diagnostics.end());
            return out;
        }

        // Build the root-domain graph view (async_fork edges suppressed).
        litegraph::dominator_graph_view<std::uint32_t> view = to_dominator_graph_view(cfg);
        litegraph::dominator_options dom_opts;
        dom_opts.compute_frontier = options.compute_frontier;
        out.dom = litegraph::compute_dominators(view, dom_opts);
        if (!out.dom.ok()) {
            out.diagnostics.insert(out.diagnostics.end(),
                                   out.dom.diagnostics.begin(), out.dom.diagnostics.end());
            return out;
        }

        // Per-domain dominator computation for async_fork and rpc_boundary sub-trees.
        // Each non-root domain gets its own dominator_graph_view rooted at the domain's
        // root_block, containing only the blocks that belong to that domain.
        if (cfg.partition.has_value() && cfg.partition->is_partitioned()) {
            const subgraph_partition& part = *cfg.partition;

            // Build a typed adjacency map once for all domain views.
            std::unordered_map<std::uint32_t, std::vector<cfg_edge>> adj;
            for (const auto& te : cfg.typed_edges) adj[te.from].push_back(te);

            for (const auto& domain : part.domains) {
                if (domain.domain_id == 0) continue; // root domain covered by out.dom

                litegraph::dominator_graph_view<std::uint32_t> dview;
                dview.entry = domain.root_block;

                // Populate the view with only the blocks that belong to this domain.
                std::unordered_set<std::uint32_t> domain_set(
                    domain.blocks.begin(), domain.blocks.end());

                for (const auto bid : domain.blocks) {
                    dview.nodes.push_back(bid);
                    dview.predecessors.try_emplace(bid);
                    dview.successors.try_emplace(bid);
                }

                for (const auto bid : domain.blocks) {
                    const auto it = adj.find(bid);
                    if (it == adj.end()) continue;
                    for (const auto& te : it->second) {
                        if (!domain_set.count(te.to)) continue;
                        dview.successors[bid].push_back(te.to);
                        dview.predecessors[te.to].push_back(bid);
                    }
                }

                auto sub_dom = litegraph::compute_dominators(dview, dom_opts);
                if (!sub_dom.ok()) {
                    // Non-fatal: record warnings but keep going.
                    for (const auto& d : sub_dom.diagnostics)
                        out.diagnostics.push_back(
                            "domain " + std::to_string(domain.domain_id) + ": " + d);
                }
                out.sub_domain_doms[domain.domain_id] = std::move(sub_dom);
            }
        }

        if (options.compute_loop_headers)
            out.loop_header_blocks = litegraph::find_loop_headers(view, out.dom);

        return out;
    }

    // -----------------------------------------------------------------------
    // Dominator query helpers
    // All functions are safe: missing blocks return false / empty results.
    // -----------------------------------------------------------------------

    // True iff block a dominates block b.
    [[nodiscard]] inline bool dominates(
        dominator_analysis_result const& r,
        const std::uint32_t a, const std::uint32_t b) {
        return litegraph::dominates(r.dom, a, b);
    }

    // Immediate dominator of block, or nullopt for the entry block.
    [[nodiscard]] inline std::optional<std::uint32_t> immediate_dominator_of(
        dominator_analysis_result const& r,
        const std::uint32_t block) {
        return litegraph::immediate_dominator_of(r.dom, block);
    }

    // Blocks directly dominated by block (one tree level down).
    [[nodiscard]] inline std::unordered_set<std::uint32_t> dominated_blocks_of(
        dominator_analysis_result const& r,
        const std::uint32_t block) {
        return litegraph::dominated_children_of(r.dom, block);
    }

    // Dominance frontier of block.  Empty if compute_frontier was false.
    [[nodiscard]] inline std::unordered_set<std::uint32_t> dominance_frontier_of(
        dominator_analysis_result const& r,
        const std::uint32_t block) {
        auto it = r.dom.dominance_frontier.find(block);
        if (it == r.dom.dominance_frontier.end()) return {};
        return it->second;
    }

    // Set of loop-header blocks.  Empty if compute_loop_headers was false.
    [[nodiscard]] inline std::unordered_set<std::uint32_t> loop_headers(
        dominator_analysis_result const& r) {
        return r.loop_header_blocks;
    }

    // Computes natural-loop information for a physical MIR function.
    //
    // Algorithm:
    //   1. Compute dominators (with loop-header detection).
    //   2. Enumerate back edges via litegraph::find_back_edges.
    //   3. For each back edge A→H, collect the natural loop body by
    //      reverse-DFS from A stopping at H (the header).
    //   4. Merge multiple back edges into the same header.
    //   5. Identify exit blocks (body blocks with successors outside the body).
    [[nodiscard]] inline loop_analysis_result analyze_loops(
        mir::physical_mir_function const& fn) {
        loop_analysis_result result;

        const auto dom = compute_dominators(fn, {.compute_loop_headers = true});
        if (!dom.ok()) {
            result.diagnostics.insert(result.diagnostics.end(),
                                      dom.diagnostics.begin(), dom.diagnostics.end());
            return result;
        }

        const auto cfg = analyze_cfg(fn);
        if (!cfg.ok()) {
            result.diagnostics.insert(result.diagnostics.end(),
                                      cfg.diagnostics.begin(), cfg.diagnostics.end());
            return result;
        }

        const auto view = to_dominator_graph_view(cfg);
        const auto back_edges = litegraph::find_back_edges(view, dom.dom);

        if (back_edges.empty()) return result;

        // Group back edges by their target (the loop header).
        std::unordered_map<std::uint32_t, loop_info> header_to_loop;
        for (const auto& be : back_edges) {
            auto& li = header_to_loop[be.to];
            li.header = be.to;
            li.back_edges.push_back({be.from, be.to});
        }

        // Build predecessor map for reverse DFS.
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> preds;
        for (const auto& [id, info] : cfg.block_info) {
            for (const auto succ : info.successors) {
                preds[succ].push_back(id);
            }
        }

        // For each loop, collect the body via reverse DFS from each back-edge
        // source, stopping at the header.
        for (auto& [header, li] : header_to_loop) {
            li.body.insert(header);
            std::vector<std::uint32_t> worklist;

            for (const auto& be : li.back_edges) {
                if (!li.body.contains(be.from)) {
                    li.body.insert(be.from);
                    worklist.push_back(be.from);
                }
            }

            while (!worklist.empty()) {
                const auto node = worklist.back();
                worklist.pop_back();
                const auto pit = preds.find(node);
                if (pit == preds.end()) continue;
                for (const auto pred : pit->second) {
                    if (pred == header) continue; // don't cross the header
                    if (li.body.contains(pred)) continue;
                    li.body.insert(pred);
                    worklist.push_back(pred);
                }
            }

            // Sanity check: the header should dominate every body block.
            // If not, emit a diagnostic and include the offending block anyway
            // (conservative — keep the loop body large rather than small).
            for (const auto body_block : li.body) {
                if (body_block == header) continue;
                if (!litegraph::dominates(dom.dom, header, body_block)) {
                    result.diagnostics.push_back(
                        "loop_analysis: header bb" + std::to_string(header)
                        + " does not dominate body block bb"
                        + std::to_string(body_block)
                        + " (irregular loop; body kept conservatively)");
                }
            }

            // Identify exit blocks.
            for (const auto body_block : li.body) {
                const auto it = cfg.block_info.find(body_block);
                if (it == cfg.block_info.end()) continue;
                for (const auto succ : it->second.successors) {
                    if (!li.body.contains(succ)) {
                        li.exit_blocks.insert(body_block);
                        break;
                    }
                }
            }
        }

        result.loops.reserve(header_to_loop.size());
        for (auto& [_, li] : header_to_loop) {
            result.loops.push_back(std::move(li));
        }

        return result;
    }

    // -----------------------------------------------------------------------
    // Group O — Abstract operation lowering framework
    //
    // Provides an extensible hook mechanism for lowering custom/abstract
    // operation domains into core MIR or target-specific MIR before backend
    // legality validation and emission.
    //
    // Design:
    //   • operation_lowering_context  — read-only inputs for a rule invocation.
    //   • operation_lowering_result   — output of a complete function lowering pass.
    //   • OperationLoweringRule       — concept: matches() + lower().
    //   • operation_lowering_pipeline — ordered list of rules applied per function.
    //
    // -----------------------------------------------------------------------

    // Context passed to each rule during function lowering.
    struct operation_lowering_context {
        const operation_registry* registry = nullptr;
        const function_ir* original_ir = nullptr;
        std::vector<std::string> diagnostics;
    };

    // Result returned by operation_lowering_pipeline::lower_function.
    struct operation_lowering_result {
        mir::physical_mir_function function;
        std::vector<std::string> diagnostics;
        bool changed = false;

        [[nodiscard]] bool ok() const {
            for (const auto& d : diagnostics) {
                if (d.rfind("error:", 0) == 0) return false;
            }
            return true;
        }
    };

    // Concept satisfied by any type that can match and lower a single
    // abstract-operation instruction.
    template <typename R>
    concept OperationLoweringRule =
        requires(const R rule,
                 const operation_id& id,
                 const allocated_instruction& inst,
                 operation_lowering_context& ctx) {
            { rule.matches(id) } -> std::convertible_to<bool>;
            { rule.lower(inst, ctx) } -> std::convertible_to<operation_lowering_result>;
        };

    class operation_lowering_pipeline {
    public:
        struct rule_entry {
            std::string name;
            std::function<bool(const operation_id &)> matches;
            std::function<operation_lowering_result(
                          const allocated_instruction&,
                          operation_lowering_context&
            )
            >
            lower;
        };

        template <OperationLoweringRule R>
        void add_rule(std::string name, R rule) {
            rule_entry entry;
            entry.name = std::move(name);
            entry.matches = [r = rule](const operation_id& id) {
                return r.matches(id);
            };
            entry.lower = [r = std::move(rule)](
                const allocated_instruction& inst,
                operation_lowering_context& ctx) mutable {
                    return r.lower(inst, ctx);
                };
            rules_.push_back(std::move(entry));
        }

        [[nodiscard]] bool remove_rule(std::string_view name) {
            const auto before = rules_.size();
            rules_.erase(
                std::remove_if(rules_.begin(), rules_.end(),
                               [name](const rule_entry& e) { return e.name == name; }),
                rules_.end());
            return rules_.size() != before;
        }

        void clear_rules() { rules_.clear(); }

        [[nodiscard]] bool empty() const { return rules_.empty(); }
        [[nodiscard]] std::size_t size() const { return rules_.size(); }

        [[nodiscard]] std::vector<std::string> rule_names() const {
            std::vector<std::string> names;
            names.reserve(rules_.size());
            for (const auto& e : rules_) names.push_back(e.name);
            return names;
        }

        // Lower all abstract-operation instructions in `fn`.
        // Instructions with no matching rule are kept as-is (not an error).
        // MIR is verified after any rewrite; on failure the original is returned.
        [[nodiscard]] operation_lowering_result
        lower_function(const mir::physical_mir_function& fn,
                       operation_lowering_context& ctx) const {
            operation_lowering_result out;
            out.function = fn;

            if (rules_.empty()) return out;

            // Build id → original instruction* map for fallback when the
            // allocated instruction has no abstract_operation metadata.
            std::unordered_map<std::uint32_t, const instruction*> orig_by_id;
            const auto& src_ir = ctx.original_ir
                                     ? ctx.original_ir->blocks
                                     : fn.function.original_vreg_ir.blocks;
            for (const auto& blk : src_ir) {
                for (const auto& orig : blk.instructions) {
                    orig_by_id.emplace(orig.id, &orig);
                }
            }

            bool any_changed = false;

            for (auto& blk : out.function.function.blocks) {
                for (auto& inst : blk.instructions) {
                    // Prefer metadata on the allocated instruction directly;
                    // fall back to original_vreg_ir lookup when absent.
                    const operation_id* op_id_ptr = nullptr;
                    if (inst.abstract_operation) {
                        op_id_ptr = &*inst.abstract_operation;
                    }
                    else {
                        const auto orig_it = orig_by_id.find(inst.id);
                        if (orig_it == orig_by_id.end()) continue;
                        const instruction* orig = orig_it->second;
                        if (!orig->abstract_operation) continue;
                        op_id_ptr = &*orig->abstract_operation;
                    }

                    const operation_id& op_id = *op_id_ptr;

                    for (const auto& rule : rules_) {
                        if (!rule.matches(op_id)) continue;

                        auto rule_result = rule.lower(inst, ctx);
                        out.diagnostics.insert(
                            out.diagnostics.end(),
                            rule_result.diagnostics.begin(),
                            rule_result.diagnostics.end());

                        if (rule_result.changed) {
                            if (!rule_result.function.function.blocks.empty()) {
                                out.function = std::move(rule_result.function);
                                any_changed = true;
                                goto restart_blocks;
                            }
                            any_changed = true;
                        }
                        break;
                    }
                }
            }
        restart_blocks:

            if (!any_changed) return out;

            out.changed = true;

            const auto verification = verify_physical_mir(out.function);
            out.function.verified = verification.ok();
            out.function.verification_diagnostics = verification.diagnostics;
            if (!verification.ok()) {
                out.diagnostics.push_back(
                    "error: operation_lowering_pipeline: verification failed after lowering; "
                    "returning original function");
                out.diagnostics.insert(out.diagnostics.end(),
                                       verification.diagnostics.begin(),
                                       verification.diagnostics.end());
                out.function = fn;
                out.changed = false;
            }

            return out;
        }

    private:
        std::vector<rule_entry> rules_;
    };

    // -----------------------------------------------------------------------
    // Group — target pipeline stage abstraction
    //
    // target_stage_kind enumerates the logical phases of a post-MIR backend
    // flow.  target_stage_result carries the outcome of a single stage.
    // target_stage wraps a callable stage runner together with its metadata.
    // target_pipeline sequences stages and drives them over a physical MIR
    // function, threading the result through each enabled stage in order.
    //
    // -----------------------------------------------------------------------

    enum class target_stage_kind : std::uint8_t {
        legalize, // check / enforce backend preconditions
        lower, // MIR → target-specific lowered form
        schedule, // instruction scheduling (framework stub)
        instrument, // inject profiling / sanitizer code
        assemble, // pseudo-assembly or real ISA emission
        link_or_finalize, // link / finalize JIT (framework stub)
        emit_artifact, // package output into a compilation_artifact
    };

    [[nodiscard]] constexpr const char* to_string(const target_stage_kind k) noexcept {
        switch (k) {
        case target_stage_kind::legalize: return "legalize";
        case target_stage_kind::lower: return "lower";
        case target_stage_kind::schedule: return "schedule";
        case target_stage_kind::instrument: return "instrument";
        case target_stage_kind::assemble: return "assemble";
        case target_stage_kind::link_or_finalize: return "link_or_finalize";
        case target_stage_kind::emit_artifact: return "emit_artifact";
        }
        return "unknown";
    }

    struct target_stage_result {
        mir::physical_mir_function function;
        compilation_artifact artifact;
        std::vector<std::string> diagnostics;
        bool changed = false;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    struct target_stage {
        using runner_type = std::function<target_stage_result(
                                          const mir::physical_mir_function&,
                                          const compilation_artifact&
        )
        >;

        std::string name;
        target_stage_kind kind = target_stage_kind::legalize;
        bool enabled = true;
        runner_type run;
    };

    class target_pipeline {
    public:
        void add_stage(target_stage stage) {
            if (stage.name.empty()) {
                stage.name = std::string{to_string(stage.kind)};
            }
            stages_.push_back(std::move(stage));
        }

        [[nodiscard]] bool remove_stage(std::string_view name) {
            const auto before = stages_.size();
            stages_.erase(
                std::remove_if(stages_.begin(), stages_.end(),
                               [name](const target_stage& s) { return s.name == name; }),
                stages_.end());
            return stages_.size() != before;
        }

        bool enable_stage(const std::string_view name) {
            for (auto& s : stages_) {
                if (s.name == name) {
                    s.enabled = true;
                    return true;
                }
            }
            return false;
        }

        bool disable_stage(const std::string_view name) {
            for (auto& s : stages_) {
                if (s.name == name) {
                    s.enabled = false;
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool contains_stage(const std::string_view name) const {
            for (const auto& s : stages_) {
                if (s.name == name) return true;
            }
            return false;
        }

        [[nodiscard]] std::vector<std::string> stage_names() const {
            std::vector<std::string> names;
            names.reserve(stages_.size());
            for (const auto& s : stages_) {
                names.push_back(s.name);
            }
            return names;
        }

        void clear() { stages_.clear(); }
        [[nodiscard]] std::size_t size() const { return stages_.size(); }
        [[nodiscard]] bool empty() const { return stages_.empty(); }

        // Attach an operation_lowering_pipeline.  When set and non-empty it is
        // invoked automatically before any stage of kind `lower` executes.
        // Passing a default-constructed (empty) pipeline removes the hook.
        void set_lowering_pipeline(operation_lowering_pipeline lp) {
            lowering_pipeline_ = std::move(lp);
        }

        [[nodiscard]] const operation_lowering_pipeline& lowering_pipeline() const {
            return lowering_pipeline_;
        }

        [[nodiscard]] operation_lowering_pipeline& lowering_pipeline() {
            return lowering_pipeline_;
        }

        // Run the stage sequence against `fn`.
        //
        // If an operation_lowering_pipeline has been attached and has rules,
        // it is automatically invoked immediately before the first enabled
        // stage of kind `lower`.  Diagnostics from lowering are collected into
        // the returned target_stage_result.  If lowering reports a fatal error
        // (ok() == false) execution stops and the partial result is returned.
        // When no lowering rules are registered the pipeline behaves exactly as
        // before, preserving backward compatibility.
        [[nodiscard]] target_stage_result run(const mir::physical_mir_function& fn) const {
            target_stage_result state;
            state.function = fn;

            bool lowering_applied = false;

            for (const auto& stage : stages_) {
                if (!stage.enabled) continue;

                // Run the operation lowering pipeline once, just before the
                // first enabled `lower` stage.  Skip when no rules are present.
                if (stage.kind == target_stage_kind::lower
                    && !lowering_applied
                    && !lowering_pipeline_.empty()) {
                    lowering_applied = true;

                    operation_lowering_context lctx;
                    auto lower_result = lowering_pipeline_.lower_function(state.function, lctx);

                    state.diagnostics.insert(
                        state.diagnostics.end(),
                        lower_result.diagnostics.begin(),
                        lower_result.diagnostics.end());

                    if (!lower_result.ok()) {
                        return state;
                    }

                    if (lower_result.changed) {
                        state.function = std::move(lower_result.function);
                        state.changed = true;
                    }
                }

                if (!stage.run) continue;

                auto step = stage.run(state.function, state.artifact);
                state.diagnostics.insert(
                    state.diagnostics.end(),
                    step.diagnostics.begin(), step.diagnostics.end());

                if (!step.ok()) return state;

                if (!step.function.function.blocks.empty()) {
                    state.function = std::move(step.function);
                    state.changed = true;
                }
                if (step.artifact.ok()) {
                    state.artifact = std::move(step.artifact);
                }
            }

            return state;
        }

    private:
        std::vector<target_stage> stages_;
        // Optional lowering pass; runs before the first `lower` stage when non-empty.
        operation_lowering_pipeline lowering_pipeline_;
    };

    // -----------------------------------------------------------------------
    // Group I — Instrumentation model
    // -----------------------------------------------------------------------

    enum class instrumentation_event_kind {
        compile_begin,
        compile_end,
        pass_begin,
        pass_end,
        target_stage_begin,
        target_stage_end,
        instruction_executed,
        block_executed,
        branch_taken,
        spill_used,
        cache_hit,
        cache_miss,
        runtime_counter,
    };

    struct instrumentation_event {
        instrumentation_event_kind kind = instrumentation_event_kind::runtime_counter;
        std::optional<std::uint32_t> block_id;
        std::optional<std::uint32_t> instruction_id;
        std::string pass_or_stage_name;
        std::int64_t value = 0;
        std::unordered_map<std::string, std::string> metadata;
    };

    struct instrumentation_trace {
        std::vector<instrumentation_event> events;

        void add(instrumentation_event ev) {
            events.push_back(std::move(ev));
        }

        void clear() { events.clear(); }
    };

    struct instrumentation_options {
        bool enabled = false;
        bool trace_compile_events = false;
        bool trace_pass_events = false;
        bool trace_target_stage_events = false;
        bool trace_block_execution = false;
        bool trace_instruction_execution = false;
        bool trace_branches = false;
        bool trace_spills = false;
        bool trace_cache = false;
    };

    // -----------------------------------------------------------------------
    // Group J — Feedback profile model
    // -----------------------------------------------------------------------

    struct block_profile {
        std::uint32_t block_id = 0;
        std::uint64_t execution_count = 0;
        bool is_hot = false;
    };

    struct instruction_profile {
        std::uint32_t instruction_id = 0;
        std::uint32_t block_id = 0;
        std::uint64_t execution_count = 0;
        bool is_hot = false;
    };

    struct branch_profile {
        std::uint32_t block_id = 0;
        std::uint64_t taken_count = 0;
        std::uint64_t not_taken_count = 0;

        [[nodiscard]] double taken_frequency() const {
            auto total = taken_count + not_taken_count;
            return total == 0 ? 0.0 : static_cast<double>(taken_count) / static_cast<double>(total);
        }
    };

    struct spill_profile {
        std::string pass_name;
        std::uint64_t spill_count = 0;
    };

    struct optimization_feedback_profile {
        std::vector<block_profile> blocks;
        std::vector<instruction_profile> instructions;
        std::vector<branch_profile> branches;
        std::vector<spill_profile> spills;

        // pass name -> number of IR nodes changed
        std::unordered_map<std::string, std::uint64_t> pass_effectiveness;

        // artifact metadata forwarded from compilation_artifact
        std::unordered_map<std::string, std::string> artifact_metadata;

        [[nodiscard]] std::vector<std::uint32_t> hot_blocks() const {
            std::vector<std::uint32_t> result;
            for (const auto& b : blocks) {
                if (b.is_hot) result.push_back(b.block_id);
            }
            return result;
        }

        [[nodiscard]] std::vector<std::uint32_t> hot_instructions() const {
            std::vector<std::uint32_t> result;
            for (const auto& i : instructions) {
                if (i.is_hot) result.push_back(i.instruction_id);
            }
            return result;
        }

        [[nodiscard]] std::uint64_t total_spills() const {
            std::uint64_t n = 0;
            for (const auto& s : spills) n += s.spill_count;
            return n;
        }
    };

    [[nodiscard]] inline optimization_feedback_profile
    build_feedback_profile(const instrumentation_trace& trace) {
        optimization_feedback_profile profile;

        std::unordered_map<std::uint32_t, std::uint64_t> block_counts;
        std::unordered_map<std::uint32_t, std::uint64_t> instr_counts;
        std::unordered_map<std::uint32_t, branch_profile> branch_map;
        std::unordered_map<std::string, std::uint64_t> spill_map;

        for (const auto& ev : trace.events) {
            switch (ev.kind) {
            case instrumentation_event_kind::block_executed:
                if (ev.block_id)
                    block_counts[*ev.block_id] += static_cast<std::uint64_t>(
                        ev.value > 0 ? ev.value : 1);
                break;
            case instrumentation_event_kind::instruction_executed:
                if (ev.instruction_id)
                    instr_counts[*ev.instruction_id] += static_cast<std::uint64_t>(
                        ev.value > 0 ? ev.value : 1);
                break;
            case instrumentation_event_kind::branch_taken:
                if (ev.block_id) {
                    auto& bp = branch_map[*ev.block_id];
                    bp.block_id = *ev.block_id;
                    if (ev.value > 0) bp.taken_count += static_cast<std::uint64_t>(ev.value);
                    else bp.not_taken_count += 1;
                }
                break;
            case instrumentation_event_kind::spill_used:
                spill_map[ev.pass_or_stage_name] += static_cast<std::uint64_t>(ev.value > 0 ? ev.value : 1);
                break;
            default:
                break;
            }
        }

        std::uint64_t max_block = 0;
        for (const auto& [id, cnt] : block_counts) max_block = std::max(max_block, cnt);
        const std::uint64_t hot_threshold = max_block / 10;

        for (const auto& [id, cnt] : block_counts) {
            block_profile bp;
            bp.block_id = id;
            bp.execution_count = cnt;
            bp.is_hot = cnt >= hot_threshold && hot_threshold > 0;
            profile.blocks.push_back(bp);
        }

        std::uint64_t max_instr = 0;
        for (const auto& [id, cnt] : instr_counts) max_instr = std::max(max_instr, cnt);
        const std::uint64_t hot_instr_threshold = max_instr / 10;

        for (const auto& [id, cnt] : instr_counts) {
            instruction_profile ip;
            ip.instruction_id = id;
            ip.execution_count = cnt;
            ip.is_hot = cnt >= hot_instr_threshold && hot_instr_threshold > 0;
            profile.instructions.push_back(ip);
        }

        for (auto& [id, bp] : branch_map) {
            profile.branches.push_back(bp);
        }

        for (const auto& [name, cnt] : spill_map) {
            spill_profile sp;
            sp.pass_name = name;
            sp.spill_count = cnt;
            profile.spills.push_back(sp);
        }

        return profile;
    }

    [[nodiscard]] inline optimization_feedback_profile
    merge_feedback_profiles(const optimization_feedback_profile& a,
                            const optimization_feedback_profile& b) {
        optimization_feedback_profile merged = a;

        // merge block counts
        std::unordered_map<std::uint32_t, std::size_t> block_idx;
        for (std::size_t i = 0; i < merged.blocks.size(); ++i)
            block_idx[merged.blocks[i].block_id] = i;
        for (const auto& bp : b.blocks) {
            auto it = block_idx.find(bp.block_id);
            if (it != block_idx.end()) {
                merged.blocks[it->second].execution_count += bp.execution_count;
            }
            else {
                block_idx[bp.block_id] = merged.blocks.size();
                merged.blocks.push_back(bp);
            }
        }

        // merge instruction counts
        std::unordered_map<std::uint32_t, std::size_t> instr_idx;
        for (std::size_t i = 0; i < merged.instructions.size(); ++i)
            instr_idx[merged.instructions[i].instruction_id] = i;
        for (const auto& ip : b.instructions) {
            auto it = instr_idx.find(ip.instruction_id);
            if (it != instr_idx.end()) {
                merged.instructions[it->second].execution_count += ip.execution_count;
            }
            else {
                instr_idx[ip.instruction_id] = merged.instructions.size();
                merged.instructions.push_back(ip);
            }
        }

        // merge branches
        std::unordered_map<std::uint32_t, std::size_t> branch_idx;
        for (std::size_t i = 0; i < merged.branches.size(); ++i)
            branch_idx[merged.branches[i].block_id] = i;
        for (const auto& bp : b.branches) {
            auto it = branch_idx.find(bp.block_id);
            if (it != branch_idx.end()) {
                merged.branches[it->second].taken_count += bp.taken_count;
                merged.branches[it->second].not_taken_count += bp.not_taken_count;
            }
            else {
                branch_idx[bp.block_id] = merged.branches.size();
                merged.branches.push_back(bp);
            }
        }

        // merge spills
        std::unordered_map<std::string, std::size_t> spill_idx;
        for (std::size_t i = 0; i < merged.spills.size(); ++i)
            spill_idx[merged.spills[i].pass_name] = i;
        for (const auto& sp : b.spills) {
            auto it = spill_idx.find(sp.pass_name);
            if (it != spill_idx.end()) {
                merged.spills[it->second].spill_count += sp.spill_count;
            }
            else {
                spill_idx[sp.pass_name] = merged.spills.size();
                merged.spills.push_back(sp);
            }
        }

        // merge pass effectiveness
        for (const auto& [name, cnt] : b.pass_effectiveness) {
            merged.pass_effectiveness[name] += cnt;
        }

        // merge artifact metadata (b overwrites on conflict)
        for (const auto& [k, v] : b.artifact_metadata) {
            merged.artifact_metadata[k] = v;
        }

        // recompute hot flags
        std::uint64_t max_block = 0;
        for (const auto& bp : merged.blocks) max_block = std::max(max_block, bp.execution_count);
        const std::uint64_t hot_block_thresh = max_block / 10;
        for (auto& bp : merged.blocks) bp.is_hot = bp.execution_count >= hot_block_thresh && hot_block_thresh > 0;

        std::uint64_t max_instr = 0;
        for (const auto& ip : merged.instructions) max_instr = std::max(max_instr, ip.execution_count);
        const std::uint64_t hot_instr_thresh = max_instr / 10;
        for (auto& ip : merged.instructions) ip.is_hot = ip.execution_count >= hot_instr_thresh && hot_instr_thresh > 0;

        return merged;
    }

    // -----------------------------------------------------------------------
    // Group K — ML optimizer hook interface
    //
    // Provides an opt-in seam for learned optimizers without any dependency on
    // ML libraries (PyTorch, ONNX, TensorFlow, etc.).  Normal compilation via
    // compile_to_physical_mir is unaffected when no advisor is supplied.
    //
    // Design rules
    //   • Interface only — no implementation here.
    //   • Deterministic fallback: when no advisor is present the caller must
    //     apply its default (preset-driven) behaviour unchanged.
    //   • Opt-in: callers wire an advisor explicitly; nothing runs automatically.
    //   • Stateless or stateful: the concept imposes no lifetime restrictions so
    //     advisors may be per-call lambdas, shared singletons, or anything in
    //     between.
    // -----------------------------------------------------------------------

    // Context forwarded to the advisor on every decision point.
    struct ml_optimization_context {
        // Compilation unit identity (empty when not known).
        std::string compilation_unit_name;
        // Target identifier forwarded from the active backend.
        std::string target_name;
        // Iteration index within a feedback-directed compilation loop (0 = first).
        std::uint32_t iteration = 0;
        // Accumulated profile from previous iterations (empty on first call).
        optimization_feedback_profile profile;
        // Read-only snapshot of the current pass-pipeline order.
        std::vector<std::string> current_pass_order;
        // Read-only codegen options active for this compilation.
        mir_opt_level current_opt_level = mir_opt_level::O0;
        // Arbitrary key/value annotations a caller may attach.
        std::unordered_map<std::string, std::string> annotations;
    };

    // The advisor fills one of these for every decision it makes.
    struct ml_optimization_decision {
        // Requested pass order (empty = keep current order).
        std::vector<std::string> pass_order;
        // Passes the advisor wants enabled by name.
        std::vector<std::string> enable_passes;
        // Passes the advisor wants disabled by name.
        std::vector<std::string> disable_passes;
        // Opt-level override (nullopt = no change).
        std::optional<mir_opt_level> opt_level_override;
        // Arbitrary per-pass option hints (pass_name -> serialised hint).
        std::unordered_map<std::string, std::string> pass_options;
        // Human-readable rationale — ignored by the pipeline, useful for logging.
        std::string rationale;
        // Confidence in [0, 1]; 0 means "no opinion" and the caller may ignore.
        double confidence = 0.0;
    };

    // No-op advisor: returns a default-constructed decision for every query,
    // producing identical behaviour to a pipeline with no advisor at all.
    struct noop_ml_advisor {
        void observe_profile(const optimization_feedback_profile&) noexcept {}

        [[nodiscard]] ml_optimization_decision
        choose_pipeline(const ml_optimization_context&) const noexcept {
            return {};
        }

        [[nodiscard]] std::vector<std::string>
        rank_passes(const ml_optimization_context&,
                    std::vector<std::string> passes) const noexcept {
            return passes;
        }

        [[nodiscard]] std::unordered_map<std::string, std::string>
        suggest_options(const ml_optimization_context&) const noexcept {
            return {};
        }
    };

    // Concept that any ML advisor must satisfy.
    // All four operations are required; the no-op implementations above show
    // the minimum viable body for each.
    template <typename T>
    concept MLOptimizationAdvisor = requires(
        T& advisor,
        const T& cadvisor,
        const optimization_feedback_profile& profile,
        const ml_optimization_context& ctx,
        std::vector<std::string> passes) {
            // Record execution feedback from a previous compilation.
            { advisor.observe_profile(profile) };
            // Decide which pipeline shape to use for the next compilation.
            { cadvisor.choose_pipeline(ctx) } -> std::convertible_to<ml_optimization_decision>;
            // Re-order (or filter) a candidate pass list; returns the preferred order.
            { cadvisor.rank_passes(ctx, passes) } -> std::convertible_to<std::vector<std::string>>;
            // Suggest free-form per-pass options; keys are pass names.
            { cadvisor.suggest_options(ctx) } -> std::convertible_to<std::unordered_map<std::string, std::string>>;
        };

    static_assert(MLOptimizationAdvisor<noop_ml_advisor>,
                  "noop_ml_advisor must satisfy MLOptimizationAdvisor");

    // Type-erased, nullable advisor handle.
    // Holds any MLOptimizationAdvisor by shared_ptr so the same advisor object
    // can be shared across compilations, pipelines, targets, or threads.
    // When empty (default-constructed) all methods silently no-op / return defaults.
    class ml_advisor_handle {
    public:
        ml_advisor_handle() = default;

        template <MLOptimizationAdvisor Advisor>
        explicit ml_advisor_handle(std::shared_ptr<Advisor> ptr)
            : observe_([ptr](const optimization_feedback_profile& p) { ptr->observe_profile(p); })
              , choose_([ptr](const ml_optimization_context& c) { return ptr->choose_pipeline(c); })
              , rank_([ptr](const ml_optimization_context& c, std::vector<std::string> v) {
                  return ptr->rank_passes(c, std::move(v));
              })
              , suggest_([ptr](const ml_optimization_context& c) { return ptr->suggest_options(c); }) {}

        [[nodiscard]] bool has_advisor() const noexcept { return static_cast<bool>(choose_); }

        void observe_profile(const optimization_feedback_profile& p) const {
            if (observe_) observe_(p);
        }

        [[nodiscard]] ml_optimization_decision
        choose_pipeline(const ml_optimization_context& ctx) const {
            return choose_ ? choose_(ctx) : ml_optimization_decision{};
        }

        [[nodiscard]] std::vector<std::string>
        rank_passes(const ml_optimization_context& ctx,
                    std::vector<std::string> passes) const {
            return rank_ ? rank_(ctx, std::move(passes)) : passes;
        }

        [[nodiscard]] std::unordered_map<std::string, std::string>
        suggest_options(const ml_optimization_context& ctx) const {
            return suggest_ ? suggest_(ctx) : std::unordered_map<std::string, std::string>{};
        }

    private:
        std::function<void(const optimization_feedback_profile&)> observe_;
        std::function<ml_optimization_decision(const ml_optimization_context&)> choose_;
        std::function<std::vector<std::string>(const ml_optimization_context &, std::vector<std::string>)> rank_;
        std::function<std::unordered_map<std::string, std::string>(const ml_optimization_context &)> suggest_;
    };

    // Convenience factory: wrap an advisor in a shared_ptr and return a handle.
    template <MLOptimizationAdvisor Advisor>
    [[nodiscard]] ml_advisor_handle make_ml_advisor(std::shared_ptr<Advisor> advisor) {
        return ml_advisor_handle{std::move(advisor)};
    }

    template <MLOptimizationAdvisor Advisor>
    [[nodiscard]] ml_advisor_handle make_ml_advisor(Advisor advisor) {
        return ml_advisor_handle{std::make_shared<Advisor>(std::move(advisor))};
    }

    // -----------------------------------------------------------------------
    // Group K.1 — Structural OptimizationAdvisor concept
    //
    // Provides a purely structural (concept-based) boundary that lets any
    // analysis engine — static heuristic, ML inference server, symbolic
    // solver, or learned policy — plug into the compilation pipeline by
    // satisfying a handful of well-typed method requirements.
    //
    // Relationship to MLOptimizationAdvisor (Group K)
    // -------------------------------------------------
    // MLOptimizationAdvisor is a broader, feedback-loop-integrated interface
    // concerned with whole-pipeline reordering across iterations.
    // OptimizationAdvisor is a narrower, synchronous, per-compilation-unit
    // interface concerned with a single binary decision: "run the heavy pass
    // set (SROA, polyhedral, loop transforms) or fall back to fast tiered
    // execution?"  The two can coexist: a caller may wrap the same object in
    // both an ml_advisor_handle and use it directly as an OptimizationAdvisor.
    //
    // Design rules (identical to Group K)
    // ------------------------------------
    //   • Interface only — no implementation details imposed.
    //   • Structural concept: zero vtable, zero overhead when the advisor type
    //     is known at compile time.
    //   • Deterministic fallback: noop_optimization_advisor always returns
    //     advisor_pipeline_choice::fast_tiered so existing callers are unaffected.
    //   • Opt-in: the advisor template parameter defaults to
    //     noop_optimization_advisor everywhere.
    //   • The concept does NOT include observe_profile/rank_passes — those
    //     remain exclusive to MLOptimizationAdvisor.
    // -----------------------------------------------------------------------

    // Enumeration returned by OptimizationAdvisor::choose_pipeline().
    // Callers are free to add backend-specific values by extending this enum
    // in a derived type, but the concept only requires convertibility to this.
    enum class advisor_pipeline_choice : std::uint8_t {
        // Run only fast CFG cleanup and copy/const propagation (Tier 1 path).
        fast_tiered = 0,
        // Run the full aggressive pass set: SROA, CSE, dead-def, peephole.
        heavy_aggressive = 1,
        // Run the conservative set (jump threading + peephole), skip SROA.
        conservative = 2,
        // Run SROA specifically, then fall back to conservative passes.
        sroa_then_conservative = 3,
    };

    // Compact summary of CFG/PDG complexity forwarded to the advisor.
    // All fields are cheaply derived from analyses that the pipeline already
    // computes; no extra work is done when no advisor is attached.
    struct compilation_complexity_summary {
        // ----- CFG metrics -----
        std::size_t block_count = 0;
        std::size_t edge_count = 0;
        std::size_t back_edge_count = 0; // loop back edges
        std::size_t loop_depth_max = 0; // maximum loop nesting depth
        std::size_t unreachable_blocks = 0;

        // ----- Instruction metrics -----
        std::size_t total_instructions = 0;
        std::size_t memory_instructions = 0; // load + store + GEP
        std::size_t call_instructions = 0;
        std::size_t gep_instructions = 0; // SROA candidacy signal

        // ----- Semantic summary -----
        // Aggregate of all semantic_info seen for instructions in the function.
        // Forwarded from the semantic registry when available; default otherwise.
        semantic::semantic_info aggregate_semantic{};

        // ----- PDG metrics (optional — populated only when PDG was computed) -----
        std::optional<std::size_t> pdg_data_edge_count;
        std::optional<std::size_t> pdg_control_edge_count;

        // Convenience: total instruction-level data + control dependences.
        [[nodiscard]] std::optional<std::size_t> pdg_total_edges() const noexcept {
            if (!pdg_data_edge_count || !pdg_control_edge_count) return std::nullopt;
            return *pdg_data_edge_count + *pdg_control_edge_count;
        }

        // Heuristic complexity score in [0, ∞).  Higher ↔ more benefit from
        // heavy passes.  Advisors that do not have their own scoring model can
        // use this as a cheap baseline feature.
        [[nodiscard]] double heuristic_score() const noexcept {
            const double loop_weight = 4.0 * static_cast<double>(back_edge_count)
                + 2.0 * static_cast<double>(loop_depth_max);
            const double gep_weight = 3.0 * static_cast<double>(gep_instructions);
            const double cfg_weight = static_cast<double>(block_count)
                + 0.5 * static_cast<double>(edge_count);
            return loop_weight + gep_weight + cfg_weight;
        }
    };

    // Build a compilation_complexity_summary from the analyses already
    // available in the pass context.  O(n) scan; re-uses cached results.
    // `pdg_data_edges` / `pdg_ctrl_edges` are optional raw counts forwarded
    // from a pdg_build_result when available — we use raw counts rather than
    // a pointer to the PDG type so the function is defined before the
    // lithe::pdg namespace is opened later in this header.
    [[nodiscard]] inline compilation_complexity_summary
    build_complexity_summary(
        const mir::physical_mir_function& fn,
        const cfg_analysis_result& cfg,
        const semantic::semantic_info& aggregate_semantic = {},
        const std::optional<std::size_t> pdg_data_edges = std::nullopt,
        const std::optional<std::size_t> pdg_ctrl_edges = std::nullopt,
        const loop_analysis_result* loops = nullptr
    ) noexcept {
        compilation_complexity_summary s;
        s.block_count = fn.function.blocks.size();
        s.edge_count = cfg.edges.size();
        s.aggregate_semantic = aggregate_semantic;

        for (const auto& blk : fn.function.blocks) {
            for (const auto& inst : blk.instructions) {
                ++s.total_instructions;
                if (inst.op == opcode::load || inst.op == opcode::store)
                    ++s.memory_instructions;
                if (inst.op == opcode::call || inst.op == opcode::indirect_call)
                    ++s.call_instructions;
                if (inst.op == opcode::get_element_ptr)
                    ++s.gep_instructions;
            }
        }

        s.unreachable_blocks = cfg.unreachable_blocks.size();
        s.pdg_data_edge_count = pdg_data_edges;
        s.pdg_control_edge_count = pdg_ctrl_edges;

        if (loops) {
            // Count all back edges across all loops.
            for (const auto& loop : loops->loops) {
                s.back_edge_count += loop.back_edges.size();
            }
            // Use loop count as a coarse depth proxy when no nesting info is present.
            s.loop_depth_max = loops->loops.size();
        }

        return s;
    }

    // -----------------------------------------------------------------------
    // OptimizationAdvisor concept
    //
    // A type T satisfies OptimizationAdvisor if it provides exactly two
    // const-qualified methods:
    //
    //   choose_pipeline(summary, semantic) → advisor_pipeline_choice
    //     Called once per compilation unit.  Returns the pipeline variant the
    //     advisor recommends given the CFG/PDG complexity summary and the
    //     aggregate semantic context of the function.
    //
    //   should_run_pass(pass_name, summary) → bool
    //     Called per-pass inside the heavy pipeline.  Allows fine-grained
    //     gating of individual passes (e.g. disable SROA for a specific hot
    //     function even when heavy_aggressive was chosen overall).
    //
    // Both methods must be callable on a `const T &`.
    // -----------------------------------------------------------------------
    template <typename T>
    concept OptimizationAdvisor =
        requires(const T& advisor,
                 const compilation_complexity_summary& summary,
                 const semantic::semantic_info& sem,
                 std::string_view pass_name) {
            {
                advisor.choose_pipeline(summary, sem)
            }
            -> std::convertible_to<advisor_pipeline_choice>;

            {
                advisor.should_run_pass(pass_name, summary)
            }
            -> std::convertible_to<bool>;
        };

    // -----------------------------------------------------------------------
    // noop_optimization_advisor
    //
    // Satisfies OptimizationAdvisor; always chooses fast_tiered and approves
    // every pass.  Used as the default template argument wherever an
    // OptimizationAdvisor is optional.
    // -----------------------------------------------------------------------
    struct noop_optimization_advisor {
        [[nodiscard]] constexpr advisor_pipeline_choice
        choose_pipeline(const compilation_complexity_summary&,
                        const semantic::semantic_info&) const noexcept {
            return advisor_pipeline_choice::fast_tiered;
        }

        [[nodiscard]] constexpr bool
        should_run_pass(std::string_view, const compilation_complexity_summary&) const noexcept {
            return true;
        }
    };

    static_assert(OptimizationAdvisor<noop_optimization_advisor>,
                  "noop_optimization_advisor must satisfy OptimizationAdvisor");

    // -----------------------------------------------------------------------
    // threshold_optimization_advisor
    //
    // A concrete, dependency-free advisor that makes its decision entirely
    // from the heuristic_score() field of the complexity summary.  Useful as
    // a drop-in default for pipelines that want automatic SROA escalation
    // without connecting an external inference engine.
    //
    // heavy_threshold   — heuristic_score() values above this trigger
    //                     heavy_aggressive.  Default: 32.0 (empirical).
    // sroa_gep_min      — minimum gep_instructions count to enable SROA even
    //                     when the overall score is below heavy_threshold.
    // disabled_passes   — individual passes the caller wants suppressed (e.g.
    //                     {"sroa_pass"} to test the pipeline without SROA).
    // -----------------------------------------------------------------------
    struct threshold_optimization_advisor {
        double heavy_threshold = 32.0;
        std::size_t sroa_gep_min = 4;
        std::unordered_set<std::string> disabled_passes;

        [[nodiscard]] advisor_pipeline_choice
        choose_pipeline(const compilation_complexity_summary& s,
                        const semantic::semantic_info&) const noexcept {
            if (s.heuristic_score() >= heavy_threshold)
                return advisor_pipeline_choice::heavy_aggressive;
            if (s.gep_instructions >= sroa_gep_min)
                return advisor_pipeline_choice::sroa_then_conservative;
            return advisor_pipeline_choice::fast_tiered;
        }

        [[nodiscard]] bool
        should_run_pass(const std::string_view name,
                        const compilation_complexity_summary&) const noexcept {
            return !disabled_passes.contains(std::string(name));
        }
    };

    static_assert(OptimizationAdvisor<threshold_optimization_advisor>,
                  "threshold_optimization_advisor must satisfy OptimizationAdvisor");

    // -----------------------------------------------------------------------
    // easy_rules_optimization_advisor
    //
    // Satisfies OptimizationAdvisor. All decisions are driven through a
    // user-configurable easy_rules::EasyRuleEngine so callers can inject
    // custom pipeline-selection rules at runtime without subclassing.
    //
    // Default rules replicate threshold_optimization_advisor behaviour:
    //   heuristic_score >= 32  → heavy_aggressive
    //   gep_instructions >= 4  → sroa_then_conservative  (if not already heavy)
    //
    // Users can register additional rules on the public `engine` member before
    // first use, or call engine.when(...) at any point to add/change policy.
    // -----------------------------------------------------------------------
    struct easy_rules_optimization_advisor {
        mutable easy_rules::EasyRuleEngine engine;

        easy_rules_optimization_advisor() { register_defaults_(); }

        [[nodiscard]] advisor_pipeline_choice
        choose_pipeline(const compilation_complexity_summary& s,
                        const semantic::semantic_info&) const {
            easy_rules::ExecutionContext ctx;
            ctx.facts.set("heuristic_score", s.heuristic_score());
            ctx.facts.set("gep_instructions", static_cast<int>(s.gep_instructions));
            ctx.facts.set("loop_depth", static_cast<int>(s.loop_depth_max));
            ctx.facts.set("back_edges", static_cast<int>(s.back_edge_count));
            ctx.facts.set("total_instructions", static_cast<int>(s.total_instructions));
            ctx.facts.set("call_count", static_cast<int>(s.call_instructions));
            ctx.facts.set("pipeline", std::string("fast_tiered"));
            engine.run(ctx);
            const auto p = ctx.facts.get_or<std::string>("pipeline", "fast_tiered");
            if (p == "heavy_aggressive") return advisor_pipeline_choice::heavy_aggressive;
            if (p == "sroa_then_conservative") return advisor_pipeline_choice::sroa_then_conservative;
            return advisor_pipeline_choice::fast_tiered;
        }

        [[nodiscard]] bool
        should_run_pass(std::string_view name,
                        const compilation_complexity_summary& s) const {
            easy_rules::ExecutionContext ctx;
            ctx.facts.set("pass_name", std::string(name));
            ctx.facts.set("heuristic_score", s.heuristic_score());
            ctx.facts.set("total_instructions", static_cast<int>(s.total_instructions));
            ctx.facts.set("run_pass", true);
            engine.run(ctx);
            return ctx.facts.get_or("run_pass", true);
        }

    private:
        void register_defaults_() {
            using easy_rules::dsl::fact;

            engine.when("heavy_aggressive_threshold",
                        fact<double>("heuristic_score") >= 32.0)
                  .then([](easy_rules::ExecutionContext& c) {
                      c.facts.set("pipeline", std::string("heavy_aggressive"));
                  })
                  .with_priority(10)
                  .with_description("use heavy_aggressive when heuristic_score >= 32");

            engine.when("sroa_threshold",
                        fact<int>("gep_instructions") >= 4)
                  .then([](easy_rules::ExecutionContext& c) {
                      if (c.facts.get_or<std::string>("pipeline", "fast_tiered") == "fast_tiered")
                          c.facts.set("pipeline", std::string("sroa_then_conservative"));
                  })
                  .with_priority(5)
                  .with_description("use sroa_then_conservative when gep_instructions >= 4");
        }
    };

    static_assert(OptimizationAdvisor<easy_rules_optimization_advisor>,
                  "easy_rules_optimization_advisor must satisfy OptimizationAdvisor");

    // -----------------------------------------------------------------------
    // make_advised_pipeline
    //
    // Builds a mir_pass_pipeline according to the advisor's recommendation.
    // Passes that are vetoed by should_run_pass() are silently omitted.
    // The peephole options from the caller propagate into all peephole passes.
    // -----------------------------------------------------------------------
    template <OptimizationAdvisor Advisor>
    [[nodiscard]] inline mir_pass_pipeline
    make_advised_pipeline(
        const Advisor& advisor,
        const compilation_complexity_summary& summary,
        const semantic::semantic_info& sem,
        const peephole_options& opts = {}
    ) {
        const advisor_pipeline_choice choice = advisor.choose_pipeline(summary, sem);

        auto gate = [&](std::string_view name) -> bool {
            return advisor.should_run_pass(name, summary);
        };

        mir_pass_pipeline pipeline;

        // Common prefix: CFG cleanup passes are always beneficial and light.
        if (gate("trivial_jump_threading_pass"))
            pipeline.add_pass("trivial_jump_threading_pass", trivial_jump_threading_pass{});
        if (gate("empty_block_merge_pass"))
            pipeline.add_pass("empty_block_merge_pass", empty_block_merge_pass{});
        if (gate("unreachable_block_elimination_pass"))
            pipeline.add_pass("unreachable_block_elimination_pass", unreachable_block_elimination_pass{});

        switch (choice) {
        case advisor_pipeline_choice::heavy_aggressive:
            if (gate("sroa_pass"))
                pipeline.add_pass("sroa_pass", sroa_pass{});
            if (gate("constant_propagation_pass"))
                pipeline.add_pass("constant_propagation_pass", constant_propagation_pass{});
            if (gate("copy_propagation_pass"))
                pipeline.add_pass("copy_propagation_pass", copy_propagation_pass{});
            if (gate("common_subexpression_elimination_pass"))
                pipeline.add_pass("common_subexpression_elimination_pass",
                                  common_subexpression_elimination_pass{});
            if (gate("dead_def_elimination_pass"))
                pipeline.add_pass("dead_def_elimination_pass", dead_def_elimination_pass{});
            if (gate("peephole_mir_pass"))
                pipeline.add_pass("peephole_mir_pass", make_peephole_mir_pass(opts));
            if (gate("unreachable_block_elimination_pass_2"))
                pipeline.add_pass("unreachable_block_elimination_pass_2",
                                  unreachable_block_elimination_pass{});
            break;

        case advisor_pipeline_choice::sroa_then_conservative:
            if (gate("sroa_pass"))
                pipeline.add_pass("sroa_pass", sroa_pass{});
            if (gate("copy_propagation_pass"))
                pipeline.add_pass("copy_propagation_pass", copy_propagation_pass{});
            if (gate("dead_def_elimination_pass"))
                pipeline.add_pass("dead_def_elimination_pass", dead_def_elimination_pass{});
            [[fallthrough]];

        case advisor_pipeline_choice::conservative:
            if (gate("peephole_mir_pass"))
                pipeline.add_pass("peephole_mir_pass", make_peephole_mir_pass(opts));
            break;

        case advisor_pipeline_choice::fast_tiered:
        default:
            // CFG cleanup only — already added in the common prefix.
            break;
        }

        return pipeline;
    }

    // -----------------------------------------------------------------------
    // Group L — Feedback-directed compilation loop
    //
    // Scaffolding for iterative AOT/JIT/compiler research workflows.
    // Normal compile_to_physical_mir is untouched; this path is opt-in.
    //
    // Conceptual loop (per iteration i):
    //   1. compile(expr, options_for_iteration_i)
    //   2. caller-supplied trace_fn runs the artifact (or simulates it)
    //      and returns an instrumentation_trace
    //   3. build_feedback_profile(trace) → optimization_feedback_profile
    //   4. advisor.observe_profile(profile)
    //      advisor.choose_pipeline(context) → ml_optimization_decision
    //   5. apply decision to derive options for iteration i+1
    //   6. score_fn(result, profile) → double; keep best-scoring result
    //
    // Neither a benchmark runner nor ML library is required.
    // -----------------------------------------------------------------------

    // Per-iteration record kept by the loop.
    struct feedback_iteration_record {
        std::uint32_t iteration = 0;
        codegen_options options_used;
        codegen_result compile_result;
        optimization_feedback_profile profile;
        ml_optimization_decision decision;
        double score = 0.0;
    };

    // Options controlling feedback_compile.
    struct feedback_compile_options {
        // Base compilation options applied on the first iteration.
        codegen_options base_options;

        // Maximum number of recompilation iterations (1 = compile once, no loop).
        std::uint32_t max_iterations = 1;

        // Optional advisor; when empty the loop runs with no ML guidance.
        ml_advisor_handle advisor;

        // Caller-supplied trace source: receives the codegen_result produced in
        // this iteration and returns an instrumentation_trace.  When not set the
        // loop uses an empty trace (profile will be empty, scores will be 0).
        std::function<instrumentation_trace(const codegen_result&)> trace_fn;

        // Caller-supplied scoring function: higher is better.  Receives the
        // compile result and the profile built from the trace for this iteration.
        // When not set every iteration scores 0.0 and the last result is kept.
        std::function<double(const codegen_result&,
                             const optimization_feedback_profile&)> score_fn;

        // Called after each iteration with the full record; useful for logging
        // or driving external tooling.  Optional.
        std::function<void(const feedback_iteration_record&)> on_iteration;

        // Stop early if score improvement between consecutive iterations falls
        // below this threshold.  Set to 0.0 (default) to always run all iterations.
        double convergence_threshold = 0.0;
    };

    // Aggregated result returned by feedback_compile.
    struct feedback_compile_result {
        // Result of the iteration with the highest score (or last if scores tie).
        codegen_result best_result;
        // Options that produced best_result.
        codegen_options best_options;
        // Profile from the best iteration.
        optimization_feedback_profile best_profile;
        // Score assigned to best_result.
        double best_score = 0.0;
        // Iteration index (0-based) at which best_result was produced.
        std::uint32_t best_iteration = 0;
        // Complete per-iteration history.
        std::vector<feedback_iteration_record> history;
        // Accumulated diagnostics across all iterations.
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }

        [[nodiscard]] std::uint32_t iterations_run() const {
            return static_cast<std::uint32_t>(history.size());
        }
    };

    // feedback_compile: opt-in iterative compilation loop.
    // Does NOT touch compile_to_physical_mir.  The caller drives execution via
    // feedback_compile_options::trace_fn; without it the loop is a dry run that
    // still exercises the advisor/scoring hook wiring.
    template <class Expr>
    [[nodiscard]] feedback_compile_result
    feedback_compile(const Expr& expr, const feedback_compile_options opts) {
        feedback_compile_result out;

        const std::uint32_t max_iter = opts.max_iterations == 0 ? 1 : opts.max_iterations;
        codegen_options current_options = opts.base_options;
        double prev_score = std::numeric_limits<double>::lowest();

        for (std::uint32_t i = 0; i < max_iter; ++i) {
            feedback_iteration_record rec;
            rec.iteration = i;
            rec.options_used = current_options;

            // Step 1 — compile.
            rec.compile_result = compile_to_physical_mir(expr, current_options);
            if (!rec.compile_result.ok()) {
                for (const auto& d : rec.compile_result.diagnostics)
                    out.diagnostics.push_back("iteration " + std::to_string(i) + ": " + d);
                out.history.push_back(rec);
                break;
            }

            // Step 2 — collect trace (caller-supplied; empty if none).
            instrumentation_trace trace;
            if (opts.trace_fn) {
                trace = opts.trace_fn(rec.compile_result);
            }

            // Step 3 — build feedback profile.
            rec.profile = build_feedback_profile(trace);

            // Step 4 — score this iteration.
            rec.score = opts.score_fn
                            ? opts.score_fn(rec.compile_result, rec.profile)
                            : 0.0;

            // Step 5 — notify advisor, ask for next pipeline shape.
            opts.advisor.observe_profile(rec.profile);
            {
                ml_optimization_context ctx;
                ctx.iteration = i;
                ctx.profile = rec.profile;
                ctx.current_opt_level = current_options.mir_optimization_level;
                ctx.current_pass_order =
                    current_options.custom_mir_pipeline
                        ? current_options.custom_mir_pipeline->pass_names()
                        : std::vector<std::string>{};
                rec.decision = opts.advisor.choose_pipeline(ctx);
            }

            // Step 6 — track best.
            if (out.history.empty() || rec.score >= out.best_score) {
                out.best_result = rec.compile_result;
                out.best_options = current_options;
                out.best_profile = rec.profile;
                out.best_score = rec.score;
                out.best_iteration = i;
            }

            // Fire iteration callback.
            if (opts.on_iteration) opts.on_iteration(rec);
            out.history.push_back(rec);

            // Convergence check (skip on the last iteration — nothing to prepare).
            if (i + 1 < max_iter) {
                const double improvement = rec.score - prev_score;
                if (i > 0
                    && opts.convergence_threshold > 0.0
                    && improvement < opts.convergence_threshold) {
                    break;
                }
                prev_score = rec.score;

                // Apply advisor decision to derive options for next iteration.
                const auto& dec = rec.decision;
                if (dec.opt_level_override) {
                    current_options.with_mir_opt_level(*dec.opt_level_override);
                }
                if (!dec.pass_order.empty() || !dec.enable_passes.empty() || !dec.disable_passes.empty()) {
                    mir_pass_pipeline next_pipeline =
                        current_options.custom_mir_pipeline
                            ? *current_options.custom_mir_pipeline
                            : make_mir_pipeline(current_options.mir_optimization_level,
                                                current_options.peephole);
                    for (const auto& name : dec.enable_passes) next_pipeline.enable_pass(name);
                    for (const auto& name : dec.disable_passes) next_pipeline.disable_pass(name);
                    current_options.with_mir_pipeline(std::move(next_pipeline));
                }
            }
        }

        return out;
    }

    // Overload: build feedback_compile_options inline from base codegen_options.
    template <class Expr>
    [[nodiscard]] feedback_compile_result
    feedback_compile(const Expr& expr, codegen_options base) {
        feedback_compile_options opts;
        opts.base_options = std::move(base);
        return feedback_compile(expr, std::move(opts));
    }

    // -----------------------------------------------------------------------
    // Group M — Artifact scoring model
    //
    // Provides a consistent, extendable way to evaluate and compare compilation
    // outputs across feedback-directed or ML-guided compilation loops.
    //
    // Design:
    //   • artifact_score holds orthogonal sub-scores plus a weighted total.
    //   • artifact_score_weights lets callers tune relative importance without
    //     changing the scoring logic.
    //   • score_artifact() implements a simple heuristic baseline.
    //   • Custom scorers can be composed by building an artifact_score directly
    //     or by overriding individual sub-scores after calling score_artifact().
    // -----------------------------------------------------------------------

    // Per-dimension weights used to compute total_score.
    // All weights default to 1.0; set any weight to 0.0 to ignore that dimension.
    struct artifact_score_weights {
        double compile_time = 1.0;
        double runtime = 1.0;
        double size = 1.0;
        double diagnostic = 1.0; // penalty weight — positive means penalise more
    };

    struct artifact_score {
        // Higher is better for all sub-scores (penalties are negated before storing).
        double compile_time_score = 0.0; // reflects pass efficiency / instruction reduction
        double runtime_score = 0.0; // reflects hot-block / hot-instruction density
        double size_score = 0.0; // reflects artifact payload size
        double diagnostic_penalty = 0.0; // negative value; magnitude = diagnostic count

        // Weighted linear combination; computed by score_artifact or set manually.
        double total_score = 0.0;

        // Recompute total from sub-scores and weights.
        void recompute_total(const artifact_score_weights& w = {}) noexcept {
            total_score = w.compile_time * compile_time_score
                + w.runtime * runtime_score
                + w.size * size_score
                + w.diagnostic * diagnostic_penalty;
        }

        // Comparison: higher total is better.
        [[nodiscard]] bool operator>(const artifact_score& o) const noexcept {
            return total_score > o.total_score;
        }

        [[nodiscard]] bool operator>=(const artifact_score& o) const noexcept {
            return total_score >= o.total_score;
        }

        [[nodiscard]] bool operator<(const artifact_score& o) const noexcept {
            return total_score < o.total_score;
        }
    };

    // Heuristic baseline scorer.
    //
    // compile_time_score — derived from pass_effectiveness in the profile:
    //   reward passes that removed instructions (more removed = higher score),
    //   normalised to [0, 1] by the maximum single-pass removal count seen.
    //
    // runtime_score — derived from block and instruction execution counts:
    //   reward low hot-block ratio (fewer hot blocks relative to total is better
    //   for a fully optimised function).  Falls back to 0 when profile is empty.
    //
    // size_score — penalises large text/binary payloads; score = 1/(1 + bytes/1024).
    //   A 0-byte artifact scores 1.0; larger artifacts approach 0.
    //
    // diagnostic_penalty — -(number of diagnostics in the artifact).
    //
    // All sub-scores are in (-inf, 1]; typical range is [-n, 1].
    [[nodiscard]] inline artifact_score
    score_artifact(const compilation_artifact& artifact,
                   const optimization_feedback_profile& profile,
                   const artifact_score_weights& weights = {}) noexcept {
        artifact_score s;

        // --- compile_time_score -------------------------------------------------
        if (!profile.pass_effectiveness.empty()) {
            std::uint64_t max_removed = 0;
            std::uint64_t total_removed = 0;
            for (const auto& [pass_name, cnt] : profile.pass_effectiveness) {
                (void)pass_name;
                total_removed += cnt;
                if (cnt > max_removed) max_removed = cnt;
            }
            // Normalise: reward total removal, cap at 1.0.
            s.compile_time_score = max_removed > 0
                                       ? std::min(1.0, static_cast<double>(total_removed)
                                                  / static_cast<double>(
                                                      max_removed * profile.pass_effectiveness.size()))
                                       : 0.0;
        }

        // --- runtime_score ------------------------------------------------------
        {
            const auto total_blocks = profile.blocks.size();
            if (total_blocks > 0) {
                const std::size_t hot_blocks = profile.hot_blocks().size();
                // Fewer hot blocks = more uniform execution = better for a fully
                // optimised function.  Score = 1 - hot_ratio.
                s.runtime_score = 1.0 - static_cast<double>(hot_blocks)
                    / static_cast<double>(total_blocks);
            }
            // Blend in instruction-level heat when available.
            const auto total_instr = profile.instructions.size();
            if (total_instr > 0) {
                const std::size_t hot_instr = profile.hot_instructions().size();
                const double instr_score = 1.0 - static_cast<double>(hot_instr)
                    / static_cast<double>(total_instr);
                // Simple average with block-level score (or use instr alone).
                s.runtime_score = total_blocks > 0
                                      ? (s.runtime_score + instr_score) / 2.0
                                      : instr_score;
            }
        }

        // --- size_score ---------------------------------------------------------
        {
            const std::size_t payload_bytes =
                artifact.binary_payload.empty()
                    ? artifact.text_payload.size()
                    : artifact.binary_payload.size();
            // 1 / (1 + KB) — approaches 0 for very large artifacts, 1 for empty.
            s.size_score = 1.0 / (1.0 + static_cast<double>(payload_bytes) / 1024.0);
        }

        // --- diagnostic_penalty -------------------------------------------------
        s.diagnostic_penalty = -static_cast<double>(artifact.diagnostics.size());

        s.recompute_total(weights);
        return s;
    }

    // Overload: score a codegen_result directly using its physical MIR
    // instruction count as a proxy for size when no artifact is available.
    [[nodiscard]] inline artifact_score
    score_result(const codegen_result& result,
                 const optimization_feedback_profile& profile,
                 const artifact_score_weights& weights = {}) noexcept {
        // Build a lightweight proxy artifact from compile diagnostics and MIR size.
        compilation_artifact proxy;
        proxy.kind = artifact_kind::none;
        proxy.diagnostics = result.diagnostics;

        // Encode instruction count as a synthetic payload so size_score is meaningful.
        std::size_t instr_count = 0;
        for (const auto& block : result.physical_mir.function.blocks)
            instr_count += block.instructions.size();
        // Use text_payload length as stand-in (1 byte per instruction, arbitrary unit).
        proxy.text_payload.resize(instr_count, ' ');

        return score_artifact(proxy, profile, weights);
    }

    // -----------------------------------------------------------------------
    // Group N — Target pipeline adapter
    //
    // Bridges the old MachineCodeBackend / emit_function APIs to the newer
    // CodeEmissionTarget / compilation_artifact layer.  Old APIs are
    // preserved unchanged; new helpers are additive.
    //
    //   emit_artifact        — runtime capability check, then drive a
    //                          CodeEmissionTarget from an existing codegen_result.
    //   emit_artifact_static — same, but when a mir_domain_hint<Caps> is provided
    //                          as a template argument the *static* capability gap
    //                          (Target::capabilities() vs Hint::required_capabilities)
    //                          is proven at compile time via static_assert.  Any
    //                          remaining runtime structural checks (no-vregs,
    //                          no-spills, …) still run at runtime.
    //   compile_to_artifact  — one-shot: compile + emit artifact.
    //
    // No dynamic_cast or RTTI is used anywhere in this section.
    // -----------------------------------------------------------------------

    // Drive a CodeEmissionTarget from an already-compiled codegen_result.
    // Returns an artifact whose kind matches Target::traits().produced_artifact.
    // If result.ok() is false, returns a diagnostic artifact without calling emit().
    // When `registry` is non-null, validate_operation_legality is run against
    // the physical MIR before emission; any violations are surfaced as diagnostics
    // and the artifact is returned with kind == artifact_kind::none.
    template <CodeEmissionTarget Target>
    [[nodiscard]] compilation_artifact
    emit_artifact(Target& target, const codegen_result& result,
                  const operation_registry* registry = nullptr) {
        compilation_artifact art;
        art.name = result.physical_mir.function.name;

        if (!result.ok()) {
            art.kind = artifact_kind::none;
            for (const auto& d : result.diagnostics) {
                art.diagnostics.push_back("compile: " + d);
            }
            return art;
        }

        const auto traits = Target::traits();

        // Operation-legality check: only when a registry is provided.
        // Build the requirement from the target's declared operation support;
        // stamp backend_name so violations carry the target's name.
        if (registry) {
            backend_capability_requirement req = traits.operation_requirements;
            if (req.backend_name.empty()) req.backend_name = traits.name;
            const auto legality_ops =
                validate_operation_legality(result.physical_mir, *registry, req);
            if (!legality_ops.ok()) {
                art.kind = artifact_kind::none;
                for (const auto& d : legality_ops.diagnostics) {
                    art.diagnostics.push_back("operation: " + d);
                }
                return art;
            }
        }

        // Validate capabilities against what the function actually needs.
        const auto legality = validate_backend_requirements(result.physical_mir, traits.capabilities);
        if (!legality.ok()) {
            art.kind = artifact_kind::none;
            for (const auto& d : legality.diagnostics) {
                art.diagnostics.push_back("capability: " + d);
            }
            return art;
        }

        return target.emit(result.physical_mir);
    }

    // -----------------------------------------------------------------------
    // emit_artifact_static<Target, Hint>
    //
    // Compile-time capability negotiation.  The Hint template parameter must
    // satisfy IsMirDomainHint (i.e. be a mir_domain_hint<Caps> specialisation).
    // The required capability set is therefore known at instantiation time,
    // which lets us:
    //
    //   1. Emit a static_assert that names every missing feature if the gap
    //      between Hint::required_capabilities and Target::capabilities() is
    //      non-empty.  The program is ill-formed (compile error) rather than a
    //      runtime failure.
    //
    //   2. Use `if constexpr` to skip the runtime validate_backend_requirements
    //      capability-bit path when the static check already proves coverage,
    //      retaining only the structural MIR checks (no-vregs, no-spills, …).
    //
    // Constraints: no dynamic_cast, no RTTI, no virtual dispatch.
    // -----------------------------------------------------------------------
    template <CodeEmissionTarget Target, IsMirDomainHint Hint>
    [[nodiscard]] compilation_artifact
    emit_artifact_static(Target& target, const codegen_result& result,
                         const operation_registry* registry = nullptr) {
        // --- Step 1: compile-time capability gap analysis -------------------
        //
        // Both sides are constexpr values, so the subtraction and the
        // conditional are fully evaluated by the compiler during instantiation.
        constexpr backend_capability_set provided = Target::capabilities();
        constexpr backend_capability_set required = Hint::required_capabilities;
        constexpr backend_capability_set gap = required.missing(provided);

        // If the gap is non-empty the target is structurally incapable of
        // handling this MIR domain.  Make the program ill-formed so the
        // developer gets a clear compile-time error instead of a silent
        // runtime failure.
        static_assert(
            gap.empty(),
            "emit_artifact_static: Target does not provide all capabilities "
            "required by the MIR domain hint.  Check Target::capabilities() "
            "against the mir_domain_hint<Required> template argument.  "
            "Use to_string(backend_feature) to identify the missing features."
        );

        // --- Step 2: early-out on failed compile ----------------------------
        compilation_artifact art;
        art.name = result.physical_mir.function.name;

        if (!result.ok()) {
            art.kind = artifact_kind::none;
            for (const auto& d : result.diagnostics) {
                art.diagnostics.push_back("compile: " + d);
            }
            return art;
        }

        // --- Step 3: optional operation-legality check ----------------------
        if (registry) {
            const auto traits = Target::traits();
            backend_capability_requirement req = traits.operation_requirements;
            if (req.backend_name.empty()) req.backend_name = traits.name;
            const auto legality_ops =
                validate_operation_legality(result.physical_mir, *registry, req);
            if (!legality_ops.ok()) {
                art.kind = artifact_kind::none;
                for (const auto& d : legality_ops.diagnostics) {
                    art.diagnostics.push_back("operation: " + d);
                }
                return art;
            }
        }

        // --- Step 4: structural MIR checks (runtime-only) -------------------
        //
        // The capability-bit path is skipped via `if constexpr` because the
        // static_assert above already proved full coverage.  We only run the
        // structural checks (no virtual registers, no unresolved spills, MIR
        // verification) which depend on the actual MIR contents and cannot be
        // proven at compile time from the domain hint alone.
        if constexpr (gap.empty()) {
            // Capability bits are guaranteed by static_assert.
            // Run only the structural legality checks.
            const auto reqs = required_backend_requirements(result.physical_mir);
            backend_legalization_result structural;
            for (const auto& req : reqs.requirements) {
                switch (req.kind) {
                case backend_requirement_kind::no_virtual_registers:
                    if (contains_virtual_registers(result.physical_mir)) {
                        structural.legal = false;
                        structural.diagnostics.push_back(
                            "legality: virtual registers present in physical MIR");
                    }
                    break;
                case backend_requirement_kind::no_unresolved_spills:
                    if (contains_unresolved_spills(result.physical_mir)) {
                        structural.legal = false;
                        structural.diagnostics.push_back(
                            "legality: unresolved spill operands present in physical MIR");
                    }
                    break;
                case backend_requirement_kind::physical_mir_verified:
                    if (!verify_physical_mir(result.physical_mir).ok()) {
                        structural.legal = false;
                        structural.diagnostics.push_back(
                            "legality: physical MIR failed verification before emission");
                    }
                    break;
                default:
                    // Capability-bit requirements already proven statically.
                    break;
                }
            }
            if (!structural.ok()) {
                art.kind = artifact_kind::none;
                for (const auto& d : structural.diagnostics) {
                    art.diagnostics.push_back("capability: " + d);
                }
                return art;
            }
        }

        return target.emit(result.physical_mir);
    }

    // Compile an expression and immediately emit a compilation_artifact via
    // the given CodeEmissionTarget.  Equivalent to:
    //   emit_artifact(target, compile_to_physical_mir(expr, options))
    // When options.op_registry is set, operation legality is validated before
    // emission.  Legacy (no registry) compile paths are unaffected.
    template <class Expr, CodeEmissionTarget Target>
    [[nodiscard]] compilation_artifact
    compile_to_artifact(const Expr& expr, Target& target,
                        const codegen_options& options = {}) {
        return emit_artifact(target, compile_to_physical_mir(expr, options),
                             options.op_registry);
    }

    // -----------------------------------------------------------------------
    // Monadic compile-and-emit helpers
    //
    // These overloads accept the codegen_expected / std::expected types so
    // callers can chain compilation and emission without try/catch or manual
    // ok()-checks:
    //
    //   auto art = compile_to_physical_mir_expected(expr, opts)
    //                  .and_then([&](const codegen_result &r) {
    //                      return emit_artifact_expected<MyTarget>(tgt, r);
    //                  });
    //   if (!art) { /* handle art.error() */ }
    //
    // emit_artifact_expected     — wraps emit_artifact result into
    //                              std::expected<compilation_artifact, codegen_error>.
    // emit_artifact_static_expected — same for the static-negotiation variant.
    // compile_to_artifact_expected  — full one-shot pipeline.
    // -----------------------------------------------------------------------

    // Wrap a codegen_expected and emit via Target if it holds a value.
    template <CodeEmissionTarget Target>
    [[nodiscard]] std::expected<compilation_artifact, codegen_error>
    emit_artifact_expected(Target& target, const codegen_expected& result,
                           const operation_registry* registry = nullptr) {
        if (!result) {
            return std::unexpected(result.error());
        }
        auto art = emit_artifact(target, *result, registry);
        if (!art.ok()) {
            codegen_error err;
            err.messages = art.diagnostics;
            err.failed_stage = "emit_artifact";
            return std::unexpected(std::move(err));
        }
        return art;
    }

    // Static-check variant: same but uses emit_artifact_static internally.
    template <CodeEmissionTarget Target, IsMirDomainHint Hint>
    [[nodiscard]] std::expected<compilation_artifact, codegen_error>
    emit_artifact_static_expected(Target& target, const codegen_expected& result,
                                  const operation_registry* registry = nullptr) {
        if (!result) {
            return std::unexpected(result.error());
        }
        auto art = emit_artifact_static<Target, Hint>(target, *result, registry);
        if (!art.ok()) {
            codegen_error err;
            err.messages = art.diagnostics;
            err.failed_stage = "emit_artifact_static";
            return std::unexpected(std::move(err));
        }
        return art;
    }

    // One-shot: compile → monadic expected → emit.
    // On any failure returns std::unexpected(codegen_error{…}).
    template <class Expr, CodeEmissionTarget Target>
    [[nodiscard]] std::expected<compilation_artifact, codegen_error>
    compile_to_artifact_expected(const Expr& expr, Target& target,
                                 const codegen_options& options = {}) {
        return compile_to_physical_mir_expected(expr, options)
            .and_then([&](const codegen_result& r)
                -> std::expected<compilation_artifact, codegen_error> {
                    auto art = emit_artifact(target, r, options.op_registry);
                    if (!art.ok()) {
                        codegen_error err;
                        err.messages = art.diagnostics;
                        err.failed_stage = "emit_artifact";
                        return std::unexpected(std::move(err));
                    }
                    return art;
                });
    }

    // -----------------------------------------------------------------------
    // Group P — Execution policy abstraction
    //
    // Unifies compile-time and runtime execution under the same evaluator
    // architecture.  Policy structs carry tags only — no evaluator rewrite.
    //
    // Each policy declares:
    //   storage_tag        — identifies the preferred storage strategy.
    //   allocation_tag     — identifies the preferred allocation strategy.
    //   diagnostics_tag    — identifies diagnostics handling behaviour.
    //   constexpr_capable  — whether this policy allows constexpr evaluation.
    //
    // The ExecutionPolicy concept validates that a type satisfies the minimum
    // interface required by the evaluator.
    //
    // Helper aliases / variables:
    //   is_constexpr_execution<P>  — true_type when P::constexpr_capable
    //   is_runtime_execution<P>    — true_type when !P::constexpr_capable
    // -----------------------------------------------------------------------

    // Storage strategy tags — carried inside each policy.
    struct stack_storage_tag {}; // prefer stack / value storage
    struct heap_storage_tag {}; // prefer heap / dynamic storage
    struct jit_storage_tag {}; // prefer JIT-arena storage

    // Allocation strategy tags.
    struct value_allocation_tag {}; // allocate by value (copy)
    struct pool_allocation_tag {}; // allocate from a pool / arena
    struct jit_allocation_tag {}; // allocate via JIT subsystem

    // Diagnostics strategy tags.
    struct constexpr_diagnostics_tag {}; // collect diagnostics in a constexpr-safe buffer
    struct runtime_diagnostics_tag {}; // collect diagnostics via runtime containers
    struct jit_diagnostics_tag {}; // forward diagnostics to JIT runtime

    // Policy structs — scaffolding only; evaluator rewrite is deferred.
    struct constexpr_execution_policy {
        using storage_tag = stack_storage_tag;
        using allocation_tag = value_allocation_tag;
        using diagnostics_tag = constexpr_diagnostics_tag;
        static constexpr bool constexpr_capable = true;
    };

    struct runtime_execution_policy {
        using storage_tag = heap_storage_tag;
        using allocation_tag = pool_allocation_tag;
        using diagnostics_tag = runtime_diagnostics_tag;
        static constexpr bool constexpr_capable = false;
    };

    struct jit_execution_policy {
        using storage_tag = jit_storage_tag;
        using allocation_tag = jit_allocation_tag;
        using diagnostics_tag = jit_diagnostics_tag;
        static constexpr bool constexpr_capable = false;
    };

    // Concept: satisfied by any type that provides the four required members.
    template <class Policy>
    concept ExecutionPolicy = requires {
        typename Policy::storage_tag;
        typename Policy::allocation_tag;
        typename Policy::diagnostics_tag;
        { Policy::constexpr_capable } -> std::convertible_to<bool>;
    };

    // Helper metafunctions.
    template <ExecutionPolicy Policy>
    using is_constexpr_execution =
    std::bool_constant<Policy::constexpr_capable>;

    template <ExecutionPolicy Policy>
    using is_runtime_execution =
    std::bool_constant<!Policy::constexpr_capable>;

    // -----------------------------------------------------------------------
    // Group Q — Constexpr-safe storage abstraction
    //
    // Replaces direct dependency on runtime-only containers in execution paths.
    //
    //   execution_storage<T>
    //     — uses constexpr-safe vector storage in constant-evaluation context
    //     — delegates to the current runtime-friendly storage otherwise
    //
    //   execution_map<K, V>
    //     — ordered (std::map) in constexpr context (no hashing required)
    //     — unordered (std::unordered_map) at runtime for O(1) lookup
    //
    //   execution_set<K>
    //     — ordered (std::set) in constexpr context
    //     — unordered (std::unordered_set) at runtime
    //
    // Rules: no std::unordered_map/set usage on the constexpr path.
    //        runtime performance path is unaffected.
    //        header-only; no separate translation unit required.
    // -----------------------------------------------------------------------

    template <class T>
    class execution_storage {
    public:
        using value_type = T;
        using size_type = std::size_t;
        using reference = T&;
        using const_reference = const T&;

        constexpr void push_back(const T& v) { data_.push_back(v); }
        constexpr void push_back(T&& v) { data_.push_back(std::move(v)); }

        template <class... Args>
        constexpr reference emplace_back(Args&&... args) {
            return data_.emplace_back(std::forward<Args>(args)...);
        }

        constexpr void clear() noexcept { data_.clear(); }
        constexpr void reserve(size_type n) { data_.reserve(n); }

        [[nodiscard]] constexpr size_type size() const noexcept { return data_.size(); }
        [[nodiscard]] constexpr bool empty() const noexcept { return data_.empty(); }
        [[nodiscard]] constexpr reference operator[](size_type i) { return data_[i]; }
        [[nodiscard]] constexpr const_reference operator[](size_type i) const { return data_[i]; }

        [[nodiscard]] constexpr auto begin() { return data_.begin(); }
        [[nodiscard]] constexpr auto end() { return data_.end(); }
        [[nodiscard]] constexpr auto begin() const { return data_.begin(); }
        [[nodiscard]] constexpr auto end() const { return data_.end(); }

        // Provide a span view of the current contents (read-only).
        [[nodiscard]] constexpr std::span<const T> span() const noexcept {
            return {data_.data(), data_.size()};
        }

    private:
        // std::vector is constexpr since C++20.
        std::vector<T> data_;
    };

    // execution_map: constexpr-safe ordered map (no hashing on constexpr path).
    // At runtime the underlying type is also std::map; callers that need the
    // O(1) amortised cost of unordered_map should use it directly — this
    // adapter is designed for paths that must be constexpr-evaluable.
    template <class K, class V>
    using execution_map = std::map<K, V>;

    // execution_set: constexpr-safe ordered set.
    template <class K>
    using execution_set = std::set<K>;

    // -----------------------------------------------------------------------
    // Group R — if consteval execution branching
    //
    // current_execution_policy_t<IsConsteval> resolves to the appropriate
    // policy type based on a compile-time boolean:
    //   IsConsteval=true  → constexpr_execution_policy
    //   IsConsteval=false → runtime_execution_policy
    //
    // In C++ a single function cannot deduce two different return types from
    // two branches of `if consteval`.  The idiomatic solution is a type alias
    // parameterised on a consteval-determined boolean, paired with a consteval
    // helper that produces that boolean:
    //
    //   consteval bool in_consteval_context() { return true; }
    //   // At runtime the compiler never calls this; it substitutes false.
    //
    // Usage pattern (inside any constexpr function):
    //
    //   using Policy = current_execution_policy_t<in_consteval_context()>;
    //   execution_context<Policy> ctx;
    //   if constexpr (is_constexpr_execution<Policy>::value) {
    //       // constexpr path — use execution_storage / execution_map
    //   } else {
    //       // runtime path — use existing containers unchanged
    //   }
    //
    // execution_context<Policy> bundles a typed policy tag with
    // constexpr-safe storage so callers can pass the active context
    // through a call chain without template parameters on every function.
    //
    // make_execution_context<IsConsteval>() is a convenience factory.
    //
    // Rules: no evaluator rewrite.  Preserves existing runtime behavior.
    //        No JIT path.  No code duplication.
    // -----------------------------------------------------------------------

    // consteval helper: always true when called in a constant-evaluation
    // context; the compiler substitutes false when not in consteval.
    consteval bool in_consteval_context() noexcept { return true; }

    // Type alias that maps the consteval boolean to the correct policy type.
    template <bool IsConsteval>
    using current_execution_policy_t =
    std::conditional_t<IsConsteval, constexpr_execution_policy, runtime_execution_policy>;

    // execution_context bundles the active policy with a diagnostic buffer.
    template <ExecutionPolicy Policy>
    struct execution_context {
        using policy_type = Policy;
        static constexpr bool is_constexpr = Policy::constexpr_capable;

        // constexpr-safe diagnostics buffer (std::vector is constexpr in C++20).
        execution_storage<std::string> diagnostics;

        void add_diagnostic(std::string msg) {
            diagnostics.push_back(std::move(msg));
        }

        [[nodiscard]] bool has_diagnostics() const noexcept {
            return !diagnostics.empty();
        }
    };

    // Factory: deduces the correct execution_context type via in_consteval_context().
    // IsConsteval defaults to false at runtime; constexpr callers pass true explicitly
    // or rely on the consteval default argument evaluation.
    template <bool IsConsteval = false>
    [[nodiscard]] constexpr auto make_execution_context() noexcept {
        return execution_context<current_execution_policy_t<IsConsteval>>{};
    }

    // -----------------------------------------------------------------------
    // constexpr_pipeline_result — returned by mir_pass_pipeline::constexpr_run.
    //
    // Uses execution_storage<std::string> (std::vector wrapper) for diagnostics
    // so the type is valid in constexpr contexts (std::vector is constexpr
    // since C++20).  Statistics are kept as plain counters to avoid any
    // runtime-only containers.
    // -----------------------------------------------------------------------

    struct constexpr_pipeline_result {
        mir::physical_mir_function function;
        execution_storage<std::string> diagnostics;
        std::size_t passes_run = 0;
        std::size_t passes_changed = 0;
        bool changed = false;

        [[nodiscard]] constexpr bool ok() const noexcept { return diagnostics.empty(); }
    };

    // -----------------------------------------------------------------------
    // mir_pass_pipeline::constexpr_run — out-of-line definition.
    //
    // Runs a statically-typed pack of passes in order.  Each pass must
    // satisfy `pass.run(fn, ctx) -> mir_pass_result`.  The runtime passes_
    // list is intentionally ignored; only the explicitly provided Passes are
    // executed, making the call graph fully visible to the constexpr evaluator.
    //
    // Constexpr invariants preserved:
    //   - No std::function dispatch.
    //   - No try/catch (runtime-only exception machinery).
    //   - No verify_physical_mir (calls runtime verification).
    //   - Diagnostics accumulated via execution_storage (constexpr vector).
    //   - Budget check uses plain counter; no map lookups by string.
    //
    // Runtime performance path (Policy = runtime_execution_policy):
    //   - Identical to sequential pass application, zero overhead vs. std::function.
    //   - The existing run() method remains the canonical runtime path.
    // -----------------------------------------------------------------------

    template <class ExecCtx, class... Passes>
    [[nodiscard]] constexpr constexpr_pipeline_result
    mir_pass_pipeline::constexpr_run(
        const mir::physical_mir_function& fn,
        ExecCtx& exec_ctx,
        Passes&&... passes
    ) const {
        constexpr_pipeline_result out;
        out.function = fn;

        mir_pass_context local_ctx;
        local_ctx.verify_after_each_pass = false; // no verification on constexpr path
        local_ctx.enable_trace = false;

        auto run_one = [&](auto&& pass) {
            auto result = pass.run(out.function, local_ctx);
            ++out.passes_run;
            if (result.changed) {
                out.function = std::move(result.function);
                out.changed = true;
                ++out.passes_changed;
            }
            for (auto& diag : result.diagnostics) {
                out.diagnostics.push_back(diag);
                exec_ctx.add_diagnostic(diag);
            }
        };

        (run_one(std::forward<Passes>(passes)), ...);

        return out;
    }

    // =======================================================================
    // Group S — MIR partial evaluator
    //
    // Design: the known-value lattice maps preg_id → typed_constant.
    // typed_constant is recursive and type-aware, so the evaluator handles:
    //
    //   integer    — width-correct wrapping arithmetic (mask to bit_width)
    //   float      — IEEE 754 fold; NaN/inf result refused
    //   boolean    — logical fold + short-circuit absorptions
    //   pointer    — null+offset identity; symbol propagation
    //   vector     — lane-wise scalar fold; splat-aware identities
    //   aggregate  — field-wise fold (element index encoded in operation_attributes)
    //   tensor     — element-wise fold over linearised layout
    //   memory /
    //   token /
    //   graph /
    //   layout /
    //   query /
    //   symbolic   — never foldable; fall through to unknown
    //
    // The pass accepts an optional operation_registry* so that instructions
    // carrying an abstract_operation can be dispatched through the registry's
    // operation_contract for richer type information.
    //
    // Conservative invariants:
    //   - Integer overflow wraps (two's-complement); division/mod by zero refused.
    //   - Float fold refused when result is NaN or ±inf.
    //   - fadd(x, -0.0) refused (IEEE 754 signed-zero).
    //   - fsub(x,  0.0) refused (same reason).
    //   - fmul(x,  0.0) absorption refused (x could be NaN/inf; no type-width proof).
    //   - Pointer arithmetic only allowed when base is null (offset from zero).
    //   - No symbolic solver; unknown operands preserved as-is.
    // =======================================================================

    struct partial_evaluation_result {
        mir::physical_mir_function function;
        std::size_t folded_instructions = 0; // fully folded to load_imm
        std::size_t simplified_to_mov = 0; // identity-simplified to mov
        std::vector<std::string> diagnostics;
        bool changed = false;
    };

    // -----------------------------------------------------------------------
    // Integer helpers
    // -----------------------------------------------------------------------

    // Mask an arbitrary int64 result to the correct bit-width with two's-complement wrap.
    // width==0 means platform-native (64-bit); width>64 treated as 64.
    [[nodiscard]] inline std::int64_t integer_wrap(const std::int64_t v, const std::uint32_t width) noexcept {
        if (width == 0 || width >= 64) return v;
        const std::uint64_t mask = (std::uint64_t{1} << width) - 1u;
        const std::uint64_t uv = static_cast<std::uint64_t>(v) & mask;
        // Sign-extend back.
        const std::uint64_t sign_bit = std::uint64_t{1} << (width - 1);
        return static_cast<std::int64_t>((uv ^ sign_bit) - sign_bit);
    }

    // -----------------------------------------------------------------------
    // Scalar two-operand fold: both lv and rv are known scalars of the same type.
    // Returns a typed_constant on success, an unknown constant to refuse.
    // -----------------------------------------------------------------------
    [[nodiscard]] inline typed_constant
    fold_scalar_binary(const opcode op, const typed_constant& lv, const typed_constant& rv) {
        // ---- integer ----
        // Semantics mirror the interpreter reference (lithe_codegen_interpreter.hpp):
        // two's-complement wrapping for add/sub/mul, arithmetic (sign-preserving) shr,
        // and div/mod guards for r==0 and the INT64_MIN/-1 overflow case.
        if (lv.is_integer() && rv.is_integer()) {
            const std::int64_t l = lv.as_integer();
            const std::int64_t r = rv.as_integer();
            const auto ul = static_cast<std::uint64_t>(l);
            const auto ur = static_cast<std::uint64_t>(r);
            const std::uint32_t w = lv.bit_width ? lv.bit_width : rv.bit_width;
            std::int64_t result = 0;
            switch (op) {
            case opcode::add: result = static_cast<std::int64_t>(ul + ur);
                break;
            case opcode::sub: result = static_cast<std::int64_t>(ul - ur);
                break;
            case opcode::mul: result = static_cast<std::int64_t>(ul * ur);
                break;
            case opcode::div:
                if (r == 0) return typed_constant::make_unknown();
                if (l == std::numeric_limits<std::int64_t>::min() && r == -1) {
                    result = std::numeric_limits<std::int64_t>::min();
                    break;
                }
                result = l / r;
                break;
            case opcode::mod:
                if (r == 0) return typed_constant::make_unknown();
                if (l == std::numeric_limits<std::int64_t>::min() && r == -1) {
                    result = 0;
                    break;
                }
                result = l % r;
                break;
            case opcode::bit_and: result = l & r;
                break;
            case opcode::bit_or: result = l | r;
                break;
            case opcode::bit_xor: result = l ^ r;
                break;
            // Shift count masked & 63 — matches the interpreter and the hardware
            // shift instructions (x86 shl/sar, AArch64 lsl/asr all mask to 6 bits).
            case opcode::shl: result = static_cast<std::int64_t>(ul << (ur & 63u));
                break;
            case opcode::shr: result = l >> static_cast<int>(ur & 63u);
                break;
            case opcode::cmp_eq: return typed_constant::make_bool(l == r);
            case opcode::cmp_ne: return typed_constant::make_bool(l != r);
            case opcode::cmp_lt: return typed_constant::make_bool(l < r);
            case opcode::cmp_le: return typed_constant::make_bool(l <= r);
            case opcode::cmp_gt: return typed_constant::make_bool(l > r);
            case opcode::cmp_ge: return typed_constant::make_bool(l >= r);
            default: return typed_constant::make_unknown();
            }
            return typed_constant::make_integer(integer_wrap(result, w), w);
        }

        // ---- float ----
        if (lv.is_float() && rv.is_float()) {
            const double l = lv.as_float();
            const double r = rv.as_float();
            const std::uint32_t w = lv.bit_width ? lv.bit_width : rv.bit_width;
            double result = 0.0;
            switch (op) {
            case opcode::add: result = l + r;
                break;
            case opcode::sub: result = l - r;
                break;
            case opcode::mul: result = l * r;
                break;
            case opcode::div: result = l / r;
                break;
            case opcode::cmp_eq: return typed_constant::make_bool(l == r);
            case opcode::cmp_ne: return typed_constant::make_bool(l != r);
            case opcode::cmp_lt: return typed_constant::make_bool(l < r);
            case opcode::cmp_le: return typed_constant::make_bool(l <= r);
            case opcode::cmp_gt: return typed_constant::make_bool(l > r);
            case opcode::cmp_ge: return typed_constant::make_bool(l >= r);
            default: return typed_constant::make_unknown();
            }
            if (!std::isfinite(result)) return typed_constant::make_unknown();
            return typed_constant::make_float(result, w);
        }

        // ---- boolean ----
        if (lv.is_bool() && rv.is_bool()) {
            const bool l = lv.as_bool();
            const bool r = rv.as_bool();
            switch (op) {
            case opcode::logical_and: return typed_constant::make_bool(l && r);
            case opcode::logical_or: return typed_constant::make_bool(l || r);
            case opcode::bit_and: return typed_constant::make_bool(l & r);
            case opcode::bit_or: return typed_constant::make_bool(l | r);
            case opcode::bit_xor: return typed_constant::make_bool(l ^ r);
            case opcode::cmp_eq: return typed_constant::make_bool(l == r);
            case opcode::cmp_ne: return typed_constant::make_bool(l != r);
            default: return typed_constant::make_unknown();
            }
        }

        return typed_constant::make_unknown();
    }

    // -----------------------------------------------------------------------
    // Vector / aggregate / tensor fold: lane-wise application of fold_scalar_binary.
    // Both lv and rv must be is_composite() with equal lane count.
    // -----------------------------------------------------------------------
    [[nodiscard]] inline typed_constant
    fold_composite_binary(const opcode op, const typed_constant& lv, const typed_constant& rv) {
        const auto& ll = lv.as_lanes();
        const auto& rl = rv.as_lanes();
        if (ll.size() != rl.size() || ll.empty()) return typed_constant::make_unknown();

        std::vector<typed_constant> result_lanes;
        result_lanes.reserve(ll.size());
        for (std::size_t i = 0; i < ll.size(); ++i) {
            auto lane_result = fold_scalar_binary(op, ll[i], rl[i]);
            if (lane_result.is_unknown()) return typed_constant::make_unknown();
            result_lanes.push_back(std::move(lane_result));
        }

        if (lv.kind == abstract_value_kind::vector)
            return typed_constant::make_vector(std::move(result_lanes));
        if (lv.kind == abstract_value_kind::tensor)
            return typed_constant::make_tensor(std::move(result_lanes), lv.lane_count);
        return typed_constant::make_aggregate(std::move(result_lanes));
    }

    // -----------------------------------------------------------------------
    // Top-level typed fold: dispatches to scalar or composite path.
    // -----------------------------------------------------------------------
    [[nodiscard]] inline typed_constant
    fold_typed_binary(const opcode op, const typed_constant& lv, const typed_constant& rv) {
        // Composite types: vector, tensor, aggregate — lane-wise.
        if (lv.is_composite() && rv.is_composite())
            return fold_composite_binary(op, lv, rv);

        // Scalar types.
        if (!lv.is_unknown() && !rv.is_unknown())
            return fold_scalar_binary(op, lv, rv);

        return typed_constant::make_unknown();
    }

    // -----------------------------------------------------------------------
    // Scalar identity / absorption: one operand known, one unknown.
    //
    // Returns the index of the surviving use operand (identity) or a
    // typed_constant to load (absorption), or nullopt for no rule.
    // -----------------------------------------------------------------------
    enum class partial_simplification_kind : std::uint8_t { identity, absorbing };

    struct partial_simplification {
        partial_simplification_kind kind;
        std::variant<std::size_t, typed_constant> payload;
    };

    [[nodiscard]] inline std::optional<partial_simplification>
    try_scalar_identity_or_absorb(const opcode op,
                                  const bool lhs_known, const typed_constant& lv,
                                  const bool rhs_known, const typed_constant& rv) noexcept {
        using K = partial_simplification_kind;

        // ---- integer ----
        if ((!lhs_known || lv.is_integer()) && (!rhs_known || rv.is_integer())) {
            const std::uint32_t w = lhs_known ? lv.bit_width : rv.bit_width;
            if (op == opcode::add) {
                if (rhs_known && rv.as_integer() == 0) return partial_simplification{K::identity, std::size_t{0}};
                if (lhs_known && lv.as_integer() == 0) return partial_simplification{K::identity, std::size_t{1}};
            }
            if (op == opcode::sub) {
                if (rhs_known && rv.as_integer() == 0) return partial_simplification{K::identity, std::size_t{0}};
            }
            if (op == opcode::mul) {
                if (rhs_known && rv.as_integer() == 1) return partial_simplification{K::identity, std::size_t{0}};
                if (lhs_known && lv.as_integer() == 1) return partial_simplification{K::identity, std::size_t{1}};
                if (rhs_known && rv.as_integer() == 0)
                    return partial_simplification{
                        K::absorbing, typed_constant::make_integer(0, w)
                    };
                if (lhs_known && lv.as_integer() == 0)
                    return partial_simplification{
                        K::absorbing, typed_constant::make_integer(0, w)
                    };
            }
            if (op == opcode::bit_and) {
                // x & 0 -> 0  (absorption)
                if (rhs_known && rv.as_integer() == 0)
                    return partial_simplification{
                        K::absorbing, typed_constant::make_integer(0, w)
                    };
                if (lhs_known && lv.as_integer() == 0)
                    return partial_simplification{
                        K::absorbing, typed_constant::make_integer(0, w)
                    };
                // x & -1 (all-ones) -> x  (identity)
                const std::int64_t all_ones = w == 0 || w >= 64
                                                  ? ~std::int64_t{0}
                                                  : static_cast<std::int64_t>((std::uint64_t{1} << w) - 1u);
                if (rhs_known && rv.as_integer() == all_ones)
                    return partial_simplification
                        {K::identity, std::size_t{0}};
                if (lhs_known && lv.as_integer() == all_ones)
                    return partial_simplification
                        {K::identity, std::size_t{1}};
            }
            if (op == opcode::bit_or) {
                // x | 0 -> x  (identity)
                if (rhs_known && rv.as_integer() == 0) return partial_simplification{K::identity, std::size_t{0}};
                if (lhs_known && lv.as_integer() == 0) return partial_simplification{K::identity, std::size_t{1}};
            }
            if (op == opcode::shl || op == opcode::shr) {
                // x << 0 -> x  or  x >> 0 -> x
                if (rhs_known && rv.as_integer() == 0) return partial_simplification{K::identity, std::size_t{0}};
            }
        }

        // ---- float ----
        if ((!lhs_known || lv.is_float()) && (!rhs_known || rv.is_float())) {
            const std::uint32_t w = lhs_known ? lv.bit_width : rv.bit_width;
            if (op == opcode::add) {
                // add(x, +0.0) -> x  only; add(x, -0.0) refused (signed-zero)
                if (rhs_known && rv.as_float() == 0.0 && !std::signbit(rv.as_float()))
                    return partial_simplification{K::identity, std::size_t{0}};
                if (lhs_known && lv.as_float() == 0.0 && !std::signbit(lv.as_float()))
                    return partial_simplification{K::identity, std::size_t{1}};
            }
            if (op == opcode::mul) {
                if (rhs_known && rv.as_float() == 1.0) return partial_simplification{K::identity, std::size_t{0}};
                if (lhs_known && lv.as_float() == 1.0) return partial_simplification{K::identity, std::size_t{1}};
                // mul(x, 0.0) absorption refused: x could be NaN/inf; no proof available.
                (void)w;
            }
        }

        // ---- boolean / predicate ----
        if ((!lhs_known || lv.is_bool()) && (!rhs_known || rv.is_bool())) {
            if (op == opcode::logical_and || op == opcode::bit_and) {
                if (rhs_known && rv.as_bool() == true) return partial_simplification{K::identity, std::size_t{0}};
                if (lhs_known && lv.as_bool() == true) return partial_simplification{K::identity, std::size_t{1}};
                if (rhs_known && rv.as_bool() == false)
                    return partial_simplification{
                        K::absorbing, typed_constant::make_bool(false)
                    };
                if (lhs_known && lv.as_bool() == false)
                    return partial_simplification{
                        K::absorbing, typed_constant::make_bool(false)
                    };
            }
            if (op == opcode::logical_or || op == opcode::bit_or) {
                if (rhs_known && rv.as_bool() == false) return partial_simplification{K::identity, std::size_t{0}};
                if (lhs_known && lv.as_bool() == false) return partial_simplification{K::identity, std::size_t{1}};
                if (rhs_known && rv.as_bool() == true)
                    return partial_simplification{
                        K::absorbing, typed_constant::make_bool(true)
                    };
                if (lhs_known && lv.as_bool() == true)
                    return partial_simplification{
                        K::absorbing, typed_constant::make_bool(true)
                    };
            }
        }

        // ---- pointer: only null + integer offset identity ----
        // add(null_ptr, 0) -> null_ptr; conservative — no arbitrary arithmetic.
        if ((!lhs_known || lv.is_pointer()) && rhs_known && rv.is_integer()) {
            if (op == opcode::add && rv.as_integer() == 0 && lhs_known && lv.is_null_pointer())
                return partial_simplification{K::identity, std::size_t{0}};
        }

        return std::nullopt;
    }

    // Vector / composite identity / absorption: apply scalar rule lane-by-lane.
    // Returns a simplification only when the rule holds for ALL lanes uniformly.
    [[nodiscard]] inline std::optional<partial_simplification>
    try_composite_identity_or_absorb(const opcode op,
                                     const bool lhs_known, const typed_constant& lv,
                                     const bool rhs_known, const typed_constant& rv) noexcept {
        using K = partial_simplification_kind;

        // Use the splat-lane scalar as a representative.
        const typed_constant* lv_scalar = lhs_known && lv.is_splat() ? &lv.scalar_or_first() : nullptr;
        const typed_constant* rv_scalar = rhs_known && rv.is_splat() ? &rv.scalar_or_first() : nullptr;

        if (!lv_scalar && !rv_scalar) return std::nullopt;

        static const typed_constant unknown_tc{};
        const typed_constant& l_rep = lv_scalar ? *lv_scalar : unknown_tc;
        const typed_constant& r_rep = rv_scalar ? *rv_scalar : unknown_tc;

        auto scalar_simp = try_scalar_identity_or_absorb(op,
                                                         lv_scalar != nullptr, l_rep,
                                                         rv_scalar != nullptr, r_rep);
        if (!scalar_simp) return std::nullopt;

        // For absorbing: produce a splat of the absorbing scalar.
        if (scalar_simp->kind == K::absorbing) {
            const auto& absorb_scalar = std::get<typed_constant>(scalar_simp->payload);
            const std::uint32_t n = lhs_known ? lv.lane_count : rv.lane_count;
            std::vector<typed_constant> lanes(n, absorb_scalar);
            typed_constant absorb_vec;
            absorb_vec.kind = lhs_known ? lv.kind : rv.kind;
            absorb_vec.lane_count = n;
            absorb_vec.bit_width = absorb_scalar.bit_width;
            absorb_vec.payload = std::move(lanes);
            return partial_simplification{K::absorbing, std::move(absorb_vec)};
        }

        // For identity: the surviving operand index is the same as for scalars.
        return partial_simplification{K::identity, std::get<std::size_t>(scalar_simp->payload)};
    }

    // -----------------------------------------------------------------------
    // Emit a load_imm for a typed_constant into an instruction (in-place).
    // Composite types cannot be expressed as a single load_imm; those cases
    // must be handled by the caller (left as unknown / not rewritten).
    // -----------------------------------------------------------------------
    inline bool rewrite_to_load_imm(allocated_instruction& inst, const typed_constant& tc) {
        inst.op = opcode::load_imm;
        if (tc.is_integer()) {
            inst.uses = {allocated_operand::as_i64(tc.as_integer())};
            return true;
        }
        if (tc.is_float()) {
            inst.uses = {allocated_operand::as_f64(tc.as_float())};
            return true;
        }
        if (tc.is_bool()) {
            inst.uses = {allocated_operand::as_i64(tc.as_bool() ? 1 : 0)};
            return true;
        }
        if (tc.is_pointer() && tc.is_null_pointer()) {
            inst.uses = {allocated_operand::as_i64(0)};
            return true;
        }
        if (tc.is_pointer()) {
            inst.uses = {allocated_operand::as_symbol(tc.as_symbol())};
            return true;
        }
        // Composite types: a single load_imm cannot represent them.
        return false;
    }

    // -----------------------------------------------------------------------
    // Seed the known-value lattice from a load_imm or load_symbol instruction.
    // Returns a typed_constant if the instruction seeds a known constant,
    // otherwise an unknown typed_constant.
    // -----------------------------------------------------------------------
    [[nodiscard]] inline typed_constant
    seed_from_instruction(const allocated_instruction& inst) {
        if (inst.op == opcode::load_imm
            && inst.defs.size() == 1
            && inst.uses.size() == 1
            && inst.defs[0].type == allocated_operand::kind::preg) {
            if (inst.uses[0].type == allocated_operand::kind::immediate_i64)
                return typed_constant::make_integer(std::get<std::int64_t>(inst.uses[0].value));
            if (inst.uses[0].type == allocated_operand::kind::immediate_f64)
                return typed_constant::make_float(std::get<double>(inst.uses[0].value));
        }
        if (inst.op == opcode::load_symbol
            && inst.defs.size() == 1
            && inst.uses.size() == 1
            && inst.defs[0].type == allocated_operand::kind::preg
            && inst.uses[0].type == allocated_operand::kind::symbol)
            return typed_constant::make_pointer(std::get<std::string>(inst.uses[0].value));
        return typed_constant::make_unknown();
    }

    // -----------------------------------------------------------------------
    // partial_evaluate: single-pass per-block evaluator.
    //
    // The known-value lattice (preg_id → typed_constant) handles integer,
    // float, boolean, pointer, vector, aggregate, and tensor types uniformly.
    // Types that have no constant representation (memory, token, graph,
    // layout, query, symbolic) fall through to unknown conservatively.
    //
    // registry: optional; used to resolve abstract_operation contracts for
    //           richer type dispatch when the opcode alone is insufficient.
    // -----------------------------------------------------------------------
    [[nodiscard]] inline partial_evaluation_result
    partial_evaluate(const mir::physical_mir_function& fn,
                     const operation_registry* registry = nullptr) {
        partial_evaluation_result out;
        out.function = fn;

        for (auto& block : out.function.function.blocks) {
            std::unordered_map<std::uint32_t, typed_constant> known;

            for (auto& inst : block.instructions) {
                // ---- seed from load_imm / load_symbol ----
                {
                    auto seeded = seed_from_instruction(inst);
                    if (!seeded.is_unknown()
                        && inst.defs.size() == 1
                        && inst.defs[0].type == allocated_operand::kind::preg) {
                        known[std::get<preg>(inst.defs[0].value).id] = std::move(seeded);
                        continue;
                    }
                }

                // ---- propagate mov ----
                if (inst.op == opcode::mov
                    && inst.defs.size() == 1
                    && inst.uses.size() == 1
                    && inst.defs[0].type == allocated_operand::kind::preg
                    && inst.uses[0].type == allocated_operand::kind::preg) {
                    const auto dst_id = std::get<preg>(inst.defs[0].value).id;
                    const auto src_id = std::get<preg>(inst.uses[0].value).id;
                    if (const auto it = known.find(src_id); it != known.end())
                        known[dst_id] = it->second;
                    else
                        known.erase(dst_id);
                    continue;
                }

                // ---- two-operand arithmetic / logic ----
                if (inst.defs.size() == 1
                    && inst.uses.size() == 2
                    && inst.defs[0].type == allocated_operand::kind::preg
                    && inst.uses[0].type == allocated_operand::kind::preg
                    && inst.uses[1].type == allocated_operand::kind::preg) {
                    const auto dst_id = std::get<preg>(inst.defs[0].value).id;
                    const auto lhs_id = std::get<preg>(inst.uses[0].value).id;
                    const auto rhs_id = std::get<preg>(inst.uses[1].value).id;
                    const auto lhs_it = known.find(lhs_id);
                    const auto rhs_it = known.find(rhs_id);
                    const bool lhs_known = lhs_it != known.end();
                    const bool rhs_known = rhs_it != known.end();

                    static const typed_constant unknown_tc{};
                    const typed_constant& lv = lhs_known ? lhs_it->second : unknown_tc;
                    const typed_constant& rv = rhs_known ? rhs_it->second : unknown_tc;

                    // Both known: full fold.
                    if (lhs_known && rhs_known) {
                        auto result = fold_typed_binary(inst.op, lv, rv);
                        if (!result.is_unknown()) {
                            if (rewrite_to_load_imm(inst, result)) {
                                known[dst_id] = std::move(result);
                                ++out.folded_instructions;
                                out.changed = true;
                                continue;
                            }
                            // Composite result can't be a single load_imm; record as known
                            // for downstream passes but don't rewrite the instruction.
                            known[dst_id] = std::move(result);
                            continue;
                        }
                        known.erase(dst_id);
                        continue;
                    }

                    // One side known: identity / absorption.
                    auto try_simp = [&]() -> std::optional<partial_simplification> {
                        if ((lv.is_composite() || rv.is_composite()))
                            return try_composite_identity_or_absorb(inst.op, lhs_known, lv, rhs_known, rv);
                        return try_scalar_identity_or_absorb(inst.op, lhs_known, lv, rhs_known, rv);
                    };

                    if (const auto simp = try_simp()) {
                        if (simp->kind == partial_simplification_kind::identity) {
                            const std::size_t keep = std::get<std::size_t>(simp->payload);
                            inst.op = opcode::mov;
                            inst.uses = {inst.uses[keep]};
                            const auto surviving_id = std::get<preg>(inst.uses[0].value).id;
                            if (const auto kit = known.find(surviving_id); kit != known.end())
                                known[dst_id] = kit->second;
                            else
                                known.erase(dst_id);
                            ++out.simplified_to_mov;
                            out.changed = true;
                        }
                        else {
                            const auto& cv = std::get<typed_constant>(simp->payload);
                            if (rewrite_to_load_imm(inst, cv)) {
                                known[dst_id] = cv;
                                ++out.folded_instructions;
                                out.changed = true;
                            }
                            else {
                                // Composite absorb: record but don't rewrite.
                                known[dst_id] = cv;
                            }
                        }
                        continue;
                    }

                    known.erase(dst_id);
                    continue;
                }

                // ---- single-operand: neg, bit_not, logical_not ----
                if (inst.defs.size() == 1
                    && inst.uses.size() == 1
                    && inst.defs[0].type == allocated_operand::kind::preg
                    && inst.uses[0].type == allocated_operand::kind::preg) {
                    const auto dst_id = std::get<preg>(inst.defs[0].value).id;
                    const auto src_id = std::get<preg>(inst.uses[0].value).id;
                    const auto src_it = known.find(src_id);
                    if (src_it == known.end()) {
                        known.erase(dst_id);
                        continue;
                    }

                    const typed_constant& sv = src_it->second;
                    typed_constant result = typed_constant::make_unknown();

                    if (inst.op == opcode::neg && sv.is_integer()) {
                        // Two's-complement negation via unsigned arithmetic — avoids
                        // signed overflow UB when sv.as_integer() == INT64_MIN.
                        const auto uv = static_cast<std::uint64_t>(sv.as_integer());
                        const auto neg_v = static_cast<std::int64_t>(~uv + 1u);
                        result = typed_constant::make_integer(integer_wrap(neg_v, sv.bit_width),
                                                              sv.bit_width);
                    }
                    else if (inst.op == opcode::neg && sv.is_float()) {
                        const double neg = -sv.as_float();
                        if (std::isfinite(neg)) result = typed_constant::make_float(neg, sv.bit_width);
                    }
                    else if (inst.op == opcode::bit_not && sv.is_integer())
                        result = typed_constant::make_integer(integer_wrap(~sv.as_integer(), sv.bit_width),
                                                              sv.bit_width);
                    else if (inst.op == opcode::logical_not && sv.is_bool())
                        result = typed_constant::make_bool(!sv.as_bool());

                    if (!result.is_unknown()) {
                        if (rewrite_to_load_imm(inst, result)) {
                            known[dst_id] = std::move(result);
                            ++out.folded_instructions;
                            out.changed = true;
                            continue;
                        }
                    }
                    known.erase(dst_id);
                    continue;
                }

                // ---- default: invalidate any preg defs ----
                for (const auto& def_op : inst.defs) {
                    if (def_op.type == allocated_operand::kind::preg)
                        known.erase(std::get<preg>(def_op.value).id);
                }

                // Suppress unused-parameter warning when registry is not yet used.
                (void)registry;
            }
        }

        return out;
    }

    // Pass wrapper: plugs partial_evaluate into the mir_pass_context pipeline.
    struct partial_evaluation_pass {
        const operation_registry* registry = nullptr;

        [[nodiscard]] mir_pass_result run(mir::physical_mir_function const& fn,
                                          mir_pass_context& ctx) const {
            mir_pass_result out;
            auto pe = partial_evaluate(fn, registry);
            out.function = std::move(pe.function);
            out.changed = pe.changed;
            ctx.changed |= pe.changed;
            out.removed_instructions = pe.folded_instructions + pe.simplified_to_mov;
            out.diagnostics = std::move(pe.diagnostics);
            return out;
        }
    };

    // -----------------------------------------------------------------------
    // Group T — Unified execution engine (pipeline-side types)
    //
    // execution_engine<Policy> is defined in
    // backends/lithe_codegen_interpreter.hpp (which includes this file) so
    // that the runtime policy path can reference interpreter_backend without
    // creating a circular include.
    //
    // This group provides only the supporting infrastructure that belongs in
    // the pipeline header:
    //
    //   execution_engine_result  — unified artifact wrapper returned by
    //                              execute() and produce_artifact().
    //
    // The full class template is in backends/lithe_codegen_interpreter.hpp.
    //   constexpr_engine = execution_engine<constexpr_execution_policy>
    //   runtime_engine   = execution_engine<runtime_execution_policy>
    // -----------------------------------------------------------------------

    struct execution_engine_result {
        compilation_artifact artifact;
        std::vector<std::string> engine_diagnostics;
        bool ok() const noexcept { return engine_diagnostics.empty(); }
    };

    // =========================================================================
    // Compatibility aliases)
    //
    // These types are also defined canonically in lithe::execution::foundation.
    // They are kept here for backward compatibility; the static_asserts guard
    // that the two definitions are structurally identical (same type) so that
    // existing lithe::codegen references keep compiling while new code can use
    // lithe::execution:: directly.
    // =========================================================================
    static_assert(sizeof(lithe::execution::backend_capability_set)
                  == sizeof(lithe::codegen::backend_capability_set),
                  "backend_capability_set size mismatch between execution and codegen");
    static_assert(sizeof(lithe::execution::backend_feature)
                  == sizeof(lithe::codegen::backend_feature),
                  "backend_feature size mismatch between execution and codegen");

    // Neutral policy defaults visible from codegen without pulling in lithe::ir.
    using no_ir_integration = lithe::execution::no_ir_integration;
    using no_pipeline_hooks = lithe::execution::no_pipeline_hooks;

    // execution_mode and execution_mode_set are new types; alias them so
    // codegen users can name them without an extra include.
    using execution_mode = lithe::execution::execution_mode;
    using execution_mode_set = lithe::execution::execution_mode_set;

    // Stage error types (not previously in codegen; alias for discovery).
    using compile_error = lithe::execution::compile_error;
    using install_error = lithe::execution::install_error;
    using compile_install_error = lithe::execution::compile_install_error;
    using selection_error = lithe::execution::selection_error;
    using execution_error = lithe::execution::execution_error;
    using ir_error = lithe::execution::ir_error;
    using native_install_unavailable = lithe::execution::native_install_unavailable;
} // namespace lithe::codegen

// Umbrella: pull in the independent namespaces that lived here before the split.
#include "lithe_pdg.hpp"
#include "lithe_poly.hpp"
#include "lithe_codegen_hl_passes.hpp"
#include "lithe_safepoint.hpp"

namespace lithe::codegen {
    // =========================================================================
    // validate_backend_type_legality
    //
    // Walks every abstract_operation attached to instructions in fn, looks up
    // the operation's contract in the registry, and validates each operand and
    // result abstract_value_type against the backend's declared type constraints.
    //
    // Rules:
    //   • Validation only — no MIR rewrites, no lowering.
    //   • Does not touch the existing backend_feature / capability-bit path.
    //   • An empty supported_type_kinds set means "accept all kinds".
    //   • An empty supported_type_ids set means "no type_id restriction".
    //   • An empty supported_vector_widths set means "accept all widths".
    //   • supported_tensor_rank_limit == UINT32_MAX means "unrestricted".
    //   • supports_dynamic_types / supports_symbolic_types are checked when
    //     operands of kind dynamic / symbolic appear in the contracts.
    // =========================================================================

    namespace detail {
        // Map abstract_value_kind → semantic::types::type_kind for display.
        [[nodiscard]] inline std::string_view avk_name(const abstract_value_kind k) noexcept {
            switch (k) {
            case abstract_value_kind::unknown: return "unknown";
            case abstract_value_kind::scalar: return "scalar";
            case abstract_value_kind::integer: return "integer";
            case abstract_value_kind::floating: return "floating";
            case abstract_value_kind::pointer: return "pointer";
            case abstract_value_kind::memory: return "memory";
            case abstract_value_kind::vector: return "vector";
            case abstract_value_kind::tensor: return "tensor";
            case abstract_value_kind::aggregate: return "aggregate";
            case abstract_value_kind::predicate: return "predicate";
            case abstract_value_kind::token: return "token";
            case abstract_value_kind::graph: return "graph";
            case abstract_value_kind::layout: return "layout";
            case abstract_value_kind::query: return "query";
            case abstract_value_kind::symbolic: return "symbolic";
            }
            return "unknown";
        }

        // Map abstract_value_kind to the nearest semantic::types::type_kind.
        [[nodiscard]] inline semantic::types::type_kind avk_to_type_kind(
            const abstract_value_kind k) noexcept {
            using tk = semantic::types::type_kind;
            switch (k) {
            case abstract_value_kind::integer: return tk::integer;
            case abstract_value_kind::floating: return tk::floating;
            case abstract_value_kind::pointer: return tk::pointer;
            case abstract_value_kind::vector: return tk::vector;
            case abstract_value_kind::tensor: return tk::tensor;
            case abstract_value_kind::aggregate: return tk::aggregate;
            case abstract_value_kind::token: return tk::token;
            case abstract_value_kind::layout: return tk::layout;
            case abstract_value_kind::query: return tk::query;
            case abstract_value_kind::symbolic: return tk::symbolic;
            default: return tk::unknown;
            }
        }

        // Validate one abstract_value_type slot against the requirement.
        // Appends diagnostics to `out` in-place.
        inline void validate_avt(
            const abstract_value_type& avt,
            const backend_capability_requirement& req,
            const std::string& context,
            backend_capability_legalization_result& out) {
            const auto kind = avt.kind;

            // ---- supported_type_kinds (mapped from abstract_value_kind) ----
            if (!req.supported_type_kinds.empty()) {
                const auto tk = static_cast<int>(avk_to_type_kind(kind));
                if (!req.supported_type_kinds.count(tk)) {
                    out.unsupported_type_kinds.push_back(avk_to_type_kind(kind));
                    out.diagnostics.push_back(
                        context + "unsupported type kind '" +
                        std::string{avk_name(kind)} + "'");
                }
            }

            // ---- supported_type_ids (matched via semantic_type name) --------
            // type_ids are cross-referenced only when the supported_type_ids
            // set is non-empty; we look up by semantic_type name in the registry.
            if (!req.supported_type_ids.empty() && !avt.semantic_type.empty()) {
                // The type_ids set contains numeric ids; we need a name→id bridge.
                // We accept the type if its name resolves to a whitelisted id.
                const auto& type_reg = semantic::types::type_registry();
                if (auto desc = type_reg.find_type(avt.semantic_type); desc.has_value()) {
                    if (!req.supported_type_ids.count(desc->id)) {
                        out.unsupported_type_ids.push_back(desc->id);
                        out.diagnostics.push_back(
                            context + "type '" + avt.semantic_type +
                            "' (id=" + std::to_string(desc->id) +
                            ") not in supported_type_ids");
                    }
                }
            }

            // ---- dynamic types ----------------------------------------------
            // abstract_value_kind has no "dynamic" — we use semantic_type name.
            if (!req.supports_dynamic_types) {
                bool is_dynamic = false;
                if (!avt.semantic_type.empty()) {
                    const auto& type_reg = semantic::types::type_registry();
                    if (auto desc = type_reg.find_type(avt.semantic_type); desc.has_value()) {
                        is_dynamic = (desc->kind == semantic::types::type_kind::dynamic);
                    }
                }
                if (is_dynamic) {
                    out.dynamic_type_rejected = true;
                    out.diagnostics.push_back(
                        context + "dynamic type '" + avt.semantic_type +
                        "' rejected (supports_dynamic_types = false)");
                }
            }

            // ---- symbolic types ---------------------------------------------
            if (!req.supports_symbolic_types &&
                kind == abstract_value_kind::symbolic) {
                out.symbolic_type_rejected = true;
                out.diagnostics.push_back(
                    context + "symbolic operand rejected (supports_symbolic_types = false)");
            }

            // ---- query / layout rejections via supported_type_kinds ---------
            // Already covered above by the kind allow-list; no extra flag needed.

            // ---- vector width -----------------------------------------------
            if (kind == abstract_value_kind::vector &&
                !req.supported_vector_widths.empty() &&
                avt.bit_width != 0) {
                if (!req.supported_vector_widths.count(avt.bit_width)) {
                    out.unsupported_vector_widths.push_back(avt.bit_width);
                    out.diagnostics.push_back(
                        context + "vector width " + std::to_string(avt.bit_width) +
                        " bits not in supported_vector_widths");
                }
            }

            // ---- tensor rank ------------------------------------------------
            if (kind == abstract_value_kind::tensor) {
                const auto rank = static_cast<std::uint32_t>(avt.shape.size());
                if (rank > req.supported_tensor_rank_limit) {
                    out.oversized_tensor_ranks.push_back(rank);
                    out.diagnostics.push_back(
                        context + "tensor rank " + std::to_string(rank) +
                        " exceeds supported_tensor_rank_limit=" +
                        std::to_string(req.supported_tensor_rank_limit));
                }
            }
        }
    } // namespace detail

    [[nodiscard]] inline backend_capability_legalization_result validate_backend_type_legality(
        const mir::physical_mir_function& fn,
        const backend_capability_requirement& req,
        const operation_registry& op_reg,
        const semantic::types::semantic_type_registry& /*type_reg*/) {
        backend_capability_legalization_result out;
        const std::string prefix = req.backend_name.empty()
                                       ? "type_legality: "
                                       : req.backend_name + " type_legality: ";

        // Determine whether any type-legality fields are actually constrained.
        const bool has_kind_constraint = !req.supported_type_kinds.empty();
        const bool has_id_constraint = !req.supported_type_ids.empty();
        const bool has_width_constraint = !req.supported_vector_widths.empty();
        const bool has_rank_constraint =
            req.supported_tensor_rank_limit != std::numeric_limits<std::uint32_t>::max();
        const bool has_dynamic_deny = !req.supports_dynamic_types;
        const bool has_symbolic_deny = !req.supports_symbolic_types;

        if (!has_kind_constraint && !has_id_constraint && !has_width_constraint &&
            !has_rank_constraint && !has_dynamic_deny && !has_symbolic_deny) {
            return out; // nothing to check
        }

        // Walk every instruction that carries an abstract_operation reference.
        // Look up its contract to obtain operand/result abstract_value_types.
        const auto& reg = op_reg;

        for (const auto& blk : fn.function.blocks) {
            for (const auto& inst : blk.instructions) {
                if (!inst.abstract_operation.has_value()) continue;

                const auto* desc = reg.find(*inst.abstract_operation);
                if (!desc) continue;

                const std::string ctx = prefix + "op '" +
                    inst.abstract_operation->domain + '.' +
                    inst.abstract_operation->name + "' bb" +
                    std::to_string(blk.id) + " inst" +
                    std::to_string(inst.id) + ": ";

                for (const auto& avt : desc->contract.operands) {
                    detail::validate_avt(avt, req, ctx + "operand: ", out);
                }
                for (const auto& avt : desc->contract.results) {
                    detail::validate_avt(avt, req, ctx + "result: ", out);
                }
            }
        }

        return out;
    }

    // Wire validate_backend_type_legality into validate_backend_capabilities.
    // The existing requirement-based overload is extended: after all existing
    // checks pass (unchanged), the type-legality sub-check is applied when at
    // least one type-legality field deviates from its default.
    [[nodiscard]] inline backend_capability_legalization_result
    validate_backend_capabilities_with_types(
        const mir::physical_mir_function& fn,
        const backend_capability_requirement& req,
        const operation_registry& op_reg,
        const semantic::types::semantic_type_registry& type_reg
            = semantic::types::type_registry()) {
        auto out = validate_backend_capabilities(fn, req);

        const bool needs_type_check =
            !req.supported_type_kinds.empty() ||
            !req.supported_type_ids.empty() ||
            !req.supported_vector_widths.empty() ||
            req.supported_tensor_rank_limit !=
            std::numeric_limits<std::uint32_t>::max() ||
            !req.supports_dynamic_types ||
            !req.supports_symbolic_types;

        if (!needs_type_check) return out;

        auto type_result = validate_backend_type_legality(fn, req, op_reg, type_reg);

        out.diagnostics.insert(out.diagnostics.end(),
                               type_result.diagnostics.begin(), type_result.diagnostics.end());
        out.unsupported_type_ids.insert(out.unsupported_type_ids.end(),
                                        type_result.unsupported_type_ids.begin(),
                                        type_result.unsupported_type_ids.end());
        out.unsupported_type_kinds.insert(out.unsupported_type_kinds.end(),
                                          type_result.unsupported_type_kinds.begin(),
                                          type_result.unsupported_type_kinds.end());
        out.unsupported_vector_widths.insert(out.unsupported_vector_widths.end(),
                                             type_result.unsupported_vector_widths.begin(),
                                             type_result.unsupported_vector_widths.end());
        out.oversized_tensor_ranks.insert(out.oversized_tensor_ranks.end(),
                                          type_result.oversized_tensor_ranks.begin(),
                                          type_result.oversized_tensor_ranks.end());
        out.dynamic_type_rejected = out.dynamic_type_rejected ||
            type_result.dynamic_type_rejected;
        out.symbolic_type_rejected = out.symbolic_type_rejected ||
            type_result.symbolic_type_rejected;

        return out;
    }

    // -----------------------------------------------------------------------
    // Backend-aware semantic specialization pipeline integration
    //
    // Bridges semantic::semantic_specialization_context to the MIR codegen
    // pipeline.  The functions below run semantic specialization as a pre-pass
    // before the main MIR pipeline so that illegal operations / types are
    // rejected at the semantic IR level rather than during MIR lowering.
    // -----------------------------------------------------------------------

    // Result of running semantic specialization over a codegen unit.
    struct pipeline_semantic_specialization_result {
        // Per-node results keyed by structural hash.
        std::unordered_map<structural_hash_t,
                           semantic::specialization_result> per_node;

        // Aggregate diagnostics from all nodes.
        std::vector<std::string> diagnostics;

        // True if all nodes are legal under the specialization context.
        bool all_legal = true;

        void merge(const structural_hash_t id,
                   semantic::specialization_result r) {
            if (!r.legal) {
                all_legal = false;
                diagnostics.insert(diagnostics.end(),
                                   r.diagnostics.begin(), r.diagnostics.end());
            }
            per_node[id] = std::move(r);
        }
    };

    // Applies semantic::specialize_for_backend to every entry in `reg` that
    // appears in `node_ids`.  Results are merged back into `reg`.
    [[nodiscard]] inline pipeline_semantic_specialization_result
    specialize_semantic_registry_for_backend(
        semantic::semantic_registry& reg,
        const std::vector<structural_hash_t>& node_ids,
        const semantic::semantic_specialization_context& ctx) {
        pipeline_semantic_specialization_result out;
        for (const auto id : node_ids) {
            auto info_opt = reg.get(id);
            if (!info_opt.has_value()) continue;
            auto result = semantic::specialize_for_backend(*info_opt, ctx);
            reg.merge(id, result.specialized, semantic::semantic_resolution{});
            out.merge(id, std::move(result));
        }
        return out;
    }

    // Applies semantic::specialize_for_constexpr to every entry in `reg`.
    [[nodiscard]] inline pipeline_semantic_specialization_result
    specialize_semantic_registry_for_constexpr(
        semantic::semantic_registry& reg,
        const std::vector<structural_hash_t>& node_ids,
        const semantic::semantic_specialization_context& ctx) {
        pipeline_semantic_specialization_result out;
        for (const auto id : node_ids) {
            auto info_opt = reg.get(id);
            if (!info_opt.has_value()) continue;
            auto result = semantic::specialize_for_constexpr(*info_opt, ctx);
            reg.merge(id, result.specialized, semantic::semantic_resolution{});
            out.merge(id, std::move(result));
        }
        return out;
    }

    // Applies semantic::specialize_for_runtime to every entry in `reg`.
    [[nodiscard]] inline pipeline_semantic_specialization_result
    specialize_semantic_registry_for_runtime(
        semantic::semantic_registry& reg,
        const std::vector<structural_hash_t>& node_ids,
        const semantic::semantic_specialization_context& ctx) {
        pipeline_semantic_specialization_result out;
        for (const auto id : node_ids) {
            auto info_opt = reg.get(id);
            if (!info_opt.has_value()) continue;
            auto result = semantic::specialize_for_runtime(*info_opt, ctx);
            reg.merge(id, result.specialized, semantic::semantic_resolution{});
            out.merge(id, std::move(result));
        }
        return out;
    }

    // Applies semantic::specialize_for_jit to every entry in `reg`.
    [[nodiscard]] inline pipeline_semantic_specialization_result
    specialize_semantic_registry_for_jit(
        semantic::semantic_registry& reg,
        const std::vector<structural_hash_t>& node_ids,
        const semantic::semantic_specialization_context& ctx) {
        pipeline_semantic_specialization_result out;
        for (const auto id : node_ids) {
            auto info_opt = reg.get(id);
            if (!info_opt.has_value()) continue;
            auto result = semantic::specialize_for_jit(*info_opt, ctx);
            reg.merge(id, result.specialized, semantic::semantic_resolution{});
            out.merge(id, std::move(result));
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // Staged execution specialization pipeline integration
    //
    // Applies semantic::infer_execution_stage / specialize_execution_stage
    // across a semantic_registry and drives compile-time vs runtime dispatch.
    // No real JIT compilation is performed; jit_stage is a notation marker.
    // -----------------------------------------------------------------------

    // Per-node result of the pipeline staged-specialization pass.
    struct pipeline_staged_node_result {
        structural_hash_t node_id = 0;
        semantic::staged_execution_descriptor descriptor;
    };

    // Aggregate result of specialize_staged_execution_for_pipeline.
    struct pipeline_staged_execution_result {
        // All per-node results, in the order supplied.
        std::vector<pipeline_staged_node_result> nodes;

        // Partition views (populated during the pass).
        std::vector<structural_hash_t> constexpr_nodes;
        std::vector<structural_hash_t> runtime_nodes;
        std::vector<structural_hash_t> deferred_nodes;
        std::vector<structural_hash_t> illegal_nodes;

        // Aggregate diagnostics from illegal nodes.
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
        [[nodiscard]] bool all_resolved() const { return deferred_nodes.empty(); }

        [[nodiscard]] semantic::execution_stage_kind stage_of(
            const structural_hash_t id) const noexcept {
            for (const auto& n : nodes) {
                if (n.node_id == id) return n.descriptor.stage;
            }
            return semantic::execution_stage_kind::deferred_stage;
        }
    };

    // Infers and validates the execution stage for every node_id present in
    // `reg`.  The `node_ids` list defines iteration order (topological order
    // is recommended so predecessor-stage propagation is correct).
    //
    // `requested_stage` may be deferred_stage (the default) to let semantic
    // inference decide per node.
    [[nodiscard]] inline pipeline_staged_execution_result
    specialize_staged_execution_for_pipeline(
        const semantic::semantic_registry& reg,
        const std::vector<structural_hash_t>& node_ids,
        semantic::execution_stage_kind requested_stage =
            semantic::execution_stage_kind::deferred_stage) {
        pipeline_staged_execution_result out;
        out.nodes.reserve(node_ids.size());

        for (const auto id : node_ids) {
            const auto info_opt = reg.get(id);
            if (!info_opt.has_value()) continue;

            auto desc = semantic::specialize_execution_stage(
                *info_opt, requested_stage);

            if (!desc.legal) {
                out.illegal_nodes.push_back(id);
                out.diagnostics.insert(out.diagnostics.end(),
                                       desc.diagnostics.begin(),
                                       desc.diagnostics.end());
            }
            else {
                switch (desc.stage) {
                case semantic::execution_stage_kind::constexpr_stage:
                    out.constexpr_nodes.push_back(id);
                    break;
                case semantic::execution_stage_kind::runtime_stage:
                    out.runtime_nodes.push_back(id);
                    break;
                case semantic::execution_stage_kind::jit_stage:
                    out.runtime_nodes.push_back(id);
                    break;
                case semantic::execution_stage_kind::deferred_stage:
                    out.deferred_nodes.push_back(id);
                    break;
                }
            }

            out.nodes.push_back({id, std::move(desc)});
        }

        return out;
    }

    // Runs partially_evaluate_staged over a semantic_registry using a flat
    // predecessor map.  `predecessors` maps each node_id to the list of its
    // direct predecessor node_ids; absent entries are treated as root nodes.
    [[nodiscard]] inline semantic::partial_evaluation_result
    partially_evaluate_registry_staged(
        const semantic::semantic_registry& reg,
        const std::vector<structural_hash_t>& node_ids,
        const std::unordered_map<structural_hash_t,
                                 std::vector<structural_hash_t>>& predecessors,
        const semantic::execution_stage_kind requested_stage =
            semantic::execution_stage_kind::deferred_stage) {
        std::vector<semantic::staged_node_entry> entries;
        entries.reserve(node_ids.size());

        for (const auto id : node_ids) {
            auto info_opt = reg.get(id);
            if (!info_opt.has_value()) continue;

            semantic::staged_node_entry entry;
            entry.node_id = id;
            entry.info = *info_opt;
            if (auto it = predecessors.find(id); it != predecessors.end()) {
                entry.predecessors = it->second;
            }
            entries.push_back(std::move(entry));
        }

        return semantic::partially_evaluate_staged(entries, requested_stage);
    }

    // =========================================================================
    // aggregate_lowering_pass
    //
    // Runs immediately after typed lowering and SROA.  Bridges the gap between
    // "typed MIR" (which still carries get_element_ptr / typed coercion
    // instructions) and purely physical MIR (raw byte-offset arithmetic).
    //
    // For every get_element_ptr instruction the pass:
    //   1. Reads the aggregate type name from operation_attributes["type_name"]
    //      and the field/index from operation_attributes["field_name"] (structs)
    //      or operation_attributes["index"] (arrays).
    //   2. Queries the layout_registry for byte offsets and element strides.
    //   3. Rewrites the GEP to physical arithmetic:
    //        struct  → add(base_ptr, imm_offset)
    //        array   → add(base_ptr, mul(index_vreg, imm_stride))
    //   4. Clears result_type_id and abstract_operation (type metadata stripped).
    //
    // Typed coercions (abstract_operation domain == "lithe.typed") are demoted
    // to plain mov instructions once the type metadata is no longer needed.
    //
    // Failures:
    //   Any get_element_ptr whose type layout cannot be resolved produces a
    //   pass_error via std::unexpected — the pipeline skips downstream passes.
    //
    // The returned mir_pass_result::function is ready for hardware emission.
    // =========================================================================

    struct aggregate_lowering_error {
        std::string instruction_context;
        std::string message;
    };

    // Detailed result carried inside mir_pass_result::diagnostics (stringified)
    // and also embedded in pass_error for the monadic pipeline.
    struct aggregate_lowering_result {
        std::size_t gep_lowered = 0;
        std::size_t coerce_demoted = 0;
        std::vector<aggregate_lowering_error> errors;

        [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
    };

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------
    namespace detail {
        // Resolve a field byte-offset inside an aggregate layout.
        // Returns std::nullopt when the field cannot be found.
        [[nodiscard]] inline std::optional<std::size_t>
        resolve_struct_field_offset(
            const semantic::layout::layout_registry& reg,
            const std::string_view type_name,
            const std::string_view field_name) {
            auto layout = reg.find(type_name);
            if (!layout) return std::nullopt;

            const auto* agg = std::get_if<semantic::layout::aggregate_layout>(&*layout);
            if (!agg) return std::nullopt;

            const auto* fld = agg->find_field(field_name);
            if (!fld) return std::nullopt;

            return fld->byte_offset;
        }

        // Resolve the element size (stride) of an array layout.
        // Returns std::nullopt when the type is not an array or element size is zero.
        [[nodiscard]] inline std::optional<std::size_t>
        resolve_array_element_size(
            const semantic::layout::layout_registry& reg,
            const std::string_view type_name) {
            auto layout = reg.find(type_name);
            if (!layout) return std::nullopt;

            const auto* arr = std::get_if<semantic::layout::array_layout>(&*layout);
            if (!arr || arr->element_count == 0) {
                // Dynamic or missing array — fall back to checking size/count from
                // layout itself.  We still need a per-element size.
                // For arrays, the "element_id" name carries the element type;
                // try resolving that.
                if (!arr) return std::nullopt;
                // element_id.name gives the element type name; look it up.
                auto elem = reg.find(arr->element_id.name);
                if (!elem) return std::nullopt;
                return std::visit([](const auto& l) -> std::size_t {
                    return l.size;
                }, *elem);
            }

            // Fixed-size array: total_size / count.
            if (arr->element_count == 0) return std::nullopt;
            return arr->size / arr->element_count;
        }

        // Append a new allocated_instruction into a block, returning a stable ref.
        inline allocated_instruction&
        emit_alloc(allocated_basic_block& block,
                   std::uint32_t& next_id,
                   const opcode op,
                   std::vector<allocated_operand> defs,
                   std::vector<allocated_operand> uses,
                   std::optional<std::string> comment = std::nullopt) {
            allocated_instruction inst;
            inst.id = next_id++;
            inst.op = op;
            inst.defs = std::move(defs);
            inst.uses = std::move(uses);
            inst.comment = std::move(comment);
            block.instructions.push_back(std::move(inst));
            return block.instructions.back();
        }

        // Lower a single get_element_ptr instruction in-place.
        //
        // GEP instruction encoding convention (set by the typed lowering pass):
        //   uses[0]   = base pointer preg
        //   uses[1]   = index preg (arrays only; absent for struct field access)
        //   defs[0]   = result preg (the computed address)
        //   operation_attributes["type_name"]  = aggregate type name in the registry
        //   operation_attributes["field_name"] = field name (struct access)
        //   operation_attributes["index"]      = "array" literal (array access)
        //
        // For array GEPs a scratch preg is used for the mul intermediate.
        // Returns an error descriptor on failure.
        [[nodiscard]] inline std::optional<aggregate_lowering_error>
        lower_gep(allocated_basic_block& block,
                  allocated_instruction& gep,
                  std::size_t inst_pos,
                  const semantic::layout::layout_registry& reg,
                  std::uint32_t& next_id,
                  preg scratch_preg) {
            const auto& attrs = gep.operation_attributes;

            const auto type_it = attrs.find("type_name");
            if (type_it == attrs.end()) {
                return aggregate_lowering_error{
                    "gep#" + std::to_string(gep.id),
                    "missing operation_attribute 'type_name'"
                };
            }
            const std::string& type_name = type_it->second;

            // Determine whether this is a struct-field or array-element GEP.
            const bool is_array = (attrs.find("index") != attrs.end() &&
                attrs.find("field_name") == attrs.end());

            if (gep.uses.empty()) {
                return aggregate_lowering_error{
                    "gep#" + std::to_string(gep.id),
                    "get_element_ptr instruction has no base pointer operand"
                };
            }
            if (gep.defs.empty()) {
                return aggregate_lowering_error{
                    "gep#" + std::to_string(gep.id),
                    "get_element_ptr instruction has no result operand"
                };
            }

            const allocated_operand base_ptr = gep.uses[0];
            const allocated_operand result = gep.defs[0];

            // ---- Struct field access: add(base, imm_offset) ------------------
            if (!is_array) {
                const auto field_it = attrs.find("field_name");
                if (field_it == attrs.end()) {
                    return aggregate_lowering_error{
                        "gep#" + std::to_string(gep.id),
                        "struct GEP missing operation_attribute 'field_name'"
                    };
                }

                auto offset_opt = resolve_struct_field_offset(reg, type_name, field_it->second);
                if (!offset_opt) {
                    return aggregate_lowering_error{
                        "gep#" + std::to_string(gep.id),
                        "cannot resolve field '" + field_it->second +
                        "' in aggregate type '" + type_name + "'"
                    };
                }

                // Rewrite in-place: opcode → add, uses = [base, imm_offset], defs = [result]
                gep.op = opcode::add;
                gep.defs = {result};
                gep.uses = {
                    base_ptr,
                    allocated_operand::as_i64(static_cast<std::int64_t>(*offset_opt))
                };
                gep.abstract_operation.reset();
                gep.result_type_id.reset();
                gep.operation_attributes.clear();
                gep.comment = "gep→add struct offset " + std::to_string(*offset_opt);
                return std::nullopt;
            }

            // ---- Array element access: add(base, mul(index, stride)) ---------
            if (gep.uses.size() < 2) {
                return aggregate_lowering_error{
                    "gep#" + std::to_string(gep.id),
                    "array GEP missing index operand (uses[1])"
                };
            }

            auto stride_opt = resolve_array_element_size(reg, type_name);
            if (!stride_opt || *stride_opt == 0) {
                return aggregate_lowering_error{
                    "gep#" + std::to_string(gep.id),
                    "cannot resolve element stride for array type '" + type_name + "'"
                };
            }

            const allocated_operand index_op = gep.uses[1];
            const std::int64_t stride = static_cast<std::int64_t>(*stride_opt);

            // We need an intermediate register for the byte offset product.
            // Build the instruction sequence in a scratch vector, then splice it
            // around inst_pos in the block's instruction list.
            //
            //   scratch = mul(index, stride_imm)
            //   result  = add(base,  scratch)
            //
            // The existing GEP slot becomes the add; we insert the mul before it.

            // Build mul instruction using the caller-provided scratch preg.
            allocated_instruction mul_inst;
            mul_inst.id = next_id++;
            mul_inst.op = opcode::mul;
            mul_inst.defs = {allocated_operand::as_preg(scratch_preg)};
            mul_inst.uses = {
                index_op,
                allocated_operand::as_i64(stride)
            };
            mul_inst.comment = "gep array stride * index";

            // Rewrite the GEP slot to an add using the scratch preg as offset.
            gep.op = opcode::add;
            gep.defs = {result};
            gep.uses = {
                base_ptr,
                allocated_operand::as_preg(scratch_preg)
            };
            gep.abstract_operation.reset();
            gep.result_type_id.reset();
            gep.operation_attributes.clear();
            gep.comment = "gep→add array element address";

            // Insert the mul before the (now-rewritten) add.
            block.instructions.insert(
                block.instructions.begin() + static_cast<std::ptrdiff_t>(inst_pos),
                std::move(mul_inst));

            return std::nullopt;
        }

        // Demote a typed coercion to a plain mov, stripping semantic metadata.
        inline void demote_typed_coercion(allocated_instruction& inst) {
            inst.op = opcode::mov;
            inst.abstract_operation.reset();
            inst.result_type_id.reset();
            inst.operation_attributes.clear();
            if (!inst.comment) {
                inst.comment = "coercion→mov";
            }
        }

        [[nodiscard]] inline bool is_typed_coercion(const allocated_instruction& inst) {
            if (!inst.abstract_operation) return false;
            return inst.abstract_operation->domain == "lithe.typed";
        }
    } // namespace detail

    // -------------------------------------------------------------------------
    // aggregate_lowering_pass
    // -------------------------------------------------------------------------
    struct aggregate_lowering_pass {
        // The layout registry to query for type sizes and field offsets.
        // Defaults to the process-wide singleton; callers may inject a private
        // registry for testing.
        const semantic::layout::layout_registry* layout_reg = nullptr;

        // Scratch physical register used as the intermediate for array GEP mul.
        // Register 255 is a safe convention for a caller-save scratch; tests may
        // override with any preg that does not alias live values in the function.
        preg scratch{255, "scratch"};

        explicit aggregate_lowering_pass(
            const semantic::layout::layout_registry& reg =
                semantic::layout::global_layout_registry(),
            preg scratch_preg = preg{255, "scratch"})
            : layout_reg(&reg), scratch(std::move(scratch_preg)) {}

        // Monadic entry point: returns std::unexpected on the first unresolvable
        // GEP so the pipeline's and_then chain skips downstream passes.
        [[nodiscard]] mir_pass_expected
        run(mir::physical_mir_function fn) const {
            aggregate_lowering_result lowering_stats;
            mir_pass_result out;

            // Track a pass-local next_id seed beyond the existing max.
            std::uint32_t next_id = 1;
            for (const auto& block : fn.function.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.id >= next_id) next_id = inst.id + 1;
                }
            }

            // Process each block.
            for (auto& block : fn.function.blocks) {
                for (std::size_t i = 0; i < block.instructions.size(); /* manual */) {
                    auto& inst = block.instructions[i];

                    // ---- GEP lowering ----------------------------------------
                    if (inst.op == opcode::get_element_ptr) {
                        if (layout_reg == nullptr) {
                            return std::unexpected(pass_error::from_message(
                                "aggregate_lowering_pass",
                                "no layout registry provided"));
                        }
                        auto err = detail::lower_gep(block, inst, i, *layout_reg, next_id, scratch);
                        if (err) {
                            return std::unexpected(pass_error::from_message(
                                "aggregate_lowering_pass",
                                err->instruction_context + ": " + err->message));
                        }
                        ++lowering_stats.gep_lowered;
                        // lower_gep may have inserted a mul before position i,
                        // so the current instruction (now the add) is at i+1 for
                        // arrays.  We always advance by 1 past the add.
                        // For struct GEPs the instruction was rewritten in-place at i.
                        // For array GEPs a mul was inserted at i; the add is at i+1.
                        // Check: if the instruction at i is now a mul, skip both.
                        if (block.instructions[i].op == opcode::mul) {
                            i += 2; // mul + add
                        }
                        else {
                            ++i;
                        }
                        continue;
                    }

                    // ---- Typed coercion demotion ------------------------------
                    if (detail::is_typed_coercion(inst)) {
                        detail::demote_typed_coercion(inst);
                        ++lowering_stats.coerce_demoted;
                    }

                    ++i;
                }
            }

            // Populate diagnostics summary in result.
            if (lowering_stats.gep_lowered > 0 || lowering_stats.coerce_demoted > 0) {
                out.diagnostics.push_back(
                    "aggregate_lowering: lowered " +
                    std::to_string(lowering_stats.gep_lowered) + " GEP(s), demoted " +
                    std::to_string(lowering_stats.coerce_demoted) + " coercion(s)");
            }
            out.changed = (lowering_stats.gep_lowered + lowering_stats.coerce_demoted) > 0;
            out.function = std::move(fn);

            // Strip phase note from metadata: the output is now physical-only.
            out.function.metadata.note = "aggregate_lowering complete";
            return out;
        }

        // Convenience overload matching the existing mir_pass_result convention
        // used by passes that do not participate in the monadic pipeline.
        [[nodiscard]] mir_pass_expected
        operator()(mir::physical_mir_function fn, mir_pass_context& /*ctx*/) const {
            return run(std::move(fn));
        }
    };

    // =========================================================================
    // safepoint_injection_pass  (lithe::codegen — standard pass interface)
    //
    // Inserts safepoint_tag instructions (op=indirect_call, domain
    // "lithe.safepoint") into a physical_mir_function:
    //   • After every indirect_call instruction.
    //   • At every loop back-edge (the last instruction of a block whose
    //     successor is a loop header AND the edge is a back-edge per
    //     loop_analysis_result).
    //
    // The uses[] of each inserted instruction are one as_preg operand per
    // register that is live (reaching-definitions) at that program point.
    //
    // De-duplication: if two insertion sites land on the same instr_id position
    // (e.g., a call at a back-edge), only one safepoint is emitted.
    //
    // Instruction IDs for the injected safepoints are assigned starting from
    // max(existing_id) + 1 and are given in original-instruction order so the
    // final sequence is deterministic.
    // =========================================================================
    struct safepoint_injection_pass {
        [[nodiscard]] mir_pass_result
        run(mir::physical_mir_function const& fn, mir_pass_context& ctx) const {
            using namespace lithe::runtime::safepoint;

            mir_pass_result out;
            out.function = fn;

            auto& blocks = out.function.function.blocks;
            if (blocks.empty()) return out;

            // ------------------------------------------------------------------
            // 1. Obtain analysis results from cache.
            // ------------------------------------------------------------------
            const auto& reach = get_or_compute_reaching_definitions(ctx, out.function);
            const auto& loops = get_or_compute_loop(ctx, out.function);

            // ------------------------------------------------------------------
            // 2. Compute the next free instruction ID.
            // ------------------------------------------------------------------
            std::uint32_t next_id = 0;
            for (const auto& b : blocks)
                for (const auto& i : b.instructions)
                    next_id = std::max(next_id, i.id);
            ++next_id;

            // ------------------------------------------------------------------
            // 3. Collect insertion sites: (block_index, after_instr_index, live_set)
            //    We'll gather them all, dedup by "after instruction ID", then
            //    insert in reverse order so earlier indices remain valid.
            // ------------------------------------------------------------------
            struct insertion_site {
                std::size_t block_idx = 0;
                std::size_t after_idx = 0; // insert AFTER this position
                std::uint32_t after_id = 0; // id of the instruction we follow
                live_set roots;
            };
            std::vector<insertion_site> sites;

            for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
                auto& block = blocks[bi];

                for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
                    const auto& inst = block.instructions[ii];

                    bool is_call = (inst.op == opcode::indirect_call);
                    // Do not double-tag an existing safepoint_tag.
                    if (is_call && inst.abstract_operation &&
                        inst.abstract_operation->domain == std::string(safepoint_domain))
                        is_call = false;

                    // Back-edge: last instruction in a block that has a
                    // successor that is a loop header AND the edge is a back-edge.
                    bool is_back_edge_site = false;
                    if (ii + 1 == block.instructions.size()) {
                        for (const auto succ : block.successors) {
                            if (loops.is_back_edge(block.id, succ)) {
                                is_back_edge_site = true;
                                break;
                            }
                        }
                    }

                    if (!is_call && !is_back_edge_site) continue;

                    // Build live_set from reaching definitions AFTER this instruction.
                    // after_instruction is keyed by instr_id → map<preg_id, definition_site>.
                    live_set roots;
                    const auto ai_it = reach.after_instruction.find(inst.id);
                    if (ai_it != reach.after_instruction.end()) {
                        roots.reserve(ai_it->second.size());
                        for (const auto& [preg_id, _def] : ai_it->second)
                            roots.push_back(static_cast<std::uint32_t>(preg_id));
                    }
                    std::ranges::sort(roots);
                    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

                    sites.push_back({bi, ii, inst.id, std::move(roots)});
                }
            }

            // ------------------------------------------------------------------
            // 4. De-duplicate: if two sites have the same after_id keep union of roots.
            // ------------------------------------------------------------------
            std::stable_sort(sites.begin(), sites.end(),
                             [](const insertion_site& a, const insertion_site& b) {
                                 return a.after_id < b.after_id;
                             });
            {
                std::vector<insertion_site> deduped;
                for (auto& s : sites) {
                    if (!deduped.empty() && deduped.back().after_id == s.after_id) {
                        // Merge roots.
                        for (auto r : s.roots) {
                            if (std::find(deduped.back().roots.begin(),
                                          deduped.back().roots.end(), r)
                                == deduped.back().roots.end())
                                deduped.back().roots.push_back(r);
                        }
                    }
                    else {
                        deduped.push_back(std::move(s));
                    }
                }
                sites = std::move(deduped);
            }

            // ------------------------------------------------------------------
            // 5. Insert in reverse order (per block, from last to first) so
            //    earlier indices remain valid.  Assign deterministic IDs by
            //    processing in original instruction order (lowest after_id first)
            //    then assigning ascending IDs.
            // ------------------------------------------------------------------
            // Assign IDs in original-instruction order first.
            std::vector<std::uint32_t> assigned_ids;
            assigned_ids.reserve(sites.size());
            for (std::size_t k = 0; k < sites.size(); ++k)
                assigned_ids.push_back(next_id++);

            // Now insert back-to-front to preserve indices.
            for (std::size_t k = sites.size(); k-- > 0;) {
                const auto& s = sites[k];
                auto instr = make_safepoint_instr(assigned_ids[k], s.roots);
                auto& target_block = blocks[s.block_idx];
                target_block.instructions.insert(
                    target_block.instructions.begin() +
                    static_cast<std::ptrdiff_t>(s.after_idx + 1),
                    std::move(instr));
            }

            out.changed = !sites.empty();
            return out;
        }
    };

    // =========================================================================
    // fuel_injection_pass
    //
    // Inserts a fuel_check_tag (domain "lithe.fuel") instruction:
    //   • at the first position of the function's entry block, and
    //   • at the first position of every loop back-edge target block
    //     (a block whose any predecessor has a higher block id, per DFS order).
    //
    // If sandbox is null the instructions are still inserted but the backend
    // will emit no machine code for them (same zero-overhead NOP contract as
    // safepoint_tag).
    // =========================================================================

    struct fuel_injection_pass {
        [[nodiscard]] mir_pass_result
        run(mir::physical_mir_function& phys,
            lithe::runtime::ExecutionSandbox* /*sandbox*/ = nullptr) const {
            mir_pass_result out;
            out.changed = false;

            auto& fn = phys.function;
            auto& cfg = fn.cfg;
            const auto entry = cfg.entry_block;

            // Collect block ids that need a fuel check:
            //   1. The entry block always gets one.
            //   2. Any block that is a back-edge target: has a predecessor with
            //      a block id >= this block's id (DFS back-edge heuristic).
            std::vector<std::uint32_t> targets;
            targets.push_back(entry);

            for (const auto& [bid, preds] : cfg.predecessors) {
                if (bid == entry) continue; // entry already covered
                for (const auto pred_id : preds) {
                    if (pred_id >= bid) {
                        targets.push_back(bid);
                        break;
                    }
                }
            }

            // Deduplicate (entry might appear twice if it has a self-edge).
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

            // Assign IDs beyond the current max instruction id in the function.
            std::uint32_t next_id = 0;
            for (const auto& block : fn.blocks)
                for (const auto& inst : block.instructions)
                    next_id = std::max(next_id, inst.id + 1);

            for (auto& block : fn.blocks) {
                if (std::find(targets.begin(), targets.end(), block.id) == targets.end())
                    continue;
                auto instr = lithe::runtime::fuel::make_fuel_check_instr(next_id++);
                block.instructions.insert(block.instructions.begin(), std::move(instr));
                out.changed = true;
            }

            return out;
        }
    };
} // namespace lithe::codegen
