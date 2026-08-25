#pragma once

// =============================================================================
// lithe_autotune.hpp — Profile-variant auto-tuner  (opt-in, NOT in lithe.hpp)
//
// Namespace:  lithe::tune
// Depends on: lithe/lithe_profiles.hpp   (profile<Bundle,Desc> — P::descriptor,
//                                        P{}(expr))
//             utils/profiler.hpp         (measure, compare, ProfileResult)
//
// Provides:
//   tune_result        — winner profile id + per-variant results + significance
//   auto_tuner<ProfileVariants...>
//       .tune(expr, bench, iters) → tune_result
//         Applies each variant's profile::operator() to expr, times each via
//         profiler::measure, selects winner by Mann-Whitney significance test
//         (profiler::compare); ties broken by determinism flag on descriptor.
//
// Design:
//   • Zero cost unless this header is included.
//   • No new benchmarking engine — delegates to profiler.hpp entirely.
//   • No virtual, no macros.  C++23.
// =============================================================================

#include "lithe_profiles.hpp"

#include <utils/profiler.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace lithe::tune {
    // =============================================================================
    // tune_result — outcome of a tuning run
    // =============================================================================

    struct tune_result {
        std::string_view winner_id; // P::descriptor.id of the winning variant
        std::size_t winner_variant_index = 0; // 0-based index into ProfileVariants
        bool is_significant = false; // Mann-Whitney p < 0.05 vs. index-0 baseline
        double speedup_vs_baseline = 0.0; // winner median / baseline median
        std::vector<profiler::ProfileResult> variant_results; // one entry per variant
    };

    // =============================================================================
    // detail
    // =============================================================================

    namespace detail {
        // Time one profile variant: apply P{}(expr) per iteration, call bench on result.
        template <class P, class Expr, class Bench>
        profiler::ProfileResult time_variant(const Expr& expr, Bench& bench,
                                             std::size_t iters) {
            profiler::ProfileConfig cfg;
            cfg.iterations = iters;
            cfg.warmup_iterations = std::min<std::size_t>(10, iters / 10);
            cfg.label = std::string{std::string_view{P::descriptor.id}};

            return profiler::measure(cfg, [&] {
                auto optimised = P{}(expr);
                bench(optimised);
            });
        }

        // Collect results for all variants via fold expression.
        template <class Expr, class Bench, class TuplePfs, std::size_t... Is>
        std::vector<profiler::ProfileResult>
        collect(const Expr& expr, Bench& bench, std::size_t iters,
                std::index_sequence<Is...>) {
            std::vector<profiler::ProfileResult> out;
            out.reserve(sizeof...(Is));
            (out.push_back(
                    time_variant<std::tuple_element_t<Is, TuplePfs>>(expr, bench, iters)),
                ...);
            return out;
        }

        // Index of the variant with the lowest median duration.
        inline std::size_t pick_fastest(const std::vector<profiler::ProfileResult>& rs) {
            std::size_t best = 0;
            for (std::size_t i = 1; i < rs.size(); ++i)
                if (rs[i].median() < rs[best].median())
                    best = i;
            return best;
        }
    } // namespace detail

    // =============================================================================
    // auto_tuner<ProfileVariants...>
    // =============================================================================

    template <class... ProfileVariants>
        requires (sizeof...(ProfileVariants) >= 1)
    struct auto_tuner {
        static constexpr std::size_t variant_count = sizeof...(ProfileVariants);

        // tune(expr, bench, iters)
        //   bench: callable(auto optimised_expr) — invoked by profiler per iteration.
        //   iters: profiler iterations per variant (default 100).
        template <class Expr, class Bench>
        [[nodiscard]] tune_result tune(const Expr& expr, Bench&& bench,
                                       std::size_t iters = 100) const {
            using pf_tuple = std::tuple<ProfileVariants...>;

            auto results = detail::collect<Expr, Bench, pf_tuple>(
                expr, bench, iters, std::make_index_sequence < variant_count >
            {}
            )
            ;

            const std::size_t fast_idx = detail::pick_fastest(results);
            const auto& baseline = results[0];
            const auto& winner_r = results[fast_idx];

            bool is_sig = false;
            double speedup = 1.0;
            if (fast_idx != 0 && baseline.median().count() > 0) {
                auto cmp = profiler::compare(baseline, winner_r);
                is_sig = cmp.is_significant;
                speedup = cmp.speedup_factor;
            }
            else if (fast_idx == 0) {
                is_sig = true;
                speedup = 1.0;
            }

            // Tie-break: if not significant, prefer the first deterministic variant
            // whose median is no worse than the raw winner.
            std::size_t final_idx = fast_idx;
            if (!is_sig) {
                constexpr std::array<bool, variant_count> dets{
                    ProfileVariants::descriptor.deterministic...
                };
                for (std::size_t i = 0; i < variant_count; ++i) {
                    if (dets[i] && results[i].median() <= winner_r.median()) {
                        final_idx = i;
                        break;
                    }
                }
            }

            // Build a constexpr array of id string_views for indexed access.
            // id is a char[32] — wrap in string_view at compile time.
            constexpr std::array<const char*, variant_count> raw_ids{
                ProfileVariants::descriptor.id...
            };

            return tune_result{
                .winner_id = std::string_view{raw_ids[final_idx]},
                .winner_variant_index = final_idx,
                .is_significant = is_sig,
                .speedup_vs_baseline = speedup,
                .variant_results = std::move(results),
            };
        }
    };
} // namespace lithe::tune
