#pragma once

// Crank's typed bridge from lowered tensor regions to Lithe's device plans.
// Host storage remains caller-owned; this header owns only the short-lived graph
// and plan descriptors required for one automatic Metal execution.

#include "languages/crank/gpu_metal_execution.hpp"
#include "languages/crank/gpu_vulkan_execution.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace crank {
    struct crank_f32_tensor_view {
        std::span<float> values;

        [[nodiscard]] std::span<const float> read_only() const noexcept { return values; }
        [[nodiscard]] std::size_t size() const noexcept { return values.size(); }
    };

    struct crank_gpu_binary_region {
        lithe::codegen::hl::hl_mir_function* function = nullptr;
        std::array<gpu_metal_input_binding, 2> inputs;
        crank_f32_tensor_view output;
    };

    // Owns Crank's graph/dataflow description. Lithe owns no Crank tensor
    // storage, and Metal tensors exist only during execute_observed().
    class crank_gpu_pipeline {
    public:
        [[nodiscard]] std::expected<litegraph::NodeId, std::string>
        add_binary_region(crank_gpu_binary_region region) {
            if (!region.function)
                return std::unexpected("crank gpu: binary region has no lowered HL-MIR function");
            const auto count = infer_count(region);
            if (!count)
                return std::unexpected("crank gpu: binary region has no statically sized f32 domain");
            for (const auto& input : region.inputs) {
                if (!input.producer) continue;
                const auto found = std::ranges::find(regions_, *input.producer, &stored_region::node);
                if (found == regions_.end())
                    return std::unexpected("crank gpu: input producer is not part of this pipeline");
                if (found->count != *count)
                    return std::unexpected("crank gpu: producer extent does not match this binary region");
            }

            gpu_region memory_region;
            memory_region.buffers.reserve(3);
            for (const auto& input : region.inputs) {
                device_buffer buffer;
                buffer.element_type = "f32";
                buffer.count = *count;
                buffer.byte_size = *count * sizeof(float);
                buffer.access = buffer_access::read;
                buffer.residency = input.producer ? residency_state::device_current
                                                  : residency_state::host_current;
                memory_region.buffers.push_back(std::move(buffer));
            }
            device_buffer output;
            output.element_type = "f32";
            output.count = *count;
            output.byte_size = *count * sizeof(float);
            output.access = buffer_access::write;
            output.residency = residency_state::invalid;
            memory_region.buffers.push_back(std::move(output));
            memory_region.host_observes_output = !region.output.values.empty();

            const auto node = graph_.add_region(std::move(memory_region));
            for (const auto& input : region.inputs) {
                if (input.producer && !graph_.add_device_dependency(*input.producer, node))
                    return std::unexpected("crank gpu: failed to add producer dependency");
            }
            regions_.push_back({node, std::move(region), *count});
            return node;
        }

        template <class Observer = ::lang::telemetry::phase_observer<>>
        [[nodiscard]] std::expected<gpu_metal_execution_summary, gpu_dispatch_result>
        execute_observed(const lithe::exec::auto_execution_policy& policy = {},
                         const std::uint64_t unit_id = 0,
                         const gpu_provider provider = gpu_backend::preferred_provider()) {
            const auto resident_bytes = estimated_device_bytes();
            if (resident_bytes > policy.max_device_cache_bytes)
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::resource_exhausted,
                    "crank gpu: device-resident chain requires " + std::to_string(resident_bytes)
                        + " bytes, exceeding the configured "
                        + std::to_string(policy.max_device_cache_bytes) + " byte budget"});
            std::vector<lithe::codegen::device::kernel_plan> plans;
            plans.reserve(regions_.size());
            for (auto& region : regions_) {
                // Fusion changes HL-MIR before plan extraction, so a legal fused
                // region emits one MSL artifact rather than merely being marked.
                const auto fusion = fuse_parallel_hl_regions(*region.region.function, policy);
                if (!fusion)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape, fusion.error()});
                auto plan = lithe::codegen::device::analyze_kernel(*region.region.function);
                if (!plan.valid())
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape,
                        plan.diagnostics.empty() ? "crank gpu: device-plan extraction failed"
                                                 : plan.diagnostics.front()});
                plans.push_back(std::move(plan));
            }

            std::vector<gpu_metal_graph_binding> bindings;
            bindings.reserve(regions_.size());
            for (std::size_t i = 0; i < regions_.size(); ++i) {
                const auto& region = regions_[i];
                bindings.push_back({
                    .node = region.node,
                    .region = {
                        .plan = std::addressof(plans[i]),
                        .inputs = region.region.inputs,
                        .host_output = region.region.output.values,
                    },
                });
            }
            if (provider == gpu_provider::metal)
                return gpu_metal_executor{}.template execute_observed<Observer>(
                    graph_, bindings, policy, unit_id);
#if defined(LITHE_VULKAN_BACKEND_AVAILABLE) && LITHE_VULKAN_BACKEND_AVAILABLE
            if (provider == gpu_provider::vulkan)
                return gpu_vulkan_executor{}.execute(graph_, bindings, policy);
#endif
            return std::unexpected(gpu_dispatch_result{
                gpu_dispatch_status::no_device, "gpu: no selected device provider"});
        }

        [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }

        // Conservative peak: the executor retains each distinct host input and
        // each graph output through submission completion. A rejected chain is
        // therefore safe to send to the caller's scalar/SIMD fallback before
        // any visible device write occurs.
        [[nodiscard]] std::uint64_t estimated_device_bytes() const {
            std::uint64_t bytes = 0;
            std::vector<std::pair<const float*, std::size_t>> host_inputs;
            host_inputs.reserve(regions_.size() * 2);
            for (const auto& entry : regions_) {
                for (const auto& input : entry.region.inputs) {
                    if (input.producer || input.host.empty()) continue;
                    const auto found = std::ranges::find(host_inputs, input.host.data(),
                                                         &std::pair<const float*, std::size_t>::first);
                    if (found != host_inputs.end() && found->second == input.host.size()) continue;
                    host_inputs.emplace_back(input.host.data(), input.host.size());
                    bytes += input.host.size() * sizeof(float);
                }
                bytes += entry.count * sizeof(float);
            }
            return bytes;
        }

    private:
        struct stored_region {
            litegraph::NodeId node;
            crank_gpu_binary_region region;
            std::size_t count = 0;
        };

        [[nodiscard]] std::optional<std::size_t>
        infer_count(const crank_gpu_binary_region& region) const noexcept {
            for (const auto& input : region.inputs) {
                if (!input.producer && !input.host.empty()) return input.host.size();
                if (input.producer) {
                    const auto found = std::ranges::find(regions_, *input.producer, &stored_region::node);
                    if (found != regions_.end()) return found->count;
                }
            }
            if (!region.output.values.empty()) return region.output.values.size();
            return std::nullopt;
        }

        gpu_execution_graph graph_;
        std::vector<stored_region> regions_;
    };
} // namespace crank
