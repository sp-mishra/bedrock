#pragma once

// =============================================================================
// backends/lithe_codegen_interpreter_facet.hpp — interpreter facet adapter
//
// Wires interpreter_backend into the lithe_execution facet vocabulary (, P3).
//
// Adds tag_invoke customisations for:
//   compile(interpreter_backend&, physical_mir_function&&)
//     -> expected<interpreter_artifact_impl, compile_error>
//
//   install(interpreter_backend&, interpreter_artifact_impl&&)
//     -> expected<interpreter_resource, install_error>
//
//   get_entry(interpreter_backend&, interpreter_resource&, type_tag<Sig>{})
//     -> expected<typed_entry<Sig>, execution_error>
//       (supported for Sig = int64_t(int64_t, int64_t) and double(int64_t, int64_t))
//
//   invoke(interpreter_backend&, interpreter_resource&, invocation_request)
//     -> expected<dynamic_execution_result, execution_error>
//       (the erased path; shares the same program object)
//
// The interpreter's existing emit() is UNTOUCHED (additive contract).
// compile_and_install is NOT provided for the interpreter: the interpreter CAN
// separate compile from install, and providing only compile+install demonstrates
// the split-path.  execute_with_fallback remains unchanged (uses emit()).
//
// Payload types defined here:
//   interpreter_program  (the program plan produced by compile)
//   interpreter_resource (the installed resource — owns the program + counter)
//
// backend_traits<interpreter_backend> specialised here.
//
// No virtual, no macros.  Header-only C++23.
// Depends: lithe_codegen_interpreter.hpp + lithe_execution/{facet,artifact,resource,entry}.hpp
// =============================================================================

#include "../lithe_execution/facet.hpp"
#include "../lithe_execution/artifact.hpp"
#include "../lithe_execution/resource.hpp"
#include "../lithe_execution/entry.hpp"
#include "lithe_codegen_interpreter.hpp"   // interpreter_backend, physical_mir_function

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>

namespace lithe::execution {
    // =========================================================================
    // interpreter_program — complete definition of the forward-declared type
    //
    // The interpreter does not produce binary code; it produces a plan object
    // that bundles the physical MIR function + initial argument bindings.
    // =========================================================================

    struct interpreter_program {
        codegen::mir::physical_mir_function fn;
        std::vector<std::int64_t> default_args;

        [[nodiscard]] bool valid() const noexcept {
            return !fn.function.blocks.empty();
        }
    };

    // =========================================================================
    // interpreter_resource — installed resource for the interpreter path
    //
    // Owns the program + a frame_counter for entry-lease/invocation-guard wiring.
    // An interpreter_resource corresponds to one "installed" program ready for
    // repeated invocations.
    // =========================================================================

    class interpreter_resource {
    public:
        interpreter_resource() = default;

        explicit interpreter_resource(interpreter_program program,
                                      codegen::backends::interpreter_backend* backend_ptr)
            : program_(std::make_shared<interpreter_program>(std::move(program)))
              , counter_(make_frame_counter())
              , backend_(backend_ptr) {}

        [[nodiscard]] bool valid() const noexcept {
            return program_ != nullptr && counter_ != nullptr && backend_ != nullptr;
        }

        [[nodiscard]] const interpreter_program& program() const noexcept { return *program_; }
        [[nodiscard]] const frame_counter_ref& counter() const noexcept { return counter_; }

        // Invoke the program synchronously with the given args.
        [[nodiscard]]
        std::expected<invocation_result, execution_error>
        invoke_impl(std::span<const std::int64_t> args) const {
            if (!valid())
                return std::unexpected(execution_error{"interpreter_resource: invalid"});

            backend_->reset_all();
            backend_->arguments.assign(args.begin(), args.end());
            auto art = backend_->emit(program_->fn);

            if (art.kind == codegen::artifact_kind::none) {
                std::string msg = "interpreter execution failed";
                if (!art.diagnostics.empty()) msg += ": " + art.diagnostics.front();
                return std::unexpected(execution_error{msg.c_str()});
            }

            if (backend_->return_value.has_value()) {
                return invocation_result::make_int(*backend_->return_value);
            }
            return invocation_result::make_int(0);
        }

    private:
        std::shared_ptr<interpreter_program> program_;
        frame_counter_ref counter_;
        codegen::backends::interpreter_backend* backend_ = nullptr;
    };

    // =========================================================================
    // backend_traits<interpreter_backend> — associated type declarations
    // =========================================================================

    using interpreter_backend_t = codegen::backends::interpreter_backend;

    template <>
    struct backend_traits<interpreter_backend_t> {
        template <class IR>
        using artifact = basic_compiled_artifact<interpreter_program>;

        template <class Artifact>
        using resource = interpreter_resource;

        template <class Resource, class Sig>
        using entry = typed_entry<Sig>;
    };
} // namespace lithe::execution

// =============================================================================
// Facet adapter — tag_invoke customisations in lithe::codegen::backends
//
// ADL for tag_invoke(compile_t{}, interpreter_backend&, ...) searches the
// associated namespaces of the non-tag arguments.  interpreter_backend lives in
// lithe::codegen::backends, so the customisations must live there too.
// =============================================================================

namespace lithe::codegen::backends {
    // --- compile -----------------------------------------------------------
    [[nodiscard]] inline
    std::expected<
        lithe::execution::basic_compiled_artifact<lithe::execution::interpreter_program>,
        lithe::execution::compile_error>
    tag_invoke(lithe::execution::cpo::compile_t,
               interpreter_backend& /*backend*/,
               mir::physical_mir_function&& fn) {
        if (fn.function.blocks.empty())
            return std::unexpected(lithe::execution::compile_error{"empty MIR function"});

        lithe::execution::interpreter_program prog;
        prog.fn = std::move(fn);

        lithe::execution::artifact_manifest manifest;
        manifest.produced_from = lithe::execution::ir_kind::physical_mir;
        manifest.role = lithe::execution::artifact_class::interpreter_plan;
        manifest.backend_id = "lithe.backend.interpreter";

        lithe::execution::compilation_metadata meta;
        meta.ir_hash = 0;
        meta.source_name = "interpreter_compile";

        return lithe::execution::basic_compiled_artifact<lithe::execution::interpreter_program>{
            std::move(manifest), std::move(prog), std::move(meta)
        };
    }

    // --- install -----------------------------------------------------------
    [[nodiscard]] inline
    std::expected<lithe::execution::interpreter_resource, lithe::execution::install_error>
    tag_invoke(lithe::execution::cpo::install_t,
               interpreter_backend& backend,
               lithe::execution::basic_compiled_artifact<
                   lithe::execution::interpreter_program>&& art) {
        if (!art.valid())
            return std::unexpected(lithe::execution::install_error{"invalid artifact"});

        return lithe::execution::interpreter_resource{std::move(art.payload), &backend};
    }

    // --- get_entry (typed: int64_t(int64_t, int64_t)) ----------------------
    [[nodiscard]] inline
    std::expected<
        lithe::execution::typed_entry < std::int64_t(std::int64_t, std::int64_t)>
    ,
    lithe::execution::execution_error
    >
    tag_invoke(lithe::execution::cpo::get_entry_t,
               interpreter_backend& /*backend*/,
               lithe::execution::interpreter_resource& res,
               lithe::execution::type_tag<std::int64_t(std::int64_t, std::int64_t)>) {
        if (!res.valid())
            return std::unexpected(lithe::execution::execution_error{"interpreter_resource invalid"});

        lithe::execution::entry_lease lease{res.counter()};

        auto fn = [res_copy = res](std::int64_t a, std::int64_t b)
            -> std::int64_t {
            std::array<std::int64_t, 2> args{a, b};
            auto r = res_copy.invoke_impl(args);
            if (!r.has_value()) return 0;
            return r->raw_value;
        };

        return lithe::execution::typed_entry < std::int64_t(std::int64_t, std::int64_t) >
        {
            std::move(lease),
                std::function < std::int64_t(std::int64_t, std::int64_t) >
            {
                std::move(fn)
            }
        };
    }

    // --- get_entry (typed: double(int64_t, int64_t)) -----------------------
    [[nodiscard]] inline
    std::expected<
        lithe::execution::typed_entry < double(std::int64_t, std::int64_t)>
    ,
    lithe::execution::execution_error
    >
    tag_invoke(lithe::execution::cpo::get_entry_t,
               interpreter_backend& /*backend*/,
               lithe::execution::interpreter_resource& res,
               lithe::execution::type_tag<double(std::int64_t, std::int64_t)>) {
        if (!res.valid())
            return std::unexpected(lithe::execution::execution_error{"interpreter_resource invalid"});

        lithe::execution::entry_lease lease{res.counter()};

        auto fn = [res_copy = res](std::int64_t a, std::int64_t b)
            -> double {
            std::array<std::int64_t, 2> args{a, b};
            auto r = res_copy.invoke_impl(args);
            if (!r.has_value()) return 0.0;
            return r->is_fp ? r->fp_value : static_cast<double>(r->raw_value);
        };

        return lithe::execution::typed_entry < double(std::int64_t, std::int64_t) >
        {
            std::move(lease),
                std::function < double(std::int64_t, std::int64_t) >
            {
                std::move(fn)
            }
        };
    }

    // --- invoke (erased path) ----------------------------------------------
    [[nodiscard]] inline
    std::expected<lithe::execution::dynamic_execution_result, lithe::execution::execution_error>
    tag_invoke(lithe::execution::cpo::invoke_t,
               interpreter_backend& /*backend*/,
               lithe::execution::interpreter_resource& res,
               lithe::execution::invocation_request req) {
        auto r = res.invoke_impl(req.args);
        if (!r.has_value())
            return std::unexpected(r.error());
        return *r;
    }
} // namespace lithe::codegen::backends
