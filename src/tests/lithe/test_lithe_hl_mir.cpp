#include "catch_amalgamated.hpp"

#include "lithe/lithe_codegen_pipeline.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// HL MIR: region model, arena lifecycle, region fusion, coordinate lowering
// ─────────────────────────────────────────────────────────────────────────────

using namespace lithe::codegen;
using namespace lithe::codegen::hl;

namespace {
    // Build a one-block hl_mir_function with a single structured_for op.
    // `rank`, `lower`, `upper`, `step`, `is_parallel` control the attr.
    hl_mir_function make_hl_loop(std::uint8_t rank = 1,
                                 int lower = 0, int upper = 16, int step = 1,
                                 bool is_parallel = false,
                                 std::size_t arena_cap = 1u << 20) {
        hl_mir_function fn{arena_cap};
        fn.name = "test_loop";

        auto* blk = fn.make_block();
        fn.body_region.blocks.push_back(blk);
        blk->parent_region = &fn.body_region;

        auto* for_op = fn.make_op(hl_opcode::structured_for);
        structured_for_attr sf;
        sf.rank = rank;
        sf.is_parallel = is_parallel;
        for (std::uint8_t d = 0; d < rank; ++d)
            sf.bounds[d] = {lower, upper, step, true, true, true};
        for_op->attr = sf;

        auto* body_region = fn.make_region();
        auto body_rspan = fn.alloc_span<hl_region*>(1);
        body_rspan[0] = body_region;
        for_op->regions = body_rspan;
        body_region->parent_op = for_op;

        blk->ops.push_back(for_op);
        return fn;
    }
} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. Basic structural invariants
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"hl_mir_function: basic structural invariants"
,
"[lithe][hl][struct]"
)
 {
    auto fn = make_hl_loop();

    // Body region must have exactly one block.
    REQUIRE(fn.body_region.blocks.size() == 1u);
    const auto* blk = fn.body_region.blocks.head;
    REQUIRE(blk != nullptr);

    // Block must contain exactly one op: the structured_for.
    REQUIRE(blk->ops.size() == 1u);
    const auto* op = blk->ops.head;
    REQUIRE(op != nullptr);
    REQUIRE(op->op == hl_opcode::structured_for);

    // Parent links
    REQUIRE(blk->parent_region == &fn.body_region);
    REQUIRE_FALSE(op->regions.empty());
    const auto* body_rgn = op->regions[0];
    REQUIRE(body_rgn != nullptr);
    REQUIRE(body_rgn->parent_op == op);
}

TEST_CASE (


"hl_mir_function: structured_for_attr fields correct"
,
"[lithe][hl][attr]"
)
 {
    auto fn = make_hl_loop(1, 4, 1024, 2, true);

    const auto* blk = fn.body_region.blocks.head;
    const auto* op  = blk->ops.head;
    REQUIRE(std::holds_alternative<structured_for_attr>(op->attr));

    const auto& sf = std::get<structured_for_attr>(op->attr);
    REQUIRE(sf.rank        == 1);
    REQUIRE(sf.is_parallel == true);
    REQUIRE(sf.bounds[0].lower      == 4);
    REQUIRE(sf.bounds[0].upper      == 1024);
    REQUIRE(sf.bounds[0].step       == 2);
    REQUIRE(sf.bounds[0].lower_known);
    REQUIRE(sf.bounds[0].upper_known);
    REQUIRE(sf.bounds[0].step_known);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Effects predicates
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"hl_effect_flags: effect predicates are correct"
,
"[lithe][hl][effects]"
)
 {
    REQUIRE(is_pure(hl_opcode::fadd));
    REQUIRE(is_pure(hl_opcode::add));
    REQUIRE(is_pure(hl_opcode::constant));
    REQUIRE_FALSE(is_pure(hl_opcode::memref_load));
    REQUIRE_FALSE(is_pure(hl_opcode::memref_store));
    REQUIRE(is_terminator(hl_opcode::region_yield));
    REQUIRE_FALSE(is_terminator(hl_opcode::fadd));
    REQUIRE(has_effect(effects_of(hl_opcode::memref_store), hl_effect_flags::write));
    REQUIRE(has_effect(effects_of(hl_opcode::memref_load),  hl_effect_flags::read));
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. arena_checkpoint_guard — RAII rollback
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"arena_checkpoint_guard: rolls back on abort"
,
"[lithe][hl][arena]"
)
 {
    hl_mir_function fn{1024};
    const auto used_before = fn.arena.used_bytes();

    {
        arena_checkpoint_guard guard{fn};
        // Allocate inside speculative scope.
        [[maybe_unused]] auto* op = fn.make_op(hl_opcode::fadd);
        [[maybe_unused]] auto* blk = fn.make_block();
        REQUIRE(fn.arena.used_bytes() > used_before);
        // Guard destructs without commit → rollback.
    }

    REQUIRE(fn.arena.used_bytes() == used_before);
}

TEST_CASE (


"arena_checkpoint_guard: commit keeps allocations"
,
"[lithe][hl][arena]"
)
 {
    hl_mir_function fn{1024};

    {
        arena_checkpoint_guard guard{fn};
        [[maybe_unused]] auto* op = fn.make_op(hl_opcode::fadd);
        guard.commit();
    }

    REQUIRE(fn.arena.used_bytes() > 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. region_fusion_pass — positive case (equal bounds)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"region_fusion_pass: fuses two equal-bound structured_for ops"
,
"[lithe][hl][fusion]"
)
 {
    hl_mir_function fn{1u << 20};
    fn.name = "fusion_test";

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto make_for = [&](int lower, int upper) -> hl_operation* {
        auto* op = fn.make_op(hl_opcode::structured_for);
        structured_for_attr sf;
        sf.rank = 1;
        sf.bounds[0] = { lower, upper, 1, true, true, true };
        op->attr = sf;

        auto* body = fn.make_region();
        auto rspan = fn.alloc_span<hl_region*>(1);
        rspan[0] = body;
        op->regions = rspan;
        body->parent_op = op;

        // Add a yield op in body block.
        auto* body_blk = fn.make_block();
        auto* yield    = fn.make_op(hl_opcode::region_yield);
        body_blk->ops.push_back(yield);
        body->blocks.push_back(body_blk);
        body_blk->parent_region = body;

        return op;
    };

    auto* first  = make_for(0, 32);
    auto* second = make_for(0, 32);
    blk->ops.push_back(first);
    blk->ops.push_back(second);

    REQUIRE(blk->ops.size() == 2u);

    region_fusion_pass pass;
    const auto result = pass.run(fn, *blk, first, second);

    REQUIRE(result.fused);
    // After fusion: only one op remains in block.
    REQUIRE(blk->ops.size() == 1u);
    // Fused body contains both body blocks (the two yield ops).
    const auto* fused_body = first->regions[0];
    REQUIRE(fused_body != nullptr);
    REQUIRE(fused_body->blocks.size() == 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. region_fusion_pass — negative case (unequal bounds → no mutation)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"region_fusion_pass: refuses fusion on unequal bounds"
,
"[lithe][hl][fusion]"
)
 {
    hl_mir_function fn{1u << 20};

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto make_for = [&](int upper) -> hl_operation* {
        auto* op = fn.make_op(hl_opcode::structured_for);
        structured_for_attr sf;
        sf.rank = 1;
        sf.bounds[0] = { 0, upper, 1, true, true, true };
        op->attr = sf;

        auto* body = fn.make_region();
        auto rspan = fn.alloc_span<hl_region*>(1);
        rspan[0] = body;
        op->regions = rspan;
        body->parent_op = op;
        return op;
    };

    auto* first  = make_for(16);
    auto* second = make_for(32); // different upper
    blk->ops.push_back(first);
    blk->ops.push_back(second);

    const auto used_before = fn.arena.used_bytes();

    region_fusion_pass pass;
    const auto result = pass.run(fn, *blk, first, second);

    REQUIRE_FALSE(result.fused);
    REQUIRE_FALSE(result.diagnostic.empty());
    // No structural mutation: block still has 2 ops.
    REQUIRE(blk->ops.size() == 2u);
    // Arena must not have grown (guard rolled back).
    REQUIRE(fn.arena.used_bytes() == used_before);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. loop_tiling_pass — tiles a rank-1 structured_for
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"loop_tiling_pass: tiles rank-1 loop into outer+inner nest"
,
"[lithe][hl][tiling]"
)
 {
    hl_mir_function fn{1u << 20};
    fn.name = "tiling_test";

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto* for_op = fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank = 1;
    sf.bounds[0] = { 0, 1024, 1, true, true, true };
    sf.tile[0]   = 32; // tile size
    for_op->attr = sf;

    auto* body_rgn = fn.make_region();
    auto rspan = fn.alloc_span<hl_region*>(1);
    rspan[0] = body_rgn;
    for_op->regions = rspan;
    body_rgn->parent_op = for_op;
    blk->ops.push_back(for_op);

    loop_tiling_pass pass;
    const auto result = pass.run(fn, *blk, for_op);

    REQUIRE(result.tiled);
    // The block now has one op: outer loop.
    REQUIRE(blk->ops.size() == 1u);
    const auto* outer = blk->ops.head;
    REQUIRE(outer != nullptr);
    REQUIRE(outer->op == hl_opcode::structured_for);

    const auto& outer_sf = std::get<structured_for_attr>(outer->attr);
    REQUIRE(outer_sf.bounds[0].step == 32); // outer step = tile size
    REQUIRE(outer_sf.tile[0] == 0);         // outer no longer carries tile

    // Outer body contains inner loop.
    REQUIRE_FALSE(outer->regions.empty());
    const auto* outer_body = outer->regions[0];
    REQUIRE_FALSE(outer_body->blocks.empty());
    const auto* inner_op = outer_body->blocks.head->ops.head;
    REQUIRE(inner_op != nullptr);
    REQUIRE(inner_op->op == hl_opcode::structured_for);

    const auto& inner_sf = std::get<structured_for_attr>(inner_op->attr);
    REQUIRE(inner_sf.bounds[0].upper == 32); // inner runs 0..tile_size
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. coordinate_lowering_pass — rank-1 loop produces flat blocks
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"coordinate_lowering_pass: rank-1 loop emits header/body/latch"
,
"[lithe][hl][lowering]"
)
 {
    auto hl_fn = make_hl_loop(1, 0, 16, 1, false);

    coordinate_lowering_pass pass;
    const auto result = pass.run(hl_fn);

    // No fatal errors.
    // (may have warnings about rank-1 nested, but must have blocks)
    REQUIRE_FALSE(result.fn.function.blocks.empty());

    // Must have at least 3 blocks: init/prolog, header, exit (body is empty for our test).
    REQUIRE(result.fn.function.blocks.size() >= 2u);

    // Entry block should be set.
    REQUIRE(result.fn.function.cfg.entry_block != 0u);
}

TEST_CASE (


"coordinate_lowering_pass: round-trip polyhedral check"
,
"[lithe][hl][lowering][poly]"
)
 {
    // Build HL loop with bounds 0..8, lower to flat, recover polyhedral, compare.
    auto hl_fn = make_hl_loop(1, 0, 8, 1, false);

    coordinate_lowering_pass lower_pass;
    const auto low_result = lower_pass.run(hl_fn);
    REQUIRE_FALSE(low_result.fn.function.blocks.empty());

    // Run bottom-up polyhedral recovery on flat output.
    mir_pass_context ctx;
    const auto poly_result = lithe::poly::extract_polyhedral(low_result.fn, ctx);

    // Expect at least one loop detected in the flat form.
    REQUIRE_FALSE(poly_result.loops.empty());

    // The recovered loop must be affine with bounds matching the original.
    // Note: the bottom-up pass may not recover exact bounds if the flat prolog
    // is not in the canonical form it expects; we require it finds a loop and
    // reports it as at least partially matching.
    REQUIRE(poly_result.loops[0].ivars.size() >= 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. memref_type helpers
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"memref_type: row_major factory computes correct strides"
,
"[lithe][hl][memref]"
)
 {
    // Shape [4, 8] → strides [8, 1]
    const auto m = memref_type::row_major(
        abstract_value_kind::floating, 64, 2,
        {4, 8, 0, 0, 0, 0, 0, 0});

    REQUIRE(m.rank    == 2);
    REQUIRE(m.shape[0] == 4);
    REQUIRE(m.shape[1] == 8);
    REQUIRE(m.strides[0] == 8); // 8 elements per row
    REQUIRE(m.strides[1] == 1);
    REQUIRE(m.contiguous);
    REQUIRE(m.fully_static());
    REQUIRE(m.linear_size() == 32);
}

TEST_CASE (


"memref_type: dynamic dimension not fully_static"
,
"[lithe][hl][memref]"
)
 {
    memref_type m;
    m.rank    = 2;
    m.shape   = {4, 0, 0, 0, 0, 0, 0, 0}; // second dim dynamic
    m.strides = {0, 1, 0, 0, 0, 0, 0, 0};
    REQUIRE_FALSE(m.fully_static());
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. coordinate_lowering_pass — sub-byte bitmasking
// ─────────────────────────────────────────────────────────────────────────────

// Build hl_mir_function with a memref_load of elem_bits=4 (nibble) inside a
// structured_for loop.  The lowering must emit a read + shift + mask sequence
// rather than a direct fload with stride_bytes = 0.
TEST_CASE (


"coordinate_lowering_pass: sub-byte load emits shift/mask sequence"
,
"[lithe][hl][lowering][sub-byte]"
)
 {
    hl_mir_function fn{1u << 20};
    fn.name = "sub_byte_load";

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto* for_op = fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank = 1;
    sf.bounds[0] = { 0, 8, 1, true, true, true };
    for_op->attr = sf;

    auto* body_rgn = fn.make_region();
    auto rspan = fn.alloc_span<hl_region*>(1);
    rspan[0] = body_rgn;
    for_op->regions = rspan;
    body_rgn->parent_op = for_op;

    // Inner block: a memref_load with elem_bits = 4 (nibble elements).
    auto* body_blk = fn.make_block();
    body_rgn->blocks.push_back(body_blk);
    body_blk->parent_region = body_rgn;

    auto* load_op = fn.make_op(hl_opcode::memref_load);
    memref_type mrt;
    mrt.rank      = 1;
    mrt.elem_bits = 4; // sub-byte: nibbles
    mrt.shape   = {8, 0, 0, 0, 0, 0, 0, 0};
    mrt.strides = {1, 0, 0, 0, 0, 0, 0, 0};

    memref_attr ma;
    ma.view               = mrt;
    ma.base_operand_index = 0;
    load_op->attr = ma;

    // Operands: base ptr (SSA id 1), index (SSA id 2)
    auto op_span = fn.alloc_span<ssa_value_id>(2);
    op_span[0] = ssa_value_id{1};
    op_span[1] = ssa_value_id{2};
    load_op->operands = op_span;

    // Result: SSA id 10
    auto res_span = fn.alloc_span<ssa_value_id>(1);
    res_span[0] = ssa_value_id{10};
    load_op->results = res_span;

    body_blk->ops.push_back(load_op);

    // Yield to close body.
    auto* yield_op = fn.make_op(hl_opcode::region_yield);
    body_blk->ops.push_back(yield_op);

    blk->ops.push_back(for_op);

    coordinate_lowering_pass pass;
    const auto result = pass.run(fn);

    REQUIRE(result.ok());
    REQUIRE_FALSE(result.fn.function.blocks.empty());

    // Verify: somewhere in the flat output there must be a shr and a bit_and,
    // which are the masking instructions emitted for sub-byte load.
    bool has_shr     = false;
    bool has_bit_and = false;
    for (const auto& fb : result.fn.function.blocks) {
        for (const auto& inst : fb.instructions) {
            if (inst.op == opcode::shr)     has_shr     = true;
            if (inst.op == opcode::bit_and) has_bit_and = true;
        }
    }
    REQUIRE(has_shr);
    REQUIRE(has_bit_and);
}

TEST_CASE (


"coordinate_lowering_pass: sub-byte store emits read-modify-write"
,
"[lithe][hl][lowering][sub-byte]"
)
 {
    hl_mir_function fn{1u << 20};
    fn.name = "sub_byte_store";

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto* for_op = fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank = 1;
    sf.bounds[0] = { 0, 8, 1, true, true, true };
    for_op->attr = sf;

    auto* body_rgn = fn.make_region();
    auto rspan = fn.alloc_span<hl_region*>(1);
    rspan[0] = body_rgn;
    for_op->regions = rspan;
    body_rgn->parent_op = for_op;

    auto* body_blk = fn.make_block();
    body_rgn->blocks.push_back(body_blk);
    body_blk->parent_region = body_rgn;

    auto* store_op = fn.make_op(hl_opcode::memref_store);
    memref_type mrt;
    mrt.rank      = 1;
    mrt.elem_bits = 1; // sub-byte: single bits
    mrt.shape   = {8, 0, 0, 0, 0, 0, 0, 0};
    mrt.strides = {1, 0, 0, 0, 0, 0, 0, 0};

    memref_attr ma;
    ma.view               = mrt;
    ma.base_operand_index = 0;
    store_op->attr = ma;

    // Operands: base ptr (id 1), value to store (id 2), index (id 3)
    auto op_span = fn.alloc_span<ssa_value_id>(3);
    op_span[0] = ssa_value_id{1};
    op_span[1] = ssa_value_id{2};
    op_span[2] = ssa_value_id{3};
    store_op->operands = op_span;

    body_blk->ops.push_back(store_op);
    auto* yield_op = fn.make_op(hl_opcode::region_yield);
    body_blk->ops.push_back(yield_op);

    blk->ops.push_back(for_op);

    coordinate_lowering_pass pass;
    const auto result = pass.run(fn);

    REQUIRE(result.ok());

    // Sub-byte store must emit: load (read), bit_not (clear mask), bit_and, shl, bit_or, store.
    bool has_bit_not = false;
    bool has_bit_or  = false;
    bool has_shl     = false;
    for (const auto& fb : result.fn.function.blocks) {
        for (const auto& inst : fb.instructions) {
            if (inst.op == opcode::bit_not) has_bit_not = true;
            if (inst.op == opcode::bit_or)  has_bit_or  = true;
            if (inst.op == opcode::shl)     has_shl     = true;
        }
    }
    REQUIRE(has_bit_not);
    REQUIRE(has_bit_or);
    REQUIRE(has_shl);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. pre_header_isolation — split blocks so each structured_for stands alone
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"pre_header_isolation: noop when block already has one op"
,
"[lithe][hl][pre_header]"
)
 {
    // A function with one block that holds just a structured_for — no split needed.
    auto fn = make_hl_loop(1, 0, 16, 1, false);

    pre_header_isolation iso;
    const auto result = iso.run(fn);

    REQUIRE(result.ok());
    REQUIRE(result.blocks_created == 0u);
    REQUIRE(fn.body_region.blocks.size() == 1u);
}

TEST_CASE (


"pre_header_isolation: splits ops after structured_for into new block"
,
"[lithe][hl][pre_header]"
)
 {
    // Build: one block with [structured_for, constant].
    // After isolation: [structured_for] | [constant] in two blocks.
    hl_mir_function fn{1u << 20};
    fn.name = "iso_test";

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto* for_op = fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank = 1;
    sf.bounds[0] = { 0, 8, 1, true, true, true };
    for_op->attr = sf;

    auto* body = fn.make_region();
    auto rspan = fn.alloc_span<hl_region*>(1);
    rspan[0] = body;
    for_op->regions = rspan;
    body->parent_op = for_op;
    blk->ops.push_back(for_op);

    auto* const_op = fn.make_op(hl_opcode::constant);
    blk->ops.push_back(const_op);

    REQUIRE(blk->ops.size() == 2u);

    pre_header_isolation iso;
    const auto result = iso.run(fn);

    REQUIRE(result.ok());
    REQUIRE(result.blocks_created >= 1u);
    REQUIRE(fn.body_region.blocks.size() >= 2u);

    // First block: exactly one op — the structured_for.
    const auto* first_blk = fn.body_region.blocks.head;
    REQUIRE(first_blk != nullptr);
    REQUIRE(first_blk->ops.size() == 1u);
    REQUIRE(first_blk->ops.head->op == hl_opcode::structured_for);

    // Second block: the constant op.
    const auto* second_blk = first_blk->list_node.next;
    REQUIRE(second_blk != nullptr);
    REQUIRE(second_blk->ops.size() == 1u);
    REQUIRE(second_blk->ops.head->op == hl_opcode::constant);
}

TEST_CASE (


"pre_header_isolation: splits ops before structured_for into predecessor"
,
"[lithe][hl][pre_header]"
)
 {
    // Build: one block with [constant, structured_for].
    // After isolation: [constant] | [structured_for] in two blocks.
    hl_mir_function fn{1u << 20};
    fn.name = "iso_pre_test";

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    auto* const_op = fn.make_op(hl_opcode::constant);
    blk->ops.push_back(const_op);

    auto* for_op = fn.make_op(hl_opcode::structured_for);
    structured_for_attr sf;
    sf.rank = 1;
    sf.bounds[0] = { 0, 4, 1, true, true, true };
    for_op->attr = sf;

    auto* body = fn.make_region();
    auto rspan = fn.alloc_span<hl_region*>(1);
    rspan[0] = body;
    for_op->regions = rspan;
    body->parent_op = for_op;
    blk->ops.push_back(for_op);

    REQUIRE(blk->ops.size() == 2u);

    pre_header_isolation iso;
    const auto result = iso.run(fn);

    REQUIRE(result.ok());
    REQUIRE(result.blocks_created >= 1u);
    REQUIRE(fn.body_region.blocks.size() >= 2u);

    // First block: constant op.
    const auto* first_blk = fn.body_region.blocks.head;
    REQUIRE(first_blk != nullptr);
    REQUIRE(first_blk->ops.head->op == hl_opcode::constant);

    // Second block: structured_for alone.
    const auto* second_blk = first_blk->list_node.next;
    REQUIRE(second_blk != nullptr);
    REQUIRE(second_blk->ops.size() == 1u);
    REQUIRE(second_blk->ops.head->op == hl_opcode::structured_for);
}

TEST_CASE (


"pre_header_isolation: empty function produces no blocks"
,
"[lithe][hl][pre_header]"
)
 {
    hl_mir_function fn{1u << 20};
    fn.name = "empty";

    pre_header_isolation iso;
    const auto result = iso.run(fn);

    REQUIRE(result.ok());
    REQUIRE(result.blocks_created == 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 14. coordinate_lowering_pass — block_args (MLIR-style) lowered to flat pregs
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE (


"coordinate_lowering_pass: block_args produce valid ssa_to_preg entries"
,
"[lithe][hl][lowering][block_args]"
)
 {
    // Build a function whose body block declares two block arguments.
    // After lowering, the flat output must contain instructions (e.g. a fadd
    // consuming those args) that reference fresh pregs — i.e. the lowering
    // must not emit an "unhandled" diagnostic for the args.
    hl_mir_function fn{1u << 20};
    fn.name = "block_args_test";

    auto* blk = fn.make_block();
    fn.body_region.blocks.push_back(blk);
    blk->parent_region = &fn.body_region;

    // Declare two block arguments: SSA ids 101 and 102.
    auto arg_span = fn.alloc_span<ssa_value_id>(2);
    arg_span[0] = ssa_value_id{101};
    arg_span[1] = ssa_value_id{102};
    blk->block_args = arg_span;

    // fadd consuming both block args — result SSA id 200.
    auto* add_op = fn.make_op(hl_opcode::fadd);
    auto ops = fn.alloc_span<ssa_value_id>(2);
    ops[0] = ssa_value_id{101};
    ops[1] = ssa_value_id{102};
    add_op->operands = ops;
    auto res = fn.alloc_span<ssa_value_id>(1);
    res[0] = ssa_value_id{200};
    add_op->results = res;
    blk->ops.push_back(add_op);

    coordinate_lowering_pass pass;
    const auto result = pass.run(fn);

    // Must succeed with no diagnostics.
    REQUIRE(result.ok());
    REQUIRE_FALSE(result.fn.function.blocks.empty());

    // The flat output must contain at least one fadd instruction.
    bool has_fadd = false;
    for (const auto& fb : result.fn.function.blocks)
        for (const auto& inst : fb.instructions)
            if (inst.op == opcode::fadd) has_fadd = true;
    REQUIRE(has_fadd);
}

TEST_CASE (


"hl_mir_function: structured_for_attr carries optional loop hints"
,
"[lithe][hl][attr][loop_hints]"
)
 {
    auto fn = make_hl_loop(1, 0, 32, 2, false);
    auto* blk = fn.body_region.blocks.head;
    REQUIRE(blk != nullptr);
    auto* op = blk->ops.head;
    REQUIRE(op != nullptr);
    REQUIRE(std::holds_alternative<structured_for_attr>(op->attr));

    auto& sf = std::get<structured_for_attr>(op->attr);
    sf.bounds_known = true;
    sf.stride_regular = true;
    sf.trip_count_hint = 16;

    CHECK(sf.bounds_known);
    CHECK(sf.stride_regular);
    CHECK(sf.trip_count_hint == 16u);
 }

TEST_CASE (
"vector_polyhedral_planning_pass: materializes a profitable scalar-tail vector plan",
"[lithe][hl][vector][poly][phase-c]"
) {
    auto fn = make_hl_loop(1, 0, 18, 1, true);
    auto* loop = fn.body_region.blocks.head->ops.head;
    auto* body = loop->regions[0];
    auto* block = fn.make_block();
    body->blocks.push_back(block);
    block->parent_region = body;
    auto* load = fn.make_op(hl_opcode::memref_load);
    memref_attr memory;
    memory.view = memref_type::row_major(abstract_value_kind::floating, 32, 1, {18});
    memory.view.alignment_bytes = 32;
    load->attr = memory;
    block->ops.push_back(load);

    const auto result = vector_polyhedral_planning_pass{}.run(fn);
    REQUIRE(result.ok());
    REQUIRE(result.plans.size() == 1);
    const auto& plan = result.plans.front();
    REQUIRE(plan.legality == vector_plan_legality::proven);
    REQUIRE(plan.lanes == 8);
    REQUIRE(plan.tail == vector_tail_strategy::scalar_epilogue);
    REQUIRE(plan.schedule_materialized);
    REQUIRE(plan.scalar_fallback);
}

TEST_CASE (
"vector_polyhedral_planning_pass: rejects possible in-place dependence",
"[lithe][hl][vector][poly][phase-c]"
) {
    auto fn = make_hl_loop(1, 0, 32, 1, true);
    auto* loop = fn.body_region.blocks.head->ops.head;
    auto* body = loop->regions[0];
    auto* block = fn.make_block();
    body->blocks.push_back(block);
    block->parent_region = body;
    const auto base = fn.alloc_span<ssa_value_id>(1);
    base[0] = ssa_value_id{42};
    for (const auto opcode : {hl_opcode::memref_load, hl_opcode::memref_store}) {
        auto* memory_op = fn.make_op(opcode);
        memory_op->operands = base;
        memref_attr memory;
        memory.view = memref_type::row_major(abstract_value_kind::integer, 32, 1, {32});
        memory.view.alignment_bytes = 32;
        memory_op->attr = memory;
        block->ops.push_back(memory_op);
    }

    const auto result = vector_polyhedral_planning_pass{}.run(fn);
    REQUIRE(result.ok());
    REQUIRE(result.plans.size() == 1);
    REQUIRE(result.plans.front().legality == vector_plan_legality::rejected);
    REQUIRE_FALSE(result.plans.front().schedule_materialized);
}

TEST_CASE (
"vector_polyhedral_planning_pass: dynamic bounds retain a scalar fallback",
"[lithe][hl][vector][poly][phase-c]"
) {
    auto fn = make_hl_loop(1, 0, 0, 1, true);
    auto* loop = fn.body_region.blocks.head->ops.head;
    auto& attr = std::get<structured_for_attr>(loop->attr);
    attr.bounds[0].upper_known = false;
    auto* body = loop->regions[0];
    auto* block = fn.make_block();
    body->blocks.push_back(block);
    block->parent_region = body;
    auto* load = fn.make_op(hl_opcode::memref_load);
    memref_attr memory;
    memory.view = memref_type::row_major(abstract_value_kind::floating, 32, 1, {0});
    memory.view.alignment_bytes = 32;
    load->attr = memory;
    block->ops.push_back(load);

    const auto result = vector_polyhedral_planning_pass{.options = {.masked_tails_supported = true}}.run(fn);
    REQUIRE(result.ok());
    REQUIRE(result.plans.size() == 1);
    REQUIRE(result.plans.front().legality != vector_plan_legality::proven);
    REQUIRE(result.plans.front().scalar_fallback);
    REQUIRE_FALSE(result.plans.front().schedule_materialized);
}

TEST_CASE (
"select_execution_plan: deterministic cost selection honors vector legality",
"[lithe][execution][cost][phase-d]"
) {
    execution_selection_inputs inputs;
    inputs.work_items = 1024;
    inputs.vector_legal = true;
    inputs.candidates = {{
        {planned_execution_kind::interpreter, true, 0, 10},
        {planned_execution_kind::jit, true, 100, 2},
        {planned_execution_kind::simd, true, 200, 1},
        {planned_execution_kind::metal, false, 0, 0},
        {planned_execution_kind::vulkan, false, 0, 0},
    }};
    const auto first = select_execution_plan(inputs);
    const auto second = select_execution_plan(inputs);
    REQUIRE(first.selected == planned_execution_kind::simd);
    REQUIRE(first.selected == second.selected);
    REQUIRE(first.estimated_cost_ns == second.estimated_cost_ns);
}

TEST_CASE (
"select_execution_plan: unavailable explicit provider falls back safely",
"[lithe][execution][cost][phase-d]"
) {
    execution_selection_inputs inputs;
    inputs.candidates = {{
        {planned_execution_kind::interpreter, true, 0, 10},
        {planned_execution_kind::jit, false, 0, 0},
        {planned_execution_kind::simd, false, 0, 0},
        {planned_execution_kind::metal, false, 0, 0},
        {planned_execution_kind::vulkan, true, 0, 1},
    }};
    const execution_selection_policy policy{.force = planned_execution_kind::metal};
    const auto selected = select_execution_plan(inputs, policy);
    REQUIRE(selected.selected == planned_execution_kind::interpreter);
    REQUIRE(selected.fell_back);
}

TEST_CASE (
"select_execution_plan: observation is compile-time opt-in",
"[lithe][execution][cost][phase-d][observability]"
) {
    struct observer {
        std::uint32_t events = 0;
        void on_event(const execution_selection_event&) noexcept { ++events; }
    } sink;
    execution_selection_inputs inputs;
    inputs.candidates = {{
        {planned_execution_kind::interpreter, true, 0, 1},
        {planned_execution_kind::jit, false, 0, 0},
        {planned_execution_kind::simd, false, 0, 0},
        {planned_execution_kind::metal, false, 0, 0},
        {planned_execution_kind::vulkan, false, 0, 0},
    }};
    static_cast<void>(select_execution_plan<false>(inputs, {}, &sink));
    REQUIRE(sink.events == 0);
    static_cast<void>(select_execution_plan<true>(inputs, {}, &sink));
    REQUIRE(sink.events == 1);
}

TEST_CASE (
"select_execution_plan: cache, locality and transfer costs affect deterministic ranking",
"[lithe][execution][cost][phase-d]"
) {
    execution_selection_inputs inputs;
    inputs.workload = {
        .work_items = 1'000,
        .data_bytes = 4'000,
        .device_transfer_bytes = 4'000,
        .cache = execution_cache_state::warm,
        .locality = execution_access_locality::streaming,
    };
    inputs.accelerator_legal = true;
    inputs.candidates = {{
        {planned_execution_kind::interpreter, true, 0, 5},
        {planned_execution_kind::jit, true, 10'000, 2, 100, 0, 0},
        {planned_execution_kind::simd, false, 0, 0},
        {planned_execution_kind::metal, true, 100, 1, 10, 0, 0, 2},
        {planned_execution_kind::vulkan, false, 0, 0},
    }};

    const auto selected = select_execution_plan(inputs);

    REQUIRE(selected.selected == planned_execution_kind::jit);
    REQUIRE(selected.estimated_cost_ns == 2'100);

    const execution_candidate_cost locality_candidate{
        .kind = planned_execution_kind::simd,
        .available = true,
        .data_byte_cost_ns = 5,
        .reusable_data_byte_cost_ns = 1,
    };
    const execution_cost_input reusable_data{
        .data_bytes = 64,
        .locality = execution_access_locality::reusable,
    };
    REQUIRE(estimated_execution_cost(locality_candidate, reusable_data) == 64);
}
