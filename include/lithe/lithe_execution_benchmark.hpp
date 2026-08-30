#pragma once

// Opt-in benchmark fixture for equivalent provider workloads. Normal execution
// neither includes nor pays for this utility.

#include "lithe_feedback.hpp"
#include "lithe_codegen_hl_passes.hpp"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lithe::benchmark {
    enum class benchmark_scope : std::uint8_t {
        full_path,
        direct_entry,
    };

    struct phase_timing_record {
        std::uint64_t compile_ns = 0;
        std::uint64_t install_ns = 0;
        std::uint64_t transfer_ns = 0;
        std::uint64_t allocation_ns = 0;
        std::vector<std::uint64_t> warm_execution_ns;
    };

    struct benchmark_options {
        std::uint32_t warmup_runs = 1;
        std::uint32_t measured_runs = 9;
        benchmark_scope scope = benchmark_scope::full_path;
    };

    struct provider_measurement {
        std::uint64_t cold_compile_ns = 0;
        std::vector<std::uint64_t> warm_execution_ns;
        bool equivalent = false;
        benchmark_scope scope = benchmark_scope::full_path;
        phase_timing_record phases{};

        [[nodiscard]] std::uint64_t median_warm_execution_ns() const noexcept {
            if (warm_execution_ns.empty()) return 0;
            auto samples = warm_execution_ns;
            std::ranges::sort(samples);
            return samples[samples.size() / 2];
        }
    };

    template <class Compile, class Execute, class Equivalent>
    [[nodiscard]] provider_measurement measure_provider(
        Compile&& compile, Execute&& execute, Equivalent&& equivalent,
        const benchmark_options options = {}) {
        provider_measurement out;
        const auto compile_start = std::chrono::steady_clock::now();
        auto artifact = std::invoke(std::forward<Compile>(compile));
        const auto compile_end = std::chrono::steady_clock::now();
        out.cold_compile_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(compile_end - compile_start).count());
        out.scope = options.scope;
        out.phases.compile_ns = out.cold_compile_ns;

        for (std::uint32_t run = 0; run < options.warmup_runs; ++run)
            static_cast<void>(std::invoke(execute, artifact));

        out.warm_execution_ns.reserve(options.measured_runs);
        for (std::uint32_t run = 0; run < options.measured_runs; ++run) {
            const auto start = std::chrono::steady_clock::now();
            auto result = std::invoke(execute, artifact);
            const auto end = std::chrono::steady_clock::now();
            out.warm_execution_ns.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
            out.phases.warm_execution_ns = out.warm_execution_ns;
            out.equivalent = std::invoke(equivalent, result);
            if (!out.equivalent) break;
        }
        return out;
    }

    struct feedback_record_options {
        std::size_t expression_hash = 0;
        feedback::hardware_signature hardware = feedback::hardware_signature::current();
        std::string backend_id;
        std::uint64_t work_items = 0;
        double memory_mb = 0.0;
        double power_w = 0.0;
    };

    struct candidate_calibration_options {
        std::uint64_t work_items = 0;
    };

    // Produces an explicit candidate value from an equivalent measurement. It
    // deliberately does not consult or mutate feedback storage: callers choose
    // whether the returned calibration participates in a later selection.
    [[nodiscard]] inline std::optional<codegen::hl::execution_candidate_cost>
    calibrate_candidate_cost(
        const codegen::hl::execution_candidate_cost& baseline,
        const provider_measurement& measurement,
        const candidate_calibration_options options) {
        const auto median_ns = measurement.median_warm_execution_ns();
        if (!measurement.equivalent || options.work_items == 0 || median_ns == 0)
            return std::nullopt;

        auto calibrated = baseline;
        calibrated.setup_cost_ns = measurement.cold_compile_ns;
        calibrated.cached_setup_cost_ns = 0;
        calibrated.cached_setup_cost_known = true;
        calibrated.work_item_cost_ns = median_ns / options.work_items
            + static_cast<std::uint64_t>(median_ns % options.work_items != 0);
        return calibrated;
    }

    [[nodiscard]] inline bool record_measurement(
        feedback::feedback_store& store, const provider_measurement& measurement,
        const feedback_record_options& options) {
        const auto median_ns = measurement.median_warm_execution_ns();
        if (!measurement.equivalent || median_ns == 0 || options.backend_id.empty()) return false;
        feedback::performance_sample sample;
        sample.expression_hash = options.expression_hash;
        sample.hw = options.hardware;
        sample.backend_id = options.backend_id;
        sample.latency_ms = static_cast<double>(median_ns) / 1'000'000.0;
        sample.throughput_gops = options.work_items == 0 ? 0.0
            : static_cast<double>(options.work_items) / static_cast<double>(median_ns);
        sample.memory_mb = options.memory_mb;
        sample.power_w = options.power_w;
        store.record(std::move(sample));
        return true;
    }
} // namespace lithe::benchmark
