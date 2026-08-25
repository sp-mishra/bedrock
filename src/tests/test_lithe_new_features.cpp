#include "catch_amalgamated.hpp"

#include "lithe/lithe_runtime.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/backends/lithe_codegen_asmjit.hpp"

#include <cstring>
#include <string_view>

using namespace lithe::runtime;
using namespace lithe::runtime::mop;
using namespace lithe::runtime::safepoint;
using namespace lithe::runtime::fuel;
using namespace lithe::runtime::ffi;
using namespace lithe::codegen;
using namespace lithe::codegen::backends;

// ===========================================================================
// Shared MIR builder helpers
// ===========================================================================
namespace {
    allocated_operand preg_op(std::uint16_t id) {
        preg r;
        r.id = id;
        return allocated_operand::as_preg(r);
    }

    allocated_operand arg_op(std::uint32_t idx) {
        return allocated_operand::as_argument_index(idx);
    }

    allocated_operand imm_op(std::int64_t v) {
        return allocated_operand::as_i64(v);
    }

    allocated_operand block_op(std::uint32_t bid) {
        return allocated_operand::as_block(bid);
    }

    allocated_instruction make_inst(std::uint32_t id, opcode op,
                                    std::vector<allocated_operand> defs = {},
                                    std::vector<allocated_operand> uses = {}) {
        allocated_instruction i;
        i.id = id;
        i.op = op;
        i.defs = std::move(defs);
        i.uses = std::move(uses);
        return i;
    }

    allocated_basic_block make_block(std::uint32_t id, std::string name,
                                     std::vector<std::uint32_t> succs,
                                     std::vector<allocated_instruction> insts) {
        allocated_basic_block bb;
        bb.id = id;
        bb.name = std::move(name);
        bb.successors = std::move(succs);
        bb.instructions = std::move(insts);
        return bb;
    }

    mir::physical_mir_function wrap(std::string name,
                                    std::vector<allocated_basic_block> blocks,
                                    std::uint32_t entry = 1) {
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = entry;
        fn.blocks = std::move(blocks);
        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }

    std::shared_ptr<jit_function_handle> jit_compile(mir::physical_mir_function fn,
                                                     asmjit_backend& backend) {
        auto art = backend.emit(fn);
        REQUIRE(art.diagnostics.empty());
        REQUIRE(art.kind == artifact_kind::jit_function);
        auto* h_ptr = asmjit_backend::get_handle(art);
        REQUIRE(h_ptr != nullptr);
        REQUIRE(h_ptr->valid());
        return {art.handle, h_ptr};
    }

    // Simple add(a,b) function IR for integration tests.
    mir::physical_mir_function fn_add_simple() {
        return wrap("add_simple", {
                        make_block(1, "entry", {}, {
                                       make_inst(1, opcode::load_arg, {preg_op(0)}, {arg_op(0)}),
                                       make_inst(2, opcode::load_arg, {preg_op(1)}, {arg_op(1)}),
                                       make_inst(3, opcode::add, {preg_op(2)}, {preg_op(0), preg_op(1)}),
                                       make_inst(4, opcode::ret, {}, {preg_op(2)}),
                                   })
                    });
    }
} // anonymous namespace

// ===========================================================================
// Section A: runtime_value and dynamic_value boxing model
// ===========================================================================

TEST_CASE (



"runtime_value: variant construction and holds_alternative"
,
"[boxing]"
)
 {
    runtime_value v_i64 = std::int64_t{42};
    CHECK(std::holds_alternative<std::int64_t>(v_i64));
    CHECK(std::get<std::int64_t>(v_i64) == 42);

    runtime_value v_f64 = double{3.14};
    CHECK(std::holds_alternative<double>(v_f64));
    CHECK(std::get<double>(v_f64) == 3.14);

    runtime_value v_bool = true;
    CHECK(std::holds_alternative<bool>(v_bool));
    CHECK(std::get<bool>(v_bool) == true);

    int dummy = 0;
    runtime_value v_ptr = static_cast<void*>(&dummy);
    CHECK(std::holds_alternative<void*>(v_ptr));
    CHECK(std::get<void*>(v_ptr) == &dummy);

    object_ptr op{&dummy, 0};
    runtime_value v_obj = op;
    CHECK(std::holds_alternative<object_ptr>(v_obj));
    CHECK(std::get<object_ptr>(v_obj).raw == &dummy);
}

TEST_CASE (



"runtime_value: trivially copyable — memcpy round-trip"
,
"[boxing]"
)
 {
    static_assert(std::is_trivially_copyable_v<runtime_value>);

    runtime_value src = std::int64_t{0xDEAD'BEEF};
    runtime_value dst;
    std::memcpy(&dst, &src, sizeof(runtime_value));
    CHECK(std::get<std::int64_t>(dst) == 0xDEAD'BEEF);

    double pi = 3.141592653589793;
    runtime_value src_f = pi;
    runtime_value dst_f;
    std::memcpy(&dst_f, &src_f, sizeof(runtime_value));
    CHECK(std::get<double>(dst_f) == pi);
}

TEST_CASE (



"dynamic_value: default construction"
,
"[boxing]"
)
 {
    static_assert(std::is_trivially_copyable_v<dynamic_value>);
    dynamic_value dv;
    CHECK(dv.type_id == 0);
    CHECK(std::holds_alternative<std::int64_t>(dv.value));
    CHECK(std::get<std::int64_t>(dv.value) == 0);
}

TEST_CASE (



"dynamic_value: parameterized construction preserves type_id and value"
,
"[boxing]"
)
 {
    dynamic_value dv{7u, runtime_value{std::int64_t{99}}};
    CHECK(dv.type_id == 7u);
    CHECK(std::get<std::int64_t>(dv.value) == 99);
}

TEST_CASE (



"dynamic_value: type_id is independent of value alternative"
,
"[boxing]"
)
 {
    dynamic_value dv{99u, runtime_value{double{3.14}}};
    CHECK(dv.type_id == 99u);
    CHECK(std::holds_alternative<double>(dv.value));
    CHECK(std::get<double>(dv.value) == 3.14);
}

TEST_CASE (



"dynamic_value: bool value stored correctly"
,
"[boxing]"
)
 {
    dynamic_value t{1u, runtime_value{true}};
    dynamic_value f{2u, runtime_value{false}};
    CHECK(std::get<bool>(t.value) == true);
    CHECK(std::get<bool>(f.value) == false);
}

// ===========================================================================
// Section B: GarbageCollector concept and trigger_safepoint
// ===========================================================================

namespace {
    struct MockGC {
        int call_count = 0;
        std::string last_fn_name;
        std::size_t last_entry_count = 0;

        void root_scan(stack_map const& sm) noexcept {
            ++call_count;
            last_fn_name = sm.fn_name;
            last_entry_count = sm.entries.size();
        }
    };

    struct BadGC {
        // no root_scan — should not satisfy GarbageCollector
    };
} // anonymous namespace

TEST_CASE (



"GarbageCollector concept: MockGC satisfies it"
,
"[gc]"
)
 {
    static_assert(GarbageCollector<MockGC>);
}

TEST_CASE (



"GarbageCollector concept: BadGC does not satisfy it"
,
"[gc]"
)
 {
    static_assert(!GarbageCollector<BadGC>);
}

TEST_CASE (



"trigger_safepoint: calls root_scan when fn_name is registered"
,
"[gc]"
)
 {
    stack_map_table tbl;
    stack_map sm;
    sm.fn_name = "foo";
    sm.insert({10, {1, 2}});
    tbl.register_map(std::move(sm));

    MockGC gc;
    trigger_safepoint("foo", tbl, gc);

    CHECK(gc.call_count == 1);
    CHECK(gc.last_fn_name == "foo");
    CHECK(gc.last_entry_count == 1);
}

TEST_CASE (



"trigger_safepoint: no-op when fn_name is absent"
,
"[gc]"
)
 {
    stack_map_table tbl;
    MockGC gc;
    trigger_safepoint("nonexistent", tbl, gc);
    CHECK(gc.call_count == 0);
}

TEST_CASE (



"trigger_safepoint: called multiple times accumulates"
,
"[gc]"
)
 {
    stack_map_table tbl;
    stack_map sm;
    sm.fn_name = "bar";
    tbl.register_map(std::move(sm));

    MockGC gc;
    trigger_safepoint("bar", tbl, gc);
    trigger_safepoint("bar", tbl, gc);
    CHECK(gc.call_count == 2);
}

// ===========================================================================
// Section C: fuel namespace helpers and fuel_injection_pass
// ===========================================================================

TEST_CASE (



"fuel: make_fuel_op domain and name"
,
"[fuel]"
)
 {
    auto op = make_fuel_op();
    CHECK(op.domain == "lithe.fuel");
    CHECK(op.name == "fuel_check_tag");
}

TEST_CASE (



"fuel: make_fuel_op stable_hash is non-zero"
,
"[fuel]"
)
 {
    auto op = make_fuel_op();
    CHECK(op.stable_hash != 0);
}

TEST_CASE (



"fuel: make_fuel_check_instr structure"
,
"[fuel]"
)
 {
    auto instr = make_fuel_check_instr(42u);
    CHECK(instr.id == 42u);
    CHECK(instr.op == opcode::indirect_call);
    REQUIRE(instr.abstract_operation.has_value());
    CHECK(instr.abstract_operation->domain == "lithe.fuel");
    CHECK(instr.abstract_operation->name == "fuel_check_tag");
    CHECK(instr.defs.empty());
    CHECK(instr.uses.empty());
}

TEST_CASE (



"ExecutionSandbox: default construction"
,
"[fuel][sandbox]"
)
 {
    ExecutionSandbox sb;
    CHECK(sb.fuel_counter == 0);
    CHECK(sb.max_memory == 0);
}

TEST_CASE (



"fuel_injection_pass: entry block always gets fuel check"
,
"[fuel][pass]"
)
 {
    auto phys = wrap("entry_only", {
        make_block(1, "entry", {}, {
            make_inst(1, opcode::load_imm, {preg_op(0)}, {imm_op(0)}),
            make_inst(2, opcode::ret,      {},            {preg_op(0)}),
        })
    });

    fuel_injection_pass pass;
    auto result = pass.run(phys);

    CHECK(result.changed == true);

    const auto &blk = phys.function.blocks[0];
    REQUIRE(!blk.instructions.empty());
    const auto &first = blk.instructions[0];
    CHECK(first.op == opcode::indirect_call);
    REQUIRE(first.abstract_operation.has_value());
    CHECK(first.abstract_operation->domain == "lithe.fuel");
}

TEST_CASE (



"fuel_injection_pass: back-edge target block gets fuel check"
,
"[fuel][pass]"
)
 {
    // Block layout:
    //   bb1 (entry) → bb2
    //   bb2 (loop header) ← bb3 (back-edge: pred 3 >= target 2)
    //   bb3 → bb2 (back-edge), bb4
    //   bb4 (exit)
    auto phys = wrap("loop_fn", {
        make_block(1, "entry",  {2}, {
            make_inst(1, opcode::branch, {}, {block_op(2)}),
        }),
        make_block(2, "header", {3, 4}, {
            make_inst(2, opcode::cmp_lt, {preg_op(0)}, {preg_op(1), imm_op(5)}),
            make_inst(3, opcode::branch_cond, {}, {preg_op(0), block_op(3), block_op(4)}),
        }),
        make_block(3, "latch",  {2}, {
            make_inst(4, opcode::add, {preg_op(1)}, {preg_op(1), imm_op(1)}),
            make_inst(5, opcode::branch, {}, {block_op(2)}),
        }),
        make_block(4, "exit",   {}, {
            make_inst(6, opcode::ret, {}, {preg_op(1)}),
        }),
    });

    // Populate cfg.predecessors so the pass can see back-edges.
    auto &cfg = phys.function.cfg;
    cfg.predecessors[1] = {};
    cfg.predecessors[2] = {1, 3};  // bb3 → bb2: pred 3 >= bid 2 → back-edge target
    cfg.predecessors[3] = {2};
    cfg.predecessors[4] = {2};

    fuel_injection_pass pass;
    auto result = pass.run(phys);
    CHECK(result.changed == true);

    // Count fuel checks.
    int fuel_count = 0;
    for (const auto &blk : phys.function.blocks)
        for (const auto &inst : blk.instructions)
            if (inst.abstract_operation &&
                inst.abstract_operation->domain == "lithe.fuel")
                ++fuel_count;

    // Entry block (bb1) + back-edge target (bb2) = 2 fuel checks.
    CHECK(fuel_count == 2);
}

TEST_CASE (



"fuel_injection_pass: self-loop entry — no duplicate fuel check"
,
"[fuel][pass]"
)
 {
    // Block 1 loops to itself: cfg.predecessors[1] = {1}
    // Entry already covered; dedup must yield exactly one fuel check.
    auto phys = wrap("self_loop", {
        make_block(1, "entry", {1}, {
            make_inst(1, opcode::branch, {}, {block_op(1)}),
        }),
    });
    phys.function.cfg.predecessors[1] = {1};

    fuel_injection_pass pass;
    auto result = pass.run(phys);
    CHECK(result.changed == true);

    int fuel_count = 0;
    for (const auto &inst : phys.function.blocks[0].instructions)
        if (inst.abstract_operation &&
            inst.abstract_operation->domain == "lithe.fuel")
            ++fuel_count;

    CHECK(fuel_count == 1);
}

TEST_CASE (



"fuel_injection_pass: sandbox=null still inserts instructions (NOP contract)"
,
"[fuel][pass]"
)
 {
    auto phys = wrap("nop_contract", {
        make_block(1, "entry", {}, {
            make_inst(1, opcode::ret, {}, {imm_op(0)}),
        })
    });

    fuel_injection_pass pass;
    auto result = pass.run(phys, nullptr);

    CHECK(result.changed == true);
    const auto &first = phys.function.blocks[0].instructions[0];
    REQUIRE(first.abstract_operation.has_value());
    CHECK(first.abstract_operation->domain == "lithe.fuel");
}

TEST_CASE (



"fuel_injection_pass: IDs are beyond existing max instruction id"
,
"[fuel][pass]"
)
 {
    auto phys = wrap("id_check", {
        make_block(1, "entry", {}, {
            make_inst(100, opcode::load_imm, {preg_op(0)}, {imm_op(1)}),
            make_inst(200, opcode::ret,      {},            {preg_op(0)}),
        })
    });

    fuel_injection_pass pass;
    (void)pass.run(phys);

    const auto &injected_instr = phys.function.blocks[0].instructions[0];
    // Injected fuel check must have id > 200 (the old max).
    CHECK(injected_instr.id > 200u);
}

TEST_CASE (



"asmjit_backend: sandbox accessor round-trip"
,
"[fuel][backend]"
)
 {
    asmjit_backend backend;
    CHECK(backend.sandbox_ptr() == nullptr);

    ExecutionSandbox sb;
    backend.set_sandbox(&sb);
    CHECK(backend.sandbox_ptr() == &sb);

    backend.set_sandbox(nullptr);
    CHECK(backend.sandbox_ptr() == nullptr);
}

// ===========================================================================
// Section D: FFI — marshal/unmarshal and native_proxy
// ===========================================================================

TEST_CASE (



"marshal_to_native: i64 is identity"
,
"[ffi]"
)
 {
    runtime_value v = std::int64_t{42};
    CHECK(marshal_to_native(v) == 42LL);

    runtime_value v_neg = std::int64_t{-1};
    CHECK(marshal_to_native(v_neg) == -1LL);
}

TEST_CASE (



"marshal_to_native: double bit-casts correctly"
,
"[ffi]"
)
 {
    double d = 3.14159265358979;
    runtime_value v = d;
    std::int64_t bits = marshal_to_native(v);

    double recovered;
    std::memcpy(&recovered, &bits, sizeof(recovered));
    CHECK(recovered == d);
}

TEST_CASE (



"marshal_to_native: bool maps to 0 or 1"
,
"[ffi]"
)
 {
    CHECK(marshal_to_native(runtime_value{true})  == 1LL);
    CHECK(marshal_to_native(runtime_value{false}) == 0LL);
}

TEST_CASE (



"marshal_to_native: void* maps to its address as integer"
,
"[ffi]"
)
 {
    int dummy = 0;
    void *p = &dummy;
    runtime_value v = p;
    auto raw = marshal_to_native(v);
    CHECK(reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw)) == p);
}

TEST_CASE (



"marshal_to_native: object_ptr maps raw pointer to integer"
,
"[ffi]"
)
 {
    int dummy = 0;
    object_ptr op{&dummy, 0};
    runtime_value v = op;
    auto raw = marshal_to_native(v);
    CHECK(reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw)) == &dummy);
}

TEST_CASE (



"unmarshal_from_native: type_hint_i64 is identity"
,
"[ffi]"
)
 {
    auto v = unmarshal_from_native(99LL, type_hint_i64);
    REQUIRE(std::holds_alternative<std::int64_t>(v));
    CHECK(std::get<std::int64_t>(v) == 99LL);
}

TEST_CASE (



"unmarshal_from_native: type_hint_f64 bit-casts correctly"
,
"[ffi]"
)
 {
    double d = 2.718281828;
    std::int64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    auto v = unmarshal_from_native(bits, type_hint_f64);
    REQUIRE(std::holds_alternative<double>(v));
    CHECK(std::get<double>(v) == d);
}

TEST_CASE (



"unmarshal_from_native: type_hint_bool"
,
"[ffi]"
)
 {
    auto vt = unmarshal_from_native(1LL, type_hint_bool);
    REQUIRE(std::holds_alternative<bool>(vt));
    CHECK(std::get<bool>(vt) == true);

    auto vf = unmarshal_from_native(0LL, type_hint_bool);
    REQUIRE(std::holds_alternative<bool>(vf));
    CHECK(std::get<bool>(vf) == false);
}

TEST_CASE (



"unmarshal_from_native: type_hint_ptr recovers pointer"
,
"[ffi]"
)
 {
    int x = 0;
    auto raw = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&x));
    auto v = unmarshal_from_native(raw, type_hint_ptr);
    REQUIRE(std::holds_alternative<void*>(v));
    CHECK(std::get<void*>(v) == &x);
}

TEST_CASE (



"unmarshal_from_native: type_hint_obj recovers object_ptr.raw"
,
"[ffi]"
)
 {
    int x = 0;
    auto raw = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&x));
    auto v = unmarshal_from_native(raw, type_hint_obj);
    REQUIRE(std::holds_alternative<object_ptr>(v));
    CHECK(std::get<object_ptr>(v).raw == &x);
}

TEST_CASE (



"unmarshal_from_native: unknown hint falls through to i64"
,
"[ffi]"
)
 {
    auto v = unmarshal_from_native(7LL, 999u);
    REQUIRE(std::holds_alternative<std::int64_t>(v));
    CHECK(std::get<std::int64_t>(v) == 7LL);
}

TEST_CASE (



"marshal/unmarshal round-trip: i64"
,
"[ffi]"
)
 {
    runtime_value orig = std::int64_t{-12345};
    auto raw = marshal_to_native(orig);
    auto back = unmarshal_from_native(raw, type_hint_i64);
    CHECK(std::get<std::int64_t>(back) == std::get<std::int64_t>(orig));
}

TEST_CASE (



"marshal/unmarshal round-trip: double"
,
"[ffi]"
)
 {
    runtime_value orig = double{1.23456789};
    auto raw = marshal_to_native(orig);
    auto back = unmarshal_from_native(raw, type_hint_f64);
    CHECK(std::get<double>(back) == std::get<double>(orig));
}

TEST_CASE (



"marshal/unmarshal round-trip: bool"
,
"[ffi]"
)
 {
    for (bool b : {true, false}) {
        runtime_value orig = b;
        auto raw  = marshal_to_native(orig);
        auto back = unmarshal_from_native(raw, type_hint_bool);
        CHECK(std::get<bool>(back) == b);
    }
}

TEST_CASE (



"marshal/unmarshal round-trip: void*"
,
"[ffi]"
)
 {
    int x = 0;
    runtime_value orig = static_cast<void*>(&x);
    auto raw  = marshal_to_native(orig);
    auto back = unmarshal_from_native(raw, type_hint_ptr);
    CHECK(std::get<void*>(back) == &x);
}

TEST_CASE (



"native_proxy: default construction is invalid"
,
"[ffi]"
)
 {
    native_proxy np;
    CHECK(np.fn_ptr == nullptr);
    CHECK(np.valid() == false);
    CHECK(np.arity == 0);
    CHECK(np.ret_type == type_hint_i64);
}

TEST_CASE (



"native_proxy: valid when fn_ptr is non-null"
,
"[ffi]"
)
 {
    auto dummy_fn = []() -> int { return 0; };
    native_proxy np;
    np.fn_ptr = reinterpret_cast<void*>(+dummy_fn);
    np.arity  = 0;
    CHECK(np.valid() == true);
}

TEST_CASE (



"native_proxy: arity and type hint fields"
,
"[ffi]"
)
 {
    native_proxy np;
    np.fn_ptr  = reinterpret_cast<void*>(std::uintptr_t{1});
    np.arity   = 3;
    np.ret_type = type_hint_f64;
    np.arg_types[0] = type_hint_i64;
    np.arg_types[1] = type_hint_f64;
    np.arg_types[2] = type_hint_bool;
    CHECK(np.arity   == 3);
    CHECK(np.ret_type == type_hint_f64);
    CHECK(np.arg_types[0] == type_hint_i64);
    CHECK(np.arg_types[1] == type_hint_f64);
    CHECK(np.arg_types[2] == type_hint_bool);
}

TEST_CASE (



"asmjit_backend: register and find native_proxy"
,
"[ffi][backend]"
)
 {
    asmjit_backend backend;

    CHECK(backend.find_native_proxy("add") == nullptr);

    native_proxy np;
    np.fn_ptr = reinterpret_cast<void*>(std::uintptr_t{0xCAFE});
    np.arity  = 2;
    backend.register_native_proxy("add", np);

    const auto *found = backend.find_native_proxy("add");
    REQUIRE(found != nullptr);
    CHECK(found->arity == 2);
    CHECK(found->fn_ptr == reinterpret_cast<void*>(std::uintptr_t{0xCAFE}));
    CHECK(backend.find_native_proxy("missing") == nullptr);
}

TEST_CASE (



"asmjit_backend: multiple proxies co-exist"
,
"[ffi][backend]"
)
 {
    asmjit_backend backend;

    native_proxy add_proxy;
    add_proxy.fn_ptr = reinterpret_cast<void*>(std::uintptr_t{1});
    add_proxy.arity  = 2;

    native_proxy mul_proxy;
    mul_proxy.fn_ptr = reinterpret_cast<void*>(std::uintptr_t{2});
    mul_proxy.arity  = 2;

    backend.register_native_proxy("add", add_proxy);
    backend.register_native_proxy("mul", mul_proxy);

    REQUIRE(backend.find_native_proxy("add") != nullptr);
    REQUIRE(backend.find_native_proxy("mul") != nullptr);
    CHECK(backend.find_native_proxy("add")->fn_ptr != backend.find_native_proxy("mul")->fn_ptr);
}

// ===========================================================================
// Section E: Integration — fuel decrement in JIT with sandbox
// ===========================================================================

TEST_CASE (



"asmjit_backend: fuel NOP when sandbox is null — function runs correctly"
,
"[fuel][backend][integration]"
)
{
    auto phys = fn_add_simple();

    (void)fuel_injection_pass{}.run(phys, nullptr);

    asmjit_backend backend;
    // sandbox not set → fuel check emits no machine code
    auto handle = jit_compile(std::move(phys), backend);
    CHECK(handle->call(3, 4) == 7);
}

TEST_CASE (



"asmjit_backend: fuel decrements by 1 per call"
,
"[fuel][backend][integration]"
)
 {
    auto phys = fn_add_simple();
    (void)fuel_injection_pass{}.run(phys);

    ExecutionSandbox sb;
    sb.fuel_counter = 100;

    asmjit_backend backend;
    backend.set_sandbox(&sb);

    auto handle = jit_compile(std::move(phys), backend);
    auto result = handle->call(3, 4);

    CHECK(result == 7);
    CHECK(sb.fuel_counter == 99);
}

TEST_CASE (



"asmjit_backend: fuel decrements on repeated calls"
,
"[fuel][backend][integration]"
)
 {
    auto phys = fn_add_simple();
    (void)fuel_injection_pass{}.run(phys);

    ExecutionSandbox sb;
    sb.fuel_counter = 10;

    asmjit_backend backend;
    backend.set_sandbox(&sb);

    auto handle = jit_compile(std::move(phys), backend);

    for (int i = 0; i < 5; ++i)
        (void)handle->call(1, 1);

    CHECK(sb.fuel_counter == 5);
}

TEST_CASE (



"asmjit_backend: fuel not decremented when no fuel_check in IR"
,
"[fuel][backend][integration]"
)
 {
    // Raw fn_add with no fuel_injection_pass → counter must not change.
    auto phys = fn_add_simple();

    ExecutionSandbox sb;
    sb.fuel_counter = 50;

    asmjit_backend backend;
    backend.set_sandbox(&sb);

    auto handle = jit_compile(std::move(phys), backend);
    (void)handle->call(2, 3);

    CHECK(sb.fuel_counter == 50);
}

// ===========================================================================
// Section: Abstract Runtime Value Layer (lithe::runtime::values)
// ===========================================================================

namespace vals = lithe::runtime::values;

TEST_CASE (


"values::object_ref: trivially copyable, valid() reflects null-ness"
,
"[values]"
)
 {
    static_assert(std::is_trivially_copyable_v<vals::object_ref>);

    vals::object_ref empty;
    CHECK_FALSE(empty.valid());
    CHECK_FALSE(static_cast<bool>(empty));

    int dummy = 0;
    vals::object_ref ref{&dummy, 42u, 7u};
    CHECK(ref.valid());
    CHECK(static_cast<bool>(ref));
    CHECK(ref.layout_id == 42u);
    CHECK(ref.plugin_tag == 7u);
}

TEST_CASE (


"values::object_ref: round-trip to/from mop::object_ptr"
,
"[values]"
)
 {
    int x = 0;
    mop::object_ptr mop_p{&x, 99u};

    auto ref = vals::object_ref::from_mop_ptr(mop_p, 3u);
    CHECK(ref.ptr == &x);
    CHECK(ref.layout_id == 99u);
    CHECK(ref.plugin_tag == 3u);

    auto back = ref.to_mop_ptr();
    CHECK(back.raw == &x);
    CHECK(back.layout_id == 99u);
}

TEST_CASE (


"values::native_function_ref: trivially copyable, valid() reflects null fn_ptr"
,
"[values]"
)
 {
    static_assert(std::is_trivially_copyable_v<vals::native_function_ref>);

    vals::native_function_ref empty;
    CHECK_FALSE(empty.valid());

    auto fn = [](std::int64_t a, std::int64_t b) -> std::int64_t { return a + b; };
    vals::native_function_ref ref;
    ref.fn_ptr = reinterpret_cast<void*>(+fn);
    ref.arity  = 2;
    ref.ret_hint = type_hint_i64;
    ref.param_hints[0] = type_hint_i64;
    ref.param_hints[1] = type_hint_i64;
    CHECK(ref.valid());
    CHECK(ref.arity == 2);
}

TEST_CASE (


"values::dynamic_value: trivially copyable, default is i64(0)"
,
"[values]"
)
 {
    static_assert(std::is_trivially_copyable_v<vals::dynamic_value>);

    vals::dynamic_value dv;
    CHECK(vals::is_i64(dv));
    CHECK(vals::as_i64(dv) == 0);
}

TEST_CASE (


"values::dynamic_value: factory helpers round-trip"
,
"[values]"
)
 {
    auto vi = vals::make_i64(42);
    CHECK(vals::is_i64(vi));
    CHECK(vals::as_i64(vi) == 42);

    auto vf = vals::make_f64(3.14);
    CHECK(vals::is_f64(vf));
    CHECK(vals::as_f64(vf) == 3.14);

    auto vb = vals::make_bool(true);
    CHECK(vals::is_bool(vb));
    CHECK(vals::as_bool(vb) == true);

    int x = 0;
    auto vp = vals::make_ptr(&x);
    CHECK(vals::is_ptr(vp));
    CHECK(vals::as_ptr(vp) == &x);

    vals::object_ref oref{&x, 1u, 0u};
    auto vo = vals::make_object(oref);
    CHECK(vals::is_object(vo));
    CHECK(vals::as_object(vo).layout_id == 1u);

    vals::native_function_ref fnref;
    fnref.fn_ptr = &x;
    fnref.arity  = 1;
    auto vfn = vals::make_func(fnref);
    CHECK(vals::is_func(vfn));
    CHECK(vals::as_func(vfn).arity == 1);
}

TEST_CASE (


"values::marshal_to_native / unmarshal_from_native: i64 round-trip"
,
"[values][marshal]"
)
 {
    auto v = vals::make_i64(0x7FFF'FFFF'FFFF'FFLL);
    std::int64_t raw = vals::marshal_to_native(v);
    CHECK(raw == 0x7FFF'FFFF'FFFF'FFLL);

    auto back = vals::unmarshal_from_native(raw, type_hint_i64);
    CHECK(vals::is_i64(back));
    CHECK(vals::as_i64(back) == 0x7FFF'FFFF'FFFF'FFLL);
}

TEST_CASE (


"values::marshal_to_native / unmarshal_from_native: f64 bit-exact round-trip"
,
"[values][marshal]"
)
 {
    constexpr double pi = 3.141592653589793;
    auto v = vals::make_f64(pi);
    std::int64_t raw = vals::marshal_to_native(v);

    // memcpy reference for the expected bit pattern
    std::int64_t expected;
    std::memcpy(&expected, &pi, sizeof(expected));
    CHECK(raw == expected);

    auto back = vals::unmarshal_from_native(raw, type_hint_f64);
    CHECK(vals::is_f64(back));
    CHECK(vals::as_f64(back) == pi);
}

TEST_CASE (


"values::marshal_to_native / unmarshal_from_native: bool round-trip"
,
"[values][marshal]"
)
 {
    auto vt = vals::make_bool(true);
    CHECK(vals::marshal_to_native(vt) == 1);

    auto vf = vals::make_bool(false);
    CHECK(vals::marshal_to_native(vf) == 0);

    auto back_t = vals::unmarshal_from_native(1, type_hint_bool);
    CHECK(vals::is_bool(back_t));
    CHECK(vals::as_bool(back_t) == true);

    auto back_f = vals::unmarshal_from_native(0, type_hint_bool);
    CHECK(vals::as_bool(back_f) == false);
}

TEST_CASE (


"values::marshal_to_native / unmarshal_from_native: void* round-trip"
,
"[values][marshal]"
)
 {
    int x = 0;
    auto vp = vals::make_ptr(&x);
    std::int64_t raw = vals::marshal_to_native(vp);
    CHECK(raw == static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&x)));

    auto back = vals::unmarshal_from_native(raw, type_hint_ptr);
    CHECK(vals::is_ptr(back));
    CHECK(vals::as_ptr(back) == &x);
}

TEST_CASE (


"values::marshal_to_native: object_ref encodes raw ptr"
,
"[values][marshal]"
)
 {
    int x = 0;
    vals::object_ref oref{&x, 7u, 1u};
    auto vobj = vals::make_object(oref);
    std::int64_t raw = vals::marshal_to_native(vobj);
    CHECK(raw == static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&x)));

    auto back = vals::unmarshal_from_native(raw, type_hint_obj);
    CHECK(vals::is_object(back));
    // ptr recovers correctly; layout_id/plugin_tag need registry lookup post-unmarshal
    CHECK(vals::as_object(back).ptr == &x);
}

TEST_CASE (


"values::boxed_value: trivially copyable, carries hint with value"
,
"[values]"
)
 {
    static_assert(std::is_trivially_copyable_v<vals::boxed_value>);

    vals::boxed_value bv{vals::make_i64(100), type_hint_i64};
    CHECK(vals::is_i64(bv.value));
    CHECK(vals::as_i64(bv.value) == 100);
    CHECK(bv.type_hint == type_hint_i64);

    vals::boxed_value bv_f{vals::make_f64(2.718), type_hint_f64};
    CHECK(vals::is_f64(bv_f.value));
    CHECK(bv_f.type_hint == type_hint_f64);
}

TEST_CASE (


"values: Terminal trait specialisations compile"
,
"[values][terminal]"
)
 {
    static_assert(lithe::is_terminal<vals::dynamic_value>::value);
    static_assert(lithe::is_terminal<vals::boxed_value>::value);
    static_assert(lithe::is_terminal<vals::object_ref>::value);
    static_assert(lithe::Terminal<vals::dynamic_value>);
    static_assert(lithe::Terminal<vals::boxed_value>);
    static_assert(lithe::Terminal<vals::object_ref>);
    SUCCEED("Terminal static_asserts passed");
}

// --- impl-1: tag_descriptor extensibility -------------------------------
namespace {
    struct my_custom_tag {};
}

template <>
struct lithe::emit::tag_descriptor<my_custom_tag> {
    static constexpr std::string_view symbol = "custom";
    static constexpr std::size_t stable_id = lithe::emit::kExtensionIdBase + 7u;
    static constexpr std::uint8_t arity = 2;
};

TEST_CASE (


"tag_descriptor: custom tag registers id/symbol/arity"
,
"[lithe][tag_descriptor]"
)
 {
    using namespace lithe::emit;
    STATIC_REQUIRE(tag_descriptor<my_custom_tag>::stable_id >= kExtensionIdBase);
    // tag_name / tag_id resolve through the descriptor
    CHECK(std::string_view{tag_name<my_custom_tag>::value} == "custom");
    CHECK(tag_id<my_custom_tag>::value == kExtensionIdBase + 7u);
    // built-ins unchanged
    CHECK(std::string_view{tag_name<lithe::add_tag>::value} == "+");
    CHECK(tag_id<lithe::add_tag>::value == 1u);
    CHECK(tag_descriptor<lithe::add_tag>::arity == 2);
    CHECK(tag_descriptor<lithe::neg_tag>::arity == 1);
    // custom id lands in a distinct structural_hash bucket from a built-in
    CHECK(tag_id<my_custom_tag>::value != tag_id<lithe::add_tag>::value);
}

// --- impl-2: value-aware structural_hash --------------------------------
namespace {
    struct payload_tag {};

    struct payload_leaf : lithe::interface<payload_leaf> {
        using is_lithe_node = void;
        using tag_type = payload_tag;
        std::tuple<> children{};
        double value{};
        explicit constexpr payload_leaf(double v) : value(v) {}
    };

    // ADL hook: mix the stored value.
    inline std::size_t structural_payload_hash(const payload_leaf& e) noexcept {
        return std::hash<double>{}(e.value);
    }

    struct plain_tag {};

    struct plain_leaf : lithe::interface<plain_leaf> {
        using is_lithe_node = void;
        using tag_type = plain_tag;
        std::tuple<> children{};
    };
}

TEST_CASE (


"structural_hash: payload hook separates distinct constants"
,
"[lithe][hash]"
)
 {
    namespace emit = lithe::emit;
    CHECK(emit::structural_hash(payload_leaf{1.0}) != emit::structural_hash(payload_leaf{2.0}));
    CHECK(emit::structural_hash(payload_leaf{3.5}) == emit::structural_hash(payload_leaf{3.5}));
    // topology-only tag unaffected: two plain leaves hash equally
    CHECK(emit::structural_hash(plain_leaf{}) == emit::structural_hash(plain_leaf{}));
}

// --- impl-3: generic compile-time folds ---------------------------------
namespace {
    template <class N>
    struct is_add_pred {
        static constexpr bool value = std::is_same_v<typename N::tag_type, lithe::add_tag>;
    };

    template <class N>
    struct always_true_pred {
        static constexpr bool value = true;
    };

    struct one_contrib {
        template <class N>
        consteval std::size_t operator()() const { return 1; }
    };
}

TEST_CASE (


"tree::all_tags_satisfy / any_tag_satisfies"
,
"[lithe][tree][fold]"
)
 {
    auto e = lithe::make_node<lithe::add_tag>(lithe::make_node<lithe::mul_tag>(1, 2), 3);
    using E = decltype(e);
    // not every node is add_tag (mul present) → all=false, any=true
    STATIC_REQUIRE(!lithe::tree::all_tags_satisfy<E, is_add_pred>());
    STATIC_REQUIRE( lithe::tree::any_tag_satisfies<E, is_add_pred>());
    STATIC_REQUIRE( lithe::tree::all_tags_satisfy<E, always_true_pred>());
}

TEST_CASE (


"tree::fold counts nodes (sum/max via combine)"
,
"[lithe][tree][fold]"
)
 {
    auto e = lithe::make_node<lithe::add_tag>(lithe::make_node<lithe::mul_tag>(1, 2), 3);
    using E = decltype(e);
    // sum of 1-per-node = number of Expression nodes (add + mul = 2; int terminals excluded)
    constexpr auto n = lithe::tree::fold<E>(
        one_contrib{}, [](std::size_t a, std::size_t b) consteval { return a + b; }, 0zu);
    STATIC_REQUIRE(n == 2);
    // max-combine of constant 1 = 1 (mirrors input_slot_count max-fold shape)
    constexpr auto m = lithe::tree::fold<E>(
        one_contrib{}, [](std::size_t a, std::size_t b) consteval { return a > b ? a : b; }, 0zu);
    STATIC_REQUIRE(m == 1);
}

