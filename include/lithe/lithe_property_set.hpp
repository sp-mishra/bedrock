#pragma once

// =============================================================================
// lithe_property_set.hpp — Open metadata carrier for the intelligence layer
//
// Namespace:  lithe::intelligence
// Depends on: <cstdint>, <cstring>, <optional>, <string_view>, <variant>
//             lithe/lithe_feature_extractor.hpp (feature_vector, SBO inline type)
//
// Provides:
//   property_domain       — semantic axis tag (source/tensor/optimization/ml)
//   property_key          — stable (domain, id) address
//   property_set          — empty-optimized, key-addressed open metadata store;
//                           no heap until first set(); SBO via inline slots
//   propagate_forward(src,dst) — copy src into dst (identity on identical refs)
//   merge(a, b, dst)           — combine two sets; b wins on key conflict
//
// Design:
//   • No virtual, no macros. C++23. Header-only.
//   • pay-for-use: empty() → zero cost; no alloc on default-construct.
//   • value variant: int64_t | double | std::string_view | feature_vector*
//     — string_view and feature_vector* are non-owning; lifetime is caller's.
//   • SBO: up to kInlineSlots entries stored inline; spills to heap vector.
//   • thread safety: none (caller's responsibility; same as feature_vector).
// =============================================================================

#include "lithe_feature_extractor.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace lithe::intelligence {
    // =============================================================================
    // property_domain
    // =============================================================================

    enum class property_domain : std::uint8_t {
        source = 0, // source-span, position, dialect metadata
        tensor = 1, // layout, rank, element-type hints
        optimization = 2, // pass hints, cost annotations, priority flags
        ml = 3, // feature_vector pointer, model predictions, confidence
    };

    // =============================================================================
    // property_key
    // =============================================================================

    struct property_key {
        property_domain domain;
        std::uint32_t id; // caller-assigned stable integer within the domain

        [[nodiscard]] constexpr bool operator==(const property_key&) const noexcept = default;
    };

    // =============================================================================
    // property_value — compact variant (non-owning for string_view / feature_vector*)
    // =============================================================================

    using property_value = std::variant<
        std::int64_t,
        double,
        std::string_view, // non-owning; lifetime managed by caller
        features::feature_vector* // non-owning pointer; use ml domain
    >;

    // =============================================================================
    // property_set
    //
    // Open, key-addressed metadata map.  Up to kInlineSlots entries are stored
    // inline (no heap).  Beyond that they spill to a heap std::vector.
    //
    // empty() == true after default construction; no allocation occurs unless
    // set() is called.  This makes it zero-cost to carry in structures that rarely
    // hold properties.
    // =============================================================================

    class property_set {
    public:
        static constexpr std::size_t kInlineSlots = 8;

    private:
        struct slot {
            property_key key;
            property_value value;
        };

        std::array<slot, kInlineSlots> inline_{};
        std::size_t inline_count_ = 0;
        std::vector<slot> overflow_; // only allocated on spill

        [[nodiscard]] const slot* find_slot(property_key k) const noexcept {
            for (std::size_t i = 0; i < inline_count_; ++i)
                if (inline_[i].key == k) return &inline_[i];
            for (const auto& s : overflow_)
                if (s.key == k) return &s;
            return nullptr;
        }

        [[nodiscard]] slot* find_slot(property_key k) noexcept {
            for (std::size_t i = 0; i < inline_count_; ++i)
                if (inline_[i].key == k) return &inline_[i];
            for (auto& s : overflow_)
                if (s.key == k) return &s;
            return nullptr;
        }

    public:
        property_set() noexcept = default;

        // Non-copyable by default — use propagate_forward/merge for semantic copies.
        property_set(const property_set&) = default;
        property_set& operator=(const property_set&) = default;
        property_set(property_set&&) noexcept = default;
        property_set& operator=(property_set&&) noexcept = default;

        // -------------------------------------------------------------------------
        // Capacity / state
        // -------------------------------------------------------------------------

        [[nodiscard]] bool empty() const noexcept {
            return inline_count_ == 0 && overflow_.empty();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return inline_count_ + overflow_.size();
        }

        // -------------------------------------------------------------------------
        // set — insert or overwrite a property
        // -------------------------------------------------------------------------

        template <class V>
            requires std::constructible_from<property_value, V>
        void set(property_key k, V v) {
            if (slot* s = find_slot(k); s) {
                s->value = property_value{std::forward<V>(v)};
                return;
            }
            if (inline_count_ < kInlineSlots) {
                inline_[inline_count_++] = slot{k, property_value{std::forward<V>(v)}};
            }
            else {
                overflow_.push_back(slot{k, property_value{std::forward<V>(v)}});
            }
        }

        // -------------------------------------------------------------------------
        // get — retrieve a typed property; nullopt if absent or type mismatch
        // -------------------------------------------------------------------------

        template <class V>
        [[nodiscard]] std::optional<V> get(property_key k) const noexcept {
            const slot* s = find_slot(k);
            if (!s) return std::nullopt;
            const V* p = std::get_if<V>(&s->value);
            if (!p) return std::nullopt;
            return *p;
        }

        // -------------------------------------------------------------------------
        // has — true iff key is present (any type)
        // -------------------------------------------------------------------------

        [[nodiscard]] bool has(property_key k) const noexcept {
            return find_slot(k) != nullptr;
        }

        // -------------------------------------------------------------------------
        // erase — remove a key if present
        // -------------------------------------------------------------------------

        void erase(property_key k) noexcept {
            for (std::size_t i = 0; i < inline_count_; ++i) {
                if (inline_[i].key == k) {
                    // Swap-remove from inline array
                    inline_[i] = inline_[--inline_count_];
                    return;
                }
            }
            for (auto it = overflow_.begin(); it != overflow_.end(); ++it) {
                if (it->key == k) {
                    overflow_.erase(it);
                    return;
                }
            }
        }

        // -------------------------------------------------------------------------
        // for_each — visit all (key, value) pairs
        // -------------------------------------------------------------------------

        template <class Fn>
        void for_each(Fn&& fn) const {
            for (std::size_t i = 0; i < inline_count_; ++i)
                fn(inline_[i].key, inline_[i].value);
            for (const auto& s : overflow_)
                fn(s.key, s.value);
        }
    };

    // =============================================================================
    // propagate_forward — copy src properties into dst
    //
    // All properties from src are written into dst.  Existing keys in dst are
    // overwritten (src wins on conflict, same as "forward propagation" semantics).
    // =============================================================================

    inline void propagate_forward(const property_set& src, property_set& dst) {
        src.for_each([&](property_key k, const property_value& v) {
            std::visit([&](const auto& val) { dst.set(k, val); }, v);
        });
    }

    // =============================================================================
    // merge — combine two property_sets into dst
    //
    // All properties from a are written first; then all properties from b.
    // On key conflict, b wins ("later wins").  dst is cleared first.
    // =============================================================================

    inline void merge(const property_set& a, const property_set& b, property_set& dst) {
        dst = property_set{};
        a.for_each([&](property_key k, const property_value& v) {
            std::visit([&](const auto& val) { dst.set(k, val); }, v);
        });
        b.for_each([&](property_key k, const property_value& v) {
            std::visit([&](const auto& val) { dst.set(k, val); }, v);
        });
    }
} // namespace lithe::intelligence
