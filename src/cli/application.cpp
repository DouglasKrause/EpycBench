#include "cli/application.hpp"

#include "cli/scenario_command.hpp"
#include "cli/results_command.hpp"
#include "cli/benchmark_run_command.hpp"
#include "cli/monitoring_q0b_command.hpp"
#include "cli/monitoring_q1_command.hpp"
#include "cli/monitoring_q2_command.hpp"
#include "cli/monitoring_q3_command.hpp"
#include "cli/monitoring_q4_command.hpp"

#include <string_view>

namespace epycbench::cli {

auto run(int argc, char* argv[], topology::cli::Environment& environment,
         runtime::ControllerProcessProductionAuthority& production_authority, std::ostream& output,
         std::ostream& error) -> int {
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
        return run_results_command(argc, argv, environment, output, error);
    }
    return topology::cli::run(argc, argv, environment, output, error);
}

} // namespace epycbench::cli
