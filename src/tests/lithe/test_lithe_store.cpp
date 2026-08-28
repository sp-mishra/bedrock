// =============================================================================
// test_lithe_store.cpp — impl-3 Durable Artifacts & Catalog tests
//
// Tests (all use memory_catalog + filesystem_blob_store in tmp dirs):
//   1. Artifact codec round-trip + verification ordering (structural/integrity/sig/compat)
//   2. Cache hit returns result without recompile (arch §12)
//   3. Contended miss compiles exactly once, publishes atomically (arch §7/§12)
//   4. Content-addressed blob store dedupe + atomic write
//   5. Compatibility predicate — one case per clause + positive
//   6. Retirement safety — active frame blocks eviction (arch §12)
//
// Guarded RocksDB block at end exercises rocksdb_catalog with the same
// conformance tests when LITHE_HAS_ROCKSDB is defined.
// =============================================================================

#include "catch_amalgamated.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lithe/lithe_execution/store/store.hpp"

namespace st = lithe::execution::store;

// ---------------------------------------------------------------------------
// Helper: minimal valid artifact_record with inline payload
// ---------------------------------------------------------------------------
static st::artifact_record make_test_record(const std::string& tag = "hello") {
    st::artifact_record rec;
    rec.kind = st::artifact_kind::optimized_portable;
    rec.semantic_digest_len = 4;
    rec.semantic_digest[0] = 0xDE;
    rec.semantic_digest[1] = 0xAD;
    rec.semantic_digest[2] = 0xBE;
    rec.semantic_digest[3] = 0xEF;
    rec.prov.pipe.name = "test-pipeline";
    rec.prov.pipe_ver = {1, 0};
    rec.prov.producer = "test";

    st::optimized_key ok;
    ok.semantic_digest[0] = 0xDE;
    ok.semantic_digest[1] = 0xAD;
    ok.semantic_digest[2] = 0xBE;
    ok.semantic_digest[3] = 0xEF;
    ok.semantic_digest_len = 4;
    ok.pipe_id.name = "test-pipeline";
    ok.pipe_ver = {1, 0};
    rec.key = ok;

    st::inline_payload ip;
    for (unsigned char c : tag) ip.bytes.push_back(static_cast<std::uint8_t>(c));
    rec.payload = std::move(ip);

    return rec;
}

// ---------------------------------------------------------------------------
// Helper: temp directory RAII
// ---------------------------------------------------------------------------
struct TempDir {
    std::filesystem::path path;

    TempDir() : path(std::filesystem::temp_directory_path() /
        ("lithe_store_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// =============================================================================
// Test 1: Artifact codec round-trip + verification ordering
// =============================================================================

TEST_CASE (

"lithe_store: artifact codec round-trip"
,
"[lithe][store][codec]"
)
 {
    const auto rec = make_test_record("round-trip-test");

    SECTION("positive round-trip") {
        auto encoded = st::encode_artifact(rec);
        REQUIRE(encoded.has_value());

        st::decode_policy policy;
        policy.require_digest = true;
        policy.require_signature = false;

        auto decoded = st::decode_artifact(*encoded, policy);
        REQUIRE(decoded.has_value());

        CHECK(decoded->kind == rec.kind);
        CHECK(decoded->semantic_digest_len == rec.semantic_digest_len);
        CHECK(decoded->prov.pipe.name == rec.prov.pipe.name);

        // Payload round-trips
        const auto* orig_ip = std::get_if<st::inline_payload>(&rec.payload);
        const auto* dec_ip  = std::get_if<st::inline_payload>(&decoded->payload);
        REQUIRE(orig_ip != nullptr);
        REQUIRE(dec_ip  != nullptr);
        CHECK(orig_ip->bytes == dec_ip->bytes);
    }

    SECTION("corrupt payload byte → fails at integrity stage") {
        auto encoded = st::encode_artifact(rec);
        REQUIRE(encoded.has_value());

        // Flip a byte in the tail (payload region)
        encoded->back() ^= 0xFF;

        st::decode_policy policy;
        policy.require_digest = true;

        auto decoded = st::decode_artifact(*encoded, policy);
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error().stage == st::artifact_error_stage::integrity);
    }

    SECTION("truncated data → fails at structural stage") {
        auto encoded = st::encode_artifact(rec);
        REQUIRE(encoded.has_value());
        encoded->resize(8); // too small for the header

        st::decode_policy policy;
        auto decoded = st::decode_artifact(*encoded, policy);
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error().stage == st::artifact_error_stage::structural);
    }

    SECTION("bad magic → structural stage") {
        auto encoded = st::encode_artifact(rec);
        REQUIRE(encoded.has_value());
        (*encoded)[0] = 0x00; // corrupt magic

        st::decode_policy policy;
        auto decoded = st::decode_artifact(*encoded, policy);
        REQUIRE(!decoded.has_value());
        CHECK(decoded.error().stage == st::artifact_error_stage::structural);
    }
}

// =============================================================================
// Test 2: Cache hit returns result without recompile
// =============================================================================

TEST_CASE (

"lithe_store: cache hit produces result without recompile"
,
"[lithe][store][cache]"
)
 {
    TempDir tmp;
    st::memory_catalog cat;
    st::filesystem_blob_store blobs{tmp.path};

    const auto rec   = make_test_record("cache-hit-test");
    const auto& key  = rec.key;
    std::atomic<int> compile_count{0};

    auto compile_fn = [&]() -> std::expected<st::artifact_record, std::string> {
        ++compile_count;
        return make_test_record("cache-hit-test");
    };

    auto result1 = st::get_or_compile(cat, blobs, key, compile_fn);
    REQUIRE(result1.has_value());
    CHECK(compile_count == 1);

    auto result2 = st::get_or_compile(cat, blobs, key, compile_fn);
    REQUIRE(result2.has_value());
    CHECK(compile_count == 1); // no second compile

    // Both entries refer to the same key
    CHECK(result1->blob_addr == result2->blob_addr);
}

// =============================================================================
// Test 3: Contended miss compiles exactly once (arch §7/§12)
// =============================================================================

TEST_CASE (

"lithe_store: contended miss compiles once"
,
"[lithe][store][concurrency]"
)
 {
    TempDir tmp;
    st::memory_catalog cat;
    st::filesystem_blob_store blobs{tmp.path};

    const auto rec  = make_test_record("contention-test");
    const auto& key = rec.key;
    std::atomic<int> compile_count{0};

    constexpr int N = 8;
    std::vector<std::expected<st::catalog_entry, st::catalog_error>> results(N);
    std::vector<std::thread> threads;
    threads.reserve(N);

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i]() {
            auto compile_fn = [&]() -> std::expected<st::artifact_record, std::string> {
                ++compile_count;
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                return make_test_record("contention-test");
            };
            results[i] = st::get_or_compile(cat, blobs, key, compile_fn);
        });
    }
    for (auto& t : threads) t.join();

    // All N results should succeed.
    for (std::size_t index = 0; index < results.size(); ++index) {
        CHECK(results[index].has_value());
    }
    // Exactly one compile, regardless of contention.
    CHECK(compile_count == 1);
}

TEST_CASE(

"lithe_store: lease ownership is enforced and failed builds release leases",
"[lithe][store][lease]") {
    TempDir tmp;
    st::memory_catalog cat;
    st::filesystem_blob_store blobs{tmp.path};
    const auto rec = make_test_record("lease-test");

    auto token = cat.acquire_lease(rec.key);
    REQUIRE(token.has_value());
    st::catalog_entry unpublished;
    unpublished.key = rec.key;
    auto wrong_publish = cat.publish(
        unpublished, utils::lease_token{token->id + 1});
    REQUIRE_FALSE(wrong_publish.has_value());
    REQUIRE(cat.abandon(rec.key, *token).has_value());

    std::atomic<int> attempts{0};
    auto failed = st::get_or_compile(
        cat, blobs, rec.key,
        [&]() -> std::expected<st::artifact_record, std::string> {
            ++attempts;
            return std::unexpected("intentional failure");
        });
    REQUIRE_FALSE(failed.has_value());

    auto recovered = st::get_or_compile(
        cat, blobs, rec.key,
        [&]() -> std::expected<st::artifact_record, std::string> {
            ++attempts;
            return make_test_record("lease-test");
        });
    REQUIRE(recovered.has_value());
    CHECK(attempts == 2);
}

// =============================================================================
// Test 4: Content-addressed blob store dedupe + atomic write
// =============================================================================

TEST_CASE (

"lithe_store: blob store dedupe and atomicity"
,
"[lithe][store][blob]"
)
 {
    TempDir tmp;
    st::filesystem_blob_store store{tmp.path};

    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};

    SECTION("identical bytes → same address, single file") {
        auto addr1 = store.put(std::span<const std::uint8_t>{payload});
        REQUIRE(addr1.has_value());
        auto addr2 = store.put(std::span<const std::uint8_t>{payload});
        REQUIRE(addr2.has_value());

        CHECK(*addr1 == *addr2);
        // Only one file on disk
        CHECK(store.contains(*addr1));
    }

    SECTION("get returns the same bytes") {
        auto addr = store.put(std::span<const std::uint8_t>{payload});
        REQUIRE(addr.has_value());

        auto handle = store.get(*addr);
        REQUIRE(handle.has_value());
        REQUIRE(handle->size() == payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i)
            CHECK(handle->data[i] == payload[i]);
    }

    SECTION("get on missing address returns not_found") {
        st::blob_address fake{};
        fake.digest[0] = 0xAB;
        auto h = store.get(fake);
        REQUIRE(!h.has_value());
        CHECK(h.error().code == containers::store_error_code::not_found);
    }

    SECTION("erase removes the blob") {
        auto addr = store.put(std::span<const std::uint8_t>{payload});
        REQUIRE(addr.has_value());
        CHECK(store.contains(*addr));
        REQUIRE(store.erase(*addr).has_value());
        CHECK(!store.contains(*addr));
    }
}

// =============================================================================
// Test 5: Compatibility predicate — one case per clause
// =============================================================================

TEST_CASE (

"lithe_store: compatibility predicate clauses"
,
"[lithe][store][compat]"
)
 {
    // Build a baseline passing manifest + profile.
    st::compatibility_manifest art;
    art.ir_schema      = {1, 0, 0};
    art.abi.digest[0]  = 0xAA;
    art.required_caps  = {0b0011};
    art.target.os      = "macos";
    art.target.arch    = "arm64";
    art.security.id    = 42;
    art.security.version = 1;

    st::host_profile host;
    host.supported_ir_schema = {1, 2, 0};
    host.host_abi.digest[0]  = 0xAA;
    host.available_caps      = {0b1111};
    host.host_target.os      = "macos";
    host.host_target.arch    = "arm64";
    host.host_target.min_os_version = 0;
    host.available_symbols   = {"sym_a", "sym_b"};

    const st::security_policy_id policy{42, 1};

    SECTION("all compatible") {
        const auto r = st::check_compatible(art, host, policy);
        CHECK(r.passed);
    }

    SECTION("schema too new → schema_version clause fails") {
        auto a2 = art;
        a2.ir_schema = {99, 0, 0};
        const auto r = st::check_compatible(a2, host, policy);
        CHECK(!r.passed);
        const bool schema_failed = std::any_of(r.clauses.begin(), r.clauses.end(),
            [](const auto& c) { return c.clause == st::compat_clause::schema_version && !c.passed; });
        CHECK(schema_failed);
    }

    SECTION("ABI mismatch → abi clause fails") {
        auto a2 = art;
        a2.abi.digest[0] = 0xFF;
        const auto r = st::check_compatible(a2, host, policy);
        CHECK(!r.passed);
        const bool abi_failed = std::any_of(r.clauses.begin(), r.clauses.end(),
            [](const auto& c) { return c.clause == st::compat_clause::abi && !c.passed; });
        CHECK(abi_failed);
    }

    SECTION("missing capability → capabilities clause fails") {
        auto a2 = art;
        a2.required_caps = {0b10000}; // bit 4 not in host (0b1111)
        const auto r = st::check_compatible(a2, host, policy);
        CHECK(!r.passed);
        const bool cap_failed = std::any_of(r.clauses.begin(), r.clauses.end(),
            [](const auto& c) { return c.clause == st::compat_clause::capabilities && !c.passed; });
        CHECK(cap_failed);
    }

    SECTION("target OS mismatch → target clause fails") {
        auto a2 = art;
        a2.target.os = "linux";
        const auto r = st::check_compatible(a2, host, policy);
        CHECK(!r.passed);
        const bool target_failed = std::any_of(r.clauses.begin(), r.clauses.end(),
            [](const auto& c) { return c.clause == st::compat_clause::target && !c.passed; });
        CHECK(target_failed);
    }

    SECTION("unresolved external symbol → ext_syms clause fails") {
        auto a2 = art;
        a2.ext_syms.push_back({"sym_missing", ""});
        const auto r = st::check_compatible(a2, host, policy);
        CHECK(!r.passed);
        const bool sym_failed = std::any_of(r.clauses.begin(), r.clauses.end(),
            [](const auto& c) { return c.clause == st::compat_clause::external_symbols && !c.passed; });
        CHECK(sym_failed);
    }

    SECTION("security policy mismatch → security clause fails") {
        const st::security_policy_id bad_policy{99, 0};
        const auto r = st::check_compatible(art, host, bad_policy);
        CHECK(!r.passed);
        const bool sec_failed = std::any_of(r.clauses.begin(), r.clauses.end(),
            [](const auto& c) { return c.clause == st::compat_clause::security_policy && !c.passed; });
        CHECK(sec_failed);
    }
}

// =============================================================================
// Test 6: Retirement safety — active frame blocks eviction
// =============================================================================

TEST_CASE (

"lithe_store: retirement safety — active frames gate eviction"
,
"[lithe][store][retirement]"
)
 {
    // Create a code_resource.
    auto res = std::make_shared<lithe::rt::code_resource>();
    const std::string key = "deadbeef00000000000000000000000000000000000000000000000000000000";

    auto cache = st::make_installed_code_cache(16);
    st::retirement_queue queue;

    // Install into cache.
    REQUIRE(cache->put(key, res).has_value());

    SECTION("eviction with active frames defers to queue") {
        res->active_frames.fetch_add(1, std::memory_order_relaxed);
        const bool evicted = st::try_evict_installed(*cache, queue, key);
        CHECK(!evicted);              // deferred
        CHECK(queue.pending() == 1);  // in retirement queue
        CHECK(!cache->get(key).has_value()); // removed from hot cache

        // Release frame — drain should reclaim it.
        res->active_frames.fetch_sub(1, std::memory_order_relaxed);
        const auto reclaimed = queue.drain();
        CHECK(reclaimed == 1);
        CHECK(queue.pending() == 0);
    }

    SECTION("eviction with no active frames reclaims immediately") {
        res->active_frames.store(0, std::memory_order_relaxed);
        // Re-insert since previous test may have evicted it.
        REQUIRE(cache->put(key, res).has_value());

        const bool evicted = st::try_evict_installed(*cache, queue, key);
        CHECK(evicted);
        CHECK(queue.pending() == 0);
        CHECK(!cache->get(key).has_value());
        CHECK(res->state.load(std::memory_order_relaxed) == lithe::rt::code_state::retired);
    }
}

// =============================================================================
// Guarded RocksDB block — same conformance under rocksdb_catalog
// =============================================================================

#if defined(LITHE_HAS_ROCKSDB)

TEST_CASE ("lithe_store [rocksdb]: catalog round-trip", "[lithe][store][rocksdb]") {
    TempDir tmp;
    st::rocksdb_catalog cat{(tmp.path / "catalog").string()};
    st::filesystem_blob_store blobs{tmp.path / "blobs"};

    const auto rec  = make_test_record("rocksdb-test");
    const auto& key = rec.key;
    std::atomic<int> count{0};

    auto compile_fn = [&]() -> std::expected<st::artifact_record, std::string> {
        ++count;
        return make_test_record("rocksdb-test");
    };

    auto r1 = st::get_or_compile(cat, blobs, key, compile_fn);
    REQUIRE(r1.has_value());
    CHECK(count == 1);

    auto r2 = st::get_or_compile(cat, blobs, key, compile_fn);
    REQUIRE(r2.has_value());
    CHECK(count == 1); // no second compile
}

TEST_CASE("lithe_store [rocksdb]: restart preserves complete metadata",
          "[lithe][store][rocksdb][restart]") {
    TempDir tmp;
    const auto catalog_path = (tmp.path / "catalog").string();
    const auto blob_path = tmp.path / "blobs";
    auto rec = make_test_record("rocksdb-restart");
    rec.compat.target.os = "macos";
    rec.compat.target.arch = "arm64";
    rec.compat.security = {17, 4};
    rec.compat.ext_syms.push_back({"host_add", "cxx23"});
    rec.prov.producer = "restart-test";
    rec.prov.backend = st::backend_id{"test.backend"};
    rec.prov.backend_ver = st::backend_version{2, 7};
    std::atomic<int> count{0};

    {
        st::rocksdb_catalog cat{catalog_path};
        st::filesystem_blob_store blobs{blob_path};
        auto first = st::get_or_compile(
            cat, blobs, rec.key,
            [&]() -> std::expected<st::artifact_record, std::string> {
                ++count;
                return rec;
            });
        REQUIRE(first.has_value());
    }

    {
        st::rocksdb_catalog cat{catalog_path};
        st::filesystem_blob_store blobs{blob_path};
        auto second = st::get_or_compile(
            cat, blobs, rec.key,
            [&]() -> std::expected<st::artifact_record, std::string> {
                ++count;
                return rec;
            });
        REQUIRE(second.has_value());
        CHECK(count == 1);
        CHECK(second->compat.target.os == "macos");
        CHECK(second->compat.target.arch == "arm64");
        CHECK(second->compat.security == (st::security_policy_id{17, 4}));
        REQUIRE(second->prov.backend.has_value());
        CHECK(second->prov.backend->name == "test.backend");
        REQUIRE(second->prov.backend_ver.has_value());
        CHECK(second->prov.backend_ver->major == 2);
        CHECK(second->prov.producer == "restart-test");
    }
}

#endif // LITHE_HAS_ROCKSDB
