#include "monitoring/host_q4_interval_summary_capacity.hpp"

#include "monitoring/host_q4_collection_capacity_inputs.hpp"

#include <array>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = MonitoringQ4CapacityReason;
using FailureKind = MonitoringQ4CapacityFailureKind;
constexpr std::uint64_t summary_fixed_byte_count = 49;

auto decimal_digits(std::uint64_t value) noexcept -> std::uint64_t {
    std::uint64_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

} // namespace

auto MonitoringQ4IntervalSummaryCapacity::make(
    const std::array<std::uint64_t, 4>& populations) noexcept
    -> std::expected<MonitoringQ4IntervalSummaryCapacity, MonitoringQ4CapacityFailure> {
    MonitoringQ4IntervalSummaryCapacity result;
    result.inventory_count_.value = populations[0];
    result.preparation_count_.value = populations[1];
    result.raw_read_count_.value = populations[2];
    result.typed_read_count_.value = populations[3];

    std::uint64_t intervals = 0;
    std::uint64_t numeric = 0;
    std::uint64_t nonzero = 0;
    for (const auto count : populations) {
        auto next = monitoring_q4_checked_add(intervals, count, Reason::interval_count_sum);
        if (!next) {
            return std::unexpected(next.error());
        }
        intervals = *next;
        if (count == 0) {
            next = monitoring_q4_checked_add(numeric, 4, Reason::interval_empty_numeric);
        } else {
            ++nonzero;
            next = monitoring_q4_checked_add(numeric, decimal_digits(count) + 60, Reason::interval_numeric_sum);
        }
        if (!next) {
            return std::unexpected(next.error());
        }
        numeric = *next;
    }
    const auto endpoints = monitoring_q4_checked_multiply(intervals, 40, Reason::interval_endpoints);
    const auto fixed = monitoring_q4_checked_multiply(populations.size(), summary_fixed_byte_count, Reason::interval_fixed);
    const auto slack = monitoring_q4_checked_multiply(nonzero, 57, Reason::interval_slack);
    if (!endpoints) return std::unexpected(endpoints.error());
    if (!fixed) return std::unexpected(fixed.error());
    if (!slack) return std::unexpected(slack.error());
    auto total = monitoring_q4_checked_add(*endpoints, *fixed, Reason::interval_fixed_sum);
    if (total) {
        total = monitoring_q4_checked_add(*total, numeric, Reason::interval_total_sum);
    }
    if (!total) {
        return std::unexpected(total.error());
    }
    result.interval_count_.value = intervals;
    result.endpoint_decimal_bytes_.value = *endpoints;
    result.nonzero_population_count_.value = nonzero;
    result.summary_fixed_bytes_.value = *fixed;
    result.summary_numeric_bytes_.value = numeric;
    result.total_interval_summary_bytes_.value = *total;
    result.maximum_summary_slack_bytes_.value = *slack;
    return result;
}

auto monitoring_q4_pass_interval_summary_capacity(const MonitoringQ4AdmittedCpuCount admitted)
    -> std::expected<MonitoringQ4IntervalSummaryCapacity, MonitoringQ4CapacityFailure> {
    if (admitted.value == 0) {
        return std::unexpected(MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::interval_pass_zero});
    }
    const auto preparations = monitoring_q4_checked_multiply(admitted.value, 3, Reason::interval_preparations);
    const auto raw = monitoring_q4_checked_multiply(admitted.value, 48, Reason::interval_reads);
    if (!preparations) return std::unexpected(preparations.error());
    if (!raw) return std::unexpected(raw.error());
    return MonitoringQ4IntervalSummaryCapacity::make({6, *preparations, *raw, *raw});
}

auto monitoring_q4_unsupported_interval_summary_capacity(
    const MonitoringQ4AdmittedCpuCount admitted)
    -> std::expected<MonitoringQ4IntervalSummaryCapacity, MonitoringQ4CapacityFailure> {
    if (admitted.value == 0) {
        return std::unexpected(MonitoringQ4CapacityFailure{FailureKind::invalid_input, Reason::interval_unsupported_zero});
    }
    return MonitoringQ4IntervalSummaryCapacity::make({1, admitted.value, 0, 0});
}

} // namespace epycbench::monitoring::qualification::detail
