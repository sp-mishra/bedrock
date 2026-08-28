#pragma once

// crank/std/process.hpp — std.process module: process/environment for Crank.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// Environment queries use std facilities (always available). Process spawning
// is backed by libuv (uv_spawn) and only registered when libuv is present
// (CRANK_STD_HAS_UV). Effect=IO, capability=Execute.

#include "languages/crank/std/detail/register.hpp"
#include "languages/crank/std/detail/uv_loop.hpp"
#include "languages/crank/effects.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace crank::stdlib {
    namespace process_fns {
        // env — value of an environment variable, or empty string if unset.
        [[nodiscard]] inline std::string env(std::string name) {
            const char* v = std::getenv(name.c_str());
            return v ? std::string(v) : std::string{};
        }
        [[nodiscard]] inline bool has_env(std::string name) noexcept {
            return std::getenv(name.c_str()) != nullptr;
        }

#if CRANK_STD_HAS_UV
        // spawn_wait — run program (no args) to completion, return its exit code.
        // Returns -1 if the spawn itself failed. Synchronous: drives a private
        // loop until the child exits.
        [[nodiscard]] inline std::int64_t spawn_wait(std::string program) {
            crank::uvx::loop lp;
            uv_process_t proc{};
            std::int64_t exit_status = -1;
            struct wait_state { std::int64_t* out; } ws{&exit_status};
            proc.data = &ws;

            char* argv[2] = {program.data(), nullptr};
            uv_process_options_t opts{};
            opts.file = program.c_str();
            opts.args = argv;
            opts.exit_cb = [](uv_process_t* h, std::int64_t status, int /*sig*/) {
                auto* s = static_cast<wait_state*>(h->data);
                if (s && s->out) *s->out = status;
                uv_close(reinterpret_cast<uv_handle_t*>(h), nullptr);
            };

            int rc = uv_spawn(lp.raw(), &proc, &opts);
            if (rc != 0) return -1;
            lp.run();
            return exit_status;
        }
#endif
    } // namespace process_fns

    inline void install_std_process(crank::context& ctx) {
        namespace p = process_fns;
        ffi_module_builder mod{"std.process"};
        const function_options rd{
            .effects = vakya::types::kEffectMaskIO,
            .capabilities = vakya::types::kCapMaskRead,
        };
        detail::add_fn<"std.process.env", &p::env>(mod, ctx, "Env", rd);
        detail::add_fn<"std.process.has_env", &p::has_env>(mod, ctx, "HasEnv", rd);

#if CRANK_STD_HAS_UV
        const function_options exec{
            .effects = vakya::types::kEffectMaskIO,
            .capabilities = vakya::types::kCapMaskExecute,
            .flags = static_cast<function_flags>(function_flag::blocking),
            .blocking = blocking_class::potentially_blocking,
        };
        detail::add_fn<"std.process.spawn_wait", &p::spawn_wait>(mod, ctx, "SpawnWait", exec);
#endif

        ctx.register_ffi_module(mod.build());
    }
} // namespace crank::stdlib
