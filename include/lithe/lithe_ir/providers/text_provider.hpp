#pragma once

// =============================================================================
// lithe_ir/providers/text_provider.hpp — canonical text Lithe IR provider
//
//
// Satisfies:
//   text_importer_for<text_provider, lithe_text_ir_doc>
//   text_exporter_for<text_provider, lithe_text_ir_doc>
//   ir_validator_for<text_provider, lithe_text_ir_doc>
//
// Two output modes:
//   canonical — byte-stable, deterministic, used for round-trip and hashing.
//               This is the DEFAULT mode.  The canonical form is always emitted
//               by export_text unless text_print_options::pretty_print = true.
//   pretty    — opt-in human-friendly indented form.  NEVER fed to the hasher.
//
// Parser features:
//   • Versioned header:  "lithe-ir 1.0 / <stage> / <dialect> / <target>"
//   • Stable op names, explicit semantic types, explicit block/value ids
//   • Quoted/escaped string literals
//   • Optional source locations (file:line:col)
//   • Unknown-op policy: optional→opaque-optional, required→unresolved
//   • Parser resource limits (max_nodes, max_nesting, max_text_bytes)
//   • Line/column diagnostics into diagnostic_list
//
// This provider supersedes diagnostic_text_stub as the canonical export_text path.
// diagnostic_text_stub remains only as the neutral fallback when this provider
// is absent.
//
// The IR object type is lithe_text_ir_doc (defined here).  It is a generic
// stable document that stage adapters can convert to/from their concrete forms.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <algorithm>
#include <any>
#include <cstdint>
#include <expected>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../lithe_execution/foundation.hpp"   // ir_error
#include "../format.hpp"                          // format_descriptor, stage, schema_version, etc.
#include "../provider.hpp"                        // ir_resolution_state, cpo tags, no_ir_provider
#include "../integration.hpp"                     // diagnostic_entry, diagnostic_list

namespace lithe::ir {
    // =========================================================================
    //a.8 lithe_text_ir_doc — the canonical text IR object type
    //
    // A structured document that carries the parsed representation of a
    // text-format Lithe IR file.  It is generic (stage-independent) at this
    // level; stage adapters convert to their concrete forms.
    // =========================================================================

    struct text_ir_section {
        std::string name; // section kind e.g. "lithe.ir.graph.nodes"
        std::vector<std::string> lines; // canonical one-per-record lines
        bool required = true;
    };

    // text_ir_op_record — one operation record in the text IR
    struct text_ir_op_record {
        std::string domain; // stable domain id
        std::string name; // stable op name
        std::string schema_str; // "major.minor.patch"
        bool is_required_op = true; // unknown+required = unresolved
        std::vector<std::string> operands; // serialised operand tokens
        std::vector<std::string> results; // result id tokens
        std::optional<std::string> source_loc; // "file:line:col"
    };

    // text_ir_block_record — one block in the text IR
    struct text_ir_block_record {
        std::string id;
        std::vector<std::string> arg_ids;
        std::vector<std::size_t> op_indices; // indices into ops table
        std::vector<std::string> succ_ids; // successor block ids
    };

    // lithe_text_ir_doc — complete parsed text IR document
    struct lithe_text_ir_doc {
        // Header fields
        std::string ir_format_version; // e.g. "1.0"
        std::string stage_str; // e.g. "physical"
        std::string dialect_str; // optional dialect id
        std::string target_str; // e.g. "x86_64" or "aarch64"

        // Content
        std::vector<text_ir_op_record> ops;
        std::vector<text_ir_block_record> blocks;
        std::vector<std::string> value_ids;
        std::vector<std::string> string_table;

        // Opaque-optional sections (preserved round-trip)
        std::vector<text_ir_section> opaque_sections;

        // Resolution bookkeeping
        bool has_unknown_required = false;
        bool has_unknown_optional = false;

        // Format descriptor for the document
        format_descriptor doc_format{};

        [[nodiscard]] bool valid() const noexcept {
            return doc_format.valid() && !ir_format_version.empty();
        }
    };

    // =========================================================================
    // text_print_options — controls export behaviour
    // =========================================================================

    struct text_print_options {
        bool pretty_print = false; // indent & space for readability
        bool emit_source_loc = true; // include source locations if present
        bool emit_comments = false; // enable only for debugging
        std::uint32_t indent_width = 2;
    };

    // text_parser_limits — resource bounds enforced before any allocation
    struct text_parser_limits {
        std::uint64_t max_text_bytes = 64ULL * 1024 * 1024; // 64 MiB
        std::uint32_t max_ops = 1'000'000;
        std::uint32_t max_blocks = 100'000;
        std::uint32_t max_values = 1'000'000;
        std::uint32_t max_nesting = 128;
        std::uint32_t max_string_len = 65'536;
    };

    // =========================================================================
    // text_provider — canonical text Lithe IR provider
    // =========================================================================

    class text_provider {
    public:
        static constexpr bool available = true;
        static constexpr std::string_view id = "lithe.ir.text";

        explicit text_provider(text_parser_limits limits = {},
                               text_print_options defaults = {})
            : limits_(limits), default_opts_(defaults) {}

        // ====================================================================
        // import_text — parse a text_ir_view into a lithe_text_ir_doc
        // ====================================================================

        [[nodiscard]] std::expected<lithe_text_ir_doc, ::lithe::execution::ir_error>
        do_import_text(const text_ir_view& view,
                       diagnostic_list& diags) const {
            if (!view.valid())
                return std::unexpected(::lithe::execution::ir_error{
                    "text_provider: invalid text_ir_view"
                });

            const std::string_view text = view.as_string_view();

            if (text.size() > limits_.max_text_bytes)
                return std::unexpected(::lithe::execution::ir_error{
                    "text_provider: input exceeds max_text_bytes limit"
                });

            return parse_document_(text, view.format, diags);
        }

        // ====================================================================
        // export_text — serialise a lithe_text_ir_doc to owned_text_ir
        //
        // Canonical mode (default): byte-stable, deterministic.
        // Pretty-print: opt-in, indented — never fed to the hasher.
        // ====================================================================

        [[nodiscard]] std::expected<owned_text_ir, ::lithe::execution::ir_error>
        do_export_text(const lithe_text_ir_doc& doc,
                       const format_descriptor& fmt,
                       const text_print_options& opts = {}) const {
            if (!fmt.valid())
                return std::unexpected(::lithe::execution::ir_error{
                    "text_provider: invalid format_descriptor for export"
                });
            if (!doc.valid())
                return std::unexpected(::lithe::execution::ir_error{
                    "text_provider: invalid doc for export"
                });

            owned_text_ir out;
            out.format = fmt;
            const std::string text = opts.pretty_print
                                         ? emit_pretty_(doc, opts)
                                         : emit_canonical_(doc);
            out.data.assign(text.begin(), text.end());
            return out;
        }

        // ====================================================================
        // validate_ir — classify the doc's resolution state
        // Gate fires AFTER validate, not before (speca.4).
        // ====================================================================

        [[nodiscard]] ir_resolution_state
        do_validate(const lithe_text_ir_doc& doc) const noexcept {
            if (doc.has_unknown_required)
                return ir_resolution_state::unresolved_required_operations;
            if (doc.has_unknown_optional)
                return ir_resolution_state::contains_opaque_optional_operations;
            return ir_resolution_state::resolved;
        }

        // ====================================================================
        // CPO tag_invoke friends (ADL, satisfies the typed concepts)
        // ====================================================================

        friend std::expected<lithe_text_ir_doc, ::lithe::execution::ir_error>
        tag_invoke(cpo::import_text_t, text_provider& self, text_ir_view view) {
            diagnostic_list diags;
            return self.do_import_text(view, diags);
        }

        friend std::expected<lithe_text_ir_doc, ::lithe::execution::ir_error>
        tag_invoke(cpo::import_text_t, const text_provider& self, text_ir_view view) {
            diagnostic_list diags;
            return self.do_import_text(view, diags);
        }

        friend std::expected<owned_text_ir, ::lithe::execution::ir_error>
        tag_invoke(cpo::export_text_t, const text_provider& self,
                   const lithe_text_ir_doc& doc, format_descriptor fmt) {
            return self.do_export_text(doc, fmt, self.default_opts_);
        }

        friend ir_resolution_state
        tag_invoke(cpo::validate_ir_t, const text_provider& self,
                   const lithe_text_ir_doc& doc) noexcept {
            return self.do_validate(doc);
        }

        // ====================================================================
        // validate_erased — type-erased validation for ir_provider_registry
        // Satisfies erased_validator concept.
        // ====================================================================

        [[nodiscard]] ir_resolution_state
        validate_erased(const std::any& ir) const noexcept {
            const auto* doc = std::any_cast<lithe_text_ir_doc>(&ir);
            if (!doc) return ir_resolution_state::unresolved_required_operations;
            return do_validate(*doc);
        }

        // ====================================================================
        // import_with_diagnostics — extended import that returns diagnostics
        // (not a CPO; called directly by tests and tooling)
        // ====================================================================

        [[nodiscard]] std::expected<lithe_text_ir_doc, ::lithe::execution::ir_error>
        import_with_diagnostics(const text_ir_view& view,
                                diagnostic_list& diags) const {
            return do_import_text(view, diags);
        }

        // Export with explicit options (not a CPO)
        [[nodiscard]] std::expected<owned_text_ir, ::lithe::execution::ir_error>
        export_with_options(const lithe_text_ir_doc& doc,
                            const format_descriptor& fmt,
                            const text_print_options& opts) const {
            return do_export_text(doc, fmt, opts);
        }

    private:
        text_parser_limits limits_;
        text_print_options default_opts_;

        // ====================================================================
        // Parser implementation
        // ====================================================================

        // Thin tokeniser: returns tokens on one line, stripping comments.
        static std::vector<std::string_view> tokenise_line_(std::string_view line) {
            std::vector<std::string_view> toks;
            std::size_t i = 0;
            const std::size_t n = line.size();
            while (i < n) {
                // skip whitespace
                while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
                if (i >= n) break;
                // comment
                if (i + 1 < n && line[i] == '/' && line[i + 1] == '/') break;
                // quoted string
                if (line[i] == '"') {
                    const std::size_t start = i++;
                    while (i < n && !(line[i] == '"' && line[i - 1] != '\\')) ++i;
                    if (i < n) ++i; // consume closing "
                    toks.push_back(line.substr(start, i - start));
                }
                else if (line[i] == '{' || line[i] == '}') {
                    // Structural braces are standalone tokens (region nesting).
                    toks.push_back(line.substr(i, 1));
                    ++i;
                }
                else if (line[i] == '(' || line[i] == ')' || line[i] == ',') {
                    // Pretty-print punctuation carries no canonical meaning: drop it.
                    ++i;
                }
                else {
                    const std::size_t start = i;
                    while (i < n && line[i] != ' ' && line[i] != '\t' &&
                        line[i] != ',' && line[i] != '(' && line[i] != ')' &&
                        line[i] != '{' && line[i] != '}')
                        ++i;
                    if (i > start) toks.push_back(line.substr(start, i - start));
                }
            }
            return toks;
        }

        // Split text into lines
        static std::vector<std::string_view> split_lines_(std::string_view text) {
            std::vector<std::string_view> lines;
            std::size_t pos = 0;
            while (pos < text.size()) {
                const std::size_t nl = text.find('\n', pos);
                const std::size_t end = (nl == std::string_view::npos) ? text.size() : nl;
                lines.push_back(text.substr(pos, end - pos));
                pos = end + 1;
            }
            return lines;
        }

        // Emit a diagnostic line/col error
        static void emit_diag_(diagnostic_list& diags,
                               diagnostic_level lvl,
                               std::uint32_t line_no,
                               std::uint32_t col_no,
                               std::string msg) {
            diagnostic_entry e;
            e.level = lvl;
            e.message = std::to_string(line_no) + ":" + std::to_string(col_no)
                + ": " + std::move(msg);
            diags.push_back(std::move(e));
        }

        // Main parse entry
        [[nodiscard]] std::expected<lithe_text_ir_doc, ::lithe::execution::ir_error>
        parse_document_(std::string_view text,
                        const format_descriptor& fmt,
                        diagnostic_list& diags) const {
            lithe_text_ir_doc doc;
            doc.doc_format = fmt;

            const auto lines = split_lines_(text);
            if (lines.empty())
                return std::unexpected(::lithe::execution::ir_error{
                    "text_provider: empty document"
                });

            std::uint32_t line_no = 0;
            std::uint32_t op_count = 0;
            std::uint32_t blk_count = 0;
            std::uint32_t nesting = 0;
            bool header_parsed = false;

            for (const auto& raw_line : lines) {
                ++line_no;

                // Strip trailing CR
                std::string_view line = raw_line;
                if (!line.empty() && line.back() == '\r')
                    line = line.substr(0, line.size() - 1);

                // Skip blank and comment lines
                const auto stripped = [](std::string_view sv) {
                    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                        sv.remove_prefix(1);
                    return sv;
                }(line);
                if (stripped.empty() || (stripped.size() > 1 &&
                    stripped[0] == '/' && stripped[1] == '/'))
                    continue;

                // Parse header line: "lithe-ir <ver> / <stage> / <dialect> / <target>"
                if (!header_parsed) {
                    if (!parse_header_line_(stripped, doc, diags, line_no))
                        return std::unexpected(::lithe::execution::ir_error{
                            "text_provider: malformed header"
                        });
                    header_parsed = true;
                    continue;
                }

                // Resource limit guards
                if (op_count >= limits_.max_ops) {
                    emit_diag_(diags, diagnostic_level::error_, line_no, 0,
                               "op count exceeds max_ops limit");
                    return std::unexpected(::lithe::execution::ir_error{
                        "text_provider: op count exceeds limit"
                    });
                }
                if (blk_count >= limits_.max_blocks) {
                    emit_diag_(diags, diagnostic_level::error_, line_no, 0,
                               "block count exceeds max_blocks limit");
                    return std::unexpected(::lithe::execution::ir_error{
                        "text_provider: block count exceeds limit"
                    });
                }
                if (nesting >= limits_.max_nesting) {
                    emit_diag_(diags, diagnostic_level::error_, line_no, 0,
                               "nesting depth exceeds max_nesting limit");
                    return std::unexpected(::lithe::execution::ir_error{
                        "text_provider: nesting exceeds limit"
                    });
                }

                const auto toks = tokenise_line_(stripped);
                if (toks.empty()) continue;

                // Detect block/region openers and closers for nesting tracking
                if (toks[0] == "{") {
                    ++nesting;
                    continue;
                }
                if (toks[0] == "}") {
                    if (nesting > 0) --nesting;
                    continue;
                }

                // "block" keyword → block record
                if (toks[0] == "block") {
                    if (!parse_block_line_(toks, doc, diags, line_no))
                        continue;
                    ++blk_count;
                    continue;
                }

                // "section" keyword → opaque section header (future-compat)
                if (toks[0] == "section") {
                    parse_section_header_(toks, doc, diags, line_no);
                    continue;
                }

                // Otherwise: try to parse as an op record
                // Format: [<results...> =] <domain>.<op> [<schema>] [<operands...>] [@ <sourceloc>]
                if (toks.size() >= 1) {
                    auto op_r = parse_op_line_(toks, doc, diags, line_no);
                    if (op_r) {
                        if (op_r->is_required_op == false)
                            doc.has_unknown_optional = true;
                        doc.ops.push_back(std::move(*op_r));
                        ++op_count;
                    }
                }
            }

            if (!header_parsed) {
                emit_diag_(diags, diagnostic_level::error_, 0, 0, "missing header line");
                return std::unexpected(::lithe::execution::ir_error{
                    "text_provider: missing header"
                });
            }

            return doc;
        }

        // Parse header line: "lithe-ir 1.0 / physical / lithe / x86_64"
        static bool parse_header_line_(std::string_view line,
                                       lithe_text_ir_doc& doc,
                                       diagnostic_list& diags,
                                       std::uint32_t line_no) {
            if (line.substr(0, 9) != "lithe-ir ") {
                emit_diag_(diags, diagnostic_level::error_, line_no, 0,
                           "expected 'lithe-ir <version>' header");
                return false;
            }
            line.remove_prefix(9);
            // split by ' / '
            auto next_sep = [&](std::string_view sv) -> std::pair<std::string, std::string_view> {
                const std::size_t pos = sv.find(" / ");
                if (pos == std::string_view::npos)
                    return {std::string{sv}, {}};
                return {std::string{sv.substr(0, pos)}, sv.substr(pos + 3)};
            };
            auto [ver, rest1] = next_sep(line);
            auto [stg, rest2] = next_sep(rest1);
            auto [dlct, rest3] = next_sep(rest2);
            std::string tgt{rest3};

            doc.ir_format_version = std::move(ver);
            doc.stage_str = std::move(stg);
            doc.dialect_str = std::move(dlct);
            doc.target_str = std::move(tgt);
            return true;
        }

        // Parse block record line: "block <id> [(<args...>)] [: <succs...>]"
        static bool parse_block_line_(const std::vector<std::string_view>& toks,
                                      lithe_text_ir_doc& doc,
                                      diagnostic_list& diags,
                                      std::uint32_t line_no) {
            if (toks.size() < 2) {
                emit_diag_(diags, diagnostic_level::error_, line_no, 0,
                           "block: expected id");
                return false;
            }
            text_ir_block_record blk;
            blk.id = std::string{toks[1]};
            // Optional args and succs — simplified: collect remaining tokens
            bool in_succs = false;
            for (std::size_t i = 2; i < toks.size(); ++i) {
                const auto t = toks[i];
                if (t == "{" || t == "}") continue; // region braces: not block args
                if (t == ":") {
                    in_succs = true;
                    continue;
                }
                if (in_succs) blk.succ_ids.push_back(std::string{t});
                else blk.arg_ids.push_back(std::string{t});
            }
            doc.blocks.push_back(std::move(blk));
            return true;
        }

        // Parse section header: "section <name> [required|optional]"
        static void parse_section_header_(const std::vector<std::string_view>& toks,
                                          lithe_text_ir_doc& doc,
                                          diagnostic_list& diags,
                                          std::uint32_t line_no) {
            if (toks.size() < 2) return;
            text_ir_section sec;
            sec.name = std::string{toks[1]};
            sec.required = (toks.size() < 3 || toks[2] != "optional");
            // Unknown sections may be opaque-optional
            if (!sec.required) doc.has_unknown_optional = true;
            doc.opaque_sections.push_back(std::move(sec));
        }

        // Parse op record: "[<results> =] <domain>.<op> [<schema>] [<operands>] [@ <loc>]"
        // Returns std::nullopt on hard error; pushes diagnostics for warnings.
        static std::optional<text_ir_op_record>
        parse_op_line_(const std::vector<std::string_view>& toks,
                       lithe_text_ir_doc& doc,
                       diagnostic_list& diags,
                       std::uint32_t line_no) {
            text_ir_op_record op;
            std::size_t i = 0;

            // results = ...
            std::vector<std::string> results;
            // Look ahead: if token after first = is '=', collect result ids
            if (toks.size() > 2) {
                std::size_t j = 0;
                while (j < toks.size() && toks[j] != "=") ++j;
                if (j < toks.size() && toks[j] == "=") {
                    for (std::size_t k = 0; k < j; ++k)
                        results.push_back(std::string{toks[k]});
                    i = j + 1;
                }
            }
            op.results = std::move(results);

            if (i >= toks.size()) {
                emit_diag_(diags, diagnostic_level::warning, line_no, 0,
                           "op line: no op name found");
                return std::nullopt;
            }

            // op name: "domain.op" or "op"
            const std::string_view op_tok = toks[i++];
            const std::size_t dot = op_tok.rfind('.');
            if (dot != std::string_view::npos) {
                op.domain = std::string{op_tok.substr(0, dot)};
                op.name = std::string{op_tok.substr(dot + 1)};
            }
            else {
                op.domain = "lithe";
                op.name = std::string{op_tok};
            }

            // Optional schema: "@schema(1.0.0)"
            if (i < toks.size() && toks[i].substr(0, 7) == "@schema") {
                op.schema_str = std::string{toks[i]};
                ++i;
            }

            // op is always required unless annotated with "optional"
            op.is_required_op = true;
            if (i < toks.size() && toks[i] == "optional") {
                op.is_required_op = false;
                ++i;
            }

            // Remaining tokens are operands (until '@' source-loc marker)
            while (i < toks.size()) {
                if (toks[i] == "@" && i + 1 < toks.size()) {
                    op.source_loc = std::string{toks[i + 1]};
                    i += 2;
                }
                else {
                    op.operands.push_back(std::string{toks[i]});
                    ++i;
                }
            }

            // Track any value ids from results
            for (const auto& r : op.results) doc.value_ids.push_back(r);

            return op;
        }

        // ====================================================================
        // Emitter implementation — canonical mode
        //
        // Canonical rules:
        //   1. Header line first (always "lithe-ir <ver> / <stage> / <dialect> / <target>")
        //   2. Ops sorted by domain+name+schema_str for determinism.
        //   3. Results before '=', operands after op name, no trailing spaces.
        //   4. Source locations emitted only if present.
        //   5. Opaque sections appended last, sorted by name.
        //   6. No comments, no blank lines (canonical: byte-stable).
        // ====================================================================

        static std::string emit_canonical_(const lithe_text_ir_doc& doc) {
            std::string out;
            out.reserve(1024);

            // Header
            out += "lithe-ir ";
            out += doc.ir_format_version.empty() ? "1.0" : doc.ir_format_version;
            out += " / ";
            out += doc.stage_str.empty() ? "unknown" : doc.stage_str;
            out += " / ";
            out += doc.dialect_str.empty() ? "lithe" : doc.dialect_str;
            out += " / ";
            out += doc.target_str.empty() ? "native" : doc.target_str;
            out += '\n';

            // Blocks (sorted by id for determinism)
            std::vector<std::size_t> blk_order(doc.blocks.size());
            std::iota(blk_order.begin(), blk_order.end(), std::size_t{0});
            std::sort(blk_order.begin(), blk_order.end(), [&](std::size_t a, std::size_t b) {
                return doc.blocks[a].id < doc.blocks[b].id;
            });
            for (const auto idx : blk_order) {
                const auto& blk = doc.blocks[idx];
                out += "block ";
                out += blk.id;
                if (!blk.arg_ids.empty()) {
                    for (const auto& a : blk.arg_ids) {
                        out += ' ';
                        out += a;
                    }
                }
                if (!blk.succ_ids.empty()) {
                    out += " :";
                    for (const auto& s : blk.succ_ids) {
                        out += ' ';
                        out += s;
                    }
                }
                out += '\n';
            }

            // Ops (sorted by domain+name+schema_str for canonical determinism)
            std::vector<std::size_t> op_order(doc.ops.size());
            std::iota(op_order.begin(), op_order.end(), std::size_t{0});
            std::stable_sort(op_order.begin(), op_order.end(), [&](std::size_t a, std::size_t b) {
                const auto& oa = doc.ops[a];
                const auto& ob = doc.ops[b];
                if (oa.domain != ob.domain) return oa.domain < ob.domain;
                if (oa.name != ob.name) return oa.name < ob.name;
                return oa.schema_str < ob.schema_str;
            });

            for (const auto idx : op_order) {
                const auto& op = doc.ops[idx];

                if (!op.results.empty()) {
                    for (std::size_t j = 0; j < op.results.size(); ++j) {
                        if (j > 0) out += ' ';
                        out += op.results[j];
                    }
                    out += " = ";
                }

                out += op.domain;
                out += '.';
                out += op.name;

                if (!op.schema_str.empty()) {
                    out += ' ';
                    out += op.schema_str;
                }
                if (!op.is_required_op) out += " optional";
                for (const auto& oper : op.operands) {
                    out += ' ';
                    out += oper;
                }
                if (op.source_loc) {
                    out += " @ ";
                    out += *op.source_loc;
                }
                out += '\n';
            }

            // Opaque sections (sorted by name)
            std::vector<std::size_t> sec_order(doc.opaque_sections.size());
            std::iota(sec_order.begin(), sec_order.end(), std::size_t{0});
            std::sort(sec_order.begin(), sec_order.end(), [&](std::size_t a, std::size_t b) {
                return doc.opaque_sections[a].name < doc.opaque_sections[b].name;
            });
            for (const auto idx : sec_order) {
                const auto& sec = doc.opaque_sections[idx];
                out += "section ";
                out += sec.name;
                out += sec.required ? " required" : " optional";
                out += '\n';
                for (const auto& line : sec.lines) {
                    out += line;
                    out += '\n';
                }
            }

            return out;
        }

        // Emitter — pretty-print mode (indented, human-friendly)
        static std::string emit_pretty_(const lithe_text_ir_doc& doc,
                                        const text_print_options& opts) {
            const std::string indent(opts.indent_width, ' ');
            std::string out;
            out.reserve(2048);

            out += "lithe-ir ";
            out += doc.ir_format_version.empty() ? "1.0" : doc.ir_format_version;
            out += " / ";
            out += doc.stage_str.empty() ? "unknown" : doc.stage_str;
            out += " / ";
            out += doc.dialect_str.empty() ? "lithe" : doc.dialect_str;
            out += " / ";
            out += doc.target_str.empty() ? "native" : doc.target_str;
            out += '\n';

            for (const auto& blk : doc.blocks) {
                out += "block " + blk.id;
                if (!blk.arg_ids.empty()) {
                    out += " (";
                    for (std::size_t j = 0; j < blk.arg_ids.size(); ++j) {
                        if (j) out += ", ";
                        out += blk.arg_ids[j];
                    }
                    out += ')';
                }
                out += " {\n";

                for (const auto& op : doc.ops) {
                    out += indent;
                    if (!op.results.empty()) {
                        for (std::size_t j = 0; j < op.results.size(); ++j) {
                            if (j) out += ", ";
                            out += op.results[j];
                        }
                        out += " = ";
                    }
                    out += op.domain + '.' + op.name;
                    if (!op.schema_str.empty()) out += ' ' + op.schema_str;
                    if (!op.operands.empty()) {
                        out += '(';
                        for (std::size_t j = 0; j < op.operands.size(); ++j) {
                            if (j) out += ", ";
                            out += op.operands[j];
                        }
                        out += ')';
                    }
                    if (opts.emit_source_loc && op.source_loc) {
                        out += "  // @" + *op.source_loc;
                    }
                    out += '\n';
                }
                out += "}\n";
            }

            return out;
        }
    }; // class text_provider

    // =========================================================================
    // Static concept checks
    // =========================================================================

    static_assert(text_provider::available);
    static_assert(text_importer_for<text_provider, lithe_text_ir_doc>,
                  "text_provider must satisfy text_importer_for<text_provider, lithe_text_ir_doc>");
    static_assert(text_exporter_for<text_provider, lithe_text_ir_doc>,
                  "text_provider must satisfy text_exporter_for<text_provider, lithe_text_ir_doc>");
    static_assert(ir_validator_for<text_provider, lithe_text_ir_doc>,
                  "text_provider must satisfy ir_validator_for<text_provider, lithe_text_ir_doc>");
} // namespace lithe::ir
