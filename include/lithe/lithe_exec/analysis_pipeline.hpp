#pragma once

// =============================================================================
// lithe_exec/analysis_pipeline.hpp — auto_execution_pass orchestrator
//
// Namespace: lithe::exec
//
// Provides:
//   auto_execution_pass — the top-level pass tying all lithe_exec analysis together.
//     run(fn, policy, target, hints) → vector<execution_plan>
//
// Pipeline (§1.1 design diagram):
//   hl_mir_function
//     → [EXTERNAL] build_pdg_pass           (SSA/dominance/deps)
//     → [EXTERNAL] extract_polyhedral_pass  (affine iteration space)
//     → effect_summary extraction
//     → memory_summary extraction
//     → region_class classification
//     → reduction recognition
//     → layout_summary derivation
//     → for each candidate kind:
//         legality check  → profitability estimate
//     → selection (auto_exec_selection_strategy)
//     → runtime_guard generation for unknown-outcome regions
//     → execution_plan assembly + versioned_plan (if needed)
//
// Transaction-region conservatism (Medha §2.5):
//   - Regions classified as transaction_region → forced scalar.
//   - @parallel or @gpu(required) inside a transaction → compile diagnostic.
//
// Design:
//   The pass consumes pre-built PDG and polyhedral results (callers have run
//   build_pdg_pass + extract_polyhedral_pass before calling run()).
//   Free-standing, no virtuals. Diagnostics emitted via injected Sink.
//   No virtual, no macros. Header-only C++23. Opt-in lithe_exec layer.
// =============================================================================

#include "exec_hint.hpp"
#include "effect_summary.hpp"
#include "execution_plan.hpp"
#include "layout_summary.hpp"
#include "legality.hpp"
#include "memory_summary.hpp"
#include "profitability.hpp"
#include "reduction.hpp"
#include "runtime_guard.hpp"
#include "selection.hpp"

// lithe_pdg.hpp and lithe_poly.hpp are internal fragments;
// lithe_codegen_pipeline.hpp defines the types they depend on.
#include "../lithe_codegen_pipeline.hpp"
#include "../lithe_codegen_hl.hpp"
#include "../lithe_pdg.hpp"
#include "../lithe_poly.hpp"
#include "../lithe_diagnostics.hpp"

#include <optional>
#include <span>
#include <vector>

namespace lithe::exec {
    // =========================================================================
    // auto_execution_pass — orchestrates all analysis and produces execution plans
    //
    // Usage:
    //   auto_execution_pass pass;
    //   auto plans = pass.run(fn, pdg_result, poly_result, policy, target, hints);
    //   // plans[i] describes the analysis outcome for region i in fn.
    // =========================================================================

    struct auto_execution_pass {
        // run — main entry point
        //
        // Parameters:
        //   fn         — HL MIR function (post build_pdg_pass + polyhedral_pass)
        //   pdg_result — pre-built PDG (build_pdg_pass output)
        //   poly_result— pre-built polyhedral analysis (extract_polyhedral_pass output)
        //   policy     — planning policy (from user / profile)
        //   target     — target hardware capabilities
        //   hints      — per-region hints from frontend attributes
        //   sink       — diagnostic sink (default: null_sink, zero cost)
        //
        // Returns a vector of execution_plan, one per loop region in fn.
        // Scalar regions get execution_kind::scalar (proven_legal, no guards).
        // Transaction regions are forced scalar with a note diagnostic if hinted.

        template <lithe::diag::diagnostic_sink Sink = lithe::diag::null_sink>
        [[nodiscard]] std::vector<execution_plan> run(
            const lithe::codegen::hl::hl_mir_function& fn,
            const lithe::pdg::pdg_build_result& pdg_result,
            const lithe::poly::polyhedral_analysis_result& poly_result,
            const auto_execution_policy& policy,
            const target_capabilities& target,
            std::span<const execution_hint> hints = {},
            Sink& sink = lithe::diag::null_sink{}) const {
            (void)fn; // HL MIR traversal deferred to phase-based callers

            std::vector<execution_plan> plans;
            plans.reserve(poly_result.loops.size());

            for (std::size_t i = 0; i < poly_result.loops.size(); ++i) {
                const auto& ploop = poly_result.loops[i];

                execution_plan plan;
                plan.region_id = static_cast<std::uint32_t>(i);

                // --- Effect summary (fold from PDG node annotations) ----------
                // In full implementation: walk pdg_result nodes for this region
                // and fold effect annotations from Vākya property store.
                // Here we produce a safe over-approximation: reads + writes.
                effect_summary effects;
                effects.add(effect_kind::reads_memory);
                effects.add(effect_kind::writes_memory);
                // Unknown effects are possible for non-affine regions.
                if (!ploop.is_affine) effects.has_unknown_effect = true;

                // --- Check for transaction region ----------------------------
                const bool in_transaction = effects.has(effect_kind::transaction);

                // --- region_class classification ----------------------------
                region_class cls = region_class::unknown;
                if (in_transaction) {
                    cls = region_class::transaction_region;
                }
                else if (ploop.is_affine && pdg_result.graph.edge_count() == 0) {
                    // No edges at all → fully independent
                    cls = region_class::independent_loop;
                }
                else if (ploop.is_affine) {
                    // May have loop-carried: check data edges for this region's nodes
                    bool has_lc = false;
                    for (const auto& node : pdg_result.graph.nodes()) {
                        for (const auto& e : pdg_result.graph.out_edges(node.instr_id)) {
                            if (e.is_data() &&
                                e.data_kind == lithe::pdg::data_dep_kind::raw) {
                                has_lc = true;
                                break;
                            }
                        }
                        if (has_lc) break;
                    }
                    cls = has_lc ? region_class::scalar_only : region_class::independent_loop;
                }

                plan.classification = cls;

                // --- Reduction recognition (stub) ----------------------------
                // Full: run recognize_reductions(ploop, pdg_result).
                std::vector<reduction_info> reductions;

                // --- Layout summary (derived from memref_type) ---------------
                layout_summary layout;
                if (!ploop.ivars.empty()) {
                    layout.rank = 1;
                    const auto& iv = ploop.ivars[0];
                    layout.dims[0] = static_cast<std::int64_t>(
                        iv.bounds.upper - iv.bounds.lower);
                    layout.strides[0] = 1;
                    layout.contiguous = ploop.is_affine;
                    layout.alignment = target.vector_width_bytes;
                    layout.space = address_space::host;
                }

                // --- loop_info_view ------------------------------------------
                loop_info_view lview;
                lview.has_loop = !ploop.ivars.empty();
                lview.is_affine = ploop.is_affine;
                lview.trip_count_known = !ploop.ivars.empty() &&
                    ploop.ivars[0].bounds.fully_known();
                if (lview.trip_count_known && !ploop.ivars.empty()) {
                    const auto& b = ploop.ivars[0].bounds;
                    lview.trip_count = b.upper - b.lower;
                }
                lview.depth = static_cast<std::uint32_t>(ploop.ivars.size());

                // --- dependency_summary from PDG edges -----------------------
                dependency_summary deps;
                deps.has_unknown_dep = !ploop.is_affine;
                // Full: classify each edge in pdg_result.graph for this region.

                // --- memory summary (stub over PDG) --------------------------
                memory_summary msummary;
                msummary.aliases.has_unknown_aliasing = !ploop.is_affine;

                plan.effects = effects.region_effects;
                plan.memory = msummary;
                plan.layout = layout;
                plan.dependencies = deps;

                // --- Transaction region: force scalar + diagnose hints -------
                if (in_transaction || cls == region_class::transaction_region) {
                    plan.kind = execution_kind::scalar;
                    plan.legality = analysis_outcome::proven_legal;

                    // Emit note if a non-scalar hint is present
                    for (const auto& h : hints) {
                        if (h.preferred && *h.preferred != execution_kind::scalar) {
                            lithe::diag::diagnostic d;
                            d.level = h.required
                                          ? lithe::diag::severity::error
                                          : lithe::diag::severity::note;
                            d.stage = lithe::diag::stage::backend;
                            d.code = h.required
                                         ? "parallel_rejected_transaction"
                                         : "parallel_downgraded_transaction";
                            d.message = "Region is inside a Medha transaction — "
                                "parallelization is conservatively rejected";
                            sink.on_diagnostic(d);
                        }
                    }

                    plans.push_back(std::move(plan));
                    continue;
                }

                // --- Selection via strategy ----------------------------------
                region_context rctx;
                rctx.region_id = plan.region_id;
                rctx.cls = cls;
                rctx.in_transaction = in_transaction;
                rctx.deps = &deps;
                rctx.reductions = reductions;

                exec_candidate_context cctx;
                cctx.effects = &effects;
                cctx.memory = &msummary;
                cctx.layout = &layout;
                cctx.loop = &lview;
                cctx.region = &rctx;
                cctx.target = &target;
                cctx.policy = &policy;
                cctx.hints = hints;

                const execution_kind chosen = select_execution_kind(cctx, sink);
                const analysis_outcome legality = check_legality(
                    chosen, rctx, effects, msummary, lview, layout, target, policy);

                plan.kind = chosen;
                plan.legality = legality;
                plan.cost = estimate(chosen, msummary, layout, target, {}, lview);

                // --- Runtime guards for unknown-outcome cases ----------------
                if (legality == analysis_outcome::unknown) {
                    // Alias guard
                    if (msummary.has_unknown_aliasing()) {
                        plan.guards.push_back({
                            .kind = guard_kind::no_alias, .operand_a = 0, .operand_b = 1
                        });
                    }
                    // Trip-count guard for threaded/gpu
                    if ((chosen == execution_kind::threaded || chosen == execution_kind::gpu)
                        && lview.has_loop && !lview.trip_count_known) {
                        plan.guards.push_back({
                            .kind = guard_kind::min_trip_count,
                            .operand_a = 0,
                            .constant = 64 // default min-profitable trip count
                        });
                    }
                    // Device residency guard for GPU
                    if (chosen == execution_kind::gpu &&
                        layout.space == address_space::unknown) {
                        plan.guards.push_back({.kind = guard_kind::device_resident});
                        plan.guards.push_back({.kind = guard_kind::device_available});
                    }

                    // Build versioned plan: fast = this plan, fallback = scalar plan
                    execution_plan scalar_fallback;
                    scalar_fallback.region_id = plan.region_id;
                    scalar_fallback.kind = execution_kind::scalar;
                    scalar_fallback.legality = analysis_outcome::proven_legal;
                    scalar_fallback.cost = estimate(execution_kind::scalar,
                                                    msummary, layout, target, {}, lview);
                    plans.push_back(std::move(scalar_fallback));

                    plan.fallback = execution_plan_id{
                        static_cast<std::uint32_t>(plans.size() - 1)
                    };

                    // Emit runtime_versioned note
                    lithe::diag::diagnostic d;
                    d.level = lithe::diag::severity::note;
                    d.stage = lithe::diag::stage::backend;
                    d.code = "runtime_versioned"; // LITHE-EXEC-034
                    d.message = "Region cannot be statically proven legal; "
                        "emitting runtime-versioned plan with scalar fallback";
                    sink.on_diagnostic(d);
                }

                plans.push_back(std::move(plan));
            }

            return plans;
        }
    };
} // namespace lithe::exec
