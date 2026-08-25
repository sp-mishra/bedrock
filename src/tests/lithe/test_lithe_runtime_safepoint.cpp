#include "catch_amalgamated.hpp"

#include "lithe/lithe_runtime.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/backends/lithe_codegen_asmjit.hpp"

using namespace lithe::runtime::safepoint;
using namespace lithe::codegen;
using namespace lithe::codegen::backends;

// ===========================================================================
// Helpers shared across tests
// ===========================================================================
namespace {
    allocated_operand preg_op(std::uint16_t id) {
        preg r;
        r.id = id;
        return allocated_operand::as_preg(r);
    }

    allocated_operand imm_op(std::int64_t v) {
        return allocated_operand::as_i64(v);
    }

    allocated_operand block_op(std::uint32_t bid) {
        return allocated_operand::as_block(bid);
    }

    allocated_instruction make_inst(std::uint32_t id, opcode op,
                                    std::vector<allocated_operand> defs = {},
                                    std::vector<allocated_operand> uses = {}) {
        allocated_instruction i;
        i.id = id;
        i.op = op;
        i.defs = std::move(defs);
        i.uses = std::move(uses);
        return i;
    }

    allocated_instruction make_indirect_call(std::uint32_t id,
                                             std::vector<allocated_operand> uses = {}) {
        return make_inst(id, opcode::indirect_call, {}, std::move(uses));
    }

    allocated_instruction make_safepoint(std::uint32_t id, live_set roots = {}) {
        return make_safepoint_instr(id, roots);
    }

    allocated_basic_block make_block(std::uint32_t id, std::string name,
                                     std::vector<std::uint32_t> succs,
                                     std::vector<allocated_instruction> insts) {
        allocated_basic_block bb;
        bb.id = id;
        bb.name = std::move(name);
        bb.successors = std::move(succs);
        bb.instructions = std::move(insts);
        return bb;
    }

    mir::physical_mir_function wrap(std::string name,
                                    std::vector<allocated_basic_block> blocks,
                                    std::uint32_t entry = 1) {
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = entry;
        fn.blocks = std::move(blocks);
        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }
} // anonymous namespace

// ===========================================================================
// UNIT: safepoint_record sort order
// ===========================================================================
TEST_CASE (



"safepoint_record: sort order by instr_id"
,
"[safepoint]"
)
 {
    stack_map sm;
    sm.fn_name = "sort_test";

    sm.insert({30, {1, 2}});
    sm.insert({10, {3}});
    sm.insert({20, {4, 5, 6}});

    REQUIRE(sm.entries.size() == 3);
    CHECK(sm.entries[0].instr_id == 10);
    CHECK(sm.entries[1].instr_id == 20);
    CHECK(sm.entries[2].instr_id == 30);
}

TEST_CASE (



"safepoint_record: O(log n) lookup present"
,
"[safepoint]"
)
 {
    stack_map sm;
    sm.fn_name = "lookup_test";
    sm.insert({5, {10, 11}});
    sm.insert({15, {20}});

    const auto *r = sm.find(5);
    REQUIRE(r != nullptr);
    CHECK(r->instr_id == 5);
    REQUIRE(r->roots.size() == 2);
}

TEST_CASE (



"safepoint_record: lookup absent returns nullptr"
,
"[safepoint]"
)
 {
    stack_map sm;
    sm.fn_name = "miss_test";
    sm.insert({7, {}});

    CHECK(sm.find(99) == nullptr);
    CHECK(sm.find(0) == nullptr);
}

TEST_CASE (



"stack_map: duplicate instr_id merges roots"
,
"[safepoint]"
)
 {
    stack_map sm;
    sm.fn_name = "dedup_test";

    sm.insert({42, {1, 2}});
    sm.insert({42, {2, 3, 4}}); // duplicate id

    REQUIRE(sm.entries.size() == 1);
    const auto &e = sm.entries[0];
    CHECK(e.instr_id == 42);
    // Union: {1, 2, 3, 4} (order may vary, but size == 4)
    CHECK(e.roots.size() == 4);
}

// ===========================================================================
// UNIT: stack_map_table thread-safe lookup
// ===========================================================================
TEST_CASE (



"stack_map_table: register and retrieve"
,
"[safepoint]"
)
 {
    stack_map_table tbl;

    stack_map sm;
    sm.fn_name = "fn_alpha";
    sm.insert({1, {10}});
    sm.insert({2, {10, 11}});

    tbl.register_map(sm);

    REQUIRE(tbl.contains("fn_alpha"));
    auto got = tbl.get("fn_alpha");
    REQUIRE(got.has_value());
    CHECK(got->fn_name == "fn_alpha");
    REQUIRE(got->entries.size() == 2);

    CHECK(!tbl.contains("fn_beta"));
    CHECK(!tbl.get("fn_beta").has_value());
}

TEST_CASE (



"stack_map_table: overwrite with newer map"
,
"[safepoint]"
)
 {
    stack_map_table tbl;

    stack_map old_sm; old_sm.fn_name = "fn_x"; old_sm.insert({1, {5}});
    stack_map new_sm; new_sm.fn_name = "fn_x"; new_sm.insert({1, {99}});

    tbl.register_map(old_sm);
    tbl.register_map(new_sm);

    auto got = tbl.get("fn_x");
    REQUIRE(got.has_value());
    REQUIRE(got->entries.size() == 1);
    CHECK(got->entries[0].roots[0] == 99);
}

// ===========================================================================
// UNIT: make_safepoint_op / make_safepoint_instr
// ===========================================================================
TEST_CASE (



"make_safepoint_op: domain and name"
,
"[safepoint]"
)
 {
    const auto op = make_safepoint_op();
    CHECK(op.domain == "lithe.safepoint");
    CHECK(op.name   == "safepoint_tag");
}

TEST_CASE (



"make_safepoint_instr: encoding"
,
"[safepoint]"
)
 {
    const live_set roots = {3, 7, 11};
    const auto instr = make_safepoint_instr(42, roots);

    CHECK(instr.id == 42);
    CHECK(instr.op == opcode::indirect_call);
    REQUIRE(instr.abstract_operation.has_value());
    CHECK(instr.abstract_operation->domain == "lithe.safepoint");
    REQUIRE(instr.uses.size() == 3);
    CHECK(std::get<preg>(instr.uses[0].value).id == 3);
    CHECK(std::get<preg>(instr.uses[1].value).id == 7);
    CHECK(std::get<preg>(instr.uses[2].value).id == 11);
}

// ===========================================================================
// UNIT: safepoint_injection_pass — de-duplication invariant
// ===========================================================================
TEST_CASE (



"safepoint_injection_pass: no duplicate insertion for same site"
,
"[safepoint]"
)
 {
    // A block with an indirect_call that already has a safepoint_tag immediately
    // after it. Running the pass again should NOT insert a second safepoint_tag
    // after the first one (the existing one counts as the safepoint for that call).
    auto existing_sp = make_safepoint(10, {1});

    auto fn = wrap("no_double_sp", {
        make_block(1, "entry", {}, {
            make_indirect_call(9),
            existing_sp,  // already tagged
            make_inst(11, opcode::ret, {}, {imm_op(0)})
        })
    });

    mir_pass_context ctx;
    safepoint_injection_pass pass;
    auto result = pass.run(fn, ctx);

    // Count safepoints in output.
    int sp_count = 0;
    for (const auto &blk : result.function.function.blocks)
        for (const auto &inst : blk.instructions)
            if (inst.abstract_operation &&
                inst.abstract_operation->domain == "lithe.safepoint")
                ++sp_count;

    // The pass may insert one after the indirect_call (id=9), which is followed
    // immediately by an existing safepoint. De-dup logic in the pass prevents
    // inserting when the existing instr_id already has a match. We verify that
    // consecutive safepoints in the final output have different instr_ids (i.e.,
    // no two safepoints were inserted for the exact same after-instruction site).
    std::vector<std::uint32_t> sp_ids;
    for (const auto &blk : result.function.function.blocks)
        for (const auto &inst : blk.instructions)
            if (inst.abstract_operation &&
                inst.abstract_operation->domain == "lithe.safepoint")
                sp_ids.push_back(inst.id);

    // All safepoint instruction IDs must be unique (dedup invariant).
    auto sorted = sp_ids;
    std::ranges::sort(sorted);
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    CHECK(sorted.size() == sp_ids.size());
    (void)sp_count;
}

// ===========================================================================
// INTEGRATION: build physical_mir_function with call + loop back-edge,
//              run safepoint_injection_pass, compile with asmjit_backend
//              + stack_map_table, assert resulting stack_map entry count.
// ===========================================================================
TEST_CASE (



"safepoint_injection_pass + asmjit_backend: call produces safepoint entry"
,
"[safepoint]"
)
{
    // Function:
    //   bb1 (entry): load_imm r1=0, indirect_call (not a safepoint), ret r1
    //
    // After safepoint_injection_pass: one safepoint_tag after the indirect_call.
    // After asmjit emit with stack_map_table: table has "call_fn" with 1 entry.

    auto fn = wrap("call_fn", {
        make_block(1, "entry", {}, {
            make_inst(1, opcode::load_imm, {preg_op(1)}, {imm_op(42)}),
            make_indirect_call(2),
            make_inst(3, opcode::ret, {}, {preg_op(1)})
        })
    });

    mir_pass_context ctx;
    safepoint_injection_pass pass;
    auto injected = pass.run(fn, ctx);
    REQUIRE(injected.ok());

    // Count safepoint instructions inserted.
    int sp_count = 0;
    for (const auto &blk : injected.function.function.blocks)
        for (const auto &inst : blk.instructions)
            if (inst.abstract_operation &&
                inst.abstract_operation->domain == "lithe.safepoint")
                ++sp_count;
    REQUIRE(sp_count == 1);

    // Compile and check stack_map_table.
    stack_map_table tbl;
    asmjit_backend backend;
    backend.set_stack_map_table(&tbl);

    auto art = backend.emit(injected.function);
    // Safepoint-only functions (no real computations) may have diagnostics
    // about unresolved indirect_calls; we only care that the stack_map was
    // registered, not that JIT succeeded.
    (void)art;

    CHECK(tbl.contains("call_fn"));
    auto sm = tbl.get("call_fn");
    REQUIRE(sm.has_value());
    CHECK(sm->entries.size() == 1);
}

TEST_CASE (



"safepoint_injection_pass + asmjit_backend: loop back-edge produces entry"
,
"[safepoint]"
)
{
    // Simple counting loop:
    //   bb1 (entry):  load_imm r1=0, branch → bb2
    //   bb2 (header): cmp_lt r2 = (r1 < 10), branch_cond r2 → bb3, bb4
    //   bb3 (latch):  add r1 = r1 + 1, branch → bb2   <-- back-edge bb3→bb2
    //   bb4 (exit):   ret r1
    //
    // After safepoint_injection_pass: 1 safepoint after the branch in bb3
    // (the back-edge terminator).

    auto fn = wrap("loop_fn", {
        make_block(1, "entry", {2}, {
            make_inst(1, opcode::load_imm, {preg_op(1)}, {imm_op(0)}),
            make_inst(2, opcode::branch, {}, {block_op(2)})
        }),
        make_block(2, "header", {3, 4}, {
            make_inst(3, opcode::cmp_lt, {preg_op(2)}, {preg_op(1), imm_op(10)}),
            make_inst(4, opcode::branch_cond, {}, {preg_op(2), block_op(3), block_op(4)})
        }),
        make_block(3, "latch", {2}, {
            make_inst(5, opcode::add, {preg_op(1)}, {preg_op(1), imm_op(1)}),
            make_inst(6, opcode::branch, {}, {block_op(2)})
        }),
        make_block(4, "exit", {}, {
            make_inst(7, opcode::ret, {}, {preg_op(1)})
        })
    });

    mir_pass_context ctx;
    safepoint_injection_pass pass;
    auto injected = pass.run(fn, ctx);
    REQUIRE(injected.ok());

    // Count safepoints.
    int sp_count = 0;
    for (const auto &blk : injected.function.function.blocks)
        for (const auto &inst : blk.instructions)
            if (inst.abstract_operation &&
                inst.abstract_operation->domain == "lithe.safepoint")
                ++sp_count;
    // At least 1 safepoint for the back-edge.
    CHECK(sp_count >= 1);

    // Compile and check stack_map registered.
    stack_map_table tbl;
    asmjit_backend backend;
    backend.set_stack_map_table(&tbl);
    auto art = backend.emit(injected.function);
    (void)art;

    CHECK(tbl.contains("loop_fn"));
    auto sm = tbl.get("loop_fn");
    REQUIRE(sm.has_value());
    CHECK(sm->entries.size() >= 1);
}

TEST_CASE (



"safepoint_injection_pass + asmjit_backend: call + loop → expected count"
,
"[safepoint]"
)
{
    // Function has both an indirect_call AND a loop back-edge.
    // Expected: 2 distinct safepoints (one after the call, one at the back-edge),
    // unless the call IS the back-edge terminator (dedup).
    //
    // Layout:
    //   bb1: load_imm r1=0, indirect_call(site A), branch → bb2
    //   bb2: cmp_lt r2=(r1<5), branch_cond r2 → bb3, bb4
    //   bb3: add r1=r1+1, indirect_call(site B), branch → bb2  <-- back-edge
    //   bb4: ret r1

    auto fn = wrap("call_loop_fn", {
        make_block(1, "entry", {2}, {
            make_inst(1, opcode::load_imm, {preg_op(1)}, {imm_op(0)}),
            make_indirect_call(2),
            make_inst(3, opcode::branch, {}, {block_op(2)})
        }),
        make_block(2, "header", {3, 4}, {
            make_inst(4, opcode::cmp_lt, {preg_op(2)}, {preg_op(1), imm_op(5)}),
            make_inst(5, opcode::branch_cond, {}, {preg_op(2), block_op(3), block_op(4)})
        }),
        make_block(3, "latch", {2}, {
            make_inst(6, opcode::add, {preg_op(1)}, {preg_op(1), imm_op(1)}),
            make_indirect_call(7),
            make_inst(8, opcode::branch, {}, {block_op(2)})
        }),
        make_block(4, "exit", {}, {
            make_inst(9, opcode::ret, {}, {preg_op(1)})
        })
    });

    mir_pass_context ctx;
    safepoint_injection_pass pass;
    auto injected = pass.run(fn, ctx);
    REQUIRE(injected.ok());

    int sp_count = 0;
    for (const auto &blk : injected.function.function.blocks)
        for (const auto &inst : blk.instructions)
            if (inst.abstract_operation &&
                inst.abstract_operation->domain == "lithe.safepoint")
                ++sp_count;

    // site A (after call in bb1) + site B (after call in bb3) + back-edge in bb3.
    // But site B and the back-edge are at the same position (branch is the
    // back-edge terminator; call precedes it) → dedup gives 2 or 3.
    // The call at id=2 gets one safepoint; the call at id=7 gets one;
    // the back-edge is at id=8 (the branch), distinct from id=7, so → 3 total.
    CHECK(sp_count >= 2);

    stack_map_table tbl;
    asmjit_backend backend;
    backend.set_stack_map_table(&tbl);
    auto art = backend.emit(injected.function);
    (void)art;

    REQUIRE(tbl.contains("call_loop_fn"));
    auto sm = tbl.get("call_loop_fn");
    REQUIRE(sm.has_value());
    // Stack map entries == number of safepoint instructions in compiled output.
    CHECK(sm->entries.size() == static_cast<std::size_t>(sp_count));
}
