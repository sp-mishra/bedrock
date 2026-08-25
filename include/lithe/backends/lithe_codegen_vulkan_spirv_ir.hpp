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
#include "../lithe_codegen_device.hpp"

namespace lithe::codegen::backends {
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
