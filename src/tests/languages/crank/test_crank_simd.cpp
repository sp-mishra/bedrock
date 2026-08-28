// =============================================================================
// test_crank_simd.cpp — §v2.8 real Highway SIMD backend + kernels.
//
// Verifies:
//   1.  simd_kernels::add is correct including the scalar tail (odd length).
//   2.  simd_kernels::mul elementwise product matches scalar.
//   3.  simd_kernels::axpy computes alpha*x + y.
//   4.  simd_kernels::reduce_sum matches a scalar accumulation.
//   5.  float_lanes() reports a plausible (>=1) native lane count.
//   6.  simd backend is registered + constructible via the registry.
//   7.  simd_backend::emit runs MIR and annotates lane width.
//   8.  crank::select_backend_name maps affinity → backend string.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "lithe/backends/lithe_codegen_simd.hpp"
#include "lithe/backends/lithe_codegen_backend_registry.hpp"
#include "languages/crank/execute.hpp"

#include <numeric>
#include <vector>

using namespace lithe::codegen;
using namespace lithe::codegen::backends;

TEST_CASE (

"simd add handles scalar tail (odd length)"
,
"[crank][simd][v2]"
)
 {
    // 13 is not a multiple of any SIMD lane count → exercises the tail loop.
    std::vector<float> a(13), b(13), out(13, 0.0f);
    for (int i = 0; i < 13; ++i) { a[i] = float(i); b[i] = float(2 * i); }

    simd_kernels::add(a, b, out);
    for (int i = 0; i < 13; ++i) {
        CHECK(out[i] == Catch::Approx(float(3 * i)));
    }
}

TEST_CASE (

"simd mul elementwise product"
,
"[crank][simd][v2]"
)
 {
    std::vector<float> a(9), b(9), out(9, 0.0f);
    for (int i = 0; i < 9; ++i) { a[i] = float(i + 1); b[i] = 3.0f; }

    simd_kernels::mul(a, b, out);
    for (int i = 0; i < 9; ++i) {
        CHECK(out[i] == Catch::Approx(float((i + 1) * 3)));
    }
}

TEST_CASE (

"simd axpy computes alpha*x + y"
,
"[crank][simd][v2]"
)
 {
    std::vector<float> x(17), y(17), out(17, 0.0f);
    for (int i = 0; i < 17; ++i) { x[i] = float(i); y[i] = 1.0f; }

    simd_kernels::axpy(2.0f, x, y, out);
    for (int i = 0; i < 17; ++i) {
        CHECK(out[i] == Catch::Approx(2.0f * float(i) + 1.0f));
    }
}

TEST_CASE (

"simd reduce_sum matches scalar accumulation"
,
"[crank][simd][v2]"
)
 {
    std::vector<float> a(101);
    std::iota(a.begin(), a.end(), 1.0f); // 1..101

    const float scalar = std::accumulate(a.begin(), a.end(), 0.0f);
    CHECK(simd_kernels::reduce_sum(a) == Catch::Approx(scalar));
}

TEST_CASE (

"simd reports a plausible native lane count"
,
"[crank][simd][v2]"
)
 {
    CHECK(simd_kernels::float_lanes() >= 1u);
    CHECK(simd_kernels::double_lanes() >= 1u);
}

TEST_CASE (

"simd backend is registered in the registry"
,
"[crank][simd][v2]"
)
 {
    const auto kind = backend_kind_from_string("simd");
    REQUIRE(kind.has_value());
    CHECK(*kind == backend_kind::simd);

    auto backends = list_available_backends();
    bool found = false;
    for (auto n : backends) if (n == "simd") found = true;
    CHECK(found);

    auto var = make_backend(backend_kind::simd);
    CHECK(std::holds_alternative<simd_backend>(var));
}

TEST_CASE (

"simd backend emit annotates lane width"
,
"[crank][simd][v2]"
)
 {
    // Trivial physical MIR: load_imm r0=5; ret r0.
    allocated_function_ir fn;
    fn.name = "simd_trivial";
    fn.cfg.entry_block = 1;

    allocated_basic_block bb;
    bb.id = 1;
    {
        allocated_instruction li;
        li.id = 1;
        li.op = opcode::load_imm;
        li.defs = {allocated_operand::as_preg({0, "r0"})};
        li.uses = {allocated_operand::as_i64(5)};
        bb.instructions.push_back(li);

        allocated_instruction ret;
        ret.id = 2;
        ret.op = opcode::ret;
        ret.uses = {allocated_operand::as_preg({0, "r0"})};
        bb.instructions.push_back(ret);
    }
    fn.blocks.push_back(std::move(bb));

    mir::physical_mir_function phys;
    phys.function = std::move(fn);
    phys.metadata.current_phase = mir::phase::physical_mir;

    simd_backend simd;
    auto art = simd.emit(phys);

    CHECK(art.metadata.at("backend") == "simd");
    CHECK(art.metadata.count("simd_float_lanes") == 1u);
    REQUIRE(art.metadata.count("return_value") == 1u);
    CHECK(art.metadata.at("return_value") == "5");
}

TEST_CASE (

"select_backend_name maps affinity to backend string"
,
"[crank][simd][v2]"
)
 {
    CHECK(crank::select_backend_name(crank::exec_affinity::simd) == "simd");
    CHECK(crank::select_backend_name(crank::exec_affinity::gpu) == "gpu");
    // cpu → "asmjit" when LITHE_HAS_ASMJIT is defined (perf-L1 §Include/2), else "interpreter".
#if defined(LITHE_HAS_ASMJIT)
    CHECK (crank::select_backend_name(crank::exec_affinity::cpu)== "asmjit");
#else
    CHECK(crank::select_backend_name(crank::exec_affinity::cpu) == "interpreter");
#endif
}
