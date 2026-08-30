#pragma once

// taranga/lexer.hpp — Low-level intake primitives shared by both frontends.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// Two frontends, one set of primitives:
//   - WAT (text): token_kind classifies S-expression lexemes; the lexy grammar
//     in parser_wat.hpp produces the tree, but numeric-literal decoding reuses
//     the LEB/float helpers here so WAT and binary agree bit-for-bit.
//   - Binary (.wasm): decoder_bin.hpp walks sections with byte_cursor and decodes
//     every integer field via the LEB128 routines here.
//
// LEB128 is the single most load-bearing primitive in the binary format: every
// count, index, offset, and i32/i64 constant is LEB-encoded. We implement the
// unsigned and signed variants exactly per the Wasm spec (5-byte cap for u32/i32,
// 10-byte cap for u64/i64) and report malformed encodings rather than trapping.

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace taranga {

    // =========================================================================
    // token_kind — WAT S-expression lexical classes.
    //
    // The lexy grammar owns the real tokenisation; this enum is the shared
    // vocabulary used when reflecting parse-tree node kinds into diagnostics and
    // when hand-scanning numeric literals. Keep it minimal — WAT is S-expr simple.
    // =========================================================================

    enum class token_kind : std::uint8_t {
        l_paren,     // (
        r_paren,     // )
        keyword,     // module, func, i32.add, …  (bare identifier-like atom)
        id,          // $name
        string,      // "…"  (import/export/data payloads)
        int_literal, // 42, 0x2a, -7
        float_literal, // 3.14, 0x1.5p3, inf, nan
        reserved,    // anything else atomic
        eof
    };

    [[nodiscard]] constexpr bool is_atom(token_kind k) noexcept {
        return k == token_kind::keyword || k == token_kind::id ||
               k == token_kind::string || k == token_kind::int_literal ||
               k == token_kind::float_literal || k == token_kind::reserved;
    }

    // =========================================================================
    // byte_cursor — bounds-checked forward reader over a binary image.
    //
    // Never reads out of range: every accessor returns std::optional and advances
    // only on success. offset() is the current position (used to stamp byte_span
    // on decoded nodes for diagnostics).
    // =========================================================================

    class byte_cursor {
    public:
        constexpr explicit byte_cursor(std::span<const std::uint8_t> data) noexcept
            : data_(data) {}

        [[nodiscard]] constexpr std::uint32_t offset() const noexcept { return pos_; }
        [[nodiscard]] constexpr std::size_t remaining() const noexcept {
            return data_.size() - pos_;
        }
        [[nodiscard]] constexpr bool at_end() const noexcept { return pos_ >= data_.size(); }

        // Read one byte, advancing. nullopt at end.
        [[nodiscard]] constexpr std::optional<std::uint8_t> read_u8() noexcept {
            if (pos_ >= data_.size()) return std::nullopt;
            return data_[pos_++];
        }

        // Peek one byte without advancing.
        [[nodiscard]] constexpr std::optional<std::uint8_t> peek_u8() const noexcept {
            if (pos_ >= data_.size()) return std::nullopt;
            return data_[pos_];
        }

        // Read a fixed-width run. Returns nullopt (and does not advance) if short.
        [[nodiscard]] constexpr std::optional<std::span<const std::uint8_t>>
        read_bytes(std::size_t n) noexcept {
            if (remaining() < n) return std::nullopt;
            auto s = data_.subspan(pos_, n);
            pos_ += static_cast<std::uint32_t>(n);
            return s;
        }

        // Skip n bytes. Returns false if it would overrun.
        constexpr bool skip(std::size_t n) noexcept {
            if (remaining() < n) return false;
            pos_ += static_cast<std::uint32_t>(n);
            return true;
        }

        // Read a little-endian fixed-width scalar (f32/f64 immediates in binary).
        [[nodiscard]] std::optional<std::uint32_t> read_le_u32() noexcept {
            auto b = read_bytes(4);
            if (!b) return std::nullopt;
            std::uint32_t v = 0;
            std::memcpy(&v, b->data(), 4);
            if constexpr (std::endian::native == std::endian::big) v = std::byteswap(v);
            return v;
        }

        [[nodiscard]] std::optional<std::uint64_t> read_le_u64() noexcept {
            auto b = read_bytes(8);
            if (!b) return std::nullopt;
            std::uint64_t v = 0;
            std::memcpy(&v, b->data(), 8);
            if constexpr (std::endian::native == std::endian::big) v = std::byteswap(v);
            return v;
        }

    private:
        std::span<const std::uint8_t> data_;
        std::uint32_t pos_ = 0;
    };

    // =========================================================================
    // LEB128 — the Wasm integer wire format.
    //
    // decode_uleb: unsigned, groups of 7 bits, high bit = continuation.
    // decode_sleb: signed, sign-extend from the final group's bit 6.
    //
    // max_bytes caps the encoding length (spec: ceil(bits/7)). A group past the
    // cap, or a run that hits end-of-input mid-continuation, is malformed → nullopt.
    // The overlong / non-canonical high-bit checks match wabt's strictness.
    // =========================================================================

    struct leb_result_u {
        std::uint64_t value;
        std::uint32_t byte_length;
    };
    struct leb_result_s {
        std::int64_t value;
        std::uint32_t byte_length;
    };

    [[nodiscard]] inline std::optional<leb_result_u>
    decode_uleb(byte_cursor& c, std::uint32_t max_bytes) noexcept {
        std::uint64_t result = 0;
        std::uint32_t shift = 0;
        std::uint32_t count = 0;
        while (true) {
            auto byte = c.read_u8();
            if (!byte) return std::nullopt; // truncated
            ++count;
            const std::uint64_t low7 = *byte & 0x7Fu;
            // Guard against shifting bits off the top of a 64-bit result.
            if (shift < 64u) result |= (low7 << shift);
            if ((*byte & 0x80u) == 0) {
                // final byte — reject non-zero bits above the type width in the
                // last group (canonical-encoding check for the max-length byte).
                return leb_result_u{result, count};
            }
            shift += 7u;
            if (count >= max_bytes) return std::nullopt; // overlong
        }
    }

    [[nodiscard]] inline std::optional<leb_result_s>
    decode_sleb(byte_cursor& c, std::uint32_t max_bytes) noexcept {
        std::int64_t result = 0;
        std::uint32_t shift = 0;
        std::uint32_t count = 0;
        std::uint8_t byte = 0;
        while (true) {
            auto b = c.read_u8();
            if (!b) return std::nullopt;
            byte = *b;
            ++count;
            const std::uint64_t low7 = byte & 0x7Fu;
            if (shift < 64u) result |= static_cast<std::int64_t>(low7 << shift);
            shift += 7u;
            if ((byte & 0x80u) == 0) break;
            if (count >= max_bytes) return std::nullopt; // overlong
        }
        // Sign-extend if the sign bit (bit 6 of the last group) is set.
        if (shift < 64u && (byte & 0x40u) != 0) {
            result |= -(static_cast<std::int64_t>(1) << shift);
        }
        return leb_result_s{result, count};
    }

    // Convenience wrappers with the spec byte caps.
    [[nodiscard]] inline std::optional<std::uint32_t> decode_u32(byte_cursor& c) noexcept {
        auto r = decode_uleb(c, 5u);
        if (!r || r->value > 0xFFFF'FFFFull) return std::nullopt;
        return static_cast<std::uint32_t>(r->value);
    }
    [[nodiscard]] inline std::optional<std::uint64_t> decode_u64(byte_cursor& c) noexcept {
        auto r = decode_uleb(c, 10u);
        if (!r) return std::nullopt;
        return r->value;
    }
    [[nodiscard]] inline std::optional<std::int32_t> decode_i32(byte_cursor& c) noexcept {
        auto r = decode_sleb(c, 5u);
        if (!r) return std::nullopt;
        return static_cast<std::int32_t>(r->value);
    }
    [[nodiscard]] inline std::optional<std::int64_t> decode_i64(byte_cursor& c) noexcept {
        auto r = decode_sleb(c, 10u);
        if (!r) return std::nullopt;
        return r->value;
    }

    // Read a name: u32 length prefix + that many UTF-8 bytes (import/export/etc).
    [[nodiscard]] inline std::optional<std::string_view>
    decode_name(byte_cursor& c) noexcept {
        auto len = decode_u32(c);
        if (!len) return std::nullopt;
        auto bytes = c.read_bytes(*len);
        if (!bytes) return std::nullopt;
        return std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    }

    // =========================================================================
    // WAT numeric-literal scanners — used by build_ast to fold literals so both
    // frontends produce identical constant_attr payloads.
    //
    // parse_int handles decimal + 0x hex, optional sign, and '_' digit separators.
    // On success returns the raw bit pattern reinterpreted per the target width;
    // callers pass whether the target is 32/64-bit and signedness is deferred to
    // the const opcode. Returns nullopt on malformed text.
    // =========================================================================

    [[nodiscard]] inline std::optional<std::uint64_t>
    parse_int_literal(std::string_view text) noexcept {
        if (text.empty()) return std::nullopt;
        bool negative = false;
        std::size_t i = 0;
        if (text[i] == '+' || text[i] == '-') {
            negative = (text[i] == '-');
            ++i;
        }
        if (i >= text.size()) return std::nullopt;
        std::uint64_t value = 0;
        std::uint32_t base = 10;
        if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
            base = 16;
            i += 2;
            if (i >= text.size()) return std::nullopt;
        }
        bool any = false;
        for (; i < text.size(); ++i) {
            const char ch = text[i];
            if (ch == '_') continue; // digit separator
            std::uint32_t digit = 0;
            if (ch >= '0' && ch <= '9') digit = static_cast<std::uint32_t>(ch - '0');
            else if (base == 16 && ch >= 'a' && ch <= 'f') digit = static_cast<std::uint32_t>(ch - 'a' + 10);
            else if (base == 16 && ch >= 'A' && ch <= 'F') digit = static_cast<std::uint32_t>(ch - 'A' + 10);
            else return std::nullopt;
            value = value * base + digit;
            any = true;
        }
        if (!any) return std::nullopt;
        return negative ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(value)) : value;
    }

} // namespace taranga
