#pragma once

// GPU-region scheduling over Pebble LiteGraph.  This is a planning bridge: it
// derives internal device-consumer boundaries before the Metal data plane is
// entered, so device residency remains automatic and explicit policy controls
// remain value-only configuration.

#include "languages/crank/gpu_memory.hpp"
#include "lithe/lithe_codegen_hl_passes.hpp"

#include "containers/graph/LiteGraph.hpp"

#include <concepts>
#include <expected>
#include <functional>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>

namespace crank {
    struct gpu_execution_schedule_item {
        litegraph::NodeId node;
        gpu_region region;
        device_execution_decision decision;
        transfer_plan transfers;
    };

    struct gpu_execution_schedule {
        std::vector<gpu_execution_schedule_item> items;
        std::size_t retained_outputs = 0;
        std::size_t fusible_links = 0;
    };

    struct gpu_fusion_report {
        std::size_t fused_regions = 0;
        std::vector<std::string> diagnostics;
    };

    // Fuse adjacent compatible parallel regions before device-plan extraction.
    // The existing Lithe pass owns the structural mutation and rollback rules;
    // this adapter merely applies the user's runtime fusion policy.
    [[nodiscard]] inline std::expected<gpu_fusion_report, std::string>
    fuse_parallel_hl_regions(lithe::codegen::hl::hl_mir_function& function,
                             const lithe::exec::auto_execution_policy& policy = {}) {
        gpu_fusion_report report;
        if (policy.device_fusion == lithe::exec::device_fusion_policy::disabled) return report;
        lithe::codegen::hl::region_fusion_pass fusion;
        for (auto* block = function.body_region.blocks.head; block != nullptr;
             block = block->list_node.next) {
            for (auto* first = block->ops.head; first != nullptr;) {
                auto* second = first->list_node.next;
                if (!second) break;
                const auto parallel_for = [](const auto* op) {
                    return op->op == lithe::codegen::hl::hl_opcode::structured_for
                        && std::holds_alternative<lithe::codegen::hl::structured_for_attr>(op->attr)
                        && std::get<lithe::codegen::hl::structured_for_attr>(op->attr).is_parallel;
                };
                if (!parallel_for(first) || !parallel_for(second)) {
                    first = second;
                    continue;
                }
                const auto result = fusion.run(function, *block, first, second);
                if (result.fused) {
                    ++report.fused_regions;
                    continue; // the fused first region may fuse with its new successor
                }
                if (policy.device_fusion == lithe::exec::device_fusion_policy::require)
                    return std::unexpected("required GPU fusion failed: " + result.diagnostic);
                if (!result.diagnostic.empty()) report.diagnostics.push_back(result.diagnostic);
                first = second;
            }
        }
        return report;
    }

    class gpu_execution_graph {
    public:
        [[nodiscard]] litegraph::NodeId add_region(gpu_region region) {
            const auto index = static_cast<std::uint32_t>(regions_.size());
            regions_.push_back(std::move(region));
            return graph_.add_node(index);
        }

        [[nodiscard]] bool add_device_dependency(const litegraph::NodeId producer,
                                                 const litegraph::NodeId consumer) {
            if (!graph_.valid_node(producer) || !graph_.valid_node(consumer) || producer == consumer)
                return false;
            return graph_.add_edge(producer, consumer, 0u).is_valid();
        }

        [[nodiscard]] std::expected<gpu_execution_schedule, std::string>
        schedule(const lithe::exec::auto_execution_policy& policy,
                 const bool device_available) const {
            auto regions = regions_;
            std::vector<std::size_t> indegree(graph_.node_capacity(), 0);
            std::vector<litegraph::NodeId> ready;
            for (const auto id : graph_.active_node_ids()) {
                indegree[id.value] = static_cast<std::size_t>(std::ranges::distance(graph_.in_edges(id)));
                if (indegree[id.value] == 0) ready.push_back(id);
            }

            // A device-dependency edge is an explicit promise that the producer
            // result feeds a compatible device consumer.  The last graph node
            // remains host-visible unless the frontend says otherwise.
            for (const auto id : graph_.active_node_ids()) {
                const auto index = graph_.node_data(id);
                bool has_device_consumer = false;
                graph_.for_each_out_edge(id, [&](const litegraph::EdgeId,
                                                  const litegraph::NodeId,
                                                  const litegraph::NodeId,
                                                  const std::uint32_t&) {
                    has_device_consumer = true;
                });
                if (has_device_consumer) {
                    regions[index].output_consumed_on_device = true;
                    regions[index].host_observes_output = false;
                }
            }

            gpu_execution_schedule result;
            for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
                const auto id = ready[cursor];
                const auto index = graph_.node_data(id);
                auto& region = regions[index];
                const auto decision = decide_device_execution(region, policy, device_available);
                if (decision.retain_outputs) ++result.retained_outputs;
                auto transfers = plan_transfers(region, policy);

                graph_.for_each_out_edge(id, [&](const litegraph::EdgeId,
                                                  const litegraph::NodeId,
                                                  const litegraph::NodeId target,
                                                  const std::uint32_t&) {
                    if (decision.fuse_with_successor) ++result.fusible_links;
                    if (--indegree[target.value] == 0) ready.push_back(target);
                });
                result.items.push_back({id, std::move(region), decision, std::move(transfers)});
            }

            if (result.items.size() != graph_.node_count())
                return std::unexpected("gpu execution graph contains a dependency cycle");
            return result;
        }

        // The resolver is a statically-bound data-plane adapter. It receives
        // each scheduled region in dependency order. It owns typed bindings
        // and any asynchronous completion tokens, so this generic scheduler
        // neither erases their types nor imposes a tensor abstraction.
        template <typename Resolver>
            requires std::same_as<
                std::remove_cvref_t<std::invoke_result_t<Resolver&, const gpu_execution_schedule_item&>>,
                std::expected<void, std::string>>
        [[nodiscard]] std::expected<gpu_execution_schedule, std::string>
        execute(const lithe::exec::auto_execution_policy& policy,
                const bool device_available,
                Resolver&& resolver) const {
            auto planned = schedule(policy, device_available);
            if (!planned) return std::unexpected(planned.error());
            for (const auto& item : planned->items) {
                auto dispatched = std::invoke(resolver, item);
                if (!dispatched) return std::unexpected(dispatched.error());
            }
            return std::move(*planned);
        }

    private:
        litegraph::Graph<std::uint32_t, std::uint32_t, litegraph::Directed> graph_;
        std::vector<gpu_region> regions_;
    };
} // namespace crank
