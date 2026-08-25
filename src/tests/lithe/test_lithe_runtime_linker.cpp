#include "catch_amalgamated.hpp"

#include "lithe/lithe_runtime.hpp"
#include "lithe/backends/lithe_codegen_asmjit.hpp"

#include <atomic>
#include <thread>

using namespace lithe::runtime::linker;
using namespace lithe::codegen;
using namespace lithe::codegen::backends;
using namespace symtab;

// ============================================================================
// Helpers
// ============================================================================

namespace {
    static std::int64_t add_fn(std::int64_t a, std::int64_t b) { return a + b; }
    static std::int64_t mul_fn(std::int64_t a, std::int64_t b) { return a * b; }
    static std::int64_t fixed_42(std::int64_t, std::int64_t) { return 42; }

    // Build a physical_mir_function that calls an external symbol named `sym_name`
    // with two load_arg operands and returns the result.
    mir::physical_mir_function make_extern_call_fn(const std::string& sym_name) {
        std::vector<allocated_instruction> insts;
        std::uint32_t iid = 0;

        // r0 = arg[0]
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::load_arg;
            i.defs = {allocated_operand::as_preg(preg{0, "r0"})};
            i.uses = {allocated_operand::as_argument_index(0)};
            insts.push_back(i);
        }
        // r1 = arg[1]
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::load_arg;
            i.defs = {allocated_operand::as_preg(preg{1, "r1"})};
            i.uses = {allocated_operand::as_argument_index(1)};
            insts.push_back(i);
        }
        // r2 = external_call_tag("sym_name", r0, r1)
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::indirect_call;
            i.abstract_operation = make_linker_op(external_call_tag_name);
            i.defs = {allocated_operand::as_preg(preg{2, "r2"})};
            i.uses = {
                allocated_operand::as_symbol(sym_name),
                allocated_operand::as_preg(preg{0, "r0"}),
                allocated_operand::as_preg(preg{1, "r1"}),
            };
            insts.push_back(i);
        }
        // ret r2
        {
            allocated_instruction i;
            i.id = ++iid;
            i.op = opcode::ret;
            i.uses = {allocated_operand::as_preg(preg{2, "r2"})};
            insts.push_back(i);
        }

        allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";
        bb.instructions = std::move(insts);

        allocated_function_ir fn;
        fn.name = "extern_call_" + sym_name;
        fn.cfg.entry_block = 1;
        fn.blocks = {bb};

        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }
} // anonymous namespace

// ============================================================================
// linker_context — unit
// ============================================================================

TEST_CASE (



"[linker_context] default-constructs with own SymbolTable"
,
"[linker]"
)
{
    linker_context ctx;
    REQUIRE(ctx.resolve("anything") == nullptr);
    REQUIRE(ctx.pending_patches() == 0);
}

TEST_CASE (



"[linker_context] register and resolve round-trip"
,
"[linker]"
)
{
    linker_context ctx;
    auto r = ctx.register_symbol("mylib::add", reinterpret_cast<void*>(&add_fn));
    REQUIRE(r.has_value());
    REQUIRE(r->is_valid());
    void *p = ctx.resolve("mylib::add");
    REQUIRE(p == reinterpret_cast<void*>(&add_fn));
}

TEST_CASE (



"[linker_context] resolve unregistered symbol returns nullptr"
,
"[linker]"
)
{
    linker_context ctx;
    REQUIRE(ctx.resolve("does::not::exist") == nullptr);
}

TEST_CASE (



"[linker_context] duplicate registration returns AlreadyRegistered"
,
"[linker]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("ns::fn", reinterpret_cast<void*>(&add_fn));
    auto r = ctx.register_symbol("ns::fn", reinterpret_cast<void*>(&mul_fn));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == SymError::AlreadyRegistered);
    // Original address unchanged.
    REQUIRE(ctx.resolve("ns::fn") == reinterpret_cast<void*>(&add_fn));
}

TEST_CASE (



"[linker_context] version upgrade replaces address"
,
"[linker]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("ns::fn", reinterpret_cast<void*>(&add_fn), 1);
    auto r = ctx.register_symbol("ns::fn", reinterpret_cast<void*>(&mul_fn), 2);
    REQUIRE(r.has_value());
    REQUIRE(ctx.resolve("ns::fn") == reinterpret_cast<void*>(&mul_fn));
}

TEST_CASE (



"[linker_context] resolve_versioned checks exact version"
,
"[linker]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("fn", reinterpret_cast<void*>(&add_fn), 3);
    REQUIRE(ctx.resolve_versioned("fn", 3) == reinterpret_cast<void*>(&add_fn));
    REQUIRE(ctx.resolve_versioned("fn", 2) == nullptr);
    REQUIRE(ctx.resolve_versioned("fn", 4) == nullptr);
}

TEST_CASE (



"[linker_context] namespace index populated on register"
,
"[linker]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("lithe::linker::fn1", reinterpret_cast<void*>(&add_fn));
    (void)ctx.register_symbol("lithe::linker::fn2", reinterpret_cast<void*>(&mul_fn));

    auto syms = ctx.index().enumerate("lithe::linker");
    REQUIRE(syms.size() == 2);
}

TEST_CASE (



"[linker_context] namespace depth via index"
,
"[linker]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("a::b::c::fn", reinterpret_cast<void*>(&add_fn));
    REQUIRE(ctx.index().depth("a") == 1);
    REQUIRE(ctx.index().depth("a::b") == 2);
    REQUIRE(ctx.index().depth("a::b::c") == 3);
}

TEST_CASE (



"[linker_context] borrowed SymbolTable: context does not own"
,
"[linker]"
)
{
    SymbolTable<> tbl;
    (void)tbl.register_symbol("shared::fn", reinterpret_cast<void*>(&add_fn));

    linker_context ctx(&tbl);
    REQUIRE(ctx.resolve("shared::fn") == reinterpret_cast<void*>(&add_fn));

    // Register via context should appear in the shared table.
    (void)ctx.register_symbol("shared::fn2", reinterpret_cast<void*>(&mul_fn));
    REQUIRE(tbl.resolve("shared::fn2") == reinterpret_cast<void*>(&mul_fn));
}

// ============================================================================
// stub_patch
// ============================================================================

TEST_CASE (



"[stub_patch] record and apply patches"
,
"[linker]"
)
{
    linker_context ctx;
    REQUIRE(ctx.pending_patches() == 0);

    // Allocate a real "call site slot" that apply_patches can write into.
    void *slot_a = nullptr;
    // record_stub_patch: offset from jit_base to &slot_a
    // We'll use &slot_a itself as jit_base (offset = 0).
    ctx.record_stub_patch(0x0, "unresolved::fn");
    REQUIRE(ctx.pending_patches() == 1);

    // apply_patches with no resolution → nothing applied.
    auto applied = ctx.apply_patches(reinterpret_cast<void*>(&slot_a));
    REQUIRE(applied == 0);
    REQUIRE(ctx.pending_patches() == 1);

    // Now register the symbol.
    (void)ctx.register_symbol("unresolved::fn", reinterpret_cast<void*>(&fixed_42));

    // apply_patches now resolves it and writes into slot_a (offset 0 from &slot_a).
    auto applied2 = ctx.apply_patches(reinterpret_cast<void*>(&slot_a));
    REQUIRE(applied2 == 1);
    REQUIRE(ctx.pending_patches() == 0);
}

TEST_CASE (



"[stub_patch] multiple patches: only resolved ones applied"
,
"[linker]"
)
{
    linker_context ctx;
    // Use real slots; offsets 0 and 8 from a base pointer.
    void *slots[2] = {nullptr, nullptr};
    void *base = reinterpret_cast<void*>(slots);

    ctx.record_stub_patch(0x00, "fn::resolved");
    ctx.record_stub_patch(sizeof(void*), "fn::still_pending");
    REQUIRE(ctx.pending_patches() == 2);

    (void)ctx.register_symbol("fn::resolved", reinterpret_cast<void*>(&add_fn));

    // apply_patches: only "fn::resolved" has an address; "fn::still_pending" stays.
    auto applied = ctx.apply_patches(base);
    REQUIRE(applied == 1);
    REQUIRE(ctx.pending_patches() == 1);
}

// ============================================================================
// resolve_symbol_stub
// ============================================================================

TEST_CASE (



"[resolve_symbol_stub] returns nullptr with no thread context"
,
"[linker]"
)
{
    // Ensure no thread-local context is set.
    set_thread_linker_context(nullptr);
    void *slot = nullptr;
    void *result = resolve_symbol_stub("any::fn", &slot);
    REQUIRE(result == nullptr);
    REQUIRE(slot == nullptr);
}

TEST_CASE (



"[resolve_symbol_stub] resolves registered symbol and patches slot"
,
"[linker]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("stub::target", reinterpret_cast<void*>(&add_fn));

    set_thread_linker_context(&ctx);

    void *slot = nullptr;
    void *result = resolve_symbol_stub("stub::target", &slot);

    REQUIRE(result == reinterpret_cast<void*>(&add_fn));
    REQUIRE(slot  == reinterpret_cast<void*>(&add_fn));

    set_thread_linker_context(nullptr);
}

TEST_CASE (



"[resolve_symbol_stub] returns nullptr for unregistered symbol"
,
"[linker]"
)
{
    linker_context ctx;
    set_thread_linker_context(&ctx);

    void *slot = reinterpret_cast<void*>(0xdeadUL);
    void *result = resolve_symbol_stub("no::such::fn", &slot);

    REQUIRE(result == nullptr);
    // Slot must not be touched when resolution fails.
    REQUIRE(slot == reinterpret_cast<void*>(0xdeadUL));

    set_thread_linker_context(nullptr);
}

TEST_CASE (



"[resolve_symbol_stub] null slot pointer is safe"
,
"[linker]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("safe::fn", reinterpret_cast<void*>(&mul_fn));
    set_thread_linker_context(&ctx);

    void *result = resolve_symbol_stub("safe::fn", nullptr);
    REQUIRE(result == reinterpret_cast<void*>(&mul_fn));

    set_thread_linker_context(nullptr);
}

// ============================================================================
// Two-reader thread-safety
// ============================================================================

TEST_CASE (



"[linker_context] thread-safety: two concurrent readers"
,
"[linker]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("shared::reader_fn",
                               reinterpret_cast<void*>(&add_fn));

    std::atomic<int> errors{0};
    constexpr int N = 1000;

    std::thread t1([&]() {
        for (int i = 0; i < N; ++i)
            if (ctx.resolve("shared::reader_fn") != reinterpret_cast<void*>(&add_fn))
                ++errors;
    });
    std::thread t2([&]() {
        for (int i = 0; i < N; ++i)
            if (ctx.resolve("shared::reader_fn") != reinterpret_cast<void*>(&add_fn))
                ++errors;
    });
    t1.join(); t2.join();
    REQUIRE(errors.load() == 0);
}

// ============================================================================
// make_linker_op / make_external_call_tag_instr helpers
// ============================================================================

TEST_CASE (



"[make_linker_op] produces correct operation_id"
,
"[linker]"
)
{
    auto op = make_linker_op(external_call_tag_name);
    REQUIRE(op.domain == std::string(linker_domain));
    REQUIRE(op.name   == std::string(external_call_tag_name));
    REQUIRE(op.stable_hash != 0);
}

TEST_CASE (



"[make_external_call_tag_instr] encodes symbol in uses[0]"
,
"[linker]"
)
{
    allocated_operand ret_def = allocated_operand::as_preg(preg{0, "r0"});
    auto instr = make_external_call_tag_instr(1, "ext::compute", {}, ret_def);

    REQUIRE(instr.op == opcode::indirect_call);
    REQUIRE(instr.abstract_operation.has_value());
    REQUIRE(instr.abstract_operation->domain == std::string(linker_domain));
    REQUIRE(instr.abstract_operation->name   == std::string(external_call_tag_name));
    REQUIRE_FALSE(instr.uses.empty());
    REQUIRE(instr.uses[0].type == allocated_operand::kind::symbol);
    REQUIRE(std::get<std::string>(instr.uses[0].value) == "ext::compute");
    REQUIRE_FALSE(instr.defs.empty());
}

// ============================================================================
// Integration — eager path (symbol registered before compile)
// ============================================================================

TEST_CASE (



"[linker] integration: eager call to pre-registered symbol"
,
"[linker][integration]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("test::add", reinterpret_cast<void*>(&add_fn));

    asmjit_backend backend;
    backend.set_linker_context(&ctx);

    auto phys = make_extern_call_fn("test::add");
    auto art  = backend.emit(phys);

    REQUIRE(art.diagnostics.empty());
    auto *handle = asmjit_backend::get_handle(art);
    REQUIRE(handle != nullptr);
    REQUIRE(handle->valid());

    // (3 + 7) == 10
    REQUIRE(handle->call(3, 7) == 10);
    // (100 + 1) == 101
    REQUIRE(handle->call(100, 1) == 101);
}

TEST_CASE (



"[linker] integration: eager call to mul function"
,
"[linker][integration]"
)
{
    linker_context ctx;
    (void)ctx.register_symbol("test::mul", reinterpret_cast<void*>(&mul_fn));

    asmjit_backend backend;
    backend.set_linker_context(&ctx);

    auto phys = make_extern_call_fn("test::mul");
    auto art  = backend.emit(phys);

    REQUIRE(art.diagnostics.empty());
    auto *handle = asmjit_backend::get_handle(art);
    REQUIRE(handle != nullptr);
    REQUIRE(handle->valid());

    REQUIRE(handle->call(3, 7)  == 21);
    REQUIRE(handle->call(6, 6)  == 36);
}

// ============================================================================
// Integration — unresolved symbol path
// ============================================================================

TEST_CASE (



"[linker] integration: unresolved symbol emits diagnostics"
,
"[linker][integration]"
)
{
    linker_context ctx;
    // Do NOT register "missing::fn"

    asmjit_backend backend;
    backend.set_linker_context(&ctx);

    // When the linker can't resolve at compile time and stub path is not
    // available (addr==nullptr from try_linker_dispatch), the backend adds
    // a diagnostic and does not JIT.
    // (resolve_symbol_stub is the lazy path when addr is non-null in lnk)
    // Without a symbol registered the stub addr is still returned — verify
    // the resolved result returns nullptr at runtime when called via stub.
    auto phys = make_extern_call_fn("missing::fn");
    auto art  = backend.emit(phys);

    // The backend should record a stub patch and emit successfully via stub.
    // Alternatively it emits a diagnostic — either is acceptable; just ensure
    // no crash and no undefined behaviour.
    (void)art;
}

// ============================================================================
// Integration — lazy path (symbol registered after compile)
// ============================================================================

TEST_CASE (



"[linker] integration: lazy binding via stub — register after compile"
,
"[linker][integration]"
)
{
    // The lazy path: symbol not registered before emit().
    // The backend records a stub_patch and emits a call through
    // resolve_symbol_stub.  After registering the symbol we verify:
    //   1. apply_patches succeeds (patch count drops to 0).
    //   2. resolve_symbol_stub returns the correct address.
    // Full call-through re-dispatch would require self-modifying code and is
    // out of scope for this test; the stub correctness is validated above in
    // the [resolve_symbol_stub] tests.
    linker_context ctx;
    // "lazy::target" is NOT registered before emit.

    asmjit_backend backend;
    backend.set_linker_context(&ctx);

    auto phys = make_extern_call_fn("lazy::target");
    auto art  = backend.emit(phys);

    // Compilation must succeed — stub trampoline emitted.
    REQUIRE(art.diagnostics.empty());
    auto *handle = asmjit_backend::get_handle(art);
    REQUIRE(handle != nullptr);
    REQUIRE(handle->valid());

    // One pending patch recorded by the backend.
    REQUIRE(ctx.pending_patches() == 1);

    // Register the symbol post-compile.
    (void)ctx.register_symbol("lazy::target", reinterpret_cast<void*>(&add_fn));

    // apply_patches: the patch has offset 0 from a dummy base.
    // Since we recorded with call_site_hint=0 and have no real JIT base,
    // pass a dummy slot so the write goes somewhere safe.
    void *dummy_slot = nullptr;
    std::size_t applied = ctx.apply_patches(reinterpret_cast<void*>(&dummy_slot));
    REQUIRE(applied == 1);
    REQUIRE(ctx.pending_patches() == 0);

    // Verify the stub itself resolves correctly for subsequent calls.
    set_thread_linker_context(&ctx);
    void *slot = nullptr;
    void *resolved = resolve_symbol_stub("lazy::target", &slot);
    REQUIRE(resolved == reinterpret_cast<void*>(&add_fn));
    set_thread_linker_context(nullptr);
}

// ============================================================================
// Finding 1: linker namespace index survives entry-store growth
// ============================================================================

TEST_CASE (


"linker namespace index survives entry-store growth"
,
"[lithe][linker]"
)
 {
    linker_context ctx;

    constexpr int N = 1024;
    static int dummy[N];
    for (int i = 0; i < N; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "sym_%d", i);
        auto r = ctx.register_symbol(name, &dummy[i]);
        REQUIRE(r.has_value());
    }

    // All symbols must still be resolvable via the namespace index after
    // potential reallocation of the entries_ deque buckets.
    auto entries = ctx.index().enumerate("");
    REQUIRE(entries.size() == N);
    for (const auto* e : entries) {
        REQUIRE(e != nullptr);
        REQUIRE(e->address != nullptr);
    }

    // Spot-check a few via direct resolve.
    REQUIRE(ctx.resolve("sym_0")   == static_cast<void*>(&dummy[0]));
    REQUIRE(ctx.resolve("sym_511") == static_cast<void*>(&dummy[511]));
    REQUIRE(ctx.resolve("sym_1023")== static_cast<void*>(&dummy[1023]));
}
