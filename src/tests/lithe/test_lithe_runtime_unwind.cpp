#include "catch_amalgamated.hpp"

#include "lithe/lithe_runtime.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/backends/lithe_codegen_asmjit.hpp"

using namespace lithe::runtime::unwind;
using namespace lithe::codegen;
using namespace lithe::codegen::backends;

// ===========================================================================
// Helpers
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
// UNIT: ip_range
// ===========================================================================

TEST_CASE (



"ip_range: contains semantics [begin, end)"
,
"[unwind]"
)
 {
    ip_range r{100, 200};
    CHECK(r.contains(100));   // begin is inclusive
    CHECK(r.contains(150));
    CHECK(r.contains(199));
    CHECK(!r.contains(200));  // end is exclusive
    CHECK(!r.contains(99));
    CHECK(!r.contains(0));
}

TEST_CASE (



"ip_range: sort order by begin"
,
"[unwind]"
)
 {
    ip_range a{10, 20};
    ip_range b{30, 40};
    ip_range c{5,  15};

    std::vector<ip_range> v = {a, b, c};
    std::sort(v.begin(), v.end());

    REQUIRE(v.size() == 3);
    CHECK(v[0].begin == 5);
    CHECK(v[1].begin == 10);
    CHECK(v[2].begin == 30);
}

// ===========================================================================
// UNIT: unwind_table binary search
// ===========================================================================

TEST_CASE (



"unwind_table: empty table returns nullptr"
,
"[unwind]"
)
 {
    unwind_table tbl;
    CHECK(tbl.find(0) == nullptr);
    CHECK(tbl.find(0xdeadbeef) == nullptr);
}

TEST_CASE (



"unwind_table: single entry hit and miss"
,
"[unwind]"
)
 {
    unwind_table tbl;
    tbl.fn_name = "fn_single";
    unwind_entry e;
    e.range = {100, 200};
    e.pad   = {0x1234, 0u};
    tbl.insert(e);

    const auto *hit = tbl.find(150);
    REQUIRE(hit != nullptr);
    CHECK(hit->pad.address == 0x1234u);

    CHECK(tbl.find(99)  == nullptr);
    CHECK(tbl.find(200) == nullptr);
}

TEST_CASE (



"unwind_table: multiple entries sorted on insert"
,
"[unwind]"
)
 {
    unwind_table tbl;
    // Insert out-of-order — table must stay sorted.
    tbl.insert({{300, 400}, {0x300, 0u}});
    tbl.insert({{100, 200}, {0x100, 0u}});
    tbl.insert({{500, 600}, {0x500, 0u}});

    REQUIRE(tbl.entries.size() == 3);
    CHECK(tbl.entries[0].range.begin == 100);
    CHECK(tbl.entries[1].range.begin == 300);
    CHECK(tbl.entries[2].range.begin == 500);

    CHECK(tbl.find(150)->pad.address == 0x100u);
    CHECK(tbl.find(350)->pad.address == 0x300u);
    CHECK(tbl.find(550)->pad.address == 0x500u);
    CHECK(tbl.find(250) == nullptr); // gap between 200 and 300
}

TEST_CASE (



"unwind_table: O(log n) search misses adjacent ranges"
,
"[unwind]"
)
 {
    unwind_table tbl;
    for (uintptr_t base = 0; base < 1000; base += 100)
        tbl.insert({{base, base + 50}, {base + 0xf000, 0u}});

    // Hits
    for (uintptr_t base = 0; base < 1000; base += 100) {
        const auto *e = tbl.find(base + 25);
        REQUIRE(e != nullptr);
        CHECK(e->range.begin == base);
    }
    // Misses (gaps [50, 100), [150, 200), etc.)
    for (uintptr_t base = 0; base < 1000; base += 100) {
        CHECK(tbl.find(base + 75) == nullptr);
    }
}

// ===========================================================================
// UNIT: unwind_registry
// ===========================================================================

TEST_CASE (



"unwind_registry: register and retrieve table"
,
"[unwind]"
)
 {
    unwind_registry reg;

    unwind_table tbl;
    tbl.fn_name = "fn_alpha";
    tbl.insert({{100, 200}, {0xabc, 1u}});
    reg.register_table(tbl);

    REQUIRE(reg.contains("fn_alpha"));
    auto got = reg.get("fn_alpha");
    REQUIRE(got.has_value());
    CHECK(got->fn_name == "fn_alpha");
    CHECK(got->entries.size() == 1);
    CHECK(!reg.contains("fn_missing"));
}

TEST_CASE (



"unwind_registry: overwrite with newer table"
,
"[unwind]"
)
 {
    unwind_registry reg;

    unwind_table old_tbl;
    old_tbl.fn_name = "fn_x";
    old_tbl.insert({{10, 20}, {0x1, 0u}});

    unwind_table new_tbl;
    new_tbl.fn_name = "fn_x";
    new_tbl.insert({{10, 20}, {0x9999, 7u}});

    reg.register_table(old_tbl);
    reg.register_table(new_tbl);

    auto got = reg.get("fn_x");
    REQUIRE(got.has_value());
    CHECK(got->entries[0].pad.address == 0x9999u);
    CHECK(got->entries[0].pad.cleanup_flags == 7u);
}

// ===========================================================================
// UNIT: personality_routine miss and hit
// ===========================================================================

TEST_CASE (



"lithe_personality_routine: miss returns unwind_error"
,
"[unwind]"
)
 {
    unwind_registry reg;  // empty
    auto result = lithe_personality_routine(0x1000, reg);
    REQUIRE(!result.has_value());
    CHECK(result.error().code == unwind_error_code::ip_not_found);
}

TEST_CASE (



"lithe_personality_routine: hit returns correct landing_pad"
,
"[unwind]"
)
 {
    unwind_registry reg;

    unwind_table tbl;
    tbl.fn_name = "fn_p";
    tbl.insert({{0x1000, 0x2000}, {0xDEAD, 42u}});
    reg.register_table(tbl);

    // Exact hit
    auto r1 = lithe_personality_routine(0x1500, reg);
    REQUIRE(r1.has_value());
    CHECK(r1->address       == 0xDEADu);
    CHECK(r1->cleanup_flags == 42u);

    // Begin boundary
    auto r2 = lithe_personality_routine(0x1000, reg);
    REQUIRE(r2.has_value());
    CHECK(r2->address == 0xDEADu);

    // End boundary (exclusive — miss)
    auto r3 = lithe_personality_routine(0x2000, reg);
    REQUIRE(!r3.has_value());

    // Outside
    auto r4 = lithe_personality_routine(0x0fff, reg);
    REQUIRE(!r4.has_value());
}

TEST_CASE (



"lithe_personality_routine: searches across multiple functions"
,
"[unwind]"
)
 {
    unwind_registry reg;

    unwind_table tbl_a; tbl_a.fn_name = "fn_a"; tbl_a.insert({{100, 200}, {0xA, 0u}});
    unwind_table tbl_b; tbl_b.fn_name = "fn_b"; tbl_b.insert({{500, 600}, {0xB, 0u}});
    reg.register_table(tbl_a);
    reg.register_table(tbl_b);

    CHECK(lithe_personality_routine(150, reg)->address == 0xAu);
    CHECK(lithe_personality_routine(550, reg)->address == 0xBu);
    CHECK(!lithe_personality_routine(300, reg).has_value());
}

// ===========================================================================
// UNIT: MIR opcode helpers
// ===========================================================================

TEST_CASE (



"make_unwind_op: domain and name"
,
"[unwind]"
)
 {
    const auto op = make_unwind_op("landing_pad_tag");
    CHECK(op.domain == "lithe.unwind");
    CHECK(op.name   == "landing_pad_tag");
}

TEST_CASE (



"make_landing_pad_tag_instr: encoding"
,
"[unwind]"
)
 {
    const auto instr = make_landing_pad_tag_instr(7, 0xFF);
    CHECK(instr.id == 7);
    CHECK(instr.op == opcode::indirect_call);
    REQUIRE(instr.abstract_operation.has_value());
    CHECK(instr.abstract_operation->domain == "lithe.unwind");
    CHECK(instr.abstract_operation->name   == "landing_pad_tag");
    REQUIRE(instr.uses.size() == 1);
    CHECK(std::get<std::int64_t>(instr.uses[0].value) == 0xFF);
}

TEST_CASE (



"make_unwind_region_begin/end: encoding"
,
"[unwind]"
)
 {
    const auto begin = make_unwind_region_begin_instr(1);
    CHECK(begin.op == opcode::indirect_call);
    CHECK(begin.abstract_operation->name == "unwind_region_begin");
    CHECK(begin.uses.empty());

    const auto end = make_unwind_region_end_instr(2);
    CHECK(end.abstract_operation->name == "unwind_region_end");
    CHECK(end.uses.empty());
}

// ===========================================================================
// INTEGRATION: emit MIR function with unwind opcodes, verify table + personality
// ===========================================================================

TEST_CASE (



"asmjit_backend + unwind_registry: basic try-region produces entry"
,
"[unwind]"
)
{
    // Function layout:
    //   bb1 (entry):
    //     load_imm r1 = 42
    //     unwind_region_begin          <- open try region
    //     add r2 = r1 + 1              <- body (real instructions)
    //     unwind_region_end            <- close try region
    //     landing_pad_tag flags=3      <- handler entry point
    //     ret r2
    //
    // After emit:
    //   - unwind_registry["try_fn"] should have 1 entry
    //   - entry.range contains code offsets for [begin, end)
    //   - entry.pad.cleanup_flags == 3
    //   - personality_routine(ip_inside_range) resolves to the landing_pad
    //   - personality_routine(ip_outside_range) fails

    auto fn = wrap("try_fn", {
        make_block(1, "entry", {}, {
            make_inst(1, opcode::load_imm, {preg_op(1)}, {imm_op(42)}),
            make_unwind_region_begin_instr(2),
            make_inst(3, opcode::add, {preg_op(2)}, {preg_op(1), imm_op(1)}),
            make_unwind_region_end_instr(4),
            make_landing_pad_tag_instr(5, /*cleanup_flags=*/3),
            make_inst(6, opcode::ret, {}, {preg_op(2)})
        })
    });

    unwind_registry reg;
    asmjit_backend backend;
    backend.set_unwind_registry(&reg);

    auto art = backend.emit(fn);
    REQUIRE(art.diagnostics.empty());
    REQUIRE(art.handle != nullptr);

    REQUIRE(reg.contains("try_fn"));
    auto tbl = reg.get("try_fn");
    REQUIRE(tbl.has_value());
    REQUIRE(tbl->entries.size() == 1);

    const auto &entry = tbl->entries[0];
    CHECK(entry.range.begin < entry.range.end);
    CHECK(entry.pad.cleanup_flags == 3u);

    // personality_routine must resolve any IP strictly inside the range.
    const uintptr_t mid = entry.range.begin + (entry.range.end - entry.range.begin) / 2;
    auto r = lithe_personality_routine(mid, reg);
    REQUIRE(r.has_value());
    CHECK(r->cleanup_flags == 3u);

    // IP before range → miss.
    if (entry.range.begin > 0) {
        auto miss = lithe_personality_routine(entry.range.begin - 1, reg);
        CHECK(!miss.has_value());
    }

    // IP at end (exclusive) → miss.
    auto miss_end = lithe_personality_routine(entry.range.end, reg);
    CHECK(!miss_end.has_value());
}

TEST_CASE (



"asmjit_backend + unwind_registry: landing_pad address is inside try-region"
,
"[unwind]"
)
{
    // The landing_pad_tag is placed after unwind_region_end (conceptually after
    // the protected region). Its address must NOT be inside the try-region's
    // ip_range — verify the landing_pad address is >= entry.range.end.

    auto fn = wrap("lp_addr_fn", {
        make_block(1, "entry", {}, {
            make_inst(1, opcode::load_imm, {preg_op(1)}, {imm_op(0)}),
            make_unwind_region_begin_instr(2),
            make_inst(3, opcode::add, {preg_op(1)}, {preg_op(1), imm_op(1)}),
            make_unwind_region_end_instr(4),
            make_landing_pad_tag_instr(5, /*cleanup_flags=*/0xFu),
            make_inst(6, opcode::ret, {}, {preg_op(1)})
        })
    });

    unwind_registry reg;
    asmjit_backend backend;
    backend.set_unwind_registry(&reg);
    auto art = backend.emit(fn);
    REQUIRE(art.diagnostics.empty());

    auto tbl = reg.get("lp_addr_fn");
    REQUIRE(tbl.has_value());
    REQUIRE(tbl->entries.size() == 1);

    const auto &e = tbl->entries[0];
    // landing_pad address must be at or after the region end.
    CHECK(e.pad.address >= e.range.end);
    CHECK(e.pad.cleanup_flags == 0xFu);
}

TEST_CASE (



"asmjit_backend + unwind_registry: no unwind opcodes → no entry registered"
,
"[unwind]"
)
{
    // A plain function with no unwind opcodes should produce no entry.
    auto fn = wrap("plain_fn", {
        make_block(1, "entry", {}, {
            make_inst(1, opcode::load_imm, {preg_op(1)}, {imm_op(7)}),
            make_inst(2, opcode::ret, {}, {preg_op(1)})
        })
    });

    unwind_registry reg;
    asmjit_backend backend;
    backend.set_unwind_registry(&reg);
    auto art = backend.emit(fn);

    CHECK(!reg.contains("plain_fn"));
}

TEST_CASE (



"asmjit_backend + unwind_registry: null registry pointer → no crash"
,
"[unwind]"
)
{
    // Not setting an unwind registry must be safe.
    auto fn = wrap("no_reg_fn", {
        make_block(1, "entry", {}, {
            make_unwind_region_begin_instr(1),
            make_inst(2, opcode::load_imm, {preg_op(1)}, {imm_op(1)}),
            make_unwind_region_end_instr(3),
            make_landing_pad_tag_instr(4, 1),
            make_inst(5, opcode::ret, {}, {preg_op(1)})
        })
    });

    asmjit_backend backend; // no set_unwind_registry call
    auto art = backend.emit(fn);
    CHECK(art.diagnostics.empty());
}
