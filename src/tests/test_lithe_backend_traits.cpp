// =============================================================================
// test_lithe_backend_traits.cpp — associated-type threading (§3.2)
//
// Verifies:
//   • artifact_t / resource_t / entry_t thread through the typed static path.
//   • artifact_value_t normalises Artifact and Artifact& to the same resource kind.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <type_traits>

#include "lithe/lithe_execution/facet.hpp"
#include "lithe/lithe_execution/artifact.hpp"
#include "lithe/lithe_execution/resource.hpp"
#include "lithe/lithe_execution/entry.hpp"
#include "lithe/backends/lithe_codegen_interpreter_facet.hpp"

namespace ex = lithe::execution;
using IB = lithe::codegen::backends::interpreter_backend;

// ============================================================================
// artifact_value_t normalisation
// ============================================================================

// basic_compiled_artifact<std::string> and its ref should normalise to same type.
using ArtT = ex::basic_compiled_artifact<std::string>;
using ArtRefT = ex::basic_compiled_artifact<std::string>&;

static_assert(std::is_same_v<ex::artifact_value_t<ArtT>, ex::artifact_value_t<ArtRefT>>,
              "artifact_value_t must normalise value and ref to the same type");

static_assert(std::is_same_v<ex::artifact_value_t<ArtT>, ArtT>,
              "artifact_value_t<T> == T for non-ref non-cv");

// ============================================================================
// interpreter backend_traits round-trip
// ============================================================================

// artifact_t<IB, physical_mir_function> should be basic_compiled_artifact<interpreter_program>
using ExpectedArtifact =
ex::basic_compiled_artifact<ex::interpreter_program>;
using ActualArtifact =
ex::artifact_t<IB, lithe::codegen::mir::physical_mir_function>;

static_assert(std::is_same_v<ActualArtifact, ExpectedArtifact>,
              "artifact_t<IB, physical_mir_function> must be basic_compiled_artifact<interpreter_program>");

// resource_t<IB, artifact> should be interpreter_resource
using ActualResource = ex::resource_t<IB, ExpectedArtifact>;
static_assert(std::is_same_v<ActualResource, ex::interpreter_resource>,
              "resource_t<IB, interpreter_artifact> must be interpreter_resource");

// entry_t<IB, interpreter_resource, Sig> should be typed_entry<Sig>
using Sig = std::int64_t(


std::int64_t
,
std::int64_t
);
using ActualEntry = ex::entry_t<IB, ex::interpreter_resource, Sig>;
static_assert(std::is_same_v<ActualEntry, ex::typed_entry<Sig>>,
              "entry_t<IB, interpreter_resource, Sig> must be typed_entry<Sig>");

// ============================================================================
// artifact_value_t handles reference normalisation for resource_t
// ============================================================================

// resource_t<IB, Artifact&> should equal resource_t<IB, Artifact>
using ResourceFromRef = ex::resource_t<IB, ExpectedArtifact&>;
static_assert(std::is_same_v<ResourceFromRef, ex::interpreter_resource>,
              "resource_t must normalise Artifact& the same as Artifact");

// ============================================================================
// Runtime tests — just confirm the type machinery above compiles
// ============================================================================

TEST_CASE (


"backend_traits: associated types resolve at compile time"
,
"[backend_traits]"
)
 {
    // Static checks above do the real work; this test just ensures they ran.
    constexpr bool artifact_ok =
        std::is_same_v<ActualArtifact, ExpectedArtifact>;
    constexpr bool resource_ok =
        std::is_same_v<ActualResource, ex::interpreter_resource>;
    constexpr bool entry_ok =
        std::is_same_v<ActualEntry, ex::typed_entry<Sig>>;

    REQUIRE(artifact_ok);
    REQUIRE(resource_ok);
    REQUIRE(entry_ok);
}

TEST_CASE (


"artifact_value_t: Artifact and Artifact& map to same resource kind"
,
"[backend_traits]"
)
 {
    constexpr bool ok =
        std::is_same_v<ex::artifact_value_t<ArtT>, ex::artifact_value_t<ArtRefT>>;
    REQUIRE(ok);
}
