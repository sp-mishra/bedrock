#include "catch_amalgamated.hpp"

#include "lithe/lithe_codegen_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
    using namespace lithe::codegen;

    allocated_instruction make_branch(std::uint32_t id, std::uint32_t target) {
        allocated_instruction inst;
        inst.id = id;
        inst.op = opcode::branch;
        inst.uses = {allocated_operand::as_block(target)};
        return inst;
    }

    allocated_instruction make_ret(std::uint32_t id) {
        allocated_instruction inst;
        inst.id = id;
        inst.op = opcode::ret;
        return inst;
    }

    allocated_instruction make_add(std::uint32_t id) {
        allocated_instruction inst;
        inst.id = id;
        inst.op = opcode::add;
        inst.defs = {allocated_operand::as_preg(preg{1, "r1"})};
        inst.uses = {
            allocated_operand::as_preg(preg{2, "r2"}),
            allocated_operand::as_preg(preg{3, "r3"})
        };
        return inst;
    }

    allocated_basic_block make_block(
        std::uint32_t id,
        std::vector<allocated_instruction> instructions,
        std::vector<std::uint32_t> successors = {}
    ) {
        allocated_basic_block block;
        block.id = id;
        block.name = "bb" + std::to_string(id);
        block.instructions = std::move(instructions);
        block.successors = std::move(successors);
        return block;
    }

    mir::physical_mir_function make_physical(
        std::vector<allocated_basic_block> blocks,
        std::uint32_t entry_block
    ) {
        allocated_function_ir fn;
        fn.name = "cfg_test";
        fn.blocks = std::move(blocks);
        fn.cfg.entry_block = entry_block;

        for (const auto& block : fn.blocks) {
            fn.cfg.successors[block.id] = block.successors;
            for (const auto succ : block.successors) {
                fn.cfg.predecessors[succ].push_back(block.id);
            }
        }

        mir::physical_mir_function out;
        out.function = std::move(fn);
        out.metadata.current_phase = mir::phase::physical_mir;
        return out;
    }

    bool has_diag(const mir::verification_result& result, std::string_view needle) {
        return std::ranges::any_of(result.diagnostics, [&](const std::string& diag) {
            return diag.find(needle) != std::string::npos;
        });
    }

    bool has_cfg_diag(const cfg_analysis_result& result, std::string_view needle) {
        return std::ranges::any_of(result.diagnostics, [&](const std::string& diag) {
            return diag.find(needle) != std::string::npos;
        });
    }

    bool has_rd_diag(const reaching_definitions_result& result, std::string_view needle) {
        return std::ranges::any_of(result.diagnostics, [&](const std::string& diag) {
            return diag.find(needle) != std::string::npos;
        });
    }

    // Defines dst_preg, uses src_preg (mov).
    allocated_instruction make_mov(std::uint32_t id, preg dst, preg src) {
        allocated_instruction inst;
        inst.id = id;
        inst.op = opcode::mov;
        inst.defs = {allocated_operand::as_preg(dst)};
        inst.uses = {allocated_operand::as_preg(src)};
        return inst;
    }

    // Uses a preg with no def (ret-like instruction that only has uses).
    allocated_instruction make_use_only(std::uint32_t id, preg used) {
        allocated_instruction inst;
        inst.id = id;
        inst.op = opcode::ret;
        inst.uses = {allocated_operand::as_preg(used)};
        return inst;
    }

    // A plain def-only instruction (load_imm-like): defines dst, no preg uses.
    allocated_instruction make_def_only(std::uint32_t id, preg dst) {
        allocated_instruction inst;
        inst.id = id;
        inst.op = opcode::load_imm;
        inst.defs = {allocated_operand::as_preg(dst)};
        inst.uses = {allocated_operand::as_i64(42)};
        return inst;
    }
}

TEST_CASE (

"MIR CFG analysis tracks reachable, unreachable, and exit blocks"
,
"[lithe][mir][cfg]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_branch(2, 3)}, {3}),
            make_block(3, {make_ret(3)}),
            make_block(4, {make_ret(4)})
        },
        1
    );

    const auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());
    REQUIRE(cfg.entry_block == 1);
    REQUIRE(cfg.reachable_blocks == std::vector<std::uint32_t>{1, 2, 3});
    REQUIRE(cfg.unreachable_blocks == std::vector<std::uint32_t>{4});
    REQUIRE(cfg.exit_blocks == std::vector<std::uint32_t>{3});

    REQUIRE(compute_reachable_blocks(cfg) == std::vector<std::uint32_t>{1, 2, 3});
    REQUIRE(compute_exit_blocks(cfg) == std::vector<std::uint32_t>{3});

    REQUIRE(reverse_postorder_block_order(cfg) == std::vector<std::uint32_t>{1, 2, 3});
    REQUIRE(topological_block_order(cfg) == std::vector<std::uint32_t>{1, 2, 3});
}

TEST_CASE (
"affine_induction_strength_reduction_pass: rewrites a canonical zero-based affine loop",
"[lithe][mir][strength-reduction]"
) {
    allocated_instruction condition;
    condition.id = 2;
    condition.op = opcode::branch_cond;
    condition.uses = {allocated_operand::as_preg(preg{9, "cond"}),
                      allocated_operand::as_block(3), allocated_operand::as_block(5)};

    allocated_instruction multiply;
    multiply.id = 3;
    multiply.op = opcode::mul;
    multiply.defs = {allocated_operand::as_preg(preg{4, "scaled"})};
    multiply.uses = {allocated_operand::as_preg(preg{1, "iv"}), allocated_operand::as_i64(4)};

    allocated_instruction address;
    address.id = 4;
    address.op = opcode::add;
    address.defs = {allocated_operand::as_preg(preg{5, "address"})};
    address.uses = {allocated_operand::as_preg(preg{2, "base"}),
                    allocated_operand::as_preg(preg{4, "scaled"})};

    allocated_instruction advance;
    advance.id = 5;
    advance.op = opcode::add;
    advance.defs = {allocated_operand::as_preg(preg{1, "iv"})};
    advance.uses = {allocated_operand::as_preg(preg{1, "iv"}), allocated_operand::as_i64(1)};

    auto fn = make_physical({
        make_block(1, {make_branch(1, 2)}, {2}),
        make_block(2, {condition}, {3, 5}),
        make_block(3, {multiply, address, make_branch(6, 4)}, {4}),
        make_block(4, {advance, make_branch(7, 2)}, {2}),
        make_block(5, {make_ret(8)})
    }, 1);
    fn.canonical_loops.push_back({1, 2, 4, 5, preg{1, "iv"}, 0, 64, 1});
    fn.affine_addresses.push_back({2, 3, 4, preg{1, "iv"}, preg{2, "base"},
                                   preg{4, "scaled"}, preg{5, "address"}, 4});

    mir_pass_context context;
    const auto result = affine_induction_strength_reduction_pass{}.run(fn, context);
    REQUIRE(result.changed);
    REQUIRE(result.function.affine_addresses[0].strength_reduced);

    const auto& body = *std::ranges::find(result.function.function.blocks, 3u,
                                           &allocated_basic_block::id);
    REQUIRE(std::ranges::none_of(body.instructions, [](const allocated_instruction& inst) {
        return inst.id == 3;
    }));
    const auto address_it = std::ranges::find(body.instructions, 4u, &allocated_instruction::id);
    REQUIRE(address_it != body.instructions.end());
    REQUIRE(address_it->op == opcode::mov);
}

TEST_CASE (
"affine_induction_strength_reduction_pass: preserves a non-zero-based loop",
"[lithe][mir][strength-reduction]"
) {
    auto fn = make_physical({
        make_block(1, {make_branch(1, 2)}, {2}),
        make_block(2, {make_branch(2, 3)}, {3}),
        make_block(3, {make_ret(3)})
    }, 1);
    fn.canonical_loops.push_back({1, 2, 2, 3, preg{1, "iv"}, 1, 8, 1});
    fn.affine_addresses.push_back({2, 0, 0, preg{1, "iv"}, preg{2, "base"},
                                   preg{3, "scaled"}, preg{4, "address"}, 4});

    mir_pass_context context;
    const auto result = affine_induction_strength_reduction_pass{}.run(fn, context);
    REQUIRE_FALSE(result.changed);
    REQUIRE_FALSE(result.function.affine_addresses[0].strength_reduced);
}

TEST_CASE (
"verify_physical_mir: rejects invalid optional loop descriptors",
"[lithe][mir][descriptor]"
) {
    auto fn = make_physical({make_block(1, {make_ret(1)})}, 1);
    fn.canonical_loops.push_back({1, 2, 3, 4, preg{1, "iv"}, 0, 8, 1});
    const auto verification = verify_physical_mir(fn);
    REQUIRE_FALSE(verification.ok());
    REQUIRE(has_diag(verification, "invalid canonical loop descriptor"));
}

TEST_CASE (

"MIR CFG topological order remains usable with cycles"
,
"[lithe][mir][cfg]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(10, 2)}, {2}),
            make_block(2, {make_branch(11, 3)}, {3}),
            make_block(3, {make_branch(12, 2)}, {2, 4}),
            make_block(4, {make_ret(13)})
        },
        1
    );

    const auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());

    const auto topo = topological_block_order(cfg);
    REQUIRE(topo.size() == 4);
    REQUIRE(topo.front() == 1);

    for (const auto block_id : std::array<std::uint32_t, 4>{1, 2, 3, 4}) {
        REQUIRE(std::ranges::find(topo, block_id) != topo.end());
    }
}

TEST_CASE (

"MIR CFG validation reports core structural errors"
,
"[lithe][mir][cfg]"
)
 {
    auto invalid = make_physical(
        {
            make_block(1, {make_branch(20, 99), make_add(21)}),
            make_block(1, {make_ret(22)})
        },
        42
    );

    const auto validation = validate_cfg(invalid);
    REQUIRE_FALSE(validation.ok());
    REQUIRE(has_diag(validation, "duplicate block id"));
    REQUIRE(has_diag(validation, "entry block is missing"));
    REQUIRE(has_diag(validation, "invalid branch target bb99"));
    REQUIRE(has_diag(validation, "non-final terminator"));
}

TEST_CASE (

"MIR physical verifier reuses CFG validation diagnostics"
,
"[lithe][mir][cfg]"
)
 {
    auto invalid = make_physical(
        {
            make_block(1, {make_branch(30, 99), make_add(31)})
        },
        1
    );

    const auto verification = verify_physical_mir(invalid);
    REQUIRE_FALSE(verification.ok());
    REQUIRE(has_diag(verification, "invalid branch target bb99"));
    REQUIRE(has_diag(verification, "non-final terminator"));
}

TEST_CASE (

"MIR CFG analysis reports diagnostics for invalid CFG"
,
"[lithe][mir][cfg]"
)
 {
    auto invalid = make_physical(
        {
            make_block(1, {make_branch(40, 99)}, {99}),
            make_block(2, {make_ret(41)})
        },
        42
    );

    const auto result = analyze_cfg(invalid);

    REQUIRE_FALSE(result.ok());
    REQUIRE(!result.diagnostics.empty());
    REQUIRE(has_cfg_diag(result, "entry block is missing"));
    REQUIRE(has_cfg_diag(result, "bb99"));
}

TEST_CASE (

"MIR physical verifier surfaces analyze_cfg missing-block diagnostics"
,
"[lithe][mir][cfg]"
)
 {
    allocated_basic_block b;
    b.id = 1;
    b.name = "bb1";
    b.instructions = {make_ret(1)};
    b.successors = {99};

    allocated_function_ir fn;
    fn.name = "missing_succ_test";
    fn.blocks = {b};
    fn.cfg.entry_block = 1;
    fn.cfg.successors[1] = {99};

    mir::physical_mir_function phys;
    phys.function = std::move(fn);
    phys.metadata.current_phase = mir::phase::physical_mir;

    const auto result = verify_physical_mir(phys);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_diag(result, "missing successor bb99"));
}

TEST_CASE (

"MIR physical verifier surfaces analyze_cfg missing-predecessor diagnostics"
,
"[lithe][mir][cfg]"
)
 {
    allocated_basic_block b;
    b.id = 1;
    b.name = "bb1";
    b.instructions = {make_ret(1)};
    b.predecessors = {99};

    allocated_function_ir fn;
    fn.name = "missing_pred_test";
    fn.blocks = {b};
    fn.cfg.entry_block = 1;

    mir::physical_mir_function phys;
    phys.function = std::move(fn);
    phys.metadata.current_phase = mir::phase::physical_mir;

    const auto result = verify_physical_mir(phys);
    REQUIRE_FALSE(result.ok());
    REQUIRE(has_diag(result, "missing predecessor bb99"));
}

TEST_CASE (

"MIR physical verifier does not duplicate analyze_cfg diagnostics"
,
"[lithe][mir][cfg]"
)
 {
    auto invalid = make_physical(
        {
            make_block(1, {make_ret(1)})
        },
        42
    );

    const auto result = verify_physical_mir(invalid);
    REQUIRE_FALSE(result.ok());

    const std::string needle = "entry block is missing";
    const auto count = std::ranges::count_if(result.diagnostics, [&](const std::string& d) {
        return d.find(needle) != std::string::npos;
    });
    REQUIRE(count == 1);
}

TEST_CASE (

"MIR physical verifier passes for valid single-block function"
,
"[lithe][mir][cfg]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_ret(1)})
        },
        1
    );

    const auto result = verify_physical_mir(fn);
    REQUIRE(result.ok());
}

// -----------------------------------------------------------------------
// UBE pass tests
// -----------------------------------------------------------------------

TEST_CASE (

"unreachable_block_elimination_pass: no-op when all blocks reachable"
,
"[lithe][mir][pass][ube]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_branch(2, 3)}, {3}),
            make_block(3, {make_ret(3)})
        },
        1
    );

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE_FALSE(result.changed);
    REQUIRE(result.removed_blocks == 0);
    REQUIRE(result.function.function.blocks.size() == 3);
    REQUIRE_FALSE(ctx.changed);
}

TEST_CASE (

"unreachable_block_elimination_pass: removes one unreachable block"
,
"[lithe][mir][pass][ube]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_branch(2, 3)}, {3}),
            make_block(3, {make_ret(3)}),
            make_block(4, {make_ret(4)})
        },
        1
    );

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_blocks == 1);
    REQUIRE(ctx.changed);

    const auto& blocks = result.function.function.blocks;
    REQUIRE(blocks.size() == 3);
    REQUIRE(std::ranges::none_of(blocks, [](const auto& b) { return b.id == 4; }));
    REQUIRE(std::ranges::any_of(blocks, [](const auto& b) { return b.id == 1; }));
}

TEST_CASE (

"unreachable_block_elimination_pass: removes multiple unreachable blocks"
,
"[lithe][mir][pass][ube]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_ret(2)}),
            make_block(3, {make_branch(3, 4)}, {4}),
            make_block(4, {make_ret(4)})
        },
        1
    );

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_blocks == 2);
    REQUIRE(ctx.changed);

    const auto& blocks = result.function.function.blocks;
    REQUIRE(blocks.size() == 2);
    REQUIRE(std::ranges::none_of(blocks, [](const auto& b) { return b.id == 3; }));
    REQUIRE(std::ranges::none_of(blocks, [](const auto& b) { return b.id == 4; }));
}

TEST_CASE (

"unreachable_block_elimination_pass: preserves entry block"
,
"[lithe][mir][pass][ube]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_ret(1)}),
            make_block(2, {make_ret(2)})
        },
        1
    );

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_blocks == 1);

    const auto& blocks = result.function.function.blocks;
    REQUIRE(std::ranges::any_of(blocks, [](const auto& b) { return b.id == 1; }));
    REQUIRE(std::ranges::none_of(blocks, [](const auto& b) { return b.id == 2; }));
}

TEST_CASE (

"unreachable_block_elimination_pass: CFG maps are consistent after removal"
,
"[lithe][mir][pass][ube]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_branch(2, 3)}, {3}),
            make_block(3, {make_ret(3)}),
            make_block(4, {make_branch(4, 3)}, {3})
        },
        1
    );

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_blocks == 1);

    const auto& out_fn = result.function.function;
    const auto it = std::ranges::find_if(out_fn.blocks, [](const auto& b) { return b.id == 3; });
    REQUIRE(it != out_fn.blocks.end());
    REQUIRE(std::ranges::none_of(it->predecessors, [](std::uint32_t p) { return p == 4; }));

    if (const auto cfg_it = out_fn.cfg.predecessors.find(3); cfg_it != out_fn.cfg.predecessors.end()) {
        REQUIRE(std::ranges::none_of(cfg_it->second, [](std::uint32_t p) { return p == 4; }));
    }
}

// -----------------------------------------------------------------------
// Value-flow analysis tests
// -----------------------------------------------------------------------

TEST_CASE (

"analyze_def_use: r1 defined once, used twice produces two-use chain"
,
"[lithe][mir][value_flow]"
)
 {
    using namespace lithe::codegen;
    const preg r1{1, "r1"};

    allocated_instruction def_inst = make_def_only(1, r1);

    allocated_instruction use1;
    use1.id = 2;
    use1.op = opcode::mov;
    use1.defs = {allocated_operand::as_preg(preg{9, "r9"})};
    use1.uses = {allocated_operand::as_preg(r1)};

    allocated_instruction use2 = make_use_only(3, r1);

    auto fn = make_physical(
        {make_block(1, {def_inst, use1, use2})},
        1
    );

    const auto result = analyze_def_use(fn);
    REQUIRE(result.ok());

    const auto it = result.def_use_chains.find(r1.id);
    REQUIRE(it != result.def_use_chains.end());
    REQUIRE(it->second.definition.instruction_id == 1);
    REQUIRE(it->second.uses.size() == 2);
}

TEST_CASE (

"analyze_def_use: r1 defined then redefined; use after redef maps to latest def"
,
"[lithe][mir][value_flow]"
)
 {
    using namespace lithe::codegen;
    const preg r1{1, "r1"};

    allocated_instruction def1 = make_def_only(1, r1);
    allocated_instruction def2 = make_def_only(2, r1);
    allocated_instruction use_inst = make_use_only(3, r1);

    auto fn = make_physical(
        {make_block(1, {def1, def2, use_inst})},
        1
    );

    const auto result = analyze_def_use(fn);
    REQUIRE(result.ok());

    const auto chain_it = result.def_use_chains.find(r1.id);
    REQUIRE(chain_it != result.def_use_chains.end());
    REQUIRE(chain_it->second.definition.instruction_id == 2);
    REQUIRE(chain_it->second.uses.size() == 1);

    bool found_use = false;
    for (const auto& udc : result.use_def_chains) {
        if (udc.use.instruction_id == 3) {
            found_use = true;
            REQUIRE(udc.reaching_definitions.size() == 1);
            REQUIRE(udc.reaching_definitions[0].instruction_id == 2);
        }
    }
    REQUIRE(found_use);
}

TEST_CASE (

"compute_reaching_definitions: two-predecessor join emits ambiguity diagnostic"
,
"[lithe][mir][value_flow]"
)
 {
    using namespace lithe::codegen;
    const preg r1{1, "r1"};

    allocated_instruction entry_branch;
    entry_branch.id = 1;
    entry_branch.op = opcode::branch_cond;
    entry_branch.uses = {
        allocated_operand::as_preg(preg{10, "r10"}),
        allocated_operand::as_block(2),
        allocated_operand::as_block(3)
    };

    allocated_instruction def_in_bb2 = make_def_only(10, r1);
    allocated_instruction branch_to_bb4_from_bb2 = make_branch(11, 4);

    allocated_instruction def_in_bb3 = make_def_only(20, r1);
    allocated_instruction branch_to_bb4_from_bb3 = make_branch(21, 4);

    allocated_instruction use_r1 = make_use_only(30, r1);

    auto fn = make_physical(
        {
            make_block(1, {entry_branch},                        {2, 3}),
            make_block(2, {def_in_bb2, branch_to_bb4_from_bb2}, {4}),
            make_block(3, {def_in_bb3, branch_to_bb4_from_bb3}, {4}),
            make_block(4, {use_r1})
        },
        1
    );

    fn.function.blocks[3].predecessors = {2, 3};

    const auto result = compute_reaching_definitions(fn);

    REQUIRE(has_rd_diag(result, "ambiguous reaching def"));
    REQUIRE(has_rd_diag(result, "preg 1"));
    REQUIRE(has_rd_diag(result, "bb4"));

    std::size_t ambig_count = 0;
    for (const auto& d : result.diagnostics) {
        if (d.find("ambiguous") != std::string::npos) ++ambig_count;
    }
    REQUIRE(ambig_count == 1);
}

TEST_CASE (

"analysis cache: get_or_compute_def_use returns stable results"
,
"[lithe][mir][value_flow]"
)
 {
    using namespace lithe::codegen;
    const preg r1{1, "r1"};

    allocated_instruction def_inst = make_def_only(1, r1);
    allocated_instruction use_inst = make_use_only(2, r1);

    auto fn = make_physical(
        {make_block(1, {def_inst, use_inst})},
        1
    );

    mir_pass_context ctx;
    REQUIRE_FALSE(ctx.analysis_cache.def_use.has_value());

    const auto& first  = get_or_compute_def_use(ctx, fn);
    const auto& second = get_or_compute_def_use(ctx, fn);

    REQUIRE(&first == &second);
    REQUIRE(ctx.analysis_cache.def_use.has_value());

    const auto it = first.def_use_chains.find(r1.id);
    REQUIRE(it != first.def_use_chains.end());
    REQUIRE(it->second.uses.size() == 1);
}

// -----------------------------------------------------------------------
// Dead-def elimination tests
// -----------------------------------------------------------------------

TEST_CASE (

"dead_def_elimination_pass: instruction defining unused preg is removed"
,
"[lithe][mir][pass][dde]"
)
 {
    using namespace lithe::codegen;
    const preg r1{1, "r1"};

    allocated_instruction dead_def = make_def_only(1, r1);
    allocated_instruction ret_inst = make_ret(2);

    auto fn = make_physical(
        {make_block(1, {dead_def, ret_inst})},
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_instructions == 1);
    REQUIRE(ctx.changed);

    const auto& block = result.function.function.blocks[0];
    REQUIRE(block.instructions.size() == 1);
    REQUIRE(block.instructions[0].op == opcode::ret);
}

TEST_CASE (

"dead_def_elimination_pass: instruction defining used preg is preserved"
,
"[lithe][mir][pass][dde]"
)
 {
    using namespace lithe::codegen;
    const preg r1{1, "r1"};

    allocated_instruction def_inst = make_def_only(1, r1);
    allocated_instruction use_inst = make_use_only(2, r1);

    auto fn = make_physical(
        {make_block(1, {def_inst, use_inst})},
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE_FALSE(result.changed);
    REQUIRE(result.removed_instructions == 0);
    REQUIRE(result.function.function.blocks[0].instructions.size() == 2);
}

TEST_CASE (

"dead_def_elimination_pass: ret/branch/call/store/load_spill/store_spill preserved"
,
"[lithe][mir][pass][dde]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction ret_inst = make_ret(1);

    allocated_instruction branch_inst;
    branch_inst.id = 2;
    branch_inst.op = opcode::branch;
    branch_inst.uses = {allocated_operand::as_block(99)};

    allocated_instruction call_inst;
    call_inst.id = 3;
    call_inst.op = opcode::call;

    allocated_instruction store_inst;
    store_inst.id = 4;
    store_inst.op = opcode::store;
    store_inst.defs = {allocated_operand::as_preg(preg{5, "r5"})};

    allocated_instruction store_spill_inst;
    store_spill_inst.id = 5;
    store_spill_inst.op = opcode::store_spill;
    store_spill_inst.defs = {allocated_operand::as_preg(preg{6, "r6"})};

    allocated_instruction load_spill_inst;
    load_spill_inst.id = 6;
    load_spill_inst.op = opcode::load_spill;
    load_spill_inst.defs = {allocated_operand::as_preg(preg{7, "r7"})};

    auto fn = make_physical(
        {make_block(1, {store_inst, store_spill_inst, load_spill_inst, call_inst, ret_inst})},
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE_FALSE(result.changed);
    REQUIRE(result.removed_instructions == 0);
    REQUIRE(result.function.function.blocks[0].instructions.size() == 5);
}

TEST_CASE (

"dead_def_elimination_pass: reports removed instruction count in statistics"
,
"[lithe][mir][pass][dde]"
)
 {
    using namespace lithe::codegen;
    const preg r1{1, "r1"};
    const preg r2{2, "r2"};

    allocated_instruction dead1 = make_def_only(1, r1);
    allocated_instruction dead2 = make_def_only(2, r2);
    allocated_instruction ret_inst = make_ret(3);

    auto fn = make_physical(
        {make_block(1, {dead1, dead2, ret_inst})},
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_instructions == 2);
    REQUIRE(ctx.statistics.total_removed_instructions == 2);
    REQUIRE(ctx.statistics.removed_instructions_by_pass.contains("dead_def_elimination"));
    REQUIRE(ctx.statistics.removed_instructions_by_pass.at("dead_def_elimination") == 2);
}

// -----------------------------------------------------------------------
// Dominance-aware dead-def elimination tests
// -----------------------------------------------------------------------

TEST_CASE (

"dead_def_elimination_pass: removes truly unused definition across two blocks"
,
"[lithe][mir][pass][dde][cross_block]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};

    auto fn = make_physical(
        {
            make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
            make_block(2, {make_ret(3)})
        },
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_instructions == 1);
    REQUIRE(result.function.function.blocks[0].instructions.size() == 1);
    REQUIRE(result.function.function.blocks[0].instructions[0].op == opcode::branch);
}

TEST_CASE (

"dead_def_elimination_pass: preserves def used in dominated successor block"
,
"[lithe][mir][pass][dde][cross_block]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};

    auto fn = make_physical(
        {
            make_block(1, {make_def_only(1, r1), make_branch(2, 2)}, {2}),
            make_block(2, {make_use_only(3, r1)})
        },
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE_FALSE(result.changed);
    REQUIRE(result.removed_instructions == 0);
    REQUIRE(result.function.function.blocks[0].instructions.size() == 2);
}

TEST_CASE (

"dead_def_elimination_pass: preserves def with ambiguous reaching uses at merge"
,
"[lithe][mir][pass][dde][cross_block]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r5{5, "r5"};

    allocated_instruction brc;
    brc.id = 100;
    brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};

    allocated_instruction li2 = make_def_only(101, r1);
    allocated_instruction li3 = make_def_only(102, r1);

    auto fn = make_physical(
        {
            make_block(1, {brc},                               {2, 3}),
            make_block(2, {li2, make_branch(103, 4)},          {4}),
            make_block(3, {li3, make_branch(104, 4)},          {4}),
            make_block(4, {make_use_only(105, r1)})
        },
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.function.function.blocks[1].instructions.size() == 2);
    REQUIRE(result.function.function.blocks[2].instructions.size() == 2);
}

TEST_CASE (

"dead_def_elimination_pass: preserves side-effect instructions in multi-block function"
,
"[lithe][mir][pass][dde][cross_block]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction call_inst;
    call_inst.id = 200;
    call_inst.op = opcode::call;

    allocated_instruction store_inst;
    store_inst.id = 201;
    store_inst.op = opcode::store;
    store_inst.defs = {allocated_operand::as_preg(preg{9, "r9"})};

    allocated_instruction load_spill_inst;
    load_spill_inst.id = 202;
    load_spill_inst.op = opcode::load_spill;
    load_spill_inst.defs = {allocated_operand::as_preg(preg{10, "r10"})};

    allocated_instruction store_spill_inst;
    store_spill_inst.id = 203;
    store_spill_inst.op = opcode::store_spill;
    store_spill_inst.defs = {allocated_operand::as_preg(preg{11, "r11"})};

    auto fn = make_physical(
        {
            make_block(1, {call_inst, store_inst, load_spill_inst, store_spill_inst, make_branch(204, 2)}, {2}),
            make_block(2, {make_ret(205)})
        },
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE_FALSE(result.changed);
    REQUIRE(result.removed_instructions == 0);
    REQUIRE(result.function.function.blocks[0].instructions.size() == 5);
}

TEST_CASE (

"dead_def_elimination_pass: preserves loop-carried definition"
,
"[lithe][mir][pass][dde][cross_block]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};
    const preg r5{5, "cond"};

    allocated_instruction li_header = make_def_only(300, r1);

    allocated_instruction brc_header;
    brc_header.id = 301;
    brc_header.op = opcode::branch_cond;
    brc_header.uses = {allocated_operand::as_preg(r5),
                       allocated_operand::as_block(3),
                       allocated_operand::as_block(4)};

    allocated_instruction add_body;
    add_body.id = 302;
    add_body.op = opcode::add;
    add_body.defs = {allocated_operand::as_preg(r1)};
    add_body.uses = {allocated_operand::as_preg(r1), allocated_operand::as_preg(r2)};

    auto fn = make_physical(
        {
            make_block(1, {make_branch(303, 2)},                    {2}),
            make_block(2, {li_header, brc_header},                  {3, 4}),
            make_block(3, {add_body, make_branch(304, 2)},          {2}),
            make_block(4, {make_use_only(305, r1)})
        },
        1
    );

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    const auto& header_block = result.function.function.blocks[1];
    bool found_li = std::ranges::any_of(header_block.instructions, [](const auto& i) {
        return i.id == 300;
    });
    REQUIRE(found_li);
}

TEST_CASE (

"to_dominator_graph_view: converts cfg_analysis_result to correct graph view"
,
"[lithe][mir][cfg][dominator]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(10, 2)}, {2}),
            make_block(2, {make_branch(20, 3)}, {3}),
            make_block(3, {make_ret(30)},       {})
        },
        1
    );

    const auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());

    const auto view = to_dominator_graph_view(cfg);

    REQUIRE(view.entry == 1u);

    const std::unordered_set<std::uint32_t> node_set(view.nodes.begin(), view.nodes.end());
    REQUIRE(node_set.count(1) == 1);
    REQUIRE(node_set.count(2) == 1);
    REQUIRE(node_set.count(3) == 1);
    REQUIRE(node_set.size() == 3);

    REQUIRE(view.successors.count(1) == 1);
    REQUIRE(view.successors.at(1) == std::vector<std::uint32_t>{2});
    REQUIRE(view.successors.count(2) == 1);
    REQUIRE(view.successors.at(2) == std::vector<std::uint32_t>{3});

    REQUIRE(view.predecessors.count(2) == 1);
    REQUIRE(view.predecessors.at(2) == std::vector<std::uint32_t>{1});
    REQUIRE(view.predecessors.count(3) == 1);
    REQUIRE(view.predecessors.at(3) == std::vector<std::uint32_t>{2});
}

// -----------------------------------------------------------------------
// CFG edge_kind classification tests
// -----------------------------------------------------------------------

TEST_CASE (

"edge_kind: fallthrough edges are emitted for blocks without explicit branch"
,
"[lithe][mir][cfg][edge_kind]"
)
 {
    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "bb1";
    bb1.instructions = {make_add(10)};
    bb1.successors = {2};

    auto fn = make_physical({bb1, make_block(2, {make_ret(20)})}, 1);

    const auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());
    REQUIRE(cfg.typed_edges.size() == 1);
    REQUIRE(cfg.typed_edges[0].from == 1u);
    REQUIRE(cfg.typed_edges[0].to   == 2u);
    REQUIRE(cfg.typed_edges[0].kind == edge_kind::fallthrough);
}

TEST_CASE (

"edge_kind: unconditional branch edges are classified as sync_branch"
,
"[lithe][mir][cfg][edge_kind]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(10, 2)}, {2}),
            make_block(2, {make_ret(20)})
        },
        1
    );

    const auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());
    REQUIRE(cfg.typed_edges.size() == 1);
    REQUIRE(cfg.typed_edges[0].from == 1u);
    REQUIRE(cfg.typed_edges[0].to   == 2u);
    REQUIRE(cfg.typed_edges[0].kind == edge_kind::sync_branch);
}

TEST_CASE (

"edge_kind: conditional branch edges are classified as sync_branch"
,
"[lithe][mir][cfg][edge_kind]"
)
 {
    const preg r5{5, "cond"};

    allocated_instruction brc;
    brc.id = 10;
    brc.op = opcode::branch_cond;
    brc.uses = {
        allocated_operand::as_preg(r5),
        allocated_operand::as_block(2),
        allocated_operand::as_block(3)
    };

    auto fn = make_physical(
        {
            make_block(1, {brc}, {2, 3}),
            make_block(2, {make_ret(20)}),
            make_block(3, {make_ret(30)})
        },
        1
    );

    const auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());

    std::size_t sb_count = 0;
    for (const auto& te : cfg.typed_edges) {
        if (te.from == 1u) {
            REQUIRE(te.kind == edge_kind::sync_branch);
            ++sb_count;
        }
    }
    REQUIRE(sb_count == 2);
}

TEST_CASE (

"edge_kind: typed_edges parallel to edges — same count and from/to pairs"
,
"[lithe][mir][cfg][edge_kind]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_branch(2, 3)}, {3}),
            make_block(3, {make_ret(3)})
        },
        1
    );

    const auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());
    REQUIRE(cfg.typed_edges.size() == cfg.edges.size());

    for (std::size_t i = 0; i < cfg.edges.size(); ++i) {
        REQUIRE(cfg.typed_edges[i].from == cfg.edges[i].from);
        REQUIRE(cfg.typed_edges[i].to   == cfg.edges[i].to);
    }
}

// -----------------------------------------------------------------------
// Subgraph partitioning tests
// -----------------------------------------------------------------------

static cfg_analysis_result inject_edge_kind(
    mir::physical_mir_function const& fn,
    std::uint32_t from, std::uint32_t to,
    edge_kind kind) {
    auto cfg = analyze_cfg(fn);
    for (auto& te : cfg.typed_edges) {
        if (te.from == from && te.to == to) {
            te.kind = kind;
        }
    }
    cfg.partition = partition_execution_domains(cfg);
    return cfg;
}

TEST_CASE (

"subgraph_partition: sequential CFG produces single domain covering all reachable blocks"
,
"[lithe][mir][cfg][partition]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_branch(2, 3)}, {3}),
            make_block(3, {make_ret(3)})
        },
        1
    );

    const auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());

    REQUIRE_FALSE(cfg.partition.has_value());

    const auto part = partition_execution_domains(cfg);
    REQUIRE(part.domains.size() == 1);
    REQUIRE(part.domains[0].domain_id == 0);
    REQUIRE_FALSE(part.is_partitioned());

    REQUIRE(part.domain_of(1) == 0u);
    REQUIRE(part.domain_of(2) == 0u);
    REQUIRE(part.domain_of(3) == 0u);
}

TEST_CASE (

"subgraph_partition: async_fork edge splits CFG into two domains"
,
"[lithe][mir][cfg][partition]"
)
 {
    const preg r5{5, "cond"};
    allocated_instruction brc;
    brc.id = 10;
    brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(4)};

    auto fn = make_physical(
        {
            make_block(1, {brc},                {2, 4}),
            make_block(2, {make_branch(20, 3)}, {3}),
            make_block(3, {make_ret(30)}),
            make_block(4, {make_ret(40)})
        },
        1
    );

    const auto cfg = inject_edge_kind(fn, 1, 4, edge_kind::async_fork);
    REQUIRE(cfg.ok());
    REQUIRE(cfg.partition.has_value());
    REQUIRE(cfg.partition->is_partitioned());

    REQUIRE(cfg.partition->domains.size() == 2);

    const auto dom_bb1 = cfg.partition->domain_of(1);
    const auto dom_bb4 = cfg.partition->domain_of(4);
    REQUIRE(dom_bb1 != dom_bb4);

    const auto& fork_domain = cfg.partition->domains[dom_bb4];
    REQUIRE(fork_domain.root_block == 4u);
    REQUIRE(fork_domain.spawned_by.has_value());
    REQUIRE(*fork_domain.spawned_by == edge_kind::async_fork);

    REQUIRE(cfg.partition->domain_of(1) == 0u);
    REQUIRE(cfg.partition->domain_of(2) == 0u);
    REQUIRE(cfg.partition->domain_of(3) == 0u);
}

TEST_CASE (

"subgraph_partition: rpc_boundary edge splits CFG into two domains"
,
"[lithe][mir][cfg][partition]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(10, 2)}, {2}),
            make_block(2, {make_ret(20)})
        },
        1
    );

    const auto cfg = inject_edge_kind(fn, 1, 2, edge_kind::rpc_boundary);
    REQUIRE(cfg.ok());
    REQUIRE(cfg.partition.has_value());
    REQUIRE(cfg.partition->is_partitioned());
    REQUIRE(cfg.partition->domains.size() == 2);

    const auto dom_bb2 = cfg.partition->domain_of(2);
    REQUIRE(dom_bb2 != 0u);
    REQUIRE(cfg.partition->domains[dom_bb2].spawned_by == edge_kind::rpc_boundary);
    REQUIRE(cfg.partition->domains[dom_bb2].root_block == 2u);

    REQUIRE(cfg.partition->domain_of(1) == 0u);
}

TEST_CASE (

"subgraph_partition: two independent async_fork edges produce three domains"
,
"[lithe][mir][cfg][partition]"
)
 {
    const preg r5{5, "cond"};
    allocated_instruction br3;
    br3.id = 10;
    br3.op = opcode::branch_cond;
    br3.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};

    auto fn = make_physical(
        {
            make_block(1, {br3}, {2, 3, 4}),
            make_block(2, {make_ret(20)}),
            make_block(3, {make_ret(30)}),
            make_block(4, {make_ret(40)})
        },
        1
    );

    auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());

    for (auto& te : cfg.typed_edges) {
        if (te.from == 1u && (te.to == 2u || te.to == 3u))
            te.kind = edge_kind::async_fork;
    }
    cfg.partition = partition_execution_domains(cfg);

    REQUIRE(cfg.partition.has_value());
    REQUIRE(cfg.partition->domains.size() == 3);
    REQUIRE(cfg.partition->is_partitioned());

    const auto d2 = cfg.partition->domain_of(2);
    const auto d3 = cfg.partition->domain_of(3);
    const auto d4 = cfg.partition->domain_of(4);

    REQUIRE(d2 != 0u);
    REQUIRE(d3 != 0u);
    REQUIRE(d4 == 0u);
    REQUIRE(d2 != d3);
}

TEST_CASE (

"subgraph_partition: blocks reachable only through fork belong exclusively to fork domain"
,
"[lithe][mir][cfg][partition]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(10, 2)}, {2}),
            make_block(2, {make_branch(20, 3)}, {3}),
            make_block(3, {make_ret(30)})
        },
        1
    );

    const auto cfg = inject_edge_kind(fn, 1, 2, edge_kind::async_fork);
    REQUIRE(cfg.ok());
    REQUIRE(cfg.partition.has_value());
    REQUIRE(cfg.partition->is_partitioned());

    const auto fork_domain_id = cfg.partition->domain_of(2);
    REQUIRE(fork_domain_id != 0u);
    REQUIRE(cfg.partition->domain_of(3) == fork_domain_id);
    REQUIRE(cfg.partition->domain_of(1) == 0u);
}

// -----------------------------------------------------------------------
// Cross-context dominance tests
// -----------------------------------------------------------------------

TEST_CASE (

"to_dominator_graph_view: async_fork edges are suppressed"
,
"[lithe][mir][cfg][dominator][edge_kind]"
)
 {
    const preg r5{5, "cond"};
    allocated_instruction brc;
    brc.id = 10;
    brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};

    auto fn = make_physical(
        {
            make_block(1, {brc}, {2, 3}),
            make_block(2, {make_ret(20)}),
            make_block(3, {make_ret(30)})
        },
        1
    );

    auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());

    for (auto& te : cfg.typed_edges) {
        if (te.from == 1u && te.to == 2u)
            te.kind = edge_kind::async_fork;
    }

    const auto view = to_dominator_graph_view(cfg);

    const auto& succs_1 = view.successors.at(1u);
    REQUIRE(std::ranges::find(succs_1, 2u) == succs_1.end());
    REQUIRE(std::ranges::find(succs_1, 3u) != succs_1.end());

    const auto& preds_2 = view.predecessors.at(2u);
    REQUIRE(preds_2.empty());
}

TEST_CASE (

"compute_dominators: standard chain dominance is unaffected by async_fork"
,
"[lithe][mir][cfg][dominator][edge_kind]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_branch(2, 3)}, {3}),
            make_block(3, {make_ret(3)})
        },
        1
    );

    const auto dom = compute_dominators(fn);
    REQUIRE(dom.ok());

    REQUIRE(dominates(dom, 1u, 2u));
    REQUIRE(dominates(dom, 1u, 3u));
    REQUIRE(dominates(dom, 2u, 3u));
    REQUIRE_FALSE(dominates(dom, 3u, 1u));

    REQUIRE(dom.sub_domain_doms.empty());
}

TEST_CASE (

"compute_dominators: async_fork target has null idom in root domain tree"
,
"[lithe][mir][cfg][dominator][edge_kind]"
)
 {
    const preg r5{5, "cond"};
    allocated_instruction brc;
    brc.id = 10;
    brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};

    auto fn = make_physical(
        {
            make_block(1, {brc}, {2, 3}),
            make_block(2, {make_ret(20)}),
            make_block(3, {make_ret(30)})
        },
        1
    );

    auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());

    for (auto& te : cfg.typed_edges) {
        if (te.from == 1u && te.to == 2u) te.kind = edge_kind::async_fork;
    }
    cfg.partition = partition_execution_domains(cfg);

    const auto view = to_dominator_graph_view(cfg);
    const auto dom_result = litegraph::compute_dominators(view, litegraph::dominator_options{});

    const auto it = dom_result.immediate_dominator.find(2u);
    const bool idom_is_null =
        (it == dom_result.immediate_dominator.end()) || !it->second.has_value();
    REQUIRE(idom_is_null);

    REQUIRE(litegraph::dominates(dom_result, 1u, 3u));
}

TEST_CASE (

"compute_dominators: per-domain sub-trees are built for async_fork CFG"
,
"[lithe][mir][cfg][dominator][edge_kind]"
)
 {
    const preg r5{5, "cond"};
    allocated_instruction brc;
    brc.id = 10;
    brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};

    auto fn = make_physical(
        {
            make_block(1, {brc},                {2, 3}),
            make_block(2, {make_branch(20, 5)}, {5}),
            make_block(3, {make_branch(30, 4)}, {4}),
            make_block(4, {make_ret(40)}),
            make_block(5, {make_ret(50)})
        },
        1
    );

    auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());

    for (auto& te : cfg.typed_edges) {
        if (te.from == 1u && te.to == 3u) te.kind = edge_kind::async_fork;
    }
    cfg.partition = partition_execution_domains(cfg);
    REQUIRE(cfg.partition.has_value());
    REQUIRE(cfg.partition->is_partitioned());

    const std::uint32_t fork_dom_id = cfg.partition->domain_of(3);
    REQUIRE(fork_dom_id != 0u);
    REQUIRE(cfg.partition->domain_of(4) == fork_dom_id);

    const auto view = to_dominator_graph_view(cfg);
    litegraph::dominator_options opts;
    opts.compute_frontier = false;
    const auto root_dom = litegraph::compute_dominators(view, opts);
    REQUIRE(root_dom.ok());

    REQUIRE(litegraph::dominates(root_dom, 1u, 2u));
    REQUIRE(litegraph::dominates(root_dom, 1u, 5u));
    REQUIRE(litegraph::dominates(root_dom, 2u, 5u));
    REQUIRE_FALSE(litegraph::dominates(root_dom, 1u, 3u));

    const auto& fork_domain = cfg.partition->domains[fork_dom_id];
    std::unordered_set<std::uint32_t> fork_set(
        fork_domain.blocks.begin(), fork_domain.blocks.end());

    litegraph::dominator_graph_view<std::uint32_t> fork_view;
    fork_view.entry = fork_domain.root_block;
    for (const auto bid : fork_domain.blocks) {
        fork_view.nodes.push_back(bid);
        fork_view.predecessors.try_emplace(bid);
        fork_view.successors.try_emplace(bid);
    }
    for (const auto& te : cfg.typed_edges) {
        if (!fork_set.count(te.from) || !fork_set.count(te.to)) continue;
        fork_view.successors[te.from].push_back(te.to);
        fork_view.predecessors[te.to].push_back(te.from);
    }

    const auto fork_dom = litegraph::compute_dominators(fork_view, opts);
    REQUIRE(fork_dom.ok());

    REQUIRE(litegraph::dominates(fork_dom, 3u, 4u));
    REQUIRE_FALSE(litegraph::dominates(fork_dom, 1u, 4u));
}

TEST_CASE (

"dominates_in_domain: routes queries to the correct sub-tree"
,
"[lithe][mir][cfg][dominator][edge_kind]"
)
 {
    const preg r5{5, "cond"};
    allocated_instruction brc;
    brc.id = 10;
    brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(r5),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};

    auto fn = make_physical(
        {
            make_block(1, {brc},                {2, 3}),
            make_block(2, {make_branch(20, 5)}, {5}),
            make_block(3, {make_branch(30, 4)}, {4}),
            make_block(4, {make_ret(40)}),
            make_block(5, {make_ret(50)})
        },
        1
    );

    auto cfg = analyze_cfg(fn);
    REQUIRE(cfg.ok());
    for (auto& te : cfg.typed_edges) {
        if (te.from == 1u && te.to == 3u) te.kind = edge_kind::async_fork;
    }
    cfg.partition = partition_execution_domains(cfg);
    REQUIRE(cfg.partition.has_value());

    const std::uint32_t fork_dom_id = cfg.partition->domain_of(3);

    litegraph::dominator_options opts{.compute_frontier = false};
    const auto view = to_dominator_graph_view(cfg);
    dominator_analysis_result dar;
    dar.dom = litegraph::compute_dominators(view, opts);

    const auto& fork_domain = cfg.partition->domains[fork_dom_id];
    std::unordered_set<std::uint32_t> fork_set(
        fork_domain.blocks.begin(), fork_domain.blocks.end());

    litegraph::dominator_graph_view<std::uint32_t> fork_view;
    fork_view.entry = fork_domain.root_block;
    for (const auto bid : fork_domain.blocks) {
        fork_view.nodes.push_back(bid);
        fork_view.predecessors.try_emplace(bid);
        fork_view.successors.try_emplace(bid);
    }
    for (const auto& te : cfg.typed_edges) {
        if (!fork_set.count(te.from) || !fork_set.count(te.to)) continue;
        fork_view.successors[te.from].push_back(te.to);
        fork_view.predecessors[te.to].push_back(te.from);
    }
    dar.sub_domain_doms[fork_dom_id] = litegraph::compute_dominators(fork_view, opts);

    REQUIRE(dar.dominates_in_domain(1u, 2u, 0u));
    REQUIRE(dar.dominates_in_domain(1u, 5u, 0u));
    REQUIRE_FALSE(dar.dominates_in_domain(3u, 4u, 0u));

    REQUIRE(dar.dominates_in_domain(3u, 4u, fork_dom_id));
    REQUIRE_FALSE(dar.dominates_in_domain(1u, 4u, fork_dom_id));
}

TEST_CASE (

"subgraph_partition: sync_join and entanglement edges do not trigger partitioning"
,
"[lithe][mir][cfg][partition]"
)
 {
    auto fn = make_physical(
        {
            make_block(1, {make_branch(1, 2)}, {2}),
            make_block(2, {make_ret(2)})
        },
        1
    );

    for (auto kind : {edge_kind::sync_branch, edge_kind::fallthrough,
                      edge_kind::sync_join,   edge_kind::entanglement}) {
        auto cfg = analyze_cfg(fn);
        REQUIRE(cfg.ok());
        for (auto& te : cfg.typed_edges) te.kind = kind;
        const auto part = partition_execution_domains(cfg);
        REQUIRE(part.domains.size() == 1);
        REQUIRE_FALSE(part.is_partitioned());
    }
}
