// =============================================================================
// test_lithe_facets.cpp — facet detection + CPO customisation contract
//
// Verifies (§3.3):
//   1. A backend exposing only some tag_invoke customisations satisfies exactly
//      those concepts.
//   2. The CPO resolves ONLY via ADL tag_invoke; a member `compile` does NOT
//      satisfy compiler_for.
//   3. Missing customisation is a hard concept failure (static_assert).
// =============================================================================

#include "catch_amalgamated.hpp"

#include <expected>
#include <span>

#include "lithe/lithe_execution/facet.hpp"
#include "lithe/lithe_execution/artifact.hpp"
#include "lithe/lithe_execution/resource.hpp"
#include "lithe/lithe_execution/entry.hpp"

namespace ex = lithe::execution;
using ex::type_tag;
using ex::cpo::compile;
using ex::cpo::install;
using ex::cpo::get_entry;
using ex::cpo::invoke;

// ============================================================================
// Minimal test backends
// ============================================================================

namespace test_backends {
    // ---- A backend with a MEMBER compile — must NOT satisfy compiler_for ----
    struct member_compile_backend {
        // member function — NOT an ADL tag_invoke customisation
        std::expected<std::string, ex::compile_error>
        compile(int) { return "member_result"; }
    };

    // ---- A properly customised compile-only backend -------------------------
    struct compile_only_backend {};

    struct compile_only_ir {};

    [[nodiscard]] inline
    std::expected<ex::basic_compiled_artifact<std::string>, ex::compile_error>
    tag_invoke(ex::cpo::compile_t, compile_only_backend&, compile_only_ir&&) {
        ex::artifact_manifest m;
        m.role = ex::artifact_class::text_report;
        return ex::basic_compiled_artifact<std::string>{m, "compiled", {}};
    }

    // ---- A compile+install backend -----------------------------------------
    struct full_backend {};

    struct full_ir {};

    struct full_payload {
        std::string data;
    };

    struct full_resource_t {
        ex::frame_counter_ref counter = ex::make_frame_counter();
        std::string data;
    };

    [[nodiscard]] inline
    std::expected<ex::basic_compiled_artifact<full_payload>, ex::compile_error>
    tag_invoke(ex::cpo::compile_t, full_backend&, full_ir&&) {
        ex::artifact_manifest m;
        m.role = ex::artifact_class::text_report;
        return ex::basic_compiled_artifact<full_payload>{m, {"hello"}, {}};
    }

    [[nodiscard]] inline
    std::expected<full_resource_t, ex::install_error>
    tag_invoke(ex::cpo::install_t, full_backend&,
               ex::basic_compiled_artifact<full_payload>&& art) {
        return full_resource_t{ex::make_frame_counter(), art.payload.data};
    }
} // namespace test_backends

// ============================================================================
// Compile-time concept checks
// ============================================================================

// member_compile_backend does NOT satisfy compiler_for (no tag_invoke).
static_assert(!ex::compiler_for<test_backends::member_compile_backend, int>,
              "member compile() must NOT satisfy compiler_for");

// compile_only_backend satisfies compiler_for for its IR type.
static_assert(ex::compiler_for<test_backends::compile_only_backend,
                               test_backends::compile_only_ir>,
              "compile_only_backend must satisfy compiler_for<compile_only_ir>");

// compile_only_backend does NOT satisfy installer_for (no tag_invoke for install).
static_assert(!ex::installer_for<test_backends::compile_only_backend,
                                 ex::basic_compiled_artifact<std::string>>,
              "compile_only_backend must NOT satisfy installer_for");

// full_backend satisfies both compiler_for and installer_for.
static_assert(ex::compiler_for<test_backends::full_backend, test_backends::full_ir>,
              "full_backend must satisfy compiler_for");
static_assert(ex::installer_for<test_backends::full_backend,
                                ex::basic_compiled_artifact<test_backends::full_payload>>,
              "full_backend must satisfy installer_for");

// ============================================================================
// Runtime tests
// ============================================================================

TEST_CASE (


"compiler_for: tag_invoke compile resolves via ADL"
,
"[facet][cpo]"
)
 {
    test_backends::compile_only_backend b;
    test_backends::compile_only_ir ir{};
    auto result = compile(b, std::move(ir));
    REQUIRE(result.has_value());
    CHECK(result->payload == "compiled");
}

TEST_CASE (


"installer_for: tag_invoke install resolves via ADL"
,
"[facet][cpo]"
)
 {
    test_backends::full_backend b;
    test_backends::full_ir ir{};
    auto art = compile(b, std::move(ir));
    REQUIRE(art.has_value());
    auto res = install(b, std::move(*art));
    REQUIRE(res.has_value());
    CHECK(res->data == "hello");
}

TEST_CASE (


"CPO poison-pill: member compile does not satisfy concept"
,
"[facet][concept]"
)
 {
    // This is a compile-time check; the static_assert above validates it.
    // Runtime: just confirm the backend exists but concept is false.
    constexpr bool ok = ex::compiler_for<test_backends::member_compile_backend, int>;
    REQUIRE_FALSE(ok);
}
