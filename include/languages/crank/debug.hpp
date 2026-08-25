#pragma once

// crank/debug.hpp — Debug-info builders + JSON serializers + pipeline stats.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Consumes existing pipeline artifacts (parse_result, resolve_result,
// lower_hl_result) and assembles the static debug model from debug_info.hpp —
// it never re-parses or re-lowers. Also serializes every debug/stat structure
// to DAP/LSP-shaped JSON for editors, language servers, and debuggers.
//
// Builders:
//   build_debug_info(parse, resolve, module_name) -> debug_info
//       Walks the resolver symbol table into a flat scope tree + variables.
//       source spans are zeroed where the symbol table cannot supply them
//       (spans live in the property_store keyed by structural_hash, not joinable
//       to symbols by name — see source_span.hpp / resolve.hpp).
//   append_line_table(debug_info&, lower_hl_result)
//       Folds HL loop-bound / defer / exit-edge spans into line-table rows,
//       marking statement boundaries a debugger may snap breakpoints to.
//
// Serializers (glaze when present, manual JSON fallback otherwise — matches the
// dump.hpp pattern: one glz::meta<> per data struct, guarded by CRANK_HAS_GLAZE):
//   dump_debug_info, dump_scopes, dump_line_table, dump_breakpoints,
//   dump_debug_event, dump_pipeline_stats.
//
// pipeline_stats_snapshot bundles every stage's stats so one call gives an
// editor the full picture.
//
// NADI (observability/nadi.hpp) is guarded by __has_include: when present,
// emit_debug_pulse routes a debug_event through a Pulse; when absent, the helper
// is an inline no-op, so debug wiring is pay-for-use.

#include "languages/crank/debug_info.hpp"
#include "languages/crank/resolve.hpp"
#include "languages/crank/lower_hl.hpp"
#include "languages/crank/parser_stats.hpp"
#include "languages/crank/execute.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Glaze JSON — guarded (same probe as dump.hpp)
#if __has_include("glaze/json.hpp")
#  include "glaze/json.hpp"
#  ifndef CRANK_HAS_GLAZE
#    define CRANK_HAS_GLAZE 1
#  endif
#endif

// NADI observability — guarded; our own lib, assumed present but pay-for-use.
#if __has_include("observability/nadi.hpp")
#  include "observability/nadi.hpp"
#  ifndef CRANK_HAS_NADI
#    define CRANK_HAS_NADI 1
#  endif
#endif

namespace crank {
    // ============================================================================
    // build_debug_info — assemble the static scope/variable model
    //
    // The resolver symbol table gives names, kinds, mutability, resolved type_id,
    // and lexical scope_depth. We synthesize:
    //   - a module root scope (depth 0),
    //   - one function scope per symbol_kind::function,
    //   - variables/params/constants placed as globals or function locals.
    //
    // The table is a flat unordered_map with no ordering and no owning fn linkage,
    // so this is a best-effort structural model: values at depth 0 become globals;
    // deeper values attach to the most recent function scope at a shallower depth.
    // decl_span is left zeroed (the symbol table carries no span — see header note).
    // ============================================================================

    namespace detail {
        // Derive a display type name from a Vakya type stable_id. We keep this honest:
        // there is no name-by-id lookup wired here, so we emit a stable "type#N" token
        // (0 → "" meaning unresolved). Editors can map the id via the type_registry.
        [[nodiscard]] inline std::string type_name_for(std::uint32_t type_id) {
            if (type_id == 0) return {};
            return "type#" + std::to_string(type_id);
        }

        [[nodiscard]] inline bool is_variable_kind(symbol_kind k) noexcept {
            return k == symbol_kind::value || k == symbol_kind::param ||
                k == symbol_kind::constant;
        }
    } // namespace detail

    [[nodiscard]] inline debug_info
    build_debug_info(const resolve_result& rr, std::string module_name) {
        debug_info di;
        di.module_name = std::move(module_name);

        // Module root scope at index 0.
        debug_scope root;
        root.kind = debug_scope_kind::module;
        root.name = di.module_name;
        root.parent = kNoParent;
        root.depth = 0;
        di.scopes.push_back(std::move(root));

        // First pass: one scope per function. Record name→scope index for locals.
        // Iteration order of the flat map is unspecified; the tree is structural,
        // not positional, so this is acceptable for a v1 model.
        struct fn_ref {
            std::uint32_t scope_index;
            std::uint32_t depth;
        };
        std::vector<fn_ref> fn_scopes;

        rr.symbols.for_each([&](const std::string&, const symbol_entry& e) {
            if (e.kind != symbol_kind::function) return;
            debug_scope fs;
            fs.kind = debug_scope_kind::function;
            fs.name = e.local_name;
            fs.parent = 0; // enclosed by module root
            fs.depth = e.scope_depth + 1;
            const auto idx = static_cast<std::uint32_t>(di.scopes.size());
            di.scopes.push_back(std::move(fs));
            fn_scopes.push_back({idx, e.scope_depth});
        });

        // Second pass: place variables. Depth-0 values → globals; deeper values →
        // the function scope with the greatest depth strictly less than the value's.
        rr.symbols.for_each([&](const std::string&, const symbol_entry& e) {
            if (!detail::is_variable_kind(e.kind)) return;

            debug_variable v;
            v.name = e.local_name;
            v.type_ref = e.type_id;
            v.type_name = detail::type_name_for(e.type_id);
            v.is_param = (e.kind == symbol_kind::param);
            v.is_mutable = (e.mutability == mutability_kind::mutable_);

            if (e.scope_depth == 0) {
                di.globals.push_back(std::move(v));
                return;
            }

            std::uint32_t best_idx = kNoParent;
            std::uint32_t best_depth = 0;
            bool found = false;
            for (const auto& f : fn_scopes) {
                if (f.depth < e.scope_depth && (!found || f.depth >= best_depth)) {
                    best_idx = f.scope_index;
                    best_depth = f.depth;
                    found = true;
                }
            }
            if (found) di.scopes[best_idx].locals.push_back(std::move(v));
            else di.globals.push_back(std::move(v));
        });

        return di;
    }

    // ============================================================================
    // append_line_table — fold HL spans into line-table rows
    //
    // The HL result carries controlled/trap exit edges (each with a source_span)
    // and loop-bound names. We emit one statement-boundary row per exit edge (the
    // point a debugger can pause on), tagged with the owning function name.
    // hl_op_index / block_index are best-effort ordinals in emission order.
    // ============================================================================

    inline void
    append_line_table(debug_info& di, const lower_hl_result& lr) {
        const std::string& fn = lr.hl_fn.name;
        std::uint32_t op_ord = 0;

        for (const auto& edge : lr.exit_edges) {
            debug_line_entry row;
            row.src = edge.at;
            row.fn_name = fn;
            row.hl_op_index = op_ord++;
            row.block_index = 0;
            // Controlled exits (return/break/continue/guard) are stmt boundaries;
            // trap/terminate edges are not snap targets (defers don't run there).
            row.is_stmt_boundary = (edge.kind == exit_edge_kind::controlled);
            di.line_table.push_back(std::move(row));
        }

        // Keep the table sorted by line for resolve_breakpoint's nearest-after scan.
        std::sort(di.line_table.begin(), di.line_table.end(),
                  [](const debug_line_entry& a, const debug_line_entry& b) {
                      if (a.src.line != b.src.line) return a.src.line < b.src.line;
                      return a.src.col < b.src.col;
                  });
    }

    // ============================================================================
    // pipeline_stats_snapshot — every stage's stats in one bundle
    // ============================================================================

    struct pipeline_stats_snapshot {
        std::optional<parse_stats> parse;
        std::optional<hl_lowering_stats> lower;
        std::optional<execute_stats> execute;
    };

    // ============================================================================
    // JSON serializers
    // ============================================================================
    // glaze metadata for the crank debug types (defined in debug_info.hpp) is provided
    // via global-scope glz::meta specializations after this namespace block — glz::meta
    // may only be specialized in a namespace enclosing glz, not inside namespace crank.

    // ---- manual-fallback helpers (also used to compose nested output) ----------

    namespace detail {
        inline void json_escape_into(std::string& out, std::string_view s) {
            for (char c : s) {
                switch (c) {
                case '"': out += "\\\"";
                    break;
                case '\\': out += "\\\\";
                    break;
                case '\n': out += "\\n";
                    break;
                case '\t': out += "\\t";
                    break;
                case '\r': out += "\\r";
                    break;
                default: out += c;
                    break;
                }
            }
        }

        inline void variable_json(std::string& out, const debug_variable& v) {
            out += "{\"name\":\"";
            json_escape_into(out, v.name);
            out += "\",\"type_name\":\"";
            json_escape_into(out, v.type_name);
            out += "\",\"type_ref\":" + std::to_string(v.type_ref);
            out += ",\"is_param\":";
            out += v.is_param ? "true" : "false";
            out += ",\"is_mutable\":";
            out += v.is_mutable ? "true" : "false";
            out += "}";
        }

        inline void scope_json(std::string& out, const debug_scope& s) {
            out += "{\"kind\":\"";
            out += to_string(s.kind);
            out += "\",\"name\":\"";
            json_escape_into(out, s.name);
            out += "\",\"parent\":" + std::to_string(s.parent);
            out += ",\"depth\":" + std::to_string(s.depth);
            out += ",\"locals\":[";
            for (std::size_t i = 0; i < s.locals.size(); ++i) {
                if (i) out += ',';
                variable_json(out, s.locals[i]);
            }
            out += "]}";
        }

        inline void line_entry_json(std::string& out, const debug_line_entry& e) {
            out += "{\"line\":" + std::to_string(e.src.line);
            out += ",\"col\":" + std::to_string(e.src.col);
            out += ",\"offset\":" + std::to_string(e.src.offset);
            out += ",\"length\":" + std::to_string(e.src.length);
            out += ",\"fn_name\":\"";
            json_escape_into(out, e.fn_name);
            out += "\",\"hl_op_index\":" + std::to_string(e.hl_op_index);
            out += ",\"block_index\":" + std::to_string(e.block_index);
            out += ",\"stmt_boundary\":";
            out += e.is_stmt_boundary ? "true" : "false";
            out += "}";
        }

        inline void breakpoint_json(std::string& out, const breakpoint_location& b) {
            out += "{\"line\":" + std::to_string(b.line);
            out += ",\"col\":" + std::to_string(b.col);
            out += ",\"fn_name\":\"";
            json_escape_into(out, b.fn_name);
            out += "\",\"verified\":";
            out += b.verified ? "true" : "false";
            out += ",\"resolved_line\":" + std::to_string(b.resolved.line);
            out += ",\"resolved_col\":" + std::to_string(b.resolved.col);
            out += "}";
        }
    } // namespace detail

    // ---- dump_scopes -----------------------------------------------------------

    [[nodiscard]] inline std::string dump_scopes(const debug_info& di) {
        std::string out = "[";
        for (std::size_t i = 0; i < di.scopes.size(); ++i) {
            if (i) out += ',';
            detail::scope_json(out, di.scopes[i]);
        }
        out += "]";
        return out;
    }

    // ---- dump_line_table -------------------------------------------------------

    [[nodiscard]] inline std::string dump_line_table(const debug_info& di) {
        std::string out = "[";
        for (std::size_t i = 0; i < di.line_table.size(); ++i) {
            if (i) out += ',';
            detail::line_entry_json(out, di.line_table[i]);
        }
        out += "]";
        return out;
    }

    // ---- dump_breakpoints ------------------------------------------------------

    [[nodiscard]] inline std::string
    dump_breakpoints(std::span<const breakpoint_location> bps) {
        std::string out = "[";
        for (std::size_t i = 0; i < bps.size(); ++i) {
            if (i) out += ',';
            detail::breakpoint_json(out, bps[i]);
        }
        out += "]";
        return out;
    }

    // ---- dump_debug_event ------------------------------------------------------

    [[nodiscard]] inline std::string dump_debug_event(const debug_event& e) {
        std::string out = "{\"kind\":\"";
        out += to_string(e.kind);
        out += "\",\"line\":" + std::to_string(e.at.line);
        out += ",\"col\":" + std::to_string(e.at.col);
        out += ",\"scope_index\":" + std::to_string(e.scope_index);
        out += ",\"detail\":\"";
        detail::json_escape_into(out, e.detail);
        out += "\"}";
        return out;
    }

    // ---- dump_debug_info — full DAP-style bundle -------------------------------

    [[nodiscard]] inline std::string dump_debug_info(const debug_info& di) {
        std::string out = "{\"module\":\"";
        detail::json_escape_into(out, di.module_name);
        out += "\",\"scopes\":" + dump_scopes(di);
        out += ",\"line_table\":" + dump_line_table(di);
        out += ",\"globals\":[";
        for (std::size_t i = 0; i < di.globals.size(); ++i) {
            if (i) out += ',';
            detail::variable_json(out, di.globals[i]);
        }
        out += "]}";
        return out;
    }

    // ---- dump_pipeline_stats ---------------------------------------------------

    [[nodiscard]] inline std::string
    dump_pipeline_stats(const pipeline_stats_snapshot& snap) {
        std::string out = "{";
        bool first = true;
        auto comma = [&] {
            if (!first) out += ',';
            first = false;
        };

        if (snap.parse) {
            const auto& p = *snap.parse;
            comma();
            out += "\"parse\":{";
            out += "\"source_bytes\":" + std::to_string(p.source_bytes);
            out += ",\"source_lines\":" + std::to_string(p.source_lines);
            out += ",\"total_tokens\":" + std::to_string(p.total_tokens);
            out += ",\"trivia_tokens\":" + std::to_string(p.trivia_tokens);
            out += ",\"asi_injections\":" + std::to_string(p.asi_injections);
            out += ",\"production_nodes\":" + std::to_string(p.production_nodes);
            out += ",\"max_depth\":" + std::to_string(p.max_depth);
            out += ",\"identifier_count\":" + std::to_string(p.identifier_count);
            out += ",\"literal_count\":" + std::to_string(p.literal_count);
            out += ",\"comment_bytes\":" + std::to_string(p.comment_bytes);
            out += ",\"deepest_fn_name_len\":" + std::to_string(p.deepest_fn_name_len);
            out += ",\"error_count\":" + std::to_string(p.error_count);
            out += ",\"warning_count\":" + std::to_string(p.warning_count);
            out += "}";
        }
        if (snap.lower) {
            const auto& l = *snap.lower;
            comma();
            out += "\"lower\":{";
            out += "\"structured_for_count\":" + std::to_string(l.structured_for_count);
            out += ",\"parallel_loop_count\":" + std::to_string(l.parallel_loop_count);
            out += ",\"memref_count\":" + std::to_string(l.memref_count);
            out += ",\"defer_site_count\":" + std::to_string(l.defer_site_count);
            out += ",\"exit_edge_count\":" + std::to_string(l.exit_edge_count);
            out += ",\"trap_edge_count\":" + std::to_string(l.trap_edge_count);
            out += ",\"block_count\":" + std::to_string(l.block_count);
            out += ",\"max_loop_nest\":" + std::to_string(l.max_loop_nest);
            out += ",\"lower_ns\":" + std::to_string(l.lower_ns);
            out += "}";
        }
        if (snap.execute) {
            const auto& x = *snap.execute;
            comma();
            out += "\"execute\":{";
            out += "\"lower_ns\":" + std::to_string(x.lower_ns);
            out += ",\"execute_ns\":" + std::to_string(x.execute_ns);
            out += ",\"instr_count\":" + std::to_string(x.instr_count);
            out += ",\"branch_count\":" + std::to_string(x.branch_count);
            out += ",\"block_count\":" + std::to_string(x.block_count);
            out += ",\"fallback_used\":";
            out += x.fallback_used ? "true" : "false";
            out += "}";
        }
        out += "}";
        return out;
    }

    // ============================================================================
    // NADI debug pulse (guarded) — emit a debug_event through observability.
    //
    // When NADI is present, crank_debug_pulse is a Pulse over the event's category,
    // kind name, source line, and detail. emit_debug_pulse<Sink> routes one event
    // to the sink. When NADI is absent, the whole helper collapses to an inline
    // no-op, so a debug build with no observability pays nothing.
    // ============================================================================

#ifdef CRANK_HAS_NADI

    using crank_debug_pulse = utils::nadi::Pulse<
        "crank.debug",
        utils::nadi::Field < "kind", std::uint8_t>,
    utils::nadi::Field<"line", std::uint32_t>,
    utils::nadi::Field<"scope", std::uint32_t>>;

    // Route a debug_event to the given sink (default NoSink = compiled-out).
    template <utils::nadi::SinkPolicy Sink = utils::nadi::NoSink>
    inline void emit_debug_pulse(const debug_event& e) noexcept {
        crank_debug_pulse pulse;
        std::get < 0 > (pulse.payload).value = static_cast<std::uint8_t>(e.kind);
        std::get < 1 > (pulse.payload).value = e.at.line;
        std::get < 2 > (pulse.payload).value = e.scope_index;
        Sink::emit(pulse);
    }

#else  // NADI absent — pay-for-use no-op keeps call sites portable.

    template <class Sink = void>
    inline void emit_debug_pulse(const debug_event&) noexcept {}

#endif // CRANK_HAS_NADI
} // namespace crank

// ============================================================================
// glaze JSON metadata for crank debug types — must live at global scope because
// glz::meta may only be specialized in a namespace enclosing glz.
// ============================================================================
#ifdef CRANK_HAS_GLAZE

template <>
struct glz::meta<crank::debug_variable> {
    using T = crank::debug_variable;
    static constexpr auto value = glz::object(
        "name", &T::name,
        "type_name", &T::type_name,
        "type_ref", &T::type_ref,
        "is_param", &T::is_param,
        "is_mutable", &T::is_mutable
    );
};

template <>
struct glz::meta<crank::debug_scope> {
    using T = crank::debug_scope;
    static constexpr auto value = glz::object(
        "kind", [](const T& s) { return std::string(crank::to_string(s.kind)); },
        "name", &T::name,
        "parent", &T::parent,
        "depth", &T::depth,
        "locals", &T::locals
    );
};

template <>
struct glz::meta<crank::debug_line_entry> {
    using T = crank::debug_line_entry;
    static constexpr auto value = glz::object(
        "line", [](const T& e) { return e.src.line; },
        "col", [](const T& e) { return e.src.col; },
        "offset", [](const T& e) { return e.src.offset; },
        "length", [](const T& e) { return e.src.length; },
        "fn_name", &T::fn_name,
        "hl_op_index", &T::hl_op_index,
        "block_index", &T::block_index,
        "stmt_boundary", &T::is_stmt_boundary
    );
};

template <>
struct glz::meta<crank::breakpoint_location> {
    using T = crank::breakpoint_location;
    static constexpr auto value = glz::object(
        "line", &T::line,
        "col", &T::col,
        "fn_name", &T::fn_name,
        "verified", &T::verified,
        "resolved_line", [](const T& b) { return b.resolved.line; },
        "resolved_col", [](const T& b) { return b.resolved.col; }
    );
};

#endif // CRANK_HAS_GLAZE
