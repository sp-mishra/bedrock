// =============================================================================
// test_crank_cfg_exec.cpp — §v2.7 CFG-aware interpreter execution.
//
// Verifies the interpreter_backend now follows control-flow edges
// (branch / branch_cond) instead of bailing with "unsupported by interpreter
// backend". Physical MIR is built directly so the branch shapes are exact and
// independent of the HL lowering pipeline.
//
//  1.  Unconditional branch: entry → target block, target returns a value.
//  2.  branch_cond true edge: condition nonzero selects the then-block.
//  3.  branch_cond false edge: condition zero selects the else-block.
//  4.  Diamond: cond picks one of two blocks that both jump to a join block.
//  5.  Backward branch loop: a counted loop accumulates then exits.
//  6.  Straight-line MIR (no branch) still executes identically (regression).
//  7.  Dangling branch target is reported, not silently mis-executed.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"

#include <optional>
#include <string>

namespace {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // Extract the interpreter's scalar return value from the emitted artifact.
    std::optional<std::int64_t> return_of(const compilation_artifact& art) {
        const auto it = art.metadata.find("return_value");
        if (it == art.metadata.end()) return std::nullopt;
        return static_cast<std::int64_t>(std::stoll(it->second));
    }

    allocated_instruction load_imm(std::uint32_t id, std::uint16_t reg,
                                   const char* rname, std::int64_t imm) {
        allocated_instruction in;
        in.id = id;
        in.op = opcode::load_imm;
        in.defs = {allocated_operand::as_preg({reg, rname})};
        in.uses = {allocated_operand::as_i64(imm)};
        return in;
    }

    allocated_instruction ret_preg(std::uint32_t id, std::uint16_t reg, const char* rname) {
        allocated_instruction in;
        in.id = id;
        in.op = opcode::ret;
        in.uses = {allocated_operand::as_preg({reg, rname})};
        return in;
    }

    mir::physical_mir_function finalize(allocated_function_ir fn) {
        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }
} // namespace

// ---------------------------------------------------------------------------
// 1. Unconditional branch reaches the target block.
// ---------------------------------------------------------------------------
TEST_CASE (

"cfg exec: unconditional branch reaches target"
,
"[crank][cfg][v2]"
)
 {
    allocated_function_ir fn;
    fn.name = "uncond";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.name = "entry";
    {
        allocated_instruction br;
        br.id = 1;
        br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(2)};
        entry.instructions.push_back(br);
    }

    allocated_basic_block target;
    target.id = 2;
    target.name = "target";
    target.instructions.push_back(load_imm(2, 0, "r0", 7));
    target.instructions.push_back(ret_preg(3, 0, "r0"));

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(target));
    fn.cfg.successors[1] = {2};
    fn.cfg.predecessors[2] = {1};

    interpreter_backend interp;
    interp.reset_runtime_state();
    auto art = interp.emit(finalize(std::move(fn)));

    REQUIRE(art.diagnostics.empty());
    REQUIRE(return_of(art).has_value());
    CHECK(*return_of(art) == 7);
}

// ---------------------------------------------------------------------------
// 2. branch_cond selects the then-block when the condition is nonzero.
// ---------------------------------------------------------------------------
TEST_CASE (

"cfg exec: branch_cond true edge"
,
"[crank][cfg][v2]"
)
 {
    allocated_function_ir fn;
    fn.name = "cond_true";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.name = "entry";
    entry.instructions.push_back(load_imm(1, 1, "r1", 1)); // cond = 1 (true)
    {
        allocated_instruction bc;
        bc.id = 2;
        bc.op = opcode::branch_cond;
        bc.uses = {allocated_operand::as_preg({1, "r1"}),
                   allocated_operand::as_block(2),   // then
                   allocated_operand::as_block(3)};  // else
        entry.instructions.push_back(bc);
    }

    allocated_basic_block then_bb;
    then_bb.id = 2;
    then_bb.name = "then";
    then_bb.instructions.push_back(load_imm(3, 0, "r0", 100));
    then_bb.instructions.push_back(ret_preg(4, 0, "r0"));

    allocated_basic_block else_bb;
    else_bb.id = 3;
    else_bb.name = "else";
    else_bb.instructions.push_back(load_imm(5, 0, "r0", 200));
    else_bb.instructions.push_back(ret_preg(6, 0, "r0"));

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(then_bb));
    fn.blocks.push_back(std::move(else_bb));
    fn.cfg.successors[1] = {2, 3};
    fn.cfg.predecessors[2] = {1};
    fn.cfg.predecessors[3] = {1};

    interpreter_backend interp;
    interp.reset_runtime_state();
    auto art = interp.emit(finalize(std::move(fn)));

    REQUIRE(art.diagnostics.empty());
    REQUIRE(return_of(art).has_value());
    CHECK(*return_of(art) == 100);
}

// ---------------------------------------------------------------------------
// 3. branch_cond selects the else-block when the condition is zero.
// ---------------------------------------------------------------------------
TEST_CASE (

"cfg exec: branch_cond false edge"
,
"[crank][cfg][v2]"
)
 {
    allocated_function_ir fn;
    fn.name = "cond_false";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.name = "entry";
    entry.instructions.push_back(load_imm(1, 1, "r1", 0)); // cond = 0 (false)
    {
        allocated_instruction bc;
        bc.id = 2;
        bc.op = opcode::branch_cond;
        bc.uses = {allocated_operand::as_preg({1, "r1"}),
                   allocated_operand::as_block(2),
                   allocated_operand::as_block(3)};
        entry.instructions.push_back(bc);
    }

    allocated_basic_block then_bb;
    then_bb.id = 2;
    then_bb.name = "then";
    then_bb.instructions.push_back(load_imm(3, 0, "r0", 100));
    then_bb.instructions.push_back(ret_preg(4, 0, "r0"));

    allocated_basic_block else_bb;
    else_bb.id = 3;
    else_bb.name = "else";
    else_bb.instructions.push_back(load_imm(5, 0, "r0", 200));
    else_bb.instructions.push_back(ret_preg(6, 0, "r0"));

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(then_bb));
    fn.blocks.push_back(std::move(else_bb));
    fn.cfg.successors[1] = {2, 3};
    fn.cfg.predecessors[2] = {1};
    fn.cfg.predecessors[3] = {1};

    interpreter_backend interp;
    interp.reset_runtime_state();
    auto art = interp.emit(finalize(std::move(fn)));

    REQUIRE(art.diagnostics.empty());
    REQUIRE(return_of(art).has_value());
    CHECK(*return_of(art) == 200);
}

// ---------------------------------------------------------------------------
// 4. Diamond CFG: both arms converge on a join block.
// ---------------------------------------------------------------------------
TEST_CASE (

"cfg exec: diamond converges on join block"
,
"[crank][cfg][v2]"
)
 {
    allocated_function_ir fn;
    fn.name = "diamond";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.instructions.push_back(load_imm(1, 1, "r1", 0)); // → else arm
    {
        allocated_instruction bc;
        bc.id = 2;
        bc.op = opcode::branch_cond;
        bc.uses = {allocated_operand::as_preg({1, "r1"}),
                   allocated_operand::as_block(2),
                   allocated_operand::as_block(3)};
        entry.instructions.push_back(bc);
    }

    allocated_basic_block then_bb; // writes r0=11, jumps to join
    then_bb.id = 2;
    then_bb.instructions.push_back(load_imm(3, 0, "r0", 11));
    {
        allocated_instruction br;
        br.id = 4;
        br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(4)};
        then_bb.instructions.push_back(br);
    }

    allocated_basic_block else_bb; // writes r0=22, jumps to join
    else_bb.id = 3;
    else_bb.instructions.push_back(load_imm(5, 0, "r0", 22));
    {
        allocated_instruction br;
        br.id = 6;
        br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(4)};
        else_bb.instructions.push_back(br);
    }

    allocated_basic_block join_bb;
    join_bb.id = 4;
    join_bb.instructions.push_back(ret_preg(7, 0, "r0"));

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(then_bb));
    fn.blocks.push_back(std::move(else_bb));
    fn.blocks.push_back(std::move(join_bb));
    fn.cfg.successors[1] = {2, 3};
    fn.cfg.successors[2] = {4};
    fn.cfg.successors[3] = {4};
    fn.cfg.predecessors[2] = {1};
    fn.cfg.predecessors[3] = {1};
    fn.cfg.predecessors[4] = {2, 3};

    interpreter_backend interp;
    interp.reset_runtime_state();
    auto art = interp.emit(finalize(std::move(fn)));

    REQUIRE(art.diagnostics.empty());
    REQUIRE(return_of(art).has_value());
    CHECK(*return_of(art) == 22); // else arm taken (cond == 0)
}

// ---------------------------------------------------------------------------
// 5. Counted loop with a backward branch: sum 1..=3 == 6.
// ---------------------------------------------------------------------------
TEST_CASE (

"cfg exec: backward-branch counted loop"
,
"[crank][cfg][v2]"
)
 {
    // r0 = acc (0), r1 = i (1), r2 = limit (3), r3 = cond, r4 = one
    allocated_function_ir fn;
    fn.name = "loop_sum";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.instructions.push_back(load_imm(1, 0, "r0", 0)); // acc = 0
    entry.instructions.push_back(load_imm(2, 1, "r1", 1)); // i = 1
    entry.instructions.push_back(load_imm(3, 2, "r2", 3)); // limit = 3
    entry.instructions.push_back(load_imm(4, 4, "r4", 1)); // one = 1
    {
        allocated_instruction br;
        br.id = 5;
        br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(2)};
        entry.instructions.push_back(br);
    }

    // header: cond = (i <= limit); branch_cond → body else exit
    allocated_basic_block header;
    header.id = 2;
    {
        allocated_instruction cmp;
        cmp.id = 6;
        cmp.op = opcode::cmp_le;
        cmp.defs = {allocated_operand::as_preg({3, "r3"})};
        cmp.uses = {allocated_operand::as_preg({1, "r1"}),
                    allocated_operand::as_preg({2, "r2"})};
        header.instructions.push_back(cmp);

        allocated_instruction bc;
        bc.id = 7;
        bc.op = opcode::branch_cond;
        bc.uses = {allocated_operand::as_preg({3, "r3"}),
                   allocated_operand::as_block(3),   // body
                   allocated_operand::as_block(4)};  // exit
        header.instructions.push_back(bc);
    }

    // body: acc += i; i += 1; branch header
    allocated_basic_block body;
    body.id = 3;
    {
        allocated_instruction add_acc;
        add_acc.id = 8;
        add_acc.op = opcode::add;
        add_acc.defs = {allocated_operand::as_preg({0, "r0"})};
        add_acc.uses = {allocated_operand::as_preg({0, "r0"}),
                        allocated_operand::as_preg({1, "r1"})};
        body.instructions.push_back(add_acc);

        allocated_instruction inc_i;
        inc_i.id = 9;
        inc_i.op = opcode::add;
        inc_i.defs = {allocated_operand::as_preg({1, "r1"})};
        inc_i.uses = {allocated_operand::as_preg({1, "r1"}),
                      allocated_operand::as_preg({4, "r4"})};
        body.instructions.push_back(inc_i);

        allocated_instruction br;
        br.id = 10;
        br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(2)};
        body.instructions.push_back(br);
    }

    allocated_basic_block exit_bb;
    exit_bb.id = 4;
    exit_bb.instructions.push_back(ret_preg(11, 0, "r0"));

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(header));
    fn.blocks.push_back(std::move(body));
    fn.blocks.push_back(std::move(exit_bb));
    fn.cfg.successors[1] = {2};
    fn.cfg.successors[2] = {3, 4};
    fn.cfg.successors[3] = {2};
    fn.cfg.predecessors[2] = {1, 3};
    fn.cfg.predecessors[3] = {2};
    fn.cfg.predecessors[4] = {2};

    interpreter_backend interp;
    interp.reset_runtime_state();
    auto art = interp.emit(finalize(std::move(fn)));

    REQUIRE(art.diagnostics.empty());
    REQUIRE(return_of(art).has_value());
    CHECK(*return_of(art) == 6); // 1 + 2 + 3
}

// ---------------------------------------------------------------------------
// 6. Straight-line MIR (no branch) still executes identically — regression.
// ---------------------------------------------------------------------------
TEST_CASE (

"cfg exec: straight-line function unchanged"
,
"[crank][cfg][v2]"
)
 {
    allocated_function_ir fn;
    fn.name = "straight";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.instructions.push_back(load_imm(1, 0, "r0", 42));
    entry.instructions.push_back(ret_preg(2, 0, "r0"));
    fn.blocks.push_back(std::move(entry));

    interpreter_backend interp;
    interp.reset_runtime_state();
    auto art = interp.emit(finalize(std::move(fn)));

    REQUIRE(art.diagnostics.empty());
    REQUIRE(return_of(art).has_value());
    CHECK(*return_of(art) == 42);
}

// ---------------------------------------------------------------------------
// 7. Dangling branch target is diagnosed rather than mis-executed.
// ---------------------------------------------------------------------------
TEST_CASE (

"cfg exec: dangling branch target diagnosed"
,
"[crank][cfg][v2]"
)
 {
    allocated_function_ir fn;
    fn.name = "dangling";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    {
        allocated_instruction br;
        br.id = 1;
        br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(99)}; // no such block
        entry.instructions.push_back(br);
    }
    fn.blocks.push_back(std::move(entry));

    interpreter_backend interp;
    interp.reset_runtime_state();
    auto art = interp.emit(finalize(std::move(fn)));

    bool saw_dangling = false;
    for (const auto& d : art.diagnostics) {
        if (d.find("branch target block id 99") != std::string::npos) {
            saw_dangling = true;
        }
    }
    CHECK(saw_dangling);
}

// =============================================================================
// Native execution tests — appended per repo rule (do not modify tests above)
//
// Tests 8–10: execute_physical_native correctness + CFG value gap (C-4 fix).
// These are SKIPPED (not failed) when asmjit is absent so cross-platform builds
// pass on targets without JIT support.
// =============================================================================

#include "languages/crank/execute.hpp"

// ---------------------------------------------------------------------------
// 8. execute_physical_native returns the same value as execute_physical for a
//    straight-line (no CFG) function.  Both are always runnable.
// ---------------------------------------------------------------------------
TEST_CASE (

"native exec: straight-line result matches interpreter"
,
"[crank][native][perf-l1]"
)
 {
    // Build: r0 = 42; ret r0
    allocated_function_ir fn;
    fn.name = "const42";
    fn.cfg.entry_block = 1;
    allocated_basic_block entry;
    entry.id = 1;
    entry.instructions.push_back(load_imm(1, 0, "r0", 42));
    entry.instructions.push_back(ret_preg(2, 0, "r0"));
    fn.blocks.push_back(std::move(entry));

    auto phys = finalize(std::move(fn));

    // Interpreter reference.
    auto interp_res = crank::execute_physical(phys);
    REQUIRE(interp_res.ok());
    REQUIRE(interp_res.return_value.has_value());
    CHECK(*interp_res.return_value == 42);

    // Native path.
    auto native_res = crank::execute_physical_native(phys);
    REQUIRE(native_res.ok());
    REQUIRE(native_res.return_value.has_value());
    CHECK(*native_res.return_value == 42);
}

// ---------------------------------------------------------------------------
// 9. execute_physical_native returns a scalar for a CFG (counted-loop) function.
//    This is the C-4 correctness fix: the old interpreter path returned nullopt
//    for CFG functions; the native path must return the actual value.
//
//    Function: sum 1..=3 → 6 (backward-branch loop, as in test 5 above).
// ---------------------------------------------------------------------------
TEST_CASE (

"native exec: CFG counted loop returns scalar (C-4 fix)"
,
"[crank][native][perf-l1]"
)
 {
    // Same MIR as test 5 (backward-branch counted loop).
    allocated_function_ir fn;
    fn.name = "cfg_loop_sum";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.instructions.push_back(load_imm(1, 0, "r0", 0)); // acc = 0
    entry.instructions.push_back(load_imm(2, 1, "r1", 1)); // i = 1
    entry.instructions.push_back(load_imm(3, 2, "r2", 3)); // limit = 3
    entry.instructions.push_back(load_imm(4, 4, "r4", 1)); // one = 1
    {
        allocated_instruction br; br.id = 5; br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(2)};
        entry.instructions.push_back(br);
    }

    allocated_basic_block header; header.id = 2;
    {
        allocated_instruction cmp; cmp.id = 6; cmp.op = opcode::cmp_le;
        cmp.defs = {allocated_operand::as_preg({3, "r3"})};
        cmp.uses = {allocated_operand::as_preg({1, "r1"}), allocated_operand::as_preg({2, "r2"})};
        header.instructions.push_back(cmp);
        allocated_instruction bc; bc.id = 7; bc.op = opcode::branch_cond;
        bc.uses = {allocated_operand::as_preg({3, "r3"}),
                   allocated_operand::as_block(3), allocated_operand::as_block(4)};
        header.instructions.push_back(bc);
    }

    allocated_basic_block body; body.id = 3;
    {
        allocated_instruction add_acc; add_acc.id = 8; add_acc.op = opcode::add;
        add_acc.defs = {allocated_operand::as_preg({0, "r0"})};
        add_acc.uses = {allocated_operand::as_preg({0, "r0"}), allocated_operand::as_preg({1, "r1"})};
        body.instructions.push_back(add_acc);
        allocated_instruction inc_i; inc_i.id = 9; inc_i.op = opcode::add;
        inc_i.defs = {allocated_operand::as_preg({1, "r1"})};
        inc_i.uses = {allocated_operand::as_preg({1, "r1"}), allocated_operand::as_preg({4, "r4"})};
        body.instructions.push_back(inc_i);
        allocated_instruction br; br.id = 10; br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(2)};
        body.instructions.push_back(br);
    }

    allocated_basic_block exit_bb; exit_bb.id = 4;
    exit_bb.instructions.push_back(ret_preg(11, 0, "r0"));

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(header));
    fn.blocks.push_back(std::move(body));
    fn.blocks.push_back(std::move(exit_bb));
    fn.cfg.successors[1] = {2};
    fn.cfg.successors[2] = {3, 4};
    fn.cfg.successors[3] = {2};
    fn.cfg.predecessors[2] = {1, 3};
    fn.cfg.predecessors[3] = {2};
    fn.cfg.predecessors[4] = {2};

    auto phys = finalize(std::move(fn));

    // Native path must return 6.
    auto native_res = crank::execute_physical_native(phys);
    REQUIRE(native_res.ok());
    REQUIRE(native_res.return_value.has_value());
    CHECK(*native_res.return_value == 6);
}

// ---------------------------------------------------------------------------
// 10. execute_physical_native fallback guard: result is valid even on fallback.
//     When the native backend is unavailable, fallback_fired=true and
//     return_value is still populated (interpreter fallback ran).
// ---------------------------------------------------------------------------
TEST_CASE (

"native exec: fallback produces a valid result"
,
"[crank][native][perf-l1]"
)
 {
    // Straight-line function — always executable by at least the interpreter.
    allocated_function_ir fn;
    fn.name = "fallback_guard";
    fn.cfg.entry_block = 1;
    allocated_basic_block entry; entry.id = 1;
    entry.instructions.push_back(load_imm(1, 0, "r0", 99));
    entry.instructions.push_back(ret_preg(2, 0, "r0"));
    fn.blocks.push_back(std::move(entry));

    auto phys = finalize(std::move(fn));

    auto res = crank::execute_physical_native(phys);
    // Must always be ok() and return a value — regardless of whether native
    // JIT fired or fell back to interpreter.
    REQUIRE(res.ok());
    REQUIRE(res.return_value.has_value());
    CHECK(*res.return_value == 99);
    // If fallback fired: still valid (interpreter ran).
    // If native ran: fallback_fired=false, same value.
    (void)res.fallback_fired; // either way is correct
}

// ---------------------------------------------------------------------------
// 11. execution_path policy routing for auto_select / jit_preferred.
// ---------------------------------------------------------------------------
TEST_CASE (

"native exec: execution_path policy routing"
,
"[crank][native][policy]"
)
 {
    // Tiny straight-line MIR should stay on interpreter in auto_select.
    allocated_function_ir tiny_fn;
    tiny_fn.name = "tiny_straight";
    tiny_fn.cfg.entry_block = 1;
    allocated_basic_block tiny_entry;
    tiny_entry.id = 1;
    tiny_entry.instructions.push_back(load_imm(1, 0, "r0", 5));
    tiny_entry.instructions.push_back(ret_preg(2, 0, "r0"));
    tiny_fn.blocks.push_back(std::move(tiny_entry));
    auto tiny_phys = finalize(std::move(tiny_fn));

    crank::execute_options auto_opts;
    auto_opts.path = crank::execute_options::execution_path::auto_select;
    CHECK_FALSE(crank::detail::should_use_native(tiny_phys, auto_opts));

    crank::execute_options jit_pref;
    jit_pref.path = crank::execute_options::execution_path::jit_preferred;
    CHECK(crank::detail::should_use_native(tiny_phys, jit_pref));

    // CFG MIR should route native in auto_select.
    allocated_function_ir cfg_fn;
    cfg_fn.name = "cfg_branch";
    cfg_fn.cfg.entry_block = 1;
    allocated_basic_block cfg_entry;
    cfg_entry.id = 1;
    cfg_entry.instructions.push_back(load_imm(1, 1, "r1", 1));
    {
        allocated_instruction bc;
        bc.id = 2;
        bc.op = opcode::branch_cond;
        bc.uses = {allocated_operand::as_preg({1, "r1"}),
                   allocated_operand::as_block(2),
                   allocated_operand::as_block(3)};
        cfg_entry.instructions.push_back(bc);
    }
    allocated_basic_block cfg_then;
    cfg_then.id = 2;
    cfg_then.instructions.push_back(load_imm(3, 0, "r0", 11));
    cfg_then.instructions.push_back(ret_preg(4, 0, "r0"));
    allocated_basic_block cfg_else;
    cfg_else.id = 3;
    cfg_else.instructions.push_back(load_imm(5, 0, "r0", 22));
    cfg_else.instructions.push_back(ret_preg(6, 0, "r0"));
    cfg_fn.blocks.push_back(std::move(cfg_entry));
    cfg_fn.blocks.push_back(std::move(cfg_then));
    cfg_fn.blocks.push_back(std::move(cfg_else));
    cfg_fn.cfg.successors[1] = {2, 3};
    cfg_fn.cfg.predecessors[2] = {1};
    cfg_fn.cfg.predecessors[3] = {1};
    auto cfg_phys = finalize(std::move(cfg_fn));

    CHECK(crank::detail::should_use_native(cfg_phys, auto_opts));
}

// ---------------------------------------------------------------------------
// 12. Repeated native execution of the same MIR keeps correctness stable.
// ---------------------------------------------------------------------------
TEST_CASE (

"native exec: repeated execute_physical_native is stable"
,
"[crank][native][cache]"
)
 {
    allocated_function_ir fn;
    fn.name = "repeat_native";
    fn.cfg.entry_block = 1;
    allocated_basic_block entry;
    entry.id = 1;
    entry.instructions.push_back(load_imm(1, 0, "r0", 123));
    entry.instructions.push_back(ret_preg(2, 0, "r0"));
    fn.blocks.push_back(std::move(entry));

    auto phys = finalize(std::move(fn));

    for (int i = 0; i < 3; ++i) {
        auto res = crank::execute_physical_native(phys);
        REQUIRE(res.ok());
        REQUIRE(res.return_value.has_value());
        CHECK(*res.return_value == 123);
    }
}

TEST_CASE(
    "native exec: stats-based routing matches execution path policy",
    "[crank][native][policy]") {
    crank::execute_options auto_opts;
    auto_opts.path = crank::execute_options::execution_path::auto_select;

    crank::detail::mir_stats tiny{
        .instr_count = 2,
        .branch_count = 0,
        .block_count = 1,
    };
    CHECK_FALSE(crank::detail::should_use_native(tiny, auto_opts));

    crank::detail::mir_stats cfg_like{
        .instr_count = 8,
        .branch_count = 1,
        .block_count = 5,
    };
    CHECK(crank::detail::should_use_native(cfg_like, auto_opts));

    crank::execute_options force_interp;
    force_interp.path = crank::execute_options::execution_path::interpreter_only;
    CHECK_FALSE(crank::detail::should_use_native(cfg_like, force_interp));
}

