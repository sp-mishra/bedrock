// =============================================================================
// test_lithe_exec_effect.cpp — Unit tests for lithe_exec/effect_summary.hpp
//
// Cases:
//   1.  effect_mask: add / has / empty
//   2.  effect_mask: merge / operator|
//   3.  effect_mask: any_of
//   4.  effect_summary: is_pure
//   5.  effect_summary: has_unknown_effect flag
//   6.  effect_summary: merge
//   7.  gpu_legal: pure loop → legal
//   8.  gpu_legal: host_call → illegal
//   9.  gpu_legal: io → illegal
//  10.  gpu_legal: network → illegal
//  11.  gpu_legal: transaction → illegal
//  12.  gpu_legal: unknown_effect flag → illegal
//  13.  simd_legal: pure + reads/writes → legal
//  14.  simd_legal: atomic → illegal
//  15.  simd_legal: host_call → illegal
//  16.  threaded_legal: reads_memory + writes_memory → legal
//  17.  threaded_legal: host_call → illegal
//  18.  threaded_legal: transaction → illegal
// =============================================================================

#include "catch_amalgamated.hpp"
#include "lithe/lithe_exec/effect_summary.hpp"

using namespace lithe::exec;

TEST_CASE (

"effect_mask: add and has"
,
"[exec][effect]"
)
 {
    effect_mask m;
    CHECK(m.empty());
    m.add(effect_kind::reads_memory);
    CHECK(m.has(effect_kind::reads_memory));
    CHECK_FALSE(m.has(effect_kind::writes_memory));
}

TEST_CASE (

"effect_mask: merge and operator|"
,
"[exec][effect]"
)
 {
    effect_mask a = effect_mask::from({effect_kind::reads_memory});
    effect_mask b = effect_mask::from({effect_kind::writes_memory});
    effect_mask c = a | b;
    CHECK(c.has(effect_kind::reads_memory));
    CHECK(c.has(effect_kind::writes_memory));
    a.merge(b);
    CHECK(a.has(effect_kind::writes_memory));
}

TEST_CASE (

"effect_mask: any_of"
,
"[exec][effect]"
)
 {
    effect_mask m = effect_mask::from({effect_kind::io, effect_kind::network});
    effect_mask forbidden = effect_mask::from({effect_kind::host_call, effect_kind::io});
    CHECK(m.any_of(forbidden));
    effect_mask clean = effect_mask::from({effect_kind::reads_memory});
    CHECK_FALSE(clean.any_of(forbidden));
}

TEST_CASE (

"effect_summary: is_pure on empty"
,
"[exec][effect]"
)
 {
    effect_summary s;
    CHECK(s.is_pure());
}

TEST_CASE (

"effect_summary: has_unknown_effect flag"
,
"[exec][effect]"
)
 {
    effect_summary s;
    s.add(effect_kind::unknown);
    CHECK(s.has_unknown_effect);
    CHECK_FALSE(s.region_effects.has(effect_kind::unknown));
}

TEST_CASE (

"effect_summary: merge"
,
"[exec][effect]"
)
 {
    effect_summary a, b;
    a.add(effect_kind::reads_memory);
    b.add(effect_kind::writes_memory);
    b.has_unknown_effect = true;
    a.merge(b);
    CHECK(a.has(effect_kind::reads_memory));
    CHECK(a.has(effect_kind::writes_memory));
    CHECK(a.has_unknown_effect);
}

TEST_CASE (

"gpu_legal: pure element-wise loop"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::reads_memory);
    s.add(effect_kind::writes_memory);
    CHECK(gpu_legal(s));
}

TEST_CASE (

"gpu_legal: host_call → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::host_call);
    CHECK_FALSE(gpu_legal(s));
}

TEST_CASE (

"gpu_legal: io → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::io);
    CHECK_FALSE(gpu_legal(s));
}

TEST_CASE (

"gpu_legal: network → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::network);
    CHECK_FALSE(gpu_legal(s));
}

TEST_CASE (

"gpu_legal: transaction region → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::transaction);
    CHECK_FALSE(gpu_legal(s));
}

TEST_CASE (

"gpu_legal: unknown effect → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.has_unknown_effect = true;
    CHECK_FALSE(gpu_legal(s));
}

TEST_CASE (

"simd_legal: pure reads+writes → legal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::reads_memory);
    s.add(effect_kind::writes_memory);
    CHECK(simd_legal(s));
}

TEST_CASE (

"simd_legal: atomic → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::atomic);
    CHECK_FALSE(simd_legal(s));
}

TEST_CASE (

"simd_legal: host_call → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::host_call);
    CHECK_FALSE(simd_legal(s));
}

TEST_CASE (

"threaded_legal: reads_memory + writes_memory → legal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::reads_memory);
    s.add(effect_kind::writes_memory);
    CHECK(threaded_legal(s));
}

TEST_CASE (

"threaded_legal: host_call → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::host_call);
    CHECK_FALSE(threaded_legal(s));
}

TEST_CASE (

"threaded_legal: transaction → illegal"
,
"[exec][effect][legality]"
)
 {
    effect_summary s;
    s.add(effect_kind::transaction);
    CHECK_FALSE(threaded_legal(s));
}
