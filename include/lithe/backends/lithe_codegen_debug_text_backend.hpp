#pragma once

#include "../lithe_codegen_pipeline.hpp"

namespace lithe::codegen::backends {
    // Emits a readable pseudo-assembly rendering of allocated machine IR.
    struct debug_text_backend {
        std::ostringstream stream;

        static constexpr lithe::plugin_descriptor<
            sizeof("lithe.backend.debug_text"),
            sizeof("Lithe")
        > descriptor{
            .id = "lithe.backend.debug_text",
            .version = {1, 0, 0},
            .author = "Lithe",
            .domain = lithe::semantic::domain_type::unknown,
        };

        static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::floating_arithmetic,
                backend_feature::spill_load_store,
                backend_feature::branches,
                backend_feature::calls,
                backend_feature::memory_operands,
                backend_feature::stack_frame
            });
        }

        static void emit_signature(std::ostringstream& os, const function_signature& signature) {
            os << "  function-signature name=" << (signature.name.empty() ? "anonymous" : signature.name) << "\n";
            os << "    args=" << signature.arguments.size() << " variadic=" << (signature.variadic ? "true" : "false")
                << "\n";
            for (std::size_t i = 0; i < signature.arguments.size(); ++i) {
                const auto loc = get_argument_location(signature, static_cast<std::uint32_t>(i));
                os << "    arg" << i << " ";
                if (loc.physical_register.has_value()) {
                    os << "register=" << loc.physical_register->name;
                }
                else if (loc.stack_slot.has_value()) {
                    os << "stack_slot=" << *loc.stack_slot;
                }
                else {
                    os << "ignored";
                }
                os << " ; reason="
                    << (loc.physical_register.has_value()
                            ? "within argument register budget"
                            : "register budget exhausted or explicit stack passing")
                    << "\n";
            }

            const auto ret = get_return_location(signature);
            os << "    return ";
            if (ret.physical_register.has_value()) {
                os << "register=" << ret.physical_register->name;
            }
            else if (ret.stack_slot.has_value()) {
                os << "stack_slot=" << *ret.stack_slot;
            }
            else {
                os << "void";
            }
            os << "\n";
        }

        [[nodiscard]] backend_result begin_function(const mir::physical_mir_function& fn, backend_state state) {
            stream.str("");
            stream.clear();
            if (state.backend_name.empty()) {
                state.backend_name = "debug_text_backend";
            }
            stream << "function " << fn.function.name << " [physical]\n";

            if (fn.abi.has_value()) {
                stream << "  target-abi name=" << fn.abi->name
                    << " kind=" << static_cast<int>(fn.abi->kind)
                    << " variadic_supported=" << (fn.abi->variadic_supported ? "true" : "false")
                    << " callee_cleans_stack=" << (fn.abi->callee_cleans_stack ? "true" : "false") << "\n";
                stream << "  abi-notes\n";
                stream <<
                    "    generic-vs-target: this dump reflects generic IR lowering with optional target ABI metadata\n";
                stream << "    limitations: no platform-specific quirks are emitted as code in this backend\n";
            }

            if (fn.signature.has_value()) {
                emit_signature(stream, *fn.signature);
            }

            if (fn.frame_layout.has_value()) {
                stream << "  stack-frame-layout\n";
                stream << dump_frame_layout(*fn.frame_layout);
                stream << "  frame-layout-notes\n";
                stream << "    spill-slot assignments and call frame placeholders come from computed frame objects\n";
            }

            if (fn.prologue.has_value()) {
                stream << "  prologue-plan\n";
                stream << dump_prologue_plan(*fn.prologue);
            }

            if (fn.epilogue.has_value()) {
                stream << "  epilogue-plan\n";
                stream << dump_epilogue_plan(*fn.epilogue);
            }

            if (!fn.function.assignments.empty()) {
                stream << "  register-allocation-decisions\n";
                std::vector<std::uint32_t> ids;
                ids.reserve(fn.function.assignments.size());
                for (const auto& [id, _] : fn.function.assignments) {
                    (void)_;
                    ids.push_back(id);
                }
                std::sort(ids.begin(), ids.end());
                for (const auto id : ids) {
                    const auto& assign = fn.function.assignments.at(id);
                    stream << "    v" << id << " -> ";
                    if (assign.physical.has_value()) {
                        stream << assign.physical->name << " ; register reuse/availability decision";
                    }
                    else if (assign.spill.has_value()) {
                        stream << "spill" << assign.spill->id << " ; spilled due to pressure";
                    }
                    else {
                        stream << "<unassigned>";
                    }
                    stream << "\n";
                }
            }

            if (!fn.function.spill_slots.empty()) {
                stream << "  spill-slot-assignments\n";
                for (const auto& slot : fn.function.spill_slots) {
                    stream << "    spill" << slot.id
                        << " size=" << slot.size
                        << " align=" << slot.alignment
                        << " offset=" << slot.frame_offset
                        << " ; frame layout decision" << "\n";
                }
            }

            {
                std::size_t call_count = 0;
                for (const auto& block : fn.function.blocks) {
                    for (const auto& inst : block.instructions) {
                        if (inst.op == opcode::call) {
                            ++call_count;
                        }
                    }
                }
                if (call_count > 0) {
                    stream << "  call-frame-layout\n";
                    stream << "    calls=" << call_count <<
                        " ; outgoing arguments use ABI register-first, stack-overflow model\n";
                }
            }

            if (fn.signature.has_value() || fn.abi.has_value() || fn.frame_layout.has_value()) {
                stream << "  debug-comments\n";
                stream <<
                    "    ABI decisions: arguments map to registers first, overflow to stack per generic convention\n";
                stream << "    Return handling: descriptor-driven register/stack/void mapping\n";
                stream << "    Prologue/Epilogue: computed plans only, no instruction emission\n";
                stream << "    Varargs/Features: represented as metadata placeholders for future backend work\n";
                stream << "    Optimizations/Special cases: tail calls and advanced ABI rewrites are TODO\n";
                stream << "    Known edge cases: aggregates/large structs are not fully specialized yet\n";
                stream << "    Backend interactions: register allocation and spills feed frame layout planning\n";
                stream << "    IR interactions: load_arg/ret semantics remain stable with added ABI annotations\n";
            }

            return backend_result::success_result(std::move(state));
        }

        [[nodiscard]] backend_result begin_function(const allocated_function_ir& fn, backend_state state) {
            stream.str("");
            stream.clear();
            if (state.backend_name.empty()) {
                state.backend_name = "debug_text_backend";
            }
            stream << "function " << fn.name << "\n";
            return backend_result::success_result(std::move(state));
        }

        [[nodiscard]] backend_result begin_block(const allocated_basic_block& block, backend_state state) {
            stream << "  bb" << block.id << " (" << block.name << ")\n";
            return backend_result::success_result(std::move(state));
        }

        [[nodiscard]] backend_result emit_instruction(const allocated_instruction& inst, backend_state state) {
            stream << "    " << to_string(inst.op);
            if (!inst.defs.empty()) {
                stream << " defs=[";
                for (std::size_t i = 0; i < inst.defs.size(); ++i) {
                    stream << dump_allocated_operand(inst.defs[i]);
                    if (i + 1 < inst.defs.size()) {
                        stream << ", ";
                    }
                }
                stream << "]";
            }
            if (!inst.uses.empty()) {
                stream << " uses=[";
                for (std::size_t i = 0; i < inst.uses.size(); ++i) {
                    stream << dump_allocated_operand(inst.uses[i]);
                    if (i + 1 < inst.uses.size()) {
                        stream << ", ";
                    }
                }
                stream << "]";
            }
            if (inst.comment.has_value()) {
                stream << " ; " << *inst.comment;
            }
            stream << "\n";
            return backend_result::success_result(std::move(state));
        }

        [[nodiscard]] backend_result end_function(const allocated_function_ir&, backend_state state) {
            auto out = backend_result::success_result(std::move(state));
            out.artifact_text = stream.str();
            return out;
        }

        [[nodiscard]] backend_result end_function(const mir::physical_mir_function&, backend_state state) {
            auto out = backend_result::success_result(std::move(state));
            out.artifact_text = stream.str();
            return out;
        }

        [[nodiscard]] static emission_target_traits traits() {
            return emission_target_traits{
                .name = "debug_text_backend",
                .input_phase = target_input_phase::physical_mir,
                .produced_artifact = artifact_kind::debug_text,
                .capabilities = capabilities(),
                // Diagnostic/print-only target: accepts any operation domain.
                // Leave supported_operation_domains empty so validate_operation_legality
                // skips the domain check entirely (empty == no restriction).
                .operation_requirements = backend_capability_requirement{
                    .backend_name = "debug_text_backend",
                },
            };
        }

        [[nodiscard]] compilation_artifact emit(mir::physical_mir_function const& fn) {
#if LITHE_HAS_PROFILER
            profiler::ScopedProfiler _prof{"lithe.debug_text.emit"};
#endif
            backend_state state;
            state.backend_name = "debug_text_backend";
            (void)begin_function(fn, state);
            for (const auto& block : fn.function.blocks) {
                (void)begin_block(block, state);
                for (const auto& inst : block.instructions) {
                    (void)emit_instruction(inst, state);
                }
            }
            compilation_artifact art;
            art.kind = artifact_kind::debug_text;
            art.name = fn.function.name;
            art.text_payload = stream.str();
            return art;
        }
    };

    // No-op backend for protocol tests or disabled codegen paths.
    struct null_backend {
        [[nodiscard]] backend_result begin_function(const allocated_function_ir&, backend_state state) {
            if (state.backend_name.empty()) {
                state.backend_name = "null_backend";
            }
            return backend_result::success_result(std::move(state));
        }

        [[nodiscard]] backend_result emit_instruction(const allocated_instruction&, backend_state state) {
            return backend_result::success_result(std::move(state));
        }

        [[nodiscard]] backend_result end_function(const allocated_function_ir&, backend_state state) {
            return backend_result::success_result(std::move(state));
        }
    };

    // -----------------------------------------------------------------------
    // integer_only_target
    //
    // A minimal CodeEmissionTarget that deliberately declares only
    // integer_arithmetic capability.  Its primary purpose is to serve as a
    // demonstration and test fixture for compile-time capability negotiation:
    //
    //   emit_artifact_static<integer_only_target,
    //       make_domain_hint_t<backend_feature::floating_arithmetic>>(t, result);
    //
    // will produce a hard compile error (static_assert) because
    // floating_arithmetic is required by the hint but not provided by this
    // target.  Using the same call with only integer-only hints compiles fine.
    //
    // Constraints satisfied:
    //   - LitheExtension  (has a static constexpr plugin_descriptor)
    //   - CodeEmissionTarget  (traits(), capabilities(), emit())
    //   - No dynamic_cast / RTTI
    // -----------------------------------------------------------------------
    struct integer_only_target {
        // LitheExtension requirement: static constexpr plugin_descriptor.
        static constexpr lithe::plugin_descriptor<
            sizeof("lithe.backend.integer_only"),
            sizeof("Lithe")
        > descriptor{
            .id = "lithe.backend.integer_only",
            .version = {1, 0, 0},
            .author = "Lithe",
            .domain = lithe::semantic::domain_type::arithmetic,
        };

        // Intentionally narrow: only integer arithmetic and branches are
        // supported.  Any MIR that requires floating_arithmetic, spill_load_store,
        // calls, etc. will fail capability negotiation before emission.
        [[nodiscard]] static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::branches,
            });
        }

        [[nodiscard]] static emission_target_traits traits() {
            return emission_target_traits{
                .name = "integer_only_target",
                .input_phase = target_input_phase::physical_mir,
                .produced_artifact = artifact_kind::debug_text,
                .capabilities = capabilities(),
                .operation_requirements = backend_capability_requirement{
                    .backend_name = "integer_only_target",
                },
            };
        }

        // emit() does not attempt to lower floating-point, spill, or call
        // instructions: the capability gate ensures they are never present.
        [[nodiscard]] compilation_artifact emit(mir::physical_mir_function const& fn) {
#if LITHE_HAS_PROFILER
            profiler::ScopedProfiler _prof{"lithe.debug_text.emit"};
#endif
            std::ostringstream os;
            os << "integer_only_target: function " << fn.function.name << "\n";
            for (const auto& block : fn.function.blocks) {
                os << "  block " << block.id << ":\n";
                for (const auto& inst : block.instructions) {
                    os << "    inst " << inst.id
                        << " op=" << static_cast<int>(inst.op) << "\n";
                }
            }
            compilation_artifact art;
            art.kind = artifact_kind::debug_text;
            art.name = fn.function.name;
            art.text_payload = os.str();
            return art;
        }
    };
} // namespace lithe::codegen::backends
