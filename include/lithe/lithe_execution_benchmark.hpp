#pragma once

// Opt-in benchmark fixture for equivalent provider workloads. Normal execution
// neither includes nor pays for this utility.

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace lithe::benchmark {
    struct benchmark_options {
        std::uint32_t warmup_runs = 1;
        std::uint32_t measured_runs = 9;
    };

    struct provider_measurement {
        std::uint64_t cold_compile_ns = 0;
        std::vector<std::uint64_t> warm_execution_ns;
        bool equivalent = false;

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

        for (std::uint32_t run = 0; run < options.warmup_runs; ++run)
            static_cast<void>(std::invoke(execute, artifact));

        out.warm_execution_ns.reserve(options.measured_runs);
        for (std::uint32_t run = 0; run < options.measured_runs; ++run) {
            const auto start = std::chrono::steady_clock::now();
            auto result = std::invoke(execute, artifact);
            const auto end = std::chrono::steady_clock::now();
            out.warm_execution_ns.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
            out.equivalent = std::invoke(equivalent, result);
            if (!out.equivalent) break;
        }
        return out;
    }
} // namespace lithe::benchmark
