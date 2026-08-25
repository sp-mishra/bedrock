#pragma once

// crank/effects.hpp — effect/capability inference for crank semantics (Module 2).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Infers effect_mask and capability_mask per function from the AST.
// Crank extension effects (@host, @gpu, @parallel-safe) registered in ext-band
// (stable_id >= kEffectExtensionBase = 1000) via crank-local add-on registries.
// No edits to vakya::types::effect/capability registries (G-VAK-3 default fix (a)).
//
// Attribute refinement:
//   @pure   → declares EffectMask = 0, CapMask = 0. Conflict with inferred effect → diagnostic.
//   @io     → declares IO effect bit. Ensures IO is present.
//   @net    → declares Network effect bit.
//   @host   → declares ext-band crank::kEffectHost cap bit.
//   @reads  → declares Read capability bit.
//   @writes → declares Write capability bit.
//
// Usage:
//   crank::effects_registry ereg = crank::make_crank_effects_registry();
//   crank::caps_registry    creg = crank::make_crank_caps_registry();
//   crank::effect_checker chk(ereg, creg);
//   chk.declare_fn("Dot", attrs, inferred_effect_mask, inferred_cap_mask);
//   auto result = chk.take();

#include "vakya/types/effect.hpp"
#include "vakya/types/capability.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
    using effect_mask = vakya::types::effect_mask;
    using capability_mask = vakya::types::capability_mask;
    using effect_registry = vakya::types::effect_registry;
    using capability_registry = vakya::types::capability_registry;

    // ============================================================================
    // Crank extension effect stable_ids (ext-band >= 1000)
    // ============================================================================

    inline constexpr std::uint32_t kEffectHost = 1000u;
    inline constexpr std::uint32_t kEffectGpu = 1001u;
    inline constexpr std::uint32_t kEffectParallelSafe = 1002u;

    // Crank extension capability stable_ids (ext-band >= 1000)
    inline constexpr std::uint32_t kCapHost = 1000u;
    inline constexpr std::uint32_t kCapGpu = 1001u;
    inline constexpr std::uint32_t kCapParallelSafe = 1002u;

    // Extension bits for effect_mask: use upper bits (above builtin 0-4)
    inline constexpr effect_mask kEffectMaskHost = 1ULL << 32;
    inline constexpr effect_mask kEffectMaskGpu = 1ULL << 33;
    inline constexpr effect_mask kEffectMaskParallelSafe = 1ULL << 34;

    // Extension bits for capability_mask
    inline constexpr capability_mask kCapMaskHost = 1ULL << 32;
    inline constexpr capability_mask kCapMaskGpu = 1ULL << 33;
    inline constexpr capability_mask kCapMaskParallelSafe = 1ULL << 34;

    // ============================================================================
    // make_crank_effects_registry — builtin effects + crank ext-band effects
    // ============================================================================

    [[nodiscard]] inline vakya::types::effect_registry make_crank_effects_registry() {
        using namespace vakya::types;

        effect_registry reg = make_builtin_effect_registry();

        auto add = [&](std::uint32_t id, std::string_view sym, effect_mask bit) {
            effect_descriptor d;
            d.stable_id = id;
            d.name_hash = containers::desc_name_hash(sym);
            d.category = effect_category::extension;
            d.symbol = sym;
            d.bit_mask = bit;
            reg.register_desc(d);
        };

        add(kEffectHost, "@host", kEffectMaskHost);
        add(kEffectGpu, "@gpu", kEffectMaskGpu);
        add(kEffectParallelSafe, "@parallel-safe", kEffectMaskParallelSafe);

        return reg;
    }

    // ============================================================================
    // make_crank_caps_registry — builtin caps + crank ext-band caps
    // ============================================================================

    [[nodiscard]] inline vakya::types::capability_registry make_crank_caps_registry() {
        using namespace vakya::types;

        capability_registry reg = make_builtin_capability_registry();

        auto add = [&](std::uint32_t id, std::string_view sym, capability_mask bit) {
            capability_descriptor d;
            d.stable_id = id;
            d.name_hash = containers::desc_name_hash(sym);
            d.category = capability_category::extension;
            d.symbol = sym;
            d.bit_mask = bit;
            reg.register_desc(d);
        };

        add(kCapHost, "@host", kCapMaskHost);
        add(kCapGpu, "@gpu", kCapMaskGpu);
        add(kCapParallelSafe, "@parallel-safe", kCapMaskParallelSafe);

        return reg;
    }

    // ============================================================================
    // fn_attribute_set — parsed attributes on a function declaration
    // ============================================================================

    struct fn_attribute_set {
        bool is_pure = false; // @pure
        bool is_io = false; // @io
        bool is_net = false; // @net
        bool is_host = false; // @host
        bool reads = false; // @reads
        bool writes = false; // @writes
        bool is_gpu = false; // @gpu(...)
        bool is_parallel = false; // @parallel(...)
    };

    // ============================================================================
    // fn_effect_info — computed effect + capability info for one function
    // ============================================================================

    struct fn_effect_info {
        std::string name;
        effect_mask declared_effects = 0; // from attributes
        effect_mask inferred_effects = 0; // from body analysis
        effect_mask final_effects = 0; // max(declared, inferred)
        capability_mask declared_caps = 0;
        capability_mask inferred_caps = 0;
        capability_mask final_caps = 0;
    };

    // ============================================================================
    // effect_diagnostic
    // ============================================================================

    struct effect_diagnostic {
        enum class kind : std::uint8_t {
            pure_conflict, // @pure declared but IO/host/net inferred
            missing_io, // @io declared but no IO inferred (warning)
        };

        kind k;
        std::string fn_name;
        std::string message;
        [[nodiscard]] bool is_error() const noexcept { return k == kind::pure_conflict; }

        [[nodiscard]] constexpr std::string_view code() const noexcept {
            return to_code(k);
        }

        [[nodiscard]] static constexpr std::string_view
        to_code(kind k) noexcept {
            switch (k) {
            case kind::pure_conflict: return "CRANK-EFF-001";
            case kind::missing_io: return "CRANK-EFF-002";
            }
            return "CRANK-EFF-000";
        }
    };

    // ============================================================================
    // effects_result
    // ============================================================================

    struct effects_result {
        std::vector<fn_effect_info> functions;
        std::vector<effect_diagnostic> diagnostics;

        [[nodiscard]] bool ok() const noexcept {
            for (const auto& d : diagnostics)
                if (d.is_error()) return false;
            return true;
        }
    };

    // ============================================================================
    // effect_checker — runs attribute refinement and conflict detection
    // ============================================================================

    class effect_checker {
    public:
        explicit effect_checker(const effect_registry& ereg,
                                const capability_registry& creg)
            : ereg_(&ereg), creg_(&creg) {}

        // Declare a function with its parsed attributes and body-inferred masks.
        void declare_fn(std::string_view name,
                        const fn_attribute_set& attrs,
                        effect_mask inferred_eff,
                        capability_mask inferred_cap) {
            using namespace vakya::types;

            fn_effect_info info;
            info.name = std::string(name);
            info.inferred_effects = inferred_eff;
            info.inferred_caps = inferred_cap;

            // --- compute declared masks from attributes ---
            effect_mask decl_eff = 0;
            capability_mask decl_cap = 0;

            if (attrs.is_io) decl_eff = add_effect(decl_eff, kEffectMaskIO);
            if (attrs.is_net) decl_eff = add_effect(decl_eff, kEffectMaskNetwork);
            if (attrs.is_host) {
                decl_eff = add_effect(decl_eff, kEffectMaskHost);
                decl_cap = add_capability(decl_cap, kCapMaskHost);
            }
            if (attrs.reads) decl_cap = add_capability(decl_cap, kCapMaskRead);
            if (attrs.writes) decl_cap = add_capability(decl_cap, kCapMaskWrite);
            if (attrs.is_gpu) {
                decl_eff = add_effect(decl_eff, kEffectMaskGpu);
                decl_cap = add_capability(decl_cap, kCapMaskGpu);
            }
            if (attrs.is_parallel) {
                decl_eff = add_effect(decl_eff, kEffectMaskParallelSafe);
                decl_cap = add_capability(decl_cap, kCapMaskParallelSafe);
            }

            info.declared_effects = decl_eff;
            info.declared_caps = decl_cap;

            // --- @pure conflict check ---
            constexpr effect_mask kSideEffectBits =
                kEffectMaskIO | kEffectMaskNetwork | kEffectMaskFileSystem
                | kEffectMaskHost | kEffectMaskGpu;

            if (attrs.is_pure) {
                if (has_effect(inferred_eff, kSideEffectBits)) {
                    result_.diagnostics.push_back({
                        effect_diagnostic::kind::pure_conflict,
                        std::string(name),
                        "@pure fn '" + std::string(name)
                        + "' has inferred side-effect bits"
                    });
                }
                // Pure: final masks = 0 (override inferred)
                info.final_effects = 0;
                info.final_caps = 0;
            }
            else {
                // Final = union of declared and inferred
                info.final_effects = decl_eff | inferred_eff;
                info.final_caps = decl_cap | inferred_cap;
            }

            result_.functions.push_back(std::move(info));
        }

        [[nodiscard]] effects_result take() { return std::move(result_); }

        [[nodiscard]] const std::vector<effect_diagnostic>& diagnostics() const noexcept {
            return result_.diagnostics;
        }

    private:
        const effect_registry* ereg_;
        const capability_registry* creg_;
        effects_result result_;

        // Lookup an effect bit by symbol name from the registry (for future by-name resolution).
        [[nodiscard]] effect_mask effect_bit(std::string_view sym) const noexcept {
            auto* d = ereg_->find_by_name(containers::desc_name_hash(sym));
            return d ? d->bit_mask : 0;
        }

        [[nodiscard]] capability_mask cap_bit(std::string_view sym) const noexcept {
            auto* d = creg_->find_by_name(containers::desc_name_hash(sym));
            return d ? d->bit_mask : 0;
        }
    };
} // namespace crank
