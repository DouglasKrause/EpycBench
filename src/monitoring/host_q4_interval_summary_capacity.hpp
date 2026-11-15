#pragma once

#include "monitoring/host_q4_structural_capacity.hpp"

#include <array>
#include <cstdint>
#include <expected>

namespace epycbench::monitoring::qualification::detail {

// This value owns only the decimal spellings of both endpoints of every duration interval and the
// complete internal JSON syntax (49 fixed bytes plus four numeric spellings) of each of the four
// summary objects. The later evidence composer owns the durations property/object punctuation,
// population property labels/separators, and all interval-record fixed syntax and other fields. It
// must not charge the 196 summary fixed bytes again.
class MonitoringQ4IntervalSummaryCapacity final {
  public:
    MonitoringQ4IntervalSummaryCapacity(const MonitoringQ4IntervalSummaryCapacity&) = default;
    MonitoringQ4IntervalSummaryCapacity(MonitoringQ4IntervalSummaryCapacity&&) noexcept = default;
    auto operator=(const MonitoringQ4IntervalSummaryCapacity&)
        -> MonitoringQ4IntervalSummaryCapacity& = delete;
    auto operator=(MonitoringQ4IntervalSummaryCapacity&&)
        -> MonitoringQ4IntervalSummaryCapacity& = delete;

    [[nodiscard]] constexpr auto inventory_count() const noexcept { return inventory_count_; }
    [[nodiscard]] constexpr auto preparation_count() const noexcept { return preparation_count_; }
    [[nodiscard]] constexpr auto raw_read_count() const noexcept { return raw_read_count_; }
    [[nodiscard]] constexpr auto typed_read_count() const noexcept { return typed_read_count_; }
    [[nodiscard]] constexpr auto interval_count() const noexcept { return interval_count_; }
    [[nodiscard]] constexpr auto endpoint_decimal_bytes() const noexcept {
        return endpoint_decimal_bytes_;
    }
    [[nodiscard]] constexpr auto nonzero_population_count() const noexcept {
        return nonzero_population_count_;
    }
    [[nodiscard]] constexpr auto summary_fixed_bytes() const noexcept {
        return summary_fixed_bytes_;
    }
    [[nodiscard]] constexpr auto summary_numeric_bytes() const noexcept {
        return summary_numeric_bytes_;
    }
    [[nodiscard]] constexpr auto total_interval_summary_bytes() const noexcept {
        return total_interval_summary_bytes_;
    }
    [[nodiscard]] constexpr auto maximum_summary_slack_bytes() const noexcept {
        return maximum_summary_slack_bytes_;
    }

  private:
    friend auto monitoring_q4_pass_interval_summary_capacity(MonitoringQ4AdmittedCpuCount)
        -> std::expected<MonitoringQ4IntervalSummaryCapacity, MonitoringQ4CapacityFailure>;
    friend auto monitoring_q4_unsupported_interval_summary_capacity(MonitoringQ4AdmittedCpuCount)
        -> std::expected<MonitoringQ4IntervalSummaryCapacity, MonitoringQ4CapacityFailure>;
    constexpr MonitoringQ4IntervalSummaryCapacity() = default;
    [[nodiscard]] static auto make(const std::array<std::uint64_t, 4>&) noexcept
        -> std::expected<MonitoringQ4IntervalSummaryCapacity, MonitoringQ4CapacityFailure>;

    MonitoringQ4CapacityCount inventory_count_{};
    MonitoringQ4CapacityCount preparation_count_{};
    MonitoringQ4CapacityCount raw_read_count_{};
    MonitoringQ4CapacityCount typed_read_count_{};
    MonitoringQ4CapacityCount interval_count_{};
    MonitoringQ4CapacityBytes endpoint_decimal_bytes_{};
    MonitoringQ4CapacityCount nonzero_population_count_{};
    MonitoringQ4CapacityBytes summary_fixed_bytes_{};
    MonitoringQ4CapacityBytes summary_numeric_bytes_{};
    MonitoringQ4CapacityBytes total_interval_summary_bytes_{};
    MonitoringQ4CapacityBytes maximum_summary_slack_bytes_{};
};

[[nodiscard]] auto monitoring_q4_pass_interval_summary_capacity(MonitoringQ4AdmittedCpuCount)
    -> std::expected<MonitoringQ4IntervalSummaryCapacity, MonitoringQ4CapacityFailure>;
[[nodiscard]] auto monitoring_q4_unsupported_interval_summary_capacity(MonitoringQ4AdmittedCpuCount)
    -> std::expected<MonitoringQ4IntervalSummaryCapacity, MonitoringQ4CapacityFailure>;

} // namespace epycbench::monitoring::qualification::detail
