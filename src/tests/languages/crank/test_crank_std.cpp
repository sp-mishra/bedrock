// =============================================================================
// test_crank_std.cpp — Crank standard library registration + invocation tests.
//
// Verifies: include/languages/crank/std/*.hpp — the reflection-driven stdlib
//           layer that projects C++/STL facilities into Crank via the host
//           embedding seams (context::register_function_descriptor +
//           ffi_module_builder). Nothing here touches the language core.
//
// Strategy: install modules into a crank::engine's context, then
//   (a) verify each surface resolves via verify_extern_fn_decl with the right
//       arity and a non-null typed thunk (the same API tooling uses), and
//   (b) invoke a representative sample directly through invoke_typed to confirm
//       the typed thunk computes the correct value across scalar / string /
//       container boundaries.
//
// Guarded modules (std.json under glaze, std.net under libuv) are exercised
// only when their __has_include guard is satisfied; otherwise those cases are
// compiled out so the suite builds with or without the optional dependencies.
//
//   1.  install_std_math — Sqrt resolves, arity 1, thunk non-null.
//   2.  invoke Sqrt(4.0) == 2.0.
//   3.  invoke Pow(2, 10) == 1024.
//   4.  invoke MinInt(3, 7) == 3, MaxInt(3, 7) == 7.
//   5.  math constants: Pi arity 0, invoke ~= 3.14159.
//   6.  install_std_string — Trim resolves, invoke trims both ends.
//   7.  invoke ToUpper / Len.
//   8.  invoke StartsWith / Contains (bool returns).
//   9.  install_std_io — Println resolves, arity 1.
//  10.  install_std_time — SleepMs resolves; NowNs resolves arity 0.
//  11.  install_std_fs — write→read round-trip on a temp file (guarded cleanup).
//  12.  install_std_all populates every always-on module.
//  13.  std.json round-trip (guarded on glaze).
//  14.  std.net descriptors resolve (guarded on libuv).
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/engine.hpp"
#include "languages/crank/std/std.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

// ============================================================================
// Test 1 — math.sqrt resolves with correct arity + non-null thunk
// ============================================================================

TEST_CASE("std: math sqrt resolves", "[crank][std][math]") {
    crank::engine e;
    crank::stdlib::install_std_math(e.context());

    auto r = crank::verify_extern_fn_decl(e.context(), "Sqrt", "std.math.sqrt", 1);
    REQUIRE(r.has_value());
    CHECK(r->arity == 1u);
    CHECK(r->descriptor != nullptr);
    CHECK(r->thunk != nullptr);
}

// ============================================================================
// Test 2 — invoke sqrt(4.0) == 2.0
// ============================================================================

TEST_CASE("std: math sqrt(4.0) == 2.0", "[crank][std][math]") {
    crank::engine e;
    crank::stdlib::install_std_math(e.context());

    auto r = crank::verify_extern_fn_decl(e.context(), "Sqrt", "std.math.sqrt", 1);
    REQUIRE(r.has_value());
    auto out = crank::invoke_typed<double>(*r->descriptor, 4.0);
    REQUIRE(out.has_value());
    CHECK(*out == Catch::Approx(2.0));
}

// ============================================================================
// Test 3 — invoke pow(2, 10) == 1024
// ============================================================================

TEST_CASE("std: math pow(2,10) == 1024", "[crank][std][math]") {
    crank::engine e;
    crank::stdlib::install_std_math(e.context());

    auto r = crank::verify_extern_fn_decl(e.context(), "Pow", "std.math.pow", 2);
    REQUIRE(r.has_value());
    auto out = crank::invoke_typed<double>(*r->descriptor, 2.0, 10.0);
    REQUIRE(out.has_value());
    CHECK(*out == Catch::Approx(1024.0));
}

// ============================================================================
// Test 4 — invoke min/max i64
// ============================================================================

TEST_CASE("std: math min/max int", "[crank][std][math]") {
    crank::engine e;
    crank::stdlib::install_std_math(e.context());

    auto rmin = crank::verify_extern_fn_decl(e.context(), "MinInt", "std.math.min_i64", 2);
    auto rmax = crank::verify_extern_fn_decl(e.context(), "MaxInt", "std.math.max_i64", 2);
    REQUIRE(rmin.has_value());
    REQUIRE(rmax.has_value());

    auto lo = crank::invoke_typed<std::int64_t>(*rmin->descriptor,
                                                std::int64_t{3}, std::int64_t{7});
    auto hi = crank::invoke_typed<std::int64_t>(*rmax->descriptor,
                                                std::int64_t{3}, std::int64_t{7});
    REQUIRE(lo.has_value());
    REQUIRE(hi.has_value());
    CHECK(*lo == 3);
    CHECK(*hi == 7);
}

// ============================================================================
// Test 5 — math constant Pi is nullary and computes
// ============================================================================

TEST_CASE("std: math Pi constant", "[crank][std][math]") {
    crank::engine e;
    crank::stdlib::install_std_math(e.context());

    auto r = crank::verify_extern_fn_decl(e.context(), "Pi", "std.math.pi", 0);
    REQUIRE(r.has_value());
    CHECK(r->arity == 0u);
    auto out = crank::invoke_typed<double>(*r->descriptor);
    REQUIRE(out.has_value());
    CHECK(*out == Catch::Approx(3.14159265).epsilon(0.0001));
}

// ============================================================================
// Test 6 — string trim resolves and trims both ends
// ============================================================================

TEST_CASE("std: string trim(\" a \") == \"a\"", "[crank][std][string]") {
    crank::engine e;
    crank::stdlib::install_std_string(e.context());

    auto r = crank::verify_extern_fn_decl(e.context(), "Trim", "std.string.trim", 1);
    REQUIRE(r.has_value());
    std::string in = "  a  ";
    auto out = crank::invoke_typed<std::string>(*r->descriptor, in);
    REQUIRE(out.has_value());
    CHECK(*out == "a");
}

// ============================================================================
// Test 7 — string to_upper + len
// ============================================================================

TEST_CASE("std: string to_upper + len", "[crank][std][string]") {
    crank::engine e;
    crank::stdlib::install_std_string(e.context());

    auto ru = crank::verify_extern_fn_decl(e.context(), "ToUpper", "std.string.to_upper", 1);
    auto rl = crank::verify_extern_fn_decl(e.context(), "Len", "std.string.len", 1);
    REQUIRE(ru.has_value());
    REQUIRE(rl.has_value());

    std::string in = "abc";
    auto up = crank::invoke_typed<std::string>(*ru->descriptor, in);
    auto n = crank::invoke_typed<std::int64_t>(*rl->descriptor, in);
    REQUIRE(up.has_value());
    REQUIRE(n.has_value());
    CHECK(*up == "ABC");
    CHECK(*n == 3);
}

// ============================================================================
// Test 8 — string starts_with + contains (bool returns)
// ============================================================================

TEST_CASE("std: string starts_with + contains", "[crank][std][string]") {
    crank::engine e;
    crank::stdlib::install_std_string(e.context());

    auto rs = crank::verify_extern_fn_decl(e.context(), "StartsWith", "std.string.starts_with", 2);
    auto rc = crank::verify_extern_fn_decl(e.context(), "Contains", "std.string.contains", 2);
    REQUIRE(rs.has_value());
    REQUIRE(rc.has_value());

    std::string hay = "hello world", pre = "hello", mid = "o w", none = "xyz";
    auto sw = crank::invoke_typed<bool>(*rs->descriptor, hay, pre);
    auto ct = crank::invoke_typed<bool>(*rc->descriptor, hay, mid);
    auto ctn = crank::invoke_typed<bool>(*rc->descriptor, hay, none);
    REQUIRE(sw.has_value());
    REQUIRE(ct.has_value());
    REQUIRE(ctn.has_value());
    CHECK(*sw);
    CHECK(*ct);
    CHECK_FALSE(*ctn);
}

// ============================================================================
// Test 9 — io Println resolves with arity 1
// ============================================================================

TEST_CASE("std: io println resolves", "[crank][std][io]") {
    crank::engine e;
    crank::stdlib::install_std_io(e.context());

    auto r = crank::verify_extern_fn_decl(e.context(), "Println", "std.io.println", 1);
    REQUIRE(r.has_value());
    CHECK(r->arity == 1u);
    CHECK(r->thunk != nullptr);
}

// ============================================================================
// Test 10 — time NowNs (arity 0) and SleepMs resolve
// ============================================================================

TEST_CASE("std: time now + sleep resolve", "[crank][std][time]") {
    crank::engine e;
    crank::stdlib::install_std_time(e.context());

    auto rnow = crank::verify_extern_fn_decl(e.context(), "NowNs", "std.time.now_ns", 0);
    auto rsleep = crank::verify_extern_fn_decl(e.context(), "SleepMs", "std.time.sleep_ms", 1);
    REQUIRE(rnow.has_value());
    REQUIRE(rsleep.has_value());
    CHECK(rnow->arity == 0u);
    CHECK(rsleep->arity == 1u);

    auto ns = crank::invoke_typed<std::int64_t>(*rnow->descriptor);
    REQUIRE(ns.has_value());
    CHECK(*ns > 0);
}

// ============================================================================
// Test 11 — fs write→read round-trip on a temp file
// ============================================================================

TEST_CASE("std: fs write then read round-trip", "[crank][std][fs]") {
    crank::engine e;
    crank::stdlib::install_std_fs(e.context());

    auto rw = crank::verify_extern_fn_decl(e.context(), "WriteFile", "std.fs.write_file", 2);
    auto rr = crank::verify_extern_fn_decl(e.context(), "ReadFile", "std.fs.read_file", 1);
    auto rx = crank::verify_extern_fn_decl(e.context(), "Remove", "std.fs.remove", 1);
    REQUIRE(rw.has_value());
    REQUIRE(rr.has_value());
    REQUIRE(rx.has_value());

    const auto p = std::filesystem::temp_directory_path() / "crank_std_test.txt";
    std::string path = p.string();
    std::string body = "crank stdlib fs round-trip";

    auto wrote = crank::invoke_typed<bool>(*rw->descriptor, path, body);
    REQUIRE(wrote.has_value());
    CHECK(*wrote);

    auto got = crank::invoke_typed<std::string>(*rr->descriptor, path);
    REQUIRE(got.has_value());
    CHECK(*got == body);

    auto removed = crank::invoke_typed<bool>(*rx->descriptor, path);
    REQUIRE(removed.has_value());
    CHECK(*removed);
    CHECK_FALSE(std::filesystem::exists(p));
}

// ============================================================================
// Test 12 — install_std_all wires up the always-on modules
// ============================================================================

TEST_CASE("std: install_std_all resolves representative symbols", "[crank][std][all]") {
    crank::engine e;
    crank::stdlib::install_std_all(e.context());

    // One representative host symbol per always-on module.
    CHECK(crank::verify_extern_fn_decl(e.context(), "Sqrt", "std.math.sqrt", 1).has_value());
    CHECK(crank::verify_extern_fn_decl(e.context(), "Trim", "std.string.trim", 1).has_value());
    CHECK(crank::verify_extern_fn_decl(e.context(), "Println", "std.io.println", 1).has_value());
    CHECK(crank::verify_extern_fn_decl(e.context(), "NowNs", "std.time.now_ns", 0).has_value());
    CHECK(crank::verify_extern_fn_decl(e.context(), "ReadFile", "std.fs.read_file", 1).has_value());
    CHECK(crank::verify_extern_fn_decl(e.context(), "Env", "std.process.env", 1).has_value());
}

// ============================================================================
// Test 13 — std.json parse → stringify round-trip (guarded on glaze)
// ============================================================================

#if CRANK_STD_HAS_GLAZE
TEST_CASE("std: json parse + typed getters", "[crank][std][json]") {
    crank::engine e;
    crank::stdlib::install_std_json(e.context());

    auto rp = crank::verify_extern_fn_decl(e.context(), "Parse", "std.json.parse", 1);
    auto rok = crank::verify_extern_fn_decl(e.context(), "IsOk", "std.json.is_ok", 1);
    auto rgs = crank::verify_extern_fn_decl(e.context(), "GetString", "std.json.get_string", 3);
    auto rgi = crank::verify_extern_fn_decl(e.context(), "GetInt", "std.json.get_i64", 3);
    REQUIRE(rp.has_value());
    REQUIRE(rok.has_value());
    REQUIRE(rgs.has_value());
    REQUIRE(rgi.has_value());

    std::string src = R"({"name":"crank","version":1})";
    auto doc = crank::invoke_typed<crank::stdlib::json_value>(*rp->descriptor, src);
    REQUIRE(doc.has_value());

    auto ok = crank::invoke_typed<bool>(*rok->descriptor, *doc);
    REQUIRE(ok.has_value());
    CHECK(*ok);

    std::string key_name = "name", key_ver = "version", fallback = "?";
    auto name = crank::invoke_typed<std::string>(*rgs->descriptor, *doc, key_name, fallback);
    auto ver = crank::invoke_typed<std::int64_t>(*rgi->descriptor, *doc, key_ver,
                                                 std::int64_t{-1});
    REQUIRE(name.has_value());
    REQUIRE(ver.has_value());
    CHECK(*name == "crank");
    CHECK(*ver == 1);
}
#endif // CRANK_STD_HAS_GLAZE

// ============================================================================
// Test 14 — std.net descriptors resolve (guarded on libuv)
// ============================================================================

#if CRANK_STD_HAS_UV
TEST_CASE("std: net descriptors resolve", "[crank][std][net]") {
    crank::engine e;
    crank::stdlib::install_std_net(e.context());

    auto rc = crank::verify_extern_fn_decl(e.context(), "ConnectTcp", "std.net.connect_tcp", 2);
    auto rl = crank::verify_extern_fn_decl(e.context(), "ListenTcp", "std.net.listen_tcp", 3);
    auto rb = crank::verify_extern_fn_decl(e.context(), "BindUdp", "std.net.bind_udp", 2);
    REQUIRE(rc.has_value());
    REQUIRE(rl.has_value());
    REQUIRE(rb.has_value());
    CHECK(rc->thunk != nullptr);
    CHECK(rl->thunk != nullptr);
    CHECK(rb->thunk != nullptr);
}
#endif // CRANK_STD_HAS_UV

// ============================================================================
// Test 15 — std.containers connected components (union_find-backed)
// ============================================================================

TEST_CASE("std: containers connected components", "[crank][std][containers]") {
    crank::engine e;
    crank::stdlib::install_std_containers(e.context());

    auto rc = crank::verify_extern_fn_decl(e.context(), "ConnectedComponents",
                                           "std.containers.connected_components", 3);
    auto rn = crank::verify_extern_fn_decl(e.context(), "ComponentCount",
                                           "std.containers.component_count", 3);
    auto rs = crank::verify_extern_fn_decl(e.context(), "SameComponent",
                                           "std.containers.same_component", 5);
    REQUIRE(rc.has_value());
    REQUIRE(rn.has_value());
    REQUIRE(rs.has_value());

    // 5 nodes, two clusters: {0-1-2} and {3-4}.
    std::vector<std::int64_t> from{0, 1, 3};
    std::vector<std::int64_t> to{1, 2, 4};

    auto labels = crank::invoke_typed<std::vector<std::int64_t>>(*rc->descriptor,
                                                                 from, to, std::int64_t{5});
    REQUIRE(labels.has_value());
    REQUIRE(labels->size() == 5u);
    // Labels are the smallest member id of each component.
    CHECK((*labels)[0] == 0);
    CHECK((*labels)[1] == 0);
    CHECK((*labels)[2] == 0);
    CHECK((*labels)[3] == 3);
    CHECK((*labels)[4] == 3);

    auto count = crank::invoke_typed<std::int64_t>(*rn->descriptor, from, to, std::int64_t{5});
    REQUIRE(count.has_value());
    CHECK(*count == 2);

    auto same = crank::invoke_typed<bool>(*rs->descriptor, from, to, std::int64_t{5},
                                          std::int64_t{0}, std::int64_t{2});
    auto diff = crank::invoke_typed<bool>(*rs->descriptor, from, to, std::int64_t{5},
                                          std::int64_t{0}, std::int64_t{4});
    REQUIRE(same.has_value());
    REQUIRE(diff.has_value());
    CHECK(*same);
    CHECK_FALSE(*diff);
}

// ============================================================================
// Test 16 — std.containers topological order + cycle detection
// ============================================================================

TEST_CASE("std: containers topo order and cycle detection", "[crank][std][containers]") {
    crank::engine e;
    crank::stdlib::install_std_containers(e.context());

    auto rt = crank::verify_extern_fn_decl(e.context(), "TopoOrder",
                                           "std.containers.topo_order", 3);
    auto rh = crank::verify_extern_fn_decl(e.context(), "HasCycle",
                                           "std.containers.has_cycle", 3);
    REQUIRE(rt.has_value());
    REQUIRE(rh.has_value());

    // DAG: 0→1, 0→2, 1→3, 2→3.
    std::vector<std::int64_t> from{0, 0, 1, 2};
    std::vector<std::int64_t> to{1, 2, 3, 3};

    auto order = crank::invoke_typed<std::vector<std::int64_t>>(*rt->descriptor,
                                                                from, to, std::int64_t{4});
    REQUIRE(order.has_value());
    REQUIRE(order->size() == 4u);
    // Validate it is a real topological order: each node appears after its preds.
    std::vector<std::size_t> pos(4);
    for (std::size_t i = 0; i < order->size(); ++i)
        pos[static_cast<std::size_t>((*order)[i])] = i;
    for (std::size_t e2 = 0; e2 < from.size(); ++e2)
        CHECK(pos[static_cast<std::size_t>(from[e2])] < pos[static_cast<std::size_t>(to[e2])]);

    auto acyclic = crank::invoke_typed<bool>(*rh->descriptor, from, to, std::int64_t{4});
    REQUIRE(acyclic.has_value());
    CHECK_FALSE(*acyclic);

    // Add a back edge 3→0 to introduce a cycle.
    std::vector<std::int64_t> cfrom{0, 0, 1, 2, 3};
    std::vector<std::int64_t> cto{1, 2, 3, 3, 0};
    auto cyc = crank::invoke_typed<bool>(*rh->descriptor, cfrom, cto, std::int64_t{4});
    auto empty_order = crank::invoke_typed<std::vector<std::int64_t>>(*rt->descriptor,
                                                                      cfrom, cto, std::int64_t{4});
    REQUIRE(cyc.has_value());
    REQUIRE(empty_order.has_value());
    CHECK(*cyc);
    CHECK(empty_order->empty());   // topo of a cyclic graph is empty
}

// ============================================================================
// Test 17 — std.containers BFS order + reachable count
// ============================================================================

TEST_CASE("std: containers bfs order and reachable count", "[crank][std][containers]") {
    crank::engine e;
    crank::stdlib::install_std_containers(e.context());

    auto rb = crank::verify_extern_fn_decl(e.context(), "BfsOrder",
                                           "std.containers.bfs_order", 4);
    auto rr = crank::verify_extern_fn_decl(e.context(), "ReachableCount",
                                           "std.containers.reachable_count", 4);
    REQUIRE(rb.has_value());
    REQUIRE(rr.has_value());

    // 0→1, 0→2, 2→3; node 4 isolated.
    std::vector<std::int64_t> from{0, 0, 2};
    std::vector<std::int64_t> to{1, 2, 3};

    auto order = crank::invoke_typed<std::vector<std::int64_t>>(*rb->descriptor,
                                                                from, to, std::int64_t{5},
                                                                std::int64_t{0});
    REQUIRE(order.has_value());
    REQUIRE_FALSE(order->empty());
    CHECK(order->front() == 0);            // BFS starts at the source

    auto reach = crank::invoke_typed<std::int64_t>(*rr->descriptor, from, to,
                                                   std::int64_t{5}, std::int64_t{0});
    REQUIRE(reach.has_value());
    CHECK(*reach == 4);                    // 0,1,2,3 reachable; 4 is not
}

// ============================================================================
// Test 18 — std.containers vector algorithms round-trip
// ============================================================================

TEST_CASE("std: containers vector algorithms", "[crank][std][containers]") {
    crank::engine e;
    crank::stdlib::install_std_containers(e.context());

    auto rs = crank::verify_extern_fn_decl(e.context(), "VecIntSort",
                                           "std.containers.vec_sort", 1);
    auto ru = crank::verify_extern_fn_decl(e.context(), "VecIntUnique",
                                           "std.containers.vec_unique", 1);
    auto rsum = crank::verify_extern_fn_decl(e.context(), "VecIntSum",
                                             "std.containers.vec_sum", 1);
    REQUIRE(rs.has_value());
    REQUIRE(ru.has_value());
    REQUIRE(rsum.has_value());

    std::vector<std::int64_t> v{5, 1, 3, 1, 5};

    auto sorted = crank::invoke_typed<std::vector<std::int64_t>>(*rs->descriptor, v);
    REQUIRE(sorted.has_value());
    CHECK(std::is_sorted(sorted->begin(), sorted->end()));

    auto uniq = crank::invoke_typed<std::vector<std::int64_t>>(*ru->descriptor, v);
    REQUIRE(uniq.has_value());
    REQUIRE(uniq->size() == 3u);           // {1,3,5}
    CHECK((*uniq)[0] == 1);
    CHECK((*uniq)[1] == 3);
    CHECK((*uniq)[2] == 5);

    auto sum = crank::invoke_typed<std::int64_t>(*rsum->descriptor, v);
    REQUIRE(sum.has_value());
    CHECK(*sum == 15);
}

// ============================================================================
// Test 19 — install_std_all wires the std.containers module
// ============================================================================

TEST_CASE("std: install_std_all resolves a containers symbol", "[crank][std][containers][all]") {
    crank::engine e;
    crank::stdlib::install_std_all(e.context());

    CHECK(crank::verify_extern_fn_decl(e.context(), "ConnectedComponents",
                                       "std.containers.connected_components", 3).has_value());
    CHECK(crank::verify_extern_fn_decl(e.context(), "TopoOrder",
                                       "std.containers.topo_order", 3).has_value());
}
