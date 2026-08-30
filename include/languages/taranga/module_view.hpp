#pragma once

// taranga/module_view.hpp — Read-only structured view over a taranga_ir_module.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// build_ast emits a flat ir_module: nodes in a vector, children in a sidecar.
// Every later band (validate, ssa_build, lower) wants to ask module-shaped
// questions — "the functions", "this func's body instructions", "the type at
// index k" — without re-deriving the layout each time. module_view is that
// typed lens: a non-owning wrapper that answers those questions in terms of
// taranga_kind, leaving the ir_module itself untouched.
//
// It is intentionally cheap (a pointer + cached root scan) and const-correct:
// nothing here mutates the module, so a view can be freely copied and passed by
// value. Index resolution (funcidx → func node) follows Wasm's index-space rules:
// imported functions precede defined ones, so a raw funcidx must skip imports.

#include "languages/taranga/build_ast.hpp"
#include "languages/taranga/wasm_types.hpp"

#include "languages/generic/ir/node.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace taranga {

    class module_view {
    public:
        explicit module_view(const taranga_module& m) noexcept : mod_(&m) { index(); }

        [[nodiscard]] const taranga_ir_module& ir() const noexcept { return mod_->ir; }
        [[nodiscard]] const taranga_module& module() const noexcept { return *mod_; }

        // Node access ---------------------------------------------------------
        [[nodiscard]] const lang::ir_node<taranga_kind, taranga_node_ext>&
        node(lang::ir_node_id id) const noexcept { return mod_->ir[id]; }

        [[nodiscard]] std::span<const lang::ir_node_id>
        children(lang::ir_node_id id) const noexcept { return mod_->ir.children(id); }

        [[nodiscard]] lang::ir_node_id root() const noexcept { return mod_->ir.root(); }

        [[nodiscard]] taranga_kind kind_of(lang::ir_node_id id) const noexcept {
            return mod_->ir[id].kind;
        }

        // Section views -------------------------------------------------------
        [[nodiscard]] std::span<const lang::ir_node_id> functions() const noexcept {
            return {functions_.data(), functions_.size()};
        }
        [[nodiscard]] std::span<const lang::ir_node_id> imports() const noexcept {
            return {imports_.data(), imports_.size()};
        }
        [[nodiscard]] std::span<const lang::ir_node_id> exports() const noexcept {
            return {exports_.data(), exports_.size()};
        }
        [[nodiscard]] std::span<const lang::ir_node_id> globals() const noexcept {
            return {globals_.data(), globals_.size()};
        }
        [[nodiscard]] std::span<const lang::ir_node_id> memories() const noexcept {
            return {memories_.data(), memories_.size()};
        }
        [[nodiscard]] std::span<const lang::ir_node_id> tables() const noexcept {
            return {tables_.data(), tables_.size()};
        }

        [[nodiscard]] std::span<const func_type> types() const noexcept {
            return {mod_->types.data(), mod_->types.size()};
        }

        [[nodiscard]] std::uint32_t defined_function_count() const noexcept {
            return static_cast<std::uint32_t>(functions_.size());
        }
        [[nodiscard]] std::uint32_t imported_function_count() const noexcept {
            return imported_functions_;
        }

        // Function body: the first (only) `body` child of a func node, or null.
        [[nodiscard]] lang::ir_node_id body_of(lang::ir_node_id func) const noexcept {
            for (auto c : children(func))
                if (kind_of(c) == taranga_kind::body) return c;
            return lang::k_null_ir;
        }

        // Resolve the func_type of a defined function node via its recorded typeidx
        // (build_ast stores typeidx in func.ext.immediate). nullptr if out of range.
        [[nodiscard]] const func_type* signature_of(lang::ir_node_id func) const noexcept {
            const auto& n = node(func);
            const std::uint32_t ti = n.ext.immediate;
            if (ti >= mod_->types.size()) return nullptr;
            return &mod_->types[ti];
        }

    private:
        void index() {
            const auto r = mod_->ir.root();
            if (r == lang::k_null_ir) return;
            for (auto c : mod_->ir.children(r)) {
                switch (mod_->ir[c].kind) {
                case taranga_kind::func: functions_.push_back(c); break;
                case taranga_kind::import:
                    imports_.push_back(c);
                    // A function import ("func" head) grows the func index space.
                    if (mod_->ir[c].ext.head == "func") ++imported_functions_;
                    break;
                case taranga_kind::export_: exports_.push_back(c); break;
                case taranga_kind::global: globals_.push_back(c); break;
                case taranga_kind::memory: memories_.push_back(c); break;
                case taranga_kind::table: tables_.push_back(c); break;
                default: break;
                }
            }
        }

        const taranga_module* mod_;
        std::vector<lang::ir_node_id> functions_;
        std::vector<lang::ir_node_id> imports_;
        std::vector<lang::ir_node_id> exports_;
        std::vector<lang::ir_node_id> globals_;
        std::vector<lang::ir_node_id> memories_;
        std::vector<lang::ir_node_id> tables_;
        std::uint32_t imported_functions_ = 0;
    };

} // namespace taranga
