#pragma once

// =============================================================================
// lithe_metrics/metrics_view.hpp — read-only query surface over collected metrics
//
// Namespace: lithe::metrics
//
// Provides:
//   metrics_view — read-only, non-owning view over a span<const stage_metric>
//
// All query methods are const / non-mutating / allocation-free (small scratch).
// This is the surface impl-5 ir_inspector exposes (attach_metrics / metrics()).
//
// No virtual, no macros.  C++23.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "stage.hpp"   // stage_metric, pipeline_stage, cost_vector

namespace lithe::metrics {
    // =============================================================================
    // metrics_view
    // =============================================================================

    class metrics_view {
    public:
        metrics_view() noexcept = default;

        explicit metrics_view(std::span<const stage_metric> data) noexcept
            : data_{data} {}

        // -------------------------------------------------------------------------
        // Basic accessors
        // -------------------------------------------------------------------------

        [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
        [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
        [[nodiscard]] std::span<const stage_metric> all() const noexcept { return data_; }

        // -------------------------------------------------------------------------
        // Per-stage aggregation
        // Returns a single stage_metric with all numeric fields summed over all
        // records matching the requested pipeline_stage.  Returns nullopt if no
        // records for that stage exist.
        // -------------------------------------------------------------------------

        [[nodiscard]] std::optional<stage_metric>
        for_stage(pipeline_stage s) const noexcept {
            bool found = false;
            stage_metric agg{};
            agg.stage = s;
            for (const auto& m : data_) {
                if (m.stage != s) continue;
                found = true;
                agg.entity_count += m.entity_count;
                agg.wall_ns += m.wall_ns;
                agg.cycles += m.cycles;
                agg.iterations += m.iterations;
                agg.rule_fired += m.rule_fired;
                agg.diag_errors = static_cast<std::uint16_t>(
                    agg.diag_errors + m.diag_errors);
                agg.diag_warnings = static_cast<std::uint16_t>(
                    agg.diag_warnings + m.diag_warnings);
                // cost_vector: accumulate estimated and measured
                agg.estimated.latency += m.estimated.latency;
                agg.estimated.memory += m.estimated.memory;
                agg.estimated.power += m.estimated.power;
                agg.estimated.throughput += m.estimated.throughput;
                agg.measured.latency += m.measured.latency;
                agg.measured.memory += m.measured.memory;
                agg.measured.power += m.measured.power;
                agg.measured.throughput += m.measured.throughput;
                // last seen unit_digest wins (all records for a unit share the same)
                agg.unit_digest = m.unit_digest;
                agg.unit_digest_len = m.unit_digest_len;
            }
            if (!found) return std::nullopt;
            return agg;
        }

        // -------------------------------------------------------------------------
        // Per-unit filtering (by semantic_digest prefix match)
        // Returns a subspan view of records whose unit_digest matches exactly.
        // NOTE: returns a vector since the matching records may not be contiguous.
        // -------------------------------------------------------------------------

        [[nodiscard]] std::vector<stage_metric>
        for_unit(std::span<const std::uint8_t> digest) const {
            std::vector<stage_metric> out;
            for (const auto& m : data_) {
                if (m.unit_digest_len == 0) continue;
                const std::size_t cmp_len = std::min(
                    static_cast<std::size_t>(m.unit_digest_len), digest.size());
                if (cmp_len == 0) continue;
                if (std::equal(m.unit_digest.begin(),
                               m.unit_digest.begin() + cmp_len,
                               digest.data()))
                    out.push_back(m);
            }
            return out;
        }

        // -------------------------------------------------------------------------
        // Totals (over all stages / all records)
        // -------------------------------------------------------------------------

        [[nodiscard]] lithe::cost::cost_vector total_estimated() const noexcept {
            lithe::cost::cost_vector acc{};
            for (const auto& m : data_) {
                acc.latency += m.estimated.latency;
                acc.memory += m.estimated.memory;
                acc.power += m.estimated.power;
                acc.throughput += m.estimated.throughput;
            }
            return acc;
        }

        [[nodiscard]] lithe::cost::cost_vector total_measured() const noexcept {
            lithe::cost::cost_vector acc{};
            for (const auto& m : data_) {
                acc.latency += m.measured.latency;
                acc.memory += m.measured.memory;
                acc.power += m.measured.power;
                acc.throughput += m.measured.throughput;
            }
            return acc;
        }

        [[nodiscard]] std::uint64_t total_wall_ns() const noexcept {
            std::uint64_t t = 0;
            for (const auto& m : data_) t += m.wall_ns;
            return t;
        }

        // -------------------------------------------------------------------------
        // Bottleneck analysis
        // -------------------------------------------------------------------------

        // Returns the pipeline_stage with the highest total wall_ns.
        // Returns nullopt if no records.
        [[nodiscard]] std::optional<pipeline_stage> hottest_stage() const noexcept {
            if (data_.empty()) return std::nullopt;

            // Accumulate wall_ns per stage into a fixed-size array (no allocation)
            std::array<std::uint64_t, k_pipeline_stage_count> ns{};
            ns.fill(0u);
            for (const auto& m : data_) {
                const auto idx = static_cast<std::uint8_t>(m.stage);
                if (idx < k_pipeline_stage_count) ns[idx] += m.wall_ns;
            }
            const auto it = std::ranges::max_element(ns);
            if (*it == 0) return std::nullopt;
            return static_cast<pipeline_stage>(
                static_cast<std::uint8_t>(it - ns.begin()));
        }

        // Fraction of total wall_ns spent at stage s (0.0 if total == 0).
        [[nodiscard]] double stage_fraction(pipeline_stage s) const noexcept {
            const std::uint64_t total = total_wall_ns();
            if (total == 0) return 0.0;
            std::uint64_t stage_ns = 0;
            for (const auto& m : data_)
                if (m.stage == s) stage_ns += m.wall_ns;
            return static_cast<double>(stage_ns) / static_cast<double>(total);
        }

    private:
        std::span<const stage_metric> data_;
    };
} // namespace lithe::metrics
