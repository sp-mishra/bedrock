// =============================================================================
// test_lithe_plugin_abi.cpp — Plugin C ABI + backend_registry integration (§P15)
//
// Structure:
//   Static asserts: execution_mode_count == 7, out_of_proc == 6 (append-only).
//   Case 1 — register / unregister via backend_registry.
//   Case 2 — live-resource refcount blocks unregister; deferred on acquire().
//   Case 3 — stale-handle detection (acquire after erase → nullopt).
//   Case 4 — untrusted_sandbox cannot be relaxed.
//   Case 5 — forbidden out_of_proc mode rejected by compile_requirements.
//   Case 6 — signed-plugin: valid sig registers; bad sig refused.
//
// Tests append-only (new file).  No virtual, no macros in core.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <cstring>
#include <cstdint>

#include "lithe/plugin/lithe_plugin_abi.hpp"
#include "lithe/lithe_execution/registry.hpp"
#include "lithe/lithe_execution/capability.hpp"
#include "lithe/lithe_execution/foundation.hpp"
#include "lithe/lithe_rt/instance.hpp"

namespace ex = lithe::execution;
namespace plug = lithe::plugin;
namespace rt = lithe::rt;

// ============================================================================
// Static asserts: out_of_proc appended after device; count bumped to 7.
// impl-2 asserts (count==6, device==5) live in test_lithe_foundation.cpp — NOT
// edited here.  These are additional, append-only assertions.
// ============================================================================

static_assert(ex::execution_mode_count == 7,
              "out_of_proc appended: count must be 7 [P15]");
static_assert(static_cast<int>(ex::execution_mode::out_of_proc) == 6,
              "out_of_proc must be ordinal 6 [P15]");
static_assert(static_cast<int>(ex::execution_mode::device) == 5,
              "device must remain ordinal 5 (stability) [P15]");

// ============================================================================
// Shared test-double: minimal in-process plugin thunk table
// ============================================================================

namespace {
    // Trivial ABI shims — compile-time constants, no dynamic state needed.
    static lithe_abi_version test_abi_version() { return {LITHE_PLUGIN_ABI_MAJOR, 0, 0}; }

    static lithe_plugin_status test_descriptor(lithe_plugin_descriptor* out) {
        if (!out) return LITHE_PLUGIN_ERR_INIT_FAILED;
        std::memset(out, 0, sizeof(*out));
        std::strncpy(out->id, "test.plugin.unit", LITHE_PLUGIN_ID_MAX);
        std::strncpy(out->author, "test", LITHE_PLUGIN_AUTHOR_MAX);
        out->version_major = 1;
        return LITHE_PLUGIN_OK;
    }

    static lithe_backend_capability_bits test_capabilities() { return 0u; }
    static lithe_plugin_status test_register(void*) { return LITHE_PLUGIN_OK; }
    static void test_unregister(void*) {}

    static const lithe_plugin_thunk_table k_test_table = {
        test_abi_version,
        test_descriptor,
        test_capabilities,
        test_register,
        test_unregister,
    };

    // Thunk table with an ABI major mismatch.
    static lithe_abi_version bad_abi_version() { return {LITHE_PLUGIN_ABI_MAJOR + 1, 0, 0}; }
    static const lithe_plugin_thunk_table k_bad_abi_table = {
        bad_abi_version,
        test_descriptor,
        test_capabilities,
        test_register,
        test_unregister,
    };
} // namespace

// ============================================================================
// Case 1 — register / unregister via backend_registry
// ============================================================================

TEST_CASE (


"plugin: register and unregister via backend_registry [P15]"
,
"[plugin][abi][register]"
)
{
    ex::backend_registry reg;
    REQUIRE(reg.empty());

    auto result = plug::load_plugin(reg, &k_test_table);
    REQUIRE(result.has_value());
    REQUIRE(result->token.valid());
    CHECK(reg.size() == 1u);

    // Acquire a ref to verify the backend is live.
    auto ref = reg.acquire(result->token.handle());
    REQUIRE(ref.has_value());
    CHECK(ref->valid());
    CHECK(ref->id() == "test.plugin.unit");

    // Drop the ref, then drop the token → unregister.
    ref.reset();
    { auto t = std::move(result->token); } // token dtor → unregister
    CHECK(reg.empty());
}

// ============================================================================
// Case 2 — live-resource refcount blocks unregister; slot transitions to retiring
// ============================================================================

TEST_CASE (


"plugin: live refcount defers unregister [P15]"
,
"[plugin][abi][lifetime]"
)
{
    ex::backend_registry reg;

    auto result = plug::load_plugin(reg, &k_test_table);
    REQUIRE(result.has_value());
    const auto h = result->token.handle();

    // Acquire a backend_ref (pins the slot — increments refcount).
    auto ref = reg.acquire(h);
    REQUIRE(ref.has_value());
    CHECK(ref->live_refs() >= 1u);

    // Request unregister while a ref is held → deferred (returns false).
    const bool erased_immediately = reg.unregister(h);
    CHECK_FALSE(erased_immediately);

    // Slot is retiring → new acquire fails.
    auto ref2 = reg.acquire(h);
    CHECK_FALSE(ref2.has_value());

    // Drop the token (already unregistered above — token's destructor is safe).
    // Move the token out of result so its destructor doesn't double-unregister.
    { auto moved = std::move(result->token); }

    // Drop the outstanding ref → slot is fully released.
    ref.reset();
    // Registry should now be empty (lifetime block freed with the last shared_ptr).
    CHECK(reg.empty());
}

// ============================================================================
// Case 3 — stale-handle detection
// ============================================================================

TEST_CASE (


"plugin: stale handle returns nullopt [P15]"
,
"[plugin][abi][stale]"
)
{
    ex::backend_registry reg;

    auto result = plug::load_plugin(reg, &k_test_table);
    REQUIRE(result.has_value());
    const auto h = result->token.handle();

    // Verify the handle is valid while the slot exists.
    auto ref = reg.acquire(h);
    REQUIRE(ref.has_value());
    ref.reset();

    // Drop the token → slot erased.
    { auto t = std::move(result->token); }
    CHECK(reg.empty());

    // Acquire on the now-stale handle → nullopt.
    auto stale_ref = reg.acquire(h);
    CHECK_FALSE(stale_ref.has_value());
}

// ============================================================================
// Case 4 — untrusted_sandbox profile cannot be relaxed
// ============================================================================

TEST_CASE (


"plugin: untrusted_sandbox profile cannot be relaxed [P15]"
,
"[plugin][abi][sandbox]"
)
{
    ex::backend_registry reg;

    // Requirements with sandbox_mode cleared — violates the monotone floor.
    ex::compile_requirements reqs;
    reqs.security.sandbox_mode = false;

    auto result = plug::load_plugin(
        reg,
        &k_test_table,
        rt::execution_profile::untrusted_sandbox,
        reqs);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().detail.find("sandbox") != std::string_view::npos);

    // With sandbox_mode set — complies with the floor.
    reqs.security.sandbox_mode = true;
    auto ok = plug::load_plugin(
        reg,
        &k_test_table,
        rt::execution_profile::untrusted_sandbox,
        reqs);
    REQUIRE(ok.has_value());
}

// ============================================================================
// Case 5 — forbidden out_of_proc mode rejected via compile_requirements
// ============================================================================

TEST_CASE (


"plugin: forbidden out_of_proc rejected by mode_allowed [P15]"
,
"[plugin][abi][out_of_proc]"
)
{
    // forbidden_modes.set(out_of_proc) → mode_allowed(out_of_proc) == false.
    ex::compile_requirements reqs;
    reqs.forbidden_modes.set(ex::execution_mode::out_of_proc);

    CHECK_FALSE(reqs.mode_allowed(ex::execution_mode::out_of_proc));
    CHECK(reqs.mode_allowed(ex::execution_mode::interpret));
    CHECK(reqs.mode_allowed(ex::execution_mode::device));

    // A requirements set that only allows interpret + device still rejects out_of_proc.
    ex::compile_requirements allowed_set;
    allowed_set.allowed_modes.set(ex::execution_mode::interpret);
    allowed_set.allowed_modes.set(ex::execution_mode::device);
    CHECK_FALSE(allowed_set.mode_allowed(ex::execution_mode::out_of_proc));
    CHECK(allowed_set.mode_allowed(ex::execution_mode::interpret));
    CHECK(allowed_set.any_mode_allowed());
}

// ============================================================================
// Case 6 — signed-plugin: valid sig registers; bad sig refused
// ============================================================================

namespace {
    // Minimal plugin_signature_verifier that accepts only a specific byte.
    struct strict_verifier {
        std::uint8_t expected_byte;

        [[nodiscard]] static constexpr std::string_view provider_id() noexcept {
            return "test.plugin.strict_verifier";
        }

        [[nodiscard]] bool verify(const plug::plugin_signature_view& sig) const noexcept {
            return sig.valid() && sig.bytes[0] == expected_byte;
        }
    };

    static_assert(plug::plugin_signature_verifier<strict_verifier>);
} // namespace

TEST_CASE (


"plugin: signed-plugin verify gates registration [P15]"
,
"[plugin][abi][signature]"
)
{
    // No sig data → no_plugin_signature always passes.
    {
        ex::backend_registry reg;
        auto ok = plug::load_plugin(reg, &k_test_table);
        REQUIRE(ok.has_value());
        // token dropped at scope exit → unregister
    }

    // Valid signature byte (0xAB) passes.
    const std::uint8_t valid_byte = 0xABu;
    plug::plugin_signature_view valid_sig{{&valid_byte, 1}};
    strict_verifier sv{valid_byte};

    {
        ex::backend_registry reg;
        auto ok = plug::load_plugin(reg, &k_test_table, rt::execution_profile::managed_language,
                                    {}, valid_sig, sv);
        REQUIRE(ok.has_value());
        // token dropped at scope exit
    }

    // Wrong byte → refused before registration.
    {
        ex::backend_registry reg;
        const std::uint8_t wrong_byte = 0x00u;
        plug::plugin_signature_view bad_sig{{&wrong_byte, 1}};

        auto fail = plug::load_plugin(reg, &k_test_table, rt::execution_profile::managed_language,
                                      {}, bad_sig, sv);
        REQUIRE_FALSE(fail.has_value());
        CHECK(fail.error().detail.find("signature") != std::string_view::npos);
    }

    // ABI mismatch → also refused.
    {
        ex::backend_registry reg;
        auto abi_fail = plug::load_plugin(reg, &k_bad_abi_table);
        REQUIRE_FALSE(abi_fail.has_value());
        CHECK(abi_fail.error().detail.find("ABI") != std::string_view::npos);
    }
}
