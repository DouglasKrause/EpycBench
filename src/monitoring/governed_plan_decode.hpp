#pragma once
#include "epycbench/core/error.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
namespace epycbench::monitoring::qualification::detail{
struct MonitoringGovernedPlanAuthoredDiagnostic final{std::string code,json_pointer;std::optional<std::uint64_t> byte_offset,line,column;};
using MonitoringGovernedPlanDecodeFailure=std::variant<core::Error,MonitoringGovernedPlanAuthoredDiagnostic>;
}
