#pragma once

// =============================================================================
// lithe_execution/exec_profiling.hpp — execution-plan diagnostics + profiling
//
// Arch §6 / §11.M4.4: emit NADI Pulse<"lithe.exec"> events at key execution
// pipeline stages. Zero cost when NADI is not linked or Sink is NoSink
// (consistent with the lithe_passes.hpp / ThreadLocalSink pattern).
//
// Events emitted (via route_pulse<Sink>):
//   "lithe.exec.plan"     — plan built; phase=Instant; fields: selected_backend
//   "lithe.exec.compile"  — compile step; Begin/End pair; fields: backend, cache_hit
//   "lithe.exec.install"  — install step; phase=Instant
//   "lithe.exec.invoke"   — invoke step; Begin/End pair; fields: served_by, fallback
//   "lithe.exec.failure"  — failure; phase=Error; fields: stage (uint8)
//
// Usage:
//   using Sink = utils::nadi::NoSink;   // zero-cost path
//   exec_profiler<Sink> prof;
//   prof.on_plan_built(plan);
//   auto t0 = prof.now();
//   // ... compile ...
//   prof.on_compile_end(backend_id, cache_hit, elapsed_ns);
//
// When Sink::enabled == false all methods reduce to no-ops and the compiler
// eliminates them entirely. When NADI is absent the fallback NoSink is used.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <string_view>

#include "execution_result.hpp"    // failure_stage
#include "execution_plan.hpp"      // execution_plan
#include "foundation.hpp"          // persisted_backend_id

// Conditional NADI inclusion — mirrors lithe_passes.hpp.
#if __has_include("../../observability/nadi.hpp")
#  include "../../observability/nadi.hpp"
#  define LITHE_EXEC_HAS_NADI 1
#else
#  define LITHE_EXEC_HAS_NADI 0
#endif

namespace lithe::execution {
    // =============================================================================
    // Fallback NoSink when NADI is absent
    // =============================================================================

#if LITHE_EXEC_HAS_NADI
    using exec_default_sink = utils::nadi::NoSink;
#else
    // Minimal no-op sink compatible with the concept shape used below.
    struct exec_no_sink {
        static constexpr bool enabled = false;
        static constexpr void emit(const auto&) noexcept {}
    };

    using exec_default_sink = exec_no_sink;
#endif

    // =============================================================================
    // exec_profiler<Sink> — collects execution-layer timing events
    //
    // All emit calls are compile-time eliminated when Sink::enabled == false.
    // =============================================================================

    template <class Sink = exec_default_sink>
    struct exec_profiler {
        // ---- now() ----------------------------------------------------------------

        [[nodiscard]] static std::uint64_t now() noexcept {
#if LITHE_EXEC_HAS_NADI
            return static_cast<std::uint64_t>(utils::nadi::SteadyClockPolicy::now());
#else
            return 0;
#endif
        }

        // ---- plan_built -----------------------------------------------------------

        void on_plan_built(const execution_plan& plan) noexcept {
#if LITHE_EXEC_HAS_NADI
            if constexpr (Sink::enabled) {
                if (!plan.valid()) return;
                using namespace utils::nadi;
                Pulse < FixedString{"lithe.exec.plan"} > pulse{};
                pulse.id = generate_event_id();
                pulse.phase = PulsePhase::Instant;
                pulse.timestamp_ns = now();
                const auto lin = capture_lineage();
                pulse.trace_id = lin.root_id.value;
                pulse.parent_id = lin.trace_id.value;
                route_pulse<Sink>(pulse);
            }
#endif
            (void)plan;
        }

        // ---- compile_begin / compile_end ------------------------------------------

        void on_compile_begin(persisted_backend_id backend) noexcept {
#if LITHE_EXEC_HAS_NADI
            if constexpr (Sink::enabled) {
                using namespace utils::nadi;
                Pulse < FixedString{"lithe.exec.compile"} > pulse{};
                pulse.id = generate_event_id();
                pulse.phase = PulsePhase::Begin;
                pulse.timestamp_ns = now();
                compile_event_id_ = pulse.id.value;
                const auto lin = capture_lineage();
                pulse.trace_id = lin.root_id.value;
                pulse.parent_id = lin.trace_id.value;
                route_pulse<Sink>(pulse);
            }
#endif
            (void)backend;
        }

        void on_compile_end(persisted_backend_id backend,
                            bool cache_hit,
                            std::uint64_t elapsed_ns) noexcept {
#if LITHE_EXEC_HAS_NADI
            if constexpr (Sink::enabled) {
                using namespace utils::nadi;
                Pulse < FixedString{"lithe.exec.compile"} > pulse{};
                pulse.id = EventId{compile_event_id_};
                pulse.phase = PulsePhase::End;
                pulse.timestamp_ns = now();
                const auto lin = capture_lineage();
                pulse.trace_id = lin.root_id.value;
                pulse.parent_id = lin.trace_id.value;
                route_pulse<Sink>(pulse);
                (void)cache_hit;
                (void)elapsed_ns;
            }
#endif
            (void)backend;
            (void)cache_hit;
            (void)elapsed_ns;
        }

        // ---- install --------------------------------------------------------------

        void on_install(persisted_backend_id backend,
                        std::size_t code_size_bytes) noexcept {
#if LITHE_EXEC_HAS_NADI
            if constexpr (Sink::enabled) {
                using namespace utils::nadi;
                Pulse < FixedString{"lithe.exec.install"} > pulse{};
                pulse.id = generate_event_id();
                pulse.phase = PulsePhase::Instant;
                pulse.timestamp_ns = now();
                const auto lin = capture_lineage();
                pulse.trace_id = lin.root_id.value;
                pulse.parent_id = lin.trace_id.value;
                route_pulse<Sink>(pulse);
                (void)code_size_bytes;
            }
#endif
            (void)backend;
            (void)code_size_bytes;
        }

        // ---- invoke_begin / invoke_end --------------------------------------------

        void on_invoke_begin(persisted_backend_id backend) noexcept {
#if LITHE_EXEC_HAS_NADI
            if constexpr (Sink::enabled) {
                using namespace utils::nadi;
                Pulse < FixedString{"lithe.exec.invoke"} > pulse{};
                pulse.id = generate_event_id();
                pulse.phase = PulsePhase::Begin;
                pulse.timestamp_ns = now();
                invoke_event_id_ = pulse.id.value;
                const auto lin = capture_lineage();
                pulse.trace_id = lin.root_id.value;
                pulse.parent_id = lin.trace_id.value;
                route_pulse<Sink>(pulse);
            }
#endif
            (void)backend;
        }

        void on_invoke_end(persisted_backend_id served_by,
                           std::uint64_t elapsed_ns,
                           bool fallback_occurred) noexcept {
#if LITHE_EXEC_HAS_NADI
            if constexpr (Sink::enabled) {
                using namespace utils::nadi;
                Pulse < FixedString{"lithe.exec.invoke"} > pulse{};
                pulse.id = EventId{invoke_event_id_};
                pulse.phase = PulsePhase::End;
                pulse.timestamp_ns = now();
                const auto lin = capture_lineage();
                pulse.trace_id = lin.root_id.value;
                pulse.parent_id = lin.trace_id.value;
                route_pulse<Sink>(pulse);
                (void)fallback_occurred;
            }
#endif
            (void)served_by;
            (void)elapsed_ns;
            (void)fallback_occurred;
        }

        // ---- exec_failure ---------------------------------------------------------

        void on_failure(failure_stage stage,
                        std::string_view detail) noexcept {
#if LITHE_EXEC_HAS_NADI
            if constexpr (Sink::enabled) {
                using namespace utils::nadi;
                Pulse < FixedString{"lithe.exec.failure"} > pulse{};
                pulse.id = generate_event_id();
                pulse.phase = PulsePhase::Error;
                pulse.timestamp_ns = now();
                const auto lin = capture_lineage();
                pulse.trace_id = lin.root_id.value;
                pulse.parent_id = lin.trace_id.value;
                route_pulse<Sink>(pulse);
                (void)stage;
                (void)detail;
            }
#endif
            (void)stage;
            (void)detail;
        }

    private:
        std::uint64_t compile_event_id_ = 0;
        std::uint64_t invoke_event_id_ = 0;
    };
} // namespace lithe::execution
