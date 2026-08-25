#pragma once

// crank/gpu_memory.hpp — Device buffers, residency, transfers, events (design §11).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Sits ATOP gpu_backend.hpp (§v2.8) — no SPIR-V or dispatch is duplicated here.
// This header owns the data-plane model the planner reasons about before a
// dispatch: which buffers exist, where their current copy lives (residency),
// and the minimal set of host↔device transfers a region needs, expressed as a
// dependency-ordered transfer_plan.
//
// The replay-safe fallback gate lives here (design §11.4 / §7.4): once a region
// has performed a *visible* device write, the planner may NOT silently fall back
// to a different backend and re-run — that would double-apply effects. synchronize
// refuses with unsafe_fallback_after_effects in that case.
//
// GPU-device work is only reachable behind gpu_backend::available(); this header
// stays pure data (device I/O is the caller's plane) and so needs no SPIR-V/
// dispatch include — it always compiles regardless of the Vulkan backend.

#include "languages/crank/exec_result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crank {
    // ============================================================================
    // address_space / residency / access / ownership (design §11.1)
    // ============================================================================

    enum class address_space : std::uint8_t {
        host, // CPU-visible memory
        device, // device-local memory
        unified, // unified memory (host + device coherent)
    };

    [[nodiscard]] constexpr std::string_view to_string(address_space s) noexcept {
        switch (s) {
        case address_space::host: return "host";
        case address_space::device: return "device";
        case address_space::unified: return "unified";
        }
        return "unknown";
    }

    // Where the authoritative copy of a buffer currently lives.
    enum class residency_state : std::uint8_t {
        host_current, // host copy is authoritative; device stale/absent
        device_current, // device copy is authoritative; host stale
        synchronized, // host and device agree
        invalid, // no valid copy (freshly allocated, no init)
    };

    [[nodiscard]] constexpr std::string_view to_string(residency_state r) noexcept {
        switch (r) {
        case residency_state::host_current: return "host_current";
        case residency_state::device_current: return "device_current";
        case residency_state::synchronized: return "synchronized";
        case residency_state::invalid: return "invalid";
        }
        return "unknown";
    }

    enum class buffer_access : std::uint8_t { read, write, read_write };

    enum class buffer_ownership : std::uint8_t { owned, borrowed };

    // ============================================================================
    // device_buffer — a typed region of device/host memory (design §11.1)
    // ============================================================================

    struct device_buffer {
        std::uint32_t device = 0; // device index
        address_space space = address_space::device;
        std::string element_type; // e.g. "f32"
        std::uint64_t count = 0; // element count
        std::uint64_t byte_size = 0; // total bytes
        buffer_access access = buffer_access::read_write;
        buffer_ownership ownership = buffer_ownership::owned;
        residency_state residency = residency_state::invalid;
    };

    // ============================================================================
    // device_event — a dependency edge in the transfer/dispatch graph (§11.3)
    // ============================================================================

    struct device_event {
        std::uint32_t device = 0;
        std::uint64_t id = 0;
        std::vector<std::uint64_t> depends_on; // event ids this waits on
    };

    // ============================================================================
    // gpu_region — the buffers a single GPU dispatch touches (design §11.2)
    //
    // crank-local descriptor: the planner fills this from a monomorphized region's
    // argument buffers. Kept independent of gpu_backend's SPIR-V kernel shape so the
    // residency model does not depend on any particular kernel.
    // ============================================================================

    struct gpu_region {
        std::uint32_t device = 0;
        std::vector<device_buffer> buffers;
        bool has_visible_writes = false; // set once a device write is committed
    };

    // ============================================================================
    // transfer — a single host↔device copy (design §11.3)
    // ============================================================================

    enum class transfer_direction : std::uint8_t { upload, download };

    [[nodiscard]] constexpr std::string_view to_string(transfer_direction d) noexcept {
        switch (d) {
        case transfer_direction::upload: return "upload";
        case transfer_direction::download: return "download";
        }
        return "unknown";
    }

    struct transfer_node {
        const device_buffer* buf = nullptr;
        address_space from = address_space::host;
        address_space to = address_space::device;
        transfer_direction direction = transfer_direction::upload;
        device_event event;
    };

    struct transfer_plan {
        std::vector<transfer_node> nodes;
        // True once any node writes device memory whose result is observable — gates
        // replay-safe fallback (design §11.4).
        bool visible_device_writes = false;

        [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }
    };

    // ============================================================================
    // plan_transfers — residency-driven minimal transfer set (design §11.3)
    //
    // Rules:
    //   * upload a buffer iff the device will READ it and the host copy is current
    //     (device copy stale/absent) — nothing to upload for a synchronized/unified
    //     buffer.
    //   * a buffer the device WRITES becomes device_current; if its result is later
    //     read on the host it needs a download and marks the plan's visible writes.
    //   * unified buffers need no explicit transfer (coherent).
    // The event ids are assigned densely; a write's download depends on the upload
    // events of the inputs (a coarse but correct ordering for elementwise regions).
    // ============================================================================

    [[nodiscard]] inline transfer_plan plan_transfers(const gpu_region& region) {
        transfer_plan plan;
        std::uint64_t next_event = 1;
        std::vector<std::uint64_t> upload_events;

        // Pass 1: uploads for device-read inputs that are host-current.
        for (const auto& b : region.buffers) {
            if (b.space == address_space::unified) continue;
            const bool device_reads =
                b.access == buffer_access::read || b.access == buffer_access::read_write;
            if (device_reads &&
                (b.residency == residency_state::host_current ||
                    b.residency == residency_state::invalid)) {
                transfer_node n;
                n.buf = &b;
                n.from = address_space::host;
                n.to = address_space::device;
                n.direction = transfer_direction::upload;
                n.event.device = region.device;
                n.event.id = next_event++;
                upload_events.push_back(n.event.id);
                plan.nodes.push_back(std::move(n));
            }
        }

        // Pass 2: downloads for device-written outputs the host will consume.
        for (const auto& b : region.buffers) {
            if (b.space == address_space::unified) continue;
            const bool device_writes =
                b.access == buffer_access::write || b.access == buffer_access::read_write;
            if (device_writes) {
                transfer_node n;
                n.buf = &b;
                n.from = address_space::device;
                n.to = address_space::host;
                n.direction = transfer_direction::download;
                n.event.device = region.device;
                n.event.id = next_event++;
                n.event.depends_on = upload_events; // download after inputs uploaded + kernel
                plan.nodes.push_back(std::move(n));
                plan.visible_device_writes = true;
            }
        }

        return plan;
    }

    // ============================================================================
    // synchronize — commit the transfer plan; replay-safe fallback gate (§11.4)
    //
    // A fully populated plan is "committed" (host/device made coherent). If the
    // region already performed visible device writes AND allow_replay is false, we
    // refuse: retrying on another backend would double-apply the writes. This is the
    // single point that enforces "no unsafe fallback after visible effects".
    // ============================================================================

    [[nodiscard]] inline execution_result<void>
    synchronize(transfer_plan& plan, bool allow_replay = true) {
        if (plan.visible_device_writes && !allow_replay) {
            return make_error_result<void>(make_error(
                execution_error_kind::unsafe_fallback_after_effects,
                "cannot re-run after visible device writes committed"));
        }

        // Data-plane copies are the caller's responsibility (gpu_backend install +
        // buffer binding). Here we validate the graph is well-formed: every download
        // that names dependencies references only ids emitted earlier in the plan.
        std::vector<std::uint64_t> seen;
        seen.reserve(plan.nodes.size());
        for (const auto& n : plan.nodes) {
            for (auto dep : n.event.depends_on) {
                bool found = false;
                for (auto s : seen) if (s == dep) {
                    found = true;
                    break;
                }
                if (!found) {
                    return make_error_result<void>(make_error(
                        execution_error_kind::gpu_sync_failure,
                        "transfer event depends on unscheduled event " + std::to_string(dep)));
                }
            }
            seen.push_back(n.event.id);
        }

        return make_completed_void();
    }
} // namespace crank
