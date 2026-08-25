#pragma once

// crank/build_ast.hpp — Post-order walk: lexy parse_tree → Vakya AST + ir_module.
//
// C++23, header-only, no virtual, no macros.
// Namespace: crank
//
// Walks the lexy parse_tree produced by grammar::source_file and builds a
// Vakya expression tree using crank AST tags (ast_tags.hpp).
//
// Each parse_tree production node maps to a Vakya node via make_node<Tag>.
// Literals become terminal expr<T> wrappers (untyped — typed in module 2).
// source_span for each Vakya node attached to the property_store keyed by
// structural_hash (design §3.1, impl-1.md §6).
//
// Public API:
//   crank::build_result build_ast(tree, src, store) -> build_result
//     .root       — vakya::graph::shared_expr of the source_file node
//     .diagnostics — collecting_sink with any build errors
//
// Typed AST node store (Stage 8 — dual storage):
//   crank_node_id    — alias for lang::ast_node_id (uint32_t; ~4B max nodes)
//   crank_ast_arena  — lang::ast_arena<crank_ast_node> (variant store, legacy)
//   crank_ir_module  — lang::ir_module<crank_kind, crank_node_ext> (Stage 8b)
//
// Both stores are populated simultaneously during the walk (dual-write).
// crank_ast_arena: children stored as vector<crank_node_id> in each variant.
// crank_ir_module: children stored in flat child_ids_ sidecar; ext carries
//   string/flag metadata; as_egraph_view()/as_adjacency() immediately usable.
// Module 2 may walk either store; prefer ir_mod for egraph/adjacency passes.

#include <lexy/parse_tree.hpp>
#include <lexy/input_location.hpp>

#include "vakya/vakya.hpp"
#include "vakya/property.hpp"
#include "vakya/diagnostics.hpp"
#include "languages/crank/ast_tags.hpp"
#include "languages/crank/source_span.hpp"
#include "languages/crank/parser_stats.hpp"
#include "languages/generic/ast/ast_arena.hpp"
#include "languages/generic/ir/ir_module.hpp"

#include <any>
#include <cassert>
#include <string>
#include <string_view>
#include <memory>
#include <variant>
#include <vector>

namespace crank {
    // ============================================================================
    // crank_node_id — alias for lang::ast_node_id
    // ============================================================================

    using crank_node_id = lang::ast_node_id;
    inline constexpr crank_node_id k_null_node = lang::k_null_node;

    // ============================================================================
    // Typed AST node types — index-based children (no std::any recursion).
    //
    // Each node stores children as vector<crank_node_id> into a shared
    // crank_ast_arena. Module 2 walks by id: arena[id] to get the node,
    // std::visit to dispatch on kind.
    // ============================================================================

    struct fn_node {
        std::string name;
        std::vector<crank_node_id> children;
    };

    struct block_node {
        std::vector<crank_node_id> children;
    };

    struct let_node {
        std::string name;
        std::string type_hint;
        std::vector<crank_node_id> children;
    };

    struct var_node {
        std::string name;
        std::string type_hint;
        std::vector<crank_node_id> children;
    };

    struct match_node {
        std::vector<crank_node_id> children;
    };

    struct call_node {
        std::string callee;
        std::vector<crank_node_id> args;
    };

    struct attribute_node {
        std::string name;
        std::vector<crank_node_id> args;
    };

    struct field_access_node {
        std::vector<crank_node_id> children;
    };

    struct index_node {
        std::vector<crank_node_id> children;
    };

    struct range_node {
        std::vector<crank_node_id> children;
    };

    struct tx_node {
        std::vector<crank_node_id> options;
        std::vector<crank_node_id> body;
    };

    struct tx_option_node {
        std::string key;
        std::string value;
    };

    struct tx_load_node {
        std::vector<crank_node_id> children;
    };

    struct tx_store_node {
        std::vector<crank_node_id> children;
    };

    struct tx_abort_node {
        std::vector<crank_node_id> children;
    }; // abort(error) §2.3
    struct tx_yield_node {
        std::vector<crank_node_id> children;
    }; // yield expr §2.2
    // view_decl_node: children = [generic_params node, source-type node, requires-clause nodes...]
    struct view_decl_node {
        std::string name;
        std::string backing_name;
        std::vector<crank_node_id> children;
    };

    // view_expr_node: children = [source-expr node, target-view-type node]
    struct view_expr_node {
        std::vector<crank_node_id> children;
    };

    // extern_fn_node: body-less function bound to a host symbol via @host.link.
    // host_link = the registered host name (e.g. "math.dot").
    // param_type_hints / return_type_hint = surface type strings from the declaration.
    struct extern_fn_node {
        std::string name;         // crank-side declared name
        std::string host_link;    // @host.link("...") value
        std::vector<std::string> param_names;
        std::vector<std::string> param_type_hints;
        std::string return_type_hint;
        std::vector<crank_node_id> children;
    };

    struct if_node {
        std::vector<crank_node_id> children;
    };

    struct for_node {
        std::vector<crank_node_id> children;
    };

    struct while_node {
        std::vector<crank_node_id> children;
    };

    struct return_node {
        std::vector<crank_node_id> children;
    };

    struct spawn_node {
        std::vector<crank_node_id> children;
    };

    struct await_node {
        std::vector<crank_node_id> children;
    };

    struct defer_node {
        std::vector<crank_node_id> children;
    };

    struct type_decl_node {
        std::string name;
        std::vector<crank_node_id> children;
    };

    // module_decl_node — a module declaration (`module M { ... }`, optionally
    // parametric `module M[T, const N] { ... }`). children hold the module
    // body's typed item nodes plus the generic_params subtree when present.
    // Genericity (parameter names/bounds) is recovered downstream by the
    // resolver, which builds a crank::generic_module_descriptor — mirroring how
    // fn_node / type_decl_node / view_decl_node leave generic params unstructured
    // at the walk level. has_params is a reserved genericity hint; it is left
    // false here and set true by the resolver once it recognises the
    // generic_params subtree (single source of truth for parameter recovery).
    struct module_decl_node {
        std::string name;
        bool has_params = false;
        std::vector<crank_node_id> children;
    };

    struct literal_node {
        std::string text;
    };

    struct ident_node {
        std::string name;
    };

    // Typed AST node variant — all struct types are complete above.
    // Module 2 pattern-matches on this with std::visit.
    using crank_ast_node = std::variant<
        fn_node,
        block_node,
        let_node,
        var_node,
        match_node,
        call_node,
        attribute_node,
        field_access_node,
        index_node,
        range_node,
        tx_node,
        tx_option_node,
        tx_load_node,
        tx_store_node,
        tx_abort_node,
        tx_yield_node,
        view_decl_node,
        view_expr_node,
        extern_fn_node,
        if_node,
        for_node,
        while_node,
        return_node,
        spawn_node,
        await_node,
        defer_node,
        type_decl_node,
        module_decl_node,
        literal_node,
        ident_node
    >;

    // ============================================================================
    // crank_ast_arena — lang::ast_arena specialised for crank_ast_node.
    //
    // Appending a node returns its id. Nodes are never relocated after insertion
    // within a single build pass (all ids remain valid for the lifetime of the arena).
    // ============================================================================

    using crank_ast_arena = lang::ast_arena<crank_ast_node>;

    // ============================================================================
    // crank_kind — discriminant enum for ir_module<crank_kind, crank_node_ext>.
    //
    // Mirrors the crank_ast_node variant alternatives; used as the KindEnum
    // parameter in the generic ir_node record.  Ordinals are stable — do not
    // renumber; append only.
    // ============================================================================

    enum class crank_kind : std::uint8_t {
        fn            =  0,
        block         =  1,
        let           =  2,
        var           =  3,
        match         =  4,
        call          =  5,
        attribute     =  6,
        field_access  =  7,
        index         =  8,
        range         =  9,
        tx            = 10,
        tx_option     = 11,
        tx_load       = 12,
        tx_store      = 13,
        tx_abort      = 14,
        tx_yield      = 15,
        view_decl     = 16,
        view_expr     = 17,
        extern_fn     = 18,
        if_           = 19,
        for_          = 20,
        while_        = 21,
        return_       = 22,
        spawn         = 23,
        await         = 24,
        defer         = 25,
        type_decl     = 26,
        module_decl   = 27,
        literal       = 28,
        ident         = 29,
    };

    // ============================================================================
    // crank_node_ext — per-node string/flag payload for ir_node<crank_kind, ...>.
    //
    // Carries every non-children field from the crank_ast_node variant members so
    // that ir_module<crank_kind, crank_node_ext> is a complete, lossless typed-AST
    // store.  Children are encoded in ir_module's flat child_ids_ vector (via
    // ir_module::append_children); they are NOT duplicated here.
    //
    // Fields are optional strings (empty = absent) to keep the struct uniform
    // across all crank_kind values and avoid a nested variant.
    //   name          — fn, let, var, call(callee), attribute, view_decl, type_decl,
    //                   module_decl, extern_fn, ident
    //   type_hint     — let, var
    //   text          — literal
    //   backing_name  — view_decl (backing type name)
    //   host_link     — extern_fn (@host.link value)
    //   key / value   — tx_option (key=value pair)
    //   has_params    — module_decl generic-params present flag
    //   param_names / param_type_hints / return_type_hint — extern_fn signature
    // ============================================================================

    struct crank_node_ext {
        std::string name;
        std::string type_hint;
        std::string text;
        std::string backing_name;
        std::string host_link;
        std::string key;
        std::string value;
        std::string return_type_hint;
        std::vector<std::string> param_names;
        std::vector<std::string> param_type_hints;
        bool has_params = false;
    };

    // ============================================================================
    // crank_ir_module — ir_module<crank_kind, crank_node_ext>
    //
    // The ir_module-based typed-AST store for crank.  crank_ast_arena (above) is
    // retained for backward compatibility; this alias provides access to the full
    // ir_module tooling (interning, egraph view, splice, adjacency).
    //
    // Children are stored in ir_module's flat child_ids_ sidecar (append_children).
    // String/flag metadata lives in ir_node<crank_kind, crank_node_ext>::ext.
    // structural_hash is carried directly in ir_node — no separate side-table.
    //
    // Parser (lexy) stays unchanged — this is a storage alias, not a parser change.
    // ============================================================================

    using crank_ir_module = lang::ir_module<crank_kind, crank_node_ext>;

    // Enforce at compile time that crank_ir_module is an ir_module instantiation.
    static_assert(std::is_same_v<crank_ir_module,
                                 lang::ir_module<crank_kind, crank_node_ext>>,
                  "crank_ir_module must be lang::ir_module<crank_kind, crank_node_ext>");

    // ============================================================================
    // crank_source_file — typed root of the AST
    //
    // top_level: vector<crank_ast_node> — typed top-level declaration nodes
    //            (fn, let, var, type) for compatibility and direct pattern-match.
    // arena:     flat node store for all nodes built during the walk.
    //            Children of arena-stored nodes are vector<crank_node_id>.
    // ============================================================================

    struct crank_source_file {
        std::string package_name;
        std::vector<std::string> imports; // import "x.y" targets (quotes stripped)
        std::vector<crank_ast_node> top_level; // typed top-level nodes (pattern-matchable)
        crank_ast_arena arena; // all nodes (children stored as ids) — legacy variant store
        crank_ir_module ir_mod; // flat ir_node store — egraph/adjacency-capable (Stage 8b)
    };

    // ============================================================================
    // build_result
    // ============================================================================

    struct build_result {
        /// Root Vakya node (fn_tag wrapping the source_file children).
    /// Uses std::any only to avoid exposing the full Vakya node type — the
    /// plugin/host boundary where type-erasure is accepted.
        std::any root;
        /// Typed AST root — usable by module 2 without std::any. Arena owns all nodes.
    /// Non-null when ok == true; null on parse failure.
        std::shared_ptr<crank_source_file> typed_ast_root;
        vakya::diag::collecting_sink diagnostics;
        parse_stats* stats_out = nullptr; // if non-null, collect stats
        bool ok = false;
    };

    // ============================================================================
    // build_ast — parse_tree → Vakya walk
    // ============================================================================
    //
    // This is a structural walk: each production name is matched to a crank tag.
    // Unknown productions emit a note diagnostic and produce an empty block node.
    // Terminals are wrapped as expr<std::string> carrying their lexeme text.
    //
    // Design constraints:
    //   - No virtual dispatch, no macros.
    //   - Nodes stay in a flat arena (crank_ast_arena); spans live in property_store.
    //   - Untyped numeric constants: stored as literal_node until module 2.

    namespace detail {
        // Identify interesting production names from the grammar.
        // lexy type_name<T> (NsCount=1) retains one namespace level for types in
        // nested namespaces, so crank::grammar::func_decl becomes "grammar::func_decl".
        // Strip the last "::" prefix so callers compare against bare names.
        [[nodiscard]] inline std::string_view
        node_kind_name(const auto& node) noexcept {
            std::string_view name = node.kind().name();
            if (auto pos = name.rfind("::"); pos != std::string_view::npos)
                name.remove_prefix(pos + 2);
            return name;
        }
    } // namespace detail

    // ============================================================================
    // AstBuilder — stateful walker accumulating Vakya nodes + spans
    // ============================================================================

    class AstBuilder {
    public:
        explicit AstBuilder(std::string_view src, vakya::property_store& store,
                            parse_stats* stats = nullptr)
            : src_(src), store_(store), stats_(stats),
              typed_root_(std::make_shared<crank_source_file>()) {}

        /// Run the walk on a traverse_range from a parse_tree.
    /// Returns the root Vakya node (opaque std::any — host/plugin boundary only).
        template <typename TraverseRange>
        [[nodiscard]] std::any walk(TraverseRange range) {
            struct Frame {
                std::string kind;
                std::vector<std::any> children; // std::any only within this walk frame
                std::vector<crank_node_id> typed_children; // typed ids collected per frame
                std::vector<lang::ir_node_id> ir_children;  // ir_module ids collected per frame
                std::uint32_t begin_offset = 0;
            };

            std::vector<Frame> stack;
            stack.push_back({});
            uint32_t current_depth = 0;

            for (auto [ev, node] : range) {
                using event = lexy::traverse_event;

                if (ev == event::enter) {
                    Frame f;
                    f.kind = std::string(detail::node_kind_name(node));
                    stack.push_back(std::move(f));

                    if (stats_) {
                        ++stats_->production_nodes;
                        ++current_depth;
                        if (current_depth > stats_->max_depth)
                            stats_->max_depth = current_depth;
                        ++stats_->production_by_name[f.kind];
                    }
                }
                else if (ev == event::leaf) {
                    auto lexeme_sv = std::string_view(
                        node.lexeme().begin(), node.lexeme().end());
                    auto terminal = vakya::as_expr(std::string(lexeme_sv));
                    Frame f;
                    f.kind = std::string(detail::node_kind_name(node));
                    f.children.push_back(std::move(terminal));

                    // Push a typed literal/ident node into the arena + ir_mod.
                    crank_node_id nid = k_null_node;
                    lang::ir_node_id ir_id = lang::k_null_ir;
                    if (typed_root_) {
                        const auto kind_sv = std::string_view(f.kind);
                        if (kind_sv.find("ident") != std::string_view::npos) {
                            nid = typed_root_->arena.push(ident_node{std::string(lexeme_sv)});
                            ir_id = push_ir_leaf(crank_kind::ident, lexeme_sv);
                        }
                        else {
                            nid = typed_root_->arena.push(literal_node{std::string(lexeme_sv)});
                            ir_id = push_ir_leaf(crank_kind::literal, lexeme_sv);
                        }
                    }

                    stack.back().children.push_back(std::move(f));
                    if (nid != k_null_node)
                        stack.back().typed_children.push_back(nid);
                    if (ir_id != lang::k_null_ir)
                        stack.back().ir_children.push_back(ir_id);

                    if (stats_) {
                        ++stats_->total_tokens;
                        if (f.kind.find("trivia_") != std::string_view::npos) {
                            ++stats_->trivia_tokens;
                            if (f.kind.find("comment") != std::string_view::npos)
                                stats_->comment_bytes +=
                                    static_cast<std::uint32_t>(lexeme_sv.size());
                        }
                        else if (f.kind == "stmt_term") {
                            ++stats_->asi_injections;
                        }
                        else if (f.kind.find("ident") != std::string_view::npos) {
                            ++stats_->identifier_count;
                            const auto len = static_cast<std::uint32_t>(lexeme_sv.size());
                            if (len > stats_->deepest_fn_name_len)
                                stats_->deepest_fn_name_len = len;
                        }
                        else if (f.kind.find("literal") != std::string_view::npos ||
                            f.kind.find("int") != std::string_view::npos ||
                            f.kind.find("float") != std::string_view::npos ||
                            f.kind.find("string") != std::string_view::npos ||
                            f.kind.find("bool") != std::string_view::npos) {
                            ++stats_->literal_count;
                        }
                    }
                }
                else { // exit
                    auto frame = std::move(stack.back());
                    stack.pop_back();

                    if (stats_) --current_depth;

                    // Build typed node for this production, push into arena + ir_mod, record ids.
                    crank_node_id typed_id = k_null_node;
                    lang::ir_node_id ir_id = lang::k_null_ir;
                    if (typed_root_) {
                        typed_id = build_typed_node(
                            frame.kind, frame.typed_children);
                        ir_id = build_ir_node(
                            frame.kind, frame.typed_children, frame.ir_children);
                    }
                    if (typed_id != k_null_node)
                        stack.back().typed_children.push_back(typed_id);
                    if (ir_id != lang::k_null_ir)
                        stack.back().ir_children.push_back(ir_id);

                    auto node_any = build_vakya_node(frame.kind, frame.children);
                    stack.back().children.push_back(std::move(node_any));
                }
            }

            if (stack.size() == 1 && stack[0].children.size() == 1)
                return std::move(stack[0].children[0]);
            diagnostics_.on_diagnostic(
                make_error("crank.ast.empty_tree", "parse_tree produced no root node"));
            return {};
        }

        [[nodiscard]] vakya::diag::collecting_sink& diagnostics() { return diagnostics_; }

        [[nodiscard]] std::shared_ptr<crank_source_file> typed_root() {
            return std::move(typed_root_);
        }

    private:
        std::string_view src_;
        vakya::property_store& store_;
        parse_stats* stats_;
        vakya::diag::collecting_sink diagnostics_;
        std::shared_ptr<crank_source_file> typed_root_;

        // Extract first ident text from typed children, falling back to first literal text.
        // Scans all children because keyword tokens (e.g. `fn`) arrive as literal_node
        // before the ident (e.g. the function name) in productions like func_decl.
        [[nodiscard]] std::string first_child_text(
            const std::vector<crank_node_id>& children) const {
            if (!typed_root_ || children.empty()) return {};
            std::string fallback;
            for (auto cid : children) {
                std::string name_val, text_val;
                std::visit([&](const auto& n) {
                    if constexpr (requires { n.name; }) name_val = n.name;
                    if constexpr (requires { n.text; }) text_val = n.text;
                }, typed_root_->arena[cid]);
                if (!name_val.empty()) return name_val;   // ident_node — prefer over literal
                if (fallback.empty() && !text_val.empty()) fallback = std::move(text_val);
            }
            return fallback;
        }

        // Deep-first leaf text: descend through wrapper nodes (block/children)
        // to the first ident/literal payload. Used for productions that wrap
        // their terminal (e.g. import_decl → string_lit → literal leaf).
        [[nodiscard]] std::string deep_first_text(crank_node_id id) const {
            if (!typed_root_ || id == k_null_node) return {};
            const auto& node = typed_root_->arena[id];
            return std::visit([&](const auto& n) -> std::string {
                if constexpr (requires { n.name; }) { if (!n.name.empty()) return n.name; }
                if constexpr (requires { n.text; }) { if (!n.text.empty()) return n.text; }
                if constexpr (requires { n.children; }) {
                    for (auto cid : n.children) {
                        auto t = deep_first_text(cid);
                        if (!t.empty()) return t;
                    }
                }
                return {};
            }, node);
        }

        // Concatenate all leaf text under a subtree, in order. Used for string
        // literals whose content the lexer splits into multiple leaf tokens
        // (e.g. `dsl::quoted` inner characters/segments).
        void deep_concat_text(crank_node_id id, std::string& out) const {
            if (!typed_root_ || id == k_null_node) return;
            std::visit([&](const auto& n) {
                if constexpr (requires { n.text; }) out += n.text;
                else if constexpr (requires { n.name; }) out += n.name;
                if constexpr (requires { n.children; }) {
                    for (auto cid : n.children) deep_concat_text(cid, out);
                }
            }, typed_root_->arena[id]);
        }

        // push_ir_leaf — push a leaf (literal/ident) ir_node into ir_mod.
        // text is stored in ext.name (ident) or ext.text (literal).
        [[nodiscard]] lang::ir_node_id
        push_ir_leaf(crank_kind k, std::string_view text) {
            if (!typed_root_) return lang::k_null_ir;
            lang::ir_node<crank_kind, crank_node_ext> nd{};
            nd.kind = k;
            if (k == crank_kind::ident)
                nd.ext.name = std::string(text);
            else
                nd.ext.text = std::string(text);
            return typed_root_->ir_mod.push(nd);
        }

        // build_ir_node — mirror of build_typed_node for ir_mod.
        // Pushes an ir_node with kind + ext fields, then appends ir_children
        // as the node's children in the sidecar. Returns the ir_node_id.
        // source_file sets ir_mod root; import_decl returns k_null_ir (metadata only).
        [[nodiscard]] lang::ir_node_id
        build_ir_node(std::string_view kind,
                      const std::vector<crank_node_id>& typed_children,
                      std::vector<lang::ir_node_id>& ir_children) {
            if (!typed_root_) return lang::k_null_ir;
            auto& ir_mod = typed_root_->ir_mod;

            auto push_node = [&](crank_kind k, crank_node_ext ext = {}) -> lang::ir_node_id {
                lang::ir_node<crank_kind, crank_node_ext> nd{};
                nd.kind = k;
                nd.ext  = std::move(ext);
                const lang::ir_node_id nid = ir_mod.push(nd);
                if (!ir_children.empty())
                    ir_mod.append_children(nid, ir_children);
                return nid;
            };

            if (kind == "source_file") {
                // Root node — set ir_mod root to this node's id.
                lang::ir_node<crank_kind, crank_node_ext> nd{};
                nd.kind = crank_kind::block; // source_file represented as block root
                if (!typed_root_->package_name.empty())
                    nd.ext.name = typed_root_->package_name;
                const lang::ir_node_id nid = ir_mod.push(nd);
                if (!ir_children.empty())
                    ir_mod.append_children(nid, ir_children);
                ir_mod.set_root(nid);
                return lang::k_null_ir; // already set as root; don't propagate up
            }
            else if (kind == "ident_token") {
                // Collapse the ident_token production into a plain ident_node so
                // func_decl (and others) see the name directly in typed_children.
                std::string name = first_child_text(typed_children);
                if (name.empty()) return lang::k_null_ir;
                return push_ir_leaf(crank_kind::ident, name);
            }
            else if (kind == "import_decl") {
                return lang::k_null_ir; // metadata captured in typed_root_->imports
            }
            else if (kind == "func_decl") {
                crank_node_ext ext;
                ext.name = first_child_text(typed_children);
                return push_node(crank_kind::fn, std::move(ext));
            }
            else if (kind == "block") {
                return push_node(crank_kind::block);
            }
            else if (kind == "let_stmt" || kind == "let_decl") {
                crank_node_ext ext;
                ext.name = first_child_text(typed_children);
                return push_node(crank_kind::let, std::move(ext));
            }
            else if (kind == "var_stmt" || kind == "var_decl") {
                crank_node_ext ext;
                ext.name = first_child_text(typed_children);
                return push_node(crank_kind::var, std::move(ext));
            }
            else if (kind == "match_stmt") {
                return push_node(crank_kind::match);
            }
            else if (kind == "transaction_expr") {
                return push_node(crank_kind::tx);
            }
            else if (kind == "transaction_arg") {
                crank_node_ext ext;
                if (typed_children.size() >= 2) {
                    ext.key   = first_child_text({typed_children[0]});
                    ext.value = first_child_text({typed_children[1]});
                }
                return push_node(crank_kind::tx_option, std::move(ext));
            }
            else if (kind == "tx_load_expr") {
                return push_node(crank_kind::tx_load);
            }
            else if (kind == "tx_store_stmt") {
                return push_node(crank_kind::tx_store);
            }
            else if (kind == "tx_abort_stmt") {
                return push_node(crank_kind::tx_abort);
            }
            else if (kind == "tx_yield_stmt") {
                return push_node(crank_kind::tx_yield);
            }
            else if (kind == "view_decl") {
                crank_node_ext ext;
                if (typed_children.size() >= 1) ext.name         = first_child_text({typed_children[0]});
                if (typed_children.size() >= 2) ext.backing_name = first_child_text({typed_children[1]});
                return push_node(crank_kind::view_decl, std::move(ext));
            }
            else if (kind == "view_expr") {
                return push_node(crank_kind::view_expr);
            }
            else if (kind == "extern_fn_decl") {
                crank_node_ext ext;
                ext.name = first_child_text(typed_children);
                return push_node(crank_kind::extern_fn, std::move(ext));
            }
            else if (kind == "call_expr" || kind == "call_stmt") {
                crank_node_ext ext;
                ext.name = first_child_text(typed_children); // callee name
                return push_node(crank_kind::call, std::move(ext));
            }
            else if (kind == "field_access_expr") {
                return push_node(crank_kind::field_access);
            }
            else if (kind == "index_expr") {
                return push_node(crank_kind::index);
            }
            else if (kind == "range_expr") {
                return push_node(crank_kind::range);
            }
            else if (kind == "attribute") {
                crank_node_ext ext;
                ext.name = first_child_text(typed_children);
                return push_node(crank_kind::attribute, std::move(ext));
            }
            else if (kind == "type_decl") {
                crank_node_ext ext;
                ext.name = first_child_text(typed_children);
                return push_node(crank_kind::type_decl, std::move(ext));
            }
            else if (kind == "module_decl") {
                crank_node_ext ext;
                ext.name = first_child_text(typed_children);
                return push_node(crank_kind::module_decl, std::move(ext));
            }
            else if (kind == "if_stmt" || kind == "if_expr") {
                return push_node(crank_kind::if_);
            }
            else if (kind == "for_stmt") {
                return push_node(crank_kind::for_);
            }
            else if (kind == "while_stmt") {
                return push_node(crank_kind::while_);
            }
            else if (kind == "return_stmt") {
                return push_node(crank_kind::return_);
            }
            else if (kind == "spawn_expr") {
                return push_node(crank_kind::spawn);
            }
            else if (kind == "await_expr") {
                return push_node(crank_kind::await);
            }
            else if (kind == "defer_stmt") {
                return push_node(crank_kind::defer);
            }
            // Unknown production → generic block node in ir_mod.
            return push_node(crank_kind::block);
        }

        // Build a typed AST node and push it to the arena; return its id.
        // Top-level declaration nodes are also pushed to typed_root_->top_level.
        [[nodiscard]] crank_node_id
        build_typed_node(std::string_view kind,
                         std::vector<crank_node_id>& children) {
            if (!typed_root_) return k_null_node;
            auto& arena = typed_root_->arena;

            if (kind == "source_file") {
                // source_file is the root; top_level already accumulated.
                // The first child subtree carries the `package` clause ident.
                if (typed_root_->package_name.empty() && !children.empty())
                    typed_root_->package_name = deep_first_text(children.front());
                // Push a block_node so arena stays in parity with ir_mod (Stage 8b).
                return arena.push(block_node{children});
            }
            else if (kind == "ident_token") {
                // Collapse into ident_node so callers see the bare name.
                std::string name = first_child_text(children);
                if (name.empty()) return k_null_node;
                return arena.push(ident_node{std::move(name)});
            }
            else if (kind == "import_decl") {
                // import "x.y" — capture the string-literal payload. The lexer
                // may split the content across leaves, so concatenate, then
                // strip any surrounding quote characters.
                if (!children.empty()) {
                    std::string raw;
                    deep_concat_text(children.back(), raw);
                    while (!raw.empty() && (raw.front() == '"')) raw.erase(raw.begin());
                    while (!raw.empty() && (raw.back() == '"')) raw.pop_back();
                    if (!raw.empty())
                        typed_root_->imports.push_back(std::move(raw));
                }
                return k_null_node;
            }
            else if (kind == "func_decl") {
                fn_node fn;
                fn.name = first_child_text(children);
                fn.children = children;
                auto id = arena.push(fn_node{fn});
                typed_root_->top_level.push_back(std::move(fn));
                return id;
            }
            else if (kind == "block") {
                return arena.push(block_node{children});
            }
            else if (kind == "let_stmt" || kind == "let_decl") {
                let_node n;
                n.name = first_child_text(children);
                n.children = children;
                auto id = arena.push(n);
                typed_root_->top_level.push_back(std::move(n));
                return id;
            }
            else if (kind == "var_stmt" || kind == "var_decl") {
                var_node n;
                n.name = first_child_text(children);
                n.children = children;
                auto id = arena.push(n);
                typed_root_->top_level.push_back(std::move(n));
                return id;
            }
            else if (kind == "match_stmt") {
                return arena.push(match_node{children});
            }
            else if (kind == "transaction_expr") {
                tx_node n;
                n.body = children;
                return arena.push(std::move(n));
            }
            else if (kind == "transaction_arg") {
                tx_option_node n;
                if (children.size() >= 2) {
                    n.key = first_child_text({children[0]});
                    n.value = first_child_text({children[1]});
                }
                return arena.push(std::move(n));
            }
            else if (kind == "tx_load_expr") {
                return arena.push(tx_load_node{children});
            }
            else if (kind == "tx_store_stmt") {
                return arena.push(tx_store_node{children});
            }
            else if (kind == "tx_abort_stmt") {
                return arena.push(tx_abort_node{children});
            }
            else if (kind == "tx_yield_stmt") {
                return arena.push(tx_yield_node{children});
            }
            else if (kind == "view_decl") {
                view_decl_node n;
                // children[0] = view name ident, children[1] = backing name ident (if present),
                // remaining = generic params / source type / requires nodes
                if (children.size() >= 1) n.name = first_child_text({children[0]});
                if (children.size() >= 2) n.backing_name = first_child_text({children[1]});
                n.children = children;
                auto id = arena.push(n);
                typed_root_->top_level.push_back(std::move(n));
                return id;
            }
            else if (kind == "view_expr") {
                return arena.push(view_expr_node{children});
            }
            else if (kind == "extern_fn_decl") {
                extern_fn_node n;
                n.name = first_child_text(children);
                // host_link extracted by the caller from the preceding @host.link attribute;
                // stored here as empty — filled in by the AST post-processor in context.hpp.
                n.children = children;
                auto id = arena.push(n);
                typed_root_->top_level.push_back(std::move(n));
                return id;
            }
            else if (kind == "call_expr" || kind == "call_stmt") {
                call_node n;
                n.callee = first_child_text(children);
                n.args = children;
                return arena.push(std::move(n));
            }
            else if (kind == "field_access_expr") {
                return arena.push(field_access_node{children});
            }
            else if (kind == "index_expr") {
                return arena.push(index_node{children});
            }
            else if (kind == "range_expr") {
                return arena.push(range_node{children});
            }
            else if (kind == "attribute") {
                attribute_node n;
                n.name = first_child_text(children);
                n.args = children;
                return arena.push(std::move(n));
            }
            else if (kind == "type_decl") {
                type_decl_node n;
                n.name = first_child_text(children);
                n.children = children;
                auto id = arena.push(n);
                typed_root_->top_level.push_back(std::move(n));
                return id;
            }
            else if (kind == "module_decl") {
                module_decl_node n;
                n.name = first_child_text(children);
                n.children = children;
                auto id = arena.push(n);
                typed_root_->top_level.push_back(std::move(n));
                return id;
            }
            else if (kind == "if_stmt" || kind == "if_expr") {
                return arena.push(if_node{children});
            }
            else if (kind == "for_stmt") {
                return arena.push(for_node{children});
            }
            else if (kind == "while_stmt") {
                return arena.push(while_node{children});
            }
            else if (kind == "return_stmt") {
                return arena.push(return_node{children});
            }
            else if (kind == "spawn_expr") {
                return arena.push(spawn_node{children});
            }
            else if (kind == "await_expr") {
                return arena.push(await_node{children});
            }
            else if (kind == "defer_stmt") {
                return arena.push(defer_node{children});
            }
            return arena.push(block_node{children});
        }

        // Build the legacy Vakya node (std::any wrapper) — only used at the
        // plugin/host output boundary (build_result.root).
        [[nodiscard]] std::any
        build_vakya_node(std::string_view kind, std::vector<std::any>& children) {
            auto leaf_text = [](const std::any& c) -> std::string {
                if (!c.has_value()) return {};
                if (const auto* s = std::any_cast<std::string>(&c)) return *s;
                return {};
            };
            std::string first_text;
            if (!children.empty()) first_text = leaf_text(children[0]);
            (void)first_text;

            if (kind == "source_file") {
                if (!typed_root_) typed_root_ = std::make_shared<crank_source_file>();
                return make_variadic<crank::fn_tag>(children);
            }
            else if (kind == "func_decl") {
                return make_variadic<crank::fn_tag>(children);
            }
            else if (kind == "block") {
                return make_variadic<crank::block_tag>(children);
            }
            else if (kind == "let_stmt" || kind == "let_decl") {
                return make_variadic<crank::let_tag>(children);
            }
            else if (kind == "var_stmt" || kind == "var_decl") {
                return make_variadic<crank::var_tag>(children);
            }
            else if (kind == "match_stmt") {
                return make_variadic<crank::match_tag>(children);
            }
            else if (kind == "transaction_expr") {
                return make_variadic<crank::transaction_tag>(children);
            }
            else if (kind == "transaction_arg") {
                return make_variadic<crank::transaction_option_tag>(children);
            }
            else if (kind == "tx_load_expr") {
                return make_variadic<crank::tx_load_tag>(children);
            }
            else if (kind == "tx_store_stmt") {
                return make_variadic<crank::tx_store_tag>(children);
            }
            else if (kind == "tx_abort_stmt") {
                return make_variadic<crank::tx_abort_tag>(children);
            }
            else if (kind == "tx_yield_stmt") {
                return make_variadic<crank::tx_yield_tag>(children);
            }
            else if (kind == "view_decl") {
                return make_variadic<crank::view_decl_tag>(children);
            }
            else if (kind == "view_expr") {
                return make_variadic<crank::view_expr_tag>(children);
            }
            else if (kind == "extern_fn_decl") {
                return make_variadic<crank::extern_fn_tag>(children);
            }
            else if (kind == "call_expr" || kind == "call_stmt") {
                return make_variadic<crank::crank_call_tag>(children);
            }
            else if (kind == "field_access_expr") {
                return make_variadic<crank::field_access_tag>(children);
            }
            else if (kind == "index_expr") {
                return make_variadic<crank::index_tag>(children);
            }
            else if (kind == "range_expr") {
                return make_variadic<crank::range_tag>(children);
            }
            else if (kind == "attribute") {
                return make_variadic<crank::attribute_tag>(children);
            }
            else if (kind == "type_decl") {
                return make_variadic<crank::block_tag>(children);
            }
            return make_variadic<crank::block_tag>(children);
        }

        template <typename Tag>
        [[nodiscard]] std::any
        make_variadic(std::vector<std::any>& children) {
            std::string summary;
            summary.reserve(64);
            for (auto& c : children) {
                if (c.has_value()) summary += "[child]";
            }
            auto terminal = vakya::as_expr(std::move(summary));
            return vakya::make_node<Tag>(terminal);
        }
    };

    // ============================================================================
    // build_ast — public entry
    // ============================================================================

    template <typename ParseTree>
    [[nodiscard]] build_result
    build_ast(const ParseTree& tree, std::string_view src, vakya::property_store& store,
              parse_stats* stats = nullptr) {
        build_result result;
        if (tree.empty()) {
            result.diagnostics.on_diagnostic(
                make_error("crank.ast.empty_input", "parse_tree is empty — no source?"));
            result.ok = false;
            return result;
        }

        AstBuilder builder(src, store, stats);
        result.root = builder.walk(tree.traverse());
        result.typed_ast_root = builder.typed_root();
        result.diagnostics = std::move(builder.diagnostics());
        result.ok = !result.diagnostics.has_errors();
        return result;
    }
} // namespace crank

