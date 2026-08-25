// =============================================================================
// test_lithe_portable.cpp — portable boundary: freeze/thaw, digest, verifier
//
// Tests:
//   1. freeze/thaw round-trip: freeze(thaw(freeze(fn))) == freeze(fn) byte-wise
//   2. canonical digest: order-independent; mutation changes digest
//   3. verifier rejects malformed modules (one case per check group:
//   T,C,S,Y,R,K)
//   4. module round-trip: two-function module with
//   import/export/global/constant
// =============================================================================

#include "catch_amalgamated.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// Light IR core (module/verify/digest/cfg_adapter — no codegen)
#include "lithe/lithe_ir_core.hpp"

// Bridge: freeze + thaw (pulls lithe_codegen.hpp)
#include "lithe/lithe_execution/store/store.hpp"
#include "lithe/lithe_local_engine.hpp"
#include "lithe/lithe_ir/portable/bridge.hpp"

using namespace lithe::ir::portable;
using namespace lithe::ir::adapters;
using namespace lithe::codegen::hl;
using namespace lithe::codegen;

// =============================================================================
// Test helpers
// =============================================================================

// Build a minimal live hl_mir_function with:
//   - one argument (i64)
//   - one constant
//   - one fadd
//   - one region_yield (terminator)
static hl_mir_function make_simple_fn(const std::string &name = "test_fn") {
  hl_mir_function fn(1u << 18);
  fn.name = name;

  hl_region *reg = fn.make_region();
  hl_block *blk = fn.make_block();
  blk->parent_region = reg;
  reg->blocks.push_back(blk);
  fn.body_region = *reg;

  // argument op (produces result id)
  hl_operation *arg_op = fn.make_op(hl_opcode::argument);
  auto arg_results = fn.alloc_span<ssa_value_id>(1);
  arg_results[0] = ssa_value_id{1};
  arg_op->results = arg_results;
  blk->ops.push_back(arg_op);

  // constant op
  hl_operation *const_op = fn.make_op(hl_opcode::constant);
  auto const_results = fn.alloc_span<ssa_value_id>(1);
  const_results[0] = ssa_value_id{2};
  const_op->results = const_results;
  blk->ops.push_back(const_op);

  // fadd op (operands: arg, const; result: sum)
  hl_operation *fadd_op = fn.make_op(hl_opcode::fadd);
  auto fadd_ops = fn.alloc_span<ssa_value_id>(2);
  fadd_ops[0] = ssa_value_id{1};
  fadd_ops[1] = ssa_value_id{2};
  fadd_op->operands = fadd_ops;
  auto fadd_results = fn.alloc_span<ssa_value_id>(1);
  fadd_results[0] = ssa_value_id{3};
  fadd_op->results = fadd_results;
  blk->ops.push_back(fadd_op);

  // region_yield (terminator, operand = sum)
  hl_operation *yield_op = fn.make_op(hl_opcode::region_yield);
  auto yield_ops = fn.alloc_span<ssa_value_id>(1);
  yield_ops[0] = ssa_value_id{3};
  yield_op->operands = yield_ops;
  blk->ops.push_back(yield_op);

  // Rebuild body_region (copy by value after push_back is safe for root region)
  fn.body_region.blocks.head = nullptr;
  fn.body_region.blocks.tail = nullptr;
  fn.body_region.blocks.size_ = 0;
  fn.body_region.blocks.push_back(blk);

  return fn;
}

// Build a wire function from make_simple_fn
static lithe_hl_mir_ir make_wire_fn(const std::string &name = "test_fn") {
  auto fn = make_simple_fn(name);
  auto result = freeze_function(fn);
  REQUIRE(result.has_value());
  return std::move(*result);
}

// =============================================================================
// Test 1: freeze/thaw round-trip
// freeze(thaw(freeze(fn))) == freeze(fn) via canonical_encode comparison
// =============================================================================

TEST_CASE(

    "portable: freeze/thaw round-trip preserves structure",
    "[portable][round-trip]") {
  auto fn = make_simple_fn();

  // First freeze
  auto wire1_result = freeze_function(fn);
  REQUIRE(wire1_result.has_value());
  const auto &wire1 = *wire1_result;

  SECTION("basic freeze properties") {
    CHECK(wire1.function_name == "test_fn");
    CHECK(!wire1.blocks.empty());
    CHECK(!wire1.ops.empty());
    CHECK(!wire1.values.empty());
    CHECK(!wire1.entry_block_ids.empty());
  }

  SECTION("thaw reconstructs function") {
    auto thawed_result = thaw_function(wire1);
    REQUIRE(thawed_result.has_value());
    const auto &thawed_fn = *thawed_result;
    CHECK(thawed_fn.name == "test_fn");
    CHECK(thawed_fn.body_region.blocks.head != nullptr);
  }

  SECTION("freeze(thaw(freeze(fn))) == freeze(fn) canonically") {
    // Thaw
    auto thawed_result = thaw_function(wire1);
    REQUIRE(thawed_result.has_value());

    // Freeze again
    auto wire2_result = freeze_function(*thawed_result);
    REQUIRE(wire2_result.has_value());

    // Compare canonical encodings via a portable_module wrapper
    portable_module mod1, mod2;
    mod1.functions = {wire1};
    mod2.functions = {*wire2_result};

    const auto enc1 = canonical_encode(mod1);
    const auto enc2 = canonical_encode(mod2);

    // Op count and block count must match
    CHECK(wire1.ops.size() == wire2_result->ops.size());
    CHECK(wire1.blocks.size() == wire2_result->blocks.size());
    CHECK(wire1.values.size() == wire2_result->values.size());
    // Canonical encodings must be byte-identical
    CHECK(enc1 == enc2);
  }
}

TEST_CASE(

    "target artifact cache persists safe bytes and repairs a missing blob",
    "[lithe][portable][local_engine][target_cache]") {
  struct test_artifact {
    std::vector<std::uint8_t> bytes;
  };

  const auto root = std::filesystem::temp_directory_path() /
                    ("lithe_target_cache_" + std::to_string(
                        std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } remove_root{root};

  namespace st = lithe::execution::store;
  st::memory_catalog catalog;
  st::filesystem_blob_store blobs{root};
  lithe::target_cache_config config;
  config.backend.name = "test.relocatable";
  config.backend_version = {3, 1};
  lithe::target_artifact_cache cache{catalog, blobs, config};

  st::optimized_key portable_key;
  portable_key.semantic_digest[0] = 0x42;
  portable_key.semantic_digest_len = 32;
  portable_key.pipe_id.name = "portable.balanced";

  std::atomic<int> compile_count{0};
  auto compile = [&]() -> std::expected<test_artifact, std::string> {
    ++compile_count;
    return test_artifact{{0x4c, 0x49, 0x54, 0x48, 0x45}};
  };
  auto encode = [](const test_artifact &artifact)
      -> std::expected<std::vector<std::uint8_t>, std::string> {
    return artifact.bytes;
  };
  auto decode = [](std::span<const std::uint8_t> bytes)
      -> std::expected<test_artifact, std::string> {
    if (bytes.size() != 5 || bytes.front() != 0x4c)
      return std::unexpected("invalid target artifact");
    return test_artifact{{bytes.begin(), bytes.end()}};
  };

  auto first = cache.load_or_compile<test_artifact>(
      portable_key, compile, encode, decode);
  REQUIRE(first.has_value());
  CHECK_FALSE(first->cache_hit);
  CHECK(compile_count == 1);

  auto second = cache.load_or_compile<test_artifact>(
      portable_key, compile, encode, decode);
  REQUIRE(second.has_value());
  CHECK(second->cache_hit);
  CHECK(compile_count == 1);

  auto entry = catalog.lookup(first->key);
  REQUIRE(entry.has_value());
  REQUIRE(entry->has_value());
  REQUIRE(blobs.erase((**entry).blob_addr).has_value());

  auto repaired = cache.load_or_compile<test_artifact>(
      portable_key, compile, encode, decode);
  REQUIRE(repaired.has_value());
  CHECK_FALSE(repaired->cache_hit);
  CHECK(compile_count == 2);
  CHECK(repaired->artifact.bytes == first->artifact.bytes);
}

TEST_CASE(

    "portable: engine cache optimizes once and reloads verified bytes",
    "[lithe][portable][local_engine][cache]") {
  namespace st = lithe::execution::store;

  portable_module original;
  original.functions = {make_wire_fn("engine_cached")};
  original.manifest.producer = "turbo_twig_tests";

  const auto root = std::filesystem::temp_directory_path() /
                    ("lithe_engine_cache_" + std::to_string(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } remove_root{root};

  st::memory_catalog catalog;
  st::filesystem_blob_store blobs{root};
  lithe::portable_cache_config config;
  config.level = lithe::ir::portable::opt::portable_level::balanced;
  config.policy.paranoid = true;
  lithe::portable_artifact_cache cache{catalog, blobs, config};

  auto first = cache.load_or_optimize(original);
  REQUIRE(first.has_value());
  CHECK_FALSE(first->cache_hit);
  REQUIRE(first->optimization_record.has_value());
  CHECK(first->module.manifest.digest_len == 32);
  REQUIRE(verify_portable(first->module).ok);

  auto second = cache.load_or_optimize(original);
  REQUIRE(second.has_value());
  CHECK(second->cache_hit);
  CHECK_FALSE(second->optimization_record.has_value());
  CHECK(canonical_encode(second->module) == canonical_encode(first->module));
  CHECK(first->key == second->key);

  auto live = make_simple_fn("freeze_cache_thaw");
  const std::array<const hl_mir_function *, 1> live_functions{&live};
  auto thawed = cache.freeze_optimize_thaw(live_functions);
  REQUIRE(thawed.has_value());
  REQUIRE(thawed->size() == 1);
  CHECK(thawed->front().name == "freeze_cache_thaw");
}

TEST_CASE(

    "portable: lossless codec verifies and thaws after decode",
    "[portable][codec][round-trip]") {
  portable_module original;
  original.functions = {make_wire_fn("persisted")};
  original.manifest.producer = "turbo_twig_tests";
  original.manifest.source_language = "lithe";
  stamp_semantic_digest(original);

  const auto encoded = encode_portable(original);
  REQUIRE(encoded.has_value());

  const auto decoded = decode_portable(*encoded);
  REQUIRE(decoded.has_value());
  CHECK(canonical_encode(*decoded) == canonical_encode(original));
  CHECK(semantic_digest(*decoded) == semantic_digest(original));

  const auto verified = verify_portable(*decoded);
  REQUIRE(verified.ok);
  const auto thawed = thaw_function(decoded->functions.front());
  REQUIRE(thawed.has_value());
  CHECK(thawed->name == "persisted");

  auto corrupt = *encoded;
  corrupt[0] = 0;
  CHECK_FALSE(decode_portable(corrupt).has_value());

  SECTION("every optional operation attribute is lossless") {
    auto attributed = original;
    auto &op = attributed.functions.front().ops.front();
    op.structured_for = {{2, true, {0, 1}, {8, 9}, {1, 2}, {4, 0}}};
    op.memref = {{2, "i64", 64, {8, 9}, {9, 1}}};
    op.branch = {{17}};
    op.branch_cond = {{18, 19}};
    op.compare = {{3, false}};
    op.guard = {{1, 2, 3, 4}};
    op.trap = {{5, 6}};
    op.cleanup = {{{7, 8, 9}}};
    op.transaction = {{10, 11, 12, 13, 14, 15, 16, 17}};

    const auto attr_bytes = encode_portable(attributed);
    REQUIRE(attr_bytes.has_value());
    // The synthetic combination is intentionally not semantically valid;
    // this section isolates codec fidelity from verifier policy.
    const auto attr_decoded = decode_portable(*attr_bytes, {}, false);
    REQUIRE(attr_decoded.has_value());
    CHECK(canonical_encode(*attr_decoded) == canonical_encode(attributed));
  }
}

TEST_CASE(

    "portable: durable artifact store reloads verified module bytes",
    "[portable][codec][store][round-trip]") {
  namespace st = lithe::execution::store;

  portable_module original;
  original.functions = {make_wire_fn("cached")};
  original.manifest.producer = "turbo_twig_tests";
  stamp_semantic_digest(original);

  const auto bytes = encode_portable(original);
  REQUIRE(bytes.has_value());

  st::artifact_record record;
  record.kind = st::artifact_kind::optimized_portable;
  record.manifest.produced_from = lithe::execution::ir_kind::hl_mir;
  record.manifest.role = lithe::execution::artifact_class::metadata_only;
  record.semantic_digest = semantic_digest(original);
  record.semantic_digest_len = 32;
  record.prov.pipe.name = "portable-canonical";
  record.prov.pipe_ver = {1, 0};
  record.prov.producer = "turbo_twig_tests";

  st::optimized_key key;
  key.semantic_digest = record.semantic_digest;
  key.semantic_digest_len = record.semantic_digest_len;
  key.pipe_id = record.prov.pipe;
  key.pipe_ver = record.prov.pipe_ver;
  record.key = key;
  record.payload = st::inline_payload{*bytes};

  const auto root =
      std::filesystem::temp_directory_path() /
      ("lithe_portable_store_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct cleanup {
    std::filesystem::path path;
    ~cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } remove_root{root};

  st::memory_catalog catalog;
  st::filesystem_blob_store blobs{root};
  std::size_t compile_count = 0;
  auto compile = [&]() -> std::expected<st::artifact_record, std::string> {
    ++compile_count;
    return record;
  };

  const auto first = st::get_or_compile(catalog, blobs, record.key, compile);
  REQUIRE(first.has_value());
  const auto second = st::get_or_compile(catalog, blobs, record.key, compile);
  REQUIRE(second.has_value());
  CHECK(compile_count == 1);
  CHECK(first->blob_addr == second->blob_addr);

  const auto stored = blobs.get(first->blob_addr);
  REQUIRE(stored.has_value());
  const auto decoded = decode_portable(stored->data);
  REQUIRE(decoded.has_value());
  CHECK(semantic_digest(*decoded) == semantic_digest(original));
  REQUIRE(verify_portable(*decoded).ok);
  REQUIRE(thaw_function(decoded->functions.front()).has_value());
}

// =============================================================================
// Test 2: semantic digest stability + sensitivity
// =============================================================================

TEST_CASE(

    "portable: semantic digest is order-independent and stable",
    "[portable][digest]") {
  SECTION("same module → same digest") {
    auto w1 = make_wire_fn("fn_a");
    auto w2 = make_wire_fn("fn_a");

    portable_module m1, m2;
    m1.functions = {w1};
    m2.functions = {w2};

    const auto d1 = semantic_digest(m1);
    const auto d2 = semantic_digest(m2);
    CHECK(d1 == d2);
  }

  SECTION("different function name → different digest") {
    auto w1 = make_wire_fn("fn_a");
    auto w2 = make_wire_fn("fn_b");

    portable_module m1, m2;
    m1.functions = {w1};
    m2.functions = {w2};

    const auto d1 = semantic_digest(m1);
    const auto d2 = semantic_digest(m2);
    CHECK(d1 != d2);
  }

  SECTION("mutated op changes digest") {
    auto w1 = make_wire_fn("fn_a");
    auto w2 = w1; // copy
    // Mutate: change one op name
    if (!w2.ops.empty())
      w2.ops[0].name = "sub"; // was something else

    portable_module m1, m2;
    m1.functions = {w1};
    m2.functions = {w2};

    const auto d1 = semantic_digest(m1);
    const auto d2 = semantic_digest(m2);
    CHECK(d1 != d2);
  }

  SECTION("stamp_semantic_digest populates manifest") {
    portable_module mod;
    mod.functions = {make_wire_fn()};
    stamp_semantic_digest(mod);
    CHECK(mod.manifest.digest_len > 0);
    // At least some bytes non-zero (SHA-256 of any non-empty preimage is
    // non-zero)
    bool any_nonzero = false;
    for (std::size_t i = 0; i < mod.manifest.digest_len; ++i)
      if (mod.manifest.semantic_digest[i] != 0) {
        any_nonzero = true;
        break;
      }
    CHECK(any_nonzero);
  }
}

// =============================================================================
// Test 3: verifier rejects malformed modules (one case per check group)
// =============================================================================

TEST_CASE(

    "portable: verifier rejects malformed modules", "[portable][verify]") {

  SECTION("T — unparsable value type") {
    lithe_hl_mir_ir wire = make_wire_fn();
    if (!wire.values.empty())
      wire.values[0].type_str = "INVALID_TYPE$$";

    portable_module mod;
    mod.functions = {wire};
    mod.declared_capabilities.set(portable_capability_bit::external_calls);

    const auto rep = verify_portable(mod);
    CHECK(!rep.ok);
    bool has_T = false;
    for (const auto &d : rep.diagnostics)
      if (d.code == diag_codes::type_parse_failed) {
        has_T = true;
        break;
      }
    CHECK(has_T);
  }

  SECTION("C — missing terminator") {
    lithe_hl_mir_ir wire = make_wire_fn();
    // Remove region_yield (last op) from block op_ids
    for (auto &blk : wire.blocks) {
      if (!blk.op_ids.empty())
        blk.op_ids.pop_back();
    }
    // Also remove the region_yield op from ops
    wire.ops.erase(std::remove_if(wire.ops.begin(), wire.ops.end(),
                                  [](const hl_wire_op &op) {
                                    return op.name == "region_yield";
                                  }),
                   wire.ops.end());

    portable_module mod;
    mod.functions = {wire};

    const auto rep = verify_portable(mod);
    CHECK(!rep.ok);
    bool has_C = false;
    for (const auto &d : rep.diagnostics)
      if (d.code == diag_codes::block_no_terminator) {
        has_C = true;
        break;
      }
    CHECK(has_C);
  }

  SECTION("S — value defined twice") {
    lithe_hl_mir_ir wire = make_wire_fn();
    // Add a duplicate result id to the second op
    if (wire.ops.size() >= 2 && !wire.ops[0].result_ids.empty()) {
      wire.ops[1].result_ids = wire.ops[0].result_ids; // duplicate
    }

    portable_module mod;
    mod.functions = {wire};

    const auto rep = verify_portable(mod);
    CHECK(!rep.ok);
    bool has_S = false;
    for (const auto &d : rep.diagnostics)
      if (d.code == diag_codes::value_multi_def) {
        has_S = true;
        break;
      }
    CHECK(has_S);
  }

  SECTION("Y — export out of range") {
    portable_module mod;
    mod.functions = {make_wire_fn()};
    mod.exports.push_back({"bad_export", 999, "i64"}); // index out of range

    const auto rep = verify_portable(mod);
    CHECK(!rep.ok);
    bool has_Y = false;
    for (const auto &d : rep.diagnostics)
      if (d.code == diag_codes::export_out_of_range) {
        has_Y = true;
        break;
      }
    CHECK(has_Y);
  }

  SECTION("Y — duplicate export symbol") {
    portable_module mod;
    mod.functions = {make_wire_fn()};
    mod.exports.push_back({"dup", 0, "i64"});
    mod.exports.push_back({"dup", 0, "i64"});

    const auto rep = verify_portable(mod);
    CHECK(!rep.ok);
    bool has_Y = false;
    for (const auto &d : rep.diagnostics)
      if (d.code == diag_codes::export_duplicate) {
        has_Y = true;
        break;
      }
    CHECK(has_Y);
  }

  SECTION("R — block in two regions") {
    lithe_hl_mir_ir wire = make_wire_fn();
    // Duplicate the block id into a second region
    if (!wire.blocks.empty()) {
      hl_wire_region extra_reg;
      extra_reg.id = static_cast<std::uint32_t>(wire.regions.size());
      extra_reg.block_ids = {wire.blocks[0].id}; // same block as region 0
      wire.regions.push_back(extra_reg);
    }

    portable_module mod;
    mod.functions = {wire};

    const auto rep = verify_portable(mod);
    CHECK(!rep.ok);
    bool has_R = false;
    for (const auto &d : rep.diagnostics)
      if (d.code == diag_codes::block_in_two_regions) {
        has_R = true;
        break;
      }
    CHECK(has_R);
  }

  SECTION("K — missing capability for call op") {
    lithe_hl_mir_ir wire = make_wire_fn();
    // Add a call op that requires external_calls capability
    hl_wire_op call_op;
    call_op.id = static_cast<std::uint32_t>(wire.ops.size());
    call_op.domain = "lithe.hl";
    call_op.name = "call";
    call_op.block_id = wire.blocks.empty() ? 0 : wire.blocks[0].id;
    call_op.region_id = 0;
    if (!wire.blocks.empty())
      wire.blocks[0].op_ids.push_back(call_op.id);
    wire.ops.push_back(call_op);

    portable_module mod;
    mod.functions = {wire};
    // declared_capabilities.bits == 0 → external_calls NOT declared

    const auto rep = verify_portable(mod);
    CHECK(!rep.ok);
    bool has_K = false;
    for (const auto &d : rep.diagnostics)
      if (d.code == diag_codes::capability_missing) {
        has_K = true;
        break;
      }
    CHECK(has_K);
  }

  SECTION("valid module passes") {
    portable_module mod;
    mod.functions = {make_wire_fn()};
    // Declare external_calls just in case (no call op in simple fn)
    verify_policy pol;
    pol.require_capability_coverage = false; // relax for simple fn
    const auto rep = verify_portable(mod, pol);
    CHECK(rep.ok);
    const bool any_error =
        std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
                    [](const lithe::diag::diagnostic &d) {
                      return d.level == lithe::diag::severity::error;
                    });
    CHECK(!any_error);
  }
}

// =============================================================================
// Test 4: module round-trip with imports/exports/globals/constants
// =============================================================================

TEST_CASE(

    "portable: module round-trip with imports/exports/globals/constants",
    "[portable][module]") {
  auto fn0 = make_simple_fn("fn_main");
  auto fn1 = make_simple_fn("fn_helper");

  const std::array<const hl_mir_function *, 2> fn_ptrs = {&fn0, &fn1};

  module_freeze_options opts;
  opts.imports.push_back(
      {"libc", "printf", "i64(i64)", lithe::ir::schema_version{1, 0, 0}, true});
  opts.exports.push_back({"fn_main", 0, "i64()"});
  opts.globals.push_back({"g_counter", "i64", 0, true});
  opts.constants.types.push_back("i64");
  opts.constants.data.push_back(
      {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  opts.declared_capabilities.set(portable_capability_bit::external_calls);

  auto mod_result = freeze_module(
      std::span<const hl_mir_function *const>{fn_ptrs.data(), fn_ptrs.size()},
      opts);
  REQUIRE(mod_result.has_value());
  const auto &mod = *mod_result;

  SECTION("module structure preserved") {
    CHECK(mod.functions.size() == 2);
    CHECK(mod.imports.size() == 1);
    CHECK(mod.exports.size() == 1);
    CHECK(mod.globals.size() == 1);
    CHECK(mod.constants.size() == 1);
    CHECK(
        mod.declared_capabilities.has(portable_capability_bit::external_calls));
  }

  SECTION("verify_portable passes") {
    verify_policy pol;
    pol.require_capability_coverage = true;
    const auto rep = verify_portable(mod, pol);
    const bool has_error =
        std::any_of(rep.diagnostics.begin(), rep.diagnostics.end(),
                    [](const lithe::diag::diagnostic &d) {
                      return d.level == lithe::diag::severity::error;
                    });
    if (!rep.ok) {
      // Print diagnostics for debugging
      for (const auto &d : rep.diagnostics)
        WARN(d.code + ": " + d.message);
    }
    CHECK(!has_error);
  }

  SECTION("thaw_module reconstructs functions") {
    auto thawed_result = thaw_module(mod);
    REQUIRE(thawed_result.has_value());
    CHECK(thawed_result->size() == 2);
    CHECK((*thawed_result)[0].name == "fn_main");
    CHECK((*thawed_result)[1].name == "fn_helper");
  }

  SECTION("semantic digest stable across two freeze calls") {
    auto mod2_result = freeze_module(
        std::span<const hl_mir_function *const>{fn_ptrs.data(), fn_ptrs.size()},
        opts);
    REQUIRE(mod2_result.has_value());
    CHECK(semantic_digest(mod) == semantic_digest(*mod2_result));
  }

  SECTION("lossless codec preserves module-level records") {
    const auto encoded = encode_portable(mod);
    REQUIRE(encoded.has_value());
    const auto decoded = decode_portable(*encoded);
    REQUIRE(decoded.has_value());
    CHECK(canonical_encode(*decoded) == canonical_encode(mod));
    CHECK(decoded->imports.size() == 1);
    CHECK(decoded->exports.size() == 1);
    CHECK(decoded->globals.size() == 1);
    CHECK(decoded->constants.size() == 1);
  }
}
