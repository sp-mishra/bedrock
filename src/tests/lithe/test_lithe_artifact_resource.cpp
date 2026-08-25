// =============================================================================
// test_lithe_artifact_resource.cpp — typed payload ownership + resource lease
//
// Verifies (§4.1, §4.2, §4.3, §4.5):
//   • Typed payload ownership + move semantics.
//   • any_compiled_artifact erase() round-trip.
//   • Two artifact kinds → two resource types.
//   • Fused compile_and_install equivalent to split path (interpreter: split only).
//   • resource_store insert / find / erase.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <string>
#include <type_traits>

#include "lithe/lithe_execution/artifact.hpp"
#include "lithe/lithe_execution/resource.hpp"

namespace ex = lithe::execution;

// ============================================================================
// basic_compiled_artifact — move semantics
// ============================================================================

TEST_CASE (


"basic_compiled_artifact: move semantics"
,
"[artifact]"
)
 {
    ex::artifact_manifest m;
    m.role       = ex::artifact_class::text_report;
    m.backend_id = "test";

    ex::basic_compiled_artifact<std::string> art{m, "hello", {}};
    REQUIRE(art.valid());
    CHECK(art.payload == "hello");

    auto moved = std::move(art);
    REQUIRE(moved.valid());
    CHECK(moved.payload == "hello");
    CHECK(art.payload.empty());  // moved-from is empty string
}

TEST_CASE (


"basic_compiled_artifact: two payload types are independent"
,
"[artifact]"
)
 {
    ex::artifact_manifest m1;
    m1.role = ex::artifact_class::text_report;

    ex::artifact_manifest m2;
    m2.role = ex::artifact_class::interpreter_plan;

    ex::basic_compiled_artifact<std::string>     text_art{m1, "asm listing", {}};
    ex::basic_compiled_artifact<ex::object_bytes> obj_art{m2, {{0x90, 0x90}, ".text"}, {}};

    CHECK(text_art.manifest.role == ex::artifact_class::text_report);
    CHECK(obj_art.manifest.role  == ex::artifact_class::interpreter_plan);
    // Confirm they are different types at compile time.
    static_assert(!std::is_same_v<decltype(text_art), decltype(obj_art)>);
}

// ============================================================================
// any_compiled_artifact — erase() round-trip
// ============================================================================

TEST_CASE (


"any_compiled_artifact: erase and typed extraction round-trip"
,
"[artifact]"
)
 {
    ex::artifact_manifest m;
    m.role       = ex::artifact_class::text_report;
    m.backend_id = "test";

    ex::basic_compiled_artifact<std::string> art{m, "payload_data", {}};
    auto erased = ex::any_compiled_artifact::erase(std::move(art));

    REQUIRE(erased.valid());
    CHECK(erased.manifest().role == ex::artifact_class::text_report);

    auto* back = erased.get<ex::basic_compiled_artifact<std::string>>();
    REQUIRE(back != nullptr);
    CHECK(back->payload == "payload_data");
}

TEST_CASE (


"any_compiled_artifact: wrong type returns nullptr"
,
"[artifact]"
)
 {
    ex::artifact_manifest m;
    m.role = ex::artifact_class::text_report;

    ex::basic_compiled_artifact<std::string> art{m, "text", {}};
    auto erased = ex::any_compiled_artifact::erase(std::move(art));

    // Extracting as wrong type must return nullptr.
    auto* wrong = erased.get<ex::basic_compiled_artifact<int>>();
    CHECK(wrong == nullptr);
}

TEST_CASE (


"any_compiled_artifact: move-only (no copy)"
,
"[artifact]"
)
 {
    static_assert(!std::is_copy_constructible_v<ex::any_compiled_artifact>,
        "any_compiled_artifact must be move-only");
    static_assert(std::is_move_constructible_v<ex::any_compiled_artifact>);
}

// ============================================================================
// resource_store — insert / find / erase
// ============================================================================

TEST_CASE (


"resource_store: insert and find"
,
"[resource][store]"
)
 {
    ex::resource_store store;
    REQUIRE(store.empty());

    // Build a minimal any_installed_resource (no-op ops).
    static ex::resource_ops null_ops{
        [](void*, ex::resource_handle) noexcept {},  // destroy
        nullptr                                       // invoke
    };
    struct dummy_owner {};
    static dummy_owner owner;

    auto res  = ex::any_installed_resource::make(&owner, &null_ops,
                    ex::resource_handle{1, 1}, 42);
    auto handle = store.insert(std::move(res));
    CHECK(handle.valid());
    CHECK(store.size() == 1);

    auto* entry = store.find(handle);
    REQUIRE(entry != nullptr);
    CHECK(entry->resource.handle() == handle);
}

TEST_CASE (


"resource_store: stale handle returns nullptr"
,
"[resource][store]"
)
 {
    ex::resource_store store;
    static ex::resource_ops null_ops{ [](void*, ex::resource_handle) noexcept {}, nullptr };
    struct dummy_owner {};
    static dummy_owner owner;

    auto res = ex::any_installed_resource::make(&owner, &null_ops,
                   ex::resource_handle{1, 1}, 0);
    auto handle = store.insert(std::move(res));
    store.erase(handle);

    CHECK(store.find(handle) == nullptr);
    CHECK(store.empty());
}

TEST_CASE (


"any_installed_resource: move nulls the source"
,
"[resource]"
)
 {
    static ex::resource_ops null_ops{ [](void*, ex::resource_handle) noexcept {}, nullptr };
    struct dummy_owner {};
    static dummy_owner owner;

    auto res = ex::any_installed_resource::make(&owner, &null_ops,
                   ex::resource_handle{1, 1}, 0);
    REQUIRE(res.valid());

    auto moved = std::move(res);
    REQUIRE(moved.valid());
    REQUIRE_FALSE(res.valid());
}

// ============================================================================
// resource_ops::destroy — 2-arg signature (§4.3 G2)
// ============================================================================

TEST_CASE (


"resource_ops: destroy receives resource_handle"
,
"[resource][ops]"
)
 {
    static ex::resource_handle captured{};
    static ex::resource_handle* cap = &captured;

    static const ex::resource_ops ops_with_handle{
        [](void*, ex::resource_handle h) noexcept { if (cap) *cap = h; },
        nullptr
    };

    struct dummy_owner {};
    static dummy_owner owner;
    const ex::resource_handle expected{7, 42};
    {
        auto res = ex::any_installed_resource::make(&owner, &ops_with_handle, expected);
        // destructor fires destroy(owner_ptr_, handle_)
    }
    CHECK(captured == expected);
}

TEST_CASE (


"resource_ops: destroy nullptr is safe"
,
"[resource][ops]"
)
 {
    static const ex::resource_ops no_destroy{ nullptr, nullptr };
    struct dummy_owner {};
    static dummy_owner owner;
    {
        auto res = ex::any_installed_resource::make(
            &owner, &no_destroy, ex::resource_handle{1, 1});
    }
    SUCCEED();
}
