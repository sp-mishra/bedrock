#pragma once

// =============================================================================
// lithe_execution/resource.hpp — installed resource as owning lease
//
// Design decisions (, ):
//
//   • A resource is an OWNING LEASE over installed code.  shared_ptr<void> is
//     banned — resource identity and lifetime are explicit.
//   • dynamic_execution_result is declared BEFORE resource_ops so that the
//     ops table can reference the result type without forward-declaration games.
//   • any_installed_resource replaces shared_ptr<void>: it carries a backend
//     lifetime cookie, an owner ptr (type-erased), a generational resource_handle,
//     and a static const resource_ops* (no per-instance vtable allocation).
//   • execution_event OWNS/PINS the submitted resource until completion — it
//     holds the same lease.
//   • resource_store holds execution-side leases with NO lithe::rt dependency.
//     It owns compilation+installation metadata for non-managed execution ().
//   • The static path uses concrete resource_t<B,Artifact> and never allocates
//     an ops table (resource_ops is only the dynamic-boundary helper).
//
// invocation_result and invocation_request
//   • invocation_request is reserved for dynamic/plugin/interpreter-dynamic paths.
//   • For the typed static path, get_entry + a direct typed call is used (no request).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "foundation.hpp"   // execution_error, execution_event, ir_kind, backend_lifetime, …
#include "artifact.hpp"     // compilation_metadata, installation_metadata, code_version_metadata

namespace lithe::execution {
    // =========================================================================
    //  Dynamic invocation types
    //
    // invocation_request   — dynamic call descriptor (erased path, plugin, reflection)
    // invocation_result    — dynamic call result
    // dynamic_execution_result — the variant returned by resource_ops::invoke
    //
    // The typed static path (get_entry + direct typed call) constructs NONE of these.
    // =========================================================================

    // Erased invocation descriptor: a span of 64-bit register words plus a type cookie.
    struct invocation_request {
        std::span<const std::int64_t> args;
        std::uint32_t type_cookie = 0; // caller-defined function-id hint
    };

    // Result of a dynamic invocation.
    struct invocation_result {
        std::int64_t raw_value = 0;
        double fp_value = 0.0;
        bool is_fp = false;
        bool ok = false;

        [[nodiscard]] static invocation_result make_int(const std::int64_t v) noexcept {
            return {v, 0.0, false, true};
        }

        [[nodiscard]] static invocation_result make_fp(const double v) noexcept {
            return {0, v, true, true};
        }

        [[nodiscard]] static invocation_result make_error() noexcept { return {}; }
    };

    // dynamic_execution_result — MUST be declared before resource_ops.
    using dynamic_execution_result = std::variant<invocation_result, execution_event>;

    // =========================================================================
    //  resource_handle — generational stale-safe handle
    //
    // Lightweight 64-bit pair (index, generation).  Stale references can be
    // detected without locking.
    // =========================================================================

    struct resource_handle {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;

        [[nodiscard]] constexpr bool valid() const noexcept {
            return generation != 0;
        }

        [[nodiscard]] constexpr bool operator==(const resource_handle&) const noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<resource_handle>);

    // =========================================================================
    //  resource_ops — static dispatch table for the erased boundary
    //
    // Only the dynamic boundary allocates one of these; the static path uses
    // concrete resource_t<B,Artifact> directly and pays zero overhead.
    // =========================================================================

    struct resource_ops {
        // destroy: release the underlying resource.  Called exactly once (by the
        // owning any_installed_resource destructor or move assignment).
        // handle identifies which slot to release — required for pooled backends
        // managing multiple installed resources from one ops table ().
        void (*destroy)(void* owner_ptr, resource_handle handle) noexcept = nullptr;

        // invoke: erased dynamic call.  Returns error if the resource is not callable.
        std::expected<dynamic_execution_result, execution_error>
        (*invoke)(void* owner_ptr, invocation_request) = nullptr;
    };

    // =========================================================================
    //  any_installed_resource — the erased dynamic lease
    //
    // Replaces shared_ptr<void>.  Ownership rules:
    //   • Live-resource refcount gates unregister.
    //   • Move nulls ops_ + owner_ptr_ (moved-from resource is empty/invalid).
    //   • Async execution_events defer destruction by holding the same lease via
    //     pinned_resource_event below.
    // =========================================================================

    class any_installed_resource {
    public:
        any_installed_resource() = default;

        // No copy — a resource is a unique ownership lease.
        any_installed_resource(const any_installed_resource&) = delete;
        any_installed_resource& operator=(const any_installed_resource&) = delete;

        any_installed_resource(any_installed_resource&& o) noexcept
            : owner_ptr_(std::exchange(o.owner_ptr_, nullptr))
              , ops_(std::exchange(o.ops_, nullptr))
              , handle_(std::exchange(o.handle_, {}))
              , backend_lifetime_(std::move(o.backend_lifetime_)) {}

        any_installed_resource& operator=(any_installed_resource&& o) noexcept {
            if (this != &o) {
                release_internal();
                owner_ptr_ = std::exchange(o.owner_ptr_, nullptr);
                ops_ = std::exchange(o.ops_, nullptr);
                handle_ = std::exchange(o.handle_, {});
                backend_lifetime_ = std::move(o.backend_lifetime_);
            }
            return *this;
        }

        ~any_installed_resource() { release_internal(); }

        // Factory: wrap a heap-allocated owner with its ops table.
        template <class Owner>
        [[nodiscard]] static any_installed_resource
        make(Owner* owner_ptr, const resource_ops* ops,
             const resource_handle handle,
             std::shared_ptr<backend_lifetime> lifetime = nullptr) noexcept {
            any_installed_resource r;
            r.owner_ptr_ = static_cast<void*>(owner_ptr);
            r.ops_ = ops;
            r.handle_ = handle;
            r.backend_lifetime_ = std::move(lifetime);
            return r;
        }

        // Legacy overload: accepts uint64_t backend_lifetime cookie (now ignored).
        // Callers should migrate to passing shared_ptr<backend_lifetime>.
        template <class Owner>
        [[nodiscard]] static any_installed_resource
        make(Owner* owner_ptr, const resource_ops* ops,
             const resource_handle handle, std::uint64_t /*legacy_cookie*/) noexcept {
            return make(owner_ptr, ops, handle);
        }

        [[nodiscard]] bool valid() const noexcept {
            return owner_ptr_ != nullptr && ops_ != nullptr && handle_.valid();
        }

        [[nodiscard]] resource_handle handle() const noexcept { return handle_; }

        [[nodiscard]] const std::shared_ptr<backend_lifetime>&
        lifetime_block() const noexcept { return backend_lifetime_; }

        [[nodiscard]] std::expected<dynamic_execution_result, execution_error>
        invoke(invocation_request req) const {
            if (!valid() || ops_->invoke == nullptr)
                return std::unexpected(execution_error{"resource not invocable"});
            return ops_->invoke(owner_ptr_, req);
        }

    private:
        void release_internal() noexcept {
            if (owner_ptr_ && ops_ && ops_->destroy) {
                ops_->destroy(owner_ptr_, handle_);
            }
            if (backend_lifetime_) {
                backend_lifetime_->unpin();
                backend_lifetime_.reset();
            }
            owner_ptr_ = nullptr;
            ops_ = nullptr;
            handle_ = {};
        }

        void* owner_ptr_ = nullptr;
        const resource_ops* ops_ = nullptr;
        resource_handle handle_ = {};
        std::shared_ptr<backend_lifetime> backend_lifetime_;
    };

    static_assert(std::is_move_constructible_v<any_installed_resource>);
    static_assert(!std::is_copy_constructible_v<any_installed_resource>);

    // =========================================================================
    //  /  async pinned event — defers resource destruction
    //
    // An execution_event that completes asynchronously may hold a resource alive
    // past the point where the caller has released its own lease.
    // pinned_resource_event is the mechanism: it owns an any_installed_resource
    // and releases it when the completion notifier fires.
    // =========================================================================

    class pinned_resource_event {
    public:
        pinned_resource_event() = default;

        explicit pinned_resource_event(any_installed_resource res, const execution_event ev)
            : res_(std::move(res)), event_(ev) {}

        [[nodiscard]] execution_event event() const noexcept { return event_; }

        // Release ownership back to the caller (e.g. completion handler).
        [[nodiscard]] any_installed_resource release() && noexcept {
            return std::move(res_);
        }

        [[nodiscard]] bool valid() const noexcept { return event_.valid() && res_.valid(); }

    private:
        any_installed_resource res_;
        execution_event event_{};
    };

    // =========================================================================
    //  resource_store — execution-side lease registry (NO lithe::rt dep)
    //
    // Holds compilation+installation metadata for non-managed execution.
    // Concurrency is the CALLER's responsibility (no internal locking).
    // =========================================================================

    class resource_store {
    public:
        struct entry {
            any_installed_resource resource;
            code_version_metadata metadata;
        };

        resource_store() = default;

        // resource_store is not copyable (resources are unique ownership).
        resource_store(const resource_store&) = delete;
        resource_store& operator=(const resource_store&) = delete;
        resource_store(resource_store&&) = default;
        resource_store& operator=(resource_store&&) = default;

        // Insert a resource.  Returns its assigned handle.
        resource_handle insert(any_installed_resource res,
                               code_version_metadata meta = {}) {
            const resource_handle h{next_index_++, ++generation_bump_};
            entries_.emplace(encode(h), entry{std::move(res), std::move(meta)});
            return h;
        }

        // Lookup by handle.  Returns nullptr if not found or stale.
        [[nodiscard]] entry* find(const resource_handle h) noexcept {
            const auto it = entries_.find(encode(h));
            return it == entries_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] const entry* find(const resource_handle h) const noexcept {
            const auto it = entries_.find(encode(h));
            return it == entries_.end() ? nullptr : &it->second;
        }

        // Erase a handle.  No-op if not found.
        bool erase(const resource_handle h) {
            return entries_.erase(encode(h)) > 0;
        }

        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    private:
        [[nodiscard]] static std::uint64_t encode(const resource_handle h) noexcept {
            return (static_cast<std::uint64_t>(h.generation) << 32) | h.index;
        }

        std::unordered_map<std::uint64_t, entry> entries_;
        std::uint32_t next_index_ = 1;
        std::uint32_t generation_bump_ = 0;
    };
} // namespace lithe::execution
