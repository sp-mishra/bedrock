#pragma once

// crank/dump.hpp — JSON dump for parse_tree, Vakya AST, typed AST, symbols, module graph.
//
// C++23, header-only, no virtual, no macros.
// Namespace: crank
//
// Module 1:
//   dump_parse_tree(tree)                 → JSON
//   dump_ast(root_any, store)             → JSON (structural placeholder)
//   dump_stats(stats)                     → JSON
//
// Module 2 (semantic dumps):
//   dump_typed_ast(root_any, store, sema) → JSON (tag + type_ref + effect/cap masks per node)
//   dump_symbols(sym_table)               → JSON (module-qualified symbol table)
//   dump_module_graph(dep_graph)          → JSON (dependency graph edges + descriptor metadata)
// crank::dump_ast(root_any, store)     -> std::string  (JSON)
//
// parse_tree dump: recursive over traverse_range, emits per-node:
//   { "kind": "...", "token": "...", "children": [...] }
//
// AST dump: placeholder — full typed dump in module 2 once types are resolved.
// For now emits structural metadata from the build_result.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <any>

#include <lexy/parse_tree.hpp>

// Glaze JSON — guarded
#if __has_include("glaze/json.hpp")
#  include "glaze/json.hpp"
#  define CRANK_HAS_GLAZE 1
#endif

#include "languages/crank/source_span.hpp"
#include "languages/crank/parser_stats.hpp"
#include "languages/crank/resolve.hpp"
#include "languages/crank/module.hpp"

namespace crank {
    // ============================================================================
    // parse_tree dump
    // ============================================================================

    namespace detail {
        /// Recursive dump of a lexy parse_tree node into a nested JSON-like structure.
        struct pt_node_json {
            std::string kind;
            std::string token; // non-empty for leaf tokens
            std::vector<pt_node_json> children;
#ifdef CRANK_HAS_GLAZE
            struct glaze {
                using T = pt_node_json;
                static constexpr auto value = glz::object(
                    "kind", &T::kind,
                    "token", &T::token,
                    "children", &T::children
                );
            };
#endif
        };
    } // namespace detail

    namespace detail {
        template <typename TraverseRange>
        [[nodiscard]] inline pt_node_json
        build_json_tree(TraverseRange range) {
            using event = lexy::traverse_event;

            std::vector<pt_node_json*> stack;
            pt_node_json root;
            stack.push_back(&root);

            for (auto [ev, node] : range) {
                pt_node_json* top = stack.back();

                if (ev == event::enter) {
                    top->children.push_back({std::string(node.kind().name()), {}, {}});
                    stack.push_back(&top->children.back());
                }
                else if (ev == event::leaf) {
                    auto sv = std::string_view(node.lexeme().begin(), node.lexeme().end());
                    top->children.push_back({std::string(node.kind().name()), std::string(sv), {}});
                }
                else { // exit
                    stack.pop_back();
                }
            }
            // root.children[0] is the actual root production
            if (!root.children.empty())
                return std::move(root.children[0]);
            return root;
        }
    } // namespace detail

    /// Dump a lexy parse_tree to a JSON string.
    template <typename ParseTree>
    [[nodiscard]] std::string dump_parse_tree(const ParseTree& tree) {
        if (tree.empty()) return "{}";
        auto jt = detail::build_json_tree(tree.traverse());
#ifdef CRANK_HAS_GLAZE
        std::string out;
        auto ec = glz::write_json(jt, out);
        if (ec) return "{\"error\": \"glaze write failed\"}";
        return out;
#else
        // Minimal fallback without glaze
        std::string out;
        std::function<void(const detail::pt_node_json&, int)> emit
            = [&](const detail::pt_node_json& n, int depth) {
            std::string indent(depth * 2, ' ');
            out += indent + "{\"kind\":\"" + n.kind + "\"";
            if (!n.token.empty())
                out += ",\"token\":\"" + n.token + "\"";
            if (!n.children.empty()) {
                out += ",\"children\":[";
                for (std::size_t i = 0; i < n.children.size(); ++i) {
                    if (i) out += ',';
                    emit(n.children[i], depth + 1);
                }
                out += ']';
            }
            out += '}';
        };
        emit(jt, 0);
        return out;
#endif
    }

    // ============================================================================
    // AST dump
    // ============================================================================

    /// Simple AST dump record — carries tag symbol + stable_id + children.
    struct ast_node_json {
        std::string tag_symbol;
        std::uint32_t stable_id = 0;
        std::string value; // for terminal leaves
        std::optional<source_span> span;
        std::vector<ast_node_json> children;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = ast_node_json;
            static constexpr auto value = glz::object(
                "tag", &T::tag_symbol,
                "id", &T::stable_id,
                "value", &T::value,
                "children", &T::children
            );
        };
#endif
    };

    /// Dump the AST root (from build_result.root) to a JSON string.
    /// Currently a stub that dumps placeholder metadata; full typed dump in module 2.
    [[nodiscard]] inline std::string dump_ast(const std::any& root, const vakya::property_store& /*store*/) {
        if (!root.has_value()) return "{\"error\":\"empty root\"}";

        // Placeholder: emit a fixed structural record.
        ast_node_json rec;
        rec.tag_symbol = "fn";
        rec.stable_id = 1000u; // crank::fn_tag stable_id

#ifdef CRANK_HAS_GLAZE
        std::string out;
        auto ec = glz::write_json(rec, out);
        if (ec) return "{\"error\": \"glaze write failed\"}";
        return out;
#else
        return "{\"tag\":\"fn\",\"id\":1000}";
#endif
    }

    /// Dump parse_stats to a JSON string.
    /// Shows source metrics, token/production counts, error distribution, timings.
    [[nodiscard]] inline std::string
    dump_stats(const parse_stats& stats) {
#ifdef CRANK_HAS_GLAZE
        // Build JSON via glaze using a simple object map
        glz::generic jobj;
        jobj["source_bytes"] = stats.source_bytes;
        jobj["source_lines"] = stats.source_lines;
        jobj["total_tokens"] = stats.total_tokens;
        jobj["trivia_tokens"] = stats.trivia_tokens;
        jobj["asi_injections"] = stats.asi_injections;
        jobj["production_nodes"] = stats.production_nodes;
        jobj["max_depth"] = stats.max_depth;
        jobj["identifier_count"] = stats.identifier_count;
        jobj["literal_count"] = stats.literal_count;
        jobj["comment_bytes"] = stats.comment_bytes;
        jobj["deepest_fn_name_len"] = stats.deepest_fn_name_len;
        jobj["error_count"] = stats.error_count;
        jobj["warning_count"] = stats.warning_count;
        jobj["note_count"] = stats.note_count;

        // Timings in nanoseconds
        glz::generic timing_obj;
        timing_obj["lex_and_parse"] = stats.timings.lex_and_parse.count();
        timing_obj["ast_build"] = stats.timings.ast_build.count();
        timing_obj["total"] = stats.timings.total.count();
        jobj["timings_ns"] = timing_obj;

        // Token frequency
        glz::generic token_freq;
        for (std::size_t i = 0; i < stats.token_by_kind.size(); ++i) {
            if (stats.token_by_kind[i] > 0) {
                // Use token kind as hex index for brevity
                token_freq[std::to_string(i)] = stats.token_by_kind[i];
            }
        }
        jobj["token_by_kind"] = token_freq;

        // Production frequency
        glz::generic prod_freq;
        for (const auto& [kind, count] : stats.production_by_name) {
            prod_freq[kind] = count;
        }
        jobj["production_by_name"] = prod_freq;

        std::string out;
        auto ec = glz::write_json(jobj, out);
        if (ec) return "{\"error\": \"glaze write failed\"}";
        return out;
#else
        // Minimal fallback JSON serialization
        std::string out = "{";
        out += "\"source_bytes\":" + std::to_string(stats.source_bytes) + ",";
        out += "\"source_lines\":" + std::to_string(stats.source_lines) + ",";
        out += "\"total_tokens\":" + std::to_string(stats.total_tokens) + ",";
        out += "\"trivia_tokens\":" + std::to_string(stats.trivia_tokens) + ",";
        out += "\"asi_injections\":" + std::to_string(stats.asi_injections) + ",";
        out += "\"production_nodes\":" + std::to_string(stats.production_nodes) + ",";
        out += "\"max_depth\":" + std::to_string(stats.max_depth) + ",";
        out += "\"identifier_count\":" + std::to_string(stats.identifier_count) + ",";
        out += "\"literal_count\":" + std::to_string(stats.literal_count) + ",";
        out += "\"comment_bytes\":" + std::to_string(stats.comment_bytes) + ",";
        out += "\"deepest_fn_name_len\":" + std::to_string(stats.deepest_fn_name_len) + ",";
        out += "\"error_count\":" + std::to_string(stats.error_count) + ",";
        out += "\"warning_count\":" + std::to_string(stats.warning_count) + ",";
        out += "\"note_count\":" + std::to_string(stats.note_count) + ",";
        out += "\"timings_ns\":{";
        out += "\"lex_and_parse\":" + std::to_string(stats.timings.lex_and_parse.count()) + ",";
        out += "\"ast_build\":" + std::to_string(stats.timings.ast_build.count()) + ",";
        out += "\"total\":" + std::to_string(stats.timings.total.count());
        out += "},";
        out += "\"token_by_kind\":{},";
        out += "\"production_by_name\":{}";
        out += "}";
        return out;
#endif
    }
} // namespace crank

// ============================================================================
// Module 2 semantic dumps
// ============================================================================

#include "languages/crank/sema_types.hpp"

namespace crank {
    // ---- typed AST dump ---------------------------------------------------------

    /// typed_ast_node — per-node semantic record for JSON emission
    struct typed_ast_node_json {
        std::string tag_symbol;
        std::uint32_t stable_id = 0;
        std::string value; // terminal leaf text
        std::uint32_t type_var_index = 0; // type_ref index (0 = unresolved)
        std::uint64_t effect_mask = 0;
        std::uint64_t capability_mask = 0;
        std::optional<source_span> span;
        std::vector<typed_ast_node_json> children;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = typed_ast_node_json;
            static constexpr auto value = glz::object(
                "tag", &T::tag_symbol,
                "id", &T::stable_id,
                "value", &T::value,
                "type_var", &T::type_var_index,
                "effects", &T::effect_mask,
                "capabilities", &T::capability_mask,
                "children", &T::children
            );
        };
#endif
    };

    /// Dump typed AST (with type and effect annotations).
    /// Emits a placeholder typed record based on the structural AST and sema_context state.
    [[nodiscard]] inline std::string
    dump_typed_ast(const std::any& root,
                   const vakya::property_store& /*store*/,
                   const sema_context& /*sema*/) {
        if (!root.has_value()) return "{\"error\":\"empty root\"}";

        // Placeholder: emit a structural typed record.
        // Full per-node walk requires the typed unpack in module 3.
        typed_ast_node_json rec;
        rec.tag_symbol = "fn";
        rec.stable_id = 1000u;
        rec.type_var_index = 0u;
        rec.effect_mask = 0u;

#ifdef CRANK_HAS_GLAZE
        std::string out;
        auto ec = glz::write_json(rec, out);
        if (ec) return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        return "{\"tag\":\"fn\",\"id\":1000,\"type_var\":0,\"effects\":0}";
#endif
    }

    // ---- symbol table dump -------------------------------------------------------

    /// symbol_entry_json — one symbol for JSON serialisation
    struct symbol_entry_json {
        std::string qualified_name;
        std::string kind;
        std::string mutability;
        std::string visibility;
        std::uint32_t type_id = 0;
        bool initialized = false;
        std::uint32_t scope_depth = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = symbol_entry_json;
            static constexpr auto value = glz::object(
                "name", &T::qualified_name,
                "kind", &T::kind,
                "mutability", &T::mutability,
                "visibility", &T::visibility,
                "type_id", &T::type_id,
                "initialized", &T::initialized,
                "scope_depth", &T::scope_depth
            );
        };
#endif
    };

    namespace detail {
        [[nodiscard]] inline std::string_view to_str(symbol_kind k) noexcept {
            switch (k) {
            case symbol_kind::value: return "value";
            case symbol_kind::function: return "function";
            case symbol_kind::type_def: return "type";
            case symbol_kind::constant: return "constant";
            case symbol_kind::param: return "param";
            case symbol_kind::view: return "view";
            }
            return "unknown";
        }

        [[nodiscard]] inline std::string_view to_str(mutability_kind m) noexcept {
            switch (m) {
            case mutability_kind::immutable: return "immutable";
            case mutability_kind::mutable_: return "mutable";
            case mutability_kind::constant: return "constant";
            }
            return "unknown";
        }

        [[nodiscard]] inline std::string_view to_str(visibility_kind v) noexcept {
            switch (v) {
            case visibility_kind::module_local: return "module_local";
            case visibility_kind::exported: return "exported";
            }
            return "unknown";
        }
    } // namespace detail

    /// Dump a symbol_table to JSON.
    [[nodiscard]] inline std::string dump_symbols(const symbol_table& tbl) {
        std::vector<symbol_entry_json> rows;
        tbl.for_each([&](const std::string& k, const symbol_entry& e) {
            symbol_entry_json j;
            j.qualified_name = k;
            j.kind = std::string(detail::to_str(e.kind));
            j.mutability = std::string(detail::to_str(e.mutability));
            j.visibility = std::string(detail::to_str(e.visibility));
            j.type_id = e.type_id;
            j.initialized = e.initialized;
            j.scope_depth = e.scope_depth;
            rows.push_back(std::move(j));
        });

#ifdef CRANK_HAS_GLAZE
        std::string out;
        auto ec = glz::write_json(rows, out);
        if (ec) return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i) out += ',';
            const auto& r = rows[i];
            out += "{\"name\":\"" + r.qualified_name + "\","
                "\"kind\":\"" + r.kind + "\","
                "\"type_id\":" + std::to_string(r.type_id) + "}";
        }
        out += "]";
        return out;
#endif
    }

    // ---- module graph dump -------------------------------------------------------

    struct dep_edge_json {
        std::string importer;
        std::string importee;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = dep_edge_json;
            static constexpr auto value = glz::object("importer", &T::importer, "importee", &T::importee);
        };
#endif
    };

    struct module_desc_json {
        std::string name;
        std::string kind;
        std::string source_path;
        std::uint64_t content_hash = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = module_desc_json;
            static constexpr auto value = glz::object(
                "name", &T::name, "kind", &T::kind,
                "source_path", &T::source_path, "content_hash", &T::content_hash);
        };
#endif
    };

    struct module_graph_json {
        std::vector<module_desc_json> modules;
        std::vector<dep_edge_json> edges;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = module_graph_json;
            static constexpr auto value = glz::object("modules", &T::modules, "edges", &T::edges);
        };
#endif
    };

    namespace detail {
        [[nodiscard]] inline std::string_view to_str(module_kind k) noexcept {
            switch (k) {
            case module_kind::source: return "source";
            case module_kind::embedded_src: return "embedded_src";
            case module_kind::embedded_artifact: return "embedded_artifact";
            case module_kind::native: return "native";
            case module_kind::package_root: return "package_root";
            }
            return "unknown";
        }
    } // namespace detail

    /// Dump a dependency_graph to JSON.
    [[nodiscard]] inline std::string dump_module_graph(const dependency_graph& g) {
        module_graph_json jg;
        for (const auto& e : g.edges())
            jg.edges.push_back({e.importer, e.importee});

        // We iterate edges to enumerate modules (dep_graph has no direct module list iterator).
        // The snapshot of all edges gives us importers + importees.
        // A dedicated for_modules() would be cleaner — deferred to module 3.
        for (const auto& e : g.edges()) {
            for (const auto& name : {e.importer, e.importee}) {
                if (const auto* d = g.descriptor(name)) {
                    module_desc_json m;
                    m.name = d->name;
                    m.kind = std::string(detail::to_str(d->kind));
                    m.source_path = d->source_path;
                    m.content_hash = d->content_hash.value;
                    jg.modules.push_back(std::move(m));
                }
            }
        }

#ifdef CRANK_HAS_GLAZE
        std::string out;
        auto ec = glz::write_json(jg, out);
        if (ec) return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "{\"modules\":[],\"edges\":[";
        for (std::size_t i = 0; i < jg.edges.size(); ++i) {
            if (i) out += ',';
            out += "{\"importer\":\"" + jg.edges[i].importer
                + "\",\"importee\":\"" + jg.edges[i].importee + "\"}";
        }
        out += "]}";
        return out;
#endif
    }
} // namespace crank

// ============================================================================
// Module 3 verification dumps
// ============================================================================

#include "languages/crank/obligations.hpp"
#include "languages/crank/assumptions.hpp"
#include "languages/crank/safety.hpp"

namespace crank {
    // ---- obligation dump --------------------------------------------------------

    struct obligation_json {
        std::string label;
        std::string family;
        std::string outcome;
        bool guard_inserted = false;
        std::uint32_t offset = 0;
        std::uint32_t line = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = obligation_json;
            static constexpr auto value = glz::object(
                "label", &T::label,
                "family", &T::family,
                "outcome", &T::outcome,
                "guard", &T::guard_inserted,
                "offset", &T::offset,
                "line", &T::line
            );
        };
#endif
    };

    namespace detail {
        [[nodiscard]] inline std::string_view to_str(obligation_family f) noexcept {
            switch (f) {
            case obligation_family::bounds: return "bounds";
            case obligation_family::div_by_zero: return "div_by_zero";
            case obligation_family::range_cast: return "range_cast";
            case obligation_family::parallel_safe: return "parallel_safe";
            case obligation_family::view: return "view";
            }
            return "unknown";
        }

        [[nodiscard]] inline std::string_view to_str(vakya::types::proof_status s) noexcept {
            switch (s) {
            case vakya::types::proof_status::proven: return "proven";
            case vakya::types::proof_status::unknown: return "unknown";
            case vakya::types::proof_status::refuted: return "refuted";
            case vakya::types::proof_status::deferred: return "deferred";
            }
            return "unknown";
        }
    } // namespace detail

    /// Dump obligation_records with discharge outcomes.
    [[nodiscard]] inline std::string
    dump_obligations(const std::vector<obligation_record>& obs) {
        std::vector<obligation_json> rows;
        rows.reserve(obs.size());
        for (const auto& r : obs) {
            obligation_json j;
            j.label = r.label;
            j.family = std::string(detail::to_str(r.family));
            j.outcome = std::string(detail::to_str(r.outcome));
            j.guard_inserted = (r.outcome == vakya::types::proof_status::unknown
                || r.outcome == vakya::types::proof_status::deferred);
            j.offset = r.at.offset;
            j.line = r.at.line;
            rows.push_back(std::move(j));
        }
#ifdef CRANK_HAS_GLAZE
        std::string out;
        auto ec = glz::write_json(rows, out);
        if (ec) return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i) out += ',';
            const auto& ob = rows[i];
            out += "{\"label\":\"" + ob.label
                + "\",\"family\":\"" + ob.family
                + "\",\"outcome\":\"" + ob.outcome
                + "\",\"guard\":" + (ob.guard_inserted ? "true" : "false")
                + ",\"line\":" + std::to_string(ob.line) + "}";
        }
        out += "]";
        return out;
#endif
    }

    // ---- assumption dump --------------------------------------------------------

    struct assumption_json {
        std::string kind;
        std::string description;
        std::uint32_t line = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = assumption_json;
            static constexpr auto value = glz::object(
                "kind", &T::kind,
                "description", &T::description,
                "line", &T::line
            );
        };
#endif
    };

    namespace detail {
        [[nodiscard]] inline std::string_view to_str(assumption_kind k) noexcept {
            switch (k) {
            case assumption_kind::requires_clause: return "requires";
            case assumption_kind::proven_assertion: return "proven_assert";
            case assumption_kind::refinement_binding: return "refinement";
            case assumption_kind::if_condition: return "if_cond";
            case assumption_kind::for_range_lower: return "for_range_lower";
            case assumption_kind::for_range_upper: return "for_range_upper";
            }
            return "unknown";
        }
    } // namespace detail

    /// Dump the active assumption context at a program point.
    [[nodiscard]] inline std::string
    dump_assumptions(const assumption_context& actx) {
        std::vector<assumption_json> rows;
        rows.reserve(actx.size());
        for (const auto& e : actx.active_assumptions()) {
            assumption_json j;
            j.kind = std::string(detail::to_str(e.kind));
            j.description = e.description;
            j.line = e.at.line;
            rows.push_back(std::move(j));
        }
#ifdef CRANK_HAS_GLAZE
        std::string out;
        auto ec = glz::write_json(rows, out);
        if (ec) return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i) out += ',';
            const auto& a = rows[i];
            out += "{\"kind\":\"" + a.kind
                + "\",\"desc\":\"" + a.description + "\"}";
        }
        out += "]";
        return out;
#endif
    }

    // ---- guard dump -------------------------------------------------------------

    struct guard_json {
        std::string label;
        std::string safety_policy;
        std::uint32_t line = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = guard_json;
            static constexpr auto value = glz::object(
                "label", &T::label,
                "safety_policy", &T::safety_policy,
                "line", &T::line
            );
        };
#endif
    };

    /// Dump obligations that resulted in an inserted runtime guard.
    [[nodiscard]] inline std::string
    dump_guards(const std::vector<obligation_record>& obs,
                safety_failure policy) {
        std::vector<guard_json> rows;
        for (const auto& r : obs) {
            bool has_guard = (r.outcome == vakya::types::proof_status::unknown
                || r.outcome == vakya::types::proof_status::deferred);
            if (!has_guard) continue;
            guard_json j;
            j.label = r.label;
            j.safety_policy = std::string(to_string(policy));
            j.line = r.at.line;
            rows.push_back(std::move(j));
        }
#ifdef CRANK_HAS_GLAZE
        std::string out;
        auto ec = glz::write_json(rows, out);
        if (ec) return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i) out += ',';
            const auto& g = rows[i];
            out += "{\"label\":\"" + g.label
                + "\",\"policy\":\"" + g.safety_policy + "\"}";
        }
        out += "]";
        return out;
#endif
    }
} // namespace crank

// ============================================================================
// Module 4 execution dumps
// ============================================================================

#include "languages/crank/lower_hl.hpp"
#include "languages/crank/aot.hpp"
#include "languages/crank/execute.hpp"
#include "languages/crank/exec_hint.hpp"
#include "languages/crank/parallel.hpp"

namespace crank {
    // ---- HL MIR dump ------------------------------------------------------------

    struct hl_op_json {
        std::uint32_t id = 0;
        std::string op;
        bool is_parallel_for = false;
        std::uint8_t rank = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = hl_op_json;
            static constexpr auto value = glz::object(
                "id", &T::id,
                "op", &T::op,
                "parallel", &T::is_parallel_for,
                "rank", &T::rank
            );
        };
#endif
    };

    struct hl_mir_json {
        std::string name;
        std::vector<hl_op_json> ops;
        std::uint32_t structured_for_count = 0;
        std::uint32_t parallel_count = 0;
        std::uint32_t memref_count = 0;
        std::int64_t lower_ns = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = hl_mir_json;
            static constexpr auto value = glz::object(
                "name", &T::name,
                "ops", &T::ops,
                "structured_for_count", &T::structured_for_count,
                "parallel_count", &T::parallel_count,
                "memref_count", &T::memref_count,
                "lower_ns", &T::lower_ns
            );
        };
#endif
    };

    /// Dump HL MIR to JSON. Walks the body region of the hl_mir_function.
    [[nodiscard]] inline std::string
    dump_hl_mir(const lower_hl_result& res) {
        using namespace lithe::codegen::hl;

        hl_mir_json j;
        j.name = res.hl_fn.name;
        j.structured_for_count = res.stats.structured_for_count;
        j.parallel_count = res.stats.parallel_loop_count;
        j.memref_count = res.stats.memref_count;
        j.lower_ns = res.stats.lower_ns;

        const auto visit = [&](auto& self, const hl_region& region) -> void {
            for (const hl_block* blk = region.blocks.head; blk; blk = blk->list_node.next) {
                for (const hl_operation* op = blk->ops.head; op; op = op->list_node.next) {
                    hl_op_json oj;
                    oj.id = op->id;
                    if (op->op == hl_opcode::structured_for) {
                        oj.op = "structured_for";
                        if (std::holds_alternative<structured_for_attr>(op->attr)) {
                            const auto& sf = std::get<structured_for_attr>(op->attr);
                            oj.is_parallel_for = sf.is_parallel;
                            oj.rank = sf.rank;
                        }
                    }
                    else if (op->op == hl_opcode::memref_load) {
                        oj.op = "memref_load";
                    }
                    else if (op->op == hl_opcode::memref_store) {
                        oj.op = "memref_store";
                    }
                    else {
                        oj.op = "op";
                    }
                    j.ops.push_back(oj);
                    for (std::size_t ri = 0; ri < op->regions.size(); ++ri)
                        if (op->regions[ri]) self(self, *op->regions[ri]);
                }
            }
        };
        visit(visit, res.hl_fn.body_region);

#ifdef CRANK_HAS_GLAZE
        std::string out;
        if (auto ec = glz::write_json(j, out); ec)
            return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "{\"name\":\"" + j.name
            + "\",\"structured_for_count\":" + std::to_string(j.structured_for_count)
            + ",\"parallel_count\":" + std::to_string(j.parallel_count)
            + ",\"memref_count\":" + std::to_string(j.memref_count)
            + ",\"lower_ns\":" + std::to_string(j.lower_ns)
            + ",\"ops\":[";
        for (std::size_t i = 0; i < j.ops.size(); ++i) {
            if (i) out += ',';
            out += "{\"id\":" + std::to_string(j.ops[i].id)
                + ",\"op\":\"" + j.ops[i].op
                + "\",\"parallel\":" + (j.ops[i].is_parallel_for ? "true" : "false")
                + ",\"rank\":" + std::to_string(j.ops[i].rank) + "}";
        }
        out += "]}";
        return out;
#endif
    }

    // ---- physical MIR dump (stats summary) --------------------------------------

    struct physical_mir_json {
        std::string name;
        std::uint32_t instr_count = 0;
        std::int64_t lower_ns = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = physical_mir_json;
            static constexpr auto value = glz::object(
                "name", &T::name,
                "instr_count", &T::instr_count,
                "lower_ns", &T::lower_ns
            );
        };
#endif
    };

    /// Dump physical MIR summary from a crank_execute_result.
    [[nodiscard]] inline std::string
    dump_physical_mir(const crank_execute_result& res, std::string_view fn_name = "") {
        physical_mir_json j;
        j.name = std::string(fn_name);
        j.instr_count = res.stats.instr_count;
        j.lower_ns = res.stats.lower_ns;

#ifdef CRANK_HAS_GLAZE
        std::string out;
        if (auto ec = glz::write_json(j, out); ec)
            return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        return "{\"name\":\"" + j.name
            + "\",\"instr_count\":" + std::to_string(j.instr_count)
            + ",\"lower_ns\":" + std::to_string(j.lower_ns) + "}";
#endif
    }

    // ---- execution plan dump ----------------------------------------------------

    struct execution_plan_json {
        std::string backend_selected;
        bool fallback_fired = false;
        bool overflow_trapped = false;
        std::int64_t execute_ns = 0;
        std::string hint_kind;
        bool hint_required = false;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = execution_plan_json;
            static constexpr auto value = glz::object(
                "backend", &T::backend_selected,
                "fallback_fired", &T::fallback_fired,
                "overflow_trapped", &T::overflow_trapped,
                "execute_ns", &T::execute_ns,
                "hint_kind", &T::hint_kind,
                "hint_required", &T::hint_required
            );
        };
#endif
    };

    /// Dump execution plan (selected backend + fallback status + hint).
    [[nodiscard]] inline std::string
    dump_execution_plan(const crank_execute_result& res,
                        const execute_options& opts = {},
                        const crank_exec_attr* hint = nullptr) {
        execution_plan_json j;
        j.backend_selected = opts.primary_backend_name.empty()
                                 ? "interpreter"
                                 : opts.primary_backend_name;
        j.fallback_fired = res.fallback_fired;
        j.overflow_trapped = res.overflow_trapped;
        j.execute_ns = res.stats.execute_ns;
        if (hint) {
            j.hint_kind = std::string(to_string(hint->kind));
            j.hint_required = hint->required;
        }

#ifdef CRANK_HAS_GLAZE
        std::string out;
        if (auto ec = glz::write_json(j, out); ec)
            return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        return "{\"backend\":\"" + j.backend_selected
            + "\",\"fallback_fired\":" + (j.fallback_fired ? "true" : "false")
            + ",\"overflow_trapped\":" + (j.overflow_trapped ? "true" : "false")
            + ",\"execute_ns\":" + std::to_string(j.execute_ns)
            + ",\"hint_kind\":\"" + j.hint_kind + "\"}";
#endif
    }

    // ---- task plan dump ---------------------------------------------------------

    struct task_plan_entry_json {
        std::uint8_t rank = 0;
        std::size_t chunk = 1;
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::int64_t step = 1;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = task_plan_entry_json;
            static constexpr auto value = glz::object(
                "rank", &T::rank, "chunk", &T::chunk,
                "start", &T::start, "end", &T::end, "step", &T::step
            );
        };
#endif
    };

    /// Dump task decomposition plans from a parallel_plan_result.
    [[nodiscard]] inline std::string
    dump_task_plan(const parallel_plan_result& res) {
        std::vector<task_plan_entry_json> rows;
        rows.reserve(res.plans.size());
        for (const auto& p : res.plans) {
            task_plan_entry_json e;
            e.rank = p.rank;
            e.chunk = p.chunk;
            if (p.rank > 0) {
                e.start = p.bounds[0].start;
                e.end = p.bounds[0].end;
                e.step = p.bounds[0].step;
            }
            rows.push_back(e);
        }

#ifdef CRANK_HAS_GLAZE
        std::string out;
        if (auto ec = glz::write_json(rows, out); ec)
            return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i) out += ',';
            out += "{\"rank\":" + std::to_string(rows[i].rank)
                + ",\"chunk\":" + std::to_string(rows[i].chunk)
                + ",\"start\":" + std::to_string(rows[i].start)
                + ",\"end\":" + std::to_string(rows[i].end)
                + ",\"step\":" + std::to_string(rows[i].step) + "}";
        }
        out += "]";
        return out;
#endif
    }

    // ---- AOT key dump -----------------------------------------------------------

    /// Dump a crank_aot_key to JSON.
    [[nodiscard]] inline std::string
    dump_aot_key(const crank_aot_key& key) {
        return key.to_json();
    }
} // namespace crank

// ============================================================================
// Module 5 dumps — transaction plan, trait witnesses, instantiations
// ============================================================================

#include "languages/crank/transaction.hpp"
#include "languages/crank/generics.hpp"
#include "languages/crank/monomorphize.hpp"

#ifdef CRANK_HAS_GLAZE
// tx_plan_record lives in namespace crank (transaction.hpp) and is an external type
// we cannot add a member glaze struct to; specialize glz::meta at global scope.
template <>
struct glz::meta<crank::tx_plan_record> {
    using T = crank::tx_plan_record;
    static constexpr auto value = glz::object(
        "isolation", &T::isolation,
        "replay", &T::replay,
        "conflict", &T::conflict,
        "retry", &T::retry,
        "partial_commit", &T::partial_commit,
        "distribution_none", &T::distribution_none,
        "transactional_resource_count", &T::transactional_resource_count,
        "non_transactional_write_count", &T::non_transactional_write_count,
        "supports_snapshot", &T::supports_snapshot,
        "read_count", &T::read_count,
        "write_count", &T::write_count,
        "resource_traits_hash", &T::resource_traits_hash,
        "medha_dialect_version", &T::medha_dialect_version
    );
};
#endif

namespace crank {
    // ---- tx_plan_record glaze metadata -----------------------------------------
    // tx_plan_record is defined in transaction.hpp; glaze metadata is provided via
    // a global-scope glz::meta specialization below (see after this namespace block).

    /// Dump a tx_plan_record (transaction options + resource metadata + read/write summary).
    [[nodiscard]] inline std::string
    dump_tx_plan(const tx_plan_record& plan) {
#ifdef CRANK_HAS_GLAZE
        std::string out;
        if (auto ec = glz::write_json(plan, out); ec)
            return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "{";
        out += "\"isolation\":\"" + plan.isolation + "\"";
        out += ",\"replay\":\"" + plan.replay + "\"";
        out += ",\"conflict\":\"" + plan.conflict + "\"";
        out += ",\"retry\":" + std::to_string(plan.retry);
        out += ",\"partial_commit\":\"" + plan.partial_commit + "\"";
        out += ",\"transactional_resource_count\":" + std::to_string(plan.transactional_resource_count);
        out += ",\"read_count\":" + std::to_string(plan.read_count);
        out += ",\"write_count\":" + std::to_string(plan.write_count);

        auto hex = [](std::uint64_t v) -> std::string {
            char buf[20];
            std::snprintf(buf, sizeof(buf), "0x%016llx",
                          static_cast<unsigned long long>(v));
            return buf;
        };
        out += ",\"resource_traits_hash\":\"" + hex(plan.resource_traits_hash) + "\"";
        out += ",\"medha_dialect_version\":" + std::to_string(plan.medha_dialect_version);
        out += "}";
        return out;
#endif
    }

    // ---- trait witnesses dump ---------------------------------------------------

    struct witness_entry_json {
        std::string bound;
        std::string trait_name;
        std::string type_name;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = witness_entry_json;
            static constexpr auto value = glz::object(
                "bound", &T::bound,
                "trait", &T::trait_name,
                "type", &T::type_name
            );
        };
#endif
    };

    /// Dump resolved trait witnesses for one generic instantiation.
    [[nodiscard]] inline std::string
    dump_trait_witnesses(const std::vector<impl_witness>& witnesses) {
        std::vector<witness_entry_json> rows;
        rows.reserve(witnesses.size());
        for (const auto& w : witnesses) {
            witness_entry_json j;
            j.bound = std::string(to_string(w.bound));
            j.trait_name = w.trait_name;
            j.type_name = w.type_name;
            rows.push_back(std::move(j));
        }
#ifdef CRANK_HAS_GLAZE
        std::string out;
        if (auto ec = glz::write_json(rows, out); ec)
            return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i) out += ',';
            out += "{\"bound\":\"" + rows[i].bound
                + "\",\"type\":\"" + rows[i].type_name + "\"}";
        }
        out += "]";
        return out;
#endif
    }

    // ---- instantiations dump ---------------------------------------------------

    struct instantiation_entry_json {
        std::string generic_name;
        std::string cache_key_fingerprint;
        bool associative = false;
        bool commutative = false;
        bool parallel_safe = false;
        bool gpu_compatible = false;
        std::uint32_t witness_count = 0;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = instantiation_entry_json;
            static constexpr auto value = glz::object(
                "generic", &T::generic_name,
                "cache_key", &T::cache_key_fingerprint,
                "associative", &T::associative,
                "commutative", &T::commutative,
                "parallel_safe", &T::parallel_safe,
                "gpu_compatible", &T::gpu_compatible,
                "witness_count", &T::witness_count
            );
        };
#endif
    };

    /// Dump all instantiation records from an instantiation_registry.
    [[nodiscard]] inline std::string
    dump_instantiations(const instantiation_registry& reg) {
        auto hex = [](std::uint64_t v) -> std::string {
            char buf[20];
            std::snprintf(buf, sizeof(buf), "0x%016llx",
                          static_cast<unsigned long long>(v));
            return buf;
        };

        std::vector<instantiation_entry_json> rows;
        rows.reserve(reg.count());
        for (const auto& r : reg.all()) {
            instantiation_entry_json j;
            j.generic_name = r.summary.generic_name;
            j.cache_key_fingerprint = hex(r.summary.cache_key_fingerprint);
            j.associative = r.summary.associative;
            j.commutative = r.summary.commutative;
            j.parallel_safe = r.summary.parallel_safe;
            j.gpu_compatible = r.summary.gpu_compatible;
            j.witness_count = static_cast<std::uint32_t>(r.witnesses.size());
            rows.push_back(std::move(j));
        }

#ifdef CRANK_HAS_GLAZE
        std::string out;
        if (auto ec = glz::write_json(rows, out); ec)
            return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i) out += ',';
            out += "{\"generic\":\"" + rows[i].generic_name
                + "\",\"cache_key\":\"" + rows[i].cache_key_fingerprint
                + "\",\"associative\":" + (rows[i].associative ? "true" : "false")
                + ",\"parallel_safe\":" + (rows[i].parallel_safe ? "true" : "false")
                + ",\"witness_count\":" + std::to_string(rows[i].witness_count)
                + "}";
        }
        out += "]";
        return out;
#endif
    }
} // namespace crank  // end module 5

// ============================================================================
// Module 6 dumps — annotation records (§5b.8)
// ============================================================================

#include "languages/crank/annotation.hpp"

namespace crank {
    struct annotation_record {
        std::string fq_name;
        std::string kind;
        std::string strength;
        std::uint32_t stable_id = 0;
        std::uint32_t version = 1;
        bool preserved = false;
        std::vector<std::string> diagnostics;
#ifdef CRANK_HAS_GLAZE
        struct glaze {
            using T = annotation_record;
            static constexpr auto value = glz::object(
                "fq_name", &T::fq_name,
                "kind", &T::kind,
                "strength", &T::strength,
                "stable_id", &T::stable_id,
                "version", &T::version,
                "preserved", &T::preserved,
                "diagnostics", &T::diagnostics
            );
        };
#endif
    };

    [[nodiscard]] inline std::vector<annotation_record>
    make_annotation_records(
        std::span<const parsed_annotation> annotations,
        std::span<const annotation_resolution> resolutions) {
        std::vector<annotation_record> out;
        out.reserve(annotations.size());
        for (std::size_t i = 0; i < annotations.size() && i < resolutions.size(); ++i) {
            annotation_record r;
            const auto& res = resolutions[i];
            const auto& ann = annotations[i];
            if (res.desc) {
                r.fq_name = std::string(res.desc->name);
                r.kind = std::string(to_string(res.desc->kind));
                r.strength = std::string(to_string(res.desc->default_strength));
                r.stable_id = res.desc->stable_id;
                r.version = res.desc->version;
            }
            else {
                r.fq_name = ann.name;
                r.kind = "unknown";
                r.strength = "unknown";
            }
            r.preserved = res.preserved;
            for (const auto& d : res.diags)
                r.diagnostics.push_back(d.message);
            out.push_back(std::move(r));
        }
        return out;
    }

    [[nodiscard]] inline std::string
    dump_annotations(std::span<const annotation_record> records) {
#ifdef CRANK_HAS_GLAZE
        std::vector<annotation_record> rows(records.begin(), records.end());
        std::string out;
        if (auto ec = glz::write_json(rows, out); ec)
            return "{\"error\":\"glaze write failed\"}";
        return out;
#else
        std::string out = "[";
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (i) out += ',';
            out += "{\"fq_name\":\"" + records[i].fq_name
                + "\",\"kind\":\"" + records[i].kind
                + "\",\"strength\":\"" + records[i].strength
                + "\",\"stable_id\":" + std::to_string(records[i].stable_id)
                + ",\"preserved\":" + (records[i].preserved ? "true" : "false")
                + "}";
        }
        out += "]";
        return out;
#endif
    }
} // namespace crank  // end module 6
