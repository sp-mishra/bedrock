#pragma once

// taranga/validate.hpp — Wasm-level structural validator + proof token.
//
// C++23, header-only, no virtual, no macros.
// Namespace: taranga
//
// Validation here is the Wasm module-level well-formedness pass — the checks the
// spec's "Validation" chapter demands before a module may be instantiated, that
// Lithe's IR verifier cannot see because they are about *Wasm* index spaces and
// entity relationships, not about MIR structure. Lithe still verifies the lowered
// MIR after freeze (single source of truth for CFG/SSA/type/region); this pass is
// strictly the pre-lowering, surface-level gate.
//
// Checks (v1):
//   - every function's typeidx is in range of the type section
//   - every export references an existing entity in its index space
//   - a start function, if present, exists and has signature [] -> []
//   - memory/table limits are canonical (min <= max when max present)
//   - at most one memory (Wasm MVP)
//
// validated_module is a proof token: a non-owning view minted ONLY by validate().
// Downstream (ssa_build, lower_hl) take a validated_module by value, so a caller
// cannot accidentally lower an un-validated module. The token cannot be forged —
// its constructor is private and validate() is its sole friend.

#include "languages/taranga/build_ast.hpp"
#include "languages/taranga/module_view.hpp"
#include "languages/taranga/source_span.hpp"
#include "languages/taranga/wasm_types.hpp"

#include "vakya/diagnostics.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace taranga {

    class validated_module; // fwd

    struct validate_result {
        vakya::diag::collecting_sink diagnostics;
        [[nodiscard]] bool ok() const noexcept { return !diagnostics.has_errors(); }
    };

    namespace detail {

        // Run every module-level check, appending diagnostics. Returns ok.
        inline bool run_validation(const module_view& view,
                                   vakya::diag::collecting_sink& diags) {
            const auto n_types = static_cast<std::uint32_t>(view.types().size());

            // Function typeidx range.
            for (auto fid : view.functions()) {
                const auto& fn = view.node(fid);
                if (fn.ext.immediate >= n_types) {
                    diags.on_diagnostic(make_error(
                        "TARANGA-VAL-001",
                        "function typeidx " + std::to_string(fn.ext.immediate) +
                            " out of range (" + std::to_string(n_types) + " types)"));
                }
            }

            // Index-space sizes for export resolution.
            const std::uint32_t total_funcs =
                view.imported_function_count() + view.defined_function_count();
            const auto n_globals = static_cast<std::uint32_t>(view.globals().size());
            const auto n_memories = static_cast<std::uint32_t>(view.memories().size());
            const auto n_tables = static_cast<std::uint32_t>(view.tables().size());

            // Export targets exist.
            for (auto eid : view.exports()) {
                const auto& ex = view.node(eid);
                const std::uint32_t idx = ex.ext.immediate;
                const std::string_view head = ex.ext.head;
                bool in_range = true;
                if (head == "func") in_range = idx < total_funcs;
                else if (head == "global") in_range = idx < n_globals;
                else if (head == "memory") in_range = idx < n_memories;
                else if (head == "table") in_range = idx < n_tables;
                if (!in_range) {
                    diags.on_diagnostic(make_error(
                        "TARANGA-VAL-002",
                        "export '" + ex.ext.text + "' references missing " +
                            std::string(head) + " index " + std::to_string(idx)));
                }
            }

            // Start function: exists and is [] -> [].
            if (view.module().start_function) {
                const std::uint32_t sidx = *view.module().start_function;
                if (sidx >= total_funcs) {
                    diags.on_diagnostic(make_error(
                        "TARANGA-VAL-003",
                        "start function index " + std::to_string(sidx) + " out of range"));
                } else if (sidx >= view.imported_function_count()) {
                    // Defined function — check its recorded signature is empty.
                    const std::uint32_t local = sidx - view.imported_function_count();
                    auto funcs = view.functions();
                    if (local < funcs.size()) {
                        const auto* sig = view.signature_of(funcs[local]);
                        if (sig && (!sig->params.empty() || !sig->results.empty())) {
                            diags.on_diagnostic(make_error(
                                "TARANGA-VAL-004",
                                "start function must have signature [] -> []"));
                        }
                    }
                }
            }

            // At most one memory (MVP).
            if (n_memories > 1) {
                diags.on_diagnostic(make_error(
                    "TARANGA-VAL-005",
                    "module declares " + std::to_string(n_memories) +
                        " memories; MVP permits at most one"));
            }

            // Canonical limits: min <= max where max is recorded (immediate2 != 0
            // signals a max; build_ast stores 0 for unbounded).
            for (auto mid : view.memories()) {
                const auto& m = view.node(mid);
                if (m.ext.immediate2 != 0 && m.ext.immediate2 < m.ext.immediate) {
                    diags.on_diagnostic(make_error(
                        "TARANGA-VAL-006", "memory limits: max < min"));
                }
            }
            for (auto tid : view.tables()) {
                const auto& t = view.node(tid);
                if (t.ext.immediate2 != 0 && t.ext.immediate2 < t.ext.immediate) {
                    diags.on_diagnostic(make_error(
                        "TARANGA-VAL-007", "table limits: max < min"));
                }
            }

            return !diags.has_errors();
        }

    } // namespace detail

    // =========================================================================
    // validated_module — proof token. Only validate() can mint one.
    // =========================================================================

    class validated_module {
    public:
        [[nodiscard]] const taranga_module& module() const noexcept { return *mod_; }
        [[nodiscard]] module_view view() const noexcept { return module_view(*mod_); }

    private:
        explicit validated_module(const taranga_module& m) noexcept : mod_(&m) {}
        const taranga_module* mod_;

        friend std::pair<validate_result, std::optional<validated_module>>
        validate(const taranga_module&);
    };

    // =========================================================================
    // validate — the sole minter. Returns diagnostics + an optional token: the
    // token is present iff validation (and the upstream frontend) had no errors.
    // =========================================================================

    [[nodiscard]] inline std::pair<validate_result, std::optional<validated_module>>
    validate(const taranga_module& m) {
        validate_result vr;
        // Propagate any frontend errors — an ill-formed AST is not validatable.
        if (m.diagnostics.has_errors()) {
            for (const auto& d : m.diagnostics.entries) vr.diagnostics.on_diagnostic(d);
            return {std::move(vr), std::nullopt};
        }
        module_view view(m);
        const bool ok = detail::run_validation(view, vr.diagnostics);
        if (!ok) return {std::move(vr), std::nullopt};
        return {std::move(vr), validated_module(m)};
    }

} // namespace taranga
