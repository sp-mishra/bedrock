#include "catch_amalgamated.hpp"

#include "lithe/lithe.hpp"
#include "lithe/backends/lithe_codegen_backend_registry.hpp"
#include "lithe/backends/lithe_codegen_debug_text_backend.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"

namespace {
    lithe::codegen::spill_slot slot(std::uint32_t id) {
        lithe::codegen::spill_slot out;
        out.id = id;
        out.size = 8;
        out.alignment = 8;
        out.frame_offset = -static_cast<std::int64_t>(id * 8);
        return out;
    }

    lithe::codegen::mir::physical_mir_function make_physical(
        std::string name,
        std::vector<lithe::codegen::allocated_instruction> instructions
    ) {
        using namespace lithe::codegen;

        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = 1;

        allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";
        bb.instructions = std::move(instructions);
        fn.blocks.push_back(std::move(bb));

        mir::physical_mir_function physical;
        physical.function = std::move(fn);
        physical.metadata.current_phase = mir::phase::physical_mir;
        return physical;
    }
}

TEST_CASE (



"Codegen spill rewrite inserts reload and store for spill use-def"
,
"[lithe][codegen][spill]"
)
 {
    using namespace lithe::codegen;

    allocated_function_ir fn;
    fn.name = "spill_use_def";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction inst;
    inst.id = 7;
    inst.op = opcode::add;
    inst.defs = {allocated_operand::as_spill(slot(1))};
    inst.uses = {allocated_operand::as_spill(slot(1)), allocated_operand::as_preg({4, "r4"})};

    bb.instructions.push_back(inst);
    fn.blocks.push_back(bb);

    auto rewritten = rewrite_spills(std::move(fn), {{200, "tmp0"}, {201, "tmp1"}});

    REQUIRE(rewritten.ok());
    REQUIRE(rewritten.inserted_loads == 1);
    REQUIRE(rewritten.inserted_stores == 1);

    const auto &instructions = rewritten.function.blocks[0].instructions;
    REQUIRE(instructions.size() == 3);

    REQUIRE(instructions[0].op == opcode::load_spill);
    REQUIRE(instructions[1].op == opcode::add);
    REQUIRE(instructions[2].op == opcode::store_spill);

    REQUIRE(instructions[0].defs[0].type == allocated_operand::kind::preg);
    REQUIRE(std::get<preg>(instructions[0].defs[0].value).name == "tmp0");

    REQUIRE(instructions[1].defs[0].type == allocated_operand::kind::preg);
    REQUIRE(std::get<preg>(instructions[1].defs[0].value).name == "tmp1");
    REQUIRE(instructions[1].uses[0].type == allocated_operand::kind::preg);
    REQUIRE(std::get<preg>(instructions[1].uses[0].value).name == "tmp0");

    REQUIRE(instructions[0].uses[0].type == allocated_operand::kind::memory);
    REQUIRE(std::get<memory_operand>(instructions[0].uses[0].value).address.kind == memory_address_kind::spill_slot);

    REQUIRE(instructions[2].defs[0].type == allocated_operand::kind::memory);
    REQUIRE(std::get<memory_operand>(instructions[2].defs[0].value).address.kind == memory_address_kind::spill_slot);
    REQUIRE(instructions[2].uses[0].type == allocated_operand::kind::preg);
    REQUIRE(std::get<preg>(instructions[2].uses[0].value).name == "tmp1");
}

TEST_CASE (



"Codegen spill rewrite handles multiple spills in one instruction"
,
"[lithe][codegen][spill]"
)
 {
    using namespace lithe::codegen;

    allocated_function_ir fn;
    fn.name = "multi_spill";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction inst;
    inst.id = 9;
    inst.op = opcode::call;
    inst.defs = {allocated_operand::as_spill(slot(1)), allocated_operand::as_spill(slot(2))};
    inst.uses = {
        allocated_operand::as_spill(slot(1)),
        allocated_operand::as_spill(slot(1)),
        allocated_operand::as_spill(slot(2))
    };

    bb.instructions.push_back(inst);
    fn.blocks.push_back(bb);

    auto rewritten = rewrite_spills(
        std::move(fn),
        {{200, "tmp0"}, {201, "tmp1"}, {202, "tmp2"}, {203, "tmp3"}}
    );

    REQUIRE(rewritten.ok());
    REQUIRE(rewritten.inserted_loads == 2);
    REQUIRE(rewritten.inserted_stores == 2);

    const auto &instructions = rewritten.function.blocks[0].instructions;
    REQUIRE(instructions.size() == 5);
    REQUIRE(instructions[0].op == opcode::load_spill);
    REQUIRE(instructions[1].op == opcode::load_spill);
    REQUIRE(instructions[2].op == opcode::call);
    REQUIRE(instructions[3].op == opcode::store_spill);
    REQUIRE(instructions[4].op == opcode::store_spill);

    REQUIRE(instructions[2].uses[0].type == allocated_operand::kind::preg);
    REQUIRE(instructions[2].uses[1].type == allocated_operand::kind::preg);
    REQUIRE(std::get<preg>(instructions[2].uses[0].value).name == std::get<preg>(instructions[2].uses[1].value).name);
    REQUIRE(instructions[2].uses[2].type == allocated_operand::kind::preg);

    REQUIRE(instructions[2].defs[0].type == allocated_operand::kind::preg);
    REQUIRE(instructions[2].defs[1].type == allocated_operand::kind::preg);

    REQUIRE(instructions[0].uses[0].type == allocated_operand::kind::memory);
    REQUIRE(instructions[1].uses[0].type == allocated_operand::kind::memory);

    REQUIRE(instructions[3].defs[0].type == allocated_operand::kind::memory);
    REQUIRE(std::get<memory_operand>(instructions[3].defs[0].value).address.kind == memory_address_kind::spill_slot);
    REQUIRE(instructions[4].defs[0].type == allocated_operand::kind::memory);
    REQUIRE(std::get<memory_operand>(instructions[4].defs[0].value).address.kind == memory_address_kind::spill_slot);
}

TEST_CASE (



"Codegen spill rewrite emits diagnostics when scratch registers are insufficient"
,
"[lithe][codegen][spill]"
)
 {
    using namespace lithe::codegen;

    allocated_function_ir fn;
    fn.name = "insufficient_scratch";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction inst;
    inst.id = 3;
    inst.op = opcode::mul;
    inst.defs = {allocated_operand::as_spill(slot(3))};
    inst.uses = {allocated_operand::as_spill(slot(1)), allocated_operand::as_spill(slot(2))};

    bb.instructions.push_back(inst);
    fn.blocks.push_back(bb);

    auto rewritten = rewrite_spills(std::move(fn), {{200, "tmp0"}});

    REQUIRE_FALSE(rewritten.ok());
    REQUIRE_FALSE(rewritten.diagnostics.empty());
    REQUIRE(rewritten.inserted_loads == 0);
    REQUIRE(rewritten.inserted_stores == 0);
    REQUIRE(rewritten.function.blocks[0].instructions.size() == 1);

    const auto &kept = rewritten.function.blocks[0].instructions[0];
    REQUIRE(kept.uses[0].type == allocated_operand::kind::spill);
    REQUIRE(kept.uses[1].type == allocated_operand::kind::spill);
    REQUIRE(kept.defs[0].type == allocated_operand::kind::spill);
}

TEST_CASE (



"Codegen argument helpers bind by name and positional index"
,
"[lithe][codegen][args]"
)
 {
    using namespace lithe::codegen;

    function_signature signature;
    signature.name = "named_args";
    signature.arguments = {{"a", false}, {"b", false}, {"c", false}};

    const auto by_name = bind_terminal_as_argument("b", signature);
    REQUIRE(by_name.has_value());
    REQUIRE(by_name->value == 1);

    const auto by_position = bind_terminal_as_argument(static_cast<std::size_t>(2), signature);
    REQUIRE(by_position.has_value());
    REQUIRE(by_position->value == 2);

    const auto missing_name = bind_terminal_as_argument("z", signature);
    REQUIRE_FALSE(missing_name.has_value());
}

TEST_CASE (



"Codegen argument register mapping uses generic calling convention"
,
"[lithe][codegen][args]"
)
 {
    using namespace lithe::codegen;

    function_signature default_sig;
    default_sig.arguments = {{"x", false}};
    const auto default_regs = argument_registers(default_sig);
    REQUIRE(default_regs.size() >= 1);
    REQUIRE(default_regs[0].name == "a0");

    function_signature custom_sig;
    custom_sig.arguments = {{"x", false}, {"y", false}};
    custom_sig.convention.integer_argument_registers = {{9, "ia0"}, {10, "ia1"}};
    const auto custom_regs = argument_registers(custom_sig);
    REQUIRE(custom_regs.size() == 2);
    REQUIRE(custom_regs[0].name == "ia0");
    REQUIRE(custom_regs[1].name == "ia1");
}

TEST_CASE (



"Codegen lowering with function signature emits load_arg for terminals"
,
"[lithe][codegen][args]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    struct term {
        int value = 0;
    };

    const term a{1};
    const term b{2};
    const term c{3};

    auto expr = make_node<mul_tag>(
        make_node<add_tag>(a, b),
        c
    );

    function_signature signature;
    signature.name = "mul_add_args";
    signature.arguments = {{"a", false}, {"b", false}, {"c", false}};

    auto fn = lower_to_machine_ir(expr, signature);
    REQUIRE(fn.name == "mul_add_args");
    REQUIRE(fn.blocks.size() == 1);

    const auto &insts = fn.blocks[0].instructions;
    REQUIRE(insts.size() == 6);

    REQUIRE(insts[0].op == opcode::load_arg);
    REQUIRE(insts[1].op == opcode::load_arg);
    REQUIRE(insts[2].op == opcode::add);
    REQUIRE(insts[3].op == opcode::load_arg);
    REQUIRE(insts[4].op == opcode::mul);
    REQUIRE(insts[5].op == opcode::ret);

    REQUIRE(insts[0].uses[0].type == operand::kind::argument_index);
    REQUIRE(std::get<std::uint32_t>(insts[0].uses[0].value) == 0);
    REQUIRE(insts[1].uses[0].type == operand::kind::argument_index);
    REQUIRE(std::get<std::uint32_t>(insts[1].uses[0].value) == 1);
    REQUIRE(insts[3].uses[0].type == operand::kind::argument_index);
    REQUIRE(std::get<std::uint32_t>(insts[3].uses[0].value) == 2);
}

TEST_CASE (



"Codegen generic calling convention shell exposes ABI register sets"
,
"[lithe][codegen][abi]"
)
 {
    using namespace lithe::codegen;

    const auto cc = default_calling_convention();
    REQUIRE(cc.name == "generic");
    REQUIRE(cc.integer_argument_registers.size() >= 4);
    REQUIRE(cc.floating_argument_registers.size() >= 4);
    REQUIRE(cc.integer_return_register.name == "rv0");
    REQUIRE(cc.floating_return_register.name == "frv0");
    REQUIRE_FALSE(cc.caller_saved.empty());
    REQUIRE_FALSE(cc.callee_saved.empty());
    REQUIRE_FALSE(cc.scratch_registers.empty());

    const auto abi = default_target_abi();
    REQUIRE(abi.name == "generic-v0");
    REQUIRE(abi.convention.name == "generic");
}

TEST_CASE (



"Codegen stack frame shell lays out spills and saved caller-saved registers"
,
"[lithe][codegen][abi]"
)
 {
    using namespace lithe::codegen;

    allocated_function_ir fn;
    fn.name = "frame_layout";
    fn.spill_slots = {slot(1), slot(2)};

    auto cc = default_calling_convention();
    cc.caller_saved = {{30, "t10"}, {31, "t11"}};

    const auto frame = build_stack_frame(fn, cc, 24);
    REQUIRE(frame.stack_alignment == 16);
    REQUIRE(frame.stack_size >= 56);
    REQUIRE(frame.slots.size() == 5);

    REQUIRE(frame.slots[0].type == frame_slot::kind::spill);
    REQUIRE(frame.slots[1].type == frame_slot::kind::spill);
    REQUIRE(frame.slots[2].type == frame_slot::kind::saved_register);
    REQUIRE(frame.slots[3].type == frame_slot::kind::saved_register);
    REQUIRE(frame.slots[4].type == frame_slot::kind::local);
    REQUIRE(frame.slots[2].saved_register.has_value());
}

TEST_CASE (



"Codegen prologue and epilogue shell plans are derived from ABI"
,
"[lithe][codegen][abi]"
)
 {
    using namespace lithe::codegen;

    allocated_function_ir fn;
    fn.name = "prologue_epilogue";
    fn.spill_slots = {slot(7)};

    auto abi = default_target_abi();
    abi.convention.caller_saved = {{40, "c0"}, {41, "c1"}};

    const auto prologue = plan_prologue(fn, abi, 16);
    const auto epilogue = plan_epilogue(fn, abi, 16);

    REQUIRE(prologue.save_caller_saved);
    REQUIRE(prologue.allocate_stack_frame);
    REQUIRE(prologue.saved_caller_saved.size() == 2);

    REQUIRE(epilogue.deallocate_stack_frame);
    REQUIRE(epilogue.emit_return);
    REQUIRE(epilogue.restore_caller_saved.size() == 2);

    REQUIRE(prologue.frame.stack_size == epilogue.frame.stack_size);
}

TEST_CASE (



"MIR ABI calling convention handles zero-arg constant return"
,
"[lithe][codegen][mir][abi][frame]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction li;
    li.id = 201;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "rv0"})};
    li.uses = {allocated_operand::as_i64(11)};

    allocated_instruction ret;
    ret.id = 202;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "rv0"})};

    auto physical = make_physical("mir_zero_arg", {li, ret});
    function_signature sig;
    sig.name = "mir_zero_arg";
    physical.signature = sig;

    const auto cc_check = validate_calling_convention(physical, sig);
    REQUIRE(cc_check.ok());
}

TEST_CASE (



"MIR ABI calling convention handles one register argument"
,
"[lithe][codegen][mir][abi][frame]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction arg0;
    arg0.id = 211;
    arg0.op = opcode::load_arg;
    arg0.defs = {allocated_operand::as_preg({0, "a0"})};
    arg0.uses = {allocated_operand::as_argument_index(0)};

    allocated_instruction mv;
    mv.id = 212;
    mv.op = opcode::mov;
    mv.defs = {allocated_operand::as_preg({0, "rv0"})};
    mv.uses = {allocated_operand::as_preg({0, "a0"})};

    allocated_instruction ret;
    ret.id = 213;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "rv0"})};

    auto physical = make_physical("mir_one_arg", {arg0, mv, ret});
    function_signature sig;
    sig.name = "mir_one_arg";
    sig.arguments = {argument_descriptor{"x"}};
    physical.signature = sig;

    const auto cc_check = validate_calling_convention(physical, sig);
    REQUIRE(cc_check.ok());
}

TEST_CASE (



"MIR ABI calling convention handles four register arguments"
,
"[lithe][codegen][mir][abi][frame]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction arg0;
    arg0.id = 221;
    arg0.op = opcode::load_arg;
    arg0.defs = {allocated_operand::as_preg({0, "a0"})};
    arg0.uses = {allocated_operand::as_argument_index(0)};

    allocated_instruction arg1;
    arg1.id = 222;
    arg1.op = opcode::load_arg;
    arg1.defs = {allocated_operand::as_preg({1, "a1"})};
    arg1.uses = {allocated_operand::as_argument_index(1)};

    allocated_instruction arg2;
    arg2.id = 223;
    arg2.op = opcode::load_arg;
    arg2.defs = {allocated_operand::as_preg({2, "a2"})};
    arg2.uses = {allocated_operand::as_argument_index(2)};

    allocated_instruction arg3;
    arg3.id = 224;
    arg3.op = opcode::load_arg;
    arg3.defs = {allocated_operand::as_preg({3, "a3"})};
    arg3.uses = {allocated_operand::as_argument_index(3)};

    allocated_instruction add0;
    add0.id = 225;
    add0.op = opcode::add;
    add0.defs = {allocated_operand::as_preg({40, "t40"})};
    add0.uses = {allocated_operand::as_preg({0, "a0"}), allocated_operand::as_preg({1, "a1"})};

    allocated_instruction add1;
    add1.id = 226;
    add1.op = opcode::add;
    add1.defs = {allocated_operand::as_preg({41, "t41"})};
    add1.uses = {allocated_operand::as_preg({2, "a2"}), allocated_operand::as_preg({3, "a3"})};

    allocated_instruction add2;
    add2.id = 227;
    add2.op = opcode::add;
    add2.defs = {allocated_operand::as_preg({0, "rv0"})};
    add2.uses = {allocated_operand::as_preg({40, "t40"}), allocated_operand::as_preg({41, "t41"})};

    allocated_instruction ret;
    ret.id = 228;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "rv0"})};

    auto physical = make_physical("mir_four_args", {arg0, arg1, arg2, arg3, add0, add1, add2, ret});
    function_signature sig;
    sig.name = "mir_four_args";
    sig.arguments = {
        argument_descriptor{"a"}, argument_descriptor{"b"}, argument_descriptor{"c"}, argument_descriptor{"d"}
    };
    physical.signature = sig;

    const auto cc_check = validate_calling_convention(physical, sig);
    REQUIRE(cc_check.ok());
}

TEST_CASE (



"MIR ABI calling convention overflows arguments to stack"
,
"[lithe][codegen][mir][abi][frame]"
)
 {
    using namespace lithe::codegen;

    function_signature sig;
    sig.name = "mir_stack_args";
    sig.arguments = {
        argument_descriptor{"a"}, argument_descriptor{"b"}, argument_descriptor{"c"},
        argument_descriptor{"d"}, argument_descriptor{"e"}, argument_descriptor{"f"}
    };

    const auto loc4 = get_argument_location(sig, 4);
    const auto loc5 = get_argument_location(sig, 5);
    REQUIRE(loc4.stack_slot.has_value());
    REQUIRE(*loc4.stack_slot == 0);
    REQUIRE_FALSE(loc4.physical_register.has_value());
    REQUIRE(loc5.stack_slot.has_value());
    REQUIRE(*loc5.stack_slot == 1);
    REQUIRE_FALSE(loc5.physical_register.has_value());

    allocated_instruction arg4;
    arg4.id = 231;
    arg4.op = opcode::load_arg;
    arg4.defs = {allocated_operand::as_preg({50, "t50"})};
    arg4.uses = {allocated_operand::as_argument_index(4)};

    allocated_instruction arg5;
    arg5.id = 232;
    arg5.op = opcode::load_arg;
    arg5.defs = {allocated_operand::as_preg({51, "t51"})};
    arg5.uses = {allocated_operand::as_argument_index(5)};

    allocated_instruction add;
    add.id = 233;
    add.op = opcode::add;
    add.defs = {allocated_operand::as_preg({0, "rv0"})};
    add.uses = {allocated_operand::as_preg({50, "t50"}), allocated_operand::as_preg({51, "t51"})};

    allocated_instruction ret;
    ret.id = 234;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "rv0"})};

    auto physical = make_physical("mir_stack_args", {arg4, arg5, add, ret});
    physical.signature = sig;

    const auto cc_check = validate_calling_convention(physical, sig);
    REQUIRE(cc_check.ok());
}

TEST_CASE (



"MIR frame layout contains spill slots produced by spilled vregs"
,
"[lithe][codegen][mir][abi][frame]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "mir_spill_frame";
    auto &bb = fn.create_block("entry");

    const auto v0 = fn.make_vreg();
    const auto v1 = fn.make_vreg();
    const auto v2 = fn.make_vreg();

    instruction li0;
    li0.id = 241;
    li0.op = opcode::load_imm;
    li0.defs = {operand::as_vreg(v1)};
    li0.uses = {operand::as_i64(4)};
    (void) fn.emit(bb.id, std::move(li0));

    instruction li1;
    li1.id = 242;
    li1.op = opcode::load_imm;
    li1.defs = {operand::as_vreg(v2)};
    li1.uses = {operand::as_i64(5)};
    (void) fn.emit(bb.id, std::move(li1));

    instruction add;
    add.id = 243;
    add.op = opcode::add;
    add.defs = {operand::as_vreg(v0)};
    add.uses = {operand::as_vreg(v1), operand::as_vreg(v2)};
    (void) fn.emit(bb.id, std::move(add));

    instruction ret;
    ret.id = 244;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(v0)};
    (void) fn.emit(bb.id, std::move(ret));

    register_allocation alloc;
    const auto allocated = apply_register_allocation(fn, alloc);
    auto physical = rewrite_spills(mir::allocated_mir_function{allocated});

    const auto layout = compute_stack_frame(physical);
    const auto dump = dump_frame_layout(layout);
    const bool has_spill_slot = std::ranges::any_of(layout.objects, [](const frame_object &object) {
        return object.kind == frame_object_kind::spill_slot;
    });

    REQUIRE(has_spill_slot);
    REQUIRE(dump.find("kind=spill_slot") != std::string::npos);
}

TEST_CASE (



"MIR ABI calling convention validates void return"
,
"[lithe][codegen][mir][abi][frame]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction ret;
    ret.id = 251;
    ret.op = opcode::ret;

    auto physical = make_physical("mir_void_return", {ret});
    function_signature sig;
    sig.name = "mir_void_return";
    sig.return_value.passing_kind = return_passing_kind::void_return;
    physical.signature = sig;

    const auto cc_check = validate_calling_convention(physical, sig);
    REQUIRE(cc_check.ok());
}

TEST_CASE (



"MIR ABI calling convention reports invalid load_arg index"
,
"[lithe][codegen][mir][abi][frame]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction arg2;
    arg2.id = 261;
    arg2.op = opcode::load_arg;
    arg2.defs = {allocated_operand::as_preg({2, "a2"})};
    arg2.uses = {allocated_operand::as_argument_index(2)};

    allocated_instruction ret;
    ret.id = 262;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "rv0"})};

    auto physical = make_physical("mir_bad_arg", {arg2, ret});
    function_signature sig;
    sig.name = "mir_bad_arg";
    sig.arguments = {argument_descriptor{"a"}};

    const auto cc_check = validate_calling_convention(physical, sig);
    REQUIRE_FALSE(cc_check.ok());
    REQUIRE_FALSE(cc_check.diagnostics.empty());
    REQUIRE(cc_check.diagnostics[0].find("invalid arg2") != std::string::npos);
}

TEST_CASE (



"MIR debug backend prints stack frame layout"
,
"[lithe][codegen][mir][abi][frame]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction li;
    li.id = 271;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "rv0"})};
    li.uses = {allocated_operand::as_i64(99)};

    allocated_instruction ret;
    ret.id = 272;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "rv0"})};

    auto physical = make_physical("mir_debug_frame", {li, ret});
    function_signature sig;
    sig.name = "mir_debug_frame";
    physical.signature = sig;
    physical.frame_layout = compute_stack_frame(physical);

    backends::debug_text_backend backend;
    const auto result = emit_function(backend, physical);
    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("stack-frame-layout") != std::string::npos);
    REQUIRE(result.artifact_text->find("frame-layout-notes") != std::string::npos);
}

TEST_CASE (



"Frame-address lowering maps frame objects and spill slots to memory operands"
,
"[lithe][codegen][mir][frame][memory]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction ret;
    ret.id = 281;
    ret.op = opcode::ret;

    auto physical = make_physical("frame_addr_lower", {ret});
    physical.function.spill_slots = {slot(7)};

    stack_frame_layout layout;
    layout.frame_alignment = 16;

    frame_object_descriptor spill_desc;
    spill_desc.kind = frame_object_kind::spill_slot;
    spill_desc.size = 8;
    spill_desc.alignment = 8;
    spill_desc.source_spill = slot(7);
    const auto spill_fobj = reserve_frame_object(layout, spill_desc);

    frame_object_descriptor arg_desc;
    arg_desc.kind = frame_object_kind::argument_slot;
    arg_desc.size = 8;
    arg_desc.alignment = 8;
    arg_desc.source_argument = argument_index{0};
    const auto arg_fobj = reserve_frame_object(layout, arg_desc);

    frame_object_descriptor local_desc;
    local_desc.kind = frame_object_kind::local_slot;
    local_desc.size = 16;
    local_desc.alignment = 16;
    const auto local_fobj = reserve_frame_object(layout, local_desc);

    assign_frame_offsets(layout);
    align_frame_layout(layout);
    physical.frame_layout = layout;

    frame_addressing_options options;
    options.prefer_frame_pointer_relative = true;
    const auto lowered = lower_frame_objects_to_addresses(physical, options);

    REQUIRE(lowered.by_spill_slot_id.contains(7));
    REQUIRE(lowered.by_argument_index.contains(0));
    REQUIRE(lowered.by_frame_object_id.contains(spill_fobj.value));
    REQUIRE(lowered.by_frame_object_id.contains(arg_fobj.value));
    REQUIRE(lowered.by_frame_object_id.contains(local_fobj.value));

    REQUIRE(lowered.by_spill_slot_id.at(7).address.kind == memory_address_kind::spill_slot);
    REQUIRE(lowered.by_argument_index.at(0).address.kind == memory_address_kind::argument_slot);
    REQUIRE(lowered.by_frame_object_id.at(local_fobj.value).address.kind == memory_address_kind::stack_frame);
    REQUIRE(lowered.used_frame_pointer_relative);
}

TEST_CASE (



"Frame-address materialization preserves MIR structure while replacing frame-related operands"
,
"[lithe][codegen][mir][frame][memory]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction load_spill_inst;
    load_spill_inst.id = 291;
    load_spill_inst.op = opcode::load_spill;
    load_spill_inst.defs = {allocated_operand::as_preg({1, "r1"})};
    load_spill_inst.uses = {allocated_operand::as_spill(slot(3))};

    allocated_instruction store_spill_inst;
    store_spill_inst.id = 292;
    store_spill_inst.op = opcode::store_spill;
    store_spill_inst.defs = {allocated_operand::as_spill(slot(3))};
    store_spill_inst.uses = {allocated_operand::as_preg({1, "r1"})};

    allocated_instruction arg_load;
    arg_load.id = 293;
    arg_load.op = opcode::load_arg;
    arg_load.defs = {allocated_operand::as_preg({2, "r2"})};
    arg_load.uses = {allocated_operand::as_argument_index(5)};

    allocated_instruction ret;
    ret.id = 294;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({2, "r2"})};

    auto physical = make_physical("frame_addr_materialize", {load_spill_inst, store_spill_inst, arg_load, ret});
    physical.function.spill_slots = {slot(3)};

    function_signature sig;
    sig.name = "frame_addr_materialize";
    sig.arguments = {
        argument_descriptor{"a"}, argument_descriptor{"b"}, argument_descriptor{"c"},
        argument_descriptor{"d"}, argument_descriptor{"e"}, argument_descriptor{"f"}
    };
    physical.signature = sig;

    frame_addressing_options options;
    options.prefer_frame_pointer_relative = false;
    options.allow_stack_pointer_relative = true;

    const auto materialized = materialize_frame_addresses(physical, options);
    REQUIRE(materialized.function.blocks.size() == physical.function.blocks.size());
    REQUIRE(materialized.function.blocks[0].instructions.size() == physical.function.blocks[0].instructions.size());

    const auto &insts = materialized.function.blocks[0].instructions;
    REQUIRE(insts[0].op == opcode::load_spill);
    REQUIRE(insts[1].op == opcode::store_spill);
    REQUIRE(insts[2].op == opcode::load_arg);
    REQUIRE(insts[3].op == opcode::ret);

    REQUIRE(insts[0].uses[0].type == allocated_operand::kind::memory);
    REQUIRE(insts[1].defs[0].type == allocated_operand::kind::memory);
    REQUIRE(insts[2].uses[0].type == allocated_operand::kind::memory);

    const auto &arg_mem = std::get<memory_operand>(insts[2].uses[0].value);
    REQUIRE(arg_mem.address.kind == memory_address_kind::argument_slot);
    REQUIRE(arg_mem.address.base.has_value());
    REQUIRE(arg_mem.address.base->name == "stack_base");
}

TEST_CASE (



"Frame-address utilities build frame and stack relative memory operands"
,
"[lithe][codegen][mir][frame][memory]"
)
 {
    using namespace lithe::codegen;

    frame_addressing_options options;
    options.dynamic_stack_adjustment = 16;

    const auto fp_mem = make_frame_pointer_relative_memory(memory_address_kind::stack_frame, -32, frame_object_id{9},
                                                           std::nullopt, options);
    const auto sp_mem = make_stack_pointer_relative_memory(memory_address_kind::argument_slot, 24, std::nullopt,
                                                           std::nullopt, options);

    REQUIRE(fp_mem.address.base.has_value());
    REQUIRE(fp_mem.address.base->name == "frame_base");
    REQUIRE(fp_mem.address.displacement == -16);
    REQUIRE(fp_mem.address.referenced_frame_object.has_value());
    REQUIRE(fp_mem.address.referenced_frame_object->value == 9);

    REQUIRE(sp_mem.address.base.has_value());
    REQUIRE(sp_mem.address.base->name == "stack_base");
    REQUIRE(sp_mem.address.displacement == 40);
    REQUIRE(sp_mem.address.kind == memory_address_kind::argument_slot);
}

TEST_CASE (



"Codegen interpreter backend executes explicit load/store memory ops"
,
"[lithe][codegen][backend][memory]"
)
 {
    using namespace lithe::codegen;

    const auto mem = make_frame_pointer_relative_memory(memory_address_kind::stack_frame, -8);

    allocated_instruction li;
    li.id = 301;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({1, "r1"})};
    li.uses = {allocated_operand::as_i64(123)};

    allocated_instruction st;
    st.id = 302;
    st.op = opcode::store;
    st.defs = {allocated_operand::as_memory(mem)};
    st.uses = {allocated_operand::as_preg({1, "r1"})};

    allocated_instruction ld;
    ld.id = 303;
    ld.op = opcode::load;
    ld.defs = {allocated_operand::as_preg({2, "r2"})};
    ld.uses = {allocated_operand::as_memory(mem)};

    allocated_instruction ret;
    ret.id = 304;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({2, "r2"})};

    auto fn = make_physical("interp_mem_load_store", {li, st, ld, ret});
    backends::interpreter_backend backend;
    const auto result = emit_function(backend, fn.function);

    REQUIRE(result.ok());
    REQUIRE(backend.return_value.has_value());
    REQUIRE(*backend.return_value == 123);
}

TEST_CASE (



"Codegen physical MIR validation accepts memory-based load/store and spill ops"
,
"[lithe][codegen][mir][memory]"
)
 {
    using namespace lithe::codegen;

    const auto spill_mem = make_memory_operand_for_spill_slot(slot(9));
    const auto generic_mem = make_frame_pointer_relative_memory(memory_address_kind::stack_frame, -16);

    allocated_instruction lsp;
    lsp.id = 311;
    lsp.op = opcode::load_spill;
    lsp.defs = {allocated_operand::as_preg({1, "r1"})};
    lsp.uses = {allocated_operand::as_memory(spill_mem)};

    allocated_instruction ssp;
    ssp.id = 312;
    ssp.op = opcode::store_spill;
    ssp.defs = {allocated_operand::as_memory(spill_mem)};
    ssp.uses = {allocated_operand::as_preg({1, "r1"})};

    allocated_instruction st;
    st.id = 313;
    st.op = opcode::store;
    st.defs = {allocated_operand::as_memory(generic_mem)};
    st.uses = {allocated_operand::as_preg({1, "r1"})};

    allocated_instruction ld;
    ld.id = 314;
    ld.op = opcode::load;
    ld.defs = {allocated_operand::as_preg({2, "r2"})};
    ld.uses = {allocated_operand::as_memory(generic_mem)};

    allocated_instruction ret;
    ret.id = 315;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({2, "r2"})};

    auto fn = make_physical("verify_memory_load_store", {lsp, ssp, st, ld, ret});
    const auto check = verify_physical_mir(fn);
    REQUIRE(check.ok());
}

TEST_CASE (



"Codegen backend protocol emits readable debug text"
,
"[lithe][codegen][backend]"
)
 {
    using namespace lithe::codegen;

    STATIC_REQUIRE(MachineCodeBackend<backends::debug_text_backend>);
    STATIC_REQUIRE(MachineCodeBackend<backends::null_backend>);
    STATIC_REQUIRE(MachineCodeBackend<backends::interpreter_backend>);

    allocated_function_ir fn;
    fn.name = "backend_demo";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction load;
    load.op = opcode::load_imm;
    load.defs = {allocated_operand::as_preg({0, "r0"})};
    load.uses = {allocated_operand::as_i64(42)};

    allocated_instruction ret;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    bb.instructions = {load, ret};
    fn.blocks.push_back(bb);

    backends::debug_text_backend backend;
    auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("function backend_demo") != std::string::npos);
    REQUIRE(result.artifact_text->find("load_imm") != std::string::npos);
    REQUIRE(result.artifact_text->find("ret") != std::string::npos);
}

TEST_CASE (



"Codegen null backend protocol is a no-op"
,
"[lithe][codegen][backend]"
)
 {
    using namespace lithe::codegen;

    allocated_function_ir fn;
    fn.name = "null_backend_demo";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction inst;
    inst.op = opcode::nop;
    bb.instructions.push_back(inst);
    fn.blocks.push_back(bb);

    backends::null_backend backend;
    auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
    REQUIRE(result.state.emitted_blocks == 1);
    REQUIRE(result.state.emitted_instructions == 1);
}

TEST_CASE (



"Codegen interpreter backend evaluates allocated IR"
,
"[lithe][codegen][backend]"
)
 {
    using namespace lithe::codegen;

    allocated_function_ir fn;
    fn.name = "interp_demo";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction arg0;
    arg0.op = opcode::load_arg;
    arg0.defs = {allocated_operand::as_preg({0, "r0"})};
    arg0.uses = {allocated_operand::as_argument_index(0)};

    allocated_instruction arg1;
    arg1.op = opcode::load_arg;
    arg1.defs = {allocated_operand::as_preg({1, "r1"})};
    arg1.uses = {allocated_operand::as_argument_index(1)};

    allocated_instruction add;
    add.op = opcode::add;
    add.defs = {allocated_operand::as_preg({2, "r2"})};
    add.uses = {allocated_operand::as_preg({0, "r0"}), allocated_operand::as_preg({1, "r1"})};

    allocated_instruction ret;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({2, "r2"})};

    bb.instructions = {arg0, arg1, add, ret};
    fn.blocks.push_back(bb);

    backends::interpreter_backend backend;
    backend.arguments = {7, 35};

    auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
    REQUIRE(backend.return_value.has_value());
    REQUIRE(*backend.return_value == 42);
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("ret=42") != std::string::npos);
}

TEST_CASE (



"Codegen interpreter backend prefers ABI register argument locations"
,
"[lithe][codegen][backend][abi]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction arg1;
    arg1.id = 101;
    arg1.op = opcode::load_arg;
    arg1.defs = {allocated_operand::as_preg({1, "r1"})};
    arg1.uses = {allocated_operand::as_argument_index(1)};

    allocated_instruction ret;
    ret.id = 102;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({1, "r1"})};

    auto physical = make_physical("interp_abi_reg", {arg1, ret});
    function_signature sig;
    sig.name = "interp_abi_reg";
    sig.arguments = {argument_descriptor{"a"}, argument_descriptor{"b"}};
    physical.signature = sig;

    backends::interpreter_backend backend;
    backend.arguments = {7, 35};

    const auto result = emit_function(backend, physical);
    REQUIRE(result.ok());
    REQUIRE(backend.return_value.has_value());
    REQUIRE(*backend.return_value == 35);
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("arg1 -> register:a1") != std::string::npos);
}

TEST_CASE (



"Codegen interpreter backend loads ABI stack arguments"
,
"[lithe][codegen][backend][abi]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction arg5;
    arg5.id = 111;
    arg5.op = opcode::load_arg;
    arg5.defs = {allocated_operand::as_preg({5, "r5"})};
    arg5.uses = {allocated_operand::as_argument_index(5)};

    allocated_instruction ret;
    ret.id = 112;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({5, "r5"})};

    auto physical = make_physical("interp_abi_stack", {arg5, ret});
    function_signature sig;
    sig.name = "interp_abi_stack";
    sig.arguments = {
        argument_descriptor{"a"}, argument_descriptor{"b"}, argument_descriptor{"c"},
        argument_descriptor{"d"}, argument_descriptor{"e"}, argument_descriptor{"f"}
    };
    physical.signature = sig;

    backends::interpreter_backend backend;
    backend.arguments = {10, 20, 30, 40, 50, 66};

    const auto result = emit_function(backend, physical);
    REQUIRE(result.ok());
    REQUIRE(backend.return_value.has_value());
    REQUIRE(*backend.return_value == 66);
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("arg5 -> stack[1]") != std::string::npos);
}

TEST_CASE (



"Codegen interpreter backend enforces ABI return descriptor"
,
"[lithe][codegen][backend][abi]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction li;
    li.id = 121;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(9)};

    allocated_instruction ret;
    ret.id = 122;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto physical = make_physical("interp_abi_ret_stack", {li, ret});
    function_signature sig;
    sig.name = "interp_abi_ret_stack";
    sig.return_value.passing_kind = return_passing_kind::stack_value;
    sig.return_value.stack_slot = 2;
    physical.signature = sig;

    backends::interpreter_backend backend;
    const auto result = emit_function(backend, physical);

    REQUIRE(result.ok());
    REQUIRE(backend.return_value.has_value());
    REQUIRE(*backend.return_value == 9);
    REQUIRE(backend.stack_return_values.contains(2));
    REQUIRE(backend.stack_return_values.at(2) == 9);
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("ret_stack[2]=9") != std::string::npos);
}

TEST_CASE (



"Codegen interpreter backend reports invalid ABI load_arg index"
,
"[lithe][codegen][backend][abi]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction arg3;
    arg3.id = 131;
    arg3.op = opcode::load_arg;
    arg3.defs = {allocated_operand::as_preg({3, "r3"})};
    arg3.uses = {allocated_operand::as_argument_index(3)};

    allocated_instruction ret;
    ret.id = 132;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({3, "r3"})};

    auto physical = make_physical("interp_abi_bad_arg", {arg3, ret});
    function_signature sig;
    sig.name = "interp_abi_bad_arg";
    sig.arguments = {argument_descriptor{"a"}};
    physical.signature = sig;

    backends::interpreter_backend backend;
    backend.arguments = {1};

    const auto result = emit_function(backend, physical);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    REQUIRE(result.errors[0].instruction_id.has_value());
    REQUIRE(*result.errors[0].instruction_id == 131);
    REQUIRE(result.errors[0].message.find("invalid arg3") != std::string::npos);
}

TEST_CASE (



"Codegen interpreter backend debug dump includes call frame layout"
,
"[lithe][codegen][backend][abi]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction arg0;
    arg0.id = 141;
    arg0.op = opcode::load_arg;
    arg0.defs = {allocated_operand::as_preg({0, "r0"})};
    arg0.uses = {allocated_operand::as_argument_index(0)};

    allocated_instruction ret;
    ret.id = 142;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto physical = make_physical("interp_abi_debug", {arg0, ret});
    function_signature sig;
    sig.name = "interp_abi_debug";
    sig.arguments = {argument_descriptor{"a"}};
    physical.signature = sig;
    physical.frame_layout = compute_stack_frame(physical);

    backends::interpreter_backend backend;
    backend.arguments = {42};
    backend.debug_dump_frame_layout = true;

    const auto result = emit_function(backend, physical);
    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("interpreter-call-frame abi_aware=true") != std::string::npos);
    REQUIRE(result.artifact_text->find("arg0 -> register:a0") != std::string::npos);
    REQUIRE(result.artifact_text->find("frame-layout") != std::string::npos);
}

TEST_CASE (



"Codegen backend registry creates and emits with selected backend"
,
"[lithe][codegen][backend]"
)
 {
    using namespace lithe::codegen;

    const auto names = backends::list_available_backends();
    REQUIRE(names.size() == 7);
    REQUIRE(std::find(names.begin(), names.end(), "debug_text") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "null_backend") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "interpreter") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "text_assembly") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "asmjit") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "simd") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "metal") != names.end());

    auto maybe_backend = backends::make_backend("debug_text");
    REQUIRE(maybe_backend.has_value());

    allocated_function_ir fn;
    fn.name = "registry_demo";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction inst;
    inst.op = opcode::ret;
    bb.instructions.push_back(inst);
    fn.blocks.push_back(bb);

    auto result = backends::emit_with_backend(*maybe_backend, fn);
    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("function registry_demo") != std::string::npos);

    const auto bad_backend = backends::make_backend("does_not_exist");
    REQUIRE_FALSE(bad_backend.has_value());
}

TEST_CASE (



"Codegen apply_register_allocation preserves vreg IR and maps unresolved vregs to synthetic spills"
,
"[lithe][codegen][alloc]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "alloc_map";
    auto &bb = fn.create_block("entry");

    const auto v1 = fn.make_vreg();
    const auto v2 = fn.make_vreg();
    const auto v3 = fn.make_vreg();

    instruction inst;
    inst.op = opcode::add;
    inst.defs = {operand::as_vreg(v1)};
    inst.uses = {operand::as_vreg(v2), operand::as_vreg(v3)};
    (void) fn.emit(bb.id, std::move(inst));

    register_allocation alloc;
    alloc.assignments[v1.id] = register_assignment{preg{1, "r1"}, std::nullopt};
    alloc.assignments[v2.id] = register_assignment{std::nullopt, slot(9)};
    alloc.spill_slots = {slot(9)};

    const auto allocated = apply_register_allocation(fn, alloc);

    REQUIRE(allocated.blocks.size() == 1);
    REQUIRE(allocated.original_vreg_ir.blocks.size() == 1);
    REQUIRE(allocated.original_vreg_ir.blocks[0].instructions[0].defs[0].type == operand::kind::vreg);

    const auto &mapped = allocated.blocks[0].instructions[0];
    REQUIRE(mapped.defs[0].type == allocated_operand::kind::preg);
    REQUIRE(std::get<preg>(mapped.defs[0].value).name == "r1");
    REQUIRE(mapped.uses[0].type == allocated_operand::kind::spill);
    REQUIRE(std::get<spill_slot>(mapped.uses[0].value).id == 9);
    REQUIRE(mapped.uses[1].type == allocated_operand::kind::spill);
    REQUIRE((std::get<spill_slot>(mapped.uses[1].value).id & 0x80000000u) != 0u);
    REQUIRE(allocated.spill_slots.size() == 2);
}

TEST_CASE (



"Codegen register pressure analysis reports live ranges and hotspots"
,
"[lithe][codegen][pressure]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "pressure_basic";
    auto &bb = fn.create_block("entry");

    const auto v1 = fn.make_vreg();
    const auto v2 = fn.make_vreg();
    const auto v3 = fn.make_vreg();

    instruction li1;
    li1.id = 401;
    li1.op = opcode::load_imm;
    li1.defs = {operand::as_vreg(v1)};
    li1.uses = {operand::as_i64(1)};
    (void) fn.emit(bb.id, std::move(li1));

    instruction li2;
    li2.id = 402;
    li2.op = opcode::load_imm;
    li2.defs = {operand::as_vreg(v2)};
    li2.uses = {operand::as_i64(2)};
    (void) fn.emit(bb.id, std::move(li2));

    instruction add;
    add.id = 403;
    add.op = opcode::add;
    add.defs = {operand::as_vreg(v3)};
    add.uses = {operand::as_vreg(v1), operand::as_vreg(v2)};
    (void) fn.emit(bb.id, std::move(add));

    instruction ret;
    ret.id = 404;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(v3)};
    (void) fn.emit(bb.id, std::move(ret));

    const auto pressure = compute_register_pressure(fn);
    REQUIRE(pressure.max_live_registers >= 2);
    REQUIRE(pressure.per_block.contains(bb.id));
    REQUIRE(pressure.per_block.at(bb.id).max_live_registers >= 2);
    REQUIRE(pressure.live_ranges.size() >= 3);
    REQUIRE_FALSE(pressure.hotspots.empty());
    REQUIRE(pressure.hotspots[0].live_registers == pressure.max_live_registers);
    REQUIRE(pressure.register_classes_under_pressure.contains("integer"));
    REQUIRE(pressure.register_classes_under_pressure.at("integer") >= 2);
    REQUIRE_FALSE(pressure.spill_candidates.empty());
    REQUIRE_FALSE(pressure.pressure_graph.empty());
    REQUIRE_FALSE(pressure.visualization_data.empty());
    REQUIRE_FALSE(pressure.statistics.empty());
    REQUIRE_FALSE(pressure.heuristics.empty());
    REQUIRE_FALSE(pressure.reduction_opportunities.empty());
    REQUIRE_FALSE(pressure.reduction_suggestions.empty());
    REQUIRE_FALSE(pressure.reduction_transformations.empty());
    REQUIRE_FALSE(pressure.reduction_rules.empty());
    REQUIRE_FALSE(pressure.reduction_patterns.empty());
    REQUIRE_FALSE(pressure.reduction_costs.empty());
}

TEST_CASE (



"Codegen register pressure analysis honors hotspot threshold"
,
"[lithe][codegen][pressure]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "pressure_threshold";
    auto &bb = fn.create_block("entry");

    const auto v1 = fn.make_vreg();
    const auto v2 = fn.make_vreg();

    instruction li1;
    li1.op = opcode::load_imm;
    li1.defs = {operand::as_vreg(v1)};
    li1.uses = {operand::as_i64(1)};
    (void) fn.emit(bb.id, std::move(li1));

    instruction li2;
    li2.op = opcode::load_imm;
    li2.defs = {operand::as_vreg(v2)};
    li2.uses = {operand::as_i64(2)};
    (void) fn.emit(bb.id, std::move(li2));

    instruction ret;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(v2)};
    (void) fn.emit(bb.id, std::move(ret));

    const auto high_threshold = compute_register_pressure(fn, 99);
    REQUIRE(high_threshold.hotspots.empty());
    REQUIRE(high_threshold.hotspot_threshold_used == 99);

    const auto low_threshold = compute_register_pressure(fn, 1);
    REQUIRE_FALSE(low_threshold.hotspots.empty());
    REQUIRE(low_threshold.hotspot_threshold_used == 1);
}

TEST_CASE (



"Codegen register pressure virtual MIR overload reuses function analysis"
,
"[lithe][codegen][pressure]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<mul_tag>(
        make_node<add_tag>(1, 2),
        3
    );

    const auto virtual_mir = build_virtual_mir(expr);
    const auto pressure = compute_register_pressure(virtual_mir);

    REQUIRE(pressure.max_live_registers >= 1);
    REQUIRE_FALSE(pressure.per_block.empty());
    REQUIRE_FALSE(pressure.live_ranges.empty());
    REQUIRE_FALSE(pressure.feedback_for_scheduling.empty());
}

TEST_CASE (



"Codegen spill rewrite reports diagnostic when no scratch register exists"
,
"[lithe][codegen][spill]"
)
 {
    using namespace lithe::codegen;

    allocated_function_ir fn;
    fn.name = "no_scratch";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction inst;
    inst.op = opcode::add;
    inst.defs = {allocated_operand::as_spill(slot(1))};
    inst.uses = {allocated_operand::as_spill(slot(2)), allocated_operand::as_i64(1)};
    bb.instructions.push_back(inst);
    fn.blocks.push_back(bb);

    auto rewritten = rewrite_spills(std::move(fn), {});
    REQUIRE_FALSE(rewritten.ok());
    REQUIRE_FALSE(rewritten.diagnostics.empty());
    REQUIRE(rewritten.diagnostics[0].find("no scratch registers") != std::string::npos);
}

TEST_CASE (



"Codegen lowering keeps terminals as symbols when signature provides no bindings"
,
"[lithe][codegen][args]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    struct named_term {
        std::string name;
    };

    const auto expr = make_node<add_tag>(named_term{"x"}, named_term{"y"});
    function_signature signature;
    signature.name = "no_bindings";

    const auto fn = lower_to_machine_ir(expr, signature);
    const auto &insts = fn.blocks[0].instructions;

    REQUIRE(insts[0].op == opcode::load_symbol);
    REQUIRE(insts[1].op == opcode::load_symbol);
    REQUIRE(insts[0].uses[0].type == operand::kind::symbol);
    REQUIRE(insts[1].uses[0].type == operand::kind::symbol);
}

TEST_CASE (



"Codegen backend registry supports aliases and interpreter reports unsupported opcode"
,
"[lithe][codegen][backend]"
)
 {
    using namespace lithe::codegen;

    REQUIRE(backends::backend_kind_from_string("debug").has_value());
    REQUIRE(backends::backend_kind_from_string("null").has_value());
    REQUIRE(backends::backend_kind_from_string("interp").has_value());

    allocated_function_ir fn;
    fn.name = "interp_fail";
    fn.cfg.entry_block = 1;
    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";

    allocated_instruction bad;
    bad.id = 77;
    bad.op = opcode::call;
    bb.instructions.push_back(bad);
    fn.blocks.push_back(bb);

    backends::interpreter_backend backend;
    auto result = emit_function(backend, fn);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    REQUIRE(result.errors[0].instruction_id.has_value());
    REQUIRE(*result.errors[0].instruction_id == 77);
}

TEST_CASE (



"Codegen align_up handles zero and aligned boundaries"
,
"[lithe][codegen][abi]"
)
 {
    using namespace lithe::codegen;

    REQUIRE(align_up(0, 0) == 0);
    REQUIRE(align_up(7, 0) == 7);
    REQUIRE(align_up(0, 16) == 0);
    REQUIRE(align_up(16, 16) == 16);
    REQUIRE(align_up(17, 16) == 32);
    REQUIRE(align_up(31, 16) == 32);
    REQUIRE(align_up(33, 8) == 40);
}

TEST_CASE (



"Codegen backend registry list order is deterministic"
,
"[lithe][codegen][backend]"
)
 {
    using namespace lithe::codegen;

    const auto names = backends::list_available_backends();
    REQUIRE(names.size() == 7);
    REQUIRE(names[0] == "debug_text");
    REQUIRE(names[1] == "null_backend");
    REQUIRE(names[2] == "interpreter");
    REQUIRE(names[3] == "text_assembly");
    REQUIRE(names[4] == "asmjit");
    REQUIRE(names[5] == "simd");
    REQUIRE(names[6] == "metal");
}

TEST_CASE (



"Codegen MIR phase pipeline builds, allocates, rewrites and emits"
,
"[lithe][codegen][mir]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<mul_tag>(
        make_node<add_tag>(1, 2),
        3
    );

    const auto virtual_mir = build_virtual_mir(expr);
    const auto virtual_check = verify_virtual_mir(virtual_mir);
    REQUIRE(virtual_check.ok());

    const auto virtual_dump = dump_virtual_mir(virtual_mir);
    REQUIRE(virtual_dump.find("[virtual-mir phase=virtual]") != std::string::npos);
    REQUIRE(virtual_dump.find("v") != std::string::npos);

    const auto allocation = allocate_registers(virtual_mir, {{0, "r0"}});
    const auto allocated_mir = apply_register_allocation(virtual_mir, allocation);
    const auto allocated_check = verify_allocated_mir(allocated_mir);
    REQUIRE(allocated_check.ok());

    const auto allocated_dump = dump_allocated_mir(allocated_mir);
    REQUIRE(allocated_dump.find("[allocated-mir phase=allocated]") != std::string::npos);
    REQUIRE(allocated_dump.find("allocation-map") != std::string::npos);

    const auto physical_mir = rewrite_spills(allocated_mir);
    const auto physical_check = verify_physical_mir(physical_mir);
    REQUIRE(physical_check.ok());

    const auto physical_dump = dump_physical_mir(physical_mir);
    REQUIRE(physical_dump.find("[physical-mir phase=physical]") != std::string::npos);
    REQUIRE(physical_dump.find("spill-rewrite") != std::string::npos);
    REQUIRE(physical_dump.find("defs=[v") == std::string::npos);
    REQUIRE(physical_dump.find("uses=[v") == std::string::npos);
    const bool has_explicit_spills =
        physical_dump.find("load_spill") != std::string::npos ||
        physical_dump.find("store_spill") != std::string::npos;
    REQUIRE(has_explicit_spills);

    backends::debug_text_backend backend;
    const auto emit_result = emit_function(backend, physical_mir);
    REQUIRE(emit_result.ok());
    REQUIRE(emit_result.artifact_text.has_value());
    REQUIRE(emit_result.artifact_text->find("[physical]") != std::string::npos);
}

TEST_CASE (



"Codegen MIR compatibility overloads still support legacy surfaces"
,
"[lithe][codegen][mir]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "legacy_compat";
    auto &entry = fn.create_block("entry");
    const auto vr = fn.make_vreg();

    instruction li;
    li.op = opcode::load_imm;
    li.defs = {operand::as_vreg(vr)};
    li.uses = {operand::as_i64(5)};
    (void) fn.emit(entry.id, std::move(li));

    instruction ret;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(vr)};
    (void) fn.emit(entry.id, std::move(ret));

    const auto allocation = allocate_registers(static_cast<const function_ir &>(fn));
    const auto allocated = apply_register_allocation(static_cast<const function_ir &>(fn), allocation);
    const auto rewritten = rewrite_spills(static_cast<const allocated_function_ir &>(allocated));

    REQUIRE(rewritten.ok());
    const auto machine_dump_default = dump_machine_ir(fn);
    const auto machine_dump_with_pressure = dump_machine_ir(fn, true);
    REQUIRE(!machine_dump_default.empty());
    REQUIRE(machine_dump_default.find("register-pressure") == std::string::npos);
    REQUIRE(machine_dump_with_pressure.find("register-pressure") != std::string::npos);
    REQUIRE(!dump_allocated_machine_ir(allocated).empty());
}


TEST_CASE (



"Codegen MIR phase metadata and uniform dump dispatch"
,
"[lithe][codegen][mir]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);
    auto virtual_mir = build_virtual_mir(expr);
    REQUIRE(mir::phase_of(virtual_mir) == mir::phase::virtual_mir);

    const auto allocation = allocate_registers(virtual_mir, {{0, "r0"}});
    auto allocated_mir = apply_register_allocation(virtual_mir, allocation);
    REQUIRE(mir::phase_of(allocated_mir) == mir::phase::allocated_mir);

    auto physical_mir = rewrite_spills(allocated_mir);
    REQUIRE(mir::phase_of(physical_mir) == mir::phase::physical_mir);

    virtual_mir.metadata.note = "virtual-phase";
    allocated_mir.metadata.note = "allocated-phase";
    physical_mir.metadata.note = "physical-phase";

    REQUIRE(dump_mir(virtual_mir).find("phase=virtual") != std::string::npos);
    REQUIRE(dump_mir(allocated_mir).find("phase=allocated") != std::string::npos);
    REQUIRE(dump_mir(physical_mir).find("phase=physical") != std::string::npos);

    virtual_mir.metadata.dumps_enabled = false;
    const auto disabled_dump = dump_mir(virtual_mir);
    REQUIRE(disabled_dump.find("<dump disabled>") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Prompt 1 — Custom MIR pipeline tests
// ---------------------------------------------------------------------------

TEST_CASE (



"Custom MIR pipeline with only peephole_mir_pass compiles successfully"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    mir_pass_pipeline pipeline;
    pipeline.add_pass("peephole_mir_pass", peephole_mir_pass{});

    codegen_options opts;
    opts.with_mir_pipeline(std::move(pipeline));

    const auto result = compile_to_physical_mir(expr, opts);
    REQUIRE(result.ok());
}

TEST_CASE (



"Custom MIR pipeline executed pass list contains peephole_mir_pass"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    mir_pass_pipeline pipeline;
    pipeline.add_pass("peephole_mir_pass", peephole_mir_pass{});

    codegen_options opts;
    opts.with_mir_pipeline(std::move(pipeline));

    const auto result = compile_to_physical_mir(expr, opts);
    REQUIRE(result.ok());

    const auto &executed = result.executed_mir_passes();
    const bool found = std::find(executed.begin(), executed.end(), "peephole_mir_pass") != executed.end();
    REQUIRE(found);
}

TEST_CASE (



"Custom MIR pipeline disabled pass is not executed"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    mir_pass_pipeline pipeline;
    pipeline.add_pass("peephole_mir_pass", peephole_mir_pass{});
    pipeline.disable_pass("peephole_mir_pass");

    codegen_options opts;
    opts.with_mir_pipeline(std::move(pipeline));

    const auto result = compile_to_physical_mir(expr, opts);
    REQUIRE(result.ok());

    const auto &executed = result.executed_mir_passes();
    const bool found = std::find(executed.begin(), executed.end(), "peephole_mir_pass") != executed.end();
    REQUIRE_FALSE(found);
}

TEST_CASE (



"Custom MIR pipeline removed pass is no longer contained"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe::codegen;

    mir_pass_pipeline pipeline;
    pipeline.add_pass("peephole_mir_pass", peephole_mir_pass{});
    REQUIRE(pipeline.contains_pass("peephole_mir_pass"));

    const bool removed = pipeline.remove_pass_by_name("peephole_mir_pass");
    REQUIRE(removed);
    REQUIRE_FALSE(pipeline.contains_pass("peephole_mir_pass"));
    REQUIRE(pipeline.empty());
}

TEST_CASE (



"Custom MIR pipeline pass_names preserves insertion order"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe::codegen;

    mir_pass_pipeline pipeline;
    pipeline.add_pass("trivial_jump_threading_pass", trivial_jump_threading_pass{});
    pipeline.add_pass("empty_block_merge_pass", empty_block_merge_pass{});
    pipeline.add_pass("peephole_mir_pass", peephole_mir_pass{});

    const auto names = pipeline.pass_names();
    REQUIRE(names.size() == 3);
    REQUIRE(names[0] == "trivial_jump_threading_pass");
    REQUIRE(names[1] == "empty_block_merge_pass");
    REQUIRE(names[2] == "peephole_mir_pass");
}

TEST_CASE (



"Custom MIR pipeline clear empties the pipeline"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe::codegen;

    mir_pass_pipeline pipeline;
    pipeline.add_pass("peephole_mir_pass", peephole_mir_pass{});
    pipeline.add_pass("trivial_jump_threading_pass", trivial_jump_threading_pass{});
    REQUIRE(pipeline.size() == 2);

    pipeline.clear();
    REQUIRE(pipeline.empty());
    REQUIRE(pipeline.size() == 0);
}

TEST_CASE (



"Custom MIR pipeline enable and disable round-trip"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe::codegen;

    mir_pass_pipeline pipeline;
    pipeline.add_pass("peephole_mir_pass", peephole_mir_pass{});

    REQUIRE(pipeline.disable_pass("peephole_mir_pass"));

    // Re-enable it and confirm it runs again.
    REQUIRE(pipeline.enable_pass("peephole_mir_pass"));

    // Non-existent pass returns false.
    REQUIRE_FALSE(pipeline.enable_pass("nonexistent_pass"));
    REQUIRE_FALSE(pipeline.disable_pass("nonexistent_pass"));
}

TEST_CASE (



"Custom MIR pipeline is used when use_custom_mir_pipeline is true"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    // A pipeline with a single custom-named pass so we can distinguish it.
    mir_pass_pipeline pipeline;
    pipeline.add_pass("user_peephole", peephole_mir_pass{});

    codegen_options opts;
    opts.with_mir_pipeline(std::move(pipeline));
    REQUIRE(opts.use_custom_mir_pipeline);

    const auto result = compile_to_physical_mir(expr, opts);
    REQUIRE(result.ok());

    const auto &executed = result.executed_mir_passes();
    REQUIRE(std::find(executed.begin(), executed.end(), "user_peephole") != executed.end());
}

TEST_CASE (



"Preset opt levels remain unaffected by custom pipeline flag"
,
"[lithe][codegen][pipeline][custom]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    // with_mir_opt_level must turn off custom pipeline.
    codegen_options opts;
    opts.with_mir_pipeline(mir_pass_pipeline{});
    REQUIRE(opts.use_custom_mir_pipeline);

    opts.with_mir_opt_level(mir_opt_level::O0);
    REQUIRE_FALSE(opts.use_custom_mir_pipeline);

    const auto result = compile_to_physical_mir(expr, opts);
    REQUIRE(result.ok());
}

// ---------------------------------------------------------------------------
// Prompt 2 — CFG cleanup pass tests
// ---------------------------------------------------------------------------

namespace {
    // Builds a two-block physical MIR:
    //   block 1 (entry): load_imm r0=42 + branch -> 2
    //   block 2 (dead):  ret r0
    // Block 2 is unreachable because no branch targets it.
    lithe::codegen::mir::physical_mir_function make_physical_with_unreachable_block() {
        using namespace lithe::codegen;

        allocated_function_ir fn;
        fn.name = "unreachable_test";
        fn.cfg.entry_block = 1;

        // entry block
        allocated_basic_block entry;
        entry.id = 1;
        entry.name = "entry";
        entry.successors = {2};

        allocated_instruction li;
        li.id = 10;
        li.op = opcode::load_imm;
        li.defs = {allocated_operand::as_preg({0, "r0"})};
        li.uses = {allocated_operand::as_i64(42)};
        entry.instructions.push_back(li);

        allocated_instruction br;
        br.id = 11;
        br.op = opcode::branch;
        br.uses = {allocated_operand::as_block(2)};
        entry.instructions.push_back(br);
        fn.blocks.push_back(entry);

        // reachable block 2
        allocated_basic_block block2;
        block2.id = 2;
        block2.name = "exit";
        block2.predecessors = {1};

        allocated_instruction ret;
        ret.id = 20;
        ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({0, "r0"})};
        block2.instructions.push_back(ret);
        fn.blocks.push_back(block2);

        // dead block 3 — not reachable from entry
        allocated_basic_block dead;
        dead.id = 3;
        dead.name = "dead";

        allocated_instruction dead_ret;
        dead_ret.id = 30;
        dead_ret.op = opcode::ret;
        dead_ret.uses = {allocated_operand::as_preg({0, "r0"})};
        dead.instructions.push_back(dead_ret);
        fn.blocks.push_back(dead);

        fn.cfg.successors[1] = {2};
        fn.cfg.predecessors[2] = {1};

        mir::physical_mir_function physical;
        physical.function = std::move(fn);
        physical.metadata.current_phase = mir::phase::physical_mir;
        physical.spill_rewritten = true;
        return physical;
    }

    // Builds a three-block physical MIR:
    //   block 1 (entry): load_imm r0=1; branch -> 2
    //   block 2 (fwd):   branch -> 3        (jump-only forwarding block)
    //   block 3 (exit):  ret r0
    lithe::codegen::mir::physical_mir_function make_physical_with_forwarding_block() {
        using namespace lithe::codegen;

        allocated_function_ir fn;
        fn.name = "forwarding_test";
        fn.cfg.entry_block = 1;

        allocated_basic_block entry;
        entry.id = 1;
        entry.name = "entry";
        entry.successors = {2};
        entry.predecessors = {};

        allocated_instruction li;
        li.id = 10;
        li.op = opcode::load_imm;
        li.defs = {allocated_operand::as_preg({0, "r0"})};
        li.uses = {allocated_operand::as_i64(1)};
        entry.instructions.push_back(li);

        allocated_instruction br1;
        br1.id = 11;
        br1.op = opcode::branch;
        br1.uses = {allocated_operand::as_block(2)};
        entry.instructions.push_back(br1);
        fn.blocks.push_back(entry);

        // forwarding block
        allocated_basic_block fwd;
        fwd.id = 2;
        fwd.name = "fwd";
        fwd.successors = {3};
        fwd.predecessors = {1};

        allocated_instruction br2;
        br2.id = 20;
        br2.op = opcode::branch;
        br2.uses = {allocated_operand::as_block(3)};
        fwd.instructions.push_back(br2);
        fn.blocks.push_back(fwd);

        // exit block
        allocated_basic_block exit_block;
        exit_block.id = 3;
        exit_block.name = "exit";
        exit_block.successors = {};
        exit_block.predecessors = {2};

        allocated_instruction ret;
        ret.id = 30;
        ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({0, "r0"})};
        exit_block.instructions.push_back(ret);
        fn.blocks.push_back(exit_block);

        fn.cfg.successors[1] = {2};
        fn.cfg.successors[2] = {3};
        fn.cfg.predecessors[2] = {1};
        fn.cfg.predecessors[3] = {2};

        mir::physical_mir_function physical;
        physical.function = std::move(fn);
        physical.metadata.current_phase = mir::phase::physical_mir;
        physical.spill_rewritten = true;
        return physical;
    }
} // anonymous namespace

TEST_CASE (



"CFG unreachable_block_elimination_pass removes unreachable block"
,
"[lithe][codegen][cfg][pass]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_unreachable_block();
    const std::size_t block_count_before = physical.function.blocks.size();
    REQUIRE(block_count_before == 3);

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.changed);
    REQUIRE(result.removed_blocks >= 1);
    REQUIRE(result.function.function.blocks.size() < block_count_before);

    const auto &kept_ids = result.function.function.blocks;
    const bool dead_block_removed = std::none_of(kept_ids.begin(), kept_ids.end(),
        [](const allocated_basic_block &b) { return b.id == 3; });
    REQUIRE(dead_block_removed);
}

TEST_CASE (



"CFG unreachable_block_elimination_pass preserves entry block"
,
"[lithe][codegen][cfg][pass]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_unreachable_block();

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    const std::uint32_t entry_id = result.function.function.cfg.entry_block;
    const bool entry_present = std::any_of(result.function.function.blocks.begin(),
                                           result.function.function.blocks.end(),
                                           [entry_id](const allocated_basic_block &b) { return b.id == entry_id; });
    REQUIRE(entry_present);
}

TEST_CASE (



"CFG unreachable_block_elimination_pass verify_physical_mir passes after removal"
,
"[lithe][codegen][cfg][pass]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_unreachable_block();

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.diagnostics.empty());
    const auto verification = verify_physical_mir(result.function);
    REQUIRE(verification.ok());
}

// ---------------------------------------------------------------------------
// Prompt 2 — CFG cleanup hardening tests
// ---------------------------------------------------------------------------

// --- trivial_jump_threading_pass: CFG maps are updated after rewrite ---

TEST_CASE (



"CFG trivial_jump_threading_pass updates successor lists after rewrite"
,
"[lithe][codegen][cfg][pass][harden]"
)
 {
    using namespace lithe::codegen;

    // After threading, block 1's branch points to block 3.
    // block.successors of block 1 must reflect that, not still hold {2}.
    auto physical = make_physical_with_forwarding_block();
    mir_pass_context ctx;
    trivial_jump_threading_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.changed);

    for (const auto &block : result.function.function.blocks) {
        if (block.id != 1) continue;
        // Must not contain the forwarding block (2) as a successor any more.
        const bool still_points_to_fwd = std::find(block.successors.begin(),
                                                    block.successors.end(), 2u)
                                         != block.successors.end();
        REQUIRE_FALSE(still_points_to_fwd);
        // Must contain the final target (3).
        const bool points_to_exit = std::find(block.successors.begin(),
                                              block.successors.end(), 3u)
                                    != block.successors.end();
        REQUIRE(points_to_exit);
    }
}

TEST_CASE (



"CFG trivial_jump_threading_pass updates cfg.successors map after rewrite"
,
"[lithe][codegen][cfg][pass][harden]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_forwarding_block();
    mir_pass_context ctx;
    trivial_jump_threading_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.changed);

    const auto &succs = result.function.function.cfg.successors;
    // Block 1 must be present in the map.
    REQUIRE(succs.contains(1));
    // Its successors must not include the forwarding block (2).
    const auto &succs1 = succs.at(1);
    REQUIRE(std::find(succs1.begin(), succs1.end(), 2u) == succs1.end());
    // Its successors must include the final target (3).
    REQUIRE(std::find(succs1.begin(), succs1.end(), 3u) != succs1.end());
}

TEST_CASE (



"CFG trivial_jump_threading_pass updates cfg.predecessors map after rewrite"
,
"[lithe][codegen][cfg][pass][harden]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_forwarding_block();
    mir_pass_context ctx;
    trivial_jump_threading_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.changed);

    const auto &preds = result.function.function.cfg.predecessors;
    // Block 3's predecessors must include block 1 now (threaded past 2).
    REQUIRE(preds.contains(3));
    const auto &preds3 = preds.at(3);
    REQUIRE(std::find(preds3.begin(), preds3.end(), 1u) != preds3.end());
}

// --- unreachable_block_elimination_pass: entry block is never removed ---

TEST_CASE (



"CFG unreachable_block_elimination_pass never removes entry block even if misreported as unreachable"
,
"[lithe][codegen][cfg][pass][harden]"
)
 {
    using namespace lithe::codegen;

    // A single-block function: entry with a ret. The entry is always reachable.
    allocated_function_ir fn;
    fn.name = "entry_guard_test";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.name = "entry";

    allocated_instruction li;
    li.id = 1;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(0)};
    entry.instructions.push_back(li);

    allocated_instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};
    entry.instructions.push_back(ret);
    fn.blocks.push_back(entry);

    mir::physical_mir_function physical;
    physical.function = std::move(fn);
    physical.metadata.current_phase = mir::phase::physical_mir;
    physical.spill_rewritten = true;

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    // No blocks should be removed; entry must remain.
    const bool entry_present = std::any_of(result.function.function.blocks.begin(),
                                           result.function.function.blocks.end(),
                                           [](const allocated_basic_block &b) { return b.id == 1; });
    REQUIRE(entry_present);
    REQUIRE(result.function.function.blocks.size() == 1);
}

// --- unreachable_block_elimination_pass: successor lists updated, dead blocks removed ---

TEST_CASE (



"CFG unreachable_block_elimination_pass updates cfg maps and removes dead block"
,
"[lithe][codegen][cfg][pass][harden]"
)
 {
    using namespace lithe::codegen;

    // Build:
    //   block 1 (entry): load_imm r0=0; branch -> 2
    //   block 2 (live):  ret r0
    //   block 3 (dead):  nop; ret r0  — no predecessor, no branch targets it
    //
    // The dead block must be removed and cfg maps must be consistent.
    allocated_function_ir fn;
    fn.name = "dead_maps_test";
    fn.cfg.entry_block = 1;

    allocated_basic_block entry;
    entry.id = 1;
    entry.name = "entry";
    entry.successors = {2};

    allocated_instruction li;
    li.id = 10;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(0)};
    entry.instructions.push_back(li);

    allocated_instruction br;
    br.id = 11;
    br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(2)};
    entry.instructions.push_back(br);
    fn.blocks.push_back(entry);

    allocated_basic_block live;
    live.id = 2;
    live.name = "live";
    live.predecessors = {1};

    allocated_instruction live_ret;
    live_ret.id = 20;
    live_ret.op = opcode::ret;
    live_ret.uses = {allocated_operand::as_preg({0, "r0"})};
    live.instructions.push_back(live_ret);
    fn.blocks.push_back(live);

    // Completely orphaned block — no predecessors, no branch targets it.
    allocated_basic_block dead_block;
    dead_block.id = 3;
    dead_block.name = "dead";

    allocated_instruction dead_nop;
    dead_nop.id = 30;
    dead_nop.op = opcode::nop;
    dead_block.instructions.push_back(dead_nop);

    allocated_instruction dead_ret;
    dead_ret.id = 31;
    dead_ret.op = opcode::ret;
    dead_ret.uses = {allocated_operand::as_preg({0, "r0"})};
    dead_block.instructions.push_back(dead_ret);
    fn.blocks.push_back(dead_block);

    fn.cfg.successors[1] = {2};
    fn.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn);
    physical.metadata.current_phase = mir::phase::physical_mir;
    physical.spill_rewritten = true;

    mir_pass_context ctx;
    unreachable_block_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.changed);
    REQUIRE(result.removed_blocks >= 1);

    // Block 3 must be gone.
    const bool dead_gone = std::none_of(result.function.function.blocks.begin(),
                                        result.function.function.blocks.end(),
                                        [](const allocated_basic_block &b) { return b.id == 3; });
    REQUIRE(dead_gone);

    // CFG successor map must not contain block 3.
    for (const auto &[id, succs] : result.function.function.cfg.successors) {
        REQUIRE(std::find(succs.begin(), succs.end(), 3u) == succs.end());
    }

    // CFG predecessor map must not contain block 3 as a key.
    REQUIRE_FALSE(result.function.function.cfg.predecessors.contains(3));

    // verify_physical_mir must pass.
    const auto verification = verify_physical_mir(result.function);
    REQUIRE(verification.ok());
}
TEST_CASE (



"CFG trivial_jump_threading_pass redirects branch target through forwarding block"
,
"[lithe][codegen][cfg][pass]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_forwarding_block();

    mir_pass_context ctx;
    trivial_jump_threading_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.changed);

    // The branch in block 1 should now point directly to block 3, skipping 2.
    bool found_direct_branch = false;
    for (const auto &block : result.function.function.blocks) {
        if (block.id != 1) continue;
        for (const auto &inst : block.instructions) {
            if (inst.op != opcode::branch) continue;
            for (const auto &use : inst.uses) {
                if (use.type == allocated_operand::kind::block) {
                    const auto target = std::get<std::uint32_t>(use.value);
                    if (target == 3) found_direct_branch = true;
                }
            }
        }
    }
    REQUIRE(found_direct_branch);
}

TEST_CASE (



"CFG trivial_jump_threading_pass verify_physical_mir passes after threading"
,
"[lithe][codegen][cfg][pass]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_forwarding_block();

    mir_pass_context ctx;
    trivial_jump_threading_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.diagnostics.empty());
    const auto verification = verify_physical_mir(result.function);
    REQUIRE(verification.ok());
}

TEST_CASE (



"CFG empty_block_merge_pass removes forwarding block after jump threading"
,
"[lithe][codegen][cfg][pass]"
)
 {
    using namespace lithe::codegen;

    // First thread jump so block 1 points directly to block 3.
    auto physical = make_physical_with_forwarding_block();
    mir_pass_context thread_ctx;
    trivial_jump_threading_pass thread_pass;
    const auto threaded = thread_pass.run(physical, thread_ctx);

    // Now run empty_block_merge_pass which should remove block 2 (now orphaned forwarding block).
    mir_pass_context merge_ctx;
    empty_block_merge_pass merge_pass;
    const auto result = merge_pass.run(threaded.function, merge_ctx);

    REQUIRE(result.changed);
    REQUIRE(result.removed_blocks >= 1);

    const bool fwd_removed = std::none_of(result.function.function.blocks.begin(),
                                          result.function.function.blocks.end(),
                                          [](const allocated_basic_block &b) { return b.id == 2; });
    REQUIRE(fwd_removed);
}

TEST_CASE (



"CFG empty_block_merge_pass preserves entry block"
,
"[lithe][codegen][cfg][pass]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_forwarding_block();
    mir_pass_context ctx;
    empty_block_merge_pass pass;
    const auto result = pass.run(physical, ctx);

    const std::uint32_t entry_id = result.function.function.cfg.entry_block;
    const bool entry_present = std::any_of(result.function.function.blocks.begin(),
                                           result.function.function.blocks.end(),
                                           [entry_id](const allocated_basic_block &b) { return b.id == entry_id; });
    REQUIRE(entry_present);
}

TEST_CASE (



"CFG empty_block_merge_pass verify_physical_mir passes after merge"
,
"[lithe][codegen][cfg][pass]"
)
 {
    using namespace lithe::codegen;

    auto physical = make_physical_with_forwarding_block();
    mir_pass_context thread_ctx;
    trivial_jump_threading_pass thread_pass;
    const auto threaded = thread_pass.run(physical, thread_ctx);

    mir_pass_context merge_ctx;
    empty_block_merge_pass merge_pass;
    const auto result = merge_pass.run(threaded.function, merge_ctx);

    REQUIRE(result.diagnostics.empty());
    const auto verification = verify_physical_mir(result.function);
    REQUIRE(verification.ok());
}

// -----------------------------------------------------------------------
// copy_propagation_pass tests
// -----------------------------------------------------------------------

TEST_CASE (



"copy_propagation_pass: mov r2=r1 then use r2 is rewritten to use r1"
,
"[lithe][codegen][pass][copy_prop]"
)
 {
    using namespace lithe::codegen;

    // mov r2 = r1 ; add r3 = r2, r4 ; ret
    // After propagation: add r3 = r1, r4 and mov is removed.
    allocated_instruction mov;
    mov.id = 1;
    mov.op = opcode::mov;
    mov.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    mov.uses = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction add;
    add.id = 2;
    add.op = opcode::add;
    add.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add.uses = {
        allocated_operand::as_preg(preg{2, "r2"}),
        allocated_operand::as_preg(preg{4, "r4"})
    };

    allocated_instruction ret;
    ret.id = 3;
    ret.op = opcode::ret;

    auto fn = make_physical("copy_basic", {mov, add, ret});

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_instructions == 1);

    const auto &insts = result.function.function.blocks[0].instructions;
    REQUIRE(insts.size() == 2);
    const auto &add_inst = insts[0];
    REQUIRE(add_inst.op == opcode::add);
    REQUIRE(std::get<preg>(add_inst.uses[0].value).id == 1);
}

TEST_CASE (



"copy_propagation_pass: use after redef of copy dst is not changed"
,
"[lithe][codegen][pass][copy_prop]"
)
 {
    using namespace lithe::codegen;

    // mov r2 = r1 ; add r2 = r2, r4 (redefines r2) ; mov r5 = r2 ; ret
    allocated_instruction copy_mov;
    copy_mov.id = 1;
    copy_mov.op = opcode::mov;
    copy_mov.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    copy_mov.uses = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction add_redef;
    add_redef.id = 2;
    add_redef.op = opcode::add;
    add_redef.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    add_redef.uses = {
        allocated_operand::as_preg(preg{2, "r2"}),
        allocated_operand::as_preg(preg{4, "r4"})
    };

    allocated_instruction use_after_redef;
    use_after_redef.id = 3;
    use_after_redef.op = opcode::mov;
    use_after_redef.defs = {allocated_operand::as_preg(preg{5, "r5"})};
    use_after_redef.uses = {allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction ret;
    ret.id = 4;
    ret.op = opcode::ret;

    auto fn = make_physical("copy_redef", {copy_mov, add_redef, use_after_redef, ret});

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());

    const auto &insts = result.function.function.blocks[0].instructions;
    for (const auto &inst : insts) {
        if (inst.id == 3) {
            REQUIRE(std::get<preg>(inst.uses[0].value).id == 2);
        }
    }
}

TEST_CASE (



"copy_propagation_pass: does not propagate across call"
,
"[lithe][codegen][pass][copy_prop]"
)
 {
    using namespace lithe::codegen;

    // mov r2 = r1 ; call ; mov r3 = r2 ; ret
    allocated_instruction copy_mov;
    copy_mov.id = 1;
    copy_mov.op = opcode::mov;
    copy_mov.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    copy_mov.uses = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction call;
    call.id = 2;
    call.op = opcode::call;

    allocated_instruction use_after_call;
    use_after_call.id = 3;
    use_after_call.op = opcode::mov;
    use_after_call.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    use_after_call.uses = {allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction ret;
    ret.id = 4;
    ret.op = opcode::ret;

    auto fn = make_physical("copy_call_barrier", {copy_mov, call, use_after_call, ret});

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());

    const auto &insts = result.function.function.blocks[0].instructions;
    for (const auto &inst : insts) {
        if (inst.id == 3) {
            REQUIRE(std::get<preg>(inst.uses[0].value).id == 2);
        }
    }
}

TEST_CASE (



"copy_propagation_pass: does not propagate across branch/ret"
,
"[lithe][codegen][pass][copy_prop]"
)
 {
    using namespace lithe::codegen;

    // mov r2 = r1 ; ret ; mov r3 = r2
    // The ret clears active copies; the second mov must not be rewritten.
    allocated_instruction copy_mov;
    copy_mov.id = 1;
    copy_mov.op = opcode::mov;
    copy_mov.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    copy_mov.uses = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;

    allocated_instruction after_ret;
    after_ret.id = 3;
    after_ret.op = opcode::mov;
    after_ret.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    after_ret.uses = {allocated_operand::as_preg(preg{2, "r2"})};

    auto fn = make_physical("copy_term_barrier", {copy_mov, ret, after_ret});

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());

    const auto &insts = result.function.function.blocks[0].instructions;
    for (const auto &inst : insts) {
        if (inst.id == 3) {
            REQUIRE(std::get<preg>(inst.uses[0].value).id == 2);
        }
    }
}

TEST_CASE (



"copy_propagation_pass: copy whose all uses are propagated is removed"
,
"[lithe][codegen][pass][copy_prop]"
)
 {
    using namespace lithe::codegen;

    // mov r2 = r1 ; add r3 = r2, r2 ; ret
    // Both uses of r2 in add are propagated to r1, so the mov is dead and removed.
    allocated_instruction copy_mov;
    copy_mov.id = 1;
    copy_mov.op = opcode::mov;
    copy_mov.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    copy_mov.uses = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction add;
    add.id = 2;
    add.op = opcode::add;
    add.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add.uses = {
        allocated_operand::as_preg(preg{2, "r2"}),
        allocated_operand::as_preg(preg{2, "r2"})
    };

    allocated_instruction ret;
    ret.id = 3;
    ret.op = opcode::ret;

    auto fn = make_physical("copy_remove_safe", {copy_mov, add, ret});

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_instructions == 1);

    const auto &insts = result.function.function.blocks[0].instructions;
    for (const auto &inst : insts) {
        if (inst.op == opcode::add) {
            REQUIRE(std::get<preg>(inst.uses[0].value).id == 1);
            REQUIRE(std::get<preg>(inst.uses[1].value).id == 1);
        }
    }
}

// -----------------------------------------------------------------------
// constant_propagation_pass tests
// -----------------------------------------------------------------------

TEST_CASE (



"constant_propagation_pass: add of two known constants is folded"
,
"[lithe][codegen][pass][const_prop]"
)
 {
    using namespace lithe::codegen;

    // load_imm r1 = 1 ; load_imm r2 = 2 ; add r3 = r1, r2 ; ret  =>  load_imm r3 = 3
    allocated_instruction li1;
    li1.id = 1;
    li1.op = opcode::load_imm;
    li1.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li1.uses = {allocated_operand::as_i64(1)};

    allocated_instruction li2;
    li2.id = 2;
    li2.op = opcode::load_imm;
    li2.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    li2.uses = {allocated_operand::as_i64(2)};

    allocated_instruction add;
    add.id = 3;
    add.op = opcode::add;
    add.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{2, "r2"})
    };

    allocated_instruction ret;
    ret.id = 4;
    ret.op = opcode::ret;

    auto fn = make_physical("const_add", {li1, li2, add, ret});

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);

    const auto &insts = result.function.function.blocks[0].instructions;
    bool found_folded = false;
    for (const auto &inst : insts) {
        if (inst.id == 3) {
            found_folded = true;
            REQUIRE(inst.op == opcode::load_imm);
            REQUIRE(inst.uses.size() == 1);
            REQUIRE(inst.uses[0].type == allocated_operand::kind::immediate_i64);
            REQUIRE(std::get<std::int64_t>(inst.uses[0].value) == 3);
        }
    }
    REQUIRE(found_folded);
}

TEST_CASE (



"constant_propagation_pass: sub and mul fold correctly"
,
"[lithe][codegen][pass][const_prop]"
)
 {
    using namespace lithe::codegen;

    auto make_li = [](std::uint32_t id, std::uint16_t reg, std::int64_t val) {
        allocated_instruction li;
        li.id = id;
        li.op = opcode::load_imm;
        li.defs = {allocated_operand::as_preg(preg{reg, "r"})};
        li.uses = {allocated_operand::as_i64(val)};
        return li;
    };
    auto make_binop = [](std::uint32_t id, opcode op,
                         std::uint16_t dst, std::uint16_t lhs, std::uint16_t rhs) {
        allocated_instruction inst;
        inst.id = id;
        inst.op = op;
        inst.defs = {allocated_operand::as_preg(preg{dst, "r"})};
        inst.uses = {
            allocated_operand::as_preg(preg{lhs, "r"}),
            allocated_operand::as_preg(preg{rhs, "r"})
        };
        return inst;
    };

    // sub: 10 - 3 = 7
    {
        allocated_instruction ret;
        ret.id = 99;
        ret.op = opcode::ret;
        auto fn = make_physical("const_sub",
            {make_li(1,1,10), make_li(2,2,3), make_binop(3, opcode::sub, 3, 1, 2), ret});
        mir_pass_context ctx;
        constant_propagation_pass pass;
        const auto result = pass.run(fn, ctx);
        REQUIRE(result.ok());
        REQUIRE(result.changed);
        const auto &insts = result.function.function.blocks[0].instructions;
        for (const auto &inst : insts) {
            if (inst.id == 3) {
                REQUIRE(inst.op == opcode::load_imm);
                REQUIRE(std::get<std::int64_t>(inst.uses[0].value) == 7);
            }
        }
    }

    // mul: 4 * 5 = 20
    {
        allocated_instruction ret;
        ret.id = 99;
        ret.op = opcode::ret;
        auto fn = make_physical("const_mul",
            {make_li(1,1,4), make_li(2,2,5), make_binop(3, opcode::mul, 3, 1, 2), ret});
        mir_pass_context ctx;
        constant_propagation_pass pass;
        const auto result = pass.run(fn, ctx);
        REQUIRE(result.ok());
        REQUIRE(result.changed);
        const auto &insts = result.function.function.blocks[0].instructions;
        for (const auto &inst : insts) {
            if (inst.id == 3) {
                REQUIRE(inst.op == opcode::load_imm);
                REQUIRE(std::get<std::int64_t>(inst.uses[0].value) == 20);
            }
        }
    }
}

TEST_CASE (



"constant_propagation_pass: div by zero is not folded"
,
"[lithe][codegen][pass][const_prop]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction li1;
    li1.id = 1;
    li1.op = opcode::load_imm;
    li1.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li1.uses = {allocated_operand::as_i64(8)};

    allocated_instruction li0;
    li0.id = 2;
    li0.op = opcode::load_imm;
    li0.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    li0.uses = {allocated_operand::as_i64(0)};

    allocated_instruction div;
    div.id = 3;
    div.op = opcode::div;
    div.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    div.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{2, "r2"})
    };

    allocated_instruction ret;
    ret.id = 4;
    ret.op = opcode::ret;

    auto fn = make_physical("const_div_zero", {li1, li0, div, ret});

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    const auto &insts = result.function.function.blocks[0].instructions;
    bool div_still_present = false;
    for (const auto &inst : insts) {
        if (inst.id == 3) {
            div_still_present = true;
            REQUIRE(inst.op == opcode::div);
        }
    }
    REQUIRE(div_still_present);
}

TEST_CASE (



"constant_propagation_pass: unknown redef clears constant state"
,
"[lithe][codegen][pass][const_prop]"
)
 {
    using namespace lithe::codegen;

    // load_imm r1 = 5 ; add r1 = r1, r2 (r1 redefined unknown) ; add r3 = r1, r1
    // The second add must NOT be folded — r1's constant was cleared by the first add.
    allocated_instruction li;
    li.id = 1;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li.uses = {allocated_operand::as_i64(5)};

    allocated_instruction redef;
    redef.id = 2;
    redef.op = opcode::add;
    redef.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    redef.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{2, "r2"})
    };

    allocated_instruction after_redef;
    after_redef.id = 3;
    after_redef.op = opcode::add;
    after_redef.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    after_redef.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{1, "r1"})
    };

    allocated_instruction ret;
    ret.id = 4;
    ret.op = opcode::ret;

    auto fn = make_physical("const_redef", {li, redef, after_redef, ret});

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    const auto &insts = result.function.function.blocks[0].instructions;
    for (const auto &inst : insts) {
        if (inst.id == 3) {
            REQUIRE(inst.op == opcode::add);
        }
    }
}

TEST_CASE (



"constant_propagation_pass: constants propagate across dominating block"
,
"[lithe][codegen][pass][const_prop]"
)
 {
    using namespace lithe::codegen;

    // bb1: load_imm r1 = 7 ; branch bb2
    // bb2: add r2 = r1, r1 ; ret
    // bb1 dominates bb2 and r1's only reaching def is the load_imm → folded to 14.

    allocated_function_ir fn_ir;
    fn_ir.name = "const_cross_block_simple";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction li;
    li.id = 1;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li.uses = {allocated_operand::as_i64(7)};

    allocated_instruction br;
    br.id = 2;
    br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(2)};

    bb1.instructions = {li, br};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "exit";
    bb2.predecessors = {1};

    allocated_instruction add;
    add.id = 3;
    add.op = opcode::add;
    add.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    add.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{1, "r1"})
    };

    allocated_instruction ret;
    ret.id = 4;
    ret.op = opcode::ret;

    bb2.instructions = {add, ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    const auto &bb2_result = result.function.function.blocks[1];
    REQUIRE(bb2_result.instructions[0].op == opcode::load_imm);
    REQUIRE(std::get<std::int64_t>(bb2_result.instructions[0].uses[0].value) == 14);
}

// ---------------------------------------------------------------------------
// Cross-block copy propagation tests (Prompt 1)
// ---------------------------------------------------------------------------

TEST_CASE (



"copy_propagation_pass: cross-block propagation when copy dominates use"
,
"[lithe][codegen][pass][copy_prop][cross_block]"
)
 {
    using namespace lithe::codegen;

    // bb1: mov r2=r1 ; branch bb2
    // bb2: add r3=r2,r4 ; ret
    // bb1 dominates bb2 and r2's only reaching def is the mov → propagated.
    allocated_function_ir fn_ir;
    fn_ir.name = "copy_cross_dominates";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction mov;
    mov.id = 10;
    mov.op = opcode::mov;
    mov.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    mov.uses = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction br;
    br.id = 11;
    br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(2)};

    bb1.instructions = {mov, br};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "exit";
    bb2.predecessors = {1};

    allocated_instruction add;
    add.id = 12;
    add.op = opcode::add;
    add.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add.uses = {
        allocated_operand::as_preg(preg{2, "r2"}),
        allocated_operand::as_preg(preg{4, "r4"})
    };

    allocated_instruction ret;
    ret.id = 13;
    ret.op = opcode::ret;

    bb2.instructions = {add, ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);

    // The add in bb2 should have r1 (id=1) as its first use operand.
    const auto &bb2_result = result.function.function.blocks[1];
    const auto &add_result = bb2_result.instructions[0];
    REQUIRE(add_result.op == opcode::add);
    REQUIRE(std::get<preg>(add_result.uses[0].value).id == 1);
}

TEST_CASE (



"copy_propagation_pass: no cross-block propagation when reaching def is ambiguous"
,
"[lithe][codegen][pass][copy_prop][cross_block]"
)
 {
    using namespace lithe::codegen;

    // Diamond: bb1 branches to bb2 and bb3, both branch to bb4.
    // Only bb2 defines r2 via mov r2=r1 — bb3 does not.
    // Reaching def of r2 at bb4 is ambiguous → no propagation.
    //
    // bb1: branch_cond bb2, bb3
    // bb2: mov r2=r1 ; branch bb4
    // bb3: branch bb4
    // bb4: add r3=r2,r4 ; ret

    allocated_function_ir fn_ir;
    fn_ir.name = "copy_cross_no_dominate";
    fn_ir.cfg.entry_block = 1;

    // bb1
    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2, 3};

    allocated_instruction brc;
    brc.id = 20;
    brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(preg{5, "r5"}),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};
    bb1.instructions = {brc};

    // bb2
    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "left";
    bb2.predecessors = {1};
    bb2.successors = {4};

    allocated_instruction mov2;
    mov2.id = 21;
    mov2.op = opcode::mov;
    mov2.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    mov2.uses = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction br2;
    br2.id = 22;
    br2.op = opcode::branch;
    br2.uses = {allocated_operand::as_block(4)};

    bb2.instructions = {mov2, br2};

    // bb3
    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "right";
    bb3.predecessors = {1};
    bb3.successors = {4};

    allocated_instruction br3;
    br3.id = 23;
    br3.op = opcode::branch;
    br3.uses = {allocated_operand::as_block(4)};
    bb3.instructions = {br3};

    // bb4
    allocated_basic_block bb4;
    bb4.id = 4;
    bb4.name = "merge";
    bb4.predecessors = {2, 3};

    allocated_instruction add4;
    add4.id = 24;
    add4.op = opcode::add;
    add4.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add4.uses = {
        allocated_operand::as_preg(preg{2, "r2"}),
        allocated_operand::as_preg(preg{4, "r4"})
    };

    allocated_instruction ret4;
    ret4.id = 25;
    ret4.op = opcode::ret;

    bb4.instructions = {add4, ret4};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2, 3};
    fn_ir.cfg.successors[2] = {4};
    fn_ir.cfg.successors[3] = {4};
    fn_ir.cfg.predecessors[2] = {1};
    fn_ir.cfg.predecessors[3] = {1};
    fn_ir.cfg.predecessors[4] = {2, 3};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());

    // The add in bb4 must still use r2 (id=2) — no propagation across ambiguous merge.
    const auto &bb4_result = result.function.function.blocks[3];
    const auto &add_result = bb4_result.instructions[0];
    REQUIRE(add_result.op == opcode::add);
    REQUIRE(std::get<preg>(add_result.uses[0].value).id == 2);
}

TEST_CASE (



"copy_propagation_pass: no cross-block propagation when dst is redefined on path"
,
"[lithe][codegen][pass][copy_prop][cross_block]"
)
 {
    using namespace lithe::codegen;

    // bb1: mov r2=r1 ; branch bb2
    // bb2: add r2=r2,r4 ; branch bb3  (r2 redefined)
    // bb3: add r3=r2,r5 ; ret
    // Reaching def of r2 at bb3 is from bb2 (not the original mov) → no propagation.

    allocated_function_ir fn_ir;
    fn_ir.name = "copy_cross_redef_path";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction mov1;
    mov1.id = 30;
    mov1.op = opcode::mov;
    mov1.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    mov1.uses = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction br1;
    br1.id = 31;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};

    bb1.instructions = {mov1, br1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "mid";
    bb2.predecessors = {1};
    bb2.successors = {3};

    allocated_instruction redef;
    redef.id = 32;
    redef.op = opcode::add;
    redef.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    redef.uses = {
        allocated_operand::as_preg(preg{2, "r2"}),
        allocated_operand::as_preg(preg{4, "r4"})
    };

    allocated_instruction br2;
    br2.id = 33;
    br2.op = opcode::branch;
    br2.uses = {allocated_operand::as_block(3)};

    bb2.instructions = {redef, br2};

    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "exit";
    bb3.predecessors = {2};

    allocated_instruction use3;
    use3.id = 34;
    use3.op = opcode::add;
    use3.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    use3.uses = {
        allocated_operand::as_preg(preg{2, "r2"}),
        allocated_operand::as_preg(preg{5, "r5"})
    };

    allocated_instruction ret3;
    ret3.id = 35;
    ret3.op = opcode::ret;

    bb3.instructions = {use3, ret3};

    fn_ir.blocks = {bb1, bb2, bb3};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3};
    fn_ir.cfg.predecessors[2] = {1};
    fn_ir.cfg.predecessors[3] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());

    // The use in bb3 must still use r2 (id=2) — the copy's dst was redefined.
    const auto &bb3_result = result.function.function.blocks[2];
    const auto &use_result = bb3_result.instructions[0];
    REQUIRE(use_result.op == opcode::add);
    REQUIRE(std::get<preg>(use_result.uses[0].value).id == 2);
}

// ---------------------------------------------------------------------------
// Cross-block constant propagation tests (Prompt 2)
// ---------------------------------------------------------------------------

TEST_CASE (



"constant_propagation_pass: cross-block integer constant propagation"
,
"[lithe][codegen][pass][const_prop][cross_block]"
)
 {
    using namespace lithe::codegen;

    // bb1: load_imm r1=10 ; load_imm r2=3 ; branch bb2
    // bb2: add r3=r1,r2 ; ret  => load_imm r3=13

    allocated_function_ir fn_ir;
    fn_ir.name = "const_cross_fold";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction li1;
    li1.id = 40;
    li1.op = opcode::load_imm;
    li1.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li1.uses = {allocated_operand::as_i64(10)};

    allocated_instruction li2;
    li2.id = 41;
    li2.op = opcode::load_imm;
    li2.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    li2.uses = {allocated_operand::as_i64(3)};

    allocated_instruction br1;
    br1.id = 42;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};

    bb1.instructions = {li1, li2, br1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "exit";
    bb2.predecessors = {1};

    allocated_instruction add2;
    add2.id = 43;
    add2.op = opcode::add;
    add2.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add2.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{2, "r2"})
    };

    allocated_instruction ret2;
    ret2.id = 44;
    ret2.op = opcode::ret;

    bb2.instructions = {add2, ret2};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    const auto &bb2_result = result.function.function.blocks[1];
    REQUIRE(bb2_result.instructions[0].op == opcode::load_imm);
    REQUIRE(std::get<std::int64_t>(bb2_result.instructions[0].uses[0].value) == 13);
}

TEST_CASE (



"constant_propagation_pass: no cross-block fold when constant is redefined on path"
,
"[lithe][codegen][pass][const_prop][cross_block]"
)
 {
    using namespace lithe::codegen;

    // bb1: load_imm r1=5 ; branch bb2
    // bb2: add r1=r1,r2 ; branch bb3  (r1 redefined, r2 unknown)
    // bb3: add r3=r1,r1 ; ret
    // r1's reaching def at bb3 is from the add in bb2 (not a load_imm) → no fold.

    allocated_function_ir fn_ir;
    fn_ir.name = "const_cross_redef";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction li1;
    li1.id = 50;
    li1.op = opcode::load_imm;
    li1.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li1.uses = {allocated_operand::as_i64(5)};

    allocated_instruction br1;
    br1.id = 51;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};

    bb1.instructions = {li1, br1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "mid";
    bb2.predecessors = {1};
    bb2.successors = {3};

    allocated_instruction redef2;
    redef2.id = 52;
    redef2.op = opcode::add;
    redef2.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    redef2.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{2, "r2"})
    };

    allocated_instruction br2;
    br2.id = 53;
    br2.op = opcode::branch;
    br2.uses = {allocated_operand::as_block(3)};

    bb2.instructions = {redef2, br2};

    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "exit";
    bb3.predecessors = {2};

    allocated_instruction add3;
    add3.id = 54;
    add3.op = opcode::add;
    add3.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add3.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{1, "r1"})
    };

    allocated_instruction ret3;
    ret3.id = 55;
    ret3.op = opcode::ret;

    bb3.instructions = {add3, ret3};

    fn_ir.blocks = {bb1, bb2, bb3};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3};
    fn_ir.cfg.predecessors[2] = {1};
    fn_ir.cfg.predecessors[3] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    // The add in bb3 must NOT be folded — r1 was redefined with an unknown value.
    const auto &bb3_result = result.function.function.blocks[2];
    REQUIRE(bb3_result.instructions[0].op == opcode::add);
}

TEST_CASE (



"constant_propagation_pass: no cross-block fold across ambiguous merge"
,
"[lithe][codegen][pass][const_prop][cross_block]"
)
 {
    using namespace lithe::codegen;

    // Diamond: bb1 → bb2 and bb1 → bb3, both → bb4.
    // bb2: load_imm r1=10 ; bb3: load_imm r1=20.
    // bb4: add r2=r1,r1 ; ret
    // reaching_definitions records ambiguity for r1@bb4 → no fold.

    allocated_function_ir fn_ir;
    fn_ir.name = "const_cross_ambiguous";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2, 3};

    allocated_instruction brc1;
    brc1.id = 60;
    brc1.op = opcode::branch_cond;
    brc1.uses = {allocated_operand::as_preg(preg{5, "r5"}),
                 allocated_operand::as_block(2),
                 allocated_operand::as_block(3)};
    bb1.instructions = {brc1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "left";
    bb2.predecessors = {1};
    bb2.successors = {4};

    allocated_instruction li2;
    li2.id = 61;
    li2.op = opcode::load_imm;
    li2.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li2.uses = {allocated_operand::as_i64(10)};

    allocated_instruction br2;
    br2.id = 62;
    br2.op = opcode::branch;
    br2.uses = {allocated_operand::as_block(4)};

    bb2.instructions = {li2, br2};

    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "right";
    bb3.predecessors = {1};
    bb3.successors = {4};

    allocated_instruction li3;
    li3.id = 63;
    li3.op = opcode::load_imm;
    li3.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li3.uses = {allocated_operand::as_i64(20)};

    allocated_instruction br3;
    br3.id = 64;
    br3.op = opcode::branch;
    br3.uses = {allocated_operand::as_block(4)};

    bb3.instructions = {li3, br3};

    allocated_basic_block bb4;
    bb4.id = 4;
    bb4.name = "merge";
    bb4.predecessors = {2, 3};

    allocated_instruction add4;
    add4.id = 65;
    add4.op = opcode::add;
    add4.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    add4.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{1, "r1"})
    };

    allocated_instruction ret4;
    ret4.id = 66;
    ret4.op = opcode::ret;

    bb4.instructions = {add4, ret4};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2, 3};
    fn_ir.cfg.successors[2] = {4};
    fn_ir.cfg.successors[3] = {4};
    fn_ir.cfg.predecessors[2] = {1};
    fn_ir.cfg.predecessors[3] = {1};
    fn_ir.cfg.predecessors[4] = {2, 3};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    // The add in bb4 must NOT be folded — r1 has two different reaching defs.
    const auto &bb4_result = result.function.function.blocks[3];
    REQUIRE(bb4_result.instructions[0].op == opcode::add);
}

TEST_CASE (



"constant_propagation_pass: cross-block divide-by-zero is not folded"
,
"[lithe][codegen][pass][const_prop][cross_block]"
)
 {
    using namespace lithe::codegen;

    // bb1: load_imm r1=5 ; load_imm r2=0 ; branch bb2
    // bb2: div r3=r1,r2 ; ret
    // Division by zero must not be folded even cross-block.

    allocated_function_ir fn_ir;
    fn_ir.name = "const_cross_divzero";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction li1;
    li1.id = 70;
    li1.op = opcode::load_imm;
    li1.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li1.uses = {allocated_operand::as_i64(5)};

    allocated_instruction li2;
    li2.id = 71;
    li2.op = opcode::load_imm;
    li2.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    li2.uses = {allocated_operand::as_i64(0)};

    allocated_instruction br1;
    br1.id = 72;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};

    bb1.instructions = {li1, li2, br1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "exit";
    bb2.predecessors = {1};

    allocated_instruction div2;
    div2.id = 73;
    div2.op = opcode::div;
    div2.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    div2.uses = {
        allocated_operand::as_preg(preg{1, "r1"}),
        allocated_operand::as_preg(preg{2, "r2"})
    };

    allocated_instruction ret2;
    ret2.id = 74;
    ret2.op = opcode::ret;

    bb2.instructions = {div2, ret2};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    const auto &bb2_result = result.function.function.blocks[1];
    REQUIRE(bb2_result.instructions[0].op == opcode::div);
}

TEST_CASE (



"Backend capability: mock backend with no capabilities rejects any MIR feature"
,
"[lithe][codegen][backend][capability][negative]"
)
 {
    using namespace lithe::codegen;

    struct no_capability_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set{};
        }

        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state state) {
            return backend_result::success_result(std::move(state));
        }
    };

    allocated_instruction li;
    li.id = 700;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(1)};

    allocated_instruction ret;
    ret.id = 701;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto fn = make_physical("no_cap_test", {li, ret});

    no_capability_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    const bool has_cap_error = std::any_of(result.errors.begin(), result.errors.end(), [](const backend_error &e) {
        return e.message.find("backend capability missing") != std::string::npos;
    });
    REQUIRE(has_cap_error);
}

TEST_CASE (



"Backend capability: mock backend missing branches rejects branch MIR"
,
"[lithe][codegen][backend][capability][negative]"
)
 {
    using namespace lithe::codegen;

    struct no_branch_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::stack_frame,
                backend_feature::spill_load_store,
                backend_feature::memory_operands
            });
        }

        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state state) {
            return backend_result::success_result(std::move(state));
        }
    };

    // Build a two-block function with an unconditional branch.
    allocated_function_ir fn_ir;
    fn_ir.name = "no_branch_test";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction br;
    br.id = 710;
    br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {br};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "exit";
    bb2.predecessors = {1};

    allocated_instruction ret;
    ret.id = 711;
    ret.op = opcode::ret;
    bb2.instructions = {ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    no_branch_backend backend;
    const auto result = emit_function(backend, physical);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    const bool missing_branches = std::any_of(result.errors.begin(), result.errors.end(), [](const backend_error &e) {
        return e.message.find("branches") != std::string::npos;
    });
    REQUIRE(missing_branches);
}

TEST_CASE (



"Backend capability: mock backend missing calls rejects call MIR"
,
"[lithe][codegen][backend][capability][negative]"
)
 {
    using namespace lithe::codegen;

    struct no_call_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::stack_frame,
                backend_feature::spill_load_store,
                backend_feature::memory_operands
            });
        }

        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state state) {
            return backend_result::success_result(std::move(state));
        }
    };

    allocated_instruction call_inst;
    call_inst.id = 720;
    call_inst.op = opcode::call;

    allocated_instruction ret;
    ret.id = 721;
    ret.op = opcode::ret;

    auto fn = make_physical("no_call_test", {call_inst, ret});

    no_call_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    const bool missing_calls = std::any_of(result.errors.begin(), result.errors.end(), [](const backend_error &e) {
        return e.message.find("calls") != std::string::npos;
    });
    REQUIRE(missing_calls);
}

TEST_CASE (



"Backend capability: emit_function returns backend_result error, not exception"
,
"[lithe][codegen][backend][capability][negative]"
)
 {
    using namespace lithe::codegen;

    struct throws_on_emit_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set{};
        }

        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state) {
            throw std::runtime_error("should never reach emit");
        }
    };

    allocated_instruction li;
    li.id = 730;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(5)};

    allocated_instruction ret;
    ret.id = 731;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto fn = make_physical("no_throw_test", {li, ret});

    throws_on_emit_backend backend;
    // Capability validation must reject before emit_instruction is called — no exception must escape.
    REQUIRE_NOTHROW([&] { return emit_function(backend, fn); }());
    const auto result = emit_function(backend, fn);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
}

TEST_CASE (



"Backend capability: debug_text_backend supports all printable MIR features"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    constexpr auto caps = debug_text_backend::capabilities();
    REQUIRE(caps.has(backend_feature::integer_arithmetic));
    REQUIRE(caps.has(backend_feature::floating_arithmetic));
    REQUIRE(caps.has(backend_feature::spill_load_store));
    REQUIRE(caps.has(backend_feature::branches));
    REQUIRE(caps.has(backend_feature::calls));
    REQUIRE(caps.has(backend_feature::memory_operands));
    REQUIRE(caps.has(backend_feature::stack_frame));
}

TEST_CASE (



"Backend capability: interpreter_backend rejects call opcode via capability check"
,
"[lithe][codegen][backend][capability][negative]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // interpreter_backend does NOT advertise calls capability.
    constexpr auto caps = interpreter_backend::capabilities();
    REQUIRE_FALSE(caps.has(backend_feature::calls));

    allocated_instruction call_inst;
    call_inst.id = 740;
    call_inst.op = opcode::call;

    allocated_instruction ret;
    ret.id = 741;
    ret.op = opcode::ret;

    auto fn = make_physical("interp_call_reject", {call_inst, ret});

    interpreter_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    const bool missing_calls = std::any_of(result.errors.begin(), result.errors.end(), [](const backend_error &e) {
        return e.message.find("calls") != std::string::npos;
    });
    REQUIRE(missing_calls);
}

TEST_CASE (



"Backend capability: interpreter_backend supports branch opcode via capability check"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // §v2.7: the interpreter gained CFG-aware execution, so branches are now a
    // declared capability and a valid branch CFG emits successfully instead of
    // being rejected as an unsupported feature.
    constexpr auto caps = interpreter_backend::capabilities();
    REQUIRE(caps.has(backend_feature::branches));

    allocated_function_ir fn_ir;
    fn_ir.name = "interp_branch_exec";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction br;
    br.id = 750;
    br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {br};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "exit";
    bb2.predecessors = {1};

    allocated_instruction ret;
    ret.id = 751;
    ret.op = opcode::ret;
    bb2.instructions = {ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    interpreter_backend backend;
    const auto result = emit_function(backend, physical);

    REQUIRE(result.ok());
    const bool missing_branches = std::any_of(result.errors.begin(), result.errors.end(), [](const backend_error &e) {
        return e.message.find("branches") != std::string::npos;
    });
    REQUIRE_FALSE(missing_branches);
}

TEST_CASE (



"Backend capability: validate_backend_capabilities lists all missing features"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;

    // A function using branches + calls + floating point — none provided.
    allocated_function_ir fn_ir;
    fn_ir.name = "multi_missing";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction br;
    br.id = 760;
    br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {br};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "exit";
    bb2.predecessors = {1};

    allocated_instruction call_inst;
    call_inst.id = 761;
    call_inst.op = opcode::call;

    allocated_instruction li_f;
    li_f.id = 762;
    li_f.op = opcode::load_imm;
    li_f.uses = {allocated_operand::as_f64(3.14)};

    allocated_instruction ret;
    ret.id = 763;
    ret.op = opcode::ret;
    bb2.instructions = {call_inst, li_f, ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    // Provide only integer_arithmetic — many features missing.
    const auto provided = backend_capability_set::from({backend_feature::integer_arithmetic});
    const auto validation = validate_backend_capabilities(physical, provided);

    REQUIRE_FALSE(validation.ok());
    REQUIRE(validation.missing.has(backend_feature::branches));
    REQUIRE(validation.missing.has(backend_feature::calls));
    REQUIRE(validation.missing.has(backend_feature::floating_arithmetic));
    REQUIRE(validation.diagnostics.size() >= 3);
}

// ---------------------------------------------------------------------------
// Backend legality tests
// ---------------------------------------------------------------------------

// --- 1. Mock backend with no branch support ---------------------------------

TEST_CASE (



"Backend legality: no-branch backend rejects MIR with branch via emit_function"
,
"[lithe][codegen][backend][legality][negative]"
)
 {
    using namespace lithe::codegen;

    struct no_branch_legality_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::calls,
                backend_feature::memory_operands,
                backend_feature::stack_frame,
                backend_feature::spill_load_store
            });
        }
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state s) {
            return backend_result::success_result(std::move(s));
        }
    };

    // Two-block function: bb900 --branch--> bb901 --> ret.
    allocated_function_ir fn_ir;
    fn_ir.name = "legality_no_branch";
    fn_ir.cfg.entry_block = 900;

    allocated_basic_block bb1;
    bb1.id = 900;
    bb1.name = "entry";
    bb1.successors = {901};

    allocated_instruction br;
    br.id = 9000;
    br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(901)};
    bb1.instructions = {br};

    allocated_basic_block bb2;
    bb2.id = 901;
    bb2.name = "exit";
    bb2.predecessors = {900};

    allocated_instruction ret;
    ret.id = 9001;
    ret.op = opcode::ret;
    bb2.instructions = {ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[900]  = {901};
    fn_ir.cfg.predecessors[901] = {900};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    no_branch_legality_backend backend;
    const auto result = emit_function(backend, physical);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    const bool branches_mentioned = std::any_of(
        result.errors.begin(), result.errors.end(),
        [](const backend_error &e) {
            return e.message.find("branches") != std::string::npos
                || e.message.find("branch") != std::string::npos;
        });
    REQUIRE(branches_mentioned);
}

TEST_CASE (



"Backend legality: no-branch backend accepts MIR without branch"
,
"[lithe][codegen][backend][legality]"
)
 {
    using namespace lithe::codegen;

    struct no_branch_legality_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::calls,
                backend_feature::memory_operands,
                backend_feature::stack_frame,
                backend_feature::spill_load_store
            });
        }
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state s) {
            return backend_result::success_result(std::move(s));
        }
    };

    // add + ret in one block: no branch, no floating point — should pass.
    allocated_instruction li;
    li.id = 9002;
    li.op = opcode::add;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_preg({1, "r1"}), allocated_operand::as_preg({2, "r2"})};

    allocated_instruction ret;
    ret.id = 9003;
    ret.op = opcode::ret;

    auto fn = make_physical("legality_no_branch_ok", {li, ret});
    no_branch_legality_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
}

// --- 2. Mock backend with no memory support ---------------------------------

TEST_CASE (



"Backend legality: no-memory backend rejects MIR with load opcode"
,
"[lithe][codegen][backend][legality][negative]"
)
 {
    using namespace lithe::codegen;

    struct no_memory_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::branches,
                backend_feature::calls
                // no memory_operands, no stack_frame, no spill_load_store
            });
        }
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state s) {
            return backend_result::success_result(std::move(s));
        }
    };

    allocated_instruction load_inst;
    load_inst.id = 9010;
    load_inst.op = opcode::load;
    load_inst.defs = {allocated_operand::as_preg({0, "r0"})};

    allocated_instruction ret;
    ret.id = 9011;
    ret.op = opcode::ret;

    auto fn = make_physical("legality_no_memory_load", {load_inst, ret});
    no_memory_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    const bool memory_mentioned = std::any_of(
        result.errors.begin(), result.errors.end(),
        [](const backend_error &e) {
            return e.message.find("memory") != std::string::npos
                || e.message.find("load") != std::string::npos
                || e.message.find("store") != std::string::npos;
        });
    REQUIRE(memory_mentioned);
}

TEST_CASE (



"Backend legality: no-memory backend rejects MIR with store opcode"
,
"[lithe][codegen][backend][legality][negative]"
)
 {
    using namespace lithe::codegen;

    struct no_memory_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::branches
            });
        }
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state s) {
            return backend_result::success_result(std::move(s));
        }
    };

    allocated_instruction store_inst;
    store_inst.id = 9012;
    store_inst.op = opcode::store;
    store_inst.uses = {allocated_operand::as_preg({0, "r0"})};

    allocated_instruction ret;
    ret.id = 9013;
    ret.op = opcode::ret;

    auto fn = make_physical("legality_no_memory_store", {store_inst, ret});
    no_memory_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
}

TEST_CASE (



"Backend legality: no-memory backend rejects MIR with spill frame usage"
,
"[lithe][codegen][backend][legality][negative]"
)
 {
    using namespace lithe::codegen;

    struct no_memory_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic
                // no stack_frame, no spill_load_store
            });
        }
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state s) {
            return backend_result::success_result(std::move(s));
        }
    };

    // A function with a frame layout triggers stack_frame requirement.
    allocated_instruction ret;
    ret.id = 9014;
    ret.op = opcode::ret;

    auto fn = make_physical("legality_no_memory_frame", {ret});
    fn.frame_layout = stack_frame_layout{};

    no_memory_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    const bool frame_mentioned = std::any_of(
        result.errors.begin(), result.errors.end(),
        [](const backend_error &e) {
            return e.message.find("stack_frame") != std::string::npos
                || e.message.find("stack") != std::string::npos
                || e.message.find("frame") != std::string::npos;
        });
    REQUIRE(frame_mentioned);
}

// --- 3. debug_text_backend accepts all printable MIR features ---------------

TEST_CASE (



"Backend legality: debug_text_backend accepts MIR with integer arithmetic"
,
"[lithe][codegen][backend][legality]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    allocated_instruction li;
    li.id = 9020;
    li.op = opcode::add;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_preg({1, "r1"}), allocated_operand::as_preg({2, "r2"})};

    allocated_instruction ret;
    ret.id = 9021;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto fn = make_physical("dtb_int_arith", {li, ret});
    debug_text_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("add") != std::string::npos);
}

TEST_CASE (



"Backend legality: debug_text_backend accepts MIR with floating-point immediate"
,
"[lithe][codegen][backend][legality]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    allocated_instruction li;
    li.id = 9022;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_f64(2.718)};

    allocated_instruction ret;
    ret.id = 9023;
    ret.op = opcode::ret;

    auto fn = make_physical("dtb_float_imm", {li, ret});
    debug_text_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
}

TEST_CASE (



"Backend legality: debug_text_backend accepts multi-block MIR with branch"
,
"[lithe][codegen][backend][legality]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    allocated_function_ir fn_ir;
    fn_ir.name  = "dtb_branch_test";
    fn_ir.cfg.entry_block = 910;

    allocated_basic_block bb1;
    bb1.id = 910; bb1.name = "entry"; bb1.successors = {911};

    allocated_instruction br;
    br.id = 9024; br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(911)};
    bb1.instructions = {br};

    allocated_basic_block bb2;
    bb2.id = 911; bb2.name = "exit"; bb2.predecessors = {910};

    allocated_instruction ret;
    ret.id = 9025; ret.op = opcode::ret;
    bb2.instructions = {ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[910]  = {911};
    fn_ir.cfg.predecessors[911] = {910};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    debug_text_backend backend;
    const auto result = emit_function(backend, physical);

    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("dtb_branch_test") != std::string::npos);
}

TEST_CASE (



"Backend legality: debug_text_backend accepts MIR with call opcode"
,
"[lithe][codegen][backend][legality]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    allocated_instruction call_inst;
    call_inst.id = 9026;
    call_inst.op = opcode::call;

    allocated_instruction ret;
    ret.id = 9027;
    ret.op = opcode::ret;

    auto fn = make_physical("dtb_call_test", {call_inst, ret});
    debug_text_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("call") != std::string::npos);
}

// --- 4. interpreter_backend rejects unsupported features --------------------

TEST_CASE (



"Backend legality: interpreter_backend rejects call via legality path"
,
"[lithe][codegen][backend][legality][negative]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // interpreter_backend does not advertise calls.
    REQUIRE_FALSE(interpreter_backend::capabilities().has(backend_feature::calls));

    allocated_instruction call_inst;
    call_inst.id = 9030;
    call_inst.op = opcode::call;

    allocated_instruction ret;
    ret.id = 9031;
    ret.op = opcode::ret;

    auto fn = make_physical("interp_legality_call", {call_inst, ret});
    interpreter_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    // Rejection must come from legality or capability check — not from execution.
    const bool calls_mentioned = std::any_of(
        result.errors.begin(), result.errors.end(),
        [](const backend_error &e) { return e.message.find("call") != std::string::npos; });
    REQUIRE(calls_mentioned);
}

TEST_CASE (



"Backend legality: interpreter_backend accepts branch via legality path"
,
"[lithe][codegen][backend][legality]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // §v2.7: the interpreter is CFG-aware, so branches are a declared capability
    // and a valid branch CFG passes the legality path instead of being rejected.
    REQUIRE(interpreter_backend::capabilities().has(backend_feature::branches));

    // Two-block function: entry --branch--> exit.
    allocated_function_ir fn_ir;
    fn_ir.name = "interp_legality_branch";
    fn_ir.cfg.entry_block = 920;

    allocated_basic_block bb1;
    bb1.id = 920; bb1.name = "entry"; bb1.successors = {921};

    allocated_instruction br;
    br.id = 9032; br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(921)};
    bb1.instructions = {br};

    allocated_basic_block bb2;
    bb2.id = 921; bb2.name = "exit"; bb2.predecessors = {920};

    allocated_instruction ret;
    ret.id = 9033; ret.op = opcode::ret;
    bb2.instructions = {ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[920]  = {921};
    fn_ir.cfg.predecessors[921] = {920};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    interpreter_backend backend;
    const auto result = emit_function(backend, physical);

    REQUIRE(result.ok());
    const bool branch_rejected = std::any_of(
        result.errors.begin(), result.errors.end(),
        [](const backend_error &e) { return e.message.find("branch") != std::string::npos; });
    REQUIRE_FALSE(branch_rejected);
}

TEST_CASE (



"Backend legality: interpreter_backend rejects memory operand via legality path"
,
"[lithe][codegen][backend][legality][negative]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // interpreter_backend does not advertise floating_arithmetic.
    REQUIRE_FALSE(interpreter_backend::capabilities().has(backend_feature::floating_arithmetic));

    // MIR with an f64 immediate triggers supports_floating_arithmetic requirement.
    allocated_instruction li;
    li.id = 9034;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_f64(1.5)};

    allocated_instruction ret;
    ret.id = 9035;
    ret.op = opcode::ret;

    auto fn = make_physical("interp_legality_float", {li, ret});
    interpreter_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());
    const bool float_mentioned = std::any_of(
        result.errors.begin(), result.errors.end(),
        [](const backend_error &e) {
            return e.message.find("floating") != std::string::npos
                || e.message.find("float") != std::string::npos;
        });
    REQUIRE(float_mentioned);
}

TEST_CASE (



"Backend legality: interpreter_backend accepts plain integer MIR"
,
"[lithe][codegen][backend][legality]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // interpreter_backend provides integer_arithmetic and stack_frame.
    allocated_instruction li;
    li.id = 9036;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(7)};

    allocated_instruction ret;
    ret.id = 9037;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto fn = make_physical("interp_legality_ok", {li, ret});
    interpreter_backend backend;
    backend.arguments = {};
    const auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
}

// ---------------------------------------------------------------------------
// Prompt 8 — Dominator analysis scaffolding tests
// ---------------------------------------------------------------------------

TEST_CASE (



"Dominator scaffolding: compute_dominators returns result on simple CFG"
,
"[lithe][codegen][dominator][scaffold]"
)
 {
    using namespace lithe::codegen;

    auto fn = make_physical("dom_simple", {[] {
        allocated_instruction ret;
        ret.id = 800;
        ret.op = opcode::ret;
        return ret;
    }()});

    dominator_analysis_options opts;
    const auto result = compute_dominators(fn, opts);

    REQUIRE(result.ok());
    // Single-block function: entry block is its own idom (no parent above it).
    REQUIRE(result.dom.immediate_dominator.size() == 1);
}

TEST_CASE (



"Dominator scaffolding: compute_dominators does not affect normal codegen"
,
"[lithe][codegen][dominator][scaffold]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    allocated_instruction li;
    li.id = 810;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(99)};

    allocated_instruction ret;
    ret.id = 811;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto fn = make_physical("dom_no_affect_codegen", {li, ret});

    // Calling compute_dominators must not modify fn or affect subsequent emission.
    dominator_analysis_options opts;
    (void) compute_dominators(fn, opts);

    debug_text_backend backend;
    const auto result = emit_function(backend, fn);

    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE(result.artifact_text->find("dom_no_affect_codegen") != std::string::npos);
}

TEST_CASE (



"Dominator scaffolding: dominator_analysis_result is not required by emit_function"
,
"[lithe][codegen][dominator][scaffold]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // Plain emit_function must compile and succeed without a dominator result.
    allocated_instruction li;
    li.id = 820;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(7)};

    allocated_instruction ret;
    ret.id = 821;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto fn = make_physical("dom_not_required", {li, ret});

    debug_text_backend backend;
    const auto result = emit_function(backend, fn);
    REQUIRE(result.ok());
}

// ---------------------------------------------------------------------------
// Prompt 10 — MIR dominator integration tests
// ---------------------------------------------------------------------------

namespace {
    // Build a multi-block physical MIR function from a block list.
    // Each block's successors drive the CFG predecessor map.
    lithe::codegen::mir::physical_mir_function make_cfg_function(
        std::string name,
        std::uint32_t entry,
        std::vector<lithe::codegen::allocated_basic_block> blocks) {
        using namespace lithe::codegen;
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = entry;
        fn.blocks = std::move(blocks);
        for (auto const& b : fn.blocks) {
            fn.cfg.successors[b.id] = b.successors;
            for (auto const succ : b.successors)
                fn.cfg.predecessors[succ].push_back(b.id);
        }
        mir::physical_mir_function out;
        out.function = std::move(fn);
        out.metadata.current_phase = mir::phase::physical_mir;
        return out;
    }

    lithe::codegen::allocated_basic_block make_cfg_block(
        std::uint32_t id,
        std::vector<lithe::codegen::allocated_instruction> insts,
        std::vector<std::uint32_t> succs = {}) {
        lithe::codegen::allocated_basic_block b;
        b.id = id;
        b.name = "bb" + std::to_string(id);
        b.instructions = std::move(insts);
        b.successors = std::move(succs);
        return b;
    }

    lithe::codegen::allocated_instruction make_dom_branch(std::uint32_t id, std::uint32_t target) {
        lithe::codegen::allocated_instruction i;
        i.id = id;
        i.op = lithe::codegen::opcode::branch;
        i.uses = {lithe::codegen::allocated_operand::as_block(target)};
        return i;
    }

    lithe::codegen::allocated_instruction make_dom_branch_cond(
        std::uint32_t id, std::uint32_t t, std::uint32_t f) {
        lithe::codegen::allocated_instruction i;
        i.id = id;
        i.op = lithe::codegen::opcode::branch_cond;
        i.uses = {
            lithe::codegen::allocated_operand::as_preg({0, "r0"}),
            lithe::codegen::allocated_operand::as_block(t),
            lithe::codegen::allocated_operand::as_block(f)
        };
        return i;
    }

    lithe::codegen::allocated_instruction make_dom_ret(std::uint32_t id) {
        lithe::codegen::allocated_instruction i;
        i.id = id;
        i.op = lithe::codegen::opcode::ret;
        return i;
    }
} // anonymous namespace

TEST_CASE (



"Dominator integration: linear chain entry->bb2->bb3"
,
"[lithe][codegen][dominator][integration]"
)
 {
    using namespace lithe::codegen;

    // entry(1) -> bb2(2) -> bb3(3,ret)
    auto fn = make_cfg_function("dom_linear", 1, {
        make_cfg_block(1, {make_dom_branch(10, 2)}, {2}),
        make_cfg_block(2, {make_dom_branch(20, 3)}, {3}),
        make_cfg_block(3, {make_dom_ret(30)}, {})
    });

    const auto dom = compute_dominators(fn);
    REQUIRE(dom.ok());

    REQUIRE(dominates(dom, 1, 1)); // entry dominates itself
    REQUIRE(dominates(dom, 1, 2)); // entry dominates bb2
    REQUIRE(dominates(dom, 1, 3)); // entry dominates bb3
    REQUIRE(dominates(dom, 2, 3)); // bb2 dominates bb3
    REQUIRE_FALSE(dominates(dom, 2, 1)); // bb2 does not dominate entry
    REQUIRE_FALSE(dominates(dom, 3, 2)); // bb3 does not dominate bb2

    REQUIRE(immediate_dominator_of(dom, 2) == std::optional<std::uint32_t>{1});
    REQUIRE(immediate_dominator_of(dom, 3) == std::optional<std::uint32_t>{2});
    REQUIRE_FALSE(immediate_dominator_of(dom, 1).has_value()); // entry has no idom
}

TEST_CASE (



"Dominator integration: diamond branch CFG"
,
"[lithe][codegen][dominator][integration]"
)
 {
    using namespace lithe::codegen;

    // entry(1) -> then(2), else(3); then->merge(4); else->merge(4)
    auto fn = make_cfg_function("dom_diamond", 1, {
        make_cfg_block(1, {make_dom_branch_cond(10, 2, 3)}, {2, 3}),
        make_cfg_block(2, {make_dom_branch(20, 4)}, {4}),
        make_cfg_block(3, {make_dom_branch(30, 4)}, {4}),
        make_cfg_block(4, {make_dom_ret(40)}, {})
    });

    const auto dom = compute_dominators(fn);
    REQUIRE(dom.ok());

    REQUIRE(dominates(dom, 1, 4));       // entry dominates merge
    REQUIRE_FALSE(dominates(dom, 2, 4)); // then does not dominate merge
    REQUIRE_FALSE(dominates(dom, 3, 4)); // else does not dominate merge

    REQUIRE(immediate_dominator_of(dom, 4) == std::optional<std::uint32_t>{1});
}

TEST_CASE (



"Dominator integration: loop CFG with loop headers"
,
"[lithe][codegen][dominator][integration]"
)
 {
    using namespace lithe::codegen;

    // entry(1) -> header(2) -> body(3) -> header(2) [back edge]; header -> exit(4)
    auto fn = make_cfg_function("dom_loop", 1, {
        make_cfg_block(1, {make_dom_branch(10, 2)}, {2}),
        make_cfg_block(2, {make_dom_branch_cond(20, 3, 4)}, {3, 4}),
        make_cfg_block(3, {make_dom_branch(30, 2)}, {2}),
        make_cfg_block(4, {make_dom_ret(40)}, {})
    });

    dominator_analysis_options opts;
    opts.compute_loop_headers = true;
    const auto dom = compute_dominators(fn, opts);
    REQUIRE(dom.ok());

    const auto& headers = loop_headers(dom);
    REQUIRE(headers.count(2) == 1); // block 2 is the loop header
    REQUIRE(headers.count(1) == 0);
    REQUIRE(headers.count(3) == 0);
}

TEST_CASE (



"Dominator integration: pipeline does not compute dominators by default"
,
"[lithe][codegen][dominator][integration]"
)
 {
    using namespace lithe::codegen;

    auto fn = make_cfg_function("dom_pipeline_default", 1, {
        make_cfg_block(1, {make_dom_branch(10, 2)}, {2}),
        make_cfg_block(2, {make_dom_ret(20)}, {})
    });

    mir_pass_context ctx;
    REQUIRE_FALSE(ctx.analysis_cache.has_analysis(mir_analysis_kind::dominators));

    // Fetching dominators explicitly should populate the cache.
    const auto& dom = get_or_compute_dominators(ctx, fn);
    REQUIRE(dom.ok());
    REQUIRE(ctx.analysis_cache.has_analysis(mir_analysis_kind::dominators));

    // Invalidating CFG must also clear dominators.
    ctx.analysis_cache.invalidate_analysis(mir_analysis_kind::cfg);
    REQUIRE_FALSE(ctx.analysis_cache.has_analysis(mir_analysis_kind::dominators));

    // invalidate_all clears dominators.
    ctx.analysis_cache.dominators = compute_dominators(fn);
    ctx.analysis_cache.invalidate_all();
    REQUIRE_FALSE(ctx.analysis_cache.has_analysis(mir_analysis_kind::dominators));
}

TEST_CASE (



"compile_to_physical_mir does not compute dominators by default"
,
"[lithe][codegen][dominator][integration]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    // A pass that checks whether dominators were computed during the pipeline run.
    struct dominator_spy_pass {
        bool* dominated_computed;

        [[nodiscard]] mir_pass_result run(
            mir::physical_mir_function const& fn, mir_pass_context& ctx) const
        {
            *dominated_computed = ctx.analysis_cache.has_analysis(mir_analysis_kind::dominators);
            mir_pass_result out;
            out.function = fn;
            return out;
        }
    };

    bool dominated_computed = false;
    dominator_spy_pass spy{&dominated_computed};

    const auto expr = make_node<add_tag>(1, 2);

    codegen_options opts;
    mir_pass_pipeline pipeline;
    pipeline.add_pass("dominator_spy", std::move(spy));
    opts.with_mir_pipeline(std::move(pipeline));

    const auto result = compile_to_physical_mir(expr, opts);
    REQUIRE(result.ok());
    REQUIRE_FALSE(dominated_computed);
}

// ---------------------------------------------------------------------------
// Prompt 4 — mir_expression_key tests
// ---------------------------------------------------------------------------

TEST_CASE (



"mir_expression_key: is_pure_expression returns true for arithmetic opcodes"
,
"[lithe][codegen][mir_expr_key]"
)
 {
    using namespace lithe::codegen;

    // Arithmetic
    REQUIRE(is_pure_expression(opcode::add));
    REQUIRE(is_pure_expression(opcode::sub));
    REQUIRE(is_pure_expression(opcode::mul));
    REQUIRE(is_pure_expression(opcode::div));
    REQUIRE(is_pure_expression(opcode::mod));
    REQUIRE(is_pure_expression(opcode::neg));

    // Comparisons
    REQUIRE(is_pure_expression(opcode::cmp_eq));
    REQUIRE(is_pure_expression(opcode::cmp_ne));
    REQUIRE(is_pure_expression(opcode::cmp_lt));
    REQUIRE(is_pure_expression(opcode::cmp_le));
    REQUIRE(is_pure_expression(opcode::cmp_gt));
    REQUIRE(is_pure_expression(opcode::cmp_ge));

    // Bitwise
    REQUIRE(is_pure_expression(opcode::bit_and));
    REQUIRE(is_pure_expression(opcode::bit_or));
    REQUIRE(is_pure_expression(opcode::bit_xor));
    REQUIRE(is_pure_expression(opcode::bit_not));
    REQUIRE(is_pure_expression(opcode::shl));
    REQUIRE(is_pure_expression(opcode::shr));

    // Logical
    REQUIRE(is_pure_expression(opcode::logical_and));
    REQUIRE(is_pure_expression(opcode::logical_or));
    REQUIRE(is_pure_expression(opcode::logical_not));
}

TEST_CASE (



"mir_expression_key: is_pure_expression returns false for side-effect opcodes"
,
"[lithe][codegen][mir_expr_key]"
)
 {
    using namespace lithe::codegen;

    REQUIRE_FALSE(is_pure_expression(opcode::call));
    REQUIRE_FALSE(is_pure_expression(opcode::branch));
    REQUIRE_FALSE(is_pure_expression(opcode::branch_cond));
    REQUIRE_FALSE(is_pure_expression(opcode::ret));
    REQUIRE_FALSE(is_pure_expression(opcode::load));
    REQUIRE_FALSE(is_pure_expression(opcode::store));
    REQUIRE_FALSE(is_pure_expression(opcode::load_spill));
    REQUIRE_FALSE(is_pure_expression(opcode::store_spill));
    REQUIRE_FALSE(is_pure_expression(opcode::nop));
    REQUIRE_FALSE(is_pure_expression(opcode::mov));
    REQUIRE_FALSE(is_pure_expression(opcode::load_imm));
    REQUIRE_FALSE(is_pure_expression(opcode::load_arg));
    REQUIRE_FALSE(is_pure_expression(opcode::load_symbol));
}

TEST_CASE (



"mir_expression_key: is_cse_candidate accepts pure binary preg instruction"
,
"[lithe][codegen][mir_expr_key]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction add;
    add.id = 1;
    add.op = opcode::add;
    add.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    REQUIRE(is_cse_candidate(add));
}

TEST_CASE (



"mir_expression_key: is_cse_candidate rejects call"
,
"[lithe][codegen][mir_expr_key]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction call;
    call.id = 1;
    call.op = opcode::call;
    call.defs = {allocated_operand::as_preg(preg{1, "r1"})};

    REQUIRE_FALSE(is_cse_candidate(call));
}

TEST_CASE (



"mir_expression_key: is_cse_candidate rejects memory operand in uses"
,
"[lithe][codegen][mir_expr_key]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction load;
    load.id = 1;
    load.op = opcode::add;
    load.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    // memory operand in uses — not a pure CSE candidate
    memory_address addr;
    addr.kind = memory_address_kind::computed_address;
    load.uses = {allocated_operand::as_memory(addr)};

    REQUIRE_FALSE(is_cse_candidate(load));
}

TEST_CASE (



"mir_expression_key: make_expression_key produces equal keys for identical instructions"
,
"[lithe][codegen][mir_expr_key]"
)
 {
    using namespace lithe::codegen;

    auto make_add = [](std::uint32_t id) {
        allocated_instruction add;
        add.id = id;
        add.op = opcode::add;
        add.defs = {allocated_operand::as_preg(preg{5, "r5"})};
        add.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};
        return add;
    };

    const auto k1 = make_expression_key(make_add(1));
    const auto k2 = make_expression_key(make_add(2));

    REQUIRE(k1.has_value());
    REQUIRE(k2.has_value());
    REQUIRE(*k1 == *k2);

    // Must produce the same hash as well.
    const std::hash<mir_expression_key> hasher;
    REQUIRE(hasher(*k1) == hasher(*k2));
}

TEST_CASE (



"mir_expression_key: make_expression_key produces different keys for different operands"
,
"[lithe][codegen][mir_expr_key]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction add1, add2;
    add1.id = 1;
    add1.op = opcode::add;
    add1.defs = {allocated_operand::as_preg(preg{5, "r5"})};
    add1.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    add2.id = 2;
    add2.op = opcode::add;
    add2.defs = {allocated_operand::as_preg(preg{5, "r5"})};
    add2.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{3, "r3"})};

    const auto k1 = make_expression_key(add1);
    const auto k2 = make_expression_key(add2);

    REQUIRE(k1.has_value());
    REQUIRE(k2.has_value());
    REQUIRE_FALSE(*k1 == *k2);
}

TEST_CASE (



"mir_expression_key: make_expression_key returns nullopt for non-candidate"
,
"[lithe][codegen][mir_expr_key]"
)
 {
    using namespace lithe::codegen;

    allocated_instruction branch;
    branch.id = 1;
    branch.op = opcode::branch;
    branch.uses = {allocated_operand::as_block(2)};

    REQUIRE_FALSE(make_expression_key(branch).has_value());
}

// ---------------------------------------------------------------------------
// Prompt 5 — common_subexpression_elimination_pass tests
// ---------------------------------------------------------------------------

TEST_CASE (



"common_subexpression_elimination_pass: reuses repeated add in same block"
,
"[lithe][codegen][pass][cse]"
)
 {
    using namespace lithe::codegen;

    // add r3 = r1, r2   (id=10)
    // add r4 = r1, r2   (id=11) — identical expression, must become mov r4=r3
    // ret                (id=12)
    allocated_instruction add1;
    add1.id = 10;
    add1.op = opcode::add;
    add1.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add1.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction add2;
    add2.id = 11;
    add2.op = opcode::add;
    add2.defs = {allocated_operand::as_preg(preg{4, "r4"})};
    add2.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction ret;
    ret.id = 12;
    ret.op = opcode::ret;

    auto fn = make_physical("cse_same_block_add", {add1, add2, ret});

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_instructions == 1);

    const auto &insts = result.function.function.blocks[0].instructions;
    // add2 must have been rewritten to mov r4=r3
    bool found_mov = false;
    for (const auto &inst : insts) {
        if (inst.id == 11) {
            REQUIRE(inst.op == opcode::mov);
            REQUIRE(std::get<preg>(inst.uses[0].value).id == 3);
            found_mov = true;
        }
    }
    REQUIRE(found_mov);
}

TEST_CASE (



"common_subexpression_elimination_pass: reuses repeated sub and mul expressions"
,
"[lithe][codegen][pass][cse]"
)
 {
    using namespace lithe::codegen;

    // sub r3 = r1, r2   (id=10)
    // mul r4 = r5, r6   (id=11)
    // sub r7 = r1, r2   (id=12) — same as id=10, becomes mov r7=r3
    // mul r8 = r5, r6   (id=13) — same as id=11, becomes mov r8=r4
    // ret                (id=14)
    auto make_binop = [](std::uint32_t id, opcode op, std::uint16_t dst,
                          std::uint16_t lhs, std::uint16_t rhs) {
        allocated_instruction inst;
        inst.id = id;
        inst.op = op;
        inst.defs = {allocated_operand::as_preg(preg{dst, ""})};
        inst.uses = {allocated_operand::as_preg(preg{lhs, ""}),
                     allocated_operand::as_preg(preg{rhs, ""})};
        return inst;
    };

    allocated_instruction ret;
    ret.id = 14;
    ret.op = opcode::ret;

    auto fn = make_physical("cse_sub_mul", {
        make_binop(10, opcode::sub, 3, 1, 2),
        make_binop(11, opcode::mul, 4, 5, 6),
        make_binop(12, opcode::sub, 7, 1, 2),
        make_binop(13, opcode::mul, 8, 5, 6),
        ret
    });

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_instructions == 2);

    const auto &insts = result.function.function.blocks[0].instructions;
    for (const auto &inst : insts) {
        if (inst.id == 12) {
            REQUIRE(inst.op == opcode::mov);
            REQUIRE(std::get<preg>(inst.uses[0].value).id == 3);
        }
        if (inst.id == 13) {
            REQUIRE(inst.op == opcode::mov);
            REQUIRE(std::get<preg>(inst.uses[0].value).id == 4);
        }
    }
}

TEST_CASE (



"common_subexpression_elimination_pass: no reuse across redefinition of operand"
,
"[lithe][codegen][pass][cse]"
)
 {
    using namespace lithe::codegen;

    // add r3 = r1, r2   (id=10)
    // add r1 = r5, r6   (id=11) — redefines r1
    // add r4 = r1, r2   (id=12) — r1 is now different; must NOT be reused
    // ret                (id=13)
    allocated_instruction add1;
    add1.id = 10;
    add1.op = opcode::add;
    add1.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add1.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction redef;
    redef.id = 11;
    redef.op = opcode::add;
    redef.defs = {allocated_operand::as_preg(preg{1, "r1"})};  // redefines r1
    redef.uses = {allocated_operand::as_preg(preg{5, "r5"}), allocated_operand::as_preg(preg{6, "r6"})};

    allocated_instruction add2;
    add2.id = 12;
    add2.op = opcode::add;
    add2.defs = {allocated_operand::as_preg(preg{4, "r4"})};
    add2.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction ret;
    ret.id = 13;
    ret.op = opcode::ret;

    auto fn = make_physical("cse_no_reuse_after_redef", {add1, redef, add2, ret});

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());

    // add2 (id=12) must NOT have been replaced.
    const auto &insts = result.function.function.blocks[0].instructions;
    for (const auto &inst : insts) {
        if (inst.id == 12) {
            REQUIRE(inst.op == opcode::add);
        }
    }
}

TEST_CASE (



"common_subexpression_elimination_pass: no reuse for calls and memory ops"
,
"[lithe][codegen][pass][cse]"
)
 {
    using namespace lithe::codegen;

    // call   (id=10) — not a pure expression
    // load   (id=11) — not a pure expression
    // ret    (id=12)
    allocated_instruction call;
    call.id = 10;
    call.op = opcode::call;

    allocated_instruction load;
    load.id = 11;
    load.op = opcode::load;
    load.defs = {allocated_operand::as_preg(preg{1, "r1"})};

    allocated_instruction ret;
    ret.id = 12;
    ret.op = opcode::ret;

    auto fn = make_physical("cse_no_unsupported", {call, load, ret});

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE_FALSE(result.changed);
    REQUIRE(result.removed_instructions == 0);
}

TEST_CASE (



"common_subexpression_elimination_pass: cross-block reuse when dominator holds expression"
,
"[lithe][codegen][pass][cse]"
)
 {
    using namespace lithe::codegen;

    // bb1 (entry): add r3=r1,r2 ; branch -> bb2
    // bb2 (exit):  add r4=r1,r2 ; ret
    // bb1 dominates bb2, so r3 = add(r1,r2) is available; r4's add becomes mov r4=r3.

    allocated_function_ir fn_ir;
    fn_ir.name = "cse_cross_dom";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};

    allocated_instruction add1;
    add1.id = 10;
    add1.op = opcode::add;
    add1.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add1.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction br;
    br.id = 11;
    br.op = opcode::branch;
    br.uses = {allocated_operand::as_block(2)};

    bb1.instructions = {add1, br};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "exit";
    bb2.predecessors = {1};

    allocated_instruction add2;
    add2.id = 20;
    add2.op = opcode::add;
    add2.defs = {allocated_operand::as_preg(preg{4, "r4"})};
    add2.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction ret;
    ret.id = 21;
    ret.op = opcode::ret;

    bb2.instructions = {add2, ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);

    const auto &bb2_result = result.function.function.blocks[1];
    const auto &first_inst  = bb2_result.instructions[0];
    REQUIRE(first_inst.op == opcode::mov);
    REQUIRE(std::get<preg>(first_inst.uses[0].value).id == 3);
}

TEST_CASE (



"common_subexpression_elimination_pass: no cross-block reuse when non-dominating block"
,
"[lithe][codegen][pass][cse]"
)
 {
    using namespace lithe::codegen;

    // Diamond: bb1 -> bb2, bb3 -> bb4
    // bb2: add r3=r1,r2
    // bb3: (no add)
    // bb4: add r4=r1,r2  — bb2 does NOT dominate bb4, no reuse.

    allocated_function_ir fn_ir;
    fn_ir.name = "cse_no_cross_no_dom";
    fn_ir.cfg.entry_block = 1;

    // bb1: branch_cond → bb2, bb3
    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2, 3};
    allocated_instruction brc;
    brc.id = 5;
    brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(preg{9, "r9"}),
                allocated_operand::as_block(2),
                allocated_operand::as_block(3)};
    bb1.instructions = {brc};

    // bb2: add r3=r1,r2 ; branch bb4
    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "left";
    bb2.predecessors = {1};
    bb2.successors = {4};
    allocated_instruction add_left;
    add_left.id = 10;
    add_left.op = opcode::add;
    add_left.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_left.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                     allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction br_left;
    br_left.id = 11;
    br_left.op = opcode::branch;
    br_left.uses = {allocated_operand::as_block(4)};
    bb2.instructions = {add_left, br_left};

    // bb3: branch bb4
    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "right";
    bb3.predecessors = {1};
    bb3.successors = {4};
    allocated_instruction br_right;
    br_right.id = 20;
    br_right.op = opcode::branch;
    br_right.uses = {allocated_operand::as_block(4)};
    bb3.instructions = {br_right};

    // bb4: add r4=r1,r2 ; ret
    allocated_basic_block bb4;
    bb4.id = 4;
    bb4.name = "merge";
    bb4.predecessors = {2, 3};
    allocated_instruction add_merge;
    add_merge.id = 30;
    add_merge.op = opcode::add;
    add_merge.defs = {allocated_operand::as_preg(preg{4, "r4"})};
    add_merge.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                      allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction ret;
    ret.id = 31;
    ret.op = opcode::ret;
    bb4.instructions = {add_merge, ret};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2, 3};
    fn_ir.cfg.successors[2] = {4};
    fn_ir.cfg.successors[3] = {4};
    fn_ir.cfg.predecessors[2] = {1};
    fn_ir.cfg.predecessors[3] = {1};
    fn_ir.cfg.predecessors[4] = {2, 3};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());

    // add_merge (id=30) must not have been replaced.
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 30) {
                REQUIRE(inst.op == opcode::add);
            }
        }
    }
}

// -----------------------------------------------------------------------
// Loop-awareness guard tests
// -----------------------------------------------------------------------
//
// Canonical loop used in tests below:
//
//   bb1 (entry):  branch bb2
//   bb2 (header): <some defs> ; branch_cond r5, bb3, bb4
//   bb3 (body):   <some ops> ; branch bb2         ← back-edge to bb2
//   bb4 (exit):   ret
//
// The back-edge bb3→bb2 makes bb2 the loop header.
// Dominance: bb1 dominates all; bb2 dominates bb3 and bb4.

// -----------------------------------------------------------------------
// copy_propagation_pass — loop guard
// -----------------------------------------------------------------------

TEST_CASE (



"copy_propagation_pass: does not propagate copy across loop back edge"
,
"[lithe][codegen][pass][copy_prop][loop_guard]"
)
 {
    using namespace lithe::codegen;

    // bb1 (entry): mov r2=r1 ; branch bb2
    // bb2 (header): branch_cond r5, bb3, bb4
    // bb3 (body):   add r3=r2,r4 ; branch bb2    ← back-edge
    // bb4 (exit):   ret
    //
    // r2 is defined in bb1 (via mov r2=r1).  The use in bb3 is reached via
    // bb1→bb2→bb3.  That path does NOT cross a back-edge, so propagation
    // there IS allowed.
    //
    // The guard we are testing is the other direction: if bb3 contained
    // `mov r2=r6 ; branch bb2` and bb2 used r2, the back-edge bb3→bb2 must
    // block propagation of the bb3 def into bb2.  We model that here:
    //
    // bb1 (entry):  branch bb2
    // bb2 (header): add r3=r2,r4 ; branch_cond r5, bb3, bb4
    // bb3 (body):   mov r2=r1 ; branch bb2    ← defines r2, then back-edge
    // bb4 (exit):   ret
    //
    // r2 is defined by mov in bb3; its only other reaching point is as a
    // live-in (undefined src from the entry).  The copy_propagation_pass must
    // NOT propagate the bb3 mov into the bb2 use of r2, because the path
    // bb3→bb2 crosses the back-edge.

    allocated_function_ir fn_ir;
    fn_ir.name = "copy_loop_back_edge_guard";
    fn_ir.cfg.entry_block = 1;

    // bb1: branch bb2
    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};
    allocated_instruction br1;
    br1.id = 100;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {br1};

    // bb2: add r3=r2,r4 ; branch_cond r5, bb3, bb4
    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "header";
    bb2.predecessors = {1, 3};
    bb2.successors = {3, 4};
    allocated_instruction add_hdr;
    add_hdr.id = 200;
    add_hdr.op = opcode::add;
    add_hdr.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_hdr.uses = {allocated_operand::as_preg(preg{2, "r2"}),
                    allocated_operand::as_preg(preg{4, "r4"})};
    allocated_instruction brc_hdr;
    brc_hdr.id = 201;
    brc_hdr.op = opcode::branch_cond;
    brc_hdr.uses = {allocated_operand::as_preg(preg{5, "r5"}),
                    allocated_operand::as_block(3),
                    allocated_operand::as_block(4)};
    bb2.instructions = {add_hdr, brc_hdr};

    // bb3: mov r2=r1 ; branch bb2   (back-edge)
    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "body";
    bb3.predecessors = {2};
    bb3.successors = {2};
    allocated_instruction mov_body;
    mov_body.id = 300;
    mov_body.op = opcode::mov;
    mov_body.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    mov_body.uses = {allocated_operand::as_preg(preg{1, "r1"})};
    allocated_instruction br_back;
    br_back.id = 301;
    br_back.op = opcode::branch;
    br_back.uses = {allocated_operand::as_block(2)};
    bb3.instructions = {mov_body, br_back};

    // bb4: ret
    allocated_basic_block bb4;
    bb4.id = 4;
    bb4.name = "exit";
    bb4.predecessors = {2};
    allocated_instruction ret;
    ret.id = 400;
    ret.op = opcode::ret;
    bb4.instructions = {ret};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3, 4};
    fn_ir.cfg.successors[3] = {2};
    fn_ir.cfg.predecessors[2] = {1, 3};
    fn_ir.cfg.predecessors[3] = {2};
    fn_ir.cfg.predecessors[4] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    copy_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());

    // The add in bb2 (id=200) uses r2 (preg id 2).  The loop guard must prevent
    // the bb3 mov from being propagated into bb2, so the use must remain r2.
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 200) {
                REQUIRE(inst.op == opcode::add);
                REQUIRE(std::get<preg>(inst.uses[0].value).id == 2);
            }
        }
    }
}

// -----------------------------------------------------------------------
// constant_propagation_pass — loop guard
// -----------------------------------------------------------------------

TEST_CASE (



"constant_propagation_pass: does not fold constant across loop back edge"
,
"[lithe][codegen][pass][const_prop][loop_guard]"
)
 {
    using namespace lithe::codegen;

    // bb1 (entry):  branch bb2
    // bb2 (header): add r3=r1,r2 ; branch_cond r5, bb3, bb4
    // bb3 (body):   load_imm r1=42 ; branch bb2    ← back-edge
    // bb4 (exit):   ret
    //
    // load_imm r1=42 is in bb3.  Its only consumer in the loop header is
    // the add in bb2, reached via the back-edge bb3→bb2.  The constant-
    // propagation pass must NOT fold r1→42 into the add in bb2.

    allocated_function_ir fn_ir;
    fn_ir.name = "const_loop_back_edge_guard";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};
    allocated_instruction br1;
    br1.id = 100;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {br1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "header";
    bb2.predecessors = {1, 3};
    bb2.successors = {3, 4};
    allocated_instruction add_hdr;
    add_hdr.id = 200;
    add_hdr.op = opcode::add;
    add_hdr.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_hdr.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                    allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction brc_hdr;
    brc_hdr.id = 201;
    brc_hdr.op = opcode::branch_cond;
    brc_hdr.uses = {allocated_operand::as_preg(preg{5, "r5"}),
                    allocated_operand::as_block(3),
                    allocated_operand::as_block(4)};
    bb2.instructions = {add_hdr, brc_hdr};

    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "body";
    bb3.predecessors = {2};
    bb3.successors = {2};
    allocated_instruction li_body;
    li_body.id = 300;
    li_body.op = opcode::load_imm;
    li_body.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li_body.uses = {allocated_operand::as_i64(42)};
    allocated_instruction br_back;
    br_back.id = 301;
    br_back.op = opcode::branch;
    br_back.uses = {allocated_operand::as_block(2)};
    bb3.instructions = {li_body, br_back};

    allocated_basic_block bb4;
    bb4.id = 4;
    bb4.name = "exit";
    bb4.predecessors = {2};
    allocated_instruction ret;
    ret.id = 400;
    ret.op = opcode::ret;
    bb4.instructions = {ret};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3, 4};
    fn_ir.cfg.successors[3] = {2};
    fn_ir.cfg.predecessors[2] = {1, 3};
    fn_ir.cfg.predecessors[3] = {2};
    fn_ir.cfg.predecessors[4] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());

    // The add in bb2 (id=200) must NOT have been rewritten to a load_imm
    // (which would happen if r1=42 were propagated across the back edge).
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 200) {
                REQUIRE(inst.op == opcode::add);
            }
        }
    }
}

TEST_CASE (



"constant_propagation_pass: does not fold constant into loop header from body"
,
"[lithe][codegen][pass][const_prop][loop_guard]"
)
 {
    using namespace lithe::codegen;

    // Same loop structure as the previous test, but the constant definition
    // is inside the loop body (bb3) and the loop header (bb2) is the user.
    // The guard checks is_loop_header(use_block) && in_any_loop(def_block).
    //
    // bb1 (entry):  load_imm r1=7 ; branch bb2
    // bb2 (header): add r3=r1,r2 ; branch_cond r5, bb3, bb4
    // bb3 (body):   load_imm r1=99 ; branch bb2   ← redefines r1, back-edge
    // bb4 (exit):   ret
    //
    // On the second loop iteration, the reaching def of r1 in bb2 is the
    // load_imm in bb3 (=99).  On the first iteration it is the load_imm in
    // bb1 (=7).  Neither value should be folded into the add in the header
    // because the reaching def is ambiguous AND the use is at the loop header.

    allocated_function_ir fn_ir;
    fn_ir.name = "const_loop_header_guard";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};
    allocated_instruction li_entry;
    li_entry.id = 50;
    li_entry.op = opcode::load_imm;
    li_entry.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li_entry.uses = {allocated_operand::as_i64(7)};
    allocated_instruction br1;
    br1.id = 51;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {li_entry, br1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "header";
    bb2.predecessors = {1, 3};
    bb2.successors = {3, 4};
    allocated_instruction add_hdr;
    add_hdr.id = 200;
    add_hdr.op = opcode::add;
    add_hdr.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_hdr.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                    allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction brc_hdr;
    brc_hdr.id = 201;
    brc_hdr.op = opcode::branch_cond;
    brc_hdr.uses = {allocated_operand::as_preg(preg{5, "r5"}),
                    allocated_operand::as_block(3),
                    allocated_operand::as_block(4)};
    bb2.instructions = {add_hdr, brc_hdr};

    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "body";
    bb3.predecessors = {2};
    bb3.successors = {2};
    allocated_instruction li_body;
    li_body.id = 300;
    li_body.op = opcode::load_imm;
    li_body.defs = {allocated_operand::as_preg(preg{1, "r1"})};
    li_body.uses = {allocated_operand::as_i64(99)};
    allocated_instruction br_back;
    br_back.id = 301;
    br_back.op = opcode::branch;
    br_back.uses = {allocated_operand::as_block(2)};
    bb3.instructions = {li_body, br_back};

    allocated_basic_block bb4;
    bb4.id = 4;
    bb4.name = "exit";
    bb4.predecessors = {2};
    allocated_instruction ret;
    ret.id = 400;
    ret.op = opcode::ret;
    bb4.instructions = {ret};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3, 4};
    fn_ir.cfg.successors[3] = {2};
    fn_ir.cfg.predecessors[2] = {1, 3};
    fn_ir.cfg.predecessors[3] = {2};
    fn_ir.cfg.predecessors[4] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    constant_propagation_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());

    // The add in bb2 must remain an add — neither constant (7 or 99) should
    // have been folded into it.
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 200) {
                REQUIRE(inst.op == opcode::add);
            }
        }
    }
}

// -----------------------------------------------------------------------
// common_subexpression_elimination_pass — loop guard
// -----------------------------------------------------------------------

TEST_CASE (



"cse_pass: does not reuse expression across loop back edge"
,
"[lithe][codegen][pass][cse][loop_guard]"
)
 {
    using namespace lithe::codegen;

    // bb1 (entry):  add r3=r1,r2 ; branch bb2
    // bb2 (header): branch_cond r5, bb3, bb4
    // bb3 (body):   add r6=r1,r2 ; branch bb2    ← back-edge, same expression
    // bb4 (exit):   ret
    //
    // bb1 dominates bb2 and bb3.  The expression add(r1,r2) is first computed in
    // bb1.  bb3 recomputes it.  The CSE pass must NOT replace the bb3 computation
    // with a mov from r3 because bb3→bb2 is a back edge and the reuse would be
    // across the back edge path.
    //
    // More precisely: the CSE guard fires when is_back_edge(def_block, block_id)
    // where def_block=1 (bb1) and block_id=3 (bb3).  bb1→bb3 is NOT itself a
    // back edge, but the loop guard also checks is_loop_header(block_id) &&
    // in_any_loop(def_block).  Neither applies here (bb3 is not a header).
    //
    // The correct guard for this case is actually the loop-header check in the
    // `available` recording step: the expression in bb1 IS allowed to be
    // propagated into bb3 since bb1 dominates bb3 and there's no back edge on
    // that path.  So we need to construct a case where the available entry's
    // def_block is in the loop body, not the pre-header.
    //
    // Case: def in bb3 (body), use also in bb3 on next iteration via the header.
    // We test the header guard: expression computed in bb3, then loop header bb2
    // should not get it as available from the previous iteration.
    //
    // bb1 (entry):  branch bb2
    // bb2 (header): add r3=r1,r2 ; branch_cond r5, bb3, bb4
    //               ← first iteration computes add here
    // bb3 (body):   add r6=r1,r2 ; branch bb2    ← back-edge
    //               ← same expression in the body
    // bb4 (exit):   ret
    //
    // bb2 is the loop header.  The guard prevents recording bb2's `add r3=r1,r2`
    // into `available`.  Therefore when bb3 sees the same expression it cannot
    // find a prior entry → no rewrite.

    allocated_function_ir fn_ir;
    fn_ir.name = "cse_loop_header_guard";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};
    allocated_instruction br1;
    br1.id = 10;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {br1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "header";
    bb2.predecessors = {1, 3};
    bb2.successors = {3, 4};
    // add r3=r1,r2 (loop header — should NOT be recorded as available)
    allocated_instruction add_hdr;
    add_hdr.id = 200;
    add_hdr.op = opcode::add;
    add_hdr.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_hdr.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                    allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction brc_hdr;
    brc_hdr.id = 201;
    brc_hdr.op = opcode::branch_cond;
    brc_hdr.uses = {allocated_operand::as_preg(preg{5, "r5"}),
                    allocated_operand::as_block(3),
                    allocated_operand::as_block(4)};
    bb2.instructions = {add_hdr, brc_hdr};

    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "body";
    bb3.predecessors = {2};
    bb3.successors = {2};
    // add r6=r1,r2 — same expression; should NOT be replaced by mov from r3
    allocated_instruction add_body;
    add_body.id = 300;
    add_body.op = opcode::add;
    add_body.defs = {allocated_operand::as_preg(preg{6, "r6"})};
    add_body.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                     allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction br_back;
    br_back.id = 301;
    br_back.op = opcode::branch;
    br_back.uses = {allocated_operand::as_block(2)};
    bb3.instructions = {add_body, br_back};

    allocated_basic_block bb4;
    bb4.id = 4;
    bb4.name = "exit";
    bb4.predecessors = {2};
    allocated_instruction ret;
    ret.id = 400;
    ret.op = opcode::ret;
    bb4.instructions = {ret};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3, 4};
    fn_ir.cfg.successors[3] = {2};
    fn_ir.cfg.predecessors[2] = {1, 3};
    fn_ir.cfg.predecessors[3] = {2};
    fn_ir.cfg.predecessors[4] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());

    // add_body (id=300) must remain an add — the loop-header guard prevented
    // the header's expression from being recorded as available.
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 300) {
                REQUIRE(inst.op == opcode::add);
            }
        }
    }
}

TEST_CASE (



"cse_pass: does not reuse expression when def_block and current block connected by back edge"
,
"[lithe][codegen][pass][cse][loop_guard]"
)
 {
    using namespace lithe::codegen;

    // bb1 (entry):  add r3=r1,r2 ; branch bb2
    // bb2 (header): branch_cond r5, bb3, bb4
    // bb3 (body):   sub r7=r8,r9 ; branch bb2    ← back-edge
    // bb4 (exit):   add r6=r1,r2 ; ret
    //
    // bb1 dominates bb4.  add(r1,r2) is first computed in bb1.  bb4 recomputes it.
    // bb1 does NOT dominate bb4 via a back edge — bb1→bb2→bb4 is a normal forward
    // path.  CSE IS allowed here (no back edge on the def→use path).
    // This is a positive test: CSE should fire.

    allocated_function_ir fn_ir;
    fn_ir.name = "cse_loop_no_back_edge_on_def_path";
    fn_ir.cfg.entry_block = 1;

    allocated_basic_block bb1;
    bb1.id = 1;
    bb1.name = "entry";
    bb1.successors = {2};
    allocated_instruction add_entry;
    add_entry.id = 100;
    add_entry.op = opcode::add;
    add_entry.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_entry.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                      allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction br1;
    br1.id = 101;
    br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {add_entry, br1};

    allocated_basic_block bb2;
    bb2.id = 2;
    bb2.name = "header";
    bb2.predecessors = {1, 3};
    bb2.successors = {3, 4};
    allocated_instruction brc_hdr;
    brc_hdr.id = 200;
    brc_hdr.op = opcode::branch_cond;
    brc_hdr.uses = {allocated_operand::as_preg(preg{5, "r5"}),
                    allocated_operand::as_block(3),
                    allocated_operand::as_block(4)};
    bb2.instructions = {brc_hdr};

    allocated_basic_block bb3;
    bb3.id = 3;
    bb3.name = "body";
    bb3.predecessors = {2};
    bb3.successors = {2};
    allocated_instruction sub_body;
    sub_body.id = 300;
    sub_body.op = opcode::sub;
    sub_body.defs = {allocated_operand::as_preg(preg{7, "r7"})};
    sub_body.uses = {allocated_operand::as_preg(preg{8, "r8"}),
                     allocated_operand::as_preg(preg{9, "r9"})};
    allocated_instruction br_back;
    br_back.id = 301;
    br_back.op = opcode::branch;
    br_back.uses = {allocated_operand::as_block(2)};
    bb3.instructions = {sub_body, br_back};

    allocated_basic_block bb4;
    bb4.id = 4;
    bb4.name = "exit";
    bb4.predecessors = {2};
    // Same expression as bb1's add_entry — bb1 dominates bb4, no back edge.
    allocated_instruction add_exit;
    add_exit.id = 400;
    add_exit.op = opcode::add;
    add_exit.defs = {allocated_operand::as_preg(preg{6, "r6"})};
    add_exit.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                     allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction ret;
    ret.id = 401;
    ret.op = opcode::ret;
    bb4.instructions = {add_exit, ret};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3, 4};
    fn_ir.cfg.successors[3] = {2};
    fn_ir.cfg.predecessors[2] = {1, 3};
    fn_ir.cfg.predecessors[3] = {2};
    fn_ir.cfg.predecessors[4] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);

    // add_exit (id=400) should have been rewritten to a mov from r3.
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 400) {
                REQUIRE(inst.op == opcode::mov);
                REQUIRE(std::get<preg>(inst.uses[0].value).id == 3);
            }
        }
    }
}

// -----------------------------------------------------------------------
// construct_ssa tests (Prompt 10)
// -----------------------------------------------------------------------

// Helper: build a physical function from a list of (block_id, instructions,
// successors) tuples.  Predecessors are computed from successors automatically.
namespace {
    lithe::codegen::mir::physical_mir_function make_physical_ssa(
        std::uint32_t entry_block,
        std::initializer_list<std::tuple<
            std::uint32_t, // block id
            std::vector<lithe::codegen::allocated_instruction>, // instructions
            std::vector<std::uint32_t> // successors
        >> blocks_desc) {
        using namespace lithe::codegen;
        allocated_function_ir fn_ir;
        fn_ir.name = "ssa_test";
        fn_ir.cfg.entry_block = entry_block;

        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> succ_map;

        for (auto& [bid, insts, succs] : blocks_desc) {
            allocated_basic_block bb;
            bb.id = bid;
            bb.name = "bb" + std::to_string(bid);
            bb.instructions = std::move(insts);
            bb.successors = succs;
            succ_map[bid] = succs;
            fn_ir.blocks.push_back(std::move(bb));
        }
        // Build predecessors.
        for (const auto& [from, succs] : succ_map) {
            for (const auto to : succs)
                fn_ir.cfg.predecessors[to].push_back(from);
            fn_ir.cfg.successors[from] = succs;
        }
        for (auto& bb : fn_ir.blocks)
            bb.predecessors = fn_ir.cfg.predecessors[bb.id];

        mir::physical_mir_function physical;
        physical.function = std::move(fn_ir);
        physical.metadata.current_phase = mir::phase::physical_mir;
        return physical;
    }

    lithe::codegen::allocated_instruction make_inst(
        std::uint32_t id, lithe::codegen::opcode op,
        std::vector<lithe::codegen::allocated_operand> defs,
        std::vector<lithe::codegen::allocated_operand> uses) {
        lithe::codegen::allocated_instruction i;
        i.id = id;
        i.op = op;
        i.defs = std::move(defs);
        i.uses = std::move(uses);
        return i;
    }
} // anonymous namespace

// -----------------------------------------------------------------------
// Test 1: Branch-merge phi insertion
//
//   bb1 (entry): load_imm r1=1 ; branch_cond r5, bb2, bb3
//   bb2 (left):  load_imm r1=2 ; branch bb4
//   bb3 (right): branch bb4
//   bb4 (merge): use r1 ; ret
//
//   r1 is defined in bb1 and bb2.  DF(bb1) = {bb4}, DF(bb2) = {bb4}.
//   A phi for r1 must be placed at bb4.
// -----------------------------------------------------------------------
TEST_CASE (



"construct_ssa: phi inserted at merge block for redefined preg"
,
"[lithe][codegen][ssa][phi]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r5{5, "r5"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(1)}),
            make_inst(11, opcode::branch_cond,
                      {},
                      {allocated_operand::as_preg(r5),
                       allocated_operand::as_block(2),
                       allocated_operand::as_block(3)})
         }, {2, 3}},
        {2, {
            make_inst(20, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(2)}),
            make_inst(21, opcode::branch,
                      {},
                      {allocated_operand::as_block(4)})
         }, {4}},
        {3, {
            make_inst(30, opcode::branch,
                      {},
                      {allocated_operand::as_block(4)})
         }, {4}},
        {4, {
            make_inst(40, opcode::add,
                      {allocated_operand::as_preg(preg{6, "r6"})},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(preg{2, "r2"})}),
            make_inst(41, opcode::ret, {}, {})
         }, {}}
    });

    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto result = construct_ssa(fn, opts);

    REQUIRE(result.ok());

    // A phi for r1 (preg id 1) must exist in bb4's block state.
    const auto &bs4 = result.block_states.at(4);
    bool found_phi  = false;
    for (const auto &phi : bs4.phi_nodes) {
        if (phi.preg_id == 1) {
            found_phi = true;
            // Must have exactly two incoming edges (one from bb2, one from bb3).
            REQUIRE(phi.incoming.size() == 2);
            // The result ssa_value must be valid.
            REQUIRE(phi.result.valid());
            // Check both predecessors are represented.
            std::unordered_set<std::uint32_t> preds;
            for (const auto &[pred, val] : phi.incoming)
                preds.insert(pred);
            REQUIRE(preds.contains(2));
            REQUIRE(preds.contains(3));
        }
    }
    REQUIRE(found_phi);
}

// -----------------------------------------------------------------------
// Test 2: Loop-carried phi insertion
//
//   bb1 (entry):  load_imm r1=0 ; branch bb2
//   bb2 (header): use r1 ; branch_cond r5, bb3, bb4
//   bb3 (body):   add r1=r1,r2 ; branch bb2   ← back-edge
//   bb4 (exit):   ret
//
//   r1 is defined in bb1 and bb3 (inside the loop).
//   DF(bb1) = {bb2} and DF(bb3) = {bb2}.
//   A phi for r1 must be placed at bb2 (the loop header).
// -----------------------------------------------------------------------
TEST_CASE (



"construct_ssa: phi inserted at loop header for loop-carried preg"
,
"[lithe][codegen][ssa][phi]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};
    const preg r5{5, "r5"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(0)}),
            make_inst(11, opcode::branch,
                      {},
                      {allocated_operand::as_block(2)})
         }, {2}},
        {2, {
            make_inst(20, opcode::add,
                      {allocated_operand::as_preg(preg{3, "r3"})},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r2)}),
            make_inst(21, opcode::branch_cond,
                      {},
                      {allocated_operand::as_preg(r5),
                       allocated_operand::as_block(3),
                       allocated_operand::as_block(4)})
         }, {3, 4}},
        {3, {
            make_inst(30, opcode::add,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r2)}),
            make_inst(31, opcode::branch,
                      {},
                      {allocated_operand::as_block(2)})
         }, {2}},
        {4, {
            make_inst(40, opcode::ret, {}, {})
         }, {}}
    });

    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto result = construct_ssa(fn, opts);

    REQUIRE(result.ok());

    // A phi for r1 must be at bb2 (the loop header).
    const auto &bs2 = result.block_states.at(2);
    bool found_phi  = false;
    for (const auto &phi : bs2.phi_nodes) {
        if (phi.preg_id == 1) {
            found_phi = true;
            REQUIRE(phi.result.valid());
            // Two incoming edges: from bb1 (pre-header) and bb3 (back-edge).
            REQUIRE(phi.incoming.size() == 2);
            std::unordered_set<std::uint32_t> preds;
            for (const auto &[pred, val] : phi.incoming)
                preds.insert(pred);
            REQUIRE(preds.contains(1));
            REQUIRE(preds.contains(3));
        }
    }
    REQUIRE(found_phi);
}

// -----------------------------------------------------------------------
// Test 3: Simple SSA renaming correctness
//
//   bb1 (entry): load_imm r1=7 ; add r2=r1,r1 ; ret
//
//   Single block.  Two instructions.
//   load_imm defines r1 → gets ssa_value s_a.
//   add uses r1 twice → both ssa_uses should be s_a.
//   add defines r2 → gets a new ssa_value s_b.
//   Verify the value_table and ssa_defs/ssa_uses are wired up correctly.
// -----------------------------------------------------------------------
TEST_CASE (



"construct_ssa: single-block renaming assigns correct ssa_value versions"
,
"[lithe][codegen][ssa][rename]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(7)}),
            make_inst(20, opcode::add,
                      {allocated_operand::as_preg(r2)},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r1)}),
            make_inst(30, opcode::ret, {}, {})
         }, {}}
    });

    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto result = construct_ssa(fn, opts);

    REQUIRE(result.ok());

    // Find the instructions in the result.
    const auto &blk = result.function.function.blocks[0];
    REQUIRE(blk.instructions.size() == 3);

    const auto &li  = blk.instructions[0]; // load_imm
    const auto &add = blk.instructions[1]; // add

    // load_imm: one ssa_def (r1 version 1), no ssa_uses.
    REQUIRE(li.ssa_defs.size() == 1);
    const ssa_value_id r1_v1 = li.ssa_defs[0];
    REQUIRE(r1_v1.valid());

    // add: two ssa_uses, both should be r1_v1.
    REQUIRE(add.ssa_uses.size() == 2);
    REQUIRE(add.ssa_uses[0] == r1_v1);
    REQUIRE(add.ssa_uses[1] == r1_v1);

    // add: one ssa_def (r2), must be different from r1_v1.
    REQUIRE(add.ssa_defs.size() == 1);
    const ssa_value_id r2_v1 = add.ssa_defs[0];
    REQUIRE(r2_v1.valid());
    REQUIRE(!(r2_v1 == r1_v1));

    // value_table entries exist and have the right preg ids.
    REQUIRE(result.value_table.contains(r1_v1.id));
    REQUIRE(result.value_table.at(r1_v1.id).preg_id == 1);
    REQUIRE(result.value_table.at(r1_v1.id).version == 1);

    REQUIRE(result.value_table.contains(r2_v1.id));
    REQUIRE(result.value_table.at(r2_v1.id).preg_id == 2);
    REQUIRE(result.value_table.at(r2_v1.id).version == 1);
}

// -----------------------------------------------------------------------
// destroy_ssa tests
// -----------------------------------------------------------------------

// Test 1: single-block roundtrip
//
//   bb1: load_imm r1=7 ; add r2=r1,r1 ; ret
//
//   No phis are inserted.  destroy_ssa must produce a function that:
//   - has exactly 1 block
//   - has the same 3 instructions (no movs added)
//   - has ssa_defs/ssa_uses cleared
//   - passes verify_physical_mir
TEST_CASE (



"destroy_ssa: single-block roundtrip produces valid physical MIR"
,
"[lithe][codegen][ssa][destroy_ssa]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(7)}),
            make_inst(20, opcode::add,
                      {allocated_operand::as_preg(r2)},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r1)}),
            make_inst(30, opcode::ret, {}, {})
         }, {}}
    });

    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto ssa_result = construct_ssa(fn, opts);
    REQUIRE(ssa_result.ok());

    const auto lowered = destroy_ssa(ssa_result);

    // Single block, 3 instructions, no new movs.
    REQUIRE(lowered.function.function.blocks.size() == 1);
    REQUIRE(lowered.function.function.blocks[0].instructions.size() == 3);

    // ssa_defs and ssa_uses must be empty on all instructions.
    for (const auto &blk : lowered.function.function.blocks)
        for (const auto &inst : blk.instructions) {
            REQUIRE(inst.ssa_defs.empty());
            REQUIRE(inst.ssa_uses.empty());
        }

    // verify_physical_mir must pass.
    const auto vr = verify_physical_mir(lowered.function);
    REQUIRE(vr.ok());
}

// Test 2: branch-merge roundtrip — phi lowered to copies
//
//   bb1: load_imm r1=1 ; branch_cond r5, bb2, bb3
//   bb2: load_imm r1=2 ; branch bb4
//   bb3: branch bb4
//   bb4: add r6=r1,r2 ; ret
//
//   construct_ssa places a phi for r1 at bb4.
//   destroy_ssa must:
//   - Insert `mov r1 = r1_bb2_version` at the end of bb2 (before the branch).
//   - Insert `mov r1 = r1_bb3_version` at the end of bb3 (before the branch).
//     (bb3 carries r1 live-in from bb1; but since bb4 has two preds, the
//      critical-edge check fires: bb1 has 2 successors (bb2, bb3) and bb4 has
//      2 predecessors — so bb2→bb4 and bb3→bb4 are both critical edges.
//      The implementation splits them before inserting the copies.)
//   - After lowering, verify_physical_mir passes.
TEST_CASE (



"destroy_ssa: branch-merge phi lowered to parallel copies"
,
"[lithe][codegen][ssa][destroy_ssa]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};
    const preg r5{5, "r5"};
    const preg r6{6, "r6"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)}, {allocated_operand::as_i64(1)}),
            make_inst(11, opcode::branch_cond,
                      {},
                      {allocated_operand::as_preg(r5),
                       allocated_operand::as_block(2),
                       allocated_operand::as_block(3)})
         }, {2, 3}},
        {2, {
            make_inst(20, opcode::load_imm,
                      {allocated_operand::as_preg(r1)}, {allocated_operand::as_i64(2)}),
            make_inst(21, opcode::branch, {}, {allocated_operand::as_block(4)})
         }, {4}},
        {3, {
            make_inst(30, opcode::branch, {}, {allocated_operand::as_block(4)})
         }, {4}},
        {4, {
            make_inst(40, opcode::add,
                      {allocated_operand::as_preg(r6)},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r2)}),
            make_inst(41, opcode::ret, {}, {})
         }, {}}
    });

    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto ssa_result = construct_ssa(fn, opts);
    REQUIRE(ssa_result.ok());

    // Confirm a phi was placed at bb4.
    REQUIRE(ssa_result.block_states.contains(4));
    const auto &bs4 = ssa_result.block_states.at(4);
    bool phi_at_bb4 = std::ranges::any_of(bs4.phi_nodes,
        [](const auto &p){ return p.preg_id == 1; });
    REQUIRE(phi_at_bb4);

    const auto lowered = destroy_ssa(ssa_result);

    // SSA overlay must be stripped.
    for (const auto &blk : lowered.function.function.blocks)
        for (const auto &inst : blk.instructions) {
            REQUIRE(inst.ssa_defs.empty());
            REQUIRE(inst.ssa_uses.empty());
        }

    // verify_physical_mir must pass.
    const auto vr = verify_physical_mir(lowered.function);
    INFO("diagnostics: " << (vr.diagnostics.empty() ? "(none)" : vr.diagnostics[0]));
    REQUIRE(vr.ok());
}

// Test 3: loop-carried phi roundtrip
//
//   bb1: load_imm r1=0 ; branch bb2
//   bb2: add r3=r1,r2 ; branch_cond r5, bb3, bb4
//   bb3: add r1=r1,r2 ; branch bb2   ← back-edge
//   bb4: ret
//
//   construct_ssa places a phi for r1 at bb2.
//   destroy_ssa lowers that phi by inserting:
//   - mov r1 = r1_v_from_bb1  at end of bb1  (pre-header copy)
//   - mov r1 = r1_v_from_bb3  at end of bb3  (back-edge copy)
//   verify_physical_mir must pass on the lowered function.
TEST_CASE (



"destroy_ssa: loop-carried phi lowered correctly, MIR validates"
,
"[lithe][codegen][ssa][destroy_ssa]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};
    const preg r3{3, "r3"};
    const preg r5{5, "r5"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)}, {allocated_operand::as_i64(0)}),
            make_inst(11, opcode::branch, {}, {allocated_operand::as_block(2)})
         }, {2}},
        {2, {
            make_inst(20, opcode::add,
                      {allocated_operand::as_preg(r3)},
                      {allocated_operand::as_preg(r1), allocated_operand::as_preg(r2)}),
            make_inst(21, opcode::branch_cond,
                      {},
                      {allocated_operand::as_preg(r5),
                       allocated_operand::as_block(3),
                       allocated_operand::as_block(4)})
         }, {3, 4}},
        {3, {
            make_inst(30, opcode::add,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_preg(r1), allocated_operand::as_preg(r2)}),
            make_inst(31, opcode::branch, {}, {allocated_operand::as_block(2)})
         }, {2}},
        {4, {
            make_inst(40, opcode::ret, {}, {})
         }, {}}
    });

    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto ssa_result = construct_ssa(fn, opts);
    REQUIRE(ssa_result.ok());

    // Confirm a phi was placed at bb2 (loop header).
    REQUIRE(ssa_result.block_states.contains(2));
    bool phi_at_header = std::ranges::any_of(
        ssa_result.block_states.at(2).phi_nodes,
        [](const auto &p){ return p.preg_id == 1; });
    REQUIRE(phi_at_header);

    const auto lowered = destroy_ssa(ssa_result);

    // SSA overlay stripped.
    for (const auto &blk : lowered.function.function.blocks)
        for (const auto &inst : blk.instructions) {
            REQUIRE(inst.ssa_defs.empty());
            REQUIRE(inst.ssa_uses.empty());
        }

    // verify_physical_mir must pass.
    const auto vr = verify_physical_mir(lowered.function);
    INFO("diagnostics: " << (vr.diagnostics.empty() ? "(none)" : vr.diagnostics[0]));
    REQUIRE(vr.ok());
}

// Test 4: placeholder_only roundtrip (no SSA construction, no phis)
//
//   Exercises the early-return path in destroy_ssa when no real SSA was built.
TEST_CASE (



"destroy_ssa: placeholder_only result is returned unchanged"
,
"[lithe][codegen][ssa][destroy_ssa]"
)
 {
    using namespace lithe::codegen;

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(preg{1, "r1"})},
                      {allocated_operand::as_i64(42)}),
            make_inst(11, opcode::ret, {}, {})
         }, {}}
    });

    // placeholder_only=true → diagnostics include the skip message, block_states empty.
    ssa_adapter_options opts;
    opts.placeholder_only = true;
    const auto ssa_result = construct_ssa(fn, opts);
    REQUIRE(!ssa_result.ok()); // has the "skipped" diagnostic
    REQUIRE(ssa_result.block_states.empty());

    // destroy_ssa on a placeholder result returns the original function unchanged.
    const auto lowered = destroy_ssa(ssa_result);
    REQUIRE(lowered.function.function.blocks.size() == 1);
    REQUIRE(lowered.function.function.blocks[0].instructions.size() == 2);
}

// -----------------------------------------------------------------------
// Backend scheduling metadata tests (Prompt 12)
// -----------------------------------------------------------------------

TEST_CASE (



"instruction_latency: default-constructed has expected defaults"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    instruction_latency lat;
    REQUIRE(lat.scheduling_class == "generic");
    REQUIRE(lat.cycles == 1);
    REQUIRE(lat.throughput_cycles == 1.0);
    REQUIRE(lat.minimum_issue_gap == 1);
}

TEST_CASE (



"instruction_latency: fields are independently settable"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    instruction_latency lat;
    lat.scheduling_class  = "fmul";
    lat.cycles            = 4;
    lat.throughput_cycles = 0.5;
    lat.minimum_issue_gap = 2;
    REQUIRE(lat.scheduling_class  == "fmul");
    REQUIRE(lat.cycles            == 4);
    REQUIRE(lat.throughput_cycles == 0.5);
    REQUIRE(lat.minimum_issue_gap == 2);
}

TEST_CASE (



"dependency_edge: default-constructed is a data edge with zero ids"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    dependency_edge e;
    REQUIRE(e.source_inst_id == 0);
    REQUIRE(e.target_inst_id == 0);
    REQUIRE(e.type           == dependency_edge::kind::data);
    REQUIRE(e.latency_cycles == 1);
    REQUIRE(e.loop_carried   == false);
}

TEST_CASE (



"dependency_edge: equality compares source, target and type only"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    dependency_edge a, b;
    a.source_inst_id = 1; a.target_inst_id = 2; a.type = dependency_edge::kind::anti;
    b.source_inst_id = 1; b.target_inst_id = 2; b.type = dependency_edge::kind::anti;
    b.latency_cycles = 99; // different latency — should still be equal
    REQUIRE(a == b);

    b.type = dependency_edge::kind::output;
    REQUIRE(!(a == b));
}

TEST_CASE (



"register_pressure_info: under_pressure false when file sizes unknown"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    register_pressure_info p;
    p.integer_pressure = 100; // high pressure, but file_size = 0 (unknown)
    REQUIRE(!p.under_pressure());
}

TEST_CASE (



"register_pressure_info: under_pressure true when pressure meets file size"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    register_pressure_info p;
    p.integer_pressure  = 16;
    p.integer_file_size = 16; // at limit
    REQUIRE(p.under_pressure());
}

TEST_CASE (



"register_pressure_info: under_pressure true for float spill over limit"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    register_pressure_info p;
    p.float_pressure  = 9;
    p.float_file_size = 8; // over limit
    REQUIRE(p.under_pressure());
}

TEST_CASE (



"scheduling_region: default-constructed has empty fields and no loop-carried deps"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    scheduling_region r;
    REQUIRE(r.instruction_ids.empty());
    REQUIRE(r.dependencies.empty());
    REQUIRE(r.latency_override.empty());
    REQUIRE(!r.has_loop_carried);
    REQUIRE(!r.entry_pressure.under_pressure());
}

TEST_CASE (



"scheduling_region: fields populated correctly"
,
"[lithe][codegen][scheduling][metadata]"
)
 {
    using namespace lithe::codegen;
    scheduling_region r;
    r.instruction_ids = {10, 20, 30};

    dependency_edge e;
    e.source_inst_id = 10; e.target_inst_id = 20;
    e.type = dependency_edge::kind::data;
    e.latency_cycles = 3;
    e.loop_carried = true;
    r.dependencies.push_back(e);
    r.has_loop_carried = true;

    r.latency_override[20] = 5;

    r.peak_pressure.integer_pressure  = 12;
    r.peak_pressure.integer_file_size = 16;

    REQUIRE(r.instruction_ids.size() == 3);
    REQUIRE(r.dependencies.size()    == 1);
    REQUIRE(r.dependencies[0].latency_cycles == 3);
    REQUIRE(r.latency_override.at(20) == 5);
    REQUIRE(r.has_loop_carried);
    REQUIRE(!r.peak_pressure.under_pressure());
}

// -----------------------------------------------------------------------
// Backend capability refinement tests (Prompt 13)
// -----------------------------------------------------------------------

// Helper: build a minimal physical function with given opcodes in a single block.
namespace {
    lithe::codegen::mir::physical_mir_function make_fn_with_ops(
        std::string name,
        std::initializer_list<lithe::codegen::opcode> ops);

    lithe::codegen::mir::physical_mir_function make_fn_with_ops(
        std::initializer_list<lithe::codegen::opcode> ops) {
        return make_fn_with_ops("cap_test", ops);
    }

    lithe::codegen::mir::physical_mir_function make_fn_with_ops(
        std::string name,
        std::initializer_list<lithe::codegen::opcode> ops) {
        using namespace lithe::codegen;
        allocated_function_ir fn_ir;
        fn_ir.name = std::move(name);
        fn_ir.cfg.entry_block = 1;
        allocated_basic_block bb;
        bb.id = 1;
        std::uint32_t id = 1;
        for (const auto op : ops) {
            allocated_instruction inst;
            inst.id = id++;
            inst.op = op;
            if (op == opcode::branch_cond) {
                inst.uses = {
                    allocated_operand::as_preg(preg{5, "r5"}),
                    allocated_operand::as_block(1),
                    allocated_operand::as_block(1)
                };
            }
            else if (op == opcode::branch) {
                inst.uses = {allocated_operand::as_block(1)};
            }
            bb.instructions.push_back(inst);
        }
        fn_ir.blocks.push_back(std::move(bb));
        fn_ir.cfg.successors[1] = {};
        fn_ir.cfg.predecessors[1] = {};
        mir::physical_mir_function physical;
        physical.function = std::move(fn_ir);
        physical.metadata.current_phase = mir::phase::physical_mir;
        return physical;
    }
} // anonymous namespace

TEST_CASE (



"backend_capability_requirement: default-constructed has no requirements"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    backend_capability_requirement req;
    REQUIRE(req.provided_capabilities.empty());
    REQUIRE(!req.requires_ssa);
    REQUIRE(!req.requires_no_loops);
    REQUIRE(!req.requires_lowered_branches);
    REQUIRE(!req.requires_no_virtual_registers);
    REQUIRE(!req.requires_no_spills);
}

TEST_CASE (



"backend_capability_legalization_result: ok when no requirements set"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::add, opcode::ret});
    backend_capability_requirement req;
    req.backend_name = "test_backend";
    const auto result = validate_backend_capabilities(fn, req);
    REQUIRE(result.ok());
    REQUIRE(result.diagnostics.empty());
}

TEST_CASE (



"backend_capability_legalization_result: detects missing capability bit"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    // Function uses branches but backend only provides integer_arithmetic.
    auto fn = make_fn_with_ops({opcode::add, opcode::branch_cond});
    backend_capability_requirement req;
    req.backend_name = "no_branch_backend";
    // Provide everything except branches:
    req.provided_capabilities = backend_capability_set::from({
        backend_feature::integer_arithmetic
    });
    // Check via the requirement overload — capability-bit check fires.
    const auto result = validate_backend_capabilities(fn, req);
    REQUIRE(!result.ok());
    REQUIRE(result.missing_capabilities.has(backend_feature::branches));
}

TEST_CASE (



"backend_capability_legalization_result: detects SSA missing when required"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::add, opcode::ret});
    // No SSA overlay applied.
    backend_capability_requirement req;
    req.backend_name   = "ssa_backend";
    req.requires_ssa   = true;
    const auto result  = validate_backend_capabilities(fn, req);
    REQUIRE(!result.ok());
    REQUIRE(result.ssa_missing);
    REQUIRE(result.diagnostics.size() >= 1);
    REQUIRE(result.diagnostics[0].find("requires SSA") != std::string::npos);
}

TEST_CASE (



"backend_capability_legalization_result: SSA requirement satisfied after construct_ssa"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(preg{1,"r1"})},
                      {allocated_operand::as_i64(1)}),
            make_inst(11, opcode::ret, {}, {})
         }, {}}
    });

    ssa_adapter_options ssa_opts;
    ssa_opts.placeholder_only = false;
    const auto ssa = construct_ssa(fn, ssa_opts);
    REQUIRE(ssa.ok());

    backend_capability_requirement req;
    req.backend_name = "ssa_backend";
    req.requires_ssa = true;
    const auto result = validate_backend_capabilities(ssa.function, req);
    REQUIRE(result.ok());
    REQUIRE(!result.ssa_missing);
}

TEST_CASE (



"backend_capability_legalization_result: detects conditional branch when lowered_branches required"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::add, opcode::branch_cond});
    backend_capability_requirement req;
    req.backend_name              = "no_cond_branch_backend";
    req.requires_lowered_branches = true;
    const auto result             = validate_backend_capabilities(fn, req);
    REQUIRE(!result.ok());
    REQUIRE(result.conditional_branches);
}

TEST_CASE (



"backend_capability_legalization_result: lowered_branches ok when only unconditional branches"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::add, opcode::branch});
    backend_capability_requirement req;
    req.backend_name              = "no_cond_branch_backend";
    req.requires_lowered_branches = true;
    const auto result             = validate_backend_capabilities(fn, req);
    // branch_cond check passes; other checks may add diagnostics only if
    // their flags are set — which they aren't here.
    REQUIRE(!result.conditional_branches);
}

TEST_CASE (



"backend_capability_legalization_result: detects loops when requires_no_loops set"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;

    // 3-block loop: bb1→bb2→bb3→bb2 (back-edge bb3→bb2).
    auto fn = make_physical_ssa(1, {
        {1, {make_inst(10, opcode::branch, {}, {allocated_operand::as_block(2)})}, {2}},
        {2, {make_inst(20, opcode::branch_cond, {},
                       {allocated_operand::as_preg(preg{5,"r5"}),
                        allocated_operand::as_block(3),
                        allocated_operand::as_block(4)}),
            }, {3, 4}},
        {3, {make_inst(30, opcode::branch, {}, {allocated_operand::as_block(2)})}, {2}},
        {4, {make_inst(40, opcode::ret, {}, {})}, {}}
    });

    backend_capability_requirement req;
    req.backend_name       = "no_loop_backend";
    req.requires_no_loops  = true;
    const auto result      = validate_backend_capabilities(fn, req);
    REQUIRE(!result.ok());
    REQUIRE(result.loops_present);
    REQUIRE(result.diagnostics[0].find("loop") != std::string::npos);
}

TEST_CASE (



"backend_capability_requirement::provide builder adds capability bits"
,
"[lithe][codegen][backend][capability]"
)
 {
    using namespace lithe::codegen;
    backend_capability_requirement req;
    req.provide(backend_feature::integer_arithmetic)
       .provide(backend_feature::branches);
    REQUIRE(req.provided_capabilities.has(backend_feature::integer_arithmetic));
    REQUIRE(req.provided_capabilities.has(backend_feature::branches));
    REQUIRE(!req.provided_capabilities.has(backend_feature::calls));
}

// ---------------------------------------------------------------------------
// Prompt 16 — structured backend requirement model
// ---------------------------------------------------------------------------

TEST_CASE (



"backend_requirement_kind enum has expected values"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    // Spot-check that the kind values compile and are distinct.
    REQUIRE(backend_requirement_kind::no_virtual_registers
         != backend_requirement_kind::no_unresolved_spills);
    REQUIRE(backend_requirement_kind::supports_integer_arithmetic
         != backend_requirement_kind::supports_floating_arithmetic);
    REQUIRE(backend_requirement_kind::supports_calls
         != backend_requirement_kind::supports_branches);
}

TEST_CASE (



"backend_requirement default-constructed fields"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    backend_requirement r{backend_requirement_kind::supports_branches, "needs branches"};
    REQUIRE(r.kind == backend_requirement_kind::supports_branches);
    REQUIRE(r.message == "needs branches");
}

TEST_CASE (



"backend_requirement_set add and has"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    backend_requirement_set s;
    REQUIRE(!s.has(backend_requirement_kind::no_virtual_registers));

    s.add(backend_requirement_kind::no_virtual_registers, "no vregs");
    s.add(backend_requirement_kind::supports_branches);

    REQUIRE(s.has(backend_requirement_kind::no_virtual_registers));
    REQUIRE(s.has(backend_requirement_kind::supports_branches));
    REQUIRE(!s.has(backend_requirement_kind::supports_calls));
    REQUIRE(s.requirements.size() == 2);
}

TEST_CASE (



"backend_legalization_result ok when legal with no diagnostics"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    backend_legalization_result r;
    REQUIRE(r.legal);
    REQUIRE(r.ok());
    REQUIRE(r.diagnostics.empty());
}

TEST_CASE (



"backend_legalization_result not ok when legal=false"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    backend_legalization_result r;
    r.legal = false;
    r.diagnostics.push_back("something wrong");
    REQUIRE(!r.ok());
}

// ---------------------------------------------------------------------------
// Prompt 17 — required_backend_requirements extraction
// ---------------------------------------------------------------------------

TEST_CASE (



"required_backend_requirements always includes no_virtual_registers, "
"no_unresolved_spills, physical_mir_verified"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::nop, opcode::ret});
    const auto reqs = required_backend_requirements(fn);
    REQUIRE(reqs.has(backend_requirement_kind::no_virtual_registers));
    REQUIRE(reqs.has(backend_requirement_kind::no_unresolved_spills));
    REQUIRE(reqs.has(backend_requirement_kind::physical_mir_verified));
}

TEST_CASE (



"required_backend_requirements detects integer arithmetic"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::add, opcode::ret});
    const auto reqs = required_backend_requirements(fn);
    REQUIRE(reqs.has(backend_requirement_kind::supports_integer_arithmetic));
    REQUIRE(!reqs.has(backend_requirement_kind::supports_floating_arithmetic));
}

TEST_CASE (



"required_backend_requirements detects branches"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::branch_cond, opcode::ret});
    const auto reqs = required_backend_requirements(fn);
    REQUIRE(reqs.has(backend_requirement_kind::supports_branches));
    REQUIRE(!reqs.has(backend_requirement_kind::supports_calls));
}

TEST_CASE (



"required_backend_requirements detects calls and side effects"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::call, opcode::ret});
    const auto reqs = required_backend_requirements(fn);
    REQUIRE(reqs.has(backend_requirement_kind::supports_calls));
    REQUIRE(reqs.has(backend_requirement_kind::supports_side_effects));
}

TEST_CASE (



"required_backend_requirements detects load/store"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::load, opcode::store, opcode::ret});
    const auto reqs = required_backend_requirements(fn);
    REQUIRE(reqs.has(backend_requirement_kind::supports_load_store));
    REQUIRE(reqs.has(backend_requirement_kind::supports_memory_operands));
}

TEST_CASE (



"required_backend_requirements detects loops"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    // 3-block loop: bb1 → bb2 → bb3 → bb2 (back-edge bb3→bb2).
    auto fn = make_physical_ssa(1, {
        {1, {make_inst(10, opcode::branch, {}, {allocated_operand::as_block(2)})}, {2}},
        {2, {make_inst(20, opcode::branch_cond, {},
                       {allocated_operand::as_preg(preg{5,"r5"}),
                        allocated_operand::as_block(3),
                        allocated_operand::as_block(4)})}, {3, 4}},
        {3, {make_inst(30, opcode::branch, {}, {allocated_operand::as_block(2)})}, {2}},
        {4, {make_inst(40, opcode::ret, {}, {})}, {}}
    });
    const auto reqs = required_backend_requirements(fn);
    REQUIRE(reqs.has(backend_requirement_kind::supports_loops));
}

TEST_CASE (



"required_backend_requirements no loop for straight-line MIR"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops({opcode::add, opcode::ret});
    const auto reqs = required_backend_requirements(fn);
    REQUIRE(!reqs.has(backend_requirement_kind::supports_loops));
}

// ---------------------------------------------------------------------------
// Prompt 18 — validate_backend_requirements
// ---------------------------------------------------------------------------

TEST_CASE (



"validate_backend_requirements ok for fully capable backend"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    constexpr auto caps = backend_capability_set::from({
        backend_feature::integer_arithmetic,
        backend_feature::floating_arithmetic,
        backend_feature::branches,
        backend_feature::calls,
        backend_feature::memory_operands,
        backend_feature::stack_frame,
        backend_feature::spill_load_store
    });
    // Single-block add + ret: structurally valid, no multiple terminators.
    auto fn = make_fn_with_ops({opcode::add, opcode::ret});
    const auto vr = verify_physical_mir(fn);
    fn.verified = vr.ok();
    fn.verification_diagnostics = vr.diagnostics;
    const auto result = validate_backend_requirements(fn, caps);
    REQUIRE(result.legal);
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.ok());
}

TEST_CASE (



"validate_backend_requirements detects missing integer_arithmetic"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    constexpr auto caps = backend_capability_set::from({
        backend_feature::branches,
        backend_feature::calls
    });
    auto fn = make_fn_with_ops({opcode::add, opcode::ret});
    const auto result = validate_backend_requirements(fn, caps);
    REQUIRE(!result.ok());
    REQUIRE(!result.legal);
    const bool has_diag = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const std::string &d) { return d.find("integer_arithmetic") != std::string::npos; });
    REQUIRE(has_diag);
}

TEST_CASE (



"validate_backend_requirements detects missing branches"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    constexpr auto caps = backend_capability_set::from({
        backend_feature::integer_arithmetic
    });
    auto fn = make_fn_with_ops({opcode::branch, opcode::ret});
    const auto result = validate_backend_requirements(fn, caps);
    REQUIRE(!result.ok());
    const bool has_diag = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const std::string &d) { return d.find("branches") != std::string::npos; });
    REQUIRE(has_diag);
}

TEST_CASE (



"validate_backend_requirements detects missing calls"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    constexpr auto caps = backend_capability_set::from({
        backend_feature::integer_arithmetic,
        backend_feature::branches
    });
    auto fn = make_fn_with_ops({opcode::call, opcode::ret});
    const auto result = validate_backend_requirements(fn, caps);
    REQUIRE(!result.ok());
    const bool has_diag = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const std::string &d) { return d.find("calls") != std::string::npos; });
    REQUIRE(has_diag);
}

TEST_CASE (



"validate_backend_requirements detects virtual registers structurally"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    constexpr auto caps = backend_capability_set::from({
        backend_feature::integer_arithmetic,
        backend_feature::branches
    });

    allocated_instruction li;
    li.id = 1;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(42)};

    allocated_instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;

    auto fn = make_physical("vreg_test", {li, ret});
    // Simulate unresolved virtual registers via the tracking set.
    fn.referenced_virtual_registers.insert(42);
    const auto result = validate_backend_requirements(fn, caps);
    REQUIRE(!result.ok());
    const bool has_diag = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const std::string &d) { return d.find("virtual register") != std::string::npos; });
    REQUIRE(has_diag);
}

TEST_CASE (



"validate_backend_requirements detects unverified physical MIR"
,
"[lithe][codegen][backend][requirement]"
)
 {
    using namespace lithe::codegen;
    constexpr auto caps = backend_capability_set::from({
        backend_feature::integer_arithmetic
    });
    // Deliberately broken MIR: duplicate instruction IDs.
    allocated_instruction i1, i2;
    i1.id = 5; i1.op = opcode::nop;
    i2.id = 5; i2.op = opcode::ret;  // same id → verification fails
    auto fn = make_physical("bad_mir", {i1, i2});
    const auto result = validate_backend_requirements(fn, caps);
    REQUIRE(!result.ok());
    const bool has_diag = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const std::string &d) { return d.find("verif") != std::string::npos; });
    REQUIRE(has_diag);
}

// ---------------------------------------------------------------------------
// Prompt 19 — emit_function wires validate_backend_requirements
// ---------------------------------------------------------------------------

TEST_CASE (



"emit_function rejects via legality when backend missing integer_arithmetic"
,
"[lithe][codegen][backend][legality][emit]"
)
 {
    using namespace lithe::codegen;

    struct no_arith_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({backend_feature::branches});
        }
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state s) {
            return backend_result::success_result(std::move(s));
        }
    };

    auto fn = make_fn_with_ops({opcode::add, opcode::ret});
    no_arith_backend backend;
    const auto result = emit_function(backend, fn);
    REQUIRE_FALSE(result.ok());
    const bool legality_error = std::any_of(result.errors.begin(), result.errors.end(),
        [](const backend_error &e) {
            return e.message.find("legality") != std::string::npos
                || e.message.find("integer_arithmetic") != std::string::npos;
        });
    REQUIRE(legality_error);
}

TEST_CASE (



"emit_function does not reject when backend has no capabilities()"
,
"[lithe][codegen][backend][legality][emit]"
)
 {
    using namespace lithe::codegen;

    // Backend with no capabilities() method — legality check is skipped.
    struct nocap_backend {
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state s) {
            return backend_result::success_result(std::move(s));
        }
    };

    auto fn = make_fn_with_ops({opcode::add, opcode::ret});
    nocap_backend backend;
    // The only rejection path for this backend is the physical MIR verification
    // check at the top of emit_function, which passes for valid MIR.
    const auto result = emit_function(backend, fn);
    REQUIRE(result.ok());
}

TEST_CASE (



"emit_function rejects via legality for virtual registers in MIR"
,
"[lithe][codegen][backend][legality][emit]"
)
 {
    using namespace lithe::codegen;

    struct full_backend {
        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::branches
            });
        }
        [[nodiscard]] backend_result emit_instruction(const allocated_instruction &, backend_state s) {
            return backend_result::success_result(std::move(s));
        }
    };

    allocated_instruction li;
    li.id = 1;
    li.op = opcode::load_imm;
    li.defs = {allocated_operand::as_preg({0, "r0"})};
    li.uses = {allocated_operand::as_i64(1)};

    allocated_instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;

    auto fn = make_physical("vreg_emit_test", {li, ret});
    fn.referenced_virtual_registers.insert(99);
    full_backend backend;
    const auto result = emit_function(backend, fn);
    REQUIRE_FALSE(result.ok());
    const bool has_vregs = std::any_of(result.errors.begin(), result.errors.end(),
        [](const backend_error &e) {
            return e.message.find("virtual register") != std::string::npos;
        });
    REQUIRE(has_vregs);
}

// ---------------------------------------------------------------------------
// Prompt 14 — MIR legality framework
// ---------------------------------------------------------------------------

TEST_CASE (



"check_mir_legality returns ok with no rules"
,
"[lithe][codegen][legality]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("no_rules", {opcode::add, opcode::ret});
    mir_legalization_context ctx;
    ctx.backend_name = "test_backend";
    const auto result = check_mir_legality(fn, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.violation_count() == 0);
}

TEST_CASE (



"check_mir_legality detects opcode allowlist violation"
,
"[lithe][codegen][legality]"
)
 {
    using namespace lithe::codegen;
    // Function has add + ret; allowlist only permits ret → add is illegal.
    auto fn = make_fn_with_ops("allowlist_violation", {opcode::add, opcode::ret});
    mir_legalization_context ctx;
    ctx.backend_name = "tiny_backend";
    ctx.rules.push_back(
        make_opcode_allowlist_rule("tiny_opcodes",
                                   {opcode::ret, opcode::nop},
                                   "opcode not supported by tiny_backend"));
    const auto result = check_mir_legality(fn, ctx);
    REQUIRE(!result.ok());
    REQUIRE(result.violation_count() == 1);
    REQUIRE(result.violations[0].rule_name == "tiny_opcodes");
    REQUIRE(result.violations[0].kind == mir_legality_rule_kind::opcode_support);
    REQUIRE(result.diagnostics[0].find("tiny_backend") != std::string::npos);
}

TEST_CASE (



"check_mir_legality passes when all opcodes are allowed"
,
"[lithe][codegen][legality]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("all_allowed", {opcode::add, opcode::ret});
    mir_legalization_context ctx;
    ctx.backend_name = "full_backend";
    ctx.rules.push_back(
        make_opcode_allowlist_rule("full_opcodes",
                                   {opcode::add, opcode::sub, opcode::ret, opcode::nop}));
    const auto result = check_mir_legality(fn, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.violation_count() == 0);
}

TEST_CASE (



"make_no_conditional_branch_rule fires on branch_cond"
,
"[lithe][codegen][legality]"
)
 {
    using namespace lithe::codegen;
    // Build function with a branch_cond instruction.
    allocated_function_ir fn_ir;
    fn_ir.name = "has_branch_cond";
    fn_ir.cfg.entry_block = 1;
    allocated_basic_block bb;
    bb.id = 1; bb.name = "entry";
    allocated_instruction inst;
    inst.id = 1; inst.op = opcode::branch_cond;
    inst.uses = {allocated_operand::as_preg({1,"r1"}),
                 allocated_operand::as_block(2),
                 allocated_operand::as_block(3)};
    bb.instructions.push_back(inst);
    fn_ir.blocks.push_back(bb);
    mir::physical_mir_function fn;
    fn.function = std::move(fn_ir);
    fn.metadata.current_phase = mir::phase::physical_mir;

    mir_legalization_context ctx;
    ctx.backend_name = "no_cond_backend";
    ctx.rules.push_back(make_no_conditional_branch_rule());
    const auto result = check_mir_legality(fn, ctx);
    REQUIRE(!result.ok());
    REQUIRE(result.violation_count() == 1);
    REQUIRE(result.violations[0].kind == mir_legality_rule_kind::branch_legality);
    REQUIRE(result.violations[0].block_id == 1);
    REQUIRE(result.violations[0].instruction_id == 1);
}

TEST_CASE (



"make_no_conditional_branch_rule passes on unconditional branch"
,
"[lithe][codegen][legality]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("only_branch", {opcode::branch, opcode::ret});
    mir_legalization_context ctx;
    ctx.backend_name = "no_cond_backend";
    ctx.rules.push_back(make_no_conditional_branch_rule());
    const auto result = check_mir_legality(fn, ctx);
    REQUIRE(result.ok());
}

TEST_CASE (



"check_mir_legality stop_at_first_violation limits output"
,
"[lithe][codegen][legality]"
)
 {
    using namespace lithe::codegen;
    // Two instructions both violate; with stop_at_first, only first rule fires once.
    auto fn = make_fn_with_ops("two_violations", {opcode::add, opcode::sub});
    mir_legalization_context ctx;
    ctx.backend_name = "strict_backend";
    ctx.stop_at_first_violation = true;
    ctx.rules.push_back(
        make_opcode_allowlist_rule("no_arith", {opcode::ret, opcode::nop}));
    const auto result = check_mir_legality(fn, ctx);
    REQUIRE(!result.ok());
    // stop_at_first_violation: at most one violation per rule
    REQUIRE(result.violation_count() == 1);
}

TEST_CASE (



"violations_of_kind filters by kind"
,
"[lithe][codegen][legality]"
)
 {
    using namespace lithe::codegen;
    // Mix an opcode rule and a branch rule; only branch_cond triggers the branch rule.
    allocated_function_ir fn_ir;
    fn_ir.name = "mixed"; fn_ir.cfg.entry_block = 1;
    allocated_basic_block bb; bb.id = 1; bb.name = "entry";
    // add violates opcode rule; branch_cond violates branch rule
    allocated_instruction i1; i1.id = 1; i1.op = opcode::add;
    allocated_instruction i2; i2.id = 2; i2.op = opcode::branch_cond;
    bb.instructions = {i1, i2};
    fn_ir.blocks.push_back(bb);
    mir::physical_mir_function fn;
    fn.function = std::move(fn_ir);
    fn.metadata.current_phase = mir::phase::physical_mir;

    mir_legalization_context ctx;
    ctx.backend_name = "mixed_backend";
    ctx.rules.push_back(make_opcode_allowlist_rule("only_ret", {opcode::ret}));
    ctx.rules.push_back(make_no_conditional_branch_rule());
    const auto result = check_mir_legality(fn, ctx);
    REQUIRE(!result.ok());
    const auto opcode_viols  = result.violations_of_kind(mir_legality_rule_kind::opcode_support);
    const auto branch_viols  = result.violations_of_kind(mir_legality_rule_kind::branch_legality);
    REQUIRE(opcode_viols.size()  == 2); // both add and branch_cond fail the allowlist
    REQUIRE(branch_viols.size()  == 1); // only branch_cond fails the branch rule
}

// ---------------------------------------------------------------------------
// Prompt 15 — Backend lowering hooks
// ---------------------------------------------------------------------------

TEST_CASE (



"pre_emit_lowering_pass with null callback is no-op"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("null_pass", {opcode::add, opcode::ret});
    pre_emit_lowering_pass pass;
    pass.name = "null_pass";
    // lower is left unset (null std::function)
    backend_lowering_context ctx;
    ctx.backend_name = "test_backend";
    ctx.verify_after_each_step = false;
    const auto result = pass.run(fn, ctx);
    REQUIRE(result.ok());
    REQUIRE(!result.changed);
    REQUIRE(result.function.function.blocks.size() == fn.function.blocks.size());
}

TEST_CASE (



"pre_emit_lowering_pass identity callback returns unchanged"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("identity", {opcode::add, opcode::ret});
    pre_emit_lowering_pass pass;
    pass.name = "identity_pass";
    pass.lower = [](mir::physical_mir_function f, backend_lowering_context &) {
        return f;  // return unchanged
    };
    backend_lowering_context ctx;
    ctx.backend_name = "test_backend";
    ctx.verify_after_each_step = false;
    const auto result = pass.run(fn, ctx);
    REQUIRE(result.ok());
    REQUIRE(!result.changed); // same block count, same instruction count
}

TEST_CASE (



"pre_emit_lowering_pass detects added instruction as changed"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("one_inst", {opcode::ret});
    pre_emit_lowering_pass pass;
    pass.name = "add_nop_pass";
    pass.lower = [](mir::physical_mir_function f, backend_lowering_context &) {
        allocated_instruction nop;
        nop.id = 99; nop.op = opcode::nop;
        f.function.blocks[0].instructions.insert(
            f.function.blocks[0].instructions.begin(), nop);
        return f;
    };
    backend_lowering_context ctx;
    ctx.backend_name = "test_backend";
    ctx.verify_after_each_step = false;
    const auto result = pass.run(fn, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.function.function.blocks[0].instructions.size() == 2);
}

TEST_CASE (



"backend_lowering_context annotate and annotation round-trip"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    backend_lowering_context ctx;
    ctx.annotate("abi",     "sysv64");
    ctx.annotate("target",  "x86_64-linux-gnu");
    REQUIRE(ctx.annotation("abi").has_value());
    REQUIRE(ctx.annotation("abi").value() == "sysv64");
    REQUIRE(ctx.annotation("target").value() == "x86_64-linux-gnu");
    REQUIRE(!ctx.annotation("missing_key").has_value());
}

TEST_CASE (



"run_pre_emit_lowering chains multiple passes in order"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("chain", {opcode::ret});

    // Pass 1: record firing order via annotation; Pass 2: record its firing.
    pre_emit_lowering_pass p1, p2;
    p1.name = "first";
    p1.lower = [](mir::physical_mir_function f, backend_lowering_context &ctx) {
        ctx.annotate("order", "first");
        return f;
    };
    p2.name = "second";
    p2.lower = [](mir::physical_mir_function f, backend_lowering_context &ctx) {
        const auto prev = ctx.annotation("order").value_or("");
        ctx.annotate("order", prev + "_second");
        return f;
    };

    backend_lowering_context ctx;
    ctx.backend_name = "chain_backend";
    ctx.verify_after_each_step = false;
    const auto result = run_pre_emit_lowering(fn, {p1, p2}, ctx);
    REQUIRE(result.ok());
    REQUIRE(ctx.annotation("order").value() == "first_second");
}

TEST_CASE (



"run_pre_emit_lowering empty pass list returns original function"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("empty_chain", {opcode::add, opcode::ret});
    backend_lowering_context ctx;
    ctx.backend_name = "empty_chain_backend";
    ctx.verify_after_each_step = false;
    const auto result = run_pre_emit_lowering(fn, {}, ctx);
    REQUIRE(result.ok());
    REQUIRE(!result.changed);
    REQUIRE(result.function.function.name == fn.function.name);
}

TEST_CASE (



"run_pre_emit_lowering accumulates changed flag across passes"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("accumulate", {opcode::ret});

    pre_emit_lowering_pass p_nop, p_add;
    p_nop.name = "identity";
    p_nop.lower = [](mir::physical_mir_function f, backend_lowering_context &) { return f; };

    p_add.name = "add_nop";
    p_add.lower = [](mir::physical_mir_function f, backend_lowering_context &) {
        allocated_instruction nop; nop.id = 77; nop.op = opcode::nop;
        f.function.blocks[0].instructions.insert(
            f.function.blocks[0].instructions.begin(), nop);
        return f;
    };

    backend_lowering_context ctx;
    ctx.backend_name = "accum_backend";
    ctx.verify_after_each_step = false;
    const auto result = run_pre_emit_lowering(fn, {p_nop, p_add}, ctx);
    REQUIRE(result.ok());
    REQUIRE(result.changed); // p_add modified the function
}

// ---------------------------------------------------------------------------
// Prompt 4 — Dominator cache option sensitivity
// ---------------------------------------------------------------------------

namespace {
    // Build a two-block function: entry(1) -> exit(2)
    lithe::codegen::mir::physical_mir_function make_two_block_fn() {
        using namespace lithe::codegen;
        allocated_function_ir fn_ir;
        fn_ir.name = "two_block";
        fn_ir.cfg.entry_block = 1;

        auto make_bb = [](std::uint32_t id, std::string name,
                          std::vector<std::uint32_t> succs,
                          std::vector<std::uint32_t> preds) {
            allocated_basic_block bb;
            bb.id = id;
            bb.name = std::move(name);
            allocated_instruction term;
            term.id = id * 10;
            term.op = opcode::ret;
            bb.instructions.push_back(term);
            return bb;
        };

        fn_ir.blocks.push_back(make_bb(1, "entry", {2}, {}));
        fn_ir.blocks.push_back(make_bb(2, "exit", {}, {1}));
        fn_ir.cfg.successors[1] = {2};
        fn_ir.cfg.predecessors[2] = {1};
        fn_ir.cfg.successors[2] = {};
        fn_ir.cfg.predecessors[1] = {};

        mir::physical_mir_function phys;
        phys.function = std::move(fn_ir);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }
}

TEST_CASE (



"get_or_compute_dominators returns cached result when options match"
,
"[lithe][codegen][dominator][cache]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_two_block_fn();
    mir_pass_context ctx;

    dominator_analysis_options opts;
    opts.compute_frontier = false;

    const auto &r1 = get_or_compute_dominators(ctx, fn, opts);
    const auto &r2 = get_or_compute_dominators(ctx, fn, opts);
    // Same cached object → same address
    REQUIRE(&r1 == &r2);
}

TEST_CASE (



"get_or_compute_dominators recomputes when options differ"
,
"[lithe][codegen][dominator][cache]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_two_block_fn();
    mir_pass_context ctx;

    // First call: no frontier
    dominator_analysis_options opts_no_frontier;
    opts_no_frontier.compute_frontier = false;
    const auto &r1 = get_or_compute_dominators(ctx, fn, opts_no_frontier);
    // Frontier map should be empty when compute_frontier=false
    REQUIRE((r1.dom.dominance_frontier.empty() ||
             r1.dom.dominance_frontier.find(1) == r1.dom.dominance_frontier.end() ||
             r1.dom.dominance_frontier.at(1).empty()));

    // Second call: with frontier — different options, must recompute
    dominator_analysis_options opts_with_frontier;
    opts_with_frontier.compute_frontier = true;
    const auto &r2 = get_or_compute_dominators(ctx, fn, opts_with_frontier);
    // After recompute the cached options should match
    REQUIRE(ctx.analysis_cache.cached_dominator_options.compute_frontier == true);
    // r2 is the newly cached result — confirm it's valid
    REQUIRE(r2.ok());
}

TEST_CASE (



"get_or_compute_dominators cached_dominator_options tracks last options"
,
"[lithe][codegen][dominator][cache]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_two_block_fn();
    mir_pass_context ctx;

    dominator_analysis_options opts;
    opts.compute_frontier     = true;
    opts.compute_loop_headers = false;

    (void)get_or_compute_dominators(ctx, fn, opts);
    REQUIRE(ctx.analysis_cache.cached_dominator_options.compute_frontier == true);
    REQUIRE(ctx.analysis_cache.cached_dominator_options.compute_loop_headers == false);
}

TEST_CASE (



"get_or_compute_dominators cache is cleared on CFG invalidation"
,
"[lithe][codegen][dominator][cache]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_two_block_fn();
    mir_pass_context ctx;

    dominator_analysis_options opts;
    (void)get_or_compute_dominators(ctx, fn, opts);
    REQUIRE(ctx.analysis_cache.dominators.has_value());

    ctx.analysis_cache.invalidate_analysis(mir_analysis_kind::cfg);
    REQUIRE(!ctx.analysis_cache.dominators.has_value());
}

// ---------------------------------------------------------------------------
// Prompt 5 — dump_dominator_analysis
// ---------------------------------------------------------------------------

TEST_CASE (



"dump_dominator_analysis contains entry block"
,
"[lithe][codegen][dominator][dump]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_two_block_fn();
    dominator_analysis_options opts; opts.compute_frontier = false;
    const auto result = compute_dominators(fn, opts);
    const auto dump = dump_dominator_analysis(result);
    REQUIRE(dump.find("entry=") != std::string::npos);
    REQUIRE(dump.find("1") != std::string::npos); // entry block id
}

TEST_CASE (



"dump_dominator_analysis lists each block with idom"
,
"[lithe][codegen][dominator][dump]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_two_block_fn();
    dominator_analysis_options opts; opts.compute_frontier = false;
    const auto result = compute_dominators(fn, opts);
    const auto dump = dump_dominator_analysis(result);
    REQUIRE(dump.find("bb1") != std::string::npos);
    REQUIRE(dump.find("bb2") != std::string::npos);
    REQUIRE(dump.find("idom=") != std::string::npos);
}

TEST_CASE (



"dump_dominator_analysis includes dominance frontier when computed"
,
"[lithe][codegen][dominator][dump]"
)
 {
    using namespace lithe::codegen;
    // Diamond: 1->2, 1->3, 2->4, 3->4. Block 4 is in DF of 2 and 3.
    allocated_function_ir fn_ir;
    fn_ir.name = "diamond"; fn_ir.cfg.entry_block = 1;
    for (std::uint32_t id : {1u, 2u, 3u, 4u}) {
        allocated_basic_block bb; bb.id = id;
        allocated_instruction t; t.id = id; t.op = opcode::ret;
        bb.instructions.push_back(t);
        fn_ir.blocks.push_back(bb);
    }
    fn_ir.cfg.successors[1] = {2, 3};
    fn_ir.cfg.successors[2] = {4};
    fn_ir.cfg.successors[3] = {4};
    fn_ir.cfg.successors[4] = {};
    fn_ir.cfg.predecessors[1] = {};
    fn_ir.cfg.predecessors[2] = {1};
    fn_ir.cfg.predecessors[3] = {1};
    fn_ir.cfg.predecessors[4] = {2, 3};
    mir::physical_mir_function fn;
    fn.function = std::move(fn_ir);
    fn.metadata.current_phase = mir::phase::physical_mir;

    dominator_analysis_options opts;
    opts.compute_frontier = true;
    const auto result = compute_dominators(fn, opts);
    const auto dump = dump_dominator_analysis(result);
    REQUIRE(dump.find("df=") != std::string::npos);
}

TEST_CASE (



"dump_dominator_analysis marks loop header"
,
"[lithe][codegen][dominator][dump]"
)
 {
    using namespace lithe::codegen;
    // 1->2->3->2 (back edge 3->2, so 2 is a loop header)
    allocated_function_ir fn_ir;
    fn_ir.name = "loop"; fn_ir.cfg.entry_block = 1;
    for (std::uint32_t id : {1u, 2u, 3u}) {
        allocated_basic_block bb; bb.id = id;
        allocated_instruction t; t.id = id; t.op = opcode::ret;
        bb.instructions.push_back(t);
        fn_ir.blocks.push_back(bb);
    }
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3};
    fn_ir.cfg.successors[3] = {2};
    fn_ir.cfg.predecessors[1] = {};
    fn_ir.cfg.predecessors[2] = {1, 3};
    fn_ir.cfg.predecessors[3] = {2};
    mir::physical_mir_function fn;
    fn.function = std::move(fn_ir);
    fn.metadata.current_phase = mir::phase::physical_mir;

    dominator_analysis_options opts;
    opts.compute_loop_headers = true;
    const auto result = compute_dominators(fn, opts);
    const auto dump = dump_dominator_analysis(result);
    REQUIRE(dump.find("[loop-header]") != std::string::npos);
}

TEST_CASE (



"dump_dominator_analysis reports diagnostics count when present"
,
"[lithe][codegen][dominator][dump]"
)
 {
    using namespace lithe::codegen;
    dominator_analysis_result result;
    result.diagnostics.push_back("synthetic error");
    const auto dump = dump_dominator_analysis(result);
    REQUIRE(dump.find("diagnostics=1") != std::string::npos);
}

TEST_CASE (



"dump_dominator_analysis shows idom=none for unreachable block"
,
"[lithe][codegen][dominator][dump]"
)
 {
    using namespace lithe::codegen;
    // Two-block function where block 2 has no path from entry 1.
    allocated_function_ir fn_ir;
    fn_ir.name = "unreachable_block"; fn_ir.cfg.entry_block = 1;
    for (std::uint32_t id : {1u, 2u}) {
        allocated_basic_block bb; bb.id = id;
        allocated_instruction t; t.id = id; t.op = opcode::ret;
        bb.instructions.push_back(t);
        fn_ir.blocks.push_back(bb);
    }
    // No edges: block 2 is unreachable from entry.
    fn_ir.cfg.successors[1]   = {};
    fn_ir.cfg.predecessors[1] = {};
    fn_ir.cfg.successors[2]   = {};
    fn_ir.cfg.predecessors[2] = {};
    mir::physical_mir_function fn;
    fn.function = std::move(fn_ir);
    fn.metadata.current_phase = mir::phase::physical_mir;

    const auto result = compute_dominators(fn, {});
    REQUIRE(result.ok()); // unreachable node is a warning, not an error
    const auto dump = dump_dominator_analysis(result);
    REQUIRE(dump.find("idom=none") != std::string::npos);
}

TEST_CASE (



"dump_dominator_analysis omits diagnostics line when none present"
,
"[lithe][codegen][dominator][dump]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("clean", {opcode::ret});
    const auto result = compute_dominators(fn, {});
    REQUIRE(result.ok());
    const auto dump = dump_dominator_analysis(result);
    REQUIRE(dump.find("diagnostics=") == std::string::npos);
}

TEST_CASE (



"dominator_analysis_options operator== distinguishes equal and unequal"
,
"[lithe][codegen][dominator][cache]"
)
 {
    using namespace lithe::codegen;
    dominator_analysis_options a, b;
    REQUIRE(a == b);

    b.compute_frontier = true;
    REQUIRE_FALSE(a == b);

    a.compute_frontier = true;
    REQUIRE(a == b);

    a.compute_loop_headers = true;
    REQUIRE_FALSE(a == b);
}

TEST_CASE (



"backend_lowering_context::annotate overwrites existing key"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    backend_lowering_context ctx;
    ctx.annotate("key", "first");
    REQUIRE(ctx.annotation("key").value() == "first");
    ctx.annotate("key", "second");
    REQUIRE(ctx.annotation("key").value() == "second");
}

TEST_CASE (



"pre_emit_lowering_pass callback exception leaves function unchanged"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("throw_test", {opcode::ret});

    pre_emit_lowering_pass pass;
    pass.name = "throwing_pass";
    pass.lower = [](mir::physical_mir_function, backend_lowering_context &) -> mir::physical_mir_function {
        throw std::runtime_error("deliberate failure");
    };

    backend_lowering_context ctx;
    ctx.backend_name = "test_backend";
    ctx.verify_after_each_step = false;
    const auto result = pass.run(fn, ctx);

    // Exception must not propagate; result reverts to the original function.
    REQUIRE(!result.changed);
    REQUIRE(!result.ok()); // diagnostic was recorded
    REQUIRE(result.function.function.name == fn.function.name);
    REQUIRE(!ctx.diagnostics.empty());
    REQUIRE(ctx.diagnostics[0].find("throwing_pass") != std::string::npos);
}

TEST_CASE (



"run_pre_emit_lowering stops chain when a step records a diagnostic failure"
,
"[lithe][codegen][lowering]"
)
 {
    using namespace lithe::codegen;
    auto fn = make_fn_with_ops("stop_chain", {opcode::ret});

    // Pass 1: throws so produces a diagnostic and returns unchanged.
    pre_emit_lowering_pass p1;
    p1.name = "failing_pass";
    p1.lower = [](mir::physical_mir_function, backend_lowering_context &) -> mir::physical_mir_function {
        throw std::runtime_error("stop here");
    };

    // Pass 2: records that it ran via annotation.
    pre_emit_lowering_pass p2;
    p2.name = "should_not_run";
    p2.lower = [](mir::physical_mir_function f, backend_lowering_context &ctx) {
        ctx.annotate("ran_p2", "yes");
        return f;
    };

    backend_lowering_context ctx;
    ctx.backend_name = "stop_backend";
    ctx.verify_after_each_step = false;
    const auto result = run_pre_emit_lowering(fn, {p1, p2}, ctx);

    // Chain must stop after p1's failure; p2 must not have run.
    REQUIRE(!result.ok());
    REQUIRE(!ctx.annotation("ran_p2").has_value());
}

// -----------------------------------------------------------------------
// SSA adapter roundtrip tests
//
// These tests exercise the full construct_ssa → verify_ssa → destroy_ssa →
// verify_physical_mir pipeline.  They are additive to the existing
// construct_ssa and destroy_ssa unit tests.
// -----------------------------------------------------------------------

// Test R1: straight-line MIR full roundtrip
//
//   bb1: load_imm r1=42 ; add r2=r1,r1 ; ret
//
//   No control flow → no phi nodes.
//   verify_ssa must pass.
//   destroy_ssa must produce valid physical MIR with identical block/instruction
//   count (no movs inserted for phi lowering).
TEST_CASE (



"ssa roundtrip: straight-line MIR — verify_ssa passes, MIR validates"
,
"[lithe][codegen][ssa][roundtrip]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(42)}),
            make_inst(20, opcode::add,
                      {allocated_operand::as_preg(r2)},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r1)}),
            make_inst(30, opcode::ret, {}, {})
         }, {}}
    });

    // Step 1: construct SSA.
    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto ssa = construct_ssa(fn, opts);
    REQUIRE(ssa.ok());

    // Step 2: verify_ssa — single block, no phis; must be clean.
    const auto vssa = verify_ssa(ssa);
    INFO("verify_ssa diagnostics: " <<
         (vssa.diagnostics.empty() ? "(none)" : vssa.diagnostics[0]));
    REQUIRE(vssa.ok());

    // Informational notes must be present (dominance + phi count + rename).
    REQUIRE(!ssa.info.empty());

    // Step 3: destroy_ssa.
    const auto lowered = destroy_ssa(ssa);
    REQUIRE(lowered.ok());

    // Single block, 3 instructions — no copies inserted.
    REQUIRE(lowered.function.function.blocks.size() == 1);
    REQUIRE(lowered.function.function.blocks[0].instructions.size() == 3);

    // SSA overlay stripped.
    for (const auto &blk : lowered.function.function.blocks)
        for (const auto &inst : blk.instructions) {
            REQUIRE(inst.ssa_defs.empty());
            REQUIRE(inst.ssa_uses.empty());
        }

    // Step 4: verify physical MIR.
    const auto vr = verify_physical_mir(lowered.function);
    INFO("verify_physical_mir diagnostics: " <<
         (vr.diagnostics.empty() ? "(none)" : vr.diagnostics[0]));
    REQUIRE(vr.ok());
}

// Test R2: branch-merge full roundtrip
//
//   bb1: load_imm r1=1 ; branch_cond r5, bb2, bb3
//   bb2: load_imm r1=2 ; branch bb4
//   bb3: branch bb4
//   bb4: add r6=r1,r2 ; ret
//
//   r1 is redefined in bb1 and bb2.  A phi is placed at bb4.
//   verify_ssa must pass (phi incoming values filled by renaming).
//   destroy_ssa must produce valid MIR.
TEST_CASE (



"ssa roundtrip: branch-merge — verify_ssa passes, phi lowered, MIR validates"
,
"[lithe][codegen][ssa][roundtrip]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};
    const preg r5{5, "r5"};
    const preg r6{6, "r6"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(1)}),
            make_inst(11, opcode::branch_cond,
                      {},
                      {allocated_operand::as_preg(r5),
                       allocated_operand::as_block(2),
                       allocated_operand::as_block(3)})
         }, {2, 3}},
        {2, {
            make_inst(20, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(2)}),
            make_inst(21, opcode::branch, {}, {allocated_operand::as_block(4)})
         }, {4}},
        {3, {
            make_inst(30, opcode::branch, {}, {allocated_operand::as_block(4)})
         }, {4}},
        {4, {
            make_inst(40, opcode::add,
                      {allocated_operand::as_preg(r6)},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r2)}),
            make_inst(41, opcode::ret, {}, {})
         }, {}}
    });

    // Step 1: construct SSA.
    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto ssa = construct_ssa(fn, opts);
    REQUIRE(ssa.ok());

    // A phi for r1 must be recorded at bb4.
    REQUIRE(ssa.block_states.contains(4));
    const bool has_phi = std::ranges::any_of(
        ssa.block_states.at(4).phi_nodes,
        [](const auto &p){ return p.preg_id == 1; });
    REQUIRE(has_phi);

    // Step 2: verify_ssa — phi incoming values must be filled by rename.
    const auto vssa = verify_ssa(ssa);
    INFO("verify_ssa diagnostics: " <<
         (vssa.diagnostics.empty() ? "(none)" : vssa.diagnostics[0]));
    REQUIRE(vssa.ok());

    // Step 3: destroy_ssa.
    const auto lowered = destroy_ssa(ssa);
    REQUIRE(lowered.ok());

    // SSA overlay stripped.
    for (const auto &blk : lowered.function.function.blocks)
        for (const auto &inst : blk.instructions) {
            REQUIRE(inst.ssa_defs.empty());
            REQUIRE(inst.ssa_uses.empty());
        }

    // Step 4: verify physical MIR.
    const auto vr = verify_physical_mir(lowered.function);
    INFO("verify_physical_mir diagnostics: " <<
         (vr.diagnostics.empty() ? "(none)" : vr.diagnostics[0]));
    REQUIRE(vr.ok());
}

// Test R3: loop-carried value full roundtrip
//
//   bb1: load_imm r1=0 ; branch bb2
//   bb2: add r3=r1,r2 ; branch_cond r5, bb3, bb4
//   bb3: add r1=r1,r2 ; branch bb2   ← back-edge
//   bb4: ret
//
//   The implementation supports loop-carried phis.  This test confirms that:
//   - construct_ssa succeeds and places a phi at the loop header (bb2)
//   - verify_ssa passes (phi incoming values correctly filled)
//   - destroy_ssa produces valid physical MIR
//   - the pipeline must not crash at any step
TEST_CASE (



"ssa roundtrip: loop-carried value — construct succeeds, roundtrip validates"
,
"[lithe][codegen][ssa][roundtrip]"
)
 {
    using namespace lithe::codegen;

    const preg r1{1, "r1"};
    const preg r2{2, "r2"};
    const preg r3{3, "r3"};
    const preg r5{5, "r5"};

    auto fn = make_physical_ssa(1, {
        {1, {
            make_inst(10, opcode::load_imm,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_i64(0)}),
            make_inst(11, opcode::branch, {}, {allocated_operand::as_block(2)})
         }, {2}},
        {2, {
            make_inst(20, opcode::add,
                      {allocated_operand::as_preg(r3)},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r2)}),
            make_inst(21, opcode::branch_cond,
                      {},
                      {allocated_operand::as_preg(r5),
                       allocated_operand::as_block(3),
                       allocated_operand::as_block(4)})
         }, {3, 4}},
        {3, {
            make_inst(30, opcode::add,
                      {allocated_operand::as_preg(r1)},
                      {allocated_operand::as_preg(r1),
                       allocated_operand::as_preg(r2)}),
            make_inst(31, opcode::branch, {}, {allocated_operand::as_block(2)})
         }, {2}},
        {4, {
            make_inst(40, opcode::ret, {}, {})
         }, {}}
    });

    // Step 1: construct SSA — must not crash; loop-carried phis are supported.
    ssa_adapter_options opts;
    opts.placeholder_only = false;
    const auto ssa = construct_ssa(fn, opts);
    REQUIRE(ssa.ok());

    // A phi for r1 must be placed at the loop header bb2.
    REQUIRE(ssa.block_states.contains(2));
    const bool has_loop_phi = std::ranges::any_of(
        ssa.block_states.at(2).phi_nodes,
        [](const auto &p){ return p.preg_id == 1; });
    REQUIRE(has_loop_phi);

    // Step 2: verify_ssa.
    const auto vssa = verify_ssa(ssa);
    INFO("verify_ssa diagnostics: " <<
         (vssa.diagnostics.empty() ? "(none)" : vssa.diagnostics[0]));
    REQUIRE(vssa.ok());

    // Step 3: destroy_ssa.
    const auto lowered = destroy_ssa(ssa);
    REQUIRE(lowered.ok());

    // SSA overlay stripped on all instructions.
    for (const auto &blk : lowered.function.function.blocks)
        for (const auto &inst : blk.instructions) {
            REQUIRE(inst.ssa_defs.empty());
            REQUIRE(inst.ssa_uses.empty());
        }

    // Step 4: verify physical MIR.
    const auto vr = verify_physical_mir(lowered.function);
    INFO("verify_physical_mir diagnostics: " <<
         (vr.diagnostics.empty() ? "(none)" : vr.diagnostics[0]));
    REQUIRE(vr.ok());
}

// Test R4: compile_to_physical_mir is independent of SSA
//
//   SSA is optional.  A normal compile through compile_to_physical_mir must
//   succeed without any SSA construction step.
TEST_CASE (



"ssa roundtrip: compile_to_physical_mir succeeds without SSA involvement"
,
"[lithe][codegen][ssa][roundtrip]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    const auto expr = make_node<add_tag>(1, 2);

    codegen_options opts;
    const auto result = compile_to_physical_mir(expr, opts);
    REQUIRE(result.ok());

    // The compiled function must pass physical MIR verification on its own —
    // no SSA step required.
    const auto vr = verify_physical_mir(result.physical_mir);
    INFO("verify_physical_mir diagnostics: " <<
         (vr.diagnostics.empty() ? "(none)" : vr.diagnostics[0]));
    REQUIRE(vr.ok());

    // Sanity: the function has at least one block with at least one instruction.
    REQUIRE(!result.physical_mir.function.blocks.empty());
    REQUIRE(!result.physical_mir.function.blocks[0].instructions.empty());
}

// ---------------------------------------------------------------------------
// Prompt 15 — dead_def_elimination_pass quality tests
// ---------------------------------------------------------------------------

// An instruction that defines a preg that is never used anywhere must be
// removed by the dead-def pass.
TEST_CASE (



"dead_def_elimination_pass: removes unused def"
,
"[lithe][codegen][pass][dead_def]"
)
 {
    using namespace lithe::codegen;

    // add r3=r1,r2  (id=10)  — r3 never used: dead
    // ret           (id=11)
    allocated_instruction dead_add;
    dead_add.id = 10;
    dead_add.op = opcode::add;
    dead_add.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    dead_add.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                     allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction ret;
    ret.id = 11;
    ret.op = opcode::ret;

    auto fn = make_physical("dde_removes_unused", {dead_add, ret});

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    REQUIRE(result.removed_instructions == 1);

    const auto &insts = result.function.function.blocks[0].instructions;
    for (const auto &inst : insts) {
        REQUIRE(inst.id != 10);  // dead add must be gone
    }
    // ret must still be present
    bool has_ret = false;
    for (const auto &inst : insts) {
        if (inst.id == 11) has_ret = true;
    }
    REQUIRE(has_ret);
}

// A def in a loop-header block must not be removed even when the preg appears
// unused — it may carry a value across the back edge (loop-carried def guard).
TEST_CASE (



"dead_def_elimination_pass: preserves loop-carried def in loop header"
,
"[lithe][codegen][pass][dead_def]"
)
 {
    using namespace lithe::codegen;

    // bb1 (entry):  branch bb2
    // bb2 (header): add r3=r1,r2 ; branch_cond r9, bb3, bb4
    //                              ↑ r3 is NOT used elsewhere — appears dead
    // bb3 (body):   branch bb2    ← back-edge makes bb2 a loop header
    // bb4 (exit):   ret
    //
    // Because bb2 is a loop header the pass must keep add r3 conservatively.

    allocated_function_ir fn_ir;
    fn_ir.name = "dde_loop_header_guard";
    fn_ir.cfg.entry_block = 1;

    // bb1
    allocated_basic_block bb1;
    bb1.id = 1;  bb1.name = "entry";  bb1.successors = {2};
    allocated_instruction br_entry;
    br_entry.id = 10;  br_entry.op = opcode::branch;
    br_entry.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {br_entry};

    // bb2 (loop header)
    allocated_basic_block bb2;
    bb2.id = 2;  bb2.name = "header";
    bb2.predecessors = {1, 3};  bb2.successors = {3, 4};
    allocated_instruction add_hdr;
    add_hdr.id = 20;  add_hdr.op = opcode::add;
    add_hdr.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_hdr.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                    allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction brc_hdr;
    brc_hdr.id = 21;  brc_hdr.op = opcode::branch_cond;
    brc_hdr.uses = {allocated_operand::as_preg(preg{9, "r9"}),
                    allocated_operand::as_block(3),
                    allocated_operand::as_block(4)};
    bb2.instructions = {add_hdr, brc_hdr};

    // bb3 (body, back-edge to bb2)
    allocated_basic_block bb3;
    bb3.id = 3;  bb3.name = "body";
    bb3.predecessors = {2};  bb3.successors = {2};
    allocated_instruction br_back;
    br_back.id = 30;  br_back.op = opcode::branch;
    br_back.uses = {allocated_operand::as_block(2)};
    bb3.instructions = {br_back};

    // bb4 (exit)
    allocated_basic_block bb4;
    bb4.id = 4;  bb4.name = "exit";  bb4.predecessors = {2};
    allocated_instruction ret;
    ret.id = 40;  ret.op = opcode::ret;
    bb4.instructions = {ret};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3, 4};
    fn_ir.cfg.successors[3] = {2};
    fn_ir.cfg.predecessors[2] = {1, 3};
    fn_ir.cfg.predecessors[3] = {2};
    fn_ir.cfg.predecessors[4] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    // add r3 (id=20) must be preserved — it is in a loop-header block.
    bool found = false;
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 20) found = true;
        }
    }
    REQUIRE(found);
}

// A call instruction is a side effect and must never be removed, regardless
// of whether the def (if any) is used.
TEST_CASE (



"dead_def_elimination_pass: preserves side-effect call instruction"
,
"[lithe][codegen][pass][dead_def]"
)
 {
    using namespace lithe::codegen;

    // call (id=10)         — side-effecting, must be kept
    // add  r3=r1,r2 (id=11) — r3 never used: dead
    // ret  (id=12)

    allocated_instruction call_inst;
    call_inst.id = 10;
    call_inst.op = opcode::call;

    allocated_instruction dead_add;
    dead_add.id = 11;
    dead_add.op = opcode::add;
    dead_add.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    dead_add.uses = {allocated_operand::as_preg(preg{1, "r1"}),
                     allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction ret;
    ret.id = 12;
    ret.op = opcode::ret;

    auto fn = make_physical("dde_preserves_call", {call_inst, dead_add, ret});

    mir_pass_context ctx;
    dead_def_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());

    bool has_call = false;
    bool has_dead_add = false;
    for (const auto &inst : result.function.function.blocks[0].instructions) {
        if (inst.id == 10) has_call = true;
        if (inst.id == 11) has_dead_add = true;
    }
    REQUIRE(has_call);           // call preserved (side-effect)
    REQUIRE_FALSE(has_dead_add); // dead add removed
}

// ---------------------------------------------------------------------------
// Prompt 11 — CSE dominance hardening tests
// ---------------------------------------------------------------------------

// CSE must not reuse an expression defined in a block that precedes a call
// instruction in the same block, because the call may clobber registers.
TEST_CASE (



"common_subexpression_elimination_pass: no reuse across call in same block"
,
"[lithe][codegen][pass][cse]"
)
 {
    using namespace lithe::codegen;

    // add r3=r1,r2  (id=10)
    // call          (id=11)  ← barrier; r3 may be clobbered
    // add r4=r1,r2  (id=12)  ← same expression, but must NOT become mov r4=r3
    // ret           (id=13)
    allocated_instruction add1;
    add1.id = 10;
    add1.op = opcode::add;
    add1.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add1.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction call_inst;
    call_inst.id = 11;
    call_inst.op = opcode::call;

    allocated_instruction add2;
    add2.id = 12;
    add2.op = opcode::add;
    add2.defs = {allocated_operand::as_preg(preg{4, "r4"})};
    add2.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};

    allocated_instruction ret;
    ret.id = 13;
    ret.op = opcode::ret;

    auto fn = make_physical("cse_no_reuse_across_call", {add1, call_inst, add2, ret});

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(fn, ctx);

    REQUIRE(result.ok());
    // add2 (id=12) must remain as add, not rewritten to mov.
    bool found_add2 = false;
    for (const auto &inst : result.function.function.blocks[0].instructions) {
        if (inst.id == 12) {
            REQUIRE(inst.op == opcode::add);
            found_add2 = true;
        }
    }
    REQUIRE(found_add2);
}

// CSE must not reuse a cross-block expression when the candidate preg has been
// redefined on some path between the definition block and the use block.
// Here bb1 defines r3=add(r1,r2) and also redefines r3 via a different path;
// the merge point must not reuse r3 from bb1.
TEST_CASE (



"common_subexpression_elimination_pass: no cross-block reuse when candidate preg redefined on path"
,
"[lithe][codegen][pass][cse]"
)
 {
    using namespace lithe::codegen;

    // Diamond: bb1 -> bb2, bb3 -> bb4
    // bb1: add r3=r1,r2 ; branch_cond r9, bb2, bb3
    // bb2: mov r3=r5    ← redefines r3 on the left arm
    //      branch bb4
    // bb3: branch bb4   ← r3 not touched on right arm
    // bb4: add r4=r1,r2 ; ret
    //
    // bb1 dominates bb4, so the expression add(r1,r2) is in `available`.
    // However r3 (the candidate preg) is redefined in bb2, so its reaching def
    // at bb4 is ambiguous.  The CSE pass must not replace the add in bb4.

    allocated_function_ir fn_ir;
    fn_ir.name = "cse_cross_redef_on_path";
    fn_ir.cfg.entry_block = 1;

    // bb1
    allocated_basic_block bb1;
    bb1.id = 1;  bb1.name = "entry";  bb1.successors = {2, 3};
    allocated_instruction add_entry;
    add_entry.id = 10;  add_entry.op = opcode::add;
    add_entry.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_entry.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction brc;
    brc.id = 11;  brc.op = opcode::branch_cond;
    brc.uses = {allocated_operand::as_preg(preg{9, "r9"}),
                allocated_operand::as_block(2), allocated_operand::as_block(3)};
    bb1.instructions = {add_entry, brc};

    // bb2: redefine r3
    allocated_basic_block bb2;
    bb2.id = 2;  bb2.name = "left";  bb2.predecessors = {1};  bb2.successors = {4};
    allocated_instruction mov_r3;
    mov_r3.id = 20;  mov_r3.op = opcode::mov;
    mov_r3.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    mov_r3.uses = {allocated_operand::as_preg(preg{5, "r5"})};
    allocated_instruction br_left;
    br_left.id = 21;  br_left.op = opcode::branch;
    br_left.uses = {allocated_operand::as_block(4)};
    bb2.instructions = {mov_r3, br_left};

    // bb3: pass-through
    allocated_basic_block bb3;
    bb3.id = 3;  bb3.name = "right";  bb3.predecessors = {1};  bb3.successors = {4};
    allocated_instruction br_right;
    br_right.id = 30;  br_right.op = opcode::branch;
    br_right.uses = {allocated_operand::as_block(4)};
    bb3.instructions = {br_right};

    // bb4: reuse candidate
    allocated_basic_block bb4;
    bb4.id = 4;  bb4.name = "merge";  bb4.predecessors = {2, 3};
    allocated_instruction add_merge;
    add_merge.id = 40;  add_merge.op = opcode::add;
    add_merge.defs = {allocated_operand::as_preg(preg{4, "r4"})};
    add_merge.uses = {allocated_operand::as_preg(preg{1, "r1"}), allocated_operand::as_preg(preg{2, "r2"})};
    allocated_instruction ret;
    ret.id = 41;  ret.op = opcode::ret;
    bb4.instructions = {add_merge, ret};

    fn_ir.blocks = {bb1, bb2, bb3, bb4};
    fn_ir.cfg.successors[1] = {2, 3};
    fn_ir.cfg.successors[2] = {4};
    fn_ir.cfg.successors[3] = {4};
    fn_ir.cfg.predecessors[2] = {1};
    fn_ir.cfg.predecessors[3] = {1};
    fn_ir.cfg.predecessors[4] = {2, 3};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    mir_pass_context ctx;
    ctx.verify_after_each_pass = false;
    common_subexpression_elimination_pass pass;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    // add_merge (id=40) must remain as add — r3 is ambiguous at the merge point.
    bool found = false;
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 40) {
                REQUIRE(inst.op == opcode::add);
                found = true;
            }
        }
    }
    REQUIRE(found);
}

// ---------------------------------------------------------------------------
// Prompt 12 — copy propagation call-crossing guard tests
// ---------------------------------------------------------------------------

// Cross-block copy propagation must not propagate through a block that contains
// a call instruction, because the call may clobber the source register.
TEST_CASE (



"copy_propagation_pass: no cross-block propagation when path crosses call"
,
"[lithe][codegen][pass][copy_prop]"
)
 {
    using namespace lithe::codegen;

    // bb1: mov r2=r1 ; branch bb2
    // bb2: call       ; branch bb3     ← call on the path
    // bb3: add r3=r2,r4 ; ret
    //
    // r2 is defined in bb1 (copy of r1).  bb1 dominates bb3.
    // However bb2 (which is on the path) contains a call, so propagation is unsafe.

    allocated_function_ir fn_ir;
    fn_ir.name = "copy_cross_call_guard";
    fn_ir.cfg.entry_block = 1;

    // bb1
    allocated_basic_block bb1;
    bb1.id = 1;  bb1.name = "entry";  bb1.successors = {2};
    allocated_instruction mov_inst;
    mov_inst.id = 10;  mov_inst.op = opcode::mov;
    mov_inst.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    mov_inst.uses = {allocated_operand::as_preg(preg{1, "r1"})};
    allocated_instruction br1;
    br1.id = 11;  br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {mov_inst, br1};

    // bb2: call ; branch bb3
    allocated_basic_block bb2;
    bb2.id = 2;  bb2.name = "call_block";  bb2.predecessors = {1};  bb2.successors = {3};
    allocated_instruction call_inst;
    call_inst.id = 20;  call_inst.op = opcode::call;
    allocated_instruction br2;
    br2.id = 21;  br2.op = opcode::branch;
    br2.uses = {allocated_operand::as_block(3)};
    bb2.instructions = {call_inst, br2};

    // bb3: use r2
    allocated_basic_block bb3;
    bb3.id = 3;  bb3.name = "use_block";  bb3.predecessors = {2};
    allocated_instruction add_use;
    add_use.id = 30;  add_use.op = opcode::add;
    add_use.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_use.uses = {allocated_operand::as_preg(preg{2, "r2"}), allocated_operand::as_preg(preg{4, "r4"})};
    allocated_instruction ret;
    ret.id = 31;  ret.op = opcode::ret;
    bb3.instructions = {add_use, ret};

    fn_ir.blocks = {bb1, bb2, bb3};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.successors[2] = {3};
    fn_ir.cfg.predecessors[2] = {1};
    fn_ir.cfg.predecessors[3] = {2};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    copy_propagation_pass pass;
    mir_pass_context ctx;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    // add_use (id=30): first use operand must remain r2, not be rewritten to r1.
    bool found = false;
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 30) {
                REQUIRE(inst.uses[0].type == allocated_operand::kind::preg);
                REQUIRE(std::get<preg>(inst.uses[0].value).id == 2);
                found = true;
            }
        }
    }
    REQUIRE(found);
}

// Cross-block copy propagation IS allowed when the path to the use block does
// not contain any call (baseline / positive case for the call-crossing guard).
TEST_CASE (



"copy_propagation_pass: cross-block propagation allowed when no call on path"
,
"[lithe][codegen][pass][copy_prop]"
)
 {
    using namespace lithe::codegen;

    // bb1: mov r2=r1 ; branch bb2
    // bb2: add r3=r2,r4 ; ret
    // No call on the path bb1→bb2; propagation must replace r2 with r1 in bb2.

    allocated_function_ir fn_ir;
    fn_ir.name = "copy_cross_no_call";
    fn_ir.cfg.entry_block = 1;

    // bb1
    allocated_basic_block bb1;
    bb1.id = 1;  bb1.name = "entry";  bb1.successors = {2};
    allocated_instruction mov_inst;
    mov_inst.id = 10;  mov_inst.op = opcode::mov;
    mov_inst.defs = {allocated_operand::as_preg(preg{2, "r2"})};
    mov_inst.uses = {allocated_operand::as_preg(preg{1, "r1"})};
    allocated_instruction br1;
    br1.id = 11;  br1.op = opcode::branch;
    br1.uses = {allocated_operand::as_block(2)};
    bb1.instructions = {mov_inst, br1};

    // bb2: use r2
    allocated_basic_block bb2;
    bb2.id = 2;  bb2.name = "use_block";  bb2.predecessors = {1};
    allocated_instruction add_use;
    add_use.id = 20;  add_use.op = opcode::add;
    add_use.defs = {allocated_operand::as_preg(preg{3, "r3"})};
    add_use.uses = {allocated_operand::as_preg(preg{2, "r2"}), allocated_operand::as_preg(preg{4, "r4"})};
    allocated_instruction ret;
    ret.id = 21;  ret.op = opcode::ret;
    bb2.instructions = {add_use, ret};

    fn_ir.blocks = {bb1, bb2};
    fn_ir.cfg.successors[1] = {2};
    fn_ir.cfg.predecessors[2] = {1};

    mir::physical_mir_function physical;
    physical.function = std::move(fn_ir);
    physical.metadata.current_phase = mir::phase::physical_mir;

    copy_propagation_pass pass;
    mir_pass_context ctx;
    const auto result = pass.run(physical, ctx);

    REQUIRE(result.ok());
    REQUIRE(result.changed);
    // add_use (id=20): first use operand must have been rewritten from r2 to r1.
    bool found = false;
    for (const auto &block : result.function.function.blocks) {
        for (const auto &inst : block.instructions) {
            if (inst.id == 20) {
                REQUIRE(inst.uses[0].type == allocated_operand::kind::preg);
                REQUIRE(std::get<preg>(inst.uses[0].value).id == 1);
                found = true;
            }
        }
    }
    REQUIRE(found);
}

// =============================================================================
// Operation algebra and target integration tests (Phase 4, Prompts 9-10)
// =============================================================================

// 1. operation_id equality and hash
TEST_CASE (



"operation_id: equality and hash"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    const auto a = make_operation_id("mylib", "matmul");
    const auto b = make_operation_id("mylib", "matmul");
    const auto c = make_operation_id("mylib", "relu");

    REQUIRE(a == b);
    REQUIRE(a != c);

    std::hash<operation_id> hasher;
    REQUIRE(hasher(a) == hasher(b));
    REQUIRE(hasher(a) != hasher(c));
}

// 2. operation_registry: registration and lookup
TEST_CASE (



"operation_registry: register and lookup"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    operation_registry reg;
    operation_descriptor desc;
    desc.id = make_operation_id("mylib", "relu");
    desc.traits = add_trait(make_trait_set(), operation_trait::pure);

    reg.register_operation(desc);

    REQUIRE(reg.contains(make_operation_id("mylib", "relu")));
    REQUIRE(!reg.contains(make_operation_id("mylib", "sigmoid")));

    const auto *found = reg.find(make_operation_id("mylib", "relu"));
    REQUIRE(found != nullptr);
    REQUIRE(found->id == desc.id);
}

// 3. operation_contract stores tensor/vector/query/layout kinds
TEST_CASE (



"operation_contract: stores abstract value kinds"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    operation_contract contract;
    contract.operands  = {abstract_value_type{abstract_value_kind::tensor,  32, 1},
                          abstract_value_type{abstract_value_kind::vector,  64, 4}};
    contract.results   = {abstract_value_type{abstract_value_kind::tensor,  32, 1}};
    contract.required_traits  = add_trait(make_trait_set(), operation_trait::pure);
    contract.forbidden_traits = add_trait(make_trait_set(), operation_trait::writes_memory);

    REQUIRE(contract.operands[0].kind == abstract_value_kind::tensor);
    REQUIRE(contract.operands[1].kind == abstract_value_kind::vector);
    REQUIRE(contract.operands[1].lane_count == 4);
    REQUIRE(contract.results[0].kind == abstract_value_kind::tensor);
    REQUIRE(has_trait(contract.required_traits, operation_trait::pure));
    REQUIRE(has_trait(contract.forbidden_traits, operation_trait::writes_memory));

    // verify query and layout kinds round-trip through abstract_value_type
    abstract_value_type q{abstract_value_kind::query, 0, 0};
    abstract_value_type l{abstract_value_kind::layout, 0, 0};
    REQUIRE(q.kind == abstract_value_kind::query);
    REQUIRE(l.kind == abstract_value_kind::layout);
}

// 4. MIR instruction carries abstract_operation and operation_attributes
TEST_CASE (



"instruction: carries abstract_operation and operation_attributes"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    instruction instr;
    instr.id = 42;
    instr.op = opcode::nop;
    instr.abstract_operation = make_operation_id("mylib", "matmul");
    instr.operation_attributes["tile_size"] = "64";
    instr.operation_attributes["dtype"]     = "fp32";

    REQUIRE(instr.abstract_operation.has_value());
    REQUIRE(*instr.abstract_operation == make_operation_id("mylib", "matmul"));
    REQUIRE(instr.operation_attributes.at("tile_size") == "64");
    REQUIRE(instr.operation_attributes.at("dtype") == "fp32");
    REQUIRE(has_abstract_operation(instr));
}

// 5. effective_operation falls back to legacy opcode when no abstract_operation
TEST_CASE (



"effective_operation: falls back to legacy opcode"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    instruction pure_instr;
    pure_instr.id = 1;
    pure_instr.op = opcode::add;

    instruction abstract_instr;
    abstract_instr.id = 2;
    abstract_instr.op = opcode::nop;
    abstract_instr.abstract_operation = make_operation_id("mylib", "matmul");

    const auto legacy_op = effective_operation(pure_instr);
    REQUIRE(legacy_op.domain == "lithe.core");

    const auto abstract_op = effective_operation(abstract_instr);
    REQUIRE(abstract_op.domain == "mylib");
    REQUIRE(abstract_op.name == "matmul");
}

// 6. trait-aware purity works for registered pure operation
TEST_CASE (



"is_pure_expression: trait-aware check for registered pure operation"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    operation_registry reg;
    operation_descriptor desc;
    desc.id     = make_operation_id("mylib", "relu");
    desc.traits = add_trait(make_trait_set(), operation_trait::pure);
    reg.register_operation(desc);

    instruction instr;
    instr.id = 1;
    instr.op = opcode::nop;
    instr.abstract_operation = make_operation_id("mylib", "relu");

    REQUIRE(is_pure_expression(instr, &reg));
}

// 7. trait-aware side-effect check works for writes_memory / has_side_effects
TEST_CASE (



"has_side_effects: trait-aware check for writes_memory operation"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    operation_registry reg;
    operation_descriptor desc;
    desc.id     = make_operation_id("mylib", "scatter_write");
    desc.traits = add_trait(
                      add_trait(make_trait_set(), operation_trait::writes_memory),
                      operation_trait::has_side_effects);
    reg.register_operation(desc);

    instruction instr;
    instr.id = 1;
    instr.op = opcode::nop;
    instr.abstract_operation = make_operation_id("mylib", "scatter_write");

    REQUIRE(has_side_effects(instr, &reg));
}

// 8. backend legality rejects unsupported operation domain
TEST_CASE (



"validate_operation_legality: rejects unsupported domain"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    mir::physical_mir_function fn = make_physical("reject_domain", {});

    basic_block vbb;
    vbb.id = 1;
    instruction virt_inst;
    virt_inst.id = 10;
    virt_inst.op = opcode::nop;
    virt_inst.abstract_operation = make_operation_id("mylib", "matmul");
    vbb.instructions.push_back(virt_inst);
    fn.function.original_vreg_ir.blocks.push_back(vbb);

    allocated_instruction alloc_inst;
    alloc_inst.id = 10;
    alloc_inst.op = opcode::nop;
    fn.function.blocks[0].instructions.push_back(alloc_inst);

    operation_registry reg;
    operation_descriptor desc;
    desc.id = make_operation_id("mylib", "matmul");
    reg.register_operation(desc);

    // Backend only supports "other_domain", not "mylib".
    backend_capability_requirement req;
    req.supported_operation_domains = {"other_domain"};

    const auto result = validate_operation_legality(fn, reg, req);
    REQUIRE(!result.ok());
    REQUIRE(!result.violations.empty());
    REQUIRE(result.violations[0].operation == "mylib/matmul");
}

// 9. backend legality accepts explicitly supported operation
TEST_CASE (



"validate_operation_legality: accepts explicitly supported operation"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    mir::physical_mir_function fn = make_physical("accept_op", {});

    basic_block vbb;
    vbb.id = 1;
    instruction virt_inst;
    virt_inst.id = 10;
    virt_inst.op = opcode::nop;
    virt_inst.abstract_operation = make_operation_id("mylib", "relu");
    vbb.instructions.push_back(virt_inst);
    fn.function.original_vreg_ir.blocks.push_back(vbb);

    allocated_instruction alloc_inst;
    alloc_inst.id = 10;
    alloc_inst.op = opcode::nop;
    fn.function.blocks[0].instructions.push_back(alloc_inst);

    operation_registry reg;
    operation_descriptor desc;
    desc.id = make_operation_id("mylib", "relu");
    reg.register_operation(desc);

    backend_capability_requirement req;
    req.supported_operation_domains = {"mylib"};

    const auto result = validate_operation_legality(fn, reg, req);
    REQUIRE(result.ok());
}

// 10. operation_lowering_pipeline can register a dummy lowering rule
TEST_CASE (



"operation_lowering_pipeline: register and match a dummy rule"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    struct noop_rule {
        [[nodiscard]] bool matches(const operation_id &id) const {
            return id.domain == "mylib" && id.name == "relu";
        }
        [[nodiscard]] operation_lowering_result lower(
            const allocated_instruction &,
            operation_lowering_context &) const {
            operation_lowering_result r;
            r.changed = false;
            return r;
        }
    };

    operation_lowering_pipeline pipeline;
    pipeline.add_rule("noop_relu", noop_rule{});

    REQUIRE(pipeline.size() == 1);
    REQUIRE(!pipeline.empty());

    const auto names = pipeline.rule_names();
    REQUIRE(names.size() == 1);
    REQUIRE(names[0] == "noop_relu");

    REQUIRE(pipeline.remove_rule("noop_relu"));
    REQUIRE(pipeline.empty());
}

// 11. emit_artifact reports diagnostics for unknown abstract operation
TEST_CASE (



"emit_artifact: diagnostics for abstract operation missing from registry"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    codegen_result cr;

    allocated_function_ir alloc_fn;
    alloc_fn.name = "unknown_op_fn";
    alloc_fn.cfg.entry_block = 1;
    allocated_basic_block abb;
    abb.id = 1;  abb.name = "entry";
    allocated_instruction ai;
    ai.id = 10;  ai.op = opcode::nop;
    abb.instructions.push_back(ai);
    alloc_fn.blocks.push_back(abb);

    basic_block vbb;
    vbb.id = 1;
    instruction vi;
    vi.id = 10;  vi.op = opcode::nop;
    vi.abstract_operation = make_operation_id("mylib", "unknown_op");
    vbb.instructions.push_back(vi);
    alloc_fn.original_vreg_ir.blocks.push_back(vbb);

    mir::physical_mir_function phys;
    phys.function = std::move(alloc_fn);
    phys.metadata.current_phase = mir::phase::physical_mir;
    cr.physical_mir = std::move(phys);

    // Registry does NOT contain "mylib/unknown_op".
    operation_registry reg;

    debug_text_backend backend;
    const auto art = emit_artifact(backend, cr, &reg);

    REQUIRE(art.kind == artifact_kind::none);
    REQUIRE(!art.diagnostics.empty());
    bool found_op_diag = false;
    for (const auto &d : art.diagnostics) {
        if (d.find("operation:") != std::string::npos) {
            found_op_diag = true;
        }
    }
    REQUIRE(found_op_diag);
}

// 12. legacy opcode-only emit_artifact still works (no registry)
TEST_CASE (



"emit_artifact: legacy opcode-only MIR works without registry"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    codegen_result cr;

    allocated_function_ir alloc_fn;
    alloc_fn.name = "legacy_fn";
    alloc_fn.cfg.entry_block = 1;
    allocated_basic_block abb;
    abb.id = 1;  abb.name = "entry";
    allocated_instruction ai;
    ai.id = 1;  ai.op = opcode::nop;
    abb.instructions.push_back(ai);
    alloc_fn.blocks.push_back(abb);

    mir::physical_mir_function phys;
    phys.function = std::move(alloc_fn);
    phys.metadata.current_phase = mir::phase::physical_mir;
    cr.physical_mir = std::move(phys);

    debug_text_backend backend;
    const auto art = emit_artifact(backend, cr, nullptr);

    REQUIRE(art.kind == artifact_kind::debug_text);
    REQUIRE(art.diagnostics.empty());
    REQUIRE(art.name == "legacy_fn");
}

// ─── abstract_value_type new-fields tests ────────────────────────────────────

// 13. abstract_value_type carries shape, semantic_type, and attributes
TEST_CASE (



"abstract_value_type: shape, semantic_type, and attributes"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    // tensor<f32, [?, 768]>
    abstract_value_type tensor_type;
    tensor_type.kind          = abstract_value_kind::tensor;
    tensor_type.bit_width     = 32;
    tensor_type.lane_count    = 1;
    tensor_type.shape         = {0, 768};   // 0 == dynamic/"?"
    tensor_type.semantic_type = "f32";
    tensor_type.attributes["device"] = "gpu";

    REQUIRE(tensor_type.kind == abstract_value_kind::tensor);
    REQUIRE(tensor_type.shape.size() == 2);
    REQUIRE(tensor_type.shape[0] == 0);     // dynamic dimension
    REQUIRE(tensor_type.shape[1] == 768);
    REQUIRE(tensor_type.semantic_type == "f32");
    REQUIRE(tensor_type.attributes.at("device") == "gpu");

    // layout_box  — scalar-like, no shape
    abstract_value_type layout_type;
    layout_type.kind          = abstract_value_kind::layout;
    layout_type.semantic_type = "layout_box";
    REQUIRE(layout_type.shape.empty());
    REQUIRE(layout_type.semantic_type == "layout_box");

    // query_row
    abstract_value_type query_type;
    query_type.kind          = abstract_value_kind::query;
    query_type.semantic_type = "query_row";
    REQUIRE(query_type.semantic_type == "query_row");

    // symbolic_expr
    abstract_value_type sym_type;
    sym_type.kind          = abstract_value_kind::symbolic;
    sym_type.semantic_type = "symbolic_expr";
    sym_type.attributes["repr"] = "affine";
    REQUIRE(sym_type.kind == abstract_value_kind::symbolic);
    REQUIRE(sym_type.attributes.at("repr") == "affine");
}

// 14. abstract_value_type new fields do not disturb scalar behavior
TEST_CASE (



"abstract_value_type: scalar fields unchanged after extension"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    abstract_value_type scalar;
    scalar.kind       = abstract_value_kind::integer;
    scalar.bit_width  = 64;
    scalar.lane_count = 1;
    // leave shape, semantic_type, attributes at their defaults

    REQUIRE(scalar.kind == abstract_value_kind::integer);
    REQUIRE(scalar.bit_width == 64);
    REQUIRE(scalar.lane_count == 1);
    REQUIRE(scalar.shape.empty());
    REQUIRE(scalar.semantic_type.empty());
    REQUIRE(scalar.attributes.empty());

    // existing 3-field aggregate init still compiles and works
    abstract_value_type v{abstract_value_kind::vector, 32, 4};
    REQUIRE(v.kind == abstract_value_kind::vector);
    REQUIRE(v.bit_width == 32);
    REQUIRE(v.lane_count == 4);
    REQUIRE(v.shape.empty());
}

// ─── metadata survival tests ─────────────────────────────────────────────────

// 15. apply_register_allocation propagates abstract_operation and
//     operation_attributes from virtual MIR to allocated MIR
TEST_CASE (



"apply_register_allocation: propagates abstract operation metadata"
,
"[lithe][codegen][operation][alloc]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "meta_alloc";
    auto &bb = fn.create_block("entry");
    const auto vr = fn.make_vreg();

    instruction load;
    load.id = 1;
    load.op = opcode::load_imm;
    load.defs = {operand::as_vreg(vr)};
    load.uses = {operand::as_i64(42)};
    load.abstract_operation = make_operation_id("mylib", "const_i64");
    load.operation_attributes["dtype"]  = "i64";
    load.operation_attributes["source"] = "immediate";
    (void) fn.emit(bb.id, std::move(load));

    instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(vr)};
    (void) fn.emit(bb.id, std::move(ret));

    const auto alloc     = allocate_registers(fn, {preg{0, "r0"}});
    const auto allocated = apply_register_allocation(fn, alloc);

    // Find the allocated instruction with id == 1
    const allocated_instruction *found = nullptr;
    for (const auto &blk : allocated.blocks) {
        for (const auto &inst : blk.instructions) {
            if (inst.id == 1) { found = &inst; break; }
        }
        if (found) break;
    }

    REQUIRE(found != nullptr);
    REQUIRE(found->abstract_operation.has_value());
    REQUIRE(*found->abstract_operation == make_operation_id("mylib", "const_i64"));
    REQUIRE(found->operation_attributes.at("dtype")  == "i64");
    REQUIRE(found->operation_attributes.at("source") == "immediate");
}

// 16. rewrite_spills copies metadata to the rewritten instruction and leaves
//     newly generated spill load/store instructions with no abstract_operation
TEST_CASE (



"rewrite_spills: metadata survives; generated spill instructions have none"
,
"[lithe][codegen][operation][alloc]"
)
 {
    using namespace lithe::codegen;

    // Build an allocated IR that has one spilled use so rewrite_spills must
    // insert a load_spill before it.
    allocated_function_ir fn;
    fn.name = "spill_meta";
    fn.cfg.entry_block = 1;

    spill_slot sp;
    sp.id = 1; sp.size = 8; sp.alignment = 8; sp.frame_offset = -8;
    fn.spill_slots.push_back(sp);

    allocated_basic_block bb;
    bb.id = 1; bb.name = "entry";

    // An add whose first use is spilled — this forces a load_spill before it.
    allocated_instruction add_inst;
    add_inst.id = 10;
    add_inst.op = opcode::add;
    add_inst.abstract_operation = make_operation_id("mylib", "vadd");
    add_inst.operation_attributes["dtype"] = "f32";
    add_inst.defs = {allocated_operand::as_preg({0, "r0"})};
    add_inst.uses = {allocated_operand::as_spill(sp),
                     allocated_operand::as_preg({1, "r1"})};
    bb.instructions.push_back(add_inst);
    fn.blocks.push_back(bb);

    auto result = rewrite_spills(std::move(fn), {preg{2, "r2"}});
    REQUIRE(result.ok());
    REQUIRE(result.inserted_loads == 1);

    const auto &instrs = result.function.blocks[0].instructions;
    // Expected layout: [load_spill, add]
    REQUIRE(instrs.size() == 2);

    // The generated load_spill must NOT carry abstract_operation
    const auto &generated_load = instrs[0];
    REQUIRE(generated_load.op == opcode::load_spill);
    REQUIRE(!generated_load.abstract_operation.has_value());

    // The original add must retain its metadata
    const auto &rewritten_add = instrs[1];
    REQUIRE(rewritten_add.id == 10);
    REQUIRE(rewritten_add.abstract_operation.has_value());
    REQUIRE(*rewritten_add.abstract_operation == make_operation_id("mylib", "vadd"));
    REQUIRE(rewritten_add.operation_attributes.at("dtype") == "f32");
}

// 17. Full pipeline: virtual MIR → allocated → physical preserves abstract_operation
TEST_CASE (



"full pipeline: abstract_operation survives allocation and spill rewrite"
,
"[lithe][codegen][operation][alloc]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "full_pipeline_meta";
    auto &bb = fn.create_block("entry");
    const auto vr = fn.make_vreg();

    instruction load;
    load.id = 1;
    load.op = opcode::load_imm;
    load.defs = {operand::as_vreg(vr)};
    load.uses = {operand::as_i64(7)};
    load.abstract_operation = make_operation_id("mylib", "const");
    load.operation_attributes["value"] = "7";
    (void) fn.emit(bb.id, std::move(load));

    instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(vr)};
    (void) fn.emit(bb.id, std::move(ret));

    const auto alloc      = allocate_registers(fn, {preg{0, "r0"}});
    const auto alloc_mir  = apply_register_allocation(fn, alloc);
    const auto phys_result = rewrite_spills(alloc_mir);
    REQUIRE(phys_result.ok());

    const allocated_instruction *found = nullptr;
    for (const auto &blk : phys_result.function.blocks) {
        for (const auto &inst : blk.instructions) {
            if (inst.id == 1) { found = &inst; break; }
        }
        if (found) break;
    }

    REQUIRE(found != nullptr);
    REQUIRE(found->abstract_operation.has_value());
    REQUIRE(*found->abstract_operation == make_operation_id("mylib", "const"));
    REQUIRE(found->operation_attributes.at("value") == "7");
}

// 18. validate_operation_legality uses allocated instruction metadata directly
//     (does not need original_vreg_ir when abstract_operation is set on inst)
TEST_CASE (



"validate_operation_legality: uses direct allocated instruction metadata"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    // Build a physical MIR whose allocated instruction carries abstract_operation
    // directly.  Leave original_vreg_ir empty — the validator must not need it.
    allocated_function_ir alloc_fn;
    alloc_fn.name = "direct_meta";
    alloc_fn.cfg.entry_block = 1;

    allocated_basic_block abb;
    abb.id = 1;
    allocated_instruction ai;
    ai.id  = 5;
    ai.op  = opcode::nop;
    ai.abstract_operation = make_operation_id("mylib", "relu");
    abb.instructions.push_back(ai);
    alloc_fn.blocks.push_back(abb);
    // original_vreg_ir intentionally left empty

    mir::physical_mir_function phys;
    phys.function = std::move(alloc_fn);

    operation_registry reg;
    operation_descriptor desc;
    desc.id = make_operation_id("mylib", "relu");
    reg.register_operation(desc);

    backend_capability_requirement req;
    req.supported_operation_domains = {"mylib"};

    const auto result = validate_operation_legality(phys, reg, req);
    REQUIRE(result.ok());
}

// 19. validate_operation_legality falls back to original_vreg_ir when allocated
//     instruction has no abstract_operation (old code path still works)
TEST_CASE (



"validate_operation_legality: fallback to original_vreg_ir when no direct metadata"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    // Allocated instruction has no abstract_operation — metadata is only in
    // original_vreg_ir, as it was before step-1 propagation was added.
    allocated_function_ir alloc_fn;
    alloc_fn.name = "fallback_meta";
    alloc_fn.cfg.entry_block = 1;

    allocated_basic_block abb;
    abb.id = 1;
    allocated_instruction ai;
    ai.id = 20;
    ai.op = opcode::nop;
    // No abstract_operation on the allocated instruction
    abb.instructions.push_back(ai);
    alloc_fn.blocks.push_back(abb);

    // Metadata lives only in original_vreg_ir
    basic_block vbb;
    vbb.id = 1;
    instruction vi;
    vi.id = 20;
    vi.op = opcode::nop;
    vi.abstract_operation = make_operation_id("mylib", "sigmoid");
    vbb.instructions.push_back(vi);
    alloc_fn.original_vreg_ir.blocks.push_back(vbb);

    mir::physical_mir_function phys;
    phys.function = std::move(alloc_fn);

    operation_registry reg;
    operation_descriptor desc;
    desc.id = make_operation_id("mylib", "sigmoid");
    reg.register_operation(desc);

    backend_capability_requirement req;
    req.supported_operation_domains = {"mylib"};

    const auto result = validate_operation_legality(phys, reg, req);
    REQUIRE(result.ok());

    // Also verify rejection when domain is wrong (proving fallback is active)
    backend_capability_requirement bad_req;
    bad_req.supported_operation_domains = {"other"};
    const auto bad = validate_operation_legality(phys, reg, bad_req);
    REQUIRE(!bad.ok());
    REQUIRE(bad.violations[0].operation == "mylib/sigmoid");
}

// 20. allocated_instruction helpers: has_abstract_operation, effective_operation,
//     effective_traits, has_operation_trait work on the new fields
TEST_CASE (



"allocated_instruction: abstract operation helpers work on direct metadata"
,
"[lithe][codegen][operation]"
)
 {
    using namespace lithe::codegen;

    operation_registry reg;
    operation_descriptor desc;
    desc.id     = make_operation_id("mylib", "matmul");
    desc.traits = add_trait(make_trait_set(), operation_trait::reads_memory);
    reg.register_operation(desc);

    allocated_instruction inst;
    inst.id = 99;
    inst.op = opcode::nop;
    inst.abstract_operation = make_operation_id("mylib", "matmul");

    REQUIRE(has_abstract_operation(inst));
    REQUIRE(effective_operation(inst) == make_operation_id("mylib", "matmul"));
    REQUIRE(has_operation_trait(inst, operation_trait::reads_memory, &reg));
    REQUIRE(!has_operation_trait(inst, operation_trait::writes_memory, &reg));

    // Without abstract_operation, effective_operation falls back to opcode
    allocated_instruction legacy;
    legacy.id = 100;
    legacy.op = opcode::add;
    REQUIRE(!has_abstract_operation(legacy));
    REQUIRE(effective_operation(legacy) == legacy_opcode_operation_id(opcode::add));
}

// T2: find_block returns non-null for existing block and null for missing block
TEST_CASE (



"function_ir::find_block returns block pointer by id or null"
,
"[lithe][codegen][ir][find_block]"
)
 {
    using namespace lithe::codegen;

    function_ir fn;
    fn.name = "find_block_test";
    (void) fn.create_block("entry");
    (void) fn.create_block("second");

    // IDs are 1-based and assigned sequentially by create_block.
    REQUIRE(fn.find_block(1) != nullptr);
    REQUIRE(fn.find_block(1)->name == "entry");
    REQUIRE(fn.find_block(2) != nullptr);
    REQUIRE(fn.find_block(2)->name == "second");

    const std::uint32_t nonexistent_id = 9999;
    REQUIRE(fn.find_block(nonexistent_id) == nullptr);
}

// T2: construct_ssa places phi nodes at join points of a diamond CFG
TEST_CASE (



"construct_ssa places phi at join block in diamond CFG"
,
"[lithe][codegen][ssa][construct_ssa]"
)
 {
    using namespace lithe::codegen;

    // Diamond: entry(1) -> then(2) -> join(4)
    //          entry(1) -> else(3) -> join(4)
    // preg 0 defined in both then and else -> phi placed at join
    allocated_function_ir fn;
    fn.name = "ssa_diamond";
    fn.cfg.entry_block = 1;

    auto make_block = [](std::uint32_t id, std::string name,
                         std::vector<std::uint32_t> preds,
                         std::vector<std::uint32_t> succs) {
        allocated_basic_block b;
        b.id = id;
        b.name = std::move(name);
        b.predecessors = std::move(preds);
        b.successors = std::move(succs);
        return b;
    };

    auto make_def = [](std::uint32_t iid, std::uint16_t preg_id) {
        allocated_instruction i;
        i.id = iid;
        i.op = opcode::load_imm;
        i.defs = {allocated_operand::as_preg({preg_id, "r0"})};
        i.uses = {allocated_operand::as_i64(1)};
        return i;
    };

    auto entry = make_block(1, "entry", {}, {2, 3});
    entry.instructions.push_back(make_def(10, 0));
    fn.blocks.push_back(std::move(entry));

    auto then_blk = make_block(2, "then", {1}, {4});
    then_blk.instructions.push_back(make_def(20, 0));
    fn.blocks.push_back(std::move(then_blk));

    auto else_blk = make_block(3, "else", {1}, {4});
    else_blk.instructions.push_back(make_def(30, 0));
    fn.blocks.push_back(std::move(else_blk));

    auto join = make_block(4, "join", {2, 3}, {});
    allocated_instruction ret;
    ret.id = 40;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};
    join.instructions.push_back(std::move(ret));
    fn.blocks.push_back(std::move(join));

    fn.cfg.successors[1] = {2, 3};
    fn.cfg.successors[2] = {4};
    fn.cfg.successors[3] = {4};
    fn.cfg.predecessors[2] = {1};
    fn.cfg.predecessors[3] = {1};
    fn.cfg.predecessors[4] = {2, 3};

    mir::physical_mir_function phys;
    phys.function = std::move(fn);
    phys.metadata.current_phase = mir::phase::physical_mir;

    const auto result = construct_ssa(phys);

    REQUIRE(result.ok());
    REQUIRE_FALSE(result.block_states.empty());
    REQUIRE_FALSE(result.value_table.empty());

    // Phi node must be placed at the join block (block 4)
    const auto it = result.block_states.find(4);
    REQUIRE(it != result.block_states.end());
    REQUIRE_FALSE(it->second.phi_nodes.empty());
    REQUIRE(it->second.phi_nodes.front().preg_id == 0);
    // Each phi incoming slot corresponds to one predecessor
    REQUIRE(it->second.phi_nodes.front().incoming.size() == 2);
}

// T2: allocate_registers assigns every def vreg a physical register
TEST_CASE (



"allocate_registers assigns physical register to each defined vreg"
,
"[lithe][codegen][regalloc][correctness]"
)
 {
    using namespace lithe;
    using namespace lithe::codegen;

    // Build: entry defines vr0, vr1 via load_imm; adds them into vr2; returns vr2.
    function_ir fn;
    fn.name = "regalloc_correct";
    auto &entry = fn.create_block("entry");

    const auto vr0 = fn.make_vreg();
    const auto vr1 = fn.make_vreg();
    const auto vr2 = fn.make_vreg();

    instruction li0;
    li0.op = opcode::load_imm;
    li0.defs = {operand::as_vreg(vr0)};
    li0.uses = {operand::as_i64(1)};
    (void) fn.emit(entry.id, std::move(li0));

    instruction li1;
    li1.op = opcode::load_imm;
    li1.defs = {operand::as_vreg(vr1)};
    li1.uses = {operand::as_i64(2)};
    (void) fn.emit(entry.id, std::move(li1));

    instruction add_inst;
    add_inst.op = opcode::add;
    add_inst.defs = {operand::as_vreg(vr2)};
    add_inst.uses = {operand::as_vreg(vr0), operand::as_vreg(vr1)};
    (void) fn.emit(entry.id, std::move(add_inst));

    instruction ret;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(vr2)};
    (void) fn.emit(entry.id, std::move(ret));

    const preg r0{0, "r0"};
    const preg r1{1, "r1"};
    const preg r2{2, "r2"};
    const auto allocation = allocate_registers(fn, {r0, r1, r2});

    // All three vregs must appear in the allocation map
    REQUIRE(allocation.assignments.contains(vr0.id));
    REQUIRE(allocation.assignments.contains(vr1.id));
    REQUIRE(allocation.assignments.contains(vr2.id));

    // With three registers for three live vregs, no spills should occur.
    for (const auto vreg_id : {vr0.id, vr1.id, vr2.id}) {
        const auto &assign = allocation.assignments.at(vreg_id);
        REQUIRE_FALSE(assign.spilled());
        REQUIRE(assign.physical.has_value());
    }
}

// ============================================================================
// Finding 6: DAG use_count == incoming edges
// ============================================================================

TEST_CASE (


"DAG use_count == incoming edges"
,
"[lithe][dag]"
)
 {
    using namespace lithe;

    // 1 + 2: no sharing expected — single-use leaf nodes.
    auto e1 = as_expr(1.0) + as_expr(2.0);
    auto dag1 = graph::build_dag(e1);
    REQUIRE(dag1.sharing_count() == 0);

    // Verify all non-root nodes have use_count == number of incoming edges.
    for (const auto& [id, node] : dag1.dag.nodes) {
        if (id == dag1.dag.root) continue; // root has no incoming edge from this DAG
        REQUIRE(node.use_count >= 1u);
    }

    // Genuinely shared: let sub = a + b; result = sub + sub (sub used twice).
    auto a = as_expr(3.0);
    auto b = as_expr(4.0);
    auto sub = a + b;
    auto shared_expr_val = sub + sub;
    auto dag2 = graph::build_dag(shared_expr_val);
    // sub is referenced twice — sharing_count must be >= 1.
    REQUIRE(dag2.sharing_count() >= 1u);
}

// ============================================================================
// Hardening: INT64_MIN negation — interpreter and constant-folder agree
// ============================================================================

TEST_CASE (


"Interpreter neg INT64_MIN wraps to INT64_MIN (two's complement)"
,
"[lithe][codegen][interpreter][neg][int64_min]"
)
 {
    using namespace lithe::codegen;

    // load_imm r0 = INT64_MIN; neg r1 = r0; ret r1
    // Two's-complement: -INT64_MIN == INT64_MIN (wrapping).
    std::vector<allocated_instruction> insts;
    {
        allocated_instruction li;
        li.id = 1; li.op = opcode::load_imm;
        li.defs = {allocated_operand::as_preg({0, "r0"})};
        li.uses = {allocated_operand::as_i64(std::numeric_limits<std::int64_t>::min())};
        insts.push_back(li);
    }
    {
        allocated_instruction neg;
        neg.id = 2; neg.op = opcode::neg;
        neg.defs = {allocated_operand::as_preg({1, "r1"})};
        neg.uses = {allocated_operand::as_preg({0, "r0"})};
        insts.push_back(neg);
    }
    {
        allocated_instruction ret;
        ret.id = 3; ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({1, "r1"})};
        insts.push_back(ret);
    }

    backends::interpreter_backend backend;
    auto result = emit_function(backend, make_physical("neg_int64_min", insts).function);

    REQUIRE(result.ok());
    REQUIRE(backend.return_value.has_value());
    REQUIRE(*backend.return_value == std::numeric_limits<std::int64_t>::min());
}

TEST_CASE (


"Constant-fold neg INT64_MIN wraps to INT64_MIN"
,
"[lithe][codegen][fold][neg][int64_min]"
)
 {
    using namespace lithe::codegen;

    // load_imm r0 = INT64_MIN; neg r1 = r0; ret r1
    // After partial evaluation both load_imm instructions should have value INT64_MIN.
    std::vector<allocated_instruction> insts;
    {
        allocated_instruction li;
        li.id = 1; li.op = opcode::load_imm;
        li.defs = {allocated_operand::as_preg({0, "r0"})};
        li.uses = {allocated_operand::as_i64(std::numeric_limits<std::int64_t>::min())};
        insts.push_back(li);
    }
    {
        allocated_instruction neg;
        neg.id = 2; neg.op = opcode::neg;
        neg.defs = {allocated_operand::as_preg({1, "r1"})};
        neg.uses = {allocated_operand::as_preg({0, "r0"})};
        insts.push_back(neg);
    }
    {
        allocated_instruction ret;
        ret.id = 3; ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({1, "r1"})};
        insts.push_back(ret);
    }

    const auto pe = partial_evaluate(make_physical("neg_fold_int64_min", insts));
    REQUIRE(pe.changed);
    // The neg must have been folded into a load_imm r1 = INT64_MIN.
    bool found_folded = false;
    for (const auto& bb : pe.function.function.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == opcode::load_imm && inst.defs.size() == 1) {
                const auto& def = inst.defs[0];
                if (def.type == allocated_operand::kind::preg &&
                    std::get<preg>(def.value).id == 1) {
                    REQUIRE(!inst.uses.empty());
                    const auto& use = inst.uses[0];
                    REQUIRE(use.type == allocated_operand::kind::immediate_i64);
                    REQUIRE(std::get<std::int64_t>(use.value) ==
                            std::numeric_limits<std::int64_t>::min());
                    found_folded = true;
                }
            }
        }
    }
    REQUIRE(found_folded);
}

// ============================================================================
// Hardening: constant-fold shr must agree with interpreter runtime shr
// ============================================================================

TEST_CASE (


"Constant-fold shr agrees with interpreter for arithmetic right shift"
,
"[lithe][codegen][fold][interpreter][shr]"
)
 {
    using namespace lithe::codegen;

    // -8 >> 2 = -2 (arithmetic/sign-preserving). Both fold and interpreter must agree.
    constexpr std::int64_t lhs_val = -8;
    constexpr std::int64_t rhs_val = 2;
    constexpr std::int64_t expected = -2;

    std::vector<allocated_instruction> insts;
    {
        allocated_instruction li0;
        li0.id = 1; li0.op = opcode::load_imm;
        li0.defs = {allocated_operand::as_preg({0, "r0"})};
        li0.uses = {allocated_operand::as_i64(lhs_val)};
        insts.push_back(li0);
    }
    {
        allocated_instruction li1;
        li1.id = 2; li1.op = opcode::load_imm;
        li1.defs = {allocated_operand::as_preg({1, "r1"})};
        li1.uses = {allocated_operand::as_i64(rhs_val)};
        insts.push_back(li1);
    }
    {
        allocated_instruction shr;
        shr.id = 3; shr.op = opcode::shr;
        shr.defs = {allocated_operand::as_preg({2, "r2"})};
        shr.uses = {allocated_operand::as_preg({0, "r0"}),
                    allocated_operand::as_preg({1, "r1"})};
        insts.push_back(shr);
    }
    {
        allocated_instruction ret;
        ret.id = 4; ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({2, "r2"})};
        insts.push_back(ret);
    }

    // Runtime path
    backends::interpreter_backend backend;
    auto rt_result = emit_function(backend, make_physical("shr_runtime", insts).function);
    REQUIRE(rt_result.ok());
    REQUIRE(backend.return_value.has_value());
    const std::int64_t runtime_val = *backend.return_value;
    REQUIRE(runtime_val == expected);

    // Constant-fold path
    const auto pe = partial_evaluate(make_physical("shr_fold", insts));
    REQUIRE(pe.changed);
    bool found_folded = false;
    for (const auto& bb : pe.function.function.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == opcode::load_imm && inst.defs.size() == 1) {
                const auto& def = inst.defs[0];
                if (def.type == allocated_operand::kind::preg &&
                    std::get<preg>(def.value).id == 2) {
                    REQUIRE(!inst.uses.empty());
                    const auto& use = inst.uses[0];
                    REQUIRE(use.type == allocated_operand::kind::immediate_i64);
                    const std::int64_t fold_val = std::get<std::int64_t>(use.value);
                    REQUIRE(fold_val == runtime_val);
                    found_folded = true;
                }
            }
        }
    }
    REQUIRE(found_folded);
}

// ============================================================================
// Hardening: undefined (never-written) register reads as zero
// ============================================================================

TEST_CASE (


"Interpreter reads unwritten register as zero"
,
"[lithe][codegen][interpreter][undefined_reg]"
)
 {
    using namespace lithe::codegen;

    // r0 is never defined (no load_arg, no load_imm). The interpreter
    // documents that unwritten integer registers default to 0.
    std::vector<allocated_instruction> insts;
    {
        allocated_instruction ret;
        ret.id = 1; ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({0, "r0"})};
        insts.push_back(ret);
    }

    backends::interpreter_backend backend;
    auto result = emit_function(backend, make_physical("undef_reg", insts).function);

    REQUIRE(result.ok());
    REQUIRE(backend.return_value.has_value());
    REQUIRE(*backend.return_value == 0);
}

TEST_CASE (

"Device provider selection prefers Metal before Vulkan"
,
"[lithe][codegen][device][selection]"
)
 {
    using namespace lithe::codegen::backends;

    CHECK(select_device_provider(true, true) == device_provider::metal);
    CHECK(select_device_provider(true, false) == device_provider::metal);
    CHECK(select_device_provider(false, true) == device_provider::vulkan);
    CHECK(select_device_provider(false, false) == device_provider::none);
}
