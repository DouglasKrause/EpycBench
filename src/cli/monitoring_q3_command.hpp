#pragma once
#include <iosfwd>
namespace epycbench::runtime {
class ControllerProcessProductionAuthority;
}
namespace epycbench::cli {
[[nodiscard]] auto run_monitoring_host_q3_command(int, char*[],
                                                  runtime::ControllerProcessProductionAuthority&,
                                                  std::ostream&, std::ostream&) noexcept -> int;
}
