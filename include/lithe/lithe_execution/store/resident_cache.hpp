#pragma once

// =============================================================================
// lithe_execution/store/resident_cache.hpp — in-memory caches (impl-3)
//
// Provides:
//   key_digest_t             — 32-byte hex string used as cache key
//   decoded_ir_cache         — Kosha ShardedLRUCache for decoded portable_module
//   installed_code_cache     — Kosha ShardedLRUCache for live any_compiled_artifact
//   make_decoded_ir_cache()  — construct with configurable capacity
//   make_installed_code_cache() — construct with configurable capacity
//   retirement_queue         — deferred reclamation for code with active frames
//   try_evict_installed()    — evict only if active_frames == 0; else defer
//
// Architecture (arch §7, §8):
//   decoded_ir_cache:   key = key_digest (32-byte hex), value = shared_ptr<portable_module>
//   installed_code_cache: key = key_digest, value = shared_ptr<code_resource>
//     Eviction gated on code_resource::active_frames == 0 (retirement safety).
//     If active_frames > 0, the entry moves to retirement_queue;
//     the queue is drained on subsequent operations (lazy reclamation).
//
// Reuses:
//   kosha::core::ShardedLRUCache — from containers/cache/kosha.hpp
//   code_resource / has_active_frames() — from lithe_rt/code_metadata.hpp
//   portable_module — from lithe_ir/portable/module.hpp
//
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "containers/cache/kosha.hpp"       // ShardedLRUCache
#include "../../lithe_ir/portable/module.hpp"         // portable_module
#include "../../lithe_rt/code_metadata.hpp"           // code_resource, has_active_frames

namespace lithe::execution::store {
    // =============================================================================
    // key_digest_t — 32-byte hex string (64 chars), primary cache key
    // =============================================================================

    using key_digest_t = std::string; // 64-char hex of compute_key_digest()

    // =============================================================================
    // decoded_ir_cache — resident decoded portable_module objects
    //
    // Keyed by key_digest_t (hex of optimized_key digest).
    // Value is a shared_ptr so the module outlives cache eviction during use.
    // =============================================================================

    using decoded_ir_cache =
    kosha::ShardedLRUCache<key_digest_t,
                           std::shared_ptr<lithe::ir::portable::portable_module>,
                           16>;

    [[nodiscard]] inline std::unique_ptr<decoded_ir_cache>
    make_decoded_ir_cache(std::size_t total_capacity = 1024) {
        return std::make_unique<decoded_ir_cache>(total_capacity);
    }

    // =============================================================================
    // installed_code_cache — resident live code_resource handles
    //
    // Keyed by key_digest_t (hex of executable_key digest).
    // Value is a shared_ptr<code_resource>; eviction is gated on active_frames.
    // =============================================================================

    using installed_code_cache =
    kosha::ShardedLRUCache<key_digest_t,
                           std::shared_ptr<lithe::rt::code_resource>,
                           16>;

    [[nodiscard]] inline std::unique_ptr<installed_code_cache>
    make_installed_code_cache(std::size_t total_capacity = 512) {
        return std::make_unique<installed_code_cache>(total_capacity);
    }

    // =============================================================================
    // retirement_queue — deferred reclamation for code_resource with active frames
    //
    // Thread-safe. Entries added when try_evict_installed fails due to active frames.
    // drain() checks each entry; removes those with active_frames == 0.
    // =============================================================================

    class retirement_queue {
    public:
        void defer(std::shared_ptr<lithe::rt::code_resource> res) {
            std::lock_guard lk(mu_);
            queue_.push_back(std::move(res));
        }

        // Drain entries whose active_frames have reached zero.
        // Returns the number of entries actually reclaimed.
        std::size_t drain() {
            std::lock_guard lk(mu_);
            std::size_t reclaimed = 0;
            std::deque<std::shared_ptr<lithe::rt::code_resource>> remaining;
            for (auto& res : queue_) {
                if (!res->has_active_frames()) {
                    res->unwind.live = false;
                    res->roots.live = false;
                    res->state.store(lithe::rt::code_state::retired,
                                     std::memory_order_release);
                    ++reclaimed;
                    // shared_ptr released here (res goes out of scope)
                }
                else {
                    remaining.push_back(std::move(res));
                }
            }
            queue_ = std::move(remaining);
            return reclaimed;
        }

        [[nodiscard]] std::size_t pending() const {
            std::lock_guard lk(mu_);
            return queue_.size();
        }

    private:
        mutable std::mutex mu_;
        std::deque<std::shared_ptr<lithe::rt::code_resource>> queue_;
    };

    // =============================================================================
    // try_evict_installed — evict only if active_frames == 0; else defer (arch §12)
    //
    // Returns true if evicted immediately, false if deferred.
    // =============================================================================

    [[nodiscard]] inline bool
    try_evict_installed(installed_code_cache& cache,
                        retirement_queue& queue,
                        const key_digest_t& key) {
        const auto res_result = cache.get(key);
        if (!res_result) return true; // not in cache, nothing to do

        const auto& res = *res_result;
        // Close the entry gate before observing the frame count.  Invocation
        // increments first and then re-checks this state, so either the caller
        // owns a live frame or it will refuse to enter; no new frame can race
        // reclamation after this store.
        res->state.store(lithe::rt::code_state::retiring,
                         std::memory_order_release);
        if (!res->has_active_frames()) {
            cache.erase(key);
            res->unwind.live = false;
            res->roots.live = false;
            res->state.store(lithe::rt::code_state::retired,
                             std::memory_order_release);
            return true;
        }

        // Active frames — defer reclamation and remove from the hot cache.
        cache.erase(key);
        queue.defer(res);
        return false;
    }
} // namespace lithe::execution::store
