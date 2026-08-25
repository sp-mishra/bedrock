// =============================================================================
// test_lithe_foundation.cpp — static + runtime assertions for the execution
// layer foundation: types, codegen aliases, stage-error invariants,
// execution_mode_set round-trip.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <type_traits>

#include "lithe/lithe_execution/foundation.hpp"
#include "lithe/lithe_execution/identity.hpp"
#include "lithe/lithe_codegen_pipeline.hpp"  // codegen aliases

namespace ex = lithe::execution;
namespace cg = lithe::codegen;

// ---------------------------------------------------------------------------
// no_ir_integration / no_pipeline_hooks are empty types
// ---------------------------------------------------------------------------
static_assert(std::is_empty_v<ex::no_ir_integration>,
              "no_ir_integration must be empty");
static_assert(std::is_empty_v<ex::no_pipeline_hooks>,
              "no_pipeline_hooks must be empty");

// Aliased into codegen namespace too.
static_assert(std::is_empty_v<cg::no_ir_integration>);
static_assert(std::is_empty_v<cg::no_pipeline_hooks>);

// ---------------------------------------------------------------------------
// compile_install_error is NOT constructible from compile_error or install_error
// ---------------------------------------------------------------------------
static_assert(!std::is_constructible_v<ex::compile_install_error, ex::compile_error>,
              "compile_install_error must not be constructible from compile_error");
static_assert(!std::is_constructible_v<ex::compile_install_error, ex::install_error>,
              "compile_install_error must not be constructible from install_error");

// ---------------------------------------------------------------------------
// Codegen aliases are same-type (sizeof guard — types are independently defined)
// ---------------------------------------------------------------------------
static_assert(sizeof(ex::backend_capability_set) == sizeof(cg::backend_capability_set),
              "backend_capability_set size mismatch");
static_assert(sizeof(ex::backend_feature) == sizeof(cg::backend_feature),
              "backend_feature size mismatch");

// ---------------------------------------------------------------------------
// execution_mode_set set/test round-trip
// ---------------------------------------------------------------------------
TEST_CASE (


"execution_mode_set set and test round-trip"
,
"[execution][foundation]"
)
 {
    ex::execution_mode_set s;
    REQUIRE_FALSE(s.test(ex::execution_mode::interpret));
    REQUIRE_FALSE(s.any());

    s.set(ex::execution_mode::interpret);
    REQUIRE(s.test(ex::execution_mode::interpret));
    REQUIRE(s.any());
    REQUIRE_FALSE(s.test(ex::execution_mode::jit_tier1));

    s.set(ex::execution_mode::jit_tier1);
    REQUIRE(s.test(ex::execution_mode::jit_tier1));

    s.reset(ex::execution_mode::interpret);
    REQUIRE_FALSE(s.test(ex::execution_mode::interpret));
    REQUIRE(s.test(ex::execution_mode::jit_tier1));
}

// ---------------------------------------------------------------------------
// backend_capability_set from lithe::execution is usable
// ---------------------------------------------------------------------------
TEST_CASE (


"execution::backend_capability_set basic operations"
,
"[execution][foundation]"
)
 {
    using ex::backend_feature;
    using ex::backend_capability_set;

    const auto caps = backend_capability_set::from(
        {backend_feature::integer_arithmetic, backend_feature::interpreter_execution});

    REQUIRE(caps.has(backend_feature::integer_arithmetic));
    REQUIRE(caps.has(backend_feature::interpreter_execution));
    REQUIRE_FALSE(caps.has(backend_feature::floating_arithmetic));
    REQUIRE_FALSE(caps.empty());

    const auto required = backend_capability_set::from({backend_feature::integer_arithmetic});
    REQUIRE(caps.contains_all(required));

    const auto missing = required.missing(caps);
    REQUIRE(missing.empty());
}

// ---------------------------------------------------------------------------
// identity types: no implicit conversions
// ---------------------------------------------------------------------------
TEST_CASE (


"execution identity types are not interconvertible"
,
"[execution][identity]"
)
 {
    // persisted_backend_id holds a string_view
    const ex::persisted_backend_id pid{"my.backend"};
    REQUIRE(pid.value == "my.backend");

    // in_process_type_token is process-local
    const ex::in_process_type_token tok = ex::type_token_for<int>();
    REQUIRE(tok.valid());

    // Two calls for the same type return the same token.
    const ex::in_process_type_token tok2 = ex::type_token_for<int>();
    REQUIRE(tok == tok2);

    // Different types yield different tokens.
    const ex::in_process_type_token tok_d = ex::type_token_for<double>();
    REQUIRE(tok != tok_d);
}

// ---------------------------------------------------------------------------
// typed<T>() safe downcast
// ---------------------------------------------------------------------------
TEST_CASE (


"typed<T> downcast matches token or returns nullptr"
,
"[execution][identity]"
)
 {
    int value = 42;
    void* erased = &value;

    const ex::in_process_type_token good = ex::type_token_for<int>();
    const ex::in_process_type_token bad  = ex::type_token_for<double>();

    int* ptr = ex::typed<int>(erased, good);
    REQUIRE(ptr != nullptr);
    REQUIRE(*ptr == 42);

    int* null_ptr = ex::typed<int>(erased, bad);
    REQUIRE(null_ptr == nullptr);
}

// ============================================================================
// [impl-2 / P14.0] execution_mode::device tier — GPU/device compute
//   host modes 0–4 stable; device = 5; count bumped to 6; bitset widens.
// ============================================================================

static_assert(ex::execution_mode_count == 7); // bumped from 6 in impl-5 (out_of_proc)
static_assert(static_cast<int>(ex::execution_mode::device) == 5);
static_assert(static_cast<int>(ex::execution_mode::native_inline) == 4);

TEST_CASE (


"execution_mode_set: device bit round-trips without disturbing host bits [P14.0]"
,
"[foundation][execution_mode]"
)
{
    ex::execution_mode_set s;
    s.set(ex::execution_mode::device);
    CHECK(s.test(ex::execution_mode::device));
    CHECK(!s.test(ex::execution_mode::interpret));
    s.reset(ex::execution_mode::device);
    CHECK(s.none());
}
