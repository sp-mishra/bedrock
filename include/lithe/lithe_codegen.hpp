#pragma once

// Backend-neutral MIR model and primitive codegen structures.
// Contains: registers, operands, instructions, blocks, functions,
//           ABI/frame/memory structures, MIR phase wrappers,
//           dump helpers, and structural verification helpers.
// Does not own: optimization policy or compile pipeline orchestration.

#include "lithe_core.hpp"
#include "lithe_semantic.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#ifndef LITHE_ENABLE_OBSERVABILITY
#define LITHE_ENABLE_OBSERVABILITY 0
#endif

#if defined(__has_include) && !defined(LITHE_HAS_NADI)
#  if __has_include("../observability/nadi.hpp")
#    include "../observability/nadi.hpp"
#    define LITHE_HAS_NADI 1
#  endif
#  if defined(LITHE_HAS_NADI) && __has_include("../observability/sinks/thread_local_sink.hpp")
#    include "../observability/sinks/thread_local_sink.hpp"
#    define LITHE_HAS_THREAD_LOCAL_SINK 1
#  endif
#  if __has_include("../utils/profiler.hpp")
#    include "../utils/profiler.hpp"
#    define LITHE_HAS_PROFILER 1
#  endif
#endif
#ifndef LITHE_HAS_NADI
#  define LITHE_HAS_NADI 0
#endif
#ifndef LITHE_HAS_THREAD_LOCAL_SINK
#  define LITHE_HAS_THREAD_LOCAL_SINK 0
#endif
#ifndef LITHE_HAS_PROFILER
#  define LITHE_HAS_PROFILER 0
#endif

namespace lithe::codegen { namespace mir { inline namespace v1 {
        // Physical and virtual register identity types — the stable ABI core.
        // Code built against v1 (or unqualified mir::) names will resolve here
        // via the inline namespace; a future v2 can redefine these alongside
        // without breaking existing plugins.

        struct vreg {
            std::uint32_t id = 0;

            constexpr bool operator==(const vreg& other) const { return id == other.id; }
        };

        struct preg {
            std::uint16_t id = 0;
            std::string name;

            constexpr bool operator==(const preg& other) const { return id == other.id; }
        };
    }} // namespace mir::v1

    // Backward-compatibility aliases: existing code using lithe::codegen::vreg /
    // lithe::codegen::preg continues to compile without modification.
    using mir::vreg;
    using mir::preg;

    struct ssa_value_id {
        std::uint64_t id = 0;

        constexpr bool operator==(const ssa_value_id& other) const { return id == other.id; }
        [[nodiscard]] constexpr bool valid() const { return id != 0; }
    };

    struct spill_slot {
        std::uint32_t id = 0;
        std::uint32_t size = 8;
        std::uint32_t alignment = 8;
        std::int64_t frame_offset = 0;
    };

    enum class memory_address_kind : std::uint8_t {
        stack_frame,
        spill_slot,
        global_symbol,
        argument_slot,
        return_slot,
        computed_address
    };

    struct argument_index {
        std::uint32_t value = 0;

        constexpr bool operator==(const argument_index& other) const { return value == other.value; }
    };

    enum class argument_passing_kind : std::uint8_t {
        register_value,
        register_reference,
        stack_value,
        stack_reference,
        ignored
    };

    enum class return_passing_kind : std::uint8_t {
        register_value,
        stack_value,
        void_return
    };

    enum class register_class : std::uint8_t {
        integer,
        floating,
        vector
    };

    struct argument_descriptor {
        std::string name;
        bool floating_point = false;
        argument_index index{};
        register_class reg_class = register_class::integer;
        argument_passing_kind passing_kind = argument_passing_kind::register_value;
        std::optional<preg> physical_register;
        std::optional<std::uint32_t> stack_slot;
        std::uint32_t size = 8;
        std::uint32_t alignment = 8;
    };

    struct return_descriptor {
        return_passing_kind passing_kind = return_passing_kind::register_value;
        register_class reg_class = register_class::integer;
        std::optional<preg> physical_register;
        std::optional<std::uint32_t> stack_slot;
        std::uint32_t size = 8;
        std::uint32_t alignment = 8;
    };

    struct frame_slot {
        enum class kind : std::uint8_t {
            spill,
            saved_register,
            local,
            outgoing_argument
        };

        kind type = kind::spill;
        std::uint32_t id = 0;
        std::uint32_t size = 8;
        std::uint32_t alignment = 8;
        std::int64_t offset = 0;
        std::optional<preg> saved_register;
        register_class reg_class = register_class::integer;
    };

    struct stack_frame {
        std::vector<frame_slot> slots;
        std::uint32_t stack_size = 0;
        std::uint32_t stack_alignment = 16;
    };

    enum class frame_object_kind : std::uint8_t {
        spill_slot,
        argument_slot,
        return_slot,
        local_slot,
        outgoing_call_slot,
        emergency_spill_slot,
        reserved_slot,
        caller_saved_register,
        callee_saved_register
    };

    struct frame_object_id {
        std::uint32_t value = 0;

        [[nodiscard]] constexpr bool valid() const { return value != 0; }
        constexpr bool operator==(const frame_object_id& other) const { return value == other.value; }
    };

    struct memory_address {
        memory_address_kind kind = memory_address_kind::computed_address;
        std::optional<preg> base;
        std::optional<preg> index;
        std::int32_t scale = 1;
        std::int64_t displacement = 0;
        std::optional<frame_object_id> referenced_frame_object;
        std::optional<std::string> referenced_symbol;
    };

    struct memory_operand {
        memory_address address;
    };

    struct frame_object_usage {
        std::unordered_set<std::uint32_t> instruction_ids;
        std::unordered_set<std::uint32_t> block_ids;
        std::unordered_set<std::string> tags;
    };

    struct frame_object_flags {
        bool for_outgoing_calls = false;
        bool reserved_special = false;
        bool emergency = false;
        bool pinned = false;
    };

    struct frame_object_descriptor {
        frame_object_kind kind = frame_object_kind::local_slot;
        std::uint32_t size = 8;
        std::uint32_t alignment = 8;
        std::optional<spill_slot> source_spill;
        std::optional<argument_index> source_argument;
        std::optional<preg> assigned_register;
        frame_object_usage usage;
        frame_object_flags flags;
    };

    struct frame_object {
        frame_object_id id{};
        frame_object_kind kind = frame_object_kind::local_slot;
        std::uint32_t size = 8;
        std::uint32_t alignment = 8;
        std::int64_t offset = 0;
        bool has_assigned_offset = false;
        std::optional<spill_slot> source_spill;
        std::optional<argument_index> source_argument;
        std::optional<preg> assigned_register;
        frame_object_usage usage;
        frame_object_flags flags;
    };

    struct frame_object_layout {
        frame_object_id id{};
        frame_object_kind kind = frame_object_kind::local_slot;
        std::uint32_t size = 0;
        std::uint32_t alignment = 1;
        std::int64_t offset = 0;
        bool has_assigned_offset = false;
    };

    struct stack_frame_layout {
        std::vector<frame_object> objects;
        std::vector<frame_object_layout> object_layouts;
        std::uint32_t frame_size = 0;
        std::uint32_t frame_alignment = 16;
        bool stack_grows_down = true;
        std::uint32_t next_object_id = 1;

        [[nodiscard]] bool empty() const { return objects.empty(); }
    };

    struct calling_convention {
        std::string name = "generic";
        std::vector<preg> integer_argument_registers;
        std::vector<preg> floating_argument_registers;
        preg integer_return_register{0, "rv0"};
        preg floating_return_register{0, "frv0"};
        std::vector<preg> caller_saved;
        std::vector<preg> callee_saved;
        std::vector<preg> scratch_registers;
        bool variadic_supported = false;
        bool callee_cleans_stack = false;
    };

    enum class target_abi_kind : std::uint8_t {
        generic,
        x86_64_system_v,
        aarch64,
        custom
    };

    struct target_abi {
        target_abi_kind kind = target_abi_kind::generic;
        std::string name = "generic-v0";
        calling_convention convention;
        bool variadic_supported = false;
        bool callee_cleans_stack = false;
    };

    struct saved_register {
        preg reg;
        std::optional<frame_object_id> frame_object;
        bool caller_saved = false;
        bool callee_saved = false;
    };

    struct frame_adjustment {
        std::int64_t before_body = 0;
        std::int64_t after_body = 0;
        std::uint32_t alignment = 16;
        bool stack_grows_down = true;
    };

    struct prologue_plan {
        stack_frame frame;
        std::vector<preg> saved_caller_saved;
        bool save_caller_saved = true;
        bool allocate_stack_frame = true;
        stack_frame_layout layout;
        std::vector<saved_register> saved_registers;
        frame_adjustment adjustment;
        std::uint32_t outgoing_argument_area_size = 0;
        std::string return_value_handling = "unknown";
        std::string varargs_handling = "none";
        std::vector<std::string> abi_features_handling;
        std::vector<preg> callee_saved_registers;
        std::vector<frame_object> spill_frame_objects;
        std::uint32_t frame_alignment = 16;
        std::uint32_t total_stack_size = 0;
        std::vector<std::string> argument_locations;
        std::string return_location = "unknown";
        bool emit_instructions = false;
    };

    struct epilogue_plan {
        stack_frame frame;
        std::vector<preg> restore_caller_saved;
        bool deallocate_stack_frame = true;
        bool emit_return = true;
        stack_frame_layout layout;
        std::vector<saved_register> saved_registers;
        frame_adjustment adjustment;
        std::uint32_t outgoing_argument_area_size = 0;
        std::string return_value_handling = "unknown";
        std::string varargs_handling = "none";
        std::vector<std::string> abi_features_handling;
        std::vector<preg> callee_saved_registers;
        std::vector<frame_object> spill_frame_objects;
        std::uint32_t frame_alignment = 16;
        std::uint32_t total_stack_size = 0;
        std::vector<std::string> argument_locations;
        std::string return_location = "unknown";
        bool emit_instructions = false;
    };

    struct function_signature {
        std::string name = "anonymous";
        std::vector<argument_descriptor> arguments;
        calling_convention convention;
        target_abi abi;
        return_descriptor return_value;
        bool variadic = false;
    };

    struct argument_location {
        argument_index index{};
        register_class reg_class = register_class::integer;
        argument_passing_kind passing_kind = argument_passing_kind::ignored;
        std::optional<preg> physical_register;
        std::optional<std::uint32_t> stack_slot;
        std::uint32_t size = 0;
        std::uint32_t alignment = 1;
        bool valid = false;
    };

    struct argument_assignment_metadata {
        std::vector<argument_location> per_argument_locations;
        std::unordered_map<std::uint32_t, argument_location> location_by_instruction_id;
        std::unordered_map<std::uint32_t, argument_location> location_by_vreg_id;
        std::vector<std::string> diagnostics;
        bool validated = false;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    struct return_location {
        register_class reg_class = register_class::integer;
        return_passing_kind passing_kind = return_passing_kind::void_return;
        std::optional<preg> physical_register;
        std::optional<std::uint32_t> stack_slot;
        std::uint32_t size = 0;
        std::uint32_t alignment = 1;
        bool valid = true;
    };

    enum class return_lowering_status_type : std::uint8_t {
        ok,
        error,
        reject,
        lowered,
        unhandled
    };

    enum class return_lowering_diagnostic_type : std::uint8_t {
        invalid_return_descriptor,
        return_descriptor_mismatch,
        stack_return_not_supported,
        register_return_not_supported,
        void_return_with_uses,
        nonvoid_return_without_uses,
        reserved_register_return
    };

    using return_lowering_status = return_lowering_status_type;
    using return_lowering_diagnostic = return_lowering_diagnostic_type;
    using return_lowering_result_type = return_location;

    inline constexpr return_lowering_status_type return_lowering_status_ok = return_lowering_status_type::ok;
    inline constexpr return_lowering_status_type return_lowering_status_error = return_lowering_status_type::error;
    inline constexpr return_lowering_status_type return_lowering_status_reject = return_lowering_status_type::reject;
    inline constexpr return_lowering_status_type return_lowering_status_lowered = return_lowering_status_type::lowered;
    inline constexpr return_lowering_status_type return_lowering_status_unhandled =
        return_lowering_status_type::unhandled;

    inline constexpr return_lowering_diagnostic_type return_lowering_diagnostic_invalid_return_descriptor =
        return_lowering_diagnostic_type::invalid_return_descriptor;
    inline constexpr return_lowering_diagnostic_type return_lowering_diagnostic_return_descriptor_mismatch =
        return_lowering_diagnostic_type::return_descriptor_mismatch;
    inline constexpr return_lowering_diagnostic_type return_lowering_diagnostic_stack_return_not_supported =
        return_lowering_diagnostic_type::stack_return_not_supported;
    inline constexpr return_lowering_diagnostic_type return_lowering_diagnostic_register_return_not_supported =
        return_lowering_diagnostic_type::register_return_not_supported;
    inline constexpr return_lowering_diagnostic_type return_lowering_diagnostic_void_return_with_uses =
        return_lowering_diagnostic_type::void_return_with_uses;
    inline constexpr return_lowering_diagnostic_type return_lowering_diagnostic_nonvoid_return_without_uses =
        return_lowering_diagnostic_type::nonvoid_return_without_uses;
    inline constexpr return_lowering_diagnostic_type return_lowering_diagnostic_reserved_register_return =
        return_lowering_diagnostic_type::reserved_register_return;

    struct return_lowering_result {
        return_lowering_status_type status = return_lowering_status_unhandled;
        return_lowering_result_type location{};
        std::vector<return_lowering_diagnostic_type> diagnostic_types;
        std::vector<std::string> diagnostics;
        std::vector<std::uint32_t> return_instruction_ids;
        bool lowered = false;

        [[nodiscard]] bool ok() const {
            return status == return_lowering_status_ok || status == return_lowering_status_lowered;
        }
    };

    struct virtual_register_abi_binding {
        vreg virtual_register{};
        argument_index index{};
        argument_location location;
        std::optional<preg> physical_register;
        std::optional<spill_slot> stack_slot;
    };

    struct virtual_register_abi_mapping_result {
        std::vector<virtual_register_abi_binding> bindings;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    struct instruction_number {
        std::uint64_t global_index = 0;
        std::uint32_t block_id = 0;
        std::uint32_t block_local_index = 0;
        std::uint64_t numbering_epoch = 0;

        [[nodiscard]] constexpr bool valid() const { return global_index != 0; }
    };

    enum class opcode : std::uint8_t {
        nop,
        mov,
        load_imm,
        load_symbol,
        load_arg,
        load,
        store,
        store_spill,
        load_spill,
        add,
        sub,
        mul,
        div,
        mod,
        neg,
        cmp_eq,
        cmp_ne,
        cmp_lt,
        cmp_le,
        cmp_gt,
        cmp_ge,
        bit_and,
        bit_or,
        bit_xor,
        bit_not,
        shl,
        shr,
        logical_and,
        logical_or,
        logical_not,
        call,
        branch,
        branch_cond,
        ret,
        // Aggregate / OO memory operations (MIR Phase 2)
        get_element_ptr,
        extract_value,
        insert_value,
        indirect_call,
        // ── Floating-point arithmetic (f64) ────────────────────────────────
        fadd, // f64 addition
        fsub, // f64 subtraction
        fmul, // f64 multiplication
        fdiv, // f64 division (IEEE 754 — no guard needed)
        fneg, // f64 negation
        // ── Floating-point memory ──────────────────────────────────────────
        fload, // load f64 from [base + imm_offset]
        fstore, // store f64 to [base + imm_offset]
        fload_imm, // load f64 immediate (immediate_f64 operand)
        // ── FP↔GPR bitcast bridges ────────────────────────────────────────
        gpr_to_fp, // FMOV Dd, Xn  — move GPR bits to FP register
        fp_to_gpr, // FMOV Xd, Dn  — move FP bits to GPR register
        // ── Floating-point comparisons ────────────────────────────────────
        fcmp_eq, // f64 ordered equal
        fcmp_ne, // f64 ordered not-equal
        fcmp_lt, // f64 ordered less-than
        fcmp_le, // f64 ordered less-or-equal
        fcmp_gt, // f64 ordered greater-than
        fcmp_ge // f64 ordered greater-or-equal
    };

    // -------------------------------------------------------------------------
    // Extensible MIR operation algebra — Phase 1
    // Decoupled from the opcode enum; allows custom domains without modifying
    // the core instruction set.
    // -------------------------------------------------------------------------

    struct operation_id {
        std::string domain;
        std::string name;
        std::uint64_t stable_hash = 0;

        [[nodiscard]] bool operator==(const operation_id& other) const noexcept {
            return domain == other.domain && name == other.name;
        }

        [[nodiscard]] bool operator!=(const operation_id& other) const noexcept {
            return !(*this == other);
        }
    };

    enum class operation_trait : std::uint32_t {
        pure = 1u << 0,
        deterministic = 1u << 1,
        commutative = 1u << 2,
        associative = 1u << 3,
        idempotent = 1u << 4,
        reads_memory = 1u << 5,
        writes_memory = 1u << 6,
        allocates_memory = 1u << 7,
        control_flow = 1u << 8,
        terminator = 1u << 9,
        call_like = 1u << 10,
        may_throw = 1u << 11,
        may_trap = 1u << 12,
        has_side_effects = 1u << 13,
        vectorizable = 1u << 14,
        parallelizable = 1u << 15,
        differentiable = 1u << 16,
        symbolic = 1u << 17,
        target_specific = 1u << 18,
        instrumentation = 1u << 19,
        lowering_only = 1u << 20,
    };

    using operation_trait_set = std::uint32_t;

    [[nodiscard]] constexpr operation_trait_set make_trait_set() noexcept { return 0u; }

    [[nodiscard]] constexpr operation_trait_set add_trait(const operation_trait_set s, operation_trait t) noexcept {
        return s | static_cast<std::uint32_t>(t);
    }

    [[nodiscard]] constexpr bool has_trait(const operation_trait_set s, operation_trait t) noexcept {
        return (s & static_cast<std::uint32_t>(t)) != 0u;
    }

    enum class abstract_value_kind : std::uint8_t {
        unknown,
        scalar,
        integer,
        floating,
        pointer,
        memory,
        vector,
        tensor,
        aggregate,
        predicate,
        token,
        graph,
        layout,
        query,
        symbolic,
    };

    struct abstract_value_type {
        abstract_value_kind kind = abstract_value_kind::unknown;
        std::uint32_t bit_width = 0;
        std::uint32_t lane_count = 1;
        // Shape dimensions for tensor/layout/query types.
        // A dimension value of 0 is used as a dynamic/"?" marker.
        // Empty for scalar kinds.
        std::vector<std::uint32_t> shape;
        // Optional domain-specific type name, e.g. "f32", "layout_box",
        // "query_row", "symbolic_expr".
        std::string semantic_type;
        // Arbitrary key-value metadata for domain-specific extensions.
        std::unordered_map<std::string, std::string> attributes;
    };

    struct operation_contract {
        std::vector<abstract_value_type> operands;
        std::vector<abstract_value_type> results;
        operation_trait_set required_traits = make_trait_set();
        operation_trait_set forbidden_traits = make_trait_set();
    };
} // namespace lithe::codegen

// std::hash for operation_id must be visible before unordered_map<operation_id, ...>
// is instantiated inside operation_registry.
namespace std {
    template <>
    struct hash<lithe::codegen::operation_id> {
        [[nodiscard]] std::size_t operator()(const lithe::codegen::operation_id& id) const noexcept {
            std::size_t h = std::hash<std::string>{}(id.domain);
            h ^= std::hash<std::string>{}(id.name) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
} // namespace std

namespace lithe::codegen {
    struct operation_descriptor {
        operation_id id;
        operation_contract contract;
        operation_trait_set traits = make_trait_set();
        std::unordered_map<std::string, std::string> attributes;
        std::string documentation;
    };

    class operation_registry {
    public:
        void register_operation(operation_descriptor desc) {
            entries_.emplace(desc.id, std::move(desc));
        }

        [[nodiscard]] bool contains(const operation_id& id) const noexcept {
            return entries_.count(id) != 0;
        }

        [[nodiscard]] const operation_descriptor* find(const operation_id& id) const noexcept {
            auto it = entries_.find(id);
            return it != entries_.end() ? &it->second : nullptr;
        }

        [[nodiscard]] operation_trait_set traits_of(const operation_id& id) const noexcept {
            const auto* d = find(id);
            return d ? d->traits : make_trait_set();
        }

        [[nodiscard]] const operation_contract* contract_of(const operation_id& id) const noexcept {
            const auto* d = find(id);
            return d ? &d->contract : nullptr;
        }

    private:
        std::unordered_map<operation_id, operation_descriptor> entries_;
    };

    // -------------------------------------------------------------------------

    struct operand {
        enum class kind : std::uint8_t {
            none,
            vreg,
            ssa_value,
            preg,
            argument_index,
            immediate_i64,
            immediate_f64,
            symbol,
            spill,
            memory,
            block
        };

        kind type = kind::none;
        std::variant<std::monostate, vreg, ssa_value_id, preg, std::int64_t, double, std::string, spill_slot,
                     memory_operand, std::uint32_t> value;

        [[nodiscard]] static operand as_vreg(vreg reg) { return operand{kind::vreg, reg}; }
        [[nodiscard]] static operand as_ssa_value(ssa_value_id value_id) { return operand{kind::ssa_value, value_id}; }
        [[nodiscard]] static operand as_preg(preg reg) { return operand{kind::preg, std::move(reg)}; }

        [[nodiscard]] static operand as_argument_index(std::uint32_t index) {
            return operand{kind::argument_index, index};
        }

        [[nodiscard]] static operand as_i64(std::int64_t imm) { return operand{kind::immediate_i64, imm}; }
        [[nodiscard]] static operand as_f64(double imm) { return operand{kind::immediate_f64, imm}; }
        [[nodiscard]] static operand as_symbol(std::string sym) { return operand{kind::symbol, std::move(sym)}; }
        [[nodiscard]] static operand as_spill(spill_slot slot) { return operand{kind::spill, slot}; }
        [[nodiscard]] static operand as_memory(memory_operand mem) { return operand{kind::memory, std::move(mem)}; }

        [[nodiscard]] static operand as_memory(memory_address address) {
            return operand{kind::memory, memory_operand{std::move(address)}};
        }

        [[nodiscard]] static operand as_block(std::uint32_t block_id) { return operand{kind::block, block_id}; }
    };

    enum class scheduling_dependency_kind : std::uint8_t {
        data,
        control,
        memory,
        phi,
        artificial
    };

    enum class scheduling_hazard_flag : std::uint8_t {
        none,
        read_after_write,
        write_after_read,
        write_after_write,
        memory_alias,
        call_barrier,
        branch_barrier
    };

    struct scheduling_dependency_edge {
        std::uint32_t source_instruction_id = 0;
        std::uint32_t target_instruction_id = 0;
        scheduling_dependency_kind kind = scheduling_dependency_kind::data;
        std::size_t latency = 0;
        bool loop_carried = false;
        std::optional<std::uint32_t> block_id;
    };

    struct scheduling_constraint {
        std::string name;
        std::string value;
    };

    struct instruction_scheduling_metadata {
        std::size_t latency = 1;
        double throughput = 1.0;
        std::string scheduling_class = "generic";
        std::vector<scheduling_hazard_flag> hazard_flags;
        std::vector<std::string> scheduling_groups;
        std::int32_t scheduling_priority = 0;
        std::vector<scheduling_constraint> scheduling_constraints;
        std::unordered_map<std::string, std::string> annotations;
        std::vector<std::uint32_t> dependency_predecessors;
        std::vector<std::uint32_t> dependency_successors;
    };

    struct instruction {
        std::uint32_t id = 0;
        std::optional<instruction_number> number;
        opcode op = opcode::nop;
        std::vector<operand> defs;
        std::vector<operand> uses;
        std::vector<ssa_value_id> ssa_defs;
        std::vector<ssa_value_id> ssa_uses;
        std::optional<std::string> comment;
        instruction_scheduling_metadata scheduling;
        std::optional<operation_id> abstract_operation;
        std::unordered_map<std::string, std::string> operation_attributes;
    };

    // -------------------------------------------------------------------------
    // Abstract operation helpers
    // -------------------------------------------------------------------------

    [[nodiscard]] inline operation_id make_operation_id(std::string domain, std::string name) noexcept {
        return operation_id{std::move(domain), std::move(name)};
    }

    [[nodiscard]] inline const operation_id& legacy_opcode_operation_id(const opcode op) {
        // Static array: one operation_id per opcode — zero allocation after program init.
        static const std::string core{"lithe.core"};
        static const operation_id table[] = {
            {core, "nop"}, {core, "mov"}, {core, "load_imm"},
            {core, "load_symbol"}, {core, "load_arg"}, {core, "load"},
            {core, "store"}, {core, "store_spill"}, {core, "load_spill"},
            {core, "add"}, {core, "sub"}, {core, "mul"},
            {core, "div"}, {core, "mod"}, {core, "neg"},
            {core, "cmp_eq"}, {core, "cmp_ne"}, {core, "cmp_lt"},
            {core, "cmp_le"}, {core, "cmp_gt"}, {core, "cmp_ge"},
            {core, "bit_and"}, {core, "bit_or"}, {core, "bit_xor"},
            {core, "bit_not"}, {core, "shl"}, {core, "shr"},
            {core, "logical_and"}, {core, "logical_or"}, {core, "logical_not"},
            {core, "call"}, {core, "branch"}, {core, "branch_cond"},
            {core, "ret"}, {core, "get_element_ptr"}, {core, "extract_value"},
            {core, "insert_value"}, {core, "indirect_call"},
            {core, "fadd"}, {core, "fsub"}, {core, "fmul"}, {core, "fdiv"}, {core, "fneg"},
            {core, "fload"}, {core, "fstore"}, {core, "fload_imm"},
            {core, "gpr_to_fp"}, {core, "fp_to_gpr"},
            {core, "fcmp_eq"}, {core, "fcmp_ne"}, {core, "fcmp_lt"},
            {core, "fcmp_le"}, {core, "fcmp_gt"}, {core, "fcmp_ge"},
        };
        static const operation_id unknown{core, "unknown"};
        const auto idx = static_cast<std::size_t>(op);
        return idx < std::size(table) ? table[idx] : unknown;
    }

    [[nodiscard]] inline bool has_abstract_operation(const instruction& instr) noexcept {
        return instr.abstract_operation.has_value();
    }

    [[nodiscard]] inline operation_id effective_operation(const instruction& instr) {
        if (instr.abstract_operation) return *instr.abstract_operation;
        return legacy_opcode_operation_id(instr.op);
    }

    [[nodiscard]] inline operation_trait_set effective_traits(
        const instruction& instr,
        const operation_registry* reg) noexcept {
        if (reg) {
            if (const auto* d = reg->find(effective_operation(instr))) return d->traits;
        }
        return make_trait_set();
    }

    [[nodiscard]] inline bool has_operation_trait(
        const instruction& instr,
        const operation_trait trait,
        const operation_registry* reg) noexcept {
        return has_trait(effective_traits(instr, reg), trait);
    }

    // -------------------------------------------------------------------------

    struct block_argument {
        ssa_value_id value;
        std::optional<std::string> name;
    };

    struct phi_incoming {
        std::uint32_t predecessor_block = 0;
        ssa_value_id value;
    };

    struct phi_placeholder {
        ssa_value_id result;
        std::vector<phi_incoming> incoming;
        std::optional<std::string> note;
    };

    // -----------------------------------------------------------------------
    //
    // These are an *optional* adapter layer built on top of the core MIR.
    // They are produced by construct_ssa() and consumed by SSA-based passes.
    // They do NOT modify allocated_instruction / allocated_basic_block.
    // -----------------------------------------------------------------------

    // A single SSA definition: a versioned binding of a physical register.
    // `preg_id`   — the original MIR preg being versioned.
    // `version`   — monotonically increasing version counter (1-based; 0 = undef).
    // `value_id`  — the globally unique ssa_value_id assigned by function_ir::make_ssa_value().
    // `def_block` — block in which this value is defined (0 = live-in / function arg).
    // `def_inst`  — instruction id of the defining instruction (0 for phi results).
    struct ssa_value {
        std::uint16_t preg_id = 0;
        std::uint32_t version = 0;
        ssa_value_id value_id = {};
        std::uint32_t def_block = 0;
        std::uint32_t def_inst = 0;

        [[nodiscard]] bool valid() const noexcept { return value_id.valid(); }

        [[nodiscard]] bool operator==(const ssa_value& o) const noexcept {
            return value_id == o.value_id;
        }
    };

    // A completed SSA phi node placed at the entry of a basic block.
    // `result`       — the SSA value produced by this phi.
    // `preg_id`      — the original MIR preg this phi versions.
    // `block_id`     — the block at whose entry this phi is placed.
    // `incoming`     — one entry per CFG predecessor: (predecessor_block_id, ssa_value).
    struct phi_node {
        ssa_value result = {};
        std::uint16_t preg_id = 0;
        std::uint32_t block_id = 0;
        std::vector<std::pair<std::uint32_t, // predecessor block id
                              ssa_value>> incoming = {};

        [[nodiscard]] bool complete() const noexcept {
            return result.valid() && !incoming.empty();
        }
    };

    // Per-block SSA construction bookkeeping.
    // Produced by construct_ssa(); one entry per block in the function.
    //
    // `block_id`      — block this state belongs to.
    // `phi_nodes`     — phi nodes placed at this block's entry.
    // `reaching`      — map from preg_id → current reaching ssa_value at the
    //                   *end* of the block (used to fill phi incoming edges).
    // `defined_here`  — set of preg_ids that have at least one def in this block
    //                   (used during phi-placement to determine which pregs need
    //                   phis in the block's dominance-frontier successors).
    struct ssa_block_state {
        std::uint32_t block_id = 0;
        std::vector<phi_node> phi_nodes = {};
        std::unordered_map<std::uint16_t, ssa_value> reaching = {};
        std::unordered_set<std::uint16_t> defined_here = {};
    };

    namespace mir::inline v1 {
        struct basic_block {
            std::uint32_t id = 0;
            std::string name;
            std::vector<block_argument> arguments;
            std::vector<phi_placeholder> phi_placeholders;
            std::vector<instruction> instructions;
            std::vector<std::uint32_t> predecessors;
            std::vector<std::uint32_t> successors;
        };
    } // namespace mir::v1

    using mir::basic_block;

    struct machine_cfg {
        std::uint32_t entry_block = 0;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> successors;
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> predecessors;
    };

    struct control_flow_edge {
        std::uint32_t from = 0;
        std::uint32_t to = 0;
    };

    struct basic_block_info {
        std::uint32_t id = 0;
        std::vector<std::uint32_t> predecessors;
        std::vector<std::uint32_t> successors;
        bool is_entry = false;
        bool is_exit = false;
        bool reachable = false;
    };

    struct dominance_frontier_placeholder {
        bool available = false;
        std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> frontier_by_block;
    };

    struct function_ir;

    enum class codegen_hook_phase : std::uint8_t {
        ssa_prepare,
        ssa_construct,
        ssa_optimize,
        dominance_prepare,
        dominance_optimize,
        register_allocate,
        graph_coloring_prepare,
        bytecode_prepare,
        jit_prepare,
        async_runtime_prepare,
        coroutine_prepare,
        mlir_prepare,
        dialect_conversion_prepare
    };

    struct codegen_hook_context {
        codegen_hook_phase phase = codegen_hook_phase::ssa_prepare;
        std::string_view pass_name;
    };

    struct function_ir;

    using codegen_hook_fn = void(*)(function_ir&, const codegen_hook_context&, void*);

    struct codegen_hook {
        codegen_hook_fn fn = nullptr;
        void* context = nullptr;
        std::function<void(function_ir&, const codegen_hook_context&)> closure;

        void operator()(function_ir& ir, const codegen_hook_context& ctx) const {
            if (fn) {
                fn(ir, ctx, context);
            }
            else if (closure) {
                closure(ir, ctx);
            }
        }

        static codegen_hook from_fn(codegen_hook_fn f, void* ctx = nullptr) {
            return {f, ctx, {}};
        }

        template <class Fn>
        static codegen_hook from_closure(Fn&& f) {
            return {nullptr, nullptr, std::forward<Fn>(f)};
        }
    };

    struct codegen_extension_hooks {
        std::unordered_map<codegen_hook_phase, std::vector<codegen_hook>> callbacks;

        void add(const codegen_hook_phase phase, codegen_hook hook) {
            callbacks[phase].push_back(std::move(hook));
        }

        void add(const codegen_hook_phase phase, codegen_hook_fn fn, void* ctx = nullptr) {
            callbacks[phase].push_back(codegen_hook::from_fn(fn, ctx));
        }

        template <class Fn>
            requires std::invocable<Fn, function_ir&, const codegen_hook_context&>
        void add(const codegen_hook_phase phase, Fn&& fn) {
            callbacks[phase].push_back(codegen_hook::from_closure(std::forward<Fn>(fn)));
        }

        void run(function_ir& fn, const codegen_hook_phase phase, const std::string_view pass_name = {}) const {
            if (const auto it = callbacks.find(phase); it != callbacks.end()) {
                codegen_hook_context ctx{phase, pass_name};
                for (const auto& hook : it->second) {
                    hook(fn, ctx);
                }
            }
        }
    };

    struct register_assignment {
        std::optional<preg> physical;
        std::optional<spill_slot> spill;

        [[nodiscard]] bool spilled() const { return spill.has_value(); }
    };

    struct function_ir {
        std::string name;
        std::vector<basic_block> blocks;
        machine_cfg cfg;
        std::uint32_t next_vreg_id = 1;
        std::uint64_t next_ssa_value_id = 1;
        std::uint32_t next_inst_id = 1;
        std::uint64_t next_instruction_number_global = 1;
        std::uint64_t numbering_epoch = 1;
        std::uint32_t next_spill_id = 1;
        std::vector<spill_slot> spill_slots;
        std::unordered_map<std::uint32_t, register_assignment> assignments;
        dominance_frontier_placeholder dominance_frontier;
        codegen_extension_hooks hooks;

    private:
        std::unordered_map<std::uint32_t, std::size_t> block_index_;

    public:
        [[nodiscard]] vreg make_vreg() { return vreg{next_vreg_id++}; }

        [[nodiscard]] ssa_value_id make_ssa_value() { return ssa_value_id{next_ssa_value_id++}; }

        [[nodiscard]] basic_block& create_block(std::string block_name = {}) {
            const auto id = static_cast<std::uint32_t>(blocks.size() + 1);
            if (block_name.empty()) {
                block_name = "bb" + std::to_string(id);
            }
            block_index_[id] = blocks.size();
            blocks.push_back(basic_block{id, std::move(block_name), {}, {}, {}, {}, {}});
            cfg.successors[id] = {};
            cfg.predecessors[id] = {};
            if (cfg.entry_block == 0) {
                cfg.entry_block = id;
            }
            return blocks.back();
        }

        [[nodiscard]] basic_block* find_block(const std::uint32_t block_id) {
            if (const auto it = block_index_.find(block_id); it != block_index_.end() && it->second < blocks.size()) {
                return std::addressof(blocks[it->second]);
            }
            return nullptr;
        }

        instruction& emit(const std::uint32_t block_id, instruction inst) {
            auto* block = find_block(block_id);
            if (block == nullptr) {
                block = std::addressof(create_block("bb" + std::to_string(block_id)));
            }
            if (inst.id == 0) {
                inst.id = next_inst_id++;
            }
            block->instructions.push_back(std::move(inst));
            auto& stored = block->instructions.back();
            stored.number = instruction_number{
                next_instruction_number_global++,
                block->id,
                static_cast<std::uint32_t>(block->instructions.size() - 1),
                numbering_epoch
            };
            return stored;
        }

        void add_edge(const std::uint32_t from, const std::uint32_t to) {
            auto* from_block = find_block(from);
            auto* to_block = find_block(to);
            if (from_block == nullptr || to_block == nullptr) {
                return;
            }
            from_block->successors.push_back(to);
            to_block->predecessors.push_back(from);
            cfg.successors[from].push_back(to);
            cfg.predecessors[to].push_back(from);
        }

        void renumber_instructions(const std::optional<std::uint64_t> epoch = std::nullopt) {
            numbering_epoch = epoch.has_value() ? *epoch : (numbering_epoch + 1);
            next_instruction_number_global = 1;
            for (auto& block : blocks) {
                for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                    auto& inst = block.instructions[i];
                    if (inst.id == 0) {
                        inst.id = next_inst_id++;
                    }
                    inst.number = instruction_number{
                        next_instruction_number_global++,
                        block.id,
                        static_cast<std::uint32_t>(i),
                        numbering_epoch
                    };
                }
            }
        }

        void run_hook(const codegen_hook_phase phase, const std::string_view pass_name = {}) {
            hooks.run(*this, phase, pass_name);
        }
    };

    namespace observability {
        inline constexpr bool enabled_by_default = false;

        struct compilation_event {
            enum class kind : std::uint8_t {
                started,
                finished,
                failed
            };

            kind type = kind::started;
            std::string phase;
            std::uint64_t timestamp_ns = 0;
        };

        struct codegen_event {
            std::string stage;
            std::uint64_t start_ns = 0;
            std::uint64_t end_ns = 0;
            structural_hash_t ir_hash = 0;
            structural_hash_t structural_hash = 0;
        };

        struct rewrite_event {
            std::string pass_name;
            std::size_t rewrites_attempted = 0;
            std::size_t rewrites_applied = 0;
            std::uint64_t timestamp_ns = 0;
        };

        struct structural_hash_event {
            std::string label;
            structural_hash_t expression_hash = 0;
            structural_hash_t structural_hash = 0;
            std::uint64_t timestamp_ns = 0;
        };

        struct codegen_diagnostic_event {
            std::string stage;
            std::string message;
            std::uint64_t timestamp_ns = 0;
        };

        struct compile_trace {
            std::vector<compilation_event> compilation_events;
            std::vector<codegen_event> codegen_events;
            std::vector<rewrite_event> rewrite_events;
            std::vector<structural_hash_event> structural_hash_events;
            std::vector<codegen_diagnostic_event> diagnostic_events;
        };

        struct null_observer {
            template <class Event>
            constexpr void on_event(const Event&) const noexcept {}
        };

        struct trace_observer {
            compile_trace trace;

            void on_event(const compilation_event& event) { trace.compilation_events.push_back(event); }
            void on_event(const codegen_event& event) { trace.codegen_events.push_back(event); }
            void on_event(const rewrite_event& event) { trace.rewrite_events.push_back(event); }
            void on_event(const structural_hash_event& event) { trace.structural_hash_events.push_back(event); }
            void on_event(const codegen_diagnostic_event& event) { trace.diagnostic_events.push_back(event); }
        };

        [[nodiscard]] inline std::uint64_t now_ns() noexcept {
#if LITHE_HAS_NADI
            return utils::nadi::SteadyClockPolicy::now();
#else
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
        }

        [[nodiscard]] inline structural_hash_t hash_text(const std::string_view text) {
            return std::hash<std::string_view>{}(text);
        }

        template <bool Enabled, class Observer, class Event>
        constexpr void emit(Observer& observer, Event event) {
            if constexpr (Enabled) {
                if constexpr (requires(Observer obs, Event e) { obs.on_event(e); }) {
                    observer.on_event(std::move(event));
                }
            }
            else {
                (void)observer;
                (void)event;
            }
        }
    } // namespace observability

    struct block_use_def {
        std::unordered_set<std::uint32_t> uses;
        std::unordered_set<std::uint32_t> defs;
    };

    struct use_def_analysis {
        std::unordered_map<std::uint32_t, block_use_def> per_block;
    };

    struct block_liveness {
        std::unordered_set<std::uint32_t> live_in;
        std::unordered_set<std::uint32_t> live_out;
    };

    struct instruction_liveness {
        std::uint32_t instruction_id = 0;
        std::unordered_set<std::uint32_t> live_in;
        std::unordered_set<std::uint32_t> live_out;
    };

    struct liveness_analysis {
        std::unordered_map<std::uint32_t, block_liveness> per_block;
        std::unordered_map<std::uint32_t, std::vector<instruction_liveness>> per_instruction;
    };

    enum class constant_kind : std::uint8_t {
        unknown,
        integer,
        floating_point,
        boolean
    };

    struct constant_value {
        constant_kind kind = constant_kind::unknown;
        std::int64_t integer_value = 0;
        double floating_value = 0.0;
        bool boolean_value = false;

        [[nodiscard]] static constant_value unknown() {
            return constant_value{};
        }

        [[nodiscard]] static constant_value integer(const std::int64_t v) {
            constant_value cv;
            cv.kind = constant_kind::integer;
            cv.integer_value = v;
            return cv;
        }

        [[nodiscard]] static constant_value floating(const double v) {
            constant_value cv;
            cv.kind = constant_kind::floating_point;
            cv.floating_value = v;
            return cv;
        }

        [[nodiscard]] static constant_value boolean(const bool v) {
            constant_value cv;
            cv.kind = constant_kind::boolean;
            cv.boolean_value = v;
            return cv;
        }
    };

    struct constant_state {
        std::unordered_map<std::uint32_t, constant_value> values_by_preg;
    };

    enum class mir_analysis_kind : std::uint8_t {
        cfg,
        def_use,
        reaching_definitions,
        register_pressure,
        value_flow,
        dominators,
        loop_analysis,
    };

    struct mir_pass_dependency {
        std::unordered_set<mir_analysis_kind> required;
    };

    struct mir_pass_invalidation {
        std::unordered_set<mir_analysis_kind> invalidated;
        std::unordered_set<mir_analysis_kind> preserved;
    };

    struct live_interval {
        std::uint32_t vreg_id = 0;
        std::size_t start = 0;
        std::size_t end = 0;
    };

    struct register_allocation {
        std::unordered_map<std::uint32_t, register_assignment> assignments;
        std::vector<spill_slot> spill_slots;
    };

    struct register_pressure_hotspot {
        std::uint32_t block_id = 0;
        std::uint32_t instruction_id = 0;
        std::size_t live_registers = 0;
    };

    struct block_register_pressure {
        std::uint32_t block_id = 0;
        std::size_t max_live_registers = 0;
        std::unordered_map<std::uint32_t, std::size_t> live_registers_by_instruction;
    };

    struct block_live_range {
        std::uint32_t block_id = 0;
        std::uint32_t vreg_id = 0;
        std::uint32_t start_instruction_id = 0;
        std::uint32_t end_instruction_id = 0;
        std::size_t span = 0;
    };

    struct register_pressure_thresholds {
        std::size_t hotspot = 0;
        std::size_t warning = 0;
        std::size_t critical = 0;
        std::size_t spill_candidate = 0;
    };

    struct register_pressure_result {
        std::size_t max_live_registers = 0;
        std::unordered_map<std::uint32_t, block_register_pressure> per_block;
        std::vector<live_interval> live_ranges;
        std::unordered_map<std::uint32_t, std::vector<block_live_range>> live_ranges_per_block;
        std::vector<register_pressure_hotspot> hotspots;
        std::unordered_map<std::string, std::size_t> register_classes_under_pressure;
        std::vector<std::uint32_t> spill_candidates;
        std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> pressure_graph;
        std::size_t hotspot_threshold_used = 0;
        register_pressure_thresholds thresholds_used;
        std::vector<std::string> warnings;
        std::unordered_map<std::string, double> statistics;
        std::vector<std::pair<std::string_view, std::string>> diagnostics;
        std::vector<std::string> visualization_data;
        std::vector<std::string> heuristics;
        std::vector<std::string> reduction_opportunities;
        std::vector<std::string> reduction_suggestions;
        std::vector<std::string> reduction_transformations;
        std::vector<std::string> reduction_rules;
        std::vector<std::string> reduction_patterns;
        std::unordered_map<std::string, double> reduction_costs;
        std::vector<std::string> feedback_for_scheduling;

        [[nodiscard]] bool ok() const { return true; }
    };

    struct scheduling_metadata_validation_result {
        std::vector<std::string> diagnostics;
        std::vector<std::string> warnings;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    struct scheduling_metadata_result {
        std::unordered_map<std::uint32_t, instruction_scheduling_metadata> instruction_metadata;
        std::vector<scheduling_dependency_edge> dependency_edges;
        std::unordered_map<std::string, std::size_t> scheduling_classes;
        std::unordered_map<std::string, std::size_t> hazard_flags;
        std::unordered_map<std::string, double> statistics;
        std::vector<std::string> warnings;
        std::vector<std::string> update_log;
        std::vector<std::pair<std::string_view, std::string>> diagnostics;

        [[nodiscard]] bool ok() const { return true; }
    };

    struct allocated_operand {
        enum class kind : std::uint8_t {
            none,
            preg,
            argument_index,
            immediate_i64,
            immediate_f64,
            symbol,
            spill,
            memory,
            block
        };

        kind type = kind::none;
        std::variant<std::monostate, preg, std::int64_t, double, std::string, spill_slot, memory_operand, std::uint32_t>
        value;

        [[nodiscard]] static allocated_operand as_preg(preg reg) {
            return allocated_operand{kind::preg, std::move(reg)};
        }

        [[nodiscard]] static allocated_operand as_argument_index(std::uint32_t index) {
            return allocated_operand{kind::argument_index, index};
        }

        [[nodiscard]] static allocated_operand as_i64(std::int64_t imm) {
            return allocated_operand{kind::immediate_i64, imm};
        }

        [[nodiscard]] static allocated_operand as_f64(double imm) {
            return allocated_operand{kind::immediate_f64, imm};
        }

        [[nodiscard]] static allocated_operand as_symbol(std::string sym) {
            return allocated_operand{kind::symbol, std::move(sym)};
        }

        [[nodiscard]] static allocated_operand as_spill(spill_slot slot) {
            return allocated_operand{kind::spill, slot};
        }

        [[nodiscard]] static allocated_operand as_memory(memory_operand mem) {
            return allocated_operand{kind::memory, std::move(mem)};
        }

        [[nodiscard]] static allocated_operand as_memory(memory_address address) {
            return allocated_operand{kind::memory, memory_operand{std::move(address)}};
        }

        [[nodiscard]] static allocated_operand as_block(std::uint32_t block_id) {
            return allocated_operand{kind::block, block_id};
        }
    };

    namespace mir::inline v1 {
        struct allocated_instruction {
            std::uint32_t id = 0;
            std::optional<instruction_number> number;
            opcode op = opcode::nop;
            std::vector<allocated_operand> defs;
            std::vector<allocated_operand> uses;
            std::vector<ssa_value_id> ssa_defs;
            std::vector<ssa_value_id> ssa_uses;
            std::optional<std::string> comment;
            instruction_scheduling_metadata scheduling;
            std::optional<operation_id> abstract_operation;
            std::unordered_map<std::string, std::string> operation_attributes;
            // Result type for memory/aggregate ops (get_element_ptr, extract_value,
            // insert_value, indirect_call). Uses semantic::types::type_id so the
            // MIR carries full type provenance without requiring a separate side-table.
            std::optional<lithe::semantic::types::type_id> result_type_id;
        };
    } // namespace mir::v1

    using mir::allocated_instruction;

    struct allocated_basic_block {
        std::uint32_t id = 0;
        std::string name;
        std::vector<block_argument> arguments;
        std::vector<phi_placeholder> phi_placeholders;
        std::vector<allocated_instruction> instructions;
        std::vector<std::uint32_t> predecessors;
        std::vector<std::uint32_t> successors;
    };

    struct allocated_function_ir {
        std::string name;
        std::vector<allocated_basic_block> blocks;
        machine_cfg cfg;
        std::unordered_map<std::uint32_t, register_assignment> assignments;
        std::vector<spill_slot> spill_slots;
        function_ir original_vreg_ir;
    };

    struct spill_rewrite_result {
        allocated_function_ir function;
        std::size_t inserted_loads = 0;
        std::size_t inserted_stores = 0;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    // -------------------------------------------------------------------------
    // Abstract operation helpers for allocated_instruction
    // -------------------------------------------------------------------------

    [[nodiscard]] inline bool has_abstract_operation(const allocated_instruction& inst) noexcept {
        return inst.abstract_operation.has_value();
    }

    [[nodiscard]] inline operation_id effective_operation(const allocated_instruction& inst) {
        if (inst.abstract_operation) return *inst.abstract_operation;
        return legacy_opcode_operation_id(inst.op);
    }

    [[nodiscard]] inline operation_trait_set effective_traits(
        const allocated_instruction& inst,
        const operation_registry* reg) noexcept {
        if (reg) {
            if (const auto* d = reg->find(effective_operation(inst))) return d->traits;
        }
        return make_trait_set();
    }

    [[nodiscard]] inline bool has_operation_trait(
        const allocated_instruction& inst,
        const operation_trait trait,
        const operation_registry* reg) noexcept {
        return has_trait(effective_traits(inst, reg), trait);
    }

    // -------------------------------------------------------------------------

    namespace mir::inline v1 {
        struct diagnostic_message {
            enum class severity_level : std::uint8_t {
                error,
                warning
            };

            std::string location;
            std::string text;
            severity_level severity = severity_level::error;
        };

        inline constexpr diagnostic_message::severity_level verification_error =
            diagnostic_message::severity_level::error;
        inline constexpr diagnostic_message::severity_level verification_warning =
            diagnostic_message::severity_level::warning;
        inline constexpr bool verification_success = true;

        enum class phase : std::uint8_t {
            virtual_mir,
            allocated_mir,
            physical_mir
        };

        struct phase_metadata {
            phase current_phase = phase::virtual_mir;
            bool dumps_enabled = true;
            std::optional<std::string> note;
        };

        [[nodiscard]] constexpr const char* to_string(const phase p) {
            switch (p) {
            case phase::virtual_mir: return "virtual";
            case phase::allocated_mir: return "allocated";
            case phase::physical_mir: return "physical";
            }
            return "unknown";
        }

        struct verification_result {
            std::vector<std::string> diagnostics;
            std::vector<diagnostic_message> detailed_messages;

            [[nodiscard]] static verification_result success() {
                return verification_result{};
            }

            [[nodiscard]] static verification_result error(const diagnostic_message& message) {
                verification_result out;
                out.diagnostics.push_back(message.text);
                out.detailed_messages.push_back(message);
                return out;
            }

            [[nodiscard]] static verification_result warning(const diagnostic_message& message) {
                verification_result out;
                out.detailed_messages.push_back(message);
                return out;
            }

            [[nodiscard]] static verification_result combine(
                const verification_result& lhs,
                const verification_result& rhs
            ) {
                verification_result out;
                out.diagnostics.reserve(lhs.diagnostics.size() + rhs.diagnostics.size());
                out.diagnostics.insert(out.diagnostics.end(), lhs.diagnostics.begin(), lhs.diagnostics.end());
                out.diagnostics.insert(out.diagnostics.end(), rhs.diagnostics.begin(), rhs.diagnostics.end());
                out.detailed_messages.reserve(lhs.detailed_messages.size() + rhs.detailed_messages.size());
                out.detailed_messages.insert(
                    out.detailed_messages.end(),
                    lhs.detailed_messages.begin(),
                    lhs.detailed_messages.end()
                );
                out.detailed_messages.insert(
                    out.detailed_messages.end(),
                    rhs.detailed_messages.begin(),
                    rhs.detailed_messages.end()
                );
                return out;
            }

            [[nodiscard]] bool has_errors() const {
                if (!diagnostics.empty()) {
                    return true;
                }
                return std::ranges::any_of(detailed_messages, [](const diagnostic_message& message) {
                    return message.severity == diagnostic_message::severity_level::error;
                });
            }

            [[nodiscard]] bool has_warnings() const {
                return std::ranges::any_of(detailed_messages, [](const diagnostic_message& message) {
                    return message.severity == diagnostic_message::severity_level::warning;
                });
            }

            [[nodiscard]] std::vector<diagnostic_message> messages() const {
                if (!detailed_messages.empty()) {
                    return detailed_messages;
                }

                std::vector<diagnostic_message> out;
                out.reserve(diagnostics.size());
                for (const auto& diag : diagnostics) {
                    out.push_back(diagnostic_message{"", diag, diagnostic_message::severity_level::error});
                }
                return out;
            }

            [[nodiscard]] bool ok() const { return !has_errors(); }
        };

        struct virtual_mir_function {
            function_ir function;
            phase_metadata metadata;
            std::optional<argument_assignment_metadata> argument_abi;

            virtual_mir_function() = default;

            explicit virtual_mir_function(function_ir fn, phase_metadata meta = {})
                : function(std::move(fn)), metadata(std::move(meta)) {
                metadata.current_phase = phase::virtual_mir;
            }
        };

        struct allocated_mir_function {
            allocated_function_ir function;
            phase_metadata metadata;

            allocated_mir_function() = default;

            explicit allocated_mir_function(allocated_function_ir fn, phase_metadata meta = {})
                : function(std::move(fn)), metadata(std::move(meta)) {
                metadata.current_phase = phase::allocated_mir;
            }
        };

        // ---------------------------------------------------------------------
        // Stack-map artifact: records the set of live vreg IDs at each
        // safepoint (async_fork edge / yield instruction).  Flat vectors are
        // used throughout to preserve cache locality.
        // ---------------------------------------------------------------------

        struct stack_map_entry {
            std::uint32_t block_id = 0; // block containing the safepoint
            std::uint32_t instruction_id = 0; // instruction index of the safepoint
            std::vector<std::uint32_t> live_vregs; // live vreg IDs at this point
        };

        struct stack_map_artifact {
            std::vector<stack_map_entry> entries;

            [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
        };

        struct physical_mir_function {
            allocated_function_ir function;
            std::size_t inserted_loads = 0;
            std::size_t inserted_stores = 0;
            std::vector<std::string> diagnostics;
            bool spill_rewritten = true;
            bool verified = false;
            std::vector<std::string> verification_diagnostics;
            std::unordered_set<std::uint32_t> instruction_ids;
            std::unordered_set<std::uint32_t> referenced_virtual_registers;
            std::unordered_set<std::uint32_t> referenced_spill_slots;
            std::optional<function_signature> signature;
            std::optional<target_abi> abi;
            std::optional<stack_frame_layout> frame_layout;
            std::optional<prologue_plan> prologue;
            std::optional<epilogue_plan> epilogue;
            std::optional<stack_map_artifact> stack_map; // populated by safepoint_injection_pass
            phase_metadata metadata;

            physical_mir_function() = default;

            physical_mir_function(
                allocated_function_ir fn,
                const std::size_t loads,
                const std::size_t stores,
                std::vector<std::string> diags,
                phase_metadata meta = {}
            )
                : function(std::move(fn)),
                  inserted_loads(loads),
                  inserted_stores(stores),
                  diagnostics(std::move(diags)),
                  metadata(std::move(meta)) {
                metadata.current_phase = phase::physical_mir;
            }
        };

        [[nodiscard]] inline phase phase_of(const virtual_mir_function&) { return phase::virtual_mir; }
        [[nodiscard]] inline phase phase_of(const allocated_mir_function&) { return phase::allocated_mir; }
        [[nodiscard]] inline phase phase_of(const physical_mir_function&) { return phase::physical_mir; }
    } // namespace mir::v1

    [[nodiscard]] mir::verification_result verify_physical_mir(const mir::physical_mir_function& fn);

    struct argument_assignment_result {
        mir::virtual_mir_function function;
        argument_assignment_metadata metadata;

        [[nodiscard]] bool ok() const { return metadata.ok(); }
    };

    struct interpreter_call_frame {
        std::unordered_map<std::uint32_t, std::int64_t> argument_by_index;
        std::unordered_map<std::uint16_t, std::int64_t> register_arguments;
        std::unordered_map<std::uint32_t, std::int64_t> stack_arguments;
        std::vector<std::string> argument_location_debug;
        std::vector<std::string> diagnostics;
        std::optional<stack_frame_layout> frame_layout;
        bool abi_aware = false;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    struct frame_addressing_options {
        bool prefer_frame_pointer_relative = true;
        bool allow_stack_pointer_relative = true;
        bool include_argument_slots = true;
        bool include_local_slots = true;
        bool include_callee_saved_slots = true;
        bool include_caller_saved_slots = true;
        bool include_return_slot = true;
        std::int64_t dynamic_stack_adjustment = 0;
        bool has_variable_sized_objects = false;
        std::uint32_t minimum_alignment = 1;
        preg frame_pointer_register{4090, "frame_base"};
        preg stack_pointer_register{4091, "stack_base"};
        std::optional<std::string> platform_hint;
        std::vector<std::string> platform_feature_flags;
    };

    struct frame_address_lowering_result {
        std::unordered_map<std::uint32_t, memory_operand> by_spill_slot_id;
        std::unordered_map<std::uint32_t, memory_operand> by_argument_index;
        std::unordered_map<std::uint32_t, memory_operand> by_frame_object_id;
        std::optional<memory_operand> return_slot;
        stack_frame_layout layout;
        frame_addressing_options options;
        std::vector<std::string> diagnostics;
        bool used_frame_pointer_relative = false;
        bool used_stack_pointer_relative = false;

        [[nodiscard]] bool ok() const { return diagnostics.empty(); }
    };

    [[nodiscard]] inline std::string dump_machine_ir(
        const function_ir& fn,
        bool include_register_pressure = false,
        std::size_t hotspot_threshold = 0
    );

    [[nodiscard]] inline std::string dump_virtual_mir(const mir::virtual_mir_function& fn);

    [[nodiscard]] inline std::string dump_allocated_mir(const mir::allocated_mir_function& fn);

    [[nodiscard]] inline std::string dump_physical_mir(const mir::physical_mir_function& fn);

    [[nodiscard]] inline std::string dump_mir(const mir::virtual_mir_function& fn);

    [[nodiscard]] inline std::string dump_mir(const mir::allocated_mir_function& fn);

    [[nodiscard]] inline std::string dump_mir(const mir::physical_mir_function& fn);

    [[nodiscard]] inline mir::verification_result verify_virtual_mir(const mir::virtual_mir_function& fn);

    [[nodiscard]] inline mir::verification_result verify_allocated_mir(const mir::allocated_mir_function& fn);

    [[nodiscard]] inline bool contains_virtual_registers(const mir::physical_mir_function& fn);

    [[nodiscard]] inline bool contains_unresolved_spills(const mir::physical_mir_function& fn);

    [[nodiscard]] inline bool has_duplicate_instruction_ids(const mir::physical_mir_function& fn);

    [[nodiscard]] inline stack_frame_layout query_frame_layout(const stack_frame_layout& layout);

    [[nodiscard]] inline std::optional<frame_object> query_frame_object(
        const stack_frame_layout& layout,
        frame_object_id id
    );

    [[nodiscard]] inline frame_object_id reserve_frame_object(
        stack_frame_layout& layout,
        const frame_object_descriptor& descriptor
    );

    [[nodiscard]] inline bool release_frame_object(stack_frame_layout& layout, frame_object_id id);

    [[nodiscard]] inline std::optional<frame_object_usage> get_frame_object_usage(
        const stack_frame_layout& layout,
        frame_object_id id
    );

    [[nodiscard]] inline std::optional<std::int64_t> get_frame_object_offset(
        const stack_frame_layout& layout,
        frame_object_id id
    );

    [[nodiscard]] inline std::optional<preg> get_frame_object_register(
        const stack_frame_layout& layout,
        frame_object_id id
    );

    [[nodiscard]] inline std::optional<frame_object_kind> get_frame_object_kind(
        const stack_frame_layout& layout,
        frame_object_id id
    );

    [[nodiscard]] inline std::optional<std::uint32_t> get_frame_object_size(
        const stack_frame_layout& layout,
        frame_object_id id
    );

    [[nodiscard]] inline std::optional<std::uint32_t> get_frame_object_alignment(
        const stack_frame_layout& layout,
        frame_object_id id
    );

    [[nodiscard]] inline std::uint32_t get_frame_layout_size(const stack_frame_layout& layout);

    [[nodiscard]] inline std::uint32_t get_frame_layout_alignment(const stack_frame_layout& layout);

    [[nodiscard]] inline std::vector<frame_object> get_frame_layout_objects(const stack_frame_layout& layout);

    [[nodiscard]] inline std::optional<frame_object> get_frame_layout_object_by_id(
        const stack_frame_layout& layout,
        frame_object_id id
    );

    [[nodiscard]] inline std::vector<frame_object> get_frame_layout_objects_by_kind(
        const stack_frame_layout& layout,
        frame_object_kind kind
    );

    [[nodiscard]] inline mir::verification_result validate_frame_object_usage(
        const stack_frame_layout& layout,
        frame_object_id id,
        const mir::physical_mir_function& fn
    );

    [[nodiscard]] inline mir::verification_result validate_frame_layout(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    );

    inline void update_frame_layout_for_spills(const mir::physical_mir_function& fn, stack_frame_layout& layout);

    inline void update_frame_layout_for_calls(const mir::physical_mir_function& fn, stack_frame_layout& layout);

    inline void update_frame_layout_for_register_allocation(
        const mir::physical_mir_function& fn,
        stack_frame_layout& layout
    );

    inline void update_frame_layout_for_prologue_epilogue(
        const mir::physical_mir_function& fn,
        stack_frame_layout& layout
    );

    [[nodiscard]] inline stack_frame_layout update_frame_layout_for_spills(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    );

    [[nodiscard]] inline stack_frame_layout update_frame_layout_for_calls(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    );

    [[nodiscard]] inline stack_frame_layout update_frame_layout_for_register_allocation(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    );

    [[nodiscard]] inline stack_frame_layout update_frame_layout_for_prologue_epilogue(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    );

    inline void assign_frame_offsets(stack_frame_layout& layout);

    [[nodiscard]] inline stack_frame_layout assign_frame_offsets(const stack_frame_layout& layout);

    inline void align_frame_layout(stack_frame_layout& layout);

    [[nodiscard]] inline stack_frame_layout align_frame_layout(const stack_frame_layout& layout);

    [[nodiscard]] inline stack_frame_layout compute_stack_frame(const mir::physical_mir_function& fn);

    [[nodiscard]] inline stack_frame_layout compute_frame_layout(const mir::physical_mir_function& fn);

    [[nodiscard]] inline memory_operand make_memory_operand_for_spill_slot(
        const spill_slot& slot,
        const frame_addressing_options& options = {}
    );

    [[nodiscard]] inline memory_operand make_memory_operand_for_frame_object(
        const frame_object& object,
        const frame_addressing_options& options = {}
    );

    [[nodiscard]] inline memory_operand make_frame_pointer_relative_memory(
        memory_address_kind kind,
        std::int64_t displacement,
        std::optional<frame_object_id> referenced_object = std::nullopt,
        std::optional<std::string> referenced_symbol = std::nullopt,
        const frame_addressing_options& options = {}
    );

    [[nodiscard]] inline memory_operand make_stack_pointer_relative_memory(
        memory_address_kind kind,
        std::int64_t displacement,
        std::optional<frame_object_id> referenced_object = std::nullopt,
        std::optional<std::string> referenced_symbol = std::nullopt,
        const frame_addressing_options& options = {}
    );

    [[nodiscard]] inline frame_address_lowering_result lower_frame_objects_to_addresses(
        const mir::physical_mir_function& fn,
        const frame_addressing_options& options = {}
    );

    [[nodiscard]] inline mir::physical_mir_function materialize_frame_addresses(
        const mir::physical_mir_function& fn,
        const frame_address_lowering_result& lowered
    );

    [[nodiscard]] inline mir::physical_mir_function materialize_frame_addresses(
        const mir::physical_mir_function& fn,
        const frame_addressing_options& options = {}
    );

    [[nodiscard]] inline interpreter_call_frame build_interpreter_call_frame(
        const function_signature& signature,
        const std::vector<std::int64_t>& arguments,
        const std::optional<stack_frame_layout>& layout = std::nullopt
    );

    [[nodiscard]] inline std::optional<std::int64_t> get_interpreter_argument_value(
        const interpreter_call_frame& frame,
        std::uint32_t index
    );

    [[nodiscard]] inline std::string dump_interpreter_call_frame(const interpreter_call_frame& frame);

    [[nodiscard]] inline argument_assignment_result lower_arguments_to_abi(
        const mir::virtual_mir_function& fn,
        const function_signature& signature
    );

    [[nodiscard]] inline return_lowering_result lower_return_to_abi(
        const mir::physical_mir_function& fn,
        const return_descriptor& descriptor
    );

    [[nodiscard]] inline argument_location get_argument_location(
        const function_signature& signature,
        argument_index index
    );

    [[nodiscard]] inline argument_location get_argument_location(
        const function_signature& signature,
        std::uint32_t index
    );

    [[nodiscard]] inline return_location get_return_location(const function_signature& signature);

    [[nodiscard]] inline virtual_register_abi_mapping_result map_virtual_registers_to_calling_convention(
        const mir::virtual_mir_function& fn,
        const function_signature& signature
    );

    [[nodiscard]] inline mir::verification_result validate_calling_convention(
        const mir::physical_mir_function& fn,
        const function_signature& signature
    );

    [[nodiscard]] inline mir::verification_result verify_calling_convention(
        const mir::physical_mir_function& fn,
        const function_signature& signature,
        const target_abi& abi
    );

    [[nodiscard]] inline std::vector<preg> default_scratch_pregs() {
        return {{1000, "scratch0"}, {1001, "scratch1"}, {1002, "scratch2"}, {1003, "scratch3"}};
    }

    [[nodiscard]] inline calling_convention default_calling_convention() {
        calling_convention cc;
        cc.name = "generic";
        cc.integer_argument_registers = {{0, "a0"}, {1, "a1"}, {2, "a2"}, {3, "a3"}};
        cc.floating_argument_registers = {{0, "fa0"}, {1, "fa1"}, {2, "fa2"}, {3, "fa3"}};
        cc.integer_return_register = {0, "rv0"};
        cc.floating_return_register = {0, "frv0"};
        cc.caller_saved = {{10, "t0"}, {11, "t1"}, {12, "t2"}, {13, "t3"}};
        cc.callee_saved = {{20, "s0"}, {21, "s1"}, {22, "s2"}, {23, "s3"}};
        cc.scratch_registers = {{1000, "scratch0"}, {1001, "scratch1"}, {1002, "scratch2"}, {1003, "scratch3"}};
        return cc;
    }

    [[nodiscard]] inline target_abi default_target_abi() {
        target_abi abi;
        abi.kind = target_abi_kind::generic;
        abi.name = "generic-v0";
        abi.convention = default_calling_convention();
        abi.variadic_supported = false;
        abi.callee_cleans_stack = false;
        return abi;
    }

    namespace detail {
        [[nodiscard]] inline bool has_codegen_signature(const function_signature& signature) {
            const bool default_like_convention = signature.convention.name == "generic"
                && signature.convention.integer_argument_registers.empty()
                && signature.convention.floating_argument_registers.empty()
                && signature.convention.caller_saved.empty()
                && signature.convention.callee_saved.empty()
                && signature.convention.scratch_registers.empty()
                && signature.convention.integer_return_register.id == 0
                && signature.convention.integer_return_register.name == "rv0"
                && signature.convention.floating_return_register.id == 0
                && signature.convention.floating_return_register.name == "frv0";
            return !signature.arguments.empty()
                || signature.variadic
                || signature.name != "anonymous"
                || signature.return_value.passing_kind != return_passing_kind::register_value
                || !default_like_convention
                || signature.abi.kind != target_abi_kind::generic
                || signature.abi.name != "generic-v0";
        }

        [[nodiscard]] inline bool has_codegen_abi(const target_abi& abi) {
            return abi.kind != target_abi_kind::generic
                || abi.name != "generic-v0"
                || !abi.convention.integer_argument_registers.empty()
                || !abi.convention.floating_argument_registers.empty()
                || !abi.convention.caller_saved.empty()
                || !abi.convention.callee_saved.empty()
                || !abi.convention.scratch_registers.empty()
                || abi.variadic_supported
                || abi.callee_cleans_stack;
        }

        [[nodiscard]] inline target_abi default_abi_for_signature(const function_signature& signature) {
            target_abi abi = signature.abi;
            if (!has_codegen_abi(abi)) {
                abi = default_target_abi();
            }
            return abi;
        }

        [[nodiscard]] inline bool is_default_like_convention(const calling_convention& cc) {
            return cc.name == "generic"
                && cc.integer_argument_registers.empty()
                && cc.floating_argument_registers.empty()
                && cc.caller_saved.empty()
                && cc.callee_saved.empty()
                && cc.scratch_registers.empty()
                && cc.integer_return_register.id == 0
                && cc.integer_return_register.name == "rv0"
                && cc.floating_return_register.id == 0
                && cc.floating_return_register.name == "frv0";
        }

        [[nodiscard]] inline register_class effective_argument_register_class(const argument_descriptor& descriptor) {
            if (descriptor.reg_class != register_class::integer) {
                return descriptor.reg_class;
            }
            return descriptor.floating_point ? register_class::floating : register_class::integer;
        }

        [[nodiscard]] inline calling_convention resolved_calling_convention(const function_signature& signature) {
            const auto default_cc = default_calling_convention();
            calling_convention cc = default_cc;

            const bool abi_has_layout = !signature.abi.convention.integer_argument_registers.empty()
                || !signature.abi.convention.floating_argument_registers.empty();
            if (abi_has_layout) {
                cc = signature.abi.convention;
            }

            if (!is_default_like_convention(signature.convention)) {
                cc = signature.convention;
            }

            if (cc.name.empty()) {
                cc.name = "generic";
            }
            if (cc.integer_argument_registers.empty()) {
                cc.integer_argument_registers = default_cc.integer_argument_registers;
            }
            if (cc.floating_argument_registers.empty()) {
                cc.floating_argument_registers = default_cc.floating_argument_registers;
            }
            if (cc.integer_return_register.name.empty()) {
                cc.integer_return_register = default_cc.integer_return_register;
            }
            if (cc.floating_return_register.name.empty()) {
                cc.floating_return_register = default_cc.floating_return_register;
            }
            if (cc.caller_saved.empty()) {
                cc.caller_saved = default_cc.caller_saved;
            }
            if (cc.callee_saved.empty()) {
                cc.callee_saved = default_cc.callee_saved;
            }
            if (cc.scratch_registers.empty()) {
                cc.scratch_registers = default_cc.scratch_registers;
            }

            return cc;
        }

        [[nodiscard]] inline argument_descriptor normalized_argument_descriptor(
            const function_signature& signature,
            const std::size_t index
        ) {
            argument_descriptor descriptor;
            descriptor.index = argument_index{static_cast<std::uint32_t>(index)};

            if (index >= signature.arguments.size()) {
                descriptor.passing_kind = argument_passing_kind::ignored;
                descriptor.size = 0;
                descriptor.alignment = 1;
                return descriptor;
            }

            descriptor = signature.arguments[index];
            if (descriptor.index.value == 0 && index != 0) {
                descriptor.index = argument_index{static_cast<std::uint32_t>(index)};
            }
            if (descriptor.size == 0) {
                descriptor.size = 8;
            }
            if (descriptor.alignment == 0) {
                descriptor.alignment = 1;
            }
            return descriptor;
        }

        [[nodiscard]] inline std::vector<argument_location> compute_argument_locations(
            const function_signature& signature) {
            std::vector<argument_location> locations;
            locations.reserve(signature.arguments.size());

            const auto cc = resolved_calling_convention(signature);
            std::size_t integer_register_cursor = 0;
            std::size_t floating_register_cursor = 0;
            std::uint32_t next_stack_slot = 0;

            for (std::size_t i = 0; i < signature.arguments.size(); ++i) {
                const auto descriptor = normalized_argument_descriptor(signature, i);
                argument_location location;
                location.index = argument_index{static_cast<std::uint32_t>(i)};
                location.reg_class = effective_argument_register_class(descriptor);
                location.passing_kind = descriptor.passing_kind;
                location.size = descriptor.size;
                location.alignment = descriptor.alignment;

                if (location.passing_kind == argument_passing_kind::ignored) {
                    location.valid = true;
                    locations.push_back(std::move(location));
                    continue;
                }

                const bool is_register_kind = location.passing_kind == argument_passing_kind::register_value
                    || location.passing_kind == argument_passing_kind::register_reference;
                if (is_register_kind) {
                    if (descriptor.physical_register.has_value()) {
                        location.physical_register = descriptor.physical_register;
                        location.valid = true;
                        locations.push_back(std::move(location));
                        continue;
                    }

                    std::optional<preg> chosen;
                    if (location.reg_class == register_class::floating) {
                        if (floating_register_cursor < cc.floating_argument_registers.size()) {
                            chosen = cc.floating_argument_registers[floating_register_cursor++];
                        }
                    }
                    else {
                        if (integer_register_cursor < cc.integer_argument_registers.size()) {
                            chosen = cc.integer_argument_registers[integer_register_cursor++];
                        }
                    }

                    if (chosen.has_value()) {
                        location.physical_register = chosen;
                        location.valid = true;
                        locations.push_back(std::move(location));
                        continue;
                    }

                    // Generic ABI fallback: overflow register arguments spill to stack slots.
                    location.passing_kind = (location.passing_kind == argument_passing_kind::register_reference)
                                                ? argument_passing_kind::stack_reference
                                                : argument_passing_kind::stack_value;
                }

                if (descriptor.stack_slot.has_value()) {
                    location.stack_slot = descriptor.stack_slot;
                    next_stack_slot = std::max(next_stack_slot, *descriptor.stack_slot + 1);
                }
                else {
                    location.stack_slot = next_stack_slot++;
                }
                location.valid = true;
                locations.push_back(std::move(location));
            }

            return locations;
        }

        [[nodiscard]] inline bool register_in_list(const std::vector<preg>& regs, const preg& reg) {
            return std::ranges::any_of(regs, [&](const preg& candidate) {
                return candidate.id == reg.id;
            });
        }

        [[nodiscard]] inline const frame_object* find_frame_object(
            const stack_frame_layout& layout,
            const frame_object_id id
        ) {
            const auto it = std::ranges::find_if(layout.objects, [&](const frame_object& object) {
                return object.id == id;
            });
            if (it == layout.objects.end()) {
                return nullptr;
            }
            return std::addressof(*it);
        }

        [[nodiscard]] inline frame_object* find_frame_object(stack_frame_layout& layout, const frame_object_id id) {
            const auto it = std::ranges::find_if(layout.objects, [&](const frame_object& object) {
                return object.id == id;
            });
            if (it == layout.objects.end()) {
                return nullptr;
            }
            return std::addressof(*it);
        }

        [[nodiscard]] inline const char* to_string(const frame_object_kind kind) {
            switch (kind) {
            case frame_object_kind::spill_slot: return "spill_slot";
            case frame_object_kind::argument_slot: return "argument_slot";
            case frame_object_kind::return_slot: return "return_slot";
            case frame_object_kind::local_slot: return "local_slot";
            case frame_object_kind::outgoing_call_slot: return "outgoing_call_slot";
            case frame_object_kind::emergency_spill_slot: return "emergency_spill_slot";
            case frame_object_kind::reserved_slot: return "reserved_slot";
            case frame_object_kind::caller_saved_register: return "caller_saved_register";
            case frame_object_kind::callee_saved_register: return "callee_saved_register";
            }
            return "unknown";
        }

        inline void rebuild_object_layouts(stack_frame_layout& layout) {
            layout.object_layouts.clear();
            layout.object_layouts.reserve(layout.objects.size());
            for (const auto& object : layout.objects) {
                layout.object_layouts.push_back(frame_object_layout{
                    object.id,
                    object.kind,
                    object.size,
                    object.alignment,
                    object.offset,
                    object.has_assigned_offset
                });
            }
        }
    } // namespace detail

    [[nodiscard]] inline stack_frame_layout query_frame_layout(const stack_frame_layout& layout) {
        return layout;
    }

    [[nodiscard]] inline std::optional<frame_object> query_frame_object(
        const stack_frame_layout& layout,
        const frame_object_id id
    ) {
        if (const auto* object = detail::find_frame_object(layout, id); object != nullptr) {
            return *object;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline frame_object_id reserve_frame_object(
        stack_frame_layout& layout,
        const frame_object_descriptor& descriptor
    ) {
        frame_object object;
        object.id = frame_object_id{layout.next_object_id++};
        object.kind = descriptor.kind;
        object.size = descriptor.size == 0 ? 1 : descriptor.size;
        object.alignment = descriptor.alignment == 0 ? 1 : descriptor.alignment;
        object.source_spill = descriptor.source_spill;
        object.source_argument = descriptor.source_argument;
        object.assigned_register = descriptor.assigned_register;
        object.usage = descriptor.usage;
        object.flags = descriptor.flags;
        layout.objects.push_back(object);
        detail::rebuild_object_layouts(layout);
        return object.id;
    }

    [[nodiscard]] inline bool release_frame_object(stack_frame_layout& layout, const frame_object_id id) {
        const auto before = layout.objects.size();
        std::erase_if(layout.objects, [&](const frame_object& object) {
            return object.id == id;
        });
        const bool erased = layout.objects.size() != before;
        if (erased) {
            detail::rebuild_object_layouts(layout);
        }
        return erased;
    }

    [[nodiscard]] inline std::optional<frame_object_usage> get_frame_object_usage(
        const stack_frame_layout& layout,
        const frame_object_id id
    ) {
        if (const auto* object = detail::find_frame_object(layout, id); object != nullptr) {
            return object->usage;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::int64_t> get_frame_object_offset(
        const stack_frame_layout& layout,
        const frame_object_id id
    ) {
        if (const auto* object = detail::find_frame_object(layout, id);
            object != nullptr && object->has_assigned_offset) {
            return object->offset;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<preg> get_frame_object_register(
        const stack_frame_layout& layout,
        const frame_object_id id
    ) {
        if (const auto* object = detail::find_frame_object(layout, id); object != nullptr) {
            return object->assigned_register;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<frame_object_kind> get_frame_object_kind(
        const stack_frame_layout& layout,
        const frame_object_id id
    ) {
        if (const auto* object = detail::find_frame_object(layout, id); object != nullptr) {
            return object->kind;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::uint32_t> get_frame_object_size(
        const stack_frame_layout& layout,
        const frame_object_id id
    ) {
        if (const auto* object = detail::find_frame_object(layout, id); object != nullptr) {
            return object->size;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::uint32_t> get_frame_object_alignment(
        const stack_frame_layout& layout,
        const frame_object_id id
    ) {
        if (const auto* object = detail::find_frame_object(layout, id); object != nullptr) {
            return object->alignment;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::uint32_t get_frame_layout_size(const stack_frame_layout& layout) {
        return layout.frame_size;
    }

    [[nodiscard]] inline std::uint32_t get_frame_layout_alignment(const stack_frame_layout& layout) {
        return layout.frame_alignment;
    }

    [[nodiscard]] inline std::vector<frame_object> get_frame_layout_objects(const stack_frame_layout& layout) {
        return layout.objects;
    }

    [[nodiscard]] inline std::optional<frame_object> get_frame_layout_object_by_id(
        const stack_frame_layout& layout,
        const frame_object_id id
    ) {
        return query_frame_object(layout, id);
    }

    [[nodiscard]] inline std::vector<frame_object> get_frame_layout_objects_by_kind(
        const stack_frame_layout& layout,
        const frame_object_kind kind
    ) {
        std::vector<frame_object> out;
        for (const auto& object : layout.objects) {
            if (object.kind == kind) {
                out.push_back(object);
            }
        }
        return out;
    }

    [[nodiscard]] inline mir::verification_result validate_frame_object_usage(
        const stack_frame_layout& layout,
        const frame_object_id id,
        const mir::physical_mir_function& fn
    ) {
        mir::verification_result out;
        const auto maybe_object = query_frame_object(layout, id);
        if (!maybe_object.has_value()) {
            out.diagnostics.emplace_back("frame object not found: id=" + std::to_string(id.value));
            return out;
        }

        std::unordered_set<std::uint32_t> instruction_ids;
        std::unordered_set<std::uint32_t> block_ids;
        for (const auto& block : fn.function.blocks) {
            block_ids.insert(block.id);
            for (const auto& inst : block.instructions) {
                instruction_ids.insert(inst.id);
            }
        }

        for (const auto instruction_id : maybe_object->usage.instruction_ids) {
            if (!instruction_ids.contains(instruction_id)) {
                out.diagnostics.emplace_back(
                    "frame object " + std::to_string(id.value) +
                    " references unknown instruction i" + std::to_string(instruction_id)
                );
            }
        }
        for (const auto block_id : maybe_object->usage.block_ids) {
            if (!block_ids.contains(block_id)) {
                out.diagnostics.emplace_back(
                    "frame object " + std::to_string(id.value) +
                    " references unknown block bb" + std::to_string(block_id)
                );
            }
        }

        return out;
    }

    inline void update_frame_layout_for_spills(const mir::physical_mir_function& fn, stack_frame_layout& layout) {
        for (const auto& slot : fn.function.spill_slots) {
            const bool exists = std::ranges::any_of(layout.objects, [&](const frame_object& object) {
                return object.kind == frame_object_kind::spill_slot
                    && object.source_spill.has_value()
                    && object.source_spill->id == slot.id;
            });
            if (exists) {
                continue;
            }

            frame_object_descriptor descriptor;
            descriptor.kind = frame_object_kind::spill_slot;
            descriptor.size = slot.size;
            descriptor.alignment = slot.alignment;
            descriptor.source_spill = slot;
            descriptor.usage.tags.insert("spill");
            (void)reserve_frame_object(layout, descriptor);
        }
    }

    inline void update_frame_layout_for_calls(const mir::physical_mir_function& fn, stack_frame_layout& layout) {
        bool has_call = false;
        std::unordered_set<std::uint32_t> call_sites;
        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == opcode::call) {
                    has_call = true;
                    call_sites.insert(inst.id);
                }
            }
        }
        if (!has_call) {
            return;
        }

        const bool exists = std::ranges::any_of(layout.objects, [](const frame_object& object) {
            return object.kind == frame_object_kind::outgoing_call_slot;
        });
        if (exists) {
            return;
        }

        frame_object_descriptor descriptor;
        descriptor.kind = frame_object_kind::outgoing_call_slot;
        descriptor.size = 8;
        descriptor.alignment = 8;
        descriptor.flags.for_outgoing_calls = true;
        descriptor.usage.instruction_ids = std::move(call_sites);
        descriptor.usage.tags.insert("call-outgoing");
        (void)reserve_frame_object(layout, descriptor);
    }

    inline void update_frame_layout_for_register_allocation(
        const mir::physical_mir_function& fn,
        stack_frame_layout& layout
    ) {
        const auto abi = default_target_abi();
        std::unordered_set<std::uint16_t> used_register_ids;
        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                auto collect = [&](const allocated_operand& op) {
                    if (op.type != allocated_operand::kind::preg) {
                        return;
                    }
                    used_register_ids.insert(std::get<preg>(op.value).id);
                };
                for (const auto& def : inst.defs) {
                    collect(def);
                }
                for (const auto& use : inst.uses) {
                    collect(use);
                }
            }
        }

        auto reserve_saved = [&](const std::vector<preg>& registers, const frame_object_kind kind,
                                 const std::string_view tag) {
            for (const auto& reg : registers) {
                if (!used_register_ids.contains(reg.id)) {
                    continue;
                }
                const bool exists = std::ranges::any_of(layout.objects, [&](const frame_object& object) {
                    return object.kind == kind
                        && object.assigned_register.has_value()
                        && object.assigned_register->id == reg.id;
                });
                if (exists) {
                    continue;
                }

                frame_object_descriptor descriptor;
                descriptor.kind = kind;
                descriptor.size = 8;
                descriptor.alignment = 8;
                descriptor.assigned_register = reg;
                descriptor.usage.tags.insert(std::string{tag});
                (void)reserve_frame_object(layout, descriptor);
            }
        };

        reserve_saved(abi.convention.caller_saved, frame_object_kind::caller_saved_register, "caller-saved");
        reserve_saved(abi.convention.callee_saved, frame_object_kind::callee_saved_register, "callee-saved");
    }

    inline void update_frame_layout_for_prologue_epilogue(
        const mir::physical_mir_function& fn,
        stack_frame_layout& layout
    ) {
        (void)fn;
        const bool has_reserved = std::ranges::any_of(layout.objects, [](const frame_object& object) {
            return object.kind == frame_object_kind::reserved_slot;
        });
        if (has_reserved) {
            return;
        }

        frame_object_descriptor descriptor;
        descriptor.kind = frame_object_kind::reserved_slot;
        descriptor.size = 16;
        descriptor.alignment = 16;
        descriptor.flags.reserved_special = true;
        descriptor.flags.pinned = true;
        descriptor.usage.tags.insert("prologue-epilogue-reserved");
        (void)reserve_frame_object(layout, descriptor);
    }

    [[nodiscard]] inline stack_frame_layout update_frame_layout_for_spills(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    ) {
        auto out = layout;
        update_frame_layout_for_spills(fn, out);
        return out;
    }

    [[nodiscard]] inline stack_frame_layout update_frame_layout_for_calls(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    ) {
        auto out = layout;
        update_frame_layout_for_calls(fn, out);
        return out;
    }

    [[nodiscard]] inline stack_frame_layout update_frame_layout_for_register_allocation(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    ) {
        auto out = layout;
        update_frame_layout_for_register_allocation(fn, out);
        return out;
    }

    [[nodiscard]] inline stack_frame_layout update_frame_layout_for_prologue_epilogue(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    ) {
        auto out = layout;
        update_frame_layout_for_prologue_epilogue(fn, out);
        return out;
    }

    inline void assign_frame_offsets(stack_frame_layout& layout) {
        auto align_value = [](const std::uint32_t value, const std::uint32_t alignment) {
            if (alignment == 0) {
                return value;
            }
            const auto remainder = value % alignment;
            return remainder == 0 ? value : value + (alignment - remainder);
        };

        std::uint32_t cursor = 0;
        for (auto& object : layout.objects) {
            const auto alignment = object.alignment == 0 ? 1u : object.alignment;
            const auto size = object.size == 0 ? 1u : object.size;
            cursor = align_value(cursor, alignment);
            if (layout.stack_grows_down) {
                object.offset = -static_cast<std::int64_t>(cursor + size);
            }
            else {
                object.offset = static_cast<std::int64_t>(cursor);
            }
            object.has_assigned_offset = true;
            cursor += size;
        }
        layout.frame_size = cursor;
        detail::rebuild_object_layouts(layout);
    }

    [[nodiscard]] inline stack_frame_layout assign_frame_offsets(const stack_frame_layout& layout) {
        auto out = layout;
        assign_frame_offsets(out);
        return out;
    }

    inline void align_frame_layout(stack_frame_layout& layout) {
        auto align_value = [](const std::uint32_t value, const std::uint32_t alignment) {
            if (alignment == 0) {
                return value;
            }
            const auto remainder = value % alignment;
            return remainder == 0 ? value : value + (alignment - remainder);
        };

        std::uint32_t alignment = 1;
        for (const auto& object : layout.objects) {
            alignment = std::max(alignment, object.alignment == 0 ? 1u : object.alignment);
        }
        alignment = std::max(alignment, layout.frame_alignment == 0 ? 1u : layout.frame_alignment);
        layout.frame_alignment = alignment;
        layout.frame_size = align_value(layout.frame_size, layout.frame_alignment);
    }

    [[nodiscard]] inline stack_frame_layout align_frame_layout(const stack_frame_layout& layout) {
        auto out = layout;
        align_frame_layout(out);
        return out;
    }

    [[nodiscard]] inline mir::verification_result validate_frame_layout(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout
    ) {
        mir::verification_result out;
        std::unordered_set<std::uint32_t> ids;

        for (const auto& object : layout.objects) {
            if (!object.id.valid()) {
                out.diagnostics.emplace_back("frame object has invalid id");
            }
            if (!ids.insert(object.id.value).second) {
                out.diagnostics.emplace_back("duplicate frame object id=" + std::to_string(object.id.value));
            }
            if (object.size == 0) {
                out.diagnostics.emplace_back("frame object id=" + std::to_string(object.id.value) + " has zero size");
            }
            if (object.alignment == 0) {
                out.diagnostics.emplace_back(
                    "frame object id=" + std::to_string(object.id.value) + " has zero alignment"
                );
            }
            if (!object.has_assigned_offset) {
                out.diagnostics.emplace_back(
                    "frame object id=" + std::to_string(object.id.value) + " has no assigned offset"
                );
            }

            const auto usage_check = validate_frame_object_usage(layout, object.id, fn);
            out.diagnostics.insert(
                out.diagnostics.end(),
                usage_check.diagnostics.begin(),
                usage_check.diagnostics.end()
            );
        }

        if (layout.frame_alignment == 0) {
            out.diagnostics.emplace_back("frame layout alignment must be non-zero");
        }
        if (layout.frame_alignment != 0 && (layout.frame_size % layout.frame_alignment) != 0) {
            out.diagnostics.emplace_back("frame size is not aligned to frame alignment");
        }

        return out;
    }

    [[nodiscard]] inline stack_frame_layout compute_stack_frame(const mir::physical_mir_function& fn) {
        stack_frame_layout layout;
        layout.frame_alignment = 16;
        layout.stack_grows_down = true;

        update_frame_layout_for_spills(fn, layout);
        update_frame_layout_for_calls(fn, layout);
        update_frame_layout_for_register_allocation(fn, layout);
        update_frame_layout_for_prologue_epilogue(fn, layout);
        assign_frame_offsets(layout);
        align_frame_layout(layout);
        detail::rebuild_object_layouts(layout);
        return layout;
    }

    [[nodiscard]] inline stack_frame_layout compute_frame_layout(const mir::physical_mir_function& fn) {
        return compute_stack_frame(fn);
    }

    [[nodiscard]] inline memory_operand make_frame_pointer_relative_memory(
        const memory_address_kind kind,
        const std::int64_t displacement,
        const std::optional<frame_object_id> referenced_object,
        std::optional<std::string> referenced_symbol,
        const frame_addressing_options& options
    ) {
        memory_address address;
        address.kind = kind;
        address.base = options.frame_pointer_register;
        address.scale = 1;
        address.displacement = displacement + options.dynamic_stack_adjustment;
        address.referenced_frame_object = referenced_object;
        address.referenced_symbol = std::move(referenced_symbol);
        return memory_operand{std::move(address)};
    }

    [[nodiscard]] inline memory_operand make_stack_pointer_relative_memory(
        const memory_address_kind kind,
        const std::int64_t displacement,
        const std::optional<frame_object_id> referenced_object,
        std::optional<std::string> referenced_symbol,
        const frame_addressing_options& options
    ) {
        memory_address address;
        address.kind = kind;
        address.base = options.stack_pointer_register;
        address.scale = 1;
        address.displacement = displacement + options.dynamic_stack_adjustment;
        address.referenced_frame_object = referenced_object;
        address.referenced_symbol = std::move(referenced_symbol);
        return memory_operand{std::move(address)};
    }

    [[nodiscard]] inline memory_operand make_memory_operand_for_spill_slot(
        const spill_slot& slot,
        const frame_addressing_options& options
    ) {
        memory_address address;
        address.kind = memory_address_kind::spill_slot;
        address.base = options.prefer_frame_pointer_relative
                           ? std::optional{options.frame_pointer_register}
                           : std::optional{options.stack_pointer_register};
        address.scale = 1;
        address.displacement = slot.frame_offset + options.dynamic_stack_adjustment;
        return memory_operand{std::move(address)};
    }

    [[nodiscard]] inline memory_operand make_memory_operand_for_frame_object(
        const frame_object& object,
        const frame_addressing_options& options
    ) {
        auto kind_from_object = [](const frame_object_kind kind) {
            switch (kind) {
            case frame_object_kind::spill_slot:
            case frame_object_kind::emergency_spill_slot:
                return memory_address_kind::spill_slot;
            case frame_object_kind::argument_slot:
            case frame_object_kind::outgoing_call_slot:
                return memory_address_kind::argument_slot;
            case frame_object_kind::return_slot:
                return memory_address_kind::return_slot;
            case frame_object_kind::local_slot:
            case frame_object_kind::reserved_slot:
            case frame_object_kind::caller_saved_register:
            case frame_object_kind::callee_saved_register:
                return memory_address_kind::stack_frame;
            }
            return memory_address_kind::computed_address;
        };

        const bool use_stack_pointer = !options.prefer_frame_pointer_relative && options.allow_stack_pointer_relative;
        auto mem = use_stack_pointer
                       ? make_stack_pointer_relative_memory(kind_from_object(object.kind), object.offset, object.id,
                                                            std::nullopt,
                                                            options)
                       : make_frame_pointer_relative_memory(kind_from_object(object.kind), object.offset, object.id,
                                                            std::nullopt, options);
        if (!object.has_assigned_offset) {
            mem.address.kind = memory_address_kind::computed_address;
        }
        return mem;
    }

    [[nodiscard]] inline frame_address_lowering_result lower_frame_objects_to_addresses(
        const mir::physical_mir_function& fn,
        const frame_addressing_options& options
    ) {
        frame_address_lowering_result out;
        out.options = options;
        out.layout = fn.frame_layout.value_or(compute_stack_frame(fn));

        if (out.options.minimum_alignment == 0) {
            out.options.minimum_alignment = 1;
            out.diagnostics.emplace_back("frame-address lowering adjusted minimum alignment from 0 to 1");
        }

        if (out.options.has_variable_sized_objects) {
            out.diagnostics.emplace_back(
                "variable-sized objects are represented via computed_address placeholders in frame-address lowering"
            );
        }
        if (out.options.dynamic_stack_adjustment != 0) {
            out.diagnostics.emplace_back(
                "dynamic stack adjustment applied to memory operand displacements: " +
                std::to_string(out.options.dynamic_stack_adjustment)
            );
        }
        if (out.options.platform_hint.has_value()) {
            out.diagnostics.emplace_back("platform hint (metadata only): " + *out.options.platform_hint);
        }
        for (const auto& flag : out.options.platform_feature_flags) {
            out.diagnostics.emplace_back("platform feature placeholder: " + flag);
        }

        for (const auto& object : out.layout.objects) {
            if (object.kind == frame_object_kind::argument_slot && !out.options.include_argument_slots) {
                continue;
            }
            if (object.kind == frame_object_kind::local_slot && !out.options.include_local_slots) {
                continue;
            }
            if (object.kind == frame_object_kind::callee_saved_register && !out.options.include_callee_saved_slots) {
                continue;
            }
            if (object.kind == frame_object_kind::caller_saved_register && !out.options.include_caller_saved_slots) {
                continue;
            }
            if (object.kind == frame_object_kind::return_slot && !out.options.include_return_slot) {
                continue;
            }

            if (object.alignment < out.options.minimum_alignment) {
                out.diagnostics.emplace_back(
                    "frame object fobj" + std::to_string(object.id.value) +
                    " has alignment below configured minimum"
                );
            }

            auto mem = make_memory_operand_for_frame_object(object, out.options);
            if (mem.address.base.has_value()) {
                if (mem.address.base->id == out.options.frame_pointer_register.id) {
                    out.used_frame_pointer_relative = true;
                }
                if (mem.address.base->id == out.options.stack_pointer_register.id) {
                    out.used_stack_pointer_relative = true;
                }
            }

            out.by_frame_object_id[object.id.value] = mem;
            if (object.source_spill.has_value()) {
                out.by_spill_slot_id[object.source_spill->id] = mem;
            }
            if (object.source_argument.has_value()) {
                out.by_argument_index[object.source_argument->value] = mem;
            }
            if (object.kind == frame_object_kind::return_slot) {
                out.return_slot = mem;
            }
        }

        for (const auto& slot : fn.function.spill_slots) {
            if (!out.by_spill_slot_id.contains(slot.id)) {
                out.by_spill_slot_id[slot.id] = make_memory_operand_for_spill_slot(slot, out.options);
            }
        }

        if (fn.signature.has_value()) {
            for (std::uint32_t i = 0; i < fn.signature->arguments.size(); ++i) {
                const auto arg_location = get_argument_location(*fn.signature, i);
                if (!arg_location.stack_slot.has_value()) {
                    continue;
                }
                if (out.by_argument_index.contains(i)) {
                    continue;
                }

                memory_address address;
                address.kind = memory_address_kind::argument_slot;
                address.base = out.options.allow_stack_pointer_relative && !out.options.prefer_frame_pointer_relative
                                   ? std::optional{out.options.stack_pointer_register}
                                   : std::optional{out.options.frame_pointer_register};
                address.displacement = static_cast<std::int64_t>(*arg_location.stack_slot) * 8
                    + out.options.dynamic_stack_adjustment;
                out.by_argument_index[i] = memory_operand{std::move(address)};
            }

            const auto ret_location = get_return_location(*fn.signature);
            if (ret_location.passing_kind == return_passing_kind::stack_value && ret_location.stack_slot.has_value()
                && !out.return_slot.has_value()) {
                memory_address address;
                address.kind = memory_address_kind::return_slot;
                address.base = out.options.prefer_frame_pointer_relative
                                   ? std::optional{out.options.frame_pointer_register}
                                   : std::optional{out.options.stack_pointer_register};
                address.displacement = static_cast<std::int64_t>(*ret_location.stack_slot) * 8
                    + out.options.dynamic_stack_adjustment;
                out.return_slot = memory_operand{std::move(address)};
            }
        }

        if (!out.used_frame_pointer_relative && !out.used_stack_pointer_relative) {
            out.diagnostics.emplace_back(
                "frame-address lowering produced no frame-relative operands; this can happen for register-only MIR"
            );
        }

        return out;
    }

    [[nodiscard]] inline mir::physical_mir_function materialize_frame_addresses(
        const mir::physical_mir_function& fn,
        const frame_address_lowering_result& lowered
    ) {
        mir::physical_mir_function out = fn;
        out.frame_layout = lowered.layout;
        out.diagnostics.insert(out.diagnostics.end(), lowered.diagnostics.begin(), lowered.diagnostics.end());

        auto spill_to_memory = [&](const spill_slot& slot) {
            if (const auto it = lowered.by_spill_slot_id.find(slot.id); it != lowered.by_spill_slot_id.end()) {
                return it->second;
            }
            return make_memory_operand_for_spill_slot(slot, lowered.options);
        };

        for (auto& block : out.function.blocks) {
            for (auto& inst : block.instructions) {
                auto materialize_operand = [&](allocated_operand& op) {
                    if (op.type == allocated_operand::kind::spill) {
                        const auto slot = std::get<spill_slot>(op.value);
                        op = allocated_operand::as_memory(spill_to_memory(slot));
                        return;
                    }
                    if (op.type == allocated_operand::kind::argument_index) {
                        const auto index = std::get<std::uint32_t>(op.value);
                        if (const auto it = lowered.by_argument_index.find(index);
                            it != lowered.by_argument_index.end()) {
                            op = allocated_operand::as_memory(it->second);
                        }
                    }
                };

                for (auto& def : inst.defs) {
                    materialize_operand(def);
                }
                for (auto& use : inst.uses) {
                    materialize_operand(use);
                }
            }
        }

        return out;
    }

    [[nodiscard]] inline mir::physical_mir_function materialize_frame_addresses(
        const mir::physical_mir_function& fn,
        const frame_addressing_options& options
    ) {
        const auto lowered = lower_frame_objects_to_addresses(fn, options);
        return materialize_frame_addresses(fn, lowered);
    }

    [[nodiscard]] inline std::string dump_stack_frame(const stack_frame_layout& layout) {
        std::ostringstream os;
        os << "stack-frame-layout size=" << layout.frame_size
            << " align=" << layout.frame_alignment
            << " growth=" << (layout.stack_grows_down ? "down" : "up") << "\n";
        for (const auto& object : layout.objects) {
            os << "  fobj" << object.id.value
                << " kind=" << detail::to_string(object.kind)
                << " size=" << object.size
                << " align=" << object.alignment;
            if (object.has_assigned_offset) {
                os << " offset=" << object.offset;
            }
            if (object.source_spill.has_value()) {
                os << " spill=spill" << object.source_spill->id;
            }
            if (object.source_argument.has_value()) {
                os << " arg=arg" << object.source_argument->value;
            }
            if (object.assigned_register.has_value()) {
                os << " reg=" << object.assigned_register->name;
            }
            if (!object.usage.tags.empty()) {
                os << " tags=[";
                bool first = true;
                for (const auto& tag : object.usage.tags) {
                    if (!first) {
                        os << ",";
                    }
                    first = false;
                    os << tag;
                }
                os << "]";
            }
            os << "\n";
        }
        return os.str();
    }

    [[nodiscard]] inline std::string dump_frame_layout(const stack_frame_layout& layout) {
        return dump_stack_frame(layout);
    }

    [[nodiscard]] inline std::string dump_prologue_plan(const prologue_plan& plan) {
        std::ostringstream os;
        os << "prologue-plan stack_size=" << plan.total_stack_size
            << " frame_align=" << plan.frame_alignment
            << " allocate_frame=" << (plan.allocate_stack_frame ? "true" : "false") << "\n";
        os << "  outgoing_arg_area=" << plan.outgoing_argument_area_size << "\n";
        os << "  return=" << plan.return_location << " ; " << plan.return_value_handling << "\n";
        os << "  varargs=" << plan.varargs_handling << "\n";

        if (!plan.saved_registers.empty()) {
            os << "  saved-registers\n";
            for (const auto& entry : plan.saved_registers) {
                os << "    " << entry.reg.name
                    << " caller_saved=" << (entry.caller_saved ? "true" : "false")
                    << " callee_saved=" << (entry.callee_saved ? "true" : "false");
                if (entry.frame_object.has_value()) {
                    os << " frame_object=fobj" << entry.frame_object->value;
                }
                os << "\n";
            }
        }

        if (!plan.spill_frame_objects.empty()) {
            os << "  spill-frame-objects\n";
            for (const auto& obj : plan.spill_frame_objects) {
                os << "    fobj" << obj.id.value << " size=" << obj.size << " align=" << obj.alignment;
                if (obj.has_assigned_offset) {
                    os << " offset=" << obj.offset;
                }
                os << "\n";
            }
        }

        if (!plan.argument_locations.empty()) {
            os << "  argument-locations\n";
            for (const auto& arg : plan.argument_locations) {
                os << "    " << arg << "\n";
            }
        }

        if (!plan.abi_features_handling.empty()) {
            os << "  abi-features\n";
            for (const auto& feature : plan.abi_features_handling) {
                os << "    " << feature << "\n";
            }
        }

        os << "  debug-notes\n";
        os << "    ABI decisions: argument and return placement inferred from lowered load_arg/ret operands\n";
        os << "    Prologue decisions: saved registers derived from frame objects and ABI saved-register sets\n";
        os << "    Frame decisions: spill/outgoing slots come from computed stack_frame_layout\n";
        os << "    Call-frame decisions: outgoing argument area is placeholder-only (no emission yet)\n";
        os << "    Return handling: generic register/stack model only\n";
        os << "    Varargs/features: summarized from target_abi flags and placeholders\n";
        os << "    Limitations: generic ABI only, no platform-specific quirks modeled here\n";
        os << "    TODO: extend for tail calls, aggregate returns, and target-specific conventions\n";
        return os.str();
    }

    [[nodiscard]] inline std::string dump_epilogue_plan(const epilogue_plan& plan) {
        std::ostringstream os;
        os << "epilogue-plan stack_size=" << plan.total_stack_size
            << " frame_align=" << plan.frame_alignment
            << " deallocate_frame=" << (plan.deallocate_stack_frame ? "true" : "false")
            << " emit_return=" << (plan.emit_return ? "true" : "false") << "\n";
        os << "  outgoing_arg_area=" << plan.outgoing_argument_area_size << "\n";
        os << "  return=" << plan.return_location << " ; " << plan.return_value_handling << "\n";

        if (!plan.saved_registers.empty()) {
            os << "  restore-registers\n";
            for (const auto& entry : plan.saved_registers) {
                os << "    " << entry.reg.name;
                if (entry.frame_object.has_value()) {
                    os << " from fobj" << entry.frame_object->value;
                }
                os << "\n";
            }
        }

        if (!plan.abi_features_handling.empty()) {
            os << "  abi-features\n";
            for (const auto& feature : plan.abi_features_handling) {
                os << "    " << feature << "\n";
            }
        }

        os << "  debug-notes\n";
        os << "    Epilogue decisions mirror prologue planning, without instruction emission\n";
        os << "    Generic ABI assumptions apply; backend keeps this as a plan artifact\n";
        return os.str();
    }

    [[nodiscard]] inline argument_assignment_result lower_arguments_to_abi(
        const mir::virtual_mir_function& fn,
        const function_signature& signature
    ) {
        argument_assignment_result out;
        out.function = fn;

        out.metadata.per_argument_locations.reserve(signature.arguments.size());
        for (std::size_t i = 0; i < signature.arguments.size(); ++i) {
            out.metadata.per_argument_locations.push_back(
                get_argument_location(signature, argument_index{static_cast<std::uint32_t>(i)})
            );
        }

        for (auto& block : out.function.function.blocks) {
            for (auto& inst : block.instructions) {
                if (inst.op != opcode::load_arg) {
                    continue;
                }

                if (inst.uses.empty() || inst.uses[0].type != operand::kind::argument_index) {
                    out.metadata.diagnostics.emplace_back(
                        "load_arg i" + std::to_string(inst.id) + " must reference an argument index"
                    );
                    continue;
                }

                const auto raw_index = std::get<std::uint32_t>(inst.uses[0].value);
                if (raw_index >= out.metadata.per_argument_locations.size()) {
                    out.metadata.diagnostics.emplace_back(
                        "load_arg i" + std::to_string(inst.id) + " references invalid arg" + std::to_string(raw_index)
                    );
                    continue;
                }

                const auto location = out.metadata.per_argument_locations[raw_index];
                out.metadata.location_by_instruction_id[inst.id] = location;

                if (!inst.defs.empty() && inst.defs[0].type == operand::kind::vreg) {
                    const auto defined = std::get<vreg>(inst.defs[0].value);
                    out.metadata.location_by_vreg_id[defined.id] = location;
                }

                if (location.physical_register.has_value()) {
                    inst.comment = "abi arg" + std::to_string(raw_index) + " -> " + location.physical_register->name;
                }
                else if (location.stack_slot.has_value()) {
                    inst.comment = "abi arg" + std::to_string(raw_index) + " -> stack[" +
                        std::to_string(*location.stack_slot) + "]";
                }
                else {
                    inst.comment = "abi arg" + std::to_string(raw_index) + " -> ignored";
                }
            }
        }

        out.metadata.validated = out.metadata.diagnostics.empty();
        out.function.argument_abi = out.metadata;
        return out;
    }

    [[nodiscard]] inline return_lowering_result lower_return_to_abi(
        const mir::physical_mir_function& fn,
        const return_descriptor& descriptor
    ) {
        return_lowering_result out;
        out.location.reg_class = descriptor.reg_class;
        out.location.passing_kind = descriptor.passing_kind;
        out.location.size = descriptor.size;
        out.location.alignment = descriptor.alignment;

        bool has_error = false;
        bool has_reject = false;
        bool has_unhandled = false;
        auto add_diag = [&](const return_lowering_diagnostic_type type, std::string message) {
            out.diagnostic_types.push_back(type);
            out.diagnostics.push_back(std::move(message));
        };

        if (descriptor.alignment == 0 ||
            (descriptor.passing_kind != return_passing_kind::void_return && descriptor.size == 0)) {
            add_diag(
                return_lowering_diagnostic_invalid_return_descriptor,
                "invalid return descriptor: non-void returns require non-zero size and alignment"
            );
            has_error = true;
        }

        const auto abi = default_target_abi();
        if (descriptor.passing_kind == return_passing_kind::register_value) {
            if (descriptor.reg_class == register_class::vector) {
                add_diag(
                    return_lowering_diagnostic_register_return_not_supported,
                    "register return is not supported for vector class in generic ABI"
                );
                has_unhandled = true;
            }

            out.location.physical_register = descriptor.physical_register.has_value()
                                                 ? descriptor.physical_register
                                                 : std::optional{
                                                     descriptor.reg_class == register_class::floating
                                                         ? abi.convention.floating_return_register
                                                         : abi.convention.integer_return_register
                                                 };
            if (!out.location.physical_register.has_value() || out.location.physical_register->name.empty()) {
                add_diag(
                    return_lowering_diagnostic_register_return_not_supported,
                    "register return is not supported by current generic ABI configuration"
                );
                has_error = true;
            }

            if (out.location.physical_register.has_value()) {
                const bool reserved = std::ranges::any_of(
                    abi.convention.scratch_registers,
                    [&](const preg& reg) {
                        return reg.id == out.location.physical_register->id;
                    }
                );
                if (reserved) {
                    add_diag(
                        return_lowering_diagnostic_reserved_register_return,
                        "return register " + out.location.physical_register->name +
                        " is reserved/scratch in generic ABI"
                    );
                    has_reject = true;
                }
            }
        }
        else if (descriptor.passing_kind == return_passing_kind::stack_value) {
            out.location.stack_slot = descriptor.stack_slot.value_or(0);
        }

        bool saw_ret = false;
        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op != opcode::ret) {
                    continue;
                }
                saw_ret = true;
                out.return_instruction_ids.push_back(inst.id);

                if (descriptor.passing_kind == return_passing_kind::void_return) {
                    if (!inst.uses.empty()) {
                        add_diag(
                            return_lowering_diagnostic_void_return_with_uses,
                            "ret i" + std::to_string(inst.id) + " has uses for void return descriptor"
                        );
                        has_error = true;
                    }
                    continue;
                }

                if (inst.uses.empty()) {
                    add_diag(
                        return_lowering_diagnostic_nonvoid_return_without_uses,
                        "ret i" + std::to_string(inst.id) + " has no return operand for non-void descriptor"
                    );
                    has_error = true;
                    continue;
                }

                const auto& ret_value = inst.uses.front();
                if (descriptor.passing_kind == return_passing_kind::register_value) {
                    if (ret_value.type != allocated_operand::kind::preg) {
                        add_diag(
                            return_lowering_diagnostic_return_descriptor_mismatch,
                            "ret i" + std::to_string(inst.id) +
                            " must return via preg for register return descriptor"
                        );
                        has_reject = true;
                        continue;
                    }

                    if (out.location.physical_register.has_value()) {
                        const auto& actual = std::get<preg>(ret_value.value);
                        if (actual.id != out.location.physical_register->id) {
                            add_diag(
                                return_lowering_diagnostic_return_descriptor_mismatch,
                                "ret i" + std::to_string(inst.id) + " returns " + actual.name +
                                " but descriptor expects " + out.location.physical_register->name
                            );
                            has_error = true;
                        }
                    }
                }
                else if (descriptor.passing_kind == return_passing_kind::stack_value) {
                    if (ret_value.type != allocated_operand::kind::spill) {
                        add_diag(
                            return_lowering_diagnostic_return_descriptor_mismatch,
                            "ret i" + std::to_string(inst.id) +
                            " must return via spill slot for stack return descriptor"
                        );
                        has_reject = true;
                        continue;
                    }

                    const auto& actual = std::get<spill_slot>(ret_value.value);
                    if (descriptor.stack_slot.has_value() && actual.id != *descriptor.stack_slot + 1) {
                        add_diag(
                            return_lowering_diagnostic_return_descriptor_mismatch,
                            "ret i" + std::to_string(inst.id) + " returns spill" + std::to_string(actual.id) +
                            " but descriptor expects spill" + std::to_string(*descriptor.stack_slot + 1)
                        );
                        has_error = true;
                    }
                    if (!descriptor.stack_slot.has_value()) {
                        out.location.stack_slot = actual.id - 1;
                    }
                }
            }
        }

        if (!saw_ret) {
            add_diag(
                return_lowering_diagnostic_return_descriptor_mismatch,
                "physical MIR has no return instruction to lower"
            );
            has_error = true;
        }

        out.location.valid = !has_error && !has_reject;
        if (has_error) {
            out.status = return_lowering_status_error;
        }
        else if (has_reject) {
            out.status = return_lowering_status_reject;
        }
        else if (has_unhandled) {
            out.status = return_lowering_status_unhandled;
        }
        else {
            out.status = return_lowering_status_lowered;
            out.lowered = true;
        }

        return out;
    }

    [[nodiscard]] inline argument_location get_argument_location(
        const function_signature& signature,
        const argument_index index
    ) {
        if (index.value >= signature.arguments.size()) {
            argument_location invalid;
            invalid.index = index;
            invalid.passing_kind = argument_passing_kind::ignored;
            invalid.valid = false;
            return invalid;
        }
        const auto locations = detail::compute_argument_locations(signature);
        return locations[index.value];
    }

    [[nodiscard]] inline argument_location get_argument_location(
        const function_signature& signature,
        const std::uint32_t index
    ) {
        return get_argument_location(signature, argument_index{index});
    }

    [[nodiscard]] inline return_location get_return_location(const function_signature& signature) {
        const auto cc = detail::resolved_calling_convention(signature);
        return_descriptor descriptor = signature.return_value;

        if (descriptor.size == 0) {
            descriptor.size = 8;
        }
        if (descriptor.alignment == 0) {
            descriptor.alignment = 1;
        }

        return_location location;
        location.reg_class = descriptor.reg_class;
        location.passing_kind = descriptor.passing_kind;
        location.size = descriptor.size;
        location.alignment = descriptor.alignment;

        if (location.passing_kind == return_passing_kind::void_return) {
            location.valid = true;
            return location;
        }

        if (location.passing_kind == return_passing_kind::stack_value) {
            location.stack_slot = descriptor.stack_slot.value_or(0);
            location.valid = true;
            return location;
        }

        if (descriptor.physical_register.has_value()) {
            location.physical_register = descriptor.physical_register;
            location.valid = true;
            return location;
        }

        location.physical_register = (location.reg_class == register_class::floating)
                                         ? cc.floating_return_register
                                         : cc.integer_return_register;
        location.valid = location.physical_register.has_value();
        return location;
    }

    [[nodiscard]] inline interpreter_call_frame build_interpreter_call_frame(
        const function_signature& signature,
        const std::vector<std::int64_t>& arguments,
        const std::optional<stack_frame_layout>& layout
    ) {
        interpreter_call_frame frame;
        frame.abi_aware = true;
        frame.frame_layout = layout;

        for (std::size_t i = 0; i < signature.arguments.size(); ++i) {
            const auto index = static_cast<std::uint32_t>(i);
            const auto loc = get_argument_location(signature, index);
            const auto value = (index < arguments.size()) ? arguments[index] : std::int64_t{0};

            frame.argument_by_index[index] = value;

            std::string debug_line = "arg" + std::to_string(index) + " -> ";
            if (loc.physical_register.has_value()) {
                frame.register_arguments[loc.physical_register->id] = value;
                debug_line += "register:" + loc.physical_register->name;
            }
            else if (loc.stack_slot.has_value()) {
                frame.stack_arguments[*loc.stack_slot] = value;
                debug_line += "stack[" + std::to_string(*loc.stack_slot) + "]";
            }
            else {
                debug_line += "ignored";
            }

            debug_line += " value=" + std::to_string(value);
            frame.argument_location_debug.push_back(std::move(debug_line));

            if (index >= arguments.size() && loc.passing_kind != argument_passing_kind::ignored) {
                frame.diagnostics.push_back(
                    "missing runtime value for arg" + std::to_string(index) + ", defaulting to 0"
                );
            }
        }

        return frame;
    }

    [[nodiscard]] inline std::optional<std::int64_t> get_interpreter_argument_value(
        const interpreter_call_frame& frame,
        const std::uint32_t index
    ) {
        if (const auto it = frame.argument_by_index.find(index); it != frame.argument_by_index.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::string dump_interpreter_call_frame(const interpreter_call_frame& frame) {
        std::ostringstream os;
        os << "interpreter-call-frame abi_aware=" << (frame.abi_aware ? "true" : "false") << "\n";
        for (const auto& line : frame.argument_location_debug) {
            os << "  " << line << "\n";
        }
        if (frame.frame_layout.has_value()) {
            os << "  frame-layout\n";
            os << dump_frame_layout(*frame.frame_layout);
        }
        for (const auto& diag : frame.diagnostics) {
            os << "  diag: " << diag << "\n";
        }
        return os.str();
    }

    [[nodiscard]] inline virtual_register_abi_mapping_result map_virtual_registers_to_calling_convention(
        const mir::virtual_mir_function& fn,
        const function_signature& signature
    ) {
        virtual_register_abi_mapping_result out;

        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op != opcode::load_arg) {
                    continue;
                }

                if (inst.defs.empty() || inst.defs[0].type != operand::kind::vreg) {
                    out.diagnostics.push_back(
                        "load_arg i" + std::to_string(inst.id) + " must define a virtual register"
                    );
                    continue;
                }
                if (inst.uses.empty() || inst.uses[0].type != operand::kind::argument_index) {
                    out.diagnostics.push_back(
                        "load_arg i" + std::to_string(inst.id) + " must reference an argument index"
                    );
                    continue;
                }

                const auto raw_index = std::get<std::uint32_t>(inst.uses[0].value);
                if (raw_index >= signature.arguments.size()) {
                    out.diagnostics.push_back(
                        "load_arg i" + std::to_string(inst.id) + " references invalid arg" + std::to_string(raw_index)
                    );
                    continue;
                }

                const auto location = get_argument_location(signature, argument_index{raw_index});
                virtual_register_abi_binding binding;
                binding.virtual_register = std::get<vreg>(inst.defs[0].value);
                binding.index = argument_index{raw_index};
                binding.location = location;

                if (location.passing_kind == argument_passing_kind::register_value
                    || location.passing_kind == argument_passing_kind::register_reference) {
                    binding.physical_register = location.physical_register;
                }
                else if (location.passing_kind == argument_passing_kind::stack_value
                    || location.passing_kind == argument_passing_kind::stack_reference) {
                    binding.stack_slot = spill_slot{
                        location.stack_slot.value_or(raw_index) + 1,
                        location.size,
                        location.alignment,
                        0
                    };
                }

                out.bindings.push_back(std::move(binding));
            }
        }

        return out;
    }

    [[nodiscard]] inline std::uint32_t align_up(const std::uint32_t value, const std::uint32_t alignment) {
        if (alignment == 0) {
            return value;
        }
        const auto remainder = value % alignment;
        if (remainder == 0) {
            return value;
        }
        return value + (alignment - remainder);
    }

    [[nodiscard]] inline stack_frame build_stack_frame(
        const allocated_function_ir& fn,
        const calling_convention& convention = default_calling_convention(),
        std::uint32_t local_bytes = 0
    ) {
        stack_frame frame;
        frame.stack_alignment = 16;

        std::uint32_t cursor = 0;
        auto place_slot = [&](const std::uint32_t size, const std::uint32_t alignment, auto&& commit) mutable {
            cursor = align_up(cursor, alignment == 0 ? 1u : alignment);
            const std::int64_t offset = -static_cast<std::int64_t>(cursor + size);
            commit(offset);
            cursor += size;
        };

        for (const auto& slot : fn.spill_slots) {
            frame_slot placed;
            placed.type = frame_slot::kind::spill;
            placed.id = slot.id;
            placed.size = slot.size;
            placed.alignment = slot.alignment;
            placed.reg_class = register_class::integer;
            place_slot(slot.size, slot.alignment, [&](const std::int64_t offset) {
                placed.offset = offset;
            });
            frame.slots.push_back(std::move(placed));
        }

        for (const auto& reg : convention.caller_saved) {
            frame_slot saved;
            saved.type = frame_slot::kind::saved_register;
            saved.id = reg.id;
            saved.size = 8;
            saved.alignment = 8;
            saved.saved_register = reg;
            saved.reg_class = register_class::integer;
            place_slot(saved.size, saved.alignment, [&](const std::int64_t offset) {
                saved.offset = offset;
            });
            frame.slots.push_back(std::move(saved));
        }

        if (local_bytes > 0) {
            frame_slot local;
            local.type = frame_slot::kind::local;
            local.id = 0;
            local.size = local_bytes;
            local.alignment = 16;
            local.reg_class = register_class::integer;
            place_slot(local.size, local.alignment, [&](const std::int64_t offset) {
                local.offset = offset;
            });
            frame.slots.push_back(std::move(local));
        }

        frame.stack_size = align_up(cursor, frame.stack_alignment);
        return frame;
    }

    namespace detail {
        [[nodiscard]] inline std::string summarize_return_location_from_physical(const mir::physical_mir_function& fn) {
            for (const auto& block : fn.function.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op != opcode::ret) {
                        continue;
                    }
                    if (inst.uses.empty()) {
                        return "void";
                    }
                    const auto& ret = inst.uses.front();
                    if (ret.type == allocated_operand::kind::preg) {
                        return "register:" + std::get<preg>(ret.value).name;
                    }
                    if (ret.type == allocated_operand::kind::spill) {
                        return "stack:spill" + std::to_string(std::get<spill_slot>(ret.value).id);
                    }
                    return "unhandled";
                }
            }
            return "missing-ret";
        }

        [[nodiscard]] inline std::vector<std::string> summarize_argument_locations_from_physical(
            const mir::physical_mir_function& fn
        ) {
            std::vector<std::string> out;
            std::unordered_map<std::uint32_t, std::string> by_index;
            for (const auto& block : fn.function.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op != opcode::load_arg) {
                        continue;
                    }
                    if (inst.uses.empty() || inst.uses.front().type != allocated_operand::kind::argument_index) {
                        continue;
                    }

                    const auto idx = std::get<std::uint32_t>(inst.uses.front().value);
                    if (!inst.defs.empty() && inst.defs.front().type == allocated_operand::kind::preg) {
                        by_index[idx] = "arg" + std::to_string(idx) + " -> register:" +
                            std::get<preg>(inst.defs.front().value).name;
                    }
                    else {
                        by_index[idx] = "arg" + std::to_string(idx) + " -> stack/indirect";
                    }
                }
            }

            std::vector<std::uint32_t> keys;
            keys.reserve(by_index.size());
            for (const auto& [key, _] : by_index) {
                (void)_;
                keys.push_back(key);
            }
            std::sort(keys.begin(), keys.end());
            for (const auto key : keys) {
                out.push_back(by_index.at(key));
            }
            return out;
        }

        inline void populate_plan_common_fields(
            const mir::physical_mir_function& fn,
            const stack_frame_layout& layout,
            const target_abi& abi,
            prologue_plan& plan
        ) {
            plan.layout = layout;
            plan.frame_alignment = layout.frame_alignment;
            plan.total_stack_size = layout.frame_size;
            plan.adjustment = frame_adjustment{
                -static_cast<std::int64_t>(layout.frame_size),
                static_cast<std::int64_t>(layout.frame_size),
                layout.frame_alignment,
                layout.stack_grows_down
            };

            for (const auto& object : layout.objects) {
                if (object.kind == frame_object_kind::outgoing_call_slot) {
                    plan.outgoing_argument_area_size += object.size;
                }
                if (object.kind == frame_object_kind::spill_slot || object.kind ==
                    frame_object_kind::emergency_spill_slot) {
                    plan.spill_frame_objects.push_back(object);
                }
                if ((object.kind == frame_object_kind::caller_saved_register
                        || object.kind == frame_object_kind::callee_saved_register)
                    && object.assigned_register.has_value()) {
                    plan.saved_registers.push_back(saved_register{
                        *object.assigned_register,
                        object.id,
                        object.kind == frame_object_kind::caller_saved_register,
                        object.kind == frame_object_kind::callee_saved_register
                    });
                }
            }

            plan.callee_saved_registers = abi.convention.callee_saved;
            plan.varargs_handling = abi.variadic_supported
                                        ? "generic variadic support placeholder"
                                        : "variadic arguments not enabled";
            plan.return_location = summarize_return_location_from_physical(fn);
            plan.return_value_handling = "generic return via " + plan.return_location;
            plan.argument_locations = summarize_argument_locations_from_physical(fn);
            plan.abi_features_handling.push_back(
                std::string{"callee_cleans_stack="} + (abi.callee_cleans_stack ? "true" : "false")
            );
            plan.abi_features_handling.push_back(
                std::string{"variadic_supported="} + (abi.variadic_supported ? "true" : "false")
            );
            plan.abi_features_handling.push_back(std::string{"target_abi="} + abi.name);
        }

        inline void populate_plan_common_fields(
            const mir::physical_mir_function& fn,
            const stack_frame_layout& layout,
            const target_abi& abi,
            epilogue_plan& plan
        ) {
            prologue_plan tmp;
            populate_plan_common_fields(fn, layout, abi, tmp);
            plan.layout = std::move(tmp.layout);
            plan.saved_registers = std::move(tmp.saved_registers);
            plan.adjustment = tmp.adjustment;
            plan.outgoing_argument_area_size = tmp.outgoing_argument_area_size;
            plan.return_value_handling = std::move(tmp.return_value_handling);
            plan.varargs_handling = std::move(tmp.varargs_handling);
            plan.abi_features_handling = std::move(tmp.abi_features_handling);
            plan.callee_saved_registers = std::move(tmp.callee_saved_registers);
            plan.spill_frame_objects = std::move(tmp.spill_frame_objects);
            plan.frame_alignment = tmp.frame_alignment;
            plan.total_stack_size = tmp.total_stack_size;
            plan.argument_locations = std::move(tmp.argument_locations);
            plan.return_location = std::move(tmp.return_location);
        }
    } // namespace detail

    [[nodiscard]] inline prologue_plan plan_prologue(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout,
        const target_abi& abi
    ) {
        prologue_plan plan;
        plan.frame = build_stack_frame(fn.function, abi.convention, 0);
        plan.saved_caller_saved = abi.convention.caller_saved;
        plan.save_caller_saved = !plan.saved_caller_saved.empty();
        plan.allocate_stack_frame = layout.frame_size > 0;
        plan.emit_instructions = false;
        detail::populate_plan_common_fields(fn, layout, abi, plan);
        return plan;
    }

    [[nodiscard]] inline epilogue_plan plan_epilogue(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout,
        const target_abi& abi
    ) {
        epilogue_plan plan;
        plan.frame = build_stack_frame(fn.function, abi.convention, 0);
        plan.restore_caller_saved = abi.convention.caller_saved;
        plan.deallocate_stack_frame = layout.frame_size > 0;
        plan.emit_return = true;
        plan.emit_instructions = false;
        detail::populate_plan_common_fields(fn, layout, abi, plan);
        return plan;
    }

    // Backward-compatible overloads retained for existing call sites.
    [[nodiscard]] inline prologue_plan plan_prologue(
        const allocated_function_ir& fn,
        const target_abi& abi = default_target_abi(),
        const std::uint32_t local_bytes = 0
    ) {
        mir::physical_mir_function physical;
        physical.function = fn;
        auto layout = compute_stack_frame(physical);
        if (local_bytes > 0) {
            layout.frame_size += local_bytes;
            layout.frame_size = align_up(layout.frame_size, layout.frame_alignment == 0 ? 1u : layout.frame_alignment);
        }
        return plan_prologue(physical, layout, abi);
    }

    [[nodiscard]] inline epilogue_plan plan_epilogue(
        const allocated_function_ir& fn,
        const target_abi& abi = default_target_abi(),
        const std::uint32_t local_bytes = 0
    ) {
        mir::physical_mir_function physical;
        physical.function = fn;
        auto layout = compute_stack_frame(physical);
        if (local_bytes > 0) {
            layout.frame_size += local_bytes;
            layout.frame_size = align_up(layout.frame_size, layout.frame_alignment == 0 ? 1u : layout.frame_alignment);
        }
        return plan_epilogue(physical, layout, abi);
    }

    [[nodiscard]] inline prologue_plan get_prologue_plan(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout,
        const target_abi& abi
    ) {
        return plan_prologue(fn, layout, abi);
    }

    [[nodiscard]] inline epilogue_plan get_epilogue_plan(
        const mir::physical_mir_function& fn,
        const stack_frame_layout& layout,
        const target_abi& abi
    ) {
        return plan_epilogue(fn, layout, abi);
    }

    [[nodiscard]] inline std::vector<saved_register> get_saved_registers(const prologue_plan& plan) {
        return plan.saved_registers;
    }

    [[nodiscard]] inline std::vector<saved_register> get_saved_registers(const epilogue_plan& plan) {
        return plan.saved_registers;
    }

    [[nodiscard]] inline frame_adjustment get_frame_adjustment(const prologue_plan& plan) {
        return plan.adjustment;
    }

    [[nodiscard]] inline frame_adjustment get_frame_adjustment(const epilogue_plan& plan) {
        return plan.adjustment;
    }

    [[nodiscard]] inline std::uint32_t get_outgoing_argument_area_size(const prologue_plan& plan) {
        return plan.outgoing_argument_area_size;
    }

    [[nodiscard]] inline std::uint32_t get_outgoing_argument_area_size(const epilogue_plan& plan) {
        return plan.outgoing_argument_area_size;
    }

    [[nodiscard]] inline std::string get_return_value_handling(const prologue_plan& plan) {
        return plan.return_value_handling;
    }

    [[nodiscard]] inline std::string get_return_value_handling(const epilogue_plan& plan) {
        return plan.return_value_handling;
    }

    [[nodiscard]] inline std::string get_varargs_handling(const prologue_plan& plan) {
        return plan.varargs_handling;
    }

    [[nodiscard]] inline std::string get_varargs_handling(const epilogue_plan& plan) {
        return plan.varargs_handling;
    }

    [[nodiscard]] inline std::vector<std::string> get_abi_features_handling(const prologue_plan& plan) {
        return plan.abi_features_handling;
    }

    [[nodiscard]] inline std::vector<std::string> get_abi_features_handling(const epilogue_plan& plan) {
        return plan.abi_features_handling;
    }

    [[nodiscard]] inline std::vector<preg> get_callee_saved_registers(const prologue_plan& plan) {
        return plan.callee_saved_registers;
    }

    [[nodiscard]] inline std::vector<preg> get_callee_saved_registers(const epilogue_plan& plan) {
        return plan.callee_saved_registers;
    }

    [[nodiscard]] inline std::vector<frame_object> get_spill_frame_objects(const prologue_plan& plan) {
        return plan.spill_frame_objects;
    }

    [[nodiscard]] inline std::vector<frame_object> get_spill_frame_objects(const epilogue_plan& plan) {
        return plan.spill_frame_objects;
    }

    [[nodiscard]] inline std::uint32_t get_frame_alignment(const prologue_plan& plan) {
        return plan.frame_alignment;
    }

    [[nodiscard]] inline std::uint32_t get_frame_alignment(const epilogue_plan& plan) {
        return plan.frame_alignment;
    }

    [[nodiscard]] inline std::uint32_t get_total_stack_size(const prologue_plan& plan) {
        return plan.total_stack_size;
    }

    [[nodiscard]] inline std::uint32_t get_total_stack_size(const epilogue_plan& plan) {
        return plan.total_stack_size;
    }

    [[nodiscard]] inline std::vector<std::string> get_argument_locations(const prologue_plan& plan) {
        return plan.argument_locations;
    }

    [[nodiscard]] inline std::vector<std::string> get_argument_locations(const epilogue_plan& plan) {
        return plan.argument_locations;
    }

    [[nodiscard]] inline std::string get_return_location(const prologue_plan& plan) {
        return plan.return_location;
    }

    [[nodiscard]] inline std::string get_return_location(const epilogue_plan& plan) {
        return plan.return_location;
    }

    [[nodiscard]] inline std::vector<preg> argument_registers(const function_signature& signature) {
        return detail::resolved_calling_convention(signature).integer_argument_registers;
    }

    [[nodiscard]] inline std::optional<argument_index> bind_terminal_as_argument(
        const std::string_view terminal_name,
        const function_signature& signature
    ) {
        for (std::size_t i = 0; i < signature.arguments.size(); ++i) {
            if (!signature.arguments[i].name.empty() && signature.arguments[i].name == terminal_name) {
                return argument_index{static_cast<std::uint32_t>(i)};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<argument_index> bind_terminal_as_argument(
        const std::string& terminal_name,
        const function_signature& signature
    ) {
        return bind_terminal_as_argument(std::string_view{terminal_name}, signature);
    }

    [[nodiscard]] inline std::optional<argument_index> bind_terminal_as_argument(
        const char* terminal_name,
        const function_signature& signature
    ) {
        return bind_terminal_as_argument(std::string_view{terminal_name == nullptr ? "" : terminal_name}, signature);
    }

    [[nodiscard]] inline std::optional<argument_index> bind_terminal_as_argument(
        const std::size_t position,
        const function_signature& signature
    ) {
        if (position >= signature.arguments.size()) {
            return std::nullopt;
        }
        return argument_index{static_cast<std::uint32_t>(position)};
    }

    template <class Terminal>
    [[nodiscard]] std::optional<argument_index> bind_terminal_as_argument(
        const Terminal& terminal,
        const function_signature& signature
    ) {
        const auto dumped = emit::dump(terminal);
        return bind_terminal_as_argument(std::string_view{dumped}, signature);
    }

    [[nodiscard]] inline std::optional<std::uint32_t> as_vreg_id(const operand& op) {
        if (op.type != operand::kind::vreg) {
            return std::nullopt;
        }
        return std::get<vreg>(op.value).id;
    }

    [[nodiscard]] inline std::optional<ssa_value_id> as_ssa_value_id(const operand& op) {
        if (op.type != operand::kind::ssa_value) {
            return std::nullopt;
        }
        return std::get<ssa_value_id>(op.value);
    }

    [[nodiscard]] inline const char* to_string(const opcode op) {
        switch (op) {
        case opcode::nop: return "nop";
        case opcode::mov: return "mov";
        case opcode::load_imm: return "load_imm";
        case opcode::load_symbol: return "load_symbol";
        case opcode::load_arg: return "load_arg";
        case opcode::load: return "load";
        case opcode::store: return "store";
        case opcode::store_spill: return "store_spill";
        case opcode::load_spill: return "load_spill";
        case opcode::add: return "add";
        case opcode::sub: return "sub";
        case opcode::mul: return "mul";
        case opcode::div: return "div";
        case opcode::mod: return "mod";
        case opcode::neg: return "neg";
        case opcode::cmp_eq: return "cmp_eq";
        case opcode::cmp_ne: return "cmp_ne";
        case opcode::cmp_lt: return "cmp_lt";
        case opcode::cmp_le: return "cmp_le";
        case opcode::cmp_gt: return "cmp_gt";
        case opcode::cmp_ge: return "cmp_ge";
        case opcode::bit_and: return "bit_and";
        case opcode::bit_or: return "bit_or";
        case opcode::bit_xor: return "bit_xor";
        case opcode::bit_not: return "bit_not";
        case opcode::shl: return "shl";
        case opcode::shr: return "shr";
        case opcode::logical_and: return "logical_and";
        case opcode::logical_or: return "logical_or";
        case opcode::logical_not: return "logical_not";
        case opcode::call: return "call";
        case opcode::branch: return "branch";
        case opcode::branch_cond: return "branch_cond";
        case opcode::ret: return "ret";
        case opcode::get_element_ptr: return "get_element_ptr";
        case opcode::extract_value: return "extract_value";
        case opcode::insert_value: return "insert_value";
        case opcode::indirect_call: return "indirect_call";
        case opcode::fadd: return "fadd";
        case opcode::fsub: return "fsub";
        case opcode::fmul: return "fmul";
        case opcode::fdiv: return "fdiv";
        case opcode::fneg: return "fneg";
        case opcode::fload: return "fload";
        case opcode::fstore: return "fstore";
        case opcode::fload_imm: return "fload_imm";
        case opcode::gpr_to_fp: return "gpr_to_fp";
        case opcode::fp_to_gpr: return "fp_to_gpr";
        case opcode::fcmp_eq: return "fcmp_eq";
        case opcode::fcmp_ne: return "fcmp_ne";
        case opcode::fcmp_lt: return "fcmp_lt";
        case opcode::fcmp_le: return "fcmp_le";
        case opcode::fcmp_gt: return "fcmp_gt";
        case opcode::fcmp_ge: return "fcmp_ge";
        }
        return "unknown";
    }

    [[nodiscard]] inline const char* to_string(const scheduling_dependency_kind kind) {
        switch (kind) {
        case scheduling_dependency_kind::data: return "data";
        case scheduling_dependency_kind::control: return "control";
        case scheduling_dependency_kind::memory: return "memory";
        case scheduling_dependency_kind::phi: return "phi";
        case scheduling_dependency_kind::artificial: return "artificial";
        }
        return "unknown";
    }

    [[nodiscard]] inline const char* to_string(const scheduling_hazard_flag flag) {
        switch (flag) {
        case scheduling_hazard_flag::none: return "none";
        case scheduling_hazard_flag::read_after_write: return "read_after_write";
        case scheduling_hazard_flag::write_after_read: return "write_after_read";
        case scheduling_hazard_flag::write_after_write: return "write_after_write";
        case scheduling_hazard_flag::memory_alias: return "memory_alias";
        case scheduling_hazard_flag::call_barrier: return "call_barrier";
        case scheduling_hazard_flag::branch_barrier: return "branch_barrier";
        }
        return "unknown";
    }

    [[nodiscard]] inline const char* to_string(const memory_address_kind kind) {
        switch (kind) {
        case memory_address_kind::stack_frame: return "stack_frame";
        case memory_address_kind::spill_slot: return "spill_slot";
        case memory_address_kind::global_symbol: return "global_symbol";
        case memory_address_kind::argument_slot: return "argument_slot";
        case memory_address_kind::return_slot: return "return_slot";
        case memory_address_kind::computed_address: return "computed_address";
        }
        return "unknown";
    }

    [[nodiscard]] inline std::string dump_memory_address(const memory_address& address) {
        std::ostringstream os;
        os << "addr{" << to_string(address.kind);
        if (address.base.has_value()) {
            os << " base=" << address.base->name;
        }
        if (address.index.has_value()) {
            os << " index=" << address.index->name;
        }
        if (address.scale != 1) {
            os << " scale=" << address.scale;
        }
        if (address.displacement != 0) {
            os << " disp=" << address.displacement;
        }
        if (address.referenced_frame_object.has_value()) {
            os << " fobj=" << address.referenced_frame_object->value;
        }
        if (address.referenced_symbol.has_value()) {
            os << " symbol=" << *address.referenced_symbol;
        }
        os << "}";
        return os.str();
    }

    [[nodiscard]] inline std::string dump_memory_operand(const memory_operand& operand) {
        return "mem(" + dump_memory_address(operand.address) + ")";
    }

    [[nodiscard]] inline std::string dump_operand(const operand& op) {
        switch (op.type) {
        case operand::kind::none: return "_";
        case operand::kind::vreg: return "v" + std::to_string(std::get<vreg>(op.value).id);
        case operand::kind::ssa_value: return "s" + std::to_string(std::get<ssa_value_id>(op.value).id);
        case operand::kind::preg: return std::get<preg>(op.value).name;
        case operand::kind::argument_index: return "arg" + std::to_string(std::get<std::uint32_t>(op.value));
        case operand::kind::immediate_i64: return std::to_string(std::get<std::int64_t>(op.value));
        case operand::kind::immediate_f64: return std::to_string(std::get<double>(op.value));
        case operand::kind::symbol: return std::get<std::string>(op.value);
        case operand::kind::spill: return "spill" + std::to_string(std::get<spill_slot>(op.value).id);
        case operand::kind::memory: return dump_memory_operand(std::get<memory_operand>(op.value));
        case operand::kind::block: return "bb" + std::to_string(std::get<std::uint32_t>(op.value));
        }
        return "?";
    }

    [[nodiscard]] inline std::string dump_allocated_operand(const allocated_operand& op) {
        switch (op.type) {
        case allocated_operand::kind::none: return "_";
        case allocated_operand::kind::preg: return std::get<preg>(op.value).name;
        case allocated_operand::kind::argument_index: return "arg" + std::to_string(
                std::get<std::uint32_t>(op.value));
        case allocated_operand::kind::immediate_i64: return std::to_string(std::get<std::int64_t>(op.value));
        case allocated_operand::kind::immediate_f64: return std::to_string(std::get<double>(op.value));
        case allocated_operand::kind::symbol: return std::get<std::string>(op.value);
        case allocated_operand::kind::spill: return "spill" + std::to_string(std::get<spill_slot>(op.value).id);
        case allocated_operand::kind::memory: return dump_memory_operand(std::get<memory_operand>(op.value));
        case allocated_operand::kind::block: return "bb" + std::to_string(std::get<std::uint32_t>(op.value));
        }
        return "?";
    }

    [[nodiscard]] inline spill_slot make_unallocated_spill_slot(const std::uint32_t vreg_id) {
        spill_slot slot;
        // Reserve a synthetic spill-id range for unresolved vregs.
        slot.id = 0x80000000u | (vreg_id & 0x7fffffffu);
        slot.size = 8;
        slot.alignment = 8;
        slot.frame_offset = 0;
        return slot;
    }

    [[nodiscard]] inline allocated_operand map_allocated_operand(
        const operand& op,
        const std::unordered_map<std::uint32_t, register_assignment>& assignments,
        std::unordered_map<std::uint32_t, spill_slot>* fallback_spills = nullptr
    ) {
        switch (op.type) {
        case operand::kind::none:
            return allocated_operand{};
        case operand::kind::vreg: {
            const auto v = std::get<vreg>(op.value).id;
            if (const auto it = assignments.find(v); it != assignments.end()) {
                if (it->second.physical.has_value()) {
                    return allocated_operand::as_preg(*it->second.physical);
                }
                if (it->second.spill.has_value()) {
                    return allocated_operand::as_spill(*it->second.spill);
                }
            }
            if (fallback_spills != nullptr) {
                const auto [it, inserted] = fallback_spills->emplace(v, make_unallocated_spill_slot(v));
                (void)inserted;
                return allocated_operand::as_spill(it->second);
            }
            return allocated_operand::as_spill(make_unallocated_spill_slot(v));
        }
        case operand::kind::ssa_value:
            return allocated_operand::as_symbol("s" + std::to_string(std::get<ssa_value_id>(op.value).id));
        case operand::kind::preg:
            return allocated_operand::as_preg(std::get<preg>(op.value));
        case operand::kind::argument_index:
            return allocated_operand::as_argument_index(std::get<std::uint32_t>(op.value));
        case operand::kind::immediate_i64:
            return allocated_operand::as_i64(std::get<std::int64_t>(op.value));
        case operand::kind::immediate_f64:
            return allocated_operand::as_f64(std::get<double>(op.value));
        case operand::kind::symbol:
            return allocated_operand::as_symbol(std::get<std::string>(op.value));
        case operand::kind::spill:
            return allocated_operand::as_spill(std::get<spill_slot>(op.value));
        case operand::kind::memory:
            return allocated_operand::as_memory(std::get<memory_operand>(op.value));
        case operand::kind::block:
            return allocated_operand::as_block(std::get<std::uint32_t>(op.value));
        }
        return allocated_operand{};
    }

    [[nodiscard]] inline allocated_function_ir apply_register_allocation(
        const function_ir& fn,
        const register_allocation& allocation
    ) {
        allocated_function_ir out;
        out.name = fn.name;
        out.cfg = fn.cfg;
        out.assignments = allocation.assignments;
        out.spill_slots = allocation.spill_slots;
        out.original_vreg_ir = fn;
        out.blocks.reserve(fn.blocks.size());
        std::unordered_map<std::uint32_t, spill_slot> fallback_spills;

        for (const auto& block : fn.blocks) {
            allocated_basic_block allocated_block;
            allocated_block.id = block.id;
            allocated_block.name = block.name;
            allocated_block.arguments = block.arguments;
            allocated_block.phi_placeholders = block.phi_placeholders;
            allocated_block.predecessors = block.predecessors;
            allocated_block.successors = block.successors;
            allocated_block.instructions.reserve(block.instructions.size());

            for (const auto& inst : block.instructions) {
                allocated_instruction allocated_inst;
                allocated_inst.id = inst.id;
                allocated_inst.number = inst.number;
                allocated_inst.op = inst.op;
                allocated_inst.comment = inst.comment;
                allocated_inst.ssa_defs = inst.ssa_defs;
                allocated_inst.ssa_uses = inst.ssa_uses;
                allocated_inst.scheduling = inst.scheduling;
                allocated_inst.abstract_operation = inst.abstract_operation;
                allocated_inst.operation_attributes = inst.operation_attributes;
                allocated_inst.defs.reserve(inst.defs.size());
                allocated_inst.uses.reserve(inst.uses.size());

                for (const auto& d : inst.defs) {
                    allocated_inst.defs.push_back(map_allocated_operand(d, allocation.assignments, &fallback_spills));
                }
                for (const auto& u : inst.uses) {
                    allocated_inst.uses.push_back(map_allocated_operand(u, allocation.assignments, &fallback_spills));
                }

                allocated_block.instructions.push_back(std::move(allocated_inst));
            }

            out.blocks.push_back(std::move(allocated_block));
        }

        if (!fallback_spills.empty()) {
            std::vector<std::uint32_t> ids;
            ids.reserve(fallback_spills.size());
            for (const auto& [id, _] : fallback_spills) {
                (void)_;
                ids.push_back(id);
            }
            std::ranges::sort(ids);
            for (const auto id : ids) {
                out.spill_slots.push_back(fallback_spills.at(id));
            }
        }

        return out;
    }

    [[nodiscard]] inline mir::allocated_mir_function apply_register_allocation(
        const mir::virtual_mir_function& fn,
        const register_allocation& allocation
    ) {
        return mir::allocated_mir_function{apply_register_allocation(fn.function, allocation)};
    }

    [[nodiscard]] inline spill_rewrite_result rewrite_spills(
        allocated_function_ir&& fn,
        std::vector<preg> scratch_registers = default_scratch_pregs()
    ) {
        spill_rewrite_result out;
        out.function = std::move(fn);

        std::uint32_t next_generated_inst_id = 1;
        for (const auto& block : out.function.blocks) {
            for (const auto& inst : block.instructions) {
                next_generated_inst_id = std::max(next_generated_inst_id, inst.id + 1);
            }
        }

        if (scratch_registers.empty()) {
            out.diagnostics.emplace_back("rewrite_spills: no scratch registers available");
            return out;
        }

        for (auto& block : out.function.blocks) {
            std::vector<allocated_instruction> rewritten;
            rewritten.reserve(block.instructions.size());

            for (const auto& inst : block.instructions) {
                allocated_instruction rewritten_inst = inst;

                struct spilled_use_binding {
                    spill_slot slot;
                    preg temp;
                };
                struct spilled_def_binding {
                    std::size_t operand_index = 0;
                    spill_slot slot;
                    preg temp;
                };

                std::vector<spilled_use_binding> use_bindings;
                std::vector<spilled_def_binding> def_bindings;

                for (const auto& [type, value] : inst.uses) {
                    if (type != allocated_operand::kind::spill) {
                        continue;
                    }
                    const auto slot = std::get<spill_slot>(value);
                    const auto duplicate = std::find_if(use_bindings.begin(), use_bindings.end(),
                                                        [&](const spilled_use_binding& entry) {
                                                            return entry.slot.id == slot.id;
                                                        });
                    if (duplicate == use_bindings.end()) {
                        use_bindings.push_back(spilled_use_binding{slot, {}});
                    }
                }

                for (std::size_t i = 0; i < inst.defs.size(); ++i) {
                    const auto& def = inst.defs[i];
                    if (def.type != allocated_operand::kind::spill) {
                        continue;
                    }
                    def_bindings.push_back(spilled_def_binding{i, std::get<spill_slot>(def.value), {}});
                }

                const std::size_t needed_scratch = use_bindings.size() + def_bindings.size();
                if (needed_scratch > scratch_registers.size()) {
                    std::ostringstream diag;
                    diag << "rewrite_spills: instruction i" << inst.id
                        << " in bb" << block.id
                        << " needs " << needed_scratch
                        << " scratch registers but only " << scratch_registers.size() << " available";
                    out.diagnostics.push_back(diag.str());
                    rewritten.push_back(inst);
                    continue;
                }

                std::size_t scratch_index = 0;
                for (auto& binding : use_bindings) {
                    binding.temp = scratch_registers[scratch_index++];
                }
                for (auto& binding : def_bindings) {
                    binding.temp = scratch_registers[scratch_index++];
                }

                for (const auto& binding : use_bindings) {
                    allocated_instruction load;
                    load.id = next_generated_inst_id++;
                    load.op = opcode::load_spill;
                    load.defs = {allocated_operand::as_preg(binding.temp)};
                    load.uses = {allocated_operand::as_memory(make_memory_operand_for_spill_slot(binding.slot))};
                    load.comment = "spill reload";
                    rewritten.push_back(std::move(load));
                    ++out.inserted_loads;
                }

                for (auto& use : rewritten_inst.uses) {
                    if (use.type != allocated_operand::kind::spill) {
                        continue;
                    }
                    const auto slot = std::get<spill_slot>(use.value);
                    const auto it = std::find_if(use_bindings.begin(), use_bindings.end(),
                                                 [&](const spilled_use_binding& entry) {
                                                     return entry.slot.id == slot.id;
                                                 });
                    if (it != use_bindings.end()) {
                        use = allocated_operand::as_preg(it->temp);
                    }
                }

                for (const auto& binding : def_bindings) {
                    rewritten_inst.defs[binding.operand_index] = allocated_operand::as_preg(binding.temp);
                }

                rewritten.push_back(std::move(rewritten_inst));

                for (const auto& binding : def_bindings) {
                    allocated_instruction store;
                    store.id = next_generated_inst_id++;
                    store.op = opcode::store_spill;
                    store.defs = {allocated_operand::as_memory(make_memory_operand_for_spill_slot(binding.slot))};
                    store.uses = {allocated_operand::as_preg(binding.temp)};
                    store.comment = "spill store";
                    rewritten.push_back(std::move(store));
                    ++out.inserted_stores;
                }
            }

            block.instructions = std::move(rewritten);
        }

        return out;
    }

    [[nodiscard]] inline spill_rewrite_result rewrite_spills(
        const allocated_function_ir& fn,
        std::vector<preg> scratch_registers = default_scratch_pregs()
    ) {
        return rewrite_spills(allocated_function_ir{fn}, std::move(scratch_registers));
    }

    [[nodiscard]] inline mir::physical_mir_function rewrite_spills(
        const mir::allocated_mir_function& fn,
        std::vector<preg> scratch_registers = default_scratch_pregs()
    ) {
        const auto rewritten = rewrite_spills(fn.function, std::move(scratch_registers));
        mir::physical_mir_function out{
            rewritten.function,
            rewritten.inserted_loads,
            rewritten.inserted_stores,
            rewritten.diagnostics
        };

        out.spill_rewritten = true;
        for (const auto& block : out.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.id != 0) {
                    out.instruction_ids.insert(inst.id);
                }
                for (const auto& def : inst.defs) {
                    if (def.type == allocated_operand::kind::spill) {
                        out.referenced_spill_slots.insert(std::get<spill_slot>(def.value).id);
                    }
                }
                for (const auto& use : inst.uses) {
                    if (use.type == allocated_operand::kind::spill) {
                        out.referenced_spill_slots.insert(std::get<spill_slot>(use.value).id);
                    }
                }
            }
        }

        const auto verification = verify_physical_mir(out);
        out.verified = verification.ok();
        out.verification_diagnostics = verification.diagnostics;
        return out;
    }

    [[nodiscard]] inline use_def_analysis analyze_use_def(const function_ir& fn) {
        use_def_analysis out;
        for (const auto& block : fn.blocks) {
            auto& info = out.per_block[block.id];
            for (const auto& inst : block.instructions) {
                for (const auto& use : inst.uses) {
                    if (const auto reg = as_vreg_id(use); reg.has_value() && !info.defs.contains(*reg)) {
                        info.uses.insert(*reg);
                    }
                }
                for (const auto& def : inst.defs) {
                    if (const auto reg = as_vreg_id(def); reg.has_value()) {
                        info.defs.insert(*reg);
                    }
                }
            }
        }
        return out;
    }

    [[nodiscard]] inline liveness_analysis analyze_liveness(const function_ir& fn) {
        liveness_analysis out;
        const auto use_def = analyze_use_def(fn);

        for (const auto& block : fn.blocks) {
            out.per_block[block.id] = {};
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& block : std::views::reverse(fn.blocks)) {
                auto prev_in = out.per_block[block.id].live_in;
                auto prev_out = out.per_block[block.id].live_out;

                std::unordered_set<std::uint32_t> new_out;
                for (const auto succ_id : block.successors) {
                    const auto succ_it = out.per_block.find(succ_id);
                    if (succ_it != out.per_block.end()) {
                        new_out.insert(succ_it->second.live_in.begin(), succ_it->second.live_in.end());
                    }
                }

                std::unordered_set<std::uint32_t> new_in = use_def.per_block.at(block.id).uses;
                for (const auto reg : new_out) {
                    if (!use_def.per_block.at(block.id).defs.contains(reg)) {
                        new_in.insert(reg);
                    }
                }

                if (new_in != prev_in || new_out != prev_out) {
                    changed = true;
                }
                out.per_block[block.id].live_in = std::move(new_in);
                out.per_block[block.id].live_out = std::move(new_out);
            }
        }

        for (const auto& block : fn.blocks) {
            std::unordered_set<std::uint32_t> live = out.per_block[block.id].live_out;
            auto& trace = out.per_instruction[block.id];
            trace.resize(block.instructions.size());
            for (std::size_t i = block.instructions.size(); i-- > 0;) {
                const auto& inst = block.instructions[i];
                instruction_liveness state;
                state.instruction_id = inst.id;
                state.live_out = live;

                for (const auto& def : inst.defs) {
                    if (const auto reg = as_vreg_id(def); reg.has_value()) {
                        live.erase(*reg);
                    }
                }
                for (const auto& use : inst.uses) {
                    if (const auto reg = as_vreg_id(use); reg.has_value()) {
                        live.insert(*reg);
                    }
                }

                state.live_in = live;
                trace[i] = std::move(state);
            }
        }

        return out;
    }

    [[nodiscard]] inline std::vector<preg> default_pregs() {
        return {{0, "r0"}, {1, "r1"}, {2, "r2"}, {3, "r3"}, {4, "r4"}, {5, "r5"}, {6, "r6"}, {7, "r7"}};
    }

    [[nodiscard]] inline std::vector<live_interval> build_live_intervals(
        const function_ir& fn,
        const std::optional<liveness_analysis>& liveness = std::nullopt
    ) {
        std::unordered_map<std::uint32_t, live_interval> by_vreg;
        std::size_t pos = 0;

        auto touch = [&](const std::uint32_t reg, const std::size_t p) {
            auto& interval = by_vreg[reg];
            if (interval.vreg_id == 0) {
                interval.vreg_id = reg;
                interval.start = p;
                interval.end = p;
                return;
            }
            interval.start = std::min(interval.start, p);
            interval.end = std::max(interval.end, p);
        };

        for (const auto& block : fn.blocks) {
            std::optional<std::reference_wrapper<const std::vector<instruction_liveness>>> inst_live;
            if (liveness.has_value()) {
                if (const auto it = liveness->per_instruction.find(block.id); it != liveness->per_instruction.end()) {
                    inst_live = std::cref(it->second);
                }
            }

            for (std::size_t i = 0; i < block.instructions.size(); ++i, ++pos) {
                const auto& inst = block.instructions[i];
                for (const auto& def : inst.defs) {
                    if (const auto reg = as_vreg_id(def); reg.has_value()) {
                        touch(*reg, pos);
                    }
                }
                for (const auto& use : inst.uses) {
                    if (const auto reg = as_vreg_id(use); reg.has_value()) {
                        touch(*reg, pos);
                    }
                }
                if (inst_live.has_value() && i < inst_live->get().size()) {
                    for (const auto reg : inst_live->get()[i].live_out) {
                        touch(reg, pos + 1);
                    }
                }
            }
        }

        std::vector<live_interval> intervals;
        intervals.reserve(by_vreg.size());
        for (const auto& [id, interval] : by_vreg) {
            (void)id;
            intervals.push_back(interval);
        }

        std::ranges::sort(intervals, [](const live_interval& a, const live_interval& b) {
            if (a.start != b.start) {
                return a.start < b.start;
            }
            return a.vreg_id < b.vreg_id;
        });
        return intervals;
    }

    [[nodiscard]] inline std::size_t default_latency_for_opcode(const opcode op) {
        switch (op) {
        case opcode::load:
        case opcode::store:
        case opcode::load_spill:
        case opcode::store_spill:
        case opcode::load_symbol: return 3;
        case opcode::mul: return 4;
        case opcode::div:
        case opcode::mod: return 10;
        case opcode::call: return 8;
        case opcode::branch:
        case opcode::branch_cond:
        case opcode::ret: return 1;
        default: return 1;
        }
    }

    [[nodiscard]] inline double default_throughput_for_opcode(const opcode op) {
        switch (op) {
        case opcode::div:
        case opcode::mod: return 0.2;
        case opcode::mul: return 0.5;
        case opcode::call: return 0.1;
        default: return 1.0;
        }
    }

    [[nodiscard]] inline std::string default_scheduling_class_for_opcode(const opcode op) {
        switch (op) {
        case opcode::load:
        case opcode::store:
        case opcode::load_spill:
        case opcode::store_spill: return "memory";
        case opcode::call: return "call";
        case opcode::branch:
        case opcode::branch_cond:
        case opcode::ret: return "control";
        case opcode::mul:
        case opcode::div:
        case opcode::mod: return "arithmetic_high_latency";
        default: return "arithmetic";
        }
    }

    [[nodiscard]] inline bool instruction_is_memory_like(const instruction& inst) {
        return inst.op == opcode::load
            || inst.op == opcode::store
            || inst.op == opcode::load_spill
            || inst.op == opcode::store_spill
            || inst.op == opcode::call;
    }

    [[nodiscard]] inline bool instruction_is_control_like(const instruction& inst) {
        return inst.op == opcode::branch
            || inst.op == opcode::branch_cond
            || inst.op == opcode::ret;
    }

    [[nodiscard]] inline std::optional<instruction_scheduling_metadata> query_instruction_scheduling_metadata(
        const function_ir& fn,
        const std::uint32_t instruction_id
    ) {
        for (const auto& block : fn.blocks) {
            const auto it = std::ranges::find_if(block.instructions, [&](const instruction& inst) {
                return inst.id == instruction_id;
            });
            if (it != block.instructions.end()) {
                return it->scheduling;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<instruction_scheduling_metadata> query_instruction_scheduling_metadata(
        const mir::virtual_mir_function& fn,
        const std::uint32_t instruction_id
    ) {
        return query_instruction_scheduling_metadata(fn.function, instruction_id);
    }

    [[nodiscard]] inline bool update_instruction_scheduling_metadata(
        function_ir& fn,
        const std::uint32_t instruction_id,
        const instruction_scheduling_metadata& metadata
    ) {
        for (auto& block : fn.blocks) {
            const auto it = std::ranges::find_if(block.instructions, [&](const instruction& inst) {
                return inst.id == instruction_id;
            });
            if (it != block.instructions.end()) {
                it->scheduling = metadata;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] inline bool update_instruction_scheduling_metadata(
        mir::virtual_mir_function& fn,
        const std::uint32_t instruction_id,
        const instruction_scheduling_metadata& metadata
    ) {
        return update_instruction_scheduling_metadata(fn.function, instruction_id, metadata);
    }

    [[nodiscard]] inline bool annotate_instruction_scheduling_metadata(
        function_ir& fn,
        const std::uint32_t instruction_id,
        std::string key,
        std::string value
    ) {
        for (auto& block : fn.blocks) {
            const auto it = std::ranges::find_if(block.instructions, [&](const instruction& inst) {
                return inst.id == instruction_id;
            });
            if (it != block.instructions.end()) {
                it->scheduling.annotations[std::move(key)] = std::move(value);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] inline bool annotate_instruction_scheduling_metadata(
        mir::virtual_mir_function& fn,
        const std::uint32_t instruction_id,
        std::string key,
        std::string value
    ) {
        return annotate_instruction_scheduling_metadata(fn.function, instruction_id, std::move(key), std::move(value));
    }

    [[nodiscard]] inline scheduling_metadata_result compute_scheduling_metadata(const function_ir& fn) {
        scheduling_metadata_result out;
        std::unordered_map<std::uint32_t, std::uint32_t> latest_def_by_vreg;
        std::optional<std::uint32_t> latest_memory_inst;

        auto add_edge = [&](const std::uint32_t src, const std::uint32_t dst, const scheduling_dependency_kind kind,
                            const std::size_t latency, std::uint32_t block_id) {
            if (src == 0 || dst == 0 || src == dst) {
                return;
            }
            out.dependency_edges.emplace_back(scheduling_dependency_edge{src, dst, kind, latency, false, block_id});
        };

        for (const auto& block : fn.blocks) {
            std::optional<std::uint32_t> previous_inst;
            for (const auto& inst : block.instructions) {
                instruction_scheduling_metadata metadata = inst.scheduling;
                if (metadata.latency == 0) {
                    metadata.latency = default_latency_for_opcode(inst.op);
                }
                if (metadata.throughput <= 0.0) {
                    metadata.throughput = default_throughput_for_opcode(inst.op);
                }
                if (metadata.scheduling_class.empty()) {
                    metadata.scheduling_class = default_scheduling_class_for_opcode(inst.op);
                }

                if (metadata.scheduling_groups.empty()) {
                    metadata.scheduling_groups.emplace_back("bb" + std::to_string(block.id));
                }
                metadata.scheduling_priority = static_cast<std::int32_t>(
                    100 - std::min<std::size_t>(metadata.latency, 90));

                if (metadata.annotations.find("opcode") == metadata.annotations.end()) {
                    metadata.annotations["opcode"] = to_string(inst.op);
                }
                if (metadata.annotations.find("block") == metadata.annotations.end()) {
                    metadata.annotations["block"] = std::to_string(block.id);
                }

                if (instruction_is_control_like(inst)) {
                    metadata.hazard_flags.emplace_back(scheduling_hazard_flag::branch_barrier);
                    metadata.scheduling_constraints.emplace_back(scheduling_constraint{"must_end_block", "true"});
                }
                if (instruction_is_memory_like(inst)) {
                    metadata.hazard_flags.emplace_back(scheduling_hazard_flag::memory_alias);
                }
                if (inst.op == opcode::call) {
                    metadata.hazard_flags.emplace_back(scheduling_hazard_flag::call_barrier);
                    metadata.scheduling_constraints.emplace_back(scheduling_constraint{
                        "preserve_calling_convention", "true"
                    });
                }

                for (const auto& use : inst.uses) {
                    const auto reg = as_vreg_id(use);
                    if (!reg.has_value()) {
                        continue;
                    }
                    if (const auto def_it = latest_def_by_vreg.find(*reg); def_it != latest_def_by_vreg.end()) {
                        add_edge(def_it->second, inst.id, scheduling_dependency_kind::data, metadata.latency, block.id);
                    }
                }

                if (instruction_is_memory_like(inst) && latest_memory_inst.has_value()) {
                    add_edge(*latest_memory_inst, inst.id, scheduling_dependency_kind::memory, metadata.latency,
                             block.id);
                }
                if (previous_inst.has_value()) {
                    add_edge(*previous_inst, inst.id, scheduling_dependency_kind::artificial, 0, block.id);
                }

                for (const auto& def : inst.defs) {
                    const auto reg = as_vreg_id(def);
                    if (reg.has_value()) {
                        latest_def_by_vreg[*reg] = inst.id;
                    }
                }

                if (instruction_is_memory_like(inst)) {
                    latest_memory_inst = inst.id;
                }

                out.scheduling_classes[metadata.scheduling_class] += 1;
                for (const auto hazard : metadata.hazard_flags) {
                    out.hazard_flags[to_string(hazard)] += 1;
                }

                out.diagnostics.emplace_back(
                    "visualization",
                    "bb" + std::to_string(block.id)
                    + ":i" + std::to_string(inst.id)
                    + " class=" + metadata.scheduling_class
                    + " lat=" + std::to_string(metadata.latency)
                    + " tput=" + std::to_string(metadata.throughput)
                );

                out.instruction_metadata[inst.id] = std::move(metadata);
                previous_inst = inst.id;
            }
        }

        for (const auto& edge : out.dependency_edges) {
            out.instruction_metadata[edge.target_instruction_id].dependency_predecessors.emplace_back(
                edge.source_instruction_id);
            out.instruction_metadata[edge.source_instruction_id].dependency_successors.emplace_back(
                edge.target_instruction_id);
        }

        out.statistics["instruction_count"] = static_cast<double>(out.instruction_metadata.size());
        out.statistics["dependency_edge_count"] = static_cast<double>(out.dependency_edges.size());
        out.statistics["scheduling_class_count"] = static_cast<double>(out.scheduling_classes.size());
        out.statistics["hazard_flag_kinds"] = static_cast<double>(out.hazard_flags.size());

        out.diagnostics.emplace_back("heuristic", "seed dependency edges from data/control/memory relationships only");
        out.diagnostics.emplace_back("heuristic",
                                     "derive latency/throughput from opcode classes until target models exist");

        return out;
    }

    [[nodiscard]] inline scheduling_metadata_result compute_scheduling_metadata(const mir::virtual_mir_function& fn) {
        return compute_scheduling_metadata(fn.function);
    }

    [[nodiscard]] inline scheduling_metadata_result update_scheduling_metadata(function_ir& fn) {
        auto out = compute_scheduling_metadata(fn);
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                if (const auto it = out.instruction_metadata.find(inst.id); it != out.instruction_metadata.end()) {
                    inst.scheduling = it->second;
                    out.update_log.emplace_back("updated i" + std::to_string(inst.id));
                }
            }
        }
        return out;
    }

    [[nodiscard]] inline scheduling_metadata_result update_scheduling_metadata(mir::virtual_mir_function& fn) {
        return update_scheduling_metadata(fn.function);
    }

    [[nodiscard]] inline scheduling_metadata_validation_result validate_scheduling_metadata(const function_ir& fn) {
        scheduling_metadata_validation_result out;
        std::unordered_set<std::uint32_t> known_ids;
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                known_ids.insert(inst.id);
                if (inst.scheduling.latency == 0) {
                    out.diagnostics.emplace_back("instruction i" + std::to_string(inst.id) + " has zero latency");
                }
                if (inst.scheduling.throughput <= 0.0) {
                    out.diagnostics.emplace_back(
                        "instruction i" + std::to_string(inst.id) + " has non-positive throughput");
                }
                if (inst.scheduling.scheduling_class.empty()) {
                    out.warnings.
                        emplace_back("instruction i" + std::to_string(inst.id) + " has empty scheduling class");
                }
                for (const auto pred : inst.scheduling.dependency_predecessors) {
                    if (pred == inst.id) {
                        out.diagnostics.
                            emplace_back("instruction i" + std::to_string(inst.id) + " has self dependency");
                    }
                }
            }
        }

        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                for (const auto pred : inst.scheduling.dependency_predecessors) {
                    if (!known_ids.contains(pred)) {
                        out.warnings.emplace_back(
                            "instruction i" + std::to_string(inst.id) + " references unknown predecessor i"
                            + std::to_string(pred)
                        );
                    }
                }
            }
        }

        return out;
    }

    [[nodiscard]] inline scheduling_metadata_validation_result validate_scheduling_metadata(
        const mir::virtual_mir_function& fn
    ) {
        return validate_scheduling_metadata(fn.function);
    }

    [[nodiscard]] inline std::string dump_scheduling_metadata(const scheduling_metadata_result& metadata) {
        std::ostringstream os;
        os << "scheduling-metadata"
            << " instructions=" << metadata.instruction_metadata.size()
            << " dependency_edges=" << metadata.dependency_edges.size()
            << " classes=" << metadata.scheduling_classes.size() << "\n";

        if (!metadata.scheduling_classes.empty()) {
            os << "  classes";
            std::vector<std::pair<std::string, std::size_t>> classes;
            classes.reserve(metadata.scheduling_classes.size());
            for (const auto& [name, count] : metadata.scheduling_classes) {
                classes.emplace_back(name, count);
            }
            std::sort(classes.begin(), classes.end(), [](const auto& lhs, const auto& rhs) {
                if (lhs.second != rhs.second) {
                    return lhs.second > rhs.second;
                }
                return lhs.first < rhs.first;
            });
            for (const auto& [name, count] : classes) {
                os << " " << name << "=" << count;
            }
            os << "\n";
        }

        if (!metadata.dependency_edges.empty()) {
            const auto limit = std::min<std::size_t>(metadata.dependency_edges.size(), 12);
            os << "  edges";
            for (std::size_t i = 0; i < limit; ++i) {
                const auto& edge = metadata.dependency_edges[i];
                os << " i" << edge.source_instruction_id
                    << "->i" << edge.target_instruction_id
                    << "(" << to_string(edge.kind) << ":lat=" << edge.latency << ")";
            }
            if (metadata.dependency_edges.size() > limit) {
                os << " ...";
            }
            os << "\n";
        }

        for (const auto& warning : metadata.warnings) {
            os << "  warning: " << warning << "\n";
        }

        return os.str();
    }

    [[nodiscard]] inline std::string dump_scheduling_metadata(const function_ir& fn) {
        return dump_scheduling_metadata(compute_scheduling_metadata(fn));
    }

    [[nodiscard]] inline std::string dump_scheduling_metadata(const mir::virtual_mir_function& fn) {
        return dump_scheduling_metadata(compute_scheduling_metadata(fn));
    }

    [[nodiscard]] inline register_pressure_result compute_register_pressure(
        const function_ir& fn,
        std::size_t hotspot_threshold = 0
    ) {
        register_pressure_result out;
        const auto liveness = analyze_liveness(fn);
        out.live_ranges = build_live_intervals(fn, liveness);

        const auto allocatable_count = default_pregs().size();
        const auto warning_threshold = std::max<std::size_t>(1, allocatable_count * 8 / 10);
        const auto critical_threshold = std::max<std::size_t>(1, allocatable_count);

        out.statistics["block_count"] = static_cast<double>(fn.blocks.size());
        out.statistics["live_range_count"] = static_cast<double>(out.live_ranges.size());
        out.statistics["allocatable_registers"] = static_cast<double>(allocatable_count);

        struct pressure_point {
            std::uint32_t block_id = 0;
            std::uint32_t instruction_id = 0;
            std::size_t live_registers = 0;
        };
        std::vector<pressure_point> points;
        std::unordered_map<std::uint32_t, std::size_t> vreg_live_frequency;
        std::unordered_map<std::uint32_t, register_class> inferred_class_by_vreg;

        auto classify_operand = [](const operand& op) {
            if (op.type == operand::kind::immediate_f64) {
                return register_class::floating;
            }
            return register_class::integer;
        };

        auto class_name = [](const register_class cls) -> std::string {
            switch (cls) {
            case register_class::integer: return "integer";
            case register_class::floating: return "floating";
            case register_class::vector: return "vector";
            }
            return "unknown";
        };

        for (const auto& block : fn.blocks) {
            block_register_pressure block_pressure;
            block_pressure.block_id = block.id;
            std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> block_ranges;

            const auto it = liveness.per_instruction.find(block.id);
            if (it != liveness.per_instruction.end()) {
                const auto& trace = it->second;
                const auto count = std::min(block.instructions.size(), trace.size());
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& inst = block.instructions[i];
                    const auto& state = trace[i];
                    auto inst_class = register_class::integer;
                    for (const auto& use : inst.uses) {
                        if (classify_operand(use) == register_class::floating) {
                            inst_class = register_class::floating;
                            break;
                        }
                    }
                    const auto live = std::max(state.live_in.size(), state.live_out.size());
                    block_pressure.max_live_registers = std::max(block_pressure.max_live_registers, live);
                    block_pressure.live_registers_by_instruction[inst.id] = live;
                    points.push_back(pressure_point{block.id, inst.id, live});
                    out.max_live_registers = std::max(out.max_live_registers, live);

                    for (const auto reg : state.live_in) {
                        ++vreg_live_frequency[reg];
                        inferred_class_by_vreg.try_emplace(reg, inst_class);
                        auto& range = block_ranges[reg];
                        if (range.first == 0) {
                            range.first = inst.id;
                            range.second = inst.id;
                        }
                        else {
                            range.second = inst.id;
                        }
                    }
                    for (const auto reg : state.live_out) {
                        ++vreg_live_frequency[reg];
                        inferred_class_by_vreg.try_emplace(reg, inst_class);
                        auto& range = block_ranges[reg];
                        if (range.first == 0) {
                            range.first = inst.id;
                            range.second = inst.id;
                        }
                        else {
                            range.second = inst.id;
                        }
                    }

                    out.diagnostics.emplace_back(
                        "visualization",
                        "bb" + std::to_string(block.id) + ":i" + std::to_string(inst.id) +
                        " live=" + std::to_string(live)
                    );
                }
            }

            std::vector<block_live_range> block_live_ranges;
            block_live_ranges.reserve(block_ranges.size());
            for (const auto& [vreg_id, endpoints] : block_ranges) {
                block_live_ranges.push_back(block_live_range{
                    block.id,
                    vreg_id,
                    endpoints.first,
                    endpoints.second,
                    endpoints.first == 0 || endpoints.second == 0
                        ? 0
                        : 1 + std::size_t(endpoints.second - endpoints.first)
                });
            }
            std::sort(block_live_ranges.begin(), block_live_ranges.end(), [](const block_live_range& lhs,
                                                                             const block_live_range& rhs) {
                if (lhs.start_instruction_id != rhs.start_instruction_id) {
                    return lhs.start_instruction_id < rhs.start_instruction_id;
                }
                return lhs.vreg_id < rhs.vreg_id;
            });
            out.live_ranges_per_block[block.id] = std::move(block_live_ranges);

            out.per_block[block.id] = std::move(block_pressure);
        }

        const auto threshold = hotspot_threshold == 0 ? out.max_live_registers : hotspot_threshold;
        out.hotspot_threshold_used = threshold;
        out.thresholds_used.hotspot = threshold;
        out.thresholds_used.warning = warning_threshold;
        out.thresholds_used.critical = critical_threshold;
        out.thresholds_used.spill_candidate = std::max<std::size_t>(2, warning_threshold);
        for (const auto& point : points) {
            if (point.live_registers == 0) {
                continue;
            }
            if (point.live_registers < threshold) {
                continue;
            }
            out.hotspots.push_back(register_pressure_hotspot{
                point.block_id,
                point.instruction_id,
                point.live_registers
            });
        }

        std::sort(out.hotspots.begin(), out.hotspots.end(), [](const register_pressure_hotspot& lhs,
                                                               const register_pressure_hotspot& rhs) {
            if (lhs.live_registers != rhs.live_registers) {
                return lhs.live_registers > rhs.live_registers;
            }
            if (lhs.block_id != rhs.block_id) {
                return lhs.block_id < rhs.block_id;
            }
            return lhs.instruction_id < rhs.instruction_id;
        });

        // Current MIR models vregs as scalar integer-like values; class buckets are placeholders for future typing.
        out.register_classes_under_pressure["integer"] = 0;
        out.register_classes_under_pressure["floating"] = 0;
        out.register_classes_under_pressure["vector"] = 0;
        out.register_classes_under_pressure["unknown"] = 0;
        for (const auto& [vreg_id, freq] : vreg_live_frequency) {
            (void)freq;
            const auto cls_it = inferred_class_by_vreg.find(vreg_id);
            if (cls_it == inferred_class_by_vreg.end()) {
                ++out.register_classes_under_pressure["unknown"];
                continue;
            }
            ++out.register_classes_under_pressure[class_name(cls_it->second)];
        }

        // Build a simple overlap graph from live-interval intersections.
        for (std::size_t i = 0; i < out.live_ranges.size(); ++i) {
            const auto& a = out.live_ranges[i];
            for (std::size_t j = i + 1; j < out.live_ranges.size(); ++j) {
                const auto& b = out.live_ranges[j];
                const bool overlaps = !(a.end < b.start || b.end < a.start);
                if (!overlaps) {
                    continue;
                }
                out.pressure_graph[a.vreg_id].insert(b.vreg_id);
                out.pressure_graph[b.vreg_id].insert(a.vreg_id);
            }
        }

        std::vector<std::pair<std::uint32_t, std::size_t>> candidate_scores;
        candidate_scores.reserve(vreg_live_frequency.size());
        for (const auto& [vreg_id, freq] : vreg_live_frequency) {
            const auto degree = out.pressure_graph.contains(vreg_id) ? out.pressure_graph[vreg_id].size() : 0;
            candidate_scores.emplace_back(vreg_id, freq + degree);
        }
        std::sort(candidate_scores.begin(), candidate_scores.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            return lhs.first < rhs.first;
        });
        const auto top_candidates = std::min<std::size_t>(candidate_scores.size(), 8);
        out.spill_candidates.reserve(top_candidates);
        for (std::size_t i = 0; i < top_candidates; ++i) {
            out.spill_candidates.push_back(candidate_scores[i].first);
        }

        out.statistics["hotspot_count"] = static_cast<double>(out.hotspots.size());
        out.statistics["spill_candidate_count"] = static_cast<double>(out.spill_candidates.size());
        out.statistics["pressure_graph_nodes"] = static_cast<double>(out.pressure_graph.size());
        std::size_t pressure_edges = 0;
        for (const auto& neighbors : out.pressure_graph | std::views::values) {
            pressure_edges += neighbors.size();
        }
        out.statistics["pressure_graph_edges"] = static_cast<double>(pressure_edges / 2);
        out.statistics["threshold_used"] = static_cast<double>(out.hotspot_threshold_used);
        out.statistics["warning_threshold"] = static_cast<double>(out.thresholds_used.warning);
        out.statistics["critical_threshold"] = static_cast<double>(out.thresholds_used.critical);
        out.statistics["hotspot_density"] = points.empty()
                                                ? 0.0
                                                : static_cast<double>(out.hotspots.size()) / static_cast<double>(points.
                                                    size());
        out.statistics["block_live_range_sets"] = static_cast<double>(out.live_ranges_per_block.size());

        if (out.max_live_registers > critical_threshold) {
            out.warnings.emplace_back("register pressure exceeds default allocatable register count; spills are likely"
            );
        }
        else if (out.max_live_registers >= warning_threshold) {
            out.warnings.emplace_back(
                "register pressure is near register-capacity threshold; review hotspot instructions"
            );
        }
        if (out.hotspots.empty()) {
            out.warnings.emplace_back("no register-pressure hotspots at current threshold");
        }

        out.diagnostics.emplace_back("heuristic", "rank spill candidates by live-frequency + interference degree");
        out.diagnostics.emplace_back("heuristic", "treat threshold as adaptive cap for hotspot reporting");
        out.diagnostics.emplace_back("reduction", "split long live ranges at control-flow boundaries");
        out.diagnostics.emplace_back("reduction", "rematerialize cheap constants instead of preserving registers");

        // Populate dedicated analysis fields from computed data.
        for (const auto& [vreg_id, freq] : vreg_live_frequency) {
            const auto degree = out.pressure_graph.contains(vreg_id) ? out.pressure_graph.at(vreg_id).size() : 0;
            out.visualization_data.push_back(
                "vreg" + std::to_string(vreg_id) +
                " freq=" + std::to_string(freq) +
                " degree=" + std::to_string(degree)
            );
        }
        for (const auto& hs : out.hotspots) {
            out.visualization_data.push_back(
                "hotspot bb" + std::to_string(hs.block_id) +
                ":i" + std::to_string(hs.instruction_id) +
                " live=" + std::to_string(hs.live_registers)
            );
        }

        out.heuristics.emplace_back("rank spill candidates by live-frequency + interference degree");
        out.heuristics.emplace_back("treat threshold as adaptive cap for hotspot reporting");
        out.heuristics.emplace_back("prefer splitting over spilling for vregs with degree > 2");

        for (const auto& vreg_id : out.spill_candidates) {
            out.reduction_opportunities.push_back("spill vreg" + std::to_string(vreg_id));
        }
        for (const auto& lr : out.live_ranges) {
            if (lr.end > lr.start + 2) {
                out.reduction_opportunities.push_back(
                    "split vreg" + std::to_string(lr.vreg_id) +
                    " range [" + std::to_string(lr.start) + "," + std::to_string(lr.end) + "]"
                );
            }
        }

        out.reduction_suggestions.emplace_back("rematerialize cheap constants instead of preserving registers");
        out.reduction_suggestions.emplace_back("split long live ranges at control-flow boundaries");
        out.reduction_suggestions.emplace_back("coalesce copy-related vregs to eliminate moves");

        out.reduction_transformations.emplace_back("live-range splitting");
        out.reduction_transformations.emplace_back("rematerialization");
        out.reduction_transformations.emplace_back("register coalescing");
        out.reduction_transformations.emplace_back("spill-code insertion");

        out.reduction_rules.emplace_back("spill highest-frequency vreg first");
        out.reduction_rules.emplace_back("prefer callee-saved regs for long-lived values");
        out.reduction_rules.emplace_back("do not split vreg across loop back-edges");

        out.reduction_patterns.emplace_back("high-degree interference cluster");
        out.reduction_patterns.emplace_back("loop-carried dependency chain");
        out.reduction_patterns.emplace_back("call-site register clobber boundary");

        out.reduction_costs["spill_load"] = 2.0;
        out.reduction_costs["spill_store"] = 2.0;
        out.reduction_costs["rematerialize"] = 1.0;
        out.reduction_costs["split"] = 0.5;
        out.reduction_costs["coalesce"] = 0.0;

        out.feedback_for_scheduling.push_back(
            "max_live=" + std::to_string(out.max_live_registers) +
            " hotspots=" + std::to_string(out.hotspots.size()) +
            " spill_candidates=" + std::to_string(out.spill_candidates.size())
        );
        for (const auto& vreg_id : out.spill_candidates) {
            out.feedback_for_scheduling.push_back("prefer_early_def vreg" + std::to_string(vreg_id));
        }
        if (out.max_live_registers >= warning_threshold) {
            out.feedback_for_scheduling.emplace_back("schedule_to_reduce_live_ranges");
        }

        return out;
    }

    [[nodiscard]] inline register_pressure_result compute_register_pressure(
        const mir::virtual_mir_function& fn,
        const std::size_t hotspot_threshold = 0
    ) {
        return compute_register_pressure(fn.function, hotspot_threshold);
    }

    [[nodiscard]] inline std::string dump_register_pressure(const register_pressure_result& pressure) {
        std::ostringstream os;
        os << "register-pressure"
            << " max_live=" << pressure.max_live_registers
            << " hotspots=" << pressure.hotspots.size()
            << " spill_candidates=" << pressure.spill_candidates.size() << "\n";
        os << "  thresholds"
            << " hotspot=" << pressure.thresholds_used.hotspot
            << " warning=" << pressure.thresholds_used.warning
            << " critical=" << pressure.thresholds_used.critical
            << " spill_candidate=" << pressure.thresholds_used.spill_candidate << "\n";

        if (!pressure.register_classes_under_pressure.empty()) {
            std::vector<std::pair<std::string, std::size_t>> classes;
            classes.reserve(pressure.register_classes_under_pressure.size());
            for (const auto& [name, count] : pressure.register_classes_under_pressure) {
                classes.emplace_back(name, count);
            }
            std::sort(classes.begin(), classes.end(), [](const auto& lhs, const auto& rhs) {
                if (lhs.second != rhs.second) {
                    return lhs.second > rhs.second;
                }
                return lhs.first < rhs.first;
            });
            os << "  classes";
            for (const auto& [name, count] : classes) {
                os << " " << name << "=" << count;
            }
            os << "\n";
        }

        if (!pressure.per_block.empty()) {
            std::vector<std::pair<std::uint32_t, std::size_t>> blocks;
            blocks.reserve(pressure.per_block.size());
            for (const auto& [block_id, block] : pressure.per_block) {
                blocks.emplace_back(block_id, block.max_live_registers);
            }
            std::sort(blocks.begin(), blocks.end(), [](const auto& lhs, const auto& rhs) {
                if (lhs.second != rhs.second) {
                    return lhs.second > rhs.second;
                }
                return lhs.first < rhs.first;
            });
            os << "  per-block-max";
            const auto limit = std::min<std::size_t>(blocks.size(), 8);
            for (std::size_t i = 0; i < limit; ++i) {
                os << " bb" << blocks[i].first << "=" << blocks[i].second;
            }
            if (blocks.size() > limit) {
                os << " ...";
            }
            os << "\n";
        }

        if (!pressure.hotspots.empty()) {
            os << "  hotspots";
            const auto limit = std::min<std::size_t>(pressure.hotspots.size(), 8);
            for (std::size_t i = 0; i < limit; ++i) {
                const auto& spot = pressure.hotspots[i];
                os << " bb" << spot.block_id << ":i" << spot.instruction_id << "=" << spot.live_registers;
            }
            if (pressure.hotspots.size() > limit) {
                os << " ...";
            }
            os << "\n";
        }

        if (!pressure.spill_candidates.empty()) {
            os << "  spill-candidates";
            for (const auto vreg_id : pressure.spill_candidates) {
                os << " v" << vreg_id;
            }
            os << "\n";
        }

        if (!pressure.warnings.empty()) {
            for (const auto& warning : pressure.warnings) {
                os << "  warning: " << warning << "\n";
            }
        }

        return os.str();
    }

    [[nodiscard]] inline std::string dump_register_pressure(
        const function_ir& fn,
        const std::size_t hotspot_threshold = 0
    ) {
        return dump_register_pressure(compute_register_pressure(fn, hotspot_threshold));
    }

    [[nodiscard]] inline std::string dump_register_pressure(
        const mir::virtual_mir_function& fn,
        const std::size_t hotspot_threshold = 0
    ) {
        return dump_register_pressure(compute_register_pressure(fn, hotspot_threshold));
    }

    [[nodiscard]] inline register_allocation allocate_registers(function_ir& fn,
                                                                std::vector<preg> allocatable = default_pregs()) {
        const auto liveness = analyze_liveness(fn);
        const auto intervals = build_live_intervals(fn, liveness);

        register_allocation out;
        std::vector<spill_slot> fresh_spills;

        struct active_interval {
            live_interval interval;
            preg assigned;
        };
        std::vector<active_interval> active;

        auto expire_old = [&](const std::size_t start) {
            std::vector<active_interval> next;
            for (const auto& entry : active) {
                if (entry.interval.end < start) {
                    allocatable.push_back(entry.assigned);
                }
                else {
                    next.push_back(entry);
                }
            }
            active = std::move(next);
        };

        auto make_spill = [&]() {
            const auto spill_id = fn.next_spill_id++;
            spill_slot slot;
            slot.id = spill_id;
            slot.size = 8;
            slot.alignment = 8;
            slot.frame_offset = -static_cast<std::int64_t>(spill_id * slot.size);
            fresh_spills.push_back(slot);
            return slot;
        };

        for (const auto& interval : intervals) {
            expire_old(interval.start);

            if (!allocatable.empty()) {
                const auto reg = allocatable.back();
                allocatable.pop_back();
                out.assignments[interval.vreg_id] = register_assignment{reg, std::nullopt};
                active.push_back(active_interval{interval, reg});
                continue;
            }

            auto spill_it = std::ranges::max_element(active,
                                                     [](const active_interval& a, const active_interval& b) {
                                                         return a.interval.end < b.interval.end;
                                                     });

            if (spill_it != active.end() && spill_it->interval.end > interval.end) {
                const auto victim_vreg = spill_it->interval.vreg_id;
                const auto reassigned = spill_it->assigned;
                out.assignments[victim_vreg] = register_assignment{std::nullopt, make_spill()};
                out.assignments[interval.vreg_id] = register_assignment{reassigned, std::nullopt};
                *spill_it = active_interval{interval, reassigned};
            }
            else {
                out.assignments[interval.vreg_id] = register_assignment{std::nullopt, make_spill()};
            }
        }

        out.spill_slots = fresh_spills;
        fn.assignments = out.assignments;
        fn.spill_slots.insert(fn.spill_slots.end(), fresh_spills.begin(), fresh_spills.end());
        return out;
    }

    [[nodiscard]] inline register_allocation allocate_registers(
        const function_ir& fn,
        std::vector<preg> allocatable = default_pregs()
    ) {
        function_ir copy = fn;
        return allocate_registers(copy, std::move(allocatable));
    }

    [[nodiscard]] inline register_allocation allocate_registers(
        const mir::virtual_mir_function& fn,
        std::vector<preg> allocatable = default_pregs()
    ) {
        return allocate_registers(fn.function, std::move(allocatable));
    }

    template <bool ObservabilityEnabled = observability::enabled_by_default,
              class Observer = observability::null_observer>
    [[nodiscard]] register_allocation allocate_registers_observed(
        function_ir& fn,
        Observer& observer,
        std::vector<preg> allocatable = default_pregs()
    ) {
#if LITHE_HAS_THREAD_LOCAL_SINK
        utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.codegen.alloc_registers"> _nadi_alloc{};
#endif
        observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                      observability::compilation_event::kind::started,
                                                      "codegen::allocate_registers",
                                                      observability::now_ns()
                                                  });

        const auto start = observability::now_ns();
#if LITHE_HAS_PROFILER
        profiler::ScopedProfiler _alloc_prof{"lithe.codegen.register_allocation"};
#endif
        auto allocation = allocate_registers(fn, std::move(allocatable));
        const auto end = observability::now_ns();

        const auto ir_dump = dump_machine_ir(fn);
        observability::emit<ObservabilityEnabled>(observer, observability::codegen_event{
                                                      "register_allocation",
                                                      start,
                                                      end,
                                                      observability::hash_text(ir_dump),
                                                      structural_hash_t{fn.blocks.size() + fn.assignments.size()}
                                                  });
        observability::emit<ObservabilityEnabled>(observer, observability::rewrite_event{
                                                      "linear_scan",
                                                      allocation.assignments.size(),
                                                      allocation.assignments.size(),
                                                      end
                                                  });
        observability::emit<ObservabilityEnabled>(observer, observability::structural_hash_event{
                                                      "machine_ir_after_alloc",
                                                      observability::hash_text(ir_dump),
                                                      structural_hash_t{fn.spill_slots.size() + fn.blocks.size()},
                                                      end
                                                  });
        observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                      observability::compilation_event::kind::finished,
                                                      "codegen::allocate_registers",
                                                      observability::now_ns()
                                                  });

        return allocation;
    }

    namespace detail {
        struct machine_lowering_visitor {
            function_ir* fn = nullptr;
            std::uint32_t block_id = 0;
            const function_signature* signature = nullptr;
            mutable std::size_t next_positional_argument_index = 0;

            [[nodiscard]] vreg emit_unary(const opcode op, const vreg input) const {
                auto dst = fn->make_vreg();
                instruction inst;
                inst.op = op;
                inst.defs = {operand::as_vreg(dst)};
                inst.uses = {operand::as_vreg(input)};
                (void)fn->emit(block_id, std::move(inst));
                return dst;
            }

            [[nodiscard]] vreg emit_binary(const opcode op, const vreg lhs, const vreg rhs) const {
                auto dst = fn->make_vreg();
                instruction inst;
                inst.op = op;
                inst.defs = {operand::as_vreg(dst)};
                inst.uses = {operand::as_vreg(lhs), operand::as_vreg(rhs)};
                (void)fn->emit(block_id, std::move(inst));
                return dst;
            }

            template <class T>
            [[nodiscard]] vreg on_terminal(const T& terminal) const {
                auto dst = fn->make_vreg();
                instruction inst;
                inst.defs = {operand::as_vreg(dst)};

                if constexpr (std::is_integral_v<std::decay_t<T>>) {
                    inst.op = opcode::load_imm;
                    inst.uses = {operand::as_i64(static_cast<std::int64_t>(terminal))};
                }
                else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
                    inst.op = opcode::load_imm;
                    inst.uses = {operand::as_f64(static_cast<double>(terminal))};
                }
                else {
                    const auto dumped_terminal = emit::dump(terminal);
                    std::optional<argument_index> arg_idx;
                    if (signature != nullptr) {
                        arg_idx = bind_terminal_as_argument(dumped_terminal, *signature);
                        if (!arg_idx.has_value()) {
                            arg_idx = bind_terminal_as_argument(next_positional_argument_index, *signature);
                            if (arg_idx.has_value()) {
                                ++next_positional_argument_index;
                            }
                        }
                    }

                    if (arg_idx.has_value()) {
                        inst.op = opcode::load_arg;
                        inst.uses = {operand::as_argument_index(arg_idx->value)};

                        const auto location = get_argument_location(*signature, *arg_idx);
                        if (location.physical_register.has_value()) {
                            inst.comment = "arg" + std::to_string(arg_idx->value) + " in " +
                                location.physical_register->name;
                        }
                        else if (location.stack_slot.has_value()) {
                            inst.comment = "arg" + std::to_string(arg_idx->value) + " via stack slot " +
                                std::to_string(*location.stack_slot);
                        }
                        else {
                            inst.comment = "arg" + std::to_string(arg_idx->value);
                        }
                    }
                    else {
                        inst.op = opcode::load_symbol;
                        inst.uses = {operand::as_symbol(dumped_terminal)};
                    }
                }

                (void)fn->emit(block_id, std::move(inst));
                return dst;
            }

            template <class Tag>
            [[nodiscard]] static constexpr opcode map_opcode() {
                if constexpr (std::is_same_v<Tag, add_tag>) return opcode::add;
                else if constexpr (std::is_same_v<Tag, sub_tag>) return opcode::sub;
                else if constexpr (std::is_same_v<Tag, mul_tag>) return opcode::mul;
                else if constexpr (std::is_same_v<Tag, div_tag>) return opcode::div;
                else if constexpr (std::is_same_v<Tag, mod_tag>) return opcode::mod;
                else if constexpr (std::is_same_v<Tag, neg_tag>) return opcode::neg;
                else if constexpr (std::is_same_v<Tag, eq_tag>) return opcode::cmp_eq;
                else if constexpr (std::is_same_v<Tag, ne_tag>) return opcode::cmp_ne;
                else if constexpr (std::is_same_v<Tag, lt_tag>) return opcode::cmp_lt;
                else if constexpr (std::is_same_v<Tag, le_tag>) return opcode::cmp_le;
                else if constexpr (std::is_same_v<Tag, gt_tag>) return opcode::cmp_gt;
                else if constexpr (std::is_same_v<Tag, ge_tag>) return opcode::cmp_ge;
                else if constexpr (std::is_same_v<Tag, and_tag>) return opcode::logical_and;
                else if constexpr (std::is_same_v<Tag, or_tag>) return opcode::logical_or;
                else if constexpr (std::is_same_v<Tag, not_tag>) return opcode::logical_not;
                else if constexpr (std::is_same_v<Tag, bit_and_tag>) return opcode::bit_and;
                else if constexpr (std::is_same_v<Tag, bit_or_tag>) return opcode::bit_or;
                else if constexpr (std::is_same_v<Tag, bit_xor_tag>) return opcode::bit_xor;
                else if constexpr (std::is_same_v<Tag, bit_not_tag>) return opcode::bit_not;
                else if constexpr (std::is_same_v<Tag, shl_tag>) return opcode::shl;
                else if constexpr (std::is_same_v<Tag, shr_tag>) return opcode::shr;
                else return opcode::call;
            }

            template <class Tag, class... Children>
            [[nodiscard]] vreg on_node(Tag, Children... children) const {
                constexpr auto op = map_opcode<Tag>();
                if constexpr (sizeof...(Children) == 1) {
                    return emit_unary(op, children...);
                }
                else if constexpr (sizeof...(Children) >= 2) {
                    auto ops = std::array<vreg, sizeof...(Children)>{children...};
                    auto acc = emit_binary(op, ops[0], ops[1]);
                    for (std::size_t i = 2; i < ops.size(); ++i) {
                        acc = emit_binary(op, acc, ops[i]);
                    }
                    return acc;
                }
                else {
                    return on_terminal(0);
                }
            }
        };
    } // namespace detail

    template <class Expr>
    [[nodiscard]] function_ir lower_to_machine_ir(const Expr& expr, std::string function_name = "anonymous") {
        function_ir fn;
        fn.name = std::move(function_name);
        auto& entry = fn.create_block("entry");

        const auto root = lithe::visit(expr, detail::machine_lowering_visitor{
                                           std::addressof(fn), entry.id, nullptr, 0
                                       });
        instruction ret;
        ret.op = opcode::ret;
        ret.uses = {operand::as_vreg(root)};
        (void)fn.emit(entry.id, std::move(ret));
        return fn;
    }

    template <class Expr>
    [[nodiscard]] function_ir lower_to_machine_ir(const Expr& expr, const function_signature& signature) {
        function_ir fn;
        fn.name = signature.name.empty() ? std::string{"anonymous"} : signature.name;
        auto& entry = fn.create_block("entry");

        const auto root = lithe::visit(expr, detail::machine_lowering_visitor{
                                           std::addressof(fn), entry.id, std::addressof(signature), 0
                                       });
        instruction ret;
        ret.op = opcode::ret;
        ret.uses = {operand::as_vreg(root)};
        (void)fn.emit(entry.id, std::move(ret));
        return fn;
    }

    template <class Expr>
    [[nodiscard]] mir::virtual_mir_function lower_to_virtual_mir(const Expr& expr) {
        return mir::virtual_mir_function{lower_to_machine_ir(expr)};
    }

    template <class Expr>
    [[nodiscard]] mir::virtual_mir_function lower_to_virtual_mir(
        const Expr& expr,
        const function_signature& signature
    ) {
        return mir::virtual_mir_function{lower_to_machine_ir(expr, signature)};
    }

    template <class Expr>
    [[nodiscard]] mir::virtual_mir_function build_virtual_mir(const Expr& expr) {
        return lower_to_virtual_mir(expr);
    }

    template <class Expr>
    [[nodiscard]] mir::virtual_mir_function build_virtual_mir(
        const Expr& expr,
        const function_signature& signature
    ) {
        return lower_to_virtual_mir(expr, signature);
    }

    template <bool ObservabilityEnabled = observability::enabled_by_default,
              class Expr,
              class Observer = observability::null_observer>
    [[nodiscard]] function_ir lower_to_machine_ir_observed(
        const Expr& expr,
        Observer& observer,
        std::string function_name = "anonymous"
    ) {
#if LITHE_HAS_THREAD_LOCAL_SINK
        utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.codegen.lower_mir"> _nadi_lower{};
#endif
        observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                      observability::compilation_event::kind::started,
                                                      "codegen::lower_to_machine_ir",
                                                      observability::now_ns()
                                                  });

        const auto start = observability::now_ns();
        try {
            auto fn = lower_to_machine_ir(expr, std::move(function_name));
            const auto end = observability::now_ns();
            const auto ir_dump = dump_machine_ir(fn);
            observability::emit<ObservabilityEnabled>(observer, observability::codegen_event{
                                                          "machine_lowering",
                                                          start,
                                                          end,
                                                          observability::hash_text(ir_dump),
                                                          structural_hash(expr)
                                                      });
            observability::emit<ObservabilityEnabled>(observer, observability::structural_hash_event{
                                                          "machine_ir",
                                                          structural_hash(expr),
                                                          observability::hash_text(ir_dump),
                                                          end
                                                      });
            observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                          observability::compilation_event::kind::finished,
                                                          "codegen::lower_to_machine_ir",
                                                          observability::now_ns()
                                                      });
            return fn;
        }
        catch (const std::exception& ex) {
            observability::emit<ObservabilityEnabled>(observer, observability::codegen_diagnostic_event{
                                                          "machine_lowering",
                                                          ex.what(),
                                                          observability::now_ns()
                                                      });
            observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                          observability::compilation_event::kind::failed,
                                                          "codegen::lower_to_machine_ir",
                                                          observability::now_ns()
                                                      });
            throw;
        }
    }

    template <bool ObservabilityEnabled = observability::enabled_by_default,
              class Expr,
              class Observer = observability::null_observer>
    [[nodiscard]] function_ir lower_to_machine_ir_observed(
        const Expr& expr,
        Observer& observer,
        const function_signature& signature
    ) {
#if LITHE_HAS_THREAD_LOCAL_SINK
        utils::nadi::PulseScope<utils::nadi::ThreadLocalSink, "lithe.codegen.lower_mir"> _nadi_lower{};
#endif
        observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                      observability::compilation_event::kind::started,
                                                      "codegen::lower_to_machine_ir",
                                                      observability::now_ns()
                                                  });

        const auto start = observability::now_ns();
        try {
            auto fn = lower_to_machine_ir(expr, signature);
            const auto end = observability::now_ns();
            const auto ir_dump = dump_machine_ir(fn);
            observability::emit<ObservabilityEnabled>(observer, observability::codegen_event{
                                                          "machine_lowering",
                                                          start,
                                                          end,
                                                          observability::hash_text(ir_dump),
                                                          structural_hash(expr)
                                                      });
            observability::emit<ObservabilityEnabled>(observer, observability::structural_hash_event{
                                                          "machine_ir",
                                                          structural_hash(expr),
                                                          observability::hash_text(ir_dump),
                                                          end
                                                      });
            observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                          observability::compilation_event::kind::finished,
                                                          "codegen::lower_to_machine_ir",
                                                          observability::now_ns()
                                                      });
            return fn;
        }
        catch (const std::exception& ex) {
            observability::emit<ObservabilityEnabled>(observer, observability::codegen_diagnostic_event{
                                                          "machine_lowering",
                                                          ex.what(),
                                                          observability::now_ns()
                                                      });
            observability::emit<ObservabilityEnabled>(observer, observability::compilation_event{
                                                          observability::compilation_event::kind::failed,
                                                          "codegen::lower_to_machine_ir",
                                                          observability::now_ns()
                                                      });
            throw;
        }
    }

    [[nodiscard]] inline std::string dump_machine_ir(
        const function_ir& fn,
        const bool include_register_pressure,
        const std::size_t hotspot_threshold
    ) {
        std::ostringstream os;
        os << "function " << fn.name << "\n";
        if (include_register_pressure) {
            os << dump_register_pressure(fn, hotspot_threshold);
        }

        for (const auto& block : fn.blocks) {
            os << "  bb" << block.id << " (" << block.name << ")";
            if (!block.successors.empty()) {
                os << " -> ";
                for (std::size_t i = 0; i < block.successors.size(); ++i) {
                    os << "bb" << block.successors[i];
                    if (i + 1 < block.successors.size()) {
                        os << ", ";
                    }
                }
            }
            os << "\n";

            if (!block.arguments.empty()) {
                os << "    args=[";
                for (std::size_t i = 0; i < block.arguments.size(); ++i) {
                    const auto& arg = block.arguments[i];
                    os << "s" << arg.value.id;
                    if (arg.name.has_value()) {
                        os << "(" << *arg.name << ")";
                    }
                    if (i + 1 < block.arguments.size()) {
                        os << ", ";
                    }
                }
                os << "]\n";
            }

            for (const auto& phi : block.phi_placeholders) {
                os << "    phi s" << phi.result.id << " <- [";
                for (std::size_t i = 0; i < phi.incoming.size(); ++i) {
                    const auto& inc = phi.incoming[i];
                    os << "bb" << inc.predecessor_block << ":s" << inc.value.id;
                    if (i + 1 < phi.incoming.size()) {
                        os << ", ";
                    }
                }
                os << "]";
                if (phi.note.has_value()) {
                    os << " ; " << *phi.note;
                }
                os << "\n";
            }

            for (const auto& inst : block.instructions) {
                os << "    i" << inst.id << ": " << to_string(inst.op);
                if (inst.number.has_value()) {
                    os << " [n=" << inst.number->global_index
                        << " b=" << inst.number->block_id
                        << " l=" << inst.number->block_local_index
                        << " e=" << inst.number->numbering_epoch << "]";
                }
                if (!inst.defs.empty()) {
                    os << " defs=[";
                    for (std::size_t i = 0; i < inst.defs.size(); ++i) {
                        os << dump_operand(inst.defs[i]);
                        if (i + 1 < inst.defs.size()) {
                            os << ", ";
                        }
                    }
                    os << "]";
                }
                if (!inst.uses.empty()) {
                    os << " uses=[";
                    for (std::size_t i = 0; i < inst.uses.size(); ++i) {
                        os << dump_operand(inst.uses[i]);
                        if (i + 1 < inst.uses.size()) {
                            os << ", ";
                        }
                    }
                    os << "]";
                }
                if (!inst.ssa_defs.empty()) {
                    os << " ssa_defs=[";
                    for (std::size_t i = 0; i < inst.ssa_defs.size(); ++i) {
                        os << "s" << inst.ssa_defs[i].id;
                        if (i + 1 < inst.ssa_defs.size()) {
                            os << ", ";
                        }
                    }
                    os << "]";
                }
                if (!inst.ssa_uses.empty()) {
                    os << " ssa_uses=[";
                    for (std::size_t i = 0; i < inst.ssa_uses.size(); ++i) {
                        os << "s" << inst.ssa_uses[i].id;
                        if (i + 1 < inst.ssa_uses.size()) {
                            os << ", ";
                        }
                    }
                    os << "]";
                }
                if (inst.comment.has_value()) {
                    os << " ; " << *inst.comment;
                }
                os << "\n";
            }
        }

        if (!fn.assignments.empty()) {
            os << "  register-assignments\n";
            std::vector<std::uint32_t> ids;
            ids.reserve(fn.assignments.size());
            for (const auto& [id, _] : fn.assignments) {
                (void)_;
                ids.push_back(id);
            }
            std::sort(ids.begin(), ids.end());
            for (const auto id : ids) {
                const auto& assign = fn.assignments.at(id);
                os << "    v" << id << " -> ";
                if (assign.physical.has_value()) {
                    os << assign.physical->name;
                }
                else if (assign.spill.has_value()) {
                    os << "spill" << assign.spill->id;
                }
                else {
                    os << "<unassigned>";
                }
                os << "\n";
            }
        }

        if (!fn.spill_slots.empty()) {
            os << "  spill-slots\n";
            for (const auto& slot : fn.spill_slots) {
                os << "    spill" << slot.id
                    << " size=" << slot.size
                    << " align=" << slot.alignment
                    << " offset=" << slot.frame_offset << "\n";
            }
        }

        return os.str();
    }

    [[nodiscard]] inline std::string dump_allocated_machine_ir(const allocated_function_ir& fn) {
        std::ostringstream os;
        os << "function " << fn.name << " [allocated]\n";

        for (const auto& block : fn.blocks) {
            os << "  bb" << block.id << " (" << block.name << ")";
            if (!block.successors.empty()) {
                os << " -> ";
                for (std::size_t i = 0; i < block.successors.size(); ++i) {
                    os << "bb" << block.successors[i];
                    if (i + 1 < block.successors.size()) {
                        os << ", ";
                    }
                }
            }
            os << "\n";

            for (const auto& inst : block.instructions) {
                os << "    i" << inst.id << ": " << to_string(inst.op);
                if (!inst.defs.empty()) {
                    os << " defs=[";
                    for (std::size_t i = 0; i < inst.defs.size(); ++i) {
                        os << dump_allocated_operand(inst.defs[i]);
                        if (i + 1 < inst.defs.size()) {
                            os << ", ";
                        }
                    }
                    os << "]";
                }
                if (!inst.uses.empty()) {
                    os << " uses=[";
                    for (std::size_t i = 0; i < inst.uses.size(); ++i) {
                        os << dump_allocated_operand(inst.uses[i]);
                        if (i + 1 < inst.uses.size()) {
                            os << ", ";
                        }
                    }
                    os << "]";
                }
                if (inst.comment.has_value()) {
                    os << " ; " << *inst.comment;
                }
                os << "\n";
            }
        }

        if (!fn.assignments.empty()) {
            os << "  allocation-map\n";
            std::vector<std::uint32_t> ids;
            ids.reserve(fn.assignments.size());
            for (const auto& [id, _] : fn.assignments) {
                (void)_;
                ids.push_back(id);
            }
            std::sort(ids.begin(), ids.end());
            for (const auto id : ids) {
                const auto& assign = fn.assignments.at(id);
                os << "    v" << id << " -> ";
                if (assign.physical.has_value()) {
                    os << assign.physical->name;
                }
                else if (assign.spill.has_value()) {
                    os << "spill" << assign.spill->id;
                }
                else {
                    os << "<unassigned>";
                }
                os << "\n";
            }
        }

        if (!fn.spill_slots.empty()) {
            os << "  spill-slots\n";
            for (const auto& slot : fn.spill_slots) {
                os << "    spill" << slot.id
                    << " size=" << slot.size
                    << " align=" << slot.alignment
                    << " offset=" << slot.frame_offset << "\n";
            }
        }

        return os.str();
    }

    [[nodiscard]] inline mir::verification_result verify_virtual_mir(const mir::virtual_mir_function& fn) {
        mir::verification_result out;
        if (fn.metadata.current_phase != mir::phase::virtual_mir) {
            out.diagnostics.emplace_back("virtual MIR metadata phase mismatch");
        }
        if (fn.function.blocks.empty()) {
            out.diagnostics.emplace_back("virtual MIR has no basic blocks");
            return out;
        }
        if (fn.function.cfg.entry_block == 0) {
            out.diagnostics.emplace_back("virtual MIR has no entry block");
        }

        std::unordered_set<std::uint32_t> block_ids;
        for (const auto& block : fn.function.blocks) {
            if (!block_ids.insert(block.id).second) {
                out.diagnostics.push_back("virtual MIR has duplicate block id bb" + std::to_string(block.id));
            }
        }
        return out;
    }

    [[nodiscard]] inline mir::verification_result verify_allocated_mir(const mir::allocated_mir_function& fn) {
        mir::verification_result out;
        if (fn.metadata.current_phase != mir::phase::allocated_mir) {
            out.diagnostics.emplace_back("allocated MIR metadata phase mismatch");
        }
        if (fn.function.blocks.empty()) {
            out.diagnostics.emplace_back("allocated MIR has no basic blocks");
            return out;
        }

        std::unordered_set<std::uint32_t> block_ids;
        for (const auto& block : fn.function.blocks) {
            if (!block_ids.insert(block.id).second) {
                out.diagnostics.push_back("allocated MIR has duplicate block id bb" + std::to_string(block.id));
            }
        }
        return out;
    }

    [[nodiscard]] inline bool contains_virtual_registers(const mir::physical_mir_function& fn) {
        return !fn.referenced_virtual_registers.empty();
    }

    [[nodiscard]] inline bool contains_unresolved_spills(const mir::physical_mir_function& fn) {
        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == opcode::load_spill || inst.op == opcode::store_spill) {
                    continue;
                }

                for (const auto& def : inst.defs) {
                    if (def.type == allocated_operand::kind::spill) {
                        return true;
                    }
                }
                for (const auto& use : inst.uses) {
                    if (use.type == allocated_operand::kind::spill) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    [[nodiscard]] inline bool has_duplicate_instruction_ids(const mir::physical_mir_function& fn) {
        std::unordered_set<std::uint32_t> seen;
        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.id == 0) {
                    continue;
                }
                if (!seen.insert(inst.id).second) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] inline mir::verification_result validate_calling_convention(
        const mir::physical_mir_function& fn,
        const function_signature& signature
    ) {
        mir::verification_result out;

        const auto cc = detail::resolved_calling_convention(signature);
        std::vector<argument_location> argument_locations;
        argument_locations.reserve(signature.arguments.size());
        for (std::size_t i = 0; i < signature.arguments.size(); ++i) {
            argument_locations.push_back(
                get_argument_location(signature, argument_index{static_cast<std::uint32_t>(i)}));
        }
        const auto return_loc = get_return_location(signature);

        for (const auto& loc : argument_locations) {
            if (!loc.physical_register.has_value()) {
                continue;
            }
            if (loc.reg_class == register_class::floating) {
                if (!detail::register_in_list(cc.floating_argument_registers, *loc.physical_register)) {
                    out.diagnostics.push_back(
                        "arg" + std::to_string(loc.index.value) + " uses non-ABI floating register " +
                        loc.physical_register->name
                    );
                }
            }
            else {
                if (!detail::register_in_list(cc.integer_argument_registers, *loc.physical_register)) {
                    out.diagnostics.push_back(
                        "arg" + std::to_string(loc.index.value) + " uses non-ABI integer register " +
                        loc.physical_register->name
                    );
                }
            }
        }

        if (return_loc.passing_kind == return_passing_kind::register_value && return_loc.physical_register.
            has_value()) {
            const auto allowed = return_loc.reg_class == register_class::floating
                                     ? detail::register_in_list(
                                         std::vector{cc.floating_return_register},
                                         *return_loc.physical_register
                                     )
                                     : detail::register_in_list(
                                         std::vector{cc.integer_return_register},
                                         *return_loc.physical_register
                                     );
            if (!allowed) {
                out.diagnostics.push_back(
                    "return uses non-ABI register " + return_loc.physical_register->name
                );
            }
        }

        bool saw_return = false;
        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == opcode::load_arg) {
                    if (inst.uses.empty() || inst.uses[0].type != allocated_operand::kind::argument_index) {
                        out.diagnostics.push_back(
                            "load_arg i" + std::to_string(inst.id) + " must use a valid argument index"
                        );
                        continue;
                    }

                    const auto arg = std::get<std::uint32_t>(inst.uses[0].value);
                    if (arg >= argument_locations.size()) {
                        out.diagnostics.push_back(
                            "load_arg i" + std::to_string(inst.id) + " references invalid arg" + std::to_string(arg)
                        );
                        continue;
                    }

                    const auto& loc = argument_locations[arg];
                    if (loc.passing_kind == argument_passing_kind::ignored) {
                        out.diagnostics.push_back(
                            "load_arg i" + std::to_string(inst.id) + " references ignored arg" + std::to_string(arg)
                        );
                        continue;
                    }

                    if (inst.defs.empty()) {
                        out.diagnostics.push_back(
                            "load_arg i" + std::to_string(inst.id) + " must define destination register"
                        );
                        continue;
                    }

                    if ((loc.passing_kind == argument_passing_kind::register_value
                            || loc.passing_kind == argument_passing_kind::register_reference)
                        && loc.physical_register.has_value()) {
                        if (inst.defs[0].type != allocated_operand::kind::preg) {
                            out.diagnostics.push_back(
                                "load_arg i" + std::to_string(inst.id) + " must define preg for register argument"
                            );
                            continue;
                        }
                        const auto& actual = std::get<preg>(inst.defs[0].value);
                        if (actual.id != loc.physical_register->id) {
                            out.diagnostics.push_back(
                                "load_arg i" + std::to_string(inst.id) + " expected " + loc.physical_register->name +
                                " for arg" + std::to_string(arg) + " but got " + actual.name
                            );
                        }
                    }
                }

                if (inst.op == opcode::ret) {
                    saw_return = true;
                    if (return_loc.passing_kind == return_passing_kind::void_return) {
                        if (!inst.uses.empty()) {
                            out.diagnostics.push_back(
                                "ret i" + std::to_string(inst.id) + " must not return a value for void signature"
                            );
                        }
                        continue;
                    }

                    if (inst.uses.size() != 1) {
                        out.diagnostics.push_back(
                            "ret i" + std::to_string(inst.id) + " must have exactly one return operand"
                        );
                        continue;
                    }

                    if (return_loc.passing_kind == return_passing_kind::register_value) {
                        if (inst.uses[0].type != allocated_operand::kind::preg) {
                            out.diagnostics.push_back(
                                "ret i" + std::to_string(inst.id) + " must use preg for register return"
                            );
                            continue;
                        }
                        if (return_loc.physical_register.has_value()) {
                            const auto& actual = std::get<preg>(inst.uses[0].value);
                            if (actual.id != return_loc.physical_register->id) {
                                out.diagnostics.push_back(
                                    "ret i" + std::to_string(inst.id) + " expected return register " +
                                    return_loc.physical_register->name + " but got " + actual.name
                                );
                            }
                        }
                        continue;
                    }

                    if (return_loc.passing_kind == return_passing_kind::stack_value) {
                        if (inst.uses[0].type != allocated_operand::kind::spill) {
                            out.diagnostics.push_back(
                                "ret i" + std::to_string(inst.id) + " must use spill slot for stack return"
                            );
                            continue;
                        }
                        if (return_loc.stack_slot.has_value()) {
                            const auto& actual = std::get<spill_slot>(inst.uses[0].value);
                            if (actual.id != (*return_loc.stack_slot + 1)) {
                                out.diagnostics.push_back(
                                    "ret i" + std::to_string(inst.id) + " expected return spill" +
                                    std::to_string(*return_loc.stack_slot + 1) + " but got spill" +
                                    std::to_string(actual.id)
                                );
                            }
                        }
                    }
                }
            }
        }

        if (!saw_return) {
            out.diagnostics.emplace_back("physical MIR does not contain a return instruction");
        }

        return out;
    }

    [[nodiscard]] inline mir::verification_result verify_calling_convention(
        const mir::physical_mir_function& fn,
        const function_signature& signature,
        const target_abi& abi
    ) {
        mir::verification_result out = validate_calling_convention(fn, signature);

        auto add_error = [&](std::string location, std::string text) {
            out = mir::verification_result::combine(
                out,
                mir::verification_result::error(
                    mir::diagnostic_message{std::move(location), std::move(text), mir::verification_error}
                )
            );
        };
        auto add_warning = [&](std::string location, std::string text) {
            out = mir::verification_result::combine(
                out,
                mir::verification_result::warning(
                    mir::diagnostic_message{std::move(location), std::move(text), mir::verification_warning}
                )
            );
        };

        const auto frame_layout = compute_stack_frame(fn);

        std::unordered_set<std::uint16_t> scratch_ids;
        for (const auto& reg : abi.convention.scratch_registers) {
            scratch_ids.insert(reg.id);
        }

        std::unordered_set<std::uint16_t> caller_saved_ids;
        for (const auto& reg : abi.convention.caller_saved) {
            caller_saved_ids.insert(reg.id);
        }

        std::unordered_set<std::uint16_t> callee_saved_ids;
        for (const auto& reg : abi.convention.callee_saved) {
            callee_saved_ids.insert(reg.id);
        }

        std::unordered_set<std::string> disallowed_special_registers = {
            "fp", "sp", "ra", "pc", "flags"
        };

        std::unordered_set<std::uint16_t> used_register_ids;
        std::unordered_set<std::uint16_t> used_callee_saved_ids;
        std::unordered_set<std::uint16_t> used_caller_saved_ids;

        std::vector<argument_location> argument_locations;
        argument_locations.reserve(signature.arguments.size());
        for (std::size_t i = 0; i < signature.arguments.size(); ++i) {
            argument_locations.push_back(
                get_argument_location(signature, argument_index{static_cast<std::uint32_t>(i)}));
        }

        auto return_location = get_return_location(signature);

        for (const auto& block : fn.function.blocks) {
            for (const auto& inst : block.instructions) {
                auto location = "bb" + std::to_string(block.id) + ":i" + std::to_string(inst.id);

                auto scan_reg = [&](const allocated_operand& op, const std::string_view context) {
                    if (op.type != allocated_operand::kind::preg) {
                        return;
                    }
                    const auto& reg = std::get<preg>(op.value);
                    used_register_ids.insert(reg.id);
                    if (callee_saved_ids.contains(reg.id)) {
                        used_callee_saved_ids.insert(reg.id);
                    }
                    if (caller_saved_ids.contains(reg.id)) {
                        used_caller_saved_ids.insert(reg.id);
                    }

                    if (scratch_ids.contains(reg.id)) {
                        add_error(location, std::string(context) + " uses reserved/scratch register " + reg.name);
                    }
                    if (disallowed_special_registers.contains(reg.name)) {
                        add_error(location, std::string(context) + " uses special-purpose register " + reg.name);
                    }
                };

                for (const auto& def : inst.defs) {
                    scan_reg(def, "def");
                }
                for (const auto& use : inst.uses) {
                    scan_reg(use, "use");
                }

                if (inst.op == opcode::branch || inst.op == opcode::branch_cond) {
                    const bool has_target = std::ranges::any_of(inst.uses, [](const allocated_operand& use) {
                        return use.type == allocated_operand::kind::block;
                    });
                    if (!has_target) {
                        add_error(location, "control-flow instruction missing explicit block target");
                    }
                }

                if (inst.op == opcode::load_arg) {
                    if (inst.uses.empty() || inst.uses[0].type != allocated_operand::kind::argument_index) {
                        add_error(location, "load_arg must reference argument index");
                        continue;
                    }

                    const auto arg_index = std::get<std::uint32_t>(inst.uses[0].value);
                    if (arg_index >= argument_locations.size()) {
                        add_error(location,
                                  "load_arg references out-of-range argument index arg" + std::to_string(arg_index));
                        continue;
                    }

                    const auto& arg_loc = argument_locations[arg_index];
                    if (arg_loc.passing_kind == argument_passing_kind::stack_value
                        || arg_loc.passing_kind == argument_passing_kind::stack_reference) {
                        const bool has_layout_slot = std::ranges::any_of(
                            frame_layout.objects, [&](const frame_object& object) {
                                return object.kind == frame_object_kind::argument_slot
                                    && object.source_argument.has_value()
                                    && object.source_argument->value == arg_index;
                            });
                        if (!has_layout_slot) {
                            add_warning(
                                location,
                                "stack-passed arg" + std::to_string(arg_index) +
                                " has no explicit argument_slot frame object"
                            );
                        }
                    }

                    if (arg_loc.physical_register.has_value()) {
                        const bool in_int = detail::register_in_list(
                            abi.convention.integer_argument_registers,
                            *arg_loc.physical_register
                        );
                        const bool in_fp = detail::register_in_list(
                            abi.convention.floating_argument_registers,
                            *arg_loc.physical_register
                        );

                        if (arg_loc.reg_class == register_class::floating && !in_fp) {
                            add_error(
                                location,
                                "floating argument arg" + std::to_string(arg_index) +
                                " assigned to non-floating register " + arg_loc.physical_register->name
                            );
                        }
                        if (arg_loc.reg_class == register_class::integer && !in_int) {
                            add_error(
                                location,
                                "integer argument arg" + std::to_string(arg_index) +
                                " assigned to non-integer register " + arg_loc.physical_register->name
                            );
                        }
                    }
                }
            }
        }

        if (return_location.passing_kind == return_passing_kind::register_value
            && return_location.physical_register.has_value()) {
            const auto& ret_reg = *return_location.physical_register;
            if (scratch_ids.contains(ret_reg.id)) {
                add_error("return", "return register " + ret_reg.name + " is scratch/reserved");
            }
            if (disallowed_special_registers.contains(ret_reg.name)) {
                add_error("return", "return register " + ret_reg.name + " is special-purpose and disallowed");
            }

            const bool in_int = detail::register_in_list(std::vector{abi.convention.integer_return_register},
                                                         ret_reg);
            const bool in_fp = detail::register_in_list(std::vector{abi.convention.floating_return_register},
                                                        ret_reg);
            if (return_location.reg_class == register_class::floating && !in_fp) {
                add_error("return", "floating return assigned to non-floating return register " + ret_reg.name);
            }
            if (return_location.reg_class == register_class::integer && !in_int) {
                add_error("return", "integer return assigned to non-integer return register " + ret_reg.name);
            }

            for (const auto& arg_loc : argument_locations) {
                if (arg_loc.physical_register.has_value() && arg_loc.physical_register->id == ret_reg.id) {
                    add_error(
                        "return",
                        "argument/return register overlap on " + ret_reg.name +
                        " (arg" + std::to_string(arg_loc.index.value) + ")"
                    );
                }
            }
        }

        if (!used_callee_saved_ids.empty()) {
            std::string msg = "callee-saved registers used: ";
            bool first = true;
            for (const auto& reg : abi.convention.callee_saved) {
                if (!used_callee_saved_ids.contains(reg.id)) {
                    continue;
                }
                if (!first) {
                    msg += ", ";
                }
                first = false;
                msg += reg.name;
            }
            add_warning("register-allocation", msg + " (save/restore planning required)");
        }

        if (!used_caller_saved_ids.empty()) {
            const bool has_saved_caller_slots = std::ranges::any_of(frame_layout.objects,
                                                                    [](const frame_object& object) {
                                                                        return object.kind ==
                                                                            frame_object_kind::caller_saved_register;
                                                                    });
            if (!has_saved_caller_slots) {
                add_warning(
                    "register-allocation",
                    "caller-saved registers are used without explicit save/restore frame objects"
                );
            }
        }

        const auto return_check = lower_return_to_abi(fn, signature.return_value);
        for (const auto& diag : return_check.diagnostics) {
            if (return_check.status == return_lowering_status_error || return_check.status ==
                return_lowering_status_reject) {
                add_error("return", diag);
            }
            else {
                add_warning("return", diag);
            }
        }

        return out;
    }

    [[nodiscard]] inline std::string dump_virtual_mir(const mir::virtual_mir_function& fn) {
        std::ostringstream os;
        os << "[virtual-mir phase=" << mir::to_string(fn.metadata.current_phase) << "]\n";
        if (!fn.metadata.dumps_enabled) {
            os << "  <dump disabled>\n";
            return os.str();
        }
        if (fn.metadata.note.has_value()) {
            os << "  note=" << *fn.metadata.note << "\n";
        }
        if (fn.argument_abi.has_value()) {
            os << "  abi-arguments validated=" << (fn.argument_abi->validated ? "true" : "false") << "\n";
            for (const auto& location : fn.argument_abi->per_argument_locations) {
                os << "    arg" << location.index.value << " ";
                if (location.physical_register.has_value()) {
                    os << "reg=" << location.physical_register->name;
                }
                else if (location.stack_slot.has_value()) {
                    os << "stack=" << *location.stack_slot;
                }
                else {
                    os << "ignored";
                }
                os << "\n";
            }
            for (const auto& diag : fn.argument_abi->diagnostics) {
                os << "    diag: " << diag << "\n";
            }
        }
        os << dump_register_pressure(fn);
        os << dump_scheduling_metadata(fn);
        os << dump_machine_ir(fn.function);
        if (const auto verification = verify_virtual_mir(fn); !verification.ok()) {
            os << "  verify-errors\n";
            for (const auto& diag : verification.diagnostics) {
                os << "    " << diag << "\n";
            }
        }
        return os.str();
    }

    [[nodiscard]] inline std::string dump_allocated_mir(const mir::allocated_mir_function& fn) {
        std::ostringstream os;
        os << "[allocated-mir phase=" << mir::to_string(fn.metadata.current_phase) << "]\n";
        if (!fn.metadata.dumps_enabled) {
            os << "  <dump disabled>\n";
            return os.str();
        }
        if (fn.metadata.note.has_value()) {
            os << "  note=" << *fn.metadata.note << "\n";
        }
        os << dump_allocated_machine_ir(fn.function);
        if (const auto verification = verify_allocated_mir(fn); !verification.ok()) {
            os << "  verify-errors\n";
            for (const auto& diag : verification.diagnostics) {
                os << "    " << diag << "\n";
            }
        }
        return os.str();
    }

    [[nodiscard]] inline std::string dump_physical_mir(const mir::physical_mir_function& fn) {
        std::ostringstream os;
        os << "[physical-mir phase=" << mir::to_string(fn.metadata.current_phase) << "]\n";
        if (!fn.metadata.dumps_enabled) {
            os << "  <dump disabled>\n";
            return os.str();
        }
        if (fn.metadata.note.has_value()) {
            os << "  note=" << *fn.metadata.note << "\n";
        }
        os << "  spill-rewrite inserted_loads=" << fn.inserted_loads
            << " inserted_stores=" << fn.inserted_stores << "\n";
        os << dump_allocated_machine_ir(fn.function);
        const auto verification = verify_physical_mir(fn);
        if (!verification.ok()) {
            os << "  verify-errors\n";
            for (const auto& diag : verification.diagnostics) {
                os << "    " << diag << "\n";
            }
        }
        return os.str();
    }

    [[nodiscard]] inline std::string dump_mir(const mir::virtual_mir_function& fn) {
        return dump_virtual_mir(fn);
    }

    [[nodiscard]] inline std::string dump_mir(const mir::allocated_mir_function& fn) {
        return dump_allocated_mir(fn);
    }

    [[nodiscard]] inline std::string dump_mir(const mir::physical_mir_function& fn) {
        return dump_physical_mir(fn);
    }

    inline void dump_machine_ir(
        std::ostream& os,
        const function_ir& fn,
        const bool include_register_pressure = false,
        const std::size_t hotspot_threshold = 0
    ) {
        os << dump_machine_ir(fn, include_register_pressure, hotspot_threshold);
    }

    inline void dump_allocated_machine_ir(std::ostream& os, const allocated_function_ir& fn) {
        os << dump_allocated_machine_ir(fn);
    }

    inline void dump_virtual_mir(std::ostream& os, const mir::virtual_mir_function& fn) {
        os << dump_virtual_mir(fn);
    }

    inline void dump_allocated_mir(std::ostream& os, const mir::allocated_mir_function& fn) {
        os << dump_allocated_mir(fn);
    }

    inline void dump_physical_mir(std::ostream& os, const mir::physical_mir_function& fn) {
        os << dump_physical_mir(fn);
    }

    inline void dump_mir(std::ostream& os, const mir::virtual_mir_function& fn) {
        os << dump_mir(fn);
    }

    inline void dump_mir(std::ostream& os, const mir::allocated_mir_function& fn) {
        os << dump_mir(fn);
    }

    inline void dump_mir(std::ostream& os, const mir::physical_mir_function& fn) {
        os << dump_mir(fn);
    }

    // -----------------------------------------------------------------------
    // MIR expression identity
    //
    // Provides a hashable key for pure arithmetic expressions so that a CSE
    // pass can detect duplicate computations without inspecting instruction
    // objects directly.
    // -----------------------------------------------------------------------

    // Normalized representation of an operand value used as part of a
    // CSE key.  Only preg ids and integer/fp immediates are meaningful for
    // pure arithmetic; all other operand kinds prevent CSE.
    struct cse_operand_key {
        allocated_operand::kind type = allocated_operand::kind::none;
        // For preg: stores preg::id cast to uint64_t.
        // For immediate_i64: bit-cast to uint64_t.
        // For immediate_f64: bit-cast to uint64_t (bit identity, not value equality).
        std::uint64_t bits = 0;

        [[nodiscard]] bool operator==(const cse_operand_key& o) const noexcept {
            return type == o.type && bits == o.bits;
        }
    };

    // Hashable key that identifies a pure MIR expression.
    // Two instructions are the same CSE candidate iff their keys compare equal.
    struct mir_expression_key {
        opcode op = opcode::nop;
        std::vector<cse_operand_key> operands;
        // flags_bits encodes semantic flags that affect the expression result.
        // Currently unused (zero) — reserved for future width/signedness flags.
        std::uint32_t flags_bits = 0;

        [[nodiscard]] bool operator==(const mir_expression_key& o) const noexcept {
            return op == o.op && flags_bits == o.flags_bits && operands == o.operands;
        }
    };

    // Returns true if opcode is a pure arithmetic / bitwise / logical operation
    // that has no side effects, does not touch memory or spill slots, is not a
    // control-flow transfer, and is not a call.
    [[nodiscard]] inline bool is_pure_expression(const opcode op) noexcept {
        switch (op) {
        case opcode::add:
        case opcode::sub:
        case opcode::mul:
        case opcode::div:
        case opcode::mod:
        case opcode::neg:
        case opcode::cmp_eq:
        case opcode::cmp_ne:
        case opcode::cmp_lt:
        case opcode::cmp_le:
        case opcode::cmp_gt:
        case opcode::cmp_ge:
        case opcode::bit_and:
        case opcode::bit_or:
        case opcode::bit_xor:
        case opcode::bit_not:
        case opcode::shl:
        case opcode::shr:
        case opcode::logical_and:
        case opcode::logical_or:
        case opcode::logical_not:
            return true;
        default:
            return false;
        }
    }

    // Trait-aware overload: if the instruction carries an abstract_operation and
    // a registry is available, consult the registered traits.  An operation is
    // considered pure when it has none of the impurity traits.  Falls back to
    // the opcode-based overload when no registry or no abstract_operation.
    [[nodiscard]] inline bool is_pure_expression(
        const instruction& instr,
        const operation_registry* reg) noexcept {
        if (reg && instr.abstract_operation) {
            const operation_trait_set ts = reg->traits_of(*instr.abstract_operation);
            constexpr operation_trait_set impure =
                static_cast<operation_trait_set>(operation_trait::reads_memory) |
                static_cast<operation_trait_set>(operation_trait::writes_memory) |
                static_cast<operation_trait_set>(operation_trait::allocates_memory) |
                static_cast<operation_trait_set>(operation_trait::control_flow) |
                static_cast<operation_trait_set>(operation_trait::terminator) |
                static_cast<operation_trait_set>(operation_trait::call_like) |
                static_cast<operation_trait_set>(operation_trait::may_throw) |
                static_cast<operation_trait_set>(operation_trait::may_trap) |
                static_cast<operation_trait_set>(operation_trait::has_side_effects);
            return (ts & impure) == 0;
        }
        return is_pure_expression(instr.op);
    }

    // Returns true when the instruction may observe or produce side effects.
    // Opcode-only version: mirrors the inverse of is_pure_expression for legacy ops
    // plus memory and control-flow opcodes.
    [[nodiscard]] inline bool has_side_effects(const opcode op) noexcept {
        switch (op) {
        case opcode::load:
        case opcode::store:
        case opcode::load_spill:
        case opcode::store_spill:
        case opcode::call:
        case opcode::branch:
        case opcode::branch_cond:
        case opcode::ret:
            return true;
        default:
            return false;
        }
    }

    // Trait-aware overload for has_side_effects.
    [[nodiscard]] inline bool has_side_effects(
        const instruction& instr,
        const operation_registry* reg) noexcept {
        if (reg && instr.abstract_operation) {
            const operation_trait_set ts = reg->traits_of(*instr.abstract_operation);
            constexpr operation_trait_set side_effect_traits =
                static_cast<operation_trait_set>(operation_trait::writes_memory) |
                static_cast<operation_trait_set>(operation_trait::allocates_memory) |
                static_cast<operation_trait_set>(operation_trait::control_flow) |
                static_cast<operation_trait_set>(operation_trait::terminator) |
                static_cast<operation_trait_set>(operation_trait::call_like) |
                static_cast<operation_trait_set>(operation_trait::may_throw) |
                static_cast<operation_trait_set>(operation_trait::may_trap) |
                static_cast<operation_trait_set>(operation_trait::has_side_effects);
            return (ts & side_effect_traits) != 0;
        }
        return has_side_effects(instr.op);
    }

    // Returns true when the instruction may trigger a hardware trap (e.g. divide
    // by zero, misaligned access).  Opcode-only version: conservative list.
    [[nodiscard]] inline bool may_trap(const opcode op) noexcept {
        switch (op) {
        case opcode::div:
        case opcode::mod:
        case opcode::load:
        case opcode::store:
            return true;
        default:
            return false;
        }
    }

    // Trait-aware overload for may_trap.
    [[nodiscard]] inline bool may_trap(
        const instruction& instr,
        const operation_registry* reg) noexcept {
        if (reg && instr.abstract_operation) {
            return has_trait(reg->traits_of(*instr.abstract_operation), operation_trait::may_trap);
        }
        return may_trap(instr.op);
    }

    // Trait-aware overload for is_pure_expression on allocated_instruction.
    [[nodiscard]] inline bool is_pure_expression(
        const allocated_instruction& inst,
        const operation_registry* reg) noexcept {
        if (reg && inst.abstract_operation) {
            const operation_trait_set ts = reg->traits_of(*inst.abstract_operation);
            constexpr operation_trait_set impure =
                static_cast<operation_trait_set>(operation_trait::reads_memory) |
                static_cast<operation_trait_set>(operation_trait::writes_memory) |
                static_cast<operation_trait_set>(operation_trait::allocates_memory) |
                static_cast<operation_trait_set>(operation_trait::control_flow) |
                static_cast<operation_trait_set>(operation_trait::terminator) |
                static_cast<operation_trait_set>(operation_trait::call_like) |
                static_cast<operation_trait_set>(operation_trait::may_throw) |
                static_cast<operation_trait_set>(operation_trait::may_trap) |
                static_cast<operation_trait_set>(operation_trait::has_side_effects);
            return (ts & impure) == 0;
        }
        return is_pure_expression(inst.op);
    }

    // Trait-aware overload for has_side_effects on allocated_instruction.
    [[nodiscard]] inline bool has_side_effects(
        const allocated_instruction& inst,
        const operation_registry* reg) noexcept {
        if (reg && inst.abstract_operation) {
            const operation_trait_set ts = reg->traits_of(*inst.abstract_operation);
            constexpr operation_trait_set side_effect_traits =
                static_cast<operation_trait_set>(operation_trait::writes_memory) |
                static_cast<operation_trait_set>(operation_trait::allocates_memory) |
                static_cast<operation_trait_set>(operation_trait::control_flow) |
                static_cast<operation_trait_set>(operation_trait::terminator) |
                static_cast<operation_trait_set>(operation_trait::call_like) |
                static_cast<operation_trait_set>(operation_trait::may_throw) |
                static_cast<operation_trait_set>(operation_trait::may_trap) |
                static_cast<operation_trait_set>(operation_trait::has_side_effects);
            return (ts & side_effect_traits) != 0;
        }
        return has_side_effects(inst.op);
    }

    // Trait-aware overload for may_trap on allocated_instruction.
    [[nodiscard]] inline bool may_trap(
        const allocated_instruction& inst,
        const operation_registry* reg) noexcept {
        if (reg && inst.abstract_operation) {
            return has_trait(reg->traits_of(*inst.abstract_operation), operation_trait::may_trap);
        }
        return may_trap(inst.op);
    }

    // Returns true if an instruction is a legal CSE candidate:
    //   - opcode is pure (is_pure_expression)
    //   - exactly one def, which must be a preg
    //   - all uses are pregs or integer/fp immediates
    //   - no uses that would require memory, spill, block, or symbol operands
    [[nodiscard]] inline bool is_cse_candidate(const allocated_instruction& inst) noexcept {
        if (!is_pure_expression(inst.op)) return false;
        if (inst.defs.size() != 1) return false;
        if (inst.defs[0].type != allocated_operand::kind::preg) return false;
        for (const auto& use : inst.uses) {
            switch (use.type) {
            case allocated_operand::kind::preg:
            case allocated_operand::kind::immediate_i64:
            case allocated_operand::kind::immediate_f64:
                break;
            default:
                return false;
            }
        }
        return true;
    }

    // Trait-aware overload for is_cse_candidate.  Uses the abstract_operation
    // carried on the allocated instruction when available; falls back to the
    // orig pointer (for callers that have not yet propagated metadata) and then
    // to the opcode-based check.  Structural constraints are unchanged.
    [[nodiscard]] inline bool is_cse_candidate(
        const allocated_instruction& inst,
        const instruction* orig,
        const operation_registry* reg) noexcept {
        if (reg) {
            if (inst.abstract_operation) {
                if (!is_pure_expression(inst, reg)) return false;
            }
            else if (orig) {
                if (!is_pure_expression(*orig, reg)) return false;
            }
            else {
                if (!is_pure_expression(inst.op)) return false;
            }
        }
        else {
            if (!is_pure_expression(inst.op)) return false;
        }
        if (inst.defs.size() != 1) return false;
        if (inst.defs[0].type != allocated_operand::kind::preg) return false;
        for (const auto& use : inst.uses) {
            switch (use.type) {
            case allocated_operand::kind::preg:
            case allocated_operand::kind::immediate_i64:
            case allocated_operand::kind::immediate_f64:
                break;
            default:
                return false;
            }
        }
        return true;
    }

    // Builds a mir_expression_key from an instruction.
    // Returns nullopt if the instruction is not a CSE candidate.
    [[nodiscard]] inline std::optional<mir_expression_key> make_expression_key(
        const allocated_instruction& inst
    ) {
        if (!is_cse_candidate(inst)) return std::nullopt;

        mir_expression_key key;
        key.op = inst.op;
        key.operands.reserve(inst.uses.size());

        for (const auto& use : inst.uses) {
            cse_operand_key ok;
            ok.type = use.type;
            switch (use.type) {
            case allocated_operand::kind::preg:
                ok.bits = std::get<preg>(use.value).id;
                break;
            case allocated_operand::kind::immediate_i64: {
                const std::int64_t iv = std::get<std::int64_t>(use.value);
                std::uint64_t bits = 0;
                std::memcpy(&bits, &iv, sizeof(bits));
                ok.bits = bits;
                break;
            }
            case allocated_operand::kind::immediate_f64: {
                const double fv = std::get<double>(use.value);
                std::uint64_t bits = 0;
                std::memcpy(&bits, &fv, sizeof(bits));
                ok.bits = bits;
                break;
            }
            default:
                return std::nullopt; // unexpected — is_cse_candidate already guards this
            }
            key.operands.push_back(ok);
        }

        return key;
    }

    // -----------------------------------------------------------------------
    // Backend scheduling metadata
    //
    // These structures are metadata-only: they describe the scheduling shape
    // of instructions and regions without implementing any scheduler.  They
    // are ISA-agnostic and carry no architecture-specific fields.
    // -----------------------------------------------------------------------

    // Per-opcode (or per-scheduling-class) latency descriptor.
    // Records the number of cycles before a consumer can begin using the
    // result of an instruction of this class.
    struct instruction_latency {
        // Scheduling class this latency applies to (matches
        // instruction_scheduling_metadata::scheduling_class).
        std::string scheduling_class = "generic";
        // Cycles from issue to result available (pipeline depth).
        std::size_t cycles = 1;
        // Optional throughput: reciprocal throughput in cycles (e.g. 0.5 = 2/cycle).
        double throughput_cycles = 1.0;
        // Minimum gap in cycles required between two instructions of this class
        // when issued on the same execution unit.
        std::size_t minimum_issue_gap = 1;
    };

    // A directed scheduling dependency between two instructions.
    // Models the three standard dependency kinds (RAW, WAR, WAW) plus control
    // and memory ordering without committing to any specific DAG representation.
    struct dependency_edge {
        enum class kind : std::uint8_t {
            data, // true dependence — RAW (read-after-write)
            anti, // WAR (write-after-read)
            output, // WAW (write-after-write)
            control, // control-flow ordering constraint
            memory, // memory ordering (load/store ordering)
            structural // resource conflict (e.g. same execution unit)
        };

        std::uint32_t source_inst_id = 0;
        std::uint32_t target_inst_id = 0;
        kind type = kind::data;
        // Minimum latency in cycles that must elapse between issue of source
        // and issue of target to satisfy this dependency.
        std::size_t latency_cycles = 1;
        // True when this edge crosses a loop back-edge (loop-carried dependence).
        bool loop_carried = false;

        [[nodiscard]] bool operator==(const dependency_edge& o) const noexcept {
            return source_inst_id == o.source_inst_id
                && target_inst_id == o.target_inst_id
                && type == o.type;
        }
    };

    // Estimated register pressure for a point in the instruction stream.
    // Pressure is stored per register class so that architectures with separate
    // integer / floating-point / vector register files can be modelled correctly.
    struct register_pressure_info {
        // Estimated number of live integer pregs at this point.
        std::uint32_t integer_pressure = 0;
        // Estimated number of live floating-point pregs.
        std::uint32_t float_pressure = 0;
        // Estimated number of live vector pregs.
        std::uint32_t vector_pressure = 0;
        // Architecture-supplied register file sizes (0 = unknown).
        std::uint32_t integer_file_size = 0;
        std::uint32_t float_file_size = 0;
        std::uint32_t vector_file_size = 0;

        // True iff any class is at or above its file size limit.
        [[nodiscard]] bool under_pressure() const noexcept {
            return (integer_file_size > 0 && integer_pressure >= integer_file_size)
                || (float_file_size > 0 && float_pressure >= float_file_size)
                || (vector_file_size > 0 && vector_pressure >= vector_file_size);
        }
    };

    // A scheduling region: a contiguous sequence of instructions (within a
    // single basic block, or across a linear chain of blocks) for which a
    // scheduler may reorder instructions subject to the dependency graph.
    //
    // This is metadata-only: the instructions themselves live in the MIR;
    // this struct carries the dependency graph and pressure estimates that
    // a future scheduling pass would consume.
    struct scheduling_region {
        // Ordered list of instruction ids in program order.
        std::vector<std::uint32_t> instruction_ids = {};
        // Dependency edges within this region.
        std::vector<dependency_edge> dependencies = {};
        // Per-instruction latency overrides (keyed by instruction id).
        // If absent for an instruction, use the global latency table.
        std::unordered_map<std::uint32_t, std::size_t> latency_override = {};
        // Register pressure estimate *before* the first instruction.
        register_pressure_info entry_pressure = {};
        // Register pressure estimate *after* the last instruction.
        register_pressure_info exit_pressure = {};
        // Maximum register pressure observed anywhere within the region.
        register_pressure_info peak_pressure = {};
        // True if any dependency in this region is loop-carried.
        bool has_loop_carried = false;
    };

    // ---- typed_constant -------------------------------------------------------

    struct typed_constant;

    // The concrete payload a typed_constant can hold.
    using typed_constant_payload = std::variant<
        std::monostate, // unknown / not a constant
        std::int64_t, // integer (all widths, sign-extended)
        double, // float (all widths, extended to double)
        bool, // boolean / predicate
        std::string, // symbol name (pointer) or "" for null ptr
        std::vector<typed_constant> // vector lanes / aggregate fields / tensor elements
    >;

    struct typed_constant {
        abstract_value_kind kind = abstract_value_kind::unknown;
        std::uint32_t bit_width = 0; // 0 = platform-native / unknown
        std::uint32_t lane_count = 1; // >1 for vector; shape linearised for tensor
        typed_constant_payload payload = std::monostate{};

        // ---- factories ----

        [[nodiscard]] static typed_constant make_unknown() noexcept {
            return typed_constant{};
        }

        [[nodiscard]] static typed_constant make_integer(std::int64_t v,
                                                         const std::uint32_t width = 64) noexcept {
            typed_constant c;
            c.kind = abstract_value_kind::integer;
            c.bit_width = width;
            c.payload = v;
            return c;
        }

        [[nodiscard]] static typed_constant make_float(double v,
                                                       const std::uint32_t width = 64) noexcept {
            typed_constant c;
            c.kind = abstract_value_kind::floating;
            c.bit_width = width;
            c.payload = v;
            return c;
        }

        [[nodiscard]] static typed_constant make_bool(bool v) noexcept {
            typed_constant c;
            c.kind = abstract_value_kind::predicate;
            c.payload = v;
            return c;
        }

        // Pointer: symbol name for a named global, "" for the null pointer.
        [[nodiscard]] static typed_constant make_pointer(std::string symbol,
                                                         const std::uint32_t width = 64) noexcept {
            typed_constant c;
            c.kind = abstract_value_kind::pointer;
            c.bit_width = width;
            c.payload = std::move(symbol);
            return c;
        }

        [[nodiscard]] static typed_constant make_null_pointer(const std::uint32_t width = 64) noexcept {
            return make_pointer("", width);
        }

        // Vector: all lanes must share the same element kind.
        [[nodiscard]] static typed_constant make_vector(std::vector<typed_constant> lanes) {
            typed_constant c;
            c.kind = abstract_value_kind::vector;
            c.lane_count = static_cast<std::uint32_t>(lanes.size());
            if (!lanes.empty()) c.bit_width = lanes[0].bit_width;
            c.payload = std::move(lanes);
            return c;
        }

        // Aggregate / tensor: fields / elements in linearised order.
        [[nodiscard]] static typed_constant make_aggregate(std::vector<typed_constant> fields) {
            typed_constant c;
            c.kind = abstract_value_kind::aggregate;
            c.lane_count = static_cast<std::uint32_t>(fields.size());
            c.payload = std::move(fields);
            return c;
        }

        [[nodiscard]] static typed_constant make_tensor(std::vector<typed_constant> elements,
                                                        const std::uint32_t lanes = 0) {
            typed_constant c;
            c.kind = abstract_value_kind::tensor;
            c.lane_count = lanes ? lanes : static_cast<std::uint32_t>(elements.size());
            c.payload = std::move(elements);
            return c;
        }

        // ---- queries ----

        [[nodiscard]] bool is_unknown() const noexcept { return std::holds_alternative<std::monostate>(payload); }
        [[nodiscard]] bool is_integer() const noexcept { return std::holds_alternative<std::int64_t>(payload); }
        [[nodiscard]] bool is_float() const noexcept { return std::holds_alternative<double>(payload); }
        [[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(payload); }
        [[nodiscard]] bool is_pointer() const noexcept { return std::holds_alternative<std::string>(payload); }

        [[nodiscard]] bool is_composite() const noexcept {
            return std::holds_alternative<std::vector<typed_constant>>(payload);
        }

        [[nodiscard]] std::int64_t as_integer() const { return std::get<std::int64_t>(payload); }
        [[nodiscard]] double as_float() const { return std::get<double>(payload); }
        [[nodiscard]] bool as_bool() const { return std::get<bool>(payload); }
        [[nodiscard]] const std::string& as_symbol() const { return std::get<std::string>(payload); }

        [[nodiscard]] const std::vector<typed_constant>& as_lanes() const {
            return std::get<std::vector<typed_constant>>(payload);
        }

        [[nodiscard]] std::vector<typed_constant>& as_lanes() {
            return std::get<std::vector<typed_constant>>(payload);
        }

        [[nodiscard]] bool is_null_pointer() const noexcept {
            return is_pointer() && as_symbol().empty();
        }

        // True when this is a vector/aggregate whose every lane holds an
        // identical scalar constant — allows splat-aware identity rules.
        // Comparison is done value-by-value at the scalar level to avoid
        // recursive operator== on the variant (std::vector<typed_constant>
        // is not fully defined inside the struct body).
        [[nodiscard]] bool is_splat() const noexcept {
            if (!is_composite()) return false;
            const auto& lanes = as_lanes();
            if (lanes.empty()) return false;
            const typed_constant& first = lanes[0];
            // Splat is only meaningful for uniform scalar lanes.
            if (first.is_unknown() || first.is_composite()) return false;
            for (std::size_t i = 1; i < lanes.size(); ++i) {
                const typed_constant& lane = lanes[i];
                if (lane.kind != first.kind) return false;
                if (first.is_integer() && lane.as_integer() != first.as_integer()) return false;
                if (first.is_float() && lane.as_float() != first.as_float()) return false;
                if (first.is_bool() && lane.as_bool() != first.as_bool()) return false;
                if (first.is_pointer() && lane.as_symbol() != first.as_symbol()) return false;
            }
            return true;
        }

        // Return the first-lane scalar for a splat vector, or *this for a scalar.
        [[nodiscard]] const typed_constant& scalar_or_first() const noexcept {
            if (is_composite()) {
                const auto& v = as_lanes();
                if (!v.empty()) return v[0];
            }
            return *this;
        }
    };

    // ---- evaluation_state & partially_evaluated_value ------------------------

    enum class evaluation_state : std::uint8_t {
        known, // fully resolved; typed_constant carries the value
        unknown, // entirely dynamic; nothing is known
        symbolic, // named placeholder; no algebra engine
        deferred, // postponed to a later pipeline stage
    };

    struct symbolic_placeholder_metadata {
        std::string name;
        std::string constraint_hint; // e.g. "non_negative", "power_of_two"
    };

    struct deferred_operation_metadata {
        opcode deferred_op = opcode::nop;
        std::vector<std::uint32_t> operand_ids;
    };

    struct partially_evaluated_value {
        evaluation_state state = evaluation_state::unknown;

        // Populated when state == known.
        std::optional<typed_constant> value;

        // Populated when state == symbolic.
        symbolic_placeholder_metadata symbolic;

        // Populated when state == deferred.
        deferred_operation_metadata deferred;

        [[nodiscard]] static partially_evaluated_value make_known(typed_constant tc) {
            partially_evaluated_value pev;
            pev.state = evaluation_state::known;
            pev.value = std::move(tc);
            return pev;
        }

        [[nodiscard]] static partially_evaluated_value make_unknown() {
            return partially_evaluated_value{};
        }

        [[nodiscard]] static partially_evaluated_value make_symbolic(std::string name,
                                                                     std::string constraint = {}) {
            partially_evaluated_value pev;
            pev.state = evaluation_state::symbolic;
            pev.symbolic.name = std::move(name);
            pev.symbolic.constraint_hint = std::move(constraint);
            return pev;
        }

        [[nodiscard]] static partially_evaluated_value make_deferred(const opcode op,
                                                                     std::vector<std::uint32_t> ids) {
            partially_evaluated_value pev;
            pev.state = evaluation_state::deferred;
            pev.deferred.deferred_op = op;
            pev.deferred.operand_ids = std::move(ids);
            return pev;
        }

        [[nodiscard]] bool is_known() const noexcept { return state == evaluation_state::known; }
        [[nodiscard]] bool is_unknown() const noexcept { return state == evaluation_state::unknown; }
        [[nodiscard]] bool is_symbolic() const noexcept { return state == evaluation_state::symbolic; }
        [[nodiscard]] bool is_deferred() const noexcept { return state == evaluation_state::deferred; }
    };
} // namespace lithe::codegen

#include "lithe_semantic.hpp"

namespace lithe::codegen {
    // -----------------------------------------------------------------------
    // Bridge: abstract_value_type ↔ semantic::types::type_descriptor
    // -----------------------------------------------------------------------

    // Convert an abstract_value_type to the nearest matching type_descriptor.
    // Registers new types in the given registry on demand.
    [[nodiscard]] inline semantic::types::type_id
    abstract_value_to_type_id(
        const abstract_value_type& avt,
        semantic::types::semantic_type_registry& reg) {
        using namespace semantic::types;
        switch (avt.kind) {
        case abstract_value_kind::integer:
            return reg.make_integer_type(
                avt.bit_width > 0 ? avt.bit_width : 64u,
                /*is_signed=*/true,
                avt.semantic_type.empty() ? "" : avt.semantic_type);

        case abstract_value_kind::floating:
            return reg.make_float_type(
                avt.bit_width > 0 ? avt.bit_width : 64u,
                avt.semantic_type.empty() ? "" : avt.semantic_type);

        case abstract_value_kind::scalar: {
            // Treat unspecified scalars as 64-bit integers.
            return reg.make_integer_type(
                avt.bit_width > 0 ? avt.bit_width : 64u, true,
                avt.semantic_type.empty() ? "scalar" : avt.semantic_type);
        }

        case abstract_value_kind::vector:
        case abstract_value_kind::tensor: {
            type_descriptor elem;
            elem.kind = type_kind::floating;
            elem.bit_width = avt.bit_width > 0 ? avt.bit_width : 32u;
            elem.name = "f" + std::to_string(elem.bit_width);
            const type_id elem_id = reg.canonicalize(elem);

            std::vector<std::uint32_t> shape;
            shape.reserve(avt.shape.size());
            for (auto d : avt.shape) shape.push_back(d);

            return reg.make_tensor_type(elem_id, std::move(shape),
                                        avt.semantic_type.empty() ? "" : avt.semantic_type);
        }

        case abstract_value_kind::predicate: {
            type_descriptor d;
            d.kind = type_kind::boolean;
            d.bit_width = 1;
            d.name = avt.semantic_type.empty() ? "bool" : avt.semantic_type;
            return reg.canonicalize(d);
        }

        case abstract_value_kind::symbolic: {
            type_descriptor d;
            d.kind = type_kind::symbolic;
            d.name = avt.semantic_type.empty() ? "symbolic" : avt.semantic_type;
            return reg.canonicalize(d);
        }

        default: {
            type_descriptor d;
            d.kind = type_kind::unknown;
            d.name = avt.semantic_type.empty() ? "unknown" : avt.semantic_type;
            return reg.canonicalize(d);
        }
        }
    }

    // Convert a semantic type_descriptor back to an abstract_value_type.
    [[nodiscard]] inline abstract_value_type
    type_id_to_abstract_value(
        semantic::types::type_id tid,
        const semantic::types::semantic_type_registry& reg) {
        abstract_value_type avt;
        if (tid == semantic::types::invalid_type_id) {
            avt.kind = abstract_value_kind::unknown;
            return avt;
        }
        auto td = reg.find_type(tid);
        if (!td.has_value()) {
            avt.kind = abstract_value_kind::unknown;
            return avt;
        }
        avt.semantic_type = td->name;
        avt.bit_width = td->bit_width;
        avt.shape = td->shape;
        switch (td->kind) {
        case semantic::types::type_kind::integer:
            avt.kind = abstract_value_kind::integer;
            break;
        case semantic::types::type_kind::floating:
            avt.kind = abstract_value_kind::floating;
            break;
        case semantic::types::type_kind::boolean:
            avt.kind = abstract_value_kind::predicate;
            break;
        case semantic::types::type_kind::tensor:
        case semantic::types::type_kind::vector:
            avt.kind = abstract_value_kind::tensor;
            break;
        case semantic::types::type_kind::symbolic:
            avt.kind = abstract_value_kind::symbolic;
            break;
        default:
            avt.kind = abstract_value_kind::unknown;
            break;
        }
        return avt;
    }

    // -----------------------------------------------------------------------
    // Contract validation result
    // -----------------------------------------------------------------------

    struct operation_contract_check_result {
        bool ok = true;
        std::vector<std::string> diagnostics;
        std::vector<semantic::types::type_id> inferred_result_types;
        // Per-operand coercion suggestions (index matches operand position).
        std::vector<std::optional<semantic::type_rules::coercion_fact>> coercions;

        void add_error(std::string msg) {
            ok = false;
            diagnostics.push_back(std::move(msg));
        }

        void add_warning(std::string msg) {
            diagnostics.push_back("[warn] " + std::move(msg));
        }
    };

    // -----------------------------------------------------------------------
    // check_operation_contract_using_types
    //
    // Validates actual operand type ids against the abstract_value_type
    // operand list in a contract, infers result types, and suggests
    // coercions when implicit conversion is legal.
    // -----------------------------------------------------------------------
    [[nodiscard]] inline operation_contract_check_result
    check_operation_contract_using_types(
        const operation_contract& contract,
        const std::vector<semantic::types::type_id>& actual_operand_type_ids,
        semantic::types::semantic_type_registry& type_reg,
        semantic::type_rules::semantic_type_rule_engine* rule_engine = nullptr) {
        operation_contract_check_result result;
        const auto& expected_operands = contract.operands;

        // 1. Validate operand count.
        if (actual_operand_type_ids.size() != expected_operands.size()) {
            result.add_error(
                "operand count mismatch: expected " +
                std::to_string(expected_operands.size()) +
                " but got " +
                std::to_string(actual_operand_type_ids.size()));
            return result;
        }

        result.coercions.resize(expected_operands.size());

        // 2. Per-operand type compatibility.
        for (std::size_t i = 0; i < expected_operands.size(); ++i) {
            const auto& expected_avt = expected_operands[i];
            const auto actual_tid = actual_operand_type_ids[i];

            // Convert expected abstract_value_type → type_id for comparison.
            const auto expected_tid = abstract_value_to_type_id(expected_avt, type_reg);

            if (actual_tid == semantic::types::invalid_type_id) {
                result.add_warning("operand " + std::to_string(i) + " has unknown type, skipping check");
                continue;
            }

            if (type_reg.equivalent(actual_tid, expected_tid) ||
                type_reg.subtype_of(actual_tid, expected_tid)) {
                continue; // directly compatible
            }

            // Ask the rule engine for a coercion.
            semantic::type_rules::semantic_type_rule_engine* eng = rule_engine;
            // Use global engine as fallback.
            if (!eng) eng = &semantic::type_rules::type_rule_engine();

            auto coercion = eng->find_coercion(actual_tid, expected_tid);
            if (coercion.has_value()) {
                result.coercions[i] = coercion;
                if (!coercion->is_lossless) {
                    result.add_warning(
                        "operand " + std::to_string(i) +
                        ": narrowing coercion required (" + coercion->description + ")");
                }
            }
            else {
                // Check reverse subtype to produce a helpful message.
                auto actual_td = type_reg.find_type(actual_tid);
                auto expected_td = type_reg.find_type(expected_tid);
                std::string actual_name = actual_td ? actual_td->name : "unknown";
                std::string expected_name = expected_td ? expected_td->name : "unknown";
                result.add_error(
                    "operand " + std::to_string(i) +
                    ": type incompatible — got '" + actual_name +
                    "' but contract requires '" + expected_name + "'");
            }
        }

        // 3. Infer result types from contract + rule engine.
        if (!result.ok) return result; // don't infer when operands are invalid

        for (const auto& result_avt : contract.results) {
            const auto tid = abstract_value_to_type_id(result_avt, type_reg);
            if (tid == semantic::types::invalid_type_id) {
                // Try inference through the rule engine.
                if (rule_engine || true) {
                    semantic::type_rules::semantic_type_rule_engine* eng =
                        rule_engine ? rule_engine : &semantic::type_rules::type_rule_engine();
                    auto inferred = eng->infer_expression_type(0, actual_operand_type_ids);
                    if (inferred.inferred) {
                        result.inferred_result_types.push_back(inferred.type_id);
                    }
                    else {
                        result.inferred_result_types.push_back(semantic::types::invalid_type_id);
                    }
                }
            }
            else {
                result.inferred_result_types.push_back(tid);
            }
        }

        // 4. If the contract has no declared results, infer from operand types.
        if (contract.results.empty() && !actual_operand_type_ids.empty()) {
            semantic::type_rules::semantic_type_rule_engine* eng =
                rule_engine ? rule_engine : &semantic::type_rules::type_rule_engine();
            auto inferred = eng->infer_expression_type(0, actual_operand_type_ids);
            if (inferred.inferred) {
                result.inferred_result_types.push_back(inferred.type_id);
                // Propagate any rule diagnostics as warnings.
                for (const auto& diag : inferred.rule_result.diagnostics) {
                    if (diag.severity == semantic::type_rules::type_rule_severity::warning ||
                        diag.severity == semantic::type_rules::type_rule_severity::error) {
                        result.add_warning(diag.message);
                    }
                }
            }
        }

        return result;
    }

    // Convenience overload: takes raw abstract_value_types as actual operands
    // (converts them to type_ids internally).
    [[nodiscard]] inline operation_contract_check_result
    check_operation_contract_using_types(
        const operation_contract& contract,
        const std::vector<abstract_value_type>& actual_operands,
        semantic::types::semantic_type_registry& type_reg,
        semantic::type_rules::semantic_type_rule_engine* rule_engine = nullptr) {
        std::vector<semantic::types::type_id> type_ids;
        type_ids.reserve(actual_operands.size());
        for (const auto& avt : actual_operands) {
            type_ids.push_back(abstract_value_to_type_id(avt, type_reg));
        }
        return check_operation_contract_using_types(
            contract, type_ids, type_reg, rule_engine);
    }
} // namespace lithe::codegen (semantic type contract extension)
namespace std {
    template <>
    struct hash<lithe::codegen::cse_operand_key> {
        [[nodiscard]] std::size_t operator()(const lithe::codegen::cse_operand_key& k) const noexcept {
            std::size_t h = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(k.type));
            h ^= std::hash<std::uint64_t>{}(k.bits) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    template <>
    struct hash<lithe::codegen::mir_expression_key> {
        [[nodiscard]] std::size_t operator()(const lithe::codegen::mir_expression_key& k) const noexcept {
            std::size_t h = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(k.op));
            h ^= std::hash<std::uint32_t>{}(k.flags_bits) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            for (const auto& ok : k.operands) {
                constexpr std::hash<lithe::codegen::cse_operand_key> op_hash{};
                h ^= op_hash(ok) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            }
            return h;
        }
    };
} // namespace std

#include "lithe_codegen_hl.hpp"
