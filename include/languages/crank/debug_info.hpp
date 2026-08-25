#pragma once

// crank/debug_info.hpp — Debugger data model + runtime debug-hook interfaces.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// This header carries the *static* debug-info data model consumed by language
// servers, editors, and debuggers (DAP/LSP-style), plus the *runtime* debug
// event/hook vocabulary. The runtime hooks are defined and wireable now but are
// not yet driven end-to-end: the scalar interpreter has no CFG execution
// (execute.hpp: execution_skipped_reason == "control_flow_unsupported"), so live
// stepping/pausing waits on a CFG-aware backend. The types below let a host wire
// breakpoints/watches and receive events the moment that backend lands.
//
// Design notes:
//   - No virtual, no macros. Dispatch is via std::function sinks + concepts.
//   - The scope tree is a flat vector with parent indices (no owning pointers),
//     so the whole model is cheap to copy and trivially serialisable (debug.hpp).
//   - source_span (source_span.hpp) is the single location currency; no new
//     location type is introduced.
//   - Zero-overhead when unused: nothing here allocates until a builder
//     (debug.hpp) fills it, and null_debug_hooks is an empty aggregate.

#include "languages/crank/source_span.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
    // ============================================================================
    // debug_scope_kind — lexical scope classification (mirrors AST tag bands)
    // ============================================================================

    enum class debug_scope_kind : std::uint8_t {
        module = 0,
        function = 1,
        block = 2,
        loop = 3,
        transaction = 4,
    };

    [[nodiscard]] constexpr std::string_view to_string(debug_scope_kind k) noexcept {
        switch (k) {
        case debug_scope_kind::module: return "module";
        case debug_scope_kind::function: return "function";
        case debug_scope_kind::block: return "block";
        case debug_scope_kind::loop: return "loop";
        case debug_scope_kind::transaction: return "transaction";
        }
        return "unknown";
    }

    // ============================================================================
    // debug_variable — one inspectable binding (param or local or global)
    // ============================================================================

    struct debug_variable {
        std::string name; ///< unqualified source name
        std::string type_name; ///< display type (may be empty if unresolved)
        std::uint32_t type_ref = 0; ///< Vakya type_registry stable_id (0 = unresolved)
        source_span decl_span{}; ///< declaration location (zeroed if unknown)
        bool is_param = false; ///< function parameter
        bool is_mutable = false; ///< var (true) vs let/const (false)
    };

    // ============================================================================
    // debug_scope — a node in the flat scope tree
    //
    // The tree is encoded as a flat vector<debug_scope>; `parent` is an index into
    // that same vector (kNoParent for roots). This keeps the model pointer-free and
    // trivially copyable/serialisable.
    // ============================================================================

    inline constexpr std::uint32_t kNoParent = 0xFFFF'FFFFu;

    struct debug_scope {
        debug_scope_kind kind = debug_scope_kind::block;
        std::string name; ///< fn/loop/module name ("" for anonymous block)
        source_span span{}; ///< scope extent (zeroed if unknown)
        std::uint32_t parent = kNoParent; ///< index of enclosing scope, or kNoParent
        std::uint32_t depth = 0; ///< lexical depth (0 = module root)
        std::vector<debug_variable> locals; ///< bindings introduced in this scope
        std::uint64_t structural_hash = 0; ///< Vakya structural hash (0 = unknown)
    };

    // ============================================================================
    // debug_line_entry — one row of the source↔IR line table
    //
    // Maps a source location to a position in the HL MIR. `is_stmt_boundary` marks
    // rows a debugger may snap a breakpoint to.
    // ============================================================================

    struct debug_line_entry {
        source_span src{}; ///< source location
        std::string fn_name; ///< owning function
        std::uint32_t hl_op_index = 0; ///< index of the HL op this row maps to
        std::uint32_t block_index = 0; ///< HL block index
        bool is_stmt_boundary = false; ///< snap-target for breakpoints
    };

    // ============================================================================
    // breakpoint_location — a resolved (or unresolved) breakpoint target
    // ============================================================================

    struct breakpoint_location {
        std::uint32_t line = 0; ///< requested 1-based line
        std::uint32_t col = 0; ///< requested 1-based column (0 = any)
        source_span resolved{}; ///< snapped location (valid iff verified)
        std::string fn_name; ///< enclosing function of resolved location
        bool verified = false; ///< true if the line was snapped to a stmt boundary
    };

    // ============================================================================
    // debug_info — static debug information for one compilation unit
    // ============================================================================

    struct debug_info {
        std::string module_name;
        std::vector<debug_scope> scopes; ///< flat tree (parent indices)
        std::vector<debug_line_entry> line_table; ///< sorted by src.line ascending
        std::vector<debug_variable> globals; ///< module-level bindings

        // --- queries ---

        /// Indices of every scope whose span contains `where` (by byte offset).
        [[nodiscard]] std::vector<std::uint32_t> scopes_at(source_span where) const {
            std::vector<std::uint32_t> out;
            const std::uint32_t off = where.offset;
            for (std::uint32_t i = 0; i < scopes.size(); ++i) {
                const auto& s = scopes[i];
                const std::uint32_t begin = s.span.offset;
                const std::uint32_t end = s.span.offset + s.span.length;
                if (s.span.length == 0) continue; // unknown extent
                if (off >= begin && off < end) out.push_back(i);
            }
            return out;
        }

        /// Locals of a scope by index (empty span if out of range).
        [[nodiscard]] std::span<const debug_variable>
        variables_in_scope(std::uint32_t scope_index) const noexcept {
            if (scope_index >= scopes.size()) return {};
            return scopes[scope_index].locals;
        }

        /// Nearest enclosing function scope name for a 1-based source line.
        [[nodiscard]] std::optional<std::string>
        enclosing_function(std::uint32_t line) const {
            std::optional<std::string> best;
            std::uint32_t best_span = 0xFFFF'FFFFu;
            for (const auto& s : scopes) {
                if (s.kind != debug_scope_kind::function) continue;
                if (s.span.length == 0) continue;
                const std::uint32_t lo = s.span.line;
                // Approximate end line: we only have the start line + byte length,
                // so treat any function whose start line <= requested line as a
                // candidate and keep the tightest byte extent.
                if (lo <= line && s.span.length < best_span) {
                    best = s.name;
                    best_span = s.span.length;
                }
            }
            return best;
        }
    };

    // ============================================================================
    // resolve_breakpoint — snap a requested line to the nearest statement boundary
    //
    // Returns an unverified location (verified == false) when no statement boundary
    // is found at or after the requested line.
    // ============================================================================

    [[nodiscard]] inline std::optional<breakpoint_location>
    resolve_breakpoint(const debug_info& di, std::uint32_t line, std::uint32_t col = 0) {
        const debug_line_entry* best = nullptr;
        for (const auto& e : di.line_table) {
            if (!e.is_stmt_boundary) continue;
            if (e.src.line < line) continue; // before requested line
            if (!best || e.src.line < best->src.line ||
                (e.src.line == best->src.line && e.src.col < best->src.col)) {
                best = &e;
            }
        }
        breakpoint_location bp;
        bp.line = line;
        bp.col = col;
        if (best) {
            bp.resolved = best->src;
            bp.fn_name = best->fn_name;
            bp.verified = true;
        }
        return bp;
    }

    // ============================================================================
    // Runtime debug events + hooks (stubbed — awaiting CFG-aware backend)
    // ============================================================================

    enum class debug_event_kind : std::uint8_t {
        breakpoint_hit = 0,
        step_line = 1,
        step_in = 2,
        step_out = 3,
        watch_write = 4,
        scope_enter = 5,
        scope_exit = 6,
        trap = 7,
    };

    [[nodiscard]] constexpr std::string_view to_string(debug_event_kind k) noexcept {
        switch (k) {
        case debug_event_kind::breakpoint_hit: return "breakpoint_hit";
        case debug_event_kind::step_line: return "step_line";
        case debug_event_kind::step_in: return "step_in";
        case debug_event_kind::step_out: return "step_out";
        case debug_event_kind::watch_write: return "watch_write";
        case debug_event_kind::scope_enter: return "scope_enter";
        case debug_event_kind::scope_exit: return "scope_exit";
        case debug_event_kind::trap: return "trap";
        }
        return "unknown";
    }

    struct debug_event {
        debug_event_kind kind = debug_event_kind::step_line;
        source_span at{};
        std::uint32_t scope_index = kNoParent; ///< active scope, or kNoParent
        std::string detail; ///< e.g. watched variable name, trap reason
    };

    // DebugEventSink — any type with `void on_event(const debug_event&)`.
    // Non-virtual: a debugger front-end models a sink and the future interpreter
    // dispatches to it via `if constexpr` / template, never a vtable.
    template <class S>
    concept DebugEventSink = requires(S& s, const debug_event& e) {
        { s.on_event(e) };
    };

    // ============================================================================
    // debug_hooks — host-supplied runtime debug configuration
    //
    // Aggregate of optional callbacks + the breakpoint/watch registries. All fields
    // default to empty; an unset std::function is simply not invoked, so an
    // unconfigured debug session costs nothing beyond the (empty) members.
    // ============================================================================

    struct debug_hooks {
        std::function<void(const debug_event&)> on_breakpoint;
        std::function<void(const debug_event&)> on_step;
        std::function<void(const debug_event&)> on_watch;

        std::vector<breakpoint_location> breakpoints;
        std::vector<std::string> watched_vars;

        /// Dispatch an event to the matching callback if one is installed.
    /// Safe to call from the future CFG interpreter; no-op when unset.
        void dispatch(const debug_event& e) const {
            switch (e.kind) {
            case debug_event_kind::breakpoint_hit:
                if (on_breakpoint) on_breakpoint(e);
                break;
            case debug_event_kind::watch_write:
                if (on_watch) on_watch(e);
                break;
            default:
                if (on_step) on_step(e);
                break;
            }
        }

        [[nodiscard]] bool any_installed() const noexcept {
            return static_cast<bool>(on_breakpoint) ||
                static_cast<bool>(on_step) ||
                static_cast<bool>(on_watch) ||
                !breakpoints.empty() || !watched_vars.empty();
        }
    };

    /// A no-op debug configuration — zero cost, satisfies "debugging disabled".
    inline const debug_hooks null_debug_hooks{};

    // ============================================================================
    // debug_stepping_state — carrier the future CFG interpreter advances
    //
    // Wired but not driven in v1 (the scalar interpreter cannot execute CFG). A
    // CFG-aware backend will own one of these, update `current`/`current_scope` as
    // it advances, and set `paused` when it hits a breakpoint or step target.
    // ============================================================================

    struct debug_stepping_state {
        bool paused = false;
        std::uint32_t current_scope = kNoParent;
        source_span current{};
    };
} // namespace crank
