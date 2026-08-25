// ============================================================================
// test_lithe_runtime_foundation.cpp — Catch2 tests for lithe::rt (Layer 1 + GC)
//
// Covers the typed-MIR value model, the unified trap model, the generational
// garbage collector, and the owning runtime_instance.  Auto-discovered by the
// CMake test glob (src/tests/*.cpp) — no CMake edit required.
// ============================================================================

#include "catch_amalgamated.hpp"

#include <atomic>
#include <thread>

#include "lithe/lithe_rt.hpp"

using namespace lithe::rt;
namespace mop = lithe::runtime::mop;
namespace sp = lithe::runtime::safepoint;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {
    // A layout with one managed-reference field at offset 0 in its payload.
    mop::object_layout make_ref_layout(std::uint64_t id, std::size_t payload) {
        mop::object_layout lay;
        lay.layout_id = id;
        lay.size_bytes = payload;
        lay.alignment = 16;
        lay.type_name = "test_obj";
        mop::field_descriptor f;
        f.name = "ref0";
        f.byte_offset = 0;
        f.size_bytes = sizeof(object_ref);
        f.type_tag = 0x8000'0000u; // managed_field_bit
        lay.field_map.emplace(f.name, f);
        return lay;
    }

    mop::object_layout make_plain_layout(std::uint64_t id, std::size_t payload) {
        mop::object_layout lay;
        lay.layout_id = id;
        lay.size_bytes = payload;
        lay.alignment = 16;
        lay.type_name = "plain";
        return lay;
    }
} // namespace

// ===========================================================================
// typed_value
// ===========================================================================
TEST_CASE (


"typed_value classifies signed/unsigned integers"
,
"[lithe][rt][types]"
)
 {
    lithe::codegen::abstract_value_type u;
    u.kind = lithe::codegen::abstract_value_kind::integer;
    u.bit_width = 32;
    u.semantic_type = "u32";
    const auto tu = classify(u);
    REQUIRE(tu.sign == int_sign::unsigned_int);
    REQUIRE(tu.bit_width == 32);

    lithe::codegen::abstract_value_type s;
    s.kind = lithe::codegen::abstract_value_kind::integer;
    s.bit_width = 64;
    s.semantic_type = "i64";
    REQUIRE(classify(s).sign == int_sign::signed_int);
}

TEST_CASE (


"typed_value classifies pointer classes"
,
"[lithe][rt][types]"
)
 {
    const auto mk = [](const char* st) {
        lithe::codegen::abstract_value_type t;
        t.kind = lithe::codegen::abstract_value_kind::pointer;
        t.semantic_type = st;
        return classify(t);
    };
    REQUIRE(mk("gc_ref").pclass == ptr_class::managed_base);
    REQUIRE(mk("gc_ref.derived").pclass == ptr_class::managed_derived);
    REQUIRE(mk("guest_ptr").pclass == ptr_class::guest_offset);
    REQUIRE(mk("host_ptr").pclass == ptr_class::raw_host);
    REQUIRE(mk("gc_ref.derived").is_derived());
    REQUIRE(mk("gc_ref").is_managed());
    REQUIRE_FALSE(mk("host_ptr").is_managed());
}

TEST_CASE (


"typed_value round-trips through to_abstract"
,
"[lithe][rt][types]"
)
 {
    typed_value v;
    v.kind = lithe::codegen::abstract_value_kind::integer;
    v.bit_width = 16;
    v.sign = int_sign::unsigned_int;
    const auto back = classify(to_abstract(v));
    REQUIRE(back.sign == int_sign::unsigned_int);
    REQUIRE(back.bit_width == 16);

    typed_value p;
    p.kind = lithe::codegen::abstract_value_kind::pointer;
    p.pclass = ptr_class::managed_derived;
    REQUIRE(classify(to_abstract(p)).pclass == ptr_class::managed_derived);
}

TEST_CASE (


"value_role composes and queries"
,
"[lithe][rt][types]"
)
 {
    const auto set = value_role::exception_value | value_role::safepoint;
    REQUIRE(has_role(set, value_role::exception_value));
    REQUIRE(has_role(set, value_role::safepoint));
    REQUIRE_FALSE(has_role(set, value_role::deopt_state));
}

// ===========================================================================
// trap
// ===========================================================================
TEST_CASE (


"trap carries full context"
,
"[lithe][rt][trap]"
)
 {
    const auto t = trap::make(trap_code::out_of_bounds, 7, 3, 42, 0x100, "idx>=len");
    REQUIRE(t.code == trap_code::out_of_bounds);
    REQUIRE(t.function_id == 7);
    REQUIRE(t.code_version == 3);
    REQUIRE(t.mir_instruction == 42);
    REQUIRE(t.machine_offset == 0x100);
    REQUIRE(t.message() == "out_of_bounds: idx>=len");
}

TEST_CASE (


"may_trap_kinds maps trapping opcodes"
,
"[lithe][rt][trap]"
)
 {
    const auto d = may_trap_kinds(lithe::codegen::opcode::div);
    REQUIRE(d.contains(trap_code::division_by_zero));
    REQUIRE(d.contains(trap_code::integer_overflow));

    const auto ld = may_trap_kinds(lithe::codegen::opcode::load);
    REQUIRE(ld.contains(trap_code::out_of_bounds));
    REQUIRE(ld.contains(trap_code::null_reference));

    const auto add = may_trap_kinds(lithe::codegen::opcode::add);
    REQUIRE(add.count == 0); // add cannot trap
}

TEST_CASE (


"trap::from_exception attaches payload"
,
"[lithe][rt][trap]"
)
 {
    object_ref payload{reinterpret_cast<void*>(0xABC), 5, 1};
    const auto t = trap::from_exception(payload, 9, 11);
    REQUIRE(t.code == trap_code::uncaught_exception);
    REQUIRE(t.exception_payload.has_value());
    REQUIRE(t.exception_payload->layout_id == 5);
}

// ===========================================================================
// generational_gc
// ===========================================================================
TEST_CASE (


"gc satisfies both collector concepts"
,
"[lithe][rt][gc]"
)
 {
    STATIC_REQUIRE(Collector<generational_gc>);
    STATIC_REQUIRE(sp::GarbageCollector<generational_gc>);
}

TEST_CASE (


"gc allocates in the young generation"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto r = gc.allocate(1);
    REQUIRE(r.has_value());
    REQUIRE(r->valid());
    REQUIRE(gc.in_young(*r));
    REQUIRE(header_of(*r)->layout_id == 1);
    REQUIRE(gc.stats().total_allocations == 1);
}

TEST_CASE (


"gc rejects unknown layout"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    generational_gc gc(&reg);
    auto r = gc.allocate(999);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == trap_code::corrupted_artifact);
}

TEST_CASE (


"gc enforces the allocation limit"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 32));
    gc_config cfg;
    cfg.maximum_object_count = 2;
    generational_gc gc(&reg, cfg);

    REQUIRE(gc.allocate(1).has_value());
    REQUIRE(gc.allocate(1).has_value());
    auto third = gc.allocate(1);
    REQUIRE_FALSE(third.has_value());
    REQUIRE(third.error().code == trap_code::out_of_memory);
}

TEST_CASE (


"gc reclaims unreachable young objects"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    // A rooted survivor and an unrooted garbage object.
    auto survivor = gc.allocate(1);
    auto garbage  = gc.allocate(1);
    REQUIRE(survivor.has_value());
    REQUIRE(garbage.has_value());

    object_ref root_slot = *survivor;
    gc.add_root(root_handle{&root_slot});

    const auto before = gc.stats().collections;
    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());
    REQUIRE(gc.stats().collections == before + 1);

    // Survivor was forwarded to a new location; root slot now points there.
    REQUIRE(root_slot.valid());
}

TEST_CASE (


"gc promotes aged survivors to the old generation"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    gc_config cfg;
    cfg.promotion_age = 2;
    generational_gc gc(&reg, cfg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());
    object_ref root_slot = *obj;
    gc.add_root(root_handle{&root_slot});

    // Survive enough collections to cross the promotion threshold.
    for (int i = 0; i < 4; ++i)
        REQUIRE(gc.collect(collection_reason::explicit_request).has_value());

    REQUIRE(gc.stats().promotions >= 1);
    REQUIRE(gc.is_live_old(root_slot));
}

TEST_CASE (


"gc clears a weak reference to a dead object"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());
    weak_handle w = gc.make_weak(*obj); // not rooted → dies on collect

    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());
    REQUIRE_FALSE(w.get().valid());
}

TEST_CASE (


"gc runs a finalizer for a dead object"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());

    bool finalized = false;
    gc.register_finalizer(*obj, [&](object_ref) { finalized = true; });

    REQUIRE(gc.collect(collection_reason::explicit_request).has_value()); // *obj is unrooted
    REQUIRE(finalized);
}

TEST_CASE (


"gc traces managed fields through a collection"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_ref_layout(2, sizeof(object_ref)));
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto child  = gc.allocate(1);
    auto parent = gc.allocate(2);
    REQUIRE(child.has_value());
    REQUIRE(parent.has_value());

    // Store child into parent's managed field via the write barrier.
    auto* field = reinterpret_cast<object_ref*>(payload_of(header_of(*parent)));
    gc.write_barrier(*parent, field, *child);

    object_ref root = *parent;
    gc.add_root(root_handle{&root});
    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());

    // Parent survived; its field still points at a live (forwarded) child.
    auto* moved_field =
        reinterpret_cast<object_ref*>(payload_of(header_of(root)));
    REQUIRE(moved_field->valid());
}

TEST_CASE (


"gc large objects bypass the young generation"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 128 * 1024)); // > large_threshold
    generational_gc gc(&reg);

    auto r = gc.allocate(1);
    REQUIRE(r.has_value());
    REQUIRE(header_of(*r)->is_large == 1);
    REQUIRE(gc.is_live_old(*r));
}

TEST_CASE (


"trigger_safepoint drives the collector"
,
"[lithe][rt][gc]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    sp::stack_map_table table;
    sp::stack_map sm;
    sm.fn_name = "f";
    table.register_map(sm);

    const auto before = gc.stats().collections;
    sp::trigger_safepoint("f", table, gc);
    REQUIRE(gc.stats().collections == before + 1);
}

// ===========================================================================
// runtime_instance
// ===========================================================================
TEST_CASE (


"runtime_instance owns its heap and registries"
,
"[lithe][rt][instance]"
)
 {
    auto rt = runtime_instance::create({execution_profile::managed_language});
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_plain_layout(1, 64));

    auto r = (*rt)->heap().allocate(1);
    REQUIRE(r.has_value());
    REQUIRE((*rt)->heap().stats().total_allocations == 1);
}

TEST_CASE (


"untrusted_sandbox locks security controls"
,
"[lithe][rt][instance]"
)
 {
    const auto d = profile_defaults::for_profile(execution_profile::untrusted_sandbox);
    REQUIRE(d.require_verification);
    REQUIRE(d.bounds_checks);
    REQUIRE(d.enforce_fuel);
    REQUIRE(d.enforce_w_xor_x);
    REQUIRE(d.restrict_imports);
    REQUIRE(d.sandbox_locked());

    const auto emb = profile_defaults::for_profile(execution_profile::trusted_embedded);
    REQUIRE_FALSE(emb.sandbox_locked());
}

TEST_CASE (


"code_manager installs and finds versions"
,
"[lithe][rt][instance]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    code_version_metadata md;
    md.function = 5;
    auto mem = executable_memory::reserve(0, false);
    REQUIRE(mem.has_value());
    const auto res = (*rt)->code().install(std::move(*mem), std::move(md));
    REQUIRE(res.has_value());
    REQUIRE((*rt)->code().size() == 1);
    const auto found = (*rt)->code().find((*res)->metadata.version_id);
    REQUIRE(found != nullptr);
    REQUIRE(found->metadata.function == 5);
}

// ===========================================================================
// M1 — collector safety (prompt §34)
// ===========================================================================
TEST_CASE (


"generational_gc is non-movable"
,
"[lithe][rt][gc][m1]"
)
 {
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<generational_gc>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<generational_gc>);
    // heap_region moves safely (no double free); copy is deleted.
    STATIC_REQUIRE(std::is_move_constructible_v<heap_region>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<heap_region>);
}

TEST_CASE (


"compute_object_size rejects non-pow2 alignment"
,
"[lithe][rt][gc][m1]"
)
 {
    auto lay = make_plain_layout(1, 64);
    lay.alignment = 24; // not a power of two
    const auto sz = compute_object_size(lay);
    REQUIRE_FALSE(sz.has_value());
    REQUIRE(sz.error().code == trap_code::corrupted_artifact);
}

TEST_CASE (


"compute_object_size accepts an over-aligned layout"
,
"[lithe][rt][gc][m1]"
)
 {
    auto lay = make_plain_layout(1, 64);
    lay.alignment = 64; // larger than gc_header's 16
    const auto sz = compute_object_size(lay);
    REQUIRE(sz.has_value());
    REQUIRE(sz->alignment == 64);
    REQUIRE((sz->payload_offset % 64) == 0);
}

TEST_CASE (


"compute_object_size detects size overflow"
,
"[lithe][rt][gc][m1]"
)
 {
    auto lay = make_plain_layout(1, SIZE_MAX - 8);
    const auto sz = compute_object_size(lay);
    REQUIRE_FALSE(sz.has_value());
    REQUIRE(sz.error().code == trap_code::out_of_memory);
}

TEST_CASE (


"compute_object_size enforces maximum_object_bytes"
,
"[lithe][rt][gc][m1]"
)
 {
    const auto lay = make_plain_layout(1, 4096);
    const auto sz = compute_object_size(lay, /*maximum_object_bytes=*/128);
    REQUIRE_FALSE(sz.has_value());
    REQUIRE(sz.error().code == trap_code::out_of_memory);
}

TEST_CASE (


"compute_object_size rejects an undersized managed field"
,
"[lithe][rt][gc][m1]"
)
 {
    auto lay = make_plain_layout(1, 64);
    mop::field_descriptor f;
    f.name = "bad";
    f.byte_offset = 0;
    f.size_bytes = 1; // smaller than object_ref
    f.type_tag = 0x8000'0000u;
    lay.field_map.emplace(f.name, f);
    const auto sz = compute_object_size(lay);
    REQUIRE_FALSE(sz.has_value());
    REQUIRE(sz.error().code == trap_code::corrupted_artifact);
}

TEST_CASE (


"pinned object survives repeated collections"
,
"[lithe][rt][gc][m1]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto p = gc.allocate_pinned(1);
    REQUIRE(p.has_value());
    const void* addr = p->ptr;
    REQUIRE(gc.is_pinned(*p));

    object_ref root = *p;
    gc.add_root(root_handle{&root});
    for (int i = 0; i < 3; ++i)
        REQUIRE(gc.collect(collection_reason::explicit_request).has_value());

    // A pinned object never moves and is never swept while reachable.
    REQUIRE(root.ptr == addr);
    REQUIRE(gc.is_live_old(root));
}

TEST_CASE (


"old object retains young ref across minor collections"
,
"[lithe][rt][gc][m1]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_ref_layout(2, sizeof(object_ref)));
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    // Force the parent into old space by pinning it there.
    auto parent = gc.allocate_pinned(2);
    auto child  = gc.allocate(1);
    REQUIRE(parent.has_value());
    REQUIRE(child.has_value());

    auto* field = reinterpret_cast<object_ref*>(payload_of(header_of(*parent)));
    gc.write_barrier(*parent, field, *child);

    object_ref root = *parent;
    gc.add_root(root_handle{&root});
    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());

    auto* moved = reinterpret_cast<object_ref*>(payload_of(header_of(root)));
    REQUIRE(moved->valid()); // young child survived via the remembered set
}

TEST_CASE (


"heap limit is enforced during promotion"
,
"[lithe][rt][gc][m1]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    gc_config cfg;
    cfg.promotion_age    = 1;      // promote quickly
    cfg.maximum_heap_bytes = 64;   // too small to hold a promoted object
    generational_gc gc(&reg, cfg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());
    object_ref root = *obj;
    gc.add_root(root_handle{&root});

    // The first collection tries to promote and must trap on the heap limit.
    const auto c = gc.collect(collection_reason::explicit_request);
    REQUIRE_FALSE(c.has_value());
    REQUIRE(c.error().code == trap_code::out_of_memory);
}

TEST_CASE (


"duplicate root registration and removal"
,
"[lithe][rt][gc][m1]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());
    object_ref slot = *obj;

    gc.add_root(root_handle{&slot});
    gc.add_root(root_handle{&slot}); // duplicate is tolerated
    gc.remove_root(root_handle{&slot});
    // A null transient root must be ignored, not crash.
    gc.add_root(root_handle{nullptr});
    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());
}

TEST_CASE (


"finalizer exceptions are swallowed as traps"
,
"[lithe][rt][gc][m1]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());
    gc.register_finalizer(*obj, [](object_ref) { throw std::runtime_error("boom"); });

    // The throwing finalizer must not propagate out of collect().
    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());
}

// ===========================================================================
// M2 — roots, thread attachment, safepoints (prompt §11–§14)
// ===========================================================================
TEST_CASE (


"rooted_ref releases its slot on destruction"
,
"[lithe][rt][roots][m2]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_plain_layout(1, 64));

    {
        auto root = (*rt)->allocate(1);
        REQUIRE(root.has_value());
        REQUIRE(root->get().valid());
    } // rooted_ref out of scope → slot released, no leak / no crash

    auto root2 = (*rt)->allocate(1);
    REQUIRE(root2.has_value());
}

TEST_CASE (


"store_reference performs the write barrier"
,
"[lithe][rt][roots][m2]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_ref_layout(2, sizeof(object_ref)));
    (*rt)->layouts().register_layout(make_plain_layout(1, 64));

    auto parent = (*rt)->allocate(2);
    auto child  = (*rt)->allocate(1);
    REQUIRE(parent.has_value());
    REQUIRE(child.has_value());

    auto st = (*rt)->store_reference(*parent, 0, child->get());
    REQUIRE(st.has_value());

    auto st_null = [&] {
        rooted_ref empty;
        return (*rt)->store_reference(empty, 0, child->get());
    }();
    REQUIRE_FALSE(st_null.has_value());
    REQUIRE(st_null.error().code == trap_code::null_reference);
}

TEST_CASE (


"thread attaches and detaches"
,
"[lithe][rt][roots][m2]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());

    {
        auto thread = (*rt)->attach_current_thread();
        REQUIRE(thread.has_value());
        REQUIRE(thread->attached());
        REQUIRE((*rt)->safepoints().thread_count() == 1);
    }
    REQUIRE((*rt)->safepoints().thread_count() == 0);
}

TEST_CASE (


"safepoint coordinator single-thread fast path"
,
"[lithe][rt][safepoint][m2]"
)
 {
    safepoint_coordinator co;
    thread_context t;
    co.register_thread(t);
    REQUIRE(co.request_collection().has_value()); // one thread → fast path
    co.end_collection();
    REQUIRE_FALSE(co.collection_requested());
    co.unregister_thread(t);
}

TEST_CASE (


"machine root location resolves to a writable slot"
,
"[lithe][rt][safepoint][m2]"
)
 {
    register_save_area regs;
    object_ref value{reinterpret_cast<void*>(0x1000), 7, 0};
    regs.registers[3] = value;

    machine_root_location loc;
    loc.kind = root_location_kind::register_location;
    loc.register_id = 3;

    safepoint_context ctx;
    ctx.registers = &regs;
    object_ref* slot = ctx.writable_slot(loc);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->layout_id == 7);
}

// ===========================================================================
// M3 — managed metadata + passes (prompt §21–§26, §36 best-effort)
// ===========================================================================
TEST_CASE (


"code_manager retires an idle version"
,
"[lithe][rt][code][m3]"
)
 {
    code_manager cm;
    auto mem = executable_memory::reserve(0, false);
    REQUIRE(mem.has_value());
    code_version_metadata md;
    const auto res = cm.install(std::move(*mem), std::move(md));
    REQUIRE(res.has_value());
    const auto id = (*res)->metadata.version_id;
    REQUIRE(cm.retire(id).has_value());
    REQUIRE(cm.find(id) == nullptr);
    REQUIRE_FALSE(cm.retire(id).has_value()); // already gone
}

TEST_CASE (


"annotate + verify a hand-built managed MIR function"
,
"[lithe][rt][passes][m3]"
)
 {
    lithe::codegen::mir::physical_mir_function fn;
    fn.function.name = "f";
    lithe::codegen::allocated_basic_block block;
    block.id = 0;

    lithe::codegen::allocated_instruction in;
    in.id = 1;
    in.op = lithe::codegen::opcode::mov;
    in.ssa_defs.push_back(lithe::codegen::ssa_value_id{10});
    in.operation_attributes.emplace("type", "gc_ref");
    block.instructions.push_back(in);
    fn.function.blocks.push_back(block);

    const auto ann = annotate_managed_mir{}.run(fn);
    REQUIRE(ann.ok());
    const typed_value* tv = ann.annotations.value_of(10);
    REQUIRE(tv != nullptr);
    REQUIRE(tv->pclass == ptr_class::managed_base);

    const auto vr = verify_managed_mir(fn, ann.annotations,
                                       execution_profile::managed_language);
    REQUIRE(vr.ok);
}

TEST_CASE (


"annotate rejects an ambiguous pointer string"
,
"[lithe][rt][passes][m3]"
)
 {
    lithe::codegen::mir::physical_mir_function fn;
    lithe::codegen::allocated_basic_block block;
    lithe::codegen::allocated_instruction in;
    in.id = 1;
    in.op = lithe::codegen::opcode::mov;
    in.ssa_defs.push_back(lithe::codegen::ssa_value_id{1});
    in.operation_attributes.emplace("type", "mystery_ptr");
    block.instructions.push_back(in);
    fn.function.blocks.push_back(block);

    const auto ann = annotate_managed_mir{}.run(fn);
    REQUIRE_FALSE(ann.ok());
    REQUIRE(ann.error->code == trap_code::corrupted_artifact);
}

// ===========================================================================
// M4 — exceptions (prompt §35)
// ===========================================================================
TEST_CASE (


"exception payload is rooted and survives a collection"
,
"[lithe][rt][exc][m4]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_plain_layout(1, 64));
    auto thread = (*rt)->attach_current_thread();
    REQUIRE(thread.has_value());

    auto obj = (*rt)->heap().allocate(1);
    REQUIRE(obj.has_value());

    const auto begun = begin_throw(**rt, thread->context(), exception_object{*obj});
    REQUIRE(begun.has_value());
    REQUIRE(thread->context().exception.active());

    // A collection during the throw window must not free the rooted payload.
    REQUIRE((*rt)->heap().collect(collection_reason::explicit_request).has_value());
    REQUIRE(thread->context().exception.in_flight.valid());

    consume_exception(**rt, thread->context());
    REQUIRE_FALSE(thread->context().exception.active());
}

TEST_CASE (


"rethrow without an active exception fails"
,
"[lithe][rt][exc][m4]"
)
 {
    thread_context t;
    const auto r = rethrow(t);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == trap_code::uncaught_exception);
}

TEST_CASE (


"handler_table selects the deepest matching catch"
,
"[lithe][rt][exc][m4]"
)
 {
    handler_table table;
    exception_handler outer;
    outer.id = 1;
    outer.region = exception_region{0, 100, 0, std::nullopt};
    outer.kind = handler_kind::catch_typed;
    outer.catch_type = 42;
    table.add(outer);

    exception_handler inner;
    inner.id = 2;
    inner.region = exception_region{10, 50, 1, handler_id{1}};
    inner.kind = handler_kind::catch_typed;
    inner.catch_type = 42;
    table.add(inner);

    table.finalize();
    REQUIRE(table.is_finalized());

    const auto dr = table.dispatch(/*ip=*/20, /*type_id=*/42);
    REQUIRE(dr.caught());
    REQUIRE(*dr.catch_handler == 2); // deepest wins
}

TEST_CASE (


"handler_table catch-all and cleanup sequence"
,
"[lithe][rt][exc][m4]"
)
 {
    handler_table table;
    exception_handler fin;
    fin.id = 1;
    fin.region = exception_region{0, 100, 0, std::nullopt};
    fin.kind = handler_kind::cleanup;
    table.add(fin);

    exception_handler all;
    all.id = 2;
    all.region = exception_region{10, 40, 1, handler_id{1}};
    all.kind = handler_kind::catch_all;
    table.add(all);
    table.finalize();

    const auto dr = table.dispatch(20, /*type_id=*/999);
    REQUIRE(dr.caught());
    REQUIRE(*dr.catch_handler == 2);
    REQUIRE(dr.cleanup_sequence.size() == 1); // the enclosing cleanup
    REQUIRE(dr.cleanup_sequence[0] == 1);
}

TEST_CASE (


"handler_table is immutable after finalize"
,
"[lithe][rt][exc][m4]"
)
 {
    handler_table table;
    exception_handler h;
    h.id = 1;
    h.region = exception_region{0, 10, 0, std::nullopt};
    h.kind = handler_kind::catch_all;
    table.add(h);
    table.finalize();

    const auto before = table.size();
    exception_handler extra;
    extra.id = 2;
    table.add(extra); // must be ignored post-finalize
    REQUIRE(table.size() == before);
}

TEST_CASE (


"uncaught exception converts to a trap retaining payload"
,
"[lithe][rt][exc][m4]"
)
 {
    thread_context t;
    t.exception.in_flight = object_ref{reinterpret_cast<void*>(0xF00), 5, 0};
    t.exception.phase = unwind_phase::searching;
    const auto tr = make_uncaught_trap(t, 3, 7);
    REQUIRE(tr.code == trap_code::uncaught_exception);
    REQUIRE(tr.exception_payload.has_value());
    REQUIRE(tr.exception_payload->layout_id == 5);
}

TEST_CASE (


"guard_foreign_boundary handles void and non-void"
,
"[lithe][rt][exc][m4]"
)
 {
    const auto v = guard_foreign_boundary([] { /* void */ });
    REQUIRE(v.has_value());

    const auto nv = guard_foreign_boundary([] { return 42; });
    REQUIRE(nv.has_value());
    REQUIRE(*nv == 42);
}

TEST_CASE (


"guard_foreign_boundary translates a foreign throw"
,
"[lithe][rt][exc][m4]"
)
 {
    const auto r = guard_foreign_boundary([]() -> int {
        throw std::runtime_error("host failed");
    });
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == trap_code::uncaught_exception);
    REQUIRE(r.error().message().find("host failed") != std::string::npos);
}

// ===========================================================================
// P0 correctness fixes (prompt scratch/prompt_fix_l.md items 1-4, 8, 9)
// ===========================================================================

// --- Item 1: finalizer two-cycle lifecycle, no use-after-free --------------

TEST_CASE (


"finalizer runs exactly once across collections"
,
"[lithe][rt][gc][p0][finalizer]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());

    int calls = 0;
    gc.register_finalizer(*obj, [&](object_ref) { ++calls; });

    // *obj is unrooted → dead this cycle; finalizer runs once.
    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());
    REQUIRE(calls == 1);
    // A second collection must not re-run it (record dropped / finalized flag).
    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());
    REQUIRE(calls == 1);
}

TEST_CASE (


"finalizer of a rooted object does not run"
,
"[lithe][rt][gc][p0][finalizer]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());
    object_ref root = *obj;
    gc.add_root(root_handle{&root});

    bool finalized = false;
    gc.register_finalizer(*obj, [&](object_ref) { finalized = true; });

    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());
    REQUIRE_FALSE(finalized); // still reachable → not finalized
    gc.remove_root(root_handle{&root});
}

TEST_CASE (


"finalizer may resurrect its object by re-rooting"
,
"[lithe][rt][gc][p0][finalizer]"
)
 {
    mop::layout_registry reg;
    reg.register_layout(make_plain_layout(1, 64));
    generational_gc gc(&reg);

    auto obj = gc.allocate(1);
    REQUIRE(obj.has_value());

    object_ref resurrect{};
    // The finalizer re-roots the (relocated) object; reading its header inside
    // the finalizer must be safe — the collector must not have freed it.
    gc.register_finalizer(*obj, [&](object_ref o) {
        resurrect = o;
        REQUIRE(header_of(o) != nullptr);
        REQUIRE(header_of(o)->finalized == 1); // marked before the call
    });

    REQUIRE(gc.collect(collection_reason::explicit_request).has_value());
    REQUIRE(resurrect.valid());
}

// --- Item 2: rooted_ref / exception_state read the authoritative slot -------

TEST_CASE (


"rooted_ref reports the relocated pointer after a collection"
,
"[lithe][rt][roots][p0]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_plain_layout(1, 64));

    auto root = (*rt)->allocate(1);
    REQUIRE(root.has_value());
    const void* before = root->get().ptr;

    // A young collection copies the survivor to to-space; get() must resolve the
    // collector-owned slot, not a stale cached copy.
    REQUIRE((*rt)->heap().collect(collection_reason::explicit_request).has_value());

    REQUIRE(root->get().valid());
    const void* after = root->get().ptr;
    REQUIRE(after != before); // object moved, handle followed it
}

TEST_CASE (


"exception_state live_payload follows relocation"
,
"[lithe][rt][exc][p0]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_plain_layout(1, 64));
    auto thread = (*rt)->attach_current_thread();
    REQUIRE(thread.has_value());

    auto obj = (*rt)->heap().allocate(1);
    REQUIRE(obj.has_value());
    const void* before = obj->ptr;

    REQUIRE(begin_throw(**rt, thread->context(), exception_object{*obj}).has_value());
    REQUIRE((*rt)->heap().collect(collection_reason::explicit_request).has_value());

    const object_ref live = thread->context().exception.live_payload(**rt);
    REQUIRE(live.valid());
    REQUIRE(live.ptr != before); // payload relocated; live slot is authoritative
    consume_exception(**rt, thread->context());
}

// --- Item 3: machine-root scanning + derived re-derivation ------------------

TEST_CASE (


"machine roots evacuate a base and re-derive its interior pointer"
,
"[lithe][rt][safepoint][p0]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_plain_layout(1, 128));

    auto base = (*rt)->heap().allocate(1);
    REQUIRE(base.has_value());
    const object_ref base_before = *base;

    // Register save area: reg[0] = base, reg[1] = interior pointer (base + 16).
    register_save_area regs;
    regs.registers[0] = *base;
    regs.registers[1] = object_ref{
        static_cast<std::byte*>(base->ptr) + 16, base->layout_id, base->plugin_tag};

    machine_root_location base_loc;
    base_loc.kind = root_location_kind::register_location;
    base_loc.register_id = 0;
    base_loc.pointer_kind = ptr_class::managed_base;

    machine_root_location derived_loc;
    derived_loc.kind = root_location_kind::register_location;
    derived_loc.register_id = 1;
    derived_loc.pointer_kind = ptr_class::managed_derived;
    derived_loc.base_root_index = 0;
    derived_loc.derived_offset = 16;

    machine_stack_map map;
    safepoint_entry entry;
    entry.safepoint_id = 5;
    entry.roots.push_back(base_loc);
    entry.roots.push_back(derived_loc);
    map.insert(std::move(entry));

    safepoint_context ctx;
    ctx.registers = &regs;
    ctx.safepoint_id = 5;

    // Publish the base, collect (relocates it), re-derive the interior pointer.
    auto added = register_machine_roots((*rt)->heap(), ctx, map);
    REQUIRE(added.size() == 1); // only the base is a root; derived is re-derived
    REQUIRE((*rt)->heap().collect(collection_reason::explicit_request).has_value());
    rederive_machine_roots(ctx, map);
    for (const auto& rh : added) (*rt)->heap().remove_root(rh);

    REQUIRE(regs.registers[0].ptr != base_before.ptr); // base moved
    // Interior pointer tracks the relocated base + offset exactly.
    REQUIRE(regs.registers[1].ptr
            == static_cast<std::byte*>(regs.registers[0].ptr) + 16);
}

// --- Item 4: safepoint park handshake (multi-thread, no strand) -------------

TEST_CASE (


"safepoint coordinator parks a running mutator via the handshake"
,
"[lithe][rt][safepoint][p0]"
)
 {
    safepoint_coordinator co;
    thread_context main_ctx;
    thread_context worker_ctx;
    co.register_thread(main_ctx);
    co.register_thread(worker_ctx);

    // Worker starts running managed code, then polls in a loop until released.
    worker_ctx.phase.store(thread_phase::running, std::memory_order_release);
    std::atomic<bool> worker_ready{false};

    std::thread worker([&] {
        register_save_area regs;
        safepoint_context ctx;
        ctx.registers = &regs;
        worker_ready.store(true, std::memory_order_release);
        // Poll until the collector's request has been observed and released.
        while (!co.collection_requested())
            std::this_thread::yield();
        co.poll(worker_ctx, ctx); // parks here until end_collection()
    });

    while (!worker_ready.load(std::memory_order_acquire)) std::this_thread::yield();

    // request_collection must block until the worker parks — never return while a
    // thread is still running (the old protocol trapped and stranded it).
    const auto req = co.request_collection();
    REQUIRE(req.has_value());
    REQUIRE(worker_ctx.phase.load(std::memory_order_acquire) == thread_phase::parked);

    co.end_collection();
    worker.join();
    REQUIRE_FALSE(co.collection_requested());
    co.unregister_thread(main_ctx);
    co.unregister_thread(worker_ctx);
}

// --- Item 8: rt_throw rooted lifecycle + detach cleanup ---------------------

TEST_CASE (


"begin_throw twice releases the prior exception root"
,
"[lithe][rt][exc][p0]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_plain_layout(1, 64));
    auto thread = (*rt)->attach_current_thread();
    REQUIRE(thread.has_value());

    auto first  = (*rt)->heap().allocate(1);
    auto second = (*rt)->heap().allocate(1);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    REQUIRE(begin_throw(**rt, thread->context(), exception_object{*first}).has_value());
    const root_token first_root = thread->context().exception.root;
    REQUIRE(first_root != null_root_token);

    // Replacing the exception must not leak the first root; a collection after
    // replacement must still succeed (no dangling root_handle).
    REQUIRE(begin_throw(**rt, thread->context(), exception_object{*second}).has_value());
    REQUIRE(thread->context().exception.root != null_root_token);
    REQUIRE((*rt)->heap().collect(collection_reason::explicit_request).has_value());

    consume_exception(**rt, thread->context());
    REQUIRE_FALSE(thread->context().exception.active());
}

TEST_CASE (


"detaching a thread mid-exception releases its root"
,
"[lithe][rt][exc][p0]"
)
 {
    auto rt = runtime_instance::create();
    REQUIRE(rt.has_value());
    (*rt)->layouts().register_layout(make_plain_layout(1, 64));

    {
        auto thread = (*rt)->attach_current_thread();
        REQUIRE(thread.has_value());
        auto obj = (*rt)->heap().allocate(1);
        REQUIRE(obj.has_value());
        REQUIRE(begin_throw(**rt, thread->context(), exception_object{*obj}).has_value());
        REQUIRE(thread->context().exception.active());
    } // detach with an active exception → root released, no dangling handle

    // A collection after detach must not touch a freed root slot.
    REQUIRE((*rt)->heap().collect(collection_reason::explicit_request).has_value());
    REQUIRE((*rt)->safepoints().thread_count() == 0);
}

// --- Item 9: representational / alignment checks ----------------------------

TEST_CASE (


"compute_object_size rejects an over-large object"
,
"[lithe][rt][gc][p0]"
)
 {
    // A payload that pushes total_size past UINT32_MAX must be rejected, not
    // silently truncated into the u32 header field.
    auto lay = make_plain_layout(1, static_cast<std::size_t>(0xFFFF'FFFFull) + 4096);
    const auto sz = compute_object_size(lay);
    REQUIRE_FALSE(sz.has_value());
    REQUIRE(sz.error().code == trap_code::out_of_memory);
}

TEST_CASE (


"over-aligned old allocation stays aligned"
,
"[lithe][rt][gc][p0]"
)
 {
    mop::layout_registry reg;
    auto lay = make_plain_layout(1, 64);
    lay.alignment = 64; // demands more than malloc's guarantee
    reg.register_layout(lay);
    generational_gc gc(&reg);

    auto pinned = gc.allocate_pinned(1); // lives in old space via aligned_raw
    REQUIRE(pinned.has_value());
    REQUIRE(reinterpret_cast<std::uintptr_t>(pinned->ptr) % 64 == 0);
}


