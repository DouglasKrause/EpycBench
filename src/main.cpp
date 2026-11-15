#include "epycbench/build_manifest.hpp"
#include "cli/application.hpp"
#include "cli/results_command.hpp"
#include "cli/monitoring_q0b_command.hpp"
#include "topology/topology_cli.hpp"
#include "runtime/controller_process_production_authority.hpp"
#include "runtime/pairwise_v2_production_entry.hpp"
#include "monitoring/host_frozen_source_launch.hpp"
#include "monitoring/host_q0b_governed_parent.hpp"

#include <iostream>
#include <string_view>

namespace epycbench::runtime::detail {
class ControllerProcessBootstrapAccess final {
  public:
    [[nodiscard]] static auto create() noexcept -> ControllerProcessProductionAuthority {
        return ControllerProcessProductionAuthority{};
    }
};
} // namespace epycbench::runtime::detail

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string_view{argv[1]} == "__runner") {
        if (argc != 2) {
            std::cerr << "epycbench: invalid internal runner invocation\n";
            return 2;
        }
        return epycbench::runtime::detail::run_stage_b_runner();
    }
    if (argc == 3 && std::string_view{argv[1]} == "version" &&
        std::string_view{argv[2]} == "--json") {
        std::cout << epycbench::build_manifest_json().dump() << '\n';
        return 0;
    }
    const bool source_invocation = epycbench::monitoring::qualification::detail::
        is_host_frozen_source_invocation(argc, argv);
    const bool baseline_invocation = epycbench::cli::is_monitoring_host_q0b_governed_command(argc, argv);
    const bool baseline_parent_invocation = epycbench::monitoring::qualification::detail::
        is_host_q0b_governed_parent(argc, argv);
    // The source fence starts before the sole authority bootstrap or any
    // source-specific fallible preparation. Collector invocation/phase origins
    // still come unchanged from its real parent, rather than this local sample.
    epycbench::runtime::RunnerCancellation source_cancellation;
    epycbench::monitoring::qualification::detail::HostFrozenSourceLaunchClock source_clock;
    source_clock.cancellation = &source_cancellation;
    std::uint64_t source_main_entry_ns{};
    if (source_invocation || baseline_invocation || baseline_parent_invocation) {
        const auto sampled = epycbench::platform_linux::host_work::source_launch_raw_now(
            source_clock.current, &source_main_entry_ns, &source_clock.actual);
        if (sampled.value != 0 || sampled.error_number) return 78;
    }
    // Source-private routing has its own original-main provider owner. Keep
    // the normal environment-before-bootstrap route and exactly this one sole
    // authority construction expression for every actual source process.
    auto environment = source_invocation || baseline_invocation || baseline_parent_invocation
        ? epycbench::topology::cli::Environment{}
        : epycbench::topology::cli::make_production_environment(
            argc > 1 && std::string_view{argv[1]} == "result"
                ? epycbench::cli::results_document_input_bytes : 64U * 1024U * 1024U);
    auto production_authority =
        epycbench::runtime::detail::ControllerProcessBootstrapAccess::create();
    if (source_invocation)
        return epycbench::monitoring::qualification::detail::run_host_frozen_source_invocation(
            argc, argv, production_authority, source_cancellation, source_clock, source_main_entry_ns);
    if (baseline_parent_invocation)
        return epycbench::monitoring::qualification::detail::run_host_q0b_governed_parent(
            argc, argv, production_authority, source_cancellation, source_clock, source_main_entry_ns);
    if (baseline_invocation)
        return epycbench::cli::run_monitoring_host_q0b_governed_command(argc, argv, production_authority,
            source_cancellation, source_clock, source_main_entry_ns, std::cout, std::cerr);
    return epycbench::cli::run(argc, argv, environment, production_authority, std::cout, std::cerr);
}
