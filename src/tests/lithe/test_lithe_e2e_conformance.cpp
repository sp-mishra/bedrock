#include "catch_amalgamated.hpp"

#include "lithe/lithe.hpp"
#include "lithe/lithe_lowering.hpp"
#include "lithe/lithe_semantic.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/backends/lithe_codegen_backend_registry.hpp"
#include "lithe/backends/lithe_codegen_debug_text_backend.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"

#include <string>
#include <algorithm>

// ============================================================================
// Helpers shared across tests
// ============================================================================

namespace {
    using namespace lithe::codegen;

    // Minimal physical MIR function: one block, given instructions, entry = 1.
    mir::physical_mir_function make_physical(
        std::string name,
        std::vector<allocated_instruction> instructions
    ) {
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = 1;

        allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";
        bb.instructions = std::move(instructions);
        fn.blocks.push_back(std::move(bb));

        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }

    // Build a two-register add-then-ret physical function.
    mir::physical_mir_function make_add_ret_physical(std::string name = "add_fn") {
        allocated_instruction add;
        add.id = 1;
        add.op = opcode::add;
        add.defs = {allocated_operand::as_preg({0, "r0"})};
        add.uses = {
            allocated_operand::as_preg({1, "r1"}),
            allocated_operand::as_preg({2, "r2"})
        };

        allocated_instruction ret;
        ret.id = 2;
        ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({0, "r0"})};

        return make_physical(std::move(name), {add, ret});
    }
} // namespace

// ============================================================================
// Test 1 — EDSL expression -> semantic type inference -> MIR -> debug artifact
// ============================================================================

TEST_CASE (



"e2e: typed expression with i32 inferred type lowers to MIR and emits debug artifact"
,
"[lithe][e2e][semantic][mir][debug]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;
    using namespace lithe::lowering;
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // Semantic layer: register i32, infer type for add(i32, i32).
    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");

    semantic_type_rule_engine engine{&reg};
    const auto inferred = engine.infer_expression_type(0, {i32, i32}, "add");
    REQUIRE(inferred.inferred);
    REQUIRE(inferred.type_id != invalid_type_id);

    // Lower a typed expression that carries the inferred type.
    typed_ir::typed_expression texpr;
    texpr.expression_key = 0x01;
    texpr.inferred_type = inferred.type_id;
    texpr.effect_metadata.effect = effect_type::pure;
    texpr.effect_metadata.purity_level = purity::pure;

    typed_lowering_context ctx;
    ctx.insert_coercions = false;

    const auto mir_instr = lower_typed_expression(texpr, ctx);
    REQUIRE(mir_instr.has_result_type());
    REQUIRE(mir_instr.primary_result_type() == inferred.type_id);

    // Backend layer: emit physical MIR through debug_text_backend.
    auto phys = make_add_ret_physical("infer_add");
    debug_text_backend backend;
    const auto result = emit_function(backend, phys);

    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE_FALSE(result.artifact_text->empty());
}

// ============================================================================
// Test 2 — imported_node -> operation_id -> typed lowering -> text assembly
// ============================================================================

TEST_CASE (



"e2e: imported_node with op::add lowers to frontend_ast and emits text assembly artifact"
,
"[lithe][e2e][frontend][assembly]"
)
 {
    using namespace lithe::frontend;
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // Frontend layer: build an imported_node for an add operation.
    imported_node node;
    node.operation = op::add;
    node.attributes["lhs"] = "a";
    node.attributes["rhs"] = "b";

    source_location begin;
    begin.file_id = 1;
    begin.line = 1;
    begin.column = 1;
    begin.offset = 0;
    source_location end;
    end.file_id = 1;
    end.line = 1;
    end.column = 5;
    end.offset = 4;
    node.span.begin = begin;
    node.span.end = end;

    const auto lower_result = lower_imported_node(node);
    REQUIRE(lower_result.ok());
    REQUIRE(lower_result.ast.node_kind == "add");

    // The lowered AST must carry forward the operation name.
    const auto &ast = lower_result.ast;
    REQUIRE_FALSE(ast.node_kind.empty());

    // Backend layer: emit through text_assembly_target.
    allocated_instruction inst;
    inst.id = 1;
    inst.op = opcode::add;
    inst.abstract_operation = make_operation_id("lithe.core", "add");
    inst.defs = {allocated_operand::as_preg({0, "r0"})};
    inst.uses = {
        allocated_operand::as_preg({1, "r1"}),
        allocated_operand::as_preg({2, "r2"})
    };

    allocated_instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto phys = make_physical("imported_add", {inst, ret});

    text_assembly_target target;
    const auto art = target.emit(phys);

    REQUIRE(art.kind == artifact_kind::assembly_text);
    REQUIRE_FALSE(art.text_payload.empty());
    REQUIRE(art.diagnostics.empty());
}

// ============================================================================
// Test 3 — int32 + int64: coercion_planner inserts numeric_widen step
// ============================================================================

TEST_CASE (



"e2e: i32 + i64 operand mismatch causes coercion_planner to insert numeric_widen"
,
"[lithe][e2e][coercion][numeric_widen]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;
    using namespace lithe::lowering;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    // Plan coercion from i32 to i64.
    coercion_planner planner{&reg};
    const auto plan = planner.plan(i32, i64);

    REQUIRE(plan.feasible);
    REQUIRE(plan.is_lossless);
    REQUIRE_FALSE(plan.steps.empty());
    REQUIRE(plan.steps.front().kind == coercion_kind::numeric_widen);
    REQUIRE(plan.steps.front().from_type == i32);
    REQUIRE(plan.steps.front().to_type == i64);

    // Lower a typed expression that carries the widening coercion plan.
    typed_ir::typed_expression texpr;
    texpr.expression_key = 0x02;
    texpr.inferred_type = i64;
    texpr.coercion = plan;
    texpr.effect_metadata.effect = effect_type::pure;
    texpr.effect_metadata.purity_level = purity::pure;

    typed_lowering_context ctx;
    ctx.insert_coercions = true;

    const auto mir_instr = lower_typed_expression(texpr, ctx);

    // The lowered instruction must reflect the widening, not the trivial identity.
    REQUIRE(mir_instr.has_result_type());
    REQUIRE(mir_instr.primary_result_type() == i64);
    REQUIRE_FALSE(mir_instr.opcode.empty());
    REQUIRE(mir_instr.opcode != "value");
}

// ============================================================================
// Test 4 — tensor shape mismatch produces semantic diagnostic before MIR
// ============================================================================

TEST_CASE (



"e2e: tensor + dynamic type mismatch is rejected by semantic rule engine before MIR"
,
"[lithe][e2e][semantic][tensor][diagnostic]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;

    semantic_type_registry reg;
    const auto f32 = reg.make_float_type(32, "f32");
    const auto tensor_t = reg.make_tensor_type(f32, {4, 4}, "tensor_4x4");
    const auto query_t = reg.make_dynamic_type("query");

    // Direct unification must fail — tensor and dynamic are structurally incompatible.
    semantic_type_unifier unifier{&reg};
    type_unification_result uni_out;
    const bool unified = unifier.unify(tensor_t, query_t, uni_out);
    REQUIRE_FALSE(unified);
    REQUIRE_FALSE(uni_out.success);
    REQUIRE_FALSE(uni_out.failed_constraints.empty());

    // Operation-contract check also must not produce a clean result.
    semantic_type_rule_engine engine{&reg};
    const auto contract = engine.check_operation_contract("add", {tensor_t, query_t});

    const bool has_diagnostic = !contract.ok ||
                                std::any_of(contract.diagnostics.begin(), contract.diagnostics.end(),
                                            [](const type_rule_diagnostic &d) {
                                                return d.severity == type_rule_severity::error ||
                                                       d.severity == type_rule_severity::warning;
                                            });
    REQUIRE(has_diagnostic);
}

// ============================================================================
// Test 5 — unsupported abstract operation rejected by asmjit backend legality
// ============================================================================

TEST_CASE (



"e2e: abstract operation from unknown domain is rejected by asmjit backend legality check"
,
"[lithe][e2e][legality][asmjit]"
)
 {
    using namespace lithe::codegen;

    // Build a physical MIR function with an instruction whose abstract_operation
    // belongs to a domain the asmjit backend does not support.
    allocated_instruction inst;
    inst.id = 10;
    inst.op = opcode::nop;
    inst.abstract_operation = make_operation_id("custom.gpu", "tensor_matmul");

    allocated_instruction ret;
    ret.id = 11;
    ret.op = opcode::ret;

    allocated_function_ir fn;
    fn.name = "legality_test";
    fn.cfg.entry_block = 1;

    basic_block vbb;
    vbb.id = 1;
    instruction virt_inst;
    virt_inst.id = 10;
    virt_inst.op = opcode::nop;
    virt_inst.abstract_operation = make_operation_id("custom.gpu", "tensor_matmul");
    vbb.instructions.push_back(virt_inst);
    fn.original_vreg_ir.blocks.push_back(vbb);

    allocated_basic_block abb;
    abb.id = 1;
    abb.instructions = {inst, ret};
    fn.blocks.push_back(std::move(abb));

    mir::physical_mir_function phys;
    phys.function = std::move(fn);
    phys.metadata.current_phase = mir::phase::physical_mir;

    // Register the operation so the registry knows it.
    operation_registry op_reg;
    operation_descriptor desc;
    desc.id = make_operation_id("custom.gpu", "tensor_matmul");
    op_reg.register_operation(desc);

    // asmjit backend only supports "lithe.core" — custom.gpu must be rejected.
    backend_capability_requirement req;
    req.backend_name = "asmjit_backend";
    req.supported_operation_domains = {"lithe.core"};

    const auto result = validate_operation_legality(phys, op_reg, req);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.violations.empty());
    REQUIRE(result.violations[0].operation.find("custom.gpu") != std::string::npos);
}

// ============================================================================
// Test 6 — debug backend emits artifact for instruction with abstract operation
// ============================================================================

TEST_CASE (



"e2e: debug_text_backend emits a valid artifact for instruction with abstract_operation"
,
"[lithe][e2e][debug][abstract_operation]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    allocated_instruction inst;
    inst.id = 1;
    inst.op = opcode::add;
    inst.abstract_operation = make_operation_id("mylib", "vector_add");
    inst.operation_attributes["dtype"] = "i64";
    inst.defs = {allocated_operand::as_preg({0, "r0"})};
    inst.uses = {
        allocated_operand::as_preg({1, "r1"}),
        allocated_operand::as_preg({2, "r2"})
    };

    allocated_instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;
    ret.uses = {allocated_operand::as_preg({0, "r0"})};

    auto phys = make_physical("abstract_op_fn", {inst, ret});

    debug_text_backend backend;
    const auto result = emit_function(backend, phys);

    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
    REQUIRE_FALSE(result.artifact_text->empty());
    // The artifact text must mention the operation in some form.
    const bool mentions_op =
            result.artifact_text->find("vector_add") != std::string::npos ||
            result.artifact_text->find("abstract") != std::string::npos ||
            result.artifact_text->find("add") != std::string::npos;
    REQUIRE(mentions_op);
}

// ============================================================================
// Test 7 — interpreter rejects unsupported dynamic operation (call opcode)
// ============================================================================

TEST_CASE (



"e2e: interpreter_backend rejects call opcode because calls capability is not declared"
,
"[lithe][e2e][interpreter][capability]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // Verify the interpreter does not advertise call support.
    REQUIRE_FALSE(interpreter_backend::capabilities().has(backend_feature::calls));

    allocated_instruction call_inst;
    call_inst.id = 1;
    call_inst.op = opcode::call;

    allocated_instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;

    auto phys = make_physical("dynamic_call_fn", {call_inst, ret});

    interpreter_backend backend;
    const auto result = emit_function(backend, phys);

    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(result.errors.empty());

    const bool call_mentioned = std::any_of(
        result.errors.begin(), result.errors.end(),
        [](const backend_error &e) {
            return e.message.find("call") != std::string::npos ||
                   e.message.find("calls") != std::string::npos;
        });
    REQUIRE(call_mentioned);
}

// ============================================================================
// Test 8 — source_span survives into diagnostic from imported_node lowering
// ============================================================================

TEST_CASE (



"e2e: source_span from imported_node survives into lowered AST and diagnostic context"
,
"[lithe][e2e][source_span][diagnostic]"
)
 {
    using namespace lithe::frontend;

    // Valid node: span must survive into ast.span.
    {
        imported_node node;
        node.operation = op::variable;

        source_location b;
        b.file_id = 3;
        b.line = 10;
        b.column = 2;
        b.offset = 200;
        source_location e;
        e.file_id = 3;
        e.line = 10;
        e.column = 9;
        e.offset = 207;
        node.span.begin = b;
        node.span.end = e;

        const auto result = lower_imported_node(node);
        REQUIRE(result.ok());
        REQUIRE(result.ast.span.has_value());
        REQUIRE(result.ast.span->line == 10u);
        REQUIRE(result.ast.span->column == 2u);
        REQUIRE(result.ast.span->offset == 200u);
        REQUIRE(result.ast.span->length == 7u);
    }

    // Unknown operation: span coordinates must appear in the diagnostic.
    {
        imported_node bad_node;
        bad_node.operation = 0xBEEF;

        source_location b;
        b.file_id = 1;
        b.line = 5;
        b.column = 1;
        b.offset = 50;
        source_location e;
        e.file_id = 1;
        e.line = 5;
        e.column = 8;
        e.offset = 57;
        bad_node.span.begin = b;
        bad_node.span.end = e;

        lithe::lowering::frontend_context ctx;
        const auto result = lower_imported_node(bad_node, ctx);

        REQUIRE_FALSE(result.ok());
        REQUIRE_FALSE(result.diagnostics.empty());
        REQUIRE_FALSE(ctx.diagnostics.empty());
        // The context diagnostic must carry the source position.
        REQUIRE(ctx.diagnostics.front().span.has_value());
        REQUIRE(ctx.diagnostics.front().span->line == 5u);
    }
}

// ============================================================================
// Test 9 — abstract_operation metadata survives Virtual -> Allocated -> Physical MIR
// ============================================================================

TEST_CASE (



"e2e: abstract_operation and operation_attributes survive virtual -> allocated -> physical MIR"
,
"[lithe][e2e][mir][abstract_operation][propagation]"
)
 {
    using namespace lithe::codegen;

    // Build a virtual MIR function with abstract_operation on load_imm.
    function_ir fn;
    fn.name = "meta_e2e";
    auto &bb = fn.create_block("entry");
    const auto vr = fn.make_vreg();

    instruction load;
    load.id = 1;
    load.op = opcode::load_imm;
    load.defs = {operand::as_vreg(vr)};
    load.uses = {operand::as_i64(99)};
    load.abstract_operation = make_operation_id("mylib", "const_i64");
    load.operation_attributes["source"] = "immediate";
    load.operation_attributes["dtype"] = "i64";
    (void) fn.emit(bb.id, std::move(load));

    instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;
    ret.uses = {operand::as_vreg(vr)};
    (void) fn.emit(bb.id, std::move(ret));

    // Virtual -> Allocated MIR.
    const auto alloc = allocate_registers(fn, {preg{0, "r0"}});
    const auto allocated = apply_register_allocation(fn, alloc);

    const allocated_instruction *alloc_inst = nullptr;
    for (const auto &blk: allocated.blocks)
        for (const auto &i: blk.instructions)
            if (i.id == 1) {
                alloc_inst = &i;
                break;
            }

    REQUIRE(alloc_inst != nullptr);
    REQUIRE(alloc_inst->abstract_operation.has_value());
    REQUIRE(*alloc_inst->abstract_operation == make_operation_id("mylib", "const_i64"));
    REQUIRE(alloc_inst->operation_attributes.at("dtype") == "i64");
    REQUIRE(alloc_inst->operation_attributes.at("source") == "immediate");

    // Allocated -> Physical MIR.
    allocated_function_ir fn_copy = allocated;
    mir::physical_mir_function phys;
    phys.function = std::move(fn_copy);
    phys.metadata.current_phase = mir::phase::physical_mir;

    const allocated_instruction *phys_inst = nullptr;
    for (const auto &blk: phys.function.blocks)
        for (const auto &i: blk.instructions)
            if (i.id == 1) {
                phys_inst = &i;
                break;
            }

    REQUIRE(phys_inst != nullptr);
    REQUIRE(phys_inst->abstract_operation.has_value());
    REQUIRE(*phys_inst->abstract_operation == make_operation_id("mylib", "const_i64"));
    REQUIRE(phys_inst->operation_attributes.at("dtype") == "i64");
}

// ============================================================================
// Test 10 — legacy opcode-only path still works unchanged
// ============================================================================

TEST_CASE (



"e2e: legacy opcode-only physical MIR emits correctly through debug_text_backend"
,
"[lithe][e2e][legacy][opcode]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    // No abstract_operation, no operation_attributes — pure legacy opcode path.
    allocated_instruction nop;
    nop.id = 1;
    nop.op = opcode::nop;

    allocated_instruction ret;
    ret.id = 2;
    ret.op = opcode::ret;

    codegen_result cr;
    cr.physical_mir = make_physical("legacy_fn", {nop, ret});

    debug_text_backend backend;
    const auto art = emit_artifact(backend, cr, nullptr);

    REQUIRE(art.kind == artifact_kind::debug_text);
    REQUIRE(art.diagnostics.empty());
    REQUIRE(art.name == "legacy_fn");
    REQUIRE_FALSE(art.text_payload.empty());
}
