#pragma once

// lithe_pattern.hpp — compatibility shim.
//
// The structural pattern-matching DSL now lives in the standalone Vākya library
// (include/vakya/pattern.hpp) under namespace vakya::pattern. This header
// re-exports it into namespace lithe::pattern so existing lithe:: call sites and
// tests keep working unchanged.
//
// Not included by lithe.hpp; opt-in via:  #include "lithe/lithe_pattern.hpp"
// Namespace: lithe::pattern

#include "lithe_passes.hpp"       // pulls lithe_core.hpp (Vākya shim) + extension
#include "vakya/pattern.hpp"

namespace lithe::pattern {
    // Types
    using vakya::pattern::match_result;
    using vakya::pattern::pattern_var;
    using vakya::pattern::literal_pattern;
    using vakya::pattern::rewrite_rule;
    using vakya::pattern::rule_set;

    // Concept
    using vakya::pattern::Pattern;

    // Public API
    using vakya::pattern::match_pattern;
    using vakya::pattern::rule;
    using vakya::pattern::make_rule_set;

    // DSL builder helpers (variable templates)
    using vakya::pattern::pv;
    using vakya::pattern::lit;
    using vakya::pattern::add;
    using vakya::pattern::sub;
    using vakya::pattern::mul;
    using vakya::pattern::div_;
    using vakya::pattern::neg;

    namespace detail {
        using namespace vakya::pattern::detail;
    }

    namespace rules { namespace arithmetic {
        using namespace vakya::pattern::rules::arithmetic;
    }}} // namespace lithe::pattern
