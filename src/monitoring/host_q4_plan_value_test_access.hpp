#pragma once

#include "monitoring/host_q4_plan_value.hpp"

#include <string>

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ4PlanValueTestAccess final {
  public:
    enum class Fault : unsigned char {
        none,
        canonical_core,
        canonical_allocation,
        canonical_unknown,
        arithmetic_core
    };
    static void set_fault(Fault) noexcept;
    [[nodiscard]] static auto authored(std::string pointer)
        -> std::expected<MonitoringQ4PlanValue, MonitoringGovernedPlanDecodeFailure>;
};

} // namespace epycbench::monitoring::qualification::detail
