#pragma once

// lithe_ir/frontend/lowering_contract.hpp — Lithe-owned frontend lowering contract.
//
// C++23, header-only, no virtual, no macros. Namespace: lithe::ir::frontend
//
// SCOPE
//   Defines the authoritative boundary between any language frontend (Crank,
//   Sutra, future languages) and Lithe IR.  Every frontend MUST target this
//   contract instead of inventing its own informal lowering rules.
//
// CONTENT
//   1. scalar_kind — frontend-agnostic element width/kind descriptor.
//   2. crank_type_to_ir_str    — Crank source type → canonical IR type string (§5).
//   3. tensor_type_to_ir_str   — tensor [N]T / []T → memref<NxT> / memref<?xT>.
//   4. crank_capability_required — Crank source feature → required capability bit.
//   5. validate_ir_type_str    — gate: type string must be §5-conformant.
//   6. lowering_violation / lowering_result — structured error surface.
//
// STABILITY
//   The type-string constants and capability mapping are stable within a major
//   schema version (aligned with lithe-ir-spec.md §16.1).
//   Any breaking change requires a schema major bump.
//
// USAGE (Crank example)
//   #include "lithe/lithe_ir/frontend/lowering_contract.hpp"
//
//   // scalar type
//   auto ts = lithe::ir::frontend::crank_type_to_ir_str("Int32"); // "i32"
//
//   // memref for a dynamic slice []Float64
//   auto mr = lithe::ir::frontend::tensor_type_to_ir_str("Float64", 1, {-1}); // "memref<?xf64>"
//
//   // capability for a transaction block
//   auto cap = lithe::ir::frontend::crank_capability_required(
//                  lithe::ir::frontend::crank_feature::transaction); // transactions bit

#include "lithe/lithe_ir/portable/module.hpp"   // portable_capability_bit

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lithe::ir::frontend {
    // ============================================================================
    // §1 — scalar_kind: element type descriptor used inside tensor mappings.
    //
    // Mirrors the canonical type grammar from lithe-ir-spec.md §5:
    //   int_ty   = "i", digits
    //   float_ty = "f", digits
    // ============================================================================

    struct scalar_kind {
        enum class family : std::uint8_t { integer, floating };

        family fam = family::integer;
        std::uint8_t bit_width = 32;

        // Produce the canonical IR type string, e.g. "i32", "f64".
        [[nodiscard]] std::string to_ir_str() const {
            std::string s;
            s += (fam == family::integer) ? 'i' : 'f';
            s += std::to_string(static_cast<unsigned>(bit_width));
            return s;
        }
    };

    // Compile-time constant scalars matching the Lithe type grammar.
    inline constexpr scalar_kind k_i1{scalar_kind::family::integer, 1};
    inline constexpr scalar_kind k_i8{scalar_kind::family::integer, 8};
    inline constexpr scalar_kind k_i16{scalar_kind::family::integer, 16};
    inline constexpr scalar_kind k_i32{scalar_kind::family::integer, 32};
    inline constexpr scalar_kind k_i64{scalar_kind::family::integer, 64};
    inline constexpr scalar_kind k_f16{scalar_kind::family::floating, 16};
    inline constexpr scalar_kind k_f32{scalar_kind::family::floating, 32};
    inline constexpr scalar_kind k_f64{scalar_kind::family::floating, 64};

    // ============================================================================
    // §2 — crank_type_to_ir_str
    //
    // Maps a Crank source type name (PascalCase or lowercase alias) to the
    // canonical Lithe IR type string per lithe-ir-spec.md §5.
    //
    // Mapping table (normative):
    //   Bool     → "i1"
    //   Int8     → "i8"    (alias: i8)
    //   Int16    → "i16"   (alias: i16)
    //   Int32    → "i32"   (alias: i32)
    //   Int64    → "i64"   (alias: i64)
    //   UInt8    → "i8"    (unsigned integers share the IR bit-width string)
    //   UInt16   → "i16"
    //   UInt32   → "i32"
    //   UInt64   → "i64"
    //   Float32  → "f32"   (alias: f32)
    //   Float64  → "f64"   (alias: f64)
    //
    // Returns std::nullopt for unknown / unmappable types.
    // The transaction and host-call features are capability-level, not type-level;
    // use crank_capability_required for those.
    // ============================================================================

    [[nodiscard]] inline std::optional<std::string>
    crank_type_to_ir_str(std::string_view crank_type) noexcept {
        struct entry {
            std::string_view src;
            std::string_view ir;
        };
        static constexpr std::array<entry, 23> table{
            {
                // Canonical PascalCase names
                {"Bool", "i1"},
                {"Int8", "i8"}, {"Int16", "i16"}, {"Int32", "i32"}, {"Int64", "i64"},
                {"UInt8", "i8"}, {"UInt16", "i16"}, {"UInt32", "i32"}, {"UInt64", "i64"},
                {"Float32", "f32"}, {"Float64", "f64"},
                // Lowercase aliases (also accepted by crank std_types lookup_primitive_id)
                {"i8", "i8"}, {"i16", "i16"}, {"i32", "i32"}, {"i64", "i64"},
                {"u8", "i8"}, {"u16", "i16"}, {"u32", "i32"}, {"u64", "i64"},
                {"f32", "f32"}, {"f64", "f64"},
                // Boolean canonical
                {"bool", "i1"}, {"Bool", "i1"},
            }
        };
        for (const auto& e : table)
            if (e.src == crank_type) return std::string{e.ir};
        return std::nullopt;
    }

    // ============================================================================
    // §3 — tensor_type_to_ir_str
    //
    // Derives the canonical memref type string from a tensor's element type,
    // rank, and static shape per lithe-ir-spec.md §5.
    //
    //   [N]T  (static)  → "memref<NxT>"            (e.g. [256]Float64 → "memref<256xf64>")
    //   []T   (dynamic) → "memref<?xT>"             (any dim == -1 → "?")
    //   rank-2 [M][N]T  → "memref<MxNxT>"
    //
    // dim values: >= 0 means static; -1 means dynamic (?).
    // Returns std::nullopt if elem_type is unmappable or rank == 0.
    // ============================================================================

    [[nodiscard]] inline std::optional<std::string>
    tensor_type_to_ir_str(std::string_view elem_type, std::uint8_t rank,
                          const std::vector<std::int64_t>& dims) noexcept {
        if (rank == 0) return std::nullopt;
        auto ir_elem = crank_type_to_ir_str(elem_type);
        if (!ir_elem) return std::nullopt;

        std::string s = "memref<";
        for (std::uint8_t i = 0; i < rank; ++i) {
            if (i < static_cast<std::uint8_t>(dims.size()) && dims[i] >= 0)
                s += std::to_string(dims[i]);
            else
                s += '?';
            s += 'x';
        }
        s += *ir_elem;
        s += '>';
        return s;
    }

    // ============================================================================
    // §4 — crank_capability_required
    //
    // Maps a Crank source-level feature to the Lithe portable_capability_bit that
    // a module MUST declare when that feature is used.
    //
    // Feature→capability mapping (normative):
    //   transaction   → portable_capability_bit::transactions
    //   host_call     → portable_capability_bit::external_calls
    //   defer_scope   → portable_capability_bit::defer_scopes
    //   atomic        → portable_capability_bit::atomics
    //   simd          → portable_capability_bit::simd_hint
    //   gpu           → portable_capability_bit::gpu_hint
    //   reflection    → portable_capability_bit::reflection
    //   exception     → portable_capability_bit::exceptions
    //
    // Returns 0 (no capability required) for features that need no special
    // capability declaration (e.g. plain arithmetic, local variables).
    // ============================================================================

    enum class crank_feature : std::uint8_t {
        none = 0,
        transaction, // transaction { } block, resource[key] access
        host_call, // @host function call / host-registered function call
        defer_scope, // defer statement
        atomic, // @atomic / std::atomic access
        simd, // @simd loop/function annotation
        gpu, // @gpu annotation / Device[Gpu] bound
        reflection, // @reflect(…) / program-built descriptors
        exception, // exception propagation (not currently in Crank grammar)
    };

    [[nodiscard]] constexpr lithe::ir::portable::portable_capability_bit
    crank_capability_required(crank_feature feat) noexcept {
        using cap = lithe::ir::portable::portable_capability_bit;
        switch (feat) {
        case crank_feature::transaction: return cap::transactions;
        case crank_feature::host_call: return cap::external_calls;
        case crank_feature::defer_scope: return cap::defer_scopes;
        case crank_feature::atomic: return cap::atomics;
        case crank_feature::simd: return cap::simd_hint;
        case crank_feature::gpu: return cap::gpu_hint;
        case crank_feature::reflection: return cap::reflection;
        case crank_feature::exception: return cap::exceptions;
        default: return cap{0};
        }
    }

    // ============================================================================
    // §5 — validate_ir_type_str
    //
    // Light-weight check that a type string is §5-conformant (scalar or memref)
    // without pulling in the full verifier.  Mirrors detail::type_str_parseable
    // from verify.hpp but callable without the module context.
    //
    // Returns true iff the string would pass T-check of verify_portable.
    // ============================================================================

    [[nodiscard]] inline bool validate_ir_type_str(std::string_view s) noexcept {
        if (s.empty()) return false;
        // scalar: i<digits> or f<digits>
        if (s.size() >= 2 && (s[0] == 'i' || s[0] == 'f')) {
            for (std::size_t i = 1; i < s.size(); ++i)
                if (s[i] < '0' || s[i] > '9') return false;
            return s.size() >= 2; // at least one digit
        }
        // opaque<digits>
        if (s.size() > 6 && s.substr(0, 6) == "opaque") {
            for (std::size_t i = 6; i < s.size(); ++i)
                if (s[i] < '0' || s[i] > '9') return false;
            return s.size() > 6;
        }
        // memref<...>: must start with "memref<" and end with ">"
        if (s.size() < 9) return false; // "memref<?" minimum = 9
        if (s.substr(0, 7) != "memref<") return false;
        if (s.back() != '>') return false;
        std::string_view inner = s.substr(7, s.size() - 8);
        // inner = dim{x dim}x scalar  — at least one dim+scalar
        if (inner.empty()) return false;
        auto pos = inner.rfind('x');
        if (pos == std::string_view::npos || pos == inner.size() - 1) return false;
        std::string_view elem = inner.substr(pos + 1);
        return validate_ir_type_str(elem);
    }

    // ============================================================================
    // §6 — lowering_violation / lowering_result
    //
    // Structured error surface returned by the frontend when a Crank source
    // construct cannot be lowered through this contract.
    // ============================================================================

    struct lowering_violation {
        std::string source_type; // Crank type or feature name
        std::string message; // human-readable reason
    };

    struct lowering_result {
        std::optional<std::string> ir_type_str; // set on success (for type mapping)
        std::vector<lowering_violation> violations; // non-empty on failure

        [[nodiscard]] bool ok() const noexcept { return violations.empty(); }
    };

    // ============================================================================
    // §7 — lower_scalar_type: combines §2 + §5 into a single validated step.
    //
    // Returns a lowering_result with ir_type_str set on success, or a violation
    // if the source type is unknown or the resulting string fails §5.
    // ============================================================================

    [[nodiscard]] inline lowering_result
    lower_scalar_type(std::string_view crank_type) noexcept {
        auto ir = crank_type_to_ir_str(crank_type);
        if (!ir) return {{}, {{std::string{crank_type}, "no IR type mapping defined"}}};
        if (!validate_ir_type_str(*ir))
            return {{}, {{std::string{crank_type}, "mapped IR type failed §5 validation: " + *ir}}};
        return {std::move(ir), {}};
    }

    // ============================================================================
    // §8 — lower_tensor_type: combines §3 + §5 into a single validated step.
    // ============================================================================

    [[nodiscard]] inline lowering_result
    lower_tensor_type(std::string_view elem_type, std::uint8_t rank,
                      const std::vector<std::int64_t>& dims) noexcept {
        auto ir = tensor_type_to_ir_str(elem_type, rank, dims);
        if (!ir) return {{}, {{std::string{elem_type}, "no memref mapping: unmappable element type or rank 0"}}};
        if (!validate_ir_type_str(*ir))
            return {{}, {{std::string{elem_type}, "derived memref failed §5 validation: " + *ir}}};
        return {std::move(ir), {}};
    }

    // ============================================================================
    // §9 — Stable string tables for compare predicates, guard kinds, trap kinds,
    //       and failure policies.
    //
    // Every frontend MUST use these canonical strings when constructing compare_attr,
    // guard_attr, and trap_attr wire fields.  This is the single source of truth
    // (spec §17); Crank and any future frontend reference these tables.
    //
    // The tables are `inline constexpr std::array<std::string_view, N>`.
    // Validation helpers check membership.
    // ============================================================================

    // Integer compare predicates (icmp) — ordered alphabetically within signed/unsigned groups.
    inline constexpr std::array<std::string_view, 10> k_icmp_predicates = {
        {
            "eq", // equal
            "ne", // not equal
            "slt", // signed less-than
            "sle", // signed less-or-equal
            "sgt", // signed greater-than
            "sge", // signed greater-or-equal
            "ult", // unsigned less-than
            "ule", // unsigned less-or-equal
            "ugt", // unsigned greater-than
            "uge", // unsigned greater-or-equal
        }
    };

    // Float compare predicates (fcmp) — "o" prefix = ordered (NaN-safe false).
    inline constexpr std::array<std::string_view, 6> k_fcmp_predicates = {
        {
            "oeq", // ordered equal
            "one", // ordered not-equal
            "olt", // ordered less-than
            "ole", // ordered less-or-equal
            "ogt", // ordered greater-than
            "oge", // ordered greater-or-equal
        }
    };

    // Guard kinds — must match codegen::hl::guard_kind enumerator order.
    inline constexpr std::array<std::string_view, 7> k_guard_kinds = {
        {
            "bounds", // array/slice bounds check
            "div_by_zero", // integer division denominator check
            "range_cast", // narrowing integer cast range check
            "assert", // user-written assertion
            "overflow", // checked arithmetic overflow
            "transaction", // transaction precondition (resource accessible)
            "parallel_safety", // data-race safety assertion
        }
    };

    // Trap kinds — must match codegen::hl::trap_kind enumerator order.
    inline constexpr std::array<std::string_view, 8> k_trap_kinds = {
        {
            "bounds_violation", // bounds guard failed
            "div_by_zero", // division-by-zero guard failed
            "range_conversion", // narrowing cast out of range
            "assert_failed", // user assertion failed
            "overflow_checked", // checked arithmetic overflowed
            "tx_failed", // transaction abort / precondition violated
            "unreachable", // code marked unreachable was reached
            "host_trap", // host-provided trap handler
        }
    };

    // Failure policies — must match codegen::hl::failure_policy enumerator order.
    inline constexpr std::array<std::string_view, 4> k_failure_policies = {
        {
            "return_result", // return an error/null result on guard failure
            "trap", // lower guard failure to a trap terminator
            "terminate", // lower to process-termination (abort/unreachable)
            "host_handler", // delegate to a registered host failure handler
        }
    };

    // Transaction isolation levels.
    inline constexpr std::array<std::string_view, 3> k_tx_isolation_levels = {
        {
            "read_committed",
            "repeatable_read",
            "serializable",
        }
    };

    // Validation helpers.

    [[nodiscard]] inline bool is_valid_icmp_predicate(std::string_view s) noexcept {
        for (const auto sv : k_icmp_predicates) if (sv == s) return true;
        return false;
    }

    [[nodiscard]] inline bool is_valid_fcmp_predicate(std::string_view s) noexcept {
        for (const auto sv : k_fcmp_predicates) if (sv == s) return true;
        return false;
    }

    [[nodiscard]] inline bool is_valid_compare_predicate(std::string_view s) noexcept {
        return is_valid_icmp_predicate(s) || is_valid_fcmp_predicate(s);
    }

    [[nodiscard]] inline bool is_valid_guard_kind(std::string_view s) noexcept {
        for (const auto sv : k_guard_kinds) if (sv == s) return true;
        return false;
    }

    [[nodiscard]] inline bool is_valid_trap_kind(std::string_view s) noexcept {
        for (const auto sv : k_trap_kinds) if (sv == s) return true;
        return false;
    }

    [[nodiscard]] inline bool is_valid_failure_policy(std::string_view s) noexcept {
        for (const auto sv : k_failure_policies) if (sv == s) return true;
        return false;
    }

    [[nodiscard]] inline bool is_valid_tx_isolation(std::string_view s) noexcept {
        for (const auto sv : k_tx_isolation_levels) if (sv == s) return true;
        return false;
    }
} // namespace lithe::ir::frontend
