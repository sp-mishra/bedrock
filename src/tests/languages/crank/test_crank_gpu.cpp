// =============================================================================
// test_crank_gpu.cpp — §v2.8 GPU elementwise SPIR-V emitter + Vulkan dispatch.
//
// The SPIR-V emitter is device-independent and always compiled, so its tests
// always run: they assert the emitted module passes the vulkan backend's
// structural validate() and is a legal GLCompute module (magic, Shader-only
// capability, LocalSize execution mode).
//
// The Vulkan dispatch tests are guarded on LITHE_VULKAN_BACKEND_AVAILABLE.
// Metal may still make gpu_backend::available() true; without Vulkan, the
// Vulkan-only install(op) overload returns an honest no_device result.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/gpu_backend.hpp"

using namespace crank;
using lithe::ir::ir_resolution_state;

TEST_CASE (

"gpu emitter produces a module that passes validate() (add)"
,
"[crank][gpu][v2]"
)
 {
    auto mod = gpu_backend{}.compile_elementwise(gpu_elementwise_op::add);
    REQUIRE_FALSE(mod.words.empty());
    CHECK(mod.words[0] == 0x07230203u); // SPIR-V magic
    CHECK(mod.validate() == ir_resolution_state::resolved);
}

TEST_CASE (

"gpu emitter produces a module that passes validate() (mul)"
,
"[crank][gpu][v2]"
)
 {
    auto mod = gpu_backend{}.compile_elementwise(gpu_elementwise_op::mul);
    REQUIRE_FALSE(mod.words.empty());
    CHECK(mod.validate() == ir_resolution_state::resolved);
}

TEST_CASE (

"gpu emitter records LocalSize execution mode"
,
"[crank][gpu][v2]"
)
 {
    auto mod = gpu_backend{}.compile_elementwise(gpu_elementwise_op::add);
    // validate() populates local_{x,y,z} from OpExecutionMode LocalSize.
    (void)mod.validate();
    CHECK(mod.local_x == lithe::codegen::backends::spirv_binary_default_local_x);
    CHECK(mod.local_y == 1u);
    CHECK(mod.local_z == 1u);
}

TEST_CASE (

"gpu emitter add/mul differ only in the arithmetic opcode"
,
"[crank][gpu][v2]"
)
 {
    auto add_mod = gpu_backend{}.compile_elementwise(gpu_elementwise_op::add);
    auto mul_mod = gpu_backend{}.compile_elementwise(gpu_elementwise_op::mul);
    REQUIRE(add_mod.words.size() == mul_mod.words.size());
    std::size_t diffs = 0;
    for (std::size_t i = 0; i < add_mod.words.size(); ++i)
        if (add_mod.words[i] != mul_mod.words[i]) ++diffs;
    // Exactly one word differs: the OpFAdd(129) vs OpFMul(133) opcode word.
    CHECK(diffs == 1u);
}

TEST_CASE (

"gpu op to_string round-trips"
,
"[crank][gpu][v2]"
)
 {
    CHECK(to_string(gpu_elementwise_op::add) == "add");
    CHECK(to_string(gpu_elementwise_op::mul) == "mul");
}

TEST_CASE (

"gpu dispatch_status to_string round-trips"
,
"[crank][gpu][v2]"
)
 {
    CHECK(to_string(gpu_dispatch_status::ok) == "ok");
    CHECK(to_string(gpu_dispatch_status::unsupported_shape) == "unsupported_shape");
    CHECK(to_string(gpu_dispatch_status::no_device) == "no_device");
}

#if defined(LITHE_VULKAN_BACKEND_AVAILABLE) && LITHE_VULKAN_BACKEND_AVAILABLE

TEST_CASE ("gpu backend advertises availability when Vulkan is compiled in", "[crank][gpu][v2][vulkan]") {
    CHECK(gpu_backend::available());
}

TEST_CASE ("gpu install returns ok on a live device or an honest no_device", "[crank][gpu][v2][vulkan]") {
    auto res = gpu_backend{}.install(gpu_elementwise_op::add);
    // Either a device accepted the pipeline (ok) or none was available
    // (no_device with a NADI-pulse note). unsupported_shape would mean the
    // emitted SPIR-V failed validation — that must not happen here.
    CHECK(res.status != gpu_dispatch_status::unsupported_shape);
    if (!res.ok()) {
        CHECK(res.status == gpu_dispatch_status::no_device);
        CHECK_FALSE(res.note.empty());
    }
}

#else

TEST_CASE (

"gpu Vulkan dispatch falls back honestly without Vulkan"
,
"[crank][gpu][v2]"
)
 {
    CHECK_FALSE(gpu_backend::vulkan_available());
    auto res = gpu_backend{}.install(gpu_elementwise_op::add);
    CHECK(res.status == gpu_dispatch_status::no_device);
    CHECK_FALSE(res.ok());
    CHECK_FALSE(res.note.empty()); // NADI-pulse fallback note
}

#endif

TEST_CASE (

"gpu provider selection prefers Metal before Vulkan"
,
"[crank][gpu][v2][selection]"
)
 {
    CHECK(gpu_backend::select_provider(true, true) == gpu_provider::metal);
    CHECK(gpu_backend::select_provider(true, false) == gpu_provider::metal);
    CHECK(gpu_backend::select_provider(false, true) == gpu_provider::vulkan);
    CHECK(gpu_backend::select_provider(false, false) == gpu_provider::none);
}

TEST_CASE("crank gpu delegates binary SPIR-V emission to Lithe", "[crank][gpu][lithe]")
{
    const auto crank_module = gpu_backend{}.compile_elementwise(gpu_elementwise_op::add);
    const auto lithe_module = lithe::codegen::backends::emit_spirv_binary_elementwise(
        lithe::codegen::backends::spirv_binary_operation::add);

    CHECK(crank_module.words == lithe_module.words);
    CHECK(crank_module.identity_hash() == lithe_module.identity_hash());
}
