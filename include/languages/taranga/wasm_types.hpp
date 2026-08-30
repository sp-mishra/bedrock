#pragma once

// taranga/wasm_types.hpp — WebAssembly type vocabulary + IR type-string bridge.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// Wasm has a small closed type universe. This header names it once and maps each
// value_type to a Lithe §5 type string, gated through the *generic* validator
// lithe::ir::frontend::validate_ir_type_str (single source of truth — we never
// hand-roll type-string legality). Scalars map directly; v128 maps to opaque128
// (byte-addressable but opaque to arithmetic — SIMD lands in a later band, §5 of
// the design). funcref/externref are reference types tracked structurally; they
// have no scalar IR string and lower only as call/table operands.
//
// We deliberately do NOT route through lithe::ir::frontend::lower_scalar_type:
// that function maps *Crank* source-type spellings. Taranga owns its own spelling
// and emits the canonical IR string, then validates with the shared gate.

#include "lithe/lithe_ir/frontend/lowering_contract.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace taranga {

    // =========================================================================
    // value_type — Wasm value types (MVP + v128 + reference types)
    //
    // Numeric encoding matches the Wasm binary format value-type bytes so a
    // decoder can cast directly; the negative binary encodings (0x7F..0x6F as
    // signed LEB) are normalised to this enum by the decoder, not stored raw.
    // =========================================================================

    enum class value_type : std::uint8_t {
        i32 = 0,
        i64 = 1,
        f32 = 2,
        f64 = 3,
        v128 = 4,     // SIMD — opaque128 in v1
        funcref = 5,  // reference type
        externref = 6 // reference type
    };

    [[nodiscard]] constexpr bool is_numeric(value_type t) noexcept {
        return t == value_type::i32 || t == value_type::i64 ||
               t == value_type::f32 || t == value_type::f64;
    }

    [[nodiscard]] constexpr bool is_integer(value_type t) noexcept {
        return t == value_type::i32 || t == value_type::i64;
    }

    [[nodiscard]] constexpr bool is_float(value_type t) noexcept {
        return t == value_type::f32 || t == value_type::f64;
    }

    [[nodiscard]] constexpr bool is_reference(value_type t) noexcept {
        return t == value_type::funcref || t == value_type::externref;
    }

    [[nodiscard]] constexpr bool is_vector(value_type t) noexcept {
        return t == value_type::v128;
    }

    // Bit width of the value's storage in linear memory / registers.
    [[nodiscard]] constexpr std::uint32_t bit_width(value_type t) noexcept {
        switch (t) {
        case value_type::i32:
        case value_type::f32: return 32u;
        case value_type::i64:
        case value_type::f64: return 64u;
        case value_type::v128: return 128u;
        case value_type::funcref:
        case value_type::externref: return 64u; // opaque handle
        }
        return 0u;
    }

    [[nodiscard]] constexpr std::uint32_t byte_width(value_type t) noexcept {
        return bit_width(t) / 8u;
    }

    // Short spelling used in diagnostics / WAT round-trips.
    [[nodiscard]] constexpr std::string_view spelling(value_type t) noexcept {
        switch (t) {
        case value_type::i32: return "i32";
        case value_type::i64: return "i64";
        case value_type::f32: return "f32";
        case value_type::f64: return "f64";
        case value_type::v128: return "v128";
        case value_type::funcref: return "funcref";
        case value_type::externref: return "externref";
        }
        return "?";
    }

    // Parse a WAT value-type keyword. Returns nullopt for unknown spellings.
    [[nodiscard]] constexpr std::optional<value_type>
    value_type_from_spelling(std::string_view s) noexcept {
        if (s == "i32") return value_type::i32;
        if (s == "i64") return value_type::i64;
        if (s == "f32") return value_type::f32;
        if (s == "f64") return value_type::f64;
        if (s == "v128") return value_type::v128;
        if (s == "funcref") return value_type::funcref;
        if (s == "externref") return value_type::externref;
        return std::nullopt;
    }

    // Normalise a Wasm binary value-type byte (0x7F..0x6F) to value_type.
    [[nodiscard]] constexpr std::optional<value_type>
    value_type_from_binary(std::uint8_t code) noexcept {
        switch (code) {
        case 0x7F: return value_type::i32;
        case 0x7E: return value_type::i64;
        case 0x7D: return value_type::f32;
        case 0x7C: return value_type::f64;
        case 0x7B: return value_type::v128;
        case 0x70: return value_type::funcref;
        case 0x6F: return value_type::externref;
        default: return std::nullopt;
        }
    }

    // =========================================================================
    // IR type-string bridge — canonical Lithe §5 scalar string per value_type.
    //
    // i32→"i32", i64→"i64", f32→"f32", f64→"f64", v128→"opaque128".
    // Reference types have no scalar string (nullopt): they never enter an
    // arithmetic op and are represented only as call/table handles.
    //
    // Every returned string is validated against the shared §5 gate so a typo
    // here can never leak an ill-typed value into freeze/verify.
    // =========================================================================

    [[nodiscard]] inline std::optional<std::string_view>
    ir_type_str(value_type t) noexcept {
        std::string_view s;
        switch (t) {
        case value_type::i32: s = "i32"; break;
        case value_type::i64: s = "i64"; break;
        case value_type::f32: s = "f32"; break;
        case value_type::f64: s = "f64"; break;
        case value_type::v128: s = "opaque128"; break;
        case value_type::funcref:
        case value_type::externref: return std::nullopt;
        }
        // Shared gate — never trust the literal above blindly.
        if (!lithe::ir::frontend::validate_ir_type_str(s)) return std::nullopt;
        return s;
    }

    // =========================================================================
    // func_type — the only composite type in Wasm MVP: params → results.
    // =========================================================================

    struct func_type {
        std::vector<value_type> params;
        std::vector<value_type> results;

        [[nodiscard]] bool operator==(const func_type&) const = default;
    };

    // =========================================================================
    // limits — shared by memory and table. max is optional (unbounded).
    // =========================================================================

    struct limits {
        std::uint32_t min = 0;
        std::optional<std::uint32_t> max;

        [[nodiscard]] bool operator==(const limits&) const = default;
    };

    // Wasm page size — linear memory is counted in 64 KiB pages.
    inline constexpr std::uint32_t k_wasm_page_bytes = 65536u;

    // =========================================================================
    // mem_type / table_type / global_type
    // =========================================================================

    struct mem_type {
        limits page_limits; // in 64 KiB pages
        bool shared = false; // threads proposal (later band)

        [[nodiscard]] bool operator==(const mem_type&) const = default;
    };

    struct table_type {
        value_type element = value_type::funcref; // funcref | externref
        limits count_limits;

        [[nodiscard]] bool operator==(const table_type&) const = default;
    };

    struct global_type {
        value_type value = value_type::i32;
        bool mutable_ = false;

        [[nodiscard]] bool operator==(const global_type&) const = default;
    };

    // =========================================================================
    // block_type — structured-control result signature.
    //
    // Wasm block/loop/if carry a block type: empty (0x40), a single value_type,
    // or a typeidx (multi-value). We store the discriminant + payload; the
    // typeidx is resolved against the module's type section by the validator.
    // =========================================================================

    enum class block_type_kind : std::uint8_t { empty, single, type_index };

    struct block_type {
        block_type_kind kind = block_type_kind::empty;
        value_type single{};        // valid iff kind == single
        std::uint32_t type_index{}; // valid iff kind == type_index

        [[nodiscard]] bool operator==(const block_type&) const = default;
    };

    // =========================================================================
    // external_kind — the four import/export descriptor kinds.
    // =========================================================================

    enum class external_kind : std::uint8_t { function, table, memory, global };

    [[nodiscard]] constexpr std::string_view spelling(external_kind k) noexcept {
        switch (k) {
        case external_kind::function: return "func";
        case external_kind::table: return "table";
        case external_kind::memory: return "memory";
        case external_kind::global: return "global";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::optional<external_kind>
    external_kind_from_binary(std::uint8_t code) noexcept {
        switch (code) {
        case 0x00: return external_kind::function;
        case 0x01: return external_kind::table;
        case 0x02: return external_kind::memory;
        case 0x03: return external_kind::global;
        default: return std::nullopt;
        }
    }

} // namespace taranga
