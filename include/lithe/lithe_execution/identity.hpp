#pragma once

// =============================================================================
// lithe_execution/identity.hpp — four backend identity notions
//
// The four notions serve different, non-interchangeable purposes:
//
//   persisted_backend_id  deterministic, serializable string.
//                         Safe to store in config / cache / artifacts.
//                         MUST NOT be cast to a C++ type.
//
//   backend_display_name  human-readable label.  Not a stable identity;
//                         never compare two backends by display name.
//
//   in_process_type_token process-local dispatch token.
//                         Used with typed<T>() for safe C++ casts.
//                         MUST NOT be persisted across processes.
//
//   plugin_descriptor     static NTTP block (from lithe_extension.hpp).
//                         The fourth notion — composite identity + metadata.
//                         Reused; no new type.
//
// No implicit conversions between any of these types.  A compile-time
// static_assert guards that in_process_type_token cannot be constructed from
// the string representation of persisted_backend_id.
//
// No virtual, no macros.  Header-only C++23.  Zero lithe::ir dependency.
// =============================================================================

#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "foundation.hpp"

namespace lithe::execution {
    // All four types are already defined in foundation.hpp:
    //   persisted_backend_id
    //   backend_display_name
    //   in_process_type_token
    // plugin_descriptor is in lithe_extension.hpp — reused, not redefined here.

    // =========================================================================
    // typed<T>() — safe same-process downcast keyed by type_token.
    //
    // A caller supplies the target type T and the token it computed for T.
    // If token matches, the void* is reinterpret_cast'd to T*.
    // Token mismatch returns nullptr without UB.
    //
    // Usage:
    //   in_process_type_token tok = type_token_for<my_backend>();
    //   void* erased = ...;
    //   if (auto* p = typed<my_backend>(erased, tok)) { ... }
    // =========================================================================

    // Compute a stable in-process token for type T using address-of-template
    // instantiation.  The address of the static local is unique per T and
    // process-stable for the lifetime of the process.  Must not be compared
    // across processes.
    template <class T>
    [[nodiscard]] inline in_process_type_token type_token_for() noexcept {
        static const char sentinel = 0;
        return in_process_type_token{
            reinterpret_cast<std::uint64_t>(&sentinel)
        };
    }

    // Safe downcast: returns T* iff token equals the type_token_for<T>() value.
    template <class T>
    [[nodiscard]] inline T*
    typed(void* erased, const in_process_type_token token) noexcept {
        if (erased == nullptr) return nullptr;
        if (token != type_token_for<T>()) return nullptr;
        return static_cast<T*>(erased);
    }

    // =========================================================================
    // Compile-time guards ( invariants)
    // =========================================================================

    // persisted_backend_id must not be constructible from a uint64 (prevents
    // numeric-id confusion with in_process_type_token.value).
    static_assert(!std::is_constructible_v<persisted_backend_id, std::uint64_t>,
                  "persisted_backend_id must not be constructible from uint64");

    // in_process_type_token must not be constructible from string_view (prevents
    // persisted-string confusion with process-local numeric token).
    static_assert(!std::is_constructible_v<in_process_type_token, std::string_view>,
                  "in_process_type_token must not be constructible from string_view");

    // The types must not be implicitly interconvertible.
    static_assert(!std::is_convertible_v<persisted_backend_id, in_process_type_token>,
                  "persisted_backend_id must not convert to in_process_type_token");
    static_assert(!std::is_convertible_v<in_process_type_token, persisted_backend_id>,
                  "in_process_type_token must not convert to persisted_backend_id");
} // namespace lithe::execution
