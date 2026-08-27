#pragma once

// =============================================================================
// backends/lithe_codegen_vulkan_spirv_ir.hpp — SPIR-V-lowered kernel-IR adapter
//
// The backend-lowered IR stage for the Vulkan/MoltenVK device backend.
// Lives BESIDE the backend: the generic lithe::ir umbrella must stay
// free of any SPIR-V/Vulkan coupling.  This adapter maps the engine's kernel IR
// down to a SPIR-V module that the backend's vkCreateShaderModule consumes.
//
// Provides:
//   spirv_module        — the 32-bit-word SPIR-V blob + LocalSize + identity hash.
//   spirv_ir_provider   — models the generic lithe::ir provider CPOs
//                         (export_text / export_binary / validate_ir) via ADL, so
//                         SPIR-V modules ride the same print/store/validate paths
//                         as every other IR — no new CPOs, lithe::ir uncoupled.
//
// Structural validation only (not semantic re-typing): magic word, an OpEntryPoint,
// an OpExecutionMode LocalSize, and OpCapability within MoltenVK's supported set.
// No external SPIR-V library — a small forward word-walker parses the module.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "../lithe_ir/provider.hpp"   // ir_resolution_state, cpo::{export_text,export_binary,validate_ir}, ir_error
#include "../lithe_ir/format.hpp"     // format_descriptor
#include "../lithe_ir/integration.hpp" // owned_text_ir / owned_binary_ir (export result payloads)
#include "../lithe_codegen_hl_passes.hpp"
#include "../lithe_execution_admission.hpp"
#include "../lithe_codegen_device.hpp"

namespace lithe::codegen::backends {
    enum class vulkan_plan_disposition : std::uint8_t { spirv_compatible, scalar_fallback };

    // A non-owning bridge between generic loop planning and the existing
    // Vulkan/MoltenVK SPIR-V kernel contract.  It has no Vulkan types so it is
    // available to portable callers even when the optional provider is absent.
    struct vulkan_plan_binding {
        vulkan_plan_disposition disposition = vulkan_plan_disposition::scalar_fallback;
        std::uint32_t planned_lanes = 0;
        std::uint32_t local_x = 0;
        hl::vector_tail_strategy tail = hl::vector_tail_strategy::scalar_fallback;

        [[nodiscard]] constexpr bool compatible() const noexcept {
            return disposition == vulkan_plan_disposition::spirv_compatible;
        }
    };

    [[nodiscard]] inline bool supports_spirv_elementwise_plan(
        const device::kernel_plan& plan) noexcept {
        return plan.elementwise_dispatch_compatible()
            && plan.element_type == device::scalar_type::f32
            && plan.readable_binding_count() == 2
            && plan.writable_binding_count() == 1
            && std::ranges::all_of(plan.bindings, [](const device::kernel_binding& binding) {
                return binding.view.rank == 1 && binding.view.contiguous;
            });
    }

    // This binding does not probe a VkDevice: provider creation remains the
    // optional runtime step.  A compatible result instead means the plan is
    // admissible by the checked-in SPIR-V ABI, so a failed runtime install can
    // safely select the same explicit fallback as any other provider failure.
    [[nodiscard]] inline vulkan_plan_binding bind_vector_plan_for_vulkan(
        const hl::vector_plan& vector_plan,
        const device::kernel_plan& kernel_plan) noexcept {
        vulkan_plan_binding binding{
            .planned_lanes = vector_plan.lanes,
            .local_x = kernel_plan.launch.local_x,
            .tail = vector_plan.tail,
        };
        if (vector_plan.legality == hl::vector_plan_legality::proven
            && vector_plan.schedule_materialized
            && vector_plan.element_bits == 32
            && vector_plan.reduction == hl::vector_reduction_shape::none
            && supports_spirv_elementwise_plan(kernel_plan)) {
            binding.disposition = vulkan_plan_disposition::spirv_compatible;
        }
        return binding;
    }

    [[nodiscard]] constexpr hl::execution_backend_admission admit_vulkan_plan(
        const vulkan_plan_binding& binding, const bool provider_available) noexcept {
        return {.kind = hl::planned_execution_kind::vulkan,
                .plan_admitted = binding.compatible(),
                .provider_available = provider_available,
                .reason = binding.compatible() ? (provider_available
                        ? hl::execution_admission_reason::admitted
                        : hl::execution_admission_reason::provider_unavailable)
                    : hl::execution_admission_reason::plan_rejected};
    }

    // =========================================================================
    // SPIR-V opcodes we recognise structurally (SPIR-V spec, unified opcode table)
    // =========================================================================
    inline constexpr std::uint32_t k_spirv_magic = 0x07230203u;
    inline constexpr std::uint16_t k_op_entry_point = 15u; // OpEntryPoint
    inline constexpr std::uint16_t k_op_execution_mode = 16u; // OpExecutionMode
    inline constexpr std::uint16_t k_op_capability = 17u; // OpCapability
    inline constexpr std::uint32_t k_exec_mode_localsize = 17u; // LocalSize (OpExecutionMode operand)
    inline constexpr std::uint32_t k_capability_shader = 1u; // Shader (MoltenVK-supported)
    inline constexpr std::size_t k_spirv_header_words = 5u; // magic,version,generator,bound,schema

    // =========================================================================
    // spirv_module — a compute SPIR-V module ready for vkCreateShaderModule.
    // =========================================================================
    struct spirv_module {
        std::vector<std::uint32_t> words; // 32-bit little-endian SPIR-V
        std::uint32_t local_x = 1, local_y = 1, local_z = 1; // OpExecutionMode LocalSize

        [[nodiscard]] bool valid() const noexcept { return !words.empty(); }

        // FNV-1a over the word blob — deterministic identity, the pipeline-cache key.
        [[nodiscard]] std::uint64_t identity_hash() const noexcept {
            std::uint64_t h = 1469598103934665603ull;
            for (const std::uint32_t w : words) {
                h ^= static_cast<std::uint64_t>(w);
                h *= 1099511628211ull;
            }
            return h;
        }

        // Structural validation.  Populates local_{x,y,z} from OpExecutionMode
        // LocalSize when present.  Returns the resolution verdict:
        //   resolved                        — well-formed compute module.
        //   unresolved_required_operations  — bad magic / truncated / no entry point
        //                                      / unsupported capability.
        [[nodiscard]] ir::ir_resolution_state validate() noexcept {
            using ir::ir_resolution_state;
            if (words.size() < k_spirv_header_words || words[0] != k_spirv_magic)
                return ir_resolution_state::unresolved_required_operations;

            bool have_entry_point = false;
            std::size_t i = k_spirv_header_words;
            while (i < words.size()) {
                const std::uint32_t inst = words[i];
                const std::uint16_t opcode = static_cast<std::uint16_t>(inst & 0xFFFFu);
                const std::uint16_t wcount = static_cast<std::uint16_t>(inst >> 16);
                if (wcount == 0 || i + wcount > words.size())
                    return ir_resolution_state::unresolved_required_operations; // truncated / malformed

                switch (opcode) {
                case k_op_entry_point:
                    have_entry_point = true;
                    break;
                case k_op_capability:
                    // words[i+1] = capability id.  Reject anything MoltenVK
                    // cannot lower (only Shader accepted structurally).
                    if (wcount >= 2 && words[i + 1] != k_capability_shader)
                        return ir_resolution_state::unresolved_required_operations;
                    break;
                case k_op_execution_mode:
                    // layout: [inst] [entry-point id] [mode] [operands...]
                    if (wcount >= 6 && words[i + 2] == k_exec_mode_localsize) {
                        local_x = words[i + 3];
                        local_y = words[i + 4];
                        local_z = words[i + 5];
                    }
                    break;
                default:
                    break;
                }
                i += wcount;
            }
            return have_entry_point
                       ? ir_resolution_state::resolved
                       : ir_resolution_state::unresolved_required_operations;
        }
    };

    enum class spirv_binary_operation : std::uint8_t { add, multiply };

    // Generic three-buffer f32 ABI: output[i] = lhs[i] op rhs[i]. The distinct
    // block types preserve MoltenVK/SPIRV-Cross binding behaviour.
    [[nodiscard]] inline spirv_module emit_spirv_binary_elementwise(
        const spirv_binary_operation operation, const std::uint32_t local_x = 64) {
        using word = std::uint32_t;
        constexpr word main = 1, void_t = 2, fn = 3, uint_t = 4, float_t = 5, v3uint = 6,
            ptr_in_v3 = 7, giid = 8, uint0 = 9, ptr_in_uint = 10, pgiid = 11,
            array = 12, sa = 13, sb = 14, sc = 15, ptra = 16, ptrb = 17, ptrc = 18,
            a = 19, b = 20, c = 21, ptrf = 22, int_t = 23, int0 = 24, label = 25,
            index = 26, pa = 27, pb = 28, pc = 29, va = 30, vb = 31, result = 32, bound = 33;
        std::vector<word> w{0x07230203u, 0x00010000u, 0u, bound, 0u};
        const auto emit = [&w](word op, std::initializer_list<word> args) {
            w.push_back((static_cast<word>(args.size() + 1) << 16) | op);
            w.insert(w.end(), args);
        };
        emit(17, {1}); emit(14, {0, 1});
        w.insert(w.end(), {(6u << 16) | 15u, 5u, main, 0x6e69616du, 0u, giid});
        emit(16, {main, 17u, local_x, 1u, 1u}); emit(71, {giid, 11u, 28u}); emit(71, {array, 6u, 4u});
        for (word s : {sa, sb, sc}) { emit(72, {s, 0u, 35u, 0u}); emit(71, {s, 2u}); }
        const auto decorate = [&emit](word var, word slot) { emit(71, {var, 34u, 0u}); emit(71, {var, 33u, slot}); };
        decorate(a, 0); decorate(b, 1); decorate(c, 2);
        emit(19, {void_t}); emit(33, {fn, void_t}); emit(21, {uint_t, 32u, 0u}); emit(21, {int_t, 32u, 1u});
        emit(22, {float_t, 32u}); emit(23, {v3uint, uint_t, 3u}); emit(32, {ptr_in_v3, 1u, v3uint}); emit(59, {ptr_in_v3, giid, 1u});
        emit(43, {uint_t, uint0, 0u}); emit(32, {ptr_in_uint, 1u, uint_t}); emit(29, {array, float_t});
        emit(30, {sa, array}); emit(30, {sb, array}); emit(30, {sc, array}); emit(32, {ptra, 12u, sa}); emit(32, {ptrb, 12u, sb}); emit(32, {ptrc, 12u, sc});
        emit(59, {ptra, a, 12u}); emit(59, {ptrb, b, 12u}); emit(59, {ptrc, c, 12u}); emit(32, {ptrf, 12u, float_t}); emit(43, {int_t, int0, 0u});
        emit(54, {void_t, main, 0u, fn}); emit(248, {label}); emit(65, {ptr_in_uint, pgiid, giid, uint0}); emit(61, {uint_t, index, pgiid});
        emit(65, {ptrf, pa, a, int0, index}); emit(61, {float_t, va, pa}); emit(65, {ptrf, pb, b, int0, index}); emit(61, {float_t, vb, pb});
        emit(operation == spirv_binary_operation::add ? 129u : 133u, {float_t, result, va, vb}); emit(65, {ptrf, pc, c, int0, index}); emit(62, {pc, result}); emit(253, {}); emit(56, {});
        return {.words = std::move(w), .local_x = local_x};
    }

    // =========================================================================
    // spirv_ir_provider — routes SPIR-V modules through the generic lithe::ir
    // provider CPOs.  ADL finds these tag_invoke overloads on spirv_ir_provider.
    // =========================================================================
    struct spirv_ir_provider {
        // validate_ir — structural verdict (const query; does not mutate the module,
        // so local sizes must have been populated at construction/compile time).
        [[nodiscard]] friend ir::ir_resolution_state
        tag_invoke(ir::cpo::validate_ir_t,
                   const spirv_ir_provider& /*prov*/,
                   const spirv_module& mod) noexcept {
            spirv_module copy = mod; // validate() populates LocalSize; keep the CPO const-correct
            return copy.validate();
        }

        // export_binary — the 32-bit-word module, well-formedness re-checked.
        [[nodiscard]] friend std::expected<ir::owned_binary_ir, ::lithe::execution::ir_error>
        tag_invoke(ir::cpo::export_binary_t,
                   const spirv_ir_provider& /*prov*/,
                   const spirv_module& mod,
                   ir::format_descriptor fmt) {
            if (mod.words.empty() || mod.words[0] != k_spirv_magic)
                return std::unexpected(::lithe::execution::ir_error{"spirv: bad magic / empty module"});
            // Byte-view over the word blob (little-endian, as SPIR-V requires).
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(mod.words.data());
            const std::size_t nbytes = mod.words.size() * sizeof(std::uint32_t);
            return ir::owned_binary_ir{std::vector<std::uint8_t>(bytes, bytes + nbytes), fmt};
        }

        // export_text — minimal SPIR-V disassembly header (print/debug path).
        [[nodiscard]] friend std::expected<ir::owned_text_ir, ::lithe::execution::ir_error>
        tag_invoke(ir::cpo::export_text_t,
                   const spirv_ir_provider& /*prov*/,
                   const spirv_module& mod,
                   ir::format_descriptor fmt) {
            if (mod.words.empty())
                return std::unexpected(::lithe::execution::ir_error{"spirv: empty module"});
            std::string out = "; SPIR-V module\n; magic=0x";
            // hex of the magic word
            const char* hex = "0123456789abcdef";
            for (int shift = 28; shift >= 0; shift -= 4)
                out.push_back(hex[(mod.words[0] >> shift) & 0xF]);
            out += "\n; words=" + std::to_string(mod.words.size());
            out += "\n; local_size=" + std::to_string(mod.local_x) + " "
                + std::to_string(mod.local_y) + " " + std::to_string(mod.local_z) + "\n";
            return ir::owned_text_ir{std::vector<char>(out.begin(), out.end()), fmt};
        }
    };
} // namespace lithe::codegen::backends
