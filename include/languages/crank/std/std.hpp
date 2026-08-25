#pragma once

// crank/std/std.hpp — Crank standard library umbrella.
//
// C++23, header-only, no virtual, no macros. Namespace: crank::stdlib
//
// The stdlib is a reflection-driven projection of C++/STL (and a thin libuv
// veneer) into Crank — not a separate runtime. Each module registers its C++
// free functions through the host embedding seams and exposes an import-visible
// `std.x` module. This header pulls every module together:
//
//   install_std_all(ctx)   — install every available module into one context.
//
// Pay-for-what-you-use: individual install_std_* remain callable on their own.
// Modules gated behind an external dependency (glaze → std.json, libuv →
// std.net, plus the spawn/async entries of std.process/std.fs) install only
// when their __has_include guard is satisfied; otherwise they are silently
// skipped and cost nothing.

#include "languages/crank/std/core.hpp"
#include "languages/crank/std/math.hpp"
#include "languages/crank/std/string.hpp"
#include "languages/crank/std/collections.hpp"
#include "languages/crank/std/containers.hpp"
#include "languages/crank/std/time.hpp"
#include "languages/crank/std/io.hpp"
#include "languages/crank/std/fs.hpp"
#include "languages/crank/std/process.hpp"
#include "languages/crank/std/net.hpp"
#include "languages/crank/std/json.hpp"

namespace crank::stdlib {
    // install_std_all — register every available std module into ctx. Pure
    // modules (core/math/string/collections/containers/time/io/fs/process)
    // always install; std.net installs only under libuv, std.json only under
    // glaze.
    inline void install_std_all(crank::context& ctx) {
        install_std_core(ctx);
        install_std_math(ctx);
        install_std_string(ctx);
        install_std_collections(ctx);
        install_std_containers(ctx);
        install_std_time(ctx);
        install_std_io(ctx);
        install_std_fs(ctx);
        install_std_process(ctx);

#if CRANK_STD_HAS_UV
        install_std_net(ctx);
#endif
#if CRANK_STD_HAS_GLAZE
        install_std_json(ctx);
#endif
    }
} // namespace crank::stdlib
