#pragma once

// crank/profiles.hpp — Crank optimization profiles (Module 4) + language feature flags.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// == Optimization profiles ==
// Defines crank.o0..o3 via profile_inherit over the std_oN bases.
//
// == Language feature flags ==
// crank_feature enum — parse-but-gate features that are syntactically accepted
// in v1 but semantically restricted or gated. The parser always accepts the
// grammar; analysis checks the active feature set.
//
//   crank_feature::domain_views — domain view declarations and view_expr (§domain_views).
//     off: view_decl / view_expr parse but are rejected with CRANK-VIEW-000.
//     on:  full domain views pipeline active.
//
//   Other features follow the same convention (associated_types, specialization,
//   structured_concurrency — see grammar §11).
//
// feature_set: bit-set over crank_feature values.
// feature_set::default_v1() — v1 active features (domain_views off by default).
// feature_set::all()        — all features enabled (test / v2).

#include "lithe/lithe_profiles.hpp"

#include <cstdint>

namespace crank {
    using namespace lithe::profile;
    using namespace lithe::passes;

    // ============================================================================
    // crank_feature — language feature flags (parse-but-gate convention)
    // ============================================================================

    enum class crank_feature : std::uint16_t {
        // v1 gated — parse accepted, semantics deferred or restricted
        associated_types = 0x0001u, // associated type declarations in traits
        specialization = 0x0002u, // type-specialized impls (shape-specialized views)
        structured_concurrency = 0x0004u, // structured task scopes

        // domain views (this feature)
        domain_views = 0x0008u, // view_decl + view_expr (§domain_views)

        // all features on
        all_features = 0xFFFFu,
    };

    // ============================================================================
    // feature_set — bitmask of active crank_feature values
    // ============================================================================

    class feature_set {
    public:
        constexpr explicit feature_set(std::uint16_t bits = 0) noexcept : bits_(bits) {}

        [[nodiscard]] constexpr bool has(crank_feature f) const noexcept {
            return (bits_ & static_cast<std::uint16_t>(f)) != 0u;
        }

        constexpr feature_set& enable(crank_feature f) noexcept {
            bits_ |= static_cast<std::uint16_t>(f);
            return *this;
        }

        constexpr feature_set& disable(crank_feature f) noexcept {
            bits_ &= ~static_cast<std::uint16_t>(f);
            return *this;
        }

        // v1 defaults: no experimental features active.
        [[nodiscard]] static constexpr feature_set default_v1() noexcept {
            return feature_set{0u};
        }

        // All features enabled (test / future).
        [[nodiscard]] static constexpr feature_set all() noexcept {
            return feature_set{static_cast<std::uint16_t>(crank_feature::all_features)};
        }

    private:
        std::uint16_t bits_ = 0;
    };

    // ============================================================================
    // crank_extra_bundle — placeholder for future crank-specific passes
    // ============================================================================

    using crank_extra_bundle = pass_bundle<>;

    // ============================================================================
    // Descriptor constants for crank profiles
    // ============================================================================

    inline constexpr profile_descriptor k_crank_o0_desc{
        "crank.o0", {1, 0, 0}, 0, ir_stage::surface, false, true
    };

    inline constexpr profile_descriptor k_crank_o1_desc{
        "crank.o1", {1, 0, 0}, 4, ir_stage::canonical, false, true
    };

    inline constexpr profile_descriptor k_crank_o2_desc{
        "crank.o2", {1, 0, 0}, 6, ir_stage::optimized, false, true
    };

    inline constexpr profile_descriptor k_crank_o3_desc{
        "crank.o3", {1, 0, 0}, 8, ir_stage::optimized, false, true
    };

    // ============================================================================
    // crank.o0..o3 profiles — inherit std_oN + override descriptor id
    // ============================================================================

    using o0_profile = profile_inherit<std_o0, crank_extra_bundle, k_crank_o0_desc>;
    using o1_profile = profile_inherit<std_o1, crank_extra_bundle, k_crank_o1_desc>;
    using o2_profile = profile_inherit<std_o2, crank_extra_bundle, k_crank_o2_desc>;
    using o3_profile = profile_inherit<std_o3, crank_extra_bundle, k_crank_o3_desc>;

    static_assert(profile_valid<o0_profile>(), "crank.o0 profile invalid");
    static_assert(profile_valid<o1_profile>(), "crank.o1 profile invalid");
    static_assert(profile_valid<o2_profile>(), "crank.o2 profile invalid");
    static_assert(profile_valid<o3_profile>(), "crank.o3 profile invalid");

    // Verify descriptor id strings
    static_assert(std::string_view(o3_profile::descriptor.id) == "crank.o3");
} // namespace crank
