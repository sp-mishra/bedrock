// Crank Language Tutorial — loads .ck scripts from src/examples/crank/resources/,
// runs parse → analyse → lower → execute for each, and displays results.
//
// The resources are adjacent to this header, so the example remains runnable
// outside a CMake build. No Crank source strings live in this header.

#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "test/example_registry.hpp"
#include "utils/log.hpp"
#include "languages/crank/frontend.hpp"
#include "languages/crank/context.hpp"
#include "languages/crank/lower_hl.hpp"
#include "languages/crank/execute.hpp"

namespace crank_tutorial {

using namespace testfw;

namespace fs = std::filesystem;

struct CrankTutorial {
    static constexpr std::string_view name() { return "crank_tutorial"; }
    static constexpr std::string_view description() {
        return "Load and execute Crank tutorial scripts from src/examples/crank/resources/";
    }
    static constexpr std::span<const std::string_view> tags() {
        static constexpr std::string_view t[] = {
            "crank", "tutorial", "beginner", "intermediate", "advanced",
            "types", "generics", "data-structures", "safety", "verification"};
        return t;
    }

    static Result run() {
        const fs::path resource_dir = resource_directory();

        if (!fs::exists(resource_dir))
            return fail("resource directory not found");

        // Collect tutorial scripts (ex*.ck), sorted for deterministic order.
        std::vector<fs::path> scripts;
        for (const auto& entry : fs::directory_iterator(resource_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".ck") {
                const auto stem = entry.path().stem().string();
                if (stem.starts_with("ex"))
                    scripts.push_back(entry.path());
            }
        }

        if (scripts.empty())
            return fail("no tutorial .ck scripts found");

        std::sort(scripts.begin(), scripts.end());

        std::size_t passed = 0;
        std::size_t failed = 0;

        for (const auto& path : scripts) {
            const std::string src = read_file(path);
            if (src.empty()) {
                lg::warn("crank tutorial: skipping empty file {}", path.filename().string());
                continue;
            }

            if (auto r = run_script(path.stem().string(), src); !r) {
                lg::error("crank tutorial [{}]: {}", path.filename().string(),
                          r.error().message);
                ++failed;
            } else {
                ++passed;
            }
        }

        lg::info("crank tutorial: {}/{} scripts passed", passed, passed + failed);

        if (failed > 0)
            return fail("one or more tutorial scripts failed");
        return {};
    }

 private:
    [[nodiscard]] static fs::path resource_directory(
        const std::source_location site = std::source_location::current()) {
        return fs::path{site.file_name()}.parent_path() / "resources";
    }

    // Run full pipeline for one script: parse → analyse → lower → execute → display.
    [[nodiscard]] static Result run_script(std::string_view label, std::string_view src) {
        // 1. Parse
        const auto parse = crank::frontend::parse(src);
        if (!parse.ok)
            return fail("parse failed");
        if (parse.diagnostics.has_errors())
            return fail("parse diagnostics errors");

        // 2. Analyse (resolve + types + effects)
        crank::context ctx;
        const auto ar = ctx.analyse(parse, std::string{label});
        if (!ar.ok)
            return fail("analyse failed");

        // 3. Lower to HL MIR (Main entry point, no extra loops/defers for tutorial scripts)
        crank::lower_input inp;
        inp.fn_name = "Main";
        const auto hl = crank::lower_to_hl(std::move(inp));
        if (!hl.ok())
            return fail("lower_to_hl failed");

        // 4. Execute via interpreter
        const auto exec = crank::execute_via_interpreter(hl);
        if (!exec.ok())
            return fail("execute failed");

        // 5. Display result
        display_result(label, exec);
        return {};
    }

    static void display_result(std::string_view label,
                                const crank::crank_execute_result& exec) {
        const std::string status = std::string{crank::to_string(exec.status)};

        if (exec.return_value.has_value()) {
            lg::info("crank tutorial [{}]: status={} return={}", label, status,
                     exec.return_value.value());
        } else {
            lg::info("crank tutorial [{}]: status={} return=<none>", label, status);
        }

        if (exec.fallback_fired)
            lg::info("crank tutorial [{}]: fallback backend used", label);

        for (const auto& note : exec.notes)
            lg::info("crank tutorial [{}]: note: {}", label, note);
    }

    [[nodiscard]] static std::string read_file(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        return std::string(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>{});
    }
};

} // namespace crank_tutorial
