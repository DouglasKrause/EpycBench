#pragma once

#include "monitoring/host_q4_governed_plan_schema_binding.hpp"

#include <span>

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ4GovernedPlanSchemaBindingTestAccess final {
  public:
    enum class Fault : unsigned char { none, allocation, contained };
    static auto bind(const MonitoringQ4GovernedInputCandidate& candidate,
                     const std::shared_ptr<const void>& expected) {
        return MonitoringQ4GovernedPlanSchemaBinding::bind(candidate, expected);
    }
    static auto candidate_binding(const MonitoringQ4GovernedInputCandidate& candidate) {
        return MonitoringQ4GovernedPlanSchemaBinding::candidate_binding(candidate);
    }
    static auto bound_to(const MonitoringQ4GovernedPlanSchemaBinding& value,
                         const std::shared_ptr<const void>& expected) noexcept {
        return value.bound_to(expected);
    }
    static void set_fault(const Fault value) noexcept {
        MonitoringQ4GovernedPlanSchemaBinding::set_fault(static_cast<unsigned char>(value));
    }
    static void set_trace(const std::span<unsigned char> events, std::size_t& count,
                          unsigned int& input_mask) noexcept {
        MonitoringQ4GovernedPlanSchemaBinding::set_trace(
            {events.data(), events.size(), &count, &input_mask});
    }
    static void clear_trace() noexcept { MonitoringQ4GovernedPlanSchemaBinding::set_trace({}); }
};

} // namespace epycbench::monitoring::qualification::detail
