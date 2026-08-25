// =============================================================================
// test_crank_reflect.cpp — §v2.16 restricted reflection.
//
// Verifies:
//   1.  reflect_builder assembles a type_descriptor with fields/traits/caps.
//   2.  facet bitmask reflects exactly what the builder populated.
//   3.  layout_is_gpu_safe true for a POD, false for a non-standard-layout type.
//   4.  capability bits accumulate into the mask.
//   5.  host_registration / backend_adapters facets are recorded.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/reflect.hpp"

#include <bit>
#include <cstddef>
#include <string>

using namespace crank;

namespace {
    // A standard-layout, trivially-copyable vec3 — GPU/SIMD uploadable.
    struct Vec3 {
        float x;
        float y;
        float z;
    };

    // Not trivially copyable (has a std::string) — not GPU-safe.
    struct HasString {
        int id;
        std::string label;
    };
}

TEST_CASE (

"v2.16 reflect_builder assembles a descriptor"
,
"[crank][reflect][v2]"
)
 {
    auto d = reflect_builder<Vec3>("Vec3")
                 .field("x", "Float32", offsetof(Vec3, x), sizeof(float))
                 .field("y", "Float32", offsetof(Vec3, y), sizeof(float))
                 .field("z", "Float32", offsetof(Vec3, z), sizeof(float))
                 .satisfies("Numeric")
                 .build();

    CHECK(d.name == "Vec3");
    CHECK(d.size == sizeof(Vec3));
    CHECK(d.align == alignof(Vec3));
    REQUIRE(d.fields.size() == 3u);
    CHECK(d.fields[0].name == "x");
    CHECK(d.fields[1].offset == offsetof(Vec3, y));
    CHECK(d.fields[2].size == sizeof(float));
    REQUIRE(d.satisfied_traits.size() == 1u);
    CHECK(d.satisfied_traits[0] == "Numeric");
}

TEST_CASE (

"v2.16 facet mask reflects populated facets"
,
"[crank][reflect][v2]"
)
 {
    auto d = reflect_builder<Vec3>("Vec3")
                 .field("x", "Float32", 0, 4)
                 .satisfies("Numeric")
                 .build();
    CHECK(d.has(reflect_facet::fields));
    CHECK(d.has(reflect_facet::traits));
    CHECK_FALSE(d.has(reflect_facet::capabilities));
    CHECK_FALSE(d.has(reflect_facet::host_registration));
    CHECK_FALSE(d.has(reflect_facet::backend_adapters));
}

TEST_CASE (

"v2.16 layout_is_gpu_safe distinguishes POD from non-POD"
,
"[crank][reflect][v2]"
)
 {
    auto pod = reflect_builder<Vec3>("Vec3").build();
    CHECK(pod.layout_is_gpu_safe());
    CHECK(type_descriptor<Vec3>::type_is_gpu_safe());

    auto npod = reflect_builder<HasString>("HasString").build();
    CHECK_FALSE(npod.layout_is_gpu_safe());
    CHECK_FALSE(type_descriptor<HasString>::type_is_gpu_safe());
}

TEST_CASE (

"v2.16 capability bits accumulate into mask"
,
"[crank][reflect][v2]"
)
 {
    auto d = reflect_builder<Vec3>("Vec3")
                 .capability(0)
                 .capability(3)
                 .build();
    CHECK(d.has(reflect_facet::capabilities));
    CHECK((d.capability_mask & (1u << 0)) != 0u);
    CHECK((d.capability_mask & (1u << 3)) != 0u);
    CHECK((d.capability_mask & (1u << 1)) == 0u);
}

TEST_CASE (

"v2.16 host_registration + backend_adapters facets recorded"
,
"[crank][reflect][v2]"
)
 {
    auto d = reflect_builder<Vec3>("Vec3")
                 .host_registration()
                 .backend_adapters()
                 .build();
    CHECK(d.has(reflect_facet::host_registration));
    CHECK(d.has(reflect_facet::backend_adapters));
    CHECK_FALSE(d.has(reflect_facet::fields));
}

// ============================================================================
// §v2.16 layout stability — layout_context + layout_fingerprint
// ============================================================================

TEST_CASE (

"v2.16 descriptor carries a native layout_context by default"
,
"[crank][reflect][v2]"
)
 {
    auto d = reflect_builder<Vec3>("Vec3").build();
    CHECK(d.layout.endianness == std::endian::native);
    CHECK(d.layout.alignment == static_cast<std::uint32_t>(alignof(Vec3)));
}

TEST_CASE (

"v2.16 layout_fingerprint is stable for identical inputs"
,
"[crank][reflect][v2]"
)
 {
    auto a = reflect_builder<Vec3>("Vec3")
                 .field("x", "Float32", offsetof(Vec3, x), sizeof(float))
                 .field("y", "Float32", offsetof(Vec3, y), sizeof(float))
                 .build();
    auto b = reflect_builder<Vec3>("Vec3")
                 .field("x", "Float32", offsetof(Vec3, x), sizeof(float))
                 .field("y", "Float32", offsetof(Vec3, y), sizeof(float))
                 .build();
    CHECK(a.layout_fingerprint() == b.layout_fingerprint());
}

TEST_CASE (

"v2.16 layout_fingerprint changes when the layout context changes"
,
"[crank][reflect][v2]"
)
 {
    auto base = reflect_builder<Vec3>("Vec3")
                    .field("x", "Float32", 0, 4)
                    .build();

    crank::layout_context packed = crank::layout_context::native_for<Vec3>();
    packed.packing = 1;
    auto p = reflect_builder<Vec3>("Vec3")
                 .field("x", "Float32", 0, 4)
                 .layout(packed)
                 .build();

    crank::layout_context be = crank::layout_context::native_for<Vec3>();
    be.endianness = std::endian::big;
    auto e = reflect_builder<Vec3>("Vec3")
                 .field("x", "Float32", 0, 4)
                 .layout(be)
                 .build();

    crank::layout_context v2 = crank::layout_context::native_for<Vec3>();
    v2.layout_version = 2;
    auto vv = reflect_builder<Vec3>("Vec3")
                  .field("x", "Float32", 0, 4)
                  .layout(v2)
                  .build();

    CHECK(base.layout_fingerprint() != p.layout_fingerprint());
    CHECK(base.layout_fingerprint() != e.layout_fingerprint());
    CHECK(base.layout_fingerprint() != vv.layout_fingerprint());
}

TEST_CASE (

"v2.16 layout_fingerprint changes when a field offset shifts"
,
"[crank][reflect][v2]"
)
 {
    auto a = reflect_builder<Vec3>("Vec3").field("x", "Float32", 0, 4).build();
    auto b = reflect_builder<Vec3>("Vec3").field("x", "Float32", 8, 4).build();
    CHECK(a.layout_fingerprint() != b.layout_fingerprint());
}

TEST_CASE (

"v2.16 reflection_matches gates on layout context identity"
,
"[crank][reflect][v2]"
)
 {
    auto ctx = crank::layout_context::native_for<Vec3>();
    auto other = ctx;
    other.layout_version = 99;
    CHECK(crank::reflection_matches(ctx, ctx));
    CHECK_FALSE(crank::reflection_matches(ctx, other));
}
