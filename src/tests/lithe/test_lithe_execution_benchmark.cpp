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

TEST_CASE(
    "calibrate_candidate_cost returns an explicit warm-cache candidate only for equivalent work",
    "[lithe][benchmark][execution][feedback]"
) {
    lithe::benchmark::provider_measurement measurement;
    measurement.cold_compile_ns = 900;
    measurement.equivalent = true;
    measurement.warm_execution_ns = {101, 99, 100};
    const lithe::codegen::hl::execution_candidate_cost baseline{
        .kind = lithe::codegen::hl::planned_execution_kind::jit,
        .available = true,
        .setup_cost_ns = 1,
        .work_item_cost_ns = 1,
    };

    const auto calibrated = lithe::benchmark::calibrate_candidate_cost(
        baseline, measurement, {.work_items = 10});

    REQUIRE(calibrated.has_value());
    REQUIRE(calibrated->setup_cost_ns == 900);
    REQUIRE(calibrated->cached_setup_cost_known);
    REQUIRE(calibrated->cached_setup_cost_ns == 0);
    REQUIRE(calibrated->work_item_cost_ns == 10);

    measurement.equivalent = false;
    REQUIRE_FALSE(lithe::benchmark::calibrate_candidate_cost(
        baseline, measurement, {.work_items = 10}).has_value());
}

TEST_CASE(
    "measure_provider annotates benchmark scope and separates phase timings",
    "[lithe][benchmark][execution][scope]"
) {
    const auto measurement = lithe::benchmark::measure_provider(
        [] { return 21; },
        [](const int artifact) { return artifact * 2; },
        [](const int result) { return result == 42; },
        lithe::benchmark::benchmark_options{
            .warmup_runs = 0,
            .measured_runs = 2,
            .scope = lithe::benchmark::benchmark_scope::direct_entry,
        });

    REQUIRE(measurement.equivalent);
    REQUIRE(measurement.scope == lithe::benchmark::benchmark_scope::direct_entry);
    REQUIRE(measurement.phases.compile_ns == measurement.cold_compile_ns);
    REQUIRE(measurement.phases.warm_execution_ns.size() == measurement.warm_execution_ns.size());
}

