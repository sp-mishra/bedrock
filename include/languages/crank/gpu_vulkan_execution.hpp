#pragma once

// Vulkan/MoltenVK execution adapter for the same Crank graph bindings used by
// Metal. Pravaha owns the typed Vulkan data plane; Lithe continues to own the
// SPIR-V pipeline, descriptors, and fence lifecycle.

#include "languages/crank/gpu_metal_execution.hpp"

#if defined(LITHE_VULKAN_BACKEND_AVAILABLE) && LITHE_VULKAN_BACKEND_AVAILABLE
#include "pravaha/backends/vulkan_gpu.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace crank {
    class gpu_vulkan_executor {
    public:
        [[nodiscard]] std::expected<gpu_metal_execution_summary, gpu_dispatch_result>
        execute(const gpu_execution_graph& graph,
                const std::span<const gpu_metal_graph_binding> bindings,
                const lithe::exec::auto_execution_policy& policy = {}) const {
            if (!gpu_backend::vulkan_available())
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::no_device, "gpu: Vulkan/MoltenVK is unavailable"});

            auto schedule = graph.schedule(policy, /*device_available=*/true);
            if (!schedule)
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::unsupported_shape, schedule.error()});
            std::size_t max_node = 0;
            for (const auto& item : schedule->items) max_node = std::max(max_node, item.node.value);
            using device_tensor = ::pravaha::backends::vulkan::vulkan_device_tensor<float>;
            std::vector<std::optional<device_tensor>> outputs(max_node + 1);
            std::vector<std::pair<const float*, device_tensor>> host_inputs;
            host_inputs.reserve(bindings.size() * 2);
            gpu_metal_execution_summary result{.schedule = std::move(*schedule)};

            for (const auto& item : result.schedule.items) {
                const auto* binding = find_binding(bindings, item.node);
                if (!binding || !binding->region.plan)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape, "gpu: graph node has no Vulkan binding"});
                const auto operation = classify_operation(*binding->region.plan);
                if (!operation)
                    return std::unexpected(operation.error());
                auto module = gpu_backend{}.compile_elementwise(*binding->region.plan, *operation);
                if (module.validate() != lithe::ir::ir_resolution_state::resolved)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape, "gpu: SPIR-V validation failed"});

                std::array<const device_tensor*, 2> inputs{};
                for (std::size_t index = 0; index < inputs.size(); ++index) {
                    const auto& source = binding->region.inputs[index];
                    if (!source.producer) {
                        const auto cached = std::ranges::find(host_inputs, source.host.data(),
                                                             &std::pair<const float*, device_tensor>::first);
                        if (cached != host_inputs.end()) {
                            inputs[index] = std::addressof(cached->second);
                            continue;
                        }
                        auto uploaded = device_tensor::from_host(source.host);
                        if (!uploaded)
                            return std::unexpected(gpu_dispatch_result{
                                gpu_dispatch_status::no_device, "gpu: Vulkan host upload failed"});
                        host_inputs.emplace_back(source.host.data(), std::move(*uploaded));
                        inputs[index] = std::addressof(host_inputs.back().second);
                        ++result.uploads;
                        continue;
                    }
                    if (!graph.has_device_dependency(*source.producer, item.node)
                        || source.producer->value >= outputs.size()
                        || !outputs[source.producer->value])
                        return std::unexpected(gpu_dispatch_result{
                            gpu_dispatch_status::unsupported_shape,
                            "gpu: Vulkan graph producer is unavailable at this schedule point"});
                    inputs[index] = std::addressof(*outputs[source.producer->value]);
                }
                if (!inputs[0] || !inputs[1] || inputs[0]->size() != inputs[1]->size())
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape,
                        "gpu: Vulkan binary inputs must be non-empty and equally sized"});

                auto output = device_tensor::allocate(inputs[0]->size());
                if (!output)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::no_device, "gpu: Vulkan device output allocation failed"});
                const auto dispatched = ::pravaha::backends::vulkan::dispatch_elementwise_device<float, 2>(
                    module.identity_hash(), module, *output, inputs, module.local_x);
                if (!dispatched)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::no_device, dispatched.error().message});
                outputs[item.node.value] = std::move(*output);
                ++result.submissions;
            }

            for (const auto& item : result.schedule.items) {
                const auto* binding = find_binding(bindings, item.node);
                if (binding->region.host_output.empty()) continue;
                const auto& output = *outputs[item.node.value];
                if (binding->region.host_output.size() != output.size())
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape,
                        "gpu: Vulkan host output extent does not match the graph result"});
                if (!output.download(binding->region.host_output))
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::no_device, "gpu: Vulkan host download failed"});
                ++result.downloads;
            }
            return result;
        }

    private:
        [[nodiscard]] static const gpu_metal_graph_binding*
        find_binding(const std::span<const gpu_metal_graph_binding> bindings,
                     const litegraph::NodeId node) noexcept {
            const auto found = std::ranges::find(bindings, node, &gpu_metal_graph_binding::node);
            return found == bindings.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] static std::expected<gpu_elementwise_op, gpu_dispatch_result>
        classify_operation(const lithe::codegen::device::kernel_plan& plan) {
            bool add = false;
            bool multiply = false;
            for (const auto* operation : plan.operations) {
                add = add || operation->op == lithe::codegen::hl::hl_opcode::fadd;
                multiply = multiply || operation->op == lithe::codegen::hl::hl_opcode::fmul;
            }
            if (add == multiply)
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::unsupported_shape,
                    "gpu: Vulkan adapter currently requires exactly one fadd or fmul operation"});
            return add ? gpu_elementwise_op::add : gpu_elementwise_op::mul;
        }
    };
} // namespace crank
#endif
