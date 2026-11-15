#include "cli/application.hpp"

#include "cli/scenario_command.hpp"
#include "cli/terminal_menu.hpp"
#include "cli/machine_commands.hpp"
#include "platform_linux/presentation_files.hpp"
#include "cli/results_command.hpp"
#include "cli/benchmark_run_command.hpp"
#include "cli/monitoring_q0b_command.hpp"
#include "cli/monitoring_q1_command.hpp"
#include "cli/monitoring_q2_command.hpp"
#include "cli/monitoring_q3_command.hpp"
#include "cli/monitoring_q4_command.hpp"

#include <string_view>
#include "runtime/kernel_module_session.hpp"
#include <optional>
#include <vector>
#include <iostream>

namespace epycbench::cli {

auto run(int argc, char* argv[], topology::cli::Environment& environment,
         runtime::ControllerProcessProductionAuthority& production_authority, std::ostream& output,
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
    // Offline result commands do not load modules or require their installation.
    const bool benchmarking = argc == 1 || (argc > 1 && std::string_view{argv[1]} == "run");
    const auto modules = benchmarking ? runtime::initialize_kernel_modules(module_directory)
                                     : std::expected<void, std::string>{};
    if (!modules && argc > 1) {
        error << "epycbench: kernel modules: " << modules.error() << '\n';
        return 2;
    }
    if (argc == 1) {
        return run_terminal_menu(std::cin, output, error,
            [&] {
                output << "\nEpycBench measures memory bandwidth and memory-access latency.\n"
                          "Choose Demo test run to use the built-in fixed example settings.\n"
                          "Demo results are saved in epycbench-demo-results in the current directory.\n"
                          "For custom settings: epycbench run SCENARIO\n"
                          "For result summaries: epycbench result export --profile summary RESULT\n\n";
                char program[]="epycbench";
                char help[]="--help";
                char* words[]{program,help};
                return topology::cli::run(2,words,environment,output,error);
            },
            [&] {
                if (!modules) { error << "epycbench: kernel modules: " << modules.error() << '\n'; return 2; }
                return benchmark_run_command::run_demo(production_authority,output,error);
            },
            [&] {return inspect_machine(environment,output,error);},
            [&] {return view_machine(environment,output,error);});
    }
    if (argc >= 2 && std::string_view{argv[1]} == "run")
        return benchmark_run_command::run(argc, argv, production_authority, output, error);
    if (argc >= 2 && std::string_view{argv[1]} == "__monitoring-q0b-c0-characterize")
        return run_monitoring_q0b_command(argc, argv, production_authority, output, error);
    if (argc >= 2 && std::string_view{argv[1]} == "__monitoring-host-q0b-c0-characterize")
        return run_monitoring_host_q0b_command(argc, argv, production_authority, output, error);
    if (argc >= 2 && std::string_view{argv[1]} == "__monitoring-host-q1-page-fault")
        return run_monitoring_host_q1_command(argc, argv, production_authority, output, error);
    if (argc >= 2 && std::string_view{argv[1]} == "__monitoring-host-q2-context-switch")
        return run_monitoring_host_q2_command(argc, argv, production_authority, output, error);
    if (argc >= 2 && std::string_view{argv[1]} == "__monitoring-host-q3-temperature")
        return run_monitoring_host_q3_command(argc, argv, production_authority, output, error);
    if (argc >= 2 && std::string_view{argv[1]} == "__monitoring-host-q4-frequency")
        return run_monitoring_host_q4_command(argc, argv, production_authority, output, error);
    if (argc >= 2 && std::string_view{argv[1]} == "scenario") {
        return run_scenario_command(argc, argv, environment, output, error);
    }
    if (argc >= 2 && std::string_view{argv[1]} == "result") {
        return run_results_command(argc, argv, environment, output, error,
            &output==&std::cout&&platform_linux::presentation_files::stdout_is_terminal());
    }
    return topology::cli::run(argc, argv, environment, output, error);
}

} // namespace epycbench::cli
