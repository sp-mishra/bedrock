#pragma once

// =============================================================================
// lithe_ir/inspect/view.hpp — ir_view concept + per-family concrete views
//
// Namespace: lithe::ir::inspect
//
// ir_view concept: read-only, zero-erasure uniform access surface over any IR
// family.  Three concrete views:
//   graph_view         — over const lithe_graph_ir&
//   hl_mir_view        — over const lithe_hl_mir_ir&
//   physical_mir_view  — over const lithe_physical_mir_ir&
//
// any_ir_view — opt-in std::variant-backed erased view (cold boundary only).
//
// All views are non-owning (hold a single const pointer).  Construction O(1),
// no allocation.  No virtual on any walk path.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <concepts>
#include <string_view>
#include <utility>
#include <variant>

#include "../adapters/graph.hpp"        // lithe_graph_ir
#include "../adapters/hl_mir.hpp"       // lithe_hl_mir_ir
#include "../adapters/physical_mir.hpp" // lithe_physical_mir_ir
#include "handles.hpp"

namespace lithe::ir::inspect {
    // =============================================================================
    // ir_view concept — the uniform read-only observation surface (arch §6)
    // =============================================================================

    template <class V>
    concept ir_view = requires(const V& v, entity_ref e) {
        // Which family this view represents.
        { v.family() } -> std::same_as<ir_family>;
        // Which pipeline stage this wire form occupies.
        { v.stage_of() } -> std::same_as<lithe::ir::stage>;
        // Schema version of this wire form.
        { v.schema() } -> std::same_as<lithe::ir::schema_version>;
        // Number of ops / nodes / instructions in this view.
        { v.entity_count() } -> std::same_as<std::size_t>;
        // Number of blocks in this view.
        { v.block_count() } -> std::same_as<std::size_t>;
        // (domain, name) pair for the op/node identified by e.
        { v.opcode_name(e) } -> std::convertible_to<std::pair<std::string_view, std::string_view>>;
        // Canonical type string of the value/node identified by e.
        { v.type_string(e) } -> std::convertible_to<std::string_view>;
        // Lightweight structural validity check (no semantic analysis).
        { v.structurally_valid() } -> std::same_as<bool>;
    };

    // =============================================================================
    // graph_view — read-only view over a lithe_graph_ir
    // =============================================================================

    class graph_view {
    public:
        explicit graph_view(const adapters::lithe_graph_ir& g) noexcept : g_(&g) {}

        [[nodiscard]] ir_family family() const noexcept { return ir_family::graph; }
        [[nodiscard]] lithe::ir::stage stage_of() const noexcept { return g_->source_stage; }
        [[nodiscard]] lithe::ir::schema_version schema() const noexcept { return g_->schema; }

        [[nodiscard]] std::size_t entity_count() const noexcept { return g_->nodes.size(); }
        [[nodiscard]] std::size_t block_count() const noexcept { return 0; } // graph IR has no blocks

        [[nodiscard]] std::pair<std::string_view, std::string_view>
        opcode_name(entity_ref e) const noexcept {
            if (e.id < g_->nodes.size())
                return {g_->nodes[e.id].op_domain, g_->nodes[e.id].op_name};
            return {{}, {}};
        }

        [[nodiscard]] std::string_view type_string(entity_ref) const noexcept {
            // Graph IR carries literal kind, not a type string per node.
            return {};
        }

        [[nodiscard]] bool structurally_valid() const noexcept { return g_->structurally_valid(); }

    private:
        const adapters::lithe_graph_ir* g_;
    };

    static_assert(ir_view<graph_view>, "graph_view must satisfy ir_view");

    // =============================================================================
    // hl_mir_view — read-only view over a lithe_hl_mir_ir
    // =============================================================================

    class hl_mir_view {
    public:
        explicit hl_mir_view(const adapters::lithe_hl_mir_ir& h) noexcept : h_(&h) {}

        [[nodiscard]] ir_family family() const noexcept { return ir_family::hl_mir; }
        [[nodiscard]] lithe::ir::stage stage_of() const noexcept { return h_->source_stage; }
        [[nodiscard]] lithe::ir::schema_version schema() const noexcept { return h_->schema; }

        [[nodiscard]] std::size_t entity_count() const noexcept { return h_->ops.size(); }
        [[nodiscard]] std::size_t block_count() const noexcept { return h_->blocks.size(); }

        [[nodiscard]] std::pair<std::string_view, std::string_view>
        opcode_name(entity_ref e) const noexcept {
            if (e.id < h_->ops.size())
                return {h_->ops[e.id].domain, h_->ops[e.id].name};
            return {{}, {}};
        }

        // Returns the canonical type string of value e.id.
        [[nodiscard]] std::string_view type_string(entity_ref e) const noexcept {
            if (e.id < h_->values.size())
                return h_->values[e.id].type_str;
            return {};
        }

        [[nodiscard]] bool structurally_valid() const noexcept { return h_->valid(); }

    private:
        const adapters::lithe_hl_mir_ir* h_;
    };

    static_assert(ir_view<hl_mir_view>, "hl_mir_view must satisfy ir_view");

    // =============================================================================
    // physical_mir_view — read-only view over a lithe_physical_mir_ir
    // =============================================================================

    class physical_mir_view {
    public:
        explicit physical_mir_view(const adapters::lithe_physical_mir_ir& p) noexcept : p_(&p) {}

        [[nodiscard]] ir_family family() const noexcept { return ir_family::physical_mir; }
        [[nodiscard]] lithe::ir::stage stage_of() const noexcept { return p_->source_stage; }
        [[nodiscard]] lithe::ir::schema_version schema() const noexcept { return p_->schema; }

        [[nodiscard]] std::size_t entity_count() const noexcept { return p_->instrs.size(); }
        [[nodiscard]] std::size_t block_count() const noexcept { return p_->blocks.size(); }

        [[nodiscard]] std::pair<std::string_view, std::string_view>
        opcode_name(entity_ref e) const noexcept {
            if (e.id < p_->instrs.size())
                return {p_->instrs[e.id].op_domain, p_->instrs[e.id].op_name};
            return {{}, {}};
        }

        [[nodiscard]] std::string_view type_string(entity_ref) const noexcept {
            // Physical MIR uses wire_value_kind (enum), not a type string per entity.
            return {};
        }

        [[nodiscard]] bool structurally_valid() const noexcept { return p_->valid(); }

    private:
        const adapters::lithe_physical_mir_ir* p_;
    };

    static_assert(ir_view<physical_mir_view>, "physical_mir_view must satisfy ir_view");

    // =============================================================================
    // any_ir_view — opt-in erased view for cold tooling boundaries
    //
    // Backed by std::variant (closed 3-family set) — no vtable-per-call.
    // Dispatch via std::visit at the cold call site.  Use the concrete *_view
    // types on every hot/static path; reserve any_ir_view for heterogeneous
    // containers in generic tooling.
    // =============================================================================

    class any_ir_view {
        using variant_t = std::variant<graph_view, hl_mir_view, physical_mir_view>;

    public:
        // Implicit conversion from any concrete view.
        template <class V>
            requires std::same_as<V, graph_view>
            || std::same_as<V, hl_mir_view>
            || std::same_as<V, physical_mir_view>
        any_ir_view(V v) noexcept : v_(std::move(v)) {} // NOLINT(google-explicit-constructor)

        [[nodiscard]] ir_family family() const noexcept {
            return std::visit([](const auto& x) { return x.family(); }, v_);
        }

        [[nodiscard]] lithe::ir::stage stage_of() const noexcept {
            return std::visit([](const auto& x) { return x.stage_of(); }, v_);
        }

        [[nodiscard]] lithe::ir::schema_version schema() const noexcept {
            return std::visit([](const auto& x) { return x.schema(); }, v_);
        }

        [[nodiscard]] std::size_t entity_count() const noexcept {
            return std::visit([](const auto& x) { return x.entity_count(); }, v_);
        }

        [[nodiscard]] std::size_t block_count() const noexcept {
            return std::visit([](const auto& x) { return x.block_count(); }, v_);
        }

        [[nodiscard]] std::pair<std::string_view, std::string_view>
        opcode_name(entity_ref e) const noexcept {
            return std::visit([e](const auto& x) { return x.opcode_name(e); }, v_);
        }

        [[nodiscard]] std::string_view type_string(entity_ref e) const noexcept {
            return std::visit([e](const auto& x) { return x.type_string(e); }, v_);
        }

        [[nodiscard]] bool structurally_valid() const noexcept {
            return std::visit([](const auto& x) { return x.structurally_valid(); }, v_);
        }

        // Access to the underlying variant for advanced dispatch.
        [[nodiscard]] const variant_t& raw() const noexcept { return v_; }

    private:
        variant_t v_;
    };

    static_assert(ir_view<any_ir_view>, "any_ir_view must satisfy ir_view");
} // namespace lithe::ir::inspect
