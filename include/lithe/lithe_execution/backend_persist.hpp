#pragma once

// =============================================================================
// lithe_execution/backend_persist.hpp — per-backend safe persistence codecs (impl-4)
//
// Arch §11.M4.3 / §8: persist only artifact forms that can safely be reused.
// Raw JIT handles and debug-text listings are NEVER persisted.
//
// Safe-to-persist matrix:
//   interpreter    — the portable_module itself is the artifact; no separate codec.
//                    Re-thaw on load. Codec: none (re-verify + thaw at load time).
//   asmjit/native  — relocatable object bytes + relocation records.
//                    NOT the live jit_compiled_payload (process-local pointer).
//   simd           — same as native (object_bytes + reloc).
//   vulkan         — SPIR-V module bytes + pipeline metadata.
//                    Rebuild vulkan_resource on load from SPIR-V.
//   debug-text     — NOT persisted (tooling only; no reuse value).
//   assembler      — NOT persisted (tooling only).
//
// All persisted payloads flow through the impl-3 artifact store:
//   encode_artifact(artifact) → std::vector<std::uint8_t>  (blob body)
//   decode_artifact(bytes)    → expected<artifact, decode_error>
//
// Codec types (all stateless, no virtual, no allocation on hot path):
//   object_persist_codec   — encode/decode object_bytes + reloc_records
//   spirv_persist_codec    — encode/decode SPIR-V + pipeline metadata
//
// Guard: these codecs only instantiate when the corresponding backend artifact
// types are available. Their encode/decode are purely byte-level; no backend
// runtime dependency.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "foundation.hpp"    // ir_kind, artifact_class
#include "artifact.hpp"      // artifact_manifest

namespace lithe::execution {
    // =============================================================================
    // persist_error — decode/encode failure
    // =============================================================================

    struct persist_error {
        std::string_view detail;
        constexpr explicit persist_error(std::string_view d = {}) noexcept : detail(d) {}
    };

    // =============================================================================
    // reloc_record — one relocation entry in a native object artifact
    //
    // The existing object_bytes (artifact.hpp) carries raw code bytes only.
    // reloc_records augments it for the persist path.
    // =============================================================================

    struct reloc_record {
        std::uint64_t offset = 0; // byte offset in code section
        std::uint32_t type = 0; // platform reloc type (e.g. R_X86_64_PC32)
        std::int64_t addend = 0;

        [[nodiscard]] bool operator==(const reloc_record&) const noexcept = default;
    };

    // =============================================================================
    // native_persist_artifact — the persist form of a native/SIMD artifact
    //
    // Wraps object_bytes.data (code) + reloc_records.
    // The live jit_compiled_payload is NEVER persisted.
    // =============================================================================

    struct native_persist_artifact {
        std::vector<std::uint8_t> code; // from object_bytes.data
        std::vector<reloc_record> relocs;

        [[nodiscard]] bool empty() const noexcept { return code.empty(); }
    };

    // =============================================================================
    // spirv_metadata — pipeline-level metadata stored alongside SPIR-V bytes
    //
    // Used by the Vulkan backend to rebuild a VkPipeline without re-compiling
    // from the Lithe IR. Kept deliberately small and platform-neutral.
    // =============================================================================

    struct spirv_metadata {
        std::uint32_t spec_version = 0x00010600; // SPIR-V 1.6 default
        std::string_view entry_point = "main";
        std::uint32_t local_size_x = 1;
        std::uint32_t local_size_y = 1;
        std::uint32_t local_size_z = 1;

        [[nodiscard]] bool operator==(const spirv_metadata&) const noexcept = default;
    };

    // =============================================================================
    // object_persist_codec — native/SIMD backend codec
    //
    // Wire format (little-endian):
    //   [4] magic = 0x4C4F424A ('LOBJ')
    //   [4] version = 1
    //   [8] code_size
    //   [code_size] code bytes
    //   [4] reloc_count
    //   per reloc: [8] offset + [4] type + [8] addend
    // =============================================================================

    struct object_persist_codec {
        static constexpr std::uint32_t magic = 0x4C4F424A;
        static constexpr std::uint32_t version = 1;

        [[nodiscard]] static std::vector<std::uint8_t>
        encode(const native_persist_artifact& obj) {
            std::vector<std::uint8_t> out;
            out.reserve(4 + 4 + 8 + obj.code.size() + 4 + obj.relocs.size() * 20);

            auto write_u32 = [&](std::uint32_t v) {
                out.push_back(static_cast<std::uint8_t>(v));
                out.push_back(static_cast<std::uint8_t>(v >> 8));
                out.push_back(static_cast<std::uint8_t>(v >> 16));
                out.push_back(static_cast<std::uint8_t>(v >> 24));
            };
            auto write_u64 = [&](std::uint64_t v) {
                for (int i = 0; i < 8; ++i)
                    out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
            };
            auto write_i64 = [&](std::int64_t v) {
                write_u64(static_cast<std::uint64_t>(v));
            };

            write_u32(magic);
            write_u32(version);
            write_u64(obj.code.size());
            out.insert(out.end(), obj.code.begin(), obj.code.end());
            write_u32(static_cast<std::uint32_t>(obj.relocs.size()));
            for (const auto& r : obj.relocs) {
                write_u64(r.offset);
                write_u32(r.type);
                write_i64(r.addend);
            }
            return out;
        }

        [[nodiscard]] static std::expected<native_persist_artifact, persist_error>
        decode(std::span<const std::uint8_t> bytes) {
            auto read_u32 = [&](std::size_t off) -> std::uint32_t {
                if (off + 4 > bytes.size()) return 0;
                return static_cast<std::uint32_t>(bytes[off])
                    | (static_cast<std::uint32_t>(bytes[off + 1]) << 8)
                    | (static_cast<std::uint32_t>(bytes[off + 2]) << 16)
                    | (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
            };
            auto read_u64 = [&](std::size_t off) -> std::uint64_t {
                if (off + 8 > bytes.size()) return 0;
                std::uint64_t v = 0;
                for (int i = 0; i < 8; ++i)
                    v |= (static_cast<std::uint64_t>(bytes[off + i]) << (i * 8));
                return v;
            };

            std::size_t pos = 0;
            if (read_u32(0) != magic) return std::unexpected(persist_error{"bad magic"});
            if (read_u32(4) != version) return std::unexpected(persist_error{"unsupported version"});
            pos = 8;

            const std::uint64_t code_size = read_u64(pos);
            pos += 8;
            if (pos + code_size > bytes.size())
                return std::unexpected(persist_error{"truncated code"});
            native_persist_artifact obj;
            obj.code.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                            bytes.begin() + static_cast<std::ptrdiff_t>(pos + code_size));
            pos += code_size;

            const std::uint32_t reloc_count = read_u32(pos);
            pos += 4;
            obj.relocs.reserve(reloc_count);
            for (std::uint32_t i = 0; i < reloc_count; ++i) {
                if (pos + 20 > bytes.size())
                    return std::unexpected(persist_error{"truncated relocs"});
                reloc_record r;
                r.offset = read_u64(pos);
                pos += 8;
                r.type = read_u32(pos);
                pos += 4;
                r.addend = static_cast<std::int64_t>(read_u64(pos));
                pos += 8;
                obj.relocs.push_back(r);
            }
            return obj;
        }
    };

    // =============================================================================
    // spirv_persist_codec — Vulkan/SPIR-V backend codec
    //
    // Wire format:
    //   [4] magic = 0x4C535056 ('LSPV')
    //   [4] version = 1
    //   [4] spec_version
    //   [4] local_size_x
    //   [4] local_size_y
    //   [4] local_size_z
    //   [8] spirv_size (bytes; must be multiple of 4)
    //   [spirv_size] SPIR-V bytes
    // entry_point is always "main" in this version.
    // =============================================================================

    struct spirv_persist_codec {
        static constexpr std::uint32_t magic = 0x4C535056;
        static constexpr std::uint32_t version = 1;

        struct spirv_artifact {
            spirv_metadata meta;
            std::vector<std::uint8_t> spirv; // raw SPIR-V bytes

            [[nodiscard]] bool valid() const noexcept {
                return !spirv.empty() && (spirv.size() % 4 == 0);
            }
        };

        [[nodiscard]] static std::vector<std::uint8_t>
        encode(const spirv_artifact& art) {
            std::vector<std::uint8_t> out;
            out.reserve(4 + 4 + 4 + 4 + 4 + 4 + 8 + art.spirv.size());

            auto write_u32 = [&](std::uint32_t v) {
                out.push_back(static_cast<std::uint8_t>(v));
                out.push_back(static_cast<std::uint8_t>(v >> 8));
                out.push_back(static_cast<std::uint8_t>(v >> 16));
                out.push_back(static_cast<std::uint8_t>(v >> 24));
            };
            auto write_u64 = [&](std::uint64_t v) {
                for (int i = 0; i < 8; ++i)
                    out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
            };

            write_u32(magic);
            write_u32(version);
            write_u32(art.meta.spec_version);
            write_u32(art.meta.local_size_x);
            write_u32(art.meta.local_size_y);
            write_u32(art.meta.local_size_z);
            write_u64(art.spirv.size());
            out.insert(out.end(), art.spirv.begin(), art.spirv.end());
            return out;
        }

        [[nodiscard]] static std::expected<spirv_artifact, persist_error>
        decode(std::span<const std::uint8_t> bytes) {
            auto read_u32 = [&](std::size_t off) -> std::uint32_t {
                if (off + 4 > bytes.size()) return 0;
                return static_cast<std::uint32_t>(bytes[off])
                    | (static_cast<std::uint32_t>(bytes[off + 1]) << 8)
                    | (static_cast<std::uint32_t>(bytes[off + 2]) << 16)
                    | (static_cast<std::uint32_t>(bytes[off + 3]) << 24);
            };
            auto read_u64 = [&](std::size_t off) -> std::uint64_t {
                if (off + 8 > bytes.size()) return 0;
                std::uint64_t v = 0;
                for (int i = 0; i < 8; ++i)
                    v |= (static_cast<std::uint64_t>(bytes[off + i]) << (i * 8));
                return v;
            };

            if (read_u32(0) != magic) return std::unexpected(persist_error{"bad magic"});
            if (read_u32(4) != version) return std::unexpected(persist_error{"unsupported version"});

            spirv_artifact art;
            art.meta.spec_version = read_u32(8);
            art.meta.local_size_x = read_u32(12);
            art.meta.local_size_y = read_u32(16);
            art.meta.local_size_z = read_u32(20);
            art.meta.entry_point = "main";

            const std::uint64_t sz = read_u64(24);
            if (32 + sz > bytes.size()) return std::unexpected(persist_error{"truncated spirv"});
            if (sz % 4 != 0) return std::unexpected(persist_error{"spirv size not word-aligned"});

            art.spirv.assign(bytes.begin() + 32,
                             bytes.begin() + 32 + static_cast<std::ptrdiff_t>(sz));
            return art;
        }
    };

    // MSL source is portable across processes for a compatible Metal toolchain;
    // a compiled MTLComputePipelineState is not.  Recompile the source on load.
    struct msl_persist_artifact {
        std::string entry_point = "lithe_metal_hl";
        std::uint32_t language_version = 0;
        std::uint32_t binding_count = 0;
        std::uint64_t device_plan_identity = 0;
        std::string element_type = "f32";
        std::string source;

        [[nodiscard]] bool valid() const noexcept {
            return !entry_point.empty() && !element_type.empty() && !source.empty();
        }
    };

    struct msl_persist_codec {
        static constexpr std::uint32_t magic = 0x4C4D534C; // 'LMSL'
        static constexpr std::uint32_t version = 1;

        [[nodiscard]] static std::vector<std::uint8_t>
        encode(const msl_persist_artifact& artifact) {
            std::vector<std::uint8_t> out;
            out.reserve(32 + artifact.entry_point.size() + artifact.element_type.size()
                + artifact.source.size());
            const auto write_u32 = [&](const std::uint32_t value) {
                for (int i = 0; i != 4; ++i)
                    out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
            };
            const auto write_u64 = [&](const std::uint64_t value) {
                for (int i = 0; i != 8; ++i)
                    out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
            };
            const auto write_string = [&](const std::string_view value) {
                write_u32(static_cast<std::uint32_t>(value.size()));
                out.insert(out.end(), value.begin(), value.end());
            };
            write_u32(magic);
            write_u32(version);
            write_u32(artifact.language_version);
            write_u32(artifact.binding_count);
            write_u64(artifact.device_plan_identity);
            write_string(artifact.entry_point);
            write_string(artifact.element_type);
            write_string(artifact.source);
            return out;
        }

        [[nodiscard]] static std::expected<msl_persist_artifact, persist_error>
        decode(const std::span<const std::uint8_t> bytes) {
            const auto read_u32 = [&](const std::size_t offset) -> std::optional<std::uint32_t> {
                if (offset > bytes.size() || bytes.size() - offset < 4) return std::nullopt;
                std::uint32_t value = 0;
                for (int i = 0; i != 4; ++i)
                    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8);
                return value;
            };
            const auto read_u64 = [&](const std::size_t offset) -> std::optional<std::uint64_t> {
                if (offset > bytes.size() || bytes.size() - offset < 8) return std::nullopt;
                std::uint64_t value = 0;
                for (int i = 0; i != 8; ++i)
                    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
                return value;
            };
            std::size_t pos = 0;
            const auto consume_u32 = [&]() -> std::optional<std::uint32_t> {
                const auto value = read_u32(pos);
                if (value) pos += 4;
                return value;
            };
            const auto consume_string = [&]() -> std::optional<std::string> {
                const auto size = consume_u32();
                if (!size || *size > bytes.size() - pos) return std::nullopt;
                std::string value;
                value.reserve(*size);
                for (std::uint32_t i = 0; i != *size; ++i)
                    value.push_back(static_cast<char>(bytes[pos + i]));
                pos += *size;
                return value;
            };
            const auto file_magic = consume_u32();
            const auto file_version = consume_u32();
            if (!file_magic || *file_magic != magic) return std::unexpected(persist_error{"bad MSL magic"});
            if (!file_version || *file_version != version) return std::unexpected(persist_error{"unsupported MSL version"});
            const auto language_version = consume_u32();
            const auto binding_count = consume_u32();
            const auto identity = read_u64(pos);
            if (!language_version || !binding_count || !identity)
                return std::unexpected(persist_error{"truncated MSL metadata"});
            pos += 8;
            auto entry_point = consume_string();
            auto element_type = consume_string();
            auto source = consume_string();
            if (!entry_point || !element_type || !source || pos != bytes.size())
                return std::unexpected(persist_error{"truncated MSL artifact"});
            msl_persist_artifact artifact{
                .entry_point = std::move(*entry_point),
                .language_version = *language_version,
                .binding_count = *binding_count,
                .device_plan_identity = *identity,
                .element_type = std::move(*element_type),
                .source = std::move(*source)};
            if (!artifact.valid()) return std::unexpected(persist_error{"invalid MSL artifact"});
            return artifact;
        }
    };

    // =============================================================================
    // backend_persist_tag<B> — opt-in per-backend marker
    //
    // Specialize this to declare that backend B has a persist codec.
    // Default: no codec (not persisted). The tag carries the codec type.
    //
    // Example specialization:
    //   template<> struct backend_persist_tag<MyNativeBackend> {
    //       using codec = object_persist_codec;
    //   };
    // =============================================================================

    template <class Backend>
    struct backend_persist_tag {
        using codec = void; // void = not persisted
    };

    // Concept: backend B has a safe persist codec.
    template <class B>
    concept persistable_backend =
        !std::is_same_v < typename backend_persist_tag<B>::codec
    ,
    void
    >;
} // namespace lithe::execution
