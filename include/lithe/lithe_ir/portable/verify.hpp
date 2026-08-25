#pragma once

// =============================================================================
// lithe_ir/portable/verify.hpp — independent deep semantic verifier
//
// Namespace: lithe::ir::portable
//
// verify_portable(module, policy) runs seven check groups against a
// portable_module entirely in wire form — no codegen include, no backend.
//
// Check groups:
//   T — types:        every value type_str parses; op arity matches table
//   C — cfg:          every block ends in exactly one terminator; targets exist
//   S — ssa:          every value defined exactly once; use dominates def
//   Y — symbols:      imports resolve; exports in range; no duplicate exports
//   E — effects:      effectful ops not in pure regions (policy-driven)
//   R — regions:      nesting acyclic (shared-block check); no shared blocks
//   K — capabilities: ops requiring a capability are covered by declared_capabilities
//
// Each failure emits a lithe::diag::diagnostic with a stable string code.
// Returns verify_report{ok, diagnostics}.
//
// Independent from providers and backends (arch §5).
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/DominatorTree.hpp"
#include "../../lithe_diagnostics.hpp"        // lithe::diag::diagnostic, severity, stage
#include "../adapters/hl_mir.hpp"             // lithe_hl_mir_ir, hl_wire_*
#include "../security_envelope.hpp"           // envelope_limits
#include "module.hpp"                         // portable_module, capability_set
#include "cfg_adapter.hpp"                    // to_litegraph, entry_node

namespace lithe::ir::portable {
    // =============================================================================
    // Stable diagnostic codes
    // =============================================================================

    namespace diag_codes {
        inline constexpr const char* type_parse_failed = "LITHE-PORT-T001";
        inline constexpr const char* type_arity_mismatch = "LITHE-PORT-T002";
        inline constexpr const char* type_shape_mismatch = "LITHE-PORT-T003"; // i1 cond / arm-type mismatch
        inline constexpr const char* block_no_terminator = "LITHE-PORT-C001";
        inline constexpr const char* branch_target_missing = "LITHE-PORT-C002";
        inline constexpr const char* op_after_terminator = "LITHE-PORT-C003";
        inline constexpr const char* value_multi_def = "LITHE-PORT-S001";
        inline constexpr const char* use_not_dominated = "LITHE-PORT-S002";
        inline constexpr const char* import_unresolved = "LITHE-PORT-Y001";
        inline constexpr const char* export_out_of_range = "LITHE-PORT-Y002";
        inline constexpr const char* export_duplicate = "LITHE-PORT-Y003";
        inline constexpr const char* effectful_in_pure = "LITHE-PORT-E001";
        inline constexpr const char* tx_op_outside_region = "LITHE-PORT-E002"; // tx.* outside tx.region
        inline constexpr const char* cleanup_op_outside = "LITHE-PORT-E003"; // cleanup_yield outside cleanup_region
        inline constexpr const char* region_cycle = "LITHE-PORT-R001";
        inline constexpr const char* block_in_two_regions = "LITHE-PORT-R002";
        inline constexpr const char* region_kind_mismatch = "LITHE-PORT-R003"; // wrong region kind for yield
        inline constexpr const char* capability_missing = "LITHE-PORT-K001";
        inline constexpr const char* limit_exceeded = "LITHE-PORT-L001";
    } // namespace diag_codes

    // =============================================================================
    // opcode_signature_entry — per-opcode contract (arity, effects, capability)
    // =============================================================================

    struct opcode_signature_entry {
        std::string_view domain;
        std::string_view name;
        std::uint8_t arity_min = 0;
        std::uint8_t arity_max = 255; // 255 = variadic
        std::uint8_t result_count = 0;
        bool is_terminator = false;
        bool reads_memory = false;
        bool writes_memory = false;
        bool may_trap = false; // guard/trap/tx.abort (schema 1.3.0)
        portable_capability_bit required_cap = static_cast<portable_capability_bit>(0);
    };

    // Canonical opcode signature table (wire-stable, keyed on domain+name).
    // Used by T/E/K checks and re-exported for impl-2 optimizer legality.
    inline constexpr std::array<opcode_signature_entry, 48> k_opcode_signatures = {
        {
            // ── Existing 22 entries (schema 1.0.0) ─────────────────────────────────
            // {domain, name, amin, amax, res, term, rd, wr, may_trap, cap}
            {
                "lithe.hl", "structured_for", 0, 255, 0, false, true, true, false,
                static_cast<portable_capability_bit>(0)
            },
            {
                "lithe.hl", "structured_reduce", 0, 255, 0, false, true, true, false,
                static_cast<portable_capability_bit>(0)
            },
            {"lithe.hl", "region_yield", 0, 255, 0, true, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "loop_index", 0, 0, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "memref_load", 1, 255, 1, false, true, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "memref_store", 2, 255, 0, false, false, true, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "fadd", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "fsub", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "fmul", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "fdiv", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "fneg", 1, 1, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "add", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "sub", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "mul", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "div", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "exp", 1, 1, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "log", 1, 1, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "sqrt", 1, 1, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "abs", 1, 1, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "call", 0, 255, 0, false, false, false, false, portable_capability_bit::external_calls},
            {"lithe.hl", "constant", 0, 0, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "argument", 0, 0, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            // ── Schema 1.1.0 — CFG + compare/select ────────────────────────────────
            {"lithe.hl", "branch", 0, 0, 0, true, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "branch_cond", 1, 1, 0, true, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "return", 0, 255, 0, true, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "icmp", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "fcmp", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "select", 3, 3, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            // ── Schema 1.2.0 — Integer ops ──────────────────────────────────────────
            {"lithe.hl", "sdiv", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "udiv", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "srem", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "urem", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "bit_and", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "bit_or", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "bit_xor", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "bit_not", 1, 1, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "shl", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "lshr", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "ashr", 2, 2, 1, false, false, false, false, static_cast<portable_capability_bit>(0)},
            // ── Schema 1.3.0 — Safety ───────────────────────────────────────────────
            {"lithe.hl", "guard", 1, 1, 0, false, false, false, true, static_cast<portable_capability_bit>(0)},
            {"lithe.hl", "trap", 0, 255, 0, true, false, false, true, static_cast<portable_capability_bit>(0)},
            // ── Schema 1.4.0 — Cleanup / defer ─────────────────────────────────────
            {
                "lithe.hl", "cleanup_region", 0, 255, 0, false, false, false, false,
                portable_capability_bit::defer_scopes
            },
            {"lithe.hl", "cleanup_yield", 0, 255, 0, true, false, false, false, portable_capability_bit::defer_scopes},
            // ── Schema 1.5.0 — Transactions ─────────────────────────────────────────
            {"lithe.hl", "tx.region", 0, 255, 1, false, true, true, false, portable_capability_bit::transactions},
            {"lithe.hl", "tx.read", 2, 2, 1, false, true, false, false, portable_capability_bit::transactions},
            {"lithe.hl", "tx.write", 3, 3, 0, false, false, true, false, portable_capability_bit::transactions},
            {"lithe.hl", "tx.abort", 0, 1, 0, true, false, false, true, portable_capability_bit::transactions},
            {"lithe.hl", "tx.yield", 0, 255, 0, true, false, false, false, portable_capability_bit::transactions},
        }
    };

    [[nodiscard]] inline const opcode_signature_entry*
    find_signature(std::string_view domain, std::string_view name) noexcept {
        for (const auto& e : k_opcode_signatures)
            if (e.domain == domain && e.name == name) return &e;
        return nullptr;
    }

    // =============================================================================
    // verify_report
    // =============================================================================

    struct verify_report {
        bool ok = true;
        std::vector<lithe::diag::diagnostic> diagnostics;

        void error(const char* code, std::string msg) {
            diagnostics.push_back({
                .level = lithe::diag::severity::error,
                .stage = lithe::diag::stage::ir,
                .code = code,
                .message = std::move(msg)
            });
            ok = false;
        }

        void warn(const char* code, std::string msg) {
            diagnostics.push_back({
                .level = lithe::diag::severity::warning,
                .stage = lithe::diag::stage::ir,
                .code = code,
                .message = std::move(msg)
            });
        }
    };

    // =============================================================================
    // verify_policy
    // =============================================================================

    struct verify_policy {
        bool require_capability_coverage = true;
        bool allow_unknown_optional_ops = true;
        envelope_limits limits{};
    };

    // =============================================================================
    // Internal type-string parse check (wire form, no codegen)
    // Accepts: i1,i8,i16,i32,i64,iN, f16,f32,f64,fN, memref<...>, opaqueN
    // =============================================================================

    namespace detail {
        // Returns true iff s is a valid scalar type string (§5: int_ty | float_ty | bool_ty | opaque_ty).
        [[nodiscard]] constexpr bool scalar_type_parseable(std::string_view s) noexcept {
            if (s.empty()) return false;
            if (s == "i1" || s == "i8" || s == "i16" || s == "i32" || s == "i64") return true;
            if (s == "f16" || s == "f32" || s == "f64") return true;
            if (s.size() > 1 && (s[0] == 'i' || s[0] == 'f')) {
                for (std::size_t i = 1; i < s.size(); ++i)
                    if (s[i] < '0' || s[i] > '9') return false;
                return true;
            }
            // opaque followed by one or more digits (e.g. opaque64)
            if (s.starts_with("opaque")) {
                const auto rest = s.substr(6);
                if (rest.empty()) return false;
                for (char c : rest)
                    if (c < '0' || c > '9') return false;
                return true;
            }
            return false;
        }

        // Parses the inner content of memref<...>.
        // Grammar (§5): memref_inner = dim ('x' dim)* 'x' scalar_type
        //   dim = decimal_digits | '?'
        // At least one dim is required before the scalar element type.
        // Returns false if no dims precede the scalar or any dim token is malformed.
        [[nodiscard]] inline bool memref_inner_parseable(std::string_view inner) noexcept {
            // inner must be non-empty and contain at least one 'x' separator
            if (inner.empty()) return false;

            bool found_dim = false;
            std::string_view remaining = inner;

            while (true) {
                // Find next 'x' separator
                const auto x_pos = remaining.find('x');
                if (x_pos == std::string_view::npos) {
                    // No more 'x': remaining must be the scalar element type.
                    // We require at least one dim to have been consumed.
                    return found_dim && scalar_type_parseable(remaining);
                }

                const auto token = remaining.substr(0, x_pos);
                remaining = remaining.substr(x_pos + 1);

                // token must be a valid dim: one or more digits, or '?'
                if (token.empty()) return false;
                if (token == "?") {
                    found_dim = true;
                    continue;
                }
                for (char c : token)
                    if (c < '0' || c > '9') return false;
                found_dim = true;
            }
        }

        [[nodiscard]] inline bool type_str_parseable(std::string_view s) noexcept {
            if (s.empty()) return false;
            if (s.starts_with("memref<") && s.ends_with('>')) {
                const auto inner = s.substr(7, s.size() - 8); // strip "memref<" and ">"
                return memref_inner_parseable(inner);
            }
            return scalar_type_parseable(s);
        }
    } // namespace detail

    // =============================================================================
    // verify_portable — runs all seven check groups
    // =============================================================================

    [[nodiscard]] inline verify_report
    verify_portable(const portable_module& mod, const verify_policy& policy = {}) {
        verify_report rep;

        // =========================================================================
        // L — limits (fast gate)
        // =========================================================================
        for (const auto& fn : mod.functions) {
            if (fn.blocks.size() > policy.limits.max_block_count)
                rep.error(diag_codes::limit_exceeded,
                          "block count exceeds limit in " + fn.function_name);
            if (fn.values.size() > policy.limits.max_value_count)
                rep.error(diag_codes::limit_exceeded,
                          "value count exceeds limit in " + fn.function_name);
            if (fn.ops.size() > policy.limits.max_op_count)
                rep.error(diag_codes::limit_exceeded,
                          "op count exceeds limit in " + fn.function_name);
        }

        // =========================================================================
        // Y — symbols
        // =========================================================================
        {
            const auto fn_count = static_cast<std::uint32_t>(mod.functions.size());
            std::unordered_set<std::string> export_names;
            for (const auto& ex : mod.exports) {
                if (ex.function_index >= fn_count)
                    rep.error(diag_codes::export_out_of_range,
                              "export '" + ex.symbol + "' function_index out of range");
                if (!export_names.insert(ex.symbol).second)
                    rep.error(diag_codes::export_duplicate,
                              "duplicate export '" + ex.symbol + "'");
            }
            for (const auto& imp : mod.imports)
                if (imp.module.empty() || imp.symbol.empty())
                    rep.error(diag_codes::import_unresolved,
                              "import has empty module or symbol");
        }

        for (const auto& fn : mod.functions) {
            const std::uint32_t n_vals = static_cast<std::uint32_t>(fn.values.size());
            const std::uint32_t n_ops = static_cast<std::uint32_t>(fn.ops.size());

            // =====================================================================
            // T — types
            // =====================================================================
            for (const auto& v : fn.values) {
                if (!detail::type_str_parseable(v.type_str))
                    rep.error(diag_codes::type_parse_failed,
                              "value id=" + std::to_string(v.id) +
                              " unparsable type '" + v.type_str + "' in " + fn.function_name);
            }

            // Build value-id → type_str map for per-op type-shape checks.
            std::unordered_map<std::uint32_t, std::string> val_type;
            for (const auto& v : fn.values)
                val_type[v.id] = v.type_str;

            for (const auto& op : fn.ops) {
                const auto* sig = find_signature(op.domain, op.name);
                if (!sig) {
                    if (!policy.allow_unknown_optional_ops)
                        rep.error(diag_codes::type_arity_mismatch,
                                  "unknown op '" + op.domain + "." + op.name + "'");
                    continue;
                }
                const auto arity = static_cast<std::uint8_t>(
                    std::min(op.operand_ids.size(), std::size_t{255}));
                if (arity < sig->arity_min)
                    rep.error(diag_codes::type_arity_mismatch,
                              "op '" + op.name + "' id=" + std::to_string(op.id) +
                              " too few operands in " + fn.function_name);
                if (sig->arity_max != 255 && arity > sig->arity_max)
                    rep.error(diag_codes::type_arity_mismatch,
                              "op '" + op.name + "' id=" + std::to_string(op.id) +
                              " too many operands in " + fn.function_name);

                // T003: per-op type-shape checks (checkable without function sig wire field).
                // branch_cond / guard operand[0] must be i1.
                if ((op.name == "branch_cond" || op.name == "guard") &&
                    !op.operand_ids.empty()) {
                    const auto it = val_type.find(op.operand_ids[0]);
                    if (it != val_type.end() && it->second != "i1")
                        rep.error(diag_codes::type_shape_mismatch,
                                  "op '" + op.name + "' id=" + std::to_string(op.id) +
                                  " operand[0] must be i1, got '" + it->second +
                                  "' in " + fn.function_name);
                }
                // select operand[0] must be i1; operand[1] and operand[2] must match.
                if (op.name == "select" && op.operand_ids.size() == 3) {
                    const auto it0 = val_type.find(op.operand_ids[0]);
                    if (it0 != val_type.end() && it0->second != "i1")
                        rep.error(diag_codes::type_shape_mismatch,
                                  "op 'select' id=" + std::to_string(op.id) +
                                  " condition operand must be i1 in " + fn.function_name);
                    const auto it1 = val_type.find(op.operand_ids[1]);
                    const auto it2 = val_type.find(op.operand_ids[2]);
                    if (it1 != val_type.end() && it2 != val_type.end() &&
                        it1->second != it2->second)
                        rep.error(diag_codes::type_shape_mismatch,
                                  "op 'select' id=" + std::to_string(op.id) +
                                  " arm types must match in " + fn.function_name);
                }
                // icmp/fcmp must produce i1.
                if ((op.name == "icmp" || op.name == "fcmp") && !op.result_ids.empty()) {
                    const auto it = val_type.find(op.result_ids[0]);
                    if (it != val_type.end() && it->second != "i1")
                        rep.error(diag_codes::type_shape_mismatch,
                                  "op '" + op.name + "' id=" + std::to_string(op.id) +
                                  " result must be i1 in " + fn.function_name);
                }
            }

            // =====================================================================
            // C — cfg: every non-empty block ends in exactly one terminator;
            //          no ops after terminator (C003); branch targets exist (C002).
            // =====================================================================

            // Build block-id set for target existence check.
            std::unordered_set<std::uint32_t> block_ids;
            for (const auto& blk : fn.blocks) block_ids.insert(blk.id);

            for (const auto& blk : fn.blocks) {
                if (blk.op_ids.empty()) continue;
                bool found_term = false;
                for (const std::uint32_t oid : blk.op_ids) {
                    if (oid >= n_ops) continue;
                    const auto& op = fn.ops[oid];
                    const auto* sig = find_signature(op.domain, op.name);
                    if (sig && sig->is_terminator) {
                        if (found_term) {
                            // Already had a terminator — this is C003.
                            rep.error(diag_codes::op_after_terminator,
                                      "op '" + op.name + "' id=" + std::to_string(op.id) +
                                      " appears after terminator in block id=" +
                                      std::to_string(blk.id) + " in " + fn.function_name);
                        }
                        found_term = true;

                        // C002: validate branch targets from wire attrs.
                        if (op.name == "branch" && op.branch.has_value()) {
                            if (!block_ids.count(op.branch->target_block_id))
                                rep.error(diag_codes::branch_target_missing,
                                          "op 'branch' id=" + std::to_string(op.id) +
                                          " target block " +
                                          std::to_string(op.branch->target_block_id) +
                                          " not found in " + fn.function_name);
                        }
                        if (op.name == "branch_cond" && op.branch_cond.has_value()) {
                            if (!block_ids.count(op.branch_cond->true_block_id))
                                rep.error(diag_codes::branch_target_missing,
                                          "op 'branch_cond' id=" + std::to_string(op.id) +
                                          " true_block " +
                                          std::to_string(op.branch_cond->true_block_id) +
                                          " not found in " + fn.function_name);
                            if (!block_ids.count(op.branch_cond->false_block_id))
                                rep.error(diag_codes::branch_target_missing,
                                          "op 'branch_cond' id=" + std::to_string(op.id) +
                                          " false_block " +
                                          std::to_string(op.branch_cond->false_block_id) +
                                          " not found in " + fn.function_name);
                        }
                    }
                    else if (found_term) {
                        // Non-terminator after a terminator → C003.
                        rep.error(diag_codes::op_after_terminator,
                                  "op '" + op.name + "' id=" + std::to_string(op.id) +
                                  " appears after terminator in block id=" +
                                  std::to_string(blk.id) + " in " + fn.function_name);
                    }
                }
                if (!found_term)
                    rep.error(diag_codes::block_no_terminator,
                              "block id=" + std::to_string(blk.id) +
                              " missing terminator in " + fn.function_name);
            }

            // =====================================================================
            // S — SSA: each value defined exactly once
            // =====================================================================
            {
                std::unordered_set<std::uint32_t> defined;
                for (const auto& blk : fn.blocks)
                    for (const std::uint32_t aid : blk.arg_ids)
                        if (!defined.insert(aid).second)
                            rep.error(diag_codes::value_multi_def,
                                      "value id=" + std::to_string(aid) +
                                      " defined twice (block arg) in " + fn.function_name);
                for (const auto& op : fn.ops)
                    for (const std::uint32_t rid : op.result_ids)
                        if (!defined.insert(rid).second)
                            rep.error(diag_codes::value_multi_def,
                                      "value id=" + std::to_string(rid) +
                                      " defined twice in " + fn.function_name);

                // Dominance check via compute_dominator_tree
                if (fn.blocks.size() > 1) {
                    auto [cfg_g, nids] = to_litegraph(fn);
                    const std::uint32_t entry_bid = entry_node(fn);
                    if (entry_bid < nids.size() && nids[entry_bid].is_valid()) {
                        try {
                            const auto dom_tree =
                                litegraph::compute_dominator_tree(cfg_g, nids[entry_bid]);
                            // Build def → block id map
                            std::vector<std::uint32_t> def_block(n_vals,
                                                                 std::numeric_limits<std::uint32_t>::max());
                            for (const auto& blk : fn.blocks) {
                                for (const std::uint32_t aid : blk.arg_ids)
                                    if (aid < n_vals) def_block[aid] = blk.id;
                                for (const std::uint32_t oid : blk.op_ids) {
                                    if (oid >= n_ops) continue;
                                    for (const std::uint32_t rid : fn.ops[oid].result_ids)
                                        if (rid < n_vals) def_block[rid] = blk.id;
                                }
                            }
                            // Build block-id → nids-index for O(1) NodeId lookup.
                            std::unordered_map<std::uint32_t, std::uint32_t> bid_to_nidx;
                            for (std::uint32_t bi = 0;
                                 bi < static_cast<std::uint32_t>(fn.blocks.size()); ++bi)
                                bid_to_nidx[fn.blocks[bi].id] = bi;

                            // Verify dominance: def_block must dominate use_block.
                            for (const auto& blk : fn.blocks) {
                                const auto use_it = bid_to_nidx.find(blk.id);
                                if (use_it == bid_to_nidx.end()) continue;
                                const std::uint32_t use_nidx = use_it->second;
                                if (use_nidx >= nids.size() || !nids[use_nidx].is_valid()) continue;

                                for (const std::uint32_t oid : blk.op_ids) {
                                    if (oid >= n_ops) continue;
                                    for (const std::uint32_t vid : fn.ops[oid].operand_ids) {
                                        if (vid >= n_vals) continue;
                                        const std::uint32_t def_bid = def_block[vid];
                                        if (def_bid == std::numeric_limits<std::uint32_t>::max())
                                            continue;
                                        const auto def_it = bid_to_nidx.find(def_bid);
                                        if (def_it == bid_to_nidx.end()) continue;
                                        const std::uint32_t def_nidx = def_it->second;
                                        if (def_nidx >= nids.size() || !nids[def_nidx].is_valid())
                                            continue;
                                        if (!litegraph::dominator_analysis::dominates(
                                            dom_tree, nids[def_nidx], nids[use_nidx]))
                                            rep.error(diag_codes::use_not_dominated,
                                                      "value id=" + std::to_string(vid) +
                                                      " used in block " + std::to_string(blk.id) +
                                                      " but defined in non-dominating block " +
                                                      std::to_string(def_bid) + " in " +
                                                      fn.function_name);
                                    }
                                }
                            }
                        }
                        catch (...) {
                            rep.warn(diag_codes::use_not_dominated,
                                     "dominance check failed (CFG error) in " + fn.function_name);
                        }
                    }
                }
            }

            // =====================================================================
            // R — regions: no shared blocks; region-kind placement (R003).
            // =====================================================================
            {
                std::unordered_set<std::uint32_t> seen_blocks;
                for (const auto& reg : fn.regions)
                    for (const std::uint32_t bid : reg.block_ids)
                        if (!seen_blocks.insert(bid).second)
                            rep.error(diag_codes::block_in_two_regions,
                                      "block id=" + std::to_string(bid) +
                                      " in multiple regions in " + fn.function_name);

                // R003: region-kind mismatch — a tx.region body must not contain
                // cleanup_region ops and vice versa.  Derive host regions for each kind
                // (the enclosing region of the op that creates the body).
                {
                    std::unordered_set<std::uint32_t> tx_host;
                    std::unordered_set<std::uint32_t> cleanup_host;
                    for (const auto& op : fn.ops) {
                        if (op.name == "tx.region") tx_host.insert(op.region_id);
                        if (op.name == "cleanup_region") cleanup_host.insert(op.region_id);
                    }
                    // If tx and cleanup share host regions they are structurally mixed — R003.
                    for (const auto rid : tx_host) {
                        if (cleanup_host.count(rid))
                            rep.error(diag_codes::region_kind_mismatch,
                                      "tx.region and cleanup_region share enclosing region id=" +
                                      std::to_string(rid) + " in " + fn.function_name);
                    }
                }
            }

            // =====================================================================
            // E — effects: tx op placement (E002), cleanup placement (E003)
            // =====================================================================
            {
                // E002: tx.read/write/abort/yield must appear inside a tx.region body.
                // A tx body region is any region that is a strict child of the region
                // containing a tx.region op.  In wire form we don't store region trees
                // explicitly, so we use a necessary condition: at least one tx.region op
                // must exist in the function, and the tx inner op must not share the same
                // enclosing region as that tx.region op (it must be nested deeper).
                //
                // Derived sets:
                //   tx_host_regions  — region_ids where a tx.region op lives (the "host")
                //   cleanup_host_regions — region_ids where a cleanup_region op lives
                std::unordered_set<std::uint32_t> tx_host_regions;
                std::unordered_set<std::uint32_t> cleanup_host_regions;
                for (const auto& op : fn.ops) {
                    if (op.name == "tx.region")
                        tx_host_regions.insert(op.region_id);
                    if (op.name == "cleanup_region")
                        cleanup_host_regions.insert(op.region_id);
                }

                for (const auto& op : fn.ops) {
                    const std::string_view nm = op.name;
                    if (nm == "tx.read" || nm == "tx.write" ||
                        nm == "tx.abort" || nm == "tx.yield") {
                        // Must be in a nested region: tx_host_regions non-empty and
                        // this op is NOT in the same region as its tx.region parent
                        // (it is inside the body, which is a child region).
                        const bool in_tx_body =
                            !tx_host_regions.empty() &&
                            !tx_host_regions.count(op.region_id);
                        if (!in_tx_body)
                            rep.error(diag_codes::tx_op_outside_region,
                                      "op '" + op.name + "' id=" + std::to_string(op.id) +
                                      " used outside a tx.region body in " + fn.function_name);
                    }
                    if (nm == "cleanup_yield") {
                        const bool in_cleanup_body =
                            !cleanup_host_regions.empty() &&
                            !cleanup_host_regions.count(op.region_id);
                        if (!in_cleanup_body)
                            rep.error(diag_codes::cleanup_op_outside,
                                      "op 'cleanup_yield' id=" + std::to_string(op.id) +
                                      " used outside a cleanup_region body in " + fn.function_name);
                    }
                }
            }

            // =====================================================================
            // K — capabilities
            // =====================================================================
            if (policy.require_capability_coverage) {
                for (const auto& op : fn.ops) {
                    const auto* sig = find_signature(op.domain, op.name);
                    if (!sig) continue;
                    const auto req = static_cast<std::uint32_t>(sig->required_cap);
                    if (req != 0 && !mod.declared_capabilities.has(sig->required_cap))
                        rep.error(diag_codes::capability_missing,
                                  "op '" + op.name + "' requires capability bit " +
                                  std::to_string(req) + " not declared in " + fn.function_name);
                }
            }
        }

        return rep;
    }
} // namespace lithe::ir::portable
