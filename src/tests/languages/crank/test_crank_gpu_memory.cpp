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

using namespace crank;

namespace {
    device_buffer buf(address_space space, buffer_access access, residency_state res) {
        device_buffer b;
        b.space = space;
        b.access = access;
        b.residency = res;
        return b;
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
