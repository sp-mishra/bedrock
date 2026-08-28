#pragma once

// crank/distributed.hpp — §v2.10 distributed execution.
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Opt-in distribution primitives layered on the same await/consume model as
// future.hpp (crank_future<T>). Nothing here changes v1 meaning: a program that
// never names a placement, never crosses a serialization_boundary, and never
// requests a distributed transaction distribution behaves exactly as v1.
//
//   placement              — a named node or group a task is *requested* to run on.
//   remote_future<T>       — like crank_future<T>, but records the placement it
//                            resolved to and whether that placement was honored
//                            or relaxed to local (honest NADI pulse on relax).
//   serialization_boundary — a marker asserting a value type is safe to cross a
//                            process/node boundary (checked, not assumed).
//   retry_policy           — retry(n, replay): bounded re-execution with a
//                            replay-safety promise. Non-idempotent + retry>0 is a
//                            diagnostic, mirroring the tx retry/replay rule.
//   tx_distribution        — transaction(distribution = local|shard|replicated):
//                            local is ungated; shard/replicated require a
//                            distributed adapter or they downgrade with a pulse.
//
// Fallback philosophy matches gpu_backend.hpp: no distributed adapter present ⇒
// the request is honestly *relaxed to local* and a pulse note is recorded, never
// silently pretended-remote. Callers inspect placement_honored()/relaxed().
//
// Design refs: §v2.10; future.hpp (consume model); transaction.hpp (distribution).

#include "languages/crank/future.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace crank {
    // ============================================================================
    // placement — where a task is *requested* to run.
    //
    // kind::local          — run in-process (always available; the fallback target).
    // kind::node           — a specific named node ("node-3", "gpu-box").
    // kind::group          — a named group/shard set ("shard-a", "replicas").
    // ============================================================================

    enum class placement_kind : std::uint8_t {
        local, // in-process; always honorable
        node, // a specific named node
        group, // a named group / shard set
    };

    [[nodiscard]] constexpr std::string_view to_string(placement_kind k) noexcept {
        switch (k) {
        case placement_kind::local: return "local";
        case placement_kind::node: return "node";
        case placement_kind::group: return "group";
        }
        return "unknown";
    }

    struct placement {
        placement_kind kind = placement_kind::local;
        std::string name; // node/group name; empty for local

        [[nodiscard]] static placement local_here() { return placement{placement_kind::local, ""}; }

        [[nodiscard]] static placement on_node(std::string n) {
            return placement{placement_kind::node, std::move(n)};
        }

        [[nodiscard]] static placement on_group(std::string n) {
            return placement{placement_kind::group, std::move(n)};
        }

        [[nodiscard]] bool is_local() const noexcept { return kind == placement_kind::local; }
    };

    // ============================================================================
    // placement_mode — hard vs soft placement (§v2.10 required/preferred rule).
    //
    // preferred — @distributed(preferred=true): if the placement cannot be honored
    //             (no adapter / can_place false), the request is relaxed to local
    //             with a NADI pulse (CRANK-DIST-001). The default; preserves the
    //             fallback philosophy.
    // required  — @distributed(required=true): a placement that cannot be honored is
    //             an error (CRANK-DIST-003). The task does NOT silently run local,
    //             because that would change latency / isolation / data-residency
    //             guarantees. The future carries the error instead.
    // ============================================================================

    enum class placement_mode : std::uint8_t {
        preferred, // soft: relax to local on miss (pulse)
        required, // hard: unmet placement is an error, never runs local
    };

    [[nodiscard]] constexpr std::string_view to_string(placement_mode m) noexcept {
        switch (m) {
        case placement_mode::preferred: return "preferred";
        case placement_mode::required: return "required";
        }
        return "unknown";
    }

    // ============================================================================
    // serialization_boundary — a checked marker that T may cross a node boundary.
    //
    // A distributed send is only safe if the payload is trivially serializable in
    // the crank model (trivially copyable, or the program supplied an explicit
    // codec). This is a *compile-time* assertion helper: crossing a boundary with a
    // type that is not boundary-safe is a diagnostic, never a silent memcpy of a
    // non-POD across the wire.
    // ============================================================================

    template <class T>
    struct serialization_boundary {
        // Boundary-safe by default only for trivially copyable payloads. Programs
        // with a custom codec specialize this to true for their type.
        static constexpr bool boundary_safe = std::is_trivially_copyable_v<T>;
    };

    template <class T>
    inline constexpr bool is_boundary_safe = serialization_boundary<T>::boundary_safe;

    // ============================================================================
    // distributed_diag — reasons a distributed request could not be honored.
    // ============================================================================

    enum class distributed_diag : std::uint8_t {
        ok, // request honored as-issued
        relaxed_to_local, // no adapter for the placement ⇒ ran local (pulse)
        boundary_unsafe, // payload type not serialization-boundary-safe
        required_unsatisfiable, // required placement could not be honored (CRANK-DIST-003)
        retry_replay_conflict, // retry>0 with non-replay-safe body (CRANK-DIST-004)
        adapter_unavailable, // shard/replicated tx requested, no adapter (CRANK-DIST-010)
    };

    [[nodiscard]] constexpr std::string_view to_string(distributed_diag d) noexcept {
        switch (d) {
        case distributed_diag::ok: return "ok";
        case distributed_diag::relaxed_to_local: return "CRANK-DIST-001";
        case distributed_diag::boundary_unsafe: return "CRANK-DIST-002";
        case distributed_diag::required_unsatisfiable: return "CRANK-DIST-003";
        case distributed_diag::retry_replay_conflict: return "CRANK-DIST-004";
        case distributed_diag::adapter_unavailable: return "CRANK-DIST-010";
        }
        return "CRANK-DIST-???";
    }

    // ============================================================================
    // retry_policy — retry(n, replay): bounded re-execution with a replay promise.
    //
    // Mirrors the transaction retry/replay rule: asking for retries on a body that
    // is not replay-safe is a conflict, because a partially-applied remote effect
    // could be duplicated. replay_safe=true asserts the body is idempotent/replayable.
    // ============================================================================

    struct retry_policy {
        std::uint32_t max_attempts = 0; // 0 = no retry (v1 default)
        bool replay_safe = false;

        [[nodiscard]] bool wants_retry() const noexcept { return max_attempts > 0; }

        // A retry request on a non-replay-safe body is a conflict.
        [[nodiscard]] bool conflicts() const noexcept {
            return wants_retry() && !replay_safe;
        }
    };

    // ============================================================================
    // remote_future<T> — a future that also records placement resolution.
    //
    // Same consume discipline as crank_future<T> (await or detach before drop), plus:
    //   - placement_requested() / placement_resolved()
    //   - placement_honored() — requested placement was actually used
    //   - relaxed()           — request was downgraded to local (pulse recorded)
    //   - diag()              — the distributed_diag for this future
    //
    // Non-copyable, movable — exclusive ownership, single consumer.
    // ============================================================================

    template <class T>
    class remote_future {
    public:
        remote_future(std::function<std::expected<T, crank_future_error>()> thunk,
                      placement requested,
                      placement resolved,
                      distributed_diag diag)
            : inner_(std::move(thunk)),
              requested_(std::move(requested)),
              resolved_(std::move(resolved)),
              diag_(diag) {}

        remote_future(const remote_future&) = delete;
        remote_future& operator=(const remote_future&) = delete;
        remote_future(remote_future&&) = default;
        remote_future& operator=(remote_future&&) = default;

        [[nodiscard]] std::expected<T, crank_future_error> await() { return inner_.await(); }
        void detach() noexcept { inner_.detach(); }

        [[nodiscard]] bool was_dropped() const noexcept { return inner_.was_dropped(); }
        [[nodiscard]] bool is_consumed() const noexcept { return inner_.is_consumed(); }

        [[nodiscard]] const placement& placement_requested() const noexcept { return requested_; }
        [[nodiscard]] const placement& placement_resolved() const noexcept { return resolved_; }
        [[nodiscard]] distributed_diag diag() const noexcept { return diag_; }

        // True iff the resolved placement matches what was requested.
        [[nodiscard]] bool placement_honored() const noexcept {
            return requested_.kind == resolved_.kind && requested_.name == resolved_.name;
        }

        // True iff the request was downgraded to local (honest NADI-pulse fallback).
        [[nodiscard]] bool relaxed() const noexcept {
            return diag_ == distributed_diag::relaxed_to_local;
        }

        // True iff a required placement could not be honored (CRANK-DIST-003): the
        // task did NOT run local; await() yields an error.
        [[nodiscard]] bool required_unmet() const noexcept {
            return diag_ == distributed_diag::required_unsatisfiable;
        }

    private:
        crank_future<T> inner_;
        placement requested_;
        placement resolved_;
        distributed_diag diag_ = distributed_diag::ok;
    };

    // ============================================================================
    // distributed_adapter — the (optional) capability that can honor a non-local
    // placement. There is no default adapter: absent one, every non-local placement
    // relaxes to local. A concrete adapter satisfies this concept without virtual.
    // ============================================================================

    template <class A>
    concept distributed_adapter = requires(const A& a, const placement& p) {
        { a.can_place(p) } -> std::convertible_to<bool>;
    };

    // ============================================================================
    // spawn_remote — spawn a task with a requested placement.
    //
    // Overload 1 (no adapter): a non-local placement cannot be honored (there is no
    // adapter). With mode=preferred it relaxes to local with a relaxed_to_local
    // pulse; with mode=required it does NOT run local — the future carries
    // required_unsatisfiable (CRANK-DIST-003) and await() yields cancelled. Local
    // requests are honored regardless of mode.
    //
    // Overload 2 (adapter): the adapter decides. can_place(p) true ⇒ honored;
    // false ⇒ relax to local (preferred) or error (required, CRANK-DIST-003).
    //
    // A boundary-unsafe payload type is reported via diag() (boundary_unsafe) but
    // still runs local — the value never actually crosses a wire without an adapter.
    // mode defaults to preferred, preserving v1/soft behavior for existing callers.
    // ============================================================================

    template <class F>
    [[nodiscard]] auto spawn_remote(placement where, F&& fn,
                                    placement_mode mode = placement_mode::preferred)
        -> remote_future<std::invoke_result_t<F>> {
        using T = std::invoke_result_t<F>;

        distributed_diag d = distributed_diag::ok;
        placement resolved = where;
        bool required_miss = false;
        if (!where.is_local()) {
            // No adapter in this overload ⇒ the placement cannot be honored.
            if (mode == placement_mode::required) {
                // Hard: never run local. Keep the requested placement, flag the miss.
                d = distributed_diag::required_unsatisfiable;
                required_miss = true;
            }
            else {
                resolved = placement::local_here();
                d = distributed_diag::relaxed_to_local;
            }
        }
        else if (!is_boundary_safe<T>) {
            // Local run is fine, but flag that this payload could not cross a wire.
            d = distributed_diag::boundary_unsafe;
        }

        return remote_future<T>(
            [f = std::forward<F>(fn), required_miss]() mutable
            -> std::expected<T, crank_future_error> {
                if (required_miss) return std::unexpected(crank_future_error::cancelled);
                return f();
            },
            std::move(where), std::move(resolved), d);
    }

    template <class A, class F>
        requires distributed_adapter<A>
    [[nodiscard]] auto spawn_remote(const A& adapter, placement where, F&& fn,
                                    placement_mode mode = placement_mode::preferred)
        -> remote_future<std::invoke_result_t<F>> {
        using T = std::invoke_result_t<F>;

        distributed_diag d = distributed_diag::ok;
        placement resolved = where;
        bool required_miss = false;
        if (!where.is_local()) {
            if (!is_boundary_safe<T>) {
                // Cannot send a non-boundary-safe payload; keep it local, flag it.
                resolved = placement::local_here();
                d = distributed_diag::boundary_unsafe;
            }
            else if (!adapter.can_place(where)) {
                if (mode == placement_mode::required) {
                    d = distributed_diag::required_unsatisfiable;
                    required_miss = true;
                }
                else {
                    resolved = placement::local_here();
                    d = distributed_diag::relaxed_to_local;
                }
            }
        }

        return remote_future<T>(
            [f = std::forward<F>(fn), required_miss]() mutable
            -> std::expected<T, crank_future_error> {
                if (required_miss) return std::unexpected(crank_future_error::cancelled);
                return f();
            },
            std::move(where), std::move(resolved), d);
    }

    // ============================================================================
    // tx_distribution — transaction(distribution = local | shard | replicated).
    //
    // local      — ungated; identical to v1 (no coordinator, single process).
    // shard      — partitioned across shards; requires a distributed adapter.
    // replicated — replicated across replicas; requires a distributed adapter.
    //
    // resolve_tx_distribution() gates the request against adapter availability:
    // local always ok; shard/replicated without an adapter ⇒ adapter_unavailable
    // (CRANK-DIST-010), and the caller must either supply an adapter or fall back
    // to local.
    // ============================================================================

    enum class tx_distribution : std::uint8_t {
        local, // v1 behavior — always allowed
        shard, // partitioned — needs adapter
        replicated, // replicated — needs adapter
    };

    [[nodiscard]] constexpr std::string_view to_string(tx_distribution d) noexcept {
        switch (d) {
        case tx_distribution::local: return "local";
        case tx_distribution::shard: return "shard";
        case tx_distribution::replicated: return "replicated";
        }
        return "unknown";
    }

    struct tx_distribution_resolution {
        tx_distribution requested = tx_distribution::local;
        bool allowed = true; // false ⇒ needs an adapter it doesn't have
        distributed_diag diag = distributed_diag::ok;
    };

    [[nodiscard]] inline tx_distribution_resolution
    resolve_tx_distribution(tx_distribution requested, bool adapter_available) noexcept {
        tx_distribution_resolution r;
        r.requested = requested;
        if (requested == tx_distribution::local) {
            r.allowed = true;
            r.diag = distributed_diag::ok;
        }
        else if (adapter_available) {
            r.allowed = true;
            r.diag = distributed_diag::ok;
        }
        else {
            r.allowed = false;
            r.diag = distributed_diag::adapter_unavailable;
        }
        return r;
    }
} // namespace crank
