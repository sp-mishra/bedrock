#pragma once

// =============================================================================
// lithe_ir/format.hpp — IR format descriptors (declarations only)
//
// Provides the format vocabulary consumed by the pipeline hook seam and 's
// import→compile path.  NO concrete IR dependency; no parser or serializer here.
//
// target_address_width == 0 is invalid; enforced by a validating constructor
// that rejects the descriptor before any byte is read.
//
// binary_native encoding is in-process only; format_descriptor::make() rejects it
// for persistent artifacts (G6).
//
// wire_endian lives here (authoritative); security_envelope.hpp imports it (G3).
//
// stable_ir_id is a fixed-size trivially-copyable dialect identifier that survives
// serialization and process boundaries.  format_descriptor carries a dialect field
// alongside ir_kind_tag (process-local hint) (G2).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../lithe_execution/foundation.hpp"   // ir_kind, ir_error

namespace lithe::ir {
    // =========================================================================
    //a.1 encoding — wire encoding for IR bytes
    // =========================================================================

    enum class encoding : std::uint8_t {
        text_utf8 = 0, // human-readable UTF-8 text
        binary_le = 1, // binary, little-endian (canonical for persistence)
        binary_be = 2, // binary, big-endian
        binary_native = 3, // in-process only — NEVER persist to disk or network
    };

    // Predicate: true iff the encoding is safe to persist (not host-endian-dependent).
    [[nodiscard]] constexpr bool is_persistent_safe(const encoding enc) noexcept {
        return enc != encoding::binary_native;
    }

    // =========================================================================
    //a.2 wire_endian — canonical wire byte order (authoritative definition)
    //
    // security_envelope.hpp imports this type rather than re-defining it.
    // =========================================================================

    enum class wire_endian : std::uint8_t {
        little = 0, // canonical for persisted artifacts
        big = 1,
    };

    // =========================================================================
    //a.1 stage — pipeline stage the IR represents
    // =========================================================================

    enum class stage : std::uint8_t {
        surface = 0, // un-optimised surface AST
        canonical = 1, // canonicalized
        optimized = 2, // after optimization passes
        lowered = 3, // after lowering (HL MIR)
        physical = 4, // physical register MIR
        managed = 5, // managed-annotated MIR (lithe_rt)
    };

    // =========================================================================
    //a.1 schema_version — semantic version for the IR schema
    // =========================================================================

    struct schema_version {
        std::uint16_t major = 0;
        std::uint16_t minor = 0;
        std::uint16_t patch = 0;

        [[nodiscard]] constexpr bool operator==(const schema_version&) const noexcept = default;

        [[nodiscard]] constexpr bool operator<(const schema_version& o) const noexcept {
            if (major != o.major) return major < o.major;
            if (minor != o.minor) return minor < o.minor;
            return patch < o.patch;
        }
    };

    static_assert(std::is_trivially_copyable_v<schema_version>);

    // =========================================================================
    //  /a.2 stable_ir_id — deterministic dialect identifier
    //
    // Survives serialization and process boundaries.  Fixed-size so the struct
    // remains trivially copyable.  Max 63 printable ASCII chars + NUL.
    // Use the k_dialect_* constants for well-known Lithe stages.
    // =========================================================================

    struct stable_ir_id {
        static constexpr std::size_t max_len = 63;
        std::array<char, max_len + 1> data = {}; // null-terminated, fixed-size
        std::uint8_t len = 0;

        constexpr stable_ir_id() noexcept = default;

        explicit constexpr stable_ir_id(const std::string_view s) noexcept {
            len = static_cast<std::uint8_t>(std::min(s.size(), max_len));
            for (std::uint8_t i = 0; i < len; ++i) data[i] = s[i];
        }

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return {data.data(), len};
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return len == 0; }
        [[nodiscard]] constexpr bool operator==(const stable_ir_id&) const noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<stable_ir_id>);

    // Well-known Lithe dialect constants — match the stage enum values.
    inline constexpr stable_ir_id k_dialect_graph{"lithe.graph"};
    inline constexpr stable_ir_id k_dialect_hl_mir{"lithe.hl_mir"};
    inline constexpr stable_ir_id k_dialect_physical_mir{"lithe.physical_mir"};
    inline constexpr stable_ir_id k_dialect_managed_mir{"lithe.managed_mir"};

    // Derive a canonical stable_ir_id from an ir_kind enum (for built-in kinds).
    [[nodiscard]] constexpr stable_ir_id dialect_from_kind(
        const ::lithe::execution::ir_kind k) noexcept {
        switch (k) {
        case ::lithe::execution::ir_kind::surface_ast: return k_dialect_graph;
        case ::lithe::execution::ir_kind::canonical_ast: return k_dialect_graph;
        case ::lithe::execution::ir_kind::optimized_ast: return k_dialect_graph;
        case ::lithe::execution::ir_kind::hl_mir: return k_dialect_hl_mir;
        case ::lithe::execution::ir_kind::physical_mir: return k_dialect_physical_mir;
        case ::lithe::execution::ir_kind::managed_mir: return k_dialect_managed_mir;
        default: return {};
        }
    }

    // =========================================================================
    //a.2 format_descriptor — full wire format description
    //
    // target_address_width == 0 is invalid.  binary_native is rejected for
    // persistence.  Use the make() factory; direct construction is permitted for
    // in-process use where all invariants are manually satisfied.
    //
    // Fields:
    //   wire_encoding        — encoding enum (text or binary variant)
    //   ir_stage             — pipeline stage
    //   version              — schema semver
    //   target_address_width — target ISA address width in bits (0 = invalid)
    //   ir_kind_tag          — in-process enum hint (fast path, not wire-stable)
    //   dialect              — stable_ir_id (wire-stable, survives serialization)
    //   endian               — wire byte order (binary encodings only; text ignores)
    // =========================================================================

    struct format_descriptor {
        encoding wire_encoding = encoding::binary_le;
        stage ir_stage = stage::physical;
        schema_version version = {1, 0, 0};
        std::uint8_t target_address_width = 0; // 0 = invalid
        ::lithe::execution::ir_kind ir_kind_tag = ::lithe::execution::ir_kind::physical_mir;
        stable_ir_id dialect; // stable cross-process id
        wire_endian endian = wire_endian::little; // binary only

        // Validating factory — rejects invalid width and non-persistent encodings.
        // For binary_native use the private constructor directly (in-process only).
        [[nodiscard]] static std::expected<format_descriptor, ::lithe::execution::ir_error>
        make(encoding enc, stage s, schema_version ver,
             std::uint8_t addr_width,
             ::lithe::execution::ir_kind kind,
             stable_ir_id dial = {},
             wire_endian endian_ = wire_endian::little) noexcept {
            if (addr_width == 0)
                return std::unexpected(
                    ::lithe::execution::ir_error{
                        "format_descriptor: target_address_width must not be 0"
                    });
            if (!is_persistent_safe(enc))
                return std::unexpected(
                    ::lithe::execution::ir_error{
                        "format_descriptor: binary_native must not be used for persistent artifacts"
                    });
            const stable_ir_id resolved_dial = dial.empty() ? dialect_from_kind(kind) : dial;
            return format_descriptor{enc, s, ver, addr_width, kind, resolved_dial, endian_};
        }

        // In-process factory — accepts binary_native; does NOT write to disk/network.
        [[nodiscard]] static constexpr format_descriptor make_in_process(
            encoding enc, stage s, schema_version ver,
            std::uint8_t addr_width,
            ::lithe::execution::ir_kind kind,
            stable_ir_id dial = {},
            wire_endian endian_ = wire_endian::little) noexcept {
            const stable_ir_id resolved_dial = dial.empty() ? dialect_from_kind(kind) : dial;
            return format_descriptor{enc, s, ver, addr_width, kind, resolved_dial, endian_};
        }

        [[nodiscard]] constexpr bool valid() const noexcept {
            return target_address_width != 0;
        }

        [[nodiscard]] constexpr bool operator==(const format_descriptor&) const noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<format_descriptor>);

    // =========================================================================
    //a.2 IR views — non-owning spans over bytes or text
    // =========================================================================

    // text_ir_view — non-owning view of text IR bytes (UTF-8).
    struct text_ir_view {
        std::span<const char> data;
        format_descriptor format;

        [[nodiscard]] constexpr bool valid() const noexcept {
            return !data.empty() && format.valid();
        }

        [[nodiscard]] std::string_view as_string_view() const noexcept {
            return {data.data(), data.size()};
        }
    };

    // Canonical design alias.
    using text_view = text_ir_view;

    // binary_ir_view — non-owning view of binary IR bytes.
    struct binary_ir_view {
        std::span<const std::uint8_t> data;
        format_descriptor format;

        [[nodiscard]] constexpr bool valid() const noexcept {
            return !data.empty() && format.valid();
        }
    };

    // Canonical design alias.
    using binary_view = binary_ir_view;

    // =========================================================================
    //a.2 Owned IR documents — owning variants for serialised artifacts
    // =========================================================================

    struct owned_text_ir {
        std::vector<char> data;
        format_descriptor format;

        [[nodiscard]] bool valid() const noexcept {
            return !data.empty() && format.valid();
        }

        [[nodiscard]] text_ir_view view() const noexcept {
            return {std::span<const char>{data}, format};
        }
    };

    struct owned_binary_ir {
        std::vector<std::uint8_t> data;
        format_descriptor format;

        [[nodiscard]] bool valid() const noexcept {
            return !data.empty() && format.valid();
        }

        [[nodiscard]] binary_ir_view view() const noexcept {
            return {std::span<const std::uint8_t>{data}, format};
        }
    };
} // namespace lithe::ir
