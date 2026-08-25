#pragma once

// =============================================================================
// lithe_execution/entry.hpp — entry_lease, invocation_guard, typed_entry
//
// Two distinct concepts ():
//
//   entry_lease      — keeps code *storage* alive (i.e. keeps the resource live),
//                      but does NOT increment the active-frame counter.
//                      Valid for the lifetime of the owning resource.
//
//   invocation_guard — RAII guard that increments the active-frame counter on
//                      construction and decrements on destruction.  Lifetime is
//                      only the duration of one call.
//
// typed_entry<Sig>   — holds an entry_lease + a typed callable.
//                      A direct typed call through typed_entry raises and drops
//                      an invocation_guard internally (no invocation_request packing).
//
// active_frame_counter — a thin std::atomic<std::uint64_t> wrapper that serves
//   as the authoritative frame counter for the static path.  For managed code
//   (lithe_rt), code_resource::active_frames is the authoritative counter;
//   the adapter header wires this value via a shared_ptr reference.
//
// invocation_request / invocation_result are reserved for the dynamic-registry /
// interpreter-dynamic / plugin / reflection paths (defined in resource.hpp).
// The typed static path NEVER constructs an invocation_request.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "foundation.hpp"  // execution_error

namespace lithe::execution {
    // =========================================================================
    //  active_frame_counter
    //
    // The authoritative counter for the static execution path.
    // Stored as a shared_ptr<atomic<uint64_t>> so that an entry_lease can hold
    // a reference to the same counter as the resource that owns the code storage,
    // even if the resource itself is moved or the backend replaces it.
    // =========================================================================

    using frame_counter_ref = std::shared_ptr<std::atomic<std::uint64_t>>;

    [[nodiscard]] inline frame_counter_ref make_frame_counter() {
        return std::make_shared<std::atomic<std::uint64_t>>(0);
    }

    // =========================================================================
    //  invocation_guard — RAII frame-counter increment
    //
    // Increment on construction; decrement on destruction.
    // Only raised for the duration of one call, NOT for the entry_lease lifetime.
    // =========================================================================

    class invocation_guard {
    public:
        // Non-owning: counter_ must outlive this guard.
        explicit invocation_guard(std::atomic<std::uint64_t>& counter) noexcept
            : counter_(&counter) {
            counter_->fetch_add(1, std::memory_order_acq_rel);
        }

        // Also accept via frame_counter_ref (shared ownership).
        explicit invocation_guard(const frame_counter_ref& ref) noexcept
            : counter_(ref.get()), ref_(ref) {
            if (counter_) counter_->fetch_add(1, std::memory_order_acq_rel);
        }

        invocation_guard(const invocation_guard&) = delete;
        invocation_guard& operator=(const invocation_guard&) = delete;

        invocation_guard(invocation_guard&& o) noexcept
            : counter_(std::exchange(o.counter_, nullptr))
              , ref_(std::move(o.ref_)) {}

        ~invocation_guard() noexcept {
            if (counter_)
                counter_->fetch_sub(1, std::memory_order_acq_rel);
        }

        [[nodiscard]] std::uint64_t active_frames() const noexcept {
            return counter_ ? counter_->load(std::memory_order_acquire) : 0;
        }

    private:
        std::atomic<std::uint64_t>* counter_ = nullptr;
        frame_counter_ref ref_; // optional shared ownership
    };

    // =========================================================================
    //  entry_lease — keeps code storage alive, does NOT count frames
    //
    // Holds a shared reference to the frame_counter (so the resource cannot be
    // retired while the lease is live), but does NOT increment it.
    //
    // The lease is valid as long as the issuing resource is alive.
    // =========================================================================

    class entry_lease {
    public:
        entry_lease() = default;

        explicit entry_lease(frame_counter_ref counter,
                             std::uint64_t version = 0) noexcept
            : counter_(std::move(counter)), version_(version) {}

        // True if the lease is associated with a live frame_counter.
        [[nodiscard]] bool valid() const noexcept { return counter_ != nullptr; }

        // Raise an invocation guard from this lease's counter.
        // The guard's lifetime is independent of the lease's lifetime.
        [[nodiscard]] invocation_guard raise_guard() const noexcept {
            assert(valid() && "entry_lease::raise_guard() on invalid lease");
            return invocation_guard{counter_};
        }

        [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

        [[nodiscard]] const frame_counter_ref& counter_ref() const noexcept {
            return counter_;
        }

    private:
        frame_counter_ref counter_;
        std::uint64_t version_ = 0;
    };

    // =========================================================================
    //  typed_entry<Sig> — entry_lease + typed callable
    //
    // Sig is a function signature type, e.g. int64_t(int64_t, int64_t).
    //
    // Usage:
    //   auto e = cpo::get_entry(backend, resource, type_tag<Sig>{});
    //   if (e) {
    //       auto result = (*e)(args...);  // guard raised for this call only
    //   }
    //
    // The operator() raises an invocation_guard for the duration of the call.
    // The entry_lease (code-storage pin) persists across calls.
    //
    // NOTE: The typed path never packs an invocation_request.
    // =========================================================================

    template <class Sig>
    class typed_entry;

    template <class Ret, class... Args>
    class typed_entry<Ret(Args...)> {
    public:
        using signature_type = Ret(Args...);
        using callable_type = std::function<Ret(Args...)>;

        typed_entry() = default;

        // Construct from a lease + typed callable.
        explicit typed_entry(entry_lease lease, callable_type fn)
            : lease_(std::move(lease)), fn_(std::move(fn)) {}

        [[nodiscard]] bool valid() const noexcept {
            return lease_.valid() && static_cast<bool>(fn_);
        }

        // Direct typed call: raises invocation_guard for this call only.
        Ret operator()(Args... args) const {
            assert(valid() && "typed_entry: call on invalid entry");
            [[maybe_unused]] auto guard = lease_.raise_guard();
            return fn_(std::forward<Args>(args)...);
        }

        [[nodiscard]] const entry_lease& lease() const noexcept { return lease_; }

        // Check active frames at the counter level (diagnostic / test use).
        [[nodiscard]] std::uint64_t active_frames() const noexcept {
            const auto* c = lease_.counter_ref().get();
            return c ? c->load(std::memory_order_acquire) : 0;
        }

    private:
        entry_lease lease_;
        callable_type fn_;
    };

    // Deduction helper — not usually needed but provided for symmetry.
    template <class Ret, class... Args>
    typed_entry(entry_lease, std::function<Ret(Args...)>) -> typed_entry<Ret(Args...)>;

    // =========================================================================
    // Static-path compile-time assertion:
    //
    // The typed path must not construct invocation_request.
    // This is enforced structurally: typed_entry<Sig>::operator() takes Args...
    // directly and never packs them into invocation_request.  The test suite
    // verifies this by checking the call expression compiles with no
    // invocation_request in scope.
    // =========================================================================
} // namespace lithe::execution
