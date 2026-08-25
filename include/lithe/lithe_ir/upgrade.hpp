#pragma once

// =============================================================================
// lithe_ir/upgrade.hpp — IR schema upgrade CPO + upgrade_registry
//
// Provides:
//   upgrade_ir CPO — ADL tag_invoke for upgrading an IR object from an older
//     schema version to the current one.  Registered per (IR, from_version) pair.
//     Returns expected<IR, ir_error>.
//
//   upgrade_registry — compile-time-empty or runtime-populated registry of
//     upgrade functions, keyed by (ir_kind_tag, schema_version).  Lives in
//     namespace lithe::ir — separate from ::lithe::execution::backend_registry.
//
// Design constraints:
//   • Upgrades must NOT reach into ::lithe::execution::algo (DAG reversal is
//     a backend concern, not an IR interchange concern).
//   • The upgrade_registry is OPTIONAL for the static engine — only tooling
//     paths instantiate it.
//   • no_upgrade_registry sentinel is the zero-cost default.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <any>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../lithe_execution/foundation.hpp"  // ir_error
#include "format.hpp"                         // schema_version, format_descriptor, ir_kind

namespace lithe::ir {
    // =========================================================================
    //a.7 upgrade_ir CPO
    //
    // tag_invoke(upgrade_ir_t{}, Provider&, IR&&, schema_version from_version)
    //   → expected<IR, ir_error>
    //
    // Customised per (Provider, IR) pair via ADL.  Provider is typically the
    // codec type for the IR format.
    // =========================================================================

    namespace cpo {
        struct upgrade_ir_t {
            template <class Prov, class IR>
            [[nodiscard]] auto operator()(Prov&& p, IR&& ir,
                                          const schema_version from_version) const
                noexcept(noexcept(tag_invoke(*this, std::forward<Prov>(p),
                                             std::forward<IR>(ir), from_version)))
                -> decltype(tag_invoke(*this, std::forward<Prov>(p),
                                       std::forward<IR>(ir), from_version)) {
                return tag_invoke(*this, std::forward<Prov>(p),
                                  std::forward<IR>(ir), from_version);
            }
        };

        inline constexpr upgrade_ir_t upgrade_ir{};
    } // namespace cpo

    // Concept: a provider can upgrade IR from a given schema version.
    template <class Prov, class IR>
    concept ir_upgrader_for = requires(Prov& p, IR ir, schema_version v) {
        {
            cpo::upgrade_ir(p, std::move(ir), v)
        }
        -> std::same_as<std::expected<IR, ::lithe::execution::ir_error>>;
    };

    // =========================================================================
    //a.9 upgrade_key — (ir_kind_tag + from_version) registry key
    // =========================================================================

    struct upgrade_key {
        ::lithe::execution::ir_kind ir_kind_tag = ::lithe::execution::ir_kind::unknown;
        schema_version from_version{};

        [[nodiscard]] bool operator==(const upgrade_key& o) const noexcept {
            return ir_kind_tag == o.ir_kind_tag && from_version == o.from_version;
        }
    };
} // namespace lithe::ir

// std::hash for upgrade_key (outside lithe::ir to follow ADL rules).
namespace std {
    template <>
    struct hash<lithe::ir::upgrade_key> {
        [[nodiscard]] std::size_t
        operator()(const lithe::ir::upgrade_key& k) const noexcept {
            const std::size_t h1 = static_cast<std::size_t>(
                static_cast<std::uint8_t>(k.ir_kind_tag));
            const std::size_t h2 = static_cast<std::size_t>(k.from_version.major) << 32 |
                static_cast<std::size_t>(k.from_version.minor) << 16 |
                static_cast<std::size_t>(k.from_version.patch);
            constexpr std::size_t kPrime = 0x9e3779b97f4a7c15ULL;
            return (h1 ^ (h2 * kPrime)) * kPrime;
        }
    };
} // namespace std

namespace lithe::ir {
    // =========================================================================
    //a.9 upgrade_registry — type-erased upgrade function registry
    //
    // Maps (ir_kind, from_version) → std::function<expected<T, ir_error>(T&&)>
    // where T is std::any-erased.  Only tooling paths instantiate this.
    //
    // For typed upgrade (known IR type at compile time) use the typed_upgrade_registry.
    // =========================================================================

    using erased_upgrade_fn = std::function<
        std::expected<std::any, ::lithe::execution::ir_error>(std::any &&)>;

    // A callable view keeps the expected-bearing erased function out of pointer
    // comparison expressions. This is required by current libc++ C++26, whose
    // expected equality constraints otherwise recurse through test frameworks
    // and container iterators.
    class erased_upgrade_callback {
    public:
        explicit erased_upgrade_callback(erased_upgrade_fn callback)
            : callback_(std::move(callback)) {}

        [[nodiscard]] std::expected<std::any, ::lithe::execution::ir_error>
        operator()(std::any&& value) const {
            return callback_(std::move(value));
        }

    private:
        erased_upgrade_fn callback_;
    };

    class upgrade_registry {
    private:
        struct upgrade_node {
            upgrade_key key;
            erased_upgrade_callback callback;
            std::unique_ptr<upgrade_node> next;
        };

    public:
        upgrade_registry() = default;

        upgrade_registry(const upgrade_registry&) = delete;
        upgrade_registry& operator=(const upgrade_registry&) = delete;
        upgrade_registry(upgrade_registry&&) = default;
        upgrade_registry& operator=(upgrade_registry&&) = default;

        // Register a typed upgrade function for (ir_kind_tag, from_version).
        // fn: IR&& → expected<IR, ir_error>
        template <class IR>
        void register_upgrade(const ::lithe::execution::ir_kind ir_kind_tag,
                              const schema_version from_version,
                              std::function<std::expected<IR, ::lithe::execution::ir_error>(IR &&)> fn) {
            const upgrade_key key{ir_kind_tag, from_version};
            erased_upgrade_fn callback = [fn = std::move(fn)](std::any&& val)
                -> std::expected<std::any, ::lithe::execution::ir_error> {
                    auto* typed = std::any_cast<IR>(&val);
                    if (!typed)
                        return std::unexpected(::lithe::execution::ir_error{
                            "upgrade_registry: IR type mismatch"
                        });
                    auto result = fn(std::move(*typed));
                    if (!result) return std::unexpected(result.error());
                    return std::any{std::move(*result)};
                };

            for (auto* node = nodes_.get(); node != nullptr; node = node->next.get()) {
                if (node->key == key) {
                    node->callback = erased_upgrade_callback{std::move(callback)};
                    return;
                }
            }

            nodes_ = std::make_unique<upgrade_node>(
                upgrade_node{key, erased_upgrade_callback{std::move(callback)},
                             std::move(nodes_)});
            ++size_;
        }

        // Lookup: returns nullptr if no upgrade registered for this key.
        [[nodiscard]] const erased_upgrade_callback*
        find(const ::lithe::execution::ir_kind ir_kind_tag,
             const schema_version from_version) const noexcept {
            const upgrade_key key{ir_kind_tag, from_version};
            for (auto* node = nodes_.get(); node != nullptr; node = node->next.get()) {
                if (node->key == key) return &node->callback;
            }
            return nullptr;
        }

        [[nodiscard]] bool has(const ::lithe::execution::ir_kind ir_kind_tag,
                               const schema_version from_version) const noexcept {
            return find(ir_kind_tag, from_version) != nullptr;
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    private:
        std::unique_ptr<upgrade_node> nodes_;
        std::size_t size_ = 0;
    };

    // =========================================================================
    //a.9 no_upgrade_registry — zero-cost default sentinel
    // =========================================================================

    struct no_upgrade_registry {
        static constexpr bool active = false;
    };

    static_assert(std::is_empty_v<no_upgrade_registry>);
} // namespace lithe::ir
