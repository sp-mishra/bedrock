#pragma once

// ============================================================================
// lithe_rt/instance.hpp — owning runtime instance (M3, shared-factory)
//
// runtime_instance is the process-level owner every compiled function holds a
// reference to (prompt).  It is created through a shared factory
// (enable_shared_from_this + create()) so managed_function handles can keep the
// runtime alive for as long as any executable page references it.  It composes
// — by value, no virtual dispatch — the heap, code manager, layout registry,
// trap manager, safepoint coordinator, root table, thread contexts, profiler,
// and security policy.
//
// execution_profile selects safe defaults; untrusted_sandbox forces the
// security-critical controls on and create() refuses to start if any required
// control cannot be enforced (prompt).
//
// The shared-metadata records (code_version_metadata et al.) live in
// code_metadata.hpp; the managed_function handle, compile(), and the out-of-line
// rooted_ref / thread_attachment members live in engine.hpp.
// ============================================================================

#include <cstdint>
#include <memory>
#include <vector>

#include "../lithe_runtime.hpp" // mop::layout_registry
#include "code_metadata.hpp"    // code_manager, code_version_metadata
#include "foundation.hpp"       // trap
#include "heap.hpp"             // heap_manager, gc_config
#include "execution.hpp"        // rooted_ref, root_slot_table, thread_context, thread_attachment, safepoint_coordinator

namespace lithe::rt {
    // =========================================================================
    // Execution profiles (prompt)
    // =========================================================================
    enum class execution_profile : std::uint8_t {
        trusted_embedded = 0,
        managed_language,
        jit_service,
        persistent_aot,
        untrusted_sandbox,
    };

    struct profile_defaults {
        bool require_verification = true;
        bool bounds_checks = true;
        bool enforce_fuel = false;
        bool enforce_w_xor_x = false;
        bool restrict_imports = false;
        bool guest_memory_model = false; // untrusted: offsets, not host ptrs
        bool forbid_host_pointers = false;

        [[nodiscard]] static constexpr profile_defaults
        for_profile(const execution_profile p) noexcept {
            switch (p) {
            case execution_profile::trusted_embedded:
                return {true, false, false, false, false, false, false};
            case execution_profile::managed_language:
                return {true, true, false, false, false, false, false};
            case execution_profile::jit_service:
                return {true, true, true, true, false, false, false};
            case execution_profile::persistent_aot:
                return {true, true, false, true, false, false, false};
            case execution_profile::untrusted_sandbox:
                // All security-critical controls forced on; not relaxable.
                return {true, true, true, true, true, true, true};
            }
            return {};
        }

        [[nodiscard]] constexpr bool sandbox_locked() const noexcept {
            return require_verification && bounds_checks && enforce_fuel
                && enforce_w_xor_x && restrict_imports
                && guest_memory_model && forbid_host_pointers;
        }
    };

    // Configuration knobs (prompt).  code_cache / import / limits are carried
    // as light PODs; the security-relevant fields flow into create()'s checks.
    struct code_cache_config {
        std::size_t reserve_bytes = 1u << 20;
    };

    struct import_policy {
        bool restrict_imports = false;
    };

    struct execution_limits {
        std::uint64_t fuel = 0;
        std::uint64_t deadline_ns = 0;
    };

    struct runtime_config {
        execution_profile profile = execution_profile::managed_language;
        gc_config gc{};
        code_cache_config code_cache{};
        import_policy imports{};
        execution_limits limits{};
    };

    // trap_manager — collects and reports structured traps.
    class trap_manager {
    public:
        void report(trap t) { traps_.push_back(std::move(t)); }
        [[nodiscard]] const std::vector<trap>& all() const noexcept { return traps_; }
        [[nodiscard]] bool any() const noexcept { return !traps_.empty(); }
        void clear() noexcept { traps_.clear(); }

    private:
        std::vector<trap> traps_;
    };

    struct profiler_service {
        std::uint64_t sample_count = 0;
    };

    struct security_policy {
        security_identity identity{};
        profile_defaults controls{};
    };

    // =========================================================================
    // runtime_instance (prompt)
    // =========================================================================
    class managed_function; // engine.hpp

    class runtime_instance : public std::enable_shared_from_this<runtime_instance> {
    public:
        // Shared factory.  Refuses to start if a profile-required control cannot
        // be enforced (prompt): heap regions unreservable, W^X unavailable
        // when required, sandbox controls not lockable.
        [[nodiscard]] static std::expected<std::shared_ptr<runtime_instance>, trap>
        create(runtime_config config = {}) {
            const auto defaults = profile_defaults::for_profile(config.profile);

            if (config.profile == execution_profile::untrusted_sandbox
                && !defaults.sandbox_locked())
                return std::unexpected(trap::make(trap_code::security_violation, 0, 0, 0, 0,
                                                  "sandbox controls not lockable"));

            // W^X enforcement is required by jit_service / persistent_aot /
            // untrusted_sandbox.  The header-only stub can always honor the flag;
            // a real backend would probe the OS here and fail if unavailable.
            if (defaults.enforce_w_xor_x) {
                auto probe = executable_memory::reserve(0, /*enforce_w_xor_x=*/true);
                if (!probe) return std::unexpected(probe.error());
            }

            // Constructed via new (private ctor) so make_shared is not required;
            // shared_ptr with a custom no-op deleter is avoided — use new + wrap.
            auto inst = std::shared_ptr<runtime_instance>(
                new runtime_instance(config, defaults));

            // Heap regions must have reserved (allocate() would otherwise trap on
            // first use); surface reservation failure at create() time.
            if (inst->heap_.usage().reserved_bytes == 0
                && config.gc.nursery_capacity != 0)
                return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                  "heap regions could not be reserved"));
            return inst;
        }

        runtime_instance(const runtime_instance&) = delete;
        runtime_instance& operator=(const runtime_instance&) = delete;
        runtime_instance(runtime_instance&&) = delete;
        runtime_instance& operator=(runtime_instance&&) = delete;

        [[nodiscard]] execution_profile profile() const noexcept { return profile_; }
        [[nodiscard]] const profile_defaults& defaults() const noexcept { return defaults_; }

        [[nodiscard]] mop::layout_registry& layouts() noexcept { return layouts_; }
        [[nodiscard]] const mop::layout_registry& layouts() const noexcept { return layouts_; }
        [[nodiscard]] heap_manager& heap() noexcept { return heap_; }
        [[nodiscard]] const heap_manager& heap() const noexcept { return heap_; }
        [[nodiscard]] code_manager& code() noexcept { return code_; }
        [[nodiscard]] trap_manager& traps() noexcept { return traps_; }
        [[nodiscard]] safepoint_coordinator& safepoints() noexcept { return safepoints_; }
        [[nodiscard]] profiler_service& profiler() noexcept { return profiler_; }
        [[nodiscard]] security_policy& security() noexcept { return security_; }

        // ---- Managed allocation as a rooted handle (prompt) ----------
        [[nodiscard]] std::expected<rooted_ref, trap>
        allocate(const std::uint64_t layout_id) {
            auto obj = heap_.allocate(layout_id);
            if (!obj) return std::unexpected(obj.error());
            return root(*obj);
        }

        // Mint a rooted_ref for an existing object_ref; registers a stable slot
        // and points the collector at it so the value is rewritten on relocation.
        [[nodiscard]] rooted_ref root(const object_ref value) {
            const root_token t = roots_.acquire(value);
            heap_.add_root(root_handle{roots_.slot(t)});
            return rooted_ref{this, t, value};
        }

        // Root a value WITHOUT a rooted_ref wrapper, returning the raw token.
        // The caller (e.g. exception_state) owns the token and must call
        // release_root(t) exactly once.  Used where ownership lives in a
        // non-RAII field rather than a stack handle.
        [[nodiscard]] root_token acquire_root(const object_ref value) {
            const root_token t = roots_.acquire(value);
            heap_.add_root(root_handle{roots_.slot(t)});
            return t;
        }

        // Authoritative root-slot accessors (P0-2).  The collector rewrites the
        // registered slot across relocation, so rooted_ref / exception_state read
        // the live value through these rather than caching a copy.
        [[nodiscard]] object_ref root_value(const root_token t) const noexcept {
            return roots_.value(t);
        }

        [[nodiscard]] object_ref* root_slot(const root_token t) noexcept {
            return roots_.slot(t);
        }

        // Validated managed store (prompt): performs the write barrier so a
        // host never writes a managed field directly.
        std::expected<void, trap>
        store_reference(rooted_ref& container, const std::uint32_t field_offset,
                        const object_ref value) {
            const object_ref c = container.get();
            if (!c.valid())
                return std::unexpected(trap::make(trap_code::null_reference, 0, 0, 0, 0,
                                                  "store_reference: null container"));
            auto* payload = static_cast<std::byte*>(payload_of(header_of(c)));
            auto* slot = reinterpret_cast<object_ref*>(payload + field_offset);
            heap_.write_barrier(c, slot, value);
            return {};
        }

        // ---- Thread attachment (prompt) ---------------------------------
        [[nodiscard]] std::expected<thread_attachment, trap> attach_current_thread() {
            auto ctx = std::make_unique<thread_context>();
            ctx->runtime = this;
            thread_context* raw = ctx.get();
            threads_.push_back(std::move(ctx));
            safepoints_.register_thread(*raw);
            return thread_attachment{this, raw};
        }

        // Stop-the-world collection that enumerates machine roots (P0-3/P0-4).
        // A scope guard guarantees the safepoint request is cleared and mutators
        // woken on EVERY exit path, including the error paths below, so a request
        // is never left dangling.  Base roots from every parked frame are
        // published, the collector evacuates them, then derived (interior)
        // pointers are recomputed from their relocated bases.
        std::expected<void, trap> collect_stw(const collection_reason reason,
                                              const machine_stack_map* map = nullptr) {
            struct request_guard {
                safepoint_coordinator* c;
                ~request_guard() { if (c) c->end_collection(); }
            };
            if (auto r = safepoints_.request_collection(); !r) return std::unexpected(r.error());
            request_guard guard{&safepoints_};

            std::vector<root_handle> machine_roots;
            if (map != nullptr)
                safepoints_.for_each_parked_context([&](safepoint_context& ctx) {
                    auto added = register_machine_roots(heap_, ctx, *map);
                    machine_roots.insert(machine_roots.end(), added.begin(), added.end());
                });

            const auto collected = heap_.collect(reason);

            if (map != nullptr) {
                safepoints_.for_each_parked_context([&](safepoint_context& ctx) {
                    rederive_machine_roots(ctx, *map);
                });
                for (const auto& rh : machine_roots) heap_.remove_root(rh);
            }
            return collected;
        }

        // ---- friends complete the RAII handles in engine.hpp -----------------
        friend class rooted_ref;
        friend class thread_attachment;
        friend class managed_function;

        // Called by rooted_ref's destructor (defined in engine.hpp).
        void release_root(const root_token t) noexcept {
            if (object_ref* s = roots_.slot(t)) heap_.remove_root(root_handle{s});
            roots_.release(t);
        }

        // Called by thread_attachment's destructor (defined in engine.hpp).
        // Releases any roots the thread still owns before dropping it (P0-8): a
        // thread detached mid-exception would otherwise leak its payload root, and
        // its host-root stack would stay published to the collector after the
        // backing storage is gone.
        void detach_thread(thread_context* ctx) noexcept {
            if (ctx == nullptr) return;
            if (ctx->exception.root != null_root_token) {
                release_root(ctx->exception.root);
                ctx->exception = exception_state{};
            }
            ctx->host_roots.unpublish_from(heap_);
            safepoints_.unregister_thread(*ctx);
            std::erase_if(threads_, [ctx](const auto& p) { return p.get() == ctx; });
        }

    private:
        runtime_instance(const runtime_config& config, const profile_defaults defaults)
            : profile_(config.profile),
              defaults_(defaults),
              heap_(&layouts_, config.gc) {
            security_.controls = defaults;
        }

        execution_profile profile_;
        profile_defaults defaults_;
        mop::layout_registry layouts_; // must precede heap_ (heap borrows it)
        heap_manager heap_;
        safepoint_coordinator safepoints_;
        code_manager code_;
        trap_manager traps_;
        root_slot_table roots_;
        std::vector<std::unique_ptr<thread_context>> threads_;
        profiler_service profiler_;
        security_policy security_;
    };
} // namespace lithe::rt
