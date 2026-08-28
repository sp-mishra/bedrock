#include "catch_amalgamated.hpp"

#include "lithe/backends/lithe_codegen_backend_registry.hpp"
#include "lithe/backends/lithe_codegen_debug_text_backend.hpp"
#include "lithe/backends/lithe_codegen_interpreter.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"
#include "lithe/lithe_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------
namespace {
    using namespace lithe::codegen;
    using namespace lithe::codegen::backends;

    mir::physical_mir_function make_physical(
        std::string name,
        std::vector<allocated_instruction> instructions
    ) {
        allocated_function_ir fn;
        fn.name = std::move(name);
        fn.cfg.entry_block = 1;

        allocated_basic_block bb;
        bb.id = 1;
        bb.name = "entry";
        bb.instructions = std::move(instructions);
        fn.blocks.push_back(std::move(bb));

        mir::physical_mir_function physical;
        physical.function = std::move(fn);
        physical.metadata.current_phase = mir::phase::physical_mir;
        return physical;
    }

    // One integer add + ret — covered by any backend advertising integer_arithmetic.
    mir::physical_mir_function make_integer_add_fn() {
        allocated_instruction add;
        add.id = 1;
        add.op = opcode::add;
        add.defs = {allocated_operand::as_preg({0, "r0"})};
        add.uses = {
            allocated_operand::as_preg({1, "r1"}),
            allocated_operand::as_preg({2, "r2"}),
        };

        allocated_instruction ret;
        ret.id = 2;
        ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({0, "r0"})};

        return make_physical("integer_add", {add, ret});
    }

    // fadd — requires floating_arithmetic; will fail on caps that lack it.
    mir::physical_mir_function make_fp_add_fn() {
        allocated_instruction fadd;
        fadd.id = 1;
        fadd.op = opcode::fadd;
        fadd.defs = {allocated_operand::as_preg({0, "f0"})};
        fadd.uses = {
            allocated_operand::as_preg({1, "f1"}),
            allocated_operand::as_preg({2, "f2"}),
        };

        allocated_instruction ret;
        ret.id = 2;
        ret.op = opcode::ret;

        return make_physical("fp_add", {fadd, ret});
    }

    bool diagnostics_contain(const compilation_artifact& art, std::string_view needle) {
        return std::ranges::any_of(art.diagnostics, [needle](const std::string& s) {
            return s.find(needle) != std::string::npos;
        });
    }
} // namespace

// ===========================================================================
// Scenario 1 — Happy-path native execution
//
// debug_text_backend advertises full capabilities; verify_backend_legality
// passes and execute_with_fallback routes directly to the primary backend
// without appending any fallback trace.
// ===========================================================================

TEST_CASE (



"capability_matrix: happy-path integer function routes to primary backend"
,
"[lithe][capability_matrix][scenario1]"
)
 {
    const auto fn = make_integer_add_fn();

    backend_variant primary  = debug_text_backend{};
    backend_variant fallback = interpreter_backend{};

    const compilation_artifact art = execute_with_fallback(fn, primary, fallback);

    SECTION("no fallback diagnostic injected") {
        REQUIRE_FALSE(diagnostics_contain(art, "falling back to interpreter"));
        REQUIRE_FALSE(diagnostics_contain(art, "fallback-reason"));
        REQUIRE_FALSE(diagnostics_contain(art, "execute_with_fallback"));
    }

    SECTION("primary emitted a non-empty text payload") {
        REQUIRE_FALSE(art.text_payload.empty());
    }
}

TEST_CASE (



"capability_matrix: verify_backend_legality passes for covered integer function"
,
"[lithe][capability_matrix][scenario1]"
)
 {
    const auto fn  = make_integer_add_fn();
    const auto caps = debug_text_backend::capabilities();
    REQUIRE(verify_backend_legality(fn, caps));
}

TEST_CASE (



"capability_matrix: verify_backend_legality passes fp function on full-caps backend"
,
"[lithe][capability_matrix][scenario1]"
)
 {
    const auto fn   = make_fp_add_fn();
    const auto caps = debug_text_backend::capabilities();
    REQUIRE(verify_backend_legality(fn, caps));
}

// ===========================================================================
// Scenario 2 — Cascading fallback routing
//
// null_backend carries no capability bits (empty set via if constexpr path).
// Any non-trivial MIR instruction fails validation → fallback to
// interpreter_backend.  The artifact must carry the mandatory trace strings.
// ===========================================================================

TEST_CASE (



"capability_matrix: integer function triggers fallback on zero-cap primary"
,
"[lithe][capability_matrix][scenario2]"
)
 {
    const auto fn = make_integer_add_fn();

    backend_variant primary  = null_backend{};
    backend_variant fallback = interpreter_backend{};

    const compilation_artifact art = execute_with_fallback(fn, primary, fallback);

    SECTION("fallback diagnostic injected") {
        REQUIRE(diagnostics_contain(art, "Primary backend invalid; falling back to interpreter"));
    }

    SECTION("at least one fallback-reason mismatch tag present") {
        REQUIRE(diagnostics_contain(art, "fallback-reason:"));
    }

    SECTION("execute_with_fallback stage tag present") {
        REQUIRE(diagnostics_contain(art, "execute_with_fallback"));
    }
}

TEST_CASE (



"capability_matrix: fp function triggers fallback on integer-only capability set"
,
"[lithe][capability_matrix][scenario2]"
)
 {
    const auto fn = make_fp_add_fn();

    // interpreter_backend only advertises integer_arithmetic + spill/memory/stack,
    // not floating_arithmetic — use its capability set as a narrow primary.
    const backend_capability_set narrow = interpreter_backend::capabilities();
    REQUIRE_FALSE(verify_backend_legality(fn, narrow));

    backend_variant primary  = null_backend{};   // zero caps → guaranteed fallback
    backend_variant fallback = interpreter_backend{};

    const compilation_artifact art = execute_with_fallback(fn, primary, fallback);
    REQUIRE(diagnostics_contain(art, "falling back to interpreter"));
}

TEST_CASE (



"capability_matrix: verify_backend_legality rejects integer function on empty caps"
,
"[lithe][capability_matrix][scenario2]"
)
 {
    const auto fn   = make_integer_add_fn();
    const auto caps = backend_capability_set{};   // empty — nothing supported
    REQUIRE_FALSE(verify_backend_legality(fn, caps));
}

TEST_CASE (



"capability_matrix: interpreter fallback correctly executes integer add"
,
"[lithe][capability_matrix][scenario2]"
)
 {
    // Confirm the interpreter can run the integer add so fallback is meaningful.
    const auto fn = make_integer_add_fn();

    interpreter_backend interp;
    interp.integer_registers[1] = 10;
    interp.integer_registers[2] = 32;

    const compilation_artifact ref = interp.emit(fn);
    REQUIRE(ref.metadata.count("return_value") > 0);
    REQUIRE(ref.metadata.at("return_value") == "42");

    // Now trigger the same execution via fallback path.
    backend_variant primary  = null_backend{};
    backend_variant fallback = interpreter_backend{};
    // Note: fallback_v is default-constructed; no pre-seeded registers.
    // We only verify the fallback trace fires — not the return value.
    const compilation_artifact art = execute_with_fallback(fn, primary, fallback);
    REQUIRE(diagnostics_contain(art, "falling back to interpreter"));
}

// ===========================================================================
// Scenario 3 — Native FFI marshalling invariants
//
// bind_native_function / marshal_to_native / invoke_bound must form a fully
// lossless round-trip for i64, f64, bool, and void* types.
// ===========================================================================

namespace {
    static constexpr std::int64_t native_add(std::int64_t a, std::int64_t b) noexcept {
        return a + b;
    }

    static constexpr std::int64_t native_sub(std::int64_t a, std::int64_t b) noexcept {
        return a - b;
    }

    static double native_mul_f64(double a, double b) noexcept {
        return a * b;
    }
} // namespace

TEST_CASE (



"capability_matrix: bind_native_function produces valid proxy for i64 add"
,
"[lithe][capability_matrix][scenario3][ffi]"
)
 {
    using namespace lithe::runtime::ffi::binding;

    const auto proxy = bind_native_function<native_add>();
    REQUIRE(proxy.has_value());
    REQUIRE(proxy->valid());
    REQUIRE(proxy->arity == 2);
}

TEST_CASE (



"capability_matrix: invoke_bound i64 add is lossless"
,
"[lithe][capability_matrix][scenario3][ffi]"
)
 {
    using namespace lithe::runtime::ffi::binding;

    const auto proxy = bind_native_function<native_add>();
    REQUIRE(proxy.has_value());

    std::array<std::int64_t, 2> regs{7LL, 35LL};
    const auto result = invoke_bound(*proxy, regs);

    REQUIRE(result.has_value());
    REQUIRE(*result == 42LL);
}

TEST_CASE (



"capability_matrix: invoke_bound i64 subtraction is lossless"
,
"[lithe][capability_matrix][scenario3][ffi]"
)
 {
    using namespace lithe::runtime::ffi::binding;

    const auto proxy = bind_native_function<native_sub>();
    REQUIRE(proxy.has_value());

    std::array<std::int64_t, 2> regs{100LL, 58LL};
    const auto result = invoke_bound(*proxy, regs);

    REQUIRE(result.has_value());
    REQUIRE(*result == 42LL);
}

TEST_CASE (



"capability_matrix: marshal_to_native / unmarshal_from_native i64 round-trip"
,
"[lithe][capability_matrix][scenario3][values]"
)
 {
    using namespace lithe::runtime::values;
    using lithe::runtime::ffi::type_hint_i64;

    const dynamic_value original = make_i64(1234567890LL);
    const std::int64_t word = marshal_to_native(original);
    const dynamic_value recovered = lithe::runtime::values::unmarshal_from_native(word, type_hint_i64);

    REQUIRE(is_i64(recovered));
    REQUIRE(as_i64(recovered) == 1234567890LL);
}

TEST_CASE (



"capability_matrix: marshal_to_native / unmarshal_from_native f64 bit-reinterpret round-trip"
,
"[lithe][capability_matrix][scenario3][values]"
)
 {
    using namespace lithe::runtime::values;
    using lithe::runtime::ffi::type_hint_f64;

    const double pi = 3.141592653589793;
    const dynamic_value original = make_f64(pi);
    const std::int64_t word = marshal_to_native(original);

    // Bit-exact: memcpy semantics, no narrowing.
    std::int64_t expected_bits = 0;
    std::memcpy(&expected_bits, &pi, sizeof(expected_bits));
    REQUIRE(word == expected_bits);

    const dynamic_value recovered = lithe::runtime::values::unmarshal_from_native(word, type_hint_f64);
    REQUIRE(is_f64(recovered));
    REQUIRE(as_f64(recovered) == pi);
}

TEST_CASE (



"capability_matrix: marshal_to_native / unmarshal_from_native bool round-trip"
,
"[lithe][capability_matrix][scenario3][values]"
)
 {
    using namespace lithe::runtime::values;
    using lithe::runtime::ffi::type_hint_bool;

    for (const bool b : {true, false}) {
        const dynamic_value original = make_bool(b);
        const std::int64_t word = marshal_to_native(original);
        const dynamic_value recovered = lithe::runtime::values::unmarshal_from_native(word, type_hint_bool);

        REQUIRE(is_bool(recovered));
        REQUIRE(as_bool(recovered) == b);
    }
}

TEST_CASE (



"capability_matrix: marshal_to_native / unmarshal_from_native void* round-trip"
,
"[lithe][capability_matrix][scenario3][values]"
)
 {
    using namespace lithe::runtime::values;
    using lithe::runtime::ffi::type_hint_ptr;

    int sentinel = 0xDEAD;
    void* const raw_ptr = &sentinel;

    const dynamic_value original = make_ptr(raw_ptr);
    const std::int64_t word = marshal_to_native(original);
    const dynamic_value recovered = lithe::runtime::values::unmarshal_from_native(word, type_hint_ptr);

    REQUIRE(is_ptr(recovered));
    REQUIRE(as_ptr(recovered) == raw_ptr);
}

TEST_CASE (



"capability_matrix: invoke_bound rejects insufficient register array"
,
"[lithe][capability_matrix][scenario3][ffi]"
)
 {
    using namespace lithe::runtime::ffi::binding;

    const auto proxy = bind_native_function<native_add>();
    REQUIRE(proxy.has_value());

    std::array<std::int64_t, 1> regs{10LL};
    const auto result = invoke_bound(*proxy, regs);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE (



"capability_matrix: bind_native_function f64 multiply proxy has correct arity"
,
"[lithe][capability_matrix][scenario3][ffi]"
)
 {
    using namespace lithe::runtime::ffi::binding;

    const auto proxy = bind_native_function<native_mul_f64>();
    REQUIRE(proxy.has_value());
    REQUIRE(proxy->valid());
    REQUIRE(proxy->arity == 2);
}

// ============================================================================
// Finding 12: execute_with_fallback correct fallback naming + incapable preflight
// ============================================================================

namespace {
    // Build a trivial physical_mir_function for use in fallback tests.
    lithe::codegen::mir::physical_mir_function make_trivial_fn_cap() {
        using namespace lithe::codegen;
        allocated_instruction li;
        li.id = 1;
        li.op = opcode::load_imm;
        li.defs = {allocated_operand::as_preg({0, "r0"})};
        li.uses = {allocated_operand::as_i64(42)};

        allocated_instruction ret_i;
        ret_i.id = 2;
        ret_i.op = opcode::ret;
        ret_i.uses = {allocated_operand::as_preg({0, "r0"})};

        allocated_function_ir fn;
        fn.name = "cap_test";
        fn.cfg.entry_block = 1;
        allocated_basic_block bb;
        bb.id = 1;
        bb.instructions = {li, ret_i};
        fn.blocks.push_back(std::move(bb));

        mir::physical_mir_function phys;
        phys.function = std::move(fn);
        phys.metadata.current_phase = mir::phase::physical_mir;
        return phys;
    }
} // namespace

TEST_CASE (


"execute_with_fallback: diagnostic names actual fallback backend"
,
"[lithe][capability_matrix]"
)
 {
    using namespace lithe::codegen::backends;

    const auto fn = make_trivial_fn_cap();

    // Use null_backend as primary (zero capabilities) + text_assembly as fallback.
    auto primary  = make_backend("null_backend");
    auto fallback = make_backend("text_assembly");
    REQUIRE(primary.has_value());
    REQUIRE(fallback.has_value());

    auto art = execute_with_fallback(fn, *primary, *fallback);

    // Diagnostics must mention the actual fallback name, not hardcoded "interpreter".
    bool found_text_assembly = false;
    for (const auto& d : art.diagnostics) {
        if (d.find("text_assembly") != std::string::npos) {
            found_text_assembly = true;
        }
        // Must NOT hardcode "interpreter" when fallback is text_assembly.
        REQUIRE(d.find("falling back to interpreter") == std::string::npos);
    }
    REQUIRE(found_text_assembly);
}

TEST_CASE (


"execute_with_fallback: incapable fallback appends diagnostic"
,
"[lithe][capability_matrix]"
)
 {
    using namespace lithe::codegen::backends;

    const auto fn = make_trivial_fn_cap();

    // Both null_backend → both incapable for a non-trivial fn.
    // Use two null_backends: primary and fallback both have zero capabilities.
    auto primary  = make_backend("null_backend");
    auto fallback = make_backend("null_backend");
    REQUIRE(primary.has_value());
    REQUIRE(fallback.has_value());

    auto art = execute_with_fallback(fn, *primary, *fallback);

    // A diagnostic noting the fallback is also incapable must appear.
    bool found_incapable = false;
    for (const auto& d : art.diagnostics) {
        if (d.find("also incapable") != std::string::npos) {
            found_incapable = true;
        }
    }
    REQUIRE(found_incapable);
}
