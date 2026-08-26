#include "catch_amalgamated.hpp"

#include "lithe/lithe_execution/compile.hpp"

#include <type_traits>

TEST_CASE("Lithe observed execution APIs remain statically selectable", "[lithe][telemetry]") {
    using observer = lang::telemetry::phase_observer<>;
    using prepared = lithe::execution::prepared_execution;

    static_assert(requires(const prepared& execution) {
        { execution.template invoke_observed<observer>() };
        { execution.native_entry() };
    });
    SUCCEED();
}
