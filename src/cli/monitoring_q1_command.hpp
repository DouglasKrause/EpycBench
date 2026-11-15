#pragma once
#include "monitoring/host_q1_backend.hpp"
#include <iosfwd>
namespace epycbench::runtime { class ControllerProcessProductionAuthority; }
namespace epycbench::cli {
[[nodiscard]] auto run_monitoring_host_q1_command(
    int, char*[], runtime::ControllerProcessProductionAuthority&,
    std::ostream&, std::ostream&) noexcept -> int;
}
