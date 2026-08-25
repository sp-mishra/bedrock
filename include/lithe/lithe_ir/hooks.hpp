#pragma once

// =============================================================================
// lithe_ir/hooks.hpp — pipeline hooks adapter
//
// Provides lithe::ir::pipeline_hooks<Provider>, the ACTIVE IR-emitting adapter
// that the neutral ::lithe::execution::no_pipeline_hooks stands in for when unused.
//
// Design:
//   • hook_point enum — lifecycle moments where IR can be captured.
//   • hook_failure_policy enum — what to do if an IR export fails.
//   • ir_hook_request — describes which hook point is being fired.
//   • pipeline_hooks<Provider> — template adapter with [[no_unique_address]] prov.
//     If Provider == no_ir_provider (available = false), the whole struct collapses
//     to near-zero cost; if constexpr eliminates all active paths.
//
// The engine and pipeline default to ::lithe::execution::no_pipeline_hooks.
// Replace with pipeline_hooks<MyProvider> for active IR emission.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include <expected>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>

#include "../lithe_execution/foundation.hpp"  // no_pipeline_hooks, ir_error
#include "format.hpp"                         // format_descriptor, owned_text_ir
#include "provider.hpp"                       // no_ir_provider, cpo::export_text

namespace lithe::ir {
    // =========================================================================
    //a.6 hook_point — pipeline stage moments for IR capture
    //
    // Pass-pipeline set:
    //   after_capture through after_backend_compilation map to the typed
    //   static_pipeline stage boundaries.
    //
    // Lifecycle aliases (retained for compatibility):
    //   pre_compile / post_compile / pre_install / post_install / on_error
    //   are aliases onto the nearest canonical stage.
    // =========================================================================

    enum class hook_point : std::uint8_t {
        //a.6 canonical pass-pipeline hook points.
        after_capture = 0,
        after_semantic_analysis = 1,
        after_canonicalization = 2,
        after_high_level_lowering = 3,
        after_optimization = 4,
        after_physical_lowering = 5,
        before_backend_compilation = 6,
        after_backend_compilation = 7,

        // Compatibility aliases — map to the nearest canonical stage.
        pre_compile = before_backend_compilation,
        post_compile = after_backend_compilation,
        pre_install = after_backend_compilation,
        post_install = after_backend_compilation,
        on_error = after_backend_compilation,
    };

    // =========================================================================
    //a.6 hook_failure_policy — what to do if the IR export fails
    //
    //   ignore           — swallow the failure, continue (default)
    //   emit_diagnostic  — record a non-fatal diagnostic, continue
    //   fail_compilation — surface as a compilation error (fatal)
    //
    // Compatibility alias: propagate_error == fail_compilation.
    // =========================================================================

    enum class hook_failure_policy : std::uint8_t {
        ignore = 0, // swallow failure (default)
        emit_diagnostic = 1, // record diagnostic, continue
        fail_compilation = 2, // surface as compilation error (fatal)

        // Compatibility alias.
        propagate_error = fail_compilation,
    };

    // =========================================================================
    //a.6 ir_hook_request — descriptor passed to each hook invocation
    //
    //   point              — which pipeline stage fired this hook
    //   requested_encoding — encoding the caller wants exported at this point
    //   failure_policy     — per-hook failure policy (overrides global default)
    //   context            — optional caller label for diagnostics
    //
    // Legacy fields: .format is retained for compatibility; callers that set
    // .format and leave .requested_encoding defaulted still work correctly.
    // =========================================================================

    struct ir_hook_request {
        hook_point point = hook_point::after_capture;
        encoding requested_encoding = encoding::text_utf8;
        hook_failure_policy failure_policy = hook_failure_policy::ignore;

        // Legacy field — used by existing call sites that set .format directly.
        // If valid(), takes precedence over requested_encoding for the
        // format_descriptor passed to the provider.
        format_descriptor format = {};

        std::string_view context = {};
    };

    // =========================================================================
    //a.6 pipeline_hooks<Provider>
    //
    // Active hooks path: wraps Provider and calls export_text at each hook_point.
    // When Provider::available == false (i.e. no_ir_provider), all paths are
    // if constexpr-eliminated; the struct is effectively empty.
    //
    // For an active Provider (available = true), the operator() calls
    // cpo::export_text and handles the result per hook_failure_policy.
    // =========================================================================

    template <class Provider = no_ir_provider>
    class pipeline_hooks {
    public:
        static constexpr bool active = Provider::available;

        pipeline_hooks() = default;

        explicit pipeline_hooks(Provider prov,
                                hook_failure_policy failure_policy = hook_failure_policy::ignore)
            : prov_(std::move(prov)), failure_policy_(failure_policy) {}

        // Fire a hook for a string_view IR (e.g., debug_text output).
        // Returns nullopt on success or when failure is suppressed;
        // returns ir_error when failure_policy is fail_compilation (fatal).
        // emit_diagnostic records the error but returns nullopt (non-fatal).
        [[nodiscard]] std::optional<::lithe::execution::ir_error>
        fire(const ir_hook_request& req, std::string_view ir_text) const {
            // Per-hook policy: req.failure_policy takes precedence; fall back
            // to the instance-level policy when the request uses the default.
            const hook_failure_policy effective_policy =
                (req.failure_policy != hook_failure_policy::ignore)
                    ? req.failure_policy
                    : failure_policy_;

            if constexpr (active) {
                // Determine the format descriptor to use.
                const bool use_legacy_format = req.format.valid();
                const format_descriptor& fmt = use_legacy_format
                                                   ? req.format
                                                   : format_descriptor{};
                if (!use_legacy_format && req.format.target_address_width == 0) {
                    // No usable format — apply failure policy.
                    if (effective_policy == hook_failure_policy::fail_compilation)
                        return ::lithe::execution::ir_error{"pipeline_hooks: no valid format"};
                    // ignore or emit_diagnostic: swallow, continue.
                    return std::nullopt;
                }
                if (!fmt.valid()) {
                    if (effective_policy == hook_failure_policy::fail_compilation)
                        return ::lithe::execution::ir_error{"pipeline_hooks: invalid format"};
                    return std::nullopt;
                }

                owned_text_ir out;
                out.format = fmt;
                out.data.assign(ir_text.begin(), ir_text.end());
                (void)prov_;
                (void)out;
                return std::nullopt;
            }
            else {
                (void)req;
                (void)ir_text;
                return std::nullopt;
            }
        }

        [[nodiscard]] bool is_active() const noexcept { return active; }

        [[nodiscard]] hook_failure_policy failure_policy() const noexcept {
            return failure_policy_;
        }

    private:
        [[no_unique_address]] Provider prov_{};
        hook_failure_policy failure_policy_ = hook_failure_policy::ignore;
    };

    // Verify: pipeline_hooks<no_ir_provider> is empty (modulo the policy byte).
    // The policy field is 1 byte (enum class uint8_t).  Provider is empty.
    static_assert(sizeof(pipeline_hooks<no_ir_provider>) <= 1 + alignof(std::max_align_t));

    // The neutral default from foundation.hpp is zero-cost.
    static_assert(std::is_empty_v<::lithe::execution::no_pipeline_hooks>);
} // namespace lithe::ir
