#include "catch_amalgamated.hpp"

#include "catch_amalgamated.hpp"

#include "lithe/backends/lithe_codegen_assembler.hpp"
#include "lithe/backends/lithe_codegen_debug_text_backend.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"

#include <array>
#include <string>

namespace {
using namespace lithe::codegen;
using namespace lithe::codegen::backends;

mir::physical_mir_function make_trivial(std::string name = "f") {
  allocated_function_ir fn;
  fn.name = std::move(name);
  fn.cfg.entry_block = 1;

  allocated_basic_block bb;
  bb.id = 1;
  bb.name = "entry";

  allocated_instruction ret;
  ret.id = 1;
  ret.op = opcode::ret;
  bb.instructions.push_back(ret);
  fn.blocks.push_back(std::move(bb));

  mir::physical_mir_function phys;
  phys.function = std::move(fn);
  phys.metadata.current_phase = mir::phase::physical_mir;
  return phys;
}

// Build a trivial physical function with one add and one ret.
mir::physical_mir_function make_add_ret(std::string name = "add_fn") {
  allocated_function_ir fn;
  fn.name = std::move(name);
  fn.cfg.entry_block = 1;

  allocated_basic_block bb;
  bb.id = 1;
  bb.name = "entry";

  allocated_instruction add;
  add.id = 1;
  add.op = opcode::add;
  add.defs = {allocated_operand::as_preg({0, "r0"})};
  add.uses = {allocated_operand::as_preg({1, "r1"}),
              allocated_operand::as_preg({2, "r2"})};
  bb.instructions.push_back(add);

  allocated_instruction ret;
  ret.id = 2;
  ret.op = opcode::ret;
  ret.uses = {allocated_operand::as_preg({0, "r0"})};
  bb.instructions.push_back(ret);

  fn.blocks.push_back(std::move(bb));
  mir::physical_mir_function phys;
  phys.function = std::move(fn);
  phys.metadata.current_phase = mir::phase::physical_mir;
  return phys;
}
} // namespace

// ---------------------------------------------------------------------------
// Test 1 — asmjit backend header compiles without asmjit installed
// ---------------------------------------------------------------------------
TEST_CASE(

    "assembler header compiles without asmjit installed",
    "[lithe][backend][asmjit][no_jit]") {
  // Including lithe_codegen_assembler.hpp (done above) defines the
  // AbstractAssembler concept and text_assembly_target.  Neither depends
  // on the asmjit library.  If this test compiles, the header is
  // asmjit-free and satisfies the "no real asmjit" rule.

  using namespace lithe::codegen::backends;

  // text_assembly_target is the non-JIT reference assembler.
  // It satisfies CodeEmissionTarget but not the full AbstractAssembler
  // surface (no begin_function / end_function etc.) by design.
  STATIC_REQUIRE(lithe::codegen::CodeEmissionTarget<text_assembly_target>);
}

// ---------------------------------------------------------------------------
// Test 2 — asmjit backend returns diagnostic artifact when unavailable
// ---------------------------------------------------------------------------
TEST_CASE(

    "emit_artifact on failed codegen returns diagnostic artifact with "
    "kind::none",
    "[lithe][backend][asmjit][diagnostic_artifact]") {
  using namespace lithe::codegen;
  using namespace lithe::codegen::backends;

  // Simulate what an unavailable JIT backend would produce: a codegen_result
  // that failed before any physical MIR was emitted.
  codegen_result failed;
  failed.diagnostics.push_back("asmjit unavailable: library not linked");

  // Any CodeEmissionTarget (here: debug_text_backend standing in for an
  // asmjit backend) should propagate the failure as a diagnostic artifact.
  debug_text_backend backend;
  const auto art = emit_artifact(backend, failed);

  REQUIRE(art.kind == artifact_kind::none);
  REQUIRE_FALSE(art.diagnostics.empty());
  REQUIRE(art.diagnostics.front().find("asmjit unavailable") !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// Test 3 — backend type legality rejects unsupported tensor operation
// ---------------------------------------------------------------------------
TEST_CASE(

    "backend type legality rejects tensor operation when backend denies tensor "
    "kind",
    "[lithe][backend][legality][tensor]") {
  using namespace lithe::codegen;
  using namespace lithe::semantic::types;

  // Register a tensor-shaped abstract operation.
  operation_registry op_reg;
  operation_descriptor desc;
  desc.id = make_operation_id("lithe.tensor", "matmul");

  abstract_value_type tensor_operand;
  tensor_operand.kind = abstract_value_kind::tensor;
  tensor_operand.shape = {4, 4};

  abstract_value_type tensor_result;
  tensor_result.kind = abstract_value_kind::tensor;
  tensor_result.shape = {4, 4};

  desc.contract.operands = {tensor_operand, tensor_operand};
  desc.contract.results = {tensor_result};
  op_reg.register_operation(desc);

  // Build a physical MIR function whose instruction carries the tensor op.
  allocated_instruction inst;
  inst.id = 1;
  inst.op = opcode::nop;
  inst.abstract_operation = make_operation_id("lithe.tensor", "matmul");

  allocated_function_ir fn;
  fn.name = "tensor_fn";
  fn.cfg.entry_block = 1;
  allocated_basic_block bb;
  bb.id = 1;
  bb.instructions.push_back(inst);

  allocated_instruction ret;
  ret.id = 2;
  ret.op = opcode::ret;
  bb.instructions.push_back(ret);

  fn.blocks.push_back(std::move(bb));
  mir::physical_mir_function physical;
  physical.function = std::move(fn);
  physical.metadata.current_phase = mir::phase::physical_mir;

  // A requirement that only accepts integer and floating kinds (no tensor).
  backend_capability_requirement req;
  req.backend_name = "no_tensor_backend";
  req.accept_type_kind(lithe::semantic::types::type_kind::integer);
  req.accept_type_kind(lithe::semantic::types::type_kind::floating);

  semantic_type_registry type_reg;
  const auto legality =
      validate_backend_type_legality(physical, req, op_reg, type_reg);

  // The tensor operand must be flagged.
  REQUIRE_FALSE(legality.diagnostics.empty());
  REQUIRE_FALSE(legality.unsupported_type_kinds.empty());
}

// ---------------------------------------------------------------------------
// Test 4 — debug/text targets accept printable abstract operations
// ---------------------------------------------------------------------------
TEST_CASE(

    "debug_text_backend and text_assembly_target accept instructions with "
    "abstract_operation",
    "[lithe][backend][legality][printable]") {
  using namespace lithe::codegen;
  using namespace lithe::codegen::backends;

  // An instruction carrying an abstract operation ID (printable, no JIT).
  allocated_instruction inst;
  inst.id = 10;
  inst.op = opcode::add;
  inst.abstract_operation = make_operation_id("mylib", "vector_add");
  inst.defs = {allocated_operand::as_preg({0, "r0"})};
  inst.uses = {allocated_operand::as_preg({1, "r1"}),
               allocated_operand::as_preg({2, "r2"})};

  allocated_instruction ret;
  ret.id = 11;
  ret.op = opcode::ret;
  ret.uses = {allocated_operand::as_preg({0, "r0"})};

  allocated_function_ir fn;
  fn.name = "abstract_op_fn";
  fn.cfg.entry_block = 1;
  allocated_basic_block bb;
  bb.id = 1;
  bb.instructions = {inst, ret};
  fn.blocks.push_back(std::move(bb));

  mir::physical_mir_function physical;
  physical.function = std::move(fn);
  physical.metadata.current_phase = mir::phase::physical_mir;

  // debug_text_backend
  {
    debug_text_backend backend;
    const auto result = emit_function(backend, physical);
    REQUIRE(result.ok());
    REQUIRE(result.artifact_text.has_value());
  }

  // text_assembly_target
  {
    text_assembly_target target;
    const auto art = target.emit(physical);
    REQUIRE(art.kind == artifact_kind::assembly_text);
    REQUIRE_FALSE(art.text_payload.empty());
  }
}

// ---------------------------------------------------------------------------
// Test 5 — interpreter target rejects unsupported dynamic operation
//           unless the operation is declared in the registry
// ---------------------------------------------------------------------------
TEST_CASE(

    "interpreter_backend rejects unsupported dynamic operation",
    "[lithe][backend][interpreter][dynamic_op]") {
  using namespace lithe::codegen;
  using namespace lithe::codegen::backends;

  // interpreter_backend does not advertise calls — use that as the
  // "unsupported dynamic operation" stand-in (call is a runtime dispatch).
  REQUIRE_FALSE(
      interpreter_backend::capabilities().has(backend_feature::calls));

  allocated_instruction call_inst;
  call_inst.id = 20;
  call_inst.op = opcode::call;

  allocated_instruction ret;
  ret.id = 21;
  ret.op = opcode::ret;

  allocated_function_ir fn;
  fn.name = "dynamic_op_fn";
  fn.cfg.entry_block = 1;
  allocated_basic_block bb;
  bb.id = 1;
  bb.instructions = {call_inst, ret};
  fn.blocks.push_back(std::move(bb));

  mir::physical_mir_function physical;
  physical.function = std::move(fn);
  physical.metadata.current_phase = mir::phase::physical_mir;

  // Without declaration: backend capability check must reject it.
  interpreter_backend backend;
  const auto result = emit_function(backend, physical);

  REQUIRE_FALSE(result.ok());
  REQUIRE_FALSE(result.errors.empty());
  // The error must mention the problematic operation.
  const bool call_mentioned = std::any_of(
      result.errors.begin(), result.errors.end(), [](const backend_error &e) {
        return e.message.find("call") != std::string::npos ||
               e.message.find("calls") != std::string::npos;
      });
  REQUIRE(call_mentioned);
}

TEST_CASE(

    "interpreter preflight reports every intentionally unsupported opcode",
    "[lithe][backend][interpreter][preflight]") {
  using namespace lithe::codegen;
  using namespace lithe::codegen::backends;

  constexpr std::array unsupported{
      opcode::load_symbol,   opcode::call,         opcode::get_element_ptr,
      opcode::extract_value, opcode::insert_value, opcode::indirect_call,
  };

  STATIC_REQUIRE(interpreter_backend::supports_opcode(opcode::add));
  STATIC_REQUIRE(interpreter_backend::supports_opcode(opcode::branch));
  STATIC_REQUIRE(interpreter_backend::supports_opcode(opcode::branch_cond));

  for (const auto op : unsupported) {
    CAPTURE(interpreter_backend::opcode_name(op));
    REQUIRE_FALSE(interpreter_backend::supports_opcode(op));

    auto physical = make_trivial("unsupported_opcode");
    physical.function.blocks.front().instructions.insert(
        physical.function.blocks.front().instructions.begin(),
        allocated_instruction{.id = 99, .op = op});

    const auto diagnostic =
        interpreter_backend::unsupported_opcode_diagnostic(physical);
    REQUIRE(diagnostic.has_value());
    CHECK(diagnostic->find(interpreter_backend::opcode_name(op)) !=
          std::string::npos);
    CHECK(diagnostic->find("lower it before interpretation") !=
          std::string::npos);
  }
}
