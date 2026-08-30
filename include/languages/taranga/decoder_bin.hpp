#pragma once

// taranga/decoder_bin.hpp — WebAssembly binary (.wasm) section-stream decoder.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// Decodes a .wasm image into a raw_module: flat, typed section tables that mirror
// the binary layout one-to-one. This is the binary counterpart to the WAT parse
// tree; build_ast.hpp consumes EITHER a WAT parse_tree OR a raw_module and emits
// the same generic AST, so the two frontends converge (structural-hash parity).
//
// We use taranga::byte_cursor (lexer.hpp) — not lexy's byte DSL — because section
// walking is a straight-line length-prefixed scan where explicit cursor control is
// simpler and strictly pay-for-use (a WAT-only build never instantiates this).
//
// Malformed input never traps: every decode step returns std::optional or records
// a TARANGA-BIN-### diagnostic and stops. Partial results are discarded on error.

#include "languages/taranga/lexer.hpp"
#include "languages/taranga/wasm_types.hpp"
#include "languages/taranga/source_span.hpp"

#include "vakya/diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taranga {

    // =========================================================================
    // Section ids — the twelve known sections (custom = 0).
    // =========================================================================

    enum class section_id : std::uint8_t {
        custom = 0,
        type = 1,
        import = 2,
        function = 3, // typeidx per defined function
        table = 4,
        memory = 5,
        global = 6,
        export_ = 7,
        start = 8,
        element = 9,
        code = 10, // function bodies
        data = 11,
        data_count = 12
    };

    // =========================================================================
    // Raw structural records — one struct per section entry. These carry byte
    // spans so diagnostics on binary input can point at the offending bytes.
    // =========================================================================

    struct raw_import {
        std::string module_name;
        std::string field_name;
        external_kind kind{};
        std::uint32_t type_index = 0;   // kind == function
        table_type table{};             // kind == table
        mem_type memory{};              // kind == memory
        global_type global{};           // kind == global
        byte_span span{};
    };

    struct raw_export {
        std::string name;
        external_kind kind{};
        std::uint32_t index = 0;
        byte_span span{};
    };

    struct raw_global {
        global_type type{};
        std::vector<std::uint8_t> init_expr; // raw const-expr bytecode (incl. 0x0B end)
        byte_span span{};
    };

    struct raw_code {
        std::vector<value_type> locals; // expanded (run-length decoded) local types
        std::span<const std::uint8_t> body; // instruction bytes, excluding the size prefix
        byte_span span{};
    };

    struct raw_data {
        std::uint32_t memory_index = 0;
        bool active = true;
        std::vector<std::uint8_t> offset_expr; // const-expr (active only)
        std::span<const std::uint8_t> bytes;
        byte_span span{};
    };

    struct raw_element {
        std::uint32_t table_index = 0;
        std::vector<std::uint8_t> offset_expr;
        std::vector<std::uint32_t> function_indices;
        byte_span span{};
    };

    // The full decoded image — flat tables in section order.
    struct raw_module {
        std::uint32_t version = 1;
        std::vector<func_type> types;
        std::vector<raw_import> imports;
        std::vector<std::uint32_t> functions; // typeidx per defined function
        std::vector<table_type> tables;
        std::vector<mem_type> memories;
        std::vector<raw_global> globals;
        std::vector<raw_export> exports;
        std::optional<std::uint32_t> start_function;
        std::vector<raw_element> elements;
        std::vector<raw_code> code;
        std::vector<raw_data> data;
    };

    struct decode_result {
        std::optional<raw_module> module;
        vakya::diag::collecting_sink diagnostics;

        [[nodiscard]] bool ok() const noexcept { return module.has_value(); }
    };

    // =========================================================================
    // Decoder — one method per section. Header-only, no state beyond the cursor
    // and the accumulating raw_module.
    // =========================================================================

    namespace detail {

        // The 8-byte module header: magic "\0asm" + u32 version (LE).
        inline constexpr std::array<std::uint8_t, 4> k_wasm_magic = {0x00, 0x61, 0x73, 0x6D};
        inline constexpr std::uint32_t k_wasm_version = 1u;

        [[nodiscard]] inline std::optional<func_type>
        decode_func_type(byte_cursor& c) {
            auto form = c.read_u8();
            if (!form || *form != 0x60) return std::nullopt; // func type tag
            func_type ft;
            auto n_params = decode_u32(c);
            if (!n_params) return std::nullopt;
            ft.params.reserve(*n_params);
            for (std::uint32_t i = 0; i < *n_params; ++i) {
                auto b = c.read_u8();
                if (!b) return std::nullopt;
                auto vt = value_type_from_binary(*b);
                if (!vt) return std::nullopt;
                ft.params.push_back(*vt);
            }
            auto n_results = decode_u32(c);
            if (!n_results) return std::nullopt;
            ft.results.reserve(*n_results);
            for (std::uint32_t i = 0; i < *n_results; ++i) {
                auto b = c.read_u8();
                if (!b) return std::nullopt;
                auto vt = value_type_from_binary(*b);
                if (!vt) return std::nullopt;
                ft.results.push_back(*vt);
            }
            return ft;
        }

        [[nodiscard]] inline std::optional<limits> decode_limits(byte_cursor& c) {
            auto flag = c.read_u8();
            if (!flag) return std::nullopt;
            auto min = decode_u32(c);
            if (!min) return std::nullopt;
            limits l;
            l.min = *min;
            if (*flag & 0x01u) { // has max
                auto max = decode_u32(c);
                if (!max) return std::nullopt;
                l.max = *max;
            }
            return l;
        }

        [[nodiscard]] inline std::optional<global_type> decode_global_type(byte_cursor& c) {
            auto b = c.read_u8();
            if (!b) return std::nullopt;
            auto vt = value_type_from_binary(*b);
            if (!vt) return std::nullopt;
            auto mut = c.read_u8();
            if (!mut || (*mut != 0x00 && *mut != 0x01)) return std::nullopt;
            return global_type{*vt, *mut == 0x01};
        }

        // Read a const-expr: raw bytes up through the terminating 0x0B (end).
        [[nodiscard]] inline std::optional<std::vector<std::uint8_t>>
        decode_const_expr(byte_cursor& c) {
            std::vector<std::uint8_t> expr;
            while (true) {
                auto b = c.read_u8();
                if (!b) return std::nullopt;
                expr.push_back(*b);
                if (*b == 0x0B) return expr; // end opcode
            }
        }

    } // namespace detail

    class binary_decoder {
    public:
        [[nodiscard]] static decode_result decode(std::span<const std::uint8_t> image) {
            decode_result result;
            byte_cursor c(image);

            // Header: magic + version.
            auto magic = c.read_bytes(4);
            if (!magic || !std::equal(magic->begin(), magic->end(),
                                      detail::k_wasm_magic.begin())) {
                result.diagnostics.on_diagnostic(make_error(
                    "TARANGA-BIN-001", "missing or bad \\0asm magic header"));
                return result;
            }
            auto version = c.read_le_u32();
            if (!version) {
                result.diagnostics.on_diagnostic(make_error(
                    "TARANGA-BIN-002", "truncated version field"));
                return result;
            }
            if (*version != detail::k_wasm_version) {
                result.diagnostics.on_diagnostic(make_error(
                    "TARANGA-BIN-003",
                    "unsupported binary version " + std::to_string(*version)));
                return result;
            }

            raw_module mod;
            mod.version = *version;

            // Section loop: id (u8) + size (u32) + payload[size].
            while (!c.at_end()) {
                const std::uint32_t sec_offset = c.offset();
                auto id_byte = c.read_u8();
                if (!id_byte) break;
                auto size = decode_u32(c);
                if (!size) {
                    result.diagnostics.on_diagnostic(make_error(
                        "TARANGA-BIN-004", "truncated section size",
                        span_from_bytes({sec_offset, 1})));
                    return result;
                }
                auto payload = c.read_bytes(*size);
                if (!payload) {
                    result.diagnostics.on_diagnostic(make_error(
                        "TARANGA-BIN-005", "section length exceeds image",
                        span_from_bytes({sec_offset, 1})));
                    return result;
                }
                if (*id_byte > static_cast<std::uint8_t>(section_id::data_count)) {
                    result.diagnostics.on_diagnostic(make_warning(
                        "TARANGA-BIN-006",
                        "unknown section id " + std::to_string(*id_byte) + " skipped",
                        span_from_bytes({sec_offset, 1})));
                    continue; // forward-compat: skip unknown
                }
                byte_cursor sub(*payload);
                if (!decode_section(static_cast<section_id>(*id_byte), sub, sec_offset,
                                    mod, result.diagnostics)) {
                    return result; // diagnostic already recorded
                }
            }

            result.module = std::move(mod);
            return result;
        }

    private:
        [[nodiscard]] static bool
        decode_section(section_id id, byte_cursor& c, std::uint32_t base,
                       raw_module& mod, vakya::diag::collecting_sink& diags) {
            switch (id) {
            case section_id::custom:
                return true; // name/producers/etc — ignored in v1
            case section_id::type: return decode_type_section(c, base, mod, diags);
            case section_id::import: return decode_import_section(c, base, mod, diags);
            case section_id::function: return decode_function_section(c, base, mod, diags);
            case section_id::table: return decode_table_section(c, base, mod, diags);
            case section_id::memory: return decode_memory_section(c, base, mod, diags);
            case section_id::global: return decode_global_section(c, base, mod, diags);
            case section_id::export_: return decode_export_section(c, base, mod, diags);
            case section_id::start: return decode_start_section(c, base, mod, diags);
            case section_id::element: return decode_element_section(c, base, mod, diags);
            case section_id::code: return decode_code_section(c, base, mod, diags);
            case section_id::data: return decode_data_section(c, base, mod, diags);
            case section_id::data_count: return true; // count hint — validated elsewhere
            }
            return true;
        }

        static bool fail(vakya::diag::collecting_sink& diags, std::string_view code,
                         std::string_view msg, std::uint32_t at) {
            diags.on_diagnostic(make_error(code, msg, span_from_bytes({at, 1})));
            return false;
        }

        // ----- per-section decoders -----------------------------------------

        static bool decode_type_section(byte_cursor& c, std::uint32_t base,
                                        raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-010", "bad type count", base);
            mod.types.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                auto ft = detail::decode_func_type(c);
                if (!ft) return fail(diags, "TARANGA-BIN-011", "malformed func type", base + c.offset());
                mod.types.push_back(std::move(*ft));
            }
            return true;
        }

        static bool decode_import_section(byte_cursor& c, std::uint32_t base,
                                          raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-020", "bad import count", base);
            mod.imports.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                const std::uint32_t off = base + c.offset();
                raw_import im;
                auto m = decode_name(c);
                auto f = decode_name(c);
                if (!m || !f) return fail(diags, "TARANGA-BIN-021", "bad import name", off);
                im.module_name = std::string(*m);
                im.field_name = std::string(*f);
                auto kb = c.read_u8();
                if (!kb) return fail(diags, "TARANGA-BIN-022", "missing import kind", off);
                auto kind = external_kind_from_binary(*kb);
                if (!kind) return fail(diags, "TARANGA-BIN-023", "bad import kind", off);
                im.kind = *kind;
                switch (*kind) {
                case external_kind::function: {
                    auto ti = decode_u32(c);
                    if (!ti) return fail(diags, "TARANGA-BIN-024", "bad import typeidx", off);
                    im.type_index = *ti;
                    break;
                }
                case external_kind::table: {
                    auto et = c.read_u8();
                    auto vt = et ? value_type_from_binary(*et) : std::nullopt;
                    auto lim = detail::decode_limits(c);
                    if (!vt || !lim) return fail(diags, "TARANGA-BIN-025", "bad import table", off);
                    im.table = table_type{*vt, *lim};
                    break;
                }
                case external_kind::memory: {
                    auto lim = detail::decode_limits(c);
                    if (!lim) return fail(diags, "TARANGA-BIN-026", "bad import memory", off);
                    im.memory = mem_type{*lim, false};
                    break;
                }
                case external_kind::global: {
                    auto gt = detail::decode_global_type(c);
                    if (!gt) return fail(diags, "TARANGA-BIN-027", "bad import global", off);
                    im.global = *gt;
                    break;
                }
                }
                im.span = {off, base + c.offset() - off};
                mod.imports.push_back(std::move(im));
            }
            return true;
        }

        static bool decode_function_section(byte_cursor& c, std::uint32_t base,
                                            raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-030", "bad function count", base);
            mod.functions.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                auto ti = decode_u32(c);
                if (!ti) return fail(diags, "TARANGA-BIN-031", "bad function typeidx", base + c.offset());
                mod.functions.push_back(*ti);
            }
            return true;
        }

        static bool decode_table_section(byte_cursor& c, std::uint32_t base,
                                         raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-040", "bad table count", base);
            mod.tables.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                auto et = c.read_u8();
                auto vt = et ? value_type_from_binary(*et) : std::nullopt;
                auto lim = detail::decode_limits(c);
                if (!vt || !lim) return fail(diags, "TARANGA-BIN-041", "bad table type", base + c.offset());
                mod.tables.push_back(table_type{*vt, *lim});
            }
            return true;
        }

        static bool decode_memory_section(byte_cursor& c, std::uint32_t base,
                                          raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-050", "bad memory count", base);
            mod.memories.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                auto lim = detail::decode_limits(c);
                if (!lim) return fail(diags, "TARANGA-BIN-051", "bad memory type", base + c.offset());
                mod.memories.push_back(mem_type{*lim, false});
            }
            return true;
        }

        static bool decode_global_section(byte_cursor& c, std::uint32_t base,
                                          raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-060", "bad global count", base);
            mod.globals.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                const std::uint32_t off = base + c.offset();
                auto gt = detail::decode_global_type(c);
                if (!gt) return fail(diags, "TARANGA-BIN-061", "bad global type", off);
                auto init = detail::decode_const_expr(c);
                if (!init) return fail(diags, "TARANGA-BIN-062", "bad global init expr", off);
                mod.globals.push_back(raw_global{*gt, std::move(*init), {off, base + c.offset() - off}});
            }
            return true;
        }

        static bool decode_export_section(byte_cursor& c, std::uint32_t base,
                                          raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-070", "bad export count", base);
            mod.exports.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                const std::uint32_t off = base + c.offset();
                auto name = decode_name(c);
                if (!name) return fail(diags, "TARANGA-BIN-071", "bad export name", off);
                auto kb = c.read_u8();
                auto kind = kb ? external_kind_from_binary(*kb) : std::nullopt;
                auto idx = decode_u32(c);
                if (!kind || !idx) return fail(diags, "TARANGA-BIN-072", "bad export descriptor", off);
                mod.exports.push_back(raw_export{std::string(*name), *kind, *idx,
                                                 {off, base + c.offset() - off}});
            }
            return true;
        }

        static bool decode_start_section(byte_cursor& c, std::uint32_t base,
                                         raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto idx = decode_u32(c);
            if (!idx) return fail(diags, "TARANGA-BIN-080", "bad start funcidx", base);
            mod.start_function = *idx;
            return true;
        }

        static bool decode_element_section(byte_cursor& c, std::uint32_t base,
                                           raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-090", "bad element count", base);
            mod.elements.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                const std::uint32_t off = base + c.offset();
                // v1: only active, table 0, flag 0x00 form (funcidx vector).
                auto flag = decode_u32(c);
                if (!flag) return fail(diags, "TARANGA-BIN-091", "bad element flag", off);
                raw_element el;
                el.table_index = 0;
                auto offset_expr = detail::decode_const_expr(c);
                if (!offset_expr) return fail(diags, "TARANGA-BIN-092", "bad element offset", off);
                el.offset_expr = std::move(*offset_expr);
                auto count = decode_u32(c);
                if (!count) return fail(diags, "TARANGA-BIN-093", "bad element func count", off);
                el.function_indices.reserve(*count);
                for (std::uint32_t j = 0; j < *count; ++j) {
                    auto fi = decode_u32(c);
                    if (!fi) return fail(diags, "TARANGA-BIN-094", "bad element funcidx", off);
                    el.function_indices.push_back(*fi);
                }
                el.span = {off, base + c.offset() - off};
                mod.elements.push_back(std::move(el));
            }
            return true;
        }

        static bool decode_code_section(byte_cursor& c, std::uint32_t base,
                                        raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-100", "bad code count", base);
            mod.code.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                const std::uint32_t off = base + c.offset();
                auto body_size = decode_u32(c);
                if (!body_size) return fail(diags, "TARANGA-BIN-101", "bad code body size", off);
                auto body = c.read_bytes(*body_size);
                if (!body) return fail(diags, "TARANGA-BIN-102", "code body exceeds section", off);
                byte_cursor body_cur(*body);
                raw_code code;
                // Locals: vector of (count, valtype) run-length groups.
                auto n_local_groups = decode_u32(body_cur);
                if (!n_local_groups) return fail(diags, "TARANGA-BIN-103", "bad locals count", off);
                for (std::uint32_t g = 0; g < *n_local_groups; ++g) {
                    auto cnt = decode_u32(body_cur);
                    auto tb = body_cur.read_u8();
                    auto vt = tb ? value_type_from_binary(*tb) : std::nullopt;
                    if (!cnt || !vt) return fail(diags, "TARANGA-BIN-104", "bad local group", off);
                    for (std::uint32_t k = 0; k < *cnt; ++k) code.locals.push_back(*vt);
                }
                // Remaining bytes in body_cur are the instruction stream.
                const std::size_t consumed = body_cur.offset();
                code.body = body->subspan(consumed);
                code.span = {off, base + c.offset() - off};
                mod.code.push_back(std::move(code));
            }
            return true;
        }

        static bool decode_data_section(byte_cursor& c, std::uint32_t base,
                                        raw_module& mod, vakya::diag::collecting_sink& diags) {
            auto n = decode_u32(c);
            if (!n) return fail(diags, "TARANGA-BIN-110", "bad data count", base);
            mod.data.reserve(*n);
            for (std::uint32_t i = 0; i < *n; ++i) {
                const std::uint32_t off = base + c.offset();
                auto flag = decode_u32(c);
                if (!flag) return fail(diags, "TARANGA-BIN-111", "bad data flag", off);
                raw_data d;
                if (*flag == 0) { // active, memory 0, offset const-expr
                    d.active = true;
                    d.memory_index = 0;
                    auto oe = detail::decode_const_expr(c);
                    if (!oe) return fail(diags, "TARANGA-BIN-112", "bad data offset expr", off);
                    d.offset_expr = std::move(*oe);
                } else if (*flag == 1) { // passive
                    d.active = false;
                } else {
                    return fail(diags, "TARANGA-BIN-113", "unsupported data segment flag", off);
                }
                auto len = decode_u32(c);
                if (!len) return fail(diags, "TARANGA-BIN-114", "bad data length", off);
                auto bytes = c.read_bytes(*len);
                if (!bytes) return fail(diags, "TARANGA-BIN-115", "data bytes exceed section", off);
                d.bytes = *bytes;
                d.span = {off, base + c.offset() - off};
                mod.data.push_back(std::move(d));
            }
            return true;
        }
    };

} // namespace taranga
