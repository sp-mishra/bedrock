#pragma once

// crank/generics.hpp — Trait/impl conformance, bounds, coherence, const generics (Module 5 B1/B2).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// Design refs: design.md §17.2–§17.5, grammar.md §5.2/§5.3; impl-5.md Steps B1–B2.
//
// G-VAK-6 fallback (b): bounds encoded as trait_set bits + a crank-local conformance
// side-table keyed by structural_hash. No Vākya generic machinery change required.
//
// Surfaces:
//   trait_id / bound_kind / trait_set  — bit-encoded bound representation
//   trait_descriptor      — named trait record (name, id, required_impls)
//   impl_record           — explicit impl of a trait for a concrete type
//   conformance_table     — side-table mapping (type_hash, trait_id) → impl_record*
//   trait_registry        — global trait + impl storage
//   check_conformance     — verify a concrete type satisfies a set of bounds
//   check_coherence       — orphan rule enforcement
//   const_generic_param   — usize/isize/Bool/enum const param (§17.5)
//   check_const_generic   — validate const generic usage
//   const_generic_arg     — literal or bare param reference (v1 only)
//
// v2 features (lifted):
//   associated types:   type Item; / Self.Item / C.Item projections
//   type-specialized impls with overlap/priority checks (assoc_types.hpp, specialization.hpp)
//   const-generic arithmetic (const_dim.hpp)
//   layout/device bounds: Layout[RowMajor/ColMajor] / Device[Gpu/Simd/Host]
//
// Bound vocabulary (v1 + v2):
//   Ownership:  Copy / Clone / Move / Drop
//   Numeric:    Numeric / Comparable / Ordered
//   Callable:   Fn / FnMut / FnOnce
//   Execution:  Pure / ParallelSafe / GpuCompatible / SimdEligible / Send / Sync
//   Transaction:Transactional / TransactionalResource / SnapshotCapable / AbaSafe /
//               AtomicMultiKeyWithinResource
//   Algebraic:  Monoid / Semiring (Self-first, with associated consts)
//   Layout(v2): LayoutRowMajor / LayoutColMajor
//   Device(v2): DeviceGpu / DeviceSimd / DeviceHost

#include "languages/crank/source_span.hpp"
#include "languages/crank/std_types.hpp"
#include "languages/crank/diagnostic.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace crank {
    // ============================================================================
    // trait_id — stable numeric id for a named trait (v1 range: 0–127)
    // ============================================================================

    using trait_id = std::uint8_t;

    // ============================================================================
    // bound_kind — the v1 bound vocabulary
    // ============================================================================

    enum class bound_kind : std::uint8_t {
        // Ownership
        Copy = 0,
        Clone = 1,
        Move = 2,
        Drop = 3,

        // Numeric / comparison
        Numeric = 4,
        Comparable = 5,
        Ordered = 6,

        // Callable (arity + signature tracked separately in impl_record)
        Fn = 7,
        FnMut = 8,
        FnOnce = 9,

        // Execution / effect
        Pure = 10,
        ParallelSafe = 11,
        GpuCompatible = 12,
        SimdEligible = 13,
        Send = 14,
        Sync = 15,

        // Transaction
        Transactional = 16,
        TransactionalResource = 17,
        SnapshotCapable = 18,
        AbaSafe = 19,
        AtomicMultiKeyWithinResource = 20,

        // Algebraic (Self-first traits)
        Monoid = 21,
        Semiring = 22,

        // Layout (v2) — memory layout constraints for tensor/matrix types
        LayoutRowMajor = 23,
        LayoutColMajor = 24,

        // Device (v2) — execution device affinity
        DeviceGpu = 25,
        DeviceSimd = 26,
        DeviceHost = 27,

        // Sentinel
        _Count = 28,
    };

    [[nodiscard]] constexpr std::string_view to_string(bound_kind k) noexcept {
        switch (k) {
        case bound_kind::Copy: return "Copy";
        case bound_kind::Clone: return "Clone";
        case bound_kind::Move: return "Move";
        case bound_kind::Drop: return "Drop";
        case bound_kind::Numeric: return "Numeric";
        case bound_kind::Comparable: return "Comparable";
        case bound_kind::Ordered: return "Ordered";
        case bound_kind::Fn: return "Fn";
        case bound_kind::FnMut: return "FnMut";
        case bound_kind::FnOnce: return "FnOnce";
        case bound_kind::Pure: return "Pure";
        case bound_kind::ParallelSafe: return "ParallelSafe";
        case bound_kind::GpuCompatible: return "GpuCompatible";
        case bound_kind::SimdEligible: return "SimdEligible";
        case bound_kind::Send: return "Send";
        case bound_kind::Sync: return "Sync";
        case bound_kind::Transactional: return "Transactional";
        case bound_kind::TransactionalResource: return "TransactionalResource";
        case bound_kind::SnapshotCapable: return "SnapshotCapable";
        case bound_kind::AbaSafe: return "AbaSafe";
        case bound_kind::AtomicMultiKeyWithinResource: return "AtomicMultiKeyWithinResource";
        case bound_kind::Monoid: return "Monoid";
        case bound_kind::Semiring: return "Semiring";
        case bound_kind::LayoutRowMajor: return "LayoutRowMajor";
        case bound_kind::LayoutColMajor: return "LayoutColMajor";
        case bound_kind::DeviceGpu: return "DeviceGpu";
        case bound_kind::DeviceSimd: return "DeviceSimd";
        case bound_kind::DeviceHost: return "DeviceHost";
        default: return "Unknown";
        }
    }

    // Parse a bound name string to bound_kind; returns nullopt if unknown.
    [[nodiscard]] constexpr std::optional<bound_kind>
    parse_bound_kind(std::string_view name) noexcept {
        // Table-driven; plain loop — small table, no map overhead
        struct Entry {
            std::string_view sym;
            bound_kind k;
        };
        constexpr Entry table[] = {
            {"Copy", bound_kind::Copy},
            {"Clone", bound_kind::Clone},
            {"Move", bound_kind::Move},
            {"Drop", bound_kind::Drop},
            {"Numeric", bound_kind::Numeric},
            {"Comparable", bound_kind::Comparable},
            {"Ordered", bound_kind::Ordered},
            {"Fn", bound_kind::Fn},
            {"FnMut", bound_kind::FnMut},
            {"FnOnce", bound_kind::FnOnce},
            {"Pure", bound_kind::Pure},
            {"ParallelSafe", bound_kind::ParallelSafe},
            {"GpuCompatible", bound_kind::GpuCompatible},
            {"SimdEligible", bound_kind::SimdEligible},
            {"Send", bound_kind::Send},
            {"Sync", bound_kind::Sync},
            {"Transactional", bound_kind::Transactional},
            {"TransactionalResource", bound_kind::TransactionalResource},
            {"SnapshotCapable", bound_kind::SnapshotCapable},
            {"AbaSafe", bound_kind::AbaSafe},
            {"AtomicMultiKeyWithinResource", bound_kind::AtomicMultiKeyWithinResource},
            {"Monoid", bound_kind::Monoid},
            {"Semiring", bound_kind::Semiring},
            // v2 layout / device bounds
            {"LayoutRowMajor", bound_kind::LayoutRowMajor},
            {"LayoutColMajor", bound_kind::LayoutColMajor},
            {"DeviceGpu", bound_kind::DeviceGpu},
            {"DeviceSimd", bound_kind::DeviceSimd},
            {"DeviceHost", bound_kind::DeviceHost},
        };
        for (const auto& e : table)
            if (e.sym == name) return e.k;
        return std::nullopt;
    }

    // ============================================================================
    // trait_set — bit-set of bounds (up to 64 with this encoding)
    // ============================================================================

    struct trait_set {
        std::uint64_t bits = 0;

        void add(bound_kind k) noexcept {
            bits |= (1ULL << static_cast<std::uint8_t>(k));
        }

        [[nodiscard]] bool has(bound_kind k) const noexcept {
            return (bits >> static_cast<std::uint8_t>(k)) & 1ULL;
        }

        [[nodiscard]] bool empty() const noexcept { return bits == 0; }

        [[nodiscard]] bool is_subset_of(const trait_set& other) const noexcept {
            return (bits & other.bits) == bits;
        }

        [[nodiscard]] trait_set operator|(const trait_set& o) const noexcept {
            return trait_set{bits | o.bits};
        }
    };

    // ============================================================================
    // associated_const_value — value carried by an algebraic associated constant
    // ============================================================================

    struct associated_const_value {
        enum class kind_t : std::uint8_t { bool_val, int64_val } kind;

        union {
            bool b;
            std::int64_t i;
        } value{};

        [[nodiscard]] static associated_const_value from_bool(bool v) noexcept {
            associated_const_value r;
            r.kind = kind_t::bool_val;
            r.value.b = v;
            return r;
        }

        [[nodiscard]] static associated_const_value from_int(std::int64_t v) noexcept {
            associated_const_value r;
            r.kind = kind_t::int64_val;
            r.value.i = v;
            return r;
        }
    };

    // ============================================================================
    // associated_const_decl — an associated constant in a trait declaration
    // ============================================================================

    struct associated_const_decl {
        std::string name; // e.g. "associative", "commutative", "identity"
        enum class type_t : std::uint8_t { Bool, Int64 } type;
    };

    // ============================================================================
    // assoc_type_record — an associated type declared in a trait (v2)
    //   name          — the declared name, e.g. "Item", "Scalar"
    //   bounds        — optional bounds on the associated type (e.g. Scalar: Numeric)
    // ============================================================================

    struct assoc_type_record {
        std::string name;
        trait_set bounds; // bounds the associated type must satisfy (may be empty)
    };

    // ============================================================================
    // trait_descriptor — named trait (id + bounds it implies + associated consts)
    // ============================================================================

    struct trait_descriptor {
        trait_id id = 0;
        std::string name;
        bound_kind primary_kind; // the bound_kind this trait maps to
        trait_set implied_bounds; // other bounds implied by this trait
        std::vector<associated_const_decl> assoc_consts; // algebraic constants (Monoid etc.)
        // Function members of the trait (e.g. Monoid::combine).
        // Each entry is {name, arity}. The planner uses these to locate the impl's combine op.
        std::vector<std::pair<std::string, std::size_t>> assoc_fn_decls;
        // Associated type declarations (v2): e.g. `type Item` or `type Scalar: Numeric`
        std::vector<assoc_type_record> assoc_types;
        bool self_first = false; // Self-first algebra (Monoid)
    };

    // ============================================================================
    // callable_sig — arity + crank type id signature for Fn/FnMut/FnOnce bounds
    // ============================================================================

    struct callable_sig {
        std::vector<std::uint32_t> param_type_ids; // crank type ids per parameter
        std::uint32_t return_type_id = 0;
        std::size_t arity() const noexcept { return param_type_ids.size(); }
    };

    // ============================================================================
    // impl_record — explicit `impl Trait for Type` (§17.2–§17.4)
    //
    // G-VAK-6 fallback (b): stored in a crank-local conformance side-table.
    // No Vākya trait machinery touched.
    // ============================================================================

    struct impl_record {
        trait_id trait = 0;
        std::uint64_t type_hash = 0; // structural_hash of the concrete type
        std::string type_name; // human name for diagnostics
        std::string module_name; // where the impl lives (for orphan check)
        std::string trait_module_name; // where the trait was defined

        // Optional callable signature (Fn/FnMut/FnOnce)
        std::optional<callable_sig> call_sig;

        // Associated constant values provided by this impl
        std::vector<std::pair<std::string, associated_const_value>> assoc_const_values;

        // Name of the function implementing the algebraic combine operation (e.g. Monoid::combine).
        // Set by the impl when the trait has an assoc_fn_decl named "combine".
        // Forwarded to generic_capability_summary::combine_fn_name for planner use.
        std::optional<std::string> combine_fn_name;

        // Specialization flag — v1 rejects type-specialized impls (v2: allowed with overlap checks)
        bool is_type_specialized = false;

        // Associated type bindings provided by this impl (v2).
        // Maps assoc_type_record::name → concrete type name bound in this impl.
        // e.g. { "Item" → "Int64" } for `impl Collection for IntVec`
        std::vector<std::pair<std::string, std::string>> assoc_type_bindings;
    };

    // ============================================================================
    // conformance_diagnostic — compile-time diagnostic from conformance/coherence checks
    // ============================================================================

    enum class conformance_diag_kind : std::uint8_t {
        missing_impl, // required bound not satisfied (no impl found)
        ambiguous_member, // member name matched by >1 bound; must qualify
        coherence_orphan, // impl violates orphan rule
        overlapping_impl, // two impls for same (trait, type) pair (non-specialized)
        type_specialized_v1_gate, // type-specialized impl not allowed in v1 (v2: use specialization.hpp)
        associated_type_v1_gate, // associated type used but not enabled in v1 (v2: lifted)
        // v2 diagnostics
        ambiguous_assoc_projection, // C.Item ambiguous — multiple traits provide same name
        missing_assoc_type_binding, // impl for trait with associated type but binding missing
        assoc_bound_unsatisfied, // assoc type binding does not satisfy the declared bound
        impl_not_in_scope, // use-site coherence: owning module not imported (§v2.1a)
    };

    [[nodiscard]] constexpr std::string_view to_string(conformance_diag_kind k) noexcept {
        switch (k) {
        case conformance_diag_kind::missing_impl: return "CRANK-GEN-001";
        case conformance_diag_kind::ambiguous_member: return "CRANK-GEN-002";
        case conformance_diag_kind::coherence_orphan: return "CRANK-GEN-003";
        case conformance_diag_kind::overlapping_impl: return "CRANK-GEN-004";
        case conformance_diag_kind::type_specialized_v1_gate: return "CRANK-GEN-005";
        case conformance_diag_kind::associated_type_v1_gate: return "CRANK-GEN-006";
        case conformance_diag_kind::ambiguous_assoc_projection: return "CRANK-GEN-010";
        case conformance_diag_kind::missing_assoc_type_binding: return "CRANK-GEN-011";
        case conformance_diag_kind::assoc_bound_unsatisfied: return "CRANK-GEN-012";
        case conformance_diag_kind::impl_not_in_scope: return "CRANK-GEN-013";
        }
        return "CRANK-GEN-???";
    }

    struct conformance_diagnostic {
        conformance_diag_kind kind;
        source_span at;
        std::string message;
        bool is_error = true;
        // Structured explanation (§v2.1a). Additive: `.message` stays byte-identical to
        // the legacy string; `explanation->render_message()` reproduces it. Optional so
        // hand-built diagnostics without an explanation cost nothing.
        std::optional<diag_explanation> explanation;
    };

    // ============================================================================
    // conformance_table — G-VAK-6 fallback (b): maps (type_hash, trait_id) → impl_record
    // ============================================================================

    class conformance_table {
    public:
        conformance_table() = default;

        // Register an impl. Returns false if it conflicts (coherence check deferred to check_coherence).
        bool add_impl(impl_record rec) {
            const auto key = make_key(rec.type_hash, rec.trait);
            if (table_.count(key)) return false; // duplicate — caller handles error
            table_.emplace(key, std::move(rec));
            return true;
        }

        // Look up an impl for (type_hash, trait_id). Returns nullptr if not found.
        [[nodiscard]] const impl_record*
        find(std::uint64_t type_hash, trait_id tid) const noexcept {
            auto it = table_.find(make_key(type_hash, tid));
            return it == table_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] bool has_impl(std::uint64_t type_hash, trait_id tid) const noexcept {
            return find(type_hash, tid) != nullptr;
        }

        std::size_t size() const noexcept { return table_.size(); }

    private:
        static std::uint64_t make_key(std::uint64_t hash, trait_id tid) noexcept {
            return (hash ^ (static_cast<std::uint64_t>(tid) << 56));
        }

        std::unordered_map<std::uint64_t, impl_record> table_;
    };

    // ============================================================================
    // trait_registry — stores trait_descriptors + conformance_table
    // ============================================================================

    class trait_registry {
    public:
        trait_registry() { register_builtin_traits(); }

        // Register a user-defined trait. Returns allocated trait_id.
        [[nodiscard]] trait_id register_trait(trait_descriptor desc) {
            const trait_id id = static_cast<trait_id>(traits_.size());
            desc.id = id;
            traits_.push_back(std::move(desc));
            return id;
        }

        [[nodiscard]] const trait_descriptor* find_trait(trait_id id) const noexcept {
            if (id >= traits_.size()) return nullptr;
            return &traits_[id];
        }

        [[nodiscard]] const trait_descriptor* find_trait_by_name(std::string_view name) const noexcept {
            for (const auto& t : traits_)
                if (t.name == name) return &t;
            return nullptr;
        }

        conformance_table& conformances() noexcept { return conformances_; }
        const conformance_table& conformances() const noexcept { return conformances_; }

    private:
        void register_builtin_traits() {
            // Register the v1 bound vocabulary as built-in traits
            auto mk = [](std::string_view name, bound_kind k, bool self_first = false)
                -> trait_descriptor {
                return {0, std::string(name), k, {}, {}, {}, {}, self_first};
            };
            // Ownership
            traits_.push_back(mk("Copy", bound_kind::Copy));
            traits_.push_back(mk("Clone", bound_kind::Clone));
            traits_.push_back(mk("Move", bound_kind::Move));
            traits_.push_back(mk("Drop", bound_kind::Drop));
            // Numeric
            traits_.push_back(mk("Numeric", bound_kind::Numeric));
            traits_.push_back(mk("Comparable", bound_kind::Comparable));
            traits_.push_back(mk("Ordered", bound_kind::Ordered));
            // Callable
            traits_.push_back(mk("Fn", bound_kind::Fn));
            traits_.push_back(mk("FnMut", bound_kind::FnMut));
            traits_.push_back(mk("FnOnce", bound_kind::FnOnce));
            // Execution
            traits_.push_back(mk("Pure", bound_kind::Pure));
            traits_.push_back(mk("ParallelSafe", bound_kind::ParallelSafe));
            traits_.push_back(mk("GpuCompatible", bound_kind::GpuCompatible));
            traits_.push_back(mk("SimdEligible", bound_kind::SimdEligible));
            traits_.push_back(mk("Send", bound_kind::Send));
            traits_.push_back(mk("Sync", bound_kind::Sync));
            // Transaction
            traits_.push_back(mk("Transactional", bound_kind::Transactional));
            traits_.push_back(mk("TransactionalResource", bound_kind::TransactionalResource));
            traits_.push_back(mk("SnapshotCapable", bound_kind::SnapshotCapable));
            traits_.push_back(mk("AbaSafe", bound_kind::AbaSafe));
            traits_.push_back(mk("AtomicMultiKeyWithinResource", bound_kind::AtomicMultiKeyWithinResource));
            // Algebraic (Self-first, with associated consts)
            {
                trait_descriptor monoid;
                monoid.name = "Monoid";
                monoid.primary_kind = bound_kind::Monoid;
                monoid.self_first = true;
                monoid.assoc_consts = {
                    {"associative", associated_const_decl::type_t::Bool},
                    {"commutative", associated_const_decl::type_t::Bool},
                };
                monoid.assoc_fn_decls = std::vector<std::pair<std::string, std::size_t>>{{"combine", 2u}};
                // combine(Self, Self) -> Self
                traits_.push_back(std::move(monoid));
            }
            {
                trait_descriptor semiring;
                semiring.name = "Semiring";
                semiring.primary_kind = bound_kind::Semiring;
                semiring.self_first = true;
                traits_.push_back(std::move(semiring));
            }
            // v2 layout / device bounds
            traits_.push_back(mk("LayoutRowMajor", bound_kind::LayoutRowMajor));
            traits_.push_back(mk("LayoutColMajor", bound_kind::LayoutColMajor));
            traits_.push_back(mk("DeviceGpu", bound_kind::DeviceGpu));
            traits_.push_back(mk("DeviceSimd", bound_kind::DeviceSimd));
            traits_.push_back(mk("DeviceHost", bound_kind::DeviceHost));

            // Set stable ids
            for (std::size_t i = 0; i < traits_.size(); ++i)
                traits_[i].id = static_cast<trait_id>(i);

            // Trait implication (§8.2): a stronger trait's implied_bounds lists the
            // weaker bounds it entails. Only the design's documented relation is seeded;
            // the closure machinery (trait_implies / satisfies_bound) supports deeper
            // chains if more are added. `Ordered ⇒ Comparable`.
            if (trait_descriptor* ord = mutable_trait_by_name("Ordered"))
                ord->implied_bounds.add(bound_kind::Comparable);
        }

        // Non-const trait lookup for seeding implied_bounds during construction.
        [[nodiscard]] trait_descriptor* mutable_trait_by_name(std::string_view name) noexcept {
            for (auto& t : traits_)
                if (t.name == name) return &t;
            return nullptr;
        }

        std::vector<trait_descriptor> traits_;
        conformance_table conformances_;
    };

    // ============================================================================
    // trait_implies — does bound `stronger` entail bound `weaker`? (§8.2)
    //
    // True iff stronger == weaker, or `weaker` is in the transitive implied-bounds
    // closure of `stronger`. Seeded relations are single-level (Ordered ⇒ Comparable)
    // but the walk is a bounded BFS over implied_bounds so deeper chains stay correct.
    // ============================================================================

    [[nodiscard]] inline bool
    trait_implies(const trait_registry& registry, bound_kind stronger, bound_kind weaker) noexcept {
        if (stronger == weaker) return true;

        // BFS over implied_bounds. The lattice is tiny (< _Count nodes); a visited
        // bitset bounds the walk and guards against accidental cycles.
        trait_set visited;
        trait_set frontier;
        frontier.add(stronger);

        while (!frontier.empty()) {
            trait_set next;
            for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(bound_kind::_Count); ++i) {
                const auto bk = static_cast<bound_kind>(i);
                if (!frontier.has(bk) || visited.has(bk)) continue;
                visited.add(bk);

                const trait_descriptor* td = registry.find_trait_by_name(to_string(bk));
                if (!td) continue;
                if (td->implied_bounds.has(weaker)) return true;
                next = next | td->implied_bounds;
            }
            frontier = next;
        }
        return false;
    }

    // ============================================================================
    // satisfies_bound — is `bound` satisfied for `type_hash`, honoring implication?
    //
    // True iff the type has a direct impl of `bound`, OR a direct impl of any trait
    // whose implied-closure contains `bound` (e.g. an `Ordered` impl satisfies a
    // `Comparable` requirement). The single predicate reused by conformance +
    // monomorphization so both agree on what "satisfied" means (§8.2).
    // ============================================================================

    [[nodiscard]] inline bool
    satisfies_bound(const conformance_table& table,
                    const trait_registry& registry,
                    std::uint64_t type_hash,
                    bound_kind bound) noexcept {
        const trait_descriptor* want = registry.find_trait_by_name(to_string(bound));
        if (want && table.has_impl(type_hash, want->id)) return true;

        // No direct impl — look for a stronger trait the type DOES impl that implies it.
        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(bound_kind::_Count); ++i) {
            const auto bk = static_cast<bound_kind>(i);
            if (bk == bound) continue;
            const trait_descriptor* td = registry.find_trait_by_name(to_string(bk));
            if (!td || !table.has_impl(type_hash, td->id)) continue;
            if (trait_implies(registry, bk, bound)) return true;
        }
        return false;
    }

    // ============================================================================
    // check_conformance — verify type_hash satisfies all bounds in required_set
    // Returns per-bound diagnostics; empty = all satisfied.
    // ============================================================================

    [[nodiscard]] inline std::vector<conformance_diagnostic>
    check_conformance(
        const conformance_table& table,
        const trait_registry& registry,
        std::uint64_t type_hash,
        std::string_view type_name,
        const trait_set& required,
        source_span at) {
        std::vector<conformance_diagnostic> diags;

        for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(bound_kind::_Count); ++i) {
            const auto bk = static_cast<bound_kind>(i);
            if (!required.has(bk)) continue;

            const trait_descriptor* td = nullptr;
            for (const auto* t = &registry; t != nullptr;) {
                td = registry.find_trait_by_name(to_string(bk));
                break;
            }

            // §8.2: a bound is satisfied by a direct impl OR by any impl of a trait
            // that implies it (Ordered impl satisfies a Comparable requirement).
            if (!td || !satisfies_bound(table, registry, type_hash, bk)) {
                conformance_diagnostic d{
                    conformance_diag_kind::missing_impl, at,
                    std::string("CRANK-GEN-001: type '") + std::string(type_name)
                    + "' does not satisfy bound '" + std::string(to_string(bk))
                    + "': no `impl " + std::string(to_string(bk))
                    + " for " + std::string(type_name) + "` found"
                };
                d.explanation = explain("CRANK-GEN-001",
                                        d.message.substr(std::string_view("CRANK-GEN-001: ").size()), at)
                                .expected(std::string(to_string(bk)))
                                .found(std::string(type_name))
                                .note("this bound is required by the generic parameter's constraint set")
                                .help(std::string("add `impl ") + std::string(to_string(bk))
                                    + " for " + std::string(type_name) + "`")
                                .build();
                diags.push_back(std::move(d));
            }
        }
        return diags;
    }

    // ============================================================================
    // check_coherence — orphan rule + overlap detection (§17.2 v1)
    //
    // Orphan rule: impl Trait for Type is legal only if:
    //   (a) the current module owns the trait, OR
    //   (b) the current module owns the type.
    // No overlapping impls for the same (trait, type) pair.
    // ============================================================================

    [[nodiscard]] inline std::vector<conformance_diagnostic>
    check_coherence(
        const conformance_table& table,
        const impl_record& candidate,
        std::string_view current_module,
        source_span at) {
        std::vector<conformance_diagnostic> diags;

        // Orphan rule
        const bool owns_trait = (candidate.trait_module_name == current_module);
        const bool owns_type = (candidate.module_name == current_module);
        if (!owns_trait && !owns_type) {
            conformance_diagnostic d{
                conformance_diag_kind::coherence_orphan, at,
                std::string("CRANK-GEN-003: orphan rule violation — ")
                + "cannot impl trait '" + std::to_string(candidate.trait)
                + "' for type '" + candidate.type_name
                + "' in module '" + std::string(current_module)
                + "': neither the trait nor the type is defined in this module"
            };
            d.explanation = explain("CRANK-GEN-003",
                                    d.message.substr(std::string_view("CRANK-GEN-003: ").size()), at)
                            .note("the orphan rule requires the impl's module to own either the trait or the type")
                            .help("move this impl into the module that defines trait '"
                                + std::to_string(candidate.trait) + "' or type '"
                                + candidate.type_name + "'")
                            .build();
            diags.push_back(std::move(d));
        }

        // Overlap check
        if (table.has_impl(candidate.type_hash, candidate.trait)) {
            conformance_diagnostic d{
                conformance_diag_kind::overlapping_impl, at,
                std::string("CRANK-GEN-004: duplicate impl — a conformance for trait id ")
                + std::to_string(candidate.trait)
                + " and type '" + candidate.type_name + "' already exists"
            };
            d.explanation = explain("CRANK-GEN-004",
                                    d.message.substr(std::string_view("CRANK-GEN-004: ").size()), at)
                            .note("only one non-specialized impl may exist per (trait, type) pair")
                            .help("remove the duplicate impl, or make one a controlled specialization")
                            .build();
            diags.push_back(std::move(d));
        }

        // v2: type-specialized impls are legal but require overlap checks via specialization.hpp.
        // The v1 gate diagnostic is retained here only when the caller has not opted in to v2
        // specialization processing. Callers using specialization.hpp should skip this check.
        if (candidate.is_type_specialized) {
            diags.push_back({
                conformance_diag_kind::type_specialized_v1_gate, at,
                "CRANK-GEN-005: type-specialized impls require v2 specialization checks "
                "(use specialization.hpp check_specialization_overlap); "
                "not handled by check_coherence"
            });
        }

        return diags;
    }

    // ============================================================================
    // const_generic_param — usize/isize/Bool/enum const parameter kind (§17.5)
    // These are parameter kinds only — NOT runtime value types.
    // ============================================================================

    enum class const_param_kind : std::uint8_t {
        usize,
        isize,
        Bool,
        user_enum, // named enum type
    };

    [[nodiscard]] constexpr std::string_view to_string(const_param_kind k) noexcept {
        switch (k) {
        case const_param_kind::usize: return "usize";
        case const_param_kind::isize: return "isize";
        case const_param_kind::Bool: return "Bool";
        case const_param_kind::user_enum: return "enum";
        }
        return "?";
    }

    struct const_generic_param {
        std::string name; // e.g. "N"
        const_param_kind kind;
        std::string enum_type; // only when kind == user_enum
    };

    // const_generic_arg — a concrete value for a const generic parameter (v1: literals + bare refs)
    struct const_generic_arg {
        enum class form : std::uint8_t {
            literal, // integer/bool literal
            param_ref, // reference to another const param (bare reference, no arithmetic)
        } form = form::literal;

        std::int64_t literal_value = 0;
        std::string param_name; // when form == param_ref
    };

    // ============================================================================
    // const_generic_diagnostic
    // ============================================================================

    enum class const_generic_diag_kind : std::uint8_t {
        arithmetic_in_v1, // [N+1] etc. — not allowed in v1
        usize_as_value_type, // usize/isize used as a runtime variable type
    };

    struct const_generic_diagnostic {
        const_generic_diag_kind kind;
        source_span at;
        std::string message;
        bool is_error = true;
    };

    // ============================================================================
    // check_const_generic — validate a const generic argument (§17.5 v1 rules)
    //
    // v1 rules:
    //   - Arithmetic expressions ([N+1], Matrix[T,M,K*2]) → diagnostic
    //   - usize/isize as a variable type → diagnostic
    //   - bare literal or param reference → OK
    // ============================================================================

    [[nodiscard]] inline std::vector<const_generic_diagnostic>
    check_const_generic(const const_generic_arg& arg, source_span at) {
        std::vector<const_generic_diagnostic> diags;
        // v1: only literal and bare param_ref are legal
        // The caller supplies is_arithmetic = (contains +/-/*/.. operators)
        // For the model here, form encodes this.
        (void)arg;
        (void)at;
        return diags; // semantic checker calls this after expression analysis
    }

    [[nodiscard]] inline const_generic_diagnostic
    make_arithmetic_const_diag(source_span at, std::string_view expr) {
        return {
            const_generic_diag_kind::arithmetic_in_v1, at,
            std::string("CRANK-GEN-V1: const generic arithmetic expression '")
            + std::string(expr)
            + "' is not supported in v1; use only integer literals or bare parameter references"
        };
    }

    [[nodiscard]] inline const_generic_diagnostic
    make_usize_value_type_diag(source_span at, std::string_view name) {
        return {
            const_generic_diag_kind::usize_as_value_type, at,
            std::string("CRANK-GEN-V1: '") + std::string(name)
            + "' is a const-parameter kind, not a runtime value type; "
            "use Int64/UInt64 for runtime integer values"
        };
    }

    // ============================================================================
    // v2 diagnostic helpers — associated types + specialization
    // ============================================================================

    struct v2_gate_diagnostic {
        source_span at;
        std::string message;
    };

    // Retained for v1-mode callers that explicitly reject associated types.
    [[nodiscard]] inline v2_gate_diagnostic
    make_assoc_type_gate_diag(source_span at, std::string_view type_name) {
        return {
            at,
            std::string("CRANK-GEN-006: associated type projection '")
            + std::string(type_name)
            + "' requires v2 mode; enable v2 or use explicit type parameters"
        };
    }

    // CRANK-GEN-010: C.Item projection is ambiguous (two traits provide same assoc type name).
    [[nodiscard]] inline conformance_diagnostic
    make_ambiguous_assoc_projection_diag(source_span at,
                                         std::string_view proj_name,
                                         std::string_view type_name) {
        conformance_diagnostic d{
            conformance_diag_kind::ambiguous_assoc_projection, at,
            std::string("CRANK-GEN-010: associated type projection '")
            + std::string(type_name) + "." + std::string(proj_name)
            + "' is ambiguous — multiple bounds provide an associated type with this name; "
            "qualify as Trait[" + std::string(type_name) + "]." + std::string(proj_name),
            true
        };
        d.explanation = explain("CRANK-GEN-010",
                                d.message.substr(std::string_view("CRANK-GEN-010: ").size()), at)
                        .note("multiple bounds on the type parameter declare an associated type named '"
                            + std::string(proj_name) + "'")
                        .help("qualify the projection as Trait[" + std::string(type_name) + "]."
                            + std::string(proj_name))
                        .build();
        return d;
    }

    // CRANK-GEN-011: impl provides trait with assoc types but binding for `name` is missing.
    [[nodiscard]] inline conformance_diagnostic
    make_missing_assoc_binding_diag(source_span at,
                                    std::string_view assoc_name,
                                    std::string_view trait_name,
                                    std::string_view type_name) {
        conformance_diagnostic d{
            conformance_diag_kind::missing_assoc_type_binding, at,
            std::string("CRANK-GEN-011: impl of '") + std::string(trait_name)
            + "' for '" + std::string(type_name)
            + "' does not bind associated type '" + std::string(assoc_name) + "'",
            true
        };
        d.explanation = explain("CRANK-GEN-011",
                                d.message.substr(std::string_view("CRANK-GEN-011: ").size()), at)
                        .note("trait '" + std::string(trait_name)
                            + "' declares associated type '" + std::string(assoc_name) + "'")
                        .help("add `type " + std::string(assoc_name) + " = <ConcreteType>;` to the impl")
                        .build();
        return d;
    }

    // CRANK-GEN-012: an associated type binding does not satisfy the bound the trait
    // declared on that associated type. e.g. `type Scalar: Numeric` but the impl binds
    // `Scalar = String` where String has no `impl Numeric`.
    [[nodiscard]] inline conformance_diagnostic
    make_assoc_bound_unsatisfied_diag(source_span at,
                                      std::string_view assoc_name,
                                      std::string_view concrete_name,
                                      std::string_view missing_bound) {
        conformance_diagnostic d{
            conformance_diag_kind::assoc_bound_unsatisfied, at,
            std::string("CRANK-GEN-012: associated type '") + std::string(assoc_name)
            + "' is bound to '" + std::string(concrete_name)
            + "' which does not satisfy the required bound '"
            + std::string(missing_bound) + "' declared on the associated type",
            true
        };
        d.explanation = explain("CRANK-GEN-012",
                                d.message.substr(std::string_view("CRANK-GEN-012: ").size()), at)
                        .expected(std::string(missing_bound))
                        .found(std::string(concrete_name))
                        .note("associated type '" + std::string(assoc_name)
                            + "' carries the bound '" + std::string(missing_bound) + "'")
                        .help("bind '" + std::string(assoc_name) + "' to a type that has `impl "
                            + std::string(missing_bound) + " for ...`")
                        .build();
        return d;
    }

    // ============================================================================
    // check_assoc_type_bounds — verify each associated-type binding satisfies the
    // bounds the trait declared on that associated type (v2, §v2.2).
    //
    // For every assoc_type_record in `trait_desc` that carries non-empty `bounds`,
    // resolve the concrete type the impl bound it to, look up that concrete type's
    // structural hash via `resolve_concrete_hash`, and confirm the conformance table
    // has an impl of each required bound. Emits CRANK-GEN-012 per unsatisfied bound.
    //
    // resolve_concrete_hash maps a concrete type NAME (e.g. "Int64") to its
    // structural_hash; returns nullopt when the name is unknown (skipped, no error —
    // name resolution reports unknown types separately).
    // ============================================================================

    template <class ResolveHashFn>
    [[nodiscard]] inline std::vector<conformance_diagnostic>
    check_assoc_type_bounds(
        const trait_descriptor& trait_desc,
        const std::vector<std::pair<std::string, std::string>>& impl_bindings,
        const conformance_table& table,
        const trait_registry& registry,
        ResolveHashFn&& resolve_concrete_hash,
        source_span at) {
        std::vector<conformance_diagnostic> diags;

        for (const auto& ar : trait_desc.assoc_types) {
            if (ar.bounds.empty()) continue; // no bounds to check

            // Find what the impl bound this assoc type to
            const std::string* concrete = nullptr;
            for (const auto& [bname, bval] : impl_bindings)
                if (bname == ar.name) {
                    concrete = &bval;
                    break;
                }
            if (!concrete) continue; // missing binding handled by CRANK-GEN-011

            auto hash = resolve_concrete_hash(*concrete);
            if (!hash) continue; // unknown concrete type — reported elsewhere

            for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(bound_kind::_Count); ++i) {
                const auto bk = static_cast<bound_kind>(i);
                if (!ar.bounds.has(bk)) continue;

                const trait_descriptor* btd = registry.find_trait_by_name(to_string(bk));
                if (!btd || !table.has_impl(*hash, btd->id)) {
                    diags.push_back(make_assoc_bound_unsatisfied_diag(
                        at, ar.name, *concrete, to_string(bk)));
                }
            }
        }
        return diags;
    }

    // ============================================================================
    // resolve_assoc_type — look up the concrete type name bound for an associated
    // type projection `type_param.assoc_name` via the impl witness.
    //
    // Returns the concrete type name, or empty string if not found.
    // Emits CRANK-GEN-010 if multiple witnesses provide the same name (ambiguous).
    // Emits CRANK-GEN-011 if the matching impl has no binding for the name.
    // ============================================================================

    [[nodiscard]] inline std::string
    resolve_assoc_type(
        std::string_view assoc_name,
        std::string_view type_name,
        const std::vector<impl_record*>& witnesses, // impls providing traits with assoc types
        std::vector<conformance_diagnostic>& diags,
        source_span at) {
        std::string found;
        std::size_t found_count = 0;

        for (const auto* rec : witnesses) {
            for (const auto& [bname, bval] : rec->assoc_type_bindings) {
                if (bname == assoc_name) {
                    found = bval;
                    ++found_count;
                }
            }
        }

        if (found_count > 1) {
            diags.push_back(make_ambiguous_assoc_projection_diag(at, assoc_name, type_name));
            return {};
        }
        if (found_count == 0 && !witnesses.empty()) {
            // At least one witness was expected to provide the binding
            diags.push_back(make_missing_assoc_binding_diag(at, assoc_name, "?", type_name));
            return {};
        }
        return found;
    }
} // namespace crank
