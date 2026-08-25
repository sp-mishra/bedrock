#pragma once

// =============================================================================
// lithe_pgo.hpp — Profile-Guided Optimization seam (namespace lithe::intelligence)
//
// Provides:
//   profile_hint         — POD hint struct (hot/cold/unroll/vectorize)
//   profile_source<P>    — concept: the seam every decision point consults
//   no_profile           — zero-cost no-op default (identity bias, no hints)
//   recorded_profile     — real source built from collected stage_metric data
//   learned_profile      — ML adapter (opt-in; fallback = no_profile)
//   any_profile_source   — type-erased wrapper (cold boundary only)
//   update_from_metrics  — closes the metrics → feedback_store loop
//
// Design:
//   - All decision entry points (impl-2 optimizer, impl-4 planner, cost model)
//     take `const ProfileSrc& = no_profile{}` as a defaulted template param.
//   - With no_profile, output is byte-identical and deterministic vs pre-impl-7.
//   - Swapping to recorded_profile or learned_profile flips all three consistently.
//   - any_profile_source is for runtime-selected sources at cold config sites only.
//
// Opt-in: NOT pulled by lithe.hpp.  Include explicitly when PGO is needed.
// Depends on: lithe_metrics/stage.hpp, lithe_cost_model.hpp, lithe_feedback.hpp.
//             lithe_ml_interfaces.hpp for learned_profile (guarded).
//
// No virtual, no macros.  C++23.
// =============================================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "lithe/lithe_cost_model.hpp"        // cost_vector
#include "lithe/lithe_feedback.hpp"           // feedback_store, hardware_signature, performance_sample
#include "lithe/lithe_metrics/stage.hpp"      // stage_metric, pipeline_stage

// learned_profile is conditionally compiled — include must be at file scope
// (never inside a namespace block) to avoid namespace pollution.
#if __has_include("lithe/lithe_ml_interfaces.hpp")
#  include "lithe/lithe_ml_interfaces.hpp"
#  define LITHE_PGO_HAS_ML 1
#else
#  define LITHE_PGO_HAS_ML 0
#endif

namespace lithe::intelligence {
    // =============================================================================
    // profile_hint — actionable hints derived from a profile_source
    // =============================================================================

    struct profile_hint {
        bool hot = false;
        bool cold = false;
        std::uint8_t suggested_unroll = 0;
        bool prefer_vectorize = false;
        bool prefer_inline = false;
        bool avoid_inline = false;
    };

    // =============================================================================
    // profile_source<P> concept
    // =============================================================================

    template <class P>
    concept profile_source =
        requires(const P& p,
                 std::span<const std::uint8_t> digest,
                 metrics::pipeline_stage s) {
            { p.has_profile(digest) } -> std::same_as<bool>;
            { p.stage_bias(digest, s) } -> std::convertible_to<cost::cost_vector>;
            { p.hint(digest, s) } -> std::convertible_to<profile_hint>;
        };

    // =============================================================================
    // no_profile — zero-cost no-op default
    // =============================================================================

    struct no_profile {
        static constexpr bool available = false;

        [[nodiscard]] bool has_profile(
            std::span<const std::uint8_t>) const noexcept { return false; }

        [[nodiscard]] cost::cost_vector stage_bias(
            std::span<const std::uint8_t>,
            metrics::pipeline_stage) const noexcept { return {}; }

        [[nodiscard]] profile_hint hint(
            std::span<const std::uint8_t>,
            metrics::pipeline_stage) const noexcept { return {}; }
    };

    static_assert(profile_source<no_profile>);
    static_assert(std::is_empty_v<no_profile>);

    // =============================================================================
    // recorded_profile — build a real source from collected stage_metric data
    // =============================================================================

    class recorded_profile {
    public:
        static constexpr bool available = true;

        // Configurable thresholds — plain struct, no nested default member init
        // inside a class body to avoid Clang parser ordering issues.
        struct config {
            double hot_fraction_threshold;
            double trim_fraction;
        };

        static constexpr config default_config() noexcept {
            return {0.30, 0.10};
        }

        explicit recorded_profile(
            std::span<const metrics::stage_metric> samples,
            config cfg = default_config(),
            feedback::feedback_store* store = nullptr)
            : cfg_{cfg}, store_{store} {
            ingest(samples);
        }

        [[nodiscard]] bool has_profile(
            std::span<const std::uint8_t> digest) const noexcept {
            return find_entry(digest) != nullptr;
        }

        [[nodiscard]] cost::cost_vector stage_bias(
            std::span<const std::uint8_t> digest,
            metrics::pipeline_stage s) const noexcept {
            const entry* e = find_entry(digest);
            if (!e) return {};
            const auto idx = static_cast<std::uint8_t>(s);
            if (idx >= metrics::k_pipeline_stage_count) return {};
            return e->stage_bias[idx];
        }

        [[nodiscard]] profile_hint hint(
            std::span<const std::uint8_t> digest,
            metrics::pipeline_stage s) const noexcept {
            const entry* e = find_entry(digest);
            if (!e) return {};
            const auto idx = static_cast<std::uint8_t>(s);
            if (idx >= metrics::k_pipeline_stage_count) return {};
            return e->stage_hints[idx];
        }

    private:
        struct entry {
            std::array<std::uint8_t, 64> key{};
            std::uint8_t key_len = 0;
            std::array<cost::cost_vector, metrics::k_pipeline_stage_count> stage_bias{};
            std::array<profile_hint, metrics::k_pipeline_stage_count> stage_hints{};
        };

        [[nodiscard]] const entry* find_entry(
            std::span<const std::uint8_t> digest) const noexcept {
            for (const auto& e : entries_) {
                if (e.key_len == 0 || digest.empty()) continue;
                const std::size_t cmp = std::min(
                    static_cast<std::size_t>(e.key_len), digest.size());
                if (std::equal(e.key.begin(), e.key.begin() + cmp, digest.data()))
                    return &e;
            }
            return nullptr;
        }

        void ingest(std::span<const metrics::stage_metric> samples) {
            for (const auto& m : samples) {
                if (m.unit_digest_len == 0) continue;

                entry* e = nullptr;
                const std::span<const std::uint8_t> dig{
                    m.unit_digest.data(),
                    static_cast<std::size_t>(m.unit_digest_len)
                };
                for (auto& candidate : entries_) {
                    const std::size_t cmp = std::min(
                        static_cast<std::size_t>(candidate.key_len), dig.size());
                    if (cmp > 0 && std::equal(candidate.key.begin(),
                                              candidate.key.begin() + cmp,
                                              dig.data())) {
                        e = &candidate;
                        break;
                    }
                }
                if (!e) {
                    entries_.push_back({});
                    e = &entries_.back();
                    e->key_len = m.unit_digest_len;
                    const std::size_t copy_len = std::min(
                        static_cast<std::size_t>(m.unit_digest_len), e->key.size());
                    std::copy(m.unit_digest.begin(),
                              m.unit_digest.begin() + copy_len,
                              e->key.begin());
                }

                const auto idx = static_cast<std::uint8_t>(m.stage);
                if (idx >= metrics::k_pipeline_stage_count) continue;

                auto& bias = e->stage_bias[idx];
                const auto& est = m.estimated;
                const auto& meas = m.measured;
                if (est.latency > 0.0 && meas.latency > 0.0)
                    bias.latency = meas.latency / est.latency;
                if (est.memory > 0.0 && meas.memory > 0.0)
                    bias.memory = meas.memory / est.memory;
                if (est.power > 0.0 && meas.power > 0.0)
                    bias.power = meas.power / est.power;
                if (est.throughput > 0.0 && meas.throughput > 0.0)
                    bias.throughput = meas.throughput / est.throughput;
            }

            // Derive hotness hints from wall_ns fractions
            for (auto& e : entries_) {
                std::uint64_t total_ns = 0;
                std::array<std::uint64_t, metrics::k_pipeline_stage_count> ns_per_stage{};
                ns_per_stage.fill(0u);
                for (const auto& m : samples) {
                    if (m.unit_digest_len == 0) continue;
                    const std::size_t cmp = std::min(
                        static_cast<std::size_t>(e.key_len),
                        static_cast<std::size_t>(m.unit_digest_len));
                    if (cmp == 0 || !std::equal(e.key.begin(), e.key.begin() + cmp,
                                                m.unit_digest.data()))
                        continue;
                    const auto idx = static_cast<std::uint8_t>(m.stage);
                    if (idx < metrics::k_pipeline_stage_count) {
                        ns_per_stage[idx] += m.wall_ns;
                        total_ns += m.wall_ns;
                    }
                }
                if (total_ns == 0) continue;
                for (std::uint8_t i = 0; i < metrics::k_pipeline_stage_count; ++i) {
                    const double frac = static_cast<double>(ns_per_stage[i])
                        / static_cast<double>(total_ns);
                    e.stage_hints[i].hot = frac >= cfg_.hot_fraction_threshold;
                    e.stage_hints[i].cold = frac < (cfg_.hot_fraction_threshold * 0.1);
                }
            }

            // Optionally persist to feedback_store
            if (!store_) return;
            const auto hw = feedback::hardware_signature::current();
            for (const auto& m : samples) {
                if (m.unit_digest_len == 0) continue;
                std::size_t digest_hash = 0;
                for (std::uint8_t i = 0;
                     i < std::min<std::uint8_t>(m.unit_digest_len, 8); ++i)
                    digest_hash = digest_hash * 31u + m.unit_digest[i];

                feedback::performance_sample ps;
                ps.expression_hash = digest_hash;
                ps.hw = hw;
                ps.latency_ms = m.measured.latency * 1e-6;
                ps.throughput_gops = m.measured.throughput;
                ps.memory_mb = m.measured.memory / (1024.0 * 1024.0);
                ps.power_w = m.measured.power;
                store_->record(std::move(ps));
            }
        }

        config cfg_;
        feedback::feedback_store* store_ = nullptr;
        std::vector<entry> entries_;
    };

    static_assert(profile_source<recorded_profile>);

    // =============================================================================
    // learned_profile — ML adapter (available when lithe_ml_interfaces.hpp present)
    // =============================================================================

#if LITHE_PGO_HAS_ML

    class learned_profile {
    public:
        static constexpr bool available = true;

        using infer_fn_t = std::function<
            std::pair<cost::cost_vector, profile_hint>(
                std::span<const std::uint8_t>,
                metrics::pipeline_stage)>;

        void set_model(infer_fn_t fn) { infer_fn_ = std::move(fn); }
        [[nodiscard]] bool trained() const noexcept { return static_cast<bool>(infer_fn_); }

        [[nodiscard]] bool has_profile(
            std::span<const std::uint8_t>) const noexcept { return trained(); }

        [[nodiscard]] cost::cost_vector stage_bias(
            std::span<const std::uint8_t> digest,
            metrics::pipeline_stage s) const noexcept {
            if (!infer_fn_) return {};
            return infer_fn_(digest, s).first;
        }

        [[nodiscard]] profile_hint hint(
            std::span<const std::uint8_t> digest,
            metrics::pipeline_stage s) const noexcept {
            if (!infer_fn_) return {};
            return infer_fn_(digest, s).second;
        }

    private:
        infer_fn_t infer_fn_;
    };

    static_assert(profile_source<learned_profile>);

#endif  // LITHE_PGO_HAS_ML

    // =============================================================================
    // any_profile_source — type-erased profile source (cold boundary only)
    // =============================================================================

    class any_profile_source {
    public:
        static constexpr bool available = true;

        any_profile_source() { bind(no_profile{}); }

        template <profile_source P>
        explicit any_profile_source(P src) { bind(std::move(src)); }

        template <profile_source P>
        void assign(P src) { bind(std::move(src)); }

        [[nodiscard]] bool has_profile(
            std::span<const std::uint8_t> digest) const noexcept {
            return has_fn_ ? has_fn_(ctx_.get(), digest) : false;
        }

        [[nodiscard]] cost::cost_vector stage_bias(
            std::span<const std::uint8_t> digest,
            metrics::pipeline_stage s) const noexcept {
            return bias_fn_ ? bias_fn_(ctx_.get(), digest, s) : cost::cost_vector{};
        }

        [[nodiscard]] profile_hint hint(
            std::span<const std::uint8_t> digest,
            metrics::pipeline_stage s) const noexcept {
            return hint_fn_ ? hint_fn_(ctx_.get(), digest, s) : profile_hint{};
        }

    private:
        template <profile_source P>
        void bind(P src) {
            ctx_ = std::make_shared<P>(std::move(src));
            has_fn_ = [](void* p, std::span<const std::uint8_t> d) noexcept {
                return static_cast<P*>(p)->has_profile(d);
            };
            bias_fn_ = [](void* p, std::span<const std::uint8_t> d,
                          metrics::pipeline_stage s) noexcept {
                return static_cast<P*>(p)->stage_bias(d, s);
            };
            hint_fn_ = [](void* p, std::span<const std::uint8_t> d,
                          metrics::pipeline_stage s) noexcept {
                return static_cast<P*>(p)->hint(d, s);
            };
        }

        using has_fn_t = bool(*)(void*, std::span<const std::uint8_t>) noexcept;
        using bias_fn_t = cost::cost_vector(*)(void*, std::span<const std::uint8_t>,
                                               metrics::pipeline_stage) noexcept;
        using hint_fn_t = profile_hint(*)(void*, std::span<const std::uint8_t>,
                                          metrics::pipeline_stage) noexcept;

        std::shared_ptr<void> ctx_;
        has_fn_t has_fn_ = nullptr;
        bias_fn_t bias_fn_ = nullptr;
        hint_fn_t hint_fn_ = nullptr;
    };

    static_assert(profile_source<any_profile_source>);

    // =============================================================================
    // update_from_metrics — close the metrics → feedback_store loop
    // =============================================================================

    inline void update_from_metrics(
        feedback::feedback_store& store,
        std::span<const metrics::stage_metric> samples) {
        const auto hw = feedback::hardware_signature::current();
        for (const auto& m : samples) {
            if (m.unit_digest_len == 0) continue;
            if (m.measured.latency == 0.0 && m.measured.throughput == 0.0) continue;

            std::size_t digest_hash = 0;
            for (std::uint8_t i = 0;
                 i < std::min<std::uint8_t>(m.unit_digest_len, 8); ++i)
                digest_hash = digest_hash * 31u + m.unit_digest[i];
            digest_hash ^= (static_cast<std::size_t>(m.stage) << 48u);

            feedback::performance_sample ps;
            ps.expression_hash = digest_hash;
            ps.hw = hw;
            ps.latency_ms = m.measured.latency * 1e-6;
            ps.throughput_gops = m.measured.throughput;
            ps.memory_mb = m.measured.memory / (1024.0 * 1024.0);
            ps.power_w = m.measured.power;
            store.record(std::move(ps));
        }
    }
} // namespace lithe::intelligence
