#pragma once

// =============================================================================
// lithe_ir/adapters/physical_mir.hpp — stage adapter for the physical_mir stage
//
//
// Maps the physical register MIR stage to/from the generic format_descriptor
// + section model.  Carries a stable serialisable form that does not require
// importing lithe_codegen.hpp.
//
// Backend-neutral: no backend include.
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
    // Wire-form types for physical MIR interchange
    // All identifiers are fixed-width integers; no host-size types on the wire.
    // =========================================================================

    enum class wire_value_kind : std::uint8_t {
        unknown = 0,
        integer = 1,
        floating = 2,
        pointer = 3,
        aggregate = 4,
        managed = 5, // GC-managed pointer
    };

    // Serialisable virtual register
    struct wire_vreg {
        std::uint32_t id = 0;
        wire_value_kind kind = wire_value_kind::unknown;
        std::uint8_t bit_width = 64; // 8/16/32/64
    };

    // Serialisable physical register (stable name; not tied to host ABI enum)
    struct wire_preg {
        std::uint16_t id = 0;
        std::string name; // e.g. "x0", "rax" — informational only
    };

    // Serialisable spill slot
    struct wire_spill_slot {
        std::uint32_t id = 0;
        std::uint32_t size_bytes = 0;
        std::uint8_t align_log2 = 3; // 2^align_log2 bytes
        std::int32_t frame_offset = 0;
    };

    // Serialisable memory operand
    enum class wire_mem_kind : std::uint8_t {
        stack_frame = 0,
        direct = 1,
        offset = 2,
        indirect = 3,
        rip_rel = 4,
    };

    struct wire_operand {
        enum class kind_tag : std::uint8_t {
            vreg = 0,
            preg = 1,
            imm_i64 = 2,
            imm_f64 = 3,
            spill_slot = 4,
            memory = 5,
        };

        kind_tag kind = kind_tag::vreg;
        std::uint32_t vreg_id = 0;
        std::uint16_t preg_id = 0;
        std::int64_t imm_i64 = 0;
        double imm_f64 = 0.0;
        std::uint32_t spill_id = 0;
        wire_mem_kind mem_kind = wire_mem_kind::direct;
        std::int32_t mem_offset = 0;
        std::uint32_t mem_base_vreg = 0;
    };

    // Serialisable MIR instruction
    struct wire_instr {
        std::uint32_t id = 0;
        std::string op_domain; // e.g. "lithe.mir"
        std::string op_name; // e.g. "add", "load", "branch", "ret"
        schema_version op_schema{1, 0, 0}; // for upgrade routing

        std::vector<wire_operand> operands;
        std::vector<wire_vreg> defs; // defined vregs
        std::vector<std::uint32_t> succ_block_ids; // for branches
    };

    // Serialisable basic block
    struct wire_block {
        std::uint32_t id = 0;
        std::string label;
        std::vector<std::uint32_t> instr_ids; // ordered
        std::vector<wire_vreg> live_in;
        std::vector<wire_vreg> live_out;
    };

    // =========================================================================
    // lithe_physical_mir_ir — stage-adapter IR object for the physical_mir stage
    // =========================================================================

    struct lithe_physical_mir_ir {
        std::string function_name;

        std::vector<wire_vreg> vregs;
        std::vector<wire_preg> pregs;
        std::vector<wire_spill_slot> spills;
        std::vector<wire_instr> instrs;
        std::vector<wire_block> blocks;

        std::uint32_t entry_block_id = 0;

        // Calling convention (stable stable string, not tied to host enum)
        std::string calling_convention; // e.g. "c", "fast", "cold"

        std::vector<wire_vreg> param_vregs; // function argument virtual registers
        std::optional<wire_vreg> return_vreg; // return value vreg (if any)

        stage source_stage = stage::physical;
        schema_version schema = {1, 0, 0};

        [[nodiscard]] bool valid() const noexcept {
            return !function_name.empty();
        }
    };

    // Section ids for physical MIR interchange
    namespace section_ids {
        inline constexpr std::string_view phys_vregs = "lithe.ir.phys.vregs";
        inline constexpr std::string_view phys_pregs = "lithe.ir.phys.pregs";
        inline constexpr std::string_view phys_spills = "lithe.ir.phys.spills";
        inline constexpr std::string_view phys_instrs = "lithe.ir.phys.instrs";
        inline constexpr std::string_view phys_blocks = "lithe.ir.phys.blocks";
        inline constexpr std::string_view phys_meta = "lithe.ir.phys.meta";
    } // namespace section_ids (phys)

    [[nodiscard]] inline constexpr bool is_physical_stage(const stage s) noexcept {
        return s == stage::physical || s == stage::managed;
    }
} // namespace lithe::ir::adapters
