#pragma once

// lithe_rewrite.hpp — AST fixpoint rewrite engine for Lithe.
//
// Not included by lithe.hpp; opt-in via:
//   #include "lithe/lithe_rewrite.hpp"
//
// Namespace: lithe::rewrite
//
// Quick-start:
//   using namespace lithe::rewrite;
//   namespace pat = lithe::pattern;
//
//   auto my_rules = pat::make_rule_set(
//       pat::rule(pat::add(pat::pv<0>, pat::lit<0>),
//                 [](const pat::match_result& m) -> std::optional<std::any> {
//                     return m.get<std::any>(std::size_t{0});
//                 }));
//
//   auto [result, changed] = rewrite_pass(expr, my_rules);
//   auto optimized         = rewrite_fixpoint(expr, my_rules);
//
// Design constraints:
//   - NO virtual, NO macros
//   - C++23: explicit object parameter, concepts, [[no_unique_address]], consteval
//   - Header-only

#include "lithe_passes.hpp"    // lithe_core.hpp + lithe_extension.hpp + pass infrastructure
#include "lithe_pattern.hpp"   // rule_set, match_result, Pattern

#include <any>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lithe::rewrite {
    // ============================================================================
    // 1.  rewrite_visitor — internal transform that applies a rule_set top-down.
    //
    //     Satisfies the lithe::evaluate / lithe::rewrite_once visitor contract:
    //       on_terminal(T) → T unchanged
    //       on_node(Tag, orig_ch..., transformed_ch...) → try rules, or rebuild
    //
    //     Note: lithe::transform calls on_node with BOTH original children (for
    //     pattern matching) AND recursively-transformed children (for rebuilding
    //     unchanged subtrees).  rewrite_visitor uses the original children for
    //     the pattern match and the transformed children for reconstruction.
    // ============================================================================

    namespace detail {
        // A thin adapter: make a rule_set look like a rewrite_once Rule.
        // rewrite_once calls:
        //   r.on_terminal(T&&) → T (unchanged)
        //   r.on_node(Tag, orig..., transformed...) → result
        //
        // We match against orig (pre-transform children) and rebuild with transformed.
        template <class RuleSet>
        struct rewrite_visitor {
            const RuleSet& rules;

            template <class T>
            constexpr T on_terminal(T&& x) const {
                return std::forward<T>(x);
            }

            // transform() passes: tag, orig_ch0, ..., orig_chN, xfm_ch0, ..., xfm_chN
            // We build a temporary node from the original children to match the pattern,
            // and if the rule fires we try to extract the rebuilt node.  If no rule
            // fires we return a node built from the transformed children.
            template <class Tag, class... HalfArgs>
            constexpr auto on_node(Tag tag, HalfArgs&&... half_args) const {
                // half_args = [orig_ch0 .. orig_chN, xfm_ch0 .. xfm_chN]
                // Split at the midpoint.
                constexpr std::size_t total = sizeof...(HalfArgs);
                constexpr std::size_t N = total / 2;

                return [&]<std::size_t... Io, std::size_t... Ix>(
                    std::index_sequence<Io...>, std::index_sequence<Ix...>) -> decltype(auto) {
                        auto args_tuple = std::forward_as_tuple(std::forward<HalfArgs>(half_args)...);

                        // Build a node from *original* children for pattern matching.
                        auto orig_node = lithe::make_node<Tag>(std::get < Io > (args_tuple)...);

                        // Try to apply the first matching rule against the original-child node.
                        auto rule_result = rules.apply_first(orig_node);
                        if (rule_result.has_value()) {
                            // The rule fired.  We return the any-wrapped result as-is.
                            // Caller must handle the any; here we return the any value.
                            // For the type-safe variant see rewrite_pass below.
                            return std::any_cast<decltype(lithe::make_node<Tag>(
                                std::get < N + Ix > (args_tuple)...))>(*rule_result);
                        }

                        // No rule fired — return node rebuilt from transformed children.
                        return lithe::make_node<Tag>(std::get < N + Ix > (args_tuple)...);
                    }(std::make_index_sequence < N >
                {}
                ,
                std::make_index_sequence < N >
                {}
                )
                ;
            }
        };
    } // namespace detail

    // ============================================================================
    // 2.  rewrite_pass — single bottom-up pass applying a rule_set.
    //
    //     Returns pair<transformed_expr, bool changed>.
    //     "changed" is true when at least one rule fired during the pass.
    //
    //     Because the rule_set's try_apply returns std::optional<std::any> and
    //     the expression type changes on each rewrite, we use lithe::rewrite_once
    //     which handles the recursion.  We detect change by comparing structural
    //     hashes of input and output.
    // ============================================================================

    // Simplified rule adapter for rewrite_once (no original/transformed split).
    // Applies rules on fully-rebuilt nodes (bottom-up: children are already rewritten).
    namespace detail {
        template <class RuleSet>
        struct bottom_up_rule {
            const RuleSet& rules;

            template <class T>
            constexpr T on_node(T /*orig_tag_unused*/, T&& node_rebuilt) const {
                // Not used — on_node receives tag + transformed children.
                return std::forward<T>(node_rebuilt);
            }

            template <class T>
            constexpr T on_terminal(T&& x) const {
                return std::forward<T>(x);
            }
        };

        // A simpler rewrite visitor compatible with lithe::evaluate (single-view).
        // on_terminal: pass through.
        // on_node(Tag, transformed_children...): rebuild, then try rules on result.
        template <class RuleSet>
        struct eval_rewrite_visitor {
            const RuleSet& rules;
            mutable bool changed = false;

            template <class T>
            constexpr std::decay_t<T> on_terminal(T&& x) const {
                return std::forward<T>(x);
            }

            template <class Tag, class... XfmChildren>
            constexpr auto on_node(Tag, XfmChildren&&... xfm) const {
                // Decay children to value types so rebuilt node holds values, not refs.
                auto rebuilt = lithe::make_node<Tag>(std::decay_t<XfmChildren>(std::forward<XfmChildren>(xfm))...);

                // Try to fire a rule on the rebuilt node.
                auto result = rules.apply_first(rebuilt);
                if (!result.has_value()) {
                    return rebuilt;
                }

                // Rule fired — attempt to extract the same node type.
                using rebuilt_t = decltype(rebuilt);
                auto* extracted = std::any_cast<rebuilt_t>(&*result);
                if (extracted) {
                    changed = true;
                    return std::move(*extracted);
                }
                // Rule fired but returned incompatible type — still mark changed,
                // return rebuilt node (best effort; caller can inspect result via any).
                changed = true;
                return rebuilt;
            }
        };
    } // namespace detail

    /// Apply a rule_set in a single bottom-up pass over the expression.
/// Returns {transformed_expr, changed}.
    template <class Expr, class RuleSet>
    [[nodiscard]] auto rewrite_pass(Expr&& expr, const RuleSet& rules)
        -> std::pair<std::decay_t<Expr>, bool> {
        detail::eval_rewrite_visitor<RuleSet> visitor{rules};
        auto result = lithe::evaluate(std::forward<Expr>(expr), visitor);
        return {std::move(result), visitor.changed};
    }

    // ============================================================================
    // 3.  rewrite_fixpoint — iterate rewrite_pass until stable or max_iters.
    //
    //     Returns an optimized_expr<E> wrapping the final result.
    // ============================================================================

    /// Repeatedly apply rewrite_pass until the expression stops changing.
/// Returns lithe::optimized_expr<E> wrapping the stabilised result.
    template <class Expr, class RuleSet>
    [[nodiscard]] auto rewrite_fixpoint(
        Expr expr,
        const RuleSet& rules,
        std::size_t max_iters = 16)
        -> lithe::optimized_expr<std::decay_t<Expr>> {
        for (std::size_t i = 0; i < max_iters; ++i) {
            auto [next, changed] = rewrite_pass(std::move(expr), rules);
            if (!changed) {
                return lithe::optimized_expr<std::decay_t<decltype(next)>>{std::move(next)};
            }
            expr = std::move(next);
        }
        return lithe::optimized_expr<std::decay_t<Expr>>{std::move(expr)};
    }

    // ============================================================================
    // 4.  rewrite_pass_adapter<RuleSet, PassId, MaxIters, StableId>
    //
    //     Wraps a rule_set as a Lithe pass object with pass_type_traits metadata.
    //     The pass expects any Lithe Expression and produces optimized_expr<E>.
    //
    //     operator()(IR) -> pass_result
    //       Accepts the raw expression (or surface_expr / canonical_expr).
    //       Returns optimized_expr<inner_type>.
    //
    //     pass_type_traits specialisation (below):
    //       category  = pass_category::optimization
    //       in_stage  = ir_stage::surface
    //       out_stage = ir_stage::optimized
    //       stable_id = StableId  (default: lithe::emit::kExtensionIdBase)
    // ============================================================================

    template <
        class RuleSet,
        lithe::fixed_string PassId,
        std::size_t MaxIters = 16,
        std::size_t StableId = lithe::emit::kExtensionIdBase
    >
    struct rewrite_pass_adapter {
        [[no_unique_address]] RuleSet rules;

        constexpr explicit rewrite_pass_adapter(RuleSet rs) : rules(std::move(rs)) {}

        // Default-constructible if RuleSet is.
        constexpr rewrite_pass_adapter()
            requires std::default_initializable<RuleSet>
        = default;

        // ---- operator() ----
        // Accepts a raw Expression.
        template <lithe::Expression Expr>
        [[nodiscard]] auto operator()(Expr&& expr) const {
            return rewrite_fixpoint(std::forward < Expr > (expr), rules, MaxIters);
        }

        // Accepts surface_expr<E> → unwrap, rewrite, re-wrap.
        template <class E>
            requires lithe::is_surface_expr_v<std::decay_t<E>>
        [[nodiscard]] auto operator()(E&& se) const {
            return rewrite_fixpoint(std::forward<E>(se).value, rules, MaxIters);
        }

        // Accepts canonical_expr<E> → unwrap, rewrite, re-wrap.
        template <class E>
            requires lithe::is_canonical_expr_v<std::decay_t<E>>
        [[nodiscard]] auto operator()(E&& ce) const {
            return rewrite_fixpoint(std::forward<E>(ce).value, rules, MaxIters);
        }

        // Accepts optimized_expr<E> → unwrap, rewrite, re-wrap (further optimise).
        template <class E>
            requires lithe::is_optimized_expr_v<std::decay_t<E>>
        [[nodiscard]] auto operator()(E&& oe) const {
            return rewrite_fixpoint(std::forward<E>(oe).value, rules, MaxIters);
        }
    };
} // namespace lithe::rewrite

// ============================================================================
// 5.  pass_type_traits specialisation for rewrite_pass_adapter
// ============================================================================

namespace lithe::passes {
    template <
        class RuleSet,
        lithe::fixed_string PassId,
        std::size_t MaxIters,
        std::size_t StableId
    >
    struct pass_type_traits<
            lithe::rewrite::rewrite_pass_adapter<RuleSet, PassId, MaxIters, StableId>
        > : pass_type_traits_base {
        static constexpr auto id = PassId;
        static constexpr lithe::version_triple version{1, 0, 0};
        static constexpr pass_category category = pass_category::optimization;
        static constexpr ir_stage in_stage = ir_stage::surface;
        static constexpr ir_stage out_stage = ir_stage::optimized;
        static constexpr std::size_t stable_id = StableId;
    };
} // namespace lithe::passes
