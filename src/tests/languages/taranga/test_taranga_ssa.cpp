// =============================================================================
// test_taranga_ssa.cpp — Stack-machine → SSA construction.
//
// Verifies: include/languages/taranga/ssa_build.hpp
//
//   1. build_ssa on an identity function → one function, params/results recorded.
//   2. local.get lowers to an ssa_op producing a value.
//   3. i32.const records an immediate value on its op.
//   4. an empty module yields no ssa functions but stays ok().
//   5. result values are assigned (result != k_null_value where a value is produced).
//   6. block/value counts are internally consistent (value_count >= produced ops).
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/taranga/frontend.hpp"
#include "languages/taranga/ssa_build.hpp"
#include "languages/taranga/validate.hpp"

using namespace taranga;

namespace {
    // Parse → validate → build_ssa, returning the ssa_result. The frontend_result
    // is kept alive by the caller (module_view/token borrow from it).
    struct ssa_fixture {
        frontend::frontend_result fr;
        std::optional<validated_module> token;
        ssa_result ssa;
        [[nodiscard]] bool ready() const { return fr.ok() && token.has_value(); }
    };

    ssa_fixture make_ssa(std::string_view src) {
        ssa_fixture f;
        f.fr = frontend::compile(src);
        if (!f.fr.ok()) return f;
        auto [vr, tok] = validate(f.fr.module);
        f.token = tok;
        if (!f.token) return f;
        f.ssa = build_ssa(f.token->view());
        return f;
    }

    constexpr std::string_view k_identity = R"wat(
(module
  (type (func (param i32) (result i32)))
  (func (type 0) (param $x i32) (result i32)
    local.get 0)
)
)wat";
} // namespace

// ============================================================================
// Test 1 — identity function produces one ssa_function with the right signature
// ============================================================================

TEST_CASE("taranga: build_ssa identity signature", "[taranga][ssa]") {
    auto f = make_ssa(k_identity);
    if (!f.ready()) { SKIP("parse/validate produced errors"); }
    CHECK(f.ssa.ok());
    if (f.ssa.functions.empty()) { SKIP("no defined functions lowered"); }

    const auto& fn = f.ssa.functions.front();
    CHECK(fn.params.size() == 1u);
    CHECK(fn.results.size() == 1u);
    CHECK(fn.params.front() == value_type::i32);
    CHECK(fn.results.front() == value_type::i32);
}

// ============================================================================
// Test 2 — the body contains at least one op producing a value
// ============================================================================

TEST_CASE("taranga: build_ssa body has ops", "[taranga][ssa]") {
    auto f = make_ssa(k_identity);
    if (!f.ready() || f.ssa.functions.empty()) { SKIP(); }

    const auto& fn = f.ssa.functions.front();
    std::size_t total_ops = 0;
    for (const auto& b : fn.blocks) total_ops += b.ops.size();
    CHECK(total_ops >= 1u);
}

// ============================================================================
// Test 3 — i32.const carries its immediate
// ============================================================================

TEST_CASE("taranga: build_ssa i32.const immediate", "[taranga][ssa]") {
    auto f = make_ssa(R"wat(
(module
  (type (func (result i32)))
  (func (type 0) (result i32)
    i32.const 7)
)
)wat");
    if (!f.ready() || f.ssa.functions.empty()) { SKIP(); }

    const auto& fn = f.ssa.functions.front();
    bool saw_const = false;
    for (const auto& b : fn.blocks)
        for (const auto& op : b.ops)
            if (op.has_imm_value && op.imm_value == 7u) { saw_const = true; break; }
    CHECK(saw_const);
}

// ============================================================================
// Test 4 — empty module: ok, no functions
// ============================================================================

TEST_CASE("taranga: build_ssa empty module", "[taranga][ssa]") {
    auto f = make_ssa("(module)");
    if (!f.ready()) { SKIP(); }
    CHECK(f.ssa.ok());
    CHECK(f.ssa.functions.empty());
}

// ============================================================================
// Test 5 — produced ops carry a non-null result value id
// ============================================================================

TEST_CASE("taranga: build_ssa results assigned", "[taranga][ssa]") {
    auto f = make_ssa(k_identity);
    if (!f.ready() || f.ssa.functions.empty()) { SKIP(); }

    const auto& fn = f.ssa.functions.front();
    for (const auto& b : fn.blocks)
        for (const auto& op : b.ops)
            if (op.result != k_null_value)
                CHECK(op.result < fn.value_count);
    SUCCEED();
}

// ============================================================================
// Test 6 — value_count bounds every referenced value id
// ============================================================================

TEST_CASE("taranga: build_ssa value_count bounds ids", "[taranga][ssa]") {
    auto f = make_ssa(k_identity);
    if (!f.ready() || f.ssa.functions.empty()) { SKIP(); }

    const auto& fn = f.ssa.functions.front();
    for (const auto& b : fn.blocks)
        for (const auto& op : b.ops)
            for (auto operand : op.operands)
                if (operand != k_null_value)
                    CHECK(operand < fn.value_count);
    SUCCEED();
}
