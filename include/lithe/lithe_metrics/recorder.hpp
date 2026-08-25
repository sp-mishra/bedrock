#pragma once

// =============================================================================
// lithe_metrics/recorder.hpp — metric_recorder concept + recorder impls
//
// Namespace: lithe::metrics
//
// Provides:
//   metric_recorder<R> concept — the swappable record() seam
//   null_recorder       — zero-cost default (dead-code-eliminated when used)
//   collecting_recorder<N> — SmallVector-backed in-memory collector
//   synchronized_collecting_recorder<N> — mutex-guarded for light concurrency
//   concurrent_collecting_recorder — MPSC-queue-backed for heavy multi-producer
//   nadi_recorder<Sink> — NADI bridge (emits stage_pulse on record())
//   tee_recorder<Rs...> — fan-out: broadcasts to each recorder in pack
//
// All types satisfy metric_recorder.  No virtual.  C++23.
// =============================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include "stage.hpp"

// NADI transport (opt-in: only included when the NADI header is present)
#if __has_include(<observability/nadi.hpp>)
#  include <observability/nadi.hpp>
#  define LITHE_METRICS_HAS_NADI 1
#else
#  define LITHE_METRICS_HAS_NADI 0
#endif

// SmallVector (SBO storage for short pipelines)
#if __has_include(<containers/dynamic/SmallVector.hpp>)
#  include <containers/dynamic/SmallVector.hpp>
#  define LITHE_METRICS_HAS_SMALL_VECTOR 1
#else
#  define LITHE_METRICS_HAS_SMALL_VECTOR 0
#endif

// Lock-free MPSC queue for high-concurrency recording
#if __has_include(<containers/lockfree/MPSCQueue.hpp>)
#  include <containers/lockfree/MPSCQueue.hpp>
#  define LITHE_METRICS_HAS_MPSC 1
#else
#  define LITHE_METRICS_HAS_MPSC 0
#endif

namespace lithe::metrics {
    // =============================================================================
    // metric_recorder concept
    //
    // R must expose:
    //   static constexpr bool enabled — true iff the recorder does real work
    //   void record(const stage_metric&) noexcept
    // =============================================================================

    template <class R>
    concept metric_recorder =
        requires(R& r, const stage_metric& m) {
            { r.record(m) } noexcept -> std::same_as<void>;
            requires std::convertible_to<decltype(R::enabled), bool>;
        };

    // =============================================================================
    // null_recorder — the zero-cost no-op default
    //
    // Template-param default everywhere; when used, the record() call compiles to
    // nothing (dead-code-eliminated).  static_assert(metric_recorder<null_recorder>)
    // =============================================================================

    struct null_recorder {
        static constexpr bool enabled = false;
        static void record(const stage_metric&) noexcept {}
    };

    static_assert(metric_recorder<null_recorder>);
    static_assert(std::is_empty_v<null_recorder>);

    // =============================================================================
    // collecting_recorder<InlineBytes> — SmallVector-backed single-thread collector
    //
    // Appends stage_metric values into an SBO vector.  No locking — caller must
    // ensure single-threaded access.  build_view() returns a span over the data.
    //
    // InlineBytes: byte budget for inline storage (default 8 metrics × sizeof(stage_metric)).
    // =============================================================================

    template <std::size_t InlineBytes =
#if LITHE_METRICS_HAS_SMALL_VECTOR
            8 * sizeof(stage_metric)
#else
            0 // placeholder; unused when SmallVector absent
#endif
    >
    class collecting_recorder {
    public:
        static constexpr bool enabled = true;

        void record(const stage_metric& m) noexcept {
#if LITHE_METRICS_HAS_SMALL_VECTOR
            data_.push_back(m);
#else
            data_.push_back(m);
#endif
        }

        [[nodiscard]] std::span<const stage_metric> view() const noexcept {
            return {data_.data(), data_.size()};
        }

        [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

        void clear() noexcept { data_.clear(); }

    private:
#if LITHE_METRICS_HAS_SMALL_VECTOR
        containers::dynamic::SmallVector<stage_metric, InlineBytes> data_;
#else
        std::vector<stage_metric> data_;
#endif
    };

    static_assert(metric_recorder<collecting_recorder<>>);

    // =============================================================================
    // synchronized_collecting_recorder<InlineBytes>
    //
    // mutex-guarded variant of collecting_recorder for light multi-producer use.
    // For high-contention parallel backends use concurrent_collecting_recorder.
    // =============================================================================

    template <std::size_t InlineBytes = 8 * sizeof(stage_metric)>
    class synchronized_collecting_recorder {
    public:
        static constexpr bool enabled = true;

        void record(const stage_metric& m) noexcept {
            std::lock_guard lk{mtx_};
            inner_.record(m);
        }

        // Drain into a caller-owned vector under lock — safe to call from consumer.
        [[nodiscard]] std::vector<stage_metric> drain() {
            std::lock_guard lk{mtx_};
            std::vector<stage_metric> out(inner_.view().begin(), inner_.view().end());
            inner_.clear();
            return out;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            std::lock_guard lk{mtx_};
            return inner_.size();
        }

    private:
        mutable std::mutex mtx_;
        collecting_recorder<InlineBytes> inner_;
    };

    static_assert(metric_recorder<synchronized_collecting_recorder<>>);

    // =============================================================================
    // concurrent_collecting_recorder
    //
    // Lock-free MPSC queue backend: many producer stages push without serializing
    // on a mutex.  A single consumer calls drain() to gather all records.
    // =============================================================================

    class concurrent_collecting_recorder {
    public:
        static constexpr bool enabled = true;

        void record(const stage_metric& m) noexcept {
#if LITHE_METRICS_HAS_MPSC
            queue_.push(m);
#else
            std::lock_guard lk{fallback_mtx_};
            fallback_.push_back(m);
#endif
            count_.fetch_add(1, std::memory_order_relaxed);
        }

        // Single-consumer drain — must be called from exactly one thread.
        [[nodiscard]] std::vector<stage_metric> drain() {
            std::vector<stage_metric> out;
#if LITHE_METRICS_HAS_MPSC
            while (auto v = queue_.pop()) out.push_back(*v);
#else
            std::lock_guard lk{fallback_mtx_};
            out.swap(fallback_);
#endif
            return out;
        }

        [[nodiscard]] std::size_t approximate_size() const noexcept {
            return count_.load(std::memory_order_relaxed);
        }

    private:
#if LITHE_METRICS_HAS_MPSC
        lockfree::MPSCQueue<stage_metric> queue_;
#else
        std::mutex fallback_mtx_;
        std::vector<stage_metric> fallback_;
#endif
        std::atomic<std::size_t> count_{0};
    };

    static_assert(metric_recorder<concurrent_collecting_recorder>);

    // =============================================================================
    // nadi_recorder<Sink> — NADI bridge
    //
    // On record(): emits a Pulse<"lithe.stage", ...> to Sink carrying the key
    // metric fields.  Zero-cost when Sink == NoSink.
    // =============================================================================

#if LITHE_METRICS_HAS_NADI

    template <utils::nadi::SinkPolicy Sink>
    class nadi_recorder {
        using stage_field = utils::nadi::Field<"stage", std::uint8_t>;
        using entity_field = utils::nadi::Field<"entity_count", std::uint32_t>;
        using wall_ns_field = utils::nadi::Field<"wall_ns", std::uint64_t>;
        using iterations_field = utils::nadi::Field<"iterations", std::uint32_t>;
        using rule_fired_field = utils::nadi::Field<"rule_fired", std::uint32_t>;
        using diag_err_field = utils::nadi::Field<"diag_errors", std::uint16_t>;

        using stage_pulse_t = utils::nadi::Pulse<"lithe.stage",
                                                 stage_field, entity_field, wall_ns_field,
                                                 iterations_field, rule_fired_field, diag_err_field>;

    public:
        static constexpr bool enabled = Sink::enabled;

        explicit nadi_recorder() = default;
        explicit nadi_recorder(Sink s) noexcept : sink_{std::move(s)} {}

        void record(const stage_metric& m) noexcept {
            if constexpr (Sink::enabled) {
                stage_pulse_t p;
                p.phase = utils::nadi::PulsePhase::Instant;
                std::get<stage_field>(p.payload).value =
                    static_cast<std::uint8_t>(m.stage);
                std::get<entity_field>(p.payload).value = m.entity_count;
                std::get<wall_ns_field>(p.payload).value = m.wall_ns;
                std::get<iterations_field>(p.payload).value = m.iterations;
                std::get<rule_fired_field>(p.payload).value = m.rule_fired;
                std::get<diag_err_field>(p.payload).value = m.diag_errors;
                sink_.emit(p);
            }
        }

    private:
        [[no_unique_address]] Sink sink_{};
    };

    static_assert(metric_recorder<nadi_recorder<utils::nadi::NoSink>>);
    static_assert(std::is_empty_v<nadi_recorder<utils::nadi::NoSink>>);

#else  // !LITHE_METRICS_HAS_NADI — stub for when NADI is absent

    template <class Sink>
    struct nadi_recorder {
        static constexpr bool enabled = false;
        static void record(const stage_metric&) noexcept {}
    };

#endif  // LITHE_METRICS_HAS_NADI

    // =============================================================================
    // tee_recorder<Rs...> — fan-out to multiple recorders
    //
    // Broadcasts every record() call to each recorder in the pack.
    // enabled = true iff at least one recorder has enabled == true.
    // =============================================================================

    template <metric_recorder... Rs>
    class tee_recorder {
    public:
        static constexpr bool enabled = (Rs::enabled || ...);

        explicit tee_recorder(Rs... rs) noexcept : recorders_{std::move(rs)...} {}

        void record(const stage_metric& m) noexcept {
            std::apply([&](auto&... r) { (r.record(m), ...); }, recorders_);
        }

    private:
        std::tuple<Rs...> recorders_;
    };

    static_assert(metric_recorder<tee_recorder<null_recorder, null_recorder>>);
} // namespace lithe::metrics
