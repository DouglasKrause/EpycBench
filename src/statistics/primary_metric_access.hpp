#pragma once

#include "epycbench/statistics/primary_metric.hpp"

namespace epycbench::statistics::detail {

struct PrimaryMetricAccess {
    static constexpr auto value(Bandwidth metric) noexcept -> double {
        return metric.bytes_per_second();
    }
    static constexpr auto value(DependentLoadLatency metric) noexcept -> double {
        return metric.seconds_per_completed_load();
    }
    static constexpr auto bandwidth(double value) noexcept -> Bandwidth { return Bandwidth{value}; }
    static constexpr auto latency(double value) noexcept -> DependentLoadLatency {
        return DependentLoadLatency{value};
    }
};

} // namespace epycbench::statistics::detail
