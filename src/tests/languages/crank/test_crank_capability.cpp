// =============================================================================
// test_crank_capability.cpp — backend capability discovery (design §8).
//
// Covers:
//   1. Scalar reference backend is always present.
//   2. allow_simd=false/allow_threads=false → scalar only.
//   3. backend_policy::inline_only narrows to scalar alone.
//   4. discover_backends caches on the policy fingerprint (same ref for same policy).
//   5. A user type satisfies the ExecutionBackend concept via static dispatch.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/capability.hpp"

using namespace crank;

TEST_CASE (

"scalar backend is always discovered"
,
"[crank][capability]"
)
 {
    // Disable optional backends; scalar is always present.
    execution_options opts;
    opts.allow_simd    = false;
    opts.allow_threads = false;
    opts.allow_gpu     = false;
    auto set = build_capability_set(opts);
    REQUIRE(set.has(execution_kind::scalar));
    REQUIRE_FALSE(set.has(execution_kind::simd));
    REQUIRE_FALSE(set.has(execution_kind::threaded));
}

TEST_CASE (

"allow flags add simd + threaded backends"
,
"[crank][capability]"
)
 {
    execution_options opts;
    opts.allow_simd    = true;
    opts.allow_threads = true;
    auto set = build_capability_set(opts);
    REQUIRE(set.has(execution_kind::scalar));
    REQUIRE(set.has(execution_kind::simd));
    REQUIRE(set.has(execution_kind::threaded));
}

TEST_CASE (

"inline_only policy narrows to scalar"
,
"[crank][capability]"
)
 {
    execution_options opts;
    opts.allow_simd = true;
    opts.backend    = backend_policy::inline_only;
    auto set = build_capability_set(opts);
    REQUIRE(set.backends.size() == 1);
    REQUIRE(set.backends.front().kind == execution_kind::scalar);
}

TEST_CASE (

"threaded_only policy keeps scalar + threaded"
,
"[crank][capability]"
)
 {
    execution_options opts;
    opts.allow_threads = true;
    opts.allow_simd    = true;
    opts.backend       = backend_policy::threaded_only;
    auto set = build_capability_set(opts);
    REQUIRE(set.has(execution_kind::scalar));
    REQUIRE(set.has(execution_kind::threaded));
    REQUIRE_FALSE(set.has(execution_kind::simd));
}

TEST_CASE (

"discover_backends caches on fingerprint"
,
"[crank][capability]"
)
 {
    execution_options opts;
    opts.allow_simd = true;
    const auto& a = discover_backends(opts);
    const auto& b = discover_backends(opts);
    REQUIRE(&a == &b);  // same policy → same cached object
}

namespace {
    // A minimal static backend that satisfies the ExecutionBackend concept.
    struct fake_backend {
        static backend_descriptor descriptor() {
            backend_descriptor d;
            d.id = 99;
            d.kind = execution_kind::host;
            d.name = "fake";
            return d;
        }

        void prepare(const verified_mir&) {}
        void execute(const verified_mir&) {}
    };
} // namespace

TEST_CASE (

"a static adapter satisfies ExecutionBackend"
,
"[crank][capability]"
)
 {
    STATIC_REQUIRE(ExecutionBackend<fake_backend>);
    REQUIRE(fake_backend::descriptor().name == "fake");
}
