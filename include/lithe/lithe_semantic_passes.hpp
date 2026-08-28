#pragma once
// Internal fragment — include via lithe_semantic.hpp (umbrella).
// Depends on: lithe_semantic.hpp preamble + lithe_semantic_inference.hpp
// (semantic_registry, semantic_context, semantic_node, semantic_resolution,
//  semantic_info, structural_hash_t, types::type_id, effect_type, domain_type,
//  capability, has_domain, purity, evaluation_strategy).

namespace lithe { namespace semantic {
        // A single semantic rewrite rule: maps a predicate over semantic_info to
        // a rewritten semantic_info.  Rules must be pure and must not mutate MIR.
        struct semantic_rewrite_rule {
            std::string name;
            std::function<bool(const semantic_info &)> matches;
            std::function<semantic_info(semantic_info)> apply;
        };

        // Records one application of a rewrite rule for diagnostic purposes.
        struct semantic_rewrite_trace {
            std::string rule_name;
            structural_hash_t node_id = 0;
            std::optional<std::string> source_span;
            std::optional<types::type_id> type_before;
            std::optional<types::type_id> type_after;
            bool changed = false;
        };

        // Aggregates all rewrites applied during a single canonicalization pass.
        struct semantic_optimization_report {
            std::vector<semantic_rewrite_trace> traces;
            std::size_t rewritten_nodes = 0;
            std::size_t visited_nodes = 0;

            [[nodiscard]] bool any_rewritten() const { return rewritten_nodes > 0; }

            void record(semantic_rewrite_trace t) {
                if (t.changed) ++rewritten_nodes;
                ++visited_nodes;
                traces.push_back(std::move(t));
            }
        };

        // A canonicalization pass that applies a sequence of rewrite rules to a
        // semantic_registry entry.  Operates purely on semantic IR; never touches MIR.
        struct semantic_canonicalization_pass {
            std::vector<semantic_rewrite_rule> rules;

            [[nodiscard]] std::pair<semantic_info, semantic_rewrite_trace>
            apply_to(semantic_info info,
                     structural_hash_t node_id,
                     std::optional<std::string> source_span = std::nullopt) const {
                semantic_rewrite_trace trace;
                trace.node_id = node_id;
                trace.source_span = std::move(source_span);
                trace.changed = false;

                for (const auto& rule : rules) {
                    if (rule.matches && rule.apply && rule.matches(info)) {
                        semantic_info rewritten = rule.apply(info);
                        if (rewritten.effect != info.effect ||
                            rewritten.domain != info.domain ||
                            rewritten.capabilities.bits != info.capabilities.bits) {
                            trace.rule_name = rule.name;
                            trace.changed = true;
                            info = std::move(rewritten);
                        }
                    }
                }

                return {std::move(info), std::move(trace)};
            }
        };

        // Pipeline of canonicalization passes run in order before MIR lowering.
        class semantic_optimization_pipeline {
        public:
            void add_pass(semantic_canonicalization_pass pass) {
                passes_.push_back(std::move(pass));
            }

            [[nodiscard]] bool empty() const { return passes_.empty(); }
            [[nodiscard]] std::size_t size() const { return passes_.size(); }

            [[nodiscard]] semantic_optimization_report
            run(semantic_registry& reg,
                const std::vector<structural_hash_t>& node_ids) const {
#if LITHE_HAS_PROFILER
                profiler::ScopedProfiler _prof{"lithe.semantic.opt_pipeline.run"};
#endif
                semantic_optimization_report report;

                for (structural_hash_t id : node_ids) {
                    auto info_opt = reg.get(id);
                    if (!info_opt.has_value()) continue;

                    semantic_info info = *info_opt;
                    for (const auto& pass : passes_) {
                        auto [rewritten, trace] = pass.apply_to(info, id);
                        report.record(std::move(trace));
                        info = std::move(rewritten);
                    }
                    reg.merge(id, info, semantic_resolution{});
                }

                return report;
            }

            [[nodiscard]] semantic_optimization_report
            run(semantic_context& ctx,
                const std::vector<structural_hash_t>& node_ids) const {
#if LITHE_HAS_PROFILER
                profiler::ScopedProfiler _prof{"lithe.semantic.opt_pipeline.run"};
#endif
                semantic_optimization_report report;

                for (structural_hash_t id : node_ids) {
                    semantic_node node{id};
                    auto info_opt = ctx.get(node);
                    if (!info_opt.has_value()) continue;

                    semantic_info info = *info_opt;
                    for (const auto& pass : passes_) {
                        auto [rewritten, trace] = pass.apply_to(info, id);
                        report.record(std::move(trace));
                        info = std::move(rewritten);
                    }
                    ctx.merge(node, info);
                }

                return report;
            }

        private:
            std::vector<semantic_canonicalization_pass> passes_;
        };

        // Built-in canonicalization rules
        namespace canon_rules {
            inline semantic_rewrite_rule constant_folding_eligible() {
                return {
                    "constant_folding_eligible",
                    [](const semantic_info& i) {
                        return i.effect == effect_type::pure &&
                            has_domain(i.domain, domain_type::arithmetic) &&
                            i.capabilities.has(capability::memoizable);
                    },
                    [](semantic_info i) {
                        i.purity_level = purity::pure;
                        i.evaluation = evaluation_strategy::eager;
                        return i;
                    }
                };
            }

            inline semantic_rewrite_rule coercion_elimination() {
                return {
                    "coercion_elimination",
                    [](const semantic_info& i) {
                        return i.effect == effect_type::pure &&
                            has_domain(i.domain, domain_type::symbolic) &&
                            has_domain(i.domain, domain_type::arithmetic);
                    },
                    [](semantic_info i) {
                        const auto sym_bits =
                            static_cast<std::uint16_t>(domain_type::symbolic);
                        i.domain = static_cast<domain_type>(
                            static_cast<std::uint16_t>(i.domain) & ~sym_bits);
                        return i;
                    }
                };
            }

            inline semantic_rewrite_rule shape_simplification() {
                return {
                    "shape_simplification",
                    [](const semantic_info& i) {
                        return has_domain(i.domain, domain_type::tensor);
                    },
                    [](semantic_info i) {
                        i.domain = domain_type::arithmetic;
                        return i;
                    }
                };
            }

            inline semantic_rewrite_rule effect_simplification() {
                return {
                    "effect_simplification",
                    [](const semantic_info& i) {
                        return i.effect == effect_type::read_only &&
                            i.purity_level == purity::pure;
                    },
                    [](semantic_info i) {
                        i.effect = effect_type::pure;
                        return i;
                    }
                };
            }

            inline semantic_rewrite_rule operation_canonicalization() {
                return {
                    "operation_canonicalization",
                    [](const semantic_info& i) {
                        return i.effect == effect_type::pure &&
                            !i.capabilities.has(capability::common_subexpression_safe);
                    },
                    [](semantic_info i) {
                        i.capabilities.add(capability::common_subexpression_safe);
                        return i;
                    }
                };
            }
        } // namespace canon_rules

        [[nodiscard]] inline semantic_canonicalization_pass
        make_default_canonicalization_pass() {
            semantic_canonicalization_pass pass;
            pass.rules.push_back(canon_rules::constant_folding_eligible());
            pass.rules.push_back(canon_rules::coercion_elimination());
            pass.rules.push_back(canon_rules::shape_simplification());
            pass.rules.push_back(canon_rules::effect_simplification());
            pass.rules.push_back(canon_rules::operation_canonicalization());
            return pass;
        }

        [[nodiscard]] inline semantic_optimization_pipeline
        make_default_semantic_pipeline() {
            semantic_optimization_pipeline pipeline;
            pipeline.add_pass(make_default_canonicalization_pass());
            return pipeline;
        }
    } // namespace semantic
} // namespace lithe
