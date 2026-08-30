#pragma once

// taranga/ast_tags.hpp — Taranga-owned Vakya tag descriptors (extension band ≥ 2000).
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// Each tag is an empty struct; its vakya::emit::tag_descriptor specialisation
// carries symbol, stable_id (>= kExtensionIdBase = 1000), arity, is_commutative.
// Taranga reserves band 2000–2031 (crank owns 1000–1099). No fork of vakya.hpp.
//
// These tags label the *generic* AST built from BOTH the WAT parse tree and the
// binary decode stream. WAT-form and binary-form of the same module produce the
// same tags in the same order → identical structural_hash (parity invariant).
//
// Tag stable_id allocation (taranga reserved: 2000 – 2031):
//   2000 module_tag        (module …)
//   2001 type_tag          (type (func …))       — a func-type in the type section
//   2002 import_tag         (import "m" "n" …)
//   2003 export_tag         (export "n" (kind idx))
//   2004 func_tag           (func …)              function definition
//   2005 param_tag          (param t…)
//   2006 result_tag         (result t…)
//   2007 local_tag          (local t…)
//   2008 global_tag         (global …)
//   2009 table_tag          (table …)
//   2010 memory_tag         (memory …)
//   2011 elem_tag           (elem …)
//   2012 data_tag           (data …)
//   2013 start_tag          (start idx)
//   2014 body_tag           function instruction sequence (flat)
//   2015 instr_tag          a single plain instruction (opcode + immediates)
//   2016 block_tag          block …  end       structured control
//   2017 loop_tag           loop …  end
//   2018 if_tag             if … [else …] end
//   2019 br_tag             br / br_if / br_table target(s)
//   2020 call_tag           call / call_indirect
//   2021 const_tag          i32.const / f64.const … (literal-bearing instruction)
//   2022 memarg_tag         load/store memarg (align, offset)
//   2023 select_tag         select [(result t)]
//   2024 local_access_tag   local.get / local.set / local.tee
//   2025 global_access_tag  global.get / global.set
//   2026 mem_op_tag         memory.size / memory.grow / memory.copy / memory.fill
//   2027 limits_tag         { min, max? } shared by table/memory
//   2028 name_tag           $name identifier binding (WAT only; index in binary)
//   2029 func_type_ref_tag   typeidx reference from a func / call_indirect
//   2030 unreachable_tag    unreachable / nop / drop (nullary effects)
//   2031 vec_instr_tag      v128 / SIMD opaque instruction (opaque128, later bands)

#include "vakya/vakya.hpp"

namespace taranga {
    // ── Tag structs ──────────────────────────────────────────────────────────────

    struct module_tag {};
    struct type_tag {};
    struct import_tag {};
    struct export_tag {};
    struct func_tag {};
    struct param_tag {};
    struct result_tag {};
    struct local_tag {};
    struct global_tag {};
    struct table_tag {};
    struct memory_tag {};
    struct elem_tag {};
    struct data_tag {};
    struct start_tag {};
    struct body_tag {};
    struct instr_tag {};
    struct block_tag {};
    struct loop_tag {};
    struct if_tag {};
    struct br_tag {};
    struct call_tag {};
    struct const_tag {};
    struct memarg_tag {};
    struct select_tag {};
    struct local_access_tag {};
    struct global_access_tag {};
    struct mem_op_tag {};
    struct limits_tag {};
    struct name_tag {};
    struct func_type_ref_tag {};
    struct unreachable_tag {};
    struct vec_instr_tag {};
} // namespace taranga

// ── Descriptor specialisations ───────────────────────────────────────────────

namespace vakya::emit {
    template <>
    struct tag_descriptor<taranga::module_tag> {
        static constexpr std::string_view symbol = "module";
        static constexpr std::uint32_t stable_id = 2000u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::type_tag> {
        static constexpr std::string_view symbol = "type";
        static constexpr std::uint32_t stable_id = 2001u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::import_tag> {
        static constexpr std::string_view symbol = "import";
        static constexpr std::uint32_t stable_id = 2002u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::export_tag> {
        static constexpr std::string_view symbol = "export";
        static constexpr std::uint32_t stable_id = 2003u;
        static constexpr std::uint8_t arity = 2u; // name + (kind idx)
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::func_tag> {
        static constexpr std::string_view symbol = "func";
        static constexpr std::uint32_t stable_id = 2004u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::param_tag> {
        static constexpr std::string_view symbol = "param";
        static constexpr std::uint32_t stable_id = 2005u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::result_tag> {
        static constexpr std::string_view symbol = "result";
        static constexpr std::uint32_t stable_id = 2006u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::local_tag> {
        static constexpr std::string_view symbol = "local";
        static constexpr std::uint32_t stable_id = 2007u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::global_tag> {
        static constexpr std::string_view symbol = "global";
        static constexpr std::uint32_t stable_id = 2008u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::table_tag> {
        static constexpr std::string_view symbol = "table";
        static constexpr std::uint32_t stable_id = 2009u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::memory_tag> {
        static constexpr std::string_view symbol = "memory";
        static constexpr std::uint32_t stable_id = 2010u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::elem_tag> {
        static constexpr std::string_view symbol = "elem";
        static constexpr std::uint32_t stable_id = 2011u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::data_tag> {
        static constexpr std::string_view symbol = "data";
        static constexpr std::uint32_t stable_id = 2012u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::start_tag> {
        static constexpr std::string_view symbol = "start";
        static constexpr std::uint32_t stable_id = 2013u;
        static constexpr std::uint8_t arity = 1u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::body_tag> {
        static constexpr std::string_view symbol = "body";
        static constexpr std::uint32_t stable_id = 2014u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::instr_tag> {
        static constexpr std::string_view symbol = "instr";
        static constexpr std::uint32_t stable_id = 2015u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::block_tag> {
        static constexpr std::string_view symbol = "block";
        static constexpr std::uint32_t stable_id = 2016u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::loop_tag> {
        static constexpr std::string_view symbol = "loop";
        static constexpr std::uint32_t stable_id = 2017u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::if_tag> {
        static constexpr std::string_view symbol = "if";
        static constexpr std::uint32_t stable_id = 2018u;
        static constexpr std::uint8_t arity = kVariadicArity; // cond region + then + optional else
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::br_tag> {
        static constexpr std::string_view symbol = "br";
        static constexpr std::uint32_t stable_id = 2019u;
        static constexpr std::uint8_t arity = kVariadicArity; // targets…
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::call_tag> {
        static constexpr std::string_view symbol = "call";
        static constexpr std::uint32_t stable_id = 2020u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::const_tag> {
        static constexpr std::string_view symbol = "const";
        static constexpr std::uint32_t stable_id = 2021u;
        static constexpr std::uint8_t arity = 0u; // literal in node ext
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::memarg_tag> {
        static constexpr std::string_view symbol = "memarg";
        static constexpr std::uint32_t stable_id = 2022u;
        static constexpr std::uint8_t arity = 0u; // align/offset in node ext
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::select_tag> {
        static constexpr std::string_view symbol = "select";
        static constexpr std::uint32_t stable_id = 2023u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::local_access_tag> {
        static constexpr std::string_view symbol = "local_access";
        static constexpr std::uint32_t stable_id = 2024u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::global_access_tag> {
        static constexpr std::string_view symbol = "global_access";
        static constexpr std::uint32_t stable_id = 2025u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::mem_op_tag> {
        static constexpr std::string_view symbol = "mem_op";
        static constexpr std::uint32_t stable_id = 2026u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::limits_tag> {
        static constexpr std::string_view symbol = "limits";
        static constexpr std::uint32_t stable_id = 2027u;
        static constexpr std::uint8_t arity = kVariadicArity; // min [max]
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::name_tag> {
        static constexpr std::string_view symbol = "name";
        static constexpr std::uint32_t stable_id = 2028u;
        static constexpr std::uint8_t arity = 0u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::func_type_ref_tag> {
        static constexpr std::string_view symbol = "func_type_ref";
        static constexpr std::uint32_t stable_id = 2029u;
        static constexpr std::uint8_t arity = 0u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::unreachable_tag> {
        static constexpr std::string_view symbol = "unreachable";
        static constexpr std::uint32_t stable_id = 2030u;
        static constexpr std::uint8_t arity = 0u;
        static constexpr bool is_commutative = false;
    };

    template <>
    struct tag_descriptor<taranga::vec_instr_tag> {
        static constexpr std::string_view symbol = "vec_instr";
        static constexpr std::uint32_t stable_id = 2031u;
        static constexpr std::uint8_t arity = kVariadicArity;
        static constexpr bool is_commutative = false;
    };
} // namespace vakya::emit
