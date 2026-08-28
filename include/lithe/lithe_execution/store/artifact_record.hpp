#pragma once

// =============================================================================
// lithe_execution/store/artifact_record.hpp — durable artifact records (impl-3)
//
// Separates persistable data from in-memory live handles (arch §11.M3.1).
//
// Provides:
//   artifact_kind             — optimized_portable | executable
//   abi_fingerprint           — ABI string identity
//   policy_fingerprint        — safety/FP/determinism/security policy hash
//   capability_fingerprint    — target capability bitfield hash
//   specialization_fingerprint — compile-time specialization params hash
//   symbol_resolution_fingerprint — external symbol binding hash
//   backend_id / backend_version  — backend identity (persistable)
//   backend_pipeline_version      — backend pipeline version
//   target_restrictions       — platform/OS/ABI constraints
//   resource_limits           — max memory, time, etc.
//   payload_ref               — inline small blob OR content-address pointer
//   signature_info            — algorithm + key id + signature bytes
//   upgrade_step              — one step in a provenance upgrade chain
//   provenance                — pipeline/backend versions + upgrade chain
//   compatibility_manifest    — input to the compatibility predicate (arch §9)
//   optimized_key             — tier-1 artifact key (portable, no backend)
//   executable_key            — tier-2 artifact key (+ backend + target facts)
//   artifact_key              — variant<optimized_key, executable_key>
//   compute_key_digest()      — stable 32-byte digest of an artifact_key
//   artifact_record           — durable persistable record (no pointers)
//
// Invariants:
//   • artifact_record contains no raw pointers, no live handles.
//   • Live handles remain in any_compiled_artifact (artifact.hpp).
//   • to_record() / from_record() bridge publish/load paths.
//   • "Normal runtime values are not key material; compile-time specialization
//     values are" (arch §7).
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "../artifact.hpp"                         // artifact_manifest, artifact_class, ir_kind
#include "../../lithe_ir/format.hpp"               // schema_version
#include "../../lithe_ir/security_envelope.hpp"    // digest_algorithm, signature_algorithm
#include "containers/canonical_codec.hpp" // canonical_writer, content_digest

namespace lithe::execution::store {
    // =============================================================================
    // artifact_kind — whether the artifact is a portable IR or backend-specific
    // =============================================================================

    enum class artifact_kind : std::uint8_t {
        optimized_portable = 0, // portable IR + applied passes; no backend binding
        executable = 1, // backend-compiled; target-specific
    };

    // =============================================================================
    // Fingerprint types — stable hashes over specific dimensions of key material
    // =============================================================================

    struct abi_fingerprint {
        std::array<std::uint8_t, 32> digest{};
        [[nodiscard]] bool operator==(const abi_fingerprint&) const noexcept = default;
    };

    struct policy_fingerprint {
        std::array<std::uint8_t, 32> digest{};
        [[nodiscard]] bool operator==(const policy_fingerprint&) const noexcept = default;
    };

    struct capability_fingerprint {
        std::array<std::uint8_t, 32> digest{};
        [[nodiscard]] bool operator==(const capability_fingerprint&) const noexcept = default;
    };

    struct specialization_fingerprint {
        std::array<std::uint8_t, 32> digest{};
        [[nodiscard]] bool operator==(const specialization_fingerprint&) const noexcept = default;
    };

    struct symbol_resolution_fingerprint {
        std::array<std::uint8_t, 32> digest{};
        [[nodiscard]] bool operator==(const symbol_resolution_fingerprint&) const noexcept = default;
    };

    // =============================================================================
    // Backend identity (persistable — not process-local type tokens)
    // =============================================================================

    struct backend_id {
        std::string name; // stable, non-empty
        [[nodiscard]] bool operator==(const backend_id&) const noexcept = default;
        [[nodiscard]] bool empty() const noexcept { return name.empty(); }
    };

    struct backend_version {
        std::uint16_t major = 1;
        std::uint16_t minor = 0;
        [[nodiscard]] bool operator==(const backend_version&) const noexcept = default;
    };

    struct backend_pipeline_version {
        std::uint16_t major = 1;
        std::uint16_t minor = 0;
        [[nodiscard]] bool operator==(const backend_pipeline_version&) const noexcept = default;
    };

    // =============================================================================
    // target_restrictions — platform/ABI/OS constraints bound to an executable key
    // =============================================================================

    struct target_restrictions {
        std::string os; // e.g. "macos", "linux"
        std::string arch; // e.g. "arm64", "x86_64"
        std::uint32_t min_os_version = 0;
        [[nodiscard]] bool operator==(const target_restrictions&) const noexcept = default;
    };

    // =============================================================================
    // resource_limits — execution resource constraints stored in the artifact
    // =============================================================================

    struct resource_limits {
        std::uint64_t max_stack_bytes = 0; // 0 = unlimited
        std::uint64_t max_heap_bytes = 0;
        std::uint64_t max_time_ns = 0;
        [[nodiscard]] bool operator==(const resource_limits&) const noexcept = default;
    };

    // =============================================================================
    // payload_ref — inline (small blobs) or content-addressed reference (large)
    //
    // Inline threshold: 256 bytes. Beyond that use the content-addressed store.
    // =============================================================================

    static constexpr std::size_t k_inline_payload_threshold = 256;

    struct inline_payload {
        std::vector<std::uint8_t> bytes;
        [[nodiscard]] bool operator==(const inline_payload&) const noexcept = default;
    };

    struct content_address_ref {
        std::array<std::uint8_t, 32> digest{}; // SHA-256 content digest
        std::uint64_t size = 0; // byte count
        [[nodiscard]] bool operator==(const content_address_ref&) const noexcept = default;
    };

    using payload_ref = std::variant<inline_payload, content_address_ref>;

    // =============================================================================
    // signature_info — stored alongside a record for authenticity
    // =============================================================================

    struct signature_info {
        lithe::ir::signature_algorithm alg = lithe::ir::signature_algorithm::none;
        std::array<std::uint8_t, 32> key_id{};
        std::vector<std::uint8_t> sig_bytes;
        [[nodiscard]] bool operator==(const signature_info&) const noexcept = default;
    };

    // =============================================================================
    // upgrade_step — one versioned upgrader step in the provenance chain (arch §9)
    // =============================================================================

    struct upgrade_step {
        std::string upgrader_id;
        std::uint16_t from_major = 0;
        std::uint16_t from_minor = 0;
        std::uint16_t to_major = 0;
        std::uint16_t to_minor = 0;
        [[nodiscard]] bool operator==(const upgrade_step&) const noexcept = default;
    };

    // =============================================================================
    // provenance (arch §9) — records the pipeline, backend, and upgrade chain
    // =============================================================================

    struct pipeline_id_record {
        std::string name;
        [[nodiscard]] bool operator==(const pipeline_id_record&) const noexcept = default;
    };

    struct pipeline_version_record {
        std::uint16_t major = 1;
        std::uint16_t minor = 0;
        [[nodiscard]] bool operator==(const pipeline_version_record&) const noexcept = default;
    };

    struct provenance {
        pipeline_id_record pipe;
        pipeline_version_record pipe_ver;
        std::optional<backend_id> backend;
        std::optional<backend_version> backend_ver;
        std::vector<upgrade_step> upgrades; // ordered: oldest-to-newest
        std::string producer; // tool/version string
        [[nodiscard]] bool operator==(const provenance&) const noexcept = default;
    };

    // =============================================================================
    // compatibility_manifest (arch §9) — input to check_compatible predicate
    // =============================================================================

    struct external_symbol_req {
        std::string name;
        std::string abi_tag;
        [[nodiscard]] bool operator==(const external_symbol_req&) const noexcept = default;
    };

    struct security_policy_id {
        std::uint64_t id = 0;
        std::uint64_t version = 0;
        [[nodiscard]] bool operator==(const security_policy_id&) const noexcept = default;
    };

    struct capability_set {
        std::uint64_t bits = 0; // bitfield; bit layout matches backend_feature enum
        [[nodiscard]] bool operator==(const capability_set&) const noexcept = default;
    };

    struct compatibility_manifest {
        lithe::ir::schema_version ir_schema{1, 0, 0};
        abi_fingerprint abi{};
        capability_set required_caps{};
        target_restrictions target{};
        std::vector<external_symbol_req> ext_syms;
        security_policy_id security{};
        [[nodiscard]] bool operator==(const compatibility_manifest&) const noexcept = default;
    };

    // =============================================================================
    // artifact_key — two-tier key derivation (arch §7)
    //
    // optimized_key: portable IR identity — semantic_digest + IR schema + ABI +
    //   pipeline id/version + policy fingerprint.
    //
    // executable_key: optimized_key + backend id/version + target capability
    //   fingerprint + backend pipeline version + specialization + ext-symbol fingerprint.
    //
    // "Normal runtime values are not key material."
    // =============================================================================

    struct optimized_key {
        std::array<std::uint8_t, 64> semantic_digest{}; // from impl-1 portable::semantic_digest
        std::uint8_t semantic_digest_len = 32; // active bytes
        lithe::ir::schema_version ir_schema{1, 0, 0};
        abi_fingerprint abi{};
        pipeline_id_record pipe_id{};
        pipeline_version_record pipe_ver{};
        policy_fingerprint policy{};
        [[nodiscard]] bool operator==(const optimized_key&) const noexcept = default;
    };

    struct executable_key {
        optimized_key base{};
        backend_id backend{};
        backend_version backend_ver{};
        capability_fingerprint target_caps{};
        backend_pipeline_version backend_pipe{};
        specialization_fingerprint spec{};
        symbol_resolution_fingerprint ext_syms{};
        [[nodiscard]] bool operator==(const executable_key&) const noexcept = default;
    };

    using artifact_key = std::variant<optimized_key, executable_key>;

    // =============================================================================
    // compute_key_digest — stable 32-byte hash of an artifact_key (primary catalog id)
    //
    // Uses canonical_writer for deterministic byte layout, then SHA-256.
    // =============================================================================

    [[nodiscard]] inline std::array<std::uint8_t, 32>
    compute_key_digest(const artifact_key& key) noexcept {
        containers::canonical_writer w;

        auto write_bytes32 = [&](const std::array<std::uint8_t, 32>& a) {
            for (auto b : a) w.write_u8(b);
        };
        auto write_str = [&](const std::string& s) {
            w.write_u32(static_cast<std::uint32_t>(s.size()));
            for (unsigned char c : s) w.write_u8(c);
        };
        auto write_schema = [&](const lithe::ir::schema_version& sv) {
            w.write_u16(sv.major);
            w.write_u16(sv.minor);
            w.write_u16(sv.patch);
        };

        auto write_optimized = [&](const optimized_key& k) {
            // semantic_digest (first k.semantic_digest_len bytes are active)
            const auto active = static_cast<std::uint8_t>(
                std::min<std::size_t>(k.semantic_digest_len, 64));
            w.write_u8(active);
            for (std::uint8_t i = 0; i < active; ++i) w.write_u8(k.semantic_digest[i]);
            write_schema(k.ir_schema);
            write_bytes32(k.abi.digest);
            write_str(k.pipe_id.name);
            w.write_u16(k.pipe_ver.major);
            w.write_u16(k.pipe_ver.minor);
            write_bytes32(k.policy.digest);
        };

        std::visit([&](const auto& k) {
            using T = std::remove_cvref_t<decltype(k)>;
            if constexpr (std::is_same_v<T, optimized_key>) {
                w.write_u8(0); // kind discriminant
                write_optimized(k);
            }
            else {
                w.write_u8(1); // kind discriminant
                write_optimized(k.base);
                write_str(k.backend.name);
                w.write_u16(k.backend_ver.major);
                w.write_u16(k.backend_ver.minor);
                write_bytes32(k.target_caps.digest);
                w.write_u16(k.backend_pipe.major);
                w.write_u16(k.backend_pipe.minor);
                write_bytes32(k.spec.digest);
                write_bytes32(k.ext_syms.digest);
            }
        }, key);

        // finalize_string_table not needed (no intern_string calls above)
        const auto bytes = w.emit();
        const auto hash = containers::content_digest<containers::sha256_digest_policy>(
            std::span<const std::uint8_t>{bytes.data(), bytes.size()});
        return hash;
    }

    // =============================================================================
    // artifact_record — durable persistable record (no raw pointers, no live handles)
    //
    // The live compiled handle (any_compiled_artifact) is NOT here.
    // to_record() / from_record() bridge the publish/load boundary.
    // =============================================================================

    struct artifact_record {
        artifact_key key;
        artifact_kind kind = artifact_kind::optimized_portable;
        artifact_manifest manifest{};
        std::array<std::uint8_t, 64> semantic_digest{};
        std::uint8_t semantic_digest_len = 32;
        payload_ref payload{inline_payload{}};
        provenance prov{};
        compatibility_manifest compat{};
        resource_limits limits{};
        std::optional<signature_info> signature;

        [[nodiscard]] bool valid() const noexcept { return manifest.valid(); }
    };
} // namespace lithe::execution::store
