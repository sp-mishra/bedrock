// =============================================================================
// test_lithe_cost_model.cpp — Unit Tests for Lithe Cost Models + Registry
//
// Verifies: include/edsl/lithe_cost_model.hpp
//           include/edsl/lithe_cost_registry.hpp
//
// Cases:
//   1. static_assert: cost_model concept satisfied for all 6 built-in models.
//   2. cpu_instruction_cost: div node costs more than mul node (4 > 1).
//   3. cpu_instruction_cost: neg node costs more than add node (2 > 1).
//   4. cpu_instruction_cost: child costs are accumulated.
//   5. gpu_parallel_cost: div penalty (8) much higher than add (1).
//   6. tensor_fusion_cost: mul node costs 0 (FMA fusion); div costs 6.
//   7. memory_cost_model: leaf cost is 1.
//   8. memory_cost_model: parent of two leaves has depth 2.
//   9. memory_cost_model: non-indirect ops do NOT double depth.
//  10. power_cost_model: div > mul > add ordering (8 > 2 > 1).
//  11. throughput_cost_model: uses max(child) not sum — verifies critical-path.
//  12. throughput_cost_model: div penalty (4 + max_child) vs add (1 + max_child).
//  13. cost_registry: register + find returns engaged optional.
//  14. cost_registry: find for unregistered key returns nullopt.
//  15. cost_registry: unregister removes an entry; subsequent find → nullopt.
//  16. cost_registry: replace existing entry; descriptor updated.
//  17. cost_registry: size() reflects number of registered models.
//  18. cost_registry: global() returns the same singleton across calls.
//  19. Back-compat: lithe::egraph::cpu_instruction_cost compiles (via lithe_egraph.hpp).
//  20. cost_model_descriptor is move-only (not copyable).
// =============================================================================

#include "catch_amalgamated.hpp"

// Primary includes under test.
#include "lithe/lithe_cost_model.hpp"
#include "lithe/lithe_cost_registry.hpp"

// Back-compat include.
#include "lithe/lithe_egraph.hpp"

#include <span>
#include <type_traits>

namespace lc = lithe::cost;
namespace leg = lithe::egraph;

// ============================================================================
// Helpers: build e_node instances for cost function testing.
// ============================================================================

namespace {
    // Build a leaf e_node with no children.
    lc::lithe_enode_t make_leaf(std::size_t payload = 0) {
        lc::lithe_enode_t n;
        n.op = 0; // leaf sentinel
        n.payload = payload;
        return n;
    }

    // Build an op e_node with given op stable_id.
    lc::lithe_enode_t make_op(std::size_t op_id) {
        lc::lithe_enode_t n;
        n.op = op_id;
        n.payload = 0;
        return n;
    }

    // Stable IDs for common ops (mirrors what lithe_cost_model.hpp uses internally).
    constexpr std::size_t add_op = lithe::emit::tag_descriptor<lithe::add_tag>::stable_id;
    constexpr std::size_t mul_op = lithe::emit::tag_descriptor<lithe::mul_tag>::stable_id;
    constexpr std::size_t div_op = lithe::emit::tag_descriptor<lithe::div_tag>::stable_id;
    constexpr std::size_t neg_op = lithe::emit::tag_descriptor<lithe::neg_tag>::stable_id;
} // anonymous namespace

// ============================================================================
// Test 1 — static_assert: cost_model concept satisfied for all 6 built-in models.
// ============================================================================

TEST_CASE (


"cost_model concept: all 6 built-in models satisfy the concept"
,
"[lithe_cost_model][concept]"
)
{
    static_assert(lc::cost_model<lc::ast_size_cost,        lc::lithe_enode_t>);
    static_assert(lc::cost_model<lc::cpu_instruction_cost, lc::lithe_enode_t>);
    static_assert(lc::cost_model<lc::gpu_parallel_cost,    lc::lithe_enode_t>);
    static_assert(lc::cost_model<lc::tensor_fusion_cost,   lc::lithe_enode_t>);
    static_assert(lc::cost_model<lc::memory_cost_model,    lc::lithe_enode_t>);
    static_assert(lc::cost_model<lc::power_cost_model,     lc::lithe_enode_t>);
    static_assert(lc::cost_model<lc::throughput_cost_model,lc::lithe_enode_t>);
    SUCCEED(); // All static_asserts passed at compile time.
}

// ============================================================================
// Test 2 — cpu_instruction_cost: div costs 4; mul costs 1.
// ============================================================================

TEST_CASE (


"cpu_instruction_cost: div node cost > mul node cost"
,
"[lithe_cost_model][cpu_instruction_cost]"
)
{
    lc::cpu_instruction_cost model;

    auto div_node = make_op(div_op);
    auto mul_node = make_op(mul_op);

    // No children: cost = op_cost + 0.
    std::array<std::size_t, 0> no_children{};
    std::span<const std::size_t> empty_span{no_children};

    const auto cost_div = model.cost(div_node, empty_span);
    const auto cost_mul = model.cost(mul_node, empty_span);

    REQUIRE(cost_div == 4u);
    REQUIRE(cost_mul == 1u);
    REQUIRE(cost_div > cost_mul);
}

// ============================================================================
// Test 3 — cpu_instruction_cost: neg costs 2; add costs 1.
// ============================================================================

TEST_CASE (


"cpu_instruction_cost: neg node cost > add node cost"
,
"[lithe_cost_model][cpu_instruction_cost]"
)
{
    lc::cpu_instruction_cost model;
    std::array<std::size_t, 0> no_children{};
    std::span<const std::size_t> empty_span{no_children};

    const auto cost_neg = model.cost(make_op(neg_op), empty_span);
    const auto cost_add = model.cost(make_op(add_op), empty_span);

    REQUIRE(cost_neg == 2u);
    REQUIRE(cost_add == 1u);
    REQUIRE(cost_neg > cost_add);
}

// ============================================================================
// Test 4 — cpu_instruction_cost: child costs are accumulated (sum).
// ============================================================================

TEST_CASE (


"cpu_instruction_cost: child costs are summed into total"
,
"[lithe_cost_model][cpu_instruction_cost]"
)
{
    lc::cpu_instruction_cost model;

    auto add_node = make_op(add_op);
    std::array<std::size_t, 2> children{3u, 5u}; // pretend children cost 3 and 5
    std::span<const std::size_t> child_span{children};

    // Expected: op_cost(add)=1 + 3 + 5 = 9
    const auto cost = model.cost(add_node, child_span);
    REQUIRE(cost == 9u);
}

// ============================================================================
// Test 5 — gpu_parallel_cost: div costs 8; add costs 1.
// ============================================================================

TEST_CASE (


"gpu_parallel_cost: div is penalized (8) vs add (1)"
,
"[lithe_cost_model][gpu_parallel_cost]"
)
{
    lc::gpu_parallel_cost model;
    std::array<std::size_t, 0> no_children{};
    std::span<const std::size_t> empty_span{no_children};

    REQUIRE(model.cost(make_op(div_op), empty_span) == 8u);
    REQUIRE(model.cost(make_op(add_op), empty_span) == 1u);
    REQUIRE(model.cost(make_op(mul_op), empty_span) == 1u);
    REQUIRE(model.cost(make_op(neg_op), empty_span) == 1u);
}

// ============================================================================
// Test 6 — tensor_fusion_cost: mul costs 0 (FMA); div costs 6.
// ============================================================================

TEST_CASE (


"tensor_fusion_cost: mul cost is 0 (absorbed by FMA); div cost is 6"
,
"[lithe_cost_model][tensor_fusion_cost]"
)
{
    lc::tensor_fusion_cost model;
    std::array<std::size_t, 0> no_children{};
    std::span<const std::size_t> empty_span{no_children};

    REQUIRE(model.cost(make_op(mul_op), empty_span) == 0u);
    REQUIRE(model.cost(make_op(div_op), empty_span) == 6u);
    REQUIRE(model.cost(make_op(add_op), empty_span) == 1u); // default
}

// ============================================================================
// Test 7 — memory_cost_model: leaf node costs 1.
// ============================================================================

TEST_CASE (


"memory_cost_model: leaf node (no children) has cost 1"
,
"[lithe_cost_model][memory_cost_model]"
)
{
    lc::memory_cost_model model;
    auto leaf = make_leaf(42u);
    std::array<std::size_t, 0> no_children{};
    std::span<const std::size_t> empty_span{no_children};

    REQUIRE(model.cost(leaf, empty_span) == 1u);
}

// ============================================================================
// Test 8 — memory_cost_model: parent of two leaves has depth 2.
// ============================================================================

TEST_CASE (


"memory_cost_model: parent of two leaves has cost 2"
,
"[lithe_cost_model][memory_cost_model]"
)
{
    lc::memory_cost_model model;
    auto parent = make_op(add_op);

    // Two leaves each cost 1.
    std::array<std::size_t, 2> child_costs{1u, 1u};
    std::span<const std::size_t> child_span{child_costs};

    // depth = max(1,1) + 1 = 2; non-indirect op — no doubling.
    REQUIRE(model.cost(parent, child_span) == 2u);
}

// ============================================================================
// Test 9 — memory_cost_model: non-indirect ops do NOT double depth.
// ============================================================================

TEST_CASE (


"memory_cost_model: non-indirect op with depth-3 child costs 4"
,
"[lithe_cost_model][memory_cost_model]"
)
{
    lc::memory_cost_model model;
    auto node = make_op(add_op);

    // Simulate one child whose sub-tree depth is 3.
    std::array<std::size_t, 1> child_costs{3u};
    std::span<const std::size_t> child_span{child_costs};

    // depth = 3 + 1 = 4; is_indirect_op always false for built-in ops.
    REQUIRE(model.cost(node, child_span) == 4u);
}

// ============================================================================
// Test 10 — power_cost_model: div(8) > mul(2) > add(1).
// ============================================================================

TEST_CASE (


"power_cost_model: div > mul > add ordering"
,
"[lithe_cost_model][power_cost_model]"
)
{
    lc::power_cost_model model;
    std::array<std::size_t, 0> no_children{};
    std::span<const std::size_t> empty_span{no_children};

    const auto cost_div = model.cost(make_op(div_op), empty_span);
    const auto cost_mul = model.cost(make_op(mul_op), empty_span);
    const auto cost_add = model.cost(make_op(add_op), empty_span);

    REQUIRE(cost_div == 8u);
    REQUIRE(cost_mul == 2u);
    REQUIRE(cost_add == 1u);

    REQUIRE(cost_div > cost_mul);
    REQUIRE(cost_mul > cost_add);
}

// ============================================================================
// Test 11 — throughput_cost_model: uses max not sum for children.
// ============================================================================

TEST_CASE (


"throughput_cost_model: uses max(child_costs) not sum"
,
"[lithe_cost_model][throughput_cost_model]"
)
{
    lc::throughput_cost_model model;

    auto add_node = make_op(add_op);

    // Two children with very different costs.
    std::array<std::size_t, 2> children{2u, 10u};
    std::span<const std::size_t> child_span{children};

    // cost = op_cost(add=1) + max(2, 10) = 11  (not 1+2+10=13)
    REQUIRE(model.cost(add_node, child_span) == 11u);
}

// ============================================================================
// Test 12 — throughput_cost_model: div penalty vs add.
// ============================================================================

TEST_CASE (


"throughput_cost_model: div (4) has higher cost than add (1) at same depth"
,
"[lithe_cost_model][throughput_cost_model]"
)
{
    lc::throughput_cost_model model;
    std::array<std::size_t, 0> no_children{};
    std::span<const std::size_t> empty_span{no_children};

    // Leaf div: op_cost=4 + max(nothing)=0 → 4.
    // Leaf add: op_cost=1 + 0 → 1.
    REQUIRE(model.cost(make_op(div_op), empty_span) == 4u);
    REQUIRE(model.cost(make_op(add_op), empty_span) == 1u);
}

// ============================================================================
// Test 13 — cost_registry: register + find returns engaged optional.
// ============================================================================

TEST_CASE (


"cost_registry: register_model then find returns engaged optional"
,
"[lithe_cost_registry][register][find]"
)
{
    lc::cost_registry reg;

    lc::cost_fn fn{[](const void* /*node*/, std::span<const float> /*ch*/) -> float {
        return 1.0f;
    }};

    reg.register_model(lc::cost_model_descriptor{
        "test.unit.add",
        "Unit-cost model for testing",
        std::move(fn)
    });

    auto found = reg.find("test.unit.add");
    REQUIRE(found.has_value());
}

// ============================================================================
// Test 14 — cost_registry: find for unregistered key returns nullopt.
// ============================================================================

TEST_CASE (


"cost_registry: find unknown key returns nullopt"
,
"[lithe_cost_registry][find]"
)
{
    lc::cost_registry reg;
    auto found = reg.find("does.not.exist");
    REQUIRE_FALSE(found.has_value());
}

// ============================================================================
// Test 15 — cost_registry: unregister removes entry; subsequent find → nullopt.
// ============================================================================

TEST_CASE (


"cost_registry: unregister removes entry"
,
"[lithe_cost_registry][unregister]"
)
{
    lc::cost_registry reg;

    lc::cost_fn fn{[](const void*, std::span<const float>) -> float { return 0.0f; }};
    reg.register_model(lc::cost_model_descriptor{"rm.model", "to remove", std::move(fn)});

    REQUIRE(reg.find("rm.model").has_value());

    reg.unregister("rm.model");
    REQUIRE_FALSE(reg.find("rm.model").has_value());
}

// ============================================================================
// Test 16 — cost_registry: replace existing entry; descriptor updated.
// ============================================================================

TEST_CASE (


"cost_registry: registering same id replaces the previous entry"
,
"[lithe_cost_registry][register][replace]"
)
{
    lc::cost_registry reg;

    {
        lc::cost_fn fn1{[](const void*, std::span<const float>) -> float { return 1.0f; }};
        reg.register_model({"replace.me", "first version", std::move(fn1)});
    }
    {
        lc::cost_fn fn2{[](const void*, std::span<const float>) -> float { return 99.0f; }};
        reg.register_model({"replace.me", "second version", std::move(fn2)});
    }

    auto found = reg.find("replace.me");
    REQUIRE(found.has_value());

    // Call the replacement fn; it should return 99.
    const lc::cost_fn& cfn = found->get();
    const float result = cfn(nullptr, {});
    REQUIRE(result == Catch::Approx(99.0f));
}

// ============================================================================
// Test 17 — cost_registry: size() reflects number of registered models.
// ============================================================================

TEST_CASE (


"cost_registry: size() reflects registration count"
,
"[lithe_cost_registry][size]"
)
{
    lc::cost_registry reg;
    REQUIRE(reg.size() == 0u);

    {
        lc::cost_fn fn{[](const void*, std::span<const float>) -> float { return 0.f; }};
        reg.register_model({"sz.a", "", std::move(fn)});
    }
    REQUIRE(reg.size() == 1u);

    {
        lc::cost_fn fn{[](const void*, std::span<const float>) -> float { return 0.f; }};
        reg.register_model({"sz.b", "", std::move(fn)});
    }
    REQUIRE(reg.size() == 2u);

    reg.unregister("sz.a");
    REQUIRE(reg.size() == 1u);
}

// ============================================================================
// Test 18 — cost_registry: global() returns the same singleton.
// ============================================================================

TEST_CASE (


"cost_registry::global() returns the same singleton instance"
,
"[lithe_cost_registry][global]"
)
{
    auto& g1 = lc::cost_registry::global();
    auto& g2 = lc::cost_registry::global();
    REQUIRE(&g1 == &g2);
}

// ============================================================================
// Test 19 — Back-compat: lithe::egraph::cpu_instruction_cost is accessible.
// ============================================================================

TEST_CASE (


"back-compat: lithe::egraph::cpu_instruction_cost is accessible via lithe_egraph.hpp"
,
"[lithe_cost_model][backcompat]"
)
{
    // This test verifies the back-compat alias compiles and produces a callable cost model.
    leg::cpu_instruction_cost model;

    lc::lithe_enode_t add_node = make_op(add_op);
    std::array<std::size_t, 0> no_children{};
    std::span<const std::size_t> empty_span{no_children};

    // Should work and return a small cost for an add.
    const auto c = model.cost(add_node, empty_span);
    REQUIRE(c >= 1u);
}

// ============================================================================
// Test 20 — cost_model_descriptor is move-only (not copyable).
// ============================================================================

TEST_CASE (


"cost_model_descriptor is move-only; not copy-constructible"
,
"[lithe_cost_registry][cost_model_descriptor]"
)
{
    static_assert(!std::is_copy_constructible_v<lc::cost_model_descriptor>,
                  "cost_model_descriptor must be move-only");
    static_assert(!std::is_copy_assignable_v<lc::cost_model_descriptor>,
                  "cost_model_descriptor must be move-only");
    static_assert(std::is_move_constructible_v<lc::cost_model_descriptor>,
                  "cost_model_descriptor must be movable");
    SUCCEED(); // Compile-time assertions passed.
}
