#pragma once

// crank/frontend.hpp — Umbrella header for the crank frontend (Module 1).
//
// C++23, header-only, no virtual, no macros.
// Namespace: crank::frontend
//
// Pulls all Module 1 headers and exposes:
//   crank::frontend::parse_options
//   crank::frontend::parse_result
//   crank::frontend::parse(source, opts) -> parse_result
//
// parse_result:
//   .vakya_root   — std::any wrapping the root Vakya node
//   .spans        — property_store with source_span per node
//   .diagnostics  — collecting_sink
//   .parse_tree_json — non-empty if opts.dump == dump_mode::parse_tree
//   .ast_json        — non-empty if opts.dump == dump_mode::ast

#include "languages/crank/source_span.hpp"
#include "languages/crank/ast_tags.hpp"
#include "languages/crank/lexer.hpp"
#include "languages/crank/parser.hpp"
#include "languages/crank/build_ast.hpp"
#include "languages/crank/parser_stats.hpp"
#include "languages/crank/dump.hpp"

#include "vakya/property.hpp"
#include "vakya/diagnostics.hpp"

#include <any>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace crank::frontend {
    // ============================================================================
    // Options
    // ============================================================================

    enum class dump_mode : std::uint8_t {
        none = 0,
        parse_tree = 1,
        ast = 2,
    };

    struct parse_options {
        dump_mode dump = dump_mode::none;
        bool collect_stats = false;
    };

    // ============================================================================
    // parse_result
    // ============================================================================

    struct parse_result {
        std::any vakya_root;
        std::unique_ptr<vakya::property_store> spans;
        vakya::diag::collecting_sink diagnostics;
        std::optional<parse_stats> stats;
        std::shared_ptr<crank_source_file> typed_ast; // typed root — set when ok
        std::string parse_tree_json;
        std::string ast_json;
        bool ok = false;
    };

    // ============================================================================
    // parse
    // ============================================================================

    [[nodiscard]] inline parse_result
    parse(std::string_view source, parse_options opts = {}) {
        parse_result result;
        result.spans = std::make_unique<vakya::property_store>();

        // Allocate stats if requested
        std::optional<parse_stats> local_stats;
        parse_stats* stats_ptr = nullptr;
        if (opts.collect_stats) {
            local_stats.emplace();
            stats_ptr = &local_stats.value();
            // Set source metrics
            stats_ptr->source_bytes = source.size();
            stats_ptr->source_lines = 1 + static_cast<uint32_t>(std::count(source.begin(), source.end(), '\n'));
        }

        // 1. Parse via lexy grammar → parse_tree (with timing)
        auto t0 = std::chrono::steady_clock::now();
        auto tree = grammar::parse(source);
        auto t1 = std::chrono::steady_clock::now();

        if (stats_ptr) {
            stats_ptr->timings.lex_and_parse =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
        }

        // 2. Optional parse_tree JSON dump
        if (opts.dump == dump_mode::parse_tree || opts.dump == dump_mode::ast) {
            result.parse_tree_json = dump_parse_tree(tree);
        }

        if (tree.empty()) {
            result.diagnostics.on_diagnostic(
                make_error("crank.parse.empty", "source produced an empty parse_tree"));
            result.ok = false;
            if (stats_ptr) {
                stats_ptr->timings.total = stats_ptr->timings.lex_and_parse;
                result.stats = std::move(local_stats);
            }
            return std::move(result);
        }

        // 3. Build AST (with timing and stats collection)
        auto t2 = std::chrono::steady_clock::now();
        auto build = build_ast(tree, source, *result.spans, stats_ptr);
        auto t3 = std::chrono::steady_clock::now();

        if (stats_ptr) {
            stats_ptr->timings.ast_build =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2);
            stats_ptr->timings.total = stats_ptr->timings.lex_and_parse + stats_ptr->timings.ast_build;

            // Count diagnostics by severity
            for (const auto& diag : build.diagnostics.entries) {
                if (diag.is_error()) {
                    ++stats_ptr->error_count;
                }
                else if (diag.level == vakya::diag::severity::warning) {
                    ++stats_ptr->warning_count;
                }
                else {
                    ++stats_ptr->note_count;
                }
            }
        }

        result.vakya_root = std::move(build.root);
        result.typed_ast  = std::move(build.typed_ast_root);
        result.diagnostics = std::move(build.diagnostics);

        // 4. Optional AST JSON dump
        if (opts.dump == dump_mode::ast) {
            result.ast_json = dump_ast(result.vakya_root, *result.spans);
        }

        result.ok = !result.diagnostics.has_errors();
        if (stats_ptr) {
            result.stats = std::move(local_stats);
        }
        return std::move(result);
    }
} // namespace crank::frontend
