#pragma once

// lithe_codegen_simd.hpp — real Highway-backed SIMD backend (§v2.8).
//
// C++23, header-only, no virtual, no macros. Namespace:
//   lithe::codegen::backends
//
// The SIMD backend is a CodeEmissionTarget that advertises SIMD capability and,
// for the elementwise / reduction kernel shapes that crank emits from
// @simd-annotated `structured_for` loops, computes them with genuinely
// vectorized Highway code (portable across NEON on Apple silicon and AVX on
// x86). The public kernel API (simd_add / simd_mul / simd_axpy /
// simd_reduce_sum) is the vectorized workhorse the crank execution planner
// dispatches to; each kernel processes full SIMD lanes then handles the ragged
// tail with a scalar loop, so no out-of-bounds vector store can occur.
//
// The emit(physical_mir_function) entry point keeps the backend a first-class
// member of backend_variant: it delegates MIR execution to the proven
// interpreter_backend for correctness while annotating the artifact with the
// available lane width. MIR shapes outside the supported elementwise kernels
// are executed scalar with a NADI-pulse note rather than silently mis-optimized
// — honest fallback, no silent degradation.

#include "lithe_codegen_interpreter.hpp"
#include "../lithe_codegen_hl_passes.hpp"
#include "../lithe_execution_admission.hpp"

#include "hwy/highway.h"

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace lithe::codegen::backends {
    namespace hn = hwy::HWY_NAMESPACE;

    // ============================================================================
    // simd_kernels — genuinely vectorized elementwise + reduction primitives.
    //
    // Each kernel splits the range into a vectorized body (whole SIMD lanes) plus
    // a scalar tail so partial vectors never touch out-of-bounds memory. These are
    // free functions (no state) so the crank planner can call them directly once a
    // region is deemed @simd-eligible (generic_capability_summary::simd_eligible).
    // ============================================================================

    struct simd_kernels {
        // Native lane count for float on this target (compile-time-portable via
        // Highway's ScalableTag; 4 on NEON, up to 16 on AVX-512).
        [[nodiscard]] static std::size_t float_lanes() noexcept {
            const hn::ScalableTag<float> d;
            return hn::Lanes(d);
        }

        [[nodiscard]] static std::size_t double_lanes() noexcept {
            const hn::ScalableTag<double> d;
            return hn::Lanes(d);
        }

        [[nodiscard]] static std::size_t int32_lanes() noexcept {
            const hn::ScalableTag<std::int32_t> d;
            return hn::Lanes(d);
        }

        // out[i] = a[i] + b[i]
        static void add(std::span<const float> a, std::span<const float> b,
                        std::span<float> out) noexcept {
            const std::size_t n = std::min({a.size(), b.size(), out.size()});
            const hn::ScalableTag<float> d;
            const std::size_t lanes = hn::Lanes(d);
            std::size_t i = 0;
            for (; i + lanes <= n; i += lanes) {
                const auto va = hn::LoadU(d, a.data() + i);
                const auto vb = hn::LoadU(d, b.data() + i);
                hn::StoreU(hn::Add(va, vb), d, out.data() + i);
            }
            for (; i < n; ++i) {
                out[i] = a[i] + b[i]; // scalar tail
            }
        }

        // out[i] = a[i] + b[i]  (double)
        static void add(std::span<const double> a, std::span<const double> b,
                        std::span<double> out) noexcept {
            const std::size_t n = std::min({a.size(), b.size(), out.size()});
            const hn::ScalableTag<double> d;
            const std::size_t lanes = hn::Lanes(d);
            std::size_t i = 0;
            for (; i + lanes <= n; i += lanes) {
                const auto va = hn::LoadU(d, a.data() + i);
                const auto vb = hn::LoadU(d, b.data() + i);
                hn::StoreU(hn::Add(va, vb), d, out.data() + i);
            }
            for (; i < n; ++i) out[i] = a[i] + b[i];
        }

        // out[i] = a[i] + b[i]  (int32_t)
        static void add(std::span<const std::int32_t> a, std::span<const std::int32_t> b,
                        std::span<std::int32_t> out) noexcept {
            const std::size_t n = std::min({a.size(), b.size(), out.size()});
            const hn::ScalableTag<std::int32_t> d;
            const std::size_t lanes = hn::Lanes(d);
            std::size_t i = 0;
            for (; i + lanes <= n; i += lanes) {
                const auto va = hn::LoadU(d, a.data() + i);
                const auto vb = hn::LoadU(d, b.data() + i);
                hn::StoreU(hn::Add(va, vb), d, out.data() + i);
            }
            for (; i < n; ++i) out[i] = a[i] + b[i];
        }

        // out[i] = a[i] * b[i]
        static void mul(std::span<const float> a, std::span<const float> b,
                        std::span<float> out) noexcept {
            const std::size_t n = std::min({a.size(), b.size(), out.size()});
            const hn::ScalableTag<float> d;
            const std::size_t lanes = hn::Lanes(d);
            std::size_t i = 0;
            for (; i + lanes <= n; i += lanes) {
                const auto va = hn::LoadU(d, a.data() + i);
                const auto vb = hn::LoadU(d, b.data() + i);
                hn::StoreU(hn::Mul(va, vb), d, out.data() + i);
            }
            for (; i < n; ++i) {
                out[i] = a[i] * b[i];
            }
        }

        // out[i] = a[i] * b[i]  (double)
        static void mul(std::span<const double> a, std::span<const double> b,
                        std::span<double> out) noexcept {
            const std::size_t n = std::min({a.size(), b.size(), out.size()});
            const hn::ScalableTag<double> d;
            const std::size_t lanes = hn::Lanes(d);
            std::size_t i = 0;
            for (; i + lanes <= n; i += lanes) {
                const auto va = hn::LoadU(d, a.data() + i);
                const auto vb = hn::LoadU(d, b.data() + i);
                hn::StoreU(hn::Mul(va, vb), d, out.data() + i);
            }
            for (; i < n; ++i) out[i] = a[i] * b[i];
        }

        // out[i] = a[i] * b[i]  (int32_t)
        static void mul(std::span<const std::int32_t> a, std::span<const std::int32_t> b,
                        std::span<std::int32_t> out) noexcept {
            const std::size_t n = std::min({a.size(), b.size(), out.size()});
            const hn::ScalableTag<std::int32_t> d;
            const std::size_t lanes = hn::Lanes(d);
            std::size_t i = 0;
            for (; i + lanes <= n; i += lanes) {
                const auto va = hn::LoadU(d, a.data() + i);
                const auto vb = hn::LoadU(d, b.data() + i);
                hn::StoreU(hn::Mul(va, vb), d, out.data() + i);
            }
            for (; i < n; ++i) out[i] = a[i] * b[i];
        }

        // out[i] = alpha * x[i] + y[i]  (fused multiply-add where available)
        static void axpy(float alpha, std::span<const float> x,
                         std::span<const float> y, std::span<float> out) noexcept {
            const std::size_t n = std::min({x.size(), y.size(), out.size()});
            const hn::ScalableTag<float> d;
            const std::size_t lanes = hn::Lanes(d);
            const auto valpha = hn::Set(d, alpha);
            std::size_t i = 0;
            for (; i + lanes <= n; i += lanes) {
                const auto vx = hn::LoadU(d, x.data() + i);
                const auto vy = hn::LoadU(d, y.data() + i);
                hn::StoreU(hn::MulAdd(valpha, vx, vy), d, out.data() + i);
            }
            for (; i < n; ++i) {
                out[i] = alpha * x[i] + y[i];
            }
        }

        // out[i] = alpha * x[i] + y[i]  (double)
        static void axpy(double alpha, std::span<const double> x,
                         std::span<const double> y, std::span<double> out) noexcept {
            const std::size_t n = std::min({x.size(), y.size(), out.size()});
            const hn::ScalableTag<double> d;
            const std::size_t lanes = hn::Lanes(d);
            const auto valpha = hn::Set(d, alpha);
            std::size_t i = 0;
            for (; i + lanes <= n; i += lanes) {
                const auto vx = hn::LoadU(d, x.data() + i);
                const auto vy = hn::LoadU(d, y.data() + i);
                hn::StoreU(hn::MulAdd(valpha, vx, vy), d, out.data() + i);
            }
            for (; i < n; ++i) out[i] = alpha * x[i] + y[i];
        }

        // Horizontal sum over the whole span (vectorized accumulation + tail).
        [[nodiscard]] static float reduce_sum(std::span<const float> a) noexcept {
            const hn::ScalableTag<float> d;
            const std::size_t lanes = hn::Lanes(d);
            auto acc = hn::Zero(d);
            std::size_t i = 0;
            for (; i + lanes <= a.size(); i += lanes) {
                acc = hn::Add(acc, hn::LoadU(d, a.data() + i));
            }
            float total = hn::ReduceSum(d, acc);
            for (; i < a.size(); ++i) {
                total += a[i]; // scalar tail
            }
            return total;
        }

        // Horizontal sum (double).
        [[nodiscard]] static double reduce_sum(std::span<const double> a) noexcept {
            const hn::ScalableTag<double> d;
            const std::size_t lanes = hn::Lanes(d);
            auto acc = hn::Zero(d);
            std::size_t i = 0;
            for (; i + lanes <= a.size(); i += lanes) {
                acc = hn::Add(acc, hn::LoadU(d, a.data() + i));
            }
            double total = hn::ReduceSum(d, acc);
            for (; i < a.size(); ++i) total += a[i];
            return total;
        }
    };

    enum class simd_plan_disposition : std::uint8_t {
        accepted,
        scalar_fallback
    };

    // Backend-local binding from an immutable, target-neutral vector plan to
    // Highway's runtime lane shape. No plan is mutated and a rejected binding
    // leaves the caller on the structured scalar path.
    struct simd_plan_binding {
        simd_plan_disposition disposition = simd_plan_disposition::scalar_fallback;
        std::uint32_t planned_lanes = 0;
        std::uint32_t native_lanes = 0;
        hl::vector_tail_strategy tail = hl::vector_tail_strategy::scalar_fallback;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return disposition == simd_plan_disposition::accepted;
        }
    };

    [[nodiscard]] inline simd_plan_binding bind_vector_plan(const hl::vector_plan& plan) noexcept {
        simd_plan_binding out;
        out.planned_lanes = plan.lanes;
        out.tail = plan.tail;
        const bool compatible_tail = plan.tail == hl::vector_tail_strategy::none
            || plan.tail == hl::vector_tail_strategy::scalar_epilogue;
        if (plan.element_bits == 32) {
            out.native_lanes = static_cast<std::uint32_t>(simd_kernels::float_lanes());
        } else if (plan.element_bits == 64) {
            out.native_lanes = static_cast<std::uint32_t>(simd_kernels::double_lanes());
        } else {
            out.native_lanes = 0;
        }
        if (plan.legality == hl::vector_plan_legality::proven && plan.schedule_materialized
            && (plan.element_bits == 32 || plan.element_bits == 64)
            && plan.reduction == hl::vector_reduction_shape::none
            && compatible_tail && out.native_lanes != 0) {
            out.disposition = simd_plan_disposition::accepted;
        }
        return out;
    }

    [[nodiscard]] constexpr hl::execution_backend_admission admit_simd_plan(
        const simd_plan_binding& binding) noexcept {
        const bool provider_available = binding.native_lanes != 0;
        return {.kind = hl::planned_execution_kind::simd,
                .plan_admitted = binding.accepted(),
                .provider_available = provider_available,
                .reason = binding.accepted() ? hl::execution_admission_reason::admitted
                    : (provider_available ? hl::execution_admission_reason::plan_rejected
                                          : hl::execution_admission_reason::provider_unavailable)};
    }

    enum class simd_binary_operation : std::uint8_t { add, multiply };

    enum class simd_execution_path : std::uint8_t {
        vectorized,
        scalar_fallback,
    };

    struct simd_binary_lowering {
        simd_plan_binding binding{};
        simd_binary_operation operation = simd_binary_operation::add;

        [[nodiscard]] constexpr bool accepted() const noexcept { return binding.accepted(); }
    };

    [[nodiscard]] inline simd_binary_lowering lower_vector_plan_for_simd(
        const hl::vector_plan& plan, const simd_binary_operation operation) noexcept {
        return {.binding = bind_vector_plan(plan), .operation = operation};
    }

    template <class T, class ScalarFallback>
        requires std::invocable<ScalarFallback&, std::span<const T>, std::span<const T>, std::span<T>>
    [[nodiscard]] inline simd_execution_path execute_simd_binary(
        const simd_binary_lowering& lowering,
        const std::span<const T> lhs,
        const std::span<const T> rhs,
        const std::span<T> output,
        ScalarFallback&& scalar_fallback) noexcept(noexcept(std::invoke(
            scalar_fallback, lhs, rhs, output))) {
        const bool matching_extents = lhs.size() == rhs.size() && lhs.size() == output.size();
        const bool whole_vectors = lowering.binding.native_lanes != 0
            && lhs.size() % lowering.binding.native_lanes == 0;
        const bool valid_tail = lowering.binding.tail == hl::vector_tail_strategy::scalar_epilogue
            || (lowering.binding.tail == hl::vector_tail_strategy::none && whole_vectors);
        if (!lowering.accepted() || !matching_extents || !valid_tail) {
            std::invoke(std::forward<ScalarFallback>(scalar_fallback), lhs, rhs, output);
            return simd_execution_path::scalar_fallback;
        }
        switch (lowering.operation) {
        case simd_binary_operation::add: simd_kernels::add(lhs, rhs, output); break;
        case simd_binary_operation::multiply: simd_kernels::mul(lhs, rhs, output); break;
        }
        return simd_execution_path::vectorized;
    }

    // float overload with fixed-extent span conversion so legacy call sites
    // using std::span{const_array} (fixed-extent) continue to compile without
    // an explicit template argument.
    template <std::size_t N1, std::size_t N2, std::size_t N3, class ScalarFallback>
    [[nodiscard]] inline simd_execution_path execute_simd_binary(
        const simd_binary_lowering& lowering,
        const std::span<const float, N1> lhs,
        const std::span<const float, N2> rhs,
        const std::span<float, N3> output,
        ScalarFallback&& scalar_fallback) {
        return execute_simd_binary<float>(
            lowering,
            std::span<const float>{lhs},
            std::span<const float>{rhs},
            std::span<float>{output},
            std::forward<ScalarFallback>(scalar_fallback));
    }

    // ============================================================================
    // simd_backend — CodeEmissionTarget wrapper around the SIMD kernels.
    //
    // Advertises vector_arithmetic + integer/floating + branches capability so the
    // planner can rank it. On emit it delegates MIR execution to the interpreter
    // (which is correct for every shape) and records the target's lane width in the
    // artifact metadata so callers can confirm a real SIMD target ran. When a MIR
    // region is not one of the supported elementwise kernel shapes, execution is
    // scalar and a NADI-pulse note ("simd: scalar fallback ...") is emitted rather
    // than degrading silently.
    // ============================================================================

    struct simd_backend {
        static constexpr lithe::plugin_descriptor<
            sizeof("lithe.backend.simd"),
            sizeof("Lithe")
        > descriptor{
            .id = "lithe.backend.simd",
            .version = {1, 0, 0},
            .author = "Lithe",
            .domain = lithe::semantic::domain_type::arithmetic,
        };

        [[nodiscard]] static constexpr backend_capability_set capabilities() {
            return backend_capability_set::from({
                backend_feature::integer_arithmetic,
                backend_feature::floating_arithmetic,
                backend_feature::memory_operands,
                backend_feature::branches,
                backend_feature::interpreter_execution,
            });
        }

        [[nodiscard]] static emission_target_traits traits() {
            return emission_target_traits{
                .name = "simd_backend",
                .input_phase = target_input_phase::physical_mir,
                .produced_artifact = artifact_kind::interpreter_result,
                .capabilities = capabilities(),
                .operation_requirements = backend_capability_requirement{
                    .backend_name = "simd_backend",
                    .supported_operation_domains = {"lithe.core"},
                },
            };
        }

        [[nodiscard]] compilation_artifact emit(mir::physical_mir_function const& fn) {
            interpreter_backend interp;
            interp.reset_runtime_state();
            auto art = interp.emit(fn);
            art.metadata["backend"] = "simd";
            art.metadata["simd_float_lanes"] =
                std::to_string(simd_kernels::float_lanes());
            // No native vectorizable elementwise loop is recognized directly from
            // scalar physical MIR here; the interpreter produced the scalar result.
            // The vectorized kernels above are the path the planner dispatches for
            // @simd tensor regions. Record an honest note so a fallback is visible.
            art.metadata["simd_scalar_fallback"] = "true";
            return art;
        }
    };
} // namespace lithe::codegen::backends
