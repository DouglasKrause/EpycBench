#pragma once

#include "epycbench/monitoring/temperature.hpp"

#include <expected>
#include <span>

namespace epycbench::monitoring::detail {

enum class TemperatureClassifierFailure : unsigned char {
    ordering,
    interval,
    boundary_overlap,
    metadata,
    sensor,
    driver,
    sample_count,
    leading_gap,
    internal_gap,
    trailing_gap,
};

struct TemperatureClassification final {
    MonitoringSignalState state{};
    std::optional<TemperatureMilliCelsius> minimum;
    std::optional<TemperatureMilliCelsius> maximum;
    std::optional<TemperatureRangeMilliCelsius> range;
    std::size_t count{};
};

[[nodiscard]] auto classify_temperature_bounded(
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end,
    core::Duration sampling_interval,
    TemperatureCaptureCompleteness completeness,
    std::span<const TimestampedMonitoringAcquisition<TemperatureSnapshot>> acquisitions,
    std::span<const TimestampedMonitoringAcquisition<TemperatureSnapshot>*>
        available_scratch) noexcept
    -> std::expected<TemperatureClassification, TemperatureClassifierFailure>;

} // namespace epycbench::monitoring::detail
