// =============================================================================
// test_crank_gpu_memory.cpp — device residency / transfers / sync gate (design §11).
//
// Covers:
//   1. plan_transfers uploads a device-read host-current input.
//   2. plan_transfers downloads a device-written output + marks visible writes.
//   3. Unified buffers need no transfer.
//   4. synchronize refuses fallback after visible device writes (allow_replay=false).
//   5. synchronize succeeds when replay is allowed / no visible writes.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/gpu_memory.hpp"
#include "languages/crank/gpu_execution_graph.hpp"
#include "languages/crank/gpu_metal_execution.hpp"
#include "languages/crank/gpu_pipeline.hpp"
#include "languages/crank/tensor_runtime.hpp"

#include <array>
#include <concepts>
#include <ranges>
#include <utility>
#include <vector>

using namespace crank;

namespace {
    device_buffer buf(address_space space, buffer_access access, residency_state res) {
        device_buffer b;
        b.space = space;
        b.access = access;
        b.residency = res;
        return b;
    }

    lithe::codegen::hl::hl_mir_function make_f32_add_kernel(const std::size_t count) {
        namespace hl = lithe::codegen::hl;
        using lithe::codegen::abstract_value_kind;
        using lithe::codegen::ssa_value_id;

        hl::hl_mir_function kernel{1u << 16};
        const auto set_operands = [&kernel](auto* op, std::initializer_list<ssa_value_id> values) {
            auto span = kernel.template alloc_span<ssa_value_id>(values.size());
            std::ranges::copy(values, span.begin());
            op->operands = span;
        };
        const auto set_results = [&kernel](auto* op, std::initializer_list<ssa_value_id> values) {
            auto span = kernel.template alloc_span<ssa_value_id>(values.size());
            std::ranges::copy(values, span.begin());
            op->results = span;
        };
        const auto make_view = [count] {
            std::array<std::int64_t, hl::memref_type::max_rank> shape{};
            shape[0] = static_cast<std::int64_t>(count);
            return hl::memref_type::row_major(abstract_value_kind::floating, 32, 1, shape);
        };

        auto* entry = kernel.make_block();
        kernel.body_region.blocks.push_back(entry);
        entry->parent_region = &kernel.body_region;
        auto* loop = kernel.make_op(hl::hl_opcode::structured_for);
        hl::structured_for_attr loop_attr;
        loop_attr.rank = 1;
        loop_attr.is_parallel = true;
        loop_attr.bounds[0] = {0, static_cast<int>(count), 1, true, true, true};
        loop_attr.bounds_known = true;
        loop_attr.trip_count_hint = count;
        loop->attr = loop_attr;
        auto* body = kernel.make_region();
        auto regions = kernel.alloc_span<hl::hl_region*>(1);
        regions[0] = body;
        loop->regions = regions;
        body->parent_op = loop;
        auto* body_block = kernel.make_block();
        body->blocks.push_back(body_block);
        body_block->parent_region = body;

        constexpr ssa_value_id index{1}, lhs_base{10}, rhs_base{11}, out_base{12};
        constexpr ssa_value_id lhs_value{20}, rhs_value{21}, sum_value{22};
        auto* loop_index = kernel.make_op(hl::hl_opcode::loop_index);
        set_results(loop_index, {index});
        body_block->ops.push_back(loop_index);
        const auto add_load = [&](const ssa_value_id base, const ssa_value_id result) {
            auto* load = kernel.make_op(hl::hl_opcode::memref_load);
            load->attr = hl::memref_attr{.view = make_view(), .base_operand_index = 0};
            set_operands(load, {base, index});
            set_results(load, {result});
            body_block->ops.push_back(load);
        };
        add_load(lhs_base, lhs_value);
        add_load(rhs_base, rhs_value);
        auto* add = kernel.make_op(hl::hl_opcode::fadd);
        set_operands(add, {lhs_value, rhs_value});
        set_results(add, {sum_value});
        body_block->ops.push_back(add);
        auto* store = kernel.make_op(hl::hl_opcode::memref_store);
        store->attr = hl::memref_attr{.view = make_view(), .base_operand_index = 0};
        set_operands(store, {out_base, sum_value, index});
        body_block->ops.push_back(store);
        body_block->ops.push_back(kernel.make_op(hl::hl_opcode::region_yield));
        entry->ops.push_back(loop);
        return kernel;
    }
} // namespace

TEST_CASE (

"device-read host-current input is uploaded"
,
"[crank][gpu_memory]"
)
 {
    gpu_region r;
    r.buffers = {buf(address_space::device, buffer_access::read, residency_state::host_current)};
    auto plan = plan_transfers(r);
    REQUIRE(plan.nodes.size() == 1);
    REQUIRE(plan.nodes[0].direction == transfer_direction::upload);
    REQUIRE_FALSE(plan.visible_device_writes);
}

TEST_CASE (

"device-written output is downloaded + marks visible writes"
,
"[crank][gpu_memory]"
)
 {
    gpu_region r;
    r.buffers = {buf(address_space::device, buffer_access::write, residency_state::invalid)};
    auto plan = plan_transfers(r);
    REQUIRE(plan.nodes.size() == 1);
    REQUIRE(plan.nodes[0].direction == transfer_direction::download);
    REQUIRE(plan.visible_device_writes);
}

TEST_CASE (

"unified buffers need no explicit transfer"
,
"[crank][gpu_memory]"
)
 {
    gpu_region r;
    r.buffers = {buf(address_space::unified, buffer_access::read_write, residency_state::synchronized)};
    auto plan = plan_transfers(r);
    REQUIRE(plan.empty());
}

TEST_CASE (

"synchronize refuses replay after visible device writes"
,
"[crank][gpu_memory]"
)
 {
    gpu_region r;
    r.buffers = {buf(address_space::device, buffer_access::write, residency_state::invalid)};
    auto plan = plan_transfers(r);
    REQUIRE(plan.visible_device_writes);
    auto res = synchronize(plan, /*allow_replay=*/false);
    REQUIRE_FALSE(res.completed());
    REQUIRE(res.error->kind == execution_error_kind::unsafe_fallback_after_effects);
}

TEST_CASE (

"synchronize succeeds with replay allowed"
,
"[crank][gpu_memory]"
)
 {
    gpu_region r;
    r.buffers = {
        buf(address_space::device, buffer_access::read, residency_state::host_current),
        buf(address_space::device, buffer_access::write, residency_state::invalid),
    };
    auto plan = plan_transfers(r);
    auto res = synchronize(plan, /*allow_replay=*/true);
    REQUIRE(res.completed());
}

TEST_CASE (

"automatic residency retains an internal device-chain output"
,
"[crank][gpu_memory][residency]"
)
 {
    gpu_region r;
    auto input = buf(address_space::device, buffer_access::read, residency_state::host_current);
    input.byte_size = 64 * 1024;
    auto output = buf(address_space::device, buffer_access::write, residency_state::invalid);
    output.byte_size = 64 * 1024;
    r.buffers = {input, output};
    r.compatible_device_chain_length = 2;
    r.output_consumed_on_device = true;
    r.host_observes_output = false;

    lithe::exec::auto_execution_policy policy;
    const auto decision = decide_device_execution(r, policy, true);
    REQUIRE(decision.use_device);
    REQUIRE(decision.retain_outputs);
    REQUIRE(decision.fuse_with_successor);

    const auto plan = plan_transfers(r, policy);
    REQUIRE(plan.nodes.size() == 1);
    REQUIRE(plan.nodes.front().direction == transfer_direction::upload);
}

TEST_CASE (

"host-only residency policy refuses automatic device execution"
,
"[crank][gpu_memory][residency]"
)
 {
    gpu_region r;
    r.output_consumed_on_device = true;
    r.host_observes_output = false;
    lithe::exec::auto_execution_policy policy;
    policy.device_residency = lithe::exec::device_residency_policy::host_only;
    const auto decision = decide_device_execution(r, policy, true);
    REQUIRE_FALSE(decision.use_device);
    REQUIRE_FALSE(decision.retain_outputs);
}

TEST_CASE (

"GPU execution graph retains an internal producer through Pebble LiteGraph"
,
"[crank][gpu_memory][graph]"
)
 {
    gpu_region producer;
    auto producer_input = buf(address_space::device, buffer_access::read, residency_state::host_current);
    producer_input.byte_size = 64 * 1024;
    auto producer_output = buf(address_space::device, buffer_access::write, residency_state::invalid);
    producer_output.byte_size = 64 * 1024;
    producer.buffers = {producer_input, producer_output};
    producer.compatible_device_chain_length = 2;

    gpu_region consumer;
    auto consumer_input = buf(address_space::device, buffer_access::read, residency_state::device_current);
    consumer_input.byte_size = 64 * 1024;
    auto consumer_output = buf(address_space::device, buffer_access::write, residency_state::invalid);
    consumer_output.byte_size = 64 * 1024;
    consumer.buffers = {consumer_input, consumer_output};

    gpu_execution_graph graph;
    const auto first = graph.add_region(std::move(producer));
    const auto second = graph.add_region(std::move(consumer));
    REQUIRE(graph.add_device_dependency(first, second));

    const auto schedule = graph.schedule({}, true);
    REQUIRE(schedule.has_value());
    REQUIRE(schedule->items.size() == 2);
    REQUIRE(schedule->retained_outputs == 1);
    REQUIRE(schedule->items.front().transfers.nodes.size() == 1);
    REQUIRE(schedule->items.front().transfers.nodes.front().direction == transfer_direction::upload);
}

TEST_CASE (

"GPU execution graph invokes a statically-bound data-plane resolver in dependency order"
,
"[crank][gpu_memory][graph]"
)
 {
    gpu_execution_graph graph;
    const auto producer = graph.add_region({});
    const auto consumer = graph.add_region({});
    REQUIRE(graph.add_device_dependency(producer, consumer));

    std::vector<litegraph::NodeId> visited;
    const auto executed = graph.execute({}, true, [&](const gpu_execution_schedule_item& item)
        -> std::expected<void, std::string> {
        visited.push_back(item.node);
        return {};
    });
    REQUIRE(executed.has_value());
    REQUIRE(visited.size() == 2);
    REQUIRE(visited[0] == producer);
    REQUIRE(visited[1] == consumer);
}

TEST_CASE (

"GPU execution graph derives device-chain eligibility from its Pebble dependencies"
,
"[crank][gpu_memory][graph]"
)
 {
    const auto make_region = [] {
        gpu_region region;
        auto input = buf(address_space::device, buffer_access::read, residency_state::host_current);
        input.byte_size = 64 * 1024;
        auto output = buf(address_space::device, buffer_access::write, residency_state::invalid);
        output.byte_size = 64 * 1024;
        region.buffers = {input, output};
        return region;
    };

    gpu_execution_graph graph;
    const auto producer = graph.add_region(make_region());
    const auto consumer = graph.add_region(make_region());
    REQUIRE(graph.add_device_dependency(producer, consumer));

    const auto schedule = graph.schedule({}, true);
    REQUIRE(schedule.has_value());
    REQUIRE(schedule->items[0].decision.use_device);
    REQUIRE(schedule->items[0].decision.retain_outputs);
    REQUIRE(schedule->items[1].decision.use_device);
}

TEST_CASE (

"unavailable device leaves the transfer plan empty"
,
"[crank][gpu_memory][residency]"
)
 {
    gpu_region region;
    auto input = buf(address_space::device, buffer_access::read, residency_state::host_current);
    input.byte_size = 64 * 1024;
    region.buffers = {input};
    region.compatible_device_chain_length = 2;

    const auto transfers = plan_transfers(region, {}, false);
    REQUIRE(transfers.empty());
}

TEST_CASE (

"Metal graph bindings express host and device dataflow without owning user memory"
,
"[crank][gpu_memory][metal]"
)
 {
    const std::array values{1.0f, 2.0f};
    const auto host_input = gpu_metal_input_binding::from_host(values);
    REQUIRE(host_input.host.data() == values.data());
    REQUIRE(host_input.host.size() == values.size());
    REQUIRE_FALSE(host_input.producer.has_value());

    const litegraph::NodeId producer{7};
    const auto device_input = gpu_metal_input_binding::from_region(producer);
    REQUIRE(device_input.host.empty());
    REQUIRE(device_input.producer == producer);
}

TEST_CASE (

"Metal graph executor retains Pebble's zero-cost default phase observer"
,
"[crank][gpu_memory][metal][telemetry]"
)
 {
    using executor = gpu_metal_executor;
    using observed_result = decltype(std::declval<const executor>().execute_observed(
        std::declval<const gpu_execution_graph&>(),
        std::declval<std::span<const gpu_metal_graph_binding>>()));
    STATIC_REQUIRE(std::same_as<observed_result,
                                std::expected<gpu_metal_execution_summary, gpu_dispatch_result>>);
}

TEST_CASE (

"Crank GPU pipeline is an opt-in typed tensor bridge"
,
"[crank][gpu_memory][metal][pipeline]"
)
 {
    crank_gpu_pipeline pipeline;
    REQUIRE(pipeline.empty());
    const auto invalid = pipeline.add_binary_region({});
    REQUIRE_FALSE(invalid.has_value());
}

TEST_CASE (

"Crank GPU pipeline estimates its retained Metal tensor budget"
,
"[crank][gpu_memory][metal][pipeline]"
)
 {
    lithe::codegen::hl::hl_mir_function function{1024};
    std::vector<float> lhs(16), rhs(16), output(16);
    crank_gpu_pipeline pipeline;
    const auto node = pipeline.add_binary_region({
        .function = std::addressof(function),
        .inputs = {
            gpu_metal_input_binding::from_host(lhs),
            gpu_metal_input_binding::from_host(rhs),
        },
        .output = {.values = output},
    });
    REQUIRE(node.has_value());
    REQUIRE(pipeline.estimated_device_bytes() == 3 * lhs.size() * sizeof(float));
}

TEST_CASE (

"Crank f32 tensor runtime safely falls back before an unsupported GPU submission"
,
"[crank][gpu_memory][tensor_runtime]"
)
 {
    lithe::codegen::hl::hl_mir_function unsupported{1024};
    f32_tensor lhs{{1.0f, 2.0f, 3.0f}};
    f32_tensor rhs{{4.0f, 5.0f, 6.0f}};
    f32_tensor output{3};
    const auto result = execute_f32_binary(
        unsupported, lhs, rhs, output,
        [](const std::span<const float> left, const std::span<const float> right,
           const std::span<float> destination) noexcept {
            for (std::size_t i = 0; i < destination.size(); ++i)
                destination[i] = left[i] + right[i];
        });
    REQUIRE(result.has_value());
    REQUIRE(result->fallback_fired);
    REQUIRE_FALSE(result->used_gpu);
    REQUIRE(output.values()[0] == 5.0f);
    REQUIRE(output.values()[2] == 9.0f);
}

TEST_CASE (

"Crank GPU pipeline rejects an over-budget chain before device submission"
,
"[crank][gpu_memory][tensor_runtime]"
)
 {
    auto kernel = make_f32_add_kernel(16);
    std::vector<float> lhs(16), rhs(16), output(16);
    crank_gpu_pipeline pipeline;
    REQUIRE(pipeline.add_binary_region({
        .function = std::addressof(kernel),
        .inputs = {
            gpu_metal_input_binding::from_host(lhs),
            gpu_metal_input_binding::from_host(rhs),
        },
        .output = {.values = output},
    }).has_value());
    lithe::exec::auto_execution_policy policy;
    policy.max_device_cache_bytes = 1;
    const auto result = pipeline.execute_observed(policy);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().status == gpu_dispatch_status::resource_exhausted);
}

TEST_CASE (

"Crank GPU pipeline retains a Metal f32 intermediate and downloads only the terminal output"
,
"[crank][gpu_memory][metal][runtime]"
)
 {
    if (gpu_backend::preferred_provider() != gpu_provider::metal) return;

    constexpr std::size_t count = 8192;
    auto first_kernel = make_f32_add_kernel(count);
    auto second_kernel = make_f32_add_kernel(count);
    std::vector<float> lhs(count, 1.0f), rhs(count, 2.0f), output(count);
    crank_gpu_pipeline pipeline;
    const auto first = pipeline.add_binary_region({
        .function = std::addressof(first_kernel),
        .inputs = {
            gpu_metal_input_binding::from_host(lhs),
            gpu_metal_input_binding::from_host(rhs),
        },
    });
    REQUIRE(first.has_value());
    const auto second = pipeline.add_binary_region({
        .function = std::addressof(second_kernel),
        .inputs = {
            gpu_metal_input_binding::from_region(*first),
            gpu_metal_input_binding::from_region(*first),
        },
        .output = {.values = output},
    });
    REQUIRE(second.has_value());

    const auto result = pipeline.execute_observed();
    REQUIRE(result.has_value());
    REQUIRE(result->uploads == 2);
    REQUIRE(result->downloads == 1);
    REQUIRE(result->submissions == 2);
    REQUIRE(output.front() == 6.0f);
    REQUIRE(output.back() == 6.0f);
}

TEST_CASE (

"Crank Vulkan pipeline retains a f32 intermediate and downloads only the terminal output"
,
"[crank][gpu_memory][vulkan][runtime]"
)
 {
#if defined(LITHE_VULKAN_BACKEND_AVAILABLE) && LITHE_VULKAN_BACKEND_AVAILABLE
    if (!gpu_backend::vulkan_available()) return;

    constexpr std::size_t count = 8192;
    auto first_kernel = make_f32_add_kernel(count);
    auto second_kernel = make_f32_add_kernel(count);
    std::vector<float> lhs(count, 1.0f), rhs(count, 2.0f), output(count);
    crank_gpu_pipeline pipeline;
    const auto first = pipeline.add_binary_region({
        .function = std::addressof(first_kernel),
        .inputs = {
            gpu_metal_input_binding::from_host(lhs),
            gpu_metal_input_binding::from_host(rhs),
        },
    });
    REQUIRE(first.has_value());
    REQUIRE(pipeline.add_binary_region({
        .function = std::addressof(second_kernel),
        .inputs = {
            gpu_metal_input_binding::from_region(*first),
            gpu_metal_input_binding::from_region(*first),
        },
        .output = {.values = output},
    }).has_value());

    const auto result = pipeline.execute_observed({}, 0, gpu_provider::vulkan);
    if (!result && result.error().status == gpu_dispatch_status::no_device) return;
    REQUIRE(result.has_value());
    REQUIRE(result->uploads == 2);
    REQUIRE(result->downloads == 1);
    REQUIRE(result->submissions == 2);
    REQUIRE(output.front() == 6.0f);
    REQUIRE(output.back() == 6.0f);
#endif
}
