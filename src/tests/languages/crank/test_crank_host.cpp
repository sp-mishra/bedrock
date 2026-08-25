// =============================================================================
// test_crank_host.cpp — Crank host embedding surface unit tests (Module 2).
//
// Verifies: include/languages/crank/host.hpp
//           include/languages/crank/context.hpp
//
//  1. make_host_fn_descriptor<"dot", dot>(): arity = 2, name matches.
//  2. Trampoline: dot(Vec3, Vec3) called via any[] args returns correct result.
//  3. crank::type_descriptor<Vec3>: make_host_type_descriptor produces correct name.
//  4. container_traits<std::vector<float>>: element_type = float, is_resizable = true.
//  5. container_traits<std::span<float>>: not resizable, data()/size() correct.
//  6. container_traits<std::optional<int>>: size()==1 when has_value, 0 otherwise.
//  7. container_traits<std::expected<int,std::string>>: size()==1 on success.
//  8. container_traits<std::mdspan<float,...>>: is_gpu_visible = true.
//  9. context::register_function — fn descriptor stored.
// 10. context::register_type<Vec3> — type descriptor stored.
// 11. context::register_container<std::vector<float>> — container descriptor stored.
// 12. context::modules() add_path / resolve sequence.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/crank/host.hpp"
#include "languages/crank/context.hpp"

#include <any>
#include <array>
#include <mdspan>
#include <span>
#include <vector>

// ============================================================================
// Test domain: Vec3 struct + dot product
// ============================================================================

struct Vec3 {
    float x, y, z;
};

static float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Register Vec3 with crank's host type descriptor seam.
template <>
struct crank::type_descriptor<Vec3> {
    static constexpr std::string_view name = "Vec3";
    static constexpr auto fields = std::tuple{
        crank::field < "x", &Vec3::x >{},
        crank::field < "y", &Vec3::y >{},
        crank::field < "z", &Vec3::z >{}
    };
};

// ============================================================================
// Test 1 — make_host_fn_descriptor: arity + name
// ============================================================================

TEST_CASE (

"crank host: make_host_fn_descriptor arity and name"
,
"[crank][host]"
)
 {
    auto d = crank::make_host_fn_descriptor<"math.dot", dot>();
    CHECK(d.name == "math.dot");
    CHECK(d.arity == 2u);
    CHECK(d.trampoline);
}

// ============================================================================
// Test 2 — trampoline: actual dot product via any[] args
// ============================================================================

TEST_CASE (

"crank host: trampoline invokes dot correctly"
,
"[crank][host]"
)
 {
    auto d = crank::make_host_fn_descriptor<"math.dot", dot>();

    Vec3 a{1.f, 0.f, 0.f};
    Vec3 b{1.f, 0.f, 0.f};

    std::array<std::any, 2> args{a, b};
    std::any ret = d.trampoline(std::span<const std::any>(args));
    REQUIRE(ret.has_value());
    float r = std::any_cast<float>(ret);
    CHECK(r == Catch::Approx(1.f));
}

// ============================================================================
// Test 3 — make_host_type_descriptor<Vec3>
// ============================================================================

TEST_CASE (

"crank host: make_host_type_descriptor Vec3 name"
,
"[crank][host]"
)
 {
    auto td = crank::make_host_type_descriptor<Vec3>();
    CHECK(td.name == "Vec3");
    CHECK(td.byte_size == sizeof(Vec3));
    CHECK(td.fields.size() == 3u);
    // Field names
    CHECK(td.fields[0].name == "x");
    CHECK(td.fields[1].name == "y");
    CHECK(td.fields[2].name == "z");
}

// ============================================================================
// Test 4 — container_traits<std::vector<float>>
// ============================================================================

TEST_CASE (

"crank host: container_traits vector<float>"
,
"[crank][host]"
)
 {
    using CT = crank::container_traits<std::vector<float>>;
    CHECK(CT::is_resizable == true);
    CHECK(CT::is_gpu_visible == false);

    std::vector<float> v{1.f, 2.f, 3.f};
    CHECK(CT::size(v) == 3u);
    CHECK(CT::data(v) == v.data());
}

// ============================================================================
// Test 5 — container_traits<std::span<float>>
// ============================================================================

TEST_CASE (

"crank host: container_traits span<float>"
,
"[crank][host]"
)
 {
    using CT = crank::container_traits<std::span<float>>;
    CHECK(CT::is_resizable == false);

    std::array<float, 4> arr{1.f, 2.f, 3.f, 4.f};
    std::span<float> s(arr);
    CHECK(CT::size(s) == 4u);
    CHECK(CT::data(s) == arr.data());
}

// ============================================================================
// Test 6 — container_traits<std::optional<int>>
// ============================================================================

TEST_CASE (

"crank host: container_traits optional<int>"
,
"[crank][host]"
)
 {
    using CT = crank::container_traits<std::optional<int>>;

    std::optional<int> some = 42;
    std::optional<int> none;

    CHECK(CT::size(some) == 1u);
    CHECK(CT::size(none) == 0u);
    REQUIRE(CT::data(some) != nullptr);
    CHECK(*CT::data(some) == 42);
    CHECK(CT::data(none) == nullptr);
}

// ============================================================================
// Test 7 — container_traits<std::expected<int,std::string>>
// ============================================================================

TEST_CASE (

"crank host: container_traits expected<int,string>"
,
"[crank][host]"
)
 {
    using CT = crank::container_traits<std::expected<int, std::string>>;

    std::expected<int, std::string> ok_val = 7;
    std::expected<int, std::string> err_val = std::unexpected("fail");

    CHECK(CT::size(ok_val)  == 1u);
    CHECK(CT::size(err_val) == 0u);
    REQUIRE(CT::data(ok_val) != nullptr);
    CHECK(*CT::data(ok_val) == 7);
}

// ============================================================================
// Test 8 — container_traits<std::mdspan<float,...>> is_gpu_visible
// ============================================================================

TEST_CASE (

"crank host: container_traits mdspan is_gpu_visible"
,
"[crank][host]"
)
 {
    using Extents = std::extents<std::size_t, std::dynamic_extent>;
    using MS      = std::mdspan<float, Extents>;
    using CT      = crank::container_traits<MS>;

    CHECK(CT::is_gpu_visible == true);
    CHECK(CT::is_resizable   == false);

    std::array<float, 4> data{1.f, 2.f, 3.f, 4.f};
    MS ms(data.data(), 4u);
    CHECK(CT::size(ms) == 4u);
    CHECK(CT::data(ms) == data.data());
}

// ============================================================================
// Test 9 — context::register_function stores descriptor
// ============================================================================

TEST_CASE (

"crank context: register_function stores fn descriptor"
,
"[crank][host][context]"
)
 {
    crank::context ctx;
    ctx.register_function<"math.dot", dot>();

    REQUIRE(ctx.host_functions().size() == 1u);
    CHECK(ctx.host_functions()[0].name == "math.dot");
    CHECK(ctx.host_functions()[0].arity == 2u);
}

// ============================================================================
// Test 10 — context::register_type<Vec3> stores type descriptor
// ============================================================================

TEST_CASE (

"crank context: register_type<Vec3> stores type descriptor"
,
"[crank][host][context]"
)
 {
    crank::context ctx;
    ctx.register_type<Vec3>();

    REQUIRE(ctx.host_types().size() == 1u);
    CHECK(ctx.host_types()[0].name == "Vec3");
}

// ============================================================================
// Test 11 — context::register_container<vector<float>> stores descriptor
// ============================================================================

TEST_CASE (

"crank context: register_container stores container descriptor"
,
"[crank][host][context]"
)
 {
    crank::context ctx;
    ctx.register_container<std::vector<float>>("float_vec");

    REQUIRE(ctx.host_containers().size() == 1u);
    CHECK(ctx.host_containers()[0].name == "float_vec");
    CHECK(ctx.host_containers()[0].is_resizable == true);
}

// ============================================================================
// Test 12 — context modules: add_path + resolve for unknown returns nullopt
// ============================================================================

TEST_CASE (

"crank context: resolve unknown module returns nullopt"
,
"[crank][host][context]"
)
 {
    crank::context ctx;
    auto result = ctx.import("no.such.module");
    CHECK_FALSE(result.has_value());
}

// ============================================================================
// Test 13 — stable_function_id determinism
// ============================================================================

TEST_CASE (

"crank host: stable_function_id is deterministic"
,
"[crank][host][ids]"
)
 {
    // Same qualified name → same ID every time.
    auto id_a = crank::detail::make_id("math.dot", crank::kKindFunction);
    auto id_b = crank::detail::make_id("math.dot", crank::kKindFunction);
    CHECK(id_a == id_b);

    // Different name → different ID.
    auto id_c = crank::detail::make_id("math.cross", crank::kKindFunction);
    CHECK(id_a != id_c);

    // Different kind → different ID (same name).
    auto id_type = crank::detail::make_id("math.dot", crank::kKindType);
    CHECK(id_a != id_type);
}

// ============================================================================
// Test 14 — typed thunk: direct invocation without std::any
// ============================================================================

TEST_CASE (

"crank host: typed_thunk invokes without std::any"
,
"[crank][host][typed]"
)
 {
    auto d = crank::make_host_fn_descriptor<"math.dot", dot>();
    REQUIRE(d.typed_thunk != nullptr);

    Vec3 a{3.f, 0.f, 0.f};
    Vec3 b{2.f, 0.f, 0.f};
    const void* args[2] = {&a, &b};
    float result = 0.f;
    d.typed_thunk(args, &result);
    CHECK(result == Catch::Approx(6.f));
}

// ============================================================================
// Test 15 — function_descriptor fields populated correctly
// ============================================================================

TEST_CASE (

"crank host: function_descriptor fields"
,
"[crank][host][descriptor]"
)
 {
    using opts = crank::function_options;
    auto d = crank::make_host_fn_descriptor<"math.dot", dot>(
        opts{.effects = 0, .flags = static_cast<crank::function_flags>(
                crank::function_flag::pure)});
    CHECK(d.name == "math.dot");
    CHECK(d.arity == 2u);
    CHECK(d.flags == static_cast<crank::function_flags>(crank::function_flag::pure));
    CHECK(d.fingerprint != 0u);
    // ID should be deterministic.
    auto expected_id = crank::detail::make_id("math.dot", crank::kKindFunction);
    CHECK(d.id == expected_id);
}

// ============================================================================
// Test 16 — type_operation_table populated for trivially-copyable type
// ============================================================================

TEST_CASE (

"crank host: type_operation_table trivial type"
,
"[crank][host][type_ops]"
)
 {
    auto td = crank::make_host_type_descriptor<Vec3>();
    CHECK(td.operations.copy    != nullptr);  // trivially copyable
    CHECK(td.operations.move    != nullptr);  // move constructible
    CHECK(td.operations.destroy != nullptr);  // destructible
    // Vec3 has no operator== so equal should be nullptr.
    CHECK(td.operations.equal   == nullptr);
}

struct EqVec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    bool operator==(const EqVec3&) const = default;
};

template <>
struct crank::type_descriptor<EqVec3> {
    static constexpr std::string_view name = "EqVec3";
    static constexpr auto fields = std::tuple{
        crank::field < "x", &EqVec3::x >{},
        crank::field < "y", &EqVec3::y >{}
    };
};

TEST_CASE (

"crank host: type_operation_table equality-comparable type"
,
"[crank][host][type_ops]"
)
 {
    auto td = crank::make_host_type_descriptor<EqVec3>();
    CHECK(td.operations.equal != nullptr);
    EqVec3 a{1.f, 2.f}, b{1.f, 2.f}, c{3.f, 4.f};
    CHECK(td.operations.equal(&a, &b) == true);
    CHECK(td.operations.equal(&a, &c) == false);
}

// ============================================================================
// Test 17 — container_capabilities computed correctly
// ============================================================================

TEST_CASE (

"crank host: container_capabilities vector<float>"
,
"[crank][host][container]"
)
 {
    auto d = crank::make_host_container_descriptor<std::vector<float>>("float_vec");
    using C = crank::container_cap;
    CHECK((d.capabilities & static_cast<crank::container_capabilities>(C::sized))     != 0u);
    CHECK((d.capabilities & static_cast<crank::container_capabilities>(C::resizable)) != 0u);
    CHECK((d.capabilities & static_cast<crank::container_capabilities>(C::mutable_))  != 0u);
    CHECK((d.capabilities & static_cast<crank::container_capabilities>(C::contiguous))!= 0u);
    CHECK((d.capabilities & static_cast<crank::container_capabilities>(C::gpu_visible))== 0u);
}

TEST_CASE (

"crank host: container_capabilities span<float>"
,
"[crank][host][container]"
)
 {
    auto d = crank::make_host_container_descriptor<std::span<float>>("float_span");
    using C = crank::container_cap;
    CHECK((d.capabilities & static_cast<crank::container_capabilities>(C::sized))      != 0u);
    CHECK((d.capabilities & static_cast<crank::container_capabilities>(C::contiguous))  != 0u);
    CHECK((d.capabilities & static_cast<crank::container_capabilities>(C::resizable))  == 0u);
}

// ============================================================================
// Test 18 — resource_descriptor registration
// ============================================================================

struct AccountStore {
    int dummy = 0;
};

TEST_CASE (

"crank host: register_resource stores descriptor"
,
"[crank][host][resource]"
)
 {
    crank::context_builder builder;
    builder.register_resource<AccountStore>("accounts",
        crank::resource_options{.threading = crank::resource_threading::concurrent});
    auto result = std::move(builder).finalize();
    REQUIRE(result.has_value());
    REQUIRE(result->resources().size() == 1u);
    CHECK(result->resources()[0].name == "accounts");
    CHECK(result->resources()[0].threading == crank::resource_threading::concurrent);
}

// ============================================================================
// Test 19 — context_builder → finalized_context round-trip
// ============================================================================

TEST_CASE (

"crank host: context_builder finalize round-trip"
,
"[crank][host][builder]"
)
 {
    crank::context_builder builder;
    builder.register_function<"math.dot", dot>();
    builder.register_type<Vec3>();
    builder.register_container<std::vector<float>>("float_vec");

    auto result = std::move(builder).finalize();
    REQUIRE(result.has_value());

    const auto& ctx = *result;
    CHECK(ctx.functions().size()   == 1u);
    CHECK(ctx.types().size()       == 1u);
    CHECK(ctx.containers().size()  == 1u);
    CHECK(ctx.functions()[0].name  == "math.dot");
    CHECK(ctx.types()[0].name      == "Vec3");
    CHECK(ctx.fingerprint()        != 0u);
}

// ============================================================================
// Test 20 — finalization rejects duplicate registration
// ============================================================================

static float dot_f(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

TEST_CASE (

"crank host: duplicate function registration produces error"
,
"[crank][host][builder]"
)
 {
    crank::context_builder builder;
    builder.register_function<"math.dot", dot>();
    builder.register_function<"math.dot", dot_f>();  // same signature → collision
    auto result = std::move(builder).finalize();
    CHECK_FALSE(result.has_value());
    if (!result.has_value()) {
        REQUIRE(!result.error().empty());
        CHECK(result.error()[0].message.find("math.dot") != std::string::npos);
    }
}

// ============================================================================
// Test 21 — error_adapter detection
// ============================================================================

struct MyHostError {
    std::string msg;
};

template <>
struct crank::error_adapter<MyHostError> {
    static crank::host_error convert(const MyHostError& e) {
        return {1u, e.msg, "MyHostError"};
    }
};

TEST_CASE (

"crank host: has_error_adapter detection"
,
"[crank][host][error]"
)
 {
    static_assert(!crank::has_error_adapter<std::runtime_error>,
        "std::runtime_error should have no default adapter");
    static_assert(crank::has_error_adapter<MyHostError>,
        "MyHostError should have adapter");
    CHECK(crank::has_error_adapter<MyHostError>);
    CHECK(!crank::has_error_adapter<std::runtime_error>);
}

// ============================================================================
// Test 22 — owned_host_value SBO path
// ============================================================================

TEST_CASE (

"crank host: owned_host_value SBO for small trivial type"
,
"[crank][host][owned_value]"
)
 {
    auto v = crank::owned_host_value::make(42);
    REQUIRE(v.has_value());
    CHECK(*static_cast<const int*>(v.get()) == 42);
}

TEST_CASE (

"crank host: owned_host_value non-trivial type"
,
"[crank][host][owned_value]"
)
 {
    std::string s = "hello world from owned_host_value";
    auto v = crank::owned_host_value::make(std::string(s));
    REQUIRE(v.has_value());
    CHECK(*static_cast<const std::string*>(v.get()) == s);
}
