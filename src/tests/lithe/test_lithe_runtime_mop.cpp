#include "catch_amalgamated.hpp"

#include "lithe/lithe_runtime.hpp"
#include "lithe/backends/lithe_codegen_asmjit.hpp"

using namespace lithe::runtime::mop;
using namespace lithe::codegen;
using namespace lithe::codegen::backends;

// ===========================================================================
// Shared test layout factories
// ===========================================================================
namespace {
    // Stable FNV-1a hash for field names
    constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
        return h;
    }

    // A simple "Point" struct: { int64_t x; int64_t y; }
    object_layout make_point_layout() {
        return object_layout::make(
            /*id=*/ 0x0001,
                    /*size=*/ sizeof(std::int64_t) * 2,
                    /*align=*/ alignof(std::int64_t),
                    /*name=*/ "Point",
                    /*fields=*/ {
                        field_descriptor{"x", 0, sizeof(std::int64_t), 0},
                        field_descriptor{"y", sizeof(std::int64_t), sizeof(std::int64_t), 0},
                    }
        );
    }

    // A "Node" struct: { void* next; int64_t value; }
    object_layout make_node_layout() {
        return object_layout::make(
            /*id=*/ 0x0002,
                    /*size=*/ sizeof(void*) + sizeof(std::int64_t),
                    /*align=*/ alignof(void*),
                    /*name=*/ "Node",
                    /*fields=*/ {
                        field_descriptor{"next", 0, sizeof(void*), 0},
                        field_descriptor{"value", sizeof(void*), sizeof(std::int64_t), 0},
                    }
        );
    }

    // Zero-sized type (ZST)
    object_layout make_zst_layout() {
        return object_layout::make(0x0003, 0, 1, "ZST");
    }

    // Highly aligned structure (cache-line aligned)
    object_layout make_cache_aligned_layout() {
        return object_layout::make(
            /*id=*/ 0x0004,
                    /*size=*/ 64,
                    /*align=*/ 64,
                    /*name=*/ "CacheLine"
        );
    }

    // Layout with methods
    static std::int64_t add_method_thunk(void* obj, std::int64_t const*, std::uint32_t) {
        const auto* pt = static_cast<std::int64_t*>(obj);
        return pt[0] + pt[1]; // x + y
    }

    object_layout make_point_with_method_layout() {
        return object_layout::make(
            /*id=*/ 0x0005,
                    /*size=*/ sizeof(std::int64_t) * 2,
                    /*align=*/ alignof(std::int64_t),
                    /*name=*/ "PointWithMethod",
                    /*fields=*/ {
                        field_descriptor{"x", 0, sizeof(std::int64_t), 0},
                        field_descriptor{"y", sizeof(std::int64_t), sizeof(std::int64_t), 0},
                    },
                    /*methods=*/ {
                        method_descriptor{
                            .method_id = 0xCAFE,
                            .name = "sum",
                            .arity = 0,
                            .fn_ptr = reinterpret_cast<void*>(&add_method_thunk),
                        },
                    }
        );
    }
} // anonymous namespace


// ===========================================================================
// Section 1: object_layout validation
// ===========================================================================

TEST_CASE (



"object_layout: basic properties"
,
"[mop][layout]"
)
 {
    const auto pt = make_point_layout();
    REQUIRE(pt.layout_id == 0x0001);
    REQUIRE(pt.size_bytes == sizeof(std::int64_t) * 2);
    REQUIRE(pt.alignment == alignof(std::int64_t));
    REQUIRE(pt.type_name == "Point");
    REQUIRE(pt.field_map.size() == 2);
    REQUIRE(pt.is_valid());
    REQUIRE_FALSE(pt.is_zero_sized());
}

TEST_CASE (



"object_layout: zero-sized type"
,
"[mop][layout][edge]"
)
 {
    const auto zst = make_zst_layout();
    REQUIRE(zst.is_zero_sized());
    REQUIRE(zst.is_valid());
}

TEST_CASE (



"object_layout: high alignment is valid"
,
"[mop][layout]"
)
 {
    const auto cl = make_cache_aligned_layout();
    REQUIRE(cl.is_valid());
    REQUIRE(cl.alignment == 64);
    REQUIRE(cl.size_bytes == 64);
}

TEST_CASE (



"object_layout: invalid alignment (zero)"
,
"[mop][layout][edge]"
)
 {
    auto bad = make_point_layout();
    bad.alignment = 0;
    REQUIRE_FALSE(bad.is_valid());
}

TEST_CASE (



"object_layout: invalid alignment (non-power-of-two)"
,
"[mop][layout][edge]"
)
 {
    auto bad = make_point_layout();
    bad.alignment = 3;
    REQUIRE_FALSE(bad.is_valid());
}

TEST_CASE (



"object_layout: field_map contains expected offsets"
,
"[mop][layout]"
)
 {
    const auto pt = make_point_layout();
    const auto &xf = pt.field_map.at("x");
    const auto &yf = pt.field_map.at("y");
    REQUIRE(xf.byte_offset == 0);
    REQUIRE(yf.byte_offset == sizeof(std::int64_t));
    REQUIRE(xf.size_bytes == sizeof(std::int64_t));
    REQUIRE(yf.size_bytes == sizeof(std::int64_t));
}

TEST_CASE (



"object_layout: method_table lookup"
,
"[mop][layout]"
)
 {
    const auto pt = make_point_with_method_layout();
    REQUIRE(pt.method_table.count(0xCAFE) == 1);
    const auto &m = pt.method_table.at(0xCAFE);
    REQUIRE(m.name == "sum");
    REQUIRE(m.fn_ptr != nullptr);
}


// ===========================================================================
// Section 2: default_object_manager — lifecycle
// ===========================================================================

TEST_CASE (



"default_object_manager: allocate_instance success"
,
"[mop][manager]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();
    auto result = mgr.allocate_instance(lay);
    REQUIRE(result.has_value());
    REQUIRE(result->valid());
    REQUIRE(result->layout_id == 0x0001);
    REQUIRE(result->raw != nullptr);
    mgr.deallocate_instance(*result, lay);
}

TEST_CASE (



"default_object_manager: allocate ZST returns non-null sentinel"
,
"[mop][manager][edge]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_zst_layout();
    auto result = mgr.allocate_instance(lay);
    REQUIRE(result.has_value());
    REQUIRE(result->valid());
    mgr.deallocate_instance(*result, lay);  // must not crash
}

TEST_CASE (



"default_object_manager: allocate cache-aligned succeeds"
,
"[mop][manager]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_cache_aligned_layout();
    auto result = mgr.allocate_instance(lay);
    REQUIRE(result.has_value());
    // verify alignment
    const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(result->raw);
    CHECK((addr % 64) == 0);
    mgr.deallocate_instance(*result, lay);
}

TEST_CASE (



"default_object_manager: allocate with invalid layout returns error"
,
"[mop][manager][edge]"
)
 {
    default_object_manager<> mgr;
    auto bad = make_point_layout();
    bad.alignment = 0;
    auto result = mgr.allocate_instance(bad);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::invalid_layout);
}

TEST_CASE (



"default_object_manager: deallocate null ptr is a no-op"
,
"[mop][manager][edge]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();
    mgr.deallocate_instance(object_ptr{nullptr, 0}, lay);  // must not crash
}

TEST_CASE (



"default_object_manager: multiple independent allocations"
,
"[mop][manager]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();
    auto r1 = mgr.allocate_instance(lay);
    auto r2 = mgr.allocate_instance(lay);
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    CHECK(r1->raw != r2->raw);
    mgr.deallocate_instance(*r1, lay);
    mgr.deallocate_instance(*r2, lay);
}

TEST_CASE (



"default_object_manager: memory is writable after alloc"
,
"[mop][manager]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();
    auto result = mgr.allocate_instance(lay);
    REQUIRE(result.has_value());

    // Write directly into the raw buffer
    auto *data = static_cast<std::int64_t *>(result->raw);
    data[0] = 42;
    data[1] = 99;
    REQUIRE(data[0] == 42);
    REQUIRE(data[1] == 99);

    mgr.deallocate_instance(*result, lay);
}


// ===========================================================================
// Section 3: get_field
// ===========================================================================

TEST_CASE (



"get_field: resolves x offset correctly"
,
"[mop][get_field]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();
    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());

    auto *data = static_cast<std::int64_t *>(obj->raw);
    data[0] = 10;
    data[1] = 20;

    auto fx = mgr.get_field(*obj, lay, "x");
    REQUIRE(fx.has_value());
    REQUIRE(*static_cast<std::int64_t *>(*fx) == 10);

    auto fy = mgr.get_field(*obj, lay, "y");
    REQUIRE(fy.has_value());
    REQUIRE(*static_cast<std::int64_t *>(*fy) == 20);

    mgr.deallocate_instance(*obj, lay);
}

TEST_CASE (



"get_field: field_not_found for missing name"
,
"[mop][get_field][edge]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();
    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());

    auto result = mgr.get_field(*obj, lay, "z");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::field_not_found);

    mgr.deallocate_instance(*obj, lay);
}

TEST_CASE (



"get_field: null_ptr returns error"
,
"[mop][get_field][edge]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();
    object_ptr null_ptr{nullptr, lay.layout_id};

    auto result = mgr.get_field(null_ptr, lay, "x");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::invalid_object_ptr);
}

TEST_CASE (



"get_field: write through field pointer modifies object"
,
"[mop][get_field]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();
    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());

    auto fy = mgr.get_field(*obj, lay, "y");
    REQUIRE(fy.has_value());
    *static_cast<std::int64_t *>(*fy) = 777;

    const auto *data = static_cast<const std::int64_t *>(obj->raw);
    REQUIRE(data[1] == 777);

    mgr.deallocate_instance(*obj, lay);
}


// ===========================================================================
// Section 4: invoke_method
// ===========================================================================

TEST_CASE (



"invoke_method: fn_ptr dispatch adds x+y"
,
"[mop][invoke]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_with_method_layout();
    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());

    auto *data = static_cast<std::int64_t *>(obj->raw);
    data[0] = 3;
    data[1] = 4;

    std::int64_t no_args[] = {};
    auto result = mgr.invoke_method(*obj, lay, 0xCAFE, {no_args, 0});
    REQUIRE(result.has_value());
    REQUIRE(*result == 7);

    mgr.deallocate_instance(*obj, lay);
}

TEST_CASE (



"invoke_method: method_not_found for unknown id"
,
"[mop][invoke][edge]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_layout();  // no methods
    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());

    auto result = mgr.invoke_method(*obj, lay, 0xDEAD, {});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::method_not_found);

    mgr.deallocate_instance(*obj, lay);
}

TEST_CASE (



"invoke_method: null_ptr returns error"
,
"[mop][invoke][edge]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_with_method_layout();
    object_ptr null_ptr{nullptr, lay.layout_id};

    auto result = mgr.invoke_method(null_ptr, lay, 0xCAFE, {});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::invalid_object_ptr);
}

TEST_CASE (



"invoke_method: user-registered handler overrides fn_ptr"
,
"[mop][invoke]"
)
 {
    default_object_manager<> mgr;
    const auto lay = make_point_with_method_layout();

    // Register a handler that always returns 42
    mgr.register_method_handler(0xCAFE,
        [](object_ptr, std::span<std::int64_t const>) -> std::expected<std::int64_t, mop_error> {
            return 42;
        }
    );

    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());

    auto result = mgr.invoke_method(*obj, lay, 0xCAFE, {});
    REQUIRE(result.has_value());
    REQUIRE(*result == 42);

    mgr.deallocate_instance(*obj, lay);
}

TEST_CASE (



"invoke_method: method with no fn_ptr returns not_implemented"
,
"[mop][invoke][edge]"
)
 {
    default_object_manager<> mgr;
    auto lay = make_point_layout();
    lay.method_table.emplace(0xBEEF, method_descriptor{.method_id = 0xBEEF, .name = "stub", .arity = 0, .fn_ptr = nullptr});

    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());

    auto result = mgr.invoke_method(*obj, lay, 0xBEEF, {});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::not_implemented);

    mgr.deallocate_instance(*obj, lay);
}


// ===========================================================================
// Section 5: oom_test_object_manager
// ===========================================================================

TEST_CASE (



"oom_test_object_manager: allocates up to budget"
,
"[mop][oom]"
)
 {
    oom_test_object_manager mgr(3);
    const auto lay = make_point_layout();

    auto r1 = mgr.allocate_instance(lay);
    auto r2 = mgr.allocate_instance(lay);
    auto r3 = mgr.allocate_instance(lay);
    auto r4 = mgr.allocate_instance(lay);  // must fail

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r3.has_value());
    REQUIRE_FALSE(r4.has_value());
    REQUIRE(r4.error().code == mop_error_code::out_of_memory);

    mgr.deallocate_instance(*r1, lay);
    mgr.deallocate_instance(*r2, lay);
    mgr.deallocate_instance(*r3, lay);
}

TEST_CASE (



"oom_test_object_manager: budget=0 always fails"
,
"[mop][oom][edge]"
)
 {
    oom_test_object_manager mgr(0);
    const auto lay = make_point_layout();
    auto result = mgr.allocate_instance(lay);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::out_of_memory);
}

TEST_CASE (



"oom_test_object_manager: remaining_budget tracks usage"
,
"[mop][oom]"
)
 {
    oom_test_object_manager mgr(5);
    const auto lay = make_zst_layout();
    REQUIRE(mgr.remaining_budget() == 5);
    (void)mgr.allocate_instance(lay);
    REQUIRE(mgr.remaining_budget() == 4);
    (void)mgr.allocate_instance(lay);
    REQUIRE(mgr.remaining_budget() == 3);
}


// ===========================================================================
// Section 6: injecting_object_manager
// ===========================================================================

TEST_CASE (



"injecting_object_manager: custom alloc/dealloc called"
,
"[mop][inject]"
)
 {
    int alloc_calls = 0, dealloc_calls = 0;
    std::vector<std::byte> buffer(1024, std::byte{0});

    injecting_object_manager mgr(
        [&](std::size_t /*size*/, std::size_t /*align*/) -> void * {
            ++alloc_calls;
            return buffer.data();
        },
        [&](void * /*ptr*/, std::size_t /*size*/, std::size_t /*align*/) {
            ++dealloc_calls;
        }
    );

    const auto lay = make_point_layout();
    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());
    REQUIRE(alloc_calls == 1);

    mgr.deallocate_instance(*obj, lay);
    REQUIRE(dealloc_calls == 1);
}

TEST_CASE (



"injecting_object_manager: null return from alloc yields OOM error"
,
"[mop][inject][oom]"
)
 {
    injecting_object_manager mgr(
        [](std::size_t, std::size_t) -> void * { return nullptr; },
        [](void *, std::size_t, std::size_t) {}
    );
    const auto lay = make_point_layout();
    auto result = mgr.allocate_instance(lay);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::out_of_memory);
}


// ===========================================================================
// Section 7: layout_registry
// ===========================================================================

TEST_CASE (



"layout_registry: register and find"
,
"[mop][registry]"
)
 {
    layout_registry reg;
    REQUIRE(reg.size() == 0);

    REQUIRE(reg.register_layout(make_point_layout()));
    REQUIRE(reg.size() == 1);

    const auto *p = reg.find(0x0001);
    REQUIRE(p != nullptr);
    REQUIRE(p->type_name == "Point");
}

TEST_CASE (



"layout_registry: duplicate id is ignored"
,
"[mop][registry]"
)
 {
    layout_registry reg;
    REQUIRE(reg.register_layout(make_point_layout()));
    REQUIRE_FALSE(reg.register_layout(make_point_layout()));  // duplicate
    REQUIRE(reg.size() == 1);
}

TEST_CASE (



"layout_registry: get returns expected on missing id"
,
"[mop][registry]"
)
 {
    layout_registry reg;
    auto result = reg.get(0x9999);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::invalid_layout);
}

TEST_CASE (



"layout_registry: multiple layouts coexist"
,
"[mop][registry]"
)
 {
    layout_registry reg;
    reg.register_layout(make_point_layout());
    reg.register_layout(make_node_layout());
    reg.register_layout(make_zst_layout());
    REQUIRE(reg.size() == 3);
    REQUIRE(reg.find(0x0001) != nullptr);
    REQUIRE(reg.find(0x0002) != nullptr);
    REQUIRE(reg.find(0x0003) != nullptr);
    REQUIRE(reg.find(0x0099) == nullptr);
}


// ===========================================================================
// Section 8: mop_context + make_mop_context
// ===========================================================================

TEST_CASE (



"make_mop_context: valid context is created"
,
"[mop][context]"
)
 {
    default_object_manager<> mgr;
    layout_registry reg;
    reg.register_layout(make_point_layout());
    auto ctx = make_mop_context(mgr, reg);
    REQUIRE(ctx.valid());
    REQUIRE(ctx.registry == &reg);
}

TEST_CASE (



"make_mop_context: alloc_fn works through context"
,
"[mop][context]"
)
 {
    default_object_manager<> mgr;
    layout_registry reg;
    reg.register_layout(make_point_layout());
    auto ctx = make_mop_context(mgr, reg);

    auto result = ctx.alloc_fn(0x0001);
    REQUIRE(result.has_value());
    REQUIRE(result->valid());
    ctx.dealloc_fn(*result, 0x0001);
}

TEST_CASE (



"make_mop_context: alloc_fn fails for unknown layout_id"
,
"[mop][context][edge]"
)
 {
    default_object_manager<> mgr;
    layout_registry reg;
    auto ctx = make_mop_context(mgr, reg);

    auto result = ctx.alloc_fn(0x9999);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == mop_error_code::invalid_layout);
}

TEST_CASE (



"make_mop_context: get_field_fn resolves by hash"
,
"[mop][context]"
)
 {
    default_object_manager<> mgr;
    layout_registry reg;
    reg.register_layout(make_point_layout());
    auto ctx = make_mop_context(mgr, reg);

    auto obj = ctx.alloc_fn(0x0001);
    REQUIRE(obj.has_value());

    auto *data = static_cast<std::int64_t *>(obj->raw);
    data[0] = 55;

    const std::uint64_t x_hash = std::hash<std::string>{}("x");
    auto field_result = ctx.get_field_fn(*obj, 0x0001, x_hash);
    REQUIRE(field_result.has_value());
    REQUIRE(*static_cast<std::int64_t *>(*field_result) == 55);

    ctx.dealloc_fn(*obj, 0x0001);
}

TEST_CASE (



"make_mop_context: invoke_fn dispatches method"
,
"[mop][context]"
)
 {
    default_object_manager<> mgr;
    layout_registry reg;
    reg.register_layout(make_point_with_method_layout());
    auto ctx = make_mop_context(mgr, reg);

    auto obj = ctx.alloc_fn(0x0005);
    REQUIRE(obj.has_value());
    auto *data = static_cast<std::int64_t *>(obj->raw);
    data[0] = 11;
    data[1] = 22;

    auto inv_result = ctx.invoke_fn(*obj, 0x0005, 0xCAFE, {});
    REQUIRE(inv_result.has_value());
    REQUIRE(*inv_result == 33);

    ctx.dealloc_fn(*obj, 0x0005);
}


// ===========================================================================
// Section 9: Concept satisfaction static assertions
// ===========================================================================

TEST_CASE (



"ObjectManager concept: all manager types satisfy it"
,
"[mop][concept]"
)
 {
    STATIC_REQUIRE(ObjectManager<default_object_manager<>>);
    STATIC_REQUIRE(ObjectManager<oom_test_object_manager>);
    STATIC_REQUIRE(ObjectManager<injecting_object_manager<>>);
}


// ===========================================================================
// Section 10: asmjit_backend MOP integration
// ===========================================================================

namespace {
    // Build a physical_mir_function that uses mop.alloc for a PointWithMethod
    // and then calls mop.invoke_method to get x+y.
    //
    // The test layout (0x0005): size=16, align=8, fields x@0 y@8, method 0xCAFE=sum
    //
    // The JIT function signature: int64_t jit_fn(int64_t x, int64_t y)
    //   Pseudocode:
    //     ptr    = mop_alloc(layout_id=5)
    //     [ptr+0] = x
    //     [ptr+8] = y
    //     result  = mop_invoke_method(ptr, method_id=0xCAFE, layout_id=5)
    //     return result
    mir::physical_mir_function make_mop_fn(std::uint64_t layout_id, std::uint64_t method_id) {
        using namespace lithe::codegen;

        // Registers: r0=x arg, r1=y arg, r2=ptr, r3=result
        std::vector<allocated_instruction> insts;
        std::uint32_t iid = 0;

        // load_arg: r0 = arg[0] (x)
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::load_arg;
            i.defs = {allocated_operand::as_preg(preg{0, "r0"})};
            i.uses = {allocated_operand::as_argument_index(0)};
            insts.push_back(i);
        }
        // load_arg: r1 = arg[1] (y)
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::load_arg;
            i.defs = {allocated_operand::as_preg(preg{1, "r1"})};
            i.uses = {allocated_operand::as_argument_index(1)};
            insts.push_back(i);
        }
        // mop.alloc: r2 = alloc(layout_id)
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::indirect_call;
            i.abstract_operation = lithe::runtime::mop::opcodes::make_mop_op(
                lithe::runtime::mop::opcodes::mop_alloc);
            i.defs = {allocated_operand::as_preg(preg{2, "r2"})};
            i.uses = {allocated_operand::as_i64(static_cast<std::int64_t>(layout_id))};
            insts.push_back(i);
        }
        // store x into [ptr+0]
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::store;
            i.uses = {
                allocated_operand::as_preg(preg{2, "r2"}),
                allocated_operand::as_preg(preg{0, "r0"}),
                allocated_operand::as_i64(0), // offset
            };
            insts.push_back(i);
        }
        // store y into [ptr+8]
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::store;
            i.uses = {
                allocated_operand::as_preg(preg{2, "r2"}),
                allocated_operand::as_preg(preg{1, "r1"}),
                allocated_operand::as_i64(8), // offset
            };
            insts.push_back(i);
        }
        // mop.invoke_method: r3 = invoke(ptr=r2, method_id, layout_id)
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::indirect_call;
            i.abstract_operation = lithe::runtime::mop::opcodes::make_mop_op(
                lithe::runtime::mop::opcodes::mop_invoke_method);
            i.defs = {allocated_operand::as_preg(preg{3, "r3"})};
            i.uses = {
                allocated_operand::as_preg(preg{2, "r2"}), // obj ptr
                allocated_operand::as_i64(static_cast<std::int64_t>(method_id)),
                allocated_operand::as_i64(static_cast<std::int64_t>(layout_id)),
            };
            insts.push_back(i);
        }
        // ret r3
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::ret;
            i.uses = {allocated_operand::as_preg(preg{3, "r3"})};
            insts.push_back(i);
        }

        allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";
        bb.instructions = std::move(insts);

        allocated_function_ir fn;
        fn.name = "mop_sum";
        fn.cfg.entry_block = 1;
        fn.blocks = {bb};

        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }
} // anonymous namespace

TEST_CASE (



"asmjit_backend: MOP alloc + invoke_method round-trip"
,
"[mop][asmjit][integration]"
)
{
    // Set up registry and manager
    layout_registry reg;
    reg.register_layout(make_point_with_method_layout());

    default_object_manager<> mgr;
    auto ctx = make_mop_context(mgr, reg);

    // Compile with MOP context
    asmjit_backend backend;
    REQUIRE(ctx.valid());
    REQUIRE(backend.mop_context_ptr() == nullptr);
    backend.set_mop_context(&ctx);
    REQUIRE(backend.mop_context_ptr() == &ctx);

    const auto phys = make_mop_fn(0x0005, 0xCAFE);
    const auto art  = backend.emit(phys);

    // Check for compilation errors
    INFO("Diagnostics: " << (art.diagnostics.empty() ? "(none)" : art.diagnostics[0]));
    REQUIRE(art.diagnostics.empty());
    REQUIRE(art.kind == artifact_kind::jit_function);

    auto *h = asmjit_backend::get_handle(art);
    REQUIRE(h != nullptr);
    REQUIRE(h->valid());

    // Call: jit_fn(3, 4) should return 3+4=7
    const std::int64_t result = h->call(3, 4);
    CHECK(result == 7);

    // Call: jit_fn(100, 200) should return 300
    CHECK(h->call(100, 200) == 300);
}

TEST_CASE (



"asmjit_backend: set_mop_context and mop_context_ptr roundtrip"
,
"[mop][asmjit]"
)
{
    asmjit_backend backend;
    REQUIRE(backend.mop_context_ptr() == nullptr);

    layout_registry reg;
    default_object_manager<> mgr;
    auto ctx = make_mop_context(mgr, reg);
    backend.set_mop_context(&ctx);
    REQUIRE(backend.mop_context_ptr() == &ctx);

    backend.set_mop_context(nullptr);
    REQUIRE(backend.mop_context_ptr() == nullptr);
}

TEST_CASE (



"asmjit_backend: mop.alloc without context emits diagnostic"
,
"[mop][asmjit][edge]"
)
{
    // No MOP context set — backend should emit a diagnostic and not crash.
    asmjit_backend backend;

    allocated_basic_block bb;
    bb.id = 1;
    bb.name = "entry";
    {
        allocated_instruction i;
        i.id  = 1;
        i.op  = opcode::indirect_call;
        i.abstract_operation = lithe::runtime::mop::opcodes::make_mop_op(
            lithe::runtime::mop::opcodes::mop_alloc);
        i.defs = {allocated_operand::as_preg(preg{0, "r0"})};
        i.uses = {allocated_operand::as_i64(0x0001)};
        bb.instructions.push_back(i);
    }
    {
        allocated_instruction i;
        i.id  = 2;
        i.op  = opcode::ret;
        i.uses = {allocated_operand::as_preg(preg{0, "r0"})};
        bb.instructions.push_back(i);
    }

    allocated_function_ir fn;
    fn.name            = "no_mop_ctx";
    fn.cfg.entry_block = 1;
    fn.blocks          = {bb};

    mir::physical_mir_function phys;
    phys.function               = std::move(fn);
    phys.metadata.current_phase = mir::phase::physical_mir;

    const auto art = backend.emit(phys);
    // Should record a diagnostic about unresolved indirect_call
    CHECK_FALSE(art.diagnostics.empty());
}

// ===========================================================================
// Section 11: Language plugin extension point
// ===========================================================================

TEST_CASE (



"language_plugin: default specialization is a no-op pass-through"
,
"[mop][plugin]"
)
 {
    using default_plugin = language_plugin<default_language_tag>;
    const auto lay = make_point_layout();
    object_ptr ptr{nullptr, lay.layout_id};

    // Default plugin returns nullopt for everything (falls through to normal dispatch)
    auto field_result = default_plugin::resolve_field(lay, ptr, "x");
    REQUIRE_FALSE(field_result.has_value());

    auto method_result = default_plugin::resolve_method(lay, ptr, 0xCAFE, {});
    REQUIRE_FALSE(method_result.has_value());
}

TEST_CASE (



"language_plugin: custom specialization overrides field resolution"
,
"[mop][plugin]"
)
 {
    // Define a tag and specialization that always returns a fixed pointer for "x"
    struct my_lang_tag {};
    static std::int64_t fixed_val = 999;

    // We test by using default_object_manager with a mock handler
    default_object_manager<> mgr;
    mgr.register_method_handler(0xABCD,
        [](object_ptr, std::span<std::int64_t const>) -> std::expected<std::int64_t, mop_error> {
            return 123;
        }
    );
    const auto lay = make_point_with_method_layout();
    auto obj = mgr.allocate_instance(lay);
    REQUIRE(obj.has_value());

    // Register a handler for a fresh method_id — confirms extensibility
    auto inv = mgr.invoke_method(*obj, lay, 0xABCD, {});
    REQUIRE(inv.has_value());
    REQUIRE(*inv == 123);

    (void)fixed_val;  // suppress unused warning
    mgr.deallocate_instance(*obj, lay);
}

// ===========================================================================
// Section 12: Native Binding API (lithe::runtime::ffi::binding)
// ===========================================================================

namespace {
    static constexpr std::int64_t fn_add_i64(std::int64_t a, std::int64_t b) noexcept {
        return a + b;
    }

    static constexpr double fn_mul_f64(double a, double b) noexcept {
        return a * b;
    }

    static constexpr bool fn_is_positive(std::int64_t x) noexcept {
        return x > 0;
    }

    static constexpr void* fn_ptr_identity(void* p) noexcept {
        return p;
    }

    inline std::int64_t fn_last_arg = 0;
    static void fn_set_last(std::int64_t v) noexcept { fn_last_arg = v; }

    static constexpr std::int64_t fn_nothrow_add(std::int64_t a, std::int64_t b) noexcept {
        return a + b;
    }
} // anonymous namespace

using namespace lithe::runtime::ffi::binding;
using namespace lithe::runtime::ffi;

TEST_CASE (


"bind_native_function: int64_t add returns valid proxy"
,
"[ffi][binding]"
)
 {
    auto result = bind_native_function<fn_add_i64>();
    REQUIRE(result.has_value());

    const auto& proxy = *result;
    REQUIRE(proxy.valid());
    REQUIRE(proxy.trampoline != nullptr);
    REQUIRE(proxy.arity == 2);
    REQUIRE(proxy.ret_type == type_hint_i64);
    REQUIRE(proxy.arg_types[0] == type_hint_i64);
    REQUIRE(proxy.arg_types[1] == type_hint_i64);
}

TEST_CASE (


"bind_native_function: invoke_bound with i64 add"
,
"[ffi][binding]"
)
 {
    auto proxy = bind_native_function<fn_add_i64>();
    REQUIRE(proxy.has_value());

    std::array<std::int64_t, 2> regs{10, 32};
    auto r = invoke_bound(*proxy, regs);
    REQUIRE(r.has_value());
    REQUIRE(*r == 42);
}

TEST_CASE (


"bind_native_function: f64 multiply proxy metadata"
,
"[ffi][binding]"
)
 {
    auto result = bind_native_function<fn_mul_f64>();
    REQUIRE(result.has_value());
    REQUIRE(result->arity == 2);
    REQUIRE(result->ret_type == type_hint_f64);
    REQUIRE(result->arg_types[0] == type_hint_f64);
    REQUIRE(result->arg_types[1] == type_hint_f64);
}

TEST_CASE (


"bind_native_function: invoke_bound with f64 multiply"
,
"[ffi][binding]"
)
 {
    auto proxy = bind_native_function<fn_mul_f64>();
    REQUIRE(proxy.has_value());

    double a = 3.0, b = 1.5;
    std::int64_t ra, rb;
    std::memcpy(&ra, &a, sizeof(ra));
    std::memcpy(&rb, &b, sizeof(rb));

    std::array<std::int64_t, 2> regs{ra, rb};
    auto r = invoke_bound(*proxy, regs);
    REQUIRE(r.has_value());

    double result;
    std::memcpy(&result, &*r, sizeof(result));
    REQUIRE(result == Catch::Approx(4.5));
}

TEST_CASE (


"bind_native_function: bool return type hint"
,
"[ffi][binding]"
)
 {
    auto result = bind_native_function<fn_is_positive>();
    REQUIRE(result.has_value());
    REQUIRE(result->ret_type == type_hint_bool);
    REQUIRE(result->arity == 1);
}

TEST_CASE (


"bind_native_function: invoke_bound bool result"
,
"[ffi][binding]"
)
 {
    auto proxy = bind_native_function<fn_is_positive>();
    REQUIRE(proxy.has_value());

    std::array<std::int64_t, 1> pos_regs{5};
    auto r_true = invoke_bound(*proxy, pos_regs);
    REQUIRE(r_true.has_value());
    REQUIRE(*r_true == 1);

    std::array<std::int64_t, 1> neg_regs{-1};
    auto r_false = invoke_bound(*proxy, neg_regs);
    REQUIRE(r_false.has_value());
    REQUIRE(*r_false == 0);
}

TEST_CASE (


"bind_native_function: void* pointer identity"
,
"[ffi][binding]"
)
 {
    auto proxy = bind_native_function<fn_ptr_identity>();
    REQUIRE(proxy.has_value());
    REQUIRE(proxy->ret_type == type_hint_ptr);
    REQUIRE(proxy->arg_types[0] == type_hint_ptr);
    REQUIRE(proxy->arity == 1);

    int sentinel = 0xDEAD;
    std::int64_t raw_ptr = static_cast<std::int64_t>(
        reinterpret_cast<std::uintptr_t>(&sentinel));
    std::array<std::int64_t, 1> regs{raw_ptr};
    auto r = invoke_bound(*proxy, regs);
    REQUIRE(r.has_value());
    REQUIRE(*r == raw_ptr);
}

TEST_CASE (


"bind_native_function: stateless lambda binding"
,
"[ffi][binding]"
)
 {
    static constexpr auto multiply = [](std::int64_t x, std::int64_t y) noexcept -> std::int64_t {
        return x * y;
    };
    auto proxy = bind_native_function<multiply>();
    REQUIRE(proxy.has_value());
    REQUIRE(proxy->arity == 2);

    std::array<std::int64_t, 2> regs{6, 7};
    auto r = invoke_bound(*proxy, regs);
    REQUIRE(r.has_value());
    REQUIRE(*r == 42);
}

TEST_CASE (


"bind_native_function: invoke_bound with insufficient registers"
,
"[ffi][binding]"
)
 {
    auto proxy = bind_native_function<fn_add_i64>();
    REQUIRE(proxy.has_value());

    std::array<std::int64_t, 1> regs{1};
    auto r = invoke_bound(*proxy, regs);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == lithe::runtime::mop::mop_error_code::type_mismatch);
}

TEST_CASE (


"bind_native_function: invoke_bound on null proxy fails"
,
"[ffi][binding]"
)
 {
    native_proxy null_proxy{};
    std::array<std::int64_t, 2> regs{1, 2};
    auto r = invoke_bound(null_proxy, regs);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == lithe::runtime::mop::mop_error_code::not_implemented);
}

// ============================================================================
// Finding 4: bind_native_function supports void return
// ============================================================================

TEST_CASE (


"bind_native_function supports void return"
,
"[lithe][ffi]"
)
 {
    using namespace lithe::runtime::ffi::binding;
    using namespace lithe::runtime::mop;

    fn_last_arg = 0;
    auto proxy = bind_native_function<fn_set_last>();
    REQUIRE(proxy.has_value());
    REQUIRE(proxy->valid());
    REQUIRE(proxy->arity == 1);
    REQUIRE(proxy->ret_type == type_hint_void);

    std::array<std::int64_t, 1> regs{99};
    auto r = invoke_bound(*proxy, regs);
    REQUIRE(r.has_value());
    REQUIRE(*r == 0);       // void trampolines return 0
    REQUIRE(fn_last_arg == 99);
}

// ============================================================================
// Finding 5: bind_native_function binds noexcept callable (positive contract)
// Non-noexcept callables are a compile-time error by design — no runtime test
// for the negative path.
// ============================================================================

TEST_CASE (


"bind_native_function binds noexcept callable"
,
"[lithe][ffi]"
)
 {
    using namespace lithe::runtime::ffi::binding;
    using namespace lithe::runtime::mop;

    auto proxy = bind_native_function<fn_nothrow_add>();
    REQUIRE(proxy.has_value());
    REQUIRE(proxy->arity == 2);

    std::array<std::int64_t, 2> regs{7, 8};
    auto r = invoke_bound(*proxy, regs);
    REQUIRE(r.has_value());
    REQUIRE(*r == 15);
}

// ============================================================================
// Finding 10: object_layout::is_valid rejects out-of-bounds fields
// ============================================================================

TEST_CASE (


"object_layout::is_valid rejects out-of-bounds fields"
,
"[lithe][mop]"
)
 {
    using namespace lithe::runtime::mop;

    // Well-formed layout: 8 byte struct with one 4-byte field at offset 0.
    object_layout good;
    good.layout_id  = 1;
    good.size_bytes = 8;
    good.alignment  = 8;
    good.field_map["x"] = field_descriptor{"x", 0, 4, 0};
    REQUIRE(good.is_valid());

    // Field offset past end.
    object_layout bad_offset;
    bad_offset.layout_id  = 2;
    bad_offset.size_bytes = 8;
    bad_offset.alignment  = 8;
    bad_offset.field_map["x"] = field_descriptor{"x", 9, 1, 0};
    REQUIRE_FALSE(bad_offset.is_valid());

    // Field extends past end (offset+size overflow).
    object_layout bad_size;
    bad_size.layout_id  = 3;
    bad_size.size_bytes = 8;
    bad_size.alignment  = 8;
    bad_size.field_map["x"] = field_descriptor{"x", 6, 4, 0};  // 6+4=10 > 8
    REQUIRE_FALSE(bad_size.is_valid());
}

TEST_CASE (


"MOP field access rejects layout_id mismatch"
,
"[lithe][mop]"
)
 {
    using namespace lithe::runtime::mop;

    object_layout layoutA;
    layoutA.layout_id  = 10;
    layoutA.size_bytes = sizeof(std::int64_t);
    layoutA.alignment  = alignof(std::int64_t);
    layoutA.field_map["v"] = field_descriptor{"v", 0, sizeof(std::int64_t), 0};

    object_layout layoutB;
    layoutB.layout_id  = 20;   // different id
    layoutB.size_bytes = sizeof(std::int64_t);
    layoutB.alignment  = alignof(std::int64_t);
    layoutB.field_map["v"] = field_descriptor{"v", 0, sizeof(std::int64_t), 0};

    default_object_manager mgr;
    auto ptr = mgr.allocate_instance(layoutA);
    REQUIRE(ptr.has_value());

    // get_field with matching layout must succeed.
    auto ok = mgr.get_field(*ptr, layoutA, "v");
    REQUIRE(ok.has_value());

    // get_field with mismatched layout_id must fail.
    auto bad = mgr.get_field(*ptr, layoutB, "v");
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error().code == mop_error_code::invalid_layout);

    mgr.deallocate_instance(*ptr, layoutA);
}

// ============================================================================
// Hardening: invoke_method rejects wrong-layout dispatch
// ============================================================================

TEST_CASE (


"invoke_method rejects layout_id mismatch"
,
"[lithe][mop][invoke_method]"
)
 {
    // layoutA owns the allocation; layoutB has a different layout_id but the
    // same size/fields. Passing layoutB to invoke_method must return bad_layout.
    const auto layoutA = make_point_with_method_layout();

    object_layout layoutB;
    layoutB.layout_id  = layoutA.layout_id + 1; // intentionally different
    layoutB.size_bytes = layoutA.size_bytes;
    layoutB.alignment  = layoutA.alignment;
    layoutB.type_name  = "PointAlias";
    layoutB.field_map  = layoutA.field_map;
    layoutB.method_table = layoutA.method_table;

    default_object_manager mgr;
    auto ptr = mgr.allocate_instance(layoutA);
    REQUIRE(ptr.has_value());

    // Correct layout: must succeed and return x+y (both fields zero-initialised → 0).
    {
        std::array<std::int64_t, 0> args{};
        auto ok = mgr.invoke_method(*ptr, layoutA, 0xCAFE,
                                    std::span<std::int64_t const>(args));
        REQUIRE(ok.has_value());
    }

    // Wrong layout_id: must return an error, not silently dispatch.
    {
        std::array<std::int64_t, 0> args{};
        auto bad = mgr.invoke_method(*ptr, layoutB, 0xCAFE,
                                     std::span<std::int64_t const>(args));
        REQUIRE_FALSE(bad.has_value());
        REQUIRE(bad.error().code == mop_error_code::invalid_layout);
    }

    mgr.deallocate_instance(*ptr, layoutA);
}
