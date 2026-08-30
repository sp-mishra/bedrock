#pragma once

// taranga/aot.hpp — AOT cache key assembly + artifact caching for taranga.
//
// C++23, header-only, no virtual, no macros. Namespace: taranga
//
// Mirrors crank/aot.hpp, adapted to taranga's IR. The key idea is unchanged: a
// fingerprint over every field that determines cache validity (module name, source
// hash, compiler/target/opt/backend identity, ABI, descriptor hashes), FNV-1a in a
// stable field order, so any change to any field forces a recompile. The compile
// path is the same converged Lithe pipeline the engine uses — coordinate_lowering
// → verify_physical_mir → execution::compile → execution::invoke — because that is
// the single source of truth for turning live HL MIR into a callable.
//
// Differences from crank:
//   • taranga_aot_key carries a Wasm-oriented identity (module + function name).
//   • The unit compiled is a taranga::lowered_function (its hl_fn), not crank's
//     lower_hl_result — taranga's lower_result holds no exec_hint, so the hint is
//     supplied by the caller (via the key's backend_id / an explicit request).
//   • freeze_function is offered separately (freeze_for_interchange) for the
//     AOT-key / serialization path; it is NOT the execution path.
//
// Caching gates on lowering + verify success (a control-flow function may not yield
// a scalar under the interpreter, so runtime success is not a precondition), exactly
// as crank does. The stored bytes are an 8-byte return-value placeholder — the live
// native handle lives in the compile_result (in-process; use lithe's artifact_store
// for cross-call native caching).

#include "languages/taranga/lower_hl.hpp"

#include "lithe/lithe_codegen.hpp"            // verify_physical_mir, mir::phase
#include "lithe/lithe_codegen_hl_passes.hpp"  // coordinate_lowering_pass
#include "lithe/lithe_execution/compile.hpp"  // execution::compile / invoke
#include "lithe/lithe_ir/portable/freeze.hpp" // freeze_function (interchange only)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace taranga {

    // =========================================================================
    // fnv1a — local constexpr helpers used in key assembly (same constants as
    // crank so a shared corpus of fingerprints stays comparable across languages).
    // =========================================================================

    namespace detail {

        [[nodiscard]] constexpr std::uint64_t
        fnv1a(std::uint64_t seed, std::string_view s) noexcept {
            constexpr std::uint64_t k_prime = 1099511628211ULL;
            for (const char c : s) {
                seed ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
                seed *= k_prime;
            }
            return seed;
        }

        [[nodiscard]] constexpr std::uint64_t
        fnv1a_u64(std::uint64_t seed, std::uint64_t v) noexcept {
            constexpr std::uint64_t k_prime = 1099511628211ULL;
            for (int i = 0; i < 8; ++i) {
                seed ^= static_cast<std::uint64_t>((v >> (i * 8)) & 0xFFu);
                seed *= k_prime;
            }
            return seed;
        }

        // Map a descriptive backend id ("lithe.backend.asmjit") to the bare name
        // the lithe registry understands ("asmjit"). The prefixed form is kept in
        // the fingerprint; only the registry lookup needs the bare name.
        [[nodiscard]] inline std::string
        registry_backend_id(std::string_view backend_id) {
            constexpr std::string_view k_prefix = "lithe.backend.";
            if (backend_id.starts_with(k_prefix))
                backend_id.remove_prefix(k_prefix.size());
            return std::string(backend_id);
        }

        // Hash the raw bytes of a source image — the source_hash field of a key.
        [[nodiscard]] constexpr std::uint64_t
        hash_source(std::string_view bytes) noexcept {
            constexpr std::uint64_t k_offset = 14695981039346656037ULL;
            return fnv1a(k_offset, bytes);
        }

    } // namespace detail

    // =========================================================================
    // taranga_aot_key — every field that determines cache validity.
    //
    // Any change to any field → fingerprint mismatch → recompile. Mirrors
    // crank_aot_key field-for-field so cross-language tooling can read either.
    // =========================================================================

    struct taranga_aot_key {
        std::string module_name;                       // module identity
        std::string function_name;                     // the compiled function
        std::uint64_t source_hash = 0;                 // FNV-1a of source bytes
        std::vector<std::uint64_t> dep_hashes;         // dependency source hashes
        std::string compiler_version;                  // e.g. "taranga.1.0.0"
        std::string target_triple;                     // e.g. "arm64-apple-macos14.0"
        std::string opt_profile_id;                    // e.g. "taranga.o3"
        std::string backend_id;                        // e.g. "lithe.backend.interpreter"
        std::uint64_t enabled_features = 0;            // feature bitmask
        std::uint64_t native_abi_hash = 0;             // ABI-relevant layout hash
        std::vector<std::uint64_t> descriptor_hashes;  // type/fn descriptors

        // fingerprint — FNV-1a over all fields in a stable order.
        [[nodiscard]] std::uint64_t fingerprint() const noexcept {
            constexpr std::uint64_t k_offset = 14695981039346656037ULL;
            std::uint64_t h = k_offset;
            h = detail::fnv1a(h, module_name);
            h = detail::fnv1a(h, function_name);
            h = detail::fnv1a_u64(h, source_hash);
            for (const auto dh : dep_hashes) h = detail::fnv1a_u64(h, dh);
            h = detail::fnv1a(h, compiler_version);
            h = detail::fnv1a(h, target_triple);
            h = detail::fnv1a(h, opt_profile_id);
            h = detail::fnv1a(h, backend_id);
            h = detail::fnv1a_u64(h, enabled_features);
            h = detail::fnv1a_u64(h, native_abi_hash);
            for (const auto dh : descriptor_hashes) h = detail::fnv1a_u64(h, dh);
            return h;
        }

        // to_json — minimal JSON for dumping/inspecting a key.
        [[nodiscard]] std::string to_json() const {
            auto hex = [](std::uint64_t v) -> std::string {
                char buf[20];
                std::snprintf(buf, sizeof(buf), "0x%016llx",
                              static_cast<unsigned long long>(v));
                return buf;
            };
            std::string out = "{";
            out += "\"module\":\"" + module_name + "\"";
            out += ",\"function\":\"" + function_name + "\"";
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

    // Build a key from module/function identity + a source hash, filling the
    // compiler/target/opt/backend fields from taranga defaults. source_hash is the
    // caller's (detail::hash_source over the source image).
    [[nodiscard]] inline taranga_aot_key
    make_aot_key(std::string_view module_name,
                 std::string_view function_name,
                 std::uint64_t source_hash,
                 std::string_view opt_profile_id = "taranga.o3",
                 std::string_view backend_id = "lithe.backend.interpreter",
                 std::string_view target_triple = "arm64-apple-macos14.0",
                 std::uint64_t native_abi_hash = 0,
                 std::uint64_t enabled_features = 0) {
        taranga_aot_key k;
        k.module_name = std::string(module_name);
        k.function_name = std::string(function_name);
        k.source_hash = source_hash;
        k.compiler_version = "taranga.1.0.0";
        k.target_triple = std::string(target_triple);
        k.opt_profile_id = std::string(opt_profile_id);
        k.backend_id = std::string(backend_id);
        k.enabled_features = enabled_features;
        k.native_abi_hash = native_abi_hash;
        return k;
    }

    // =========================================================================
    // aot_cache — in-process fingerprint → raw bytes cache.
    //
    // The test / embedded path (a production deployment would persist to disk).
    // Single-threaded access assumed.
    // =========================================================================

    enum class aot_cache_status : std::uint8_t {
        miss,       // not in cache
        hit,        // valid entry found
        recompile,  // entry present but fingerprint differs — key changed
    };

    [[nodiscard]] constexpr std::string_view to_string(aot_cache_status s) noexcept {
        switch (s) {
        case aot_cache_status::miss:      return "miss";
        case aot_cache_status::hit:       return "hit";
        case aot_cache_status::recompile: return "recompile";
        }
        return "miss";
    }

    struct aot_cache_entry {
        std::uint64_t fingerprint = 0;
        std::vector<std::uint8_t> bytes; // serialized artifact bytes
    };

    class aot_cache {
    public:
        aot_cache() = default;

        void store(const taranga_aot_key& key, std::vector<std::uint8_t> bytes) {
            const auto fp = key.fingerprint();
            entries_[fp] = aot_cache_entry{fp, std::move(bytes)};
        }

        [[nodiscard]] const aot_cache_entry*
        find(const taranga_aot_key& key) const noexcept {
            auto it = entries_.find(key.fingerprint());
            if (it == entries_.end()) return nullptr;
            return &it->second;
        }

        void invalidate(const taranga_aot_key& key) {
            entries_.erase(key.fingerprint());
        }

        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
        void clear() noexcept { entries_.clear(); }

    private:
        std::unordered_map<std::uint64_t, aot_cache_entry> entries_;
    };

    // =========================================================================
    // compile_and_cache — compile one lowered function, caching the artifact.
    //
    // Workflow (mirrors crank::compile_and_cache):
    //   1. Check cache: hit → return stored bytes immediately.
    //   2. Miss: coordinate_lowering → set phase → verify_physical_mir.
    //      Fatal on any lowering/verify diagnostic.
    //   3. Compile via the converged lithe pipeline (native primary, interpreter
    //      fallback), biasing the backend from the key's backend_id.
    //   4. Best-effort invoke for a scalar return value.
    //   5. Serialize an 8-byte return-value placeholder and store.
    // =========================================================================

    struct aot_cache_result {
        aot_cache_status status = aot_cache_status::miss;
        std::vector<std::uint8_t> bytes;              // artifact bytes (cache or fresh)
        std::optional<std::int64_t> return_value;
        std::vector<std::string> diagnostics;         // fatal compile diagnostics only
        std::vector<std::string> notes;               // non-fatal notes
        bool fallback_fired = false;                  // interpreter ran (not native)

        // ok() reflects compile success — fatal diagnostics only.
        [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
    };

    [[nodiscard]] inline aot_cache_result
    compile_and_cache(aot_cache& cache,
                      const taranga_aot_key& key,
                      const lowered_function& lf,
                      std::span<const std::int64_t> args = {}) {
        namespace hl = lithe::codegen::hl;
        namespace cg = lithe::codegen;
        aot_cache_result res;

        // 1. Cache check.
        if (const aot_cache_entry* entry = cache.find(key)) {
            res.status = aot_cache_status::hit;
            res.bytes = entry->bytes;
            return res;
        }
        res.status = aot_cache_status::miss;

        // 2. Live HL MIR → physical MIR, then verify.
        hl::coordinate_lowering_pass lower_pass;
        auto lr = lower_pass.run(lf.hl_fn);
        lr.fn.metadata.current_phase = cg::mir::phase::physical_mir;

        for (const auto& d : lr.diagnostics) res.diagnostics.push_back(d);
        if (!res.diagnostics.empty()) return res;

        const auto verification = cg::verify_physical_mir(lr.fn);
        if (!verification.ok()) {
            for (const auto& d : verification.diagnostics)
                res.diagnostics.push_back(d);
            return res;
        }
        lr.fn.verified = true;

        // 3. Build the compile request, biasing the preferred backend from the key.
        lithe::execution::compile_request req;
        req.policy = {};
        const auto registry_name = detail::registry_backend_id(key.backend_id);
        if (registry_name == "asmjit" || registry_name == "jit")
            req.hint.preferred = lithe::exec::execution_kind::scalar;
        else if (registry_name == "simd")
            req.hint.preferred = lithe::exec::execution_kind::simd;
        else if (registry_name == "gpu")
            req.hint.preferred = lithe::exec::execution_kind::gpu;
        // "interpreter" → leave the hint unbiased; plan() picks the best available.

        auto cr = lithe::execution::compile(lr.fn, req);
        res.return_value = lithe::execution::invoke(cr, args);
        res.fallback_fired = cr.fallback_fired;
        for (const auto& d : cr.diagnostics) res.notes.push_back(d);

        // 5. Serialize an 8-byte return-value placeholder (backward-compat cache
        //    entry). The live native handle stays in cr.artifact (in-process).
        std::vector<std::uint8_t> artifact_bytes(sizeof(std::uint64_t), 0);
        if (res.return_value) {
            const std::int64_t v = *res.return_value;
            std::memcpy(artifact_bytes.data(), &v, sizeof(v));
        }
        cache.store(key, artifact_bytes);
        res.bytes = std::move(artifact_bytes);
        return res;
    }

    // =========================================================================
    // freeze_for_interchange — the AOT-key / serialization projection.
    //
    // freeze turns live HL MIR into the portable lithe_hl_mir_ir interchange form.
    // This is deliberately separate from execution: it is what a persistent AOT
    // store or a cross-process handoff serializes, NOT what compile_and_cache runs
    // (which drives the live physical MIR directly). Offered here so a caller that
    // wants a stable, hashable projection of a lowered function has one call.
    // Returns lithe's expected<…, freeze_error>: an unmapped opcode or dangling
    // operand surfaces as the error, never a partially-formed interchange module.
    // =========================================================================

    [[nodiscard]] inline std::expected<lithe::ir::adapters::lithe_hl_mir_ir,
                                       lithe::ir::portable::freeze_error>
    freeze_for_interchange(const lowered_function& lf,
                           lithe::ir::portable::freeze_options opts = {}) {
        return lithe::ir::portable::freeze_function(lf.hl_fn, opts);
    }

} // namespace taranga
