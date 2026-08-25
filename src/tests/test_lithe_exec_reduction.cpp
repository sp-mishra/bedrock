// =============================================================================
// test_lithe_exec_reduction.cpp — Unit tests for lithe_exec/reduction.hpp
//
// Cases:
//   1.  make_reduction_info: integer add → associative, reorder allowed
//   2.  make_reduction_info: FP add, deterministic=false → reorder allowed
//   3.  make_reduction_info: FP add, deterministic=true → reorder NOT allowed
//   4.  make_reduction_info: integer add, deterministic=true → reorder still allowed (int)
//   5.  make_reduction_info: mul → associative + commutative
//   6.  make_reduction_info: min/max → associative, not fp_reorder_eligible
//   7.  make_reduction_info: custom → not associative (no contract)
//   8.  reduction_contract concept: conforming type satisfies it
//   9.  reduction_contract concept: non-conforming type does not satisfy it
//  10.  to_string(reduction_op) spot checks
// =============================================================================

#include "catch_amalgamated.hpp"
#include "lithe/lithe_exec/reduction.hpp"

using namespace lithe::exec;

namespace {
    struct conforming_contract {
        static constexpr std::uint32_t op_id = 1001;
        static constexpr bool associative = true;
        static constexpr bool deterministic = false;
        static constexpr std::uint64_t identity_value() noexcept { return 0; }
    };

    static_assert(reduction_contract<conforming_contract>);

    struct bad_contract {
        int x;
    };

    static_assert(!reduction_contract<bad_contract>);
} // namespace

TEST_CASE (

"reduction: integer add → associative + reorder allowed"
,
"[exec][reduction]"
)
 {
    auto r = make_reduction_info(1, 2, reduction_op::add,
                                 /*deterministic=*/false, /*is_integer=*/true);
    CHECK(r.associative);
    CHECK(r.commutative);
    CHECK(r.fp_reordering_allowed);
}

TEST_CASE (

"reduction: FP add non-deterministic → reorder allowed"
,
"[exec][reduction]"
)
 {
    auto r = make_reduction_info(1, 2, reduction_op::add,
                                 /*deterministic=*/false, /*is_integer=*/false);
    CHECK(r.associative);
    CHECK(r.fp_reordering_allowed);
    CHECK_FALSE(r.deterministic);
}

TEST_CASE (

"reduction: FP add deterministic → reorder NOT allowed"
,
"[exec][reduction]"
)
 {
    auto r = make_reduction_info(1, 2, reduction_op::add,
                                 /*deterministic=*/true, /*is_integer=*/false);
    CHECK(r.associative);
    CHECK_FALSE(r.fp_reordering_allowed);
    CHECK(r.deterministic);
}

TEST_CASE (

"reduction: integer add deterministic → reorder still allowed"
,
"[exec][reduction]"
)
 {
    auto r = make_reduction_info(1, 2, reduction_op::add,
                                 /*deterministic=*/true, /*is_integer=*/true);
    CHECK(r.fp_reordering_allowed); // integer add is always reorderable
}

TEST_CASE (

"reduction: mul → associative + commutative"
,
"[exec][reduction]"
)
 {
    auto r = make_reduction_info(1, 2, reduction_op::mul, false, true);
    CHECK(r.associative);
    CHECK(r.commutative);
}

TEST_CASE (

"reduction: min → associative, not fp_reorder_eligible"
,
"[exec][reduction]"
)
 {
    auto r = make_reduction_info(1, 2, reduction_op::min_val, false, false);
    CHECK(r.associative);
    CHECK_FALSE(impl::traits_for(reduction_op::min_val).fp_reorder_eligible);
}

TEST_CASE (

"reduction: custom → not associative (no contract)"
,
"[exec][reduction]"
)
 {
    auto r = make_reduction_info(1, 2, reduction_op::custom, false, false);
    CHECK_FALSE(r.associative);
    CHECK_FALSE(r.commutative);
}

TEST_CASE (

"reduction_contract concept: conforming type"
,
"[exec][reduction]"
)
 {
    static_assert(reduction_contract<conforming_contract>);
    SUCCEED();
}

TEST_CASE (

"reduction_contract concept: non-conforming type"
,
"[exec][reduction]"
)
 {
    static_assert(!reduction_contract<bad_contract>);
    SUCCEED();
}

TEST_CASE (

"to_string(reduction_op) spot checks"
,
"[exec][reduction]"
)
 {
    CHECK(to_string(reduction_op::add)     == "add");
    CHECK(to_string(reduction_op::mul)     == "mul");
    CHECK(to_string(reduction_op::custom)  == "custom");
    CHECK(to_string(reduction_op::min_val) == "min");
}
