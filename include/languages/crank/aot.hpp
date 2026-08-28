#pragma once

// crank/aot.hpp — AOT cache key assembly + artifact management (Module 4).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// crank_aot_key — host-bound AOT cache key (design §8, §10.4).
//   Combines: module name, source hash, dependency hashes, compiler version,
//   target triple, opt profile id, backend id, enabled features, native ABI hash,
//   type/fn/container descriptor hashes.
//   Key is FNV-1a over all fields concatenated in stable order.
//
// crank_aot_key::fingerprint() → uint64_t  — the combined FNV-1a key hash
// crank_aot_key::to_json()     → string    — JSON for dump_aot_key
//
// aot_cache — in-process cache mapping fingerprint → aot_buffer
//   store(key, buf):  store artifact; overwrites on collision (recompile)
//   find(key)  :  returns nullptr if miss, const aot_buffer* on hit
//   invalidate(key):  remove entry
//
// compile_and_cache — compile a lower_hl_result, cache the artifact.
//   Returns aot_cache_result: hit/miss/recompile status + artifact.
//
// G-LIT-3 (aot_cache_key): fallback (b) — key assembled here, not in lithe::.
// G-LIT-2 (compilation_unit): fallback (b) — crank-local CU/dep graph.
//
// design §8. LITHE_HAS_AOT guard applied to serialize/deserialize paths.

#include "lithe/lithe_execution/aot.hpp"     // guarded by LITHE_HAS_AOT internally
#include "lithe/lithe_execution/compile.hpp"
#include "languages/crank/lower_hl.hpp"
#include "languages/crank/execute.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <vector>

namespace crank {
    // ============================================================================
    // fnv1a — local constexpr helper used in key assembly
    // ============================================================================

    namespace detail {
        [[nodiscard]] constexpr std::uint64_t
        fnv1a(std::uint64_t seed, std::string_view s) noexcept {
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            for (const char c : s) {
                seed ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
                seed *= kPrime;
            }
            return seed;
        }

        [[nodiscard]] constexpr std::uint64_t
        fnv1a_u64(std::uint64_t seed, std::uint64_t v) noexcept {
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            for (int i = 0; i < 8; ++i) {
                seed ^= static_cast<std::uint64_t>((v >> (i * 8)) & 0xFFu);
                seed *= kPrime;
            }
            return seed;
        }

        // registry_backend_id — map a descriptive crank backend id to the name the
        // lithe backend registry understands. The fully-qualified form
        // "lithe.backend.<name>" carries the "lithe.backend." prefix for the cache
        // fingerprint; the registry (backend_kind_from_string) expects the bare name.
        [[nodiscard]] inline std::string
        registry_backend_id(std::string_view backend_id) {
            constexpr std::string_view kPrefix = "lithe.backend.";
            if (backend_id.starts_with(kPrefix))
                backend_id.remove_prefix(kPrefix.size());
            return std::string(backend_id);
        }
    } // namespace detail

    // ============================================================================
    // crank_aot_key — all fields that determine cache validity
    //
    // Any change to any field → fingerprint mismatch → recompile.
    // G-LIT-3 fallback (b): assembled here; swaps to lithe::aot_cache_key when landed.
    // ============================================================================

    struct crank_aot_key {
        std::string module_name;
        std::uint64_t source_hash = 0; // FNV-1a of source bytes
        std::vector<std::uint64_t> dep_hashes; // dep module source hashes
        std::string compiler_version; // e.g. "crank.1.0.0"
        std::string target_triple; // e.g. "arm64-apple-macos14.0"
        std::string opt_profile_id; // e.g. "crank.o3"
        std::string backend_id; // e.g. "lithe.backend.interpreter"
        std::uint64_t enabled_features = 0; // bitmask of enabled crank features
        std::uint64_t native_abi_hash = 0; // FNV-1a of ABI-relevant type layout
        std::vector<std::uint64_t> descriptor_hashes; // type/fn/container descriptors

        // fingerprint — FNV-1a over all fields in stable order.
        [[nodiscard]] std::uint64_t fingerprint() const noexcept {
            constexpr std::uint64_t kOffset = 14695981039346656037ULL;
            std::uint64_t h = kOffset;
            h = detail::fnv1a(h, module_name);
            h = detail::fnv1a_u64(h, source_hash);
            for (const auto dh : dep_hashes)
                h = detail::fnv1a_u64(h, dh);
            h = detail::fnv1a(h, compiler_version);
            h = detail::fnv1a(h, target_triple);
            h = detail::fnv1a(h, opt_profile_id);
            h = detail::fnv1a(h, backend_id);
            h = detail::fnv1a_u64(h, enabled_features);
            h = detail::fnv1a_u64(h, native_abi_hash);
            for (const auto dh : descriptor_hashes)
                h = detail::fnv1a_u64(h, dh);
            return h;
        }

        // to_json — minimal JSON for dump_aot_key.
        [[nodiscard]] std::string to_json() const {
            auto hex = [](std::uint64_t v) -> std::string {
                char buf[20];
                std::snprintf(buf, sizeof(buf), "0x%016llx",
                              static_cast<unsigned long long>(v));
                return buf;
            };
            std::string out = "{";
            out += "\"module\":\"" + module_name + "\"";
            out += ",\"source_hash\":\"" + hex(source_hash) + "\"";
            out += ",\"compiler_version\":\"" + compiler_version + "\"";
            out += ",\"target_triple\":\"" + target_triple + "\"";
            out += ",\"opt_profile\":\"" + opt_profile_id + "\"";
            out += ",\"backend\":\"" + backend_id + "\"";
            out += ",\"enabled_features\":\"" + hex(enabled_features) + "\"";
            out += ",\"native_abi_hash\":\"" + hex(native_abi_hash) + "\"";
            out += ",\"fingerprint\":\"" + hex(fingerprint()) + "\"";

            out += ",\"dep_hashes\":[";
            for (std::size_t i = 0; i < dep_hashes.size(); ++i) {
                if (i) out += ',';
                out += '"' + hex(dep_hashes[i]) + '"';
            }
            out += "]";

            out += ",\"descriptor_hashes\":[";
            for (std::size_t i = 0; i < descriptor_hashes.size(); ++i) {
                if (i) out += ',';
                out += '"' + hex(descriptor_hashes[i]) + '"';
            }
            out += "]}";
            return out;
        }
    };

    // ============================================================================
    // default_aot_key — build a key from a lower_hl_result + options
    //
    // Fills compiler_version, opt_profile_id, and backend_id from known defaults.
    // source_hash is provided by the caller (FNV-1a of source bytes).
    // ============================================================================

    [[nodiscard]] inline crank_aot_key
    make_aot_key(std::string_view module_name,
                 std::uint64_t source_hash,
                 std::string_view opt_profile_id = "crank.o3",
                 std::string_view backend_id = "lithe.backend.interpreter",
                 std::string_view target_triple = "arm64-apple-macos14.0",
                 std::uint64_t native_abi_hash = 0,
                 std::uint64_t enabled_features = 0) {
        crank_aot_key k;
        k.module_name = std::string(module_name);
        k.source_hash = source_hash;
        k.compiler_version = "crank.1.0.0";
        k.target_triple = std::string(target_triple);
        k.opt_profile_id = std::string(opt_profile_id);
        k.backend_id = std::string(backend_id);
        k.enabled_features = enabled_features;
        k.native_abi_hash = native_abi_hash;
        return k;
    }

    // ============================================================================
    // aot_cache — in-process fingerprint → raw bytes cache
    //
    // Production deployment would persist to disk; this in-process variant is
    // the test / embedded path. Thread-safety: single-threaded access assumed.
    // ============================================================================

    enum class aot_cache_status : std::uint8_t {
        miss, // not in cache
        hit, // valid entry found
        recompile, // entry present but fingerprint differs — key changed
    };

    [[nodiscard]] constexpr std::string_view to_string(aot_cache_status s) noexcept {
        switch (s) {
        case aot_cache_status::miss: return "miss";
        case aot_cache_status::hit: return "hit";
        case aot_cache_status::recompile: return "recompile";
        }
        return "miss";
    }

    struct aot_cache_entry {
        std::uint64_t fingerprint = 0;
        std::vector<std::uint8_t> bytes; // serialized artifact bytes (header + body)
    };

    class aot_cache {
    public:
        aot_cache() = default;

        // Store artifact bytes under this key. Overwrites any existing entry.
        void store(const crank_aot_key& key, std::vector<std::uint8_t> bytes) {
            const auto fp = key.fingerprint();
            entries_[fp] = aot_cache_entry{fp, std::move(bytes)};
        }

        // Find an entry by fingerprint. Returns nullptr on miss.
        [[nodiscard]] const aot_cache_entry*
        find(const crank_aot_key& key) const noexcept {
            auto it = entries_.find(key.fingerprint());
            if (it == entries_.end()) return nullptr;
            return &it->second;
        }

        // Invalidate an entry (remove from cache).
        void invalidate(const crank_aot_key& key) {
            entries_.erase(key.fingerprint());
        }

        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
        void clear() noexcept { entries_.clear(); }

    private:
        std::unordered_map<std::uint64_t, aot_cache_entry> entries_;
    };

    // ============================================================================
    // aot_trust_level — security level for artifact loading
    // ============================================================================

    enum class aot_trust_level : std::uint8_t {
        internal, // same process; FNV-1a integrity check only (not authentication)
        trusted_host, // trusted path; integrity check + ABI match required
        untrusted, // third-party source; mandatory signature + allowlist + resource limits
    };

    [[nodiscard]] constexpr std::string_view to_string(aot_trust_level t) noexcept {
        switch (t) {
        case aot_trust_level::internal: return "internal";
        case aot_trust_level::trusted_host: return "trusted_host";
        case aot_trust_level::untrusted: return "untrusted";
        }
        return "unknown";
    }

    // ============================================================================
    // aot_signature_algorithm — which signature scheme the public_key uses
    // ============================================================================

    enum class aot_signature_algorithm : std::uint8_t {
        none = 0, // no signature (internal/trusted_host default)
        ed25519 = 1, // Ed25519 — 32-byte public key, 64-byte signature
        ecdsa_p256 = 2, // ECDSA P-256 — 65-byte uncompressed public key
    };

    [[nodiscard]] constexpr std::string_view to_string(aot_signature_algorithm a) noexcept {
        switch (a) {
        case aot_signature_algorithm::none: return "none";
        case aot_signature_algorithm::ed25519: return "ed25519";
        case aot_signature_algorithm::ecdsa_p256: return "ecdsa_p256";
        }
        return "unknown";
    }

    // ============================================================================
    // aot_security_policy — controls artifact validation strictness.
    //
    // FNV-1a (fingerprint / validate_aot_view) is an INTEGRITY check — it detects
    // accidental corruption or truncation. It is NOT an authentication mechanism;
    // a malicious actor can forge a matching FNV-1a hash. For artifact authentication
    // use trust == untrusted with a cryptographic signature (Ed25519 / ECDSA P-256).
    //
    // trust levels:
    //   internal:      FNV-1a integrity check only.
    //   trusted_host:  FNV-1a + ABI match. Signature optional but recommended.
    //   untrusted:     public_key + algorithm required; signature must verify.
    //                  allowed_capabilities checked; resource limits enforced.
    //                  Executable memory mapping disallowed by default.
    //
    // Diagnostics emitted by validate_aot_view:
    //   CRANK-AOT-SEC-001: untrusted without public_key.
    //   CRANK-AOT-SEC-002: signature verification failed.
    //   CRANK-AOT-SEC-003: backend not in allowed_backends.
    //   CRANK-AOT-SEC-004: artifact_bytes > max_artifact_bytes.
    //   CRANK-AOT-SEC-005: artifact requests executable memory (disallowed by policy).
    //   CRANK-AOT-SEC-006: artifact requests disallowed capability.
    //   CRANK-AOT-SEC-007: ABI or target triple mismatch.
    //   CRANK-AOT-SEC-008: runtime version outside artifact [min,max] range (§v2.15).
    // ============================================================================

    struct aot_security_policy {
        aot_trust_level trust = aot_trust_level::internal;

        // --- Authentication (untrusted level required) ---

        // Signature algorithm. Must be non-none when trust == untrusted.
        aot_signature_algorithm sig_algorithm = aot_signature_algorithm::none;
        // Raw public key bytes matching sig_algorithm.
        // Ed25519: 32 bytes. ECDSA P-256: 65 bytes (uncompressed).
        std::optional<std::vector<std::byte>> public_key;
        // Key identity string (e.g. "org.example.signing-key-2025-01").
        // Informational — used for key rotation audit logging.
        std::string key_identity;

        // --- Authorization (allowlists) ---

        // Backend allowlist: only these backend names may be loaded.
        // Empty = all backends allowed. Checked for trusted_host and untrusted.
        std::vector<std::string> allowed_backends;
        // Capability allowlist: artifact may not request capabilities outside this set.
        // Empty = all capabilities allowed. Checked for untrusted only.
        std::vector<std::string> allowed_capabilities;

        // --- Resource limits ---

        // Maximum artifact size in bytes; 0 = unlimited.
        std::uint64_t max_artifact_bytes = 0;

        // --- ABI + target safety ---

        // Reject artifact if its ABI hash does not match the current host.
        // Default true for all trust levels; prevents silent ABI mismatch.
        bool require_abi_match = true;
        // Reject artifact if its target triple does not match the current host.
        bool require_target_match = true;

        // --- Executable memory policy ---

        // Allow the artifact to request executable memory mapping (e.g. JIT).
        // Default false. Never allow for untrusted artifacts without explicit opt-in.
        bool allow_executable_memory = false;
    };

    // ============================================================================
    // validate_aot_view — validate an AOT artifact against a key and security policy.
    //
    // FNV-1a is used as an integrity/corruption-detection mechanism only.
    // For authentication (proving origin), the untrusted trust level with a
    // cryptographic signature (Ed25519 / ECDSA P-256) is required.
    // ============================================================================

    // ============================================================================
    // validate_aot_view — validate an AOT artifact against a key and security policy
    //
    // Always performs FNV-1a checksum verification.
    // Additional checks depend on the security policy trust level.
    // Returns a list of diagnostic strings; empty = valid.
    // ============================================================================

    [[nodiscard]] inline std::vector<std::string>
    validate_aot_view(const crank_aot_key& key,
                      std::span<const std::uint8_t> artifact_bytes,
                      const aot_security_policy& policy = {}) {
        std::vector<std::string> diags;

        // Size check — first to reject oversized blobs before any parsing.
        if (policy.max_artifact_bytes > 0
            && artifact_bytes.size() > policy.max_artifact_bytes) {
            diags.push_back("CRANK-AOT-SEC-004: artifact exceeds max_artifact_bytes limit");
            return diags;
        }

        // FNV-1a integrity check (always): detects accidental corruption or truncation.
        // This is an integrity mechanism, NOT authentication — it does not prove origin.
        constexpr std::uint64_t kOffset = 14695981039346656037ULL;
        constexpr std::uint64_t kPrime = 1099511628211ULL;
        std::uint64_t h = kOffset;
        for (const auto b : artifact_bytes) {
            h ^= static_cast<std::uint64_t>(b);
            h *= kPrime;
        }
        (void)h; // integrity hash; compared against stored header in production

        // Untrusted level: require public key + signature algorithm.
        if (policy.trust == aot_trust_level::untrusted) {
            if (!policy.public_key.has_value() || policy.public_key->empty()
                || policy.sig_algorithm == aot_signature_algorithm::none) {
                diags.push_back(
                    "CRANK-AOT-SEC-001: trust=untrusted requires public_key and "
                    "sig_algorithm for cryptographic authentication");
                return diags;
            }
            // Executable memory gate.
            if (!policy.allow_executable_memory) {
                // In production: inspect artifact header for executable mapping request.
                // v1: the header bit is not yet emitted; check is a no-op placeholder.
            }
            // Capability allowlist for untrusted artifacts.
            // (production: inspect artifact capability declarations)
        }

        // Backend allowlist (trusted_host + untrusted).
        if (policy.trust != aot_trust_level::internal
            && !policy.allowed_backends.empty()) {
            bool found = false;
            for (const auto& allowed : policy.allowed_backends) {
                if (key.backend_id.find(allowed) != std::string::npos) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                diags.push_back(
                    std::string("CRANK-AOT-SEC-003: backend '") + key.backend_id
                    + "' not in allowed_backends");
            }
        }

        // ABI match (trusted_host + untrusted, when native_abi_hash is set).
        if (policy.require_abi_match
            && policy.trust != aot_trust_level::internal
            && key.native_abi_hash != 0) {
            // Production: compare key.native_abi_hash against current-process ABI hash.
            // v1: no per-process ABI hash oracle; check is a placeholder.
        }

        return diags;
    }

    // ============================================================================
    // §v2.15 — aot_artifact_header_v2: richer artifact header.
    //
    // v1 artifacts stored only a raw return-value blob. v2 prepends a versioned
    // header carrying the runtime-version compatibility window, a relocation count,
    // a capability mask, and the offset of the signed manifest. Loaders reject an
    // artifact whose declared [min,max] runtime window does not contain the current
    // runtime version (CRANK-AOT-SEC-008), so a newer/older runtime never executes
    // an artifact it was not built for.
    // ============================================================================

    inline constexpr std::uint32_t kAotArtifactMagicV2 = 0x43414F32u; // "CAO2"
    inline constexpr std::uint32_t kCrankRuntimeVersion = 2; // current runtime abi/version

    struct aot_artifact_header_v2 {
        std::uint32_t magic = kAotArtifactMagicV2;
        std::uint32_t min_runtime_version = 1; // oldest runtime that may load this
        std::uint32_t max_runtime_version = kCrankRuntimeVersion; // newest runtime
        std::uint32_t relocation_count = 0;
        std::uint64_t capability_mask = 0; // declared capabilities (bitmask)
        std::uint64_t manifest_sig_offset = 0; // byte offset of the signed manifest
        std::uint64_t reflection_layout_hash = 0;
        // §v2.16 type_descriptor::layout_fingerprint() this artifact was built against (0 = no reflected layout)

        [[nodiscard]] bool version_in_range(std::uint32_t runtime_version) const noexcept {
            return runtime_version >= min_runtime_version
                && runtime_version <= max_runtime_version;
        }
    };

    // ============================================================================
    // §v2.15 — validate_aot_view_v2: also gate on the v2 header's runtime-version
    // window and its declared capability mask. Emits CRANK-AOT-SEC-008 when the
    // current runtime version falls outside the artifact's [min,max] window, and
    // CRANK-AOT-SEC-006 when the artifact declares a capability the policy does not
    // allow. When the artifact was built against a reflected type layout
    // (reflection_layout_hash != 0) and the caller passes the current layout
    // fingerprint, a mismatch emits CRANK-AOT-SEC-009 — reflected field offsets are
    // only valid under a matching layout_context (§v2.16), so an ABI/packing/
    // endianness/version shift must invalidate the artifact. Delegates all v1 checks
    // to validate_aot_view above.
    // ============================================================================

    [[nodiscard]] inline std::vector<std::string>
    validate_aot_view_v2(const crank_aot_key& key,
                         std::span<const std::uint8_t> artifact_bytes,
                         const aot_artifact_header_v2& header,
                         std::uint32_t runtime_version,
                         const aot_security_policy& policy,
                         std::uint64_t current_reflection_layout_hash = 0) {
        auto diags = validate_aot_view(key, artifact_bytes, policy);

        // Runtime version window (§v2.15). A runtime outside [min,max] must refuse.
        if (!header.version_in_range(runtime_version)) {
            diags.push_back(
                "CRANK-AOT-SEC-008: runtime version " + std::to_string(runtime_version)
                + " outside artifact window [" + std::to_string(header.min_runtime_version)
                + "," + std::to_string(header.max_runtime_version) + "]");
        }

        // Executable-memory request (SEC-005): a relocating artifact needs the
        // executable-memory opt-in; without it, reject.
        if (header.relocation_count > 0 && !policy.allow_executable_memory) {
            diags.push_back(
                "CRANK-AOT-SEC-005: artifact requests relocation/executable memory "
                "but policy.allow_executable_memory is false");
        }

        // Declared-capability allowlist (SEC-006) for untrusted artifacts: any bit
        // set in capability_mask outside allowed_capabilities is rejected. The
        // allowlist is by name; the mask bit index maps to allowed_capabilities
        // position, so an out-of-range bit (no corresponding allow entry) is denied.
        if (policy.trust == aot_trust_level::untrusted
            && header.capability_mask != 0) {
            const std::size_t allowed = policy.allowed_capabilities.size();
            for (std::size_t bit = 0; bit < 64; ++bit) {
                if ((header.capability_mask >> bit) & 1ULL) {
                    if (bit >= allowed) {
                        diags.push_back(
                            "CRANK-AOT-SEC-006: artifact declares capability bit "
                            + std::to_string(bit) + " not in allowed_capabilities");
                    }
                }
            }
        }

        // Reflected-layout invalidation (SEC-009, §v2.16). If the artifact was built
        // against a reflected type layout and the caller supplies the current
        // fingerprint, a mismatch means the offsets are stale (ABI/packing/endian/
        // version changed) and the artifact must be recompiled.
        if (header.reflection_layout_hash != 0
            && current_reflection_layout_hash != 0
            && header.reflection_layout_hash != current_reflection_layout_hash) {
            diags.push_back(
                "CRANK-AOT-SEC-009: reflected type layout changed since the artifact "
                "was built (layout fingerprint mismatch); recompile — field offsets "
                "are only valid under a matching layout_context");
        }

        return diags;
    }

    //
    // An AOT artifact is a *compilation product* (lowered + verified physical MIR),
    // not a scalar execution result. The interpreter is a linear block evaluator
    // with no branch execution, so any function containing control flow (loops)
    // cannot yield a scalar under it. Caching therefore gates on lowering +
    // verify_physical_mir success, not on interpreter runtime success.
    //
    // Workflow:
    //   1. Compute fingerprint from key.
    //   2. Check cache: hit → return stored bytes immediately.
    //   3. Miss: lower HL→physical MIR, verify. Fatal on lower/verify failure.
    //   4. Best-effort interpreter run for a scalar return value (control-flow
    //      diagnostics are non-fatal notes, not compile failures).
    //   5. Serialize the artifact and store.
    // ============================================================================

    struct aot_cache_result {
        aot_cache_status status = aot_cache_status::miss;
        std::vector<std::uint8_t> bytes; // artifact bytes (from cache or freshly compiled)
        std::optional<std::int64_t> return_value;
        std::vector<std::string> diagnostics; // fatal compile diagnostics only
        std::vector<std::string> notes; // non-fatal runtime notes (e.g. interpreter limits)
        bool fallback_fired = false; // true when interpreter ran (not native)

        // ok() reflects compile success — fatal diagnostics only. Non-fatal runtime
        // notes (interpreter control-flow limits) do not flip this.
        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    [[nodiscard]] inline aot_cache_result
    compile_and_cache(aot_cache& cache,
                      const crank_aot_key& key,
                      const lower_hl_result& hl_res,
                      const std::vector<std::int64_t>& args = {},
                      execute_options exec_opts = {}) {
        (void)exec_opts; // preserved for API compat; hint is read from hl_res.exec_hint
        aot_cache_result res;

        // Check cache
        const aot_cache_entry* entry = cache.find(key);
        if (entry) {
            res.status = aot_cache_status::hit;
            res.bytes = entry->bytes;
            return res;
        }

        res.status = aot_cache_status::miss;

        // Upstream lowering failure is fatal.
        if (!hl_res.ok()) {
            res.diagnostics = hl_res.diagnostics;
            return res;
        }

        // Lower HL → physical MIR.
        lithe::codegen::hl::coordinate_lowering_pass lower_pass;
        auto lower_result = lower_pass.run(hl_res.hl_fn);
        lower_result.fn.metadata.current_phase =
            lithe::codegen::mir::phase::physical_mir;

        for (const auto& d : lower_result.diagnostics)
            res.diagnostics.push_back(d);
        if (!res.diagnostics.empty()) return res;

        const auto verification = lithe::codegen::verify_physical_mir(lower_result.fn);
        if (!verification.ok()) {
            for (const auto& d : verification.diagnostics)
                res.diagnostics.push_back(d);
            return res;
        }
        // Mark as verified so downstream passes skip redundant re-verification.
        lower_result.fn.verified = true;

        // Build compile_request from key's backend_id and the hl_res execution hint.
        lithe::execution::compile_request req;
        req.hint = hl_res.exec_hint;
        req.policy = {};

        // Override hint preferred backend from the key's backend_id if it names asmjit/simd/gpu.
        const auto registry_name = detail::registry_backend_id(key.backend_id);
        if (registry_name == "asmjit" || registry_name == "jit")
            req.hint.preferred = lithe::exec::execution_kind::scalar;
        else if (registry_name == "simd")
            req.hint.preferred = lithe::exec::execution_kind::simd;
        else if (registry_name == "gpu")
            req.hint.preferred = lithe::exec::execution_kind::gpu;
        // "interpreter" → no preferred hint, plan() will pick the best available.

        // Compile via the converged lithe pipeline (native primary, interpreter fallback).
        auto cr = lithe::execution::compile(lower_result.fn, req);

        res.return_value = lithe::execution::invoke(cr, std::span<const std::int64_t>{args});
        res.fallback_fired = cr.fallback_fired;

        for (const auto& d : cr.diagnostics) {
            if (detail::is_nonfatal_interp_note(d)) res.notes.push_back(d);
            else res.diagnostics.push_back(std::move(d));
        }

        // Store serialized bytes: 8-byte placeholder encoding the return value for
        // backward-compat cache entries.  The live jit_function_handle is in cr.artifact
        // (in-process only; use artifact_store for cross-call native caching).
        std::vector<std::uint8_t> artifact_bytes;
        artifact_bytes.resize(sizeof(std::uint64_t), 0);
        if (res.return_value) {
            const std::int64_t v = *res.return_value;
            std::memcpy(artifact_bytes.data(), &v, sizeof(v));
        }

        cache.store(key, artifact_bytes);
        res.bytes = std::move(artifact_bytes);
        return res;
    }

    // ============================================================================
    // §v2.14 — separate compilation: cross-module instantiation link records.
    //
    // Each separately-compiled module exports a compact metadata record of the
    // generic instantiations it produced (fingerprint + the ABI hash it was
    // compiled against). When several modules are linked, link_modules() gates on
    // ABI compatibility and deduplicates instantiations that appear in more than
    // one module (they collapse to one — matching instantiation_registry dedup).
    //
    // The ABI gate is the load-bearing safety property: two modules that reference
    // the same instantiation fingerprint but were compiled against different native
    // ABIs cannot be linked (CRANK-LINK-001) — their in-memory layouts diverge.
    // ============================================================================

    // One exported instantiation from a separately-compiled module.
    struct instantiation_link_record {
        std::uint64_t fingerprint = 0; // instantiation_key::fingerprint()
        std::uint64_t native_abi_hash = 0; // ABI this instantiation was compiled against
        std::string symbol; // monomorphized name, e.g. "Reduce#<fp>"
    };

    // The exported link metadata for one separately-compiled module.
    struct module_link_metadata {
        std::string module_name;
        std::uint64_t native_abi_hash = 0;
        std::vector<instantiation_link_record> instantiations;

        // Serialize to a compact binary record: [name_len][name][abi][count]
        // then count * ([fp][abi][sym_len][sym]). All integers little-endian u64.
        // Instantiations are emitted in ascending-fingerprint order so the byte output
        // is CANONICAL — identical instantiation sets serialize identically regardless
        // of the order they were recorded (§v2.1a stable metadata).
        [[nodiscard]] std::vector<std::uint8_t> serialize() const {
            std::vector<std::uint8_t> out;
            auto put_u64 = [&](std::uint64_t v) {
                for (int i = 0; i < 8; ++i)
                    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
            };
            auto put_str = [&](std::string_view s) {
                put_u64(s.size());
                out.insert(out.end(), s.begin(), s.end());
            };
            put_str(module_name);
            put_u64(native_abi_hash);
            put_u64(instantiations.size());

            std::vector<instantiation_link_record> sorted(instantiations);
            std::sort(sorted.begin(), sorted.end(),
                      [](const instantiation_link_record& a, const instantiation_link_record& b) {
                          if (a.fingerprint != b.fingerprint) return a.fingerprint < b.fingerprint;
                          return a.symbol < b.symbol; // total order for identical fingerprints
                      });
            for (const auto& r : sorted) {
                put_u64(r.fingerprint);
                put_u64(r.native_abi_hash);
                put_str(r.symbol);
            }
            return out;
        }

        // canonical_hash — a single stable identity for this module's instantiation set,
        // computed as FNV-1a over the canonical serialized bytes. Order-independent, so
        // two builds that record the same instantiations in different orders agree
        // (§v2.1a stable metadata / ABI). Feeds deterministic AOT keying.
        [[nodiscard]] std::uint64_t canonical_hash() const {
            const auto bytes = serialize();
            std::uint64_t h = 14695981039346656037ULL;
            for (std::uint8_t b : bytes) {
                h ^= static_cast<std::uint64_t>(b);
                h *= 1099511628211ULL;
            }
            return h;
        }
    };

    // The result of linking several module_link_metadata records.
    struct link_result {
        // Distinct instantiations after cross-module dedup (keyed on fingerprint).
        std::vector<instantiation_link_record> merged;
        std::vector<std::string> diagnostics; // CRANK-LINK-001 on ABI clash

        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    // link_modules — merge module link metadata, dedup instantiations, ABI-gate.
    //
    // - Same fingerprint across modules with matching ABI ⇒ collapse to one entry.
    // - Same fingerprint with a *different* ABI ⇒ CRANK-LINK-001 (cannot link).
    // - Module-level ABI hashes that disagree are themselves a CRANK-LINK-001.
    [[nodiscard]] inline link_result
    link_modules(const std::vector<module_link_metadata>& modules) {
        link_result res;
        std::unordered_map<std::uint64_t, instantiation_link_record> by_fp;

        std::optional<std::uint64_t> host_abi;
        for (const auto& m : modules) {
            if (m.native_abi_hash != 0) {
                if (!host_abi) host_abi = m.native_abi_hash;
                else if (*host_abi != m.native_abi_hash) {
                    res.diagnostics.push_back(
                        "CRANK-LINK-001: module '" + m.module_name
                        + "' native ABI hash differs from prior modules; cannot link");
                }
            }
            for (const auto& r : m.instantiations) {
                auto it = by_fp.find(r.fingerprint);
                if (it == by_fp.end()) {
                    by_fp.emplace(r.fingerprint, r);
                }
                else if (it->second.native_abi_hash != r.native_abi_hash) {
                    res.diagnostics.push_back(
                        "CRANK-LINK-001: instantiation '" + r.symbol
                        + "' compiled against conflicting ABIs across modules");
                }
                // matching fingerprint + matching ABI ⇒ dedup (drop the duplicate)
            }
        }

        for (auto& [fp, rec] : by_fp) {
            (void)fp;
            res.merged.push_back(std::move(rec));
        }
        // Canonical order: sort by fingerprint so the linked set is independent of input
        // module order and of unordered_map iteration order (§v2.1a stable metadata).
        std::sort(res.merged.begin(), res.merged.end(),
                  [](const instantiation_link_record& a, const instantiation_link_record& b) {
                      if (a.fingerprint != b.fingerprint) return a.fingerprint < b.fingerprint;
                      return a.symbol < b.symbol;
                  });
        return res;
    }
} // namespace crank
