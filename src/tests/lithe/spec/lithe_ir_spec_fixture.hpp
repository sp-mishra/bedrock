#pragma once

// =============================================================================
// src/tests/spec/lithe_ir_spec_fixture.hpp — spec conformance constants
//
// Transcribes the normative enumerated constants from docs/spec/lithe-ir-spec.md.
// test_lithe_ir_spec_conformance.cpp asserts code == fixture bidirectionally.
//
// If code and spec diverge, the conformance test fails — forcing a spec update.
//
// NEVER include this from production headers.  Test-tree only.
// =============================================================================

#include <array>
#include <cstdint>
#include <string_view>

namespace lithe::spec::fixture {
    // =============================================================================
    // §3.1 — Stage integer values (STABLE)
    // =============================================================================

    struct spec_stage_entry {
        std::string_view name;
        std::uint8_t value;
    };

    inline constexpr std::array<spec_stage_entry, 6> k_spec_stages = {
        {
            {"surface", 0},
            {"canonical", 1},
            {"optimized", 2},
            {"lowered", 3},
            {"physical", 4},
            {"managed", 5},
        }
    };

    // =============================================================================
    // §6.2 / §7.4 / §9.3 — Section id string constants (STABLE)
    // =============================================================================

    // Graph IR section ids
    inline constexpr std::array<std::string_view, 4> k_spec_graph_section_ids = {
        {
            "lithe.ir.graph.nodes",
            "lithe.ir.graph.strings",
            "lithe.ir.graph.roots",
            "lithe.ir.graph.meta",
        }
    };

    // HL MIR section ids
    inline constexpr std::array<std::string_view, 6> k_spec_hl_mir_section_ids = {
        {
            "lithe.ir.hl_mir.values",
            "lithe.ir.hl_mir.ops",
            "lithe.ir.hl_mir.blocks",
            "lithe.ir.hl_mir.regions",
            "lithe.ir.hl_mir.meta",
            "lithe.ir.hl_mir.strings",
        }
    };

    // Physical MIR section ids
    inline constexpr std::array<std::string_view, 6> k_spec_phys_section_ids = {
        {
            "lithe.ir.phys.vregs",
            "lithe.ir.phys.pregs",
            "lithe.ir.phys.spills",
            "lithe.ir.phys.instrs",
            "lithe.ir.phys.blocks",
            "lithe.ir.phys.meta",
        }
    };

    // =============================================================================
    // §12.1 — Envelope magic (STABLE)
    // =============================================================================

    inline constexpr std::array<std::uint8_t, 4> k_spec_envelope_magic = {
        0x4C, 0x54, 0x49, 0x52 // "LTIR"
    };

    // =============================================================================
    // §12.4 — Digest algorithm ids (STABLE)
    // =============================================================================

    struct spec_digest_entry {
        std::string_view name;
        std::uint8_t id;
        std::uint8_t size_bytes; // 0 = no digest
    };

    inline constexpr std::array<spec_digest_entry, 4> k_spec_digest_algs = {
        {
            {"none", 0, 0},
            {"sha256", 1, 32},
            {"sha3_256", 2, 32},
            {"blake3", 3, 32},
        }
    };

    // Canonical default digest size for sha256.
    inline constexpr std::uint8_t k_spec_sha256_digest_size = 32;

    // =============================================================================
    // §8.2 — Opcode signature registry (STABLE)
    // =============================================================================

    struct spec_opcode_entry {
        std::string_view domain;
        std::string_view name;
        std::uint8_t arity_min;
        std::uint8_t arity_max; // 255 = variadic
        std::uint8_t result_count;
        bool is_terminator;
        bool reads_memory;
        bool writes_memory;
        bool may_trap; // schema 1.3.0 (guard/trap/tx.abort)
        bool requires_external_calls_cap; // true iff required_cap == external_calls
    };

    inline constexpr std::array<spec_opcode_entry, 48> k_spec_opcodes = {
        {
            // ── Schema 1.0.0 ── 22 existing entries
            // {domain, name, amin, amax, res, term, rd, wr, may_trap, ext_call}
            {"lithe.hl", "structured_for", 0, 255, 0, false, true, true, false, false},
            {"lithe.hl", "structured_reduce", 0, 255, 0, false, true, true, false, false},
            {"lithe.hl", "region_yield", 0, 255, 0, true, false, false, false, false},
            {"lithe.hl", "loop_index", 0, 0, 1, false, false, false, false, false},
            {"lithe.hl", "memref_load", 1, 255, 1, false, true, false, false, false},
            {"lithe.hl", "memref_store", 2, 255, 0, false, false, true, false, false},
            {"lithe.hl", "fadd", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "fsub", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "fmul", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "fdiv", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "fneg", 1, 1, 1, false, false, false, false, false},
            {"lithe.hl", "add", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "sub", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "mul", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "div", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "exp", 1, 1, 1, false, false, false, false, false},
            {"lithe.hl", "log", 1, 1, 1, false, false, false, false, false},
            {"lithe.hl", "sqrt", 1, 1, 1, false, false, false, false, false},
            {"lithe.hl", "abs", 1, 1, 1, false, false, false, false, false},
            {"lithe.hl", "call", 0, 255, 0, false, false, false, false, true},
            {"lithe.hl", "constant", 0, 0, 1, false, false, false, false, false},
            {"lithe.hl", "argument", 0, 0, 1, false, false, false, false, false},
            // ── Schema 1.1.0 — CFG + compare/select ────────────────────────────────
            {"lithe.hl", "branch", 0, 0, 0, true, false, false, false, false},
            {"lithe.hl", "branch_cond", 1, 1, 0, true, false, false, false, false},
            {"lithe.hl", "return", 0, 255, 0, true, false, false, false, false},
            {"lithe.hl", "icmp", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "fcmp", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "select", 3, 3, 1, false, false, false, false, false},
            // ── Schema 1.2.0 — Integer ops ──────────────────────────────────────────
            {"lithe.hl", "sdiv", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "udiv", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "srem", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "urem", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "bit_and", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "bit_or", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "bit_xor", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "bit_not", 1, 1, 1, false, false, false, false, false},
            {"lithe.hl", "shl", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "lshr", 2, 2, 1, false, false, false, false, false},
            {"lithe.hl", "ashr", 2, 2, 1, false, false, false, false, false},
            // ── Schema 1.3.0 — Safety ───────────────────────────────────────────────
            {"lithe.hl", "guard", 1, 1, 0, false, false, false, true, false},
            {"lithe.hl", "trap", 0, 255, 0, true, false, false, true, false},
            // ── Schema 1.4.0 — Cleanup / defer ─────────────────────────────────────
            {"lithe.hl", "cleanup_region", 0, 255, 0, false, false, false, false, false},
            {"lithe.hl", "cleanup_yield", 0, 255, 0, true, false, false, false, false},
            // ── Schema 1.5.0 — Transactions ─────────────────────────────────────────
            {"lithe.hl", "tx.region", 0, 255, 1, false, true, true, false, false},
            {"lithe.hl", "tx.read", 2, 2, 1, false, true, false, false, false},
            {"lithe.hl", "tx.write", 3, 3, 0, false, false, true, false, false},
            {"lithe.hl", "tx.abort", 0, 1, 0, true, false, false, true, false},
            {"lithe.hl", "tx.yield", 0, 255, 0, true, false, false, false, false},
        }
    };

    // =============================================================================
    // §5.3 — Type grammar accept/reject tables
    // =============================================================================

    // Valid type strings that parse_value_type MUST accept
    inline constexpr std::array<std::string_view, 12> k_spec_valid_types = {
        {
            "i1", "i8", "i16", "i32", "i64",
            "f16", "f32", "f64",
            "i128", "opaque64",
            "memref<4xi32>",
            "memref<2x?xi64>",
        }
    };

    // Invalid type strings that parse_value_type MUST reject
    inline constexpr std::array<std::string_view, 6> k_spec_invalid_types = {
        {
            "", // empty
            "int32", // wrong spelling
            "float", // wrong spelling
            "i 32", // whitespace
            "MEMREF<4xi32>", // wrong case
            "memref<xi32>", // missing dim count
        }
    };

    // =============================================================================
    // §13 — Stable diagnostic codes
    // =============================================================================

    inline constexpr std::array<std::string_view, 19> k_spec_diag_codes = {
        {
            "LITHE-PORT-T001",
            "LITHE-PORT-T002",
            "LITHE-PORT-T003", // i1 type-shape mismatch (schema 1.1.0)
            "LITHE-PORT-C001",
            "LITHE-PORT-C002",
            "LITHE-PORT-C003", // op after terminator (schema 1.1.0)
            "LITHE-PORT-S001",
            "LITHE-PORT-S002",
            "LITHE-PORT-Y001",
            "LITHE-PORT-Y002",
            "LITHE-PORT-Y003",
            "LITHE-PORT-E001",
            "LITHE-PORT-E002", // tx op outside tx.region (schema 1.5.0)
            "LITHE-PORT-E003", // cleanup_yield outside cleanup_region (schema 1.4.0)
            "LITHE-PORT-R001",
            "LITHE-PORT-R002",
            "LITHE-PORT-R003", // region-kind mismatch (schema 1.4.0/1.5.0)
            "LITHE-PORT-K001",
            "LITHE-PORT-L001",
        }
    };
} // namespace lithe::spec::fixture

