#include "catch_amalgamated.hpp"

#include "lithe/lithe_execution_benchmark.hpp"

TEST_CASE(
    "measure_provider separates cold compilation from warm equivalent execution",
    "[lithe][benchmark][execution]"
) {
    const auto measurement = lithe::benchmark::measure_provider(
        [] { return 7; },
        [](const int artifact) { return artifact * 6; },
        [](const int result) { return result == 42; },
        lithe::benchmark::benchmark_options{.warmup_runs = 1, .measured_runs = 3});

    REQUIRE(measurement.equivalent);
    REQUIRE(measurement.warm_execution_ns.size() == 3);
}

TEST_CASE(
    "measure_provider stops samples on inequivalent execution",
    "[lithe][benchmark][execution]"
) {
    const auto measurement = lithe::benchmark::measure_provider(
        [] { return 1; },
        [](const int) { return 0; },
        [](const int result) { return result == 1; },
        lithe::benchmark::benchmark_options{.warmup_runs = 0, .measured_runs = 4});

    REQUIRE_FALSE(measurement.equivalent);
    REQUIRE(measurement.warm_execution_ns.size() == 1);
}

TEST_CASE(
    "record_measurement publishes only equivalent warm measurements",
    "[lithe][benchmark][feedback]"
) {
    lithe::benchmark::provider_measurement measurement;
    measurement.equivalent = true;
    measurement.warm_execution_ns = {200, 100, 300};
    lithe::feedback::feedback_store store;
    const lithe::benchmark::feedback_record_options options{
        .expression_hash = 17,
        .hardware = {},
        .backend_id = "simd",
        .work_items = 1000,
    };
    REQUIRE(lithe::benchmark::record_measurement(store, measurement, options));
    const auto profile = store.query(17, {});
    REQUIRE(profile.has_value());
    REQUIRE(profile->sample_count == 1);
    REQUIRE(profile->best_backend_id == "simd");
}
