#pragma once

// Petika-backed durable catalog adapter.  The core catalog contract remains
// independent of Petika; this header is included only by the store umbrella or
// directly by users that select Petika persistence.

#include "catalog.hpp"

#include "petika/petika.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <variant>

namespace lithe::execution::store {
    using petika_catalog_value = std::variant<catalog_entry, lease_record>;

    template <class Store>
    concept petika_catalog_store = requires(
        Store& store,
        std::string key,
        petika_catalog_value value) {
        { store.get(key) } -> std::same_as<petika::Result<petika_catalog_value>>;
        { store.put(key, value) } -> std::same_as<petika::Result<void>>;
        { store.erase(key) } -> std::same_as<petika::Result<void>>;
        { store.transaction() };
        store.for_each([](const auto&, const auto&) {});
    };

    // Store must use std::string keys and petika_catalog_value values.  A
    // serializer for that value belongs to the application/Petika provider,
    // never to Lithe's stable API.
    template <petika_catalog_store Store>
    class petika_catalog {
    public:
        explicit petika_catalog(Store& store, std::chrono::seconds lease_ttl = std::chrono::seconds{30})
            : store_{std::addressof(store)}, lease_ttl_{lease_ttl} {}

        [[nodiscard]] std::expected<std::optional<catalog_entry>, catalog_error>
        lookup(const artifact_key& key) {
            auto value = store_->get(entry_key(key));
            if (!value) {
                if (value.error() == petika::StorageError::NotFound) return std::optional<catalog_entry>{};
                return std::unexpected(catalog_error::backend("Petika catalog lookup failed"));
            }
            if (const auto* entry = std::get_if<catalog_entry>(std::addressof(*value))) return *entry;
            return std::unexpected(catalog_error::codec("Petika catalog entry has an invalid record kind"));
        }

        [[nodiscard]] std::expected<utils::lease_token, catalog_error>
        acquire_lease(const artifact_key& key) {
            const auto now = now_ns();
            const auto lease_key_value = lease_key(key);
            auto transaction = store_->transaction();
            if (auto existing = transaction.get(lease_key_value); existing) {
                if (const auto* lease = std::get_if<lease_record>(std::addressof(*existing));
                    lease != nullptr && lease->expires_ns > now)
                    return std::unexpected(catalog_error::contended("Petika compile lease is held"));
            }

            const auto token = next_token();
            const lease_record lease{token.id, now, now + lease_ttl_ns()};
            if (auto put = transaction.put(lease_key_value, petika_catalog_value{lease}); !put)
                return std::unexpected(catalog_error::backend("Petika compile lease write failed"));
            if (auto committed = transaction.commit(); !committed)
                return std::unexpected(catalog_error::contended("Petika compile lease transaction conflicted"));
            return token;
        }

        [[nodiscard]] std::expected<void, catalog_error>
        publish(const catalog_entry& entry, const utils::lease_token& token) {
            if (!token.valid()) return std::unexpected(catalog_error::backend("invalid compile lease"));
            auto transaction = store_->transaction();
            const auto lease_key_value = lease_key(entry.key);
            auto lease = transaction.get(lease_key_value);
            if (!lease || !owns(*lease, token))
                return std::unexpected(catalog_error::contended("Petika compile lease was lost"));
            if (auto put = transaction.put(entry_key(entry.key), petika_catalog_value{entry}); !put)
                return std::unexpected(catalog_error::backend("Petika artifact publish failed"));
            if (auto erase = transaction.erase(lease_key_value); !erase)
                return std::unexpected(catalog_error::backend("Petika compile lease release failed"));
            if (auto committed = transaction.commit(); !committed)
                return std::unexpected(catalog_error::backend("Petika artifact publish transaction failed"));
            return {};
        }

        [[nodiscard]] std::expected<void, catalog_error>
        abandon(const artifact_key& key, const utils::lease_token& token) {
            auto transaction = store_->transaction();
            const auto lease_key_value = lease_key(key);
            auto lease = transaction.get(lease_key_value);
            if (!lease || !owns(*lease, token)) return {};
            if (auto erased = transaction.erase(lease_key_value); !erased)
                return std::unexpected(catalog_error::backend("Petika compile lease release failed"));
            if (auto committed = transaction.commit(); !committed)
                return std::unexpected(catalog_error::backend("Petika compile lease transaction failed"));
            return {};
        }

        void touch(const artifact_key& key) {
            auto found = lookup(key);
            if (!found || !*found) return;
            auto entry = **found;
            entry.accessed_ns = now_ns();
            (void)store_->put(entry_key(key), petika_catalog_value{std::move(entry)});
        }

        [[nodiscard]] std::expected<void, catalog_error> evict(const artifact_key& key) {
            if (auto erased = store_->erase(entry_key(key)); !erased && erased.error() != petika::StorageError::NotFound)
                return std::unexpected(catalog_error::backend("Petika artifact eviction failed"));
            return {};
        }

        [[nodiscard]] std::vector<catalog_entry> list_evictable(const eviction_query& query) const {
            std::vector<catalog_entry> result;
            store_->for_each([&](const std::string& key, const petika_catalog_value& value) {
                if (!key.starts_with("a/") || result.size() >= query.max_entries) return;
                if (const auto* entry = std::get_if<catalog_entry>(std::addressof(value));
                    entry != nullptr && entry->accessed_ns < query.accessed_before_ns)
                    result.push_back(*entry);
            });
            return result;
        }

    private:
        [[nodiscard]] static std::uint64_t now_ns() noexcept {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        [[nodiscard]] std::uint64_t lease_ttl_ns() const noexcept {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(lease_ttl_).count());
        }

        [[nodiscard]] static utils::lease_token next_token() noexcept {
            static std::atomic<std::uint64_t> sequence{1};
            return {sequence.fetch_add(1, std::memory_order_relaxed)};
        }

        [[nodiscard]] static std::string digest_key(std::string_view prefix, const artifact_key& key) {
            constexpr char digits[] = "0123456789abcdef";
            const auto digest = compute_key_digest(key);
            std::string result{prefix};
            result.reserve(prefix.size() + digest.size() * 2);
            for (const auto byte : digest) {
                result.push_back(digits[(byte >> 4u) & 0x0fu]);
                result.push_back(digits[byte & 0x0fu]);
            }
            return result;
        }

        [[nodiscard]] static std::string entry_key(const artifact_key& key) { return digest_key("a/", key); }
        [[nodiscard]] static std::string lease_key(const artifact_key& key) { return digest_key("l/", key); }

        [[nodiscard]] static bool owns(const petika_catalog_value& value, const utils::lease_token& token) noexcept {
            const auto* lease = std::get_if<lease_record>(std::addressof(value));
            return lease != nullptr && lease->owner_id == token.id;
        }

        Store* store_;
        std::chrono::seconds lease_ttl_;
    };
} // namespace lithe::execution::store
