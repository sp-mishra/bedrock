#pragma once

// crank/limits.hpp — Instantiation termination + compile-time resource controls
// (generics maturation, §11.4 + §14).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// generics.hpp/monomorphize.hpp resolve one instantiation at a time; they have no
// notion of an *expansion stack* and so cannot detect a generic that instantiates
// itself without bound (`Foo[T] → Foo[Vector[T]] → Foo[Vector[Vector[T]]] → …`) or
// a program that simply asks for more monomorphizations than a build should permit.
// This header adds those two guards, kept opt-in and free-function/plain-object so
// monomorphize.hpp gains no mandatory dependency (mirrors coherence.hpp's style).
//
// Two termination signals (§11.4):
//   - Depth:  the active instantiation stack must not exceed max_nesting_depth.
//             A plain-too-deep stack is CRANK-GEN-014.
//   - Growth: the SAME generic recurring on the stack with STRICTLY GROWING
//             structural size is divergence — CRANK-GEN-015. A *repeated* key
//             (equal size) is ordinary, terminating recursion and is allowed.
//
// Resource controls (§14):
//   - Per-generic monomorphization budget (max_monomorphizations_per_generic) —
//     CRANK-GEN-016 naming the "monomorphizations" limit.
//   - Candidate-set size (max_trait_candidates) at resolution sites —
//     CRANK-GEN-016 naming the "trait candidates" limit.
//
// Every limit diagnostic carries the expansion CHAIN in its explanation (§14:
// "report the generic expansion chain rather than only 'limit exceeded'").
//
// Surfaces:
//   instantiation_limits     — tunable caps (generous defaults, caller-overridable)
//   structural_size          — cheap node count of an instantiation_key's args
//   expansion_frame          — one active instantiation on the stack
//   instantiation_guard      — RAII-ish push/pop depth + growth detector
//   monomorphization_budget  — per-generic monomorphization counter
//   check_candidate_count    — candidate-set size guard
//   monomorphize_bounded     — convenience driver: budget + guard + monomorphize

#include "languages/crank/monomorphize.hpp"
#include "languages/crank/diagnostic.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace crank {
    // ============================================================================
    // instantiation_limits — tunable compile-time caps. Defaults are generous; a
    // caller building under tighter constraints lowers them. No call site hardcodes.
    // ============================================================================

    struct instantiation_limits {
        std::uint32_t max_nesting_depth = 64;
        std::uint32_t max_monomorphizations_per_generic = 4096;
        std::uint32_t max_trait_candidates = 256;
    };

    // ============================================================================
    // structural_size — cheap structural measure of an instantiation's arguments.
    //
    // The current type_arg model is flat, so size is (type_args + const_args) plus a
    // per-arg constant that keeps a bare `Foo[]` distinct from `Foo` under growth
    // comparison. Honest about today's representation; deepen (recurse into nested
    // type structure) when type_arg carries sub-args.
    // ============================================================================

    [[nodiscard]] inline std::uint32_t structural_size(const instantiation_key& key) noexcept {
        return 1u
            + static_cast<std::uint32_t>(key.type_args.size())
            + static_cast<std::uint32_t>(key.const_args.size());
    }

    // ============================================================================
    // expansion_frame — one active instantiation on the guard's stack.
    // ============================================================================

    struct expansion_frame {
        std::string generic_name;
        std::uint64_t key_fingerprint = 0;
        std::uint32_t structural_size = 0;
    };

    // ============================================================================
    // instantiation_guard — push/pop depth + growth detector (§11.4).
    //
    // push() returns a diagnostic (and does NOT push) when a limit trips; otherwise
    // pushes a frame and returns nullopt. pop() removes the top frame. The guard is a
    // plain object — the caller owns push/pop pairing (see monomorphize_bounded).
    // ============================================================================

    class instantiation_guard {
    public:
        explicit instantiation_guard(instantiation_limits limits = {}) noexcept
            : limits_(limits) {}

        [[nodiscard]] std::optional<monomorphize_diagnostic>
        push(const instantiation_key& key, source_span at) {
            const std::uint32_t size = structural_size(key);
            const std::uint64_t fp = key.fingerprint();

            // CRANK-GEN-014 — depth limit. Would-be depth is stack + this frame.
            if (stack_.size() >= limits_.max_nesting_depth) {
                return limit_diag(
                    "CRANK-GEN-014",
                    std::string("instantiation nesting depth exceeded max of ")
                    + std::to_string(limits_.max_nesting_depth)
                    + " while expanding generic '" + key.generic_name + "'",
                    "reduce generic nesting, or raise instantiation_limits.max_nesting_depth",
                    key, size, fp, at);
            }

            // CRANK-GEN-015 — divergence: same generic already on the stack with a
            // STRICTLY SMALLER structural size (this frame grows it). A repeated
            // (equal-size) key is ordinary recursion and is permitted.
            for (const auto& f : stack_) {
                if (f.generic_name == key.generic_name && f.structural_size < size) {
                    return limit_diag(
                        "CRANK-GEN-015",
                        std::string("instantiation of generic '") + key.generic_name
                        + "' diverges: structural size grows from "
                        + std::to_string(f.structural_size) + " to "
                        + std::to_string(size) + " on the same expansion path",
                        "the generic instantiates itself on a strictly larger type; "
                        "add a base case or a bound that terminates the recursion",
                        key, size, fp, at);
                }
            }

            stack_.push_back(expansion_frame{key.generic_name, fp, size});
            return std::nullopt;
        }

        void pop() noexcept {
            if (!stack_.empty()) stack_.pop_back();
        }

        [[nodiscard]] std::size_t depth() const noexcept { return stack_.size(); }
        [[nodiscard]] const std::vector<expansion_frame>& stack() const noexcept { return stack_; }
        [[nodiscard]] const instantiation_limits& limits() const noexcept { return limits_; }

    private:
        // Build a diagnostic whose explanation lists the full expansion chain (each
        // active frame rendered `generic[fp,size]`) plus the frame that tripped it.
        [[nodiscard]] monomorphize_diagnostic
        limit_diag(std::string_view code, std::string summary, std::string_view help,
                   const instantiation_key& key, std::uint32_t size,
                   std::uint64_t fp, source_span at) const {
            auto ex = explain(std::string(code), std::move(summary), at);
            ex.note("generic expansion chain (outermost first):");
            for (const auto& f : stack_)
                ex.note(render_frame(f.generic_name, f.key_fingerprint, f.structural_size));
            ex.note(std::string("  → ") + render_frame(key.generic_name, fp, size));
            ex.help(std::string(help));
            auto built = ex.build();

            monomorphize_diagnostic d{built.render_message(), at, true};
            d.explanation = std::move(built);
            return d;
        }

        [[nodiscard]] static std::string
        render_frame(std::string_view name, std::uint64_t fp, std::uint32_t size) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(fp));
            return std::string(name) + "[fp=0x" + buf + ",size="
                + std::to_string(size) + "]";
        }

        instantiation_limits limits_;
        std::vector<expansion_frame> stack_;
    };

    // ============================================================================
    // monomorphization_budget — per-generic monomorphization counter (§14).
    //
    // charge(name) increments the generic's count and returns CRANK-GEN-016 once it
    // exceeds max_monomorphizations_per_generic. Counts persist across charges so a
    // build-wide cap holds even when instantiations are interleaved.
    // ============================================================================

    class monomorphization_budget {
    public:
        explicit monomorphization_budget(instantiation_limits limits = {}) noexcept
            : limits_(limits) {}

        [[nodiscard]] std::optional<monomorphize_diagnostic>
        charge(std::string_view generic_name, source_span at) {
            const std::uint32_t n = ++counts_[std::string(generic_name)];
            if (n > limits_.max_monomorphizations_per_generic) {
                std::string summary = std::string("generic '") + std::string(generic_name)
                    + "' exceeded the compile-time limit of "
                    + std::to_string(limits_.max_monomorphizations_per_generic)
                    + " monomorphizations";
                auto ex = explain("CRANK-GEN-016", std::move(summary), at)
                          .note("resource limit: monomorphizations per generic")
                          .note(std::string("count for '") + std::string(generic_name)
                              + "' reached " + std::to_string(n))
                          .help("reduce distinct instantiations, or raise "
                              "instantiation_limits.max_monomorphizations_per_generic")
                          .build();
                monomorphize_diagnostic d{ex.render_message(), at, true};
                d.explanation = std::move(ex);
                return d;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::uint32_t count(std::string_view generic_name) const {
            auto it = counts_.find(std::string(generic_name));
            return it == counts_.end() ? 0u : it->second;
        }

    private:
        instantiation_limits limits_;
        std::unordered_map<std::string, std::uint32_t> counts_;
    };

    // ============================================================================
    // check_candidate_count — guard a gathered candidate-impl set's size (§14).
    //
    // Resolution sites that collect candidate impls call this before ranking; a set
    // larger than max_trait_candidates is CRANK-GEN-016 naming the "trait candidates"
    // limit. Returns nullopt when within budget.
    // ============================================================================

    [[nodiscard]] inline std::optional<monomorphize_diagnostic>
    check_candidate_count(std::size_t n, std::string_view generic_name,
                          const instantiation_limits& limits, source_span at) {
        if (n <= limits.max_trait_candidates) return std::nullopt;
        std::string summary = std::string("resolution for '") + std::string(generic_name)
            + "' gathered " + std::to_string(n)
            + " candidate impls, exceeding the limit of "
            + std::to_string(limits.max_trait_candidates);
        auto ex = explain("CRANK-GEN-016", std::move(summary), at)
                  .note("resource limit: trait candidates")
                  .help("narrow the bound or reduce overlapping impls, or raise "
                      "instantiation_limits.max_trait_candidates")
                  .build();
        monomorphize_diagnostic d{ex.render_message(), at, true};
        d.explanation = std::move(ex);
        return d;
    }

    // ============================================================================
    // monomorphize_bounded — convenience driver: budget + guard + monomorphize.
    //
    // (a) charges the per-generic budget, (b) pushes the guard, (c) runs the plain
    // monomorphizer, (d) pops the guard, (e) merges any limit diagnostics into the
    // result. A tripped limit short-circuits: the monomorphizer does not run and the
    // limit diagnostic is the result's sole diagnostic. Callers that do not care
    // about limits keep calling monomorphizer::monomorphize directly — existing
    // behavior is byte-for-byte unchanged.
    // ============================================================================

    [[nodiscard]] inline monomorphize_result
    monomorphize_bounded(const monomorphizer& mm,
                         instantiation_guard& guard,
                         monomorphization_budget& budget,
                         const instantiation_key& key,
                         const trait_registry& registry,
                         const trait_set& required,
                         std::uint64_t fn_type_hash,
                         std::string_view fn_type_name,
                         source_span at) {
        monomorphize_result res;
        res.key = key;

        if (auto d = budget.charge(key.generic_name, at)) {
            res.diagnostics.push_back(std::move(*d));
            return res;
        }
        if (auto d = guard.push(key, at)) {
            res.diagnostics.push_back(std::move(*d));
            return res;
        }

        res = mm.monomorphize(key, registry, required, fn_type_hash, fn_type_name, at);
        guard.pop();
        return res;
    }
} // namespace crank
