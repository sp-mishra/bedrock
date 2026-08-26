#pragma once

// gpu_backend.hpp — minimal MIR→SPIR-V elementwise GPU backend (§v2.8).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Scope (honest + real):
//   * A programmatic SPIR-V 1.0 compute-shader emitter for elementwise binary
//     kernels — out[i] = a[i] OP b[i], OP ∈ {add, mul} — over three float
//     storage buffers, indexed by gl_GlobalInvocationID.x, LocalSize (64,1,1).
//     The emitted `spirv_module` passes the vulkan backend's validate() (magic,
//     Shader capability, LocalSize execution mode) and is a legal GLCompute
//     module MoltenVK can consume.
//   * When the Vulkan backend is compiled in (LITHE_VULKAN_BACKEND_AVAILABLE),
//     gpu_dispatch_elementwise() compiles + installs the module through
//     vulkan_backend and dispatches it. When a device is unavailable it returns
//     an honest error so the planner can NADI-pulse fall back to SIMD/CPU.
//   * Unsupported kernel shapes never silently degrade: they return
//     gpu_unsupported so the caller can fall back explicitly.
//
// The SPIR-V bytes are assembled by hand (there is no high-level builder in the
// tree); this file owns that encoding. GPU dispatch is only reachable behind
// the Vulkan availability macro, so the header always compiles.

#include "lithe/backends/lithe_codegen_vulkan_spirv_ir.hpp"
#include "lithe/backends/lithe_codegen_metal.hpp"
#include "lithe/backends/lithe_codegen_backend_registry.hpp"
#include "lithe/lithe_codegen_device.hpp"

#if __has_include(<vulkan/vulkan.h>) && (defined(HAS_MOLTENVK) || defined(HAS_VULKAN))
#  include "lithe/backends/lithe_codegen_vulkan.hpp"
#  include "pravaha/backends/vulkan_gpu.hpp"
#endif

#include <cstdint>
#include <array>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace crank {
    // ============================================================================
    // gpu_elementwise_op — the binary elementwise kernels the emitter supports.
    // ============================================================================

    enum class gpu_elementwise_op : std::uint8_t { add, mul };

    [[nodiscard]] constexpr std::string_view to_string(gpu_elementwise_op op) noexcept {
        switch (op) {
        case gpu_elementwise_op::add: return "add";
        case gpu_elementwise_op::mul: return "mul";
        }
        return "unknown";
    }

    // ============================================================================
    // spirv_elementwise_emitter — hand-assembled SPIR-V for a float elementwise
    // binary kernel over three std430 storage buffers (set 0, bindings 0/1/2):
    //
    //   layout(local_size_x = 64) in;
    //   layout(std430, binding=0) buffer A { float a[]; };
    //   layout(std430, binding=1) buffer B { float b[]; };
    //   layout(std430, binding=2) buffer C { float c[]; };
    //   void main() { uint i = gl_GlobalInvocationID.x; c[i] = a[i] OP b[i]; }
    //
    // Ids are assigned linearly; `bound` is the final id count + 1. Every emitted
    // instruction is `(word_count << 16) | opcode` followed by its operands.
    // ============================================================================

    class spirv_elementwise_emitter {
    public:
        // Local workgroup size along X. 64 is a safe, portable default for MoltenVK.
        static constexpr std::uint32_t k_local_x = 64;

        [[nodiscard]] lithe::codegen::backends::spirv_module
        emit(gpu_elementwise_op op) const {
            using W = std::uint32_t;
            std::vector<W> w;

            // ---- id allocation (dense, starting at 1) ----------------------------
            // Each storage buffer gets its OWN Block-decorated struct type. MoltenVK's
            // SPIRV-Cross keys MSL buffer structs by the Block struct type, so sharing
            // one struct across the three bindings makes it index resource tables past
            // their size (crash in is_msl_resource_binding_used). Distinct structs are
            // the portable shape.
            const W id_pgiid = 1; // Input ptr to giid.x (OpAccessChain result)
            const W id_main = 2; // %main
            const W id_void = 3; // %void
            const W id_fn_void = 4; // %fn = OpTypeFunction %void
            const W id_uint = 5; // %uint
            const W id_float = 6; // %float
            const W id_v3uint = 7; // %v3uint
            const W id_ptr_in_v3uint = 8; // Input ptr to v3uint
            const W id_giid = 9; // gl_GlobalInvocationID
            const W id_uint0 = 10; // const uint 0
            const W id_ptr_in_uint = 11; // Input ptr to uint
            const W id_rta_float = 12; // OpTypeRuntimeArray float
            const W id_struct_a = 13; // struct A { float[] }  (buffer block)
            const W id_struct_b = 14; // struct B { float[] }
            const W id_struct_c = 15; // struct C { float[] }
            const W id_ptr_sb_a = 16; // StorageBuffer ptr to struct A
            const W id_ptr_sb_b = 17; // StorageBuffer ptr to struct B
            const W id_ptr_sb_c = 18; // StorageBuffer ptr to struct C
            const W id_var_a = 19;
            const W id_var_b = 20;
            const W id_var_c = 21;
            const W id_ptr_sb_float = 22; // StorageBuffer ptr to float
            const W id_int0 = 23; // const int 0 (member index)
            const W id_int_type = 24; // %int
            const W id_label = 25; // entry label
            const W id_idx = 26; // loaded invocation index (uint)
            const W id_pa = 27; // ptr into A
            const W id_pb = 28; // ptr into B
            const W id_pc = 29; // ptr into C
            const W id_va = 30; // loaded a[i]
            const W id_vb = 31; // loaded b[i]
            const W id_res = 32; // a[i] OP b[i]
            const W bound = 33; // == max id + 1

            auto inst = [&](W opcode, std::initializer_list<W> operands) {
                const W wc = static_cast<W>(1 + operands.size());
                w.push_back((wc << 16) | opcode);
                for (W o : operands) w.push_back(o);
            };

            // ---- Header ---------------------------------------------------------
            w.push_back(0x07230203u); // magic
            w.push_back(0x00010000u); // version 1.0
            w.push_back(0x00000000u); // generator
            w.push_back(bound); // bound
            w.push_back(0x00000000u); // schema

            // OpCapability Shader (17)
            inst(17, {1});
            // OpMemoryModel Logical(0) GLSL450(1) (14)
            inst(14, {0, 1});
            // OpEntryPoint GLCompute(5) %main "main" %giid (15)
            //   A SPIR-V literal string is NUL-terminated and zero-padded to a word
            //   boundary. "main" is exactly 4 bytes, so the NUL forces a whole
            //   second name word (0x00000000); without it SPIRV-Cross keeps reading
            //   into the interface id and reports "Entry point does not exist".
            w.push_back((6u << 16) | 15u); // wc=6: opcode + exec_model + id + 2 name words + interface
            w.push_back(5u); // GLCompute
            w.push_back(id_main);
            w.push_back(0x6e69616du); // "main" (little-endian: 'm','a','i','n')
            w.push_back(0x00000000u); // NUL terminator + word padding
            w.push_back(id_giid); // interface: the builtin we read
            // OpExecutionMode %main LocalSize(17) k_local_x 1 1 (16)
            inst(16, {id_main, 17u, k_local_x, 1u, 1u});

            // ---- Decorations ----------------------------------------------------
            // gl_GlobalInvocationID: BuiltIn(11) GlobalInvocationId(28)
            // OpDecorate %giid BuiltIn GlobalInvocationId (71)
            inst(71, {id_giid, 11u, 28u});
            // Runtime array stride 4 (float): OpDecorate %rta ArrayStride(6) 4
            inst(71, {id_rta_float, 6u, 4u});
            // Each buffer struct: member 0 at Offset 0, decorated Block. One decoration
            // set per distinct struct so SPIRV-Cross sees three independent blocks.
            auto decorate_block = [&](W struct_id) {
                // OpMemberDecorate %struct 0 Offset(35) 0
                inst(72, {struct_id, 0u, 35u, 0u});
                // OpDecorate %struct Block(2)
                inst(71, {struct_id, 2u});
            };
            decorate_block(id_struct_a);
            decorate_block(id_struct_b);
            decorate_block(id_struct_c);
            // Bindings + descriptor set for the three buffers.
            auto decorate_buffer = [&](W var, W binding) {
                // OpDecorate %var DescriptorSet(34) 0
                inst(71, {var, 34u, 0u});
                // OpDecorate %var Binding(33) <binding>
                inst(71, {var, 33u, binding});
            };
            decorate_buffer(id_var_a, 0);
            decorate_buffer(id_var_b, 1);
            decorate_buffer(id_var_c, 2);

            // ---- Types / constants ---------------------------------------------
            inst(19, {id_void}); // OpTypeVoid
            inst(33, {id_fn_void, id_void}); // OpTypeFunction %void
            inst(21, {id_uint, 32u, 0u}); // OpTypeInt 32 unsigned
            inst(21, {id_int_type, 32u, 1u}); // OpTypeInt 32 signed
            inst(22, {id_float, 32u}); // OpTypeFloat 32
            // OpTypeVector (23) %v3uint = %uint x3
            inst(23, {id_v3uint, id_uint, 3u});
            // OpTypePointer (32) Input(1) %v3uint
            inst(32, {id_ptr_in_v3uint, 1u, id_v3uint});
            // %giid = OpVariable (59) %ptr_in_v3uint Input(1)
            inst(59, {id_ptr_in_v3uint, id_giid, 1u});
            // const uint 0 : OpConstant (43)
            inst(43, {id_uint, id_uint0, 0u});
            // OpTypePointer Input %uint
            inst(32, {id_ptr_in_uint, 1u, id_uint});
            // OpTypeRuntimeArray (29) %float — shared array type, per-struct wrapping.
            inst(29, {id_rta_float, id_float});
            // Three distinct OpTypeStruct (30), each { %rta }.
            inst(30, {id_struct_a, id_rta_float});
            inst(30, {id_struct_b, id_rta_float});
            inst(30, {id_struct_c, id_rta_float});
            // Three distinct StorageBuffer(12) struct pointer types.
            inst(32, {id_ptr_sb_a, 12u, id_struct_a});
            inst(32, {id_ptr_sb_b, 12u, id_struct_b});
            inst(32, {id_ptr_sb_c, 12u, id_struct_c});
            // three storage buffer variables: OpVariable %ptr StorageBuffer
            inst(59, {id_ptr_sb_a, id_var_a, 12u});
            inst(59, {id_ptr_sb_b, id_var_b, 12u});
            inst(59, {id_ptr_sb_c, id_var_c, 12u});
            // OpTypePointer StorageBuffer %float
            inst(32, {id_ptr_sb_float, 12u, id_float});
            // const int 0 : OpConstant %int 0
            inst(43, {id_int_type, id_int0, 0u});

            // ---- Function body --------------------------------------------------
            // %main = OpFunction %void None(0) %fn_void (54)
            inst(54, {id_void, id_main, 0u, id_fn_void});
            // OpLabel (248)
            inst(248, {id_label});
            //   %pgiid = OpAccessChain %ptr_in_uint %giid %uint0   (&giid.x)
            //   %idx   = OpLoad %uint %pgiid
            inst(65, {id_ptr_in_uint, id_pgiid, id_giid, id_uint0});
            inst(61, {id_uint, id_idx, id_pgiid}); // OpLoad %idx

            // c[i] = a[i] OP b[i]
            inst(65, {id_ptr_sb_float, id_pa, id_var_a, id_int0, id_idx}); // &A.data[i]
            inst(61, {id_float, id_va, id_pa}); // load a[i]
            inst(65, {id_ptr_sb_float, id_pb, id_var_b, id_int0, id_idx}); // &B.data[i]
            inst(61, {id_float, id_vb, id_pb}); // load b[i]
            // OpFAdd(129) / OpFMul(133)
            inst(op == gpu_elementwise_op::add ? 129u : 133u,
                 {id_float, id_res, id_va, id_vb});
            inst(65, {id_ptr_sb_float, id_pc, id_var_c, id_int0, id_idx}); // &C.data[i]
            inst(62, {id_pc, id_res}); // OpStore c[i]
            // OpReturn (253)
            inst(253, {});
            // OpFunctionEnd (56)
            inst(56, {});

            lithe::codegen::backends::spirv_module mod;
            mod.words = std::move(w);
            return mod;
        }
    };

    // ============================================================================
    // gpu_dispatch_status — outcome of a GPU dispatch attempt.
    // ============================================================================

    enum class gpu_dispatch_status : std::uint8_t {
        ok, // dispatched on device
        unsupported_shape, // kernel shape not one the emitter handles
        no_device, // Vulkan not compiled in, or no device available
        resource_exhausted, // selected device path exceeds its configured resource budget
    };

    [[nodiscard]] constexpr std::string_view to_string(gpu_dispatch_status s) noexcept {
        switch (s) {
        case gpu_dispatch_status::ok: return "ok";
        case gpu_dispatch_status::unsupported_shape: return "unsupported_shape";
        case gpu_dispatch_status::no_device: return "no_device";
        case gpu_dispatch_status::resource_exhausted: return "resource_exhausted";
        }
        return "unknown";
    }

    struct gpu_dispatch_result {
        gpu_dispatch_status status = gpu_dispatch_status::no_device;
        std::string note; // NADI-pulse text on fallback
        [[nodiscard]] bool ok() const noexcept { return status == gpu_dispatch_status::ok; }
    };

    // Crank consumes Lithe's shared provider-selection policy.
    using gpu_provider = lithe::codegen::backends::device_provider;
    using gpu_f32_tensor = lithe::codegen::backends::metal_f32_tensor;
    using gpu_device_submission = lithe::codegen::backends::metal_device_submission;

    // ============================================================================
    // gpu_backend — capability probe + elementwise SPIR-V compile/install/dispatch.
    //
    // compile_elementwise() produces a validated SPIR-V module. Availability is
    // true when native Metal or Vulkan is usable; the Vulkan-only overloads
    // return no_device when Vulkan is not compiled in.
    // ============================================================================

    struct gpu_backend {
        [[nodiscard]] static bool metal_available() noexcept {
            return lithe::codegen::backends::metal_backend::available();
        }

        [[nodiscard]] static constexpr bool vulkan_available() noexcept {
#if defined(LITHE_VULKAN_BACKEND_AVAILABLE) && LITHE_VULKAN_BACKEND_AVAILABLE
            return true;
#else
            return false;
#endif
        }

        [[nodiscard]] static constexpr gpu_provider
        select_provider(const bool metal_is_available,
                        const bool vulkan_is_available) noexcept {
            return lithe::codegen::backends::select_device_provider(
                metal_is_available, vulkan_is_available);
        }

        [[nodiscard]] static gpu_provider preferred_provider() noexcept {
            return select_provider(metal_available(), vulkan_available());
        }

        [[nodiscard]] static bool available() noexcept {
            return preferred_provider() != gpu_provider::none;
        }

        [[nodiscard]] static bool supports(const lithe::codegen::device::kernel_plan& plan) noexcept {
            return lithe::codegen::backends::metal_backend::supports(plan)
                && lithe::codegen::backends::supports_spirv_elementwise_plan(plan);
        }

        // Build a validated SPIR-V module for the requested elementwise op. This is
        // always available (no device needed) and is what the test suite checks.
        [[nodiscard]] lithe::codegen::backends::spirv_module
        compile_elementwise(gpu_elementwise_op op) const {
            return spirv_elementwise_emitter{}.emit(op);
        }

        [[nodiscard]] lithe::codegen::backends::spirv_module
        compile_elementwise(const lithe::codegen::device::kernel_plan& plan,
                            const gpu_elementwise_op op) const {
            if (!supports(plan)) return {};
            return compile_elementwise(op);
        }

        [[nodiscard]] lithe::codegen::compilation_artifact
        compile_metal(const lithe::codegen::device::kernel_plan& plan) const {
            if (!supports(plan)) {
                lithe::codegen::compilation_artifact artifact;
                artifact.diagnostics.push_back("crank gpu: HL-MIR kernel is outside the shared f32 binary contract");
                return artifact;
            }
            return lithe::codegen::backends::metal_backend{}.emit(plan);
        }

        [[nodiscard]] gpu_dispatch_result
        install(const lithe::codegen::device::kernel_plan& plan,
                const gpu_elementwise_op op) const {
            if (!supports(plan)) {
                return {gpu_dispatch_status::unsupported_shape,
                        "gpu: HL-MIR region is outside the shared f32 binary contract"};
            }
            if (preferred_provider() == gpu_provider::metal) {
                auto artifact = compile_metal(plan);
                if (!artifact.ok()) {
                    return {gpu_dispatch_status::no_device,
                            artifact.diagnostics.empty() ? "gpu: Metal pipeline compilation failed"
                                                         : artifact.diagnostics.back()};
                }
                return {gpu_dispatch_status::ok, "gpu: native Metal pipeline installed"};
            }
#if defined(LITHE_VULKAN_BACKEND_AVAILABLE) && LITHE_VULKAN_BACKEND_AVAILABLE
            if (preferred_provider() != gpu_provider::vulkan) {
                return {gpu_dispatch_status::no_device,
                        "gpu: neither Metal nor Vulkan/MoltenVK is available"};
            }
            namespace vk_be = lithe::codegen::backends;
            namespace ex = lithe::execution;
            auto module = compile_elementwise(plan, op);
            if (module.validate() != lithe::ir::ir_resolution_state::resolved)
                return {gpu_dispatch_status::unsupported_shape, "gpu: shared-plan SPIR-V validation failed"};
            vk_be::vulkan_backend backend;
            auto installed = tag_invoke(ex::cpo::compile_and_install_t{}, backend, std::move(module));
            if (!installed)
                return {gpu_dispatch_status::no_device, "gpu: no Vulkan/MoltenVK device"};
            return {gpu_dispatch_status::ok, "gpu: Vulkan/MoltenVK pipeline installed"};
#else
            static_cast<void>(op);
            return {gpu_dispatch_status::no_device, "gpu: neither Metal nor Vulkan/MoltenVK is available"};
#endif
        }

        [[nodiscard]] gpu_dispatch_result dispatch_metal(
            const lithe::codegen::device::kernel_plan& plan,
            const std::span<float> output,
            const std::span<const float> lhs,
            const std::span<const float> rhs) const {
            if (!supports(plan))
                return {gpu_dispatch_status::unsupported_shape, "gpu: unsupported HL-MIR Metal kernel"};
            auto artifact = compile_metal(plan);
            if (!artifact.ok())
                return {gpu_dispatch_status::no_device,
                        artifact.diagnostics.empty() ? "gpu: Metal pipeline compilation failed"
                                                     : artifact.diagnostics.back()};
            const std::array inputs{lhs, rhs};
            auto dispatched = lithe::codegen::backends::metal_backend::dispatch_f32<2>(
                artifact, output, inputs);
            if (!dispatched)
                return {gpu_dispatch_status::no_device, dispatched.error().message};
            return {gpu_dispatch_status::ok, {}};
        }

        // Explicit device-resident path.  The returned submission owns only the
        // command-buffer completion token; the caller owns its tensors and may
        // use the output tensor as a later kernel input without downloading it.
        [[nodiscard]] std::expected<gpu_device_submission, gpu_dispatch_result>
        dispatch_metal_device_async(
            const lithe::codegen::device::kernel_plan& plan,
            gpu_f32_tensor& output,
            const std::array<const gpu_f32_tensor*, 2>& inputs) const {
            if (!supports(plan))
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::unsupported_shape, "gpu: unsupported HL-MIR Metal kernel"});
            auto artifact = compile_metal(plan);
            if (!artifact.ok())
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::no_device,
                    artifact.diagnostics.empty() ? "gpu: Metal pipeline compilation failed"
                                                 : artifact.diagnostics.back()});
            auto dispatched = lithe::codegen::backends::metal_backend::dispatch_f32_device_async<2>(
                artifact, output, inputs);
            if (!dispatched)
                return std::unexpected(gpu_dispatch_result{
                    gpu_dispatch_status::no_device, dispatched.error().message});
            return std::move(*dispatched);
        }

#if defined(LITHE_VULKAN_BACKEND_AVAILABLE) && LITHE_VULKAN_BACKEND_AVAILABLE
        // Explicit Vulkan/MoltenVK dispatch for opt-in tuning and comparison.
        // Automatic Crank execution still uses preferred_provider(), which ranks
        // native Metal first on macOS. Pravaha owns the transient Vulkan buffers;
        // Lithe owns the SPIR-V module and Vulkan resource lifetime.
        [[nodiscard]] gpu_dispatch_result
        dispatch_vulkan(const lithe::codegen::device::kernel_plan& plan,
                        const std::span<float> output,
                        const std::span<const float> lhs,
                        const std::span<const float> rhs) const {
            if (!supports(plan))
                return {gpu_dispatch_status::unsupported_shape,
                        "gpu: unsupported HL-MIR Vulkan kernel"};
            if (output.empty() || output.size() != lhs.size() || lhs.size() != rhs.size())
                return {gpu_dispatch_status::unsupported_shape,
                        "gpu: Vulkan binary buffers must be non-empty and equally sized"};
            auto module = compile_elementwise(plan, gpu_elementwise_op::add);
            if (module.validate() != lithe::ir::ir_resolution_state::resolved)
                return {gpu_dispatch_status::unsupported_shape,
                        "gpu: shared-plan SPIR-V validation failed"};

            ::pravaha::compute::buffer_descriptor descriptor;
            descriptor.shape.push_back(output.size());
            descriptor.element_type = ::pravaha::compute::data_element_type::f32;
            auto destination = ::pravaha::compute::make_view(output.data(), descriptor);
            const std::array sources{
                ::pravaha::compute::make_const_view(lhs.data(), descriptor),
                ::pravaha::compute::make_const_view(rhs.data(), descriptor),
            };
            const auto dispatched = ::pravaha::backends::vulkan::dispatch_elementwise_full<float, 2>(
                module.identity_hash(), module, destination, sources, module.local_x);
            if (!dispatched)
                return {gpu_dispatch_status::no_device, dispatched.error().message};
            return {gpu_dispatch_status::ok, {}};
        }

        // Compile → install → (device buffers bound by caller) → dispatch. Returns
        // no_device if a physical device / queue could not be acquired so the
        // planner can NADI-pulse to SIMD/CPU. Buffer binding is the caller's
        // data-plane responsibility (storage_buffer_binding); this drives the
        // pipeline + dispatch seam only.
        [[nodiscard]] gpu_dispatch_result
        install(gpu_elementwise_op op) const {
            namespace vk_be = lithe::codegen::backends;
            namespace ex = lithe::execution;

            auto mod = compile_elementwise(op);
            if (mod.validate() != lithe::ir::ir_resolution_state::resolved) {
                return {
                    gpu_dispatch_status::unsupported_shape,
                    "gpu: emitted SPIR-V failed validation"
                };
            }

            vk_be::vulkan_backend backend;
            auto res = tag_invoke(ex::cpo::compile_and_install_t{}, backend, std::move(mod));
            if (!res.has_value()) {
                return {
                    gpu_dispatch_status::no_device,
                    "gpu: no Vulkan device (" + std::string(to_string(op))
                    + " kernel); falling back to SIMD/CPU"
                };
            }
            return {gpu_dispatch_status::ok, ""};
        }
#else
        // No Vulkan: honest no_device so callers fall back explicitly.
        [[nodiscard]] gpu_dispatch_result install(gpu_elementwise_op op) const {
            return {
                gpu_dispatch_status::no_device,
                "gpu: Vulkan backend not compiled in (" + std::string(to_string(op))
                + " kernel); falling back to SIMD/CPU"
            };
        }
#endif
    };
} // namespace crank
