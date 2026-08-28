#pragma once

// =============================================================================
// lithe_execution/capability.hpp — compile_requirements + execution_mode_set view
//
// Defines the struct a selector consumes to pick the right backend and mode.
// The selection *algorithm* (cost_based_backend_selector) lands in  (P5);
// only the struct + its trivial predicates land here in .
//
// Allowed/forbidden mode sets correctly distinguish an interpreter-capable
// backend that also supports JIT from a JIT-only backend — a flat capability
// mask cannot express that distinction.
//
// No virtual, no macros.  Header-only C++23.
// =============================================================================

#include "foundation.hpp"

namespace lithe::execution {
    // =========================================================================
    // Security / artifact / target constraint types (lightweight PODs)
    // =========================================================================

    // security_constraints — what the execution context demands of the backend.
    struct security_constraints {
        bool require_w_xor_x = false; // enforce W^X memory protection
        bool forbid_host_pointers = false; // disallow raw host pointers in code
        bool require_verification = false; // backend must verify before install
        bool sandbox_mode = false; // restrict to guest memory model
    };

    // artifact_constraints — what artifact forms are acceptable.
    struct artifact_constraints {
        artifact_class required_class = artifact_class::none; // none = any
        ir_kind accepted_input = ir_kind::physical_mir; // default input
    };

    // target_constraints — physical target requirements.
    struct target_constraints {
        memory_domain required_domain = memory_domain::host_cpu;
        bool allow_async = false;
    };

    // required_services — runtime services the compiled code needs.
    struct required_services {
        bool gc_roots = false; // managed GC root tracking
        bool write_barriers = false; // GC write barrier protocol
        bool safepoints = false; // stop-the-world safepoint polling
        bool exception_unwind = false; // language exception unwinding
        bool fuel_accounting = false; // bounded-execution fuel meter
    };

    // =========================================================================
    // compile_requirements ()
    // =========================================================================

    struct compile_requirements {
        // Capability bits that MUST be present.  Selection fails if a backend
        // does not satisfy all required bits.
        backend_capability_set required;

        // Capability bits that improve quality but are not mandatory.  A selector
        // uses preferred to break ties between equally-capable backends.
        backend_capability_set preferred;

        // Modes that the invocation may use.  Default (empty) = all modes OK.
        // If non-empty, only modes in this set are considered.
        execution_mode_set allowed_modes;

        // Modes that are explicitly forbidden.  A backend that ONLY supports a
        // forbidden mode is rejected.
        execution_mode_set forbidden_modes;

        // Runtime service requirements.
        required_services services;

        // Artifact / security / target constraints.
        artifact_constraints artifact;
        security_constraints security;
        target_constraints target;

        // ---- Trivial predicates (scoring / selection use these) --------------

        // True iff the provided capability set satisfies all required bits.
        [[nodiscard]] constexpr bool
        satisfies_required(const backend_capability_set& provided) const noexcept {
            return provided.contains_all(required);
        }

        // True iff the given mode is allowed (not forbidden, and in allowed_modes
        // when the set is non-empty).
        [[nodiscard]] constexpr bool mode_allowed(const execution_mode m) const noexcept {
            if (forbidden_modes.test(m)) return false;
            if (allowed_modes.any() && !allowed_modes.test(m)) return false;
            return true;
        }

        // True iff at least one mode passes mode_allowed.
        [[nodiscard]] constexpr bool any_mode_allowed() const noexcept {
            for (std::size_t i = 0; i < execution_mode_count; ++i) {
                if (mode_allowed(static_cast<execution_mode>(i))) return true;
            }
            return false;
        }

        // Convenience builders.
        [[nodiscard]] static constexpr compile_requirements interpreter_only() noexcept {
            compile_requirements r;
            r.allowed_modes.set(execution_mode::interpret);
            r.required = backend_capability_set::from({backend_feature::interpreter_execution});
            return r;
        }

        // Requirements preset for wiring lithe::exec analysis-layer output.
        // Sets artifact.accepted_input = ir_kind::hl_mir so that hl_mir_function
        // plans from auto_execution_pass are accepted without caller override.
        [[nodiscard]] static constexpr compile_requirements for_exec_plan() noexcept {
            compile_requirements r;
            r.artifact.accepted_input = ir_kind::hl_mir;
            return r;
        }
    };

    static_assert(std::is_trivially_destructible_v<execution_mode_set>,
                  "execution_mode_set must be trivially destructible");
} // namespace lithe::execution
