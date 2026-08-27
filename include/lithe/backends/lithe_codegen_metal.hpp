#pragma once

// Native Metal physical-MIR backend.  Its stable kernel ABI is deliberately
// small: input int64 elements in buffer(0), output int64 elements in buffer(1),
// count in buffer(2), and one MIR invocation per Metal thread.

#include "../lithe_codegen_pipeline.hpp"
#include "../lithe_codegen_hl_passes.hpp"
#include "../lithe_execution_admission.hpp"
#include "../lithe_codegen_device.hpp"
#include "../lithe_execution/backend_persist.hpp"
#include "pravaha/backends/metal_gpu.hpp"

#include <expected>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace lithe::codegen::backends {
    struct metal_runtime_error { std::string message; };

    struct metal_backend;

    enum class metal_plan_disposition : std::uint8_t { accepted, scalar_fallback };

    struct metal_plan_binding {
        metal_plan_disposition disposition = metal_plan_disposition::scalar_fallback;
        std::uint32_t lanes = 0;
        hl::vector_tail_strategy tail = hl::vector_tail_strategy::scalar_fallback;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return disposition == metal_plan_disposition::accepted;
        }
    };

    // Ephemeral, move-only f32 storage owned by the selected Metal device.
    // It never crosses the portable artifact boundary: explicit upload() and
    // download() are the only host-transfer operations.
    class metal_f32_tensor {
    public:
        metal_f32_tensor() = default;
        metal_f32_tensor(const metal_f32_tensor&) = delete;
        metal_f32_tensor& operator=(const metal_f32_tensor&) = delete;
        metal_f32_tensor(metal_f32_tensor&& other) noexcept { move_from(std::move(other)); }
        metal_f32_tensor& operator=(metal_f32_tensor&& other) noexcept {
            if (this != std::addressof(other)) {
                reset();
                move_from(std::move(other));
            }
            return *this;
        }
        ~metal_f32_tensor() { reset(); }

        [[nodiscard]] static std::expected<metal_f32_tensor, metal_runtime_error>
        allocate(const std::size_t count) {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            metal_f32_tensor tensor;
            tensor.size_ = count;
            if (count == 0) return tensor;
            auto& runtime = ::pravaha::backends::metal::metal_gpu_backend::instance();
            if (!runtime.available())
                return std::unexpected(metal_runtime_error{"metal: no device for tensor allocation"});
            tensor.buffer_ = runtime.device->newBuffer(
                count * sizeof(float), MTL::ResourceStorageModeShared);
            if (!tensor.buffer_)
                return std::unexpected(metal_runtime_error{"metal: tensor allocation failed"});
            return tensor;
#else
            static_cast<void>(count);
            return std::unexpected(metal_runtime_error{"metal: native Metal requires Apple platform and HAS_METAL_CPP"});
#endif
        }

        [[nodiscard]] static std::expected<metal_f32_tensor, metal_runtime_error>
        from_host(const std::span<const float> host) {
            auto tensor = allocate(host.size());
            if (!tensor) return std::unexpected(tensor.error());
            if (const auto copied = tensor->upload(host); !copied) return std::unexpected(copied.error());
            return tensor;
        }

        [[nodiscard]] std::expected<void, metal_runtime_error>
        upload(const std::span<const float> host) {
            if (host.size() != size_)
                return std::unexpected(metal_runtime_error{"metal: upload size does not match tensor extent"});
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            if (size_ != 0 && !buffer_)
                return std::unexpected(metal_runtime_error{"metal: tensor has no device buffer"});
            if (size_ != 0) std::memcpy(buffer_->contents(), host.data(), size_ * sizeof(float));
            return {};
#else
            return std::unexpected(metal_runtime_error{"metal: native Metal requires Apple platform and HAS_METAL_CPP"});
#endif
        }

        [[nodiscard]] std::expected<void, metal_runtime_error>
        download(const std::span<float> host) const {
            if (host.size() != size_)
                return std::unexpected(metal_runtime_error{"metal: download size does not match tensor extent"});
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            if (size_ != 0 && !buffer_)
                return std::unexpected(metal_runtime_error{"metal: tensor has no device buffer"});
            if (size_ != 0) std::memcpy(host.data(), buffer_->contents(), size_ * sizeof(float));
            return {};
#else
            return std::unexpected(metal_runtime_error{"metal: native Metal requires Apple platform and HAS_METAL_CPP"});
#endif
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool valid() const noexcept {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            return size_ == 0 || buffer_ != nullptr;
#else
            return false;
#endif
        }

    private:
        friend struct metal_backend;
        std::size_t size_ = 0;
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
        MTL::Buffer* buffer_ = nullptr;
#endif

        void reset() noexcept {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            if (buffer_) buffer_->release();
            buffer_ = nullptr;
#endif
            size_ = 0;
        }
        void move_from(metal_f32_tensor&& other) noexcept {
            size_ = std::exchange(other.size_, 0);
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            buffer_ = std::exchange(other.buffer_, nullptr);
#endif
        }
    };

    class metal_device_submission {
    public:
        metal_device_submission() = default;
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
        explicit metal_device_submission(::pravaha::backends::metal::metal_submission submission,
                                         std::shared_ptr<void> pipeline_lifetime) noexcept
            : submission_(std::move(submission)), pipeline_lifetime_(std::move(pipeline_lifetime)) {}
#endif
        metal_device_submission(const metal_device_submission&) = delete;
        metal_device_submission& operator=(const metal_device_submission&) = delete;
        metal_device_submission(metal_device_submission&&) noexcept = default;
        metal_device_submission& operator=(metal_device_submission&&) noexcept = default;

        [[nodiscard]] std::expected<void, metal_runtime_error> wait() const {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            if (const auto result = submission_.wait(); !result)
                return std::unexpected(metal_runtime_error{result.error().message});
            return {};
#else
            return std::unexpected(metal_runtime_error{"metal: native Metal requires Apple platform and HAS_METAL_CPP"});
#endif
        }

    private:
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
        ::pravaha::backends::metal::metal_submission submission_;
        std::shared_ptr<void> pipeline_lifetime_;
#endif
    };

    namespace metal_detail {
        [[nodiscard]] inline std::uint64_t source_cache_key(const std::string_view source,
                                                             std::uint64_t seed) noexcept {
            for (const auto ch : source) {
                seed ^= static_cast<unsigned char>(ch);
                seed *= 1099511628211ULL;
            }
            return seed;
        }

        [[nodiscard]] inline std::string hl_value_name(const ssa_value_id value) {
            return "v_" + std::to_string(value.id);
        }

        [[nodiscard]] inline std::string compare_operator(const hl::compare_predicate predicate) {
            using enum hl::compare_predicate;
            switch (predicate) {
            case eq: case oeq: return "==";
            case ne: case one: return "!=";
            case slt: case ult: case olt: return "<";
            case sle: case ule: case ole: return "<=";
            case sgt: case ugt: case ogt: return ">";
            case sge: case uge: case oge: return ">=";
            }
            return {};
        }

        [[nodiscard]] inline std::string lower_hl_to_msl(const device::kernel_plan& plan,
                                                          std::vector<std::string>& diagnostics) {
            if (plan.requirements.loop_carried_values) {
                diagnostics.push_back(
                    "metal: loop-carried reductions require a workgroup reduction lowering");
                return {};
            }
            if (!plan.elementwise_dispatch_compatible()) {
                diagnostics.push_back("metal: HL kernel is outside the elementwise dispatch contract");
                return {};
            }
            if (plan.element_type != device::scalar_type::f32) {
                diagnostics.push_back("metal: the initial HL kernel path supports f32; other legalized types need a runtime binding");
                return {};
            }
            if (plan.bindings.size() < 2 || plan.writable_binding_count() != 1) {
                diagnostics.push_back("metal: an elementwise kernel requires at least one input and exactly one output");
                return {};
            }
            for (const auto& binding : plan.bindings) {
                if (binding.view.rank != 1 || !binding.view.contiguous) {
                    diagnostics.push_back("metal: initial HL emission requires contiguous rank-1 memrefs");
                    return {};
                }
            }

            std::unordered_map<std::uint64_t, std::string> values;
            const auto lookup = [&](const ssa_value_id value) -> std::string {
                if (const auto found = values.find(value.id); found != values.end()) return found->second;
                return {};
            };
            const auto set_result = [&](const hl::hl_operation& op, std::string expression,
                                        std::string& body) -> bool {
                if (op.results.size() != 1) {
                    diagnostics.push_back("metal: operation #" + std::to_string(op.id)
                        + " must produce exactly one SSA value");
                    return false;
                }
                const auto name = hl_value_name(op.results.front());
                values[op.results.front().id] = name;
                body += "    const float " + name + " = " + std::move(expression) + ";\n";
                return true;
            };

            std::string parameters;
            for (const auto& binding : plan.bindings) {
                parameters += binding.writable() ? "    device float* " : "    device const float* ";
                parameters += "buffer_" + std::to_string(binding.index)
                    + " [[buffer(" + std::to_string(binding.index) + ")]],\n";
            }
            parameters += "    constant uint& element_count [[buffer("
                + std::to_string(plan.bindings.size()) + ")]],\n";

            std::string body;
            const auto& loop = std::get<hl::structured_for_attr>(plan.root->attr);
            const auto lower = loop.bounds[0].lower_known ? loop.bounds[0].lower : 0;
            const auto step = loop.bounds[0].step_known ? loop.bounds[0].step : 1;

            for (const auto* op : plan.operations) {
                if (op == nullptr) continue;
                if (op->op == hl::hl_opcode::region_yield || op->op == hl::hl_opcode::ret
                    || op->op == hl::hl_opcode::argument) continue;
                if (op->op == hl::hl_opcode::loop_index) {
                    if (op->results.size() != 1) {
                        diagnostics.push_back("metal: loop_index must produce exactly one value");
                        return {};
                    }
                    values[op->results.front().id] = lower == 0 && step == 1
                        ? "gid"
                        : "(" + std::to_string(lower) + " + gid * " + std::to_string(step) + ")";
                    continue;
                }
                if (op->op == hl::hl_opcode::structured_for
                    || op->op == hl::hl_opcode::structured_reduce) {
                    diagnostics.push_back("metal: nested loops and reductions require workgroup lowering");
                    return {};
                }
                if (op->op == hl::hl_opcode::constant) {
                    if (!std::holds_alternative<hl::constant_attr>(op->attr)) {
                        diagnostics.push_back("metal: constant is missing its payload");
                        return {};
                    }
                    const auto& constant = std::get<hl::constant_attr>(op->attr);
                    if (constant.kind != hl::constant_kind::floating_point) {
                        diagnostics.push_back("metal: f32 kernels require floating-point constants");
                        return {};
                    }
                    if (!set_result(*op, std::to_string(constant.floating_point) + "f", body)) return {};
                    continue;
                }
                if (op->op == hl::hl_opcode::memref_load || op->op == hl::hl_opcode::memref_store) {
                    if (!std::holds_alternative<hl::memref_attr>(op->attr)) return {};
                    const auto& attr = std::get<hl::memref_attr>(op->attr);
                    const auto base_pos = static_cast<std::size_t>(attr.base_operand_index);
                    const auto* binding = plan.binding_for(op->operands[base_pos]);
                    if (binding == nullptr) {
                        diagnostics.push_back("metal: memref operation has no analyzed binding");
                        return {};
                    }
                    const auto index_pos = base_pos + (op->op == hl::hl_opcode::memref_store ? 2 : 1);
                    std::string index = "gid";
                    if (index_pos < op->operands.size()) {
                        index = lookup(op->operands[index_pos]);
                        if (index.empty()) {
                            diagnostics.push_back("metal: memref index is not defined inside the kernel region");
                            return {};
                        }
                    }
                    if (op->op == hl::hl_opcode::memref_load) {
                        if (!set_result(*op, "buffer_" + std::to_string(binding->index) + "[" + index + "]", body))
                            return {};
                    }
                    else {
                        const auto value_pos = base_pos + 1;
                        if (value_pos >= op->operands.size()) {
                            diagnostics.push_back("metal: memref_store has no value operand");
                            return {};
                        }
                        const auto value = lookup(op->operands[value_pos]);
                        if (value.empty()) {
                            diagnostics.push_back("metal: memref_store value is not defined inside the kernel region");
                            return {};
                        }
                        body += "    buffer_" + std::to_string(binding->index) + "[" + index + "] = " + value + ";\n";
                    }
                    continue;
                }

                if (op->op == hl::hl_opcode::fneg) {
                    if (op->operands.size() != 1) return {};
                    const auto value = lookup(op->operands.front());
                    if (value.empty() || !set_result(*op, "(-" + value + ")", body)) return {};
                    continue;
                }
                if (op->op == hl::hl_opcode::fadd || op->op == hl::hl_opcode::fsub
                    || op->op == hl::hl_opcode::fmul || op->op == hl::hl_opcode::fdiv) {
                    if (op->operands.size() != 2) return {};
                    const auto lhs = lookup(op->operands[0]);
                    const auto rhs = lookup(op->operands[1]);
                    if (lhs.empty() || rhs.empty()) {
                        diagnostics.push_back("metal: floating operation references an unavailable SSA value");
                        return {};
                    }
                    const std::string_view symbol = op->op == hl::hl_opcode::fadd ? "+"
                        : op->op == hl::hl_opcode::fsub ? "-"
                        : op->op == hl::hl_opcode::fmul ? "*" : "/";
                    if (!set_result(*op, "(" + lhs + " " + std::string(symbol) + " " + rhs + ")", body)) return {};
                    continue;
                }
                if (op->op == hl::hl_opcode::fcmp) {
                    if (op->operands.size() != 2 || !std::holds_alternative<hl::compare_attr>(op->attr)) return {};
                    const auto lhs = lookup(op->operands[0]);
                    const auto rhs = lookup(op->operands[1]);
                    const auto symbol = compare_operator(std::get<hl::compare_attr>(op->attr).pred);
                    if (lhs.empty() || rhs.empty() || symbol.empty()) return {};
                    if (!set_result(*op, "(" + lhs + " " + symbol + " " + rhs + ")", body)) return {};
                    continue;
                }
                if (op->op == hl::hl_opcode::select) {
                    if (op->operands.size() != 3) return {};
                    const auto condition = lookup(op->operands[0]);
                    const auto lhs = lookup(op->operands[1]);
                    const auto rhs = lookup(op->operands[2]);
                    if (condition.empty() || lhs.empty() || rhs.empty()) return {};
                    if (!set_result(*op, "(" + condition + " ? " + lhs + " : " + rhs + ")", body)) return {};
                    continue;
                }

                diagnostics.push_back("metal: HL operation #" + std::to_string(op->id)
                    + " is legal for a device target but not implemented by the f32 Metal emitter");
                return {};
            }

            if (body.empty()) {
                diagnostics.push_back("metal: HL kernel emitted no executable operations");
                return {};
            }
            return "#include <metal_stdlib>\nusing namespace metal;\n\n"
                   "kernel void lithe_metal_hl(\n" + parameters
                + "    uint gid [[thread_position_in_grid]]) {\n"
                  "    if (gid >= element_count) return;\n" + body + "}\n";
        }

        [[nodiscard]] inline std::string register_name(const preg& reg) {
            return "r_" + std::to_string(reg.id);
        }

        [[nodiscard]] inline const preg* as_preg(const allocated_operand& op) noexcept {
            return op.type == allocated_operand::kind::preg ? &std::get<preg>(op.value) : nullptr;
        }

        [[nodiscard]] inline std::string operand_text(const allocated_operand& op,
                                                      std::vector<std::string>& diagnostics) {
            switch (op.type) {
            case allocated_operand::kind::preg: return register_name(std::get<preg>(op.value));
            case allocated_operand::kind::immediate_i64: {
                const auto value = std::get<std::int64_t>(op.value);
                if (value == std::numeric_limits<std::int64_t>::min()) {
                    return "(long(-9223372036854775807) - long(1))";
                }
                return "long(" + std::to_string(value) + ")";
            }
            default:
                diagnostics.push_back("metal: operand is not representable by the int64 elementwise ABI");
                return {};
            }
        }

        [[nodiscard]] inline bool one_register_def(const allocated_instruction& inst,
                                                   std::vector<std::string>& diagnostics) {
            if (inst.defs.size() == 1 && as_preg(inst.defs.front())) return true;
            diagnostics.push_back("metal: instruction " + std::to_string(inst.id)
                + " requires exactly one physical-register definition");
            return false;
        }

        [[nodiscard]] inline bool exact_uses(const allocated_instruction& inst, const std::size_t count,
                                             std::vector<std::string>& diagnostics) {
            if (inst.uses.size() == count) return true;
            diagnostics.push_back("metal: instruction " + std::to_string(inst.id)
                + " has an unsupported operand count");
            return false;
        }

        [[nodiscard]] inline std::string binary_operator(const opcode op) {
            switch (op) {
            case opcode::add: return "+"; case opcode::sub: return "-";
            case opcode::mul: return "*"; case opcode::div: return "/";
            case opcode::mod: return "%"; case opcode::bit_and: return "&";
            case opcode::bit_or: return "|"; case opcode::bit_xor: return "^";
            case opcode::shl: return "<<"; case opcode::shr: return ">>";
            default: return {};
            }
        }

        [[nodiscard]] inline std::string comparison_operator(const opcode op) {
            switch (op) {
            case opcode::cmp_eq: return "=="; case opcode::cmp_ne: return "!=";
            case opcode::cmp_lt: return "<"; case opcode::cmp_le: return "<=";
            case opcode::cmp_gt: return ">"; case opcode::cmp_ge: return ">=";
            default: return {};
            }
        }

        [[nodiscard]] inline std::string lower_to_msl(const mir::physical_mir_function& fn,
                                                        std::vector<std::string>& diagnostics) {
            if (fn.function.blocks.size() != 1) {
                diagnostics.push_back("metal: the elementwise ABI accepts exactly one basic block");
                return {};
            }
            const auto& block = fn.function.blocks.front();
            if (!block.phi_placeholders.empty()) {
                diagnostics.push_back("metal: phi nodes require CFG-to-predication lowering");
                return {};
            }

            std::string body;
            std::unordered_set<std::uint16_t> declared;
            bool returned = false;
            const auto declare = [&](const preg& reg) -> std::string {
                return declared.insert(reg.id).second ? "long " : "";
            };
            const auto assignment = [&](const allocated_instruction& inst, const std::string& value) {
                const auto* dst = as_preg(inst.defs.front());
                body += "    " + declare(*dst) + register_name(*dst) + " = " + value + ";\n";
            };

            for (const auto& inst : block.instructions) {
                if (returned) {
                    diagnostics.push_back("metal: instructions after ret are not legal in a kernel body");
                    return {};
                }
                if (inst.op == opcode::nop) continue;

                if (inst.op == opcode::load_arg) {
                    if (!one_register_def(inst, diagnostics) || !exact_uses(inst, 1, diagnostics)
                        || inst.uses.front().type != allocated_operand::kind::argument_index
                        || std::get<std::uint32_t>(inst.uses.front().value) != 0) {
                        diagnostics.push_back("metal: only load_arg 0 maps to the elementwise input buffer");
                        return {};
                    }
                    assignment(inst, "input[gid]");
                    continue;
                }
                if (inst.op == opcode::load_imm || inst.op == opcode::mov) {
                    if (!one_register_def(inst, diagnostics) || !exact_uses(inst, 1, diagnostics)) return {};
                    const auto value = operand_text(inst.uses.front(), diagnostics);
                    if (value.empty()) return {};
                    assignment(inst, value);
                    continue;
                }
                if (const auto binary = binary_operator(inst.op); !binary.empty()) {
                    if (!one_register_def(inst, diagnostics) || !exact_uses(inst, 2, diagnostics)) return {};
                    const auto lhs = operand_text(inst.uses[0], diagnostics);
                    const auto rhs = operand_text(inst.uses[1], diagnostics);
                    if (lhs.empty() || rhs.empty()) return {};
                    assignment(inst, "(" + lhs + " " + binary + " " + rhs + ")");
                    continue;
                }
                if (const auto comparison = comparison_operator(inst.op); !comparison.empty()) {
                    if (!one_register_def(inst, diagnostics) || !exact_uses(inst, 2, diagnostics)) return {};
                    const auto lhs = operand_text(inst.uses[0], diagnostics);
                    const auto rhs = operand_text(inst.uses[1], diagnostics);
                    if (lhs.empty() || rhs.empty()) return {};
                    assignment(inst, "((" + lhs + " " + comparison + " " + rhs + ") ? long(1) : long(0))");
                    continue;
                }
                if (inst.op == opcode::neg || inst.op == opcode::bit_not || inst.op == opcode::logical_not) {
                    if (!one_register_def(inst, diagnostics) || !exact_uses(inst, 1, diagnostics)) return {};
                    const auto value = operand_text(inst.uses.front(), diagnostics);
                    if (value.empty()) return {};
                    const std::string_view prefix = inst.op == opcode::neg ? "-" : inst.op == opcode::bit_not ? "~" : "!";
                    assignment(inst, inst.op == opcode::logical_not
                        ? "((" + std::string(prefix) + value + ") ? long(1) : long(0))"
                        : "(" + std::string(prefix) + value + ")");
                    continue;
                }
                if (inst.op == opcode::logical_and || inst.op == opcode::logical_or) {
                    if (!one_register_def(inst, diagnostics) || !exact_uses(inst, 2, diagnostics)) return {};
                    const auto lhs = operand_text(inst.uses[0], diagnostics);
                    const auto rhs = operand_text(inst.uses[1], diagnostics);
                    if (lhs.empty() || rhs.empty()) return {};
                    const std::string_view operation = inst.op == opcode::logical_and ? "&&" : "||";
                    assignment(inst, "((" + lhs + " " + std::string(operation) + " " + rhs + ") ? long(1) : long(0))");
                    continue;
                }
                if (inst.op == opcode::ret) {
                    if (!inst.defs.empty() || !exact_uses(inst, 1, diagnostics)) return {};
                    const auto value = operand_text(inst.uses.front(), diagnostics);
                    if (value.empty()) return {};
                    body += "    output[gid] = " + value + ";\n    return;\n";
                    returned = true;
                    continue;
                }
                diagnostics.push_back("metal: opcode " + legacy_opcode_operation_id(inst.op).name
                    + " is outside the int64 elementwise kernel contract");
                return {};
            }
            if (!returned) {
                diagnostics.push_back("metal: an elementwise kernel must terminate with ret value");
                return {};
            }
            return "#include <metal_stdlib>\nusing namespace metal;\n\n"
                   "kernel void lithe_metal_i64_elementwise(\n"
                   "    device const long* input [[buffer(0)]],\n"
                   "    device long* output [[buffer(1)]],\n"
                   "    constant uint& element_count [[buffer(2)]],\n"
                   "    uint gid [[thread_position_in_grid]]) {\n"
                   "    if (gid >= element_count) return;\n"
                + body + "}\n";
        }
    } // namespace metal_detail

    struct metal_backend {
        static constexpr lithe::plugin_descriptor<sizeof("lithe.backend.metal"), sizeof("Lithe")> descriptor{
            .id = "lithe.backend.metal", .version = {1, 0, 0}, .author = "Lithe",
            .domain = lithe::semantic::domain_type::arithmetic,
        };

        [[nodiscard]] static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({backend_feature::integer_arithmetic});
        }

        [[nodiscard]] static emission_target_traits traits() {
            return {.name = "metal_backend", .input_phase = target_input_phase::physical_mir,
                    .produced_artifact = artifact_kind::device_kernel, .capabilities = capabilities(),
                    .operation_requirements = {.backend_name = "metal_backend",
                                               .supported_operation_domains = {"lithe.core"}}};
        }

        [[nodiscard]] static bool available() noexcept {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            return ::pravaha::backends::metal::metal_gpu_backend::instance().available();
#else
            return false;
#endif
        }

        [[nodiscard]] compilation_artifact emit(const mir::physical_mir_function& fn) const {
            compilation_artifact art;
            art.name = fn.function.name;
            art.metadata["backend"] = "metal";
            art.metadata["kernel_abi"] = "lithe.metal.int64.elementwise.v1";
            auto source = metal_detail::lower_to_msl(fn, art.diagnostics);
            if (source.empty()) return art;
            art.text_payload = std::move(source);
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            const auto pipeline = ::pravaha::backends::metal::get_or_compile_source(
                metal_detail::source_cache_key(art.text_payload, 0x19d6e8a4f25b3c71ULL),
                art.text_payload, "lithe_metal_i64_elementwise");
            if (!pipeline) {
                art.diagnostics.push_back("metal: pipeline compilation failed: " + pipeline.error().message);
                return art;
            }
            auto* native_pipeline = (*pipeline).get();
            native_pipeline->retain();
            auto handle = std::make_shared<artifact_handle>();
            handle->kind = artifact_handle_kind::native_compute_pipeline;
            handle->payload = std::shared_ptr<void>{native_pipeline, [](void* raw) noexcept {
                static_cast<MTL::ComputePipelineState*>(raw)->release();
            }};
            art.handle = std::move(handle);
            art.kind = artifact_kind::device_kernel;
            art.metadata["metal_pipeline"] = "compiled";
#else
            art.diagnostics.push_back("metal: native Metal requires Apple platform and HAS_METAL_CPP");
#endif
            return art;
        }

        [[nodiscard]] static bool supports(const device::kernel_plan& plan) noexcept {
            return plan.elementwise_dispatch_compatible()
                && plan.element_type == device::scalar_type::f32
                && std::ranges::all_of(plan.bindings, [](const device::kernel_binding& binding) {
                    return binding.view.rank == 1 && binding.view.contiguous;
                });
        }

        [[nodiscard]] compilation_artifact emit(const device::kernel_plan& plan) const {
            compilation_artifact art;
            art.name = plan.source != nullptr ? plan.source->name : std::string{};
            art.metadata["backend"] = "metal";
            art.metadata["source_ir"] = "lithe.hl_mir";
            art.metadata["kernel_abi"] = "lithe.device.elementwise.v1";
            art.metadata["device_plan_identity"] = std::to_string(plan.identity);
            art.metadata["binding_count"] = std::to_string(plan.bindings.size());
            art.metadata["element_type"] = plan.element_type
                ? std::string(device::to_string(*plan.element_type)) : "unknown";
            art.metadata["persistable_payload"] = "msl";
            art.metadata["native_handle_persistable"] = "false";
            if (!supports(plan)) {
                art.diagnostics.insert(art.diagnostics.end(), plan.diagnostics.begin(), plan.diagnostics.end());
                art.diagnostics.push_back("metal: device plan is not supported by the current HL Metal contract");
                return art;
            }
            art.text_payload = metal_detail::lower_hl_to_msl(plan, art.diagnostics);
            if (art.text_payload.empty()) return art;
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            const auto pipeline = ::pravaha::backends::metal::get_or_compile_source(
                metal_detail::source_cache_key(art.text_payload,
                    plan.identity ^ 0x4a6f9b8ce312d705ULL),
                art.text_payload, "lithe_metal_hl");
            if (!pipeline) {
                art.diagnostics.push_back("metal: HL pipeline compilation failed: " + pipeline.error().message);
                return art;
            }
            auto* native_pipeline = (*pipeline).get();
            native_pipeline->retain();
            auto handle = std::make_shared<artifact_handle>();
            handle->kind = artifact_handle_kind::native_compute_pipeline;
            handle->payload = std::shared_ptr<void>{native_pipeline, [](void* raw) noexcept {
                static_cast<MTL::ComputePipelineState*>(raw)->release();
            }};
            art.handle = std::move(handle);
            art.kind = artifact_kind::device_kernel;
            art.metadata["metal_pipeline"] = "compiled";
#else
            art.diagnostics.push_back("metal: native Metal requires Apple platform and HAS_METAL_CPP");
#endif
            return art;
        }

        [[nodiscard]] static std::expected<execution::msl_persist_artifact, metal_runtime_error>
        persistable_artifact(const compilation_artifact& artifact) {
            if (artifact.text_payload.empty()
                || !artifact.metadata.contains("persistable_payload")
                || artifact.metadata.at("persistable_payload") != "msl") {
                return std::unexpected(metal_runtime_error{
                    "metal: artifact does not contain a persistable MSL payload"});
            }
            const auto parse_u64 = [](const std::string& value) -> std::optional<std::uint64_t> {
                std::uint64_t parsed = 0;
                const auto [end, error] = std::from_chars(
                    value.data(), value.data() + value.size(), parsed);
                if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
                return parsed;
            };
            const auto parse_u32 = [&parse_u64](const std::string& value) -> std::optional<std::uint32_t> {
                const auto parsed = parse_u64(value);
                if (!parsed || *parsed > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
                return static_cast<std::uint32_t>(*parsed);
            };
            const auto identity = artifact.metadata.contains("device_plan_identity")
                ? parse_u64(artifact.metadata.at("device_plan_identity")) : std::nullopt;
            const auto bindings = artifact.metadata.contains("binding_count")
                ? parse_u32(artifact.metadata.at("binding_count")) : std::nullopt;
            if (!identity || !bindings || !artifact.metadata.contains("element_type")) {
                return std::unexpected(metal_runtime_error{
                    "metal: MSL artifact is missing compatibility metadata"});
            }
            return execution::msl_persist_artifact{
                .binding_count = *bindings,
                .device_plan_identity = *identity,
                .element_type = artifact.metadata.at("element_type"),
                .source = artifact.text_payload};
        }

        template <std::size_t InputCount>
        [[nodiscard]] static std::expected<void, metal_runtime_error> dispatch_f32(
            const compilation_artifact& art,
            const std::span<float> output,
            const std::array<std::span<const float>, InputCount>& inputs
        ) {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            if (!art.handle || art.handle->kind != artifact_handle_kind::native_compute_pipeline)
                return std::unexpected(metal_runtime_error{"metal: artifact has no native compute pipeline"});
            if (art.metadata.contains("element_type") && art.metadata.at("element_type") != "f32")
                return std::unexpected(metal_runtime_error{"metal: artifact is not an f32 kernel"});
            if constexpr (InputCount == 0)
                return std::unexpected(metal_runtime_error{"metal: dispatch requires at least one input"});
            const auto count = inputs.front().size();
            if (count == 0) return {};
            if (output.size() < count)
                return std::unexpected(metal_runtime_error{"metal: output span is shorter than the input domain"});
            if (std::ranges::any_of(inputs, [count](const auto input) { return input.size() != count; }))
                return std::unexpected(metal_runtime_error{"metal: all input spans must have equal length"});
            if (art.metadata.contains("binding_count")
                && art.metadata.at("binding_count") != std::to_string(InputCount + 1))
                return std::unexpected(metal_runtime_error{"metal: dispatch binding count does not match the artifact ABI"});

            auto* pipeline = art.handle->get<MTL::ComputePipelineState>();
            if (!pipeline) return std::unexpected(metal_runtime_error{"metal: pipeline handle is null"});
            ::pravaha::compute::buffer_descriptor descriptor;
            descriptor.shape.push_back(count);
            descriptor.element_type = ::pravaha::compute::data_element_type::f32;
            descriptor.is_unified = false;
            std::array<::pravaha::compute::compute_view<const float>, InputCount> source_views;
            for (std::size_t i = 0; i < InputCount; ++i)
                source_views[i] = ::pravaha::compute::make_const_view(inputs[i].data(), descriptor);
            auto destination = ::pravaha::compute::make_view(output.data(), descriptor);
            const auto grid = ::pravaha::hetero::compute_grid_descriptor::from_flat(count);
            const auto result = ::pravaha::backends::metal::metal_gpu_backend::instance().dispatch_multi(
                pipeline, destination, source_views, grid);
            if (!result)
                return std::unexpected(metal_runtime_error{"metal: dispatch failed: " + result.error().message});
            return {};
#else
            static_cast<void>(art); static_cast<void>(output); static_cast<void>(inputs);
            return std::unexpected(metal_runtime_error{"metal: native Metal requires Apple platform and HAS_METAL_CPP"});
#endif
        }

        // Asynchronous device-resident dispatch. Inputs and output stay on the
        // Metal device until the caller explicitly downloads a tensor. This is
        // the composition path for chains of compatible elementwise kernels.
        template <std::size_t InputCount>
        [[nodiscard]] static std::expected<metal_device_submission, metal_runtime_error>
        dispatch_f32_device_async(
            const compilation_artifact& art,
            metal_f32_tensor& output,
            const std::array<const metal_f32_tensor*, InputCount>& inputs) {
            static_assert(InputCount > 0, "a Metal elementwise kernel requires an input tensor");
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            if (!art.handle || art.handle->kind != artifact_handle_kind::native_compute_pipeline)
                return std::unexpected(metal_runtime_error{"metal: artifact has no native compute pipeline"});
            if (art.metadata.contains("element_type") && art.metadata.at("element_type") != "f32")
                return std::unexpected(metal_runtime_error{"metal: artifact is not an f32 kernel"});
            const auto count = inputs.front() != nullptr ? inputs.front()->size() : 0;
            if (count == 0) return std::unexpected(metal_runtime_error{"metal: device dispatch requires a non-empty input"});
            if (!output.valid() || output.size() != count)
                return std::unexpected(metal_runtime_error{"metal: output tensor does not match the input domain"});
            if (std::ranges::any_of(inputs, [count](const auto* input) {
                    return input == nullptr || !input->valid() || input->size() != count;
                }))
                return std::unexpected(metal_runtime_error{"metal: input tensors do not share a valid extent"});
            if (art.metadata.contains("binding_count")
                && art.metadata.at("binding_count") != std::to_string(InputCount + 1))
                return std::unexpected(metal_runtime_error{"metal: dispatch binding count does not match the artifact ABI"});
            auto* pipeline = art.handle->get<MTL::ComputePipelineState>();
            if (!pipeline) return std::unexpected(metal_runtime_error{"metal: pipeline handle is null"});
            std::array<MTL::Buffer*, InputCount> input_buffers{};
            for (std::size_t i = 0; i < InputCount; ++i) input_buffers[i] = inputs[i]->buffer_;
            const auto grid = ::pravaha::hetero::compute_grid_descriptor::from_flat(count);
            auto submitted = ::pravaha::backends::metal::metal_gpu_backend::instance()
                .dispatch_device_multi_async(pipeline, output.buffer_, input_buffers, count, grid);
            if (!submitted)
                return std::unexpected(metal_runtime_error{"metal: device dispatch failed: " + submitted.error().message});
            return metal_device_submission{std::move(*submitted), art.handle->payload};
#else
            static_cast<void>(art); static_cast<void>(output); static_cast<void>(inputs);
            return std::unexpected(metal_runtime_error{"metal: native Metal requires Apple platform and HAS_METAL_CPP"});
#endif
        }

        [[nodiscard]] static std::expected<void, metal_runtime_error> dispatch_i64(
            const compilation_artifact& art, const std::span<std::int64_t> output,
            const std::span<const std::int64_t> input) {
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
            if (!art.handle || art.handle->kind != artifact_handle_kind::native_compute_pipeline)
                return std::unexpected(metal_runtime_error{"metal: artifact has no native compute pipeline"});
            if (input.empty()) return {};
            if (output.size() < input.size())
                return std::unexpected(metal_runtime_error{"metal: output span is shorter than input span"});
            auto* pipeline = art.handle->get<MTL::ComputePipelineState>();
            if (!pipeline) return std::unexpected(metal_runtime_error{"metal: pipeline handle is null"});
            ::pravaha::compute::buffer_descriptor source_desc;
            source_desc.shape.push_back(input.size());
            source_desc.element_type = ::pravaha::compute::data_element_type::i64;
            source_desc.is_unified = false;
            auto destination_desc = source_desc;
            auto source = ::pravaha::compute::make_const_view(input.data(), std::move(source_desc));
            auto destination = ::pravaha::compute::make_view(output.data(), std::move(destination_desc));
            const auto grid = ::pravaha::hetero::compute_grid_descriptor::from_flat(input.size());
            const auto result = ::pravaha::backends::metal::metal_gpu_backend::instance().dispatch(
                pipeline, destination, source, grid);
            if (!result) return std::unexpected(metal_runtime_error{"metal: dispatch failed: " + result.error().message});
            return {};
#else
            static_cast<void>(art); static_cast<void>(output); static_cast<void>(input);
            return std::unexpected(metal_runtime_error{"metal: native Metal requires Apple platform and HAS_METAL_CPP"});
#endif
        }
    };

    // Binding is deliberately separate from MSL emission: the common plan owns
    // legality, while this backend only decides whether its current native
    // contract can consume that already-proven work.
    [[nodiscard]] inline metal_plan_binding bind_vector_plan_for_metal(
        const hl::vector_plan& plan) noexcept {
        metal_plan_binding binding{
            .lanes = plan.lanes,
            .tail = plan.tail,
        };
        const bool compatible_tail = plan.tail == hl::vector_tail_strategy::none
            || plan.tail == hl::vector_tail_strategy::scalar_epilogue
            || plan.tail == hl::vector_tail_strategy::masked;
        if (plan.legality == hl::vector_plan_legality::proven
            && plan.schedule_materialized
            && plan.element_bits == 32
            && plan.reduction == hl::vector_reduction_shape::none
            && compatible_tail
            && metal_backend::available()) {
            binding.disposition = metal_plan_disposition::accepted;
        }
        return binding;
    }

    struct metal_plan_lowering {
        metal_plan_binding binding{};
        const device::kernel_plan* kernel = nullptr;

        [[nodiscard]] bool accepted() const noexcept {
            return binding.accepted() && kernel != nullptr && metal_backend::supports(*kernel);
        }
    };

    [[nodiscard]] inline metal_plan_lowering lower_vector_plan_for_metal(
        const hl::vector_plan& vector_plan,
        const device::kernel_plan& kernel_plan) noexcept {
        return {.binding = bind_vector_plan_for_metal(vector_plan), .kernel = std::addressof(kernel_plan)};
    }

    [[nodiscard]] inline std::optional<compilation_artifact> emit_metal_plan(
        const metal_plan_lowering& lowering) {
        if (!lowering.accepted()) return std::nullopt;
        return metal_backend{}.emit(*lowering.kernel);
    }

    [[nodiscard]] inline hl::execution_backend_admission admit_metal_plan(
        const metal_plan_binding& binding) noexcept {
        const bool provider_available = metal_backend::available();
        return {.kind = hl::planned_execution_kind::metal,
                .plan_admitted = binding.accepted(),
                .provider_available = provider_available,
                .reason = binding.accepted() ? hl::execution_admission_reason::admitted
                    : (provider_available ? hl::execution_admission_reason::plan_rejected
                                          : hl::execution_admission_reason::provider_unavailable)};
    }

    static_assert(CodeEmissionTarget<metal_backend>);
} // namespace lithe::codegen::backends
