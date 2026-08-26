// Bedrock Crank examples entry point.
//
// Usage: bedrock_examples [--list | --all | --filter <tag> | --benchmark-gpu | <name>]

#include "example_crank.hpp"
#include "example_crank_tutorial.hpp"

#include <string_view>

namespace {

using examples = testfw::Registry<CrankExample, crank_tutorial::CrankTutorial>;

} // namespace

int main(const int argc, char* argv[]) {
    if (argc < 2 || std::string_view{argv[1]} == "--all") {
        return examples::run_all();
    }

    const std::string_view command{argv[1]};
    if (command == "--list") {
        examples::print_list();
        return 0;
    }

    if (command == "--filter") {
        return argc == 3 ? examples::run_by_tag(argv[2]) : 1;
    }

    if (command == "--benchmark-gpu") {
        return crank_ex::ex36_e2e_gpu_elementwise(/*benchmark_backends=*/true) ? 0 : 2;
    }

    return examples::run_by_name(command);
}
