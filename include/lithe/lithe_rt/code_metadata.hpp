#pragma once

// ============================================================================
// lithe_rt/code_metadata.hpp — shared code-version metadata + code_resource (M3)
//
// The ONE shared metadata system (prompt).  Every later runtime service
// — GC relocation, deopt, hot replacement, AOT serialization, JIT tiering —
// reads/writes code_version_metadata rather than an independent per-feature side
// table that drifts.  It reuses codegen::mir::stack_map_artifact,
// safepoint::stack_map, and unwind::unwind_table instead of reinventing them,
// and every id is stable/deterministic (no std::hash in serialized identity).
//
// Extracted from the former instance.hpp so both instance.hpp and engine.hpp can
// depend on it without a cycle.  Adds's stable code_manager: install/find/
// retire hand out std::shared_ptr<code_resource> — never a pointer into a
// reallocating vector.
//
// No virtual, no macros.  Header-only C++23.
// ============================================================================

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../lithe_codegen.hpp" // codegen::mir::stack_map_artifact
#include "../lithe_runtime.hpp" // safepoint::stack_map, unwind::unwind_table
#include "foundation.hpp"       // ptr_class, trap
#include "execution.hpp"        // machine_stack_map

namespace lithe::rt {
    using function_id = std::uint32_t;
    using code_version_id = std::uint32_t;

    // =========================================================================
    // Metadata component records (moved verbatim from instance.hpp)
    // =========================================================================

    struct code_range {
        std::uint64_t begin = 0;
        std::uint64_t size = 0;
        [[nodiscard]] std::uint64_t end() const noexcept { return begin + size; }
    };

    struct gc_slot {
        std::uint32_t safepoint_id = 0;
        std::uint32_t slot = 0;
        ptr_class kind = ptr_class::managed_base;
        std::uint32_t base_slot = 0;
        std::int32_t derived_offset = 0;
    };

    struct gc_pointer_map {
        std::vector<gc_slot> slots;
        [[nodiscard]] bool empty() const noexcept { return slots.empty(); }
    };

    enum class deopt_slot_kind : std::uint8_t {
        constant, register_slot, stack_slot, materialized_object,
    };

    struct deopt_slot {
        std::uint32_t guard_id = 0;
        std::uint32_t source_var = 0;
        deopt_slot_kind kind = deopt_slot_kind::register_slot;
        std::uint64_t payload = 0;
    };

    struct deopt_map {
        std::vector<deopt_slot> slots;
        [[nodiscard]] bool empty() const noexcept { return slots.empty(); }
    };

    enum class relocation_kind : std::uint16_t {
        call_rel32, jump_rel32, abs64, pc_rel32,
        got_entry, plt_entry, const_pool, runtime_helper,
        tls, entry_cell, external_data, exception_table,
    };

    struct relocation {
        relocation_kind kind = relocation_kind::abs64;
        std::uint64_t offset = 0;
        std::uint32_t symbol = 0;
        std::int64_t addend = 0;
        std::uint8_t width = 8;
    };

    struct patchpoint {
        std::uint32_t id = 0;
        std::uint64_t offset = 0;
        std::uint32_t byte_size = 0;
    };

    struct source_position {
        std::uint64_t machine_offset = 0;
        std::uint32_t mir_instruction = 0;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
    };

    struct profiling_counter_loc {
        std::uint32_t counter_id = 0;
        std::uint64_t offset = 0;
    };

    struct version_dependency {
        std::uint64_t id = 0;
        std::uint32_t version = 0;
    };

    struct security_identity {
        std::uint64_t policy_id = 0;
        std::uint64_t policy_version = 0;
    };

    struct code_version_metadata {
        code_range range{};
        function_id function = 0;
        code_version_id version_id = 0;

        codegen::mir::stack_map_artifact stack_map{}; // MIR-level safepoints
        runtime::safepoint::stack_map machine_map{}; // machine-level roots
        runtime::unwind::unwind_table unwind{}; // exception/unwind table
        machine_stack_map physical_roots{}; // machine root map

        gc_pointer_map gc_map{};
        deopt_map deopt{};
        std::vector<relocation> relocations;
        std::vector<patchpoint> patchpoints;
        std::vector<source_position> source_positions;
        std::vector<profiling_counter_loc> profiling_counters;
        std::vector<version_dependency> import_dependencies;
        std::vector<version_dependency> layout_dependencies;
        std::vector<version_dependency> symbol_dependencies;
        security_identity security{};

        [[nodiscard]] bool has_gc_roots() const noexcept {
            return !gc_map.empty() || !stack_map.empty();
        }
    };

    // =========================================================================
    // Executable memory + code_resource (prompt)
    // =========================================================================

    // W^X executable memory (prompt).  Owns its pages; move-only.  Real
    // page mapping / mprotect wiring is the JIT-backend boundary (D5); this owns
    // a byte buffer and records whether W^X was enforced so a sandbox instance
    // can refuse to start when it was not.
    class executable_memory {
    public:
        executable_memory() = default;

        [[nodiscard]] static std::expected<executable_memory, trap>
        reserve(const std::size_t bytes, const bool enforce_w_xor_x) {
            executable_memory m;
            m.bytes_ = bytes;
            m.w_xor_x_ = enforce_w_xor_x;
            if (bytes != 0) {
                m.pages_ = std::make_unique<std::byte[]>(bytes);
                if (!m.pages_)
                    return std::unexpected(trap::make(trap_code::out_of_memory, 0, 0, 0, 0,
                                                      "executable_memory: reserve failed"));
            }
            return m;
        }

        executable_memory(executable_memory&&) noexcept = default;
        executable_memory& operator=(executable_memory&&) noexcept = default;
        executable_memory(const executable_memory&) = delete;
        executable_memory& operator=(const executable_memory&) = delete;

        [[nodiscard]] std::byte* base() noexcept { return pages_.get(); }
        [[nodiscard]] std::size_t size() const noexcept { return bytes_; }
        [[nodiscard]] bool w_xor_x() const noexcept { return w_xor_x_; }

    private:
        std::unique_ptr<std::byte[]> pages_;
        std::size_t bytes_ = 0;
        bool w_xor_x_ = false;
    };

    // Executable entry point.  A stable cell the caller invokes; indirection lets
    // hot-replacement swap the target without patching every call site.
    struct entry_cell {
        void* target = nullptr; // machine entry (backend-filled)
    };

    enum class code_state : std::uint8_t { installed = 0, active, retiring, retired };

    // Owning registrations (stub tokens for the unwind / stack-map registries).
    // Real registry insertion is the backend boundary; these carry the identity
    // so retirement can unregister deterministically.
    struct unwind_registration {
        code_version_id version = 0;
        bool live = false;
    };

    struct stack_map_registration {
        code_version_id version = 0;
        bool live = false;
    };

    // code_resource — owns every resource a code version needs (prompt).
    struct code_resource {
        code_version_metadata metadata;
        executable_memory memory;
        unwind_registration unwind;
        stack_map_registration roots;
        entry_cell entry{};
        std::atomic<std::uint64_t> active_frames{0};
        std::atomic<code_state> state{code_state::installed};

        code_resource() = default;
        code_resource(const code_resource&) = delete;
        code_resource& operator=(const code_resource&) = delete;

        // P0A: Authoritative active-frame counter accessor.  The invocation
        // guard ( ) and the retirement drain ( ) both read
        // this without depending on engine.hpp — use this stable free path.
        [[nodiscard]] std::uint64_t
        active_frame_count(const std::memory_order order =
            std::memory_order_acquire) const noexcept {
            return active_frames.load(order);
        }

        [[nodiscard]] bool has_active_frames() const noexcept {
            return active_frame_count() != 0;
        }
    };

    // =========================================================================
    // code_manager — stable shared_ptr storage (prompt)
    // =========================================================================
    class code_manager {
    public:
        std::expected<std::shared_ptr<code_resource>, trap>
        install(executable_memory memory, code_version_metadata metadata) {
            auto res = std::make_shared<code_resource>();
            const code_version_id id = next_version_++;
            metadata.version_id = id;
            res->metadata = std::move(metadata);
            res->memory = std::move(memory);
            res->unwind = unwind_registration{id, true};
            res->roots = stack_map_registration{id, true};
            res->state.store(code_state::installed, std::memory_order_release);
            versions_.emplace(id, res);
            return res;
        }

        [[nodiscard]] std::shared_ptr<const code_resource>
        find(const code_version_id v) const {
            const auto it = versions_.find(v);
            return it == versions_.end() ? nullptr : it->second;
        }

        [[nodiscard]] std::shared_ptr<code_resource>
        find_mutable(const code_version_id v) {
            const auto it = versions_.find(v);
            return it == versions_.end() ? nullptr : it->second;
        }

        // Retire a version once no frames are active.  Refuses while frames run
        // (the caller drops references; retirement completes on the last one).
        //
        // P0A retirement invariants:
        //   - A version in state::retiring MUST NOT re-enter active: the
        //     invocation guard increments active_frames, re-checks state, and
        //     refuses entry if retirement won the race.  It never rewrites the
        //     lifecycle state.
        //   - Unwind and stack-map registrations are marked dead only after
        //     active_frames reaches 0, so the unwinder never loses a live table
        //     mid-frame.
        //   - erase() from the versions_ map transfers the last shared_ptr
        //     reference here; the code_resource destructor then reclaims all
        //     owned resources (executable_memory, registrations) exactly once.
        std::expected<void, trap> retire(const code_version_id v) {
            const auto it = versions_.find(v);
            if (it == versions_.end())
                return std::unexpected(trap::make(trap_code::unresolved_symbol, 0, v, 0, 0,
                                                  "retire: unknown code version"));
            if (it->second->active_frames.load(std::memory_order_acquire) != 0) {
                it->second->state.store(code_state::retiring, std::memory_order_release);
                return {}; // deferred until frames exit
            }
            it->second->state.store(code_state::retired, std::memory_order_release);
            it->second->unwind.live = false;
            it->second->roots.live = false;
            versions_.erase(it);
            return {};
        }

        [[nodiscard]] std::size_t size() const noexcept { return versions_.size(); }

    private:
        std::unordered_map<code_version_id, std::shared_ptr<code_resource>> versions_;
        code_version_id next_version_ = 1;
    };
} // namespace lithe::rt
