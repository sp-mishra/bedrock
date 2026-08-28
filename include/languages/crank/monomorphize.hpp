#pragma once

// crank/monomorphize.hpp — Monomorphizer + generic_capability_summary (Module 5 B3).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Design refs: design.md §17.1/§17.6; impl-5.md Step B3.
//
// Each generic instantiation = distinct concrete Vākya subtree → distinct Lithe
// compilation → distinct cache key. Trait calls statically resolved (selected impl
// inlined; no runtime dictionary in hot loops — witness objects only at explicit
// dynamic boundaries: host-erased values, plugin edges, forced erasure).
//
// Surfaces:
//   type_arg         — a concrete type argument (type_id + name)
//   const_arg        — a concrete const argument (value + kind)
//   instantiation_key — (source_generic_name, type_args, const_args) → distinct key
//   generic_capability_summary — per-instantiation fact set consumed by planner + verifier
//   monomorphize_result — result of monomorphizing one instantiation
//   monomorphizer      — stateless monomorphization driver

#include "languages/crank/generics.hpp"
#include "languages/crank/const_dim.hpp"
#include "languages/crank/specialization.hpp"
#include "languages/crank/aot.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace crank {
    // ============================================================================
    // layout_kind / device_affinity — v2 layout + execution-device constraints (§v2.6)
    //
    // Distilled from bound_kind::Layout*/Device* by build_capability_summary and
    // consumed by the execution planner's backend ranking.
    // ============================================================================

    enum class layout_kind : std::uint8_t { none, row_major, col_major };

    enum class device_affinity : std::uint8_t { none, host, simd, gpu };

    [[nodiscard]] constexpr std::string_view to_string(layout_kind k) noexcept {
        switch (k) {
        case layout_kind::none: return "none";
        case layout_kind::row_major: return "row_major";
        case layout_kind::col_major: return "col_major";
        }
        return "none";
    }

    [[nodiscard]] constexpr std::string_view to_string(device_affinity d) noexcept {
        switch (d) {
        case device_affinity::none: return "none";
        case device_affinity::host: return "host";
        case device_affinity::simd: return "simd";
        case device_affinity::gpu: return "gpu";
        }
        return "none";
    }

    // ============================================================================
    // type_arg — concrete type argument for a generic instantiation
    // ============================================================================

    struct type_arg {
        std::uint32_t type_id = 0; // crank type system id
        std::string type_name; // human name ("Int64", "Float32", etc.)
        std::uint64_t type_hash = 0; // structural_hash of the concrete type
    };

    // ============================================================================
    // const_arg — concrete const generic argument
    // ============================================================================

    struct const_arg {
        const_param_kind kind;
        std::int64_t value = 0; // integer value for usize/isize
        bool bool_val = false;
        std::string enum_name; // for user_enum kind
    };

    // ============================================================================
    // instantiation_key — (source generic, type args, const args) = one monomorphization
    // ============================================================================

    struct instantiation_key {
        std::string generic_name; // e.g. "Reduce", "Dot2", "MapGpu"
        std::vector<type_arg> type_args;
        std::vector<const_arg> const_args;

        // Fingerprint for cache key (FNV-1a over all fields)
        [[nodiscard]] std::uint64_t fingerprint() const noexcept {
            constexpr std::uint64_t kOffset = 14695981039346656037ULL;
            constexpr std::uint64_t kPrime = 1099511628211ULL;
            std::uint64_t h = kOffset;

            auto mix_sv = [&](std::string_view sv) {
                for (char c : sv) {
                    h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
                    h *= kPrime;
                }
            };
            auto mix_u64 = [&](std::uint64_t v) {
                for (int i = 0; i < 8; ++i) {
                    h ^= static_cast<std::uint64_t>((v >> (i * 8)) & 0xFFu);
                    h *= kPrime;
                }
            };

            mix_sv(generic_name);
            for (const auto& ta : type_args) {
                mix_u64(ta.type_id);
                mix_u64(ta.type_hash);
            }
            for (const auto& ca : const_args) {
                mix_u64(static_cast<std::uint64_t>(ca.kind));
                mix_u64(static_cast<std::uint64_t>(ca.value));
            }
            return h;
        }

        [[nodiscard]] bool operator==(const instantiation_key& o) const noexcept {
            return fingerprint() == o.fingerprint();
        }
    };

    // ============================================================================
    // impl_witness — resolved impl for one bound at a specific call site
    // ============================================================================

    struct impl_witness {
        bound_kind bound;
        std::string trait_name;
        std::string type_name;
        std::uint64_t impl_type_hash = 0;
        // Module that defined the impl (from impl_record::module_name). Used by
        // use-site coherence (coherence.hpp) to reject impls not in scope (§v2.1a).
        std::string impl_module_name;
        // Associated const values resolved from this impl
        std::vector<std::pair<std::string, associated_const_value>> assoc_consts;
        std::optional<std::string> combine_fn_name; // from impl_record, for planner use
        // Associated type bindings resolved from this impl (v2).
        // Maps assoc_type_record::name → concrete type name.
        // e.g. { "Item" → "Int64" } when impl Collection for IntVec binds Item = Int64
        std::vector<std::pair<std::string, std::string>> assoc_type_map;
    };

    // ============================================================================
    // generic_capability_summary — per-instantiation fact set
    //
    // The single fact source consumed by:
    //   - automatic execution planner (module 4): effects/capabilities/reductions
    //   - Tarka verifier (module 3): proof obligations
    //
    // Reduction facts (from Monoid-like assoc consts) allow a generic Reduce to
    // parallelize while respecting @parallel(deterministic=true).
    // ============================================================================

    struct generic_capability_summary {
        std::string generic_name;
        instantiation_key key;
        std::vector<impl_witness> witnesses; // resolved impl per bound

        // Effect / capability bitmasks (from effects.hpp extension band)
        std::uint64_t effect_mask = 0;
        std::uint64_t capability_mask = 0;

        // Satisfied bounds (subset of required bounds after impl resolution)
        trait_set satisfied_bounds;

        // Reduction facts (from Monoid assoc consts — feeds planner)
        bool associative = false;
        bool commutative = false;
        bool parallel_safe = false; // ParallelSafe bound satisfied
        bool gpu_compatible = false; // GpuCompatible bound satisfied

        // Resource traits (from transaction bounds)
        bool transactional = false;
        bool snapshot_capable = false;
        bool aba_safe = true;

        // Layout + device constraints (v2, §v2.6) — distilled from Layout*/Device*/Simd
        // bounds. Fed into the execution planner's backend ranking. `none` = unconstrained.
        layout_kind layout = layout_kind::none;
        device_affinity device = device_affinity::none;
        bool simd_eligible = false; // SimdEligible bound satisfied

        // Proof obligations (bounds that need verification: shape dims, etc.)
        std::vector<std::string> pending_proof_labels;

        // Combine function name from Monoid impl (e.g. "Int64::add").
        // Empty when no Monoid bound is satisfied or impl has no combine_fn_name.
        // The execution planner checks this before emitting a parallel reduction.
        std::optional<std::string> combine_fn_name;

        // Merged associated type map from all witnesses (v2).
        // Maps assoc_type_record::name → concrete type name across all resolved impls.
        // Projection C.Item is resolved by looking up the assoc_name in this map.
        std::vector<std::pair<std::string, std::string>> assoc_type_map;

        // Cache key for the instantiation (extends module 4 AOT key)
        std::uint64_t cache_key_fingerprint = 0;

        // Layout info (size/alignment of concrete type — deferred to layout pass)
        std::uint32_t concrete_type_id = 0;
    };

    // ============================================================================
    // monomorphize_diagnostic — per-instantiation diagnostic
    // ============================================================================

    struct monomorphize_diagnostic {
        std::string message;
        source_span at;
        bool is_error = true;
        std::optional<diag_explanation> explanation; // §v2.1a structured explanation
    };

    // ============================================================================
    // monomorphize_result — output of monomorphizing one instantiation
    // ============================================================================

    struct monomorphize_result {
        instantiation_key key;
        generic_capability_summary summary;
        std::vector<impl_witness> witnesses;
        std::vector<monomorphize_diagnostic> diagnostics;
        bool has_runtime_dictionary = false; // always false in hot-loop path

        [[nodiscard]] bool ok() const noexcept {
            for (const auto& d : diagnostics) if (d.is_error) return false;
            return true;
        }
    };

    // ============================================================================
    // resolve_witnesses — look up impl witnesses for required bounds against a type
    //
    // Emits a monomorphize_diagnostic for each unsatisfied bound.
    // Hot path: all witnesses resolved statically; no dictionary emitted unless
    // forced (host-erased boundary).
    // ============================================================================

    [[nodiscard]] inline std::vector<impl_witness>
    resolve_witnesses(
        const conformance_table& table,
        const trait_registry& registry,
        std::uint64_t type_hash,
        std::string_view type_name,
        const trait_set& required,
        std::vector<monomorphize_diagnostic>& diags,
        source_span at) {
        std::vector<impl_witness> ws;

        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(bound_kind::_Count); ++i) {
            const auto bk = static_cast<bound_kind>(i);
            if (!required.has(bk)) continue;

            const trait_descriptor* td = registry.find_trait_by_name(to_string(bk));
            if (!td) continue;

            const impl_record* rec = table.find(type_hash, td->id);

            // §8.2 implication: no direct impl — accept an impl of any trait that
            // implies this bound (e.g. an `Ordered` impl witnesses a `Comparable`
            // requirement). A direct impl always wins when present (deterministic).
            if (!rec) {
                for (std::uint8_t j = 0; j < static_cast<std::uint8_t>(bound_kind::_Count); ++j) {
                    const auto sbk = static_cast<bound_kind>(j);
                    if (sbk == bk) continue;
                    const trait_descriptor* std_ = registry.find_trait_by_name(to_string(sbk));
                    if (!std_) continue;
                    const impl_record* srec = table.find(type_hash, std_->id);
                    if (srec && trait_implies(registry, sbk, bk)) {
                        rec = srec;
                        break;
                    }
                }
            }

            if (!rec) {
                monomorphize_diagnostic d{
                    std::string("CRANK-GEN-001: no impl of '")
                    + std::string(to_string(bk)) + "' for type '"
                    + std::string(type_name) + "' at this instantiation",
                    at, true
                };
                d.explanation = explain("CRANK-GEN-001",
                                        d.message.substr(std::string_view("CRANK-GEN-001: ").size()), at)
                                .expected(std::string(to_string(bk)))
                                .found(std::string(type_name))
                                .note("required to monomorphize generic '"
                                    + std::string(type_name) + "' against bound '"
                                    + std::string(to_string(bk)) + "'")
                                .help(std::string("add `impl ") + std::string(to_string(bk))
                                    + " for " + std::string(type_name) + "`")
                                .build();
                diags.push_back(std::move(d));
                continue;
            }

            impl_witness w;
            w.bound = bk;
            w.trait_name = std::string(to_string(bk));
            w.type_name = rec->type_name;
            w.impl_type_hash = rec->type_hash;
            w.impl_module_name = rec->module_name;
            w.assoc_consts = rec->assoc_const_values;
            w.combine_fn_name = rec->combine_fn_name;
            // Propagate associated type bindings from impl_record (v2)
            w.assoc_type_map = rec->assoc_type_bindings;
            ws.push_back(std::move(w));
        }
        return ws;
    }

    // ============================================================================
    // build_capability_summary — assemble generic_capability_summary from witnesses
    // ============================================================================

    [[nodiscard]] inline generic_capability_summary
    build_capability_summary(
        const instantiation_key& key,
        const std::vector<impl_witness>& witnesses,
        const trait_set& satisfied) {
        generic_capability_summary s;
        s.generic_name = key.generic_name;
        s.key = key;
        s.witnesses = witnesses;
        s.satisfied_bounds = satisfied;
        s.cache_key_fingerprint = key.fingerprint();

        for (const auto& w : witnesses) {
            s.parallel_safe |= (w.bound == bound_kind::ParallelSafe);
            s.gpu_compatible |= (w.bound == bound_kind::GpuCompatible);
            s.transactional |= (w.bound == bound_kind::Transactional
                || w.bound == bound_kind::TransactionalResource);
            s.snapshot_capable |= (w.bound == bound_kind::SnapshotCapable);

            // v2 §v2.6: distill layout / device / simd bounds into planner facts.
            s.simd_eligible |= (w.bound == bound_kind::SimdEligible);
            if (w.bound == bound_kind::LayoutRowMajor) s.layout = layout_kind::row_major;
            if (w.bound == bound_kind::LayoutColMajor) s.layout = layout_kind::col_major;
            if (w.bound == bound_kind::DeviceGpu) s.device = device_affinity::gpu;
            if (w.bound == bound_kind::DeviceSimd) s.device = device_affinity::simd;
            if (w.bound == bound_kind::DeviceHost) s.device = device_affinity::host;

            // Extract algebraic facts from Monoid witness
            if (w.bound == bound_kind::Monoid) {
                for (const auto& [name, val] : w.assoc_consts) {
                    if (name == "associative"
                        && val.kind == associated_const_value::kind_t::bool_val)
                        s.associative = val.value.b;
                    if (name == "commutative"
                        && val.kind == associated_const_value::kind_t::bool_val)
                        s.commutative = val.value.b;
                }
                if (w.combine_fn_name) s.combine_fn_name = w.combine_fn_name;
            }

            // Merge associated type bindings into the summary map (v2).
            // Later witnesses overwrite earlier ones if names collide (last-wins; callers
            // using ambiguous projections should detect via CRANK-GEN-010 in resolve_assoc_type).
            for (const auto& [aname, aval] : w.assoc_type_map) {
                auto it = std::ranges::find_if(s.assoc_type_map,
                                               [&](const auto& p) { return p.first == aname; });
                if (it == s.assoc_type_map.end())
                    s.assoc_type_map.emplace_back(aname, aval);
                else
                    it->second = aval;
            }
        }

        return s;
    }

    // ============================================================================
    // monomorphizer — stateless driver: given a key + registry, produce a result
    // ============================================================================

    class monomorphizer {
    public:
        monomorphizer() = default;

        // Monomorphize a generic instantiation.
        //   key          — (generic_name, type_args, const_args)
        //   registry     — trait registry with conformance table
        //   required     — bounds required by the generic's parameter constraints
        //   fn_type_hash — structural hash of the concrete type (for conformance lookup)
        //   fn_type_name — human name for diagnostics
        //   at           — source location for diagnostics
        [[nodiscard]] monomorphize_result
        monomorphize(
            const instantiation_key& key,
            const trait_registry& registry,
            const trait_set& required,
            std::uint64_t fn_type_hash,
            std::string_view fn_type_name,
            source_span at) const {
            return monomorphize(key, registry, required, fn_type_hash, fn_type_name, at,
                                [](std::string_view) -> std::optional<std::uint64_t> {
                                    return std::nullopt;
                                });
        }

        // Overload accepting a concrete-type-name → structural-hash resolver, used to
        // enforce v2 §v2.2 associated-type bounds (CRANK-GEN-012). When the resolver
        // returns nullopt for a name, that binding's bound check is skipped (unknown
        // types are reported by name resolution, not here).
        template <class ResolveHashFn>
        [[nodiscard]] monomorphize_result
        monomorphize(
            const instantiation_key& key,
            const trait_registry& registry,
            const trait_set& required,
            std::uint64_t fn_type_hash,
            std::string_view fn_type_name,
            source_span at,
            ResolveHashFn&& resolve_concrete_hash) const {
            monomorphize_result res;
            res.key = key;
            res.has_runtime_dictionary = false; // hot path: statically resolved

            const auto& ctable = registry.conformances();
            res.witnesses = resolve_witnesses(
                ctable, registry, fn_type_hash, fn_type_name,
                required, res.diagnostics, at);

            if (!res.ok()) return res;

            // Build satisfied set from found witnesses
            trait_set satisfied;
            for (const auto& w : res.witnesses)
                satisfied.add(w.bound);

            res.summary = build_capability_summary(key, res.witnesses, satisfied);

            // v2 §v2.1/§v2.2: enforce associated-type bindings for every witness whose
            // trait declares associated types. Missing bindings → CRANK-GEN-011;
            // bindings that violate a declared bound → CRANK-GEN-012.
            for (const auto& w : res.witnesses) {
                const trait_descriptor* td = registry.find_trait_by_name(w.trait_name);
                if (!td || td->assoc_types.empty()) continue;

                auto push_conf = [&](const conformance_diagnostic& d) {
                    monomorphize_diagnostic m{d.message, d.at, d.is_error};
                    m.explanation = d.explanation; // carry structured explanation through
                    res.diagnostics.push_back(std::move(m));
                };

                // CRANK-GEN-011: every declared assoc type must be bound by the impl.
                for (const auto& ar : td->assoc_types) {
                    const bool bound = std::ranges::any_of(w.assoc_type_map,
                                                           [&](const auto& p) { return p.first == ar.name; });
                    if (!bound)
                        push_conf(make_missing_assoc_binding_diag(
                            at, ar.name, td->name, fn_type_name));
                }

                // CRANK-GEN-012: each binding must satisfy the assoc type's bounds.
                for (const auto& d : check_assoc_type_bounds(
                         *td, w.assoc_type_map, ctable, registry,
                         resolve_concrete_hash, at))
                    push_conf(d);
            }

            // Callable bound check: Fn/FnMut/FnOnce — effectful fn rejects GpuCompatible
            // If the callable has effects and GpuCompatible is required → reject
            const bool callable_req = required.has(bound_kind::Fn)
                || required.has(bound_kind::FnMut)
                || required.has(bound_kind::FnOnce);
            const bool gpu_req = required.has(bound_kind::GpuCompatible);
            // Effect check: if effect_mask has non-pure bits and GpuCompatible is required
            if (callable_req && gpu_req && res.summary.effect_mask != 0) {
                res.diagnostics.push_back({
                    "CRANK-GEN-GPU: callable bound Fn/FnMut/FnOnce + GpuCompatible requires "
                    "an effect-free (Pure) function; the supplied callable has non-pure effects",
                    at, true
                });
            }

            return res;
        }
    };

    // ============================================================================
    // evaluate_const_dim_arg — evaluate a const-generic dimension expression (§v2.4)
    //
    // Wires const_dim.hpp into the monomorphization path. Given a dim_expr in a
    // dimension position (e.g. `[N + 1]`, `[M * K]`) and the instantiation's const
    // bindings, produce a concrete const_arg or a monomorphize_diagnostic carrying
    // the CRANK-GEN-DIM-001/002/003/004 code.
    //
    // Const bindings are taken from a name → int64 map (built from the generic's
    // const_param names and the instantiation's const_args).
    // ============================================================================

    [[nodiscard]] inline dim_bindings
    make_dim_bindings(const std::vector<const_generic_param>& params,
                      const std::vector<const_arg>& args) {
        dim_bindings b;
        const std::size_t n = std::min(params.size(), args.size());
        for (std::size_t i = 0; i < n; ++i)
            b.emplace(params[i].name, args[i].value);
        return b;
    }

    [[nodiscard]] inline std::expected<const_arg, monomorphize_diagnostic>
    evaluate_const_dim_arg(const dim_expr& expr,
                           const dim_bindings& bindings,
                           const_param_kind kind,
                           source_span at) {
        const bool nonneg = (kind == const_param_kind::usize);
        auto r = evaluate_dim(expr, bindings, at, nonneg);
        if (!r) {
            return std::unexpected(monomorphize_diagnostic{
                r.error().message, r.error().at, r.error().is_error
            });
        }
        const_arg ca;
        ca.kind = kind;
        ca.value = *r;
        return ca;
    }

    // ============================================================================
    // instantiation_registry — tracks all monomorphized instantiations per session
    // Provides distinct cache keys (one per instantiation) and JSON dump source.
    //
    // v2 additions:
    //   §v2.5  — holds a specialization_table; select_specialization() picks the most
    //            specific impl for a (trait, concrete_type_hash) with overlap checks.
    //   §v2.14 — record() deduplicates instantiations by fingerprint so cross-module
    //            monomorphizations of the same (generic, type_args, const_args) collapse
    //            to a single cached entry.
    // ============================================================================

    class instantiation_registry {
    public:
        // Record an instantiation, deduplicating by fingerprint (§v2.14).
        // Returns true if newly recorded, false if a matching instantiation existed.
        bool record(monomorphize_result res) {
            const std::uint64_t fp = res.summary.cache_key_fingerprint;
            if (fp != 0 && seen_.count(fp)) return false; // dedup
            if (fp != 0) seen_.insert(fp);
            results_.push_back(std::move(res));
            return true;
        }

        [[nodiscard]] const std::vector<monomorphize_result>& all() const noexcept {
            return results_;
        }

        [[nodiscard]] std::size_t count() const noexcept { return results_.size(); }

        // Extend a crank_aot_key with all instantiation fingerprints, in ascending
        // fingerprint order so the resulting AOT key is independent of the order in
        // which instantiations were recorded (§v2.1a stable metadata / ABI).
        void extend_aot_key(crank_aot_key& key) const {
            std::vector<std::uint64_t> fps;
            fps.reserve(results_.size());
            for (const auto& r : results_)
                fps.push_back(r.summary.cache_key_fingerprint);
            std::sort(fps.begin(), fps.end());
            for (std::uint64_t fp : fps)
                key.descriptor_hashes.push_back(fp);
        }

        // --- §v2.5 controlled specialization -----------------------------------

        // Register a specialization after running overlap/coherence checks.
        // Diagnostics (CRANK-GEN-007/008/009) are appended to `diags`; the record is
        // added regardless so later selection can still resolve, but an error diag
        // signals the caller to reject the program.
        void add_specialization(specialization_record rec,
                                std::uint64_t base_effect_mask,
                                std::string_view current_module,
                                std::vector<specialization_diagnostic>& diags,
                                source_span at) {
            auto d = check_specialization_overlap(
                specializations_, rec, base_effect_mask, current_module, at);
            for (auto& x : d) diags.push_back(std::move(x));
            specializations_.add(std::move(rec));
        }

        // Select the most-specific specialization for (trait, concrete_type_hash).
        // Returns nullopt when no specialization matches (caller uses the generic impl).
        [[nodiscard]] std::optional<const specialization_record*>
        select_specialization(trait_id tid, std::uint64_t type_hash,
                              std::vector<specialization_diagnostic>& diags,
                              source_span at) const {
            return select_impl(specializations_, tid, type_hash, diags, at);
        }

        [[nodiscard]] const specialization_table& specializations() const noexcept {
            return specializations_;
        }

    private:
        std::vector<monomorphize_result> results_;
        std::unordered_set<std::uint64_t> seen_; // §v2.14 dedup
        specialization_table specializations_; // §v2.5
    };
} // namespace crank
