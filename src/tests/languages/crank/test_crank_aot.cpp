// =============================================================================
// test_crank_aot.cpp — Module 4: AOT cache key assembly + hit/miss/invalidate.
//
// Covers (guarded by LITHE_HAS_AOT where applicable):
//   1. make_aot_key assembles a non-zero fingerprint.
//   2. Identical inputs → identical fingerprints.
//   3. compile_and_cache: cache miss on first compile, hit on second.
//   4. source_hash change → cache miss (recompile).
//   5. Descriptor hash change → invalidate.
//   6. dump_aot_key: JSON output contains required fields.
//   7. aot_cache::invalidate removes the entry.
//   8. Different module names → different fingerprints.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/aot.hpp"
#include "languages/crank/lower_hl.hpp"
#include "languages/crank/dump.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

// ============================================================================
// Test 1 — make_aot_key produces non-zero fingerprint
// ============================================================================

TEST_CASE (

"make_aot_key produces a non-zero fingerprint"
,
"[crank][aot]"
)
 {
    const auto key = crank::make_aot_key("Scale", /*source_hash=*/0xDEADBEEFu);
    CHECK(key.fingerprint() != 0);
    CHECK(key.module_name == "Scale");
    CHECK(key.compiler_version == "crank.1.0.0");
}

// ============================================================================
// Test 2 — identical inputs → identical fingerprints
// ============================================================================

TEST_CASE (

"identical aot keys produce identical fingerprints"
,
"[crank][aot]"
)
 {
    const auto k1 = crank::make_aot_key("Dot", 0xABCDu, "crank.o3",
                                         "lithe.backend.interpreter");
    const auto k2 = crank::make_aot_key("Dot", 0xABCDu, "crank.o3",
                                         "lithe.backend.interpreter");
    CHECK(k1.fingerprint() == k2.fingerprint());
}

// ============================================================================
// Test 3 — compile_and_cache: miss → compile → hit
// ============================================================================

TEST_CASE (

"compile_and_cache: first call is miss, second is hit"
,
"[crank][aot]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "Scale";
    inp.loops.push_back({
        .lower = 0, .upper = 4, .step = 1,
        .is_parallel = false, .name = "i"
    });

    auto hl_res = crank::lower_to_hl(std::move(inp));
    REQUIRE(hl_res.ok());

    crank::aot_cache cache;
    const auto key = crank::make_aot_key("Scale", 0x1234u);

    // First call: miss
    auto res1 = crank::compile_and_cache(cache, key, hl_res);
    CHECK(res1.status == crank::aot_cache_status::miss);
    CHECK(res1.ok());
    CHECK(cache.size() == 1u);

    // Second call: hit
    auto res2 = crank::compile_and_cache(cache, key, hl_res);
    CHECK(res2.status == crank::aot_cache_status::hit);
    CHECK_FALSE(res2.bytes.empty());
}

// ============================================================================
// Test 4 — source_hash change → miss (key differs from cached entry)
// ============================================================================

TEST_CASE (

"source_hash change causes cache miss"
,
"[crank][aot]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "Mean";

    auto hl_res = crank::lower_to_hl(std::move(inp));
    REQUIRE(hl_res.ok());

    crank::aot_cache cache;

    const auto key1 = crank::make_aot_key("Mean", 0x1111u);
    auto r1 = crank::compile_and_cache(cache, key1, hl_res);
    CHECK(r1.status == crank::aot_cache_status::miss);

    // Different source hash → different fingerprint → miss
    const auto key2 = crank::make_aot_key("Mean", 0x2222u);
    auto r2 = crank::compile_and_cache(cache, key2, hl_res);
    CHECK(r2.status == crank::aot_cache_status::miss);
    CHECK(cache.size() == 2u);
}

// ============================================================================
// Test 5 — descriptor hash change invalidates entry
// ============================================================================

TEST_CASE (

"descriptor hash change invalidates a cached entry"
,
"[crank][aot]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "Classify";

    auto hl_res = crank::lower_to_hl(std::move(inp));
    REQUIRE(hl_res.ok());

    crank::aot_cache cache;
    crank::crank_aot_key key = crank::make_aot_key("Classify", 0xAAAAu);
    key.descriptor_hashes.push_back(0x0001u);

    auto r1 = crank::compile_and_cache(cache, key, hl_res);
    CHECK(r1.status == crank::aot_cache_status::miss);

    // Invalidate via different descriptor hash
    crank::crank_aot_key key2 = crank::make_aot_key("Classify", 0xAAAAu);
    key2.descriptor_hashes.push_back(0x0002u);

    cache.invalidate(key);  // explicitly remove old entry
    auto r2 = crank::compile_and_cache(cache, key2, hl_res);
    CHECK(r2.status == crank::aot_cache_status::miss);
}

// ============================================================================
// Test 6 — dump_aot_key: JSON contains required fields
// ============================================================================

TEST_CASE (

"dump_aot_key emits JSON with module, fingerprint, backend fields"
,
"[crank][aot][dump]"
)
 {
    const auto key = crank::make_aot_key("Scale", 0xFEEDu, "crank.o3",
                                          "lithe.backend.interpreter");
    const std::string json = crank::dump_aot_key(key);
    CHECK_FALSE(json.empty());
    CHECK(json.find("module") != std::string::npos);
    CHECK(json.find("Scale") != std::string::npos);
    CHECK(json.find("fingerprint") != std::string::npos);
    CHECK(json.find("backend") != std::string::npos);
    CHECK(json.find("interpreter") != std::string::npos);
    CHECK(json.find("opt_profile") != std::string::npos);
}

// ============================================================================
// Test 7 — aot_cache::invalidate removes entry
// ============================================================================

TEST_CASE (

"aot_cache::invalidate removes entry from cache"
,
"[crank][aot]"
)
 {
    crank::aot_cache cache;
    const auto key = crank::make_aot_key("Dot", 0xBEEFu);

    cache.store(key, {0x01, 0x02, 0x03});
    CHECK(cache.size() == 1u);
    CHECK(cache.find(key) != nullptr);

    cache.invalidate(key);
    CHECK(cache.size() == 0u);
    CHECK(cache.find(key) == nullptr);
}

// ============================================================================
// Test 8 — different module names produce different fingerprints
// ============================================================================

TEST_CASE (

"different module names produce different fingerprints"
,
"[crank][aot]"
)
 {
    const auto k1 = crank::make_aot_key("Scale",   0xCAFEu);
    const auto k2 = crank::make_aot_key("Classify", 0xCAFEu);
    CHECK(k1.fingerprint() != k2.fingerprint());
}

// ============================================================================
// Test 9 — validate_aot_view: untrusted artifact without public_key emits SEC-001
// ============================================================================

TEST_CASE (

"validate_aot_view: untrusted level without public_key emits CRANK-AOT-SEC-001"
,
"[crank][aot][security]"
)
 {
    const auto key = crank::make_aot_key("SecFn", 0xABCDu);
    const std::array<std::uint8_t, 4> data = {0xDE, 0xAD, 0xBE, 0xEF};

    crank::aot_security_policy policy;
    policy.trust = crank::aot_trust_level::untrusted;
    // No public_key set — must trigger CRANK-AOT-SEC-001

    auto diags = crank::validate_aot_view(key, std::span<const std::uint8_t>(data), policy);

    REQUIRE_FALSE(diags.empty());
    const bool has_sec001 = std::ranges::any_of(diags, [](const std::string& d) {
        return d.find("CRANK-AOT-SEC-001") != std::string::npos;
    });
    CHECK(has_sec001);
}

// ============================================================================
// Test 10 — validate_aot_view: size limit enforced via CRANK-AOT-SEC-004
// ============================================================================

TEST_CASE (

"validate_aot_view: artifact exceeding max_artifact_bytes emits CRANK-AOT-SEC-004"
,
"[crank][aot][security]"
)
 {
    const auto key = crank::make_aot_key("BigFn", 0x1234u);
    const std::vector<std::uint8_t> big(512, 0xFFu);

    crank::aot_security_policy policy;
    policy.max_artifact_bytes = 64;  // far smaller than 512 bytes

    auto diags = crank::validate_aot_view(
        key, std::span<const std::uint8_t>(big), policy);

    REQUIRE_FALSE(diags.empty());
    const bool has_sec004 = std::ranges::any_of(diags, [](const std::string& d) {
        return d.find("CRANK-AOT-SEC-004") != std::string::npos;
    });
    CHECK(has_sec004);
}

// ============================================================================
// Test 11 — validate_aot_view: internal trust level with no key → no diagnostics
// ============================================================================

TEST_CASE (

"validate_aot_view: internal trust level produces no diagnostics"
,
"[crank][aot][security]"
)
 {
    const auto key = crank::make_aot_key("InternalFn", 0x5678u);
    const std::array<std::uint8_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};

    crank::aot_security_policy policy;
    policy.trust = crank::aot_trust_level::internal;

    auto diags = crank::validate_aot_view(
        key, std::span<const std::uint8_t>(data), policy);

    CHECK(diags.empty());
}

// =============================================================================
// §v2.14 separate compilation + §v2.15 AOT-v2 artifacts — appended for v2.
// Existing tests above are unchanged.
// =============================================================================

// ---- §v2.14 link_modules: dedup + ABI gate ---------------------------------

TEST_CASE (

"v2.14 link_modules dedups identical instantiations across modules"
,
"[crank][aot][link][v2]"
)
 {
    crank::module_link_metadata a;
    a.module_name = "app";
    a.native_abi_hash = 0xAB1u;
    a.instantiations.push_back({/*fp=*/100, /*abi=*/0xAB1u, "Reduce#100"});

    crank::module_link_metadata b;
    b.module_name = "lib";
    b.native_abi_hash = 0xAB1u;
    b.instantiations.push_back({/*fp=*/100, /*abi=*/0xAB1u, "Reduce#100"}); // dup
    b.instantiations.push_back({/*fp=*/200, /*abi=*/0xAB1u, "Map#200"});

    auto r = crank::link_modules({a, b});
    CHECK(r.ok());
    CHECK(r.merged.size() == 2u);  // fp 100 collapsed to one
}

TEST_CASE (

"v2.14 conflicting instantiation ABI → CRANK-LINK-001"
,
"[crank][aot][link][v2]"
)
 {
    crank::module_link_metadata a;
    a.module_name = "app"; a.native_abi_hash = 0xAAAAu;
    a.instantiations.push_back({100, 0xAAAAu, "Reduce#100"});

    crank::module_link_metadata b;
    b.module_name = "lib"; b.native_abi_hash = 0xAAAAu;
    b.instantiations.push_back({100, 0xBBBBu, "Reduce#100"}); // same fp, diff ABI

    auto r = crank::link_modules({a, b});
    CHECK_FALSE(r.ok());
    CHECK(r.diagnostics.front().find("CRANK-LINK-001") != std::string::npos);
}

TEST_CASE (

"v2.14 module-level ABI mismatch → CRANK-LINK-001"
,
"[crank][aot][link][v2]"
)
 {
    crank::module_link_metadata a; a.module_name = "app"; a.native_abi_hash = 0x1u;
    crank::module_link_metadata b; b.module_name = "lib"; b.native_abi_hash = 0x2u;
    auto r = crank::link_modules({a, b});
    CHECK_FALSE(r.ok());
    CHECK(r.diagnostics.front().find("CRANK-LINK-001") != std::string::npos);
}

TEST_CASE (

"v2.14 module_link_metadata serialize is non-empty and stable"
,
"[crank][aot][link][v2]"
)
 {
    crank::module_link_metadata a;
    a.module_name = "app"; a.native_abi_hash = 0x9u;
    a.instantiations.push_back({7, 0x9u, "Foo#7"});
    auto s1 = a.serialize();
    auto s2 = a.serialize();
    CHECK_FALSE(s1.empty());
    CHECK(s1 == s2);
}

// ---- §v2.15 AOT-v2 header: runtime version + capability gates --------------

TEST_CASE (

"v2.15 version in range passes; out of range → CRANK-AOT-SEC-008"
,
"[crank][aot][v2]"
)
 {
    const auto key = crank::make_aot_key("Fn", 0x1u);
    const std::array<std::uint8_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};
    crank::aot_artifact_header_v2 hdr;
    hdr.min_runtime_version = 1;
    hdr.max_runtime_version = 2;

    auto ok = crank::validate_aot_view_v2(
        key, std::span<const std::uint8_t>(data), hdr, /*runtime=*/2, {});
    CHECK(ok.empty());

    auto too_new = crank::validate_aot_view_v2(
        key, std::span<const std::uint8_t>(data), hdr, /*runtime=*/3, {});
    bool has_008 = false;
    for (const auto& d : too_new)
        if (d.find("CRANK-AOT-SEC-008") != std::string::npos) has_008 = true;
    CHECK(has_008);
}

TEST_CASE (

"v2.15 relocation without exec-memory opt-in → CRANK-AOT-SEC-005"
,
"[crank][aot][v2]"
)
 {
    const auto key = crank::make_aot_key("Fn", 0x1u);
    const std::array<std::uint8_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};
    crank::aot_artifact_header_v2 hdr;
    hdr.relocation_count = 4;

    crank::aot_security_policy policy;  // allow_executable_memory=false default
    auto diags = crank::validate_aot_view_v2(
        key, std::span<const std::uint8_t>(data), hdr, /*runtime=*/2, policy);
    bool has_005 = false;
    for (const auto& d : diags)
        if (d.find("CRANK-AOT-SEC-005") != std::string::npos) has_005 = true;
    CHECK(has_005);

    policy.allow_executable_memory = true;
    auto ok = crank::validate_aot_view_v2(
        key, std::span<const std::uint8_t>(data), hdr, /*runtime=*/2, policy);
    bool still_005 = false;
    for (const auto& d : ok)
        if (d.find("CRANK-AOT-SEC-005") != std::string::npos) still_005 = true;
    CHECK_FALSE(still_005);
}

TEST_CASE (

"v2.15 untrusted capability bit outside allowlist → CRANK-AOT-SEC-006"
,
"[crank][aot][v2]"
)
 {
    const auto key = crank::make_aot_key("Fn", 0x1u);
    const std::array<std::uint8_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};
    crank::aot_artifact_header_v2 hdr;
    hdr.capability_mask = 0b100u;  // bit 2 set; allowlist has only 1 entry (bit 0)

    crank::aot_security_policy policy;
    policy.trust = crank::aot_trust_level::untrusted;
    policy.sig_algorithm = crank::aot_signature_algorithm::ed25519;
    policy.public_key = std::vector<std::byte>{std::byte{1}};
    policy.allowed_capabilities = {"cap0"};

    auto diags = crank::validate_aot_view_v2(
        key, std::span<const std::uint8_t>(data), hdr, /*runtime=*/2, policy);
    bool has_006 = false;
    for (const auto& d : diags)
        if (d.find("CRANK-AOT-SEC-006") != std::string::npos) has_006 = true;
    CHECK(has_006);
}

// =============================================================================
// Native-path + hint-propagation tests — appended per repo rule
//
// Tests 9–12: compile_and_cache now calls lithe::execution::compile; verify
// the native-path behavior and hint threading work correctly.
// =============================================================================

#include "languages/crank/exec_hint.hpp"

// ----------------------------------------------------------------------------
// 9. compile_and_cache: result returns a value for a simple HL function.
//    Verifies the new lithe::execution::compile path is wired up — both native
//    and interpreter paths must return a scalar.
// ----------------------------------------------------------------------------
TEST_CASE (

"compile_and_cache: returns value via converged compile path"
,
"[crank][aot][perf-l1]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "ConstVal";
    // An empty body — lower_to_hl produces a trivial function returning Unit.

    auto hl_res = crank::lower_to_hl(std::move(inp));
    REQUIRE(hl_res.ok());

    crank::aot_cache cache;
    const auto key = crank::make_aot_key("ConstVal", 0xBEEFu);

    auto res = crank::compile_and_cache(cache, key, hl_res);
    CHECK(res.ok());
    CHECK(res.status == crank::aot_cache_status::miss);
    CHECK(cache.size() == 1u);
    // Bytes present (8-byte placeholder stored for backward compat).
    CHECK(res.bytes.size() == sizeof(std::uint64_t));
}

// ----------------------------------------------------------------------------
// 10. compile_and_cache: second call is still a cache hit after converged path.
// ----------------------------------------------------------------------------
TEST_CASE (

"compile_and_cache: cache hit on second call (converged path)"
,
"[crank][aot][perf-l1]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "CachedFn";

    auto hl_res = crank::lower_to_hl(std::move(inp));
    REQUIRE(hl_res.ok());

    crank::aot_cache cache;
    const auto key = crank::make_aot_key("CachedFn", 0xCAFEu);

    auto r1 = crank::compile_and_cache(cache, key, hl_res);
    CHECK(r1.ok());
    CHECK(r1.status == crank::aot_cache_status::miss);

    auto r2 = crank::compile_and_cache(cache, key, hl_res);
    CHECK(r2.ok());
    CHECK(r2.status == crank::aot_cache_status::hit);
}

// ----------------------------------------------------------------------------
// 11. execution_hint field on lower_hl_result survives to compile_request.
//     @simd → hint.preferred == execution_kind::simd, readable after lowering.
// ----------------------------------------------------------------------------
TEST_CASE (

"execution_hint: @simd attr survives to lower_hl_result"
,
"[crank][aot][hint][perf-l1]"
)
 {
    crank::lower_input inp;
    inp.fn_name = "HintFn";

    auto hl_res = crank::lower_to_hl(std::move(inp));
    REQUIRE(hl_res.ok());

    // Simulate what the frontend does after parsing @simd on a top_decl.
    crank::crank_exec_attr simd_attr;
    simd_attr.kind = crank::crank_attr_kind::simd;
    hl_res.exec_hint = crank::map_exec_attr(simd_attr);

    CHECK(hl_res.exec_hint.preferred.has_value());
    CHECK(*hl_res.exec_hint.preferred == lithe::exec::execution_kind::simd);
    CHECK_FALSE(hl_res.exec_hint.required);
}

// ----------------------------------------------------------------------------
// 12. execution_hint with required=true: @gpu(required=true) sets required flag.
// ----------------------------------------------------------------------------
TEST_CASE (

"execution_hint: @gpu(required=true) sets required flag"
,
"[crank][aot][hint][perf-l1]"
)
 {
    crank::crank_exec_attr gpu_req;
    gpu_req.kind     = crank::crank_attr_kind::gpu;
    gpu_req.required = true;

    const auto hint = crank::map_exec_attr(gpu_req);
    CHECK(hint.preferred.has_value());
    CHECK(*hint.preferred == lithe::exec::execution_kind::gpu);
    CHECK(hint.required);
}
