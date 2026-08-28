#pragma once

// =============================================================================
// lithe_cost_registry.hpp — Runtime registry for named Lithe cost models
//
// Namespace:  lithe::cost
// Depends on: lithe/lithe_algorithms/selection.hpp  (algorithm_box)
//             <shared_mutex>, <unordered_map>, <string>, <optional>,
//             <functional>, <span>, <cstddef>
//
// Provides:
//   cost_fn                — type-erased SBO callable:
//                            float(const void* node, std::span<const float>)
//   cost_model_descriptor  — {id, description, cost_fn}; move-only
//   cost_registry          — thread-safe runtime registry (shared_mutex)
//     void register_model(cost_model_descriptor)
//     std::optional<std::reference_wrapper<const cost_fn>>
//                  find(std::string_view id)
//     void unregister(std::string_view id)
//     static cost_registry& global()
//
// Design notes:
//   • algorithm_box is move-only (copy deleted), so cost_model_descriptor is
//     move-only.  The registry stores descriptors by string key in an
//     unordered_map; all accesses are guarded by a shared_mutex.
//   • find() returns std::optional<std::reference_wrapper<const cost_fn>>.
//     The reference is valid only while the caller holds the shared_lock;
//     callers should copy the function object or complete their call before
//     releasing the lock.  In practice, registries are long-lived and
//     unregistration is rare, so pointer stability is sufficient for most
//     use-cases.
//   • global() returns the process-wide singleton (Meyer's singleton; safe
//     for static initialisation order).
// =============================================================================

#include "lithe_algorithms/selection.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lithe::cost {
    // =============================================================================
    // cost_fn — type-erased, SBO-backed cost callable
    //
    // Signature: float(const void* node_ptr, std::span<const float> child_costs)
    //
    // The void* carries a pointer to the concrete e_node (or equivalent) that
    // the cost function inspects.  Callers are responsible for passing the
    // correct pointer type; the registry does not enforce type safety at runtime.
    //
    // InlineBytes = 64: one cache line; sufficient for all built-in cost models.
    // =============================================================================

    using cost_fn = algorithms::algorithm_box<float(const void*, std::span<const float>), 64>;

    // =============================================================================
    // cost_model_descriptor
    //
    // Bundles a stable string identifier, a human-readable description, and the
    // cost callable.  Move-only because algorithm_box is move-only.
    // =============================================================================

    struct cost_model_descriptor {
        std::string id; // stable ASCII key, e.g. "lithe.cost.cpu"
        std::string description; // human-readable, e.g. "CPU instruction heuristic"
        cost_fn fn; // move-only callable

        // Explicitly delete copy operations (cost_fn is already move-only, but
        // being explicit here improves diagnostic messages).
        cost_model_descriptor(const cost_model_descriptor&) = delete;
        cost_model_descriptor& operator=(const cost_model_descriptor&) = delete;

        cost_model_descriptor(cost_model_descriptor&&) = default;
        cost_model_descriptor& operator=(cost_model_descriptor&&) = default;

        // Convenience constructor.
        cost_model_descriptor(std::string id_, std::string desc_, cost_fn fn_)
            : id(std::move(id_))
              , description(std::move(desc_))
              , fn(std::move(fn_)) {}
    };

    // =============================================================================
    // cost_registry — thread-safe registry of named cost models
    //
    // All public methods are thread-safe:
    //   • register_model / unregister acquire an exclusive (write) lock.
    //   • find acquires a shared (read) lock and returns a reference_wrapper
    //     pointing into the map.  The reference is stable as long as no concurrent
    //     unregister() call removes the entry — callers that need long-lived access
    //     should copy the cost_fn out under the lock, not hold the reference.
    // =============================================================================

    class cost_registry {
    public:
        cost_registry() = default;
        ~cost_registry() = default;

        // Non-copyable, non-movable (singleton semantics).
        cost_registry(const cost_registry&) = delete;
        cost_registry& operator=(const cost_registry&) = delete;
        cost_registry(cost_registry&&) = delete;
        cost_registry& operator=(cost_registry&&) = delete;

        // -------------------------------------------------------------------------
        // register_model — add or replace a cost model under descriptor.id.
        //
        // If a model with the same id is already registered it is replaced.
        // Thread-safe (exclusive lock).
        // -------------------------------------------------------------------------
        void register_model(cost_model_descriptor descriptor) {
            std::unique_lock lock(mutex_);
            const std::string key = descriptor.id; // copy before move
            models_.insert_or_assign(std::move(key), std::move(descriptor));
        }

        // -------------------------------------------------------------------------
        // find — look up a cost model by its stable string id.
        //
        // Returns std::optional<std::reference_wrapper<const cost_fn>>.
        //   present → the caller has a reference to the fn stored in the map.
        //   empty   → no model with this id is registered.
        //
        // Thread-safe (shared lock held for the duration of the lookup).
        // The returned reference is valid while no unregister() removes the entry.
        // -------------------------------------------------------------------------
        [[nodiscard]] std::optional<std::reference_wrapper<const cost_fn>>
        find(std::string_view id) const {
            std::shared_lock lock(mutex_);
            const auto it = models_.find(std::string{id});
            if (it == models_.end()) return std::nullopt;
            return std::cref(it->second.fn);
        }

        // -------------------------------------------------------------------------
        // find_descriptor — look up the full descriptor (id, description, fn).
        //
        // Returns a pointer into the internal map; valid while the entry is not
        // unregistered.  Returns nullptr if not found.
        // Thread-safe (shared lock held for lookup only; pointer lifetime as above).
        // -------------------------------------------------------------------------
        [[nodiscard]] const cost_model_descriptor*
        find_descriptor(std::string_view id) const {
            std::shared_lock lock(mutex_);
            const auto it = models_.find(std::string{id});
            if (it == models_.end()) return nullptr;
            return &it->second;
        }

        // -------------------------------------------------------------------------
        // unregister — remove a cost model by id.
        //
        // No-op if the id is not registered.  Thread-safe (exclusive lock).
        // -------------------------------------------------------------------------
        void unregister(std::string_view id) {
            std::unique_lock lock(mutex_);
            models_.erase(std::string{id});
        }

        // -------------------------------------------------------------------------
        // size — number of currently registered models (for diagnostics/testing).
        // Thread-safe (shared lock).
        // -------------------------------------------------------------------------
        [[nodiscard]] std::size_t size() const noexcept {
            std::shared_lock lock(mutex_);
            return models_.size();
        }

        // -------------------------------------------------------------------------
        // global() — process-wide singleton registry.
        //
        // Meyer's singleton: thread-safe initialisation (C++11 §6.7).
        // -------------------------------------------------------------------------
        [[nodiscard]] static cost_registry& global() {
            static cost_registry instance;
            return instance;
        }

    private:
        mutable std::shared_mutex mutex_;
        std::unordered_map<std::string, cost_model_descriptor> models_;
    };
} // namespace lithe::cost
