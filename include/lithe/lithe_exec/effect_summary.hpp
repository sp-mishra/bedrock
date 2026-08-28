#pragma once

// =============================================================================
// lithe_exec/effect_summary.hpp — Effect propagation over HL MIR regions
//
// Namespace: lithe::exec
//
// Provides:
//   effect_kind      — per-operation effect class (pure, io, host_call, ...)
//   effect_mask      — bitset of effect_kind values (mirrors backend_capability_set)
//   effect_summary   — rollup over a HL MIR region (all effects + unknown flag)
//
//   Legality predicates (pure functions):
//     gpu_legal(mask)     — false if mask has host_call|io|network|transaction
//     simd_legal(mask)    — false if mask has io|network|host_call|atomic|transaction
//     threaded_legal(mask)— false if mask has io|host_call|network|transaction
//                          (write-conflict legality is memory_summary's job)
//
// Design:
//   Source of truth for per-op effects = Vākya annotations.
//   Lithe only propagates/folds; never reinterprets.
//   effect_mask uses the same bit-set idiom as backend_capability_set.
//   No virtual, no macros. Header-only C++23.
// =============================================================================

#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace lithe::exec {
    // =========================================================================
    // effect_kind
    // =========================================================================

    enum class effect_kind : std::uint8_t {
        pure = 0,
        reads_memory = 1,
        writes_memory = 2,
        host_call = 3, // calls into host runtime / FFI
        io = 4, // file / console / device I/O
        network = 5,
        allocates = 6, // heap allocation
        atomic = 7, // atomic read-modify-write
        transaction = 8, // inside a Medha transaction region
        unknown = 9, // callee with unknown effects
    };

    static_assert(static_cast<std::uint8_t>(effect_kind::unknown) < 64,
                  "effect_kind must fit in a 64-bit mask");

    // =========================================================================
    // effect_mask — bitset over effect_kind values
    //
    // Mirrors backend_capability_set in style (bits + add/has/merge/op|).
    // =========================================================================

    struct effect_mask {
        std::uint64_t bits = 0;

        constexpr effect_mask() noexcept = default;
        constexpr explicit effect_mask(std::uint64_t raw) noexcept : bits(raw) {}

        [[nodiscard]] static constexpr effect_mask from(
            std::initializer_list<effect_kind> kinds) noexcept {
            effect_mask m;
            for (auto k : kinds) m.add(k);
            return m;
        }

        constexpr void add(effect_kind k) noexcept {
            bits |= (std::uint64_t{1} << static_cast<std::uint8_t>(k));
        }

        [[nodiscard]] constexpr bool has(effect_kind k) const noexcept {
            return (bits & (std::uint64_t{1} << static_cast<std::uint8_t>(k))) != 0;
        }

        constexpr void merge(const effect_mask& o) noexcept { bits |= o.bits; }

        [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }

        [[nodiscard]] constexpr bool any_of(const effect_mask& o) const noexcept {
            return (bits & o.bits) != 0;
        }

        [[nodiscard]] friend constexpr effect_mask
        operator|(effect_mask a, effect_mask b) noexcept {
            return effect_mask{a.bits | b.bits};
        }

        [[nodiscard]] friend constexpr effect_mask
        operator&(effect_mask a, effect_mask b) noexcept {
            return effect_mask{a.bits & b.bits};
        }

        [[nodiscard]] friend constexpr bool
        operator==(effect_mask a, effect_mask b) noexcept {
            return a.bits == b.bits;
        }
    };

    static_assert(std::is_trivially_copyable_v<effect_mask>);

    // =========================================================================
    // effect_summary — rollup for one HL MIR region
    // =========================================================================

    struct effect_summary {
        effect_mask region_effects;
        bool has_unknown_effect = false;

        [[nodiscard]] constexpr bool is_pure() const noexcept {
            return region_effects.empty() && !has_unknown_effect;
        }

        [[nodiscard]] constexpr bool has(effect_kind k) const noexcept {
            return region_effects.has(k);
        }

        void add(effect_kind k) noexcept {
            if (k == effect_kind::unknown) {
                has_unknown_effect = true;
                return;
            }
            region_effects.add(k);
        }

        void merge(const effect_summary& o) noexcept {
            region_effects.merge(o.region_effects);
            has_unknown_effect |= o.has_unknown_effect;
        }
    };

    // =========================================================================
    // Backend legality predicates — pure functions, no state
    // =========================================================================

    // Forbidden on GPU: any non-pure effect that requires host synchronization
    // or side-effects Vākya cannot lower to device code.
    [[nodiscard]] inline constexpr bool gpu_legal(const effect_mask& m) noexcept {
        static constexpr effect_mask forbidden = effect_mask::from({
            effect_kind::host_call,
            effect_kind::io,
            effect_kind::network,
            effect_kind::allocates,
            effect_kind::transaction,
            effect_kind::unknown,
        });
        return !m.any_of(forbidden);
    }

    [[nodiscard]] inline constexpr bool gpu_legal(const effect_summary& s) noexcept {
        return !s.has_unknown_effect && gpu_legal(s.region_effects);
    }

    // SIMD is local (single thread), so we only reject effects that require
    // ordering / external I/O guarantees that SIMD cannot preserve.
    [[nodiscard]] inline constexpr bool simd_legal(const effect_mask& m) noexcept {
        static constexpr effect_mask forbidden = effect_mask::from({
            effect_kind::io,
            effect_kind::network,
            effect_kind::host_call,
            effect_kind::atomic,
            effect_kind::transaction,
            effect_kind::unknown,
        });
        return !m.any_of(forbidden);
    }

    [[nodiscard]] inline constexpr bool simd_legal(const effect_summary& s) noexcept {
        return !s.has_unknown_effect && simd_legal(s.region_effects);
    }

    // Threaded: write conflicts are handled separately by memory_summary.
    // Here we only reject effects that cannot be parallelized at all.
    [[nodiscard]] inline constexpr bool threaded_legal(const effect_mask& m) noexcept {
        static constexpr effect_mask forbidden = effect_mask::from({
            effect_kind::io,
            effect_kind::network,
            effect_kind::host_call,
            effect_kind::transaction,
            effect_kind::unknown,
        });
        return !m.any_of(forbidden);
    }

    [[nodiscard]] inline constexpr bool threaded_legal(const effect_summary& s) noexcept {
        return !s.has_unknown_effect && threaded_legal(s.region_effects);
    }
} // namespace lithe::exec
