#pragma once

// =============================================================================
// backends/lithe_codegen_vulkan.hpp — Vulkan / MoltenVK compute backend
//
// A per-backend facet adapter following the interpreter-facet shape:
//   (1) payload / resource types in namespace lithe::execution,
//   (2) backend_traits<vulkan_backend> specialised there,
//   (3) tag_invoke customisations in namespace lithe::codegen::backends (ADL).
//
// Unlike the interpreter, the Vulkan backend is FUSED: vkCreateShaderModule +
// vkCreateComputePipelines (compile) and command-pool / descriptor-pool /
// descriptor-set allocation (install) happen together against a live VkDevice,
// so it models cpo::compile_and_install (like AsmJit's JitRuntime::add()).  It
// adds an async execution_event path backed by VkFence.
//
// PLATFORM GUARD : the entire backend body is wrapped so it compiles to
// nothing when no Vulkan driver is present.  The engine core never names
// HAS_MOLTENVK / HAS_VULKAN — those live only here.  One backend body serves
// MoltenVK (macOS portability driver) and native ICDs.
//
// Errors fold into install_error / execution_error / compile_install_error —
// never ir_error () — so selection can fall back to a host backend.
//
// No virtual, no macros in the engine core.  Header-only C++23, macOS-first.
// =============================================================================

#if __has_include(<vulkan/vulkan.h>) && (defined(HAS_MOLTENVK) || defined(HAS_VULKAN))
#define LITHE_VULKAN_BACKEND_AVAILABLE 1
#else
#define LITHE_VULKAN_BACKEND_AVAILABLE 0
#endif

#if LITHE_VULKAN_BACKEND_AVAILABLE

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "../lithe_execution/facet.hpp"        // cpo::*, type_tag
#include "../lithe_execution/artifact.hpp"     // basic_compiled_artifact, artifact_manifest
#include "../lithe_execution/resource.hpp"     // invocation_request/result, dynamic_execution_result
#include "../lithe_execution/entry.hpp"        // typed_entry, entry_lease, frame_counter_ref
#include "../lithe_execution/foundation.hpp"   // memory_domain, buffer, kernel_launch, execution_event, backend_lifetime, execution_mode::device


#include "../lithe_extension.hpp"              // plugin_descriptor, version_triple
#include "../lithe_semantic.hpp"               // domain_type (complete type for descriptor)
#include "lithe_codegen_vulkan_spirv_ir.hpp"   // spirv_module + spirv_ir_provider ()

#include "containers/associative/slot_map.hpp"
#include "containers/handle/generational_handle.hpp"

namespace lithe::execution {
    // =========================================================================
    // VkContext — shared device / queue slot (RAII, stack-owned via shared_ptr).
    //
    // One instance per backend slot, SHARED across resources (not per-resource).
    // Instance creation enables the portability path so the same body serves
    // MoltenVK and native ICDs.  Non-copyable / non-movable (raw handles).
    // =========================================================================
    class VkContext {
    public:
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice phys_dev = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        std::uint32_t compute_family = VK_QUEUE_FAMILY_IGNORED;
        VkQueue queue = VK_NULL_HANDLE;

        VkContext() = default;
        VkContext(const VkContext&) = delete;
        VkContext& operator=(const VkContext&) = delete;

        ~VkContext() {
            if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
            if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        }

        [[nodiscard]] bool valid() const noexcept {
            return device != VK_NULL_HANDLE && queue != VK_NULL_HANDLE;
        }

        // Bring up instance + physical device + logical device + compute queue.
        // Returns false on any failure (context left invalid, safe to destroy).
        [[nodiscard]] bool create() noexcept {

#ifdef __APPLE__
// Suppress MoltenVK info/warning spam (errors still visible at
// level 1). Must precede the first vkCreateInstance so the ICD
// reads it at init. setenv overwrite=0 respects a user override.
::setenv ("MVK_CONFIG_LOG_LEVEL", "1", 0);
#endif
VkApplicationInfo app_info{};
app_info.sType= VK_STRUCTURE_TYPE_APPLICATION_INFO;
app_info.apiVersion= VK_API_VERSION_1_0;

const char* exts[] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
};

VkInstanceCreateInfo ci{};
ci.sType= VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
ci.pApplicationInfo=&app_info;
ci.enabledExtensionCount=1;
ci.ppEnabledExtensionNames= exts;
ci.flags= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

            if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS)
                return false;

std::uint32_t count = 0;
vkEnumeratePhysicalDevices(instance, &count, nullptr);
            if (count== 0) return false;
std::vector<VkPhysicalDevice> devs(count);
vkEnumeratePhysicalDevices(instance, &count, devs.data());
phys_dev= devs[0];

std::uint32_t qf_count = 0;
vkGetPhysicalDeviceQueueFamilyProperties(phys_dev, &qf_count, nullptr);
std::vector<VkQueueFamilyProperties> qf(qf_count);
vkGetPhysicalDeviceQueueFamilyProperties(phys_dev, &qf_count, qf.data());
            for (std::uint32_t i =0; i<qf_count;++i) {
                if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    compute_family = i;
                    break;
                }
            }
            if (compute_family== VK_QUEUE_FAMILY_IGNORED) return false;

const float prio = 1.0f;
VkDeviceQueueCreateInfo qci{};
qci.sType= VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
qci.queueFamilyIndex= compute_family;
qci.queueCount=1;
qci.pQueuePriorities=&prio;

VkDeviceCreateInfo dci{};
dci.sType= VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
dci.queueCreateInfoCount=1;
dci.pQueueCreateInfos=&qci;

            if (vkCreateDevice(phys_dev, &dci, nullptr, &device) != VK_SUCCESS)
                return false;

vkGetDeviceQueue(device, compute_family, 0, &queue);
            return queue!= VK_NULL_HANDLE;
        }
    };

// Map a lithe memory_domain to Vulkan memory property flags.  Returns 0 for
// an unsupported domain (guest_sandbox) so the caller folds to install_error.
[[nodiscard]] inline VkMemoryPropertyFlags
vk_memory_flags_for(const memory_domain d) noexcept {
    switch (d) {
    case memory_domain::host_cpu:
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    case memory_domain::device_gpu:
        return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    case memory_domain::shared_unified: // Apple Silicon unified fast path
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    case memory_domain::guest_sandbox: // unsupported on device
    default:
        return 0;
    }
}

// =========================================================================
// vulkan_pipeline_payload — owns the compiled compute pipeline objects.
//
// Analogous to AsmJit's JitRuntime-owned function block, but a compute
// pipeline.  RAII: destroys in reverse creation order on the shared device.
// =========================================================================
struct vulkan_pipeline_payload {
    std::shared_ptr<VkContext> ctx;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout dset_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::uint32_t local_x = 1, local_y = 1, local_z = 1;
    std::uint32_t binding_count = 2; // storage buffers in the set layout

    vulkan_pipeline_payload() = default;
    vulkan_pipeline_payload(const vulkan_pipeline_payload&) = delete;
    vulkan_pipeline_payload& operator=(const vulkan_pipeline_payload&) = delete;
    vulkan_pipeline_payload(vulkan_pipeline_payload&& o) noexcept { move_from(o); }

    vulkan_pipeline_payload& operator=(vulkan_pipeline_payload&& o) noexcept {
        if (this != &o) {
            destroy();
            move_from(o);
        }
        return *this;
    }

    ~vulkan_pipeline_payload() { destroy(); }

    [[nodiscard]] bool valid() const noexcept {
        return ctx && ctx->valid() && pipeline != VK_NULL_HANDLE;
    }

private:
    void move_from(vulkan_pipeline_payload& o) noexcept {
        ctx = std::move(o.ctx);
        module = o.module;
        o.module = VK_NULL_HANDLE;
        dset_layout = o.dset_layout;
        o.dset_layout = VK_NULL_HANDLE;
        pipe_layout = o.pipe_layout;
        o.pipe_layout = VK_NULL_HANDLE;
        pipeline = o.pipeline;
        o.pipeline = VK_NULL_HANDLE;
        local_x = o.local_x;
        local_y = o.local_y;
        local_z = o.local_z;
        binding_count = o.binding_count;
    }

    void destroy() noexcept {
        if (!ctx || ctx->device == VK_NULL_HANDLE) return;
        VkDevice dev = ctx->device;
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
        if (pipe_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipe_layout, nullptr);
        if (dset_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, dset_layout, nullptr);
        if (module != VK_NULL_HANDLE) vkDestroyShaderModule(dev, module, nullptr);
        pipeline = VK_NULL_HANDLE;
        pipe_layout = VK_NULL_HANDLE;
        dset_layout = VK_NULL_HANDLE;
        module = VK_NULL_HANDLE;
    }
};

// =========================================================================
// storage_buffer_binding — pure-Vulkan seam for binding a caller-owned
// VkBuffer to a resource's descriptor set.  No ownership; no non-Vulkan
// types.  Lets a data-plane owner (buffer alloc + staging lives with the
// caller) drive Lithe's device/pipeline/dispatch path generically.
// =========================================================================
struct storage_buffer_binding {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize range = VK_WHOLE_SIZE;
};

// =========================================================================
// vulkan_resource — installed device lease.
//
// Owns the compiled pipeline (shared), the shared device/queue, a command
// pool + descriptor pool/set, and a backend_lifetime pin for the lease.
// =========================================================================
class vulkan_resource {
public:
    vulkan_resource() = default;

    vulkan_resource(std::shared_ptr<vulkan_pipeline_payload> payload,
                    std::shared_ptr<VkContext> ctx,
                    VkCommandPool cmd_pool,
                    VkDescriptorPool desc_pool,
                    VkDescriptorSet desc_set,
                    std::shared_ptr<backend_lifetime> life)
        : payload_(std::move(payload))
          , ctx_(std::move(ctx))
          , cmd_pool_(cmd_pool)
          , desc_pool_(desc_pool)
          , desc_set_(desc_set)
          , life_(std::move(life))
          , counter_(make_frame_counter()) {}

    vulkan_resource(const vulkan_resource&) = default;
    vulkan_resource& operator=(const vulkan_resource&) = default;
    vulkan_resource(vulkan_resource&&) noexcept = default;
    vulkan_resource& operator=(vulkan_resource&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return payload_ && payload_->valid() && ctx_ && ctx_->valid()
            && cmd_pool_ != VK_NULL_HANDLE && counter_ != nullptr;
    }

    [[nodiscard]] const frame_counter_ref& counter() const noexcept { return counter_; }

    // Synchronous dispatch: record → submit → vkWaitForFences → return.
    [[nodiscard]]
    std::expected<invocation_result, execution_error>
    dispatch_sync(const kernel_launch& launch) const {
        if (!valid())
            return std::unexpected(execution_error{"vulkan_resource: invalid"});

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!record_dispatch(launch, cmd))
            return std::unexpected(execution_error{"vulkan: command record failed"});

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(ctx_->device, &fci, nullptr, &fence) != VK_SUCCESS)
            return std::unexpected(execution_error{"vulkan: vkCreateFence failed"});

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(ctx_->queue, 1, &si, fence) != VK_SUCCESS) {
            vkDestroyFence(ctx_->device, fence, nullptr);
            return std::unexpected(execution_error{"vulkan: vkQueueSubmit failed"});
        }

        constexpr std::uint64_t timeout_ns = 2'000'000'000ULL;
        const VkResult wr = vkWaitForFences(ctx_->device, 1, &fence, VK_TRUE, timeout_ns);
        vkDestroyFence(ctx_->device, fence, nullptr);
        if (wr != VK_SUCCESS)
            return std::unexpected(execution_error{"vulkan: fence wait timed out"});

        // A compute kernel writes results into bound storage buffers; the
        // scalar return channel is 0 (device results ride buffer.domain memory).
        return invocation_result::make_int(0);
    }

    // Record a compute dispatch: bind pipeline + descriptor set + dispatch.
    // block_{x,y} MUST match the SPIR-V LocalSize; validated at compile-install.
    [[nodiscard]] bool record_dispatch(const kernel_launch& launch,
                                       VkCommandBuffer& out_cmd) const {
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cmd_pool_;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(ctx_->device, &cbai, &out_cmd) != VK_SUCCESS)
            return false;

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(out_cmd, &begin) != VK_SUCCESS) return false;

        vkCmdBindPipeline(out_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, payload_->pipeline);
        if (desc_set_ != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(out_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    payload_->pipe_layout, 0, 1, &desc_set_, 0, nullptr);
        }
        const std::uint32_t gx = launch.grid_x ? launch.grid_x : 1;
        const std::uint32_t gy = launch.grid_y ? launch.grid_y : 1;
        vkCmdDispatch(out_cmd, gx, gy, 1);
        return vkEndCommandBuffer(out_cmd) == VK_SUCCESS;
    }

    // Async submit: record + submit signalling `fence` (created by the
    // caller / backend fence registry).  Returns false on record/submit
    // failure.  The caller owns the fence lifetime and the wait.
    [[nodiscard]] bool submit_fenced(const kernel_launch& launch,
                                     VkFence fence) const {
        if (!valid() || fence == VK_NULL_HANDLE) return false;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (!record_dispatch(launch, cmd)) return false;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        return vkQueueSubmit(ctx_->queue, 1, &si, fence) == VK_SUCCESS;
    }

    [[nodiscard]] VkDevice device() const noexcept {
        return ctx_ ? ctx_->device : VK_NULL_HANDLE;
    }

    // Validate that a launch's block dims match the pipeline's LocalSize.
    [[nodiscard]] bool block_matches_local_size(const kernel_launch& launch) const noexcept {
        if (!payload_) return false;
        const std::uint32_t bx = launch.block_x ? launch.block_x : 1;
        const std::uint32_t by = launch.block_y ? launch.block_y : 1;
        return bx == payload_->local_x && by == payload_->local_y;
    }

    // Bind caller-owned storage buffers to this resource's own descriptor
    // set at bindings 0..N-1.  Generic (pure Vulkan handles): the caller
    // owns buffer allocation + staging; Lithe owns the set.  Safe by design:
    //   - resource must be valid,
    //   - non-empty, and size() must not exceed the layout's binding_count,
    //   - every buffer handle must be non-null.
    // Reuses the already-allocated desc_set_ (no new pool/set, no leak).
    [[nodiscard]] std::expected<void, execution_error>
    bind_storage_buffers(std::span<const storage_buffer_binding> binds) const {
        if (!valid())
            return std::unexpected(execution_error{"bind_storage_buffers: invalid resource"});
        if (desc_set_ == VK_NULL_HANDLE)
            return std::unexpected(execution_error{"bind_storage_buffers: no descriptor set"});
        if (binds.empty())
            return std::unexpected(execution_error{"bind_storage_buffers: empty bindings"});
        if (binds.size() > payload_->binding_count)
            return std::unexpected(execution_error{"bind_storage_buffers: too many bindings"});
        for (const auto& b : binds)
            if (b.buffer == VK_NULL_HANDLE)
                return std::unexpected(execution_error{"bind_storage_buffers: null buffer"});

        std::vector<VkDescriptorBufferInfo> infos(binds.size());
        std::vector<VkWriteDescriptorSet> writes(binds.size());
        for (std::size_t i = 0; i < binds.size(); ++i) {
            infos[i].buffer = binds[i].buffer;
            infos[i].offset = binds[i].offset;
            infos[i].range = binds[i].range;

            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = desc_set_;
            writes[i].dstBinding = static_cast<std::uint32_t>(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(ctx_->device,
                               static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        return {};
    }

private:
    std::shared_ptr<vulkan_pipeline_payload> payload_;
    std::shared_ptr<VkContext> ctx_;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
    std::shared_ptr<backend_lifetime> life_;
    frame_counter_ref counter_;
};

// =========================================================================
// backend_traits<vulkan_backend> — associated type declarations.
// vulkan_backend is defined below in lithe::codegen::backends; forward it.
// =========================================================================
} // namespace lithe::execution

namespace lithe::codegen::backends {
    struct vulkan_backend;
}

namespace lithe::execution {
    template <>
    struct backend_traits<codegen::backends::vulkan_backend> {
        template <class IR>
        using artifact = basic_compiled_artifact<vulkan_pipeline_payload>;
        template <class Artifact>
        using resource = vulkan_resource;
        template <class R, class Sig>
        using entry = typed_entry<Sig>;
    };
} // namespace lithe::execution

// =============================================================================
// vulkan_backend + fused tag_invoke customisations (namespace ...backends).
// ADL for tag_invoke(compile_and_install_t{}, vulkan_backend&, ...) searches
// the associated namespaces of the backend argument, which lives here.
// =============================================================================

namespace lithe::codegen::backends {
    // Phantom tag for the fence-registry slot_map handle.
    struct vk_fence_tag {};

    using vk_fence_handle = containers::generational_handle<vk_fence_tag>;

    struct vulkan_backend {
        // --- LitheExtension protocol -----------------------------------------
        static constexpr lithe::plugin_descriptor<
            sizeof("lithe.backend.vulkan"),
            sizeof("Lithe")
        > descriptor{
            .id = "lithe.backend.vulkan",
            .version = {1, 0, 0},
            .author = "Lithe",
            .domain = lithe::semantic::domain_type::unknown,
        };

        // Device tier reported to selection (compile_requirements::target).
        static constexpr lithe::execution::execution_mode tier
            = lithe::execution::execution_mode::device;

        vulkan_backend()
            : ctx_(std::make_shared<lithe::execution::VkContext>())
              , life_(std::make_shared<lithe::execution::backend_lifetime>()) {}

        // Bring the shared device up on first use.  Idempotent; safe to fail
        // (leaves ctx invalid so compile_and_install returns install_error).
        [[nodiscard]] bool ensure_device() {
            if (ctx_->valid()) return true;
            return ctx_->create();
        }

        [[nodiscard]] const std::shared_ptr<lithe::execution::VkContext>& context() const noexcept {
            return ctx_;
        }

        [[nodiscard]] const std::shared_ptr<lithe::execution::backend_lifetime>&
        lifetime() const noexcept { return life_; }

        // Fence registry: execution_event.id ↔ VkFence via slot_map handle.
        [[nodiscard]] lithe::execution::execution_event register_fence(VkFence f) {
            const vk_fence_handle h = fences_.insert(f);
            return lithe::execution::execution_event{pack_handle(h)};
        }

        [[nodiscard]] VkFence* find_fence(const lithe::execution::execution_event ev) {
            return fences_.find(unpack_handle(ev.id));
        }

        void retire_fence(const lithe::execution::execution_event ev) {
            fences_.erase(unpack_handle(ev.id));
        }

        // Async dispatch: create a VkFence, submit the launch signalling it,
        // register the fence, pin the lease.  The returned event OWNS/PINS the
        // device resource until wait_event()/poll_event() observes signal ():
        // the retirement drain counts an outstanding event like a live frame.
        [[nodiscard]]
        std::expected<lithe::execution::execution_event, lithe::execution::execution_error>
        dispatch_async(lithe::execution::vulkan_resource& res,
                       const lithe::execution::kernel_launch& launch) {
            namespace ex = lithe::execution;
            if (!res.valid())
                return std::unexpected(ex::execution_error{"vulkan: async on invalid resource"});
            VkFenceCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            if (vkCreateFence(res.device(), &fci, nullptr, &fence) != VK_SUCCESS)
                return std::unexpected(ex::execution_error{"vulkan: vkCreateFence failed"});
            if (!res.submit_fenced(launch, fence)) {
                vkDestroyFence(res.device(), fence, nullptr);
                return std::unexpected(ex::execution_error{"vulkan: async submit failed"});
            }
            (void)life_->try_pin(); // event pins the lease
            ev_device_ = res.device();
            return register_fence(fence);
        }

        // Blocking wait (bounded timeout): on signal, unpin + free the fence.
        [[nodiscard]]
        std::expected<void, lithe::execution::execution_error>
        wait_event(const lithe::execution::execution_event ev) {
            namespace ex = lithe::execution;
            VkFence* f = find_fence(ev);
            if (!f) return std::unexpected(ex::execution_error{"vulkan: unknown event"});
            constexpr std::uint64_t timeout_ns = 2'000'000'000ULL;
            const VkResult wr = vkWaitForFences(ev_device_, 1, f, VK_TRUE, timeout_ns);
            if (wr != VK_SUCCESS)
                return std::unexpected(ex::execution_error{"vulkan: event wait timed out"});
            vkDestroyFence(ev_device_, *f, nullptr);
            retire_fence(ev);
            life_->unpin();
            return {};
        }

        // Non-blocking poll: true once the fence has signalled (does not retire).
        [[nodiscard]] bool poll_event(const lithe::execution::execution_event ev) {
            VkFence* f = find_fence(ev);
            if (!f) return false;
            return vkGetFenceStatus(ev_device_, *f) == VK_SUCCESS;
        }

        // Pack/unpack a generational_handle into the u64 execution_event id.
        static constexpr std::uint64_t pack_handle(const vk_fence_handle h) noexcept {
            return (static_cast<std::uint64_t>(h.generation) << 32)
                | static_cast<std::uint64_t>(h.index);
        }

        static constexpr vk_fence_handle unpack_handle(const std::uint64_t id) noexcept {
            return vk_fence_handle{
                static_cast<std::uint32_t>(id & 0xFFFF'FFFFu),
                static_cast<std::uint32_t>(id >> 32)
            };
        }

    private:
        std::shared_ptr<lithe::execution::VkContext> ctx_;
        std::shared_ptr<lithe::execution::backend_lifetime> life_;
        containers::slot_map<VkFence, vk_fence_handle> fences_;
        VkDevice ev_device_ = VK_NULL_HANDLE;
    };

    // --- pipeline build helper (compile stage) -----------------------------
    // Validates SPIR-V structurally then builds shader module + descriptor-set
    // layout + pipeline layout + compute pipeline into a payload.  Returns a
    // filled payload on success (RAII-owned), or nullptr on any failure.
    [[nodiscard]] inline std::shared_ptr<lithe::execution::vulkan_pipeline_payload>
    vk_build_pipeline(vulkan_backend& backend,
                      lithe::codegen::backends::spirv_module&& ir,
                      std::uint32_t binding_count = 2) {
        namespace ex = lithe::execution;

        // Structural SPIR-V validation: magic / entry point / LocalSize / caps.
        if (ir.validate() != lithe::ir::ir_resolution_state::resolved) return nullptr;
        if (binding_count == 0) return nullptr;
        if (!backend.ensure_device()) return nullptr;

        auto ctx = backend.context();
        auto payload = std::make_shared<ex::vulkan_pipeline_payload>();
        payload->ctx = ctx;
        payload->local_x = ir.local_x;
        payload->local_y = ir.local_y;
        payload->local_z = ir.local_z;
        payload->binding_count = binding_count;

        // vkCreateShaderModule from the 32-bit-word blob.
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = ir.words.size() * sizeof(std::uint32_t);
        smci.pCode = ir.words.data();
        if (vkCreateShaderModule(ctx->device, &smci, nullptr, &payload->module) != VK_SUCCESS)
            return nullptr;

        // Descriptor-set layout: `binding_count` storage buffers (bindings 0..N-1).
        std::vector<VkDescriptorSetLayoutBinding> bindings(binding_count);
        for (std::uint32_t i = 0; i < binding_count; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = binding_count;
        dslci.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(ctx->device, &dslci, nullptr,
                                        &payload->dset_layout) != VK_SUCCESS)
            return nullptr;

        // Pipeline layout (+ push-constant range for small scalar params).
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = 16;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &payload->dset_layout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(ctx->device, &plci, nullptr,
                                   &payload->pipe_layout) != VK_SUCCESS)
            return nullptr;

        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = payload->module;
        cpci.stage.pName = "main";
        cpci.layout = payload->pipe_layout;
        if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                     &payload->pipeline) != VK_SUCCESS)
            return nullptr;

        return payload;
    }

    // --- pool alloc + resource wrap helper (install stage) ------------------
    // Allocates command pool + descriptor pool + descriptor set, pins the lease,
    // and wraps into a live vulkan_resource.  Returns nullopt on failure.
    [[nodiscard]] inline std::optional<lithe::execution::vulkan_resource>
    vk_alloc_pools_and_wrap(vulkan_backend& backend,
                            std::shared_ptr<lithe::execution::vulkan_pipeline_payload> payload) {
        namespace ex = lithe::execution;
        if (!payload || !payload->valid()) return std::nullopt;
        auto ctx = payload->ctx;

        VkCommandPoolCreateInfo poolci{};
        poolci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolci.queueFamilyIndex = ctx->compute_family;
        poolci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool cmd_pool = VK_NULL_HANDLE;
        if (vkCreateCommandPool(ctx->device, &poolci, nullptr, &cmd_pool) != VK_SUCCESS)
            return std::nullopt;

        VkDescriptorPoolSize dps{};
        dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        dps.descriptorCount = payload->binding_count;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &dps;
        VkDescriptorPool desc_pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(ctx->device, &dpci, nullptr, &desc_pool) != VK_SUCCESS) {
            vkDestroyCommandPool(ctx->device, cmd_pool, nullptr);
            return std::nullopt;
        }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = desc_pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &payload->dset_layout;
        VkDescriptorSet desc_set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(ctx->device, &dsai, &desc_set) != VK_SUCCESS) {
            vkDestroyDescriptorPool(ctx->device, desc_pool, nullptr);
            vkDestroyCommandPool(ctx->device, cmd_pool, nullptr);
            return std::nullopt;
        }

        (void)backend.lifetime()->try_pin();
        return ex::vulkan_resource{
            std::move(payload), ctx, cmd_pool, desc_pool, desc_set, backend.lifetime()
        };
    }

    // --- compile (split path: build the pipeline payload) ------------------
    // Provided so compiler_for<vulkan_backend, spirv_module> holds; the device
    // objects live in the artifact payload until install allocates its pools.
    [[nodiscard]] inline
    std::expected<lithe::execution::basic_compiled_artifact<lithe::execution::vulkan_pipeline_payload>,
                  lithe::execution::compile_error>
    tag_invoke(lithe::execution::cpo::compile_t,
               vulkan_backend& backend,
               lithe::codegen::backends::spirv_module&& ir) {
        namespace ex = lithe::execution;
        auto payload = vk_build_pipeline(backend, std::move(ir));
        if (!payload)
            return std::unexpected(ex::compile_error{"vulkan: SPIR-V compile failed"});

        ex::artifact_manifest manifest;
        manifest.produced_from = ex::ir_kind::jit_code;
        manifest.role = ex::artifact_class::native_code;
        manifest.backend_id = "lithe.backend.vulkan";

        return ex::basic_compiled_artifact<ex::vulkan_pipeline_payload>{
            std::move(manifest), std::move(*payload)
        };
    }

    // --- install (split path: allocate pools + wrap) -----------------------
    [[nodiscard]] inline
    std::expected<lithe::execution::vulkan_resource, lithe::execution::install_error>
    tag_invoke(lithe::execution::cpo::install_t,
               vulkan_backend& backend,
               lithe::execution::basic_compiled_artifact<
                   lithe::execution::vulkan_pipeline_payload>&& art) {
        namespace ex = lithe::execution;
        if (!art.payload.valid())
            return std::unexpected(ex::install_error{"vulkan: invalid artifact payload"});
        auto payload = std::make_shared<ex::vulkan_pipeline_payload>(std::move(art.payload));
        auto res = vk_alloc_pools_and_wrap(backend, std::move(payload));
        if (!res)
            return std::unexpected(ex::install_error{"vulkan: install pool allocation failed"});
        return std::move(*res);
    }

    // --- fused compile_and_install -----------------------------------------
    // SPIR-V module in → live vulkan_resource out.  Returns compile_install_error
    // on any failure (never ir_error, ) so selection falls back to a host
    // backend.
    [[nodiscard]] inline
    std::expected<lithe::execution::vulkan_resource,
                  lithe::execution::compile_install_error>
    tag_invoke(lithe::execution::cpo::compile_and_install_t,
               vulkan_backend& backend,
               lithe::codegen::backends::spirv_module&& ir) {
        namespace ex = lithe::execution;
        auto payload = vk_build_pipeline(backend, std::move(ir));
        if (!payload)
            return std::unexpected(ex::compile_install_error{"vulkan: SPIR-V compile+install failed"});
        auto res = vk_alloc_pools_and_wrap(backend, std::move(payload));
        if (!res)
            return std::unexpected(ex::compile_install_error{"vulkan: install pool allocation failed"});
        return std::move(*res);
    }

    // --- get_entry (typed: int64_t(int64_t, int64_t)) ----------------------
    // The device entry launches a single-workgroup dispatch and returns the
    // scalar channel (buffer results ride device memory, not this return path).
    [[nodiscard]] inline
    std::expected<
        lithe::execution::typed_entry < std::int64_t(std::int64_t, std::int64_t)>
    ,
    lithe::execution::execution_error
    >
    tag_invoke(lithe::execution::cpo::get_entry_t,
               vulkan_backend& /*backend*/,
               lithe::execution::vulkan_resource& res,
               lithe::execution::type_tag<std::int64_t(std::int64_t, std::int64_t)>) {
        namespace ex = lithe::execution;
        if (!res.valid())
            return std::unexpected(ex::execution_error{"vulkan_resource invalid"});

        ex::entry_lease lease{res.counter()};

        auto fn = [res_copy = res](std::int64_t /*a*/, std::int64_t /*b*/) -> std::int64_t {
            ex::kernel_launch launch{};
            launch.grid_x = 1;
            launch.grid_y = 1;
            auto r = res_copy.dispatch_sync(launch);
            if (!r.has_value()) return 0;
            return r->raw_value;
        };

        return ex::typed_entry < std::int64_t(std::int64_t, std::int64_t) >
        {
            std::move(lease),
                std::function < std::int64_t(std::int64_t, std::int64_t) >
            {
                std::move(fn)
            }
        };
    }

    // --- invoke (erased path) ----------------------------------------------
    [[nodiscard]] inline
    std::expected<lithe::execution::dynamic_execution_result,
                  lithe::execution::execution_error>
    tag_invoke(lithe::execution::cpo::invoke_t,
               vulkan_backend& /*backend*/,
               lithe::execution::vulkan_resource& res,
               lithe::execution::invocation_request /*req*/) {
        namespace ex = lithe::execution;
        ex::kernel_launch launch{};
        launch.grid_x = 1;
        launch.grid_y = 1;
        auto r = res.dispatch_sync(launch);
        if (!r.has_value())
            return std::unexpected(r.error());
        return ex::dynamic_execution_result{*r};
    }

    // --- release ------------------------------------------------------------
    inline void
    tag_invoke(lithe::execution::cpo::release_t,
               vulkan_backend& backend,
               lithe::execution::vulkan_resource&& /*res*/) {
        // Resource RAII destroys its own pipeline/pools; unpin the lease so a
        // pending unregister can drain.
        backend.lifetime()->unpin();
    }
} // namespace lithe::codegen::backends

#endif // LITHE_VULKAN_BACKEND_AVAILABLE
