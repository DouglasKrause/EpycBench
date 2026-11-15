#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"

#include <cassert>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::monitoring {

enum class MonitoringChannel {
    page_faults,
    context_switches,
    temperature,
    frequency,
    throttling,
};

enum class MonitoringSamplePhase {
    before_observation,
    during_observation,
    after_observation,
};

enum class MonitoringSourceSupport { supported, unsupported };

enum class MonitoringAcquisitionStatus { available, unavailable };

enum class MonitoringSignalState {
    clear,
    detected,
    unavailable,
    not_applicable,
};

class MonitoringImplementationId {
  public:
    MonitoringImplementationId(const MonitoringImplementationId&) = default;
    MonitoringImplementationId(MonitoringImplementationId&&) noexcept = default;
    auto operator=(const MonitoringImplementationId&) -> MonitoringImplementationId& = delete;
    auto operator=(MonitoringImplementationId&&) -> MonitoringImplementationId& = delete;
    ~MonitoringImplementationId() noexcept = default;

    // The channel contract supplies a nonempty stable semantic identifier constant.
    [[nodiscard]] static auto create(std::string identifier) -> MonitoringImplementationId;
    [[nodiscard]] auto value() const noexcept -> std::string_view;

    auto operator==(const MonitoringImplementationId&) const noexcept -> bool = default;

  private:
    explicit MonitoringImplementationId(std::string identifier) noexcept;

    std::string identifier_;
};

namespace detail {

[[nodiscard]] auto invalid_source_operations_error(std::string_view missing_operation)
    -> core::Error;
class PairwisePreparedCaptureStorageAccess;

} // namespace detail

class MonitoringClockOperations {
  public:
    using NowOperation =
        std::expected<platform_linux::RawMonotonicTimestamp, core::Error> (*)(void*) noexcept;

    [[nodiscard]] static auto create(void* context, NowOperation operation)
        -> std::expected<MonitoringClockOperations, core::Error>;

    [[nodiscard]] auto now() const noexcept
        -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
        return now_(context_);
    }

  private:
    MonitoringClockOperations(void* context, NowOperation operation) noexcept
        : context_{context}, now_{operation} {}

    void* context_;
    NowOperation now_;
};

template <class T> class MonitoringPayloadResult {
  public:
    MonitoringPayloadResult(const MonitoringPayloadResult&) = default;
    MonitoringPayloadResult(MonitoringPayloadResult&&) noexcept = default;
    auto operator=(const MonitoringPayloadResult&) -> MonitoringPayloadResult& = delete;
    auto operator=(MonitoringPayloadResult&&) -> MonitoringPayloadResult& = delete;
    ~MonitoringPayloadResult() noexcept = default;

    [[nodiscard]] static auto available(T payload) -> MonitoringPayloadResult {
        return MonitoringPayloadResult{MonitoringAcquisitionStatus::available,
                                       std::optional<T>{std::move(payload)}};
    }

    [[nodiscard]] static auto unavailable() -> MonitoringPayloadResult {
        return MonitoringPayloadResult{MonitoringAcquisitionStatus::unavailable, std::nullopt};
    }

    [[nodiscard]] auto status() const noexcept -> MonitoringAcquisitionStatus { return status_; }
    [[nodiscard]] auto payload() const noexcept -> const std::optional<T>& { return payload_; }

  private:
    MonitoringPayloadResult(MonitoringAcquisitionStatus status, std::optional<T> payload) noexcept
        : status_{status}, payload_{std::move(payload)} {}

    MonitoringAcquisitionStatus status_;
    std::optional<T> payload_;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

template <class T> class MonitoringSourceOperations {
  public:
    using SupportOperation = MonitoringSourceSupport (*)(void*) noexcept;
    using ReadOperation =
        std::expected<MonitoringPayloadResult<T>, core::Error> (*)(void*) noexcept;

    [[nodiscard]] static auto create(void* context, SupportOperation support_operation,
                                     ReadOperation read_operation)
        -> std::expected<MonitoringSourceOperations, core::Error> {
        if (support_operation == nullptr) {
            return std::unexpected{detail::invalid_source_operations_error("support")};
        }
        if (read_operation == nullptr) {
            return std::unexpected{detail::invalid_source_operations_error("read")};
        }
        return MonitoringSourceOperations{context, support_operation, read_operation};
    }

    [[nodiscard]] auto support() const noexcept -> MonitoringSourceSupport {
        return support_(context_);
    }

    [[nodiscard]] auto read() const noexcept
        -> std::expected<MonitoringPayloadResult<T>, core::Error> {
        return read_(context_);
    }

  private:
    MonitoringSourceOperations(void* context, SupportOperation support_operation,
                               ReadOperation read_operation) noexcept
        : context_{context}, support_{support_operation}, read_{read_operation} {}

    void* context_;
    SupportOperation support_;
    ReadOperation read_;
};

template <class T> class TimestampedMonitoringAcquisition {
  public:
    TimestampedMonitoringAcquisition(const TimestampedMonitoringAcquisition&) = default;
    TimestampedMonitoringAcquisition(TimestampedMonitoringAcquisition&&) noexcept = default;
    auto operator=(const TimestampedMonitoringAcquisition&)
        -> TimestampedMonitoringAcquisition& = delete;
    auto operator=(TimestampedMonitoringAcquisition&&)
        -> TimestampedMonitoringAcquisition& = delete;
    ~TimestampedMonitoringAcquisition() noexcept = default;

    [[nodiscard]] auto channel() const noexcept -> MonitoringChannel { return channel_; }
    [[nodiscard]] auto implementation_id() const noexcept -> const MonitoringImplementationId& {
        return implementation_id_;
    }
    [[nodiscard]] auto phase() const noexcept -> MonitoringSamplePhase { return phase_; }
    [[nodiscard]] auto collection_start() const noexcept -> platform_linux::RawMonotonicTimestamp {
        return collection_start_;
    }
    [[nodiscard]] auto collection_end() const noexcept -> platform_linux::RawMonotonicTimestamp {
        return collection_end_;
    }
    [[nodiscard]] auto collection_duration() const noexcept -> core::Duration {
        return collection_duration_;
    }
    [[nodiscard]] auto status() const noexcept -> MonitoringAcquisitionStatus {
        return payload_result_.status();
    }
    [[nodiscard]] auto payload() const noexcept -> const std::optional<T>& {
        return payload_result_.payload();
    }

  private:
    TimestampedMonitoringAcquisition(MonitoringChannel channel,
                                     MonitoringImplementationId implementation_id,
                                     MonitoringSamplePhase phase,
                                     platform_linux::RawMonotonicTimestamp collection_start,
                                     platform_linux::RawMonotonicTimestamp collection_end,
                                     core::Duration collection_duration,
                                     MonitoringPayloadResult<T> payload_result) noexcept
        : channel_{channel}, implementation_id_{std::move(implementation_id)}, phase_{phase},
          collection_start_{collection_start}, collection_end_{collection_end},
          collection_duration_{collection_duration}, payload_result_{std::move(payload_result)} {}

    MonitoringChannel channel_;
    MonitoringImplementationId implementation_id_;
    MonitoringSamplePhase phase_;
    platform_linux::RawMonotonicTimestamp collection_start_;
    platform_linux::RawMonotonicTimestamp collection_end_;
    core::Duration collection_duration_;
    MonitoringPayloadResult<T> payload_result_;

    template <class U>
    friend auto acquire_monitoring_sample(MonitoringChannel, MonitoringImplementationId,
                                          MonitoringSamplePhase, const MonitoringClockOperations&,
                                          const MonitoringSourceOperations<U>&)
        -> std::expected<TimestampedMonitoringAcquisition<U>, core::Error>;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

template <class T>
[[nodiscard]] auto
acquire_monitoring_sample(MonitoringChannel channel, MonitoringImplementationId implementation_id,
                          MonitoringSamplePhase phase, const MonitoringClockOperations& clock,
                          const MonitoringSourceOperations<T>& source)
    -> std::expected<TimestampedMonitoringAcquisition<T>, core::Error> {
    auto start = clock.now();
    if (!start) {
        return std::unexpected{std::move(start.error())};
    }

    auto payload_result = source.read();
    if (!payload_result) {
        return std::unexpected{std::move(payload_result.error())};
    }

    auto end = clock.now();
    if (!end) {
        return std::unexpected{std::move(end.error())};
    }

    assert(*start <= *end);
    const auto duration =
        core::Duration::from_base_units(end->nanoseconds() - start->nanoseconds());
    return TimestampedMonitoringAcquisition<T>{
        channel,  std::move(implementation_id), phase, *start, *end,
        duration, std::move(*payload_result)};
}

} // namespace epycbench::monitoring
