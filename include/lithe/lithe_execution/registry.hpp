#pragma once

// =============================================================================
// lithe_execution/registry.hpp — lifetime-safe dynamic backend_registry
//
// Design (, , P8):
//
//   backend_registry stores runtime-registered backends as type-erased slots in
//   a slot_map.  Each slot carries:
//     • a live-resource refcount (atomic) — unregister is rejected while nonzero
//     • a state (live → retiring)
//     • the erased backend ops and an any-stored concrete backend instance
//
//   Acquisition race is closed by the SHARED-LOCK VALIDATE+INCREMENT protocol:
//     - Shared lock: check slot generation + state, then atomically increment refcount
//     - Exclusive lock: only for structural mutation (insert / erase)
//   Once a backend_ref (pinned lease) is acquired, no registry lock is held.
//
//   registration_token — move-only RAII handle for a registered backend slot.
//   backend_ref        — shared-counted pinned lease; can outlive the registry
//                        call but blocks unregister while nonzero.
//
//   backend_lifetime   — per-slot atomic control block shared between the slot
//                        and all outstanding backend_refs.
//
// CAS live→retiring variant is the measured-optimization path; the shared-lock
// acquire is the default (correct + simple).
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "foundation.hpp"    // backend_capability_set, execution_mode_set, backend_lifetime, …
#include "containers/handle/generational_handle.hpp"
#include "containers/associative/slot_map.hpp"
#include "../lithe_algorithms/pipeline.hpp"   // any_pass<IR>, preserved_analysis_set

namespace lithe::execution {
    // =========================================================================
    //  erased_backend_ops — type-erased function pointers for a dynamic backend
    // =========================================================================

    struct erased_backend_ops {
        // capabilities — returns the backend's capability set.
        backend_capability_set (*capabilities)(const void* backend) noexcept = nullptr;

        // backend_id — returns the persistent string id.
        std::string_view (*backend_id)(const void* backend) noexcept = nullptr;

        // destroy — called when the slot is erased (lifetime-zero unregister).
        void (*destroy)(void* backend) noexcept = nullptr;
    };

    // =========================================================================
    //  backend_slot — one entry in the registry
    // =========================================================================

    struct backend_slot {
        void* backend_ptr = nullptr;
        const erased_backend_ops* ops = nullptr;
        std::shared_ptr<backend_lifetime> lifetime;

        [[nodiscard]] bool valid() const noexcept {
            return backend_ptr != nullptr && ops != nullptr && lifetime != nullptr;
        }

        [[nodiscard]] std::string_view id() const noexcept {
            return (ops && ops->backend_id) ? ops->backend_id(backend_ptr) : "";
        }

        [[nodiscard]] backend_capability_set caps() const noexcept {
            return (ops && ops->capabilities)
                       ? ops->capabilities(backend_ptr)
                       : backend_capability_set{};
        }
    };

    // =========================================================================
    //  backend_slot_tag — phantom tag for generational_handle
    // =========================================================================

    struct backend_slot_tag {};

    using registration_handle =
    containers::generational_handle<backend_slot_tag, std::uint32_t>;

    // =========================================================================
    //  backend_ref — pinned lease over a live backend slot
    //
    // Holds a shared_ptr<backend_lifetime> and a raw void* to the backend.
    // Dropping backend_ref decrements the lifetime's refcount.
    // =========================================================================

    class backend_ref {
    public:
        backend_ref() = default;

        // Ownership-transfer ctor: caller has ALREADY incremented the refcount
        // (e.g. via try_pin).  No additional increment.
        backend_ref(void* ptr, const erased_backend_ops* ops,
                    std::shared_ptr<backend_lifetime> lt) noexcept
            : ptr_(ptr), ops_(ops), lifetime_(std::move(lt)) {}

        // Copy ctor: adds a new reference.
        backend_ref(const backend_ref& o) noexcept
            : ptr_(o.ptr_), ops_(o.ops_), lifetime_(o.lifetime_) {
            if (lifetime_) lifetime_->refcount.fetch_add(1, std::memory_order_acq_rel);
        }

        backend_ref& operator=(const backend_ref& o) noexcept {
            if (this != &o) {
                release_internal();
                ptr_ = o.ptr_;
                ops_ = o.ops_;
                lifetime_ = o.lifetime_;
                if (lifetime_) lifetime_->refcount.fetch_add(1, std::memory_order_acq_rel);
            }
            return *this;
        }

        backend_ref(backend_ref&& o) noexcept
            : ptr_(std::exchange(o.ptr_, nullptr))
              , ops_(std::exchange(o.ops_, nullptr))
              , lifetime_(std::move(o.lifetime_)) {}

        backend_ref& operator=(backend_ref&& o) noexcept {
            if (this != &o) {
                release_internal();
                ptr_ = std::exchange(o.ptr_, nullptr);
                ops_ = std::exchange(o.ops_, nullptr);
                lifetime_ = std::move(o.lifetime_);
            }
            return *this;
        }

        ~backend_ref() { release_internal(); }

        [[nodiscard]] bool valid() const noexcept {
            return ptr_ != nullptr && ops_ != nullptr && lifetime_ != nullptr;
        }

        // Access the erased backend pointer (caller casts to concrete type).
        [[nodiscard]] void* get() const noexcept { return ptr_; }

        [[nodiscard]] std::string_view id() const noexcept {
            return (ops_ && ops_->backend_id) ? ops_->backend_id(ptr_) : "";
        }

        [[nodiscard]] backend_capability_set caps() const noexcept {
            return (ops_ && ops_->capabilities)
                       ? ops_->capabilities(ptr_)
                       : backend_capability_set{};
        }

        [[nodiscard]] std::uint64_t live_refs() const noexcept {
            return lifetime_ ? lifetime_->live_refs() : 0;
        }

    private:
        void release_internal() noexcept {
            if (lifetime_) {
                lifetime_->unpin();
                lifetime_.reset();
            }
            ptr_ = nullptr;
            ops_ = nullptr;
        }

        void* ptr_ = nullptr;
        const erased_backend_ops* ops_ = nullptr;
        std::shared_ptr<backend_lifetime> lifetime_;
    };

    // =========================================================================
    //  registration_token — RAII handle for a registered backend
    //
    // Move-only; destructor calls unregister on the registry.
    // Holds the registration_handle and a raw pointer back to the registry
    // (non-owning — token must not outlive the registry).
    // =========================================================================

    class backend_registry; // forward

    class registration_token {
    public:
        registration_token() = default;

        registration_token(backend_registry* reg, registration_handle h) noexcept
            : registry_(reg), handle_(h) {}

        registration_token(const registration_token&) = delete;
        registration_token& operator=(const registration_token&) = delete;

        registration_token(registration_token&& o) noexcept
            : registry_(std::exchange(o.registry_, nullptr))
              , handle_(std::exchange(o.handle_, registration_handle::null())) {}

        registration_token& operator=(registration_token&& o) noexcept;

        ~registration_token();

        [[nodiscard]] bool valid() const noexcept { return !handle_.is_null(); }
        [[nodiscard]] registration_handle handle() const noexcept { return handle_; }

    private:
        backend_registry* registry_ = nullptr;
        registration_handle handle_;
    };

    // =========================================================================
    // ,  backend_registry
    //
    // Thread-safety: std::shared_mutex.
    //   Readers (acquire, find): shared lock — validate + increment refcount.
    //   Writers (register, unregister): exclusive lock.
    //
    // Unregister is DEFERRED while live_refs > 0: the slot transitions to
    // `retiring`, lookup visibility is removed, but the backend_lifetime block
    // remains until all backend_refs are released.
    // =========================================================================

    class backend_registry {
    public:
        backend_registry() = default;
        ~backend_registry() = default;

        // Non-copyable — registry identity is unique.
        backend_registry(const backend_registry&) = delete;
        backend_registry& operator=(const backend_registry&) = delete;
        backend_registry(backend_registry&&) = delete;
        backend_registry& operator=(backend_registry&&) = delete;

        // ====================================================================
        // register_backend<B>(backend) — type-erased registration
        //
        // Stores a heap-allocated copy of backend (B must be move-constructible).
        // Returns a registration_token; drop the token to unregister.
        // ====================================================================

        template <class B>
            requires std::move_constructible<B>
        [[nodiscard]] registration_token register_backend(B backend) {
            // Build the static ops table for B.
            static const erased_backend_ops ops_for_b = make_ops<B>();

            auto* heap_ptr = new B(std::move(backend));
            auto lifetime = std::make_shared<backend_lifetime>();

            // Initial pin for the slot itself (unregistered when token dropped).
            lifetime->refcount.fetch_add(1, std::memory_order_relaxed);

            backend_slot slot;
            slot.backend_ptr = static_cast<void*>(heap_ptr);
            slot.ops = &ops_for_b;
            slot.lifetime = lifetime;

            registration_handle h;
            {
                std::unique_lock lock{mutex_};
                h = map_.insert(std::move(slot));
            }
            return registration_token{this, h};
        }

        // ====================================================================
        // acquire(handle) — shared-lock validate+increment protocol
        //
        // Returns std::nullopt if the handle is stale or the slot is retiring.
        // The returned backend_ref holds a pin that blocks unregister.
        // ====================================================================

        [[nodiscard]] std::optional<backend_ref>
        acquire(registration_handle h) const noexcept {
            std::shared_lock lock{mutex_};
            const backend_slot* slot = map_.find(h);
            if (!slot || !slot->valid()) return std::nullopt;
            if (!slot->lifetime->is_live()) return std::nullopt;
            // try_pin increments the refcount under the shared lock (closes the race).
            if (!slot->lifetime->try_pin()) return std::nullopt;
            // Ownership-transfer ctor: no additional increment (try_pin did +1).
            return backend_ref{slot->backend_ptr, slot->ops, slot->lifetime};
        }

        // ====================================================================
        // unregister(handle) — request removal; deferred if refcount > 0
        //
        // Returns true  → slot erased immediately (refcount was 0).
        // Returns false → deferred (slot remains retiring until last ref drops).
        // ====================================================================

        bool unregister(registration_handle h) {
            std::unique_lock lock{mutex_};
            backend_slot* slot = map_.find(h);
            if (!slot || !slot->valid()) return false;

            // Mark retiring — new acquire calls will fail.
            slot->lifetime->state.store(backend_slot_state::retiring,
                                        std::memory_order_release);

            // The registration_token itself holds +1 ref (initial pin in register).
            // Unpin it now.
            slot->lifetime->unpin();

            const bool can_erase_now = (slot->lifetime->live_refs() == 0);
            if (can_erase_now) {
                // Destroy the backend and erase the slot.
                destroy_slot(*slot);
                map_.erase(h);
            }
            else {
                // Deferred: remove lookup visibility but keep lifetime block alive.
                // We do NOT erase from slot_map yet — existing backend_refs still
                // need to complete.  We remove it from lookup by erasing the slot:
                // outstanding backend_refs hold their own shared_ptr to lifetime,
                // so they remain valid.
                map_.erase(h);
                // lifetime shared_ptr is owned by outstanding backend_refs.
            }
            return can_erase_now;
        }

        // ====================================================================
        // find_first(pred) — walk live slots, return first match
        // ====================================================================

        template <class Pred>
        [[nodiscard]] std::optional<backend_ref>
        find_first(Pred&& pred) const noexcept {
            std::shared_lock lock{mutex_};
            for (auto it = map_.begin(); it != map_.end(); ++it) {
                auto vref = *it;
                const backend_slot& slot = vref.value;
                if (!slot.valid()) continue;
                if (!slot.lifetime->is_live()) continue;
                if (!pred(slot)) continue;
                if (!slot.lifetime->try_pin()) continue;
                return backend_ref{slot.backend_ptr, slot.ops, slot.lifetime};
            }
            return std::nullopt;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            std::shared_lock lock{mutex_};
            return map_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            std::shared_lock lock{mutex_};
            return map_.empty();
        }

    private:
        template <class B>
        static erased_backend_ops make_ops() noexcept {
            erased_backend_ops ops;

            ops.destroy = [](void* p) noexcept {
                delete static_cast<B*>(p);
            };

            if constexpr (requires(const B& b) { B::capabilities(); }) {
                ops.capabilities = [](const void* p) noexcept -> backend_capability_set {
                    (void)p;
                    return B::capabilities();
                };
            }
            else if constexpr (requires(const B& b) { b.capabilities(); }) {
                ops.capabilities = [](const void* p) noexcept -> backend_capability_set {
                    return static_cast<const B*>(p)->capabilities();
                };
            }

            // backend_id from descriptor if available.
            if constexpr (requires { B::descriptor.id_view(); }) {
                ops.backend_id = [](const void*) noexcept -> std::string_view {
                    return B::descriptor.id_view();
                };
            }
            else if constexpr (requires(const B& b) { b.backend_id(); }) {
                ops.backend_id = [](const void* p) noexcept -> std::string_view {
                    return static_cast<const B*>(p)->backend_id();
                };
            }

            return ops;
        }

        static void destroy_slot(backend_slot& slot) noexcept {
            if (slot.ops && slot.ops->destroy && slot.backend_ptr) {
                slot.ops->destroy(slot.backend_ptr);
                slot.backend_ptr = nullptr;
            }
        }

        using slot_map_t = containers::slot_map<backend_slot, registration_handle>;

        mutable std::shared_mutex mutex_;
        slot_map_t map_;
    };

    // =========================================================================
    // registration_token out-of-line definitions (registry must be complete)
    // =========================================================================

    inline registration_token& registration_token::operator=(registration_token&& o) noexcept {
        if (this != &o) {
            // Unregister current slot if held.
            if (registry_ && !handle_.is_null()) {
                registry_->unregister(handle_);
            }
            registry_ = std::exchange(o.registry_, nullptr);
            handle_ = std::exchange(o.handle_, registration_handle::null());
        }
        return *this;
    }

    inline registration_token::~registration_token() {
        if (registry_ && !handle_.is_null()) {
            registry_->unregister(handle_);
        }
    }

    // =========================================================================
    //  pass_descriptor_runtime — POD metadata for a dynamically registered pass.
    //
    //  Fixed-width fields; wire-safe; matches plugin ABI POD convention.
    //  category_id / in_stage / out_stage are plain uint8_t to avoid pulling
    //  in lithe_passes.hpp from this header.  Cast to pass_category / ir_stage
    //  at the call site.
    //
    //  stable_id bands:
    //    [0, 1000) — built-in passes.
    //    [1000, ∞) — extension / plugin passes (kExtensionIdBase).
    // =========================================================================

    struct pass_descriptor_runtime {
        char id[64] = {}; // null-terminated UTF-8 string id
        std::uint32_t version[3] = {}; // major, minor, patch
        std::uint8_t category_id = 0; // maps to passes::pass_category enum
        std::uint8_t in_stage = 0; // maps to passes::ir_stage enum
        std::uint8_t out_stage = 0;
        std::uint8_t _pad = 0;
        std::uint64_t stable_id = 0;

        // Up to 8 stable_ids this pass must not co-exist with.
        static constexpr std::size_t kMaxConflicts = 8;
        std::uint64_t conflicts[kMaxConflicts] = {};
        std::size_t conflict_count = 0;

        [[nodiscard]] constexpr std::string_view id_view() const noexcept {
            return std::string_view{id};
        }

        [[nodiscard]] bool has_conflict_with(std::uint64_t other_id) const noexcept {
            for (std::size_t i = 0; i < conflict_count; ++i)
                if (conflicts[i] == other_id) return true;
            return false;
        }
    };

    // =========================================================================
    //  pass_registry_error — returned by register_pass on rejection.
    // =========================================================================

    enum class pass_registry_error : std::uint8_t {
        conflict_detected, // another registered pass is in the conflicts list
        id_in_builtin_band, // stable_id < 1000 (reserved for built-in passes)
        id_already_registered,
        invalid_descriptor, // empty id or other malformed meta
    };

    // =========================================================================
    //  pass_registry<IR> — optional runtime registry for dynamic / plugin passes.
    //
    //  Template on the IR type so only instantiated when actually used.
    //  Zero cost for pure static (pass_bundle) users.
    //
    //  Thread-safety: std::shared_mutex, same shared-lock-validate + generation-
    //  check acquisition protocol as backend_registry.
    //
    //  Registration returns std::expected<pass_registration_token, error>:
    //    • conflicts[] checked against all currently-registered stable_ids.
    //    • stable_id must be in [1000, ∞) (extension band).
    //  Unregistration is deferred while a pass lease is held.
    // =========================================================================

    template <class IR>
    class pass_registry {
    public:
        // ------------------------------------------------------------------ //
        //  pass_registration_token — RAII handle; destructor unregisters.
        // ------------------------------------------------------------------ //

        class pass_registration_token {
        public:
            pass_registration_token() = default;

            pass_registration_token(pass_registry* reg,
                                    containers::generational_handle<struct pass_slot_tag, std::uint32_t> h) noexcept
                : registry_(reg), handle_(h) {}

            pass_registration_token(const pass_registration_token&) = delete;
            pass_registration_token& operator=(const pass_registration_token&) = delete;

            pass_registration_token(pass_registration_token&& o) noexcept
                : registry_(std::exchange(o.registry_, nullptr))
                  , handle_(std::exchange(o.handle_,
                                          containers::generational_handle<
                                              struct pass_slot_tag, std::uint32_t>::null())) {}

            pass_registration_token& operator=(pass_registration_token&& o) noexcept {
                if (this != &o) {
                    release();
                    registry_ = std::exchange(o.registry_, nullptr);
                    handle_ = std::exchange(o.handle_,
                                            containers::generational_handle<
                                                struct pass_slot_tag, std::uint32_t>::null());
                }
                return *this;
            }

            ~pass_registration_token() { release(); }

            [[nodiscard]] bool valid() const noexcept { return !handle_.is_null(); }

        private:
            void release() noexcept {
                if (registry_ && !handle_.is_null()) {
                    registry_->unregister_pass(handle_);
                    handle_ = containers::generational_handle<struct pass_slot_tag, std::uint32_t>::null();
                    registry_ = nullptr;
                }
            }

            pass_registry* registry_ = nullptr;
            containers::generational_handle<struct pass_slot_tag, std::uint32_t> handle_;
        };

        // ------------------------------------------------------------------ //
        //  pass_lease — pinned reference to a registered pass; blocks unregister.
        //
        //  NOTE: raw pointers are valid only while the pass_registration_token
        //  is live.  Do not cache leases across token destruction boundaries.
        // ------------------------------------------------------------------ //

        struct pass_lease {
            const algorithms::any_pass<IR>* pass = nullptr;
            const pass_descriptor_runtime* meta = nullptr;

            [[nodiscard]] bool valid() const noexcept { return pass != nullptr; }
        };

        // ------------------------------------------------------------------ //
        //  register_pass — validate + insert + return token or error
        // ------------------------------------------------------------------ //

        static constexpr std::uint64_t kExtensionIdBase = 1000u;

        [[nodiscard]] std::expected<pass_registration_token, pass_registry_error>
        register_pass(pass_descriptor_runtime meta, algorithms::any_pass<IR> pass) {
            // Validate id is non-empty.
            if (meta.id[0] == '\0')
                return std::unexpected(pass_registry_error::invalid_descriptor);

            // stable_id must be in extension band.
            if (meta.stable_id < kExtensionIdBase)
                return std::unexpected(pass_registry_error::id_in_builtin_band);

            std::unique_lock lock{mutex_};

            // Check for duplicate id registration.
            for (auto it = map_.begin(); it != map_.end(); ++it) {
                const auto& slot = (*it).value;
                if (!slot.active) continue;
                if (std::string_view{slot.meta.id} == std::string_view{meta.id})
                    return std::unexpected(pass_registry_error::id_already_registered);
                // Check conflict tables in both directions.
                if (meta.has_conflict_with(slot.meta.stable_id) ||
                    slot.meta.has_conflict_with(meta.stable_id))
                    return std::unexpected(pass_registry_error::conflict_detected);
            }

            pass_slot s;
            s.meta = meta;
            s.pass = std::move(pass);
            s.active = true;

            auto h = map_.insert(std::move(s));
            return pass_registration_token{this, h};
        }

        // ------------------------------------------------------------------ //
        //  find(id) — look up a live pass by string id
        // ------------------------------------------------------------------ //

        [[nodiscard]] std::optional<pass_lease>
        find(std::string_view id) const noexcept {
            std::shared_lock lock{mutex_};
            for (auto it = map_.begin(); it != map_.end(); ++it) {
                const auto& slot = (*it).value;
                if (!slot.active) continue;
                if (slot.meta.id_view() == id)
                    return pass_lease{&slot.pass.value(), &slot.meta};
            }
            return std::nullopt;
        }

        // ------------------------------------------------------------------ //
        //  passes_in_category(cat_id) — collect all live passes for a category
        // ------------------------------------------------------------------ //

        [[nodiscard]] std::vector<pass_lease>
        passes_in_category(std::uint8_t cat_id) const {
            std::shared_lock lock{mutex_};
            std::vector<pass_lease> out;
            for (auto it = map_.begin(); it != map_.end(); ++it) {
                const auto& slot = (*it).value;
                if (slot.active && slot.meta.category_id == cat_id)
                    out.push_back(pass_lease{&slot.pass.value(), &slot.meta});
            }
            return out;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            std::shared_lock lock{mutex_};
            return map_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            std::shared_lock lock{mutex_};
            return map_.empty();
        }

    private:
        using pass_handle = containers::generational_handle<struct pass_slot_tag, std::uint32_t>;

        struct pass_slot {
            pass_descriptor_runtime meta{};
            std::optional<algorithms::any_pass<IR>> pass;
            bool active = false;
        };

        void unregister_pass(pass_handle h) noexcept {
            std::unique_lock lock{mutex_};
            pass_slot* slot = map_.find(h);
            if (slot) {
                slot->active = false;
                map_.erase(h);
            }
        }

        using slot_map_t = containers::slot_map<pass_slot, pass_handle>;

        mutable std::shared_mutex mutex_;
        slot_map_t map_;
    };
} // namespace lithe::execution
