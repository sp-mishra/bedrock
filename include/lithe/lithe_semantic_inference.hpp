#pragma once
// Internal fragment — include via lithe_semantic.hpp (umbrella).
// Depends on: lithe_semantic.hpp preamble (lithe::detail namespace etc.).
#include "rules/easy_rules.hpp"

namespace lithe { namespace semantic {
        enum class effect_type : std::uint8_t {
            unknown,
            pure,
            read_only,
            writes_local,
            writes_global,
            io_operation,
            throws,
            terminates
        };

        enum class domain_type : std::uint16_t {
            unknown,
            arithmetic = 1u << 0,
            symbolic = 1u << 1,
            query = 1u << 2,
            task = 1u << 3,
            layout = 1u << 4,
            tensor = 1u << 5,
            custom = 1u << 6
        };

        constexpr domain_type operator|(domain_type a, domain_type b) {
            return static_cast<domain_type>(
                static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b)
            );
        }

        constexpr domain_type& operator|=(domain_type& a, const domain_type b) {
            a = a | b;
            return a;
        }

        constexpr bool has_domain(domain_type composed, domain_type probe) {
            return (static_cast<std::uint16_t>(composed) & static_cast<std::uint16_t>(probe)) != 0u;
        }

        enum class capability : std::uint32_t {
            none = 0u,
            reorderable = 1u << 0,
            common_subexpression_safe = 1u << 1,
            vectorizable = 1u << 2,
            parallelizable = 1u << 3,
            memoizable = 1u << 4,
            requires_guard = 1u << 5
        };

        constexpr capability operator|(capability a, capability b) {
            return static_cast<capability>(
                static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)
            );
        }

        constexpr capability& operator|=(capability& a, const capability b) {
            a = a | b;
            return a;
        }

        constexpr bool has_capability(capability flags, capability flag) {
            return (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(flag)) != 0u;
        }

        struct capability_set {
            std::uint32_t bits = 0u;

            constexpr capability_set() = default;

            constexpr capability_set(capability cap)
                : bits(static_cast<std::uint32_t>(cap)) {}

            [[nodiscard]] constexpr bool has(capability cap) const {
                return (bits & static_cast<std::uint32_t>(cap)) != 0u;
            }

            constexpr void add(capability cap) {
                bits |= static_cast<std::uint32_t>(cap);
            }

            constexpr void merge(const capability_set& other) {
                bits |= other.bits;
            }

            [[nodiscard]] constexpr bool empty() const {
                return bits == 0u;
            }

            [[nodiscard]] static constexpr capability_set from(const capability cap) {
                return capability_set{cap};
            }
        };

        constexpr capability_set operator|(const capability_set lhs, const capability_set rhs) {
            capability_set merged = lhs;
            merged.merge(rhs);
            return merged;
        }


        constexpr capability_set operator|(capability_set lhs, const capability rhs) {
            lhs.add(rhs);
            return lhs;
        }

        constexpr capability_set operator|(const capability lhs, capability_set rhs) {
            rhs.add(lhs);
            return rhs;
        }

        enum class ownership_semantics : std::uint8_t {
            unknown,
            value,
            borrowed,
            shared,
            unique,
            transferred
        };

        enum class purity : std::uint8_t {
            unknown,
            pure,
            deterministic,
            impure
        };

        enum class mutability : std::uint8_t {
            unknown,
            immutable,
            shallow_mutable,
            deep_mutable
        };

        enum class allocation_behavior : std::uint8_t {
            unknown,
            none,
            stack,
            heap,
            pooled,
            external
        };

        enum class synchronization_behavior : std::uint8_t {
            unknown,
            none,
            thread_confined,
            lock_free,
            lock_based,
            blocking
        };

        enum class evaluation_strategy : std::uint8_t {
            unknown,
            eager,
            lazy,
            short_circuit,
            memoized,
            deferred
        };

        enum class semantic_conflict : std::uint8_t {
            none,
            effect,
            domain,
            ownership,
            purity,
            mutability,
            allocation,
            synchronization,
            evaluation
        };

        enum class primitive_type : std::uint8_t {
            unknown,
            boolean,
            signed_integer,
            unsigned_integer,
            floating_point,
            string,
            symbolic,
            custom
        };

        struct numeric_type_info {
            bool is_signed = true;
            bool is_integer = true;
            std::uint16_t bit_width = 0;
        };

        struct tensor_type_info {
            std::size_t rank = 0;
            std::vector<std::size_t> static_shape;
            primitive_type element_type = primitive_type::unknown;
        };

        struct nullable_type_info {
            bool nullable = true;
            std::optional<primitive_type> wrapped_primitive;
        };

        struct custom_type_info {
            std::string name;
            std::vector<std::string> qualifiers;
        };

        enum class type_relation : std::uint8_t {
            unknown,
            identical,
            assignable,
            promotable,
            convertible,
            incompatible
        };

        // Host-side type descriptor: rich optional-field model used by
        // relate_types / check_types / host_type_descriptor<T>().
        // The canonical IR-level descriptor lives in semantic::types::type_descriptor.
        struct host_type_descriptor {
            primitive_type primitive = primitive_type::unknown;
            std::optional<numeric_type_info> numeric;
            std::optional<tensor_type_info> tensor;
            std::optional<nullable_type_info> nullable;
            std::optional<custom_type_info> custom;
            bool host_backed = true;
            bool is_const = false;
            bool is_reference = false;
            std::string debug_name;

            [[nodiscard]] bool is_numeric() const {
                return primitive == primitive_type::signed_integer ||
                    primitive == primitive_type::unsigned_integer ||
                    primitive == primitive_type::floating_point;
            }

            [[nodiscard]] bool is_tensor_like() const {
                return tensor.has_value();
            }

            [[nodiscard]] structural_hash_t stable_key() const {
                std::size_t seed = std::hash<int>{}(static_cast<int>(primitive));
                seed = emit::hash_combine(seed, std::hash<bool>{}(host_backed));
                seed = emit::hash_combine(seed, std::hash<bool>{}(is_const));
                seed = emit::hash_combine(seed, std::hash<bool>{}(is_reference));
                if (numeric.has_value()) {
                    seed = emit::hash_combine(seed, std::hash<bool>{}(numeric->is_signed));
                    seed = emit::hash_combine(seed, std::hash<bool>{}(numeric->is_integer));
                    seed = emit::hash_combine(seed, std::hash<std::uint16_t>{}(numeric->bit_width));
                }
                if (tensor.has_value()) {
                    seed = emit::hash_combine(seed, std::hash<std::size_t>{}(tensor->rank));
                    seed = emit::hash_combine(seed, std::hash<int>{}(static_cast<int>(tensor->element_type)));
                    for (auto dim : tensor->static_shape) {
                        seed = emit::hash_combine(seed, std::hash<std::size_t>{}(dim));
                    }
                }
                if (nullable.has_value()) {
                    seed = emit::hash_combine(seed, std::hash<bool>{}(nullable->nullable));
                    if (nullable->wrapped_primitive.has_value()) {
                        seed = emit::hash_combine(
                            seed, std::hash<int>{}(static_cast<int>(*nullable->wrapped_primitive)));
                    }
                }
                if (custom.has_value()) {
                    seed = emit::hash_combine(seed, std::hash<std::string>{}(custom->name));
                    for (const auto& q : custom->qualifiers) {
                        seed = emit::hash_combine(seed, std::hash<std::string>{}(q));
                    }
                }
                if (!debug_name.empty()) {
                    seed = emit::hash_combine(seed, std::hash<std::string>{}(debug_name));
                }
                return seed;
            }
        };

        // Backward-compatible alias so all existing call sites keep working
        // without rename churn.  New code should use host_type_descriptor directly.
        using type_descriptor = host_type_descriptor;

        struct type_check_result {
            bool ok = false;
            type_relation relation = type_relation::unknown;
            type_descriptor expected;
            type_descriptor actual;
            std::string message;
        };

        struct semantic_info;

        [[nodiscard]] inline type_relation relate_types(const type_descriptor& lhs, const type_descriptor& rhs) {
            if (lhs.stable_key() == rhs.stable_key()) {
                return type_relation::identical;
            }
            if (lhs.primitive == primitive_type::unknown || rhs.primitive == primitive_type::unknown) {
                return type_relation::unknown;
            }
            if (lhs.is_numeric() && rhs.is_numeric()) {
                if (lhs.numeric.has_value() && rhs.numeric.has_value()) {
                    if (lhs.numeric->is_integer && rhs.numeric->is_integer &&
                        lhs.numeric->bit_width >= rhs.numeric->bit_width) {
                        return type_relation::promotable;
                    }
                    if (!lhs.numeric->is_integer || !rhs.numeric->is_integer) {
                        return type_relation::convertible;
                    }
                }
                return type_relation::assignable;
            }
            if (lhs.is_tensor_like() && rhs.is_tensor_like()) {
                return lhs.tensor->rank == rhs.tensor->rank ? type_relation::assignable : type_relation::incompatible;
            }
            if (lhs.primitive == rhs.primitive) {
                return type_relation::assignable;
            }
            return type_relation::incompatible;
        }

        [[nodiscard]] inline type_check_result check_types(const type_descriptor& expected,
                                                           const type_descriptor& actual) {
            const auto relation = relate_types(expected, actual);
            return type_check_result{
                relation == type_relation::identical ||
                relation == type_relation::assignable ||
                relation == type_relation::promotable ||
                relation == type_relation::convertible,
                relation,
                expected,
                actual,
                relation == type_relation::incompatible ? "type mismatch" : "ok"
            };
        }

        template <class T>
        [[nodiscard]] type_descriptor host_type_descriptor() {
            using D = std::decay_t<T>;
            type_descriptor desc;
            desc.host_backed = true;
            if constexpr (std::same_as<D, bool>) {
                desc.primitive = primitive_type::boolean;
                desc.debug_name = "bool";
            }
            else if constexpr (std::integral<D>&& std::signed_integral<D>) {
                desc.primitive = primitive_type::signed_integer;
                desc.numeric = numeric_type_info{true, true, static_cast<std::uint16_t>(sizeof(D) * 8)};
                desc.debug_name = "signed-int";
            }
            else if constexpr (std::integral<D>) {
                desc.primitive = primitive_type::unsigned_integer;
                desc.numeric = numeric_type_info{false, true, static_cast<std::uint16_t>(sizeof(D) * 8)};
                desc.debug_name = "unsigned-int";
            }
            else if constexpr (std::floating_point<D>) {
                desc.primitive = primitive_type::floating_point;
                desc.numeric = numeric_type_info{true, false, static_cast<std::uint16_t>(sizeof(D) * 8)};
                desc.debug_name = "floating";
            }
            else if constexpr (std::same_as<D, std::string> || std::same_as<D, std::string_view>) {
                desc.primitive = primitive_type::string;
                desc.debug_name = "string";
            }
            else {
                desc.primitive = primitive_type::custom;
                desc.custom = custom_type_info{typeid(D).name(), {"host"}};
                desc.debug_name = "custom";
            }
            return desc;
        }

        [[nodiscard]] type_descriptor infer_type_descriptor(const semantic_info& info);


        struct propagation_rule {
            bool inherit_effect = true;
            bool inherit_domain = true;
            bool inherit_capabilities = true;
            bool inherit_constraints = true;
            bool inherit_ownership = true;
            bool inherit_purity = true;
            bool inherit_mutability = true;
            bool inherit_allocation = true;
            bool inherit_synchronization = true;
            bool inherit_evaluation = true;
        };

        struct semantic_resolution {
            bool normalize = true;
            bool keep_user_annotations = true;
            bool prefer_overlay_on_conflict = true;
        };

        enum class semantic_merge_strategy : std::uint8_t {
            conservative,
            prefer_specific,
            accumulate
        };

        struct semantic_conflict_entry {
            semantic_conflict kind = semantic_conflict::none;
            std::string detail;
        };


        struct constraint {
            std::string name;
            bool satisfied = true;
            std::string description;
        };

        struct contract {
            std::vector<constraint> preconditions;
            std::vector<constraint> postconditions;
            effect_type max_effect = effect_type::unknown;
        };

        struct effect_summary {
            effect_type strongest = effect_type::unknown;
            bool is_pure = false;
            bool reads_state = false;
            bool writes_state = false;
            bool has_io = false;
            bool may_throw = false;
            bool may_terminate = false;
        };

        struct constraint_result {
            bool all_satisfied = true;
            std::vector<constraint> unsatisfied;
        };

        enum class semantic_key : std::uint8_t {
            domain,
            effect,
            capabilities,
            capability_requirement,
            ownership,
            purity,
            mutability,
            allocation,
            synchronization,
            evaluation,
            constraint,
            constraints,
            contract,
            effect_summary,
            custom
        };

        using semantic_value = std::variant<
            std::monostate,
            bool,
            std::int64_t,
            double,
            std::string,
            effect_type,
            domain_type,
            capability,
            capability_set,
            ownership_semantics,
            purity,
            mutability,
            allocation_behavior,
            synchronization_behavior,
            evaluation_strategy,
            constraint,
            std::vector<constraint>,
            contract,
            effect_summary
        >;

        using annotation_map = ::lithe::detail::flat_map<semantic_key, semantic_value>;

        struct annotation {
            semantic_key key;
            semantic_value value;
        };

        struct semantic_annotation {
            std::optional<effect_type> effect;
            std::optional<domain_type> domain;
            capability_set capabilities;
            std::optional<ownership_semantics> ownership;
            std::optional<purity> purity_level;
            std::optional<mutability> mutability_kind;
            std::optional<allocation_behavior> allocation;
            std::optional<synchronization_behavior> synchronization;
            std::optional<evaluation_strategy> evaluation;
            std::vector<constraint> constraints;
            std::optional<contract> semantic_contract;
            annotation_map extras;
        };

        struct semantic_info {
            effect_type effect = effect_type::unknown;
            domain_type domain = domain_type::unknown;
            capability_set capabilities;
            ownership_semantics ownership = ownership_semantics::unknown;
            purity purity_level = purity::unknown;
            mutability mutability_kind = mutability::unknown;
            allocation_behavior allocation = allocation_behavior::unknown;
            synchronization_behavior synchronization = synchronization_behavior::unknown;
            evaluation_strategy evaluation = evaluation_strategy::unknown;
            std::vector<constraint> constraints;
            std::optional<contract> semantic_contract;
            annotation_map extras;
            bool has_user_annotation = false;

            [[nodiscard]] effect_summary summarize_effect() const {
                effect_summary summary;
                summary.strongest = effect;
                summary.is_pure = (effect == effect_type::pure);
                summary.reads_state =
                    effect == effect_type::read_only ||
                    effect == effect_type::writes_local ||
                    effect == effect_type::writes_global ||
                    effect == effect_type::io_operation;
                summary.writes_state =
                    effect == effect_type::writes_local ||
                    effect == effect_type::writes_global ||
                    effect == effect_type::io_operation;
                summary.has_io = effect == effect_type::io_operation;
                summary.may_throw = effect == effect_type::throws;
                summary.may_terminate = effect == effect_type::terminates;
                return summary;
            }

            [[nodiscard]] constraint_result check_constraints() const {
                constraint_result result;
                for (const auto& c : constraints) {
                    if (!c.satisfied) {
                        result.all_satisfied = false;
                        result.unsatisfied.push_back(c);
                    }
                }
                return result;
            }

            [[nodiscard]] static constexpr effect_type merge_effect(effect_type lhs, effect_type rhs) {
                return static_cast<int>(lhs) >= static_cast<int>(rhs) ? lhs : rhs;
            }

            [[nodiscard]] static constexpr domain_type merge_domain(const domain_type lhs, const domain_type rhs) {
                if (lhs == domain_type::unknown) return rhs;
                if (rhs == domain_type::unknown) return lhs;
                return lhs | rhs;
            }

            template <class T>
            [[nodiscard]] static constexpr T merge_prefer_known(T lhs, T rhs, bool prefer_rhs) {
                const T unknown = T::unknown;
                if (lhs == unknown) return rhs;
                if (rhs == unknown) return lhs;
                return prefer_rhs ? rhs : lhs;
            }

            void apply_annotation(const annotation& entry) {
                std::visit(
                    [&](const auto& v) {
                        using V = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<V, std::monostate>) {
                            return;
                        }
                        else if constexpr (std::is_same_v<V, effect_type>) {
                            if (entry.key == semantic_key::effect) {
                                effect = v;
                            }
                        }
                        else if constexpr (std::is_same_v<V, domain_type>) {
                            if (entry.key == semantic_key::domain) {
                                domain = v;
                            }
                        }
                        else if constexpr (std::is_same_v<V, ownership_semantics>) {
                            if (entry.key == semantic_key::ownership) {
                                ownership = v;
                            }
                        }
                        else if constexpr (std::is_same_v<V, purity>) {
                            if (entry.key == semantic_key::purity) {
                                purity_level = v;
                            }
                        }
                        else if constexpr (std::is_same_v<V, mutability>) {
                            if (entry.key == semantic_key::mutability) {
                                mutability_kind = v;
                            }
                        }
                        else if constexpr (std::is_same_v<V, allocation_behavior>) {
                            if (entry.key == semantic_key::allocation) {
                                allocation = v;
                            }
                        }
                        else if constexpr (std::is_same_v<V, synchronization_behavior>) {
                            if (entry.key == semantic_key::synchronization) {
                                synchronization = v;
                            }
                        }
                        else if constexpr (std::is_same_v<V, evaluation_strategy>) {
                            if (entry.key == semantic_key::evaluation) {
                                evaluation = v;
                            }
                        }
                        else if constexpr (std::is_same_v<V, capability_set>) {
                            if (entry.key == semantic_key::capabilities ||
                                entry.key == semantic_key::capability_requirement) {
                                capabilities.merge(v);
                            }
                        }
                        else if constexpr (std::is_same_v<V, capability>) {
                            if (entry.key == semantic_key::capabilities ||
                                entry.key == semantic_key::capability_requirement) {
                                capabilities.add(v);
                            }
                        }
                        else if constexpr (std::is_same_v<V, constraint>) {
                            if (entry.key == semantic_key::constraint) {
                                constraints.push_back(v);
                            }
                        }
                        else if constexpr (std::is_same_v<V, std::vector<constraint>>) {
                            if (entry.key == semantic_key::constraints) {
                                constraints.insert(constraints.end(), v.begin(), v.end());
                            }
                        }
                        else if constexpr (std::is_same_v<V, contract>) {
                            if (entry.key == semantic_key::contract) {
                                semantic_contract = v;
                            }
                        }
                    },
                    entry.value
                );
                extras[entry.key] = entry.value;
                has_user_annotation = true;
            }

            void apply_annotation_map(const annotation_map& map) {
                for (const auto& [key, value] : map) {
                    apply_annotation(annotation{key, value});
                }
            }

            void merge_annotation(const semantic_annotation& annotation) {
                if (annotation.effect.has_value()) {
                    effect = *annotation.effect;
                }
                if (annotation.domain.has_value()) {
                    domain = *annotation.domain;
                }
                capabilities.merge(annotation.capabilities);
                if (annotation.ownership.has_value()) {
                    ownership = *annotation.ownership;
                }
                if (annotation.purity_level.has_value()) {
                    purity_level = *annotation.purity_level;
                }
                if (annotation.mutability_kind.has_value()) {
                    mutability_kind = *annotation.mutability_kind;
                }
                if (annotation.allocation.has_value()) {
                    allocation = *annotation.allocation;
                }
                if (annotation.synchronization.has_value()) {
                    synchronization = *annotation.synchronization;
                }
                if (annotation.evaluation.has_value()) {
                    evaluation = *annotation.evaluation;
                }
                constraints.insert(constraints.end(), annotation.constraints.begin(), annotation.constraints.end());
                if (annotation.semantic_contract.has_value()) {
                    semantic_contract = annotation.semantic_contract;
                }
                apply_annotation_map(annotation.extras);
                has_user_annotation = true;
            }

            void normalize() {
                if (purity_level == purity::unknown) {
                    if (effect == effect_type::pure) {
                        purity_level = purity::pure;
                    }
                    else if (effect == effect_type::read_only) {
                        purity_level = purity::deterministic;
                    }
                    else if (effect != effect_type::unknown) {
                        purity_level = purity::impure;
                    }
                }

                if (effect == effect_type::pure) {
                    if (mutability_kind == mutability::unknown) {
                        mutability_kind = mutability::immutable;
                    }
                    if (allocation == allocation_behavior::unknown) {
                        allocation = allocation_behavior::none;
                    }
                    if (synchronization == synchronization_behavior::unknown) {
                        synchronization = synchronization_behavior::none;
                    }
                }
            }

            [[nodiscard]] std::vector<semantic_conflict_entry> detect_conflicts(const semantic_info& other) const {
                std::vector<semantic_conflict_entry> conflicts;
                auto push_conflict = [&](const semantic_conflict kind, const char* detail) {
                    conflicts.push_back(semantic_conflict_entry{kind, detail});
                };

                if (effect != effect_type::unknown && other.effect != effect_type::unknown && effect != other.effect) {
                    push_conflict(semantic_conflict::effect, "effect mismatch");
                }
                if (domain != domain_type::unknown && other.domain != domain_type::unknown &&
                    !has_domain(domain, other.domain) && !has_domain(other.domain, domain)) {
                    push_conflict(semantic_conflict::domain, "domain mismatch");
                }
                if (ownership != ownership_semantics::unknown &&
                    other.ownership != ownership_semantics::unknown && ownership != other.ownership) {
                    push_conflict(semantic_conflict::ownership, "ownership mismatch");
                }
                if (purity_level != purity::unknown && other.purity_level != purity::unknown &&
                    purity_level != other.purity_level) {
                    push_conflict(semantic_conflict::purity, "purity mismatch");
                }
                if (mutability_kind != mutability::unknown && other.mutability_kind != mutability::unknown &&
                    mutability_kind != other.mutability_kind) {
                    push_conflict(semantic_conflict::mutability, "mutability mismatch");
                }
                if (allocation != allocation_behavior::unknown && other.allocation != allocation_behavior::unknown &&
                    allocation != other.allocation) {
                    push_conflict(semantic_conflict::allocation, "allocation mismatch");
                }
                if (synchronization != synchronization_behavior::unknown &&
                    other.synchronization != synchronization_behavior::unknown &&
                    synchronization != other.synchronization) {
                    push_conflict(semantic_conflict::synchronization, "synchronization mismatch");
                }
                if (evaluation != evaluation_strategy::unknown && other.evaluation != evaluation_strategy::unknown &&
                    evaluation != other.evaluation) {
                    push_conflict(semantic_conflict::evaluation, "evaluation strategy mismatch");
                }
                return conflicts;
            }

            void inherit_from_child(const semantic_info& child, const propagation_rule& rule = {}) {
                if (rule.inherit_effect) {
                    effect = merge_effect(effect, child.effect);
                }
                if (rule.inherit_domain) {
                    domain = merge_domain(domain, child.domain);
                }
                if (rule.inherit_capabilities) {
                    capabilities.merge(child.capabilities);
                }
                if (rule.inherit_constraints) {
                    constraints.insert(constraints.end(), child.constraints.begin(), child.constraints.end());
                }
                if (rule.inherit_ownership) {
                    ownership = merge_prefer_known(ownership, child.ownership, false);
                }
                if (rule.inherit_purity) {
                    purity_level = merge_prefer_known(purity_level, child.purity_level, false);
                }
                if (rule.inherit_mutability) {
                    mutability_kind = merge_prefer_known(mutability_kind, child.mutability_kind, true);
                }
                if (rule.inherit_allocation) {
                    allocation = merge_prefer_known(allocation, child.allocation, false);
                }
                if (rule.inherit_synchronization) {
                    synchronization = merge_prefer_known(synchronization, child.synchronization, true);
                }
                if (rule.inherit_evaluation) {
                    evaluation = merge_prefer_known(evaluation, child.evaluation, false);
                }
            }

            void merge_overlay(const semantic_info& overlay, const semantic_resolution resolution = {}) {
                if (overlay.effect != effect_type::unknown) {
                    effect = resolution.prefer_overlay_on_conflict
                                 ? overlay.effect
                                 : merge_effect(effect, overlay.effect);
                }
                if (overlay.domain != domain_type::unknown) {
                    domain = merge_domain(domain, overlay.domain);
                }
                capabilities.merge(overlay.capabilities);
                ownership = merge_prefer_known(ownership, overlay.ownership, resolution.prefer_overlay_on_conflict);
                purity_level = merge_prefer_known(purity_level, overlay.purity_level,
                                                  resolution.prefer_overlay_on_conflict);
                mutability_kind = merge_prefer_known(
                    mutability_kind,
                    overlay.mutability_kind,
                    resolution.prefer_overlay_on_conflict
                );
                allocation = merge_prefer_known(allocation, overlay.allocation, resolution.prefer_overlay_on_conflict);
                synchronization = merge_prefer_known(
                    synchronization,
                    overlay.synchronization,
                    resolution.prefer_overlay_on_conflict
                );
                evaluation = merge_prefer_known(evaluation, overlay.evaluation, resolution.prefer_overlay_on_conflict);
                constraints.insert(constraints.end(), overlay.constraints.begin(), overlay.constraints.end());
                if (overlay.semantic_contract.has_value()) {
                    semantic_contract = overlay.semantic_contract;
                }
                for (const auto& [key, value] : overlay.extras) {
                    extras.insert_or_assign(key, value);
                }
                has_user_annotation = resolution.keep_user_annotations &&
                    (has_user_annotation || overlay.has_user_annotation);
                if (resolution.normalize) {
                    normalize();
                }
            }
        };

        [[nodiscard]] inline type_descriptor infer_type_descriptor(const semantic_info& info) {
            type_descriptor desc;
            desc.host_backed = false;

            if (has_domain(info.domain, domain_type::tensor)) {
                desc.primitive = primitive_type::custom;
                desc.tensor = tensor_type_info{0, {}, primitive_type::floating_point};
                desc.debug_name = "tensor";
            }
            else if (has_domain(info.domain, domain_type::arithmetic)) {
                desc.primitive = primitive_type::floating_point;
                desc.numeric = numeric_type_info{true, false, 64};
                desc.debug_name = "semantic-arithmetic";
            }
            else if (has_domain(info.domain, domain_type::symbolic)) {
                desc.primitive = primitive_type::symbolic;
                desc.debug_name = "semantic-symbolic";
            }
            else if (has_domain(info.domain, domain_type::query) || has_domain(info.domain, domain_type::task)) {
                desc.primitive = primitive_type::custom;
                desc.custom = custom_type_info{"semantic-domain", {"query-task"}};
                desc.debug_name = "semantic-custom";
            }
            else {
                desc.primitive = primitive_type::unknown;
                desc.debug_name = "unknown";
            }

            if (info.mutability_kind == mutability::immutable) {
                desc.is_const = true;
            }

            if (info.effect == effect_type::read_only || info.effect == effect_type::unknown) {
                desc.nullable = nullable_type_info{false, desc.primitive};
            }

            return desc;
        }

        struct semantic_validation {
            bool valid = true;
            semantic_info inferred;
            std::vector<semantic_conflict_entry> conflicts;
            std::vector<constraint> unsatisfied_constraints;
        };

        class semantic_registry {
        public:
            constexpr void annotate(const structural_hash_t key, const annotation& entry) {
                entries_[key].apply_annotation(entry);
            }

            constexpr void annotate(const structural_hash_t key, const semantic_annotation& annotation) {
                entries_[key].merge_annotation(annotation);
            }

            constexpr void annotate(const structural_hash_t key, const annotation_map& annotations) {
                entries_[key].apply_annotation_map(annotations);
            }

            constexpr void merge(const structural_hash_t key, const semantic_info& overlay,
                                 const semantic_resolution resolution = {}) {
                entries_[key].merge_overlay(overlay, resolution);
            }

            [[nodiscard]] constexpr std::optional<semantic_info> get(const structural_hash_t key) const {
                if (auto it = entries_.find(key); it != entries_.end()) {
                    return it->second;
                }
                return std::nullopt;
            }

            [[nodiscard]] constexpr semantic_info get_or_default(const structural_hash_t key) const {
                if (auto it = entries_.find(key); it != entries_.end()) {
                    return it->second;
                }
                return {};
            }

        private:
            ::lithe::detail::flat_map<structural_hash_t, semantic_info> entries_;
        };

        struct backend_capability {
            std::string backend_name;
            ::lithe::detail::flat_set<std::string> operations;
            std::vector<domain_type> domains;
            std::vector<effect_type> effects;
            std::vector<type_descriptor> types;

            [[nodiscard]] bool supports_operation(const std::string_view operation) const {
                return operations.contains(operation);
            }

            [[nodiscard]] bool supports_domain(const domain_type domain) const {
                return std::ranges::any_of(domains, [&](const domain_type supported) {
                    return supported == domain || has_domain(domain, supported) || has_domain(supported, domain);
                });
            }

            [[nodiscard]] bool supports_effect(const effect_type effect) const {
                return std::ranges::find(effects, effect) != effects.end();
            }

            [[nodiscard]] bool supports_type(const type_descriptor& type) const {
                const auto key = type.stable_key();
                return std::ranges::any_of(types, [&](const type_descriptor& candidate) {
                    return candidate.stable_key() == key;
                });
            }
        };

        struct backend_profile {
            std::string backend_name;
            bool require_declared_operations = true;
            bool require_declared_domains = true;
            bool require_declared_effects = true;
            bool require_declared_types = true;

            bool allow_mutable_tensor_ops = true;
            bool allow_filesystem_effects = true;
            bool allow_symbolic_only_nodes = true;
            bool allow_high_level_query_ops = true;

            ::lithe::detail::flat_set<std::string> denied_operations;
            std::vector<domain_type> denied_domains;
            std::vector<effect_type> denied_effects;
            std::vector<primitive_type> denied_primitives;
        };

        struct lowering_legality_result {
            bool legal = true;
            std::string backend_name;
            std::vector<std::string> reasons;

            void reject(std::string reason) {
                legal = false;
                reasons.push_back(std::move(reason));
            }

            void merge(const lowering_legality_result& other) {
                legal = legal && other.legal;
                if (backend_name.empty()) {
                    backend_name = other.backend_name;
                }
                reasons.insert(reasons.end(), other.reasons.begin(), other.reasons.end());
            }
        };

        class capability_registry {
        public:
            void register_backend(backend_capability capability) {
                capabilities_.insert_or_assign(capability.backend_name, std::move(capability));
            }

            [[nodiscard]] std::optional<backend_capability> get_backend(const std::string_view backend_name) const {
                const auto it = capabilities_.find(std::string{backend_name});
                if (it == capabilities_.end()) {
                    return std::nullopt;
                }
                return it->second;
            }

            [[nodiscard]] bool supports_operation(const std::string_view backend_name,
                                                  const std::string_view operation) const {
                if (const auto backend = get_backend(backend_name); backend.has_value()) {
                    return backend->supports_operation(operation);
                }
                return false;
            }

            [[nodiscard]] bool supports_domain(const std::string_view backend_name, const domain_type domain) const {
                if (const auto backend = get_backend(backend_name); backend.has_value()) {
                    return backend->supports_domain(domain);
                }
                return false;
            }

            [[nodiscard]] bool supports_effect(const std::string_view backend_name, const effect_type effect) const {
                if (const auto backend = get_backend(backend_name); backend.has_value()) {
                    return backend->supports_effect(effect);
                }
                return false;
            }

            [[nodiscard]] bool supports_type(const std::string_view backend_name, const type_descriptor& type) const {
                if (const auto backend = get_backend(backend_name); backend.has_value()) {
                    return backend->supports_type(type);
                }
                return false;
            }

            [[nodiscard]] std::vector<std::string> registered_backends() const {
                std::vector<std::string> names;
                names.reserve(capabilities_.size());
                for (const auto& [name, capability] : capabilities_) {
                    (void)capability;
                    names.push_back(name);
                }
                std::ranges::sort(names);
                return names;
            }

            [[nodiscard]] lowering_legality_result validate_lowering(
                std::string_view backend_name,
                std::string_view operation,
                const semantic_info& semantic,
                const type_descriptor& type,
                backend_profile profile = {}
            ) const {
                lowering_legality_result result;
                result.backend_name = std::string{backend_name};
                if (profile.backend_name.empty()) {
                    profile.backend_name = std::string{backend_name};
                }

                const auto backend = get_backend(backend_name);
                if (!backend.has_value()) {
                    result.reject("backend capability profile not registered: " + std::string{backend_name});
                    return result;
                }

                if (!cap_engine_built_) build_cap_engine_();

                // Populate facts from backend capability + semantic + type + profile.
                easy_rules::ExecutionContext ctx;
                ctx.facts.set("backend_name", std::string{backend_name});
                ctx.facts.set("op_name", std::string{operation});
                ctx.facts.set("op_supported", backend->supports_operation(operation));
                ctx.facts.set("domain_known", semantic.domain != domain_type::unknown);
                ctx.facts.set("domain_supported", backend->supports_domain(semantic.domain));
                ctx.facts.set("effect_known", semantic.effect != effect_type::unknown);
                ctx.facts.set("effect_supported", backend->supports_effect(semantic.effect));
                ctx.facts.set("type_known", type.primitive != primitive_type::unknown);
                ctx.facts.set("type_supported", backend->supports_type(type));
                ctx.facts.set("require_declared_ops", profile.require_declared_operations);
                ctx.facts.set("require_declared_domains", profile.require_declared_domains);
                ctx.facts.set("require_declared_effects", profile.require_declared_effects);
                ctx.facts.set("require_declared_types", profile.require_declared_types);
                ctx.facts.set("allow_mutable_tensor", profile.allow_mutable_tensor_ops);
                ctx.facts.set("allow_filesystem_effects", profile.allow_filesystem_effects);
                ctx.facts.set("allow_symbolic_only", profile.allow_symbolic_only_nodes);
                ctx.facts.set("allow_high_level_query", profile.allow_high_level_query_ops);
                ctx.facts.set("is_tensor", has_domain(semantic.domain, domain_type::tensor));
                ctx.facts.set("is_mutable", semantic.mutability_kind != mutability::immutable &&
                              semantic.mutability_kind != mutability::unknown);
                ctx.facts.set("is_io_effect", semantic.effect == effect_type::io_operation);
                ctx.facts.set("is_symbolic", has_domain(semantic.domain, domain_type::symbolic));
                ctx.facts.set("is_arithmetic", has_domain(semantic.domain, domain_type::arithmetic));
                ctx.facts.set("is_query_domain", has_domain(semantic.domain, domain_type::query));
                ctx.facts.set("op_denied", profile.denied_operations.contains(operation));
                ctx.facts.set("reject", false);

                cap_engine_.run(ctx);

                if (ctx.facts.get_or("reject", false)) {
                    result.reject(ctx.facts.get_or<std::string>("reject_msg", "backend validation failed"));
                }

                // Deny-list loops require per-call data — handle after rule engine.
                for (const auto denied_domain : profile.denied_domains) {
                    if (has_domain(semantic.domain, denied_domain)) {
                        result.reject("semantic domain denied by backend profile");
                        break;
                    }
                }
                for (const auto denied_effect : profile.denied_effects) {
                    if (semantic.effect == denied_effect) {
                        result.reject("effect denied by backend profile");
                        break;
                    }
                }
                for (const auto denied_primitive : profile.denied_primitives) {
                    if (type.primitive == denied_primitive) {
                        result.reject("primitive type denied by backend profile");
                        break;
                    }
                }

                return result;
            }

            // Access the capability-check audit trail.
            [[nodiscard]] const easy_rules::AuditListener& cap_audit() const {
                return cap_audit_;
            }

            [[nodiscard]] lowering_legality_result validate_lowering(
                const std::string_view backend_name,
                const semantic_info& semantic,
                const type_descriptor& type,
                backend_profile profile = {}
            ) const {
                return validate_lowering(backend_name, "<unknown-op>", semantic, type, std::move(profile));
            }

        private:
            ::lithe::detail::flat_map<std::string, backend_capability> capabilities_;
            mutable easy_rules::EasyRuleEngine cap_engine_;
            mutable easy_rules::AuditListener cap_audit_;
            mutable bool cap_engine_built_ = false;

            void build_cap_engine_() const {
                using easy_rules::dsl::fact;
                namespace dsl = easy_rules::dsl;

                cap_engine_.add_listener(cap_audit_);
                cap_engine_.config.stop_on_first_match = false;

                // op not declared by backend
                cap_engine_.when("op_not_declared",
                                 dsl::operator&&(fact<bool>("require_declared_ops") == true,
                                                 fact<bool>("op_supported") == false))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("unsupported operation for backend '") +
                                             ctx.facts.get_or<std::string>("backend_name", "?") +
                                             "': " +
                                             ctx.facts.get_or<std::string>("op_name", "?"));
                           }).with_priority(10).with_description("reject undeclared operation");

                // domain not declared by backend
                cap_engine_.when("domain_not_declared",
                                 dsl::operator&&(fact<bool>("require_declared_domains") == true,
                                                 dsl::operator&&(fact<bool>("domain_known") == true,
                                                                 fact<bool>("domain_supported") == false)))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("unsupported semantic domain for backend '") +
                                             ctx.facts.get_or<std::string>("backend_name", "?") + "'");
                           }).with_priority(20).with_description("reject undeclared domain");

                // effect not declared by backend
                cap_engine_.when("effect_not_declared",
                                 dsl::operator&&(fact<bool>("require_declared_effects") == true,
                                                 dsl::operator&&(fact<bool>("effect_known") == true,
                                                                 fact<bool>("effect_supported") == false)))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("unsupported effect model for backend '") +
                                             ctx.facts.get_or<std::string>("backend_name", "?") + "'");
                           }).with_priority(30).with_description("reject undeclared effect");

                // type not declared by backend
                cap_engine_.when("type_not_declared",
                                 dsl::operator&&(fact<bool>("require_declared_types") == true,
                                                 dsl::operator&&(fact<bool>("type_known") == true,
                                                                 fact<bool>("type_supported") == false)))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("unsupported type descriptor for backend '") +
                                             ctx.facts.get_or<std::string>("backend_name", "?") + "'");
                           }).with_priority(40).with_description("reject undeclared type");

                // mutable tensor denied
                cap_engine_.when("mutable_tensor_denied",
                                 dsl::operator&&(fact<bool>("allow_mutable_tensor") == false,
                                                 dsl::operator&&(fact<bool>("is_tensor") == true,
                                                                 fact<bool>("is_mutable") == true)))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("mutable tensor operation rejected by backend profile"));
                           }).with_priority(50).with_description("reject mutable tensor op");

                // io effect denied
                cap_engine_.when("io_effect_denied",
                                 dsl::operator&&(fact<bool>("allow_filesystem_effects") == false,
                                                 fact<bool>("is_io_effect") == true))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("filesystem/io effect rejected by backend profile"));
                           }).with_priority(60).with_description("reject io effect");

                // symbolic-only node denied
                cap_engine_.when("symbolic_only_denied",
                                 dsl::operator&&(fact<bool>("allow_symbolic_only") == false,
                                                 dsl::operator&&(fact<bool>("is_symbolic") == true,
                                                                 fact<bool>("is_arithmetic") == false)))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("symbolic-only node rejected by backend profile"));
                           }).with_priority(70).with_description("reject symbolic-only node");

                // high-level query op denied
                cap_engine_.when("query_op_denied",
                                 dsl::operator&&(fact<bool>("allow_high_level_query") == false,
                                                 fact<bool>("is_query_domain") == true))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("high-level query operator rejected by backend profile"));
                           }).with_priority(80).with_description("reject high-level query op");

                // op on deny-list
                cap_engine_.when("op_denied",
                                 fact<bool>("op_denied") == true)
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("reject", true);
                               ctx.facts.set("reject_msg",
                                             std::string("operation denied by backend profile: ") +
                                             ctx.facts.get_or<std::string>("op_name", "?"));
                           }).with_priority(90).with_description("reject deny-listed operation");

                cap_engine_built_ = true;
            }
        };

        using backend_capability_registry = capability_registry;

        inline semantic_registry& registry() {
            static semantic_registry instance;
            return instance;
        }

        struct semantic_node {
            structural_hash_t structural_id = 0;

            [[nodiscard]] static semantic_node from_key(const structural_hash_t key) {
                return semantic_node{key};
            }

            template <class Expr>
            [[nodiscard]] static semantic_node from_expr(const Expr& expr) {
                return semantic_node{lithe::structural_key(expr)};
            }
        };

        class semantic_context {
        public:
            semantic_context() = default;

            [[nodiscard]] semantic_registry& store() { return registry_; }
            [[nodiscard]] const semantic_registry& store() const { return registry_; }

            [[nodiscard]] propagation_rule rule() const { return propagation_rule_; }
            void set_rule(const propagation_rule rule) { propagation_rule_ = rule; }

            [[nodiscard]] semantic_resolution resolution() const { return resolution_; }
            void set_resolution(const semantic_resolution resolution) { resolution_ = resolution; }

            void annotate(const semantic_node node, const semantic_annotation& annotation) {
                registry_.annotate(node.structural_id, annotation);
            }

            void annotate(const semantic_node node, const annotation& entry) {
                registry_.annotate(node.structural_id, entry);
            }

            void annotate(const semantic_node node, const annotation_map& annotations) {
                registry_.annotate(node.structural_id, annotations);
            }

            void merge(const semantic_node node, const semantic_info& overlay) {
                registry_.merge(node.structural_id, overlay, resolution_);
            }

            [[nodiscard]] std::optional<semantic_info> get(const semantic_node node) const {
                return registry_.get(node.structural_id);
            }

            [[nodiscard]] semantic_info get_or_default(const semantic_node node) const {
                return registry_.get_or_default(node.structural_id);
            }

        private:
            semantic_registry registry_;
            propagation_rule propagation_rule_{};
            semantic_resolution resolution_{};
        };

        class semantic_query {
        public:
            explicit semantic_query(const semantic_registry& store = registry())
                : store_(&store) {}

            [[nodiscard]] explicit semantic_query(const semantic_context& context)
                : store_(&context.store()) {}

            [[nodiscard]] std::optional<semantic_info> get(const semantic_node node) const {
                return store_->get(node.structural_id);
            }

            template <class Expr>
            [[nodiscard]] std::optional<semantic_info> get(const Expr& expr) const {
                return get(semantic_node::from_expr(expr));
            }

            template <class Expr>
            [[nodiscard]] bool has_effect(const Expr& expr, effect_type expected) const {
                if (auto info = get(expr); info.has_value()) {
                    return info->effect == expected;
                }
                return false;
            }

            template <class Expr>
            [[nodiscard]] bool has_capability(const Expr& expr, capability cap) const {
                if (auto info = get(expr); info.has_value()) {
                    return info->capabilities.has(cap);
                }
                return false;
            }

            template <class Expr>
            [[nodiscard]] domain_type domain_of(const Expr& expr) const {
                if (auto info = get(expr); info.has_value()) {
                    return info->domain;
                }
                return domain_type::unknown;
            }

        private:
            const semantic_registry* store_;
        };

        namespace detail {
            template <class T>
            concept has_semantic_contract_member = requires(std::remove_cvref_t<T> t) {
                t.semantic_contract;
            };

            template <class T>
            concept has_semantic_info_member = requires(std::remove_cvref_t<T> t) {
                t.semantic_info;
            };

            constexpr int severity(effect_type effect) {
                return static_cast<int>(effect);
            }

            constexpr effect_type merge_effect(const effect_type lhs, const effect_type rhs) {
                return severity(lhs) >= severity(rhs) ? lhs : rhs;
            }

            constexpr domain_type merge_domain(const domain_type lhs, const domain_type rhs) {
                if (lhs == domain_type::unknown) return rhs;
                if (rhs == domain_type::unknown) return lhs;
                return lhs | rhs;
            }

            template <class Tag>
            constexpr effect_type classify_effect() {
                if constexpr (std::is_same_v<Tag, add_tag> || std::is_same_v<Tag, sub_tag> ||
                    std::is_same_v<Tag, mul_tag> || std::is_same_v<Tag, neg_tag> ||
                    std::is_same_v<Tag, shl_tag> || std::is_same_v<Tag, shr_tag>) {
                    return effect_type::pure;
                }
                else if constexpr (std::is_same_v<Tag, div_tag> || std::is_same_v<Tag, mod_tag>) {
                    return effect_type::throws;
                }
                else if constexpr (std::is_same_v<Tag, call_tag>) {
                    return effect_type::writes_global;
                }
                else {
                    return effect_type::read_only;
                }
            }

            template <class Tag>
            constexpr domain_type classify_domain() {
                if constexpr (std::is_same_v<Tag, add_tag> || std::is_same_v<Tag, sub_tag> ||
                    std::is_same_v<Tag, mul_tag> || std::is_same_v<Tag, div_tag> ||
                    std::is_same_v<Tag, mod_tag> || std::is_same_v<Tag, neg_tag>) {
                    return domain_type::arithmetic;
                }
                else if constexpr (std::is_same_v<Tag, and_tag> || std::is_same_v<Tag, or_tag> ||
                    std::is_same_v<Tag, not_tag>) {
                    return domain_type::symbolic;
                }
                else if constexpr (std::is_same_v<Tag, bit_and_tag> || std::is_same_v<Tag, bit_or_tag> ||
                    std::is_same_v<Tag, bit_xor_tag> || std::is_same_v<Tag, bit_not_tag> ||
                    std::is_same_v<Tag, shl_tag> || std::is_same_v<Tag, shr_tag>) {
                    return domain_type::arithmetic | domain_type::symbolic;
                }
                else if constexpr (std::is_same_v<Tag, if_tag> || std::is_same_v<Tag, while_tag> ||
                    std::is_same_v<Tag, for_tag> || std::is_same_v<Tag, seq_tag>) {
                    return domain_type::task;
                }
                else if constexpr (std::is_same_v<Tag, call_tag>) {
                    return domain_type::query | domain_type::custom;
                }
                else {
                    return domain_type::unknown;
                }
            }

            template <class Tag>
            constexpr capability_set classify_capability() {
                if constexpr (std::is_same_v<Tag, add_tag> || std::is_same_v<Tag, sub_tag> ||
                    std::is_same_v<Tag, mul_tag> || std::is_same_v<Tag, neg_tag>) {
                    return capability::reorderable | capability::common_subexpression_safe |
                        capability::vectorizable | capability::parallelizable | capability::memoizable;
                }
                else if constexpr (std::is_same_v<Tag, div_tag> || std::is_same_v<Tag, mod_tag>) {
                    return capability::requires_guard | capability::memoizable;
                }
                else {
                    return capability_set{};
                }
            }

            template <class Tag>
            constexpr ownership_semantics classify_ownership() {
                if constexpr (std::is_same_v<Tag, call_tag>) {
                    return ownership_semantics::shared;
                }
                else {
                    return ownership_semantics::value;
                }
            }

            template <class Tag>
            constexpr mutability classify_mutability() {
                if constexpr (std::is_same_v<Tag, call_tag> || std::is_same_v<Tag, while_tag> ||
                    std::is_same_v<Tag, for_tag>) {
                    return mutability::deep_mutable;
                }
                else {
                    return mutability::immutable;
                }
            }

            template <class Tag>
            constexpr allocation_behavior classify_allocation() {
                if constexpr (std::is_same_v<Tag, call_tag>) {
                    return allocation_behavior::external;
                }
                else {
                    return allocation_behavior::none;
                }
            }

            template <class Tag>
            constexpr synchronization_behavior classify_synchronization() {
                if constexpr (std::is_same_v<Tag, call_tag>) {
                    return synchronization_behavior::lock_based;
                }
                else {
                    return synchronization_behavior::none;
                }
            }

            template <class Tag>
            constexpr evaluation_strategy classify_evaluation() {
                if constexpr (std::is_same_v<Tag, and_tag> || std::is_same_v<Tag, or_tag>) {
                    return evaluation_strategy::short_circuit;
                }
                else {
                    return evaluation_strategy::eager;
                }
            }

            constexpr domain_type merge_domain_with_strategy(
                const domain_type lhs,
                const domain_type rhs,
                const semantic_merge_strategy strategy
            ) {
                if (lhs == domain_type::unknown) return rhs;
                if (rhs == domain_type::unknown) return lhs;
                if (lhs == rhs) return lhs;

                // Keep tensor semantics dominant when mixed with arithmetic/symbolic children.
                if (has_domain(lhs, domain_type::tensor) || has_domain(rhs, domain_type::tensor)) {
                    return domain_type::tensor;
                }

                if (strategy == semantic_merge_strategy::conservative) {
                    return domain_type::unknown;
                }
                if (strategy == semantic_merge_strategy::prefer_specific) {
                    if (lhs == domain_type::arithmetic) return rhs;
                    if (rhs == domain_type::arithmetic) return lhs;
                }
                return lhs | rhs;
            }

            constexpr mutability merge_mutability(const mutability lhs, const mutability rhs) {
                if (lhs == mutability::deep_mutable || rhs == mutability::deep_mutable) {
                    return mutability::deep_mutable;
                }
                if (lhs == mutability::shallow_mutable || rhs == mutability::shallow_mutable) {
                    return mutability::shallow_mutable;
                }
                if (lhs == mutability::unknown) return rhs;
                if (rhs == mutability::unknown) return lhs;
                return lhs;
            }

            struct default_semantic_rules {
                template <class T>
                static semantic_info terminal(T&&) {
                    semantic_info info;
                    using terminal_t = std::decay_t<T>;
                    if constexpr (std::is_arithmetic_v<terminal_t>) {
                        info.effect = effect_type::pure;
                        info.domain = domain_type::arithmetic;
                        info.capabilities = capability::reorderable |
                            capability::common_subexpression_safe |
                            capability::memoizable;
                        info.ownership = ownership_semantics::value;
                        info.purity_level = purity::pure;
                        info.mutability_kind = mutability::immutable;
                        info.allocation = allocation_behavior::none;
                        info.synchronization = synchronization_behavior::none;
                        info.evaluation = evaluation_strategy::eager;
                    }
                    else {
                        info.effect = effect_type::read_only;
                        info.domain = domain_type::unknown;
                        info.ownership = ownership_semantics::borrowed;
                        info.purity_level = purity::deterministic;
                        info.mutability_kind = mutability::unknown;
                        info.allocation = allocation_behavior::unknown;
                        info.synchronization = synchronization_behavior::unknown;
                        info.evaluation = evaluation_strategy::unknown;
                    }
                    if constexpr (has_semantic_contract_member<terminal_t> ||
                        has_semantic_info_member<terminal_t>) {
                        info.capabilities.add(capability::requires_guard);
                    }
                    return info;
                }

                template <class Tag>
                static semantic_info node_base() {
                    semantic_info info;
                    info.effect = classify_effect<Tag>();
                    info.domain = classify_domain<Tag>();
                    info.capabilities = classify_capability<Tag>();
                    info.ownership = classify_ownership<Tag>();
                    info.mutability_kind = classify_mutability<Tag>();
                    info.allocation = classify_allocation<Tag>();
                    info.synchronization = classify_synchronization<Tag>();
                    info.evaluation = classify_evaluation<Tag>();
                    return info;
                }

                static void merge_child(
                    semantic_info& parent,
                    const semantic_info& child,
                    const propagation_rule& rule,
                    const semantic_merge_strategy strategy
                ) {
                    if (rule.inherit_effect) {
                        parent.effect = merge_effect(parent.effect, child.effect);
                    }
                    if (rule.inherit_domain) {
                        parent.domain = merge_domain_with_strategy(parent.domain, child.domain, strategy);
                    }
                    if (rule.inherit_capabilities) {
                        parent.capabilities.merge(child.capabilities);
                    }
                    if (rule.inherit_constraints) {
                        parent.constraints.insert(parent.constraints.end(), child.constraints.begin(),
                                                  child.constraints.end());
                    }
                    if (rule.inherit_ownership) {
                        parent.ownership = semantic_info::merge_prefer_known(parent.ownership, child.ownership, false);
                    }
                    if (rule.inherit_purity) {
                        parent.purity_level = semantic_info::merge_prefer_known(
                            parent.purity_level, child.purity_level, false);
                    }
                    if (rule.inherit_mutability) {
                        parent.mutability_kind = merge_mutability(parent.mutability_kind, child.mutability_kind);
                    }
                    if (rule.inherit_allocation) {
                        parent.allocation = semantic_info::merge_prefer_known(
                            parent.allocation, child.allocation, false);
                    }
                    if (rule.inherit_synchronization) {
                        parent.synchronization = semantic_info::merge_prefer_known(
                            parent.synchronization,
                            child.synchronization,
                            true
                        );
                    }
                    if (rule.inherit_evaluation) {
                        parent.evaluation = semantic_info::merge_prefer_known(
                            parent.evaluation, child.evaluation, false);
                    }
                }

                template <class Tag>
                static void finalize(semantic_info& info) {
                    if constexpr (std::is_same_v<Tag, div_tag> || std::is_same_v<Tag, mod_tag>) {
                        info.constraints.push_back(constraint{
                            "non_zero_divisor",
                            false,
                            "division/modulo requires a non-zero divisor"
                        });
                    }
                }
            };

            struct semantic_analyzer {
                template <class T>
                semantic_info on_terminal(T&& t) const {
                    semantic_info info = default_semantic_rules::terminal(std::forward<T>(t));
                    return info;
                }

                template <class Tag, class... ChildInfo>
                semantic_info on_node(Tag, ChildInfo&&... children) const {
                    semantic_info info = default_semantic_rules::node_base<Tag>();
                    (default_semantic_rules::merge_child(
                        info,
                        children,
                        propagation_rule{},
                        semantic_merge_strategy::accumulate
                    ), ...);
                    default_semantic_rules::finalize<Tag>(info);

                    info.normalize();

                    return info;
                }
            };
        } // namespace detail

        template <class Rules = detail::default_semantic_rules>
        class semantic_propagator {
        public:
            explicit semantic_propagator(
                const semantic_registry* store = nullptr,
                const propagation_rule propagation = {},
                const semantic_resolution resolution = {},
                const semantic_merge_strategy strategy = semantic_merge_strategy::prefer_specific
            )
                : store_(store),
                  propagation_(propagation),
                  resolution_(resolution),
                  strategy_(strategy) {}

            template <class T>
            semantic_info on_terminal(T&& terminal) const {
                semantic_info info = make_terminal(std::forward<T>(terminal));
                overlay_existing(terminal, info);
                finalize(info);
                return info;
            }

            template <class Tag, class... ChildInfo>
            semantic_info on_node(Tag, ChildInfo&&... children) const {
                semantic_info info = make_node_base<Tag>();
                (merge_child(info, children), ...);
                finalize_tag<Tag>(info);
                finalize(info);
                return info;
            }

        private:
            template <class T>
            [[nodiscard]] semantic_info make_terminal(T&& terminal) const {
                if constexpr (requires { Rules::terminal(std::forward<T>(terminal)); }) {
                    return Rules::terminal(std::forward<T>(terminal));
                }
                else {
                    return detail::default_semantic_rules::terminal(std::forward<T>(terminal));
                }
            }

            template <class Tag>
            [[nodiscard]] semantic_info make_node_base() const {
                if constexpr (requires { Rules::template node_base<Tag>(); }) {
                    return Rules::template node_base<Tag>();
                }
                else {
                    return detail::default_semantic_rules::template node_base<Tag>();
                }
            }

            void merge_child(semantic_info& parent, const semantic_info& child) const {
                if constexpr (requires {
                    Rules::merge_child(parent, child, propagation_, strategy_);
                }) {
                    Rules::merge_child(parent, child, propagation_, strategy_);
                }
                else {
                    detail::default_semantic_rules::merge_child(parent, child, propagation_, strategy_);
                }
            }

            template <class Tag>
            void finalize_tag(semantic_info& info) const {
                if constexpr (requires { Rules::template finalize<Tag>(info); }) {
                    Rules::template finalize<Tag>(info);
                }
                else {
                    detail::default_semantic_rules::template finalize<Tag>(info);
                }
            }

            template <class ExprLike>
            void overlay_existing(const ExprLike& expr, semantic_info& info) const {
                if (store_ == nullptr) {
                    return;
                }
                if (auto existing = store_->get(lithe::structural_key(expr)); existing.has_value()) {
                    info.merge_overlay(*existing, resolution_);
                }
            }

            void finalize(semantic_info& info) const {
                if (resolution_.normalize) {
                    info.normalize();
                }
            }

            const semantic_registry* store_;
            propagation_rule propagation_;
            semantic_resolution resolution_;
            semantic_merge_strategy strategy_;
        };

        template <class Expr>
        void annotate(const Expr& expr, const semantic_annotation& annotation) {
            registry().annotate(lithe::structural_key(expr), annotation);
        }

        inline void annotate(const semantic_node node, const semantic_annotation& annotation) {
            registry().annotate(node.structural_id, annotation);
        }

        template <class Expr>
        void annotate(semantic_context& context, const Expr& expr, const semantic_annotation& annotation) {
            context.annotate(semantic_node::from_expr(expr), annotation);
        }

        template <class Expr>
        void annotate(const Expr& expr, const annotation& entry) {
            registry().annotate(lithe::structural_key(expr), entry);
        }

        inline void annotate(const semantic_node node, const annotation& entry) {
            registry().annotate(node.structural_id, entry);
        }

        template <class Expr>
        void annotate(semantic_context& context, const Expr& expr, const annotation& entry) {
            context.annotate(semantic_node::from_expr(expr), entry);
        }

        template <class Expr>
        void annotate(const Expr& expr, const annotation_map& annotations) {
            registry().annotate(lithe::structural_key(expr), annotations);
        }

        inline void annotate(const semantic_node node, const annotation_map& annotations) {
            registry().annotate(node.structural_id, annotations);
        }

        template <class Expr>
        void annotate(semantic_context& context, const Expr& expr, const annotation_map& annotations) {
            context.annotate(semantic_node::from_expr(expr), annotations);
        }

        template <class Expr>
        Expr&& with_domain(Expr&& expr, domain_type domain) {
            annotate(expr, annotation{semantic_key::domain, domain});
            return std::forward<Expr>(expr);
        }

        template <class Expr>
        Expr&& with_effect(Expr&& expr, effect_type effect) {
            annotate(expr, annotation{semantic_key::effect, effect});
            return std::forward<Expr>(expr);
        }

        template <class Expr>
        Expr&& requires_capability(Expr&& expr, capability cap) {
            annotate(expr, annotation{semantic_key::capability_requirement, cap});
            return std::forward<Expr>(expr);
        }

        template <class Expr>
        [[nodiscard]] std::optional<semantic_info> get_semantics(const Expr& expr) {
            return registry().get(lithe::structural_key(expr));
        }

        [[nodiscard]] inline std::optional<semantic_info> get_semantics(const semantic_node node) {
            return registry().get(node.structural_id);
        }

        template <class Expr>
        [[nodiscard]] std::optional<semantic_info> get_semantics(const semantic_context& context, const Expr& expr) {
            return context.get(semantic_node::from_expr(expr));
        }

        template <class Expr, class Rules = detail::default_semantic_rules>
        [[nodiscard]] semantic_info infer_semantics(
            const Expr& expr,
            semantic_merge_strategy strategy = semantic_merge_strategy::prefer_specific,
            propagation_rule propagation = {},
            semantic_resolution resolution = {}
        ) {
            semantic_propagator<Rules> propagator{std::addressof(registry()), propagation, resolution, strategy};
            return lithe::visit(expr, propagator);
        }

        template <class Expr, class Rules = detail::default_semantic_rules>
        [[nodiscard]] semantic_info infer_semantics(
            const semantic_context& context,
            const Expr& expr,
            semantic_merge_strategy strategy = semantic_merge_strategy::prefer_specific
        ) {
            semantic_propagator<Rules> propagator{
                std::addressof(context.store()),
                context.rule(),
                context.resolution(),
                strategy
            };
            return lithe::visit(expr, propagator);
        }

        template <class Expr, class Rules = detail::default_semantic_rules>
        [[nodiscard]] type_descriptor infer_type_descriptor(
            const Expr& expr,
            const semantic_merge_strategy strategy = semantic_merge_strategy::prefer_specific,
            const propagation_rule propagation = {},
            const semantic_resolution resolution = {}
        ) {
            const auto info = infer_semantics<Expr, Rules>(expr, strategy, propagation, resolution);
            return infer_type_descriptor(info);
        }

        template <class Expr, class Rules = detail::default_semantic_rules>
        [[nodiscard]] semantic_info propagate_semantics(
            const Expr& expr,
            const semantic_merge_strategy strategy = semantic_merge_strategy::prefer_specific,
            const propagation_rule propagation = {},
            const semantic_resolution resolution = {}
        ) {
            semantic_info inferred = infer_semantics<Expr, Rules>(expr, strategy, propagation, resolution);
            registry().merge(lithe::structural_key(expr), inferred, resolution);
            return registry().get_or_default(lithe::structural_key(expr));
        }

        template <class Expr, class Rules = detail::default_semantic_rules>
        [[nodiscard]] semantic_info propagate_semantics(
            semantic_context& context,
            const Expr& expr,
            const semantic_merge_strategy strategy = semantic_merge_strategy::prefer_specific
        ) {
            semantic_info inferred = infer_semantics<Expr, Rules>(context, expr, strategy);
            context.merge(semantic_node::from_expr(expr), inferred);
            return context.get_or_default(semantic_node::from_expr(expr));
        }

        template <class Expr, class Rules = detail::default_semantic_rules>
        [[nodiscard]] semantic_validation validate_semantics(
            const Expr& expr,
            const semantic_merge_strategy strategy = semantic_merge_strategy::prefer_specific,
            const propagation_rule propagation = {},
            const semantic_resolution resolution = {}
        ) {
            semantic_validation validation;
            validation.inferred = infer_semantics<Expr, Rules>(expr, strategy, propagation, resolution);
            if (auto existing = get_semantics(expr); existing.has_value()) {
                validation.conflicts = validation.inferred.detect_conflicts(*existing);
            }
            const auto checked = validation.inferred.check_constraints();
            validation.unsatisfied_constraints = checked.unsatisfied;
            validation.valid = validation.conflicts.empty() && checked.all_satisfied;
            return validation;
        }

        template <class Expr, class Rules = detail::default_semantic_rules>
        [[nodiscard]] semantic_validation validate_semantics(
            const semantic_context& context,
            const Expr& expr,
            const semantic_merge_strategy strategy = semantic_merge_strategy::prefer_specific
        ) {
            semantic_validation validation;
            validation.inferred = infer_semantics<Expr, Rules>(context, expr, strategy);
            if (auto existing = get_semantics(context, expr); existing.has_value()) {
                validation.conflicts = validation.inferred.detect_conflicts(*existing);
            }
            const auto checked = validation.inferred.check_constraints();
            validation.unsatisfied_constraints = checked.unsatisfied;
            validation.valid = validation.conflicts.empty() && checked.all_satisfied;
            return validation;
        }

        template <class Expr>
        [[nodiscard]] semantic_info analyze_semantics(const Expr& expr) {
            semantic_info analyzed = infer_semantics(expr, semantic_merge_strategy::accumulate);
            if (auto existing = registry().get(lithe::structural_key(expr)); existing.has_value()) {
                analyzed.merge_overlay(*existing);
            }
            analyzed.normalize();
            return analyzed;
        }

        template <class Expr>
        [[nodiscard]] semantic_info analyze_semantics(const semantic_context& context, const Expr& expr) {
            semantic_info analyzed = infer_semantics(context, expr, semantic_merge_strategy::accumulate);
            if (auto existing = context.get(semantic_node::from_expr(expr)); existing.has_value()) {
                analyzed.merge_overlay(*existing, context.resolution());
            }
            analyzed.normalize();
            return analyzed;
        }

        [[nodiscard]] inline semantic_info merge_semantics(const semantic_info& a, const semantic_info& b) {
            semantic_info merged = a;
            merged.merge_overlay(b);
            return merged;
        }

        [[nodiscard]] inline std::vector<semantic_conflict_entry>
        detect_semantic_conflicts(const semantic_info& a, const semantic_info& b) {
            return a.detect_conflicts(b);
        }

        [[nodiscard]] inline semantic_info normalize_semantics(semantic_info info) {
            info.normalize();
            return info;
        }

        [[nodiscard]] inline semantic_info propagate_semantics(
            semantic_info parent,
            const semantic_info& child,
            const propagation_rule& rule = {}
        ) {
            parent.inherit_from_child(child, rule);
            parent.normalize();
            return parent;
        }

        [[nodiscard]] inline semantic_info merge(const semantic_info& a, const semantic_info& b) {
            return merge_semantics(a, b);
        }

        template <class Expr>
        [[nodiscard]] bool has_effect(const Expr& expr, effect_type expected) {
            return semantic_query{}.has_effect(expr, expected);
        }

        template <class Expr>
        [[nodiscard]] bool has_effect(const semantic_context& context, const Expr& expr, effect_type expected) {
            return semantic_query{context}.has_effect(expr, expected);
        }

        template <class Expr>
        [[nodiscard]] bool has_capability(const Expr& expr, capability cap) {
            return semantic_query{}.has_capability(expr, cap);
        }

        template <class Expr>
        [[nodiscard]] bool has_capability(const semantic_context& context, const Expr& expr, capability cap) {
            return semantic_query{context}.has_capability(expr, cap);
        }

        template <class Expr>
        [[nodiscard]] domain_type domain_of(const Expr& expr) {
            return semantic_query{}.domain_of(expr);
        }

        template <class Expr>
        [[nodiscard]] domain_type domain_of(const semantic_context& context, const Expr& expr) {
            return semantic_query{context}.domain_of(expr);
        }

        [[nodiscard]] inline bool is_pure(const semantic_info& info) {
            return info.summarize_effect().is_pure;
        }

        [[nodiscard]] inline bool is_no_throw(const semantic_info& info) {
            const auto summary = info.summarize_effect();
            return !summary.may_throw && !summary.may_terminate;
        }

        [[nodiscard]] inline bool is_safe_to_reorder(const semantic_info& a, const semantic_info& b) {
            const auto a_summary = a.summarize_effect();
            const auto b_summary = b.summarize_effect();

            if (a_summary.writes_state || b_summary.writes_state) {
                return false;
            }
            if (a_summary.has_io || b_summary.has_io || a_summary.may_throw || b_summary.may_throw ||
                a_summary.may_terminate || b_summary.may_terminate) {
                return false;
            }

            const bool capability_ok =
                a.capabilities.has(capability::reorderable) &&
                b.capabilities.has(capability::reorderable);
            return capability_ok;
        }

        [[nodiscard]] inline bool is_safe_to_cse(const semantic_info& info) {
            const auto summary = info.summarize_effect();
            const auto constraints = info.check_constraints();
            if (!constraints.all_satisfied) {
                return false;
            }
            if (summary.writes_state || summary.has_io || summary.may_throw || summary.may_terminate) {
                return false;
            }
            return info.capabilities.has(capability::common_subexpression_safe) ||
                (summary.is_pure && info.capabilities.has(capability::memoizable));
        }

        // -------------------------------------------------------------------------
        // Type Framework (Prompt 3 + 4): Lithe-owned semantic type authority
        // -------------------------------------------------------------------------
        namespace types {
            using type_id = std::uint64_t;
            static constexpr type_id invalid_type_id = 0;

            enum class type_kind : std::uint8_t {
                unknown,
                void_type,
                boolean,
                integer,
                floating,
                pointer,
                reference,
                function,
                aggregate,
                object,
                vector,
                tensor,
                symbolic,
                query,
                layout,
                dynamic,
                token,
                type_variable
            };

            enum class type_qualifier : std::uint8_t {
                none = 0,
                const_q = 1u << 0,
                volatile_q = 1u << 1,
                restrict_q = 1u << 2,
                atomic_q = 1u << 3
            };

            constexpr type_qualifier operator|(type_qualifier a, type_qualifier b) {
                return static_cast<type_qualifier>(
                    static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
            }

            constexpr bool has_qualifier(type_qualifier composed, type_qualifier probe) {
                return (static_cast<std::uint8_t>(composed) & static_cast<std::uint8_t>(probe)) != 0u;
            }

            struct type_descriptor {
                type_id id = invalid_type_id;
                type_kind kind = type_kind::unknown;
                std::string name;
                std::uint32_t bit_width = 0;
                std::vector<std::uint32_t> shape;
                std::vector<type_id> parameters;
                // Sorted flat map — avoids heap-map overhead and is safe in
                // compile-time-adjacent paths.
                ::lithe::detail::flat_map<std::string, std::string> attributes;

                [[nodiscard]] bool valid() const { return id != invalid_type_id; }

                [[nodiscard]] bool is_numeric() const {
                    return kind == type_kind::integer || kind == type_kind::floating;
                }

                [[nodiscard]] bool is_aggregate_like() const {
                    return kind == type_kind::aggregate || kind == type_kind::object ||
                        kind == type_kind::vector || kind == type_kind::tensor;
                }
            };

            // -----------------------------------------------------------------
            // semantic_type_registry  (Prompt 4)
            // Header-only, no parser/frontend dependency.
            // -----------------------------------------------------------------
            class semantic_type_registry {
            public:
                // Register a fully-formed descriptor; returns its id.
                type_id register_type(type_descriptor desc) {
                    std::lock_guard lock(mutex_);
                    if (desc.id == invalid_type_id) {
                        desc.id = next_id_++;
                    }
                    const type_id id = desc.id;
                    by_id_[id] = std::move(desc);
                    return id;
                }

                // Look up by id.
                [[nodiscard]] std::optional<type_descriptor> find_type(const type_id id) const {
                    std::lock_guard lock(mutex_);
                    if (auto it = by_id_.find(id); it != by_id_.end()) {
                        return it->second;
                    }
                    return std::nullopt;
                }

                // Look up by name.
                [[nodiscard]] std::optional<type_descriptor> find_type(const std::string_view name) const {
                    std::lock_guard lock(mutex_);
                    for (const auto& [id, desc] : by_id_) {
                        if (desc.name == name) return desc;
                    }
                    return std::nullopt;
                }

                // Return the canonical representative for a structurally equivalent type.
                [[nodiscard]] type_id canonicalize(const type_descriptor& desc) {
                    std::lock_guard lock(mutex_);
                    const auto key = structural_key_(desc);
                    if (auto it = canonical_.find(key); it != canonical_.end()) {
                        return it->second;
                    }
                    type_descriptor copy = desc;
                    if (copy.id == invalid_type_id) {
                        copy.id = next_id_++;
                    }
                    const type_id id = copy.id;
                    by_id_[id] = copy;
                    canonical_[key] = id;
                    return id;
                }

                // True if two type_ids refer to structurally equivalent types.
                [[nodiscard]] bool equivalent(const type_id a, const type_id b) const {
                    std::lock_guard lock(mutex_);
                    if (a == b) return true;
                    auto ia = by_id_.find(a);
                    auto ib = by_id_.find(b);
                    if (ia == by_id_.end() || ib == by_id_.end()) return false;
                    return structural_key_(ia->second) == structural_key_(ib->second);
                }

                // True if `sub` is a subtype of `super` (structural / width-based rules).
                [[nodiscard]] bool subtype_of(const type_id sub, const type_id super) const {
                    std::lock_guard lock(mutex_);
                    if (sub == super) return true;
                    auto is = by_id_.find(sub);
                    auto ip = by_id_.find(super);
                    if (is == by_id_.end() || ip == by_id_.end()) return false;
                    const auto& s = is->second;
                    const auto& p = ip->second;
                    // integer widening: smaller integer is subtype of larger
                    if (s.kind == type_kind::integer && p.kind == type_kind::integer) {
                        return s.bit_width <= p.bit_width;
                    }
                    // float widening
                    if (s.kind == type_kind::floating && p.kind == type_kind::floating) {
                        return s.bit_width <= p.bit_width;
                    }
                    // integer → floating promotion
                    if (s.kind == type_kind::integer && p.kind == type_kind::floating) {
                        return true;
                    }
                    return false;
                }

                // Factory helpers ---------------------------------------------------

                type_id make_integer_type(const std::uint32_t bit_width, const bool is_signed,
                                          std::string name = {}) {
                    type_descriptor d;
                    d.kind = type_kind::integer;
                    d.bit_width = bit_width;
                    d.name = name.empty()
                                 ? (is_signed ? "i" : "u") + std::to_string(bit_width)
                                 : std::move(name);
                    d.attributes["signed"] = is_signed ? "true" : "false";
                    return canonicalize(d);
                }

                type_id make_float_type(const std::uint32_t bit_width, std::string name = {}) {
                    type_descriptor d;
                    d.kind = type_kind::floating;
                    d.bit_width = bit_width;
                    d.name = name.empty() ? "f" + std::to_string(bit_width) : std::move(name);
                    return canonicalize(d);
                }

                type_id make_tensor_type(type_id element_type,
                                         std::vector<std::uint32_t> shape,
                                         std::string name = {}) {
                    type_descriptor d;
                    d.kind = type_kind::tensor;
                    d.shape = std::move(shape);
                    d.parameters = {element_type};
                    d.name = name.empty() ? "tensor<" + std::to_string(element_type) + ">" : std::move(name);
                    return canonicalize(d);
                }

                type_id make_function_type(const type_id return_type,
                                           std::vector<type_id> param_types,
                                           std::string name = {}) {
                    type_descriptor d;
                    d.kind = type_kind::function;
                    d.parameters = std::move(param_types);
                    d.parameters.insert(d.parameters.begin(), return_type);
                    d.name = name.empty() ? "fn" : std::move(name);
                    return canonicalize(d);
                }

                type_id make_dynamic_type(std::string name = "dynamic") {
                    type_descriptor d;
                    d.kind = type_kind::dynamic;
                    d.name = std::move(name);
                    return canonicalize(d);
                }

            private:
                // Stable structural hash for deduplication — no heap allocations.
                static std::uint64_t structural_key_(const type_descriptor& d) {
                    auto mix = [](std::uint64_t h, std::uint64_t v) noexcept -> std::uint64_t {
                        return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
                    };
                    std::uint64_t h = static_cast<std::uint64_t>(d.kind);
                    h = mix(h, static_cast<std::uint64_t>(d.bit_width));
                    for (auto dim : d.shape) h = mix(h, static_cast<std::uint64_t>(dim));
                    for (auto p : d.parameters) h = mix(h, static_cast<std::uint64_t>(p));
                    for (const auto& [ak, av] : d.attributes) {
                        h = mix(h, std::hash<std::string>{}(ak));
                        h = mix(h, std::hash<std::string>{}(av));
                    }
                    return h;
                }

                ::lithe::detail::flat_map<type_id, type_descriptor> by_id_;
                ::lithe::detail::flat_map<std::uint64_t, type_id> canonical_;
                type_id next_id_ = 1;
                mutable std::mutex mutex_;
            };

            inline semantic_type_registry& type_registry() {
                static semantic_type_registry instance;
                return instance;
            }

            // -----------------------------------------------------------------
            // type_variable_id — opaque handle for a type variable (Prompt 1)
            // -----------------------------------------------------------------
            struct type_variable_id {
                std::uint32_t id = std::numeric_limits<std::uint32_t>::max();

                constexpr bool is_valid() const noexcept {
                    return id != std::numeric_limits<std::uint32_t>::max();
                }

                constexpr auto operator<=>(const type_variable_id&) const noexcept = default;
            };

            inline constexpr type_variable_id invalid_type_variable_id{};

            struct type_variable_descriptor {
                std::uint32_t id = std::numeric_limits<std::uint32_t>::max();
                std::string debug_name;
                std::optional<type_id> resolved_type;
                bool rigid = false;
                bool inferred = false;
                ::lithe::detail::flat_set<type_qualifier> qualifiers;
                ::lithe::detail::flat_map<std::string, std::string> attributes;

                [[nodiscard]] bool is_resolved() const noexcept {
                    return resolved_type.has_value();
                }
            };
        } // namespace types

        // =====================================================================
        // Semantic type-rule engine — Prompt 5
        // Integrates EasyRules for type inference, coercion, and diagnostics.
        // Falls back to a minimal built-in engine when the header is absent.
        // =====================================================================
        namespace type_rules {
            // ----------------------------------------------------------------
            // Facts that populate the rule engine's working memory
            // ----------------------------------------------------------------

            // Basic type identification fact.
            struct type_fact {
                types::type_id id = types::invalid_type_id;
                types::type_kind kind = types::type_kind::unknown;
                std::string name;
                std::uint32_t bit_width = 0;
            };

            // Type of a complete expression (keyed by structural hash).
            struct expression_type_fact {
                structural_hash_t expr_key = 0;
                types::type_id type_id = types::invalid_type_id;
                std::string debug_label;
            };

            // Types observed at an operation's inputs and output.
            struct operation_type_fact {
                std::string op_name;
                std::vector<types::type_id> operand_type_ids;
                types::type_id result_type_id = types::invalid_type_id;
                bool result_known = false;
            };

            // A legal implicit coercion between two types.
            struct coercion_fact {
                types::type_id from_type = types::invalid_type_id;
                types::type_id to_type = types::invalid_type_id;
                bool is_lossless = true;
                std::string description;
            };

            // An active constraint on a type.
            struct constraint_fact {
                std::string name;
                types::type_id constrained_type = types::invalid_type_id;
                std::string predicate_description;
                bool satisfied = true;
            };

            // ----------------------------------------------------------------
            // Result / diagnostic types
            // ----------------------------------------------------------------

            enum class type_rule_severity : std::uint8_t {
                ok,
                hint,
                warning,
                error
            };

            struct type_rule_diagnostic {
                type_rule_severity severity = type_rule_severity::ok;
                std::string rule_name;
                std::string message;
                types::type_id involved_type = types::invalid_type_id;
                types::type_id expected_type = types::invalid_type_id;
            };

            struct type_rule_result {
                bool ok = true;
                std::vector<type_rule_diagnostic> diagnostics;
                std::optional<coercion_fact> suggested_coercion;

                void add_error(std::string rule, std::string msg,
                               const types::type_id t = types::invalid_type_id,
                               const types::type_id exp = types::invalid_type_id) {
                    ok = false;
                    diagnostics.push_back({type_rule_severity::error, std::move(rule), std::move(msg), t, exp});
                }

                void add_warning(std::string rule, std::string msg,
                                 const types::type_id t = types::invalid_type_id) {
                    diagnostics.push_back({type_rule_severity::warning, std::move(rule), std::move(msg), t});
                }

                void add_hint(std::string rule, std::string msg,
                              const types::type_id t = types::invalid_type_id) {
                    diagnostics.push_back({type_rule_severity::hint, std::move(rule), std::move(msg), t});
                }
            };

            struct inferred_type_result {
                types::type_id type_id = types::invalid_type_id;
                bool inferred = false;
                std::string reasoning;
                type_rule_result rule_result;
            };


            // ----------------------------------------------------------------
            // semantic_type_rule_engine
            // ----------------------------------------------------------------
            class semantic_type_rule_engine {
            public:
                explicit semantic_type_rule_engine(
                    types::semantic_type_registry* registry = &types::type_registry())
                    : registry_(registry) {
                    engine_.add_listener(audit_);
                    register_builtin_rules_();
                }

                // --- primary API -------------------------------------------

                // Infer the result type of an expression.
                [[nodiscard]] inferred_type_result infer_expression_type(
                    structural_hash_t expr_key,
                    const std::vector<types::type_id>& operand_types,
                    std::string_view op_name = "") {
                    inferred_type_result result;
                    if (operand_types.empty()) {
                        result.reasoning = "no operands — type unknown";
                        return result;
                    }

                    operation_type_fact fact;
                    fact.op_name = std::string{op_name};
                    fact.operand_type_ids = operand_types;
                    type_rule_result rule_result;

                    run_rules_(fact, rule_result);

                    result.rule_result = rule_result;
                    result.inferred = fact.result_known;
                    result.type_id = fact.result_type_id;
                    if (fact.result_known && registry_) {
                        if (auto td = registry_->find_type(fact.result_type_id); td.has_value()) {
                            result.reasoning = "inferred as " + td->name;
                        }
                    }
                    return result;
                }

                // Validate that operand types satisfy an operation's contract.
                [[nodiscard]] type_rule_result check_operation_contract(
                    const std::string_view op_name,
                    const std::vector<types::type_id>& operand_types,
                    const std::optional<types::type_id> expected_result_type = std::nullopt) {
                    type_rule_result result;
                    if (operand_types.empty()) {
                        result.add_error(std::string{op_name}, "operation has no operands");
                        return result;
                    }

                    operation_type_fact fact;
                    fact.op_name = std::string{op_name};
                    fact.operand_type_ids = operand_types;
                    if (expected_result_type.has_value()) {
                        fact.result_type_id = *expected_result_type;
                        fact.result_known = true;
                    }

                    run_rules_(fact, result);

                    if (expected_result_type.has_value() && fact.result_known &&
                        fact.result_type_id != *expected_result_type) {
                        if (registry_ && !registry_->subtype_of(fact.result_type_id, *expected_result_type)) {
                            result.add_error(std::string{op_name},
                                             "result type mismatch: inferred type is not compatible with expected type",
                                             fact.result_type_id, *expected_result_type);
                        }
                    }
                    return result;
                }

                // Check that rhs_type is legally assignable to lhs_type.
                [[nodiscard]] type_rule_result check_assignment(
                    const types::type_id lhs_type,
                    const types::type_id rhs_type) const {
                    type_rule_result result;
                    if (!registry_) return result;

                    if (lhs_type == rhs_type || registry_->equivalent(lhs_type, rhs_type)) {
                        return result;
                    }
                    if (registry_->subtype_of(rhs_type, lhs_type)) {
                        result.add_hint("assignment_check",
                                        "implicit widening applied",
                                        rhs_type);
                        return result;
                    }

                    // Look for a registered coercion.
                    auto coercion = find_coercion(rhs_type, lhs_type);
                    if (coercion.has_value()) {
                        if (!coercion->is_lossless) {
                            result.add_warning("assignment_check",
                                               "narrowing coercion: " + coercion->description,
                                               rhs_type);
                        }
                        result.suggested_coercion = coercion;
                        return result;
                    }

                    result.add_error("assignment_check",
                                     "cannot assign: no valid coercion found",
                                     rhs_type, lhs_type);
                    return result;
                }

                // Find a registered coercion between two types.
                [[nodiscard]] std::optional<coercion_fact> find_coercion(
                    const types::type_id from_type,
                    const types::type_id to_type) const {
                    for (const auto& c : coercions_) {
                        if (c.from_type == from_type && c.to_type == to_type) {
                            return c;
                        }
                    }
                    // Structural / registry-based fallback.
                    if (registry_ && registry_->subtype_of(from_type, to_type)) {
                        return coercion_fact{from_type, to_type, true, "widening"};
                    }
                    return std::nullopt;
                }

                // Produce a human-readable explanation for a type error.
                [[nodiscard]] std::string explain_type_error(
                    const type_rule_result& result) const {
                    if (result.ok) return "no error";
                    std::string msg;
                    for (const auto& diag : result.diagnostics) {
                        if (diag.severity == type_rule_severity::error ||
                            diag.severity == type_rule_severity::warning) {
                            msg += "[" + diag.rule_name + "] " + diag.message;
                            if (registry_) {
                                if (diag.involved_type != types::invalid_type_id) {
                                    if (auto td = registry_->find_type(diag.involved_type))
                                        msg += " (got: " + td->name + ")";
                                }
                                if (diag.expected_type != types::invalid_type_id) {
                                    if (auto td = registry_->find_type(diag.expected_type))
                                        msg += " (expected: " + td->name + ")";
                                }
                            }
                            msg += '\n';
                        }
                    }
                    if (result.suggested_coercion.has_value()) {
                        msg += "  suggestion: apply coercion (" +
                            result.suggested_coercion->description + ")\n";
                    }
                    return msg;
                }

                // Register a custom coercion rule.
                void register_coercion(coercion_fact coercion) {
                    coercions_.push_back(std::move(coercion));
                }

                // Access the type-rule audit trail (which rules fired/failed/skipped).
                [[nodiscard]] const easy_rules::AuditListener& type_rule_audit() const {
                    return audit_;
                }

            private:
                // ---- built-in rules ----------------------------------------

                void register_builtin_rules_() {
                    engine_.config.verbose = false;
                    engine_.config.stop_on_first_match = false;

                    const int kInt = static_cast<int>(types::type_kind::integer);
                    const int kFloat = static_cast<int>(types::type_kind::floating);
                    const int kUnknown = static_cast<int>(types::type_kind::unknown);

                    using easy_rules::dsl::fact;
                    namespace dsl = easy_rules::dsl;

                    engine_.when("integer_widening",
                                 dsl::operator&&(
                                     dsl::operator&&(fact<int>("lhs_kind") == kInt,
                                                     fact<int>("rhs_kind") == kInt),
                                     [](const easy_rules::Facts& f) {
                                         auto lw = f.get<int>("lhs_width");
                                         auto rw = f.get<int>("rhs_width");
                                         return lw.has_value() && rw.has_value() && *lw != *rw;
                                     }))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               auto lhs_w = ctx.facts.get<int>("lhs_width");
                               auto rhs_w = ctx.facts.get<int>("rhs_width");
                               if (lhs_w && rhs_w) {
                                   ctx.facts.set("result_width", std::max(*lhs_w, *rhs_w));
                                   ctx.facts.set("result_kind",
                                                 static_cast<int>(types::type_kind::integer));
                                   ctx.facts.set("result_known", true);
                               }
                           })
                           .with_priority(10)
                           .with_description("widen integer operands to the wider type");

                    engine_.when("int_float_promotion",
                                 dsl::operator||(
                                     dsl::operator&&(fact<int>("lhs_kind") == kInt,
                                                     fact<int>("rhs_kind") == kFloat),
                                     dsl::operator&&(fact<int>("lhs_kind") == kFloat,
                                                     fact<int>("rhs_kind") == kInt)))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               auto lhs_w = ctx.facts.get<int>("lhs_width");
                               auto rhs_w = ctx.facts.get<int>("rhs_width");
                               ctx.facts.set("result_kind",
                                             static_cast<int>(types::type_kind::floating));
                               ctx.facts.set("result_width",
                                             std::max(lhs_w.value_or(32), rhs_w.value_or(32)));
                               ctx.facts.set("result_known", true);
                           })
                           .with_priority(20)
                           .with_description("int + float promotes to float");

                    engine_.when("homogeneous_type",
                                 [](const easy_rules::Facts& f) {
                                     auto lk = f.get<int>("lhs_kind"), rk = f.get<int>("rhs_kind");
                                     auto lw = f.get<int>("lhs_width"), rw = f.get<int>("rhs_width");
                                     return lk.has_value() && rk.has_value() &&
                                         lw.has_value() && rw.has_value() &&
                                         *lk == *rk && *lw == *rw;
                                 })
                           .then([](easy_rules::ExecutionContext& ctx) {
                               auto lhs_id = ctx.facts.get<int>("lhs_id");
                               if (lhs_id) {
                                   ctx.facts.set("result_id", *lhs_id);
                                   ctx.facts.set("result_known", true);
                               }
                           })
                           .with_priority(5)
                           .with_description("identical operand types yield same result type");

                    engine_.when("incompatible_types",
                                 dsl::operator&&(
                                     dsl::operator&&(
                                         dsl::operator&&(fact<int>("lhs_kind") != kInt,
                                                         fact<int>("lhs_kind") != kFloat),
                                         dsl::operator&&(fact<int>("lhs_kind") != kUnknown,
                                                         dsl::operator&&(fact<int>("rhs_kind") != kInt,
                                                                         fact<int>("rhs_kind") != kFloat))),
                                     dsl::operator&&(
                                         fact<int>("rhs_kind") != kUnknown,
                                         [](const easy_rules::Facts& f) {
                                             auto lk = f.get<int>("lhs_kind");
                                             auto rk = f.get<int>("rhs_kind");
                                             return lk.has_value() && rk.has_value() && *lk != *rk;
                                         })))
                           .then([](easy_rules::ExecutionContext& ctx) {
                               ctx.facts.set("type_error", true);
                               ctx.facts.set("type_error_msg",
                                             std::string("incompatible non-numeric type pair"));
                           })
                           .with_priority(30)
                           .with_description("flag non-numeric type incompatibility");
                }

                // ---- rule dispatch ------------------------------------------

                void run_rules_(operation_type_fact& fact, type_rule_result& result) {
                    run_easy_rules_(fact, result);
                }

                void run_easy_rules_(operation_type_fact& fact, type_rule_result& result) {
                    if (fact.operand_type_ids.size() < 2 || !registry_) {
                        run_fallback_rules_(fact, result);
                        return;
                    }

                    auto lhs_td = registry_->find_type(fact.operand_type_ids[0]);
                    auto rhs_td = registry_->find_type(fact.operand_type_ids[1]);
                    if (!lhs_td || !rhs_td) {
                        run_fallback_rules_(fact, result);
                        return;
                    }

                    easy_rules::ExecutionContext ctx;
                    ctx.facts.set("lhs_id", static_cast<int>(lhs_td->id));
                    ctx.facts.set("rhs_id", static_cast<int>(rhs_td->id));
                    ctx.facts.set("lhs_kind", static_cast<int>(lhs_td->kind));
                    ctx.facts.set("rhs_kind", static_cast<int>(rhs_td->kind));
                    ctx.facts.set("lhs_width", static_cast<int>(lhs_td->bit_width));
                    ctx.facts.set("rhs_width", static_cast<int>(rhs_td->bit_width));
                    ctx.facts.set("result_known", false);
                    ctx.facts.set("type_error", false);

                    engine_.run(ctx);

                    // Harvest result.
                    auto has_error = ctx.facts.get<bool>("type_error");
                    if (has_error && *has_error) {
                        auto msg = ctx.facts.get<std::string>("type_error_msg");
                        result.add_error("easy_rules",
                                         msg.value_or("type error detected"),
                                         fact.operand_type_ids[0],
                                         fact.operand_type_ids.size() > 1
                                             ? fact.operand_type_ids[1]
                                             : types::invalid_type_id);
                    }

                    auto known = ctx.facts.get<bool>("result_known");
                    if (known && *known) {
                        // Prefer explicit id set by rules.
                        auto rid = ctx.facts.get<int>("result_id");
                        if (rid && static_cast<types::type_id>(*rid) != types::invalid_type_id) {
                            fact.result_type_id = static_cast<types::type_id>(*rid);
                            fact.result_known = true;
                        }
                        else {
                            // Construct the result type on the fly.
                            auto rkind = ctx.facts.get<int>("result_kind");
                            auto rwidth = ctx.facts.get<int>("result_width");
                            if (rkind && rwidth) {
                                auto rk = static_cast<types::type_kind>(*rkind);
                                types::type_id tid = types::invalid_type_id;
                                if (rk == types::type_kind::integer) {
                                    tid = registry_->make_integer_type(
                                        static_cast<std::uint32_t>(*rwidth), true);
                                }
                                else if (rk == types::type_kind::floating) {
                                    tid = registry_->make_float_type(
                                        static_cast<std::uint32_t>(*rwidth));
                                }
                                if (tid != types::invalid_type_id) {
                                    fact.result_type_id = tid;
                                    fact.result_known = true;
                                }
                            }
                        }
                    }
                }

                void run_fallback_rules_(operation_type_fact& fact, type_rule_result& result) const {
                    if (fact.operand_type_ids.empty() || !registry_) return;

                    // Gather type descriptors for all operands.
                    std::vector<types::type_descriptor> descs;
                    descs.reserve(fact.operand_type_ids.size());
                    for (auto tid : fact.operand_type_ids) {
                        auto td = registry_->find_type(tid);
                        if (!td.has_value()) {
                            result.add_error("fallback", "unknown operand type id",
                                             tid);
                            return;
                        }
                        descs.push_back(*td);
                    }

                    // All unknown → stay unknown.
                    bool all_unknown = std::ranges::all_of(descs, [](const auto& d) {
                        return d.kind == types::type_kind::unknown;
                    });
                    if (all_unknown) return;

                    // Numeric promotion: walk operands, keep widest numeric type.
                    types::type_id result_type = fact.operand_type_ids[0];
                    for (std::size_t i = 1; i < descs.size(); ++i) {
                        const auto& a = descs[i - 1];
                        const auto& b = descs[i];

                        if (a.kind == b.kind && a.bit_width == b.bit_width) continue;

                        if (a.is_numeric() && b.is_numeric()) {
                            // float wins over integer.
                            if (b.kind == types::type_kind::floating &&
                                a.kind == types::type_kind::integer) {
                                result_type = registry_->make_float_type(
                                    std::max(a.bit_width, b.bit_width));
                            }
                            else if (a.kind == types::type_kind::floating &&
                                b.kind == types::type_kind::integer) {
                                result_type = registry_->make_float_type(
                                    std::max(a.bit_width, b.bit_width));
                            }
                            else {
                                // Both integer or both float: widen.
                                auto wider_w = std::max(a.bit_width, b.bit_width);
                                bool is_signed = (a.attributes.count("signed") &&
                                    a.attributes.at("signed") == "true");
                                if (a.kind == types::type_kind::floating) {
                                    result_type = registry_->make_float_type(wider_w);
                                }
                                else {
                                    result_type = registry_->make_integer_type(wider_w, is_signed);
                                }
                            }
                        }
                        else if (!a.is_numeric() || !b.is_numeric()) {
                            // Non-numeric pair: check for coercion.
                            auto coercion = find_coercion(fact.operand_type_ids[i], fact.operand_type_ids[0]);
                            if (!coercion.has_value()) {
                                result.add_error("fallback",
                                                 "incompatible operand types: no coercion available",
                                                 fact.operand_type_ids[i], fact.operand_type_ids[0]);
                                return;
                            }
                            result.suggested_coercion = coercion;
                        }
                    }

                    fact.result_type_id = result_type;
                    fact.result_known = true;
                }

                types::semantic_type_registry* registry_ = nullptr;
                std::vector<coercion_fact> coercions_;
                easy_rules::AuditListener audit_;
                mutable easy_rules::EasyRuleEngine engine_;
            };

            inline semantic_type_rule_engine& type_rule_engine() {
                static semantic_type_rule_engine instance;
                return instance;
            }

            // ----------------------------------------------------------------
            // Coercion kind — identifies what abstract transformation is needed.
            // These are semantic classifications, not machine code sequences.
            // ----------------------------------------------------------------
            enum class coercion_kind : std::uint8_t {
                identity, // no-op: source and target are semantically the same
                numeric_widen, // lossless widening: i32 → i64, f32 → f64, i32 → f64
                numeric_narrow, // potentially lossy narrowing: i64 → i32, f64 → f32
                pointer_cast, // reinterpret pointer type without changing representation
                checked_cast, // checked downcast with runtime type identity (avoids C++ keyword)
                box, // value → heap-allocated owning reference (e.g., int → Integer)
                unbox, // heap-allocated reference → value (e.g., Integer → int)
                tensor_reshape, // same element count/type, different shape (no data copy implied)
                user_defined // coercion described by a registered user rule
            };

            [[nodiscard]] constexpr std::string_view coercion_kind_name(const coercion_kind k) noexcept {
                switch (k) {
                case coercion_kind::identity: return "identity";
                case coercion_kind::numeric_widen: return "numeric_widen";
                case coercion_kind::numeric_narrow: return "numeric_narrow";
                case coercion_kind::pointer_cast: return "pointer_cast";
                case coercion_kind::checked_cast: return "dynamic_cast";
                case coercion_kind::box: return "box";
                case coercion_kind::unbox: return "unbox";
                case coercion_kind::tensor_reshape: return "tensor_reshape";
                case coercion_kind::user_defined: return "user_defined";
                }
                return "unknown";
            }

            // Is this coercion guaranteed to be lossless?
            [[nodiscard]] constexpr bool coercion_is_lossless(const coercion_kind k) noexcept {
                return k == coercion_kind::identity ||
                    k == coercion_kind::numeric_widen ||
                    k == coercion_kind::pointer_cast ||
                    k == coercion_kind::tensor_reshape;
            }

            // ----------------------------------------------------------------
            // coercion_rule — declarative rule binding a (from, to) type pair
            // to a specific coercion_kind, with an optional custom label and
            // validation predicate.
            // ----------------------------------------------------------------
            struct coercion_rule {
                std::string name;
                types::type_id from_type = types::invalid_type_id;
                types::type_id to_type = types::invalid_type_id;
                coercion_kind kind = coercion_kind::identity;
                bool is_implicit = false; // allowed without explicit cast
                bool is_lossless = true;
                std::string description;

                // Optional user predicate: given the type registry, should this rule fire?
                std::function<bool(const types::semantic_type_registry&,
                                   types::type_id from, types::type_id to)> precondition;

                [[nodiscard]] bool applies_to(const types::type_id from, const types::type_id to) const noexcept {
                    return from_type == from && to_type == to;
                }

                [[nodiscard]] bool check_precondition(const types::semantic_type_registry& reg,
                                                      const types::type_id from, const types::type_id to) const {
                    if (!precondition) return true;
                    return precondition(reg, from, to);
                }
            };

            // ----------------------------------------------------------------
            // coercion_step — one atomic coercion in a multi-hop plan.
            // ----------------------------------------------------------------
            struct coercion_step {
                types::type_id from_type = types::invalid_type_id;
                types::type_id to_type = types::invalid_type_id;
                coercion_kind kind = coercion_kind::identity;
                bool is_lossless = true;
                std::string rule_name;
                std::string description;
            };

            // ----------------------------------------------------------------
            // coercion_plan — fully resolved conversion from a source type to
            // a target type, potentially via intermediate steps.
            //
            // The plan is produced by semantic_type_rule_engine::plan_coercion
            // and consumed by the coercion_lowering_pass in lithe_lowering.hpp.
            // ----------------------------------------------------------------
            struct coercion_plan {
                types::type_id source_type = types::invalid_type_id;
                types::type_id target_type = types::invalid_type_id;
                std::vector<coercion_step> steps;
                bool feasible = false;
                bool is_lossless = true;
                std::string failure_reason;

                [[nodiscard]] bool trivial() const noexcept {
                    return steps.size() == 1 && steps[0].kind == coercion_kind::identity;
                }

                [[nodiscard]] std::string describe() const {
                    if (!feasible) return "infeasible: " + failure_reason;
                    if (steps.empty()) return "identity";
                    std::string s;
                    for (std::size_t i = 0; i < steps.size(); ++i) {
                        if (i > 0) s += " -> ";
                        s += std::string{coercion_kind_name(steps[i].kind)};
                    }
                    return s;
                }
            };

            // ----------------------------------------------------------------
            // Extend semantic_type_rule_engine with coercion-rule registration
            // and coercion plan synthesis.
            // ----------------------------------------------------------------

            // Forward declaration: these free functions wrap the global engine.
            [[nodiscard]] inline coercion_plan plan_coercion(
                types::type_id from, types::type_id to,
                const types::semantic_type_registry* registry = nullptr);

            [[nodiscard]] inline std::optional<coercion_rule> find_coercion_rule(
                types::type_id from, types::type_id to);

            inline void register_coercion_rule(coercion_rule rule);

            // ----------------------------------------------------------------
            // coercion_rule_registry — store for named coercion rules.
            // Registration happens at program startup from a single thread;
            // no synchronisation is needed here.
            // ----------------------------------------------------------------
            class coercion_rule_registry {
            public:
                void add(coercion_rule rule) {
                    rules_.push_back(std::move(rule));
                }

                [[nodiscard]] std::optional<coercion_rule> find(
                    const types::type_id from, const types::type_id to) const {
                    for (const auto& r : rules_) {
                        if (r.applies_to(from, to)) return r;
                    }
                    return std::nullopt;
                }

                // Find all rules whose source is `from` (for multi-hop planning).
                [[nodiscard]] std::vector<coercion_rule> find_from(const types::type_id from) const {
                    std::vector<coercion_rule> out;
                    for (const auto& r : rules_) {
                        if (r.from_type == from) out.push_back(r);
                    }
                    return out;
                }

                [[nodiscard]] const std::vector<coercion_rule>& all() const {
                    return rules_;
                }

            private:
                std::vector<coercion_rule> rules_;
            };

            inline coercion_rule_registry& coercion_registry() {
                static coercion_rule_registry instance;
                return instance;
            }

            // ----------------------------------------------------------------
            // coercion_planner — resolves a (from, to) pair into a coercion_plan.
            // Uses registered coercion_rules; falls back to registry subtype rules.
            // ----------------------------------------------------------------
            class coercion_planner {
            public:
                explicit coercion_planner(
                    const types::semantic_type_registry* type_reg = &types::type_registry(),
                    const coercion_rule_registry* rule_reg = &coercion_registry())
                    : type_reg_(type_reg), rule_reg_(rule_reg) {}

                [[nodiscard]] coercion_plan plan(
                    const types::type_id from, const types::type_id to) const {
                    coercion_plan out;
                    out.source_type = from;
                    out.target_type = to;

                    if (from == to) {
                        out.feasible = true;
                        out.is_lossless = true;
                        out.steps.push_back({from, to, coercion_kind::identity, true, "identity", "same type"});
                        return out;
                    }

                    // Direct rule lookup.
                    if (rule_reg_) {
                        if (auto rule = rule_reg_->find(from, to); rule.has_value()) {
                            if (!type_reg_ || rule->check_precondition(*type_reg_, from, to)) {
                                out.feasible = true;
                                out.is_lossless = rule->is_lossless;
                                out.steps.push_back({
                                    from, to, rule->kind, rule->is_lossless,
                                    rule->name, rule->description
                                });
                                return out;
                            }
                        }
                    }

                    // Registry-based structural widening fallback.
                    if (type_reg_) {
                        if (type_reg_->subtype_of(from, to)) {
                            auto k = derive_kind_from_registry_(from, to);
                            out.feasible = true;
                            out.is_lossless = true;
                            out.steps.push_back({from, to, k, true, "registry_widening", "structural widening"});
                            return out;
                        }
                    }

                    // BFS for a multi-hop path through registered rules (max depth 3).
                    if (rule_reg_) {
                        auto path = bfs_plan_(from, to, 3);
                        if (!path.empty()) {
                            out.feasible = true;
                            out.is_lossless = true;
                            for (const auto& step : path) {
                                out.is_lossless = out.is_lossless && step.is_lossless;
                            }
                            out.steps = std::move(path);
                            return out;
                        }
                    }

                    out.feasible = false;
                    out.failure_reason = "no coercion path found from type " +
                        std::to_string(from) + " to type " +
                        std::to_string(to);
                    return out;
                }

            private:
                [[nodiscard]] coercion_kind derive_kind_from_registry_(
                    types::type_id from, types::type_id to) const noexcept {
                    if (!type_reg_) return coercion_kind::user_defined;
                    auto fa = type_reg_->find_type(from);
                    auto ta = type_reg_->find_type(to);
                    if (!fa || !ta) return coercion_kind::user_defined;

                    const bool from_num = fa->is_numeric();
                    const bool to_num = ta->is_numeric();
                    if (from_num && to_num) {
                        return (fa->bit_width <= ta->bit_width)
                                   ? coercion_kind::numeric_widen
                                   : coercion_kind::numeric_narrow;
                    }
                    if (fa->kind == types::type_kind::tensor && ta->kind == types::type_kind::tensor) {
                        return coercion_kind::tensor_reshape;
                    }
                    if (fa->kind == types::type_kind::pointer || ta->kind == types::type_kind::pointer) {
                        return coercion_kind::pointer_cast;
                    }
                    return coercion_kind::user_defined;
                }

                [[nodiscard]] std::vector<coercion_step> bfs_plan_(
                    const types::type_id from, const types::type_id to, const int max_depth) const {
                    if (max_depth <= 0) return {};
                    struct Node {
                        types::type_id type;
                        std::vector<coercion_step> path;
                    };

                    std::vector<Node> frontier{{from, {}}};
                    ::lithe::detail::flat_set<types::type_id> visited;
                    visited.insert(from);

                    for (int depth = 0; depth < max_depth && !frontier.empty(); ++depth) {
                        std::vector<Node> next;
                        for (const auto& node : frontier) {
                            for (const auto& rule : rule_reg_->find_from(node.type)) {
                                if (visited.contains(rule.to_type)) continue;
                                auto path = node.path;
                                path.push_back({
                                    node.type, rule.to_type, rule.kind,
                                    rule.is_lossless, rule.name, rule.description
                                });
                                if (rule.to_type == to) return path;
                                visited.insert(rule.to_type);
                                next.push_back({rule.to_type, std::move(path)});
                            }
                        }
                        frontier = std::move(next);
                    }
                    return {};
                }

                const types::semantic_type_registry* type_reg_;
                const coercion_rule_registry* rule_reg_;
            };

            // Free-function wrappers for ergonomic use.
            [[nodiscard]] inline coercion_plan plan_coercion(
                const types::type_id from, const types::type_id to,
                const types::semantic_type_registry* registry) {
                return coercion_planner{registry, &coercion_registry()}.plan(from, to);
            }

            [[nodiscard]] inline std::optional<coercion_rule> find_coercion_rule(
                const types::type_id from, const types::type_id to) {
                return coercion_registry().find(from, to);
            }

            inline void register_coercion_rule(coercion_rule rule) {
                coercion_registry().add(std::move(rule));
            }

            // Convenience: build a coercion_plan using the global type registry.
            [[nodiscard]] inline coercion_plan plan_coercion(
                const types::type_id from, const types::type_id to) {
                return coercion_planner{&types::type_registry(), &coercion_registry()}.plan(from, to);
            }

            // =================================================================
            // Prompt 1: Type constraints
            // =================================================================

            // source_span — lightweight source location for constraint diagnostics.
            struct source_span {
                std::uint32_t line = 0;
                std::uint32_t column = 0;
                std::uint32_t length = 0;
                std::string_view file;

                [[nodiscard]] bool valid() const noexcept { return line != 0; }
            };

            enum class type_constraint_kind : std::uint8_t {
                equality,
                subtype,
                assignable,
                convertible,
                callable,
                indexable,
                iterable,
                tensor_compatible,
                layout_compatible,
                symbolic_compatible,
                backend_legal,
                effect_compatible
            };

            [[nodiscard]] constexpr std::string_view type_constraint_kind_name(
                const type_constraint_kind k) noexcept {
                switch (k) {
                case type_constraint_kind::equality: return "equality";
                case type_constraint_kind::subtype: return "subtype";
                case type_constraint_kind::assignable: return "assignable";
                case type_constraint_kind::convertible: return "convertible";
                case type_constraint_kind::callable: return "callable";
                case type_constraint_kind::indexable: return "indexable";
                case type_constraint_kind::iterable: return "iterable";
                case type_constraint_kind::tensor_compatible: return "tensor_compatible";
                case type_constraint_kind::layout_compatible: return "layout_compatible";
                case type_constraint_kind::symbolic_compatible: return "symbolic_compatible";
                case type_constraint_kind::backend_legal: return "backend_legal";
                case type_constraint_kind::effect_compatible: return "effect_compatible";
                }
                return "unknown";
            }

            struct type_constraint {
                type_constraint_kind kind = type_constraint_kind::equality;
                types::type_id lhs = types::invalid_type_id;
                types::type_id rhs = types::invalid_type_id;
                source_span span;
                std::string diagnostic_context;
                bool hard_constraint = true;
            };

            // =================================================================
            // Prompt 1: Unification result
            // =================================================================

            struct type_unification_result {
                bool success = true;
                std::vector<type_constraint> failed_constraints;
                std::vector<type_rule_diagnostic> diagnostics;
                ::lithe::detail::flat_map<types::type_variable_id, types::type_id> substitutions;

                [[nodiscard]] bool ok() const noexcept { return success; }

                void add_failure(type_constraint c,
                                 std::string rule, std::string msg,
                                 const types::type_id t = types::invalid_type_id,
                                 const types::type_id exp = types::invalid_type_id) {
                    success = false;
                    failed_constraints.push_back(std::move(c));
                    diagnostics.push_back({
                        type_rule_severity::error,
                        std::move(rule),
                        std::move(msg),
                        t, exp
                    });
                }
            };

            // =================================================================
            // Prompt 1: semantic_type_unifier
            //
            // Uses DisjointSet<type_id> internally for equivalence classes.
            // Supports:
            //   - unify(type_id, type_id) → merge two types into same class
            //   - unify_constraint(type_constraint)
            //   - resolve(type_variable_id)
            //   - apply_substitutions(type_id) → walk through subst map
            //   - snapshot() / rollback(token) / commit(token) for backtracking
            // =================================================================

            class semantic_type_unifier {
            public:
                using UnifierDS = disjointset::DisjointSet<types::type_id>;

                // Opaque snapshot token — wraps the DS snapshot depth so the
                // caller can't accidentally use the wrong integer.
                struct snapshot_token {
                    std::size_t depth = 0;
                };

                explicit semantic_type_unifier(
                    types::semantic_type_registry* reg = &types::type_registry())
                    : registry_(reg) {}

                // ---- core unification API ------------------------------------

                // Unify two concrete type_ids: merge their equivalence classes.
                // Returns true if the unification is structurally valid.
                bool unify(const types::type_id a, const types::type_id b,
                           type_unification_result& out) {
                    if (a == types::invalid_type_id || b == types::invalid_type_id) {
                        out.add_failure({}, "unify", "invalid type_id in unification", a, b);
                        return false;
                    }
                    if (a == b) return true;

                    ensure_registered_(a);
                    ensure_registered_(b);

                    // Check structural compatibility before merging.
                    if (!structurally_compatible_(a, b)) {
                        type_constraint fc{type_constraint_kind::equality, a, b, {}, "structural mismatch"};
                        out.add_failure(std::move(fc), "structural_unify",
                                        "cannot unify structurally incompatible types", a, b);
                        return false;
                    }

                    [[maybe_unused]] auto _u = ds_.unite(a, b);

                    // Propagate substitutions to any type variables bound to a or b.
                    for (auto& [var, bound_id] : var_bindings_) {
                        if (bound_id == a) bound_id = canonical(b);
                        else if (bound_id == b) bound_id = canonical(a);
                    }
                    return true;
                }

                // Unify according to a typed constraint.
                bool unify_constraint(const type_constraint& c,
                                      type_unification_result& out) {
                    switch (c.kind) {
                    case type_constraint_kind::equality:
                        return unify(c.lhs, c.rhs, out);

                    case type_constraint_kind::subtype:
                        if (registry_ && !registry_->subtype_of(c.lhs, c.rhs)) {
                            auto fc = c;
                            out.add_failure(std::move(fc), "subtype_constraint",
                                            "subtype constraint violated", c.lhs, c.rhs);
                            return false;
                        }
                        return true;

                    case type_constraint_kind::assignable:
                        if (registry_) {
                            if (c.lhs == c.rhs || registry_->subtype_of(c.lhs, c.rhs) ||
                                registry_->subtype_of(c.rhs, c.lhs))
                                return true;
                        }
                        return unify(c.lhs, c.rhs, out);

                    case type_constraint_kind::convertible: {
                        auto plan = coercion_planner{registry_, &coercion_registry()}.plan(c.lhs, c.rhs);
                        if (!plan.feasible) {
                            auto fc = c;
                            out.add_failure(std::move(fc), "convertible_constraint",
                                            "no coercion path: " + plan.failure_reason,
                                            c.lhs, c.rhs);
                            return false;
                        }
                        return true;
                    }

                    default:
                        // For capability constraints, simply record the constraint.
                        pending_constraints_.push_back(c);
                        return true;
                    }
                }

                // Batch unification: all-or-nothing if any hard constraint fails.
                type_unification_result unify_all(const std::vector<type_constraint>& constraints) {
                    type_unification_result result;
                    auto snap = snapshot();
                    for (const auto& c : constraints) {
                        if (!unify_constraint(c, result) && c.hard_constraint) {
                            rollback(snap);
                            return result;
                        }
                    }
                    commit(snap);
                    return result;
                }

                // ---- type variable API -------------------------------------

                // Allocate a fresh type variable and record its descriptor.
                types::type_variable_id new_variable(std::string debug_name = {},
                                                     const bool rigid = false) {
                    const std::uint32_t vid = next_var_id_++;
                    types::type_variable_id varid{vid};

                    types::type_variable_descriptor desc;
                    desc.id = vid;
                    desc.debug_name = debug_name.empty()
                                          ? ("?T" + std::to_string(vid))
                                          : std::move(debug_name);
                    desc.rigid = rigid;
                    var_descriptors_[varid] = std::move(desc);

                    // Each variable gets a synthetic type_id so it can live in the DS.
                    const types::type_id synthetic_id = synthetic_id_base_ + vid;
                    ensure_registered_(synthetic_id);
                    var_to_type_id_[varid] = synthetic_id;
                    type_id_to_var_[synthetic_id] = varid;
                    return varid;
                }

                // Bind a type variable to a concrete type_id (unification-based).
                bool bind_variable(const types::type_variable_id var,
                                   const types::type_id concrete_type,
                                   type_unification_result& out) {
                    if (auto it = var_to_type_id_.find(var); it != var_to_type_id_.end()) {
                        return unify(it->second, concrete_type, out);
                    }
                    out.add_failure({}, "bind_variable", "unknown type variable", concrete_type);
                    return false;
                }

                // Resolve a type variable: follow the DS to its canonical representative.
                [[nodiscard]] std::optional<types::type_id>
                resolve(const types::type_variable_id var) const {
                    auto it = var_to_type_id_.find(var);
                    if (it == var_to_type_id_.end()) return std::nullopt;

                    const types::type_id synth = it->second;
                    const types::type_id root = canonical(synth);

                    // If the root is still a synthetic (unresolved variable), return nullopt.
                    if (type_id_to_var_.contains(root)) return std::nullopt;
                    return root;
                }

                // Walk substitution map and return concrete type for an id.
                // If the id is a synthetic variable id, follow through to concrete.
                [[nodiscard]] types::type_id
                apply_substitutions(const types::type_id id) const {
                    if (id == types::invalid_type_id) return id;
                    // Check if this id is a synthetic variable.
                    if (auto vit = type_id_to_var_.find(id); vit != type_id_to_var_.end()) {
                        if (auto resolved = resolve(vit->second); resolved.has_value())
                            return *resolved;
                        return id; // still unresolved
                    }
                    // For concrete ids, return the canonical representative.
                    return canonical(id);
                }

                // Convenience overload that resolves a variable descriptor's resolved_type.
                [[nodiscard]] std::optional<types::type_id>
                apply_substitutions(const types::type_variable_id var) const {
                    return resolve(var);
                }

                // ---- snapshot/rollback API ----------------------------------

                snapshot_token snapshot() {
                    ds_.push_snapshot();
                    return snapshot_token{ds_.snapshot_depth()};
                }

                void rollback(const snapshot_token& token) {
                    while (ds_.snapshot_depth() >= token.depth) {
                        [[maybe_unused]] auto _r = ds_.rollback();
                    }
                }

                void commit(const snapshot_token& token) {
                    while (ds_.snapshot_depth() >= token.depth) {
                        [[maybe_unused]] auto _c = ds_.commit();
                    }
                }

                // ---- query API --------------------------------------------

                [[nodiscard]] bool are_unified(const types::type_id a, const types::type_id b) const {
                    if (a == b) return true;
                    if (!ds_.contains(a) || !ds_.contains(b)) return false;
                    return ds_.connected(a, b);
                }

                [[nodiscard]] types::type_id canonical(const types::type_id id) const {
                    if (!ds_.contains(id)) return id;
                    auto r = ds_.representative(id);
                    return r ? *r : id;
                }

                // Collect all pending (non-equality) constraints.
                [[nodiscard]] const std::vector<type_constraint>&
                pending_constraints() const noexcept {
                    return pending_constraints_;
                }

                // Access variable descriptor (for diagnostics/debugging).
                [[nodiscard]] std::optional<types::type_variable_descriptor>
                variable_descriptor(const types::type_variable_id var) const {
                    auto it = var_descriptors_.find(var);
                    if (it == var_descriptors_.end()) return std::nullopt;
                    return it->second;
                }

                // Produce a snapshot of all current substitutions (resolved variables).
                [[nodiscard]] ::lithe::detail::flat_map<types::type_variable_id, types::type_id>
                substitution_snapshot() const {
                    ::lithe::detail::flat_map<types::type_variable_id, types::type_id> result;
                    for (const auto& [var, synth_id] : var_to_type_id_) {
                        if (auto resolved = resolve(var); resolved.has_value())
                            result[var] = *resolved;
                    }
                    return result;
                }

            private:
                // Ensure a type_id is registered in the DisjointSet.
                void ensure_registered_(const types::type_id id) {
                    if (!ds_.contains(id))
                        ds_.insert_or_get(id);
                }

                // Structural compatibility: checks that two concrete types can
                // be unified (same kind, compatible widths).  Variables are
                // always compatible — they are resolved, not checked here.
                bool structurally_compatible_(types::type_id a,
                                              types::type_id b) const {
                    if (!registry_) return true;
                    // If either is a synthetic variable id, always compatible.
                    if (type_id_to_var_.contains(a) || type_id_to_var_.contains(b))
                        return true;

                    auto da = registry_->find_type(a);
                    auto db = registry_->find_type(b);
                    if (!da || !db) return true; // unknown types: optimistically compatible

                    if (da->kind == types::type_kind::unknown ||
                        db->kind == types::type_kind::unknown)
                        return true;

                    // Same kind required (numeric widening handled by subtype_of).
                    if (da->kind != db->kind) {
                        // Allow integer ↔ floating numeric promotion.
                        const bool a_num = da->is_numeric();
                        const bool b_num = db->is_numeric();
                        return a_num && b_num;
                    }
                    return true;
                }

                types::semantic_type_registry* registry_ = nullptr;
                UnifierDS ds_;
                std::vector<type_constraint> pending_constraints_;

                // Variable bookkeeping
                std::uint32_t next_var_id_ = 0;
                static constexpr types::type_id synthetic_id_base_ =
                    static_cast<types::type_id>(1) << 32;

                ::lithe::detail::flat_map<types::type_variable_id,
                                          types::type_id> var_to_type_id_;
                ::lithe::detail::flat_map<types::type_id,
                                          types::type_variable_id> type_id_to_var_;
                ::lithe::detail::flat_map<types::type_variable_id,
                                          types::type_variable_descriptor> var_descriptors_;
                ::lithe::detail::flat_map<types::type_variable_id,
                                          types::type_id> var_bindings_;
            };

            // Global instance accessor.
            inline semantic_type_unifier& type_unifier() {
                static semantic_type_unifier instance;
                return instance;
            }

            // =================================================================
            // Prompt 2: Inference graph and propagation engine
            // =================================================================

            // -----------------------------------------------------------------
            // Node and edge id types
            // -----------------------------------------------------------------
            struct inference_node_id {
                std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

                constexpr bool is_valid() const noexcept {
                    return value != std::numeric_limits<std::uint32_t>::max();
                }

                constexpr auto operator<=>(const inference_node_id&) const noexcept = default;
            };

            struct inference_edge_id {
                std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

                constexpr bool is_valid() const noexcept {
                    return value != std::numeric_limits<std::uint32_t>::max();
                }

                constexpr auto operator<=>(const inference_edge_id&) const noexcept = default;
            };

            inline constexpr inference_node_id invalid_inference_node{};
            inline constexpr inference_edge_id invalid_inference_edge{};

            // -----------------------------------------------------------------
            // Inference graph nodes and edges
            // -----------------------------------------------------------------
            struct semantic_inference_node {
                inference_node_id id;
                types::type_id type = types::invalid_type_id;

                // Optional references to the AST element this node represents.
                std::optional<structural_hash_t> expression_key;
                std::optional<std::string> operation_ref;

                source_span span;
                ::lithe::detail::flat_map<std::string, std::string> attributes;

                [[nodiscard]] bool has_type() const noexcept {
                    return type != types::invalid_type_id;
                }
            };

            struct semantic_inference_edge {
                inference_edge_id id;
                inference_node_id source;
                inference_node_id target;
                type_constraint_kind constraint_kind = type_constraint_kind::equality;

                struct propagation_rule_meta {
                    std::string rule_name;
                    bool bidirectional = false;
                    bool strong = true;
                };

                propagation_rule_meta rule_meta;
            };

            // -----------------------------------------------------------------
            // semantic_inference_graph
            //
            // A directed constraint graph where nodes are typed values and
            // edges are type constraints between them.  Populated by the
            // propagation engine during type inference.
            // -----------------------------------------------------------------
            class semantic_inference_graph {
            public:
                // Add a node (typed value).
                inference_node_id add_node(const types::type_id type = types::invalid_type_id,
                                           const std::optional<structural_hash_t> expr_key = std::nullopt,
                                           source_span span = {}) {
                    const inference_node_id id{next_node_id_++};
                    semantic_inference_node n;
                    n.id = id;
                    n.type = type;
                    n.expression_key = expr_key;
                    n.span = span;
                    nodes_[id] = std::move(n);
                    return id;
                }

                // Add an edge (constraint between two nodes).
                inference_edge_id add_edge(const inference_node_id src,
                                           const inference_node_id dst,
                                           const type_constraint_kind kind,
                                           semantic_inference_edge::propagation_rule_meta meta = {}) {
                    const inference_edge_id eid{next_edge_id_++};
                    semantic_inference_edge e;
                    e.id = eid;
                    e.source = src;
                    e.target = dst;
                    e.constraint_kind = kind;
                    e.rule_meta = std::move(meta);
                    edges_[eid] = e;
                    adj_out_[src].push_back(eid);
                    adj_in_[dst].push_back(eid);
                    return eid;
                }

                // Retrieve a node by id.
                [[nodiscard]] std::optional<semantic_inference_node>
                node(const inference_node_id id) const {
                    auto it = nodes_.find(id);
                    if (it == nodes_.end()) return std::nullopt;
                    return it->second;
                }

                // Mutable node access.
                [[nodiscard]] semantic_inference_node*
                node_mut(const inference_node_id id) {
                    auto it = nodes_.find(id);
                    return it == nodes_.end() ? nullptr : &it->second;
                }

                // Retrieve an edge by id.
                [[nodiscard]] std::optional<semantic_inference_edge>
                edge(const inference_edge_id id) const {
                    auto it = edges_.find(id);
                    if (it == edges_.end()) return std::nullopt;
                    return it->second;
                }

                // All outgoing edges from a node.
                [[nodiscard]] std::vector<semantic_inference_edge>
                outgoing(const inference_node_id id) const {
                    std::vector<semantic_inference_edge> result;
                    auto it = adj_out_.find(id);
                    if (it == adj_out_.end()) return result;
                    for (auto eid : it->second)
                        if (auto e = edge(eid); e.has_value()) result.push_back(*e);
                    return result;
                }

                // All incoming edges to a node.
                [[nodiscard]] std::vector<semantic_inference_edge>
                incoming(const inference_node_id id) const {
                    std::vector<semantic_inference_edge> result;
                    auto it = adj_in_.find(id);
                    if (it == adj_in_.end()) return result;
                    for (auto eid : it->second)
                        if (auto e = edge(eid); e.has_value()) result.push_back(*e);
                    return result;
                }

                // Look up a node by expression structural key.
                [[nodiscard]] std::optional<inference_node_id>
                find_by_expr(const structural_hash_t key) const {
                    auto it = expr_to_node_.find(key);
                    if (it == expr_to_node_.end()) return std::nullopt;
                    return it->second;
                }

                // Register a mapping from expression key → node.
                void associate_expr(structural_hash_t key, const inference_node_id id) {
                    expr_to_node_[key] = id;
                    if (auto* n = node_mut(id)) n->expression_key = key;
                }

                [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
                [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }

                // Returns all registered node IDs; use this for iteration rather than 0..node_count().
                [[nodiscard]] std::vector<inference_node_id> all_node_ids() const {
                    std::vector<inference_node_id> ids;
                    ids.reserve(nodes_.size());
                    for (const auto& [id, _] : nodes_) ids.push_back(id);
                    return ids;
                }

                void clear() {
                    nodes_.clear();
                    edges_.clear();
                    adj_out_.clear();
                    adj_in_.clear();
                    expr_to_node_.clear();
                    next_node_id_ = 0;
                    next_edge_id_ = 0;
                }

            private:
                ::lithe::detail::flat_map<inference_node_id,
                                          semantic_inference_node> nodes_;
                ::lithe::detail::flat_map<inference_edge_id,
                                          semantic_inference_edge> edges_;
                ::lithe::detail::flat_map<inference_node_id,
                                          std::vector<inference_edge_id>> adj_out_;
                ::lithe::detail::flat_map<inference_node_id,
                                          std::vector<inference_edge_id>> adj_in_;
                ::lithe::detail::flat_map<structural_hash_t, inference_node_id> expr_to_node_;

                std::uint32_t next_node_id_ = 0;
                std::uint32_t next_edge_id_ = 0;
            };

            // -----------------------------------------------------------------
            // Prompt 2: Diagnostics — human-readable explanation chain
            // -----------------------------------------------------------------

            struct constraint_explanation {
                type_constraint constraint;
                std::string rule_name;
                std::string human_message;
                source_span span;
                std::vector<std::string> context_chain;

                [[nodiscard]] std::string render() const {
                    std::string s;
                    if (span.valid())
                        s += std::string{span.file} + ":" +
                            std::to_string(span.line) + ":" +
                            std::to_string(span.column) + ": ";
                    s += "[" + rule_name + "] " + human_message;
                    for (const auto& c : context_chain) s += "\n  note: " + c;
                    return s;
                }
            };

            struct type_conflict_trace {
                inference_node_id origin;
                std::vector<type_constraint> conflict_chain;
                std::vector<constraint_explanation> explanations;
                source_span primary_span;

                [[nodiscard]] bool has_conflict() const noexcept {
                    return !conflict_chain.empty();
                }

                [[nodiscard]] std::string render(
                    const types::semantic_type_registry* reg = nullptr) const {
                    std::string s;
                    if (primary_span.valid())
                        s += std::string{primary_span.file} + ":" +
                            std::to_string(primary_span.line) + ": ";
                    s += "type conflict trace (" +
                        std::to_string(conflict_chain.size()) + " constraint(s)):\n";
                    for (const auto& exp : explanations)
                        s += "  " + exp.render() + "\n";
                    return s;
                }
            };

            // -----------------------------------------------------------------
            // Prompt 2: semantic_constraint_propagator
            //
            // Propagates type constraints through a semantic_inference_graph,
            // inferring missing types from operands and operation contracts,
            // and detecting conflicts.
            // -----------------------------------------------------------------
            class semantic_constraint_propagator {
            public:
                explicit semantic_constraint_propagator(
                    semantic_inference_graph* graph = nullptr,
                    semantic_type_unifier* unifier = nullptr,
                    types::semantic_type_registry* reg = &types::type_registry())
                    : graph_(graph), unifier_(unifier), registry_(reg) {}

                // Main entry points ----------------------------------------

                // Propagate equality/subtype constraints through the graph
                // using a worklist fixed-point iteration.
                type_unification_result propagate_constraints() {
                    type_unification_result result;
                    if (!graph_ || !unifier_) return result;

                    bool changed = true;
                    std::size_t iter = 0;
                    static constexpr std::size_t max_iter = 64;

                    while (changed && iter < max_iter) {
                        changed = false;
                        ++iter;
                        for (const auto nid : graph_->all_node_ids()) {
                            changed |= propagate_node_(nid, result);
                        }
                    }
                    return result;
                }

                // Infer missing types by walking outgoing edges.
                void infer_missing_types() {
                    if (!graph_ || !unifier_ || !registry_) return;
                    type_unification_result dummy;
                    for (const auto node : graph_->all_node_ids()) {
                        auto n = graph_->node(node);
                        if (!n || n->has_type()) continue;
                        // Try to infer from incoming equality-class neighbours.
                        for (const auto& edge : graph_->incoming(node)) {
                            if (edge.constraint_kind != type_constraint_kind::equality)
                                continue;
                            auto src = graph_->node(edge.source);
                            if (src && src->has_type()) {
                                auto* mn = graph_->node_mut(node);
                                if (mn) {
                                    mn->type = unifier_->apply_substitutions(src->type);
                                    break;
                                }
                            }
                        }
                    }
                }

                // Run propagation until no further changes occur.
                type_unification_result stabilize_constraints() {
                    infer_missing_types();
                    return propagate_constraints();
                }

                // Detect conflicting constraints for a single node.
                [[nodiscard]] type_conflict_trace
                detect_conflicts(inference_node_id node_id) {
                    type_conflict_trace trace;
                    trace.origin = node_id;
                    if (!graph_ || !unifier_) return trace;

                    auto node = graph_->node(node_id);
                    if (!node) return trace;
                    if (node->span.valid()) trace.primary_span = node->span;

                    auto edges = graph_->outgoing(node_id);
                    for (const auto& edge : edges) {
                        auto tgt = graph_->node(edge.target);
                        if (!tgt || !node->has_type() || !tgt->has_type()) continue;

                        type_constraint c{
                            edge.constraint_kind,
                            node->type,
                            tgt->type,
                            node->span,
                            "propagation",
                            true
                        };

                        type_unification_result probe;
                        auto snap = unifier_->snapshot();
                        const bool ok = unifier_->unify_constraint(c, probe);
                        unifier_->rollback(snap);

                        if (!ok || !probe.ok()) {
                            trace.conflict_chain.push_back(c);
                            constraint_explanation exp;
                            exp.constraint = c;
                            exp.rule_name = std::string{
                                type_constraint_kind_name(edge.constraint_kind)
                            };
                            exp.human_message = probe.diagnostics.empty()
                                                    ? "constraint violated"
                                                    : probe.diagnostics.front().message;
                            exp.span = node->span;
                            exp.context_chain.push_back(
                                edge.rule_meta.rule_name.empty()
                                    ? "inference edge"
                                    : edge.rule_meta.rule_name);
                            trace.explanations.push_back(std::move(exp));
                        }
                    }
                    return trace;
                }

                // Detect conflicts for every node; returns per-node traces.
                [[nodiscard]] std::vector<type_conflict_trace>
                detect_all_conflicts() {
#if LITHE_HAS_PROFILER
                    profiler::ScopedProfiler _prof{"lithe.semantic.detect_all_conflicts"};
#endif
                    std::vector<type_conflict_trace> out;
                    if (!graph_) return out;
                    for (const auto nid : graph_->all_node_ids()) {
                        auto trace = detect_conflicts(nid);
                        if (trace.has_conflict()) out.push_back(std::move(trace));
                    }
                    return out;
                }

                // Build a human-readable explanation for a conflict trace.
                [[nodiscard]] std::string explain_conflict(
                    const type_conflict_trace& trace) const {
                    return trace.render(registry_);
                }

            private:
                // Propagate a single node: unify all equality-constrained
                // neighbours in the DS.  Returns true if any type changed.
                bool propagate_node_(inference_node_id nid,
                                     type_unification_result& result) {
                    bool changed = false;
                    auto node = graph_->node(nid);
                    if (!node) return false;

                    for (const auto& edge : graph_->outgoing(nid)) {
                        if (edge.constraint_kind != type_constraint_kind::equality)
                            continue;

                        auto tgt = graph_->node(edge.target);
                        if (!tgt) continue;

                        if (node->has_type() && tgt->has_type()) {
                            // Both known: unify them.
                            if (!unifier_->are_unified(node->type, tgt->type)) {
                                unifier_->unify(node->type, tgt->type, result);
                                changed = true;
                            }
                        }
                        else if (node->has_type() && !tgt->has_type()) {
                            // Propagate from source to target.
                            auto* mn = graph_->node_mut(edge.target);
                            if (mn) {
                                mn->type = unifier_->apply_substitutions(node->type);
                                changed = true;
                            }
                        }
                        else if (!node->has_type() && tgt->has_type()) {
                            // Propagate from target back to source.
                            auto* mn = graph_->node_mut(nid);
                            if (mn) {
                                mn->type = unifier_->apply_substitutions(tgt->type);
                                changed = true;
                            }
                        }
                    }
                    return changed;
                }

                semantic_inference_graph* graph_ = nullptr;
                semantic_type_unifier* unifier_ = nullptr;
                types::semantic_type_registry* registry_ = nullptr;
            };
        } // namespace type_rules
    } // namespace semantic
} // namespace lithe
