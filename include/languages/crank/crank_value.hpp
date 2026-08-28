#pragma once

// crank/crank_value.hpp — Type-erased value abstraction for transaction evaluators.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// crank_value: type-erased value with explicit ownership semantics.
//   owned    — value is owned by this crank_value; may be moved or copied.
//   borrowed — value is a non-owning view; caller must not outlive the source.
//   staged   — value is staged for a transactional write; resource manages lifetime.
//
// Construction: crank_value::from<T>(v)  — stores T by value (owned).
// Extraction:   val.to<T>()              — returns std::expected<T, std::string>.
//
// Used by tx_evaluator in execute_tx.hpp (read_fn / write_fn).

#include <any>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace crank {
    struct crank_value {
        enum class ownership : std::uint8_t {
            owned, // value is owned; may be moved or copied
            borrowed, // non-owning view; caller manages lifetime
            staged, // staged for transactional write; resource owns lifetime
        };

        std::any payload;
        ownership own = ownership::owned;

        crank_value() = default;

        // Construct an owned value from any T.
        template <class T>
        [[nodiscard]] static crank_value from(T&& v, ownership o = ownership::owned) {
            crank_value cv;
            cv.payload = std::forward<T>(v);
            cv.own = o;
            return cv;
        }

        // Extract a T from the payload. Returns unexpected on type mismatch.
        template <class T>
        [[nodiscard]] std::expected<T, std::string> to() const {
            const T* ptr = std::any_cast<T>(&payload);
            if (!ptr) return std::unexpected(std::string("crank_value: type mismatch"));
            return *ptr;
        }

        [[nodiscard]] bool has_value() const noexcept { return payload.has_value(); }
    };
} // namespace crank
