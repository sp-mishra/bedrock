#pragma once
#include "lithe/lithe_cost_model.hpp"
#include "lithe/lithe_decision_engine.hpp"
#include "lithe/lithe_passes.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <sstream>

// lithe_explain.hpp — Opt-in optimization explanation layer.
//
// Converts pass_event streams into human-readable optimization explanations.
// No virtual dispatch; all observer wiring is done at compile time via templates.
//
// Usage:
//   auto [result, expl] = lithe::explain::explain_optimization(expr, pass1, pass2);
//   std::puts(expl.format(/*markdown=*/true).c_str());

namespace lithe::explain {
    // ============================================================================
    // § 1  rule_application_entry — one rewrite-rule firing recorded per pass
    // ============================================================================

    struct rule_application_entry {
        std::string rule_name;
        std::string pass_id;
        std::size_t nodes_before = 0;
        std::size_t nodes_after = 0;
        std::uint64_t pass_cost_ns = 0;
        std::optional<std::string> source_span;
        std::string reason; // human-readable reason (from registry)
    };

    // ============================================================================
    // § 2  optimization_explanation — aggregated explanation for one compile run
    // ============================================================================

    struct optimization_explanation {
        std::vector<rule_application_entry> applications;

        /// Total nanoseconds spent across all tracked passes.
        [[nodiscard]] std::uint64_t total_cost_ns() const noexcept {
            std::uint64_t t = 0;
            for (const auto& a : applications) t += a.pass_cost_ns;
            return t;
        }

        /// Number of passes that produced at least one node-count change.
        [[nodiscard]] std::size_t changed_passes() const noexcept {
            std::size_t n = 0;
            for (const auto& a : applications)
                if (a.nodes_before != a.nodes_after) ++n;
            return n;
        }

        /// Format the explanation as plain text or Markdown.
        [[nodiscard]] std::string format(bool markdown = false) const {
            std::ostringstream os;
            if (markdown) {
                os << "## Optimization Explanation\n\n";
                os << "| Pass | Rule | Nodes (before→after) | Cost (ns) | Reason |\n";
                os << "|------|------|----------------------|-----------|--------|\n";
                for (const auto& a : applications) {
                    os << "| " << a.pass_id
                        << " | " << (a.rule_name.empty() ? "-" : a.rule_name)
                        << " | " << a.nodes_before << "→" << a.nodes_after
                        << " | " << a.pass_cost_ns
                        << " | " << (a.reason.empty() ? "-" : a.reason)
                        << " |\n";
                }
                os << "\n**Total cost:** " << total_cost_ns() << " ns  \n";
                os << "**Changed passes:** " << changed_passes() << "\n";
            }
            else {
                os << "Optimization Explanation\n";
                os << "------------------------\n";
                for (const auto& a : applications) {
                    os << "[" << a.pass_id << "] "
                        << "rule=" << (a.rule_name.empty() ? "<none>" : a.rule_name)
                        << "  nodes=" << a.nodes_before << "->" << a.nodes_after
                        << "  cost=" << a.pass_cost_ns << "ns";
                    if (!a.reason.empty()) os << "  reason=\"" << a.reason << "\"";
                    os << "\n";
                }
                os << "Total cost: " << total_cost_ns() << " ns\n";
                os << "Changed passes: " << changed_passes() << "\n";
            }
            return os.str();
        }
    };

    // ============================================================================
    // § 3  rule_reason_registry — singleton mapping rule names → human reasons
    // ============================================================================

    class rule_reason_registry {
    public:
        rule_reason_registry(const rule_reason_registry&) = delete;
        rule_reason_registry& operator=(const rule_reason_registry&) = delete;

        /// Register a human-readable reason for a rule name.
        void register_reason(std::string_view rule_name, std::string_view reason) {
            std::unique_lock lk{mutex_};
            reasons_.insert_or_assign(std::string{rule_name}, std::string{reason});
        }

        /// Look up the reason for a rule; returns the rule name itself when unknown.
        [[nodiscard]] std::string_view get_reason(std::string_view rule_name) const {
            std::shared_lock lk{mutex_};
            const auto it = reasons_.find(std::string{rule_name});
            if (it != reasons_.end()) return it->second;
            return rule_name;
        }

        /// Process-wide singleton. Thread-safe after first call.
        [[nodiscard]] static rule_reason_registry& global() {
            static rule_reason_registry inst;
            return inst;
        }

    private:
        rule_reason_registry() {
            // Built-in arithmetic identity rules.
            reasons_["add_zero"] = "Arithmetic Identity";
            reasons_["mul_one"] = "Multiplicative Identity";
            reasons_["mul_zero"] = "Zero Annihilation";
            reasons_["double_neg"] = "Double Negation Elimination";
        }

        mutable std::shared_mutex mutex_;
        std::unordered_map<std::string, std::string> reasons_;
    };

    // ============================================================================
    // § 4  collecting_pass_observer — wraps trace_observer; exposes accumulated
    //       pass_events after the pipeline runs.
    // ============================================================================

    class collecting_pass_observer {
    public:
        using pass_event_t = ::lithe::compiler::observability::pass_event;

        // Accept any event type; only pass_event is collected.
        template <class Event>
        void on_event(const Event& ev) {
            if constexpr (std::same_as<std::remove_cvref_t<Event>, pass_event_t>) {
                std::scoped_lock lk{mutex_};
                events_.push_back(ev);
            }
        }

        /// Take ownership of the accumulated events (clears internal buffer).
        [[nodiscard]] std::vector<pass_event_t> take_events() {
            std::scoped_lock lk{mutex_};
            return std::exchange(events_, {});
        }

        [[nodiscard]] const std::vector<pass_event_t>& events() const noexcept { return events_; }

    private:
        mutable std::mutex mutex_;
        std::vector<pass_event_t> events_;
    };

    // ============================================================================
    // § 5  explain_from_events — converts a vector of pass_events to an explanation
    // ============================================================================

    [[nodiscard]] inline optimization_explanation
    explain_from_events(std::vector<compiler::observability::pass_event> events) {
        optimization_explanation expl;
        expl.applications.reserve(events.size());
        auto& reg = rule_reason_registry::global();
        for (auto& ev : events) {
            rule_application_entry entry;
            entry.pass_id = ev.pass_name.empty()
                                ? ("pass_" + std::to_string(ev.pass_index))
                                : ev.pass_name;
            entry.rule_name = ev.rule_fired;
            entry.nodes_before = ev.nodes_before;
            entry.nodes_after = ev.nodes_after;
            entry.pass_cost_ns = ev.pass_cost_ns;
            if (!ev.rule_fired.empty())
                entry.reason = std::string{reg.get_reason(ev.rule_fired)};
            expl.applications.push_back(std::move(entry));
        }
        return expl;
    }

    // ============================================================================
    // § 6  optimized_expr — thin wrapper carrying the result expression
    // ============================================================================

    template <class E>
    struct optimized_expr {
        E value;
        explicit optimized_expr(E v) : value{std::move(v)} {}
    };

    // ============================================================================
    // § 7  explain_optimization — template entry point
    //
    // Runs the pass pipeline under a collecting_pass_observer (observability
    // enabled = true), then builds an optimization_explanation from the recorded
    // pass_event stream.
    //
    // Requirements:
    //   - E satisfies lithe::Operand
    //   - Each P satisfies std::invocable<P, E> (or with pass_context&)
    // ============================================================================

    namespace detail {
        // Fold-based observed pipeline runner (wraps compile_observed with
        // observability forced on).
        template <class E, class Observer, class... Passes>
        [[nodiscard]] auto run_observed(E&& expr, Observer& obs, Passes&&... passes) {
            ::lithe::compiler::pass_context ctx;
            return ::lithe::compiler::compile_observed < true > (
                std::forward<E>(expr), ctx, obs, std::forward<Passes>(passes)
            ...
            )
            ;
        }
    } // namespace detail

    template <class... Passes, Operand E>
    [[nodiscard]] auto explain_optimization(const E& expr, Passes&&... passes)
        -> std::pair<optimized_expr<std::remove_cvref_t<E>>, optimization_explanation> {
        collecting_pass_observer obs;
        auto result = detail::run_observed(expr, obs, std::forward<Passes>(passes)...);
        auto expl = explain_from_events(obs.take_events());
        return {optimized_expr<std::remove_cvref_t<E>>{std::move(result)}, std::move(expl)};
    }
} // namespace lithe::explain


// =============================================================================
// § 8  decision_explanation — explains a decision_engine ranking result
//
// Works for any T with a std::string conversion (or std::string_view).
// Typical usage:
//   auto ranking = engine.decide<backend_id_t>(...);
//   auto expl    = lithe::explain::explain_decision(ranking);
//   std::puts(expl.format(true).c_str());
// =============================================================================

namespace lithe::explain {
    struct decision_candidate_record {
        std::string label; // string form of the chosen T value
        cost::cost_vector cost;
        double score = 0.0;
    };

    struct decision_explanation {
        std::string chosen; // label of ranked[0]
        std::string reason; // e.g. "42% lower estimated latency"
        std::vector<decision_candidate_record> candidates; // all candidates, best-first

        [[nodiscard]] std::string format(bool markdown = false) const {
            std::ostringstream os;
            if (markdown) {
                os << "## Decision Explanation\n\n";
                os << "**Chosen**: " << chosen << "  \n";
                os << "**Reason**: " << reason << "  \n\n";
                os << "| Rank | Candidate | Latency | Memory | Power | Throughput | Score |\n";
                os << "|------|-----------|---------|--------|-------|------------|-------|\n";
                for (std::size_t i = 0; i < candidates.size(); ++i) {
                    const auto& c = candidates[i];
                    os << "| " << (i + 1)
                        << " | " << c.label
                        << " | " << c.cost.latency
                        << " | " << c.cost.memory
                        << " | " << c.cost.power
                        << " | " << c.cost.throughput
                        << " | " << c.score
                        << " |\n";
                }
            }
            else {
                os << "Decision Explanation\n";
                os << "--------------------\n";
                os << "Chosen:  " << chosen << "\n";
                os << "Reason:  " << reason << "\n";
                for (std::size_t i = 0; i < candidates.size(); ++i) {
                    const auto& c = candidates[i];
                    os << "  [" << (i + 1) << "] " << c.label
                        << "  score=" << c.score
                        << "  latency=" << c.cost.latency
                        << "\n";
                }
            }
            return os.str();
        }
    };

    // explain_decision(ranked<T>) — build a decision_explanation from any ranked<T>
    // where T is convertible to std::string via std::format (or has a to_string ADL).
    //
    // The reason string is derived from the score delta between rank[0] and rank[1]
    // (expressed as a percentage of rank[1]'s latency cost).

    template <class T>
    [[nodiscard]] decision_explanation
    explain_decision(const intelligence::ranked<T>& ranking) {
        decision_explanation expl;
        if (ranking.empty()) {
            expl.chosen = "<none>";
            expl.reason = "no candidates";
            return expl;
        }

        expl.candidates.reserve(ranking.ordered.size());
        for (const auto& c : ranking.ordered) {
            decision_candidate_record rec;
            if constexpr (requires { std::string{c.value}; }) {
                rec.label = std::string{c.value};
            }
            else {
                rec.label = std::to_string(static_cast<long long>(c.value));
            }
            rec.cost = c.cost;
            rec.score = c.score;
            expl.candidates.push_back(std::move(rec));
        }

        expl.chosen = expl.candidates.front().label;

        // Build reason from latency delta vs runner-up
        if (ranking.ordered.size() >= 2) {
            const float best_lat = ranking.ordered[0].cost.latency;
            const float runnerup_lat = ranking.ordered[1].cost.latency;
            if (runnerup_lat > 0.0f && best_lat < runnerup_lat) {
                const float pct = (runnerup_lat - best_lat) / runnerup_lat * 100.0f;
                std::ostringstream os;
                os << static_cast<int>(pct) << "% lower estimated latency than "
                    << expl.candidates[1].label;
                expl.reason = os.str();
            }
            else {
                expl.reason = "best score";
            }
        }
        else {
            expl.reason = "only candidate";
        }

        return expl;
    }
} // namespace lithe::explain
