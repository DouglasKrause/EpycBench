#pragma once
#include "monitoring/host_q2_backend.hpp"
#include <iosfwd>
namespace epycbench::runtime { class ControllerProcessProductionAuthority; }
namespace epycbench::cli {
[[nodiscard]] auto run_monitoring_host_q2_command(int, char*[],
    runtime::ControllerProcessProductionAuthority&, std::ostream&, std::ostream&) noexcept -> int;
}
