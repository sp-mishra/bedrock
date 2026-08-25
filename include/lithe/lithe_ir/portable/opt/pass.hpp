#pragma once

// =============================================================================
// lithe_ir/portable/opt/pass.hpp — portable pass contract (arch §4.4)
//
// Namespace: lithe::ir::portable::opt
//
// Defines the portable pass descriptor contract, semantic policy, analysis
// identity types, and the portable_pass / analysis_provider concepts.
//
// Distinct from lithe_passes.hpp (AST-level pass metadata) and
// lithe_algorithms/pipeline.hpp (AST analysis_id/analysis_manager).
// This layer operates on portable_module (wire form, ISA-agnostic).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "../../../lithe_diagnostics.hpp"  // lithe::diag::diagnostic, severity, stage
#include "../module.hpp"                  // portable_module

namespace lithe::ir::portable::opt {
    // =============================================================================
    // pass_id — stable identifier for each portable pass
    // =============================================================================

    enum class pass_id : std::uint16_t {
        canonicalize = 0x0001,
        cfg_simplify = 0x0002,
        sccp = 0x0003,
        dce = 0x0004,
        pure_cse = 0x0005,
        check_elim = 0x0006,
        tail_call_form = 0x0007,
        inline_pure = 0x0008,
    };

    // =============================================================================
    // pass_version — major.minor version for provenance
    // =============================================================================

    struct pass_version {
        std::uint16_t major = 1;
        std::uint16_t minor = 0;

        [[nodiscard]] constexpr bool operator==(const pass_version&) const noexcept = default;
    };

    // =============================================================================
    // analysis_id — portable-IR-scoped analysis identities
    //
    // Distinct from lithe_algorithms/pipeline.hpp::analysis_id (AST-scoped).
    // =============================================================================

    enum class analysis_id : std::uint16_t {
        dominance = 0x0001,
        liveness = 0x0002,
        effects = 0x0003,
        purity = 0x0004,
        ranges = 0x0005,
        aliasing = 0x0006,
        cfg_reachability = 0x0007,
    };

    // Bitset mask over analysis_id values (bit N = 1 << (analysis_id - 1))
    using analysis_mask = std::uint32_t;

    namespace detail {
        [[nodiscard]] constexpr analysis_mask mask_of(analysis_id id) noexcept {
            return 1u << (static_cast<std::uint16_t>(id) - 1u);
        }
    } // namespace detail

    inline constexpr analysis_mask mask_dominance = detail::mask_of(analysis_id::dominance);
    inline constexpr analysis_mask mask_liveness = detail::mask_of(analysis_id::liveness);
    inline constexpr analysis_mask mask_effects = detail::mask_of(analysis_id::effects);
    inline constexpr analysis_mask mask_purity = detail::mask_of(analysis_id::purity);
    inline constexpr analysis_mask mask_ranges = detail::mask_of(analysis_id::ranges);
    inline constexpr analysis_mask mask_aliasing = detail::mask_of(analysis_id::aliasing);
    inline constexpr analysis_mask mask_cfg_reachability = detail::mask_of(analysis_id::cfg_reachability);

    // =============================================================================
    // determinism_class — pass-level output determinism
    //
    // Only deterministic and deterministic_within_policy are legal in a
    // deterministic pipeline.
    // =============================================================================

    enum class determinism_class : std::uint8_t {
        deterministic = 0, // identical bytes for identical IR+policy
        deterministic_within_policy = 1, // identical given same semantic_policy
        nondeterministic = 2, // illegal in deterministic pipelines
    };

    // =============================================================================
    // semantic_policy — conditions legality of policy-sensitive passes (arch §4.1/4.3/4.4)
    //
    // Conservative defaults: trap on overflow, strict FP, preserve everything.
    // An unspecified policy never weakens defined behavior (arch §4.3).
    // =============================================================================

    enum class integer_overflow_mode : std::uint8_t {
        trap = 0, // defined overflow → trap (no fold that hides trap)
        wrapping = 1, // two's complement wrap
        undef = 2, // C-style UB (permits aggressive algebraic rewrites)
    };

    enum class fp_mode : std::uint8_t {
        strict = 0, // IEEE-754 strict; no reassociation, NaN/sign-sensitive
        fast = 1, // permit reassociation, ignore NaN semantics
    };

    enum class determinism_requirement : std::uint8_t {
        bitwise = 0, // bit-for-bit identical across runs
        semantic = 1, // observationally equivalent but not byte-identical
    };

    // Bitset describing which semantic_policy modes a pass REQUIRES to operate.
    // 0 = no requirements (compatible with all policies).
    // Set bits indicate the pass requires a specific non-conservative mode.
    using semantic_policy_mask = std::uint32_t;

    inline constexpr semantic_policy_mask policy_compat_all = 0x0000'0000u;
    // no requirements — compatible with all policies
    inline constexpr semantic_policy_mask policy_compat_wrapping_ok = 0x0000'0001u;
    inline constexpr semantic_policy_mask policy_compat_fast_fp_ok = 0x0000'0002u;
    inline constexpr semantic_policy_mask policy_compat_undef_ok = 0x0000'0004u;

    struct semantic_policy {
        integer_overflow_mode int_overflow = integer_overflow_mode::trap;
        fp_mode fp = fp_mode::strict;
        bool preserve_defer = true;
        bool preserve_exceptions = true;
        bool preserve_transactions = true;
        bool preserve_traps = true;
        determinism_requirement determinism = determinism_requirement::bitwise;
        bool paranoid = false; // re-verify after pipeline end

        [[nodiscard]] constexpr bool operator==(const semantic_policy&) const noexcept = default;
    };

    // =============================================================================
    // pass_descriptor — portable pass contract fields (arch §4.4)
    // =============================================================================

    struct pass_descriptor {
        pass_id id;
        pass_version version;
        analysis_mask requires_; // analyses this pass reads
        analysis_mask preserves; // analyses this pass guarantees remain valid
        analysis_mask invalidates; // analyses this pass may corrupt
        semantic_policy_mask policy; // policy modes this pass is compatible with
        determinism_class determinism;
    };

    // =============================================================================
    // pass_outcome — result of a single pass run
    // =============================================================================

    enum class pass_outcome : std::uint8_t {
        unchanged = 0,
        changed = 1,
        error = 2,
    };

    // =============================================================================
    // pass_diagnostics — collects diagnostics from a single pass run
    // =============================================================================

    struct pass_diagnostics {
        std::vector<lithe::diag::diagnostic> entries;

        void error(const char* code, std::string msg) {
            entries.push_back({
                .level = lithe::diag::severity::error,
                .stage = lithe::diag::stage::ir,
                .code = code,
                .message = std::move(msg)
            });
        }

        void warn(const char* code, std::string msg) {
            entries.push_back({
                .level = lithe::diag::severity::warning,
                .stage = lithe::diag::stage::ir,
                .code = code,
                .message = std::move(msg)
            });
        }

        [[nodiscard]] bool has_errors() const noexcept {
            for (const auto& d : entries)
                if (d.level == lithe::diag::severity::error) return true;
            return false;
        }
    };

    // =============================================================================
    // Forward declaration: analysis_cache (defined in analysis.hpp)
    // =============================================================================

    class analysis_cache;

    // =============================================================================
    // portable_pass concept — compile-time pass contract (arch §10)
    //
    // A type P satisfies portable_pass iff:
    //   P::descriptor() returns a pass_descriptor (static, constexpr)
    //   p.run(module, cache, policy, diags) returns pass_outcome
    // =============================================================================

    template <class P>
    concept portable_pass =
        requires(P& p, portable_module& m,
                 analysis_cache& ac,
                 const semantic_policy& sp,
                 pass_diagnostics& d) {
            { P::descriptor() } -> std::same_as<pass_descriptor>;
            { p.run(m, ac, sp, d) } -> std::same_as<pass_outcome>;
        };

    // =============================================================================
    // analysis_provider concept — compile-time analysis contract (arch §10)
    //
    // A type A satisfies analysis_provider iff:
    //   A::id() returns analysis_id (static, constexpr)
    //   a.compute(module) computes the analysis fact (type determined by provider)
    // =============================================================================

    template <class A>
    concept analysis_provider =
        requires(A& a, const portable_module& m) {
            { A::id() } -> std::same_as<analysis_id>;
            { a.compute(m) };
        };
} // namespace lithe::ir::portable::opt
