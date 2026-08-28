#pragma once

// Typed Crank f32 tensor values and automatic execution entry for a lowered
// binary HL-MIR region. This is intentionally separate from scalar engine.hpp.

#include "languages/crank/gpu_pipeline.hpp"

#include <concepts>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace crank {
    class f32_tensor {
    public:
        f32_tensor() = default;
        explicit f32_tensor(std::vector<float> values) noexcept : values_(std::move(values)) {}
        explicit f32_tensor(const std::size_t size) : values_(size) {}

        [[nodiscard]] std::span<float> values() noexcept { return values_; }
        [[nodiscard]] std::span<const float> values() const noexcept { return values_; }
        [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
        [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

    private:
        std::vector<float> values_;
    };

    struct tensor_execution_report {
        bool used_gpu = false;
        bool fallback_fired = false;
        gpu_metal_execution_summary gpu{};
    };

    // HostFallback has the same observable semantics as the lowered binary
    // region. It is statically bound, so scalar/SIMD fallback has no erased
    // callback boundary. The fallback is invoked only before a GPU submission.
    template <typename HostFallback,
              class Observer = ::lang::telemetry::phase_observer<>>
        requires std::invocable<HostFallback&, std::span<const float>,
                                std::span<const float>, std::span<float>>
    [[nodiscard]] std::expected<tensor_execution_report, gpu_dispatch_result>
    execute_f32_binary(lithe::codegen::hl::hl_mir_function& function,
                       const f32_tensor& lhs,
                       const f32_tensor& rhs,
                       f32_tensor& output,
                       HostFallback&& fallback,
                       const lithe::exec::auto_execution_policy& policy = {},
                       const std::uint64_t unit_id = 0) {
        if (lhs.empty() || rhs.empty() || lhs.size() != rhs.size() || output.size() != lhs.size())
            return std::unexpected(gpu_dispatch_result{
                gpu_dispatch_status::unsupported_shape,
                "crank tensor: binary f32 operands and output must share a non-empty extent"});

        crank_gpu_pipeline pipeline;
        const auto region = pipeline.add_binary_region({
            .function = std::addressof(function),
            .inputs = {
                gpu_metal_input_binding::from_host(lhs.values()),
                gpu_metal_input_binding::from_host(rhs.values()),
            },
            .output = {.values = output.values()},
        });
        if (region) {
            auto dispatched = pipeline.template execute_observed<Observer>(policy, unit_id);
            if (dispatched)
                return tensor_execution_report{.used_gpu = true, .gpu = std::move(*dispatched)};
            if (policy.fallback == lithe::exec::fallback_policy::none
                || policy.device_residency == lithe::exec::device_residency_policy::require_device)
                return std::unexpected(dispatched.error());
        }
        else if (policy.fallback == lithe::exec::fallback_policy::none
                 || policy.device_residency == lithe::exec::device_residency_policy::require_device) {
            return std::unexpected(gpu_dispatch_result{
                gpu_dispatch_status::unsupported_shape, region.error()});
        }

        std::invoke(fallback, lhs.values(), rhs.values(), output.values());
        return tensor_execution_report{.fallback_fired = true};
    }
} // namespace crank
