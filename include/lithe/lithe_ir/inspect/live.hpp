#pragma once

// =============================================================================
// lithe_ir/inspect/live.hpp — inspect_live: freeze-then-view (opt-in)
//
// Namespace: lithe::ir::inspect
//
// inspect_live(fn, opts) freezes a live hl_mir_function (const-in, untouched)
// into an inspector-owned portable_module and returns an ir_inspector over it.
// The live function is never mutated.
//
// This is the one exception to the "borrow, don't own" rule — the inspector
// owns the module it froze (it created it).
//
// OPT-IN: pulls lithe_codegen.hpp via portable/freeze.hpp.
// Include only when live MIR access is required.  Base inspect headers compile
// without codegen.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <expected>
#include <utility>

#include "../portable/freeze.hpp"   // freeze_function, freeze_options, freeze_error
#include "../portable/module.hpp"
#include "handles.hpp"
#include "inspector.hpp"

namespace lithe::ir::inspect {
    // =============================================================================
    // owning_inspector — ir_inspector that owns the portable_module it froze
    // =============================================================================

    class owning_inspector {
    public:
        explicit owning_inspector(portable::portable_module m) noexcept
            : owned_(std::move(m))
              , inspector_(owned_) {}

        // Delegate all ir_inspector operations.
        [[nodiscard]] const ir_inspector& get() const noexcept { return inspector_; }
        [[nodiscard]] ir_inspector& get() noexcept { return inspector_; }

        // Convenience forwarding for common operations.
        [[nodiscard]] std::size_t function_count() const noexcept { return inspector_.function_count(); }

        [[nodiscard]] std::string_view function_name(std::uint32_t i) const noexcept {
            return inspector_.function_name(i);
        }

        [[nodiscard]] hl_mir_view function_view(std::uint32_t i) const noexcept { return inspector_.function_view(i); }

        [[nodiscard]] portable::verify_report verify(const portable::verify_policy& p = {}) const {
            return inspector_.verify(p);
        }

        [[nodiscard]] std::array<std::uint8_t, 64> semantic_digest() const { return inspector_.semantic_digest(); }
        [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const { return inspector_.canonical_bytes(); }

    private:
        portable::portable_module owned_;
        ir_inspector inspector_;
    };

    // =============================================================================
    // inspect_live — freeze-then-view (const-in; live fn untouched)
    // =============================================================================

    [[nodiscard]] inline std::expected<owning_inspector, inspect_error>
    inspect_live(const codegen::hl::hl_mir_function& fn,
                 const portable::freeze_options& opts = {}) {
        auto result = portable::freeze_function(fn, opts);
        if (!result) {
            return std::unexpected(
                inspect_error{
                    inspect_error::code::dump_failed,
                    result.error().detail
                });
        }

        portable::portable_module mod;
        mod.functions.push_back(std::move(*result));
        return owning_inspector{std::move(mod)};
    }
} // namespace lithe::ir::inspect
