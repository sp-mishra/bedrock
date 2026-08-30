// =============================================================================
// test_taranga_std.cpp — Taranga stdlib registry tests.
//
// Verifies: include/languages/taranga/std/*.hpp
//
// The Taranga stdlib mirrors crank's install-per-module shape and currently
// exposes a lightweight host registry for std symbols. These tests validate
// registration, lookup, and direct callback execution.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/taranga/std/std.hpp"

#include <array>
#include <cstdint>
#include <span>

TEST_CASE("taranga std: math install + invoke", "[taranga][std][math]") {
    taranga::stdlib::registry reg;
    taranga::stdlib::install_std_math(reg);

    const auto* add = reg.find("std.math.add_i64");
    REQUIRE(add != nullptr);
    CHECK(add->arity == 2u);
    REQUIRE(add->fn != nullptr);

    std::array<std::int64_t, 2> args{20, 22};
    CHECK(add->fn(std::span<const std::int64_t>(args)) == 42);
}

TEST_CASE("taranga std: core clamp", "[taranga][std][core]") {
    taranga::stdlib::registry reg;
    taranga::stdlib::install_std_core(reg);

    const auto* clamp = reg.find("std.core.clamp_i64");
    REQUIRE(clamp != nullptr);
    CHECK(clamp->arity == 3u);
    REQUIRE(clamp->fn != nullptr);

    std::array<std::int64_t, 3> low{-5, 0, 100};
    std::array<std::int64_t, 3> high{205, 0, 100};
    std::array<std::int64_t, 3> ok{42, 0, 100};
    CHECK(clamp->fn(std::span<const std::int64_t>(low)) == 0);
    CHECK(clamp->fn(std::span<const std::int64_t>(high)) == 100);
    CHECK(clamp->fn(std::span<const std::int64_t>(ok)) == 42);
}

TEST_CASE("taranga std: install_all has representative symbols", "[taranga][std][all]") {
    taranga::stdlib::registry reg;
    taranga::stdlib::install_std_all(reg);

    CHECK(reg.find("std.core.identity_i64") != nullptr);
    CHECK(reg.find("std.math.mul_i64") != nullptr);
    CHECK(reg.find("std.time.now_ns") != nullptr);
    CHECK(reg.find("std.io.println_i64") != nullptr);
}

