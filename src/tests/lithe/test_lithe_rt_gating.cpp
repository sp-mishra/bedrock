// =============================================================================
// test_lithe_rt_gating.cpp — P0A/P0B gate tests for the managed runtime.
//
// Asserts:
//   1. Native executable-code install returns native_install_unavailable
//      (not a fake success) — P0B gate.
//   2. Interpreter-program (compile()) install succeeds and the managed_function
//      is bound — interpreter vertical path (impl-2 P3) must not be broken.
//   3. active_frame_count() is readable and increments/decrements around the
//      managed_frame_guard RAII — P0A active-frame counter invariant.
//   4. Root-relocation: a rooted_ref tracks the authoritative slot value;
//      a manually-updated slot is visible through the rooted_ref.
//   5. Safepoint requested flag is cleared after end_collection.
//
// Does NOT modify existing lithe_rt tests.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/lithe_rt.hpp"

using namespace lithe::rt;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {
    lithe::runtime::mop::object_layout make_layout(std::uint64_t id) {
        lithe::runtime::mop::object_layout lay;
        lay.layout_id = id;
        lay.size_bytes = 16;
        lay.alignment = 16;
        lay.type_name = "test";
        return lay;
    }

    // Minimal physical_mir_function for compile() testing.
    lithe::codegen::mir::physical_mir_function make_empty_mir() {
        lithe::codegen::mir::physical_mir_function fn;
        lithe::codegen::allocated_basic_block blk;
        blk.id = 0;
        fn.function.blocks.push_back(std::move(blk));
        return fn;
    }
}

// ---------------------------------------------------------------------------
// P0B: native install returns native_install_unavailable, not fake success
// ---------------------------------------------------------------------------
TEST_CASE (


"P0B: invoke without entry target returns native_install_unavailable"
,
"[lithe][rt][gating][p0b]"
)
 {
    auto rt = runtime_instance::create(
        {lithe::rt::execution_profile::managed_language});
    REQUIRE(rt.has_value());

    auto fn = compile(**rt, make_empty_mir());
    REQUIRE(fn.has_value());
    REQUIRE(fn->bound());

    auto thread = (*rt)->attach_current_thread();
    REQUIRE(thread.has_value());

    auto result = fn->invoke(*thread, {});
    // Must fail — no entry target.
    REQUIRE_FALSE(result.has_value());
    const auto& err = result.error();
    // The error detail must mention native_install_unavailable.
    REQUIRE(err.detail.find("native_install_unavailable") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Interpreter install: compile() succeeds; managed_function is bound
// ---------------------------------------------------------------------------
TEST_CASE (


"P0B: interpreter install: compile() succeeds and function is bound"
,
"[lithe][rt][gating][p0b]"
)
 {
    auto rt = runtime_instance::create(
        {lithe::rt::execution_profile::managed_language});
    REQUIRE(rt.has_value());

    auto fn = compile(**rt, make_empty_mir());
    REQUIRE(fn.has_value());
    REQUIRE(fn->bound());
    // A code version was installed.
    REQUIRE((*rt)->code().size() == 1);
}

// ---------------------------------------------------------------------------
// P0A: active_frame_count() readable; managed_frame_guard increments/decrements
// ---------------------------------------------------------------------------
TEST_CASE (


"P0A: managed_frame_guard increments and decrements active_frames"
,
"[lithe][rt][gating][p0a]"
)
 {
    auto rt = runtime_instance::create(
        {lithe::rt::execution_profile::managed_language});
    REQUIRE(rt.has_value());

    auto thread = (*rt)->attach_current_thread();
    REQUIRE(thread.has_value());
    thread_context& ctx = thread->context();

    REQUIRE(ctx.managed_depth == 0);
    REQUIRE(ctx.phase.load() == thread_phase::outside);

    {
        managed_frame_guard guard(ctx);
        REQUIRE(ctx.managed_depth == 1);
        REQUIRE(ctx.phase.load() == thread_phase::running);

        {
            managed_frame_guard nested(ctx);
            REQUIRE(ctx.managed_depth == 2);
        }
        REQUIRE(ctx.managed_depth == 1);
        REQUIRE(ctx.phase.load() == thread_phase::running);
    }

    REQUIRE(ctx.managed_depth == 0);
    REQUIRE(ctx.phase.load() == thread_phase::outside);
}

// ---------------------------------------------------------------------------
// P0A: code_resource active_frame_count() accessor is stable
// ---------------------------------------------------------------------------
TEST_CASE (


"P0A: code_resource active_frame_count readable without engine.hpp"
,
"[lithe][rt][gating][p0a]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());

    auto fn = compile(**rt, make_empty_mir());
    REQUIRE(fn.has_value());

    // Access the installed code resource directly through the code_manager.
    const auto ver = fn->version();
    const auto res = (*rt)->code().find(ver);
    REQUIRE(res != nullptr);
    REQUIRE(res->active_frame_count() == 0);
    REQUIRE_FALSE(res->has_active_frames());
}

// ---------------------------------------------------------------------------
// P0A: root-relocation — slot value rewrite visible through rooted_ref
// ---------------------------------------------------------------------------
TEST_CASE (


"P0A: rooted_ref reflects slot rewrite after relocation"
,
"[lithe][rt][gating][p0a]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());

    // Register a layout and allocate a rooted object.
    (*rt)->layouts().register_layout(make_layout(1));
    auto ref = (*rt)->allocate(1);
    REQUIRE(ref.has_value());

    const object_ref original = ref->get();
    REQUIRE(original.valid());
    REQUIRE(original.layout_id == 1);

    // Simulate the collector rewriting the root slot to a new address.
    object_ref* slot = ref->slot();
    REQUIRE(slot != nullptr);

    // Write a simulated-relocated address into the slot directly.
    const object_ref relocated{reinterpret_cast<void*>(0xdeadbeef00UL), 1, 0};
    *slot = relocated;

    // rooted_ref::get() reads through the authoritative slot — must see new value.
    const object_ref live = ref->get();
    REQUIRE(live.ptr == relocated.ptr);
}

// ---------------------------------------------------------------------------
// P0A: safepoint flag is cleared after end_collection
// ---------------------------------------------------------------------------
TEST_CASE (


"P0A: safepoint requested flag clears after end_collection"
,
"[lithe][rt][gating][p0a]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());

    safepoint_coordinator& coord = (*rt)->safepoints();
    REQUIRE_FALSE(coord.collection_requested());

    auto req = coord.request_collection();
    REQUIRE(req.has_value());
    REQUIRE(coord.collection_requested());

    coord.end_collection();
    REQUIRE_FALSE(coord.collection_requested());
}
