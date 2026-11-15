#pragma once

#include "epycbench/monitoring/monitor_source.hpp"
#include "epycbench/platform_linux/hwmon_temperature.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace epycbench::monitoring {

namespace detail {
class TemperatureSourceAccess;
class PairwisePreparedCaptureStorageAccess;
}

enum class TemperatureSensorKind { cpu_control };
enum class TemperatureCaptureCompleteness { complete, incomplete };

class TemperatureMilliCelsius {
  public:
    [[nodiscard]] static constexpr auto from_millicelsius(std::int64_t value) noexcept
        -> TemperatureMilliCelsius { return TemperatureMilliCelsius{value}; }
    [[nodiscard]] constexpr auto value() const noexcept -> std::int64_t { return value_; }
    constexpr auto operator<=>(const TemperatureMilliCelsius&) const noexcept = default;
  private:
    constexpr explicit TemperatureMilliCelsius(std::int64_t value) noexcept : value_{value} {}
    std::int64_t value_;
};

class TemperatureRangeMilliCelsius {
  public:
    [[nodiscard]] static constexpr auto from_millicelsius(std::uint64_t value) noexcept
        -> TemperatureRangeMilliCelsius { return TemperatureRangeMilliCelsius{value}; }
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }
    constexpr auto operator<=>(const TemperatureRangeMilliCelsius&) const noexcept = default;
  private:
    constexpr explicit TemperatureRangeMilliCelsius(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_;
};

class TemperatureSnapshot {
  public:
    [[nodiscard]] static constexpr auto create(TemperatureSensorKind kind,
                                               TemperatureMilliCelsius temperature) noexcept
        -> TemperatureSnapshot { return TemperatureSnapshot{kind, temperature}; }
    [[nodiscard]] constexpr auto sensor_kind() const noexcept -> TemperatureSensorKind { return kind_; }
    [[nodiscard]] constexpr auto temperature() const noexcept -> TemperatureMilliCelsius { return temperature_; }
    constexpr auto operator==(const TemperatureSnapshot&) const noexcept -> bool = default;
  private:
    constexpr TemperatureSnapshot(TemperatureSensorKind kind, TemperatureMilliCelsius temperature) noexcept
        : kind_{kind}, temperature_{temperature} {}
    TemperatureSensorKind kind_;
    TemperatureMilliCelsius temperature_;
};

class TemperatureSource {
  public:
    [[nodiscard]] static auto prepare()
        -> std::expected<std::optional<TemperatureSource>, core::Error>;
    [[nodiscard]] auto operations() noexcept
        -> std::expected<MonitoringSourceOperations<TemperatureSnapshot>, core::Error>;
    [[nodiscard]] static auto implementation_id() -> MonitoringImplementationId;
    [[nodiscard]] constexpr auto sensor_kind() const noexcept -> TemperatureSensorKind { return TemperatureSensorKind::cpu_control; }
    TemperatureSource(TemperatureSource&&) noexcept;
    auto operator=(TemperatureSource&&) noexcept -> TemperatureSource&;
    TemperatureSource(const TemperatureSource&) = delete;
    auto operator=(const TemperatureSource&) -> TemperatureSource& = delete;
    ~TemperatureSource() noexcept;
  private:
    using ReadOperation = std::expected<std::int64_t, core::Error> (*)(void*) noexcept;
    TemperatureSource(void* context, ReadOperation read,
                      std::unique_ptr<platform_linux::HwmonTemperatureSource> source) noexcept;
    static auto support_callback(void*) noexcept -> MonitoringSourceSupport;
    static auto read_callback(void*) noexcept
        -> std::expected<MonitoringPayloadResult<TemperatureSnapshot>, core::Error>;
    void* context_{}; ReadOperation read_{};
    std::unique_ptr<platform_linux::HwmonTemperatureSource> source_;
    friend class detail::TemperatureSourceAccess;
};

class TemperatureObservationEvidence {
  public:
    [[nodiscard]] auto sensor_kind() const noexcept -> TemperatureSensorKind;
    [[nodiscard]] auto state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto observed_minimum() const noexcept -> const std::optional<TemperatureMilliCelsius>&;
    [[nodiscard]] auto observed_maximum() const noexcept -> const std::optional<TemperatureMilliCelsius>&;
    [[nodiscard]] auto observed_range() const noexcept -> const std::optional<TemperatureRangeMilliCelsius>&;
    [[nodiscard]] auto sample_count() const noexcept -> std::size_t;
    [[nodiscard]] auto sampling_interval() const noexcept -> core::Duration;
    [[nodiscard]] auto capture_completeness() const noexcept -> TemperatureCaptureCompleteness;
    auto operator==(const TemperatureObservationEvidence&) const noexcept -> bool = default;
  private:
    TemperatureObservationEvidence(MonitoringSignalState, std::optional<TemperatureMilliCelsius>,
      std::optional<TemperatureMilliCelsius>, std::optional<TemperatureRangeMilliCelsius>,
      std::size_t, core::Duration, TemperatureCaptureCompleteness) noexcept;
    MonitoringSignalState state_; std::optional<TemperatureMilliCelsius> minimum_, maximum_;
    std::optional<TemperatureRangeMilliCelsius> range_; std::size_t count_; core::Duration interval_;
    TemperatureCaptureCompleteness completeness_;
    friend auto classify_temperature(platform_linux::RawMonotonicTimestamp,
      platform_linux::RawMonotonicTimestamp, core::Duration, TemperatureCaptureCompleteness,
      std::span<const TimestampedMonitoringAcquisition<TemperatureSnapshot>>)
      -> std::expected<TemperatureObservationEvidence, core::Error>;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

[[nodiscard]] auto classify_temperature(platform_linux::RawMonotonicTimestamp observation_start,
  platform_linux::RawMonotonicTimestamp observation_end, core::Duration sampling_interval,
  TemperatureCaptureCompleteness completeness,
  std::span<const TimestampedMonitoringAcquisition<TemperatureSnapshot>> acquisitions)
  -> std::expected<TemperatureObservationEvidence, core::Error>;

} // namespace epycbench::monitoring
