#pragma once

// =============================================================================
// lithe_ir/portable/opt/manager.hpp — deterministic portable pass manager
//
// Namespace: lithe::ir::portable::opt
//
// static_pass_pipeline<Passes...>:
//   Compile-time fold over portable passes; zero type erasure on the hot path.
//   Fixed declared order + policy gating + mask invalidation.
//   Matches the arch §4.4 run-loop contract exactly.
//
// dynamic_pass_pipeline:
//   Type-erased pipeline for plugin/registry-assembled passes (cold path only).
//
// pass_record / pass_run_entry:
//   Reproducible provenance (arch §4.4, §7, §11.M2.4).
//   pipeline_provenance_digest(record) feeds the impl-3 artifact key.
//
// portable_pass_registry:
//   String-keyed, shared_mutex-guarded plugin registry (cold path).
//   Mirrors lithe_ir/registry.hpp conventions.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

#include "containers/canonical_codec.hpp"  // canonical_writer
#include "containers/dynamic/SmallVector.hpp"
#include "../digest.hpp"    // digest_algorithm, semantic_digest
#include "../verify.hpp"    // verify_portable
#include "analysis.hpp"     // analysis_cache, all_providers
#include "pass.hpp"         // pass_descriptor, semantic_policy, pass_outcome, portable_pass

namespace lithe::ir::portable::opt {
    // =============================================================================
    // pass_run_entry — single pass execution record
    // =============================================================================

    struct pass_run_entry {
        pass_id id;
        pass_version version;
        pass_outcome outcome;
        analysis_mask used; // analyses the pass read
        std::uint64_t ns = 0; // wall-clock nanoseconds (excluded from equality)
        bool skipped = false; // policy-incompatible skip
        std::string skip_reason;
    };

    // =============================================================================
    // pipeline_id / pipeline_version
    // =============================================================================

    struct pipeline_id {
        std::string name;
        [[nodiscard]] bool operator==(const pipeline_id&) const noexcept = default;
    };

    struct pipeline_version {
        std::uint16_t major = 1;
        std::uint16_t minor = 0;
        [[nodiscard]] bool operator==(const pipeline_version&) const noexcept = default;
    };

    // =============================================================================
    // pass_record — full provenance artifact (arch §4.4, §7, §11.M2.4)
    // =============================================================================

    struct pass_record {
        pipeline_id id;
        pipeline_version version;
        semantic_policy policy;
        containers::dynamic::SmallVector<pass_run_entry, 16> entries;

        [[nodiscard]] bool operator==(const pass_record& o) const noexcept {
            if (id != o.id || version != o.version || policy != o.policy) return false;
            if (entries.size() != o.entries.size()) return false;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& a = entries[i];
                const auto& b = o.entries[i];
                // Exclude ns from equality (timing is non-deterministic)
                if (a.id != b.id || a.version != b.version || a.outcome != b.outcome
                    || a.used != b.used || a.skipped != b.skipped)
                    return false;
            }
            return true;
        }
    };

    // =============================================================================
    // pipeline_provenance_digest — keying material for impl-3 artifact key
    //
    // Folds: pipeline id/version, policy, each pass id/version/outcome/used_mask.
    // Timing fields (ns) are excluded for determinism.
    // =============================================================================

    [[nodiscard]] inline std::array<std::uint8_t, 64>
    pipeline_provenance_digest(const pass_record& rec,
                               digest_algorithm alg = digest_algorithm::sha256) {
        containers::canonical_writer w;

        // Pipeline identity
        w.write_u32(static_cast<std::uint32_t>(rec.id.name.size()));
        for (unsigned char c : rec.id.name) w.write_u8(c);
        w.write_u16(rec.version.major);
        w.write_u16(rec.version.minor);

        // Policy
        w.write_u8(static_cast<std::uint8_t>(rec.policy.int_overflow));
        w.write_u8(static_cast<std::uint8_t>(rec.policy.fp));
        w.write_bool(rec.policy.preserve_defer);
        w.write_bool(rec.policy.preserve_exceptions);
        w.write_bool(rec.policy.preserve_transactions);
        w.write_bool(rec.policy.preserve_traps);
        w.write_u8(static_cast<std::uint8_t>(rec.policy.determinism));

        // Entries
        w.write_u32(static_cast<std::uint32_t>(rec.entries.size()));
        for (const auto& e : rec.entries) {
            w.write_u16(static_cast<std::uint16_t>(e.id));
            w.write_u16(e.version.major);
            w.write_u16(e.version.minor);
            w.write_u8(static_cast<std::uint8_t>(e.outcome));
            w.write_u32(e.used);
            w.write_bool(e.skipped);
        }
        w.finalize_string_table();

        const auto bytes = w.emit();
        const std::span<const std::uint8_t> sp{bytes.data(), bytes.size()};

        std::array<std::uint8_t, 64> out{};
        if (alg == digest_algorithm::sha256) {
            const auto h = containers::content_digest<containers::sha256_digest_policy>(sp);
            for (std::size_t i = 0; i < h.size() && i < out.size(); ++i) out[i] = h[i];
        }
        return out;
    }

    // =============================================================================
    // pipeline_result — output of running a pipeline
    // =============================================================================

    struct pipeline_result {
        bool ok = true;
        pass_record record;
        pass_diagnostics diags;
    };

    // =============================================================================
    // policy_compatible — check pass descriptor against active semantic_policy
    // =============================================================================

    [[nodiscard]] inline bool
    policy_compatible(const pass_descriptor& desc, const semantic_policy& pol) noexcept {
        // A pass that requires wrapping/fast_fp/undef is incompatible with conservative modes.
        if ((desc.policy & policy_compat_wrapping_ok) &&
            pol.int_overflow == integer_overflow_mode::trap)
            return false;
        if ((desc.policy & policy_compat_fast_fp_ok) && pol.fp == fp_mode::strict)
            return false;
        if ((desc.policy & policy_compat_undef_ok) &&
            pol.int_overflow != integer_overflow_mode::undef)
            return false;
        if (desc.determinism == determinism_class::nondeterministic)
            return false;
        return true;
    }

    // =============================================================================
    // static_pass_pipeline<Passes...> — compile-time ordered pipeline (arch §4.4)
    //
    // All passes are statically declared; the dependency check is compile-time where
    // possible.  The run loop respects policy_compatible, mask invalidation, and
    // records provenance.
    //
    // Passes must satisfy the portable_pass concept.
    // =============================================================================

    template <portable_pass... Passes>
    class static_pass_pipeline {
    public:
        explicit static_pass_pipeline(
            pipeline_id id,
            pipeline_version version,
            Passes... passes)
            : id_(std::move(id))
              , version_(std::move(version))
              , passes_(std::move(passes)...) {}

        [[nodiscard]] pipeline_result run(
            portable_module& mod,
            const semantic_policy& policy,
            all_providers& providers) const {
            pipeline_result result;
            result.record.id = id_;
            result.record.version = version_;
            result.record.policy = policy;

            analysis_cache cache;

            // Fold over passes at compile time
            bool ok = std::apply(
                [&](const Passes&... ps) {
                    return (run_one(ps, mod, cache, policy, providers, result) && ...);
                },
                passes_);

            result.ok = ok;

            // End-of-pipeline verification (arch §4.4: paranoid / debug/safe levels)
            if (policy.paranoid && ok) {
                auto vr = verify_portable(mod);
                if (!vr.ok) {
                    result.ok = false;
                    for (auto& d : vr.diagnostics)
                        result.diags.entries.push_back(std::move(d));
                }
            }

            return result;
        }

    private:
        pipeline_id id_;
        pipeline_version version_;
        std::tuple<Passes...> passes_;

        template <portable_pass P>
        [[nodiscard]] bool run_one(
            const P& pass,
            portable_module& mod,
            analysis_cache& cache,
            const semantic_policy& policy,
            all_providers& providers,
            pipeline_result& result) const {
            const auto desc = P::descriptor();
            pass_run_entry entry;
            entry.id = desc.id;
            entry.version = desc.version;
            entry.used = 0;

            // Policy compatibility gate
            if (!policy_compatible(desc, policy)) {
                entry.skipped = true;
                entry.skip_reason = "policy_incompatible";
                entry.outcome = pass_outcome::unchanged;
                result.record.entries.push_back(std::move(entry));
                return true;
            }

            // Ensure required analyses are computed
            ensure_analyses(cache, mod, providers, desc.requires_);
            entry.used = desc.requires_;

            // Time the pass
            const auto t0 = std::chrono::steady_clock::now();
            auto mutable_pass = pass; // passes are value types
            pass_diagnostics diags;
            const auto outcome = mutable_pass.run(mod, cache, policy, diags);
            const auto t1 = std::chrono::steady_clock::now();
            entry.ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            // Collect diagnostics
            for (auto& d : diags.entries) result.diags.entries.push_back(std::move(d));

            entry.outcome = outcome;
            if (outcome == pass_outcome::changed)
                cache.invalidate(desc.invalidates);

            result.record.entries.push_back(std::move(entry));

            if (outcome == pass_outcome::error) {
                result.ok = false;
                return false;
            }
            return true;
        }

        static void ensure_analyses(
            analysis_cache& cache,
            const portable_module& mod,
            all_providers& providers,
            analysis_mask required) {
            if (required & mask_dominance)
                (void)cache.get<dominance_facts>(mod, providers.dominance);
            if (required & mask_liveness)
                (void)cache.get<liveness_facts>(mod, providers.liveness);
            if (required & mask_effects)
                (void)cache.get<effects_facts>(mod, providers.effects);
            if (required & mask_purity)
                (void)cache.get<purity_facts>(mod, providers.purity);
            if (required & mask_ranges)
                (void)cache.get<ranges_facts>(mod, providers.ranges);
            if (required & mask_aliasing)
                (void)cache.get<aliasing_facts>(mod, providers.aliasing);
            if (required & mask_cfg_reachability)
                (void)cache.get<cfg_reachability_facts>(mod, providers.cfg_reachability);
        }
    };

    // Deduction guide
    template <portable_pass... Passes>
    static_pass_pipeline(pipeline_id, pipeline_version, Passes...)
    ->
    static_pass_pipeline<Passes...>;

    // =============================================================================
    // dynamic_pass — type-erased pass for plugin/registry use (cold path only)
    // =============================================================================

    class dynamic_pass {
    public:
        template <portable_pass P>
        explicit dynamic_pass(P p)
            : desc_(P::descriptor())
              , fn_([p = std::move(p)](portable_module& m, analysis_cache& c,
                                       const semantic_policy& sp,
                                       pass_diagnostics& d) mutable {
                  return p.run(m, c, sp, d);
              }) {}

        [[nodiscard]] const pass_descriptor& descriptor() const noexcept { return desc_; }

        [[nodiscard]] pass_outcome run(portable_module& m, analysis_cache& c,
                                       const semantic_policy& sp, pass_diagnostics& d) {
            return fn_(m, c, sp, d);
        }

    private:
        pass_descriptor desc_;
        std::function<pass_outcome(portable_module &, analysis_cache &,
                      const semantic_policy&, pass_diagnostics&
        )
        >
        fn_;
    };

    // =============================================================================
    // dynamic_pass_pipeline — erased pipeline for plugin-assembled sequences
    // (arch §4.4: erasure only at registry/selection boundary, never in hot path)
    // =============================================================================

    class dynamic_pass_pipeline {
    public:
        explicit dynamic_pass_pipeline(pipeline_id id, pipeline_version version)
            : id_(std::move(id)), version_(std::move(version)) {}

        void add(dynamic_pass p) { passes_.push_back(std::move(p)); }

        [[nodiscard]] pipeline_result run(
            portable_module& mod,
            const semantic_policy& policy,
            all_providers& providers) const {
            pipeline_result result;
            result.record.id = id_;
            result.record.version = version_;
            result.record.policy = policy;

            analysis_cache cache;

            for (auto& pass : const_cast<std::vector<dynamic_pass>&>(passes_)) {
                const auto& desc = pass.descriptor();
                pass_run_entry entry;
                entry.id = desc.id;
                entry.version = desc.version;
                entry.used = 0;

                if (!policy_compatible(desc, policy)) {
                    entry.skipped = true;
                    entry.skip_reason = "policy_incompatible";
                    entry.outcome = pass_outcome::unchanged;
                    result.record.entries.push_back(std::move(entry));
                    continue;
                }

                ensure_analyses_dyn(cache, mod, providers, desc.requires_);
                entry.used = desc.requires_;

                const auto t0 = std::chrono::steady_clock::now();
                pass_diagnostics diags;
                const auto outcome = pass.run(mod, cache, policy, diags);
                const auto t1 = std::chrono::steady_clock::now();
                entry.ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

                for (auto& d : diags.entries) result.diags.entries.push_back(std::move(d));

                entry.outcome = outcome;
                if (outcome == pass_outcome::changed) cache.invalidate(desc.invalidates);

                result.record.entries.push_back(std::move(entry));

                if (outcome == pass_outcome::error) {
                    result.ok = false;
                    return result;
                }
            }

            if (policy.paranoid && result.ok) {
                auto vr = verify_portable(mod);
                if (!vr.ok) {
                    result.ok = false;
                    for (auto& d : vr.diagnostics)
                        result.diags.entries.push_back(std::move(d));
                }
            }

            return result;
        }

    private:
        pipeline_id id_;
        pipeline_version version_;
        std::vector<dynamic_pass> passes_;

        static void ensure_analyses_dyn(analysis_cache& cache, const portable_module& mod,
                                        all_providers& providers, analysis_mask required) {
            if (required & mask_dominance)
                (void)cache.get<dominance_facts>(mod, providers.dominance);
            if (required & mask_liveness)
                (void)cache.get<liveness_facts>(mod, providers.liveness);
            if (required & mask_effects)
                (void)cache.get<effects_facts>(mod, providers.effects);
            if (required & mask_purity)
                (void)cache.get<purity_facts>(mod, providers.purity);
            if (required & mask_ranges)
                (void)cache.get<ranges_facts>(mod, providers.ranges);
            if (required & mask_aliasing)
                (void)cache.get<aliasing_facts>(mod, providers.aliasing);
            if (required & mask_cfg_reachability)
                (void)cache.get<cfg_reachability_facts>(mod, providers.cfg_reachability);
        }
    };

    // =============================================================================
    // portable_pass_registry — string-keyed plugin registration (cold path only)
    //
    // Mirrors lithe_ir/registry.hpp conventions.
    // Registration: insert a factory; selection: build a dynamic_pass by name.
    // =============================================================================

    class portable_pass_registry {
    public:
        using factory_fn = std::function<dynamic_pass()>;

        void register_pass(std::string name, factory_fn factory) {
            std::unique_lock lock(mutex_);
            factories_[std::move(name)] = std::move(factory);
        }

        [[nodiscard]] std::optional<dynamic_pass> create(const std::string& name) const {
            std::shared_lock lock(mutex_);
            const auto it = factories_.find(name);
            if (it == factories_.end()) return std::nullopt;
            return it->second();
        }

        [[nodiscard]] bool contains(const std::string& name) const {
            std::shared_lock lock(mutex_);
            return factories_.count(name) > 0;
        }

        [[nodiscard]] std::vector<std::string> registered_names() const {
            std::shared_lock lock(mutex_);
            std::vector<std::string> names;
            names.reserve(factories_.size());
            for (const auto& [k, _] : factories_) names.push_back(k);
            return names;
        }

    private:
        mutable std::shared_mutex mutex_;
        std::unordered_map<std::string, factory_fn> factories_;
    };
} // namespace lithe::ir::portable::opt
