#pragma once

// ============================================================================
// lithe_rt/execution.hpp — RAII roots, managed threads, machine stack maps, and
// the stop-the-world safepoint coordinator (M2)
//
// Two tightly-coupled concerns, one header (safepoints need thread_context):
//
//   Roots (prompt):
//     root_slot_table   a stable-address slab of object_ref slots.  A root is a
//                       u64 token into this table, never a pointer into a
//                       reallocating vector, so the collector can rewrite a live
//                       root's value across relocation without dangling.
//     rooted_ref        move-only handle owning one root slot.  Members that
//                       dereference runtime_instance are defined out-of-line in
//                       engine.hpp (where the type is complete) — the same
//                       deferred-member pattern heap.hpp uses for weak_handle.
//     thread_context /  per-thread managed execution state and its RAII
//     thread_attachment attachment handle.  The exception_state / unwind_phase
//                       data core lives here so thread_context is a complete type
//                       without a circular include; the dispatch/unwind *logic*
//                       lives in engine.hpp.
//
//   Safepoints (prompt):
//     machine_root_location  where one root physically sits at a safepoint (a
//                       register, a stack slot, or a compile-time null).  Derived
//                       pointers name their base by index + byte offset so the
//                       collector can relocate a base and re-derive interior
//                       pointers (the LLVM statepoint relationship).
//     safepoint_entry / machine_stack_map  the root set at each machine offset.
//     safepoint_context      what the backend hands the runtime at a poll.
//     safepoint_coordinator  the STW protocol; a single-managed-thread process
//                       takes a lock-free fast path.
//
// No virtual, no macros.  Header-only C++23.  Depends only on heap.hpp (for the
// collector + object_ref) and foundation.hpp (trap); runtime_instance is merely
// forward-declared here.
// ============================================================================

#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "foundation.hpp" // object_ref, trap, ptr_class
#include "heap.hpp"        // generational_gc, root_handle, object_ref

namespace lithe::rt {
    class runtime_instance; // completed in instance.hpp / engine.hpp

    // =========================================================================
    // Stable root slot table (prompt)
    // =========================================================================

    // A stable u64 handle into a root_slot_table.  0 is the null token.
    using root_token = std::uint64_t;
    inline constexpr root_token null_root_token = 0;

    // Slab of object_ref slots with stable addresses.  A std::deque never
    // relocates existing elements on growth, so a slot's address stays valid for
    // the collector to rewrite; freed slots are recycled via a free list.
    class root_slot_table {
    public:
        [[nodiscard]] root_token acquire(const object_ref value) {
            if (!free_.empty()) {
                const root_token t = free_.back();
                free_.pop_back();
                // P0A invariant: the slot must be in the table (1-based index).
                assert(index_of(t) < slots_.size() && "stale root token in free list");
                slots_[index_of(t)] = value;
                return t;
            }
            slots_.push_back(value);
            // P0A invariant: address stability — deque never moves existing
            // elements on push_back; existing slot pointers remain valid.
            return static_cast<root_token>(slots_.size()); // 1-based → 0 is null
        }

        void release(const root_token t) noexcept {
            if (t == null_root_token || index_of(t) >= slots_.size()) return;
            slots_[index_of(t)] = object_ref{};
            free_.push_back(t);
        }

        [[nodiscard]] object_ref* slot(const root_token t) noexcept {
            if (t == null_root_token || index_of(t) >= slots_.size()) return nullptr;
            return &slots_[index_of(t)];
        }

        [[nodiscard]] object_ref value(const root_token t) const noexcept {
            if (t == null_root_token || index_of(t) >= slots_.size()) return object_ref{};
            return slots_[index_of(t)];
        }

        // Register every live slot with the collector (called at attach / before
        // a collection).  A std::deque element address is stable so root_handle
        // stays valid until release().
        void publish_to(generational_gc& gc) {
            for (root_token t = 1; t <= slots_.size(); ++t) {
                if (is_free(t)) continue;
                gc.add_root(root_handle{&slots_[index_of(t)]});
            }
        }

        void unpublish_from(generational_gc& gc) {
            for (root_token t = 1; t <= slots_.size(); ++t) {
                if (is_free(t)) continue;
                gc.remove_root(root_handle{&slots_[index_of(t)]});
            }
        }

        [[nodiscard]] std::size_t live_count() const noexcept {
            return slots_.size() - free_.size();
        }

    private:
        [[nodiscard]] static std::size_t index_of(const root_token t) noexcept {
            return static_cast<std::size_t>(t - 1);
        }

        [[nodiscard]] bool is_free(const root_token t) const noexcept {
            for (const root_token f : free_) if (f == t) return true;
            return false;
        }

        std::deque<object_ref> slots_;
        std::vector<root_token> free_;
    };

    // A host root stack: the roots a single thread holds.  Reuses the table.
    using root_stack = root_slot_table;

    // =========================================================================
    // rooted_ref — move-only RAII host root (prompt)
    //
    // The registered slot in runtime_instance's root_slot_table is AUTHORITATIVE
    // (P0-2): the collector rewrites it across relocation, so get()/slot() must
    // resolve through the runtime + token, never a cached copy.  Those members
    // dereference runtime_instance and so are defined out-of-line in engine.hpp.
    // =========================================================================
    class rooted_ref {
    public:
        rooted_ref() = default;

        rooted_ref(rooted_ref&& o) noexcept
            : runtime_(std::exchange(o.runtime_, nullptr)),
              token_(std::exchange(o.token_, null_root_token)) {}

        rooted_ref& operator=(rooted_ref&&) noexcept; // engine.hpp
        rooted_ref(const rooted_ref&) = delete;
        rooted_ref& operator=(const rooted_ref&) = delete;
        ~rooted_ref(); // engine.hpp

        [[nodiscard]] object_ref get() const noexcept; // engine.hpp
        [[nodiscard]] object_ref* slot() noexcept; // engine.hpp
        [[nodiscard]] runtime::values::managed_handle handle() noexcept {
            return {slot()};
        }
        [[nodiscard]] bool rooted() const noexcept {
            return runtime_ != nullptr && token_ != null_root_token;
        }

    private:
        friend class runtime_instance;

        rooted_ref(runtime_instance* rt, const root_token t, const object_ref) noexcept
            : runtime_(rt), token_(t) {}

        runtime_instance* runtime_ = nullptr;
        root_token token_ = null_root_token;
    };

    // =========================================================================
    // Managed thread state (prompt)
    // =========================================================================

    // Coordination phase for stop-the-world (prompt).  running = executing
    // managed code; parked = stopped at a safepoint, safe to scan; outside =
    // in host/native code (also safe to scan — no managed frame is mutating).
    enum class thread_phase : std::uint8_t { running = 0, parked, outside };

    // Opaque machine frame chain node; the backend links these on entry so the
    // unwinder / root scanner can walk managed frames without executing code.
    struct machine_frame {
        machine_frame* caller = nullptr;
        std::uint32_t function_id = 0;
        std::uint32_t code_version = 0;
        std::uint64_t return_offset = 0; // machine offset of the return site
        void* frame_base = nullptr;
    };

    // Unwind progress for an in-flight exception (M4 uses this).
    enum class unwind_phase : std::uint8_t { inactive = 0, searching, cleanup, caught };

    // exception_state — data core kept in thread_context so the payload is a
    // rooted GC value for its whole lifetime (prompt).  The dispatch/unwind
    // *logic* lives in engine.hpp; the *state* lives here to keep thread_context
    // a complete type without a circular include.
    //
    // The rooted slot (identified by `root`) is AUTHORITATIVE once begin_throw has
    // rooted the payload (P0-2/P0-8): the collector rewrites it across relocation.
    // `in_flight` is the value at throw time and the source of type_id; the live
    // payload after a possible GC is read through the root via live_payload().
    struct exception_state {
        object_ref in_flight{}; // thrown payload (0 = none)
        root_token root = null_root_token; // slot keeping it live
        unwind_phase phase = unwind_phase::inactive;

        [[nodiscard]] bool active() const noexcept {
            return phase != unwind_phase::inactive && in_flight.valid();
        }

        // Exception type is derived from the payload, never a separate id .
        [[nodiscard]] std::uint64_t type_id() const noexcept {
            return in_flight.layout_id;
        }

        // Live payload from the authoritative root slot (engine.hpp); falls back
        // to in_flight when unrooted.
        [[nodiscard]] object_ref live_payload(runtime_instance& rt) const noexcept;
    };

    struct thread_context {
        runtime_instance* runtime = nullptr;
        std::atomic<thread_phase> phase{thread_phase::outside};
        machine_frame* current_frame = nullptr;
        root_stack host_roots;
        exception_state exception;
        std::uint64_t fuel = 0;
        std::uint32_t managed_depth = 0;
    };

    // P0A: RAII guard for managed-frame entry / exit.  's invocation guard
    // () wraps every managed call in one of these; 's retirement drain
    // () polls has_active_frames() on code_resource.  The counter is the
    // authoritative source of truth — do not increment/decrement it manually.
    struct managed_frame_guard {
        thread_context& ctx;

        explicit managed_frame_guard(thread_context& t) noexcept : ctx(t) {
            ++ctx.managed_depth;
            ctx.phase.store(thread_phase::running, std::memory_order_release);
        }

        ~managed_frame_guard() noexcept {
            assert(ctx.managed_depth > 0 && "managed_frame_guard: underflow");
            if (--ctx.managed_depth == 0)
                ctx.phase.store(thread_phase::outside, std::memory_order_release);
        }

        managed_frame_guard(const managed_frame_guard&) = delete;
        managed_frame_guard& operator=(const managed_frame_guard&) = delete;
    };

    // =========================================================================
    // thread_attachment — RAII managed-thread attachment (prompt)
    // =========================================================================
    class thread_attachment {
    public:
        thread_attachment() = default;

        thread_attachment(thread_attachment&& o) noexcept
            : runtime_(std::exchange(o.runtime_, nullptr)),
              context_(std::exchange(o.context_, nullptr)) {}

        thread_attachment& operator=(thread_attachment&&) noexcept; // engine.hpp
        thread_attachment(const thread_attachment&) = delete;
        thread_attachment& operator=(const thread_attachment&) = delete;
        ~thread_attachment(); // engine.hpp

        [[nodiscard]] thread_context& context() noexcept { return *context_; }
        [[nodiscard]] const thread_context& context() const noexcept { return *context_; }
        [[nodiscard]] bool attached() const noexcept { return context_ != nullptr; }

    private:
        friend class runtime_instance;

        thread_attachment(runtime_instance* rt, thread_context* ctx) noexcept
            : runtime_(rt), context_(ctx) {}

        runtime_instance* runtime_ = nullptr;
        thread_context* context_ = nullptr;
    };

    // =========================================================================
    // Machine stack maps (prompt)
    // =========================================================================

    enum class root_location_kind : std::uint8_t {
        register_location = 0,
        stack_location,
        constant_null,
    };

    struct machine_root_location {
        root_location_kind kind = root_location_kind::constant_null;
        std::uint16_t register_id = 0; // register_location
        std::int32_t stack_offset = 0; // stack_location (from frame base)
        ptr_class pointer_kind = ptr_class::managed_base;
        std::uint16_t base_root_index = 0; // managed_derived: index of the base
        std::int32_t derived_offset = 0; // managed_derived: byte offset

        [[nodiscard]] bool is_derived() const noexcept {
            return pointer_kind == ptr_class::managed_derived;
        }
    };

    struct safepoint_entry {
        std::uint32_t safepoint_id = 0;
        std::uint64_t machine_offset = 0;
        std::vector<machine_root_location> roots;
    };

    // The machine-level safepoint table for one code version.  Lookup is by
    // safepoint id (stable) or by machine offset (poll dispatch).
    class machine_stack_map {
    public:
        void insert(safepoint_entry e) { entries_.push_back(std::move(e)); }

        [[nodiscard]] const safepoint_entry* find_by_id(const std::uint32_t id) const noexcept {
            for (const auto& e : entries_) if (e.safepoint_id == id) return &e;
            return nullptr;
        }

        [[nodiscard]] const safepoint_entry*
        find_by_offset(const std::uint64_t off) const noexcept {
            for (const auto& e : entries_) if (e.machine_offset == off) return &e;
            return nullptr;
        }

        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    private:
        std::vector<safepoint_entry> entries_;
    };

    // Saved general-purpose register file at a safepoint.  Fixed-size, no
    // allocation; index by machine_root_location::register_id.  A root held in a
    // register is a writable slot the collector can rewrite in place.
    struct register_save_area {
        static constexpr std::size_t register_count = 32;
        std::array<object_ref, register_count> registers{};

        [[nodiscard]] object_ref* slot(const std::uint16_t id) noexcept {
            return id < register_count ? &registers[id] : nullptr;
        }
    };

    struct safepoint_context {
        thread_context* thread = nullptr;
        void* stack_pointer = nullptr;
        register_save_area* registers = nullptr;
        std::uint32_t safepoint_id = 0;
        std::uint32_t code_version = 0;

        // Resolve a machine_root_location to its writable object_ref slot, or
        // nullptr for a constant-null / derived location (derived pointers are
        // re-derived from their base, not rewritten independently).
        [[nodiscard]] object_ref* writable_slot(const machine_root_location& loc) noexcept {
            switch (loc.kind) {
            case root_location_kind::register_location:
                return registers ? registers->slot(loc.register_id) : nullptr;
            case root_location_kind::stack_location:
                if (stack_pointer == nullptr) return nullptr;
                return reinterpret_cast<object_ref*>(
                    static_cast<std::byte*>(stack_pointer) + loc.stack_offset);
            case root_location_kind::constant_null:
                return nullptr;
            }
            return nullptr;
        }
    };

    // =========================================================================
    // Machine-root enumeration (prompt / P0-3)
    //
    // At a stop-the-world collection the collector must scan the physical roots of
    // every parked managed frame.  A base root sits in a writable location (a
    // saved register or a stack slot) that the collector rewrites in place when it
    // relocates the object.  A derived (interior) pointer is NOT scanned or
    // rewritten independently: it is recomputed from its (already relocated) base
    // plus a compile-time byte offset — the LLVM statepoint relationship.
    //
    // The wiring is two calls around the collector's own root loop:
    //   1. register_machine_roots(gc, ctx, map) BEFORE collect(): publishes every
    //      base slot as a root_handle so the copying collector evacuates it.
    //   2. rederive_machine_roots(ctx, map)     AFTER collect(): rewrites each
    //      derived slot = relocated_base + derived_offset, then the caller removes
    //      the base handles again.
    // =========================================================================

    // Publish base-root slots from one parked frame as collector roots.  Returns
    // the handles added so the caller can remove them after the collection.
    inline std::vector<root_handle>
    register_machine_roots(generational_gc& gc, safepoint_context& ctx,
                           const machine_stack_map& map) {
        std::vector<root_handle> added;
        const safepoint_entry* e = map.find_by_id(ctx.safepoint_id);
        if (e == nullptr) return added;
        for (const auto& loc : e->roots) {
            if (loc.is_derived()) continue; // derived: re-derived later
            if (object_ref* s = ctx.writable_slot(loc)) {
                gc.add_root(root_handle{s});
                added.push_back(root_handle{s});
            }
        }
        return added;
    }

    // Recompute derived (interior) pointers after their bases were relocated.
    inline void
    rederive_machine_roots(safepoint_context& ctx, const machine_stack_map& map) {
        const safepoint_entry* e = map.find_by_id(ctx.safepoint_id);
        if (e == nullptr) return;
        for (const auto& loc : e->roots) {
            if (!loc.is_derived()) continue;
            object_ref* derived = ctx.writable_slot(loc);
            if (derived == nullptr) continue;
            if (loc.base_root_index >= e->roots.size()) continue;
            const machine_root_location& base_loc = e->roots[loc.base_root_index];
            object_ref* base = ctx.writable_slot(base_loc);
            if (base == nullptr || !base->valid()) {
                *derived = object_ref{};
                continue;
            }
            // Derived = relocated base + byte offset; preserves interior identity.
            derived->ptr = static_cast<std::byte*>(base->ptr) + loc.derived_offset;
            derived->layout_id = base->layout_id;
            derived->plugin_tag = base->plugin_tag;
        }
    }

    // =========================================================================
    // Stop-the-world coordination (prompt / P0-4)
    // =========================================================================

    // Coordinates parking of managed threads before a collection.  The common
    // single-managed-thread case takes a fast path with no lock contention;
    // additional threads engage a mutex/condition-variable handshake so a request
    // is never left dangling (the previous protocol returned a trap while leaving
    // requested_==true, stranding later pollers — P0-4).  A parked mutator hands
    // the coordinator its safepoint_context so the collector can enumerate that
    // thread's physical roots (P0-3).
    class safepoint_coordinator {
    public:
        void register_thread(thread_context& t) {
            std::lock_guard lock(mtx_);
            threads_.push_back(&t);
        }

        void unregister_thread(thread_context& t) noexcept {
            std::lock_guard lock(mtx_);
            std::erase(threads_, &t);
            parked_.erase(&t);
        }

        [[nodiscard]] std::size_t thread_count() const noexcept {
            std::lock_guard lock(mtx_);
            return threads_.size();
        }

        [[nodiscard]] bool collection_requested() const noexcept {
            return requested_.load(std::memory_order_acquire);
        }

        // Collector side: ask every managed thread to reach a safe point and BLOCK
        // until each is parked or executing outside managed code.  Single-thread
        // fast path returns immediately (the caller is the only mutator and is, by
        // definition, at the request site).  Never returns while a thread is still
        // running managed code, so a later poll can never strand.
        [[nodiscard]] std::expected<void, trap> request_collection() {
            std::unique_lock lock(mtx_);
            requested_.store(true, std::memory_order_release);
            if (threads_.size() <= 1) return {}; // fast path: sole mutator
            cv_.wait(lock, [this] { return all_safe_locked(); });
            return {};
        }

        void end_collection() noexcept {
            {
                std::lock_guard lock(mtx_);
                // P0A invariant: requested_ must be cleared before waking
                // mutators so no re-entering thread sees a stale request.
                requested_.store(false, std::memory_order_release);
            }
            cv_.notify_all();
        }

        // Iterate the parked frames' safepoint_contexts (collector, under STW).
        template <class Fn>
        void for_each_parked_context(Fn&& fn) {
            std::lock_guard lock(mtx_);
            for (auto& [thread, ctx] : parked_)
                if (ctx != nullptr) fn(*ctx);
        }

        // Mutator side: called at entry / alloc / call / backedge.  If a collection
        // is pending, publish this frame's context, park, and block until release.
        // The context pointer must outlive the park (it is a stack object in the
        // polling frame, which is by definition suspended here).
        void poll(thread_context& t, safepoint_context& ctx) noexcept {
            if (!requested_.load(std::memory_order_acquire)) return;
            std::unique_lock lock(mtx_);
            if (!requested_.load(std::memory_order_acquire)) return;
            parked_[&t] = &ctx;
            t.phase.store(thread_phase::parked, std::memory_order_release);
            cv_.notify_all(); // wake the collector to re-check all_safe
            cv_.wait(lock, [this] { return !requested_.load(std::memory_order_acquire); });
            parked_.erase(&t);
            t.phase.store(thread_phase::running, std::memory_order_release);
        }

    private:
        // A thread is "safe" to scan when parked at a safepoint or outside managed
        // code (no managed frame is mutating).  Caller holds mtx_.
        [[nodiscard]] bool all_safe_locked() const noexcept {
            for (const thread_context* t : threads_) {
                const thread_phase p = t->phase.load(std::memory_order_acquire);
                if (p == thread_phase::running) return false;
            }
            return true;
        }

        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::vector<thread_context*> threads_;
        std::unordered_map<thread_context*, safepoint_context*> parked_;
        std::atomic<bool> requested_{false};
    };
} // namespace lithe::rt
