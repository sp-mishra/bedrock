#pragma once

#include "../lithe_codegen_pipeline.hpp"

namespace lithe::codegen::backends {
    // -----------------------------------------------------------------------
    // assembler_options — configuration passed to an assembler at construction
    // or before begin_function.
    // -----------------------------------------------------------------------

    struct assembler_options {
        std::string function_name;
        bool emit_comments = false;
        std::string target_triple; // empty = target-independent
    };

    // -----------------------------------------------------------------------
    // assembler_result — outcome of a single assembler operation (not the
    // full finalize result; that is compilation_artifact).
    // -----------------------------------------------------------------------

    struct assembler_result {
        bool ok = true;
        std::string diagnostic;

        [[nodiscard]] static assembler_result success() { return {true, {}}; }

        [[nodiscard]] static assembler_result fail(std::string msg) {
            return {false, std::move(msg)};
        }
    };

    // -----------------------------------------------------------------------
    // text_assembly_target
    //
    // A non-JIT CodeEmissionTarget that consumes physical MIR and emits
    // human-readable pseudo-assembly text.  Returned artifact has
    // kind = assembly_text.
    //
    // Rules: pseudo assembly only — no real ISA, no object file, no linker.
    // -----------------------------------------------------------------------

    struct text_assembly_target {
        std::ostringstream stream;

        static constexpr lithe::plugin_descriptor<
            sizeof("lithe.backend.text_assembly"),
            sizeof("Lithe")
        > descriptor{
            .id = "lithe.backend.text_assembly",
            .version = {1, 0, 0},
            .author = "Lithe",
            .domain = lithe::semantic::domain_type::unknown,
        };

        [[nodiscard]] static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::floating_arithmetic,
                backend_feature::spill_load_store,
                backend_feature::branches,
                backend_feature::calls,
                backend_feature::memory_operands,
                backend_feature::stack_frame,
            });
        }

        [[nodiscard]] static emission_target_traits traits() {
            return emission_target_traits{
                .name = "text_assembly_target",
                .input_phase = target_input_phase::physical_mir,
                .produced_artifact = artifact_kind::assembly_text,
                .capabilities = capabilities(),
                .operation_requirements = backend_capability_requirement{
                    .backend_name = "text_assembly_target",
                    .supported_operation_domains = {"lithe.core"},
                },
            };
        }

        [[nodiscard]] compilation_artifact emit(mir::physical_mir_function const& fn) {
#if LITHE_HAS_PROFILER
            profiler::ScopedProfiler _prof{"lithe.asm.emit"};
#endif
            stream.str("");
            stream.clear();

            stream << fn.function.name << ":\n";

            for (const auto& block : fn.function.blocks) {
                stream << "  .bb" << block.id;
                if (!block.name.empty()) {
                    stream << " ; " << block.name;
                }
                stream << "\n";

                for (const auto& inst : block.instructions) {
                    emit_one(inst);
                }
            }

            compilation_artifact art;
            art.kind = artifact_kind::assembly_text;
            art.name = fn.function.name;
            art.text_payload = stream.str();
            return art;
        }

    private:
        void emit_one(allocated_instruction const& inst) {
            stream << "    " << to_string(inst.op);

            if (!inst.defs.empty()) {
                stream << " defs=[";
                for (std::size_t i = 0; i < inst.defs.size(); ++i) {
                    if (i) stream << ", ";
                    stream << dump_allocated_operand(inst.defs[i]);
                }
                stream << "]";
            }

            if (!inst.uses.empty()) {
                stream << " uses=[";
                for (std::size_t i = 0; i < inst.uses.size(); ++i) {
                    if (i) stream << ", ";
                    stream << dump_allocated_operand(inst.uses[i]);
                }
                stream << "]";
            }

            if (inst.comment.has_value()) {
                stream << " ; " << *inst.comment;
            }
            stream << "\n";
        }
    };

    static_assert(CodeEmissionTarget<text_assembly_target>,
                  "text_assembly_target must satisfy CodeEmissionTarget");
} // namespace lithe::codegen::backends
