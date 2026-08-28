#pragma once

// ============================================================================
// lithe_rt/foundation.hpp — typed-MIR value model + unified trap model
//
// The runtime foundation's pure vocabulary layer: how a value is classified
// (sign, pointer class, value role) and how a failure is classified (trap_code,
// trap).  Zero heap dependencies — every other runtime-foundation header builds
// on this one.
//
// Additive overlay on lithe::codegen: classify() reads an existing
// abstract_value_type and to_abstract() writes one back, so no existing MIR
// struct, pass, or backend changes.  New runtime operations are expressed via
// the existing operation_id extension mechanism (domain "lithe.rt"), exactly as
// safepoint_tag / mop.* already do — no new opcode enumerator, no macro.
// ============================================================================

#include <array>
#include <cstdint>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>

#include "../lithe_codegen.hpp" // abstract_value_type / _kind, opcode, may_trap
#include "../lithe_runtime.hpp" // runtime::values::object_ref

namespace lithe::rt {
    // =========================================================================
    // Typed-MIR value model
    // =========================================================================

    // Integer signedness.  not_integer for non-integer values.
    enum class int_sign : std::uint8_t {
        not_integer = 0,
        signed_int,
        unsigned_int,
    };

    // Pointer classification.  The runtime must never confuse a raw host pointer
    // with a managed (GC-tracked) reference or a guest-memory offset.
    //
    //   none            value is not a pointer
    //   raw_host        untracked native pointer (FFI, host data)
    //   managed_base    GC-tracked reference to an object head
    //   managed_derived interior pointer; derived_base names the base vreg
    //   guest_offset    offset into linear guest memory (untrusted profile)
    enum class ptr_class : std::uint8_t {
        none = 0,
        raw_host,
        managed_base,
        managed_derived,
        guest_offset,
    };

    // Value-role flags.  A value may simultaneously be, e.g., an exception
    // payload carried across a safepoint.  Bitmask so roles compose.
    enum class value_role : std::uint8_t {
        none = 0,
        exception_value = 1u << 0, // carries a thrown exception object
        deopt_state = 1u << 1, // participates in a deoptimization frame-state
        safepoint = 1u << 2, // live across a GC safepoint (a root)
    };

    [[nodiscard]] constexpr value_role operator|(value_role a, value_role b) noexcept {
        return static_cast<value_role>(
            static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }

    [[nodiscard]] constexpr value_role operator&(value_role a, value_role b) noexcept {
        return static_cast<value_role>(
            static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
    }

    [[nodiscard]] constexpr bool has_role(value_role set, value_role probe) noexcept {
        return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(probe)) != 0;
    }

    // typed_value — compact per-SSA-value runtime descriptor.  Trivially
    // copyable; sits in a side table keyed by vreg id (never embedded in the
    // flat MIR instruction stream, preserving MIR ABI stability).
    struct typed_value {
        codegen::abstract_value_kind kind = codegen::abstract_value_kind::unknown;
        std::uint32_t bit_width = 0; // element width in bits (0 = unspecified)
        int_sign sign = int_sign::not_integer;
        ptr_class pclass = ptr_class::none;
        std::uint32_t derived_base = 0; // vreg id of base (managed_derived only; 0 = none)
        value_role roles = value_role::none;

        [[nodiscard]] constexpr bool is_managed() const noexcept {
            return pclass == ptr_class::managed_base
                || pclass == ptr_class::managed_derived;
        }

        [[nodiscard]] constexpr bool is_gc_root() const noexcept {
            return is_managed() && has_role(roles, value_role::safepoint);
        }

        [[nodiscard]] constexpr bool is_derived() const noexcept {
            return pclass == ptr_class::managed_derived;
        }
    };

    static_assert(std::is_trivially_copyable_v<typed_value>);

    // classify — derive a typed_value from an existing abstract_value_type.
    //
    // The flat MIR only records kind/bit_width; sign and pointer class are
    // recovered from the semantic_type string when present (e.g. "u32", "i64",
    // "gc_ref", "gc_ref.derived", "guest_ptr", "host_ptr").  Unknown strings
    // fall back to conservative defaults (signed for integers, raw_host for
    // pointers) — never a managed classification without explicit evidence, so
    // the GC can never be fooled into scanning a raw pointer.
    [[nodiscard]] inline typed_value
    classify(const codegen::abstract_value_type& t) noexcept {
        typed_value v;
        v.kind = t.kind;
        v.bit_width = t.bit_width;

        const std::string_view st{t.semantic_type};

        if (t.kind == codegen::abstract_value_kind::integer
            || t.kind == codegen::abstract_value_kind::predicate) {
            v.sign = (!st.empty() && st.front() == 'u')
                         ? int_sign::unsigned_int
                         : int_sign::signed_int;
        }

        if (t.kind == codegen::abstract_value_kind::pointer) {
            if (st.starts_with("gc_ref.derived") || st.starts_with("managed.derived")) {
                v.pclass = ptr_class::managed_derived;
            }
            else if (st.starts_with("gc_ref") || st.starts_with("managed")) {
                v.pclass = ptr_class::managed_base;
            }
            else if (st.starts_with("guest")) {
                v.pclass = ptr_class::guest_offset;
            }
            else {
                v.pclass = ptr_class::raw_host;
            }
        }
        return v;
    }

    // to_abstract — project a typed_value back onto an abstract_value_type.
    // Preserves kind + bit_width and writes a canonical semantic_type string so
    // a subsequent classify() round-trips the sign / pointer class.
    [[nodiscard]] inline codegen::abstract_value_type
    to_abstract(const typed_value& v) {
        codegen::abstract_value_type t;
        t.kind = v.kind;
        t.bit_width = v.bit_width;

        if (v.kind == codegen::abstract_value_kind::integer
            || v.kind == codegen::abstract_value_kind::predicate) {
            t.semantic_type = (v.sign == int_sign::unsigned_int ? "u" : "i");
            if (v.bit_width != 0) t.semantic_type += std::to_string(v.bit_width);
        }
        else if (v.kind == codegen::abstract_value_kind::pointer) {
            switch (v.pclass) {
            case ptr_class::managed_base: t.semantic_type = "gc_ref";
                break;
            case ptr_class::managed_derived: t.semantic_type = "gc_ref.derived";
                break;
            case ptr_class::guest_offset: t.semantic_type = "guest_ptr";
                break;
            case ptr_class::raw_host: t.semantic_type = "host_ptr";
                break;
            case ptr_class::none: break;
            }
        }
        return t;
    }

    // =========================================================================
    // Unified trap model
    //
    // Every execution failure — bounds error, null deref, division by zero, out
    // of fuel, deopt request, security violation, corrupted artifact — becomes
    // one structured `trap`, carrying function/version identity, faulting MIR
    // instruction + machine offset, source location, and an optional exception
    // payload.  The runtime result convention is `std::expected<T, trap>`.
    // =========================================================================

    // trap_code — closed set of runtime failure classes (prompt ).
    enum class trap_code : std::uint16_t {
        out_of_bounds = 0,
        null_reference,
        division_by_zero,
        integer_overflow,
        invalid_indirect_call,
        unresolved_symbol,
        out_of_fuel,
        deadline_exceeded,
        stack_overflow,
        out_of_memory,
        uncaught_exception,
        deoptimization_requested,
        security_violation,
        corrupted_artifact,
        unknown_runtime_fault, // trapping op with no specific mapping (never mislabelled)
    };

    [[nodiscard]] constexpr std::string_view trap_code_name(const trap_code c) noexcept {
        switch (c) {
        case trap_code::out_of_bounds: return "out_of_bounds";
        case trap_code::null_reference: return "null_reference";
        case trap_code::division_by_zero: return "division_by_zero";
        case trap_code::integer_overflow: return "integer_overflow";
        case trap_code::invalid_indirect_call: return "invalid_indirect_call";
        case trap_code::unresolved_symbol: return "unresolved_symbol";
        case trap_code::out_of_fuel: return "out_of_fuel";
        case trap_code::deadline_exceeded: return "deadline_exceeded";
        case trap_code::stack_overflow: return "stack_overflow";
        case trap_code::out_of_memory: return "out_of_memory";
        case trap_code::uncaught_exception: return "uncaught_exception";
        case trap_code::deoptimization_requested: return "deoptimization_requested";
        case trap_code::security_violation: return "security_violation";
        case trap_code::corrupted_artifact: return "corrupted_artifact";
        case trap_code::unknown_runtime_fault: return "unknown_runtime_fault";
        }
        return "unknown";
    }

    // trap — structured failure record.
    struct trap {
        trap_code code = trap_code::out_of_bounds;
        std::uint32_t function_id = 0;
        std::uint32_t code_version = 0;
        std::uint32_t mir_instruction = 0; // faulting MIR instruction id
        std::uint64_t machine_offset = 0; // byte offset into the code version
        std::source_location src{};
        std::optional<runtime::values::object_ref> exception_payload;
        std::string detail; // optional human-readable context

        [[nodiscard]] static trap make(
            const trap_code code,
            const std::uint32_t function_id = 0,
            const std::uint32_t code_version = 0,
            const std::uint32_t mir_instruction = 0,
            const std::uint64_t machine_offset = 0,
            std::string detail = {},
            const std::source_location src = std::source_location::current()) {
            trap t;
            t.code = code;
            t.function_id = function_id;
            t.code_version = code_version;
            t.mir_instruction = mir_instruction;
            t.machine_offset = machine_offset;
            t.detail = std::move(detail);
            t.src = src;
            return t;
        }

        // Attach a thrown object as payload; set code to uncaught_exception.
        [[nodiscard]] static trap
        from_exception(const runtime::values::object_ref payload,
                       const std::uint32_t function_id = 0,
                       const std::uint32_t mir_instruction = 0) {
            trap t = make(trap_code::uncaught_exception, function_id, 0, mir_instruction);
            t.exception_payload = payload;
            return t;
        }

        [[nodiscard]] std::string message() const {
            std::string m(trap_code_name(code));
            if (!detail.empty()) {
                m += ": ";
                m += detail;
            }
            return m;
        }
    };

    // may_trap_kinds — for a MIR opcode, the trap_codes it can raise.  Built on
    // the existing codegen::may_trap(opcode) predicate: opcodes that cannot trap
    // return an empty range.  Fixed-capacity, no allocation.
    struct trap_kind_set {
        std::array<trap_code, 2> codes{};
        std::uint8_t count = 0;

        [[nodiscard]] bool contains(const trap_code c) const noexcept {
            for (std::uint8_t i = 0; i < count; ++i)
                if (codes[i] == c) return true;
            return false;
        }

        [[nodiscard]] std::span<const trap_code> view() const noexcept {
            return {codes.data(), count};
        }
    };

    [[nodiscard]] inline trap_kind_set may_trap_kinds(const codegen::opcode op) noexcept {
        trap_kind_set s;
        if (!codegen::may_trap(op)) return s;
        switch (op) {
        case codegen::opcode::div:
        case codegen::opcode::mod:
            s.codes = {trap_code::division_by_zero, trap_code::integer_overflow};
            s.count = 2;
            break;
        case codegen::opcode::load:
        case codegen::opcode::store:
            s.codes = {trap_code::out_of_bounds, trap_code::null_reference};
            s.count = 2;
            break;
        default:
            // may_trap said yes but we have no specific mapping — report a
            // dedicated unknown fault rather than mislabelling as out_of_bounds.
            s.codes[0] = trap_code::unknown_runtime_fault;
            s.count = 1;
            break;
        }
        return s;
    }

    // =========================================================================
    // Managed operation vocabulary (operation_id extension, domain "lithe.rt")
    //
    // MIR has no gc_alloc / throw / safepoint / write_barrier opcode.  Managed
    // operations ride the existing codegen::operation_id extension mechanism so
    // the flat-MIR ABI stays stable.  These are the exact canonical names — the
    // annotation / verification passes match on them EXACTLY (never by prefix),
    // so a stray "gc_allocate" cannot be mistaken for "gc_alloc".
    // =========================================================================
    namespace managed_op {
        inline constexpr std::string_view domain = "lithe.rt";
        inline constexpr std::string_view gc_alloc = "gc_alloc";
        inline constexpr std::string_view gc_alloc_pinned = "gc_alloc_pinned";
        inline constexpr std::string_view write_barrier = "write_barrier";
        inline constexpr std::string_view safepoint = "safepoint";
        inline constexpr std::string_view throw_op = "throw";
        inline constexpr std::string_view rethrow_op = "rethrow";
        // Checked (trapping) variants — see prompt Option A.  The plain
        // div/mod/load/store keep their defined MIR semantics; these trap.
        inline constexpr std::string_view checked_div = "checked_div";
        inline constexpr std::string_view checked_mod = "checked_mod";
        inline constexpr std::string_view checked_load = "checked_load";
        inline constexpr std::string_view checked_store = "checked_store";
        inline constexpr std::string_view checked_index = "checked_index";

        // Trap set for a checked op name (exact match; empty for unknown names).
        [[nodiscard]] inline trap_kind_set trap_kinds(const std::string_view name) noexcept {
            trap_kind_set s;
            if (name == checked_div || name == checked_mod) {
                s.codes = {trap_code::division_by_zero, trap_code::integer_overflow};
                s.count = 2;
            }
            else if (name == checked_load || name == checked_store
                || name == checked_index) {
                s.codes = {trap_code::out_of_bounds, trap_code::null_reference};
                s.count = 2;
            }
            return s;
        }
    } // namespace managed_op
} // namespace lithe::rt
