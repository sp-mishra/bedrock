#pragma once

// taranga/build_ast.hpp — Both frontends → one generic typed AST (ir_module).
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// This is the convergence point of the two frontends. A WAT parse_tree
// (parser_wat.hpp) and a decoded raw_module (decoder_bin.hpp) both build the SAME
// taranga_ir_module here, so downstream bands (validate → ssa → lower → engine)
// never learn which surface the module came from. Structural parity is the whole
// point: `(func (param i32) (i32.add …))` and its assembled `.wasm` produce
// isomorphic trees, hence identical structural_hash roots.
//
// Node model — kinds mirror ast_tags.hpp one-to-one. Children are stored in the
// ir_module flat sidecar (append_children); non-child data (head keyword, literal
// text, value-type spelling, folded numeric bits) lives in taranga_node_ext.
//
// WAT interpretation is head-keyword driven: WAT is generic S-expressions, so the
// parse_tree productions are all `sexpr`/`element`/`atom`. We classify each list
// by its first atom (the head) — `module`, `func`, `i32.const`, `i32.add`, … —
// exactly as the WAT spec's abbreviation-expanded folded form does.

#include "languages/taranga/decoder_bin.hpp"
#include "languages/taranga/lexer.hpp"
#include "languages/taranga/source_span.hpp"
#include "languages/taranga/wasm_types.hpp"

#include "languages/generic/ir/ir_module.hpp"
#include "languages/generic/ir/node.hpp"

#include "vakya/diagnostics.hpp"

#include <lexy/parse_tree.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taranga {

    // =========================================================================
    // taranga_kind — ir_module discriminant. Ordinals stable; append only.
    // Mirrors ast_tags.hpp.
    // =========================================================================

    enum class taranga_kind : std::uint8_t {
        module_       = 0,
        type          = 1,  // (type (func (param …) (result …)))
        import        = 2,
        export_       = 3,
        func          = 4,
        param         = 5,
        result        = 6,
        local         = 7,
        global        = 8,
        table         = 9,
        memory        = 10,
        elem          = 11,
        data          = 12,
        start         = 13,
        body          = 14, // function instruction sequence
        instr         = 15, // generic numeric/plain instruction (head keyword in ext.head)
        block         = 16,
        loop          = 17,
        if_           = 18,
        br            = 19, // br / br_if / br_table
        call          = 20, // call / call_indirect
        const_        = 21, // i32.const / f64.const …
        select        = 22,
        local_access  = 23, // local.get / local.set / local.tee
        global_access = 24, // global.get / global.set
        mem_op        = 25, // *.load / *.store  (memarg in ext)
        unreachable   = 26,
        name          = 27, // $id or export name — leaf
    };

    [[nodiscard]] constexpr std::string_view kind_spelling(taranga_kind k) noexcept {
        switch (k) {
        case taranga_kind::module_: return "module";
        case taranga_kind::type: return "type";
        case taranga_kind::import: return "import";
        case taranga_kind::export_: return "export";
        case taranga_kind::func: return "func";
        case taranga_kind::param: return "param";
        case taranga_kind::result: return "result";
        case taranga_kind::local: return "local";
        case taranga_kind::global: return "global";
        case taranga_kind::table: return "table";
        case taranga_kind::memory: return "memory";
        case taranga_kind::elem: return "elem";
        case taranga_kind::data: return "data";
        case taranga_kind::start: return "start";
        case taranga_kind::body: return "body";
        case taranga_kind::instr: return "instr";
        case taranga_kind::block: return "block";
        case taranga_kind::loop: return "loop";
        case taranga_kind::if_: return "if";
        case taranga_kind::br: return "br";
        case taranga_kind::call: return "call";
        case taranga_kind::const_: return "const";
        case taranga_kind::select: return "select";
        case taranga_kind::local_access: return "local_access";
        case taranga_kind::global_access: return "global_access";
        case taranga_kind::mem_op: return "mem_op";
        case taranga_kind::unreachable: return "unreachable";
        case taranga_kind::name: return "name";
        }
        return "?";
    }

    // =========================================================================
    // taranga_node_ext — per-node metadata carried alongside kind + children.
    //
    // head       — the WAT/opcode keyword ("i32.add", "local.get", …). This is the
    //              single source of opcode identity; opcode_map.hpp reads it.
    // text       — literal text (const value), or an identifier/name spelling.
    // value      — folded numeric bits for const_ nodes (see lexer::parse_int_literal
    //              / binary immediates). Interpretation follows `vtype`.
    // vtype      — value_type of a param/result/local/global/const (spelling()).
    // immediate  — a plain integer immediate (local index, br depth, memarg align/off).
    // has_value  — const_ nodes only: whether `value` is populated.
    // =========================================================================

    struct taranga_node_ext {
        std::string   head;
        std::string   text;
        std::uint64_t value = 0;
        value_type    vtype = value_type::i32;
        std::uint32_t immediate = 0;
        std::uint32_t immediate2 = 0; // memarg offset (immediate = align), call_indirect typeidx
        bool          has_value = false;
        bool          has_vtype = false;
    };

    using taranga_ir_module = lang::ir_module<taranga_kind, taranga_node_ext>;

    // A fully-built module: the typed IR plus the resolved type section (so both
    // frontends expose func signatures uniformly to the validator) and diagnostics.
    struct taranga_module {
        taranga_ir_module ir;
        std::vector<func_type> types;         // module type section (both frontends)
        std::vector<std::uint32_t> func_types; // typeidx per defined function
        std::optional<std::uint32_t> start_function;
        vakya::diag::collecting_sink diagnostics;

        [[nodiscard]] bool ok() const noexcept { return !diagnostics.has_errors(); }
    };

    // =========================================================================
    // Node factory — small helpers so both builders push nodes identically.
    // =========================================================================

    namespace detail {

        [[nodiscard]] inline lang::ir_node_id
        push_node(taranga_ir_module& ir, taranga_kind kind, taranga_node_ext ext = {},
                  byte_span span = {}) {
            lang::ir_node<taranga_kind, taranga_node_ext> nd{};
            nd.kind = kind;
            nd.span = span;
            nd.ext = std::move(ext);
            return ir.push(nd);
        }

        inline void attach(taranga_ir_module& ir, lang::ir_node_id parent,
                           const std::vector<lang::ir_node_id>& kids) {
            if (!kids.empty()) ir.append_children(parent, kids);
        }

    } // namespace detail

    // =========================================================================
    // Binary path — raw_module → taranga_ir_module.
    //
    // The decoder already produced typed tables, so this is a direct structural
    // projection: module → (type|import|func|table|memory|global|export|start|
    // elem|data|code) children. Function bodies stay as raw instruction bytes on
    // the func node's ext.text (hex is not decoded here — ssa_build walks the
    // byte stream with opcode_map). This keeps binary intake O(module size) and
    // defers per-instruction structure to the SSA band, matching the WAT path
    // where instructions are only structured, not yet SSA-formed.
    // =========================================================================

    [[nodiscard]] inline taranga_module
    build_from_binary(const raw_module& raw) {
        taranga_module out;
        out.types = raw.types;
        out.func_types = raw.functions;
        out.start_function = raw.start_function;

        auto& ir = out.ir;
        std::vector<lang::ir_node_id> module_children;

        // Type section.
        for (const auto& ft : raw.types) {
            taranga_node_ext ext;
            std::vector<lang::ir_node_id> sig_children;
            for (auto p : ft.params) {
                taranga_node_ext pe;
                pe.vtype = p;
                pe.has_vtype = true;
                pe.text = std::string(spelling(p));
                sig_children.push_back(detail::push_node(ir, taranga_kind::param, std::move(pe)));
            }
            for (auto r : ft.results) {
                taranga_node_ext re;
                re.vtype = r;
                re.has_vtype = true;
                re.text = std::string(spelling(r));
                sig_children.push_back(detail::push_node(ir, taranga_kind::result, std::move(re)));
            }
            const auto tid = detail::push_node(ir, taranga_kind::type, std::move(ext));
            detail::attach(ir, tid, sig_children);
            module_children.push_back(tid);
        }

        // Imports.
        for (const auto& im : raw.imports) {
            taranga_node_ext ext;
            ext.head = std::string(spelling(im.kind));
            ext.text = im.module_name + "." + im.field_name;
            ext.immediate = im.type_index; // meaningful for function imports
            module_children.push_back(
                detail::push_node(ir, taranga_kind::import, std::move(ext), im.span));
        }

        // Defined functions — one func node per code entry; body bytes on ext.text
        // as a raw byte-string (ssa_build re-walks with byte_cursor + opcode_map).
        const std::uint32_t defined = static_cast<std::uint32_t>(raw.code.size());
        for (std::uint32_t i = 0; i < defined; ++i) {
            const auto& code = raw.code[i];
            taranga_node_ext ext;
            ext.immediate = (i < raw.functions.size()) ? raw.functions[i] : 0u; // typeidx
            ext.has_value = true;
            std::vector<lang::ir_node_id> func_children;
            // Locals as explicit local nodes (type-only; index is positional).
            for (auto lt : code.locals) {
                taranga_node_ext le;
                le.vtype = lt;
                le.has_vtype = true;
                le.text = std::string(spelling(lt));
                func_children.push_back(detail::push_node(ir, taranga_kind::local, std::move(le)));
            }
            // Body node carries the raw instruction bytes.
            taranga_node_ext be;
            be.text.assign(reinterpret_cast<const char*>(code.body.data()), code.body.size());
            const auto body_id = detail::push_node(ir, taranga_kind::body, std::move(be), code.span);
            func_children.push_back(body_id);
            const auto fid = detail::push_node(ir, taranga_kind::func, std::move(ext), code.span);
            detail::attach(ir, fid, func_children);
            module_children.push_back(fid);
        }

        // Tables / memories / globals — structural only (init exprs deferred).
        for (const auto& t : raw.tables) {
            taranga_node_ext ext;
            ext.vtype = t.element;
            ext.has_vtype = true;
            ext.immediate = t.count_limits.min;
            ext.immediate2 = t.count_limits.max.value_or(0);
            module_children.push_back(detail::push_node(ir, taranga_kind::table, std::move(ext)));
        }
        for (const auto& m : raw.memories) {
            taranga_node_ext ext;
            ext.immediate = m.page_limits.min;
            ext.immediate2 = m.page_limits.max.value_or(0);
            module_children.push_back(detail::push_node(ir, taranga_kind::memory, std::move(ext)));
        }
        for (const auto& g : raw.globals) {
            taranga_node_ext ext;
            ext.vtype = g.type.value;
            ext.has_vtype = true;
            ext.immediate = g.type.mutable_ ? 1u : 0u;
            ext.text.assign(reinterpret_cast<const char*>(g.init_expr.data()), g.init_expr.size());
            module_children.push_back(detail::push_node(ir, taranga_kind::global, std::move(ext), g.span));
        }

        // Exports.
        for (const auto& ex : raw.exports) {
            taranga_node_ext ext;
            ext.head = std::string(spelling(ex.kind));
            ext.text = ex.name;
            ext.immediate = ex.index;
            module_children.push_back(
                detail::push_node(ir, taranga_kind::export_, std::move(ext), ex.span));
        }

        if (raw.start_function) {
            taranga_node_ext ext;
            ext.immediate = *raw.start_function;
            module_children.push_back(detail::push_node(ir, taranga_kind::start, std::move(ext)));
        }

        const auto module_id = detail::push_node(ir, taranga_kind::module_, {});
        detail::attach(ir, module_id, module_children);
        ir.set_root(module_id);
        return out;
    }

    // =========================================================================
    // WAT path — parse_tree → taranga_ir_module.
    //
    // Two-stage: (1) fold the lexy parse_tree into a lightweight s-expression DOM
    // (list-of-atoms/lists with byte spans), then (2) interpret that DOM by head
    // keyword into taranga nodes. Stage 1 is trivial and mechanical; stage 2 holds
    // the WAT semantics and produces exactly the same node kinds as the binary
    // path for the same construct.
    // =========================================================================

    namespace detail {

        // A folded s-expression: either an atom (leaf text) or a list of sexprs.
        struct sexpr_dom {
            bool is_list = false;
            std::string atom;              // valid iff !is_list
            std::vector<sexpr_dom> items;  // valid iff is_list
            byte_span span{};

            [[nodiscard]] std::string_view head() const noexcept {
                if (is_list && !items.empty() && !items.front().is_list)
                    return items.front().atom;
                return {};
            }
        };

        // Fold a lexy parse_tree into a sexpr_dom forest via a traverse walk.
        // The grammar nests `sexpr` lists and emits `atom`/`string_lit` leaves;
        // we rebuild the paren structure by tracking a stack of open lists.
        template <typename ParseTree>
        [[nodiscard]] inline std::vector<sexpr_dom>
        fold_tree(const ParseTree& tree) {
            std::vector<sexpr_dom> roots;
            std::vector<sexpr_dom> stack; // open lists
            auto commit = [&](sexpr_dom node) {
                if (stack.empty()) roots.push_back(std::move(node));
                else stack.back().items.push_back(std::move(node));
            };

            for (auto [ev, node] : tree.traverse()) {
                using event = lexy::traverse_event;
                std::string_view kname = node.kind().name();
                if (auto pos = kname.rfind("::"); pos != std::string_view::npos)
                    kname.remove_prefix(pos + 2);

                if (ev == event::enter) {
                    if (kname == "sexpr") {
                        sexpr_dom lst;
                        lst.is_list = true;
                        stack.push_back(std::move(lst));
                    }
                } else if (ev == event::leaf) {
                    auto lex = node.lexeme();
                    std::string_view text(reinterpret_cast<const char*>(lex.begin()),
                                          static_cast<std::size_t>(lex.end() - lex.begin()));
                    // Skip pure structural punctuation ( ) and trivia.
                    if (text == "(" || text == ")" || text.empty()) continue;
                    // Whitespace-only leaf → skip.
                    bool ws = true;
                    for (char c : text) if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { ws = false; break; }
                    if (ws) continue;
                    sexpr_dom atom;
                    atom.is_list = false;
                    atom.atom = std::string(text);
                    commit(std::move(atom));
                } else { // exit
                    if (kname == "sexpr" && !stack.empty()) {
                        auto done = std::move(stack.back());
                        stack.pop_back();
                        commit(std::move(done));
                    }
                }
            }
            return roots;
        }

        // Strip a leading '$' from an identifier atom, or surrounding quotes.
        [[nodiscard]] inline std::string clean_name(std::string_view s) {
            if (!s.empty() && s.front() == '$') s.remove_prefix(1);
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                s.remove_prefix(1);
                s.remove_suffix(1);
            }
            return std::string(s);
        }

        // Instruction-keyword classification into a taranga_kind.
        [[nodiscard]] inline taranga_kind classify_instr(std::string_view head) noexcept {
            if (head.ends_with(".const")) return taranga_kind::const_;
            if (head.starts_with("local.")) return taranga_kind::local_access;
            if (head.starts_with("global.")) return taranga_kind::global_access;
            if (head.ends_with(".load") || head.ends_with(".store") ||
                head.find(".load") != std::string_view::npos ||
                head.find(".store") != std::string_view::npos)
                return taranga_kind::mem_op;
            if (head == "block") return taranga_kind::block;
            if (head == "loop") return taranga_kind::loop;
            if (head == "if") return taranga_kind::if_;
            if (head == "br" || head == "br_if" || head == "br_table") return taranga_kind::br;
            if (head == "call" || head == "call_indirect") return taranga_kind::call;
            if (head == "select") return taranga_kind::select;
            if (head == "unreachable") return taranga_kind::unreachable;
            return taranga_kind::instr;
        }

    } // namespace detail

    // Forward decl for the recursive WAT interpreter.
    namespace detail {
        lang::ir_node_id interpret_wat(taranga_ir_module& ir, const sexpr_dom& node,
                                       taranga_module& out);

        // Interpret a function-type signature body: (param …)/(result …) items.
        // Populates a func_type and pushes param/result nodes; returns them.
        inline void interpret_sig(taranga_ir_module& ir, const sexpr_dom& list,
                                  func_type& sig, std::vector<lang::ir_node_id>& kids) {
            for (const auto& item : list.items) {
                if (!item.is_list) continue;
                auto h = item.head();
                if (h == "param" || h == "result") {
                    for (const auto& t : item.items) {
                        if (t.is_list || t.atom == std::string(h)) continue;
                        auto vt = value_type_from_spelling(t.atom);
                        if (!vt) continue;
                        if (h == "param") sig.params.push_back(*vt);
                        else sig.results.push_back(*vt);
                        taranga_node_ext ext;
                        ext.vtype = *vt;
                        ext.has_vtype = true;
                        ext.text = t.atom;
                        kids.push_back(push_node(ir, h == "param" ? taranga_kind::param
                                                                  : taranga_kind::result,
                                                 std::move(ext), item.span));
                    }
                }
            }
        }

        // Interpret a single instruction s-expression (folded form) recursively:
        // (i32.add (i32.const 1) (local.get 0)) → an instr node with two children.
        inline lang::ir_node_id interpret_instr(taranga_ir_module& ir, const sexpr_dom& node,
                                               taranga_module& out) {
            const auto head = node.head();
            const auto kind = classify_instr(head);
            taranga_node_ext ext;
            ext.head = std::string(head);

            // Collect operand children + fold immediates.
            std::vector<lang::ir_node_id> kids;
            bool first = true;
            for (const auto& item : node.items) {
                if (first) { first = false; continue; } // skip head atom
                if (item.is_list) {
                    kids.push_back(interpret_instr(ir, item, out));
                } else {
                    // Immediate atom: numeric const, index, or type keyword.
                    if (kind == taranga_kind::const_) {
                        if (auto v = parse_int_literal(item.atom)) {
                            ext.value = *v;
                            ext.has_value = true;
                        } else {
                            ext.text = item.atom; // float literal — keep textual
                            ext.has_value = false;
                        }
                        // value_type from the head prefix (i32.const → i32).
                        auto dot = head.find('.');
                        if (dot != std::string_view::npos) {
                            if (auto vt = value_type_from_spelling(head.substr(0, dot))) {
                                ext.vtype = *vt;
                                ext.has_vtype = true;
                            }
                        }
                    } else if (auto idx = parse_int_literal(item.atom)) {
                        ext.immediate = static_cast<std::uint32_t>(*idx);
                    } else {
                        // named target ($id) — store cleaned text for later resolve.
                        if (ext.text.empty()) ext.text = clean_name(item.atom);
                    }
                }
            }
            const auto id = push_node(ir, kind, std::move(ext), node.span);
            attach(ir, id, kids);
            return id;
        }

        // Interpret one top-level or nested module field.
        inline lang::ir_node_id interpret_wat(taranga_ir_module& ir, const sexpr_dom& node,
                                              taranga_module& out) {
            if (!node.is_list) {
                taranga_node_ext ext;
                ext.text = clean_name(node.atom);
                return push_node(ir, taranga_kind::name, std::move(ext), node.span);
            }
            const auto head = node.head();

            if (head == "module") {
                std::vector<lang::ir_node_id> kids;
                bool first = true;
                for (const auto& item : node.items) {
                    if (first) { first = false; continue; }
                    if (item.is_list) kids.push_back(interpret_wat(ir, item, out));
                }
                const auto id = push_node(ir, taranga_kind::module_, {}, node.span);
                attach(ir, id, kids);
                return id;
            }
            if (head == "type") {
                func_type sig;
                std::vector<lang::ir_node_id> kids;
                for (const auto& item : node.items) {
                    if (item.is_list && item.head() == "func")
                        interpret_sig(ir, item, sig, kids);
                }
                out.types.push_back(std::move(sig));
                taranga_node_ext ext;
                const auto id = push_node(ir, taranga_kind::type, std::move(ext), node.span);
                attach(ir, id, kids);
                return id;
            }
            if (head == "func") {
                taranga_node_ext fext;
                func_type sig;
                std::vector<lang::ir_node_id> kids;
                std::vector<lang::ir_node_id> body_instrs;
                std::optional<std::uint32_t> explicit_type_idx;
                bool first = true;
                for (const auto& item : node.items) {
                    if (first) { first = false; continue; }
                    if (!item.is_list) {
                        // $name or export shorthand handled by cleaning.
                        if (fext.text.empty() && !item.atom.empty() && item.atom.front() == '$')
                            fext.text = clean_name(item.atom);
                        continue;
                    }
                    auto h = item.head();
                    if (h == "type") {
                        // (type N) — explicit type reference; record the index.
                        for (const auto& t : item.items) {
                            if (!t.is_list && t.atom != "type") {
                                if (auto idx = parse_int_literal(t.atom)) {
                                    explicit_type_idx = static_cast<std::uint32_t>(*idx);
                                    fext.immediate = *explicit_type_idx;
                                }
                            }
                        }
                    } else if (h == "param" || h == "result") {
                        interpret_sig(ir, node, sig, kids); // fill sig once
                        // (interpret_sig scans all param/result in node — dedup below)
                    } else if (h == "local") {
                        for (const auto& t : item.items) {
                            if (t.is_list || t.atom == "local") continue;
                            auto vt = value_type_from_spelling(t.atom);
                            if (!vt) continue;
                            taranga_node_ext le;
                            le.vtype = *vt;
                            le.has_vtype = true;
                            le.text = t.atom;
                            kids.push_back(push_node(ir, taranga_kind::local, std::move(le), item.span));
                        }
                    } else {
                        body_instrs.push_back(interpret_instr(ir, item, out));
                    }
                }
                // Second pass: collect flat (non-folded) body instructions.
                // Flat WAT encodes instructions as bare atoms ("local.get", "0") rather
                // than s-expression lists. We group consecutive atoms that form an
                // instruction keyword + immediates into synthetic sexpr_dom lists.
                {
                    auto is_instr_keyword = [](std::string_view a) -> bool {
                        if (a.find('.') != std::string_view::npos) return true;
                        static constexpr std::string_view plain[] = {
                            "return", "unreachable", "nop", "drop", "select",
                        };
                        for (auto kw : plain) if (a == kw) return true;
                        return false;
                    };

                    sexpr_dom pending;
                    bool skip_first = true;
                    for (const auto& item : node.items) {
                        if (skip_first) { skip_first = false; continue; }
                        if (item.is_list) {
                            // Flush any in-progress flat instruction.
                            if (!pending.items.empty()) {
                                body_instrs.push_back(interpret_instr(ir, pending, out));
                                pending = sexpr_dom{};
                            }
                            continue; // list-based instrs already handled in first pass
                        }
                        const std::string_view a = item.atom;
                        // Skip declaration-level atoms (already handled above or irrelevant).
                        if (a.empty() || a == "func" || a == "type" ||
                            a == "param" || a == "result" || a == "local" ||
                            (!a.empty() && a.front() == '$'))
                            continue;
                        if (is_instr_keyword(a)) {
                            if (!pending.items.empty()) {
                                body_instrs.push_back(interpret_instr(ir, pending, out));
                                pending = sexpr_dom{};
                            }
                            pending.is_list = true;
                            pending.span = item.span;
                            sexpr_dom head_node;
                            head_node.is_list = false;
                            head_node.atom = item.atom;
                            head_node.span = item.span;
                            pending.items.push_back(std::move(head_node));
                        } else if (!pending.items.empty()) {
                            // Immediate atom for the current instruction.
                            pending.items.push_back(item);
                        }
                    }
                    if (!pending.items.empty())
                        body_instrs.push_back(interpret_instr(ir, pending, out));
                }
                // If an explicit (type N) reference was given, use that type index
                // directly. Otherwise derive the signature from inline param/result.
                if (explicit_type_idx.has_value()) {
                    fext.immediate = *explicit_type_idx;
                    out.func_types.push_back(*explicit_type_idx);
                } else {
                    // Rebuild sig cleanly (interpret_sig above may double-count); recompute.
                    sig = func_type{};
                    std::vector<lang::ir_node_id> sig_kids_unused;
                    interpret_sig(ir, node, sig, sig_kids_unused);
                    out.types.push_back(sig);
                    fext.immediate = static_cast<std::uint32_t>(out.types.size() - 1);
                    out.func_types.push_back(fext.immediate);
                }

                if (!body_instrs.empty()) {
                    const auto body_id = push_node(ir, taranga_kind::body, {}, node.span);
                    attach(ir, body_id, body_instrs);
                    kids.push_back(body_id);
                }
                const auto id = push_node(ir, taranga_kind::func, std::move(fext), node.span);
                attach(ir, id, kids);
                return id;
            }
            if (head == "export") {
                taranga_node_ext ext;
                for (const auto& item : node.items) {
                    if (!item.is_list && item.atom != "export") {
                        if (ext.text.empty()) ext.text = clean_name(item.atom);
                    } else if (item.is_list) {
                        ext.head = std::string(item.head()); // func/table/memory/global
                    }
                }
                return push_node(ir, taranga_kind::export_, std::move(ext), node.span);
            }
            if (head == "import") {
                taranga_node_ext ext;
                std::vector<std::string> names;
                for (const auto& item : node.items) {
                    if (!item.is_list && item.atom != "import")
                        names.push_back(clean_name(item.atom));
                    else if (item.is_list)
                        ext.head = std::string(item.head());
                }
                if (names.size() >= 2) ext.text = names[0] + "." + names[1];
                return push_node(ir, taranga_kind::import, std::move(ext), node.span);
            }
            if (head == "memory") {
                taranga_node_ext ext;
                bool first = true;
                std::uint32_t which = 0;
                for (const auto& item : node.items) {
                    if (first) { first = false; continue; }
                    if (!item.is_list) {
                        if (auto v = parse_int_literal(item.atom)) {
                            if (which == 0) { ext.immediate = static_cast<std::uint32_t>(*v); ++which; }
                            else ext.immediate2 = static_cast<std::uint32_t>(*v);
                        }
                    }
                }
                return push_node(ir, taranga_kind::memory, std::move(ext), node.span);
            }
            if (head == "global") {
                taranga_node_ext ext;
                std::vector<lang::ir_node_id> kids;
                for (const auto& item : node.items) {
                    if (!item.is_list) {
                        if (auto vt = value_type_from_spelling(item.atom)) {
                            ext.vtype = *vt; ext.has_vtype = true;
                        }
                    } else if (item.head() == "mut") {
                        ext.immediate = 1u;
                        for (const auto& t : item.items)
                            if (!t.is_list) if (auto vt = value_type_from_spelling(t.atom)) {
                                ext.vtype = *vt; ext.has_vtype = true;
                            }
                    } else {
                        kids.push_back(interpret_instr(ir, item, out)); // init expr
                    }
                }
                const auto id = push_node(ir, taranga_kind::global, std::move(ext), node.span);
                attach(ir, id, kids);
                return id;
            }
            if (head == "start") {
                taranga_node_ext ext;
                for (const auto& item : node.items)
                    if (!item.is_list && item.atom != "start")
                        if (auto v = parse_int_literal(item.atom)) {
                            ext.immediate = static_cast<std::uint32_t>(*v);
                            out.start_function = ext.immediate;
                        }
                return push_node(ir, taranga_kind::start, std::move(ext), node.span);
            }
            if (head == "table") {
                taranga_node_ext ext;
                for (const auto& item : node.items) {
                    if (!item.is_list) {
                        if (auto vt = value_type_from_spelling(item.atom)) { ext.vtype = *vt; ext.has_vtype = true; }
                        else if (auto v = parse_int_literal(item.atom)) ext.immediate = static_cast<std::uint32_t>(*v);
                    }
                }
                return push_node(ir, taranga_kind::table, std::move(ext), node.span);
            }
            // Unknown top-level field → treat as an instruction (defensive).
            return interpret_instr(ir, node, out);
        }

    } // namespace detail

    template <typename ParseTree>
    [[nodiscard]] inline taranga_module
    build_from_wat(const ParseTree& tree, std::string_view /*src*/) {
        taranga_module out;
        if (tree.empty()) {
            out.diagnostics.on_diagnostic(
                make_error("TARANGA-PARSE-001", "WAT parse_tree is empty"));
            return out;
        }
        auto roots = detail::fold_tree(tree);
        if (roots.empty()) {
            out.diagnostics.on_diagnostic(
                make_error("TARANGA-PARSE-002", "no top-level s-expression"));
            return out;
        }
        // The first root is the module (or a bare field wrapped for test fragments).
        const auto& top = roots.front();
        const auto root_id = detail::interpret_wat(out.ir, top, out);
        out.ir.set_root(root_id);
        return out;
    }

} // namespace taranga
