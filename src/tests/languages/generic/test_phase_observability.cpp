#include "catch_amalgamated.hpp"

#include "languages/generic/observability/phase.hpp"
#include "observability/sinks/ring_buffer_sink.hpp"

#include <atomic>
#include <type_traits>

namespace {
    struct test_feedback {
        static constexpr bool enabled = true;
        static inline std::atomic_uint32_t submissions = 0;
        static inline lang::telemetry::phase_metric last{};

        static void reset() noexcept {
            submissions.store(0, std::memory_order_relaxed);
            last = {};
        }

        static void submit(const lang::telemetry::phase_metric metric) noexcept {
            last = metric;
            submissions.fetch_add(1, std::memory_order_relaxed);
        }
    };
}

TEST_CASE("generic phase telemetry: disabled observer is empty", "[generic][telemetry]") {
    using observer = lang::telemetry::phase_observer<>;
    static_assert(!observer::enabled);
    static_assert(std::is_empty_v<lang::telemetry::phase_scope<observer>>);

    lang::telemetry::phase_scope<observer> scope{{
        .unit_id = 7,
        .stage = lang::telemetry::phase::parse,
    }};
    scope.set_outcome(lang::telemetry::phase_outcome::failed);
    SUCCEED();
}

TEST_CASE("generic phase telemetry: feedback receives one final POD metric", "[generic][telemetry]") {
    using observer = lang::telemetry::phase_observer<utils::nadi::NoSink, test_feedback>;
    test_feedback::reset();

    {
        lang::telemetry::phase_scope<observer> scope{{
            .unit_id = 42,
            .stage = lang::telemetry::phase::semantic_check,
            .entity_count = 9,
        }};
        scope.set_iterations(3);
        scope.set_transformations(2);
        scope.set_outcome(lang::telemetry::phase_outcome::fallback);
    }

    REQUIRE(test_feedback::submissions.load(std::memory_order_relaxed) == 1);
    CHECK(test_feedback::last.context.unit_id == 42);
    CHECK(test_feedback::last.context.stage == lang::telemetry::phase::semantic_check);
    CHECK(test_feedback::last.context.entity_count == 9);
    CHECK(test_feedback::last.iterations == 3);
    CHECK(test_feedback::last.transformations == 2);
    CHECK(test_feedback::last.outcome == lang::telemetry::phase_outcome::fallback);
}

TEST_CASE("generic phase telemetry: Nadi receives begin metric and end", "[generic][telemetry][nadi]") {
    using sink = utils::nadi::RingBufferSink<16>;
    using observer = lang::telemetry::phase_observer<sink>;
    auto& buffer = sink::instance();
    while (buffer.try_pop()) {}

    {
        lang::telemetry::phase_scope<observer> scope{{
            .unit_id = 9,
            .stage = lang::telemetry::phase::generic_ir_lower,
        }};
    }

    std::size_t count = 0;
    while (buffer.try_pop()) ++count;
    CHECK(count == 3);
}
