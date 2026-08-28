#pragma once

// =============================================================================
// lithe_ir/adapters/hl_mir.hpp — stage adapter for the hl_mir IR stage
//
//
// Maps the high-level MIR stage (lowered) to/from the generic format_descriptor
// + section model used by text_provider and binary_provider.
//
// The adapter works on a stable serialisable form (lithe_hl_mir_ir) that does
// not import lithe_codegen.hpp — it carries the interchange-safe subset.
//
// Backend-neutral: no backend include, no codegen dependency.
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../format.hpp"   // stage, schema_version

namespace lithe::ir::adapters {
    // =========================================================================
    // Interchange HL opcode — stable string identity (never an integer, never
    // tied to the internal hl_opcode enum values which may change).
    // =========================================================================

    // hl_wire_op — single HL-MIR operation in the interchange form
    struct hl_wire_op {
        std::uint32_t id = 0; // stable per-document id
        std::string domain; // e.g. "lithe.hl"
        std::string name; // e.g. "structured_for", "memref_load", "fadd"

        std::vector<std::uint32_t> operand_ids; // SSA value ids
        std::vector<std::uint32_t> result_ids; // SSA value ids produced
        std::uint32_t block_id = 0; // enclosing block
        std::uint32_t region_id = 0; // enclosing region
        schema_version op_schema{1, 0, 0}; // op schema for upgrade routing

        // Optional structured_for attributes (present iff name == "structured_for").
        struct for_attr {
            std::uint8_t rank = 1;
            bool is_parallel = false;
            // Bounds are serialised as {lower, upper, step} triples, rank-many.
            std::vector<std::int64_t> lower_bounds;
            std::vector<std::int64_t> upper_bounds;
            std::vector<std::int64_t> steps;
            std::vector<std::uint32_t> tile_sizes; // 0 = untiled
        };

        std::optional<for_attr> structured_for;

        // Optional memref type descriptor (present for load/store ops).
        struct memref_desc {
            std::uint8_t rank = 1;
            std::string element_kind; // "i8","i16","i32","i64","f32","f64"
            std::uint8_t elem_bits = 64;
            std::vector<std::uint64_t> shape;
            std::vector<std::int64_t> strides;
        };

        std::optional<memref_desc> memref;

        // Immediate scalar payload for a constant operation.  The unused
        // fields are ignored according to kind; this is intentionally a
        // stable, allocation-free wire representation.
        struct constant_wire_attr {
            std::uint8_t kind = 0; // codegen::hl::constant_kind
            std::int64_t integer = 0;
            double floating_point = 0.0;
            bool boolean = false;
        };

        std::optional<constant_wire_attr> constant;

        // Branch attrs (schema 1.1.0)
        struct branch_wire_attr {
            std::uint32_t target_block_id = 0;
        };

        std::optional<branch_wire_attr> branch;

        struct branch_cond_wire_attr {
            std::uint32_t true_block_id = 0;
            std::uint32_t false_block_id = 0;
        };

        std::optional<branch_cond_wire_attr> branch_cond;

        // Compare attr — shared by icmp / fcmp; predicate is a string-table index into the
        // canonical predicate set (eq,ne,slt,...,oeq,...); ordered meaningful only for fcmp.
        struct compare_wire_attr {
            std::uint32_t predicate_idx = 0;
            bool ordered = true;
        };

        std::optional<compare_wire_attr> compare;

        // Guard attr (schema 1.3.0) — indices into the function's strings table.
        struct guard_wire_attr {
            std::uint32_t guard_kind_idx = 0;
            std::uint32_t policy_idx = 0;
            std::uint32_t diag_code_idx = 0;
            std::uint32_t source_span_idx = 0;
        };

        std::optional<guard_wire_attr> guard;

        // Trap attr (schema 1.3.0)
        struct trap_wire_attr {
            std::uint32_t trap_kind_idx = 0;
            std::uint32_t diag_code_idx = 0;
        };

        std::optional<trap_wire_attr> trap;

        // Cleanup attr (schema 1.4.0)
        struct cleanup_wire_attr {
            std::vector<std::uint32_t> cleanup_ids;
        };

        std::optional<cleanup_wire_attr> cleanup;

        // Transaction attr (schema 1.5.0) — enum fields encoded as stable string indices.
        struct tx_wire_attr {
            std::uint32_t isolation_idx = 0; // string-table index
            std::uint16_t retry = 0;
            std::uint32_t replay_idx = 0;
            std::uint32_t conflict_idx = 0;
            std::uint32_t partial_idx = 0;
            std::uint32_t durability_idx = 0;
            std::uint32_t distribution_idx = 0;
            std::uint32_t coordinator_idx = 0;
        };

        std::optional<tx_wire_attr> transaction;
    };

    // hl_wire_block — basic block in serialisable form
    struct hl_wire_block {
        std::uint32_t id = 0;
        std::vector<std::uint32_t> op_ids; // ops in this block (ordered)
        std::vector<std::uint32_t> arg_ids; // block argument SSA ids
    };

    // hl_wire_region — region (containing blocks) in serialisable form
    struct hl_wire_region {
        std::uint32_t id = 0;
        std::vector<std::uint32_t> block_ids; // ordered
        std::vector<std::uint32_t> arg_ids; // region argument SSA ids
    };

    // hl_wire_value — SSA value descriptor
    struct hl_wire_value {
        std::uint32_t id = 0;
        std::string type_str; // stable type string e.g. "i64", "f64", "memref<2xi64>"
    };

    // =========================================================================
    // lithe_hl_mir_ir — stage-adapter IR object for the hl_mir / lowered stage
    // =========================================================================

    struct lithe_hl_mir_ir {
        std::string function_name;

        std::vector<hl_wire_value> values;
        std::vector<hl_wire_op> ops;
        std::vector<hl_wire_block> blocks;
        std::vector<hl_wire_region> regions;

        std::vector<std::uint32_t> entry_block_ids; // function entry blocks (per region)
        std::vector<std::string> strings; // string literal table

        stage source_stage = stage::lowered;
        schema_version schema = {1, 0, 0};

        [[nodiscard]] bool valid() const noexcept {
            return !function_name.empty() && !blocks.empty();
        }
    };

    // Section ids for HL-MIR IR interchange
    namespace section_ids {
        inline constexpr std::string_view hl_mir_values = "lithe.ir.hl_mir.values";
        inline constexpr std::string_view hl_mir_ops = "lithe.ir.hl_mir.ops";
        inline constexpr std::string_view hl_mir_blocks = "lithe.ir.hl_mir.blocks";
        inline constexpr std::string_view hl_mir_regions = "lithe.ir.hl_mir.regions";
        inline constexpr std::string_view hl_mir_meta = "lithe.ir.hl_mir.meta";
        inline constexpr std::string_view hl_mir_strings = "lithe.ir.hl_mir.strings";
    } // namespace section_ids (hl_mir)

    [[nodiscard]] inline constexpr bool is_hl_mir_stage(const stage s) noexcept {
        return s == stage::lowered;
    }
} // namespace lithe::ir::adapters
