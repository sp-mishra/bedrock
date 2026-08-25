// =============================================================================
// test_lithe_exec_memory.cpp — Unit tests for lithe_exec/memory_summary.hpp
//
// Cases:
//   1.  memory_access: is_read / is_write on each access_kind
//   2.  stride_info: unit stride detection
//   3.  affine_index: is_unit_stride
//   4.  memory_summary: empty → no writes, no unknown aliasing
//   5.  memory_summary: has_writes
//   6.  alias_summary: all_no_alias with proven pairs
//   7.  alias_summary: has_unknown_aliasing flag
//   8.  memory_summary: proven_no_alias with empty pairs
//   9.  to_string(access_kind)
// =============================================================================

#include "catch_amalgamated.hpp"
#include "lithe/lithe_exec/memory_summary.hpp"

using namespace lithe::exec;

TEST_CASE (

"memory_access: is_read / is_write flags"
,
"[exec][memory]"
)
 {
    memory_access r{.kind = access_kind::read};
    CHECK(r.is_read());
    CHECK_FALSE(r.is_write());

    memory_access w{.kind = access_kind::write};
    CHECK(w.is_write());
    CHECK_FALSE(w.is_read());

    memory_access rw{.kind = access_kind::read_write};
    CHECK(rw.is_read());
    CHECK(rw.is_write());
}

TEST_CASE (

"stride_info: unit stride"
,
"[exec][memory]"
)
 {
    stride_info unit{.coeff = 1, .offset = 0, .known = true};
    CHECK(unit.coeff == 1);
    CHECK(unit.known);

    stride_info unknown{.coeff = 2, .known = false};
    CHECK_FALSE(unknown.known);
}

TEST_CASE (

"affine_index: is_unit_stride"
,
"[exec][memory]"
)
 {
    affine_index unit{.iv_id = 1, .coeff = 1, .constant = 0, .is_affine = true};
    CHECK(unit.is_unit_stride());

    affine_index strided{.iv_id = 1, .coeff = 2, .constant = 0, .is_affine = true};
    CHECK_FALSE(strided.is_unit_stride());

    affine_index non_affine{.iv_id = 1, .coeff = 1, .constant = 0, .is_affine = false};
    CHECK_FALSE(non_affine.is_unit_stride());
}

TEST_CASE (

"memory_summary: empty has no writes and no aliasing"
,
"[exec][memory]"
)
 {
    memory_summary s;
    CHECK_FALSE(s.has_writes());
    CHECK_FALSE(s.has_unknown_aliasing());
    CHECK(s.proven_no_alias());
}

TEST_CASE (

"memory_summary: has_writes when write access present"
,
"[exec][memory]"
)
 {
    memory_summary s;
    s.writes.push_back({.kind = access_kind::write});
    CHECK(s.has_writes());
}

TEST_CASE (

"alias_summary: all_no_alias with proven pairs"
,
"[exec][memory]"
)
 {
    alias_summary a;
    a.pairs.push_back({.access_a = 0, .access_b = 1, .proven_no_alias = true});
    a.pairs.push_back({.access_a = 0, .access_b = 2, .proven_no_alias = true});
    CHECK(a.all_no_alias());
}

TEST_CASE (

"alias_summary: not all_no_alias when one unproven"
,
"[exec][memory]"
)
 {
    alias_summary a;
    a.pairs.push_back({.access_a = 0, .access_b = 1, .proven_no_alias = true});
    a.pairs.push_back({.access_a = 0, .access_b = 2, .proven_no_alias = false});
    CHECK_FALSE(a.all_no_alias());
}

TEST_CASE (

"alias_summary: has_unknown_aliasing flag"
,
"[exec][memory]"
)
 {
    alias_summary a;
    a.has_unknown_aliasing = true;
    CHECK_FALSE(a.all_no_alias());
}

TEST_CASE (

"memory_summary: proven_no_alias with no pairs"
,
"[exec][memory]"
)
 {
    memory_summary s;
    CHECK(s.proven_no_alias());
}

TEST_CASE (

"to_string(access_kind)"
,
"[exec][memory]"
)
 {
    CHECK(to_string(access_kind::read)       == "read");
    CHECK(to_string(access_kind::write)      == "write");
    CHECK(to_string(access_kind::read_write) == "read_write");
}
