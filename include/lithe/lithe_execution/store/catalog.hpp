#pragma once

// =============================================================================
// lithe_execution/store/catalog.hpp — artifact catalog + publish protocol (impl-3)
//
// Provides:
//   catalog_entry         — per-artifact catalog row (metadata, no payload bytes)
//   lease_record          — per-key compile lease
//   catalog_error         — error type for catalog operations
//   catalog concept       — abstract store-agnostic interface (arch §8)
//   memory_catalog        — in-process fallback (std::unordered_map, zero deps)
//   petika_catalog        — durable Petika-backed catalog (in petika_catalog.hpp)
//   host_profile          — the runtime environment for compatibility checking
//   compatibility_result  — structured pass/fail with per-clause diagnostics (arch §9)
//   check_compatible()    — conjunctive compatibility predicate
//   get_or_compile()      — per-key lease + compute-once + atomic publish (arch §7)
//
// Key-space layout (Petika adapter, values supplied by its serializer):
//   "a:" + key_digest[32]     → catalog_entry
//   "l:" + key_digest[32]     → lease_record
//   "x:" + accessed_ns[8] + digest[32] → "" (LRU eviction secondary index)
//
// Atomicity is provided by the selected catalog implementation.  Petika uses
// its transaction and durable journal; memory_catalog is process-local.
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "artifact_record.hpp"   // artifact_key, compatibility_manifest, …
#include "blob_store.hpp"        // blob_address
#include "utils/single_flight.hpp" // utils::lease_token

namespace lithe::execution::store {
    // =============================================================================
    // catalog_entry — one row in the artifact catalog (no payload bytes)
    // =============================================================================

    struct catalog_entry {
        artifact_key key{optimized_key{}};
        artifact_kind kind = artifact_kind::optimized_portable;
        blob_address blob_addr{}; // content-address into blob store
        std::uint64_t blob_size = 0;
        std::array<std::uint8_t, 64> semantic_digest{};
        std::uint8_t semantic_digest_len = 32;
        lithe::ir::schema_version ir_schema{1, 0, 0};
        abi_fingerprint abi_fp{};
        std::optional<backend_id> backend{};
        std::optional<backend_version> backend_ver{};
        capability_fingerprint target_caps_fp{};
        std::uint64_t created_ns = 0;
        std::uint64_t accessed_ns = 0;
        provenance prov{};
        compatibility_manifest compat{};
        std::optional<signature_info> sig{};

        [[nodiscard]] std::array<std::uint8_t, 32> key_digest() const noexcept {
            return compute_key_digest(key);
        }
    };

    // =============================================================================
    // lease_record — per-key compile lease (expires to prevent wedging)
    // =============================================================================

    struct lease_record {
        std::uint64_t owner_id = 0; // monotonic counter
        std::uint64_t acquired_ns = 0;
        std::uint64_t expires_ns = 0; // wall clock ns; 0 = never expires
    };

    // =============================================================================
    // catalog_error
    // =============================================================================

    enum class catalog_error_code : std::uint8_t {
        not_found,
        lease_contended,
        publish_failed,
        backend_error,
        codec_error,
    };

    struct catalog_error {
        catalog_error_code code = catalog_error_code::backend_error;
        std::string detail;

        [[nodiscard]] static catalog_error not_found(std::string d = {}) {
            return {catalog_error_code::not_found, std::move(d)};
        }

        [[nodiscard]] static catalog_error contended(std::string d = {}) {
            return {catalog_error_code::lease_contended, std::move(d)};
        }

        [[nodiscard]] static catalog_error backend(std::string d) {
            return {catalog_error_code::backend_error, std::move(d)};
        }

        [[nodiscard]] static catalog_error codec(std::string d) {
            return {catalog_error_code::codec_error, std::move(d)};
        }
    };

    // =============================================================================
    // eviction_query — parameters for list_evictable
    // =============================================================================

    struct eviction_query {
        std::size_t max_entries = 64; // maximum number of entries to return
        std::uint64_t accessed_before_ns = 0; // return entries accessed before this time
    };

    // =============================================================================
    // catalog concept (arch §8 store-agnostic interface)
    // =============================================================================

    template <class C>
    concept catalog =
        requires(C& c,
                 const artifact_key& k,
                 const catalog_entry& e,
                 const utils::lease_token& t,
                 const eviction_query& eq) {
            { c.lookup(k) } -> std::same_as<
                std::expected<std::optional<catalog_entry>, catalog_error>>;
            { c.acquire_lease(k) } -> std::same_as<std::expected<utils::lease_token, catalog_error>>;
            { c.publish(e, t) } -> std::same_as<std::expected<void, catalog_error>>;
            { c.abandon(k, t) } -> std::same_as<std::expected<void, catalog_error>>;
            { c.touch(k) };
            { c.evict(k) } -> std::same_as<std::expected<void, catalog_error>>;
            { c.list_evictable(eq) } -> std::same_as<std::vector<catalog_entry>>;
        };

    // =============================================================================
    // Helper: current wall-clock nanoseconds
    // =============================================================================

    [[nodiscard]] inline std::uint64_t now_ns() noexcept {
        // Lease timestamps are durable catalog data.  A steady-clock epoch is
        // process/boot dependent and therefore cannot be interpreted after a
        // restart; use Unix wall time for persisted TTL comparisons.
        const auto tp = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
    }

    // =============================================================================
    // memory_catalog — in-process zero-dep fallback (satisfies catalog concept)
    // =============================================================================

    class memory_catalog {
    public:
        [[nodiscard]] std::expected<std::optional<catalog_entry>, catalog_error>
        lookup(const artifact_key& key) {
            const auto digest = compute_key_digest(key);
            std::shared_lock lk(mu_);
            const auto it = entries_.find(hex(digest));
            if (it == entries_.end()) return std::optional<catalog_entry>{};
            return std::optional<catalog_entry>{it->second};
        }

        [[nodiscard]] std::expected<utils::lease_token, catalog_error>
        acquire_lease(const artifact_key& key) {
            const auto digest = compute_key_digest(key);
            const auto hk = hex(digest);
            std::unique_lock lk(mu_);

            // If already published by a concurrent winner, fail — caller re-looks-up.
            if (entries_.contains(hk))
                return std::unexpected(catalog_error::contended("already published"));

            // Check for an active non-expired lease.
            const auto ns = now_ns();
            if (const auto lit = leases_.find(hk); lit != leases_.end()) {
                const auto& lr = lit->second;
                if (lr.expires_ns == 0 || lr.expires_ns > ns)
                    return std::unexpected(catalog_error::contended("lease held"));
                // Expired lease — take over.
            }

            const auto owner = ++next_owner_;
            lease_record lr;
            lr.owner_id = owner;
            lr.acquired_ns = ns;
            lr.expires_ns = ns + k_lease_ttl_ns;
            leases_[hk] = lr;
            return utils::lease_token{owner};
        }

        [[nodiscard]] std::expected<void, catalog_error>
        publish(const catalog_entry& entry, const utils::lease_token& token) {
            const auto digest = compute_key_digest(entry.key);
            const auto hk = hex(digest);
            std::unique_lock lk(mu_);

            // Verify the token matches the held lease.
            const auto lit = leases_.find(hk);
            if (lit == leases_.end())
                return std::unexpected(catalog_error::backend("publish without lease"));
            if (lit->second.owner_id != token.id)
                return std::unexpected(catalog_error::backend("lease token mismatch"));
            if (lit->second.expires_ns != 0 && lit->second.expires_ns <= now_ns())
                return std::unexpected(catalog_error::backend("publish with expired lease"));
            entries_[hk] = entry;
            leases_.erase(hk);
            // Insert eviction index (key = accessed_ns_hex + "_" + digest_hex).
            eviction_index_.emplace(eviction_key(entry.accessed_ns, digest), hk);
            return {};
        }

        [[nodiscard]] std::expected<void, catalog_error>
        abandon(const artifact_key& key, const utils::lease_token& token) {
            const auto hk = hex(compute_key_digest(key));
            std::unique_lock lk(mu_);
            const auto it = leases_.find(hk);
            if (it == leases_.end()) return {};
            if (it->second.owner_id != token.id)
                return std::unexpected(catalog_error::backend(
                    "cannot abandon another lease owner"));
            leases_.erase(it);
            return {};
        }

        void touch(const artifact_key& key) {
            const auto digest = compute_key_digest(key);
            const auto hk = hex(digest);
            std::unique_lock lk(mu_);
            const auto it = entries_.find(hk);
            if (it == entries_.end()) return;
            const auto old_ns = it->second.accessed_ns;
            const auto new_ns = now_ns();
            it->second.accessed_ns = new_ns;
            eviction_index_.erase(eviction_key(old_ns, digest));
            eviction_index_.emplace(eviction_key(new_ns, digest), hk);
        }

        [[nodiscard]] std::expected<void, catalog_error>
        evict(const artifact_key& key) {
            const auto digest = compute_key_digest(key);
            const auto hk = hex(digest);
            std::unique_lock lk(mu_);
            const auto it = entries_.find(hk);
            if (it != entries_.end()) {
                eviction_index_.erase(eviction_key(it->second.accessed_ns, digest));
                entries_.erase(it);
            }
            return {};
        }

        [[nodiscard]] std::vector<catalog_entry>
        list_evictable(const eviction_query& q) {
            std::shared_lock lk(mu_);
            std::vector<catalog_entry> result;
            result.reserve(q.max_entries);
            for (const auto& [ekey, hk] : eviction_index_) {
                if (result.size() >= q.max_entries) break;
                const auto it = entries_.find(hk);
                if (it == entries_.end()) continue;
                if (q.accessed_before_ns > 0 && it->second.accessed_ns >= q.accessed_before_ns)
                    continue;
                result.push_back(it->second);
            }
            return result;
        }

        [[nodiscard]] std::size_t size() const {
            std::shared_lock lk(mu_);
            return entries_.size();
        }

    private:
        static constexpr std::uint64_t k_lease_ttl_ns = 30'000'000'000ULL; // 30 s

        mutable std::shared_mutex mu_;
        std::unordered_map<std::string, catalog_entry> entries_;
        std::unordered_map<std::string, lease_record> leases_;
        std::map<std::string, std::string> eviction_index_; // ordered
        std::uint64_t next_owner_ = 0;

        // Hex encode 32 bytes
        [[nodiscard]] static std::string hex(const std::array<std::uint8_t, 32>& d) {
            static constexpr char kH[] = "0123456789abcdef";
            std::string s;
            s.reserve(64);
            for (auto b : d) {
                s.push_back(kH[(b >> 4) & 0xf]);
                s.push_back(kH[b & 0xf]);
            }
            return s;
        }

        [[nodiscard]] static std::string eviction_key(
            std::uint64_t ns, const std::array<std::uint8_t, 32>& d) {
            // Pad ns to 20 decimal digits for lexicographic order.
            char buf[21];
            std::snprintf(buf, sizeof(buf), "%020llu",
                          static_cast<unsigned long long>(ns));
            return std::string(buf) + "_" + hex(d);
        }
    };

    static_assert(catalog<memory_catalog>);

    // Durable catalogs are supplied by petika_catalog.hpp.

    // =============================================================================
    // host_profile — the runtime environment used by check_compatible (arch §9)
    // =============================================================================

    struct host_profile {
        lithe::ir::schema_version supported_ir_schema{1, 0, 0};
        abi_fingerprint host_abi{};
        capability_set available_caps{};
        target_restrictions host_target{};
        std::vector<std::string> available_symbols;
        security_policy_id active_policy{};
    };

    // =============================================================================
    // compatibility_result — structured pass/fail with per-clause diagnostics (arch §9)
    // =============================================================================

    enum class compat_clause : std::uint8_t {
        schema_version,
        abi,
        capabilities,
        target,
        external_symbols,
        security_policy,
    };

    struct compat_clause_result {
        compat_clause clause;
        bool passed = false;
        std::string detail;
    };

    struct compatibility_result {
        bool passed = true;
        std::vector<compat_clause_result> clauses;

        void fail(compat_clause c, std::string detail) {
            passed = false;
            clauses.push_back({c, false, std::move(detail)});
        }

        void pass(compat_clause c) {
            clauses.push_back({c, true, {}});
        }
    };

    // =============================================================================
    // check_compatible — conjunctive compatibility predicate (arch §9)
    //
    // Checks: schema supported AND semantic ABI compatible AND required capabilities
    //   available AND target restrictions satisfied AND external symbols resolve
    //   AND security policy permits. Returns structured diagnostics.
    // =============================================================================

    [[nodiscard]] inline compatibility_result
    check_compatible(const compatibility_manifest& artifact,
                     const host_profile& host,
                     const security_policy_id& policy) {
        compatibility_result r;

        // 1. IR schema: artifact's schema must be ≤ host supported
        const auto& as = artifact.ir_schema;
        const auto& hs = host.supported_ir_schema;
        if (as.major > hs.major ||
            (as.major == hs.major && as.minor > hs.minor) ||
            (as.major == hs.major && as.minor == hs.minor && as.patch > hs.patch)) {
            r.fail(compat_clause::schema_version,
                   "artifact schema " + std::to_string(as.major) + "." +
                   std::to_string(as.minor) + "." + std::to_string(as.patch) +
                   " > host " + std::to_string(hs.major) + "." +
                   std::to_string(hs.minor) + "." + std::to_string(hs.patch));
        }
        else {
            r.pass(compat_clause::schema_version);
        }

        // 2. ABI fingerprint must match
        if (artifact.abi.digest != host.host_abi.digest)
            r.fail(compat_clause::abi, "ABI fingerprint mismatch");
        else
            r.pass(compat_clause::abi);

        // 3. Required capabilities ⊆ available capabilities
        if ((artifact.required_caps.bits & host.available_caps.bits) !=
            artifact.required_caps.bits) {
            r.fail(compat_clause::capabilities,
                   "required capabilities not all available (missing bits: " +
                   std::to_string(artifact.required_caps.bits & ~host.available_caps.bits) + ")");
        }
        else {
            r.pass(compat_clause::capabilities);
        }

        // 4. Target restrictions: OS + arch must match (if specified)
        const auto& at = artifact.target;
        const auto& ht = host.host_target;
        if ((!at.os.empty() && at.os != ht.os) ||
            (!at.arch.empty() && at.arch != ht.arch) ||
            (at.min_os_version > 0 && at.min_os_version > ht.min_os_version)) {
            r.fail(compat_clause::target, "target restrictions not satisfied");
        }
        else {
            r.pass(compat_clause::target);
        }

        // 5. External symbols must all be available
        for (const auto& req : artifact.ext_syms) {
            const bool found = std::any_of(
                host.available_symbols.begin(), host.available_symbols.end(),
                [&](const std::string& sym) { return sym == req.name; });
            if (!found) {
                r.fail(compat_clause::external_symbols,
                       "unresolved external symbol: " + req.name);
            }
        }
        if (std::none_of(r.clauses.begin(), r.clauses.end(),
                         [](const auto& c) {
                             return c.clause == compat_clause::external_symbols && !c.passed;
                         })) {
            r.pass(compat_clause::external_symbols);
        }

        // 6. Security policy: artifact's policy id/version must match active policy
        if (artifact.security.id != 0 &&
            (artifact.security.id != policy.id ||
                artifact.security.version > policy.version)) {
            r.fail(compat_clause::security_policy, "security policy mismatch");
        }
        else {
            r.pass(compat_clause::security_policy);
        }

        return r;
    }

    // =============================================================================
    // get_or_compile — per-key lease + compute-once + atomic publish (arch §7, §12)
    //
    // Template params:
    //   Cat         — satisfies catalog<Cat>
    //   BlobStore   — satisfies artifact_store<BlobStore>
    //   CompileFn   — () → std::expected<artifact_record, E>
    //
    // Protocol:
    //   lookup → hit:  touch + return entry
    //   lookup → miss: acquire_lease → compute → validate → put blob → publish
    //   contended miss: retry until winner publishes or lease expires
    // =============================================================================

    template <catalog Cat, artifact_store BlobStore, class CompileFn>
    [[nodiscard]] std::expected<catalog_entry, catalog_error>
    get_or_compile(Cat& cat, BlobStore& blobs, const artifact_key& key,
                   CompileFn&& compile_fn,
                   std::uint32_t max_retry = 16,
                   std::chrono::milliseconds retry_delay = std::chrono::milliseconds{5}) {
        auto lookup = [&]()
            -> std::expected<std::optional<catalog_entry>, catalog_error> {
            auto result = cat.lookup(key);
            if (result) return result;
            // A malformed metadata row is recoverable: remove its lookup/index
            // visibility and rebuild from the authoritative input.  Backend I/O
            // failures remain failures and are never disguised as misses.
            if (result.error().code != catalog_error_code::codec_error)
                return std::unexpected(result.error());
            auto evicted = cat.evict(key);
            if (!evicted) return std::unexpected(evicted.error());
            return std::optional<catalog_entry>{};
        };

        // Fast path: hit.
        auto initial = lookup();
        if (!initial) return std::unexpected(initial.error());
        if (*initial) {
            cat.touch(key);
            return **initial;
        }

        for (std::uint32_t attempt = 0; attempt < max_retry; ++attempt) {
            auto token = cat.acquire_lease(key);
            if (token) {
                // Winner: compile OUTSIDE the catalog write path.
                auto computed = std::invoke(std::forward<CompileFn>(compile_fn));
                if (!computed) {
                    (void)cat.abandon(key, *token);
                    return std::unexpected(catalog_error::backend("compile_fn failed"));
                }

                const artifact_record& rec = *computed;
                if (rec.key != key) {
                    (void)cat.abandon(key, *token);
                    return std::unexpected(catalog_error::backend(
                        "compile_fn returned a record for a different key"));
                }

                // Put payload blob.
                std::span<const std::uint8_t> payload_bytes;
                std::vector<std::uint8_t> inline_copy;
                if (const auto* ip = std::get_if<inline_payload>(&rec.payload)) {
                    payload_bytes = std::span<const std::uint8_t>(ip->bytes.data(), ip->bytes.size());
                }
                blob_address addr{};
                if (!payload_bytes.empty()) {
                    auto put_result = blobs.put(payload_bytes);
                    if (!put_result) {
                        (void)cat.abandon(key, *token);
                        return std::unexpected(catalog_error::backend("blob put failed: " +
                            put_result.error().detail));
                    }
                    addr = *put_result;
                }

                // Build catalog entry.
                catalog_entry entry;
                entry.key = rec.key;
                entry.kind = rec.kind;
                entry.blob_addr = addr;
                entry.blob_size = payload_bytes.size();
                entry.semantic_digest = rec.semantic_digest;
                entry.semantic_digest_len = rec.semantic_digest_len;
                entry.ir_schema = rec.compat.ir_schema;
                entry.abi_fp = rec.compat.abi;
                entry.backend = rec.prov.backend;
                entry.backend_ver = rec.prov.backend_ver;
                if (const auto* executable =
                        std::get_if<executable_key>(&rec.key))
                    entry.target_caps_fp = executable->target_caps;
                entry.prov = rec.prov;
                entry.compat = rec.compat;
                entry.sig = rec.signature;
                entry.created_ns = now_ns();
                entry.accessed_ns = entry.created_ns;

                // Atomic publish.
                auto pub = cat.publish(entry, *token);
                if (!pub) {
                    (void)cat.abandon(key, *token);
                    return std::unexpected(pub.error());
                }

                return entry;
            }

            if (token.error().code != catalog_error_code::lease_contended)
                return std::unexpected(token.error());

            // Loser: sleep + re-lookup (winner may have finished).
            std::this_thread::sleep_for(retry_delay);
            auto retried = lookup();
            if (!retried) return std::unexpected(retried.error());
            if (*retried) {
                cat.touch(key);
                return **retried;
            }
        }

        return std::unexpected(catalog_error::backend("get_or_compile: max_retry exceeded"));
    }
} // namespace lithe::execution::store
