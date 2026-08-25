#pragma once

// crank/assumptions.hpp — Proof environment / assumption context (Module 3).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Per-scope stack of in-scope assumption terms (design §7a.3).
// G-TRK-2 fallback (b): push/pop stack managed in crank::, calling backend
// push/pop directly. Crank decides *what* to assume; Tarka manages solver scope.
//
// Push rules:
//   push_requires / push_where  — fn preconditions on fn entry
//   push_proven_assert          — a proven `assert` / `proof p`
//   push_refinement             — p[v:=x] for a refinement-typed binding
//   push_if_branch              — `c` in then-arm, `!c` in else-arm
//   push_for_range              — `lo <= i && i < hi` in for i:=range lo..hi body
//
// Assumptions are asserted into the Tarka context before discharging each goal.
// Example: `requires len(xs)==len(out)` assumed → loop bounds check discharges `proven`.
//
// Usage:
//   crank::assumption_context actx;
//   actx.push_requires(precond_payload, "len(xs)==len(out)");
//   {
//     auto scope = actx.enter_scope();
//     actx.push_for_range(lo_hash, hi_hash, idx_name, span);
//     // discharge obligations under actx.active_assumptions()
//   }  // scope exits: pops back to pre-for state

#include "languages/crank/source_span.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
    // ============================================================================
    // assumption_entry — one assumption in the proof environment
    // ============================================================================

    enum class assumption_kind : std::uint8_t {
        requires_clause, // fn `requires` or `where`
        proven_assertion, // proven `assert` / `proof p`
        refinement_binding, // p[v:=x] from a refinement type
        if_condition, // c or !c from an if-branch
        for_range_lower, // lo <= i (for range)
        for_range_upper, // i < hi (for range)
    };

    struct assumption_entry {
        std::uint64_t term_payload; // tarka::Term* or hash on no-SMT path
        assumption_kind kind;
        std::string description;
        source_span at;
    };

    // ============================================================================
    // assumption_context
    //
    // RAII-scope-stack. enter_scope() returns a guard that pops on destruction.
    // Assumptions below a scope boundary are invisible after the scope exits.
    // ============================================================================

    class assumption_context {
    public:
        assumption_context() = default;

        // RAII scope guard — pops all assumptions added during its lifetime
        class scope_guard {
        public:
            explicit scope_guard(assumption_context& ctx) noexcept
                : ctx_(&ctx), mark_(ctx.stack_.size()) {}

            scope_guard(scope_guard&&) noexcept = default;
            scope_guard(const scope_guard&) = delete;
            scope_guard& operator=(scope_guard&&) noexcept = default;
            scope_guard& operator=(const scope_guard&) = delete;

            ~scope_guard() noexcept {
                if (ctx_) ctx_->stack_.resize(mark_);
            }

        private:
            assumption_context* ctx_;
            std::size_t mark_;
        };

        // Returns a guard; all pushes after this call are popped when guard destructs
        [[nodiscard]] scope_guard enter_scope() noexcept { return scope_guard{*this}; }

        // Push fn requires / where clause
        void push_requires(std::uint64_t term_payload,
                           std::string_view desc,
                           source_span at = {}) {
            push({term_payload, assumption_kind::requires_clause, std::string(desc), at});
        }

        // Push `where` clause (same structure as requires)
        void push_where(std::uint64_t term_payload,
                        std::string_view desc,
                        source_span at = {}) {
            push({term_payload, assumption_kind::requires_clause, std::string(desc), at});
        }

        // Push proven assert / proof result
        void push_proven_assertion(std::uint64_t term_payload,
                                   std::string_view desc,
                                   source_span at = {}) {
            push({term_payload, assumption_kind::proven_assertion, std::string(desc), at});
        }

        // Push refinement substitution p[v:=x]
        void push_refinement(std::uint64_t term_payload,
                             std::string_view desc,
                             source_span at = {}) {
            push({term_payload, assumption_kind::refinement_binding, std::string(desc), at});
        }

        // Push if-condition (then=true arm uses cond; else arm uses negation)
        void push_if_cond(std::uint64_t cond_payload, bool negate,
                          std::string_view cond_desc, source_span at = {}) {
            assumption_entry e;
            e.term_payload = negate ? ~cond_payload : cond_payload;
            e.kind = assumption_kind::if_condition;
            e.description = negate ? ("!" + std::string(cond_desc)) : std::string(cond_desc);
            e.at = at;
            push(std::move(e));
        }

        // Push for i in lo..hi body: lo <= i AND i < hi
        void push_for_range(std::uint64_t lo_payload, std::uint64_t hi_payload,
                            std::string_view idx_name, source_span at = {}) {
            // lower bound: lo <= idx
            assumption_entry lo_entry;
            lo_entry.term_payload = lo_payload ^ std::hash<std::string>{}(std::string(idx_name));
            lo_entry.kind = assumption_kind::for_range_lower;
            lo_entry.description = std::string(idx_name) + " >= lo";
            lo_entry.at = at;
            push(std::move(lo_entry));

            // upper bound: idx < hi
            assumption_entry hi_entry;
            hi_entry.term_payload = hi_payload ^ std::hash<std::string>{}(std::string(idx_name));
            hi_entry.kind = assumption_kind::for_range_upper;
            hi_entry.description = std::string(idx_name) + " < hi";
            hi_entry.at = at;
            push(std::move(hi_entry));
        }

        // Read view of active assumptions
        [[nodiscard]] const std::vector<assumption_entry>& active_assumptions() const noexcept {
            return stack_;
        }

        [[nodiscard]] std::size_t size() const noexcept { return stack_.size(); }
        [[nodiscard]] bool empty() const noexcept { return stack_.empty(); }

        void clear() noexcept { stack_.clear(); }

    private:
        void push(assumption_entry e) { stack_.push_back(std::move(e)); }

        std::vector<assumption_entry> stack_;
    };

    // ============================================================================
    // assumption_stats — debug/dump statistics
    // ============================================================================

    struct assumption_stats {
        std::uint32_t total = 0;
        std::uint32_t requires_count = 0;
        std::uint32_t assertion_count = 0;
        std::uint32_t refinement_count = 0;
        std::uint32_t if_cond_count = 0;
        std::uint32_t range_count = 0;
    };

    [[nodiscard]] inline assumption_stats
    collect_assumption_stats(const std::vector<assumption_entry>& entries) noexcept {
        assumption_stats s;
        s.total = static_cast<std::uint32_t>(entries.size());
        for (const auto& e : entries) {
            switch (e.kind) {
            case assumption_kind::requires_clause: ++s.requires_count;
                break;
            case assumption_kind::proven_assertion: ++s.assertion_count;
                break;
            case assumption_kind::refinement_binding: ++s.refinement_count;
                break;
            case assumption_kind::if_condition: ++s.if_cond_count;
                break;
            case assumption_kind::for_range_lower:
            case assumption_kind::for_range_upper: ++s.range_count;
                break;
            }
        }
        return s;
    }
} // namespace crank
