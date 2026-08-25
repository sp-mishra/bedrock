#pragma once

// ============================================================================
// lithe_rt/heap.hpp — GC object model + generational collector (M1: safety)
//
// This is the correctness rewrite of the managed heap (prompt Part I):
//
//   1. heap_region — an OWNING semispace.  The previous generational_gc held
//      raw malloc pointers under defaulted move ops, so moving it double-freed.
//      heap_region moves via std::exchange; generational_gc move ops are DELETED
//      (runtime_instance is already non-movable — the safest first impl).
//
//   2. compute_object_size — CHECKED sizing/alignment.  Rejects non-pow2 align,
//      arithmetic overflow, out-of-payload / undersized / misaligned managed
//      fields, and over-large objects.  The result (payload_offset + total_size)
//      is cached in the gc_header so scanning never recomputes from mutable
//      layout metadata.
//
//   3. generational_gc — copying young gen (Cheney), mark-sweep old gen with
//      CROSS-GENERATIONAL tracing, large-object space, an OBJECT-LEVEL
//      remembered set (not an absolute-address card table), real pinning (pinned
//      objects live in old space, never a reused semispace), RAII weak_handle,
//      two-cycle finalization, heap/allocation limits, and collect() returning
//      std::expected<void, trap>.
//
// generational_gc still satisfies runtime::safepoint::GarbageCollector
// (root_scan(stack_map const&)), so trigger_safepoint drives it unchanged.
//
// No virtual, no macros.  Header-only C++23.
// ============================================================================

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../lithe_runtime.hpp" // mop::layout_registry, safepoint::stack_map, values::object_ref
#include "foundation.hpp"       // trap, trap_code

namespace lithe::rt {
    namespace mop = runtime::mop;
    namespace sp = runtime::safepoint;
    using runtime::values::object_ref;

    // =========================================================================
    // Owning heap region (prompt)
    // =========================================================================

    // A single aligned block of raw storage with move-only ownership.  Moving
    // transfers the pointer via std::exchange so no two instances free the same
    // memory; copying is deleted.
    class heap_region {
    public:
        heap_region() = default;

        [[nodiscard]] static std::expected<heap_region, trap>
        allocate(const std::size_t size, const std::size_t alignment) {
            if (size == 0) return heap_region{}; // empty region is valid
            if (alignment == 0 || !std::has_single_bit(alignment))
                return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                                  "heap_region: bad alignment"));
            void* p = nullptr;
            // posix_memalign is the portable macOS path (std::aligned_alloc
            // requires size to be a multiple of alignment).
            const std::size_t align = alignment < sizeof(void*) ? sizeof(void*) : alignment;
            if (::posix_memalign(&p, align, size) != 0 || p == nullptr)
                return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                  "heap_region: allocation failed"));
            heap_region r;
            r.memory_ = p;
            r.size_ = size;
            r.alignment_ = align;
            return r;
        }

        heap_region(heap_region&& o) noexcept
            : memory_(std::exchange(o.memory_, nullptr)),
              size_(std::exchange(o.size_, 0)),
              alignment_(std::exchange(o.alignment_, 0)) {}

        heap_region& operator=(heap_region&& o) noexcept {
            if (this != &o) {
                std::free(memory_);
                memory_ = std::exchange(o.memory_, nullptr);
                size_ = std::exchange(o.size_, 0);
                alignment_ = std::exchange(o.alignment_, 0);
            }
            return *this;
        }

        heap_region(const heap_region&) = delete;
        heap_region& operator=(const heap_region&) = delete;

        ~heap_region() { std::free(memory_); }

        [[nodiscard]] std::byte* base() noexcept {
            return static_cast<std::byte*>(memory_);
        }

        [[nodiscard]] const std::byte* base() const noexcept {
            return static_cast<const std::byte*>(memory_);
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }

        [[nodiscard]] bool contains(const void* p) const noexcept {
            return memory_ != nullptr && p >= memory_
                && p < static_cast<const std::byte*>(memory_) + size_;
        }

    private:
        void* memory_ = nullptr;
        std::size_t size_ = 0;
        std::size_t alignment_ = 0;
    };

    // =========================================================================
    // GC object model
    // =========================================================================

    enum class mark_colour : std::uint8_t { white = 0, grey = 1, black = 2 };

    // Managed-field marker: a field whose type_tag has this bit set holds an
    // object_ref the collector must trace.
    inline constexpr std::uint32_t managed_field_bit = 0x8000'0000u;

    // gc_header — the 16-byte-aligned prefix on every managed object.  The
    // prompt  mandates caching total_size + payload_offset here so a scan
    // never recomputes them from mutable layout metadata.
    struct alignas(16) gc_header {
        std::uint64_t layout_id = 0;
        std::uint32_t total_size = 0; // full object size incl. header (bytes)
        std::uint16_t payload_offset = 0; // header→payload distance (bytes)

        std::uint16_t colour : 2 = static_cast<std::uint16_t>(mark_colour::white);
        std::uint16_t age : 6 = 0;
        std::uint16_t forwarded : 1 = 0; // forwarding_address is live
        std::uint16_t pinned : 1 = 0; // must never move
        std::uint16_t is_large : 1 = 0; // large-object space
        std::uint16_t has_finalizer : 1 = 0;
        std::uint16_t finalized : 1 = 0; // finalizer already executed (two-cycle)
        std::uint16_t reserved : 3 = 0;

        object_ref forwarding_address{};
    };

    static_assert(std::is_trivially_copyable_v<gc_header>);
    static_assert(alignof(gc_header) == 16);

    inline constexpr std::size_t gc_header_size = sizeof(gc_header);

    [[nodiscard]] inline gc_header* header_of(const object_ref ref) noexcept {
        return static_cast<gc_header*>(ref.ptr);
    }

    [[nodiscard]] inline void* payload_of(gc_header* h) noexcept {
        return reinterpret_cast<std::byte*>(h) + h->payload_offset;
    }

    [[nodiscard]] inline const void* payload_of(const gc_header* h) noexcept {
        return reinterpret_cast<const std::byte*>(h) + h->payload_offset;
    }

    // Follow a forwarding chain to the current location (idempotent).
    [[nodiscard]] inline object_ref resolve_forwarding(object_ref ref) noexcept {
        gc_header* h = header_of(ref);
        while (h != nullptr && h->forwarded != 0) {
            ref = h->forwarding_address;
            h = header_of(ref);
        }
        return ref;
    }

    // =========================================================================
    // Checked object sizing (prompt)
    // =========================================================================

    struct object_size {
        std::size_t payload_offset = 0;
        std::size_t total_size = 0;
        std::size_t alignment = 0;
    };

    [[nodiscard]] constexpr std::size_t
    align_up(const std::size_t n, const std::size_t a) noexcept {
        return (n + (a - 1)) & ~(a - 1);
    }

    // Over-aligned raw allocation for old/large/pinned/promoted objects.
    // std::malloc guarantees only max_align_t; managed objects may demand more,
    // so route every non-nursery allocation through posix_memalign (macOS-first,
    // std::free-able).  align is clamped up to sizeof(void*) as posix_memalign
    // requires.  Returns nullptr on failure.
    [[nodiscard]] inline void*
    aligned_raw(const std::size_t size, const std::size_t alignment) noexcept {
        const std::size_t a = std::max<std::size_t>(alignment, sizeof(void*));
        void* p = nullptr;
        if (::posix_memalign(&p, a, size) != 0) return nullptr;
        return p;
    }

    // Validate a layout and compute its cached header/payload geometry.  Every
    // rejection path returns a trap rather than producing a corrupt object.
    [[nodiscard]] inline std::expected<object_size, trap>
    compute_object_size(const mop::object_layout& layout,
                        const std::size_t maximum_object_bytes = 0) {
        const std::size_t layout_align = layout.alignment == 0 ? 1 : layout.alignment;
        if (!std::has_single_bit(layout_align))
            return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                              "layout alignment not power of two"));

        const std::size_t alignment = std::max<std::size_t>(alignof(gc_header), layout_align);
        if (!std::has_single_bit(alignment))
            return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                              "combined alignment not power of two"));

        const std::size_t payload_offset = align_up(gc_header_size, layout_align);

        std::size_t sum = 0;
        if (__builtin_add_overflow(payload_offset, layout.size_bytes, &sum))
            return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                              "object size overflow"));
        // align_up can also overflow near SIZE_MAX.
        if (sum > SIZE_MAX - (alignment - 1))
            return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                              "object alignment overflow"));
        const std::size_t total_size = align_up(sum, alignment);

        if (maximum_object_bytes != 0 && total_size > maximum_object_bytes)
            return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                              "object exceeds maximum_object_bytes"));

        // Representational limits — gc_header caches total_size as u32 and
        // payload_offset as u16 (prompt); a value that would truncate must be
        // rejected before it is stored, never silently wrapped.
        if (total_size > 0xFFFF'FFFFull)
            return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                              "object total_size exceeds UINT32_MAX"));
        if (payload_offset > 0xFFFFull)
            return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                              "payload_offset exceeds UINT16_MAX"));

        // Validate every managed field: within payload, >= object_ref, aligned.
        for (const auto& [name, f] : layout.field_map) {
            if ((f.type_tag & managed_field_bit) == 0) continue;
            if (f.size_bytes < sizeof(object_ref))
                return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                                  "managed field smaller than object_ref"));
            std::size_t field_end = 0;
            if (__builtin_add_overflow(f.byte_offset, f.size_bytes, &field_end)
                || field_end > layout.size_bytes)
                return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                                  "managed field outside payload"));
            if ((f.byte_offset % alignof(object_ref)) != 0)
                return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                                  "managed field misaligned"));
        }

        return object_size{payload_offset, total_size, alignment};
    }

    // Construct a header in raw memory with cached geometry; return an object_ref.
    [[nodiscard]] inline object_ref
    emplace_header(void* raw, const mop::object_layout& layout,
                   const object_size& size, const std::uint32_t plugin_tag = 0) noexcept {
        auto* h = ::new(raw) gc_header{};
        h->layout_id = layout.layout_id;
        h->total_size = static_cast<std::uint32_t>(size.total_size);
        h->payload_offset = static_cast<std::uint16_t>(size.payload_offset);
        return object_ref{raw, layout.layout_id, plugin_tag};
    }

    // =========================================================================
    // Collector configuration / stats (prompt)
    // =========================================================================

    enum class collection_reason : std::uint8_t {
        allocation_failure, explicit_request, heap_limit, safepoint,
    };

    struct heap_usage {
        std::size_t reserved_bytes = 0;
        std::size_t committed_bytes = 0;
        std::size_t young_used_bytes = 0;
        std::size_t old_live_bytes = 0;
        std::size_t large_live_bytes = 0;
        std::uint64_t live_objects = 0;
    };

    struct gc_stats {
        std::uint64_t total_allocations = 0;
        std::uint64_t total_bytes = 0;
        std::uint64_t live_bytes = 0;
        std::uint64_t collections = 0;
        std::uint64_t promotions = 0;
        std::uint64_t bytes_reclaimed = 0;
        std::uint64_t max_pause_ns = 0;
        std::uint64_t total_pause_ns = 0;
    };

    struct gc_config {
        std::size_t nursery_capacity = 1u << 20; // 1 MiB semispace
        std::size_t maximum_heap_bytes = 1u << 26; // 64 MiB cap (0 = unlimited)
        std::size_t maximum_object_bytes = 0; // 0 = unlimited
        std::uint64_t maximum_object_count = 0; // 0 = unlimited
        std::size_t large_object_threshold = 32u * 1024;
        std::uint8_t promotion_age = 3;
    };

    // Opaque root handle — a stable pointer to a caller-owned object_ref slot.
    struct root_handle {
        object_ref* slot = nullptr;
        [[nodiscard]] bool valid() const noexcept { return slot != nullptr; }
        [[nodiscard]] bool operator==(const root_handle&) const noexcept = default;
    };

    // Per-thread execution state passed to safepoint(): the on-stack roots this
    // thread wants scanned at the poll.
    struct thread_state {
        std::vector<object_ref*> stack_roots;
    };

    // RAII weak reference (prompt).  The collector stores weak entries by a
    // stable id; weak_handle forgets its registration on destruction.  Declared
    // here, defined after generational_gc.
    class generational_gc;

    class weak_handle {
    public:
        weak_handle() = default;

        weak_handle(weak_handle&& o) noexcept
            : owner_(std::exchange(o.owner_, nullptr)), id_(std::exchange(o.id_, 0)) {}

        weak_handle& operator=(weak_handle&& o) noexcept;
        weak_handle(const weak_handle&) = delete;
        weak_handle& operator=(const weak_handle&) = delete;
        ~weak_handle();

        [[nodiscard]] object_ref get() const noexcept;
        [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    private:
        friend class generational_gc;

        weak_handle(generational_gc* owner, std::uint64_t id) noexcept
            : owner_(owner), id_(id) {}

        generational_gc* owner_ = nullptr;
        std::uint64_t id_ = 0;
    };

    // =========================================================================
    // Collector concept — collector-neutral interface (no virtual)
    // =========================================================================
    template <class C>
    concept Collector = requires(C c, std::uint64_t layout_id,
                                 collection_reason r, thread_state& ts, root_handle rh) {
        { c.allocate(layout_id) } -> std::same_as<std::expected<object_ref, trap>>;
        { c.collect(r) } -> std::same_as<std::expected<void, trap>>;
        { c.safepoint(ts) } -> std::same_as<void>;
        { c.add_root(rh) } -> std::same_as<void>;
        { c.remove_root(rh) } -> std::same_as<void>;
    };

    // =========================================================================
    // generational_gc
    // =========================================================================
    class generational_gc {
    public:
        explicit generational_gc(const mop::layout_registry* layouts,
                                 const gc_config cfg = {})
            : layouts_(layouts), cfg_(cfg) {
            // Reserve both semispaces up front; a failure leaves an empty region
            // and allocate() will trap on out_of_memory.
            if (auto r = heap_region::allocate(cfg_.nursery_capacity, alignof(gc_header)))
                young_from_ = std::move(*r);
            if (auto r = heap_region::allocate(cfg_.nursery_capacity, alignof(gc_header)))
                young_to_ = std::move(*r);
        }

        // Deleted move: heap_region members are movable, but the GC is owned
        // through a stable address (runtime_instance is non-movable).  Deleting
        // movement is the prompt's preferred safe first implementation .
        generational_gc(const generational_gc&) = delete;
        generational_gc& operator=(const generational_gc&) = delete;
        generational_gc(generational_gc&&) = delete;
        generational_gc& operator=(generational_gc&&) = delete;

        ~generational_gc() {
            for (auto& b : old_gen_) std::free(b.base);
            for (auto& b : large_gen_) std::free(b.base);
        }

        // ---- Collector interface -------------------------------------------

        [[nodiscard]] std::expected<object_ref, trap>
        allocate(const std::uint64_t layout_id) {
            const mop::object_layout* layout =
                layouts_ ? layouts_->find(layout_id) : nullptr;
            if (layout == nullptr)
                return std::unexpected(trap::make(trap_code::corrupted_artifact, 0, 0, 0, 0,
                                                  "unknown layout_id"));

            auto sz = compute_object_size(*layout, cfg_.maximum_object_bytes);
            if (!sz) return std::unexpected(sz.error());

            if (cfg_.maximum_object_count != 0
                && stats_.total_allocations >= cfg_.maximum_object_count)
                return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                  "object count limit reached"));

            if (sz->total_size >= cfg_.large_object_threshold)
                return allocate_large(*layout, *sz);

            void* raw = bump_young(sz->total_size);
            if (raw == nullptr) {
                if (auto c = collect(collection_reason::allocation_failure); !c)
                    return std::unexpected(c.error());
                raw = bump_young(sz->total_size);
                if (raw == nullptr)
                    return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                      "young gen exhausted after GC"));
            }
            object_ref ref = emplace_header(raw, *layout, *sz);
            record_alloc(sz->total_size);
            return ref;
        }

        // Allocate directly in old/pinned space — the object never lives in a
        // reused semispace (prompt: pinned objects must be movable-free).
        [[nodiscard]] std::expected<object_ref, trap>
        allocate_pinned(const std::uint64_t layout_id) {
            const mop::object_layout* layout =
                layouts_ ? layouts_->find(layout_id) : nullptr;
            if (layout == nullptr)
                return std::unexpected(trap::make(trap_code::corrupted_artifact));
            auto sz = compute_object_size(*layout, cfg_.maximum_object_bytes);
            if (!sz) return std::unexpected(sz.error());
            auto ref = allocate_old(*layout, *sz);
            if (ref) header_of(*ref)->pinned = 1;
            return ref;
        }

        [[nodiscard]] std::expected<void, trap>
        collect(const collection_reason reason) {
            const auto t0 = std::chrono::steady_clock::now();

            // --- Minor: copying young collection (Cheney) ---
            young_top_to_ = 0;
            worklist_.clear();

            // Evacuate explicit roots.
            for (const auto& rh : roots_)
                if (rh.valid()) {
                    if (auto e = evacuate_slot(*rh.slot); !e) return std::unexpected(e.error());
                }
            // Evacuate young references reachable from remembered old objects.
            for (void* obj : remembered_objects_)
                if (auto e = scan_remembered(obj); !e) return std::unexpected(e.error());

            // Cheney scan of copied survivors.
            std::size_t scan = 0;
            while (scan < young_top_to_) {
                auto* h = reinterpret_cast<gc_header*>(young_to_.base() + scan);
                if (auto e = scan_object_fields(h); !e) return std::unexpected(e.error());
                scan += h->total_size;
            }

            std::swap(young_from_, young_to_);
            young_top_from_ = young_top_to_;
            young_top_to_ = 0;

            // --- Major: cross-generational mark-sweep under heap pressure ---
            if (major_due()) {
                if (auto e = mark_sweep_major(); !e) return std::unexpected(e.error());
            }

            process_weak_refs();
            prune_remembered();
            run_finalizers();

            const auto t1 = std::chrono::steady_clock::now();
            const auto ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            ++stats_.collections;
            stats_.total_pause_ns += ns;
            stats_.max_pause_ns = std::max(stats_.max_pause_ns, ns);
            (void)reason;
            return {};
        }

        void safepoint(thread_state& ts) {
            const std::size_t added = ts.stack_roots.size();
            for (object_ref* r : ts.stack_roots)
                if (r) roots_.push_back(root_handle{r});
            if (young_pressure())
                (void)collect(collection_reason::safepoint);
            if (added != 0) roots_.resize(roots_.size() - added);
        }

        void add_root(const root_handle rh) { if (rh.valid()) roots_.push_back(rh); }
        void remove_root(const root_handle rh) { std::erase(roots_, rh); }

        // ---- Write barrier (prompt) -------------------------------------
        void write_barrier(const object_ref container, object_ref* slot,
                           const object_ref value) noexcept {
            if (slot == nullptr) return;
            if (in_old_or_large(container) && in_young(value))
                remembered_objects_.insert(container.ptr);
            *slot = value;
        }

        // ---- Weak refs (prompt) -----------------------------------------
        [[nodiscard]] weak_handle make_weak(const object_ref target) {
            const std::uint64_t id = next_weak_id_++;
            weak_slots_.emplace(id, target);
            return weak_handle{this, id};
        }

        // ---- Finalizers (prompt) ----------------------------------------
        void register_finalizer(const object_ref obj, std::function<void(object_ref)> fn) {
            if (obj.valid()) {
                header_of(obj)->has_finalizer = 1;
                finalizers_.emplace(obj.ptr, std::move(fn));
            }
        }

        // ---- Pinning (prompt) -------------------------------------------
        // Pin an existing object: if young, move it to old space and rewrite the
        // caller's slot; if already old/large, just mark it.  Must be called at a
        // safepoint (no managed frames executing).
        [[nodiscard]] std::expected<void, trap> pin(object_ref& obj) {
            if (!obj.valid()) return {};
            if (in_old_or_large(obj)) {
                header_of(obj)->pinned = 1;
                return {};
            }
            gc_header* h = header_of(obj);
            const mop::object_layout* l = layouts_ ? layouts_->find(h->layout_id) : nullptr;
            if (l == nullptr)
                return std::unexpected(trap::make(trap_code::corrupted_artifact));
            void* raw = aligned_raw(h->total_size, alignof(gc_header));
            if (raw == nullptr) return std::unexpected(trap::make(trap_code::out_of_memory));
            std::memcpy(raw, h, h->total_size);
            old_gen_.push_back(block{raw, h->total_size});
            old_bytes_ += h->total_size;
            object_ref moved{raw, obj.layout_id, obj.plugin_tag};
            header_of(moved)->pinned = 1;
            h->forwarded = 1;
            h->forwarding_address = moved;
            retarget_finalizer(obj.ptr, raw);
            obj = moved;
            return {};
        }

        void unpin(const object_ref obj) noexcept {
            if (obj.valid()) header_of(obj)->pinned = 0;
        }

        // ---- safepoint::GarbageCollector integration -----------------------
        void root_scan(const sp::stack_map& /*sm*/) noexcept {
            (void)collect(collection_reason::safepoint);
        }

        // ---- Introspection --------------------------------------------------
        [[nodiscard]] const gc_stats& stats() const noexcept { return stats_; }
        [[nodiscard]] const gc_config& config() const noexcept { return cfg_; }

        [[nodiscard]] heap_usage usage() const noexcept {
            heap_usage u;
            u.reserved_bytes = young_from_.size() + young_to_.size();
            u.committed_bytes = u.reserved_bytes + static_cast<std::size_t>(old_bytes_);
            u.young_used_bytes = young_top_from_;
            u.old_live_bytes = static_cast<std::size_t>(old_bytes_);
            u.live_objects = stats_.total_allocations; // upper bound; refined by sweep
            return u;
        }

        [[nodiscard]] bool in_young(const object_ref r) const noexcept {
            return young_from_.contains(r.ptr) || young_to_.contains(r.ptr);
        }

        [[nodiscard]] bool is_live_old(const object_ref r) const noexcept {
            return in_old_or_large(r);
        }

        [[nodiscard]] bool is_pinned(const object_ref r) const noexcept {
            return r.valid() && header_of(r)->pinned != 0;
        }

    private:
        struct block {
            void* base = nullptr;
            std::size_t size = 0;
        };

        friend class weak_handle;
        void forget_weak(const std::uint64_t id) noexcept { weak_slots_.erase(id); }

        [[nodiscard]] object_ref weak_get(const std::uint64_t id) const noexcept {
            const auto it = weak_slots_.find(id);
            return it == weak_slots_.end() ? object_ref{} : it->second;
        }

        // ---- region predicates ----
        [[nodiscard]] static bool in_block(const void* p, const block& b) noexcept {
            return b.base && p >= b.base
                && p < static_cast<const std::byte*>(b.base) + b.size;
        }

        [[nodiscard]] bool in_old_or_large(const object_ref r) const noexcept {
            for (const auto& b : old_gen_) if (in_block(r.ptr, b)) return true;
            for (const auto& b : large_gen_) if (in_block(r.ptr, b)) return true;
            return false;
        }

        [[nodiscard]] bool young_pressure() const noexcept {
            return young_top_from_ > (young_from_.size() * 3) / 4;
        }

        [[nodiscard]] bool major_due() const noexcept {
            return cfg_.maximum_heap_bytes != 0
                && old_bytes_ > cfg_.maximum_heap_bytes / 2;
        }

        void* bump_young(const std::size_t total) noexcept {
            if (young_top_from_ + total > young_from_.size()) return nullptr;
            void* p = young_from_.base() + young_top_from_;
            young_top_from_ += total;
            return p;
        }

        [[nodiscard]] std::expected<object_ref, trap>
        allocate_old(const mop::object_layout& layout, const object_size& sz) {
            if (cfg_.maximum_heap_bytes
                && (old_bytes_ + sz.total_size) > cfg_.maximum_heap_bytes)
                return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                  "heap limit reached (old alloc)"));
            void* raw = aligned_raw(sz.total_size, sz.alignment);
            if (raw == nullptr) return std::unexpected(trap::make(trap_code::out_of_memory));
            old_gen_.push_back(block{raw, sz.total_size});
            object_ref ref = emplace_header(raw, layout, sz);
            old_bytes_ += sz.total_size;
            record_alloc(sz.total_size);
            return ref;
        }

        [[nodiscard]] std::expected<object_ref, trap>
        allocate_large(const mop::object_layout& layout, const object_size& sz) {
            if (cfg_.maximum_heap_bytes
                && (old_bytes_ + sz.total_size) > cfg_.maximum_heap_bytes)
                return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                  "heap limit reached (large object)"));
            void* raw = aligned_raw(sz.total_size, sz.alignment);
            if (raw == nullptr) return std::unexpected(trap::make(trap_code::out_of_memory));
            large_gen_.push_back(block{raw, sz.total_size});
            object_ref ref = emplace_header(raw, layout, sz);
            header_of(ref)->is_large = 1;
            old_bytes_ += sz.total_size;
            record_alloc(sz.total_size);
            return ref;
        }

        void record_alloc(const std::size_t bytes) noexcept {
            ++stats_.total_allocations;
            stats_.total_bytes += bytes;
            stats_.live_bytes += bytes;
        }

        // ---- young copying core ----
        // Evacuate the object referenced by *slot, updating *slot to its new
        // location.  Destination is chosen BEFORE copying (prompt).
        [[nodiscard]] std::expected<void, trap> evacuate_slot(object_ref& slot) {
            auto r = evacuate(slot);
            if (!r) return std::unexpected(r.error());
            slot = *r;
            return {};
        }

        [[nodiscard]] std::expected<object_ref, trap> evacuate(const object_ref ref) {
            if (!ref.valid()) return ref;
            gc_header* h = header_of(ref);
            if (!in_young(ref)) return ref; // only young objects move
            if (h->forwarded) return h->forwarding_address;

            const std::size_t sz = h->total_size;
            const std::uint16_t new_age = static_cast<std::uint16_t>(
                h->age < 0x3F ? h->age + 1 : h->age);

            // Destination selection before copy.
            object_ref moved;
            if (new_age >= cfg_.promotion_age) {
                // Promote to old space.  A promoted object must NOT also consume
                // to-space (the previous bug); it lives only in old.
                if (cfg_.maximum_heap_bytes && (old_bytes_ + sz) > cfg_.maximum_heap_bytes)
                    return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                      "heap limit reached (promotion)"));
                void* raw = aligned_raw(sz, alignof(gc_header));
                if (raw == nullptr) return std::unexpected(trap::make(trap_code::out_of_memory));
                std::memcpy(raw, h, sz);
                old_gen_.push_back(block{raw, sz});
                old_bytes_ += sz;
                moved = object_ref{raw, ref.layout_id, ref.plugin_tag};
                header_of(moved)->age = static_cast<std::uint16_t>(new_age);
                ++stats_.promotions;
            }
            else {
                if (young_top_to_ + sz > young_to_.size())
                    return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                      "to-space exhausted"));
                void* dst = young_to_.base() + young_top_to_;
                std::memcpy(dst, h, sz);
                young_top_to_ += sz;
                moved = object_ref{dst, ref.layout_id, ref.plugin_tag};
                header_of(moved)->age = static_cast<std::uint16_t>(new_age);
            }

            h->forwarded = 1;
            h->forwarding_address = moved;
            retarget_finalizer(ref.ptr, moved.ptr);
            return moved;
        }

        // Scan a copied object's managed fields, evacuating their targets.
        [[nodiscard]] std::expected<void, trap> scan_object_fields(gc_header* h) {
            const mop::object_layout* l = layouts_ ? layouts_->find(h->layout_id) : nullptr;
            if (l == nullptr) return {};
            auto* payload = static_cast<std::byte*>(payload_of(h));
            for (const auto& [name, f] : l->field_map) {
                if ((f.type_tag & managed_field_bit) == 0) continue;
                auto* slot = reinterpret_cast<object_ref*>(payload + f.byte_offset);
                if (auto e = evacuate_slot(*slot); !e) return std::unexpected(e.error());
            }
            return {};
        }

        // Scan a remembered old object's fields for young references.
        [[nodiscard]] std::expected<void, trap> scan_remembered(void* obj) {
            auto* h = static_cast<gc_header*>(obj);
            return scan_object_fields(h);
        }

        // Retain a remembered old object only if it still holds a young ref.
        void prune_remembered() {
            std::erase_if(remembered_objects_, [&](void* obj) {
                auto* h = static_cast<gc_header*>(obj);
                const mop::object_layout* l =
                    layouts_ ? layouts_->find(h->layout_id) : nullptr;
                if (l == nullptr) return true;
                auto* payload = static_cast<std::byte*>(payload_of(h));
                for (const auto& [name, f] : l->field_map) {
                    if ((f.type_tag & managed_field_bit) == 0) continue;
                    const object_ref v = *reinterpret_cast<object_ref*>(payload + f.byte_offset);
                    if (in_young(v)) return false; // keep
                }
                return true; // no young ref remains → drop
            });
        }

        // ---- major mark-sweep (prompt): traces across ALL generations ----
        [[nodiscard]] std::expected<void, trap> mark_sweep_major() {
            std::unordered_set<void*> live;
            worklist_.clear();

            // Seed from EVERY strong root, regardless of generation.
            for (const auto& rh : roots_)
                if (rh.valid() && (*rh.slot).valid()) worklist_.push_back(*rh.slot);

            // trace_object: inspect every managed field, follow to any generation.
            while (!worklist_.empty()) {
                object_ref r = worklist_.back();
                worklist_.pop_back();
                if (!r.valid() || !live.insert(r.ptr).second) continue;
                gc_header* h = header_of(r);
                const mop::object_layout* l =
                    layouts_ ? layouts_->find(h->layout_id) : nullptr;
                if (l == nullptr) continue;
                auto* payload = static_cast<std::byte*>(payload_of(h));
                for (const auto& [name, f] : l->field_map) {
                    if ((f.type_tag & managed_field_bit) == 0) continue;
                    worklist_.push_back(*reinterpret_cast<object_ref*>(payload + f.byte_offset));
                }
            }

            std::uint64_t reclaimed = 0;
            // Reclaim an unreachable block ONLY when it is safe to free: a block
            // with a not-yet-run finalizer is kept alive this cycle and queued for
            // finalization (prompt / P0-1).  run_finalizers() inspects these
            // headers, so freeing here would be a use-after-free.  A block already
            // finalized (or never finalizable) is freed now, after its finalizer
            // and remembered-set records are erased.
            const auto dead = [&](const block& b) {
                if (live.count(b.base)) return false;
                auto* h = static_cast<gc_header*>(b.base);
                if (h != nullptr && h->pinned) return false; // pinned never swept
                if (h != nullptr && h->has_finalizer && h->finalized == 0) {
                    pending_finalization_.push_back(b.base); // resurrection window
                    return false; // keep alive this cycle
                }
                finalizers_.erase(b.base); // drop record before free
                remembered_objects_.erase(b.base); // never dangle a remembered ptr
                reclaimed += b.size;
                std::free(b.base);
                return true;
            };
            std::erase_if(old_gen_, dead);
            std::erase_if(large_gen_, dead);
            old_bytes_ -= std::min<std::uint64_t>(old_bytes_, reclaimed);
            stats_.bytes_reclaimed += reclaimed;
            stats_.live_bytes -= std::min<std::uint64_t>(stats_.live_bytes, reclaimed);
            return {};
        }

        void process_weak_refs() noexcept {
            for (auto& [id, ref] : weak_slots_) {
                if (!ref.valid()) continue;
                gc_header* h = header_of(ref);
                if (in_young(ref) && !h->forwarded)
                    ref = object_ref{}; // young target died
                else if (h->forwarded)
                    ref = h->forwarding_address; // follow relocation
            }
        }

        // Two-cycle finalization (prompt / P0-1).  Finalizers run AFTER the
        // sweep has decided what is unreachable, but on objects the sweep kept
        // alive — never on freed memory.  Two sources of dead-but-finalizable:
        //
        //   * pending_finalization_ : old/large blocks mark_sweep_major found
        //     unreachable this cycle.  Their storage is intact; run the finalizer
        //     once, set finalized=1, and leave the block in old_gen_.  A later
        //     major cycle frees it (unless the finalizer resurrected it by
        //     re-rooting, in which case it marks live and is not swept).
        //
        //   * young unforwarded finalizables : dead after the copy.  from-space
        //     storage is still intact at this point (reused only on the next
        //     allocation), so reading the header is safe; run once, mark, and drop
        //     the record because the semispace flip reclaims the storage.
        void run_finalizers() {
            // (a) Old/large blocks queued by the major sweep.
            for (void* p : pending_finalization_) {
                auto* h = static_cast<gc_header*>(p);
                if (h == nullptr || h->finalized) continue;
                const auto it = finalizers_.find(p);
                if (it == finalizers_.end()) continue;
                h->finalized = 1;
                try { it->second(object_ref{p}); }
                catch (...) { /* structured trap conversion at boundary */ }
            }
            pending_finalization_.clear();

            // (b) Young objects unreachable this cycle.  After the semispace flip
            // above, survivors live in young_from_ (their record was retargeted to
            // the to-space copy by evacuate).  A dead young finalizable was never
            // evacuated, so its record still keys on its original address, which now
            // falls in young_to_ (the just-vacated from-space).  Membership in
            // young_to_ — NOT the forwarded flag — is what distinguishes a corpse
            // from a live survivor, because a survivor also reads forwarded==0 at
            // its new home.  from-space storage is intact until the next allocation,
            // so reading the header here is safe; run once and drop the record (the
            // flip reclaims the storage, no per-object free).
            for (auto it = finalizers_.begin(); it != finalizers_.end();) {
                auto* h = static_cast<gc_header*>(it->first);
                const bool young_dead = h != nullptr && h->finalized == 0
                    && young_to_.contains(it->first);
                if (young_dead) {
                    h->finalized = 1;
                    try { it->second(object_ref{it->first}); }
                    catch (...) { /* structured trap conversion at boundary */ }
                    it = finalizers_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        void retarget_finalizer(void* from, void* to) noexcept {
            const auto it = finalizers_.find(from);
            if (it == finalizers_.end()) return;
            auto fn = std::move(it->second);
            finalizers_.erase(it);
            finalizers_.emplace(to, std::move(fn));
        }

        const mop::layout_registry* layouts_ = nullptr;
        gc_config cfg_{};

        heap_region young_from_{};
        heap_region young_to_{};
        std::size_t young_top_from_ = 0;
        std::size_t young_top_to_ = 0;

        std::vector<block> old_gen_;
        std::vector<block> large_gen_;
        std::uint64_t old_bytes_ = 0;

        std::vector<root_handle> roots_;
        std::unordered_set<void*> remembered_objects_; // object-level (prompt)
        std::unordered_map<std::uint64_t, object_ref> weak_slots_;
        std::uint64_t next_weak_id_ = 1;
        std::unordered_map<void*, std::function<void(object_ref)>> finalizers_;
        std::vector<void*> pending_finalization_; // dead old/large awaiting finalize

        std::vector<object_ref> worklist_;
        gc_stats stats_{};
    };

    static_assert(Collector<generational_gc>);
    static_assert(sp::GarbageCollector<generational_gc>);

    // ---- weak_handle out-of-line members ----
    inline weak_handle& weak_handle::operator=(weak_handle&& o) noexcept {
        if (this != &o) {
            if (owner_) owner_->forget_weak(id_);
            owner_ = std::exchange(o.owner_, nullptr);
            id_ = std::exchange(o.id_, 0);
        }
        return *this;
    }

    inline weak_handle::~weak_handle() {
        if (owner_) owner_->forget_weak(id_);
    }

    inline object_ref weak_handle::get() const noexcept {
        return owner_ ? owner_->weak_get(id_) : object_ref{};
    }

    using heap_manager = generational_gc;
} // namespace lithe::rt
