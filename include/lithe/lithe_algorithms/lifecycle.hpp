#pragma once

// =============================================================================
// lithe_algorithms/lifecycle.hpp — tiering / eviction / retirement policy stubs
//   +: retirement driver with Kosha eviction + frame-drain protocol
//
// Design:
//   Policy concepts (structural, no inheritance):
//     tiering_policy<P>    — decides when to re-compile to a higher tier
//     eviction_policy<P>   — decides when to evict a resource_store entry
//     retirement_policy<P> — decides when a code version is fully retired
//
//   Retirement algorithm (eviction ≠ retirement):
//     Step 1 (Kosha eviction): remove lookup visibility (done by the cache).
//     Retirement driver:
//       mark retiring → redirect stable entry cell → wait for frame drain →
//       unregister unwind/stack-map metadata → release backend resource.
//
//   Active frames and outstanding execution_events are BOTH counted as live
//   frames for the purposes of the drain gate (: an event counts like a
//   live frame).
//
//   tiering_driver: consumes profiler input (call counts) → fires tiering_policy.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "../lithe_execution/foundation.hpp"   // execution_mode, ir_kind, execution_event
#include "../lithe_execution/entry.hpp"        // frame_counter_ref

namespace lithe::algorithms {
    // =========================================================================
    //  tiering_policy concept
    // =========================================================================

    template <class P>
    concept tiering_policy =
        requires {
            { P::descriptor_id() } -> std::convertible_to<std::string_view>;
        } &&
        requires(P& p, std::uint64_t version_id,
                 execution::execution_mode mode, std::uint64_t calls) {
            { p.should_tier(version_id, mode, calls) } -> std::same_as<bool>;
            { p.target_tier(mode) } -> std::same_as<execution::execution_mode>;
        };

    // =========================================================================
    //  eviction_policy concept
    // =========================================================================

    template <class P>
    concept eviction_policy =
        requires {
            { P::descriptor_id() } -> std::convertible_to<std::string_view>;
        } &&
        requires(P& p, std::uint64_t version_id,
                 std::uint64_t last_epoch, std::uint64_t now_epoch) {
            { p.should_evict(version_id, last_epoch, now_epoch) } -> std::same_as<bool>;
        };

    // =========================================================================
    //  retirement_policy concept
    // =========================================================================

    template <class P>
    concept retirement_policy =
        requires {
            { P::descriptor_id() } -> std::convertible_to<std::string_view>;
        } &&
        requires(P& p, std::uint64_t version_id, std::uint64_t active_frames) {
            { p.can_retire(version_id, active_frames) } -> std::same_as<bool>;
        };

    // =========================================================================
    //  no_op_tiering_policy — safe default: never tiers
    // =========================================================================

    struct no_op_tiering_policy {
        [[nodiscard]] static constexpr std::string_view descriptor_id() noexcept {
            return "lithe.algorithms.lifecycle.no_op_tiering";
        }

        [[nodiscard]] constexpr bool
        should_tier(std::uint64_t, execution::execution_mode,
                    std::uint64_t) const noexcept { return false; }

        [[nodiscard]] constexpr execution::execution_mode
        target_tier(execution::execution_mode m) const noexcept { return m; }
    };

    static_assert(tiering_policy<no_op_tiering_policy>);
    static_assert(std::is_empty_v<no_op_tiering_policy>);

    // =========================================================================
    //  no_op_eviction_policy — safe default: never evicts
    // =========================================================================

    struct no_op_eviction_policy {
        [[nodiscard]] static constexpr std::string_view descriptor_id() noexcept {
            return "lithe.algorithms.lifecycle.no_op_eviction";
        }

        [[nodiscard]] constexpr bool
        should_evict(std::uint64_t, std::uint64_t, std::uint64_t) const noexcept {
            return false;
        }
    };

    static_assert(eviction_policy<no_op_eviction_policy>);
    static_assert(std::is_empty_v<no_op_eviction_policy>);

    // =========================================================================
    //  no_op_retirement_policy — safe default: retires when no active frames
    // =========================================================================

    struct no_op_retirement_policy {
        [[nodiscard]] static constexpr std::string_view descriptor_id() noexcept {
            return "lithe.algorithms.lifecycle.no_op_retirement";
        }

        [[nodiscard]] constexpr bool
        can_retire(std::uint64_t, const std::uint64_t active_frames) const noexcept {
            return active_frames == 0;
        }
    };

    static_assert(retirement_policy<no_op_retirement_policy>);
    static_assert(std::is_empty_v<no_op_retirement_policy>);

    // =========================================================================
    //  call_count_tiering_policy — tier after N calls
    //
    // Upgrades interpret → jit_tier1 → jit_tier2 based on call count thresholds.
    // =========================================================================

    struct call_count_tiering_policy {
        std::uint64_t tier1_threshold = 100; // calls before jit_tier1
        std::uint64_t tier2_threshold = 1000; // calls before jit_tier2

        [[nodiscard]] static constexpr std::string_view descriptor_id() noexcept {
            return "lithe.algorithms.lifecycle.call_count_tiering";
        }

        [[nodiscard]] bool
        should_tier(std::uint64_t /*version_id*/,
                    execution::execution_mode mode,
                    std::uint64_t call_count) const noexcept {
            using em = execution::execution_mode;
            if (mode == em::interpret && call_count >= tier1_threshold) return true;
            if (mode == em::jit_tier1 && call_count >= tier2_threshold) return true;
            return false;
        }

        [[nodiscard]] execution::execution_mode
        target_tier(const execution::execution_mode mode) const noexcept {
            using em = execution::execution_mode;
            if (mode == em::interpret) return em::jit_tier1;
            if (mode == em::jit_tier1) return em::jit_tier2;
            return mode;
        }
    };

    static_assert(tiering_policy<call_count_tiering_policy>);

    // =========================================================================
    //  default_lifecycle_policies — convenience bundle
    // =========================================================================

    template <class Tiering = no_op_tiering_policy,
              class Eviction = no_op_eviction_policy,
              class Retirement = no_op_retirement_policy>
    struct lifecycle_policies {
        [[no_unique_address]] Tiering tiering{};
        [[no_unique_address]] Eviction eviction{};
        [[no_unique_address]] Retirement retirement{};
    };

    using default_lifecycle_policies = lifecycle_policies<>;
    static_assert(std::is_empty_v<default_lifecycle_policies>,
                  "default_lifecycle_policies with all no-op slots must be empty");

    // =========================================================================
    // P10 retirement_state — per-version lifecycle state
    //
    // Tracks the phase of one code version's retirement.
    // =========================================================================

    enum class retirement_state : std::uint8_t {
        live = 0, // code is in active use
        evicted = 1, // removed from lookup; draining frames
        retiring = 2, // redirecting stable entry cell
        draining = 3, // waiting for frame_counter to reach 0
        unregistering = 4, // removing unwind/stack-map metadata
        released = 5, // backend resource released
    };

    // =========================================================================
    // P10 retirement_record — one entry in the retirement queue
    //
    // Retirement is initiated by the Kosha eviction callback; the driver then
    // drives the record through the state machine.
    // =========================================================================

    struct retirement_record {
        std::uint64_t version_id = 0;
        execution::frame_counter_ref frame_counter;
        std::atomic<retirement_state> state{retirement_state::live};
        std::function<void()> unregister_metadata_fn; // unwind/stack-map
        std::function<void()> release_resource_fn; // destroy backend resource
        std::uint32_t outstanding_events = 0; // async events count

        retirement_record() = default;

        retirement_record(const retirement_record&) = delete;
        retirement_record& operator=(const retirement_record&) = delete;
        retirement_record(retirement_record&&) = delete;
        retirement_record& operator=(retirement_record&&) = delete;

        [[nodiscard]] std::uint64_t live_frame_count() const noexcept {
            if (!frame_counter) return 0;
            return frame_counter->load(std::memory_order_acquire) +
                static_cast<std::uint64_t>(outstanding_events);
        }

        [[nodiscard]] bool can_proceed_past_drain() const noexcept {
            return live_frame_count() == 0;
        }
    };

    // =========================================================================
    // P10 retirement_driver
    //
    // Drives retirement_records through the state machine.
    // Called periodically (e.g., from a background thread or on each compile).
    //
    // Sequence per record:
    //   live      → evicted       (triggered by Kosha eviction callback)
    //   evicted   → retiring      (set by driver: redirect stable entry cell)
    //   retiring  → draining      (entry cell redirected; now count frames)
    //   draining  → unregistering (frame_counter + events == 0)
    //   unregistering → released  (unwind/stack-map removed; resource released)
    //
    // Kosha eviction owns step 1 (remove lookup visibility); driver owns 2–5.
    // =========================================================================

    class retirement_driver {
    public:
        retirement_driver() = default;

        retirement_driver(const retirement_driver&) = delete;
        retirement_driver& operator=(const retirement_driver&) = delete;
        retirement_driver(retirement_driver&&) = default;
        retirement_driver& operator=(retirement_driver&&) = default;

        // Enqueue a new retirement record.  Called by Kosha eviction callback.
        // After enqueue the record is in `evicted` state.
        void enqueue(std::shared_ptr<retirement_record> rec) {
            if (!rec) return;
            rec->state.store(retirement_state::evicted, std::memory_order_release);
            pending_.push_back(std::move(rec));
        }

        // tick() — drive all pending records one step forward.
        //
        // Returns the number of records that reached `released` this tick.
        // Callers should call tick() periodically.
        std::size_t tick() {
            std::size_t released_count = 0;
            auto it = pending_.begin();
            while (it != pending_.end()) {
                auto& rec = *it;
                if (!rec) {
                    it = pending_.erase(it);
                    continue;
                }

                const retirement_state s =
                    rec->state.load(std::memory_order_acquire);

                switch (s) {
                case retirement_state::evicted:
                    // Step 2: redirect stable entry cell.
                    // In the full implementation this would patch a jump/thunk table.
                    // Here we atomically advance to retiring.
                    rec->state.store(retirement_state::retiring,
                                     std::memory_order_release);
                    break;

                case retirement_state::retiring:
                    // Step 3: begin drain.
                    rec->state.store(retirement_state::draining,
                                     std::memory_order_release);
                    break;

                case retirement_state::draining:
                    // Step 4: wait for all frames + events to drain.
                    if (rec->can_proceed_past_drain()) {
                        rec->state.store(retirement_state::unregistering,
                                         std::memory_order_release);
                    }
                    break;

                case retirement_state::unregistering:
                    // Step 5a: remove unwind/stack-map metadata.
                    if (rec->unregister_metadata_fn)
                        rec->unregister_metadata_fn();
                    // Step 5b: release backend resource.
                    if (rec->release_resource_fn)
                        rec->release_resource_fn();
                    rec->state.store(retirement_state::released,
                                     std::memory_order_release);
                    ++released_count;
                    it = pending_.erase(it);
                    continue;

                case retirement_state::released:
                    // Already released; clean up the queue entry.
                    it = pending_.erase(it);
                    continue;

                case retirement_state::live:
                    // Should not happen; skip.
                    break;
                }
                ++it;
            }
            return released_count;
        }

        // drain_all(timeout) — call tick() until all pending records retire
        // or the timeout expires.  Useful for shutdown.
        bool drain_all(const std::chrono::milliseconds timeout =
            std::chrono::milliseconds{5000}) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!pending_.empty()) {
                tick();
                if (pending_.empty()) return true;
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::yield();
            }
            return true;
        }

        [[nodiscard]] std::size_t pending_count() const noexcept {
            return pending_.size();
        }

        [[nodiscard]] bool idle() const noexcept { return pending_.empty(); }

    private:
        std::vector<std::shared_ptr<retirement_record>> pending_;
    };

    // =========================================================================
    // P10 profiling_counter — lightweight per-version call counter
    //
    // Used by tiering_driver to decide when to tier a code version.
    // =========================================================================

    struct profiling_counter {
        std::uint64_t version_id = 0;
        std::atomic<std::uint64_t> call_count{0};
        execution::execution_mode current_mode = execution::execution_mode::interpret;

        profiling_counter() = default;

        profiling_counter(const std::uint64_t vid,
                          const execution::execution_mode mode) noexcept
            : version_id(vid), current_mode(mode) {}

        profiling_counter(const profiling_counter&) = delete;
        profiling_counter& operator=(const profiling_counter&) = delete;

        void record_call() noexcept {
            call_count.fetch_add(1, std::memory_order_relaxed);
        }

        [[nodiscard]] std::uint64_t calls() const noexcept {
            return call_count.load(std::memory_order_relaxed);
        }
    };

    // =========================================================================
    // P10 tiering_driver<Policy>
    //
    // Consumes profiling_counters and fires tiering_policy::should_tier to
    // decide when to upgrade a code version.
    //
    // Callers register a recompile_fn; the driver calls it when the policy fires.
    // =========================================================================

    template <tiering_policy Policy = no_op_tiering_policy>
    class tiering_driver {
    public:
        using policy_type = Policy;

        struct tier_request {
            std::uint64_t version_id;
            execution::execution_mode target_mode;
        };

        explicit tiering_driver(Policy policy = {}) : policy_(std::move(policy)) {}

        // Register a profiling counter for a code version.
        void register_counter(std::shared_ptr<profiling_counter> counter) {
            if (counter) counters_.push_back(std::move(counter));
        }

        // tick() — inspect all counters; return tier_requests for versions that
        // should be tiered up.
        std::vector<tier_request> tick() {
            std::vector<tier_request> requests;
            for (const auto& c : counters_) {
                if (!c) continue;
                if (policy_.should_tier(c->version_id, c->current_mode, c->calls())) {
                    requests.push_back({
                        c->version_id,
                        policy_.target_tier(c->current_mode)
                    });
                }
            }
            return requests;
        }

        [[nodiscard]] Policy& policy() noexcept { return policy_; }
        [[nodiscard]] const Policy& policy() const noexcept { return policy_; }
        [[nodiscard]] std::size_t counter_count() const noexcept { return counters_.size(); }

    private:
        Policy policy_;
        std::vector<std::shared_ptr<profiling_counter>> counters_;
    };
} // namespace lithe::algorithms

