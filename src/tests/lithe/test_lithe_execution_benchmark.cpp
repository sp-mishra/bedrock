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
