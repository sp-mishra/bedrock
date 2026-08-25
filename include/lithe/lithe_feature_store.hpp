#pragma once

// =============================================================================
// lithe_feature_store.hpp — Persistent feature snapshot store
//
// Namespace:  lithe::features
// Depends on: lithe/lithe_feature_extractor.hpp (feature_vector, feature bundles)
//             containers/cache/kosha.hpp        (Cache, ShardedCache, LRUPolicy)
//             <chrono>, <cstddef>, <cstdint>, <optional>, <string_view>
//
// Provides:
//   feature_source          — enum: which extractor produced a snapshot
//   feature_snapshot        — immutable record: hash key + feature_vector +
//                             source + dims + timestamp_ns
//   feature_store           — ShardedCache-backed store; thread-safe put/get/evict
//                             keyed by structural_hash (uint64_t)
//
// Architecture:
//   IR → Feature Extractor → feature_snapshot → feature_store
//
//   Auto-tuning, adaptive optimization, telemetry, and ML cost models all
//   consume the store.  When a snapshot is present, feature re-extraction is
//   skipped entirely — O(1) lookup replaces potentially expensive tree walks.
//
// Design:
//   • feature_snapshot is a plain aggregate; trivially movable.
//   • feature_store wraps ShardedCache<Cache<uint64_t,feature_snapshot,LRU>, 8>.
//   • get() returns std::optional (wraps kosha's expected → optional conversion).
//   • global() is a Meyer's singleton; thread-safe by C++11 §6.7.
//   • No virtual, no macros. C++23. Header-only.
// =============================================================================

#include "lithe_feature_extractor.hpp"
#include "containers/cache/kosha.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace lithe::features {
    // =========================================================================
    // feature_source — identifies which extractor produced the snapshot
    // =========================================================================

    enum class feature_source : std::uint8_t {
        graph = 0, // graph_feature_extractor (Expression AST)
        expression = 1, // expression_feature_extractor
        mir = 2, // mir_feature_extractor (physical_mir_function)
        runtime = 3, // runtime_feature_extractor (execution statistics)
        combined = 4, // combined_feature_extractor output
        custom = 5, // user-supplied extractor
    };

    [[nodiscard]] inline constexpr std::string_view to_string(feature_source s) noexcept {
        switch (s) {
        case feature_source::graph: return "graph";
        case feature_source::expression: return "expression";
        case feature_source::mir: return "mir";
        case feature_source::runtime: return "runtime";
        case feature_source::combined: return "combined";
        case feature_source::custom: return "custom";
        }
        return "unknown";
    }

    // =========================================================================
    // feature_snapshot — immutable record stored in the feature_store
    //
    // Fields:
    //   hash         — structural_hash of the IR/expression at extraction time
    //   fv           — the extracted feature_vector
    //   source       — which extractor produced this snapshot
    //   dims         — fv.size() cached for O(1) access
    //   timestamp_ns — steady_clock nanoseconds when the snapshot was taken;
    //                  0 = not recorded
    // =========================================================================

    struct feature_snapshot {
        std::uint64_t hash = 0;
        feature_vector fv = {};
        feature_source source = feature_source::custom;
        std::uint32_t dims = 0;
        std::int64_t timestamp_ns = 0;

        // Factory: build a snapshot and record the current time.
        [[nodiscard]] static feature_snapshot make(
            std::uint64_t h,
            feature_vector v,
            feature_source src) noexcept {
            feature_snapshot s;
            s.hash = h;
            s.fv = std::move(v);
            s.source = src;
            s.dims = static_cast<std::uint32_t>(s.fv.size());
            s.timestamp_ns = static_cast<std::int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            return s;
        }

        [[nodiscard]] bool valid() const noexcept { return hash != 0 && dims > 0; }
    };

    // =========================================================================
    // feature_store — ShardedCache of feature_snapshots
    //
    // Backed by kosha::ShardedCache<Cache<uint64_t, feature_snapshot, LRU>, 8>.
    // Thread-safe: ShardedCache uses hash-striped shard locking internally.
    //
    // API:
    //   put(hash, snapshot)           — insert / replace
    //   put(hash, fv, source)         — build snapshot inline then insert
    //   get(hash) → optional<snap>    — O(1) lookup; nullopt on miss
    //   evict(hash)                   — remove entry (call after IR invalidation)
    //   size() → size_t               — approximate live entry count
    //   static global() → store&      — process-wide singleton
    //
    // Default capacity: kFeatureStoreDefaultCapacity entries across 8 shards.
    // The store is heap-allocated lazily behind a unique_ptr so its construction
    // cost is paid only when feature storage is actually used.
    // =========================================================================

    inline constexpr std::size_t kFeatureStoreDefaultCapacity = 4096;

    class feature_store {
    public:
        // Cache type: LRU cache keyed by structural_hash.
        using inner_cache_t = kosha::Cache<
            std::uint64_t,
            feature_snapshot,
            kosha::LRUPolicy<std::uint64_t>>;

        // Sharded wrapper: 8 hash-striped shards, false-sharing free.
        using cache_t = kosha::ShardedCache<inner_cache_t, 8>;

        // Explicit capacity constructor.
        explicit feature_store(std::size_t capacity = kFeatureStoreDefaultCapacity)
            : cache_(std::make_unique<cache_t>(capacity)) {}

        // Non-copyable (ShardedCache is non-copyable).
        feature_store(const feature_store&) = delete;
        feature_store& operator=(const feature_store&) = delete;

        // Movable via the unique_ptr.
        feature_store(feature_store&&) = default;
        feature_store& operator=(feature_store&&) = default;

        ~feature_store() = default;

        // Store a snapshot.  Replaces an existing entry for the same hash.
        void put(std::uint64_t hash, feature_snapshot snap) {
            (void)cache_->put(hash, std::move(snap));
        }

        // Convenience: build and store a snapshot inline.
        void put(std::uint64_t hash, feature_vector fv, feature_source src) {
            put(hash, feature_snapshot::make(hash, std::move(fv), src));
        }

        // Look up by structural hash.  Returns nullopt on miss.
        [[nodiscard]] std::optional<feature_snapshot> get(std::uint64_t hash) const {
            auto result = cache_->get(hash);
            if (!result) return std::nullopt;
            return std::move(*result);
        }

        // Remove entry (e.g. after the IR changes and the snapshot is stale).
        void evict(std::uint64_t hash) {
            cache_->erase(hash);
        }

        // Approximate live entry count (sum over all shards; no global lock).
        [[nodiscard]] std::size_t size() const noexcept {
            return cache_->size();
        }

        // Process-wide singleton (Meyer's singleton; thread-safe by C++11 §6.7).
        [[nodiscard]] static feature_store& global() {
            static feature_store instance;
            return instance;
        }

    private:
        std::unique_ptr<cache_t> cache_;
    };
} // namespace lithe::features
