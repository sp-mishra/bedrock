#pragma once

// crank/view_registry.hpp — view descriptor registry (domain views).
//
// C++23, header-only, no virtual, no macros. Namespace: crank
//
// view_descriptor carries: stable_id, name_hash, backing_type_ref, generic_arity,
// backing_name, requires-predicate node ids, domain metadata, method table.
//
// Backed by containers::descriptor_registry<view_descriptor> — generic, reusable,
// not tensor-specific. Populated in the same pass as the conformance_table.
//
// method_entry: one method for a view; keyed by (method_name, generic_arg_hash).
// view_method_table: flat per-view method map.
//
// view_registry: global registry, one per compilation unit.
// view_domain_meta: compile-stage @sutra.* annotation fields.

#include "containers/descriptor_registry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace crank {
    // ============================================================================
    // view_domain_meta — compile-stage Sutra annotation fields on a view / method.
    //
    // Set by the annotation resolver when @sutra.domain/op/laws/affinity are present.
    // Ephemeral in v1 — steers Crank lowering + planner; dropped at IR trust boundary.
    // ============================================================================

    struct view_domain_meta {
        std::string domain_name; // @sutra.domain(name=…) — foundation: "scalar","tensor","tree","graph","data"
        std::string op_name; // @sutra.op(name=…) — e.g. "tensor.matmul"

        // @sutra.laws flags
        bool law_pure = false;
        bool law_deterministic = false;
        bool law_commutative = false;
        bool law_associative = false;

        // @sutra.affinity flags
        bool affinity_simd = false;
        bool affinity_gpu = false;
        bool affinity_dag = false;
        bool affinity_streaming = false;

        [[nodiscard]] bool has_domain() const noexcept { return !domain_name.empty(); }
        [[nodiscard]] bool has_op() const noexcept { return !op_name.empty(); }

        [[nodiscard]] bool fast_path_ok() const noexcept {
            // Pure + Deterministic → zero SMT, no Tarka round-trip (§4.5.2)
            return law_pure && law_deterministic;
        }
    };

    // ============================================================================
    // method_entry — one view method with its generic-arg signature + meta
    // ============================================================================

    struct method_entry {
        std::string method_name;
        std::uint64_t generic_arg_hash = 0; // hash of generic_args; 0 = non-specialized
        std::uint32_t fn_node_id = 0; // crank_node_id of the fn_node in the arena
        view_domain_meta meta; // resolved @sutra.* on this method
    };

    // ============================================================================
    // view_method_table — flat map (method_name + generic_arg_hash) → method_entry
    // ============================================================================

    class view_method_table {
    public:
        // Insert or overwrite a method entry.
        void insert(method_entry e) {
            const auto key = make_key(e.method_name, e.generic_arg_hash);
            entries_[key] = std::move(e);
        }

        // Look up the most specific entry: first try exact generic_arg_hash, then 0 (non-specialized).
        [[nodiscard]] const method_entry* find(std::string_view name,
                                               std::uint64_t generic_arg_hash = 0) const {
            auto it = entries_.find(make_key(name, generic_arg_hash));
            if (it != entries_.end()) return &it->second;
            // Fallback: non-specialized entry
            if (generic_arg_hash != 0) {
                it = entries_.find(make_key(name, 0));
                if (it != entries_.end()) return &it->second;
            }
            return nullptr;
        }

        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

        template <class Fn>
        void for_each(Fn&& fn) const {
            for (const auto& [k, v] : entries_) fn(v);
        }

    private:
        std::unordered_map<std::string, method_entry> entries_;

        [[nodiscard]] static std::string make_key(std::string_view name,
                                                  std::uint64_t hash) {
            return std::string(name) + "@" + std::to_string(hash);
        }
    };

    // ============================================================================
    // view_category — category tag for descriptor_registry indexing
    // ============================================================================

    enum class view_category : std::uint32_t {
        generic = 0, // standard view (non-specialized)
        specialized = 1, // shape-specialized view (v2-gated)
    };

    // ============================================================================
    // view_descriptor — one registered view (satisfies RegistrableDescriptor)
    //
    // stable_id: assigned at registration; serialization-stable.
    // name_hash: FNV-1a of the view's qualified name.
    // category:  view_category
    // ============================================================================

    struct view_descriptor {
        // RegistrableDescriptor requirements
        std::uint32_t stable_id = 0;
        std::uint64_t name_hash = 0;
        view_category category = view_category::generic;

        // View metadata
        std::string qualified_name; // "module::Tensor"
        std::string backing_name; // backing binding ("base")
        std::uint32_t backing_type_id = 0; // resolved source-type stable_id
        std::uint8_t generic_arity = 0; // number of generic params

        // requires-predicate node ids from the view_decl
        std::vector<std::uint32_t> requires_node_ids;

        // @sutra.* annotation metadata on the view decl itself
        view_domain_meta domain_meta;

        // Method table populated by impl … ViewName { … } passes
        view_method_table methods;
    };

    // ============================================================================
    // FNV-1a helper — same as desc_name_hash in containers/descriptor_registry.hpp
    // ============================================================================

    [[nodiscard]] constexpr std::uint64_t view_name_hash(std::string_view name) noexcept {
        constexpr std::uint64_t basis = 14695981039346656037ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;
        std::uint64_t h = basis;
        for (unsigned char c : name) {
            h ^= c;
            h *= prime;
        }
        return h;
    }

    // ============================================================================
    // view_registry — descriptor_registry<view_descriptor> alias
    // ============================================================================

    using view_registry = containers::descriptor_registry<view_descriptor>;
} // namespace crank
