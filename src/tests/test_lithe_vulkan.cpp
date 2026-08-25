// =============================================================================
// test_lithe_vulkan.cpp — SPIR-V structural adapter + Vulkan/MoltenVK backend
//
// Structure (impl-4 Part B):
//   • Cases 1–3 are pure SPIR-V structural checks (no VkDevice) and ALWAYS run.
//   • Cases 4–9 exercise the device/async path; they compile only when a Vulkan
//     driver header is present AND skip cleanly (not fail) when no device can be
//     created, so CI without a GPU still passes.
//
// The SPIR-V module shape mirrors the hand-assembled trivial compute module in
// src/examples/example_vulkan_moltenvk.hpp (void main(){}, LocalSize 1 1 1).
//
// Tests append-only (new file).  No virtual, no macros in core.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <cstdint>
#include <vector>

// SPIR-V IR adapter is device-free (no vulkan.h) — always available.
#include "lithe/backends/lithe_codegen_vulkan_spirv_ir.hpp"
#include "lithe/lithe_ir/provider.hpp"
#include "lithe/lithe_ir/format.hpp"

namespace vk_be = lithe::codegen::backends;
namespace irns = lithe::ir;

// ============================================================================
// Shared: the trivial compute module (magic, caps, entry point, LocalSize 1 1 1)
// ============================================================================

namespace {
    std::vector<std::uint32_t> trivial_spirv_words() {
        return {
            // Header
            0x07230203u, // Magic
            0x00010000u, // Version 1.0
            0x000d000bu, // Generator
            0x00000005u, // Bound
            0x00000000u, // Schema
            // OpCapability Shader (opcode 17, wc 2)
            0x00020011u, 0x00000001u,
            // OpMemoryModel Logical GLSL450 (opcode 14, wc 3)
            0x0003000eu, 0x00000000u, 0x00000001u,
            // OpEntryPoint GLCompute %main "main" (opcode 15, wc 5)
            0x0005000fu, 0x00000005u, 0x00000001u, 0x6e69616du, 0x00000000u,
            // OpExecutionMode %main LocalSize 1 1 1 (opcode 16, wc 6)
            0x00060010u, 0x00000001u, 0x00000011u, 0x00000001u, 0x00000001u, 0x00000001u,
            // %void = OpTypeVoid (opcode 19, wc 2)
            0x00020013u, 0x00000002u,
            // %fn_t = OpTypeFunction %void (opcode 33, wc 3)
            0x00030021u, 0x00000003u, 0x00000002u,
            // %main = OpFunction %void None %fn_t (opcode 54, wc 5)
            0x00050036u, 0x00000002u, 0x00000001u, 0x00000000u, 0x00000003u,
            // %entry = OpLabel (opcode 248, wc 2)
            0x000200f8u, 0x00000004u,
            // OpReturn (opcode 253, wc 1)
            0x000100fdu,
            // OpFunctionEnd (opcode 56, wc 1)
            0x00010038u,
        };
    }
} // namespace

// ============================================================================
// Case 1 — validate_ir accepts a well-formed module → resolved; local_x == 1
// ============================================================================

TEST_CASE (


"spirv validate_ir: well-formed compute module resolves [P14b]"
,
"[vulkan][spirv][validate]"
)
{
    vk_be::spirv_module mod;
    mod.words = trivial_spirv_words();

    const auto state = mod.validate();
    CHECK(state == irns::ir_resolution_state::resolved);
    CHECK(mod.local_x == 1u);
    CHECK(mod.local_y == 1u);
    CHECK(mod.local_z == 1u);
    CHECK(mod.valid());

    // Same verdict via the generic provider CPO (ADL on spirv_ir_provider).
    vk_be::spirv_ir_provider prov;
    CHECK(irns::cpo::validate_ir(prov, mod) == irns::ir_resolution_state::resolved);
}

// ============================================================================
// Case 2 — validate_ir rejects truncated / bad-magic modules
// ============================================================================

TEST_CASE (


"spirv validate_ir: bad magic and truncated modules are not resolved [P14b]"
,
"[vulkan][spirv][validate]"
)
{
    // Bad magic.
    {
        vk_be::spirv_module mod;
        mod.words = trivial_spirv_words();
        mod.words[0] = 0xDEADBEEFu;
        CHECK(mod.validate() != irns::ir_resolution_state::resolved);
    }
    // Truncated header (fewer than 5 header words).
    {
        vk_be::spirv_module mod;
        mod.words = {0x07230203u, 0x00010000u};
        CHECK(mod.validate() != irns::ir_resolution_state::resolved);
    }
    // Truncated instruction (word-count overruns the buffer).
    {
        vk_be::spirv_module mod;
        mod.words = trivial_spirv_words();
        mod.words.resize(7); // cut mid-instruction after header + partial cap
        CHECK(mod.validate() != irns::ir_resolution_state::resolved);
    }
    // No entry point (header only, well-formed but missing OpEntryPoint).
    {
        vk_be::spirv_module mod;
        mod.words = {0x07230203u, 0x00010000u, 0x000d000bu, 0x00000001u, 0x00000000u};
        CHECK(mod.validate() != irns::ir_resolution_state::resolved);
    }
    // Empty module.
    {
        vk_be::spirv_module mod;
        CHECK(!mod.valid());
        CHECK(mod.validate() != irns::ir_resolution_state::resolved);
    }
}

// ============================================================================
// Case 3 — export_binary round-trips words; word-count sum == words.size()
// ============================================================================

TEST_CASE (


"spirv export_binary: round-trips words and is well-formed [P14b]"
,
"[vulkan][spirv][export]"
)
{
    vk_be::spirv_module mod;
    mod.words = trivial_spirv_words();

    vk_be::spirv_ir_provider prov;
    irns::format_descriptor fmt{
        irns::encoding::binary_le,
        irns::stage::physical,
        {1, 0, 0},
        32,
        lithe::execution::ir_kind::jit_code};

    auto result = irns::cpo::export_binary(prov, mod, fmt);
    REQUIRE(result.has_value());
    CHECK(result->data.size() == mod.words.size() * sizeof(std::uint32_t));

    // Independent structural well-formedness: the high 16 bits of each header
    // instruction are its word count; summing over the body must equal the
    // remaining word span exactly.
    const auto& w = mod.words;
    std::size_t i = 5; // skip 5 header words
    std::size_t consumed = 0;
    bool overran = false;
    while (i < w.size()) {
        const std::uint16_t wc = static_cast<std::uint16_t>(w[i] >> 16);
        if (wc == 0 || i + wc > w.size()) { overran = true; break; }
        consumed += wc;
        i += wc;
    }
    CHECK(!overran);
    CHECK(consumed == w.size() - 5);

    // Deterministic identity is stable and content-derived (pipeline-cache key).
    CHECK(mod.identity_hash() == vk_be::spirv_module{mod.words}.identity_hash());
}

// ============================================================================
// Cases 4–9 — device path (Vulkan present).  Skip cleanly with no device.
// ============================================================================

#if __has_include(<vulkan/vulkan.h>) && defined(HAS_MOLTENVK)

#include "lithe/backends/lithe_codegen_vulkan.hpp"
#include "lithe/lithe_execution/capability.hpp"
#include "lithe/lithe_engine.hpp"                 // lithe::selectable_backend

#include <type_traits>

namespace ex = lithe::execution;

// Compile-time (Case 4): selectable_backend holds via the fused OR branch.
// vulkan_backend provides compile + install (split) AND compile_and_install
// (fused), so selectable_backend is satisfied for SPIR-V IR + a device signature.
static_assert(
    lithe::selectable_backend<
        vk_be::vulkan_backend,
        vk_be::spirv_module,
        std::int64_t(std::int64_t, std::int64_t)>,
    "vulkan_backend must be selectable for spirv_module (fused branch)");

// Case 9 (partial, compile-time): device tier is reported and mode-selectable.
static_assert(vk_be::vulkan_backend::tier == ex::execution_mode::device);

namespace {
    // Try to bring a device up; returns true if a real compute device exists.
    bool device_available() {
        lithe::execution::VkContext ctx;
        return ctx.create();
    }
}

TEST_CASE ("vulkan: fused compile_and_install returns a live resource [P14b]",
          "[vulkan][device][compile_and_install]")
{
    if (!device_available()) { SKIP("no Vulkan compute device present"); }

    vk_be::vulkan_backend backend;
    vk_be::spirv_module mod;
    mod.words = trivial_spirv_words();

    auto res = tag_invoke(ex::cpo::compile_and_install_t{}, backend, std::move(mod));
    REQUIRE(res.has_value());
    CHECK(res->valid());
}

TEST_CASE ("vulkan: split compile then install also yields a live resource [P14b]",
          "[vulkan][device][compile]")
{
    if (!device_available()) { SKIP("no Vulkan compute device present"); }

    vk_be::vulkan_backend backend;
    vk_be::spirv_module mod;
    mod.words = trivial_spirv_words();

    auto art = tag_invoke(ex::cpo::compile_t{}, backend, std::move(mod));
    REQUIRE(art.has_value());
    auto res = tag_invoke(ex::cpo::install_t{}, backend, std::move(*art));
    REQUIRE(res.has_value());
    CHECK(res->valid());
}

TEST_CASE ("vulkan: memory_domain mapping (host/shared supported, guest unsupported) [P14b]",
          "[vulkan][device][memory]")
{
    // Domain→flag mapping is pure and testable without a device.
    CHECK(ex::vk_memory_flags_for(ex::memory_domain::host_cpu) != 0);
    CHECK(ex::vk_memory_flags_for(ex::memory_domain::shared_unified) != 0);
    CHECK(ex::vk_memory_flags_for(ex::memory_domain::device_gpu) != 0);
    // guest_sandbox is unsupported on device → 0 (folds to install_error upstream).
    CHECK(ex::vk_memory_flags_for(ex::memory_domain::guest_sandbox) == 0);
}

TEST_CASE ("vulkan: kernel_launch block dims must match SPIR-V LocalSize [P14b]",
          "[vulkan][device][geometry]")
{
    if (!device_available()) { SKIP("no Vulkan compute device present"); }

    vk_be::vulkan_backend backend;
    vk_be::spirv_module mod;
    mod.words = trivial_spirv_words();          // LocalSize 1 1 1

    auto res = tag_invoke(ex::cpo::compile_and_install_t{}, backend, std::move(mod));
    REQUIRE(res.has_value());

    ex::kernel_launch good{};
    good.grid_x = 4; good.block_x = 1; good.block_y = 1;
    CHECK(res->block_matches_local_size(good));

    ex::kernel_launch bad{};
    bad.grid_x = 4; bad.block_x = 8; bad.block_y = 1; // mismatch vs LocalSize 1
    CHECK(!res->block_matches_local_size(bad));
}

TEST_CASE ("vulkan: async execution_event is VkFence-backed and pins the lease [P14b]",
          "[vulkan][device][async]")
{
    if (!device_available()) { SKIP("no Vulkan compute device present"); }

    vk_be::vulkan_backend backend;
    vk_be::spirv_module mod;
    mod.words = trivial_spirv_words();

    auto res = tag_invoke(ex::cpo::compile_and_install_t{}, backend, std::move(mod));
    REQUIRE(res.has_value());

    const std::uint64_t refs_before = backend.lifetime()->live_refs();

    ex::kernel_launch launch{};
    launch.grid_x = 1; launch.grid_y = 1;
    auto ev = backend.dispatch_async(*res, launch);
    REQUIRE(ev.has_value());
    CHECK(ev->valid());

    // Outstanding event pins the lease (drain counts it like a live frame).
    CHECK(backend.lifetime()->live_refs() > refs_before);

    // Blocking wait (bounded) completes and unpins.
    auto w = backend.wait_event(*ev);
    REQUIRE(w.has_value());
    CHECK(backend.lifetime()->live_refs() == refs_before);
}

TEST_CASE ("vulkan: execution_mode::device selection under compile_requirements [P14b]",
          "[vulkan][device][selection]")
{
    // A device-only requirement admits the device tier and forbids none of it.
    ex::compile_requirements reqs;
    reqs.allowed_modes.set(ex::execution_mode::device);
    CHECK(reqs.mode_allowed(ex::execution_mode::device));
    CHECK(!reqs.mode_allowed(ex::execution_mode::interpret));
    CHECK(reqs.any_mode_allowed());

    // A forbidden-device requirement rejects the vulkan tier.
    ex::compile_requirements forbid;
    forbid.forbidden_modes.set(ex::execution_mode::device);
    CHECK(!forbid.mode_allowed(vk_be::vulkan_backend::tier));
}

#endif // Vulkan present
