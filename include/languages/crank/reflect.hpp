#pragma once

// crank/reflect.hpp — §v2.16 restricted reflection.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Restricted, compile-time reflection: a program can describe a type's fields
// (name/type/offset), the trait set it satisfies, and the effect/capability
// mask it carries — enough to auto-generate typed GPU/SIMD struct layouts and
// host-registration glue, without exposing arbitrary runtime type queries
// (those are v3). Everything here is opt-in and pay-for-use: a type is only
// reflected if the program builds a type_descriptor<T> for it.
//
// Model:
//   reflected_field       — one field: name, type name, byte offset, size.
//   type_descriptor<T>    — a value describing T: name, size, align, fields,
//                           satisfied trait names, capability mask.
//   reflect_builder<T>    — fluent builder (field/trait/capability) → descriptor.
//   layout_is_gpu_safe()  — a descriptor is GPU/SIMD-uploadable iff its type is
//                           trivially copyable and standard-layout (so offsets
//                           are meaningful across the host/device boundary).
//
// The @reflect(...) annotation (fields, traits, capabilities, host_registration,
// backend_adapters) selects which facets to emit; reflect_facets mirrors it as a
// bitset the builder consults. No arbitrary RTTI, no virtual — a descriptor is a
// plain value the program constructs where the concrete type is known.
//
// Design refs: §v2.16; annotation.hpp (@reflect schema registration).

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace crank {
    // ============================================================================
    // reflect_facets — which facets @reflect(...) requested. Bitmask; a builder
    // only populates the facets that are enabled (pay-for-use).
    // ============================================================================

    enum class reflect_facet : std::uint32_t {
        fields = 1u << 0,
        traits = 1u << 1,
        capabilities = 1u << 2,
        host_registration = 1u << 3,
        backend_adapters = 1u << 4,
    };

    [[nodiscard]] constexpr std::uint32_t operator|(reflect_facet a, reflect_facet b) noexcept {
        return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
    }

    [[nodiscard]] constexpr std::uint32_t operator|(std::uint32_t a, reflect_facet b) noexcept {
        return a | static_cast<std::uint32_t>(b);
    }

    [[nodiscard]] constexpr bool has_facet(std::uint32_t mask, reflect_facet f) noexcept {
        return (mask & static_cast<std::uint32_t>(f)) != 0u;
    }

    // ============================================================================
    // layout_context — the target-dependent facts that make a field offset
    // *meaningful*. A reflected offset is only valid within a matching context:
    // change the ABI, packing, alignment, endianness, or layout policy version and
    // the offsets may shift, so a descriptor built for one context must not be
    // trusted (or replayed from an AOT artifact) under another (§v2.16).
    // ============================================================================

    struct layout_context {
        std::uint64_t target_abi_hash = 0; // FNV-1a of the target ABI descriptor
        std::uint32_t packing = 0; // pragma-pack value; 0 = natural
        std::uint32_t alignment = 0; // struct alignment in bytes
        std::endian endianness = std::endian::native; // byte order the offsets assume
        std::uint32_t layout_version = 1; // layout-policy version, bumped on ABI change

        [[nodiscard]] bool operator==(const layout_context&) const noexcept = default;

        // native_for<T>() — the layout_context of T as compiled in this process.
        template <class T>
        [[nodiscard]] static layout_context native_for() noexcept {
            return layout_context{
                /*target_abi_hash*/ 0,
                /*packing*/ 0,
                /*alignment*/ static_cast<std::uint32_t>(alignof(T)),
                /*endianness*/ std::endian::native,
                /*layout_version*/ 1
            };
        }
    };

    // reflection_matches — a loader check: are two layout contexts compatible?
    // A reflected descriptor / AOT artifact built under `artifact` is only safe to
    // use under `current` when the two contexts are identical.
    [[nodiscard]] inline bool
    reflection_matches(const layout_context& artifact, const layout_context& current) noexcept {
        return artifact == current;
    }

    // ============================================================================
    // reflected_field — one field of a reflected type.
    // ============================================================================

    struct reflected_field {
        std::string name; // field identifier
        std::string type_name; // spelled type, e.g. "Float32"
        std::size_t offset = 0; // byte offset within the struct
        std::size_t size = 0; // byte size of the field
    };

    // ============================================================================
    // type_descriptor<T> — restricted reflection value for T.
    // ============================================================================

    template <class T>
    struct type_descriptor {
        std::string name;
        std::size_t size = sizeof(T);
        std::size_t align = alignof(T);
        std::vector<reflected_field> fields;
        std::vector<std::string> satisfied_traits;
        std::uint64_t capability_mask = 0;
        std::uint32_t facets = 0; // reflect_facet bitmask emitted
        layout_context layout = layout_context::native_for<T>(); // §v2.16 offset validity context

        // Standard-layout + trivially-copyable ⇒ field offsets are portable across a
        // host/device (GPU/SIMD) boundary, so the descriptor can drive a typed upload.
        [[nodiscard]] static constexpr bool type_is_gpu_safe() noexcept {
            return std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;
        }

        [[nodiscard]] bool layout_is_gpu_safe() const noexcept { return type_is_gpu_safe(); }

        [[nodiscard]] bool has(reflect_facet f) const noexcept { return has_facet(facets, f); }

        // layout_fingerprint — a stable hash over the layout_context AND every field
        // offset. Any change to ABI/packing/alignment/endianness/version, or to a
        // field offset, produces a different fingerprint — so an AOT artifact keyed
        // on this hash is invalidated when the reflected layout shifts (§v2.16).
        [[nodiscard]] std::uint64_t layout_fingerprint() const noexcept {
            constexpr std::uint64_t kOffset = 14695981039346656037ULL;
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            std::uint64_t h = kOffset;
            auto mix = [&](std::uint64_t v) noexcept {
                for (int i = 0; i < 8; ++i) {
                    h ^= (v & 0xFFu);
                    h *= kPrime;
                    v >>= 8;
                }
            };
            mix(layout.target_abi_hash);
            mix(layout.packing);
            mix(layout.alignment);
            mix(static_cast<std::uint64_t>(layout.endianness == std::endian::big));
            mix(layout.layout_version);
            mix(size);
            for (const auto& f : fields) mix(f.offset);
            return h;
        }
    };

    // ============================================================================
    // reflect_builder<T> — fluent construction of a type_descriptor<T>.
    //
    // The program supplies field offsets/sizes where the concrete layout is known
    // (typically via offsetof for standard-layout structs). Each builder call also
    // records the facet it populated so the descriptor reports what @reflect asked
    // for.
    // ============================================================================

    template <class T>
    class reflect_builder {
    public:
        explicit reflect_builder(std::string type_name) {
            desc_.name = std::move(type_name);
        }

        // Add a field (facet: fields).
        reflect_builder& field(std::string name, std::string type_name,
                               std::size_t offset, std::size_t size) {
            desc_.fields.push_back({std::move(name), std::move(type_name), offset, size});
            desc_.facets |= static_cast<std::uint32_t>(reflect_facet::fields);
            return *this;
        }

        // Record a satisfied trait (facet: traits).
        reflect_builder& satisfies(std::string trait_name) {
            desc_.satisfied_traits.push_back(std::move(trait_name));
            desc_.facets |= static_cast<std::uint32_t>(reflect_facet::traits);
            return *this;
        }

        // Set a capability bit (facet: capabilities).
        reflect_builder& capability(std::uint32_t bit) {
            desc_.capability_mask |= (std::uint64_t{1} << bit);
            desc_.facets |= static_cast<std::uint32_t>(reflect_facet::capabilities);
            return *this;
        }

        // Mark that host-registration glue should be emitted for T (facet).
        reflect_builder& host_registration() {
            desc_.facets |= static_cast<std::uint32_t>(reflect_facet::host_registration);
            return *this;
        }

        // Mark that backend-adapter glue should be emitted for T (facet).
        reflect_builder& backend_adapters() {
            desc_.facets |= static_cast<std::uint32_t>(reflect_facet::backend_adapters);
            return *this;
        }

        // Set the layout context (target ABI/packing/endianness/version) the field
        // offsets were computed under. Defaults to the native context of T.
        reflect_builder& layout(layout_context ctx) {
            desc_.layout = ctx;
            return *this;
        }

        [[nodiscard]] type_descriptor<T> build() const { return desc_; }

    private:
        type_descriptor<T> desc_;
    };

    // ============================================================================
    // reflect_field — offsetof/sizeof helper so callers don't repeat the boilerplate.
    // Usage: b.field("x", "Float32", CRANK_REFLECT_FIELD is avoided (no macros); use
    // the offsetof expression directly at the call site).
    // ============================================================================

    template <class T>
    [[nodiscard]] reflected_field
    make_field(std::string name, std::string type_name,
               std::size_t offset, std::size_t size) {
        (void)sizeof(T);
        return reflected_field{std::move(name), std::move(type_name), offset, size};
    }
} // namespace crank
