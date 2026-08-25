#pragma once

// Backend-neutral GPU legality and binding analysis over existing HL-MIR.
// This is a non-owning plan: operations remain owned by hl_mir_function and no
// second instruction representation is introduced.

#include "lithe_codegen.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lithe::codegen::device {
    enum class scalar_type : std::uint8_t { f16, f32, i32, u32 };
    enum class binding_access : std::uint8_t { read, write, read_write };

    [[nodiscard]] constexpr std::string_view to_string(const scalar_type type) noexcept {
        switch (type) {
        case scalar_type::f16: return "f16";
        case scalar_type::f32: return "f32";
        case scalar_type::i32: return "i32";
        case scalar_type::u32: return "u32";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view msl_name(const scalar_type type) noexcept {
        switch (type) {
        case scalar_type::f16: return "half";
        case scalar_type::f32: return "float";
        case scalar_type::i32: return "int";
        case scalar_type::u32: return "uint";
        }
        return "void";
    }

    [[nodiscard]] constexpr std::optional<scalar_type> scalar_type_of(const hl::memref_type& view) noexcept {
        if (view.elem_kind == abstract_value_kind::floating && view.elem_bits == 16) return scalar_type::f16;
        if (view.elem_kind == abstract_value_kind::floating && view.elem_bits == 32) return scalar_type::f32;
        if (view.elem_kind == abstract_value_kind::integer && view.elem_bits == 32) return scalar_type::i32;
        return std::nullopt;
    }

    struct kernel_binding {
        ssa_value_id base_value{};
        hl::memref_type view{};
        binding_access access = binding_access::read;
        std::uint32_t index = 0;

        [[nodiscard]] constexpr bool readable() const noexcept {
            return access == binding_access::read || access == binding_access::read_write;
        }

        [[nodiscard]] constexpr bool writable() const noexcept {
            return access == binding_access::write || access == binding_access::read_write;
        }
    };

    struct launch_geometry {
        std::uint32_t global_x = 1;
        std::uint32_t global_y = 1;
        std::uint32_t global_z = 1;
        std::uint32_t local_x = 64;
        std::uint32_t local_y = 1;
        std::uint32_t local_z = 1;
        bool dynamic_global_x = false;
    };

    struct kernel_requirements {
        bool reduction = false;
        bool loop_carried_values = false;
        bool non_contiguous_memory = false;
        bool dynamic_shape = false;
        bool control_flow = false;
        bool atomics = false;
        bool workgroup_memory = false;
    };

    struct kernel_plan {
        const hl::hl_mir_function* source = nullptr;
        const hl::hl_operation* root = nullptr;
        std::vector<const hl::hl_operation*> operations;
        std::vector<kernel_binding> bindings;
        launch_geometry launch{};
        kernel_requirements requirements{};
        std::optional<scalar_type> element_type;
        std::uint64_t identity = 0;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return source != nullptr && root != nullptr && element_type.has_value()
                && !operations.empty() && !bindings.empty() && diagnostics.empty();
        }

        [[nodiscard]] const kernel_binding* binding_for(const ssa_value_id value) const noexcept {
            const auto it = std::ranges::find(bindings, value, &kernel_binding::base_value);
            return it == bindings.end() ? nullptr : std::addressof(*it);
        }

        [[nodiscard]] std::size_t readable_binding_count() const noexcept {
            return static_cast<std::size_t>(std::ranges::count_if(bindings, &kernel_binding::readable));
        }

        [[nodiscard]] std::size_t writable_binding_count() const noexcept {
            return static_cast<std::size_t>(std::ranges::count_if(bindings, &kernel_binding::writable));
        }

        [[nodiscard]] bool elementwise_dispatch_compatible() const noexcept {
            return valid() && !requirements.reduction && !requirements.control_flow
                && !requirements.atomics && !requirements.workgroup_memory
                && !requirements.non_contiguous_memory
                && writable_binding_count() == 1
                && std::ranges::none_of(bindings, [](const kernel_binding& binding) {
                    return binding.access == binding_access::read_write;
                });
        }
    };

    struct legalization_options {
        std::uint32_t preferred_local_x = 64;
        bool allow_reductions = true;
        bool allow_non_contiguous_memory = true;
        bool require_parallel_root = true;
    };

    namespace detail {
        [[nodiscard]] constexpr bool is_scalar_compute(const hl::hl_opcode op) noexcept {
            using enum hl::hl_opcode;
            switch (op) {
            case fadd: case fsub: case fmul: case fdiv: case fneg:
            case add: case sub: case mul: case div:
            case icmp: case fcmp: case select:
            case sdiv: case udiv: case srem: case urem:
            case bit_and: case bit_or: case bit_xor: case bit_not:
            case shl: case lshr: case ashr:
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] constexpr bool is_gpu_legal_operation(const hl::hl_opcode op) noexcept {
            using enum hl::hl_opcode;
            return is_scalar_compute(op) || op == structured_for || op == structured_reduce
                || op == region_yield || op == loop_index || op == memref_load
                || op == memref_store || op == constant || op == argument || op == ret;
        }

        template <class Fn>
        void visit_region(const hl::hl_region& region, Fn&& visitor) {
            for (auto* block = region.blocks.head; block != nullptr; block = block->list_node.next) {
                for (auto* op = block->ops.head; op != nullptr; op = op->list_node.next) {
                    visitor(*op);
                    for (const auto* nested : op->regions) {
                        if (nested != nullptr) visit_region(*nested, visitor);
                    }
                }
            }
        }

        [[nodiscard]] inline binding_access merge_access(const binding_access lhs,
                                                         const binding_access rhs) noexcept {
            return lhs == rhs ? lhs : binding_access::read_write;
        }

        [[nodiscard]] inline std::uint64_t mix(std::uint64_t state, const std::uint64_t value) noexcept {
            state ^= value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
            return state;
        }
    } // namespace detail

    [[nodiscard]] inline kernel_plan analyze_kernel(const hl::hl_mir_function& function,
                                                    const legalization_options options = {}) {
        kernel_plan plan;
        plan.source = std::addressof(function);
        plan.launch.local_x = std::max<std::uint32_t>(1, options.preferred_local_x);

        std::vector<const hl::hl_operation*> candidates;
        detail::visit_region(function.body_region, [&](const hl::hl_operation& op) {
            if (op.op != hl::hl_opcode::structured_for && op.op != hl::hl_opcode::structured_reduce) return;
            if (!std::holds_alternative<hl::structured_for_attr>(op.attr)) return;
            const auto& attr = std::get<hl::structured_for_attr>(op.attr);
            if (attr.is_parallel) candidates.push_back(std::addressof(op));
        });

        if (candidates.empty()) {
            plan.diagnostics.push_back("device: no parallel structured kernel region found");
            return plan;
        }
        if (candidates.size() != 1) {
            plan.diagnostics.push_back("device: kernel extraction currently requires exactly one parallel root region");
            return plan;
        }
        plan.root = candidates.front();
        if (plan.root->regions.size() != 1 || plan.root->regions.front() == nullptr) {
            plan.diagnostics.push_back("device: parallel kernel root must own exactly one body region");
            return plan;
        }

        const auto& loop = std::get<hl::structured_for_attr>(plan.root->attr);
        plan.requirements.loop_carried_values = !plan.root->operands.empty()
            || !plan.root->results.empty();
        plan.requirements.reduction |= plan.requirements.loop_carried_values;
        if (loop.rank != 1) {
            plan.diagnostics.push_back("device: initial shared lowering supports one-dimensional parallel roots");
        }
        if (loop.bounds_known && loop.bounds[0].step > 0 && loop.bounds[0].upper > loop.bounds[0].lower) {
            const auto extent = static_cast<std::uint64_t>(loop.bounds[0].upper - loop.bounds[0].lower);
            const auto step = static_cast<std::uint64_t>(loop.bounds[0].step);
            plan.launch.global_x = static_cast<std::uint32_t>((extent + step - 1) / step);
        }
        else if (loop.trip_count_hint > 0) {
            plan.launch.global_x = static_cast<std::uint32_t>(loop.trip_count_hint);
        }
        else {
            plan.launch.dynamic_global_x = true;
        }

        std::unordered_map<std::uint64_t, std::size_t> binding_by_value;
        const auto add_binding = [&](const hl::hl_operation& op, const binding_access access) {
            if (!std::holds_alternative<hl::memref_attr>(op.attr)) {
                plan.diagnostics.push_back("device: memref operation #" + std::to_string(op.id) + " has no memref_attr");
                return;
            }
            const auto& attr = std::get<hl::memref_attr>(op.attr);
            if (attr.base_operand_index < 0
                || static_cast<std::size_t>(attr.base_operand_index) >= op.operands.size()) {
                plan.diagnostics.push_back("device: memref operation #" + std::to_string(op.id)
                    + " has no valid base SSA operand");
                return;
            }
            const auto base = op.operands[static_cast<std::size_t>(attr.base_operand_index)];
            if (!base.valid()) {
                plan.diagnostics.push_back("device: memref operation #" + std::to_string(op.id)
                    + " references an invalid base SSA value");
                return;
            }
            const auto scalar = scalar_type_of(attr.view);
            if (!scalar) {
                plan.diagnostics.push_back("device: memref operation #" + std::to_string(op.id)
                    + " uses an unsupported element type; supported types are f16, f32 and i32");
                return;
            }
            if (plan.element_type && plan.element_type != scalar) {
                plan.diagnostics.push_back("device: mixed element types require an explicit conversion operation");
                return;
            }
            plan.element_type = scalar;
            plan.requirements.non_contiguous_memory |= !attr.view.contiguous;
            plan.requirements.dynamic_shape |= !attr.view.fully_static();

            if (const auto found = binding_by_value.find(base.id); found != binding_by_value.end()) {
                plan.bindings[found->second].access = detail::merge_access(plan.bindings[found->second].access, access);
                return;
            }
            binding_by_value.emplace(base.id, plan.bindings.size());
            plan.bindings.push_back(kernel_binding{base, attr.view, access, 0});
        };

        detail::visit_region(*plan.root->regions.front(), [&](const hl::hl_operation& op) {
            plan.operations.push_back(std::addressof(op));
            if (!detail::is_gpu_legal_operation(op.op)) {
                plan.diagnostics.push_back("device: HL operation #" + std::to_string(op.id)
                    + " is not GPU-legal");
                return;
            }
            if (op.op == hl::hl_opcode::memref_load) add_binding(op, binding_access::read);
            if (op.op == hl::hl_opcode::memref_store) add_binding(op, binding_access::write);
            if (op.op == hl::hl_opcode::structured_reduce) plan.requirements.reduction = true;
            if (op.op == hl::hl_opcode::branch || op.op == hl::hl_opcode::branch_cond)
                plan.requirements.control_flow = true;
        });

        if (plan.requirements.reduction && !options.allow_reductions)
            plan.diagnostics.push_back("device: reductions are disabled by legalization policy");
        if (plan.requirements.non_contiguous_memory && !options.allow_non_contiguous_memory)
            plan.diagnostics.push_back("device: non-contiguous memrefs are disabled by legalization policy");
        if (plan.bindings.empty()) plan.diagnostics.push_back("device: kernel has no memory bindings");

        std::stable_sort(plan.bindings.begin(), plan.bindings.end(), [](const kernel_binding& lhs,
                                                                      const kernel_binding& rhs) {
            return static_cast<unsigned>(lhs.access) < static_cast<unsigned>(rhs.access);
        });
        for (std::uint32_t i = 0; i < plan.bindings.size(); ++i) plan.bindings[i].index = i;

        if (plan.launch.dynamic_global_x) {
            const auto output = std::ranges::find_if(plan.bindings, &kernel_binding::writable);
            if (output != plan.bindings.end() && output->view.fully_static()) {
                plan.launch.global_x = static_cast<std::uint32_t>(output->view.linear_size());
                plan.launch.dynamic_global_x = false;
            }
        }

        std::uint64_t identity = 1469598103934665603ULL;
        identity = detail::mix(identity, plan.root->id);
        for (const auto* op : plan.operations) {
            identity = detail::mix(identity, op->id);
            identity = detail::mix(identity, static_cast<std::uint8_t>(op->op));
        }
        for (const auto& binding : plan.bindings) {
            identity = detail::mix(identity, binding.base_value.id);
            identity = detail::mix(identity, binding.index);
            identity = detail::mix(identity, static_cast<std::uint8_t>(binding.access));
            identity = detail::mix(identity, binding.view.elem_bits);
        }
        plan.identity = identity;
        return plan;
    }
} // namespace lithe::codegen::device
