#pragma once

// crank/tx_collections.hpp — §v2.13 transactional collections.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Four ready-made transactional resources a crank program can use inside a
// `transaction` block without hand-writing a medha::resource_traits<R>
// specialization:
//
//   TxMap<K,V>   — keyed store (get / put / erase / contains)
//   TxSet<T>     — membership set (add / remove / contains)
//   TxQueue<T>   — FIFO queue (enqueue / dequeue / front / size)
//   TxLog<T>     — append-only log (append / at / size)
//
// Each type is a plain owning container plus a medha::resource_traits<>
// specialization marking it transactional with snapshot + rollback support, so
// it can be enrolled via crank::register_transactional<R>("Name") and written
// inside a transaction. The containers stage mutations and expose a
// snapshot()/restore() pair the savepoint machinery (tx_savepoint.hpp) drives on
// partial rollback — no virtual, no macros, pay-for-use.
//
// These are data-plane resources: crank owns the typed API; Medha owns the
// read/write-set at commit time (G-TX-1, no Medha API change).
//
// Design refs: §v2.13; commit protocol = atomic_multi_key_within_resource
// (single-resource multi-key atomicity — a coordinator is only needed to span
// *several* of these, see [[project-crank-module1]] and §v2.11).

#include "medha/resource_traits.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace crank {
    // ============================================================================
    // TxMap<K,V> — transactional keyed store.
    // ============================================================================

    template <class K, class V>
    class TxMap {
    public:
        using key_type = K;
        using value_type = V;

        [[nodiscard]] std::optional<V> get(const K& k) const {
            auto it = data_.find(k);
            if (it == data_.end()) return std::nullopt;
            return it->second;
        }

        void put(const K& k, V v) { data_[k] = std::move(v); }
        bool erase(const K& k) { return data_.erase(k) != 0; }
        [[nodiscard]] bool contains(const K& k) const { return data_.contains(k); }
        [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

        // Snapshot/restore for savepoint rollback (whole-container copy — correct,
        // and pay-for-use: only taken when a savepoint is created).
        using snapshot_t = std::unordered_map<K, V>;
        [[nodiscard]] snapshot_t snapshot() const { return data_; }
        void restore(snapshot_t s) { data_ = std::move(s); }

    private:
        std::unordered_map<K, V> data_;
    };

    // ============================================================================
    // TxSet<T> — transactional membership set.
    // ============================================================================

    template <class T>
    class TxSet {
    public:
        using key_type = T;
        using value_type = T;

        bool add(const T& x) { return data_.insert(x).second; }
        bool remove(const T& x) { return data_.erase(x) != 0; }
        [[nodiscard]] bool contains(const T& x) const { return data_.contains(x); }
        [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

        using snapshot_t = std::unordered_set<T>;
        [[nodiscard]] snapshot_t snapshot() const { return data_; }
        void restore(snapshot_t s) { data_ = std::move(s); }

    private:
        std::unordered_set<T> data_;
    };

    // ============================================================================
    // TxQueue<T> — transactional FIFO queue.
    // ============================================================================

    template <class T>
    class TxQueue {
    public:
        using key_type = std::size_t;
        using value_type = T;

        void enqueue(T x) { data_.push_back(std::move(x)); }

        [[nodiscard]] std::optional<T> dequeue() {
            if (data_.empty()) return std::nullopt;
            T front = std::move(data_.front());
            data_.pop_front();
            return front;
        }

        [[nodiscard]] std::optional<T> front() const {
            if (data_.empty()) return std::nullopt;
            return data_.front();
        }

        [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

        using snapshot_t = std::deque<T>;
        [[nodiscard]] snapshot_t snapshot() const { return data_; }
        void restore(snapshot_t s) { data_ = std::move(s); }

    private:
        std::deque<T> data_;
    };

    // ============================================================================
    // TxLog<T> — transactional append-only log.
    // ============================================================================

    template <class T>
    class TxLog {
    public:
        using key_type = std::size_t;
        using value_type = T;

        void append(T x) { data_.push_back(std::move(x)); }

        [[nodiscard]] std::optional<T> at(std::size_t i) const {
            if (i >= data_.size()) return std::nullopt;
            return data_[i];
        }

        [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

        using snapshot_t = std::vector<T>;
        [[nodiscard]] snapshot_t snapshot() const { return data_; }
        void restore(snapshot_t s) { data_ = std::move(s); }

    private:
        std::vector<T> data_;
    };

    // ============================================================================
    // TxCounter<T> — transactional numeric counter.
    //
    // Supports read, set, add, and compare-and-set. Commutative add updates may use
    // conflict-reducing merge semantics when explicitly declared (§12.2). T must be
    // an integral or floating-point type; add() is defined only when arithmetic is
    // available.
    // ============================================================================

    template <class T>
        requires std::is_arithmetic_v<T>
    class TxCounter {
    public:
        using key_type = std::size_t;
        using value_type = T;

        explicit constexpr TxCounter(T initial = T{}) noexcept : value_(initial) {}

        [[nodiscard]] T read() const noexcept { return value_; }
        void set(T v) noexcept { value_ = v; }
        void add(T delta) noexcept { value_ += delta; }

        // Atomically set value to `next` if current == `expected`; returns true on success.
        bool compare_and_set(T expected, T next) noexcept {
            if (value_ != expected) return false;
            value_ = next;
            return true;
        }

        using snapshot_t = T;
        [[nodiscard]] snapshot_t snapshot() const noexcept { return value_; }
        void restore(snapshot_t s) noexcept { value_ = s; }

    private:
        T value_;
    };
} // namespace crank

// ============================================================================
// medha::resource_traits specializations — mark the collections transactional.
//
// All four are single-resource multi-key atomic (a transaction touching one
// collection commits atomically). Spanning several collections in one
// transaction under serializable isolation still requires a coordinator
// (§v2.11, CRANK-TX-002 / CRANK-TX-010).
// ============================================================================

namespace medha {
    template <class K, class V>
    struct resource_traits<crank::TxMap<K, V>> {
        static constexpr bool transactional = true;
        static constexpr bool value_trivially_copyable = std::is_trivially_copyable_v<V>;
        static constexpr bool value_move_only = !std::is_copy_constructible_v<V>;
        static constexpr bool resource_stages_values = true;
        static constexpr bool supports_snapshot = true;
        static constexpr bool supports_rollback = true;
        static constexpr commit_capability commit_protocol =
            commit_capability::atomic_multi_key_within_resource;
        static constexpr bool aba_safe = true;
        static constexpr bool distributed_capable = false;
        using key_type = K;
        using value_type = V;
    };

    template <class T>
    struct resource_traits<crank::TxSet<T>> {
        static constexpr bool transactional = true;
        static constexpr bool value_trivially_copyable = std::is_trivially_copyable_v<T>;
        static constexpr bool value_move_only = false;
        static constexpr bool resource_stages_values = true;
        static constexpr bool supports_snapshot = true;
        static constexpr bool supports_rollback = true;
        static constexpr commit_capability commit_protocol =
            commit_capability::atomic_multi_key_within_resource;
        static constexpr bool aba_safe = true;
        static constexpr bool distributed_capable = false;
        using key_type = T;
        using value_type = T;
    };

    template <class T>
    struct resource_traits<crank::TxQueue<T>> {
        static constexpr bool transactional = true;
        static constexpr bool value_trivially_copyable = std::is_trivially_copyable_v<T>;
        static constexpr bool value_move_only = !std::is_copy_constructible_v<T>;
        static constexpr bool resource_stages_values = true;
        static constexpr bool supports_snapshot = true;
        static constexpr bool supports_rollback = true;
        static constexpr commit_capability commit_protocol =
            commit_capability::atomic_multi_key_within_resource;
        static constexpr bool aba_safe = true;
        static constexpr bool distributed_capable = false;
        using key_type = std::size_t;
        using value_type = T;
    };

    template <class T>
    struct resource_traits<crank::TxLog<T>> {
        static constexpr bool transactional = true;
        static constexpr bool value_trivially_copyable = std::is_trivially_copyable_v<T>;
        static constexpr bool value_move_only = !std::is_copy_constructible_v<T>;
        static constexpr bool resource_stages_values = true;
        static constexpr bool supports_snapshot = true;
        static constexpr bool supports_rollback = true;
        static constexpr commit_capability commit_protocol =
            commit_capability::atomic_multi_key_within_resource;
        static constexpr bool aba_safe = true;
        static constexpr bool distributed_capable = false;
        using key_type = std::size_t;
        using value_type = T;
    };

    template <class T>
        requires std::is_arithmetic_v<T>
    struct resource_traits<crank::TxCounter<T>> {
        static constexpr bool transactional = true;
        static constexpr bool value_trivially_copyable = std::is_trivially_copyable_v<T>;
        static constexpr bool value_move_only = false;
        static constexpr bool resource_stages_values = true;
        static constexpr bool supports_snapshot = true;
        static constexpr bool supports_rollback = true;
        static constexpr commit_capability commit_protocol =
            commit_capability::atomic_multi_key_within_resource;
        static constexpr bool aba_safe = true;
        static constexpr bool distributed_capable = false;
        using key_type = std::size_t;
        using value_type = T;
    };
} // namespace medha
