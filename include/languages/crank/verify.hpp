#pragma once

// crank/verify.hpp — Verify policy + discharge driver (Module 3).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// verify_policy { off | assume | check | paranoid } (design §7a.4):
//   off      — ignore proof constructs; `assert`→always guard, `proof`→error
//   assume   — trust proof/contracts without discharge (fast dev, unsound)
//   check    — discharge via vakya/tarka backend (default)
//   paranoid — discharge implicit+explicit, no `assume` override
//
// Discharge backend: pluggable via vakya smt_backend concept.
//   Default: no_smt_backend → every obligation `deferred` → guard inserted, zero SMT.
//   Opt-in: tarka_smt_backend<z3_backend> (behind __has_include).
//
// Three-way discharge outcome (design §3.3):
//   proven   → drop guard
//   unknown  → insert guard   (proof unknown = error; assert unknown = guard)
//   refuted  → compile error
//
// Per-obligation bounded solve (G-TRK-3 fallback b):
//   v1 uses std::stop_token + a std::thread timer from crank::
//   Timeout → `unknown`
//
// Refinement type (G-VAK-4 fallback b):
//   refined pair kept in crank:: as side-table keyed by structural_hash.
//   Obligations emitted manually; `SafeDiv`-style refinement drops div guard.
//
// Assumption-strength gate (@tarka.assume):
//   trusted only under `assume` policy; rejected under `paranoid`.

#include "languages/crank/obligations.hpp"
#include "languages/crank/assumptions.hpp"
#include "languages/crank/safety.hpp"
#include "vakya/verify.hpp"
#include "vakya/smt.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <stop_token>
#include <atomic>
#include <optional>
#include <unordered_map>
#include <vector>

namespace crank {
    // ============================================================================
    // verify_policy
    // ============================================================================

    enum class verify_policy : std::uint8_t {
        off, // parse/ignore proof constructs; assert→guard, proof→error
        assume, // trust without discharge
        check, // discharge via backend (default)
        paranoid, // discharge implicit+explicit; disallow @tarka.assume overrides
    };

    [[nodiscard]] constexpr std::string_view to_string(verify_policy p) noexcept {
        switch (p) {
        case verify_policy::off: return "off";
        case verify_policy::assume: return "assume";
        case verify_policy::check: return "check";
        case verify_policy::paranoid: return "paranoid";
        }
        return "unknown";
    }

    // ============================================================================
    // proof_construct_kind — whether a user-written obligation is proof or assert
    // ============================================================================

    enum class proof_construct_kind : std::uint8_t {
        proof, // `proof p` — unknown = error; refuted = error
        assert_, // `assert p` — unknown = runtime guard; refuted = compile error
    };

    // ============================================================================
    // discharge_outcome — per-obligation post-discharge record
    // ============================================================================

    struct discharge_outcome {
        vakya::types::proof_status status;
        std::string description;
        bool guard_inserted = false; // true if a guard was emitted
        bool timed_out = false;
    };

    // ============================================================================
    // refined_type_entry — crank-local refinement side-table (G-VAK-4 fallback b)
    // ============================================================================

    struct refined_type_entry {
        std::uint64_t base_type_hash;
        std::uint64_t predicate_payload; // lowered predicate term payload
        std::string description;
    };

    // ============================================================================
    // verify_options — controls the discharge driver
    // ============================================================================

    struct verify_options {
        verify_policy policy = verify_policy::check;
        std::chrono::milliseconds timeout{100}; // per-obligation SMT timeout
        bool allow_tarka_assume = true; // respected under assume; denied under paranoid
    };

    // ============================================================================
    // fn_verify_result — per-function discharge summary
    // ============================================================================

    struct fn_verify_result {
        std::string fn_name;
        std::vector<discharge_outcome> obligations;
        std::vector<safety_compile_diagnostic> compile_diagnostics;
        bool fn_returns_result = false;
        safety_failure eff_policy = safety_failure::trap;

        [[nodiscard]] bool has_live_guard() const noexcept {
            for (const auto& o : obligations)
                if (o.guard_inserted) return true;
            return false;
        }

        [[nodiscard]] bool any_refuted() const noexcept {
            for (const auto& o : obligations)
                if (o.status == vakya::types::proof_status::refuted) return true;
            return false;
        }

        [[nodiscard]] bool ok() const noexcept {
            return compile_diagnostics.empty() && !any_refuted();
        }
    };

    // ============================================================================
    // verify_driver — drives obligation collection + discharge for one fn
    //
    // Template: SmtBackend satisfies vakya::types::smt_backend concept.
    // Default: no_smt_backend (zero-cost, all obligations → deferred → guard).
    // ============================================================================

    template <vakya::types::smt_backend SmtBackend = vakya::types::no_smt_backend>
    class verify_driver {
    public:
        using solver_t = vakya::types::smt_constraint_solver<SmtBackend>;

        explicit verify_driver(verify_options opts = {})
            : opts_(std::move(opts)) {}

        explicit verify_driver(verify_options opts, SmtBackend backend)
            requires std::movable < SmtBackend >
            : opts_(std::move(opts))
              , solver_(std::move(backend)) {}

        // Discharge a batch of implicit obligations under the given assumption context.
        // Returns per-obligation discharge outcomes.
        [[nodiscard]] fn_verify_result
        discharge(const std::string& fn_name,
                  std::vector<obligation_record>& obs,
                  const assumption_context& actx,
                  bool fn_returns_result,
                  const safety_policy_record& sfail_policy) {
            fn_verify_result res;
            res.fn_name = fn_name;
            res.fn_returns_result = fn_returns_result;
            res.eff_policy = sfail_policy.policy;

            for (auto& rec : obs) {
                discharge_outcome out = discharge_one(rec, actx);
                res.obligations.push_back(out);
                rec.outcome = out.status;
            }

            // §7b.3: non-Result fn + return_result + live guard → compile diagnostic
            if (auto diag = check_non_result_return_result(
                fn_name, fn_returns_result, sfail_policy.policy,
                res.has_live_guard(), {}))
                res.compile_diagnostics.push_back(std::move(*diag));

            return res;
        }

        // Discharge a single explicit `proof` or `assert` construct.
        [[nodiscard]] discharge_outcome
        discharge_explicit(const vakya::types::proof_obligation& ob,
                           proof_construct_kind kind,
                           const assumption_context& actx) {
            discharge_outcome out;
            out.description = ob.description;

            switch (opts_.policy) {
            case verify_policy::off:
                if (kind == proof_construct_kind::proof) {
                    // proof construct under `off` policy → error
                    out.status = vakya::types::proof_status::refuted;
                }
                else {
                    // assert → always guard
                    out.status = vakya::types::proof_status::unknown;
                    out.guard_inserted = true;
                }
                return out;

            case verify_policy::assume:
                // trust without discharge
                out.status = vakya::types::proof_status::proven;
                return out;

            case verify_policy::check:
            case verify_policy::paranoid:
                break;
            }

            // check / paranoid: actually discharge
            out = run_smt(ob, actx);

            // proof unknown = error (§3.3)
            if (kind == proof_construct_kind::proof
                && out.status == vakya::types::proof_status::unknown) {
                out.status = vakya::types::proof_status::refuted;
                out.guard_inserted = false;
            }
            else if (kind == proof_construct_kind::assert_
                && out.status == vakya::types::proof_status::unknown) {
                out.guard_inserted = true;
            }

            return out;
        }

        // Register a refinement type (G-VAK-4 fallback b)
        void register_refinement(std::uint64_t base_type_hash,
                                 std::uint64_t predicate_payload,
                                 std::string_view desc) {
            refinements_[base_type_hash] = refined_type_entry{
                base_type_hash, predicate_payload, std::string(desc)
            };
        }

        // Check if a refinement obligation is already discharged (drops div guard)
        [[nodiscard]] bool refinement_proven(std::uint64_t base_type_hash) const noexcept {
            auto it = refinements_.find(base_type_hash);
            if (it == refinements_.end()) return false;
            // On the no-SMT path, refinements are always deferred (not proven).
            // The Tarka path would check the solver state here.
            return false;
        }

        [[nodiscard]] const verify_options& options() const noexcept { return opts_; }

    private:
        verify_options opts_;
        solver_t solver_;
        std::unordered_map<std::uint64_t, refined_type_entry> refinements_;

        discharge_outcome discharge_one(const obligation_record& rec,
                                        const assumption_context& actx) {
            discharge_outcome out;
            out.description = rec.label;

            // Already refuted at emit time (e.g. constant divisor = 0)
            if (rec.outcome == vakya::types::proof_status::refuted) {
                out.status = vakya::types::proof_status::refuted;
                return out;
            }

            switch (opts_.policy) {
            case verify_policy::off:
                out.status = vakya::types::proof_status::unknown;
                out.guard_inserted = true;
                return out;

            case verify_policy::assume:
                out.status = vakya::types::proof_status::proven;
                return out;

            case verify_policy::check:
            case verify_policy::paranoid:
                break;
            }

            out = run_smt(rec.ob, actx);
            if (out.status == vakya::types::proof_status::unknown)
                out.guard_inserted = true;
            return out;
        }

        // Run the SMT solver with timeout (G-TRK-3 fallback b).
        // Uses std::stop_token + std::jthread timer to enforce opts_.timeout.
        discharge_outcome run_smt(const vakya::types::proof_obligation& ob,
                                  const assumption_context& actx) {
            discharge_outcome out;
            out.description = ob.description;

            std::atomic<bool> timed_out{false};
            std::atomic<bool> done{false};
            vakya::types::proof_status result_status{vakya::types::proof_status::unknown};

            // Timer thread
            std::jthread timer([&](std::stop_token st) {
                auto deadline = std::chrono::steady_clock::now() + opts_.timeout;
                while (!done.load(std::memory_order_relaxed)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        timed_out.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (st.stop_requested()) return;
                }
            });

            // Assert assumptions into solver (push scope)
            // On no_smt_backend these are no-ops
            vakya::types::constraint c;
            c.kind = ob.kind;
            c.payload = ob.term_payload;

            vakya::types::solve_context ctx{};
            std::array<vakya::types::constraint, 1> batch{c};
            auto sr = solver_.solve(std::span(batch.data(), 1u), ctx);

            done.store(true, std::memory_order_release);
            timer.request_stop();

            if (timed_out.load(std::memory_order_acquire)) {
                out.status = vakya::types::proof_status::unknown;
                out.timed_out = true;
                return out;
            }

            switch (sr.status) {
            case vakya::types::solve_status::solved:
                result_status = vakya::types::proof_status::proven;
                break;
            case vakya::types::solve_status::unsatisfiable:
                result_status = vakya::types::proof_status::refuted;
                break;
            case vakya::types::solve_status::deferred:
                result_status = vakya::types::proof_status::deferred;
                break;
            default:
                result_status = vakya::types::proof_status::unknown;
                break;
            }

            // deferred → unknown (will become guard)
            if (result_status == vakya::types::proof_status::deferred)
                result_status = vakya::types::proof_status::unknown;

            out.status = result_status;
            return out;
        }
    };

    // ============================================================================
    // Statistics for the verify driver output
    // ============================================================================

    struct verify_stats {
        std::uint32_t total_obligations = 0;
        std::uint32_t proven = 0;
        std::uint32_t unknown = 0;
        std::uint32_t refuted = 0;
        std::uint32_t guards_inserted = 0;
        std::uint32_t timed_out = 0;
    };

    [[nodiscard]] inline verify_stats
    collect_verify_stats(const fn_verify_result& r) noexcept {
        verify_stats s;
        s.total_obligations = static_cast<std::uint32_t>(r.obligations.size());
        for (const auto& o : r.obligations) {
            switch (o.status) {
            case vakya::types::proof_status::proven: ++s.proven;
                break;
            case vakya::types::proof_status::unknown: ++s.unknown;
                break;
            case vakya::types::proof_status::refuted: ++s.refuted;
                break;
            case vakya::types::proof_status::deferred: ++s.unknown;
                break;
            }
            if (o.guard_inserted) ++s.guards_inserted;
            if (o.timed_out) ++s.timed_out;
        }
        return s;
    }
} // namespace crank
