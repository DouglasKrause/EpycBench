#pragma once

#include "epycbench/monitoring/q0b_backend_driver.hpp"

#include <iosfwd>
#include <cstdint>

namespace epycbench::runtime {
class ControllerProcessProductionAuthority;
class RunnerCancellation;
}
namespace epycbench::monitoring::qualification::detail { struct HostFrozenSourceLaunchClock; }

namespace epycbench::cli {

[[nodiscard]] auto render_monitoring_q0b_result(
    std::expected<monitoring::qualification::Q0bBackendResult,
                  monitoring::qualification::Q0bBackendFailure> result,
    std::ostream& output, std::ostream& error) noexcept -> int;

[[nodiscard]] auto run_monitoring_q0b_command(
    int argc, char* argv[], runtime::ControllerProcessProductionAuthority& authority,
    std::ostream& output, std::ostream& error) noexcept -> int;
[[nodiscard]] auto run_monitoring_host_q0b_command(
    int argc, char* argv[], runtime::ControllerProcessProductionAuthority& authority,
    std::ostream& output, std::ostream& error) noexcept -> int;

// Fixed internal handoff from the sealed ordinary campaign invoker. Main
// recognizes this before creating the ordinary command environment.
[[nodiscard]] auto is_monitoring_host_q0b_governed_command(int, char*[]) noexcept -> bool;
[[nodiscard]] auto run_monitoring_host_q0b_governed_command(
    int argc, char* argv[], runtime::ControllerProcessProductionAuthority&,
    runtime::RunnerCancellation&, monitoring::qualification::detail::HostFrozenSourceLaunchClock&,
    std::uint64_t original_main_entry_ns, std::ostream&, std::ostream&) noexcept -> int;

} // namespace epycbench::cli
