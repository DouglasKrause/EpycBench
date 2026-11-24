#include "cli/application.hpp"

#include "cli/scenario_command.hpp"
#include "cli/package_run.hpp"
#include "runtime/package_catalog.hpp"
#include "platform_linux/package_process.hpp"
#include "cli/terminal_menu.hpp"
#include "cli/tui.hpp"
#include "cli/workbench_actions.hpp"
#include "cli/machine_commands.hpp"
#include "platform_linux/presentation_files.hpp"
#include "cli/results_command.hpp"
#include "cli/benchmark_run_command.hpp"
#include "cli/experiment_command.hpp"
#include <string_view>

#include <optional>
#include <vector>
#include <iostream>

namespace epycbench::cli {

auto run(int argc, char* argv[], topology::cli::Environment& environment,
         std::ostream& output,
         std::ostream& error) -> int {
    std::optional<std::filesystem::path> module_directory;
    std::vector<char*> filtered;
    filtered.reserve(static_cast<std::size_t>(argc) + 1);
    for (int i = 0; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--module-dir") {
            if (module_directory || i + 1 >= argc || std::string_view{argv[i + 1]}.empty()) {
                error << "epycbench: --module-dir requires one directory\n";
                return 2;
            }
            module_directory = argv[++i];
        } else filtered.push_back(argv[i]);
    }
    argc = static_cast<int>(filtered.size());
    filtered.push_back(nullptr);
    argv = filtered.data();
    if ((argc == 1 && &output == &std::cout && platform_linux::presentation_files::interactive_terminal()) ||
        (argc >= 2 && std::string_view{argv[1]} == "tui")) {
        std::optional<std::filesystem::path> path, workers;
        constexpr auto usage = "Usage: epycbench tui [PATH] [--supplied-workers FILE]\n";
        for (int i = 2; i < argc; ++i) {
            const std::string_view word{argv[i]};
            if (word == "--help" || word == "-h") { output << usage; return 0; }
            if (word == "--supplied-workers" && i + 1 < argc && !workers) workers = argv[++i];
            else if (!path && !word.starts_with('-')) path = argv[i];
            else { error << usage; return 2; }
        }
        return tui::launch(path, environment, module_directory, error, workers);
    }
    if (argc >= 2 && std::string_view{argv[1]} == "kernel") {
        if (argc != 3 || (std::string_view{argv[2]} != "list" && std::string_view{argv[2]} != "validate")) {
            const bool help = argc == 3 && std::string_view{argv[2]} == "--help";
            (help ? output : error) << "Usage: epycbench kernel list|validate [--module-dir DIRECTORY]\n";
            return help ? 0 : 2;
        }
        const auto catalog = runtime::discover_packages(module_directory);
        if (!catalog) {
            error << catalog.error().message << ' ' << catalog.error().context.dump() << '\n';
            return 6;
        }
        if (catalog->implementations().empty()) { error << "No *.kernel.yaml manifests found.\n"; return 6; }
        auto rows=nlohmann::json::array();
        for (const auto& impl:catalog->implementations()) rows.push_back(kernel::packages::package_identity(impl));
        if (std::string_view{argv[2]}=="validate") {
            std::vector<std::filesystem::path> paths;
            for (const auto& impl:catalog->implementations()) paths.push_back(impl.path);
            auto reply=platform_linux::package_process::collect({{"operation","validate"},{"manifests",paths}});
            if (!reply) {
                error << reply.error().code.value() << ' ' << reply.error().message << ' ' << reply.error().context.dump() << '\n';
                return 6;
            }
            if (reply->at("validated")!=rows) { error << "Installed packages changed during validation\n"; return 6; }
        }
        output << rows.dump(2) << '\n';
        return output ? 0 : 3;
    }
    if (argc == 1) {
        const auto workbench = make_workbench_entries(std::cin, output,
            [&](std::span<const std::string> words, std::ostream& command_output) {
                std::vector<std::string> arguments(words.begin(), words.end());
                if (module_directory) {
                    arguments.emplace_back("--module-dir");
                    arguments.push_back(module_directory->string());
                }
                std::vector<char*> pointers;
                pointers.reserve(arguments.size());
                for (auto& argument : arguments) pointers.push_back(argument.data());
                return epycbench::cli::run(static_cast<int>(pointers.size()), pointers.data(), environment,
                    command_output, error);
            },
            [&](const std::filesystem::path& path, std::string_view bytes) {
                if (!environment.publish_file) return 1;
                const auto saved = environment.publish_file(path, bytes, false);
                if (saved) return 0;
                error << "Could not save the requested file (" << saved.error().code << ").\n";
                return 3;
            }, [&] { return edit_saved_scenario(std::cin, output, error, environment, module_directory); });
        return run_terminal_menu(std::cin, output, error,
            [&] {
                output << "\nEpycBench measures memory performance and compares task implementations.\n"
                          "Choose Demo test run to use the built-in fixed example settings.\n"
                          "Demo results are saved in epycbench-demo-results in the current directory.\n"
                          "Build a Scenario (6) and save the comparison.\n"
                          "Create a sweep after saving, or reopen a saved Scenario with Build a sweep recipe (11).\n"
                          "Plan the saved recipe with an attempt budget using the experiment queue (18) or search queue (24).\n"
                          "For custom settings: epycbench run SCENARIO\n"
                          "For result summaries: epycbench result export --profile summary RESULT\n\n";
                char program[]="epycbench";
                char help[]="--help";
                char* words[]{program,help};
                return topology::cli::run(2,words,environment,output,error);
            },
            [&] {
                return run_package_demo(module_directory,environment,output,error);
            },
            [&] {return inspect_machine(environment,output,error);},
            [&] {return view_machine(environment,output,error);}, workbench);
    }
    if (argc >= 2 && std::string_view{argv[1]} == "run") {
        return run_package(argc, argv, module_directory, environment, output, error);
    }
    if (argc >= 2 && std::string_view{argv[1]} == "scenario") {
        return run_scenario_command(argc, argv, environment, output, error, module_directory, &std::cin);
    }
    if (argc >= 2 && std::string_view{argv[1]} == "experiment") {
        return run_experiment_command(argc, argv, environment, output, error, module_directory);
    }
    if (argc >= 2 && std::string_view{argv[1]} == "result") {
        return run_results_command(argc, argv, environment, output, error,
            &output==&std::cout&&platform_linux::presentation_files::stdout_is_terminal());
    }
    const auto status = topology::cli::run(argc, argv, environment, output, error);
    if (status == 0 && argc == 2 && (std::string_view{argv[1]} == "--help" || std::string_view{argv[1]} == "-h")) {
        output << "\nExperiment workflow\n"
                  "  tui [PATH]          Open Workbench Home, a saved Result, or a frozen study.\n"
                  "  kernel list         Discover YAML packages. Use kernel validate to check their binary interfaces.\n"
                  "  scenario build      Create a base Scenario on a saved topology.\n"
                  "  scenario sweep      Use --scenario FILE to author a sweep or --edit FILE to revise a recipe.\n"
                  "  experiment plan     Choose a search strategy and attempt budget for the saved recipe.\n";
    }
    return status;
}

} // namespace epycbench::cli
