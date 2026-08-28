#pragma once

// Typed Metal data-plane executor for a Pebble LiteGraph GPU schedule.
// This remains separate from gpu_execution_graph.hpp: graph planning stays
// portable while this opt-in adapter owns Metal tensors and submissions.

#include "languages/crank/gpu_backend.hpp"
#include "languages/crank/gpu_execution_graph.hpp"
#include "languages/generic/observability/phase.hpp"

#include <algorithm>
#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace crank {
    struct gpu_metal_input_binding {
        std::span<const float> host;
        std::optional<litegraph::NodeId> producer;

        [[nodiscard]] static gpu_metal_input_binding from_host(const std::span<const float> values) noexcept {
            return {.host = values};
        }

        [[nodiscard]] static gpu_metal_input_binding
        from_region(const litegraph::NodeId node) noexcept {
            return {.producer = node};
        }
    };

    // The plan and host spans are borrowed only for execute(). Device tensors
    // are created internally and never cross Lithe's portable artifact boundary.
    struct gpu_metal_region_binding {
        const lithe::codegen::device::kernel_plan* plan = nullptr;
        std::array<gpu_metal_input_binding, 2> inputs;
        std::span<float> host_output;
    };

    struct gpu_metal_graph_binding {
        litegraph::NodeId node;
        gpu_metal_region_binding region;
    };

    struct gpu_metal_execution_summary {
        gpu_execution_schedule schedule;
        std::size_t uploads = 0;
        std::size_t downloads = 0;
        std::size_t submissions = 0;
    };

    class gpu_metal_executor {
    public:
        [[nodiscard]] std::expected<gpu_metal_execution_summary, gpu_dispatch_result>
        execute(const gpu_execution_graph& graph,
                const std::span<const gpu_metal_graph_binding> bindings,
                const lithe::exec::auto_execution_policy& policy = {}) const {
            if (!gpu_backend::metal_available())
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::no_device, "gpu: native Metal is unavailable"});

            auto schedule = graph.schedule(policy, /*device_available=*/true);
            if (!schedule)
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::unsupported_shape, schedule.error()});

            std::size_t max_node = 0;
            for (const auto& item : schedule->items) max_node = std::max(max_node, item.node.value);
            std::vector<std::optional<gpu_f32_tensor>> outputs(max_node + 1);
            std::vector<gpu_device_submission> submissions;
            submissions.reserve(schedule->items.size());
            std::vector<host_tensor> host_inputs;
            host_inputs.reserve(bindings.size() * 2);

            gpu_metal_execution_summary result{.schedule = std::move(*schedule)};
            for (const auto& item : result.schedule.items) {
                const auto* binding = find_binding(bindings, item.node);
                if (!binding || !binding->region.plan)
                    return missing_binding(item.node);
                if (!gpu_backend::supports(*binding->region.plan))
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape,
                        "gpu: graph region is outside the shared f32 binary contract"});

                std::array<const gpu_f32_tensor*, 2> inputs{};
                for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
                    const auto& source = binding->region.inputs[input_index];
                    if (source.producer) {
                        if (!graph.has_device_dependency(*source.producer, item.node))
                            return std::unexpected(gpu_dispatch_result{
                                gpu_dispatch_status::unsupported_shape,
                                "gpu: a device input is missing its graph dependency"});
                        const auto producer = source.producer->value;
                        if (producer >= outputs.size() || !outputs[producer])
                            return std::unexpected(gpu_dispatch_result{
                                gpu_dispatch_status::unsupported_shape,
                                "gpu: producer output is unavailable at this schedule point"});
                        inputs[input_index] = std::addressof(*outputs[producer]);
                    } else {
                        auto uploaded = find_or_upload(host_inputs, source.host);
                        if (!uploaded) return std::unexpected(uploaded.error());
                        if (uploaded->uploaded_now) ++result.uploads;
                        inputs[input_index] = uploaded->tensor;
                    }
                }

                const auto element_count = inputs.front() ? inputs.front()->size() : 0;
                if (element_count == 0 || inputs[1] == nullptr || inputs[1]->size() != element_count)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape,
                        "gpu: graph region requires two equally sized non-empty f32 inputs"});
                if (!binding->region.host_output.empty()
                    && binding->region.host_output.size() != element_count)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape,
                        "gpu: graph host output does not match its input extent"});
                if (item.region.host_observes_output && binding->region.host_output.empty())
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::unsupported_shape,
                        "gpu: a host-observed graph output requires a host destination"});

                auto output = gpu_f32_tensor::allocate(element_count);
                if (!output)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::no_device, output.error().message});
                const gpu_backend backend;
                auto submitted = backend.dispatch_metal_device_async(
                    *binding->region.plan, *output, inputs);
                if (!submitted) return std::unexpected(submitted.error());
                outputs[item.node.value] = std::move(*output);
                submissions.push_back(std::move(*submitted));
                ++result.submissions;
            }

            // A Metal command queue preserves submission order. Waiting after
            // all commands are encoded maximizes overlap while keeping every
            // pipeline and command-buffer lifetime token alive.
            for (const auto& submission : submissions) {
                if (const auto completed = submission.wait(); !completed)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::no_device, completed.error().message});
            }

            for (const auto& item : result.schedule.items) {
                const auto* binding = find_binding(bindings, item.node);
                if (binding->region.host_output.empty()) continue;
                if (const auto copied = outputs[item.node.value]->download(binding->region.host_output); !copied)
                    return std::unexpected(gpu_dispatch_result{
                        gpu_dispatch_status::no_device, copied.error().message});
                ++result.downloads;
            }
            return result;
        }

        // Reuses Pebble's policy-selected language telemetry. With the default
        // observer this is an empty scope: no clock, NADI pulse, or feedback
        // call is generated for consumers that do not opt in.
        template <class Observer = ::lang::telemetry::phase_observer<>>
        [[nodiscard]] std::expected<gpu_metal_execution_summary, gpu_dispatch_result>
        execute_observed(const gpu_execution_graph& graph,
                         const std::span<const gpu_metal_graph_binding> bindings,
                         const lithe::exec::auto_execution_policy& policy = {},
                         const std::uint64_t unit_id = 0) const {
            ::lang::telemetry::phase_scope<Observer> scope{{
                .unit_id = unit_id,
                .stage = ::lang::telemetry::phase::execute,
                .entity_count = static_cast<std::uint32_t>(bindings.size()),
            }};
            auto result = execute(graph, bindings, policy);
            scope.set_transformations(result ? static_cast<std::uint32_t>(result->submissions) : 0);
            scope.set_outcome(result ? ::lang::telemetry::phase_outcome::success
                                     : ::lang::telemetry::phase_outcome::fallback);
            return result;
        }

    private:
        struct host_tensor {
            const float* data = nullptr;
            std::size_t size = 0;
            gpu_f32_tensor tensor;
        };

        struct resolved_host_tensor {
            const gpu_f32_tensor* tensor = nullptr;
            bool uploaded_now = false;
        };

        [[nodiscard]] static const gpu_metal_graph_binding*
        find_binding(const std::span<const gpu_metal_graph_binding> bindings,
                     const litegraph::NodeId node) noexcept {
            const auto found = std::ranges::find(bindings, node, &gpu_metal_graph_binding::node);
            return found == bindings.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] static std::expected<resolved_host_tensor, gpu_dispatch_result>
        find_or_upload(std::vector<host_tensor>& cache, const std::span<const float> host) {
            if (host.empty())
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::unsupported_shape, "gpu: graph host input is empty"});
            const auto found = std::ranges::find_if(cache, [&](const host_tensor& entry) {
                return entry.data == host.data() && entry.size == host.size();
            });
            if (found != cache.end()) return resolved_host_tensor{std::addressof(found->tensor), false};
            auto tensor = gpu_f32_tensor::from_host(host);
            if (!tensor)
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::no_device, tensor.error().message});
            cache.push_back({host.data(), host.size(), std::move(*tensor)});
            return resolved_host_tensor{std::addressof(cache.back().tensor), true};
        }

        [[nodiscard]] static std::unexpected<gpu_dispatch_result>
        missing_binding(const litegraph::NodeId node) {
            return std::unexpected(gpu_dispatch_result{
                gpu_dispatch_status::unsupported_shape,
                "gpu: graph node " + std::to_string(node.value) + " has no Metal binding"});
        }
    };
} // namespace crank
