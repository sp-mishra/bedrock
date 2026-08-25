#include "catch_amalgamated.hpp"

#include "lithe/lithe_semantic.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/lithe_lowering.hpp"

// ============================================================================
// Section 1 — Type unification
// ============================================================================

TEST_CASE (



"Type unification: integer widening unifies i32 and i64 to a common equivalence class"
,
"[lithe][semantic][unification]"
)
 {
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    semantic_type_unifier unifier{&reg};
    type_unification_result out;
    const bool ok = unifier.unify(i32, i64, out);

    // Widening integers are structurally compatible and must unify.
    REQUIRE(ok);
    REQUIRE(out.success);
    REQUIRE(out.failed_constraints.empty());

    // After unification the two type_ids are in the same equivalence class
    // inside the unifier's DisjointSet (not the registry's canonicalization map).
    REQUIRE(unifier.are_unified(i32, i64));
}

TEST_CASE (



"Type unification: type variable binds to a concrete type and the binding is unified"
,
"[lithe][semantic][unification]"
)
 {
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");

    semantic_type_unifier unifier{&reg};

    // Create a fresh inference variable and bind it to i32.
    const auto var = unifier.new_variable("T");
    type_unification_result out;
    const bool bound = unifier.bind_variable(var, i32, out);

    REQUIRE(bound);
    REQUIRE(out.success);
    REQUIRE(out.failed_constraints.empty());

    // After binding, the variable's synthetic type_id and i32 are in the same
    // equivalence class.  We can confirm this by attempting a second bind to
    // the same concrete type — the internal unify call must succeed trivially.
    type_unification_result out2;
    const bool rebound = unifier.bind_variable(var, i32, out2);
    REQUIRE(rebound);
    REQUIRE(out2.success);

    // apply_substitutions on the concrete id itself must be a no-op (id is already
    // concrete; canonical returns the DS root, which may be i32 or the synthetic id,
    // but must stay within the same class).
    const auto canon = unifier.canonical(i32);
    REQUIRE(unifier.are_unified(canon, i32));
}

TEST_CASE (



"Type unification: tensor and dynamic query types are structurally incompatible"
,
"[lithe][semantic][unification]"
)
 {
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;

    semantic_type_registry reg;
    const auto f32 = reg.make_float_type(32, "f32");
    const auto tensor = reg.make_tensor_type(f32, {4, 4}, "tensor_4x4");
    const auto query = reg.make_dynamic_type("query");

    semantic_type_unifier unifier{&reg};
    type_unification_result out;
    const bool ok = unifier.unify(tensor, query, out);

    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(out.success);
    REQUIRE_FALSE(out.failed_constraints.empty());
}

// ============================================================================
// Section 2 — Constraint propagation
// ============================================================================

TEST_CASE (



"Constraint propagation: add(i32, i32) infers an integer result type"
,
"[lithe][semantic][constraint][propagation]"
)
 {
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");

    semantic_type_rule_engine engine{&reg};

    // Use infer_expression_type to check the result type of add(i32, i32).
    const auto inferred = engine.infer_expression_type(0, {i32, i32}, "add");

    // The engine must infer a known result type.
    REQUIRE(inferred.inferred);
    REQUIRE(inferred.type_id != invalid_type_id);

    const auto desc = reg.find_type(inferred.type_id);
    REQUIRE(desc.has_value());
    REQUIRE(desc->kind == type_kind::integer);
}

TEST_CASE (



"Constraint propagation: callable constraint unifies parameter type through inference"
,
"[lithe][semantic][constraint][callable]"
)
 {
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    semantic_type_unifier unifier{&reg};

    // A callable subtype constraint: i32 must unify with i64 (widening).
    type_constraint c;
    c.kind = type_constraint_kind::subtype;
    c.lhs = i32;
    c.rhs = i64;
    c.hard_constraint = true;

    type_unification_result out;
    const bool ok = unifier.unify_constraint(c, out);

    REQUIRE(ok);
    REQUIRE(out.success);
    // A subtype constraint check validates the relationship but does not merge
    // equivalence classes — no further structural assertion needed beyond success.
}

TEST_CASE (



"Constraint propagation: subtype constraint is satisfied for i16 narrower than i32"
,
"[lithe][semantic][constraint][subtype]"
)
 {
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;

    semantic_type_registry reg;
    const auto i16 = reg.make_integer_type(16, true, "i16");
    const auto i32 = reg.make_integer_type(32, true, "i32");

    // Direct subtype check via registry.
    REQUIRE(reg.subtype_of(i16, i32));

    // Constraint unification must succeed.
    semantic_type_unifier unifier{&reg};
    type_constraint c;
    c.kind = type_constraint_kind::subtype;
    c.lhs = i16;
    c.rhs = i32;
    c.hard_constraint = true;

    type_unification_result out;
    const bool ok = unifier.unify_constraint(c, out);
    REQUIRE(ok);
    REQUIRE(out.success);
}

// ============================================================================
// Section 3 — Typed lowering
// ============================================================================

TEST_CASE (



"Typed lowering: typed expression preserves type metadata into MIR instruction"
,
"[lithe][semantic][typed_lowering]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::lowering;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");

    typed_ir::typed_expression texpr;
    texpr.expression_key = 0xABCD;
    texpr.inferred_type = i32;
    texpr.effect_metadata.effect = effect_type::pure;
    texpr.effect_metadata.purity_level = purity::pure;

    typed_lowering_context ctx;
    ctx.insert_coercions = false;

    const auto instr = lower_typed_expression(texpr, ctx);

    REQUIRE(instr.has_result_type());
    REQUIRE(instr.primary_result_type() == i32);

    REQUIRE(instr.semantic.has_value());
    REQUIRE(instr.semantic->effect == effect_type::pure);
}

TEST_CASE (



"Typed lowering: widening coercion produces a non-trivial opcode in MIR"
,
"[lithe][semantic][typed_lowering][coercion]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::type_rules;
    using namespace lithe::lowering;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    // Plan a lossless i32 → i64 widening.
    coercion_planner planner{&reg};
    const auto plan = planner.plan(i32, i64);
    REQUIRE(plan.feasible);
    REQUIRE(plan.is_lossless);

    typed_ir::typed_expression texpr;
    texpr.expression_key = 0x1;
    texpr.inferred_type = i64;
    texpr.coercion = plan;
    texpr.effect_metadata.effect = effect_type::pure;

    typed_lowering_context ctx;
    ctx.insert_coercions = true;

    const auto instr = lower_typed_expression(texpr, ctx);

    // The opcode must reflect the coercion, not the no-op "value" opcode.
    REQUIRE_FALSE(instr.opcode.empty());
    REQUIRE(instr.opcode != "value");
    REQUIRE(instr.has_result_type());
    REQUIRE(instr.primary_result_type() == i64);
}

TEST_CASE (



"Typed lowering: typed function preserves parameter and return type metadata"
,
"[lithe][semantic][typed_lowering]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::lowering;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    typed_ir::typed_function fn;
    fn.name = "add_wide";
    fn.parameter_types = {i32, i32};
    fn.return_type = i64;

    typed_ir::typed_expression body_expr;
    body_expr.inferred_type = i64;
    body_expr.effect_metadata.effect = effect_type::pure;
    fn.body.push_back(body_expr);

    typed_lowering_context ctx;
    ctx.insert_coercions = false;

    const auto mir_fn = lower_typed_function(fn, ctx);

    REQUIRE(mir_fn.name == "add_wide");
    REQUIRE(mir_fn.parameter_types.size() == 2);
    REQUIRE(mir_fn.parameter_types[0].type_id == i32);
    REQUIRE(mir_fn.parameter_types[1].type_id == i32);
    REQUIRE(mir_fn.has_return_type());
    REQUIRE(mir_fn.return_type.type_id == i64);
    REQUIRE_FALSE(mir_fn.blocks.empty());
    REQUIRE_FALSE(mir_fn.blocks.front().instructions.empty());
}

// ============================================================================
// Section 4 — Effect system
// ============================================================================

TEST_CASE (



"Effect system: constexpr specialization rejects io_operation effect"
,
"[lithe][semantic][effects][constexpr]"
)
 {
    using namespace lithe::semantic;

    semantic_info io_info;
    io_info.effect = effect_type::io_operation;
    io_info.purity_level = purity::impure;

    semantic_specialization_context ctx;
    ctx.backend_name = "constexpr_test";
    ctx.constexpr_capable = true;
    ctx.runtime_capable = false;

    const auto result = specialize_for_constexpr(io_info, ctx);

    REQUIRE_FALSE(result.legal);
    REQUIRE_FALSE(result.diagnostics.empty());
    REQUIRE_FALSE(result.rejected_effects.empty());
}

TEST_CASE (



"Effect system: runtime specialization accepts io_operation effect"
,
"[lithe][semantic][effects][runtime]"
)
 {
    using namespace lithe::semantic;

    semantic_info io_info;
    io_info.effect = effect_type::io_operation;
    io_info.purity_level = purity::impure;

    semantic_specialization_context ctx;
    ctx.backend_name = "runtime_test";
    ctx.runtime_capable = true;

    const auto result = specialize_for_runtime(io_info, ctx);

    REQUIRE(result.legal);
    REQUIRE(result.rejected_effects.empty());
    REQUIRE(result.specialized.effect == effect_type::io_operation);
}

TEST_CASE (



"Effect system: merging pure and io_operation yields io_operation (monotone lattice)"
,
"[lithe][semantic][effects][merge]"
)
 {
    using namespace lithe::semantic;

    semantic_info pure_info;
    pure_info.effect = effect_type::pure;
    pure_info.purity_level = purity::pure;

    semantic_info io_info;
    io_info.effect = effect_type::io_operation;
    io_info.purity_level = purity::impure;

    const auto merged = merge_semantics(pure_info, io_info);
    REQUIRE(merged.effect == effect_type::io_operation);
    REQUIRE(merged.purity_level == purity::impure);

    // merge_effect is monotone: io_operation > pure in the effect lattice.
    REQUIRE(semantic_info::merge_effect(effect_type::pure, effect_type::io_operation)
        == effect_type::io_operation);
    REQUIRE(semantic_info::merge_effect(effect_type::io_operation, effect_type::pure)
        == effect_type::io_operation);
}

TEST_CASE (



"Effect system: merging pure with read_only yields read_only"
,
"[lithe][semantic][effects][merge]"
)
 {
    using namespace lithe::semantic;

    REQUIRE(semantic_info::merge_effect(effect_type::pure, effect_type::read_only)
        == effect_type::read_only);
    REQUIRE(semantic_info::merge_effect(effect_type::read_only, effect_type::pure)
        == effect_type::read_only);
}

// ============================================================================
// Section 5 — Generic types
// ============================================================================

TEST_CASE (



"Generic types: instantiate_generic_callable produces a concrete callable per type argument"
,
"[lithe][semantic][generics][callable]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::generics;
    using namespace lithe::semantic::callable;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");
    const auto i64 = reg.make_integer_type(64, true, "i64");

    // Generic fn<T>(T)->T with a numeric constraint on T.
    generic_parameter_descriptor param_T;
    param_T.id = generic_parameter_id{0};
    param_T.name = "T";
    generic_constraint c;
    c.constraint_kind = generic_constraint::kind::is_numeric;
    c.hard = true;
    param_T.constraints.push_back(c);

    const std::vector<generic_parameter_descriptor> params = {param_T};

    // Build a generic callable: signature has a placeholder parameter.
    callable_descriptor generic_callee;
    generic_callee.debug_name = "identity";
    generic_callee.signature.parameter_types = {invalid_type_id};
    generic_callee.signature.return_types = {invalid_type_id};

    const std::vector<type_id> args32 = {i32};
    const std::vector<type_id> args64 = {i64};

    auto [callee32, inst32] = instantiate_generic_callable(generic_callee, params, args32, &reg);
    auto [callee64, inst64] = instantiate_generic_callable(generic_callee, params, args64, &reg);

    REQUIRE(inst32.fully_resolved);
    REQUIRE(inst64.fully_resolved);
    REQUIRE(inst32.instantiated_type_id != invalid_type_id);
    REQUIRE(inst64.instantiated_type_id != invalid_type_id);
    // Different type arguments must yield different instantiation ids.
    REQUIRE(inst32.instantiated_type_id != inst64.instantiated_type_id);

    (void) callee32;
    (void) callee64;
}

TEST_CASE (



"Generic types: validate_generic_constraints rejects non-numeric type for is_numeric bound"
,
"[lithe][semantic][generics][constraints]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::generics;

    semantic_type_registry reg;
    const auto query_t = reg.make_dynamic_type("query");

    generic_parameter_descriptor param_T;
    param_T.id = generic_parameter_id{0};
    param_T.name = "T";
    generic_constraint c;
    c.constraint_kind = generic_constraint::kind::is_numeric;
    c.hard = true;
    param_T.constraints.push_back(c);

    const auto result = validate_generic_constraints(param_T, query_t, &reg);

    REQUIRE_FALSE(result.valid);
    REQUIRE_FALSE(result.violations.empty());
}

TEST_CASE (



"Generic types: instantiate_generic_type canonicalizes identical type arguments to the same id"
,
"[lithe][semantic][generics][canonicalize]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::semantic::types;
    using namespace lithe::semantic::generics;

    semantic_type_registry reg;
    const auto i32 = reg.make_integer_type(32, true, "i32");

    generic_parameter_descriptor param_T;
    param_T.id = generic_parameter_id{0};
    param_T.name = "T";

    const std::vector<generic_parameter_descriptor> params = {param_T};
    const std::vector<type_id> type_args = {i32};

    const auto inst_a = instantiate_generic_type("box", params, type_args, &reg);
    const auto inst_b = instantiate_generic_type("box", params, type_args, &reg);

    REQUIRE(inst_a.instantiated_type_id != invalid_type_id);
    REQUIRE(inst_a.instantiated_type_id == inst_b.instantiated_type_id);
}

// ============================================================================
// Section 6 — Backend specialization
// ============================================================================

TEST_CASE (



"Backend specialization: rejects symbolic domain when not in supported domain list"
,
"[lithe][semantic][specialization][backend]"
)
 {
    using namespace lithe::semantic;

    semantic_info symbolic_info;
    symbolic_info.effect = effect_type::pure;
    symbolic_info.domain = domain_type::symbolic;

    semantic_specialization_context ctx;
    ctx.backend_name = "numeric_only_backend";
    ctx.supported_operation_domains = {domain_type::arithmetic};

    const auto result = specialize_for_backend(symbolic_info, ctx);

    REQUIRE_FALSE(result.legal);
    REQUIRE_FALSE(result.rejected_domains.empty());
    REQUIRE(result.rejected_domains.front() == domain_type::symbolic);
}

TEST_CASE (



"Backend specialization: vectorizable capability is narrowed by non-zero max_vector_width"
,
"[lithe][semantic][specialization][backend]"
)
 {
    using namespace lithe::semantic;

    semantic_info tensor_info;
    tensor_info.effect = effect_type::pure;
    tensor_info.domain = domain_type::tensor;
    tensor_info.capabilities.add(capability::vectorizable);

    semantic_specialization_context ctx;
    ctx.backend_name = "narrow_vector_backend";
    ctx.max_vector_width = 4;
    ctx.supported_operation_domains = {domain_type::tensor};

    const auto result = specialize_for_backend(tensor_info, ctx);

    REQUIRE(result.legal);
    REQUIRE_FALSE(result.specialized.capabilities.has(capability::vectorizable));
    REQUIRE_FALSE(result.coerced_types.empty());
}

TEST_CASE (



"Backend specialization: terminates effect is rejected in constexpr context"
,
"[lithe][semantic][specialization][backend]"
)
 {
    using namespace lithe::semantic;

    semantic_info term_info;
    term_info.effect = effect_type::terminates;
    term_info.purity_level = purity::impure;

    semantic_specialization_context ctx;
    ctx.backend_name = "constexpr_only";
    ctx.constexpr_capable = true;
    ctx.runtime_capable = false;
    ctx.supported_effects = {effect_type::pure, effect_type::read_only};

    const auto result = specialize_for_constexpr(term_info, ctx);

    REQUIRE_FALSE(result.legal);
    REQUIRE_FALSE(result.diagnostics.empty());
}

// ============================================================================
// Section 7 — Staged execution
// ============================================================================

TEST_CASE (



"Staged execution: pure deterministic expression infers constexpr_stage"
,
"[lithe][semantic][staged]"
)
 {
    using namespace lithe::semantic;

    semantic_info pure_info;
    pure_info.effect = effect_type::pure;
    pure_info.purity_level = purity::pure;
    pure_info.allocation = allocation_behavior::none;
    pure_info.synchronization = synchronization_behavior::none;

    REQUIRE(infer_execution_stage(pure_info) == execution_stage_kind::constexpr_stage);

    const auto desc = specialize_execution_stage(pure_info,
                                                 execution_stage_kind::deferred_stage, "pure_expr");

    REQUIRE(desc.is_constexpr());
    REQUIRE(desc.inputs_are_static);
    REQUIRE(desc.legal);
}

TEST_CASE (



"Staged execution: io_operation effect infers runtime_stage"
,
"[lithe][semantic][staged]"
)
 {
    using namespace lithe::semantic;

    semantic_info io_info;
    io_info.effect = effect_type::io_operation;
    io_info.purity_level = purity::impure;

    REQUIRE(infer_execution_stage(io_info) == execution_stage_kind::runtime_stage);

    const auto desc = specialize_execution_stage(io_info,
                                                 execution_stage_kind::deferred_stage, "io_op");

    REQUIRE(desc.is_runtime());
    REQUIRE(desc.has_dynamic_effect);
    REQUIRE(desc.legal);
}

TEST_CASE (



"Staged execution: requesting constexpr_stage for an io effect is illegal"
,
"[lithe][semantic][staged]"
)
 {
    using namespace lithe::semantic;

    semantic_info io_info;
    io_info.effect = effect_type::io_operation;
    io_info.purity_level = purity::impure;

    const auto desc = specialize_execution_stage(io_info,
                                                 execution_stage_kind::constexpr_stage, "bad_fold");

    REQUIRE_FALSE(desc.legal);
    REQUIRE_FALSE(desc.diagnostics.empty());
}

TEST_CASE (



"Staged execution: mixed graph partially evaluates with correct per-node stages"
,
"[lithe][semantic][staged][partial]"
)
 {
    using namespace lithe::semantic;

    // A — pure: constexpr-eligible.
    semantic_info a_info;
    a_info.effect = effect_type::pure;
    a_info.purity_level = purity::pure;

    // B — io: must be runtime.
    semantic_info b_info;
    b_info.effect = effect_type::io_operation;
    b_info.purity_level = purity::impure;

    // C — pure with only the constexpr node A as predecessor: stays constexpr.
    semantic_info c_info;
    c_info.effect = effect_type::pure;
    c_info.purity_level = purity::pure;

    // D — fully unknown, depends on runtime B: promoted to runtime.
    semantic_info d_info;

    const lithe::structural_hash_t id_a = 10, id_b = 20, id_c = 30, id_d = 40;

    const std::vector<staged_node_entry> nodes = {
        {id_a, a_info, {}, "A"},
        {id_b, b_info, {}, "B"},
        {id_c, c_info, {id_a}, "C"},
        {id_d, d_info, {id_b}, "D"},
    };

    const auto result = partially_evaluate_staged(nodes);

    REQUIRE(result.stage_of(id_a) == execution_stage_kind::constexpr_stage);
    REQUIRE(result.stage_of(id_b) == execution_stage_kind::runtime_stage);
    REQUIRE(result.stage_of(id_c) == execution_stage_kind::constexpr_stage);
    REQUIRE(result.stage_of(id_d) == execution_stage_kind::runtime_stage);

    REQUIRE(result.illegal_nodes.empty());

    REQUIRE(std::ranges::find(result.constexpr_nodes, id_a) != result.constexpr_nodes.end());
    REQUIRE(std::ranges::find(result.runtime_nodes, id_b) != result.runtime_nodes.end());
    REQUIRE(std::ranges::find(result.constexpr_nodes, id_c) != result.constexpr_nodes.end());
    REQUIRE(std::ranges::find(result.runtime_nodes, id_d) != result.runtime_nodes.end());
}

TEST_CASE (



"Staged execution: pipeline helper classifies registry nodes into constexpr and runtime"
,
"[lithe][semantic][staged][pipeline]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::codegen;

    semantic_registry reg;

    const lithe::structural_hash_t id_pure = 100;
    const lithe::structural_hash_t id_io = 200;

    semantic_info pure_info;
    pure_info.effect = effect_type::pure;
    pure_info.purity_level = purity::pure;

    semantic_info io_info;
    io_info.effect = effect_type::io_operation;
    io_info.purity_level = purity::impure;

    reg.merge(id_pure, pure_info, semantic_resolution{});
    reg.merge(id_io, io_info, semantic_resolution{});

    const std::vector<lithe::structural_hash_t> node_ids = {id_pure, id_io};
    const auto result = specialize_staged_execution_for_pipeline(reg, node_ids);

    REQUIRE(result.ok());
    REQUIRE(result.stage_of(id_pure) == execution_stage_kind::constexpr_stage);
    REQUIRE(result.stage_of(id_io) == execution_stage_kind::runtime_stage);

    REQUIRE(std::ranges::find(result.constexpr_nodes, id_pure) != result.constexpr_nodes.end());
    REQUIRE(std::ranges::find(result.runtime_nodes, id_io) != result.runtime_nodes.end());
}

TEST_CASE (



"Staged execution: partially_evaluate_registry_staged promotes deferred node with runtime predecessor"
,
"[lithe][semantic][staged][pipeline]"
)
 {
    using namespace lithe::semantic;
    using namespace lithe::codegen;

    semantic_registry reg;

    const lithe::structural_hash_t id_root = 1;
    const lithe::structural_hash_t id_child = 2;

    semantic_info root_info;
    root_info.effect = effect_type::io_operation;
    root_info.purity_level = purity::impure;

    semantic_info child_info; // fully unknown — deferred by default

    reg.merge(id_root, root_info, semantic_resolution{});
    reg.merge(id_child, child_info, semantic_resolution{});

    const std::vector<lithe::structural_hash_t> node_ids = {id_root, id_child};
    const std::unordered_map<lithe::structural_hash_t,
        std::vector<lithe::structural_hash_t> > preds = {
        {id_child, {id_root}}
    };

    const auto result = partially_evaluate_registry_staged(reg, node_ids, preds);

    REQUIRE(result.stage_of(id_root) == execution_stage_kind::runtime_stage);
    REQUIRE(result.stage_of(id_child) == execution_stage_kind::runtime_stage);
}

// ============================================================================
// Section 8 — easy_rules_optimization_advisor
// ============================================================================

TEST_CASE (



"easy_rules_optimization_advisor: low-complexity summary selects fast_tiered pipeline"
,
"[lithe][codegen][advisor][easy_rules]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::semantic;

    easy_rules_optimization_advisor advisor;

    // Score = 0: no loops, no GEPs, no blocks.
    compilation_complexity_summary s;
    semantic_info sem;

    const auto choice = advisor.choose_pipeline(s, sem);
    REQUIRE(choice == advisor_pipeline_choice::fast_tiered);
}

TEST_CASE (



"easy_rules_optimization_advisor: heuristic_score >= 32 selects heavy_aggressive pipeline"
,
"[lithe][codegen][advisor][easy_rules]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::semantic;

    easy_rules_optimization_advisor advisor;

    // back_edge_count = 4 → loop_weight = 4*4 = 16; loop_depth_max = 4 → 2*4 = 8; total = 24
    // block_count = 8 → cfg_weight = 8; grand total = 32 → triggers heavy_aggressive rule.
    compilation_complexity_summary s;
    s.back_edge_count = 4;
    s.loop_depth_max  = 4;
    s.block_count     = 8;

    semantic_info sem;
    const auto choice = advisor.choose_pipeline(s, sem);
    REQUIRE(choice == advisor_pipeline_choice::heavy_aggressive);
}

TEST_CASE (



"easy_rules_optimization_advisor: gep_instructions >= 4 selects sroa_then_conservative"
,
"[lithe][codegen][advisor][easy_rules]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::semantic;

    easy_rules_optimization_advisor advisor;

    // Score well below 32 but gep_instructions >= 4 → sroa_then_conservative.
    compilation_complexity_summary s;
    s.gep_instructions = 4;
    // heuristic_score = 3*4 = 12 (below 32).

    semantic_info sem;
    const auto choice = advisor.choose_pipeline(s, sem);
    REQUIRE(choice == advisor_pipeline_choice::sroa_then_conservative);
}

TEST_CASE (



"easy_rules_optimization_advisor: heavy_aggressive wins over sroa when both thresholds met"
,
"[lithe][codegen][advisor][easy_rules]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::semantic;

    easy_rules_optimization_advisor advisor;

    // Both thresholds exceeded: heavy_aggressive has priority 10, sroa has 5.
    compilation_complexity_summary s;
    s.back_edge_count  = 4;
    s.loop_depth_max   = 4;
    s.block_count      = 8;   // score = 32 → heavy_aggressive fires
    s.gep_instructions = 4;   // sroa rule also fires, but lower priority

    semantic_info sem;
    const auto choice = advisor.choose_pipeline(s, sem);
    REQUIRE(choice == advisor_pipeline_choice::heavy_aggressive);
}

TEST_CASE (



"easy_rules_optimization_advisor: should_run_pass returns true by default"
,
"[lithe][codegen][advisor][easy_rules]"
)
 {
    using namespace lithe::codegen;

    easy_rules_optimization_advisor advisor;
    compilation_complexity_summary s;

    REQUIRE(advisor.should_run_pass("any_pass", s));
    REQUIRE(advisor.should_run_pass("peephole", s));
    REQUIRE(advisor.should_run_pass("dce", s));
}

TEST_CASE (



"easy_rules_optimization_advisor: custom rule can suppress a named pass"
,
"[lithe][codegen][advisor][easy_rules]"
)
 {
    using namespace lithe::codegen;

    easy_rules_optimization_advisor advisor;

    // Inject a rule that sets run_pass=false for "expensive_pass".
    advisor.engine.when("suppress_expensive",
        [](const easy_rules::Facts &f) {
            auto name = f.get<std::string>("pass_name");
            return name.has_value() && *name == "expensive_pass";
        })
        .then([](easy_rules::ExecutionContext &c) {
            c.facts.set("run_pass", false);
        })
        .with_priority(50);

    compilation_complexity_summary s;
    REQUIRE_FALSE(advisor.should_run_pass("expensive_pass", s));
    REQUIRE(advisor.should_run_pass("cheap_pass", s));
}

TEST_CASE (



"easy_rules_optimization_advisor: custom pipeline rule overrides default selection"
,
"[lithe][codegen][advisor][easy_rules]"
)
 {
    using namespace lithe::codegen;
    using namespace lithe::semantic;

    easy_rules_optimization_advisor advisor;

    // Force sroa_then_conservative regardless of score by injecting a top-priority rule.
    advisor.engine.when("force_sroa",
        [](const easy_rules::Facts &) { return true; })
        .then([](easy_rules::ExecutionContext &c) {
            c.facts.set("pipeline", std::string("sroa_then_conservative"));
        })
        .with_priority(100);

    compilation_complexity_summary s; // zero score — normally fast_tiered
    semantic_info sem;

    const auto choice = advisor.choose_pipeline(s, sem);
    REQUIRE(choice == advisor_pipeline_choice::sroa_then_conservative);
}
