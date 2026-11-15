#pragma once

#include "monitoring/host_q0b_backend_driver.hpp"
#include "monitoring/host_q0b_governed_work.hpp"
#include <iosfwd>

namespace epycbench::platform_linux::detail { class ControllerSignalScope; }
namespace epycbench::runtime { class RunnerCancellation; }

namespace epycbench::cli::detail {

using HostQ0bBackendDispatch = auto (*)(const std::filesystem::path&,std::string_view,
  runtime::ControllerProcessProductionAuthority&) noexcept
  ->std::expected<monitoring::qualification::MonitoringHostQ0bBackendResult,
                   monitoring::qualification::Q0bBackendFailure>;

class MonitoringQ0bCommandTestAccess final {
 public:
  static void set_host_dispatch(HostQ0bBackendDispatch) noexcept;
  static void reset() noexcept;
  // The actual CLI/driver/governed owner body with injected native leaves.
  // Shares production final clock/cleanup/rendering; consumes the caller's
  // descriptor and any supplied active signal scope, including body refusal.
  static auto run_governed(int, char*[], runtime::ControllerProcessProductionAuthority&,
      platform_linux::FileDescriptor&, monitoring::qualification::detail::HostQ0bBackendDependencies,
      monitoring::qualification::detail::HostQ0bGovernedLeaves,
      platform_linux::host_work::GuardOperations, std::uint64_t original_main_entry_ns,
      std::ostream&, std::ostream&,
      monitoring::qualification::detail::HostQ0bWorkSnapshot* = nullptr,
      platform_linux::detail::ControllerSignalScope* = nullptr, runtime::RunnerCancellation* = nullptr) noexcept -> int;
};
}
