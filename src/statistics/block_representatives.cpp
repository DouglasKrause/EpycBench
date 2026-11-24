#include "epycbench/statistics/block_representatives.hpp"
#include "statistics/midpoint.hpp"
#include "statistics/primary_metric_access.hpp"

#include <cmath>
#include <string>

namespace epycbench::statistics {
namespace {

auto error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::statistics,
            .code = core::StableErrorCode{"statistics.block_representative.numeric_failure"},
            .message = "Block representative calculation failed",
            .operation = "statistics.block_representative.calculate",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

template <PrimaryMetric Metric, typename MakeMetric>
auto calculate(std::array<Metric, 2> first, std::array<Metric, 2> second,
               MakeMetric make_metric) -> std::expected<BlockRepresentatives<Metric>, core::Error> {
    const double first_value = detail::difference_first_midpoint(
        detail::PrimaryMetricAccess::value(first[0]),
        detail::PrimaryMetricAccess::value(first[1]));
    const double second_value = detail::difference_first_midpoint(
        detail::PrimaryMetricAccess::value(second[0]),
        detail::PrimaryMetricAccess::value(second[1]));
    if (!std::isfinite(first_value) || first_value <= 0.0 ||
        !std::isfinite(second_value) || second_value <= 0.0) {
        return std::unexpected{error("invalid_midpoint")};
    }
    return BlockRepresentatives<Metric>{make_metric(first_value), make_metric(second_value)};
}

} // namespace

auto block_bandwidth_representatives(std::array<Bandwidth, 2> first,
                                     std::array<Bandwidth, 2> second)
    -> std::expected<BlockRepresentatives<Bandwidth>, core::Error> {
    return calculate(first, second, detail::PrimaryMetricAccess::bandwidth);
}

auto block_dependent_load_latency_representatives(
    std::array<DependentLoadLatency, 2> first,
    std::array<DependentLoadLatency, 2> second)
    -> std::expected<BlockRepresentatives<DependentLoadLatency>, core::Error> {
    return calculate(first, second, detail::PrimaryMetricAccess::latency);
}

auto block_task_throughput_representatives(std::array<TaskThroughput, 2> first,
                                           std::array<TaskThroughput, 2> second)
    -> std::expected<BlockRepresentatives<TaskThroughput>, core::Error> {
    return calculate(first, second, detail::PrimaryMetricAccess::task_throughput);
}

} // namespace epycbench::statistics
