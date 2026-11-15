#pragma once

#include "epycbench/core/id.hpp"
#include "epycbench/monitoring/monitor_source.hpp"
#include "epycbench/platform_linux/cpu_frequency.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace epycbench::monitoring {

namespace detail {
class FrequencySourceAccess;
class FrequencyClassifierAccess;
class PairwisePreparedCaptureStorageAccess;
}

enum class FrequencySourceKind { hardware_current, scaling_current };
enum class FrequencyCaptureCompleteness { complete, incomplete };

class FrequencyDomainId {
  public:
    [[nodiscard]] static auto create(std::vector<core::LinuxCpuId> cpus)
        -> std::expected<FrequencyDomainId, core::Error>;
    [[nodiscard]] auto cpus() const noexcept -> std::span<const core::LinuxCpuId>;
    auto operator<=>(const FrequencyDomainId&) const noexcept
        -> std::strong_ordering;
    auto operator==(const FrequencyDomainId&) const noexcept -> bool;

  private:
    explicit FrequencyDomainId(std::vector<core::LinuxCpuId> cpus) noexcept;
    FrequencyDomainId(
        std::shared_ptr<const std::vector<core::LinuxCpuId>> storage,
        std::size_t offset, std::size_t count) noexcept;
    std::vector<core::LinuxCpuId> cpus_;
    std::shared_ptr<const std::vector<core::LinuxCpuId>> shared_cpus_;
    std::size_t offset_{};
    std::size_t count_{};
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class FrequencyHz {
  public:
    [[nodiscard]] static constexpr auto from_hz(std::uint64_t value) noexcept -> FrequencyHz {
        return FrequencyHz{value};
    }
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }
    constexpr auto operator<=>(const FrequencyHz&) const noexcept = default;

  private:
    explicit constexpr FrequencyHz(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_;
};

class FrequencyRangeHz {
  public:
    [[nodiscard]] static constexpr auto from_hz(std::uint64_t value) noexcept -> FrequencyRangeHz {
        return FrequencyRangeHz{value};
    }
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }
    constexpr auto operator<=>(const FrequencyRangeHz&) const noexcept = default;

  private:
    explicit constexpr FrequencyRangeHz(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_;
};

class FrequencyVariationRatio {
  public:
    [[nodiscard]] static auto create(std::uint64_t numerator, std::uint64_t denominator)
        -> std::expected<FrequencyVariationRatio, core::Error>;
    [[nodiscard]] auto numerator() const noexcept -> std::uint64_t;
    [[nodiscard]] auto denominator() const noexcept -> std::uint64_t;
    auto operator==(const FrequencyVariationRatio&) const noexcept -> bool = default;

  private:
    FrequencyVariationRatio(std::uint64_t numerator, std::uint64_t denominator) noexcept;
    std::uint64_t numerator_;
    std::uint64_t denominator_;
};

class FrequencyDriverId {
  public:
    [[nodiscard]] static auto create(std::string value) -> FrequencyDriverId;
    [[nodiscard]] auto value() const noexcept -> std::string_view;
    auto operator==(const FrequencyDriverId&) const noexcept -> bool = default;
  private:
    explicit FrequencyDriverId(std::string value) noexcept;
    std::string value_;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class FrequencyGovernorId {
  public:
    [[nodiscard]] static auto create(std::string value) -> FrequencyGovernorId;
    [[nodiscard]] auto value() const noexcept -> std::string_view;
    auto operator==(const FrequencyGovernorId&) const noexcept -> bool = default;
  private:
    explicit FrequencyGovernorId(std::string value) noexcept;
    std::string value_;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class FrequencyPolicyMetadata {
  public:
    [[nodiscard]] static auto create(FrequencyDomainId domain_id, FrequencySourceKind kind,
                                     FrequencyDriverId driver, FrequencyGovernorId governor,
                                     FrequencyHz minimum, FrequencyHz maximum)
        -> FrequencyPolicyMetadata;
    [[nodiscard]] auto domain_id() const noexcept -> const FrequencyDomainId&;
    [[nodiscard]] auto source_kind() const noexcept -> FrequencySourceKind;
    [[nodiscard]] auto scaling_driver() const noexcept -> const FrequencyDriverId&;
    [[nodiscard]] auto scaling_governor() const noexcept -> const FrequencyGovernorId&;
    [[nodiscard]] auto scaling_minimum() const noexcept -> FrequencyHz;
    [[nodiscard]] auto scaling_maximum() const noexcept -> FrequencyHz;
    auto operator==(const FrequencyPolicyMetadata&) const noexcept -> bool = default;
  private:
    FrequencyPolicyMetadata(FrequencyDomainId, FrequencySourceKind, FrequencyDriverId,
                            FrequencyGovernorId, FrequencyHz, FrequencyHz) noexcept;
    FrequencyDomainId domain_id_;
    FrequencySourceKind kind_;
    FrequencyDriverId driver_;
    FrequencyGovernorId governor_;
    FrequencyHz minimum_;
    FrequencyHz maximum_;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class FrequencySnapshot {
  public:
    [[nodiscard]] static auto create(FrequencyDomainId domain_id, FrequencySourceKind kind,
                                     FrequencyHz frequency) -> FrequencySnapshot;
    [[nodiscard]] auto domain_id() const noexcept -> const FrequencyDomainId&;
    [[nodiscard]] auto source_kind() const noexcept -> FrequencySourceKind;
    [[nodiscard]] auto frequency() const noexcept -> FrequencyHz;
    auto operator==(const FrequencySnapshot&) const noexcept -> bool = default;
  private:
    FrequencySnapshot(FrequencyDomainId, FrequencySourceKind, FrequencyHz) noexcept;
    FrequencyDomainId domain_id_;
    FrequencySourceKind kind_;
    FrequencyHz frequency_;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class FrequencySource {
  public:
    [[nodiscard]] static auto prepare(core::LinuxCpuId cpu)
        -> std::expected<std::optional<FrequencySource>, core::Error>;
    [[nodiscard]] auto operations() noexcept
        -> std::expected<MonitoringSourceOperations<FrequencySnapshot>, core::Error>;
    [[nodiscard]] auto implementation_id() const -> MonitoringImplementationId;
    [[nodiscard]] auto metadata() const noexcept -> const FrequencyPolicyMetadata&;
    FrequencySource(FrequencySource&&) noexcept;
    auto operator=(FrequencySource&&) noexcept -> FrequencySource&;
    FrequencySource(const FrequencySource&) = delete;
    auto operator=(const FrequencySource&) -> FrequencySource& = delete;
    ~FrequencySource() noexcept;
  private:
    using ReadOperation = std::expected<std::uint64_t, core::Error> (*)(void*) noexcept;
    FrequencySource(FrequencyPolicyMetadata metadata, void* context, ReadOperation read,
                    std::unique_ptr<platform_linux::CpuFrequencyPolicySource> source) noexcept;
    static auto support_callback(void*) noexcept -> MonitoringSourceSupport;
    static auto read_callback(void*) noexcept
        -> std::expected<MonitoringPayloadResult<FrequencySnapshot>, core::Error>;
    FrequencyPolicyMetadata metadata_;
    void* context_{};
    ReadOperation read_{};
    std::unique_ptr<platform_linux::CpuFrequencyPolicySource> source_;
    friend class detail::FrequencySourceAccess;
};

using FrequencyAcquisition = TimestampedMonitoringAcquisition<FrequencySnapshot>;

struct FrequencyDomainAcquisitions {
    FrequencyDomainId domain_id;
    std::span<const FrequencyAcquisition> acquisitions;
};

class FrequencyDomainEvidence {
  public:
    [[nodiscard]] static auto create(FrequencyDomainId, FrequencySourceKind,
                                     MonitoringSignalState, std::optional<FrequencyHz>,
                                     std::optional<FrequencyHz>, std::optional<FrequencyRangeHz>,
                                     std::optional<FrequencyVariationRatio>, std::size_t)
        -> FrequencyDomainEvidence;
    [[nodiscard]] auto domain_id() const noexcept -> const FrequencyDomainId&;
    [[nodiscard]] auto source_kind() const noexcept -> FrequencySourceKind;
    [[nodiscard]] auto state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto observed_minimum() const noexcept -> const std::optional<FrequencyHz>&;
    [[nodiscard]] auto observed_maximum() const noexcept -> const std::optional<FrequencyHz>&;
    [[nodiscard]] auto observed_range() const noexcept -> const std::optional<FrequencyRangeHz>&;
    [[nodiscard]] auto observed_variation() const noexcept
        -> const std::optional<FrequencyVariationRatio>&;
    [[nodiscard]] auto sample_count() const noexcept -> std::size_t;
    auto operator==(const FrequencyDomainEvidence&) const noexcept -> bool = default;
  private:
    FrequencyDomainEvidence(FrequencyDomainId, FrequencySourceKind, MonitoringSignalState,
                            std::optional<FrequencyHz>, std::optional<FrequencyHz>,
                            std::optional<FrequencyRangeHz>,
                            std::optional<FrequencyVariationRatio>, std::size_t) noexcept;
    FrequencyDomainId domain_id_;
    FrequencySourceKind kind_;
    MonitoringSignalState state_;
    std::optional<FrequencyHz> minimum_, maximum_;
    std::optional<FrequencyRangeHz> range_;
    std::optional<FrequencyVariationRatio> variation_;
    std::size_t count_;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class FrequencyObservationEvidence {
  public:
    [[nodiscard]] auto state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto domains() const noexcept -> std::span<const FrequencyDomainEvidence>;
    [[nodiscard]] auto sampling_interval() const noexcept -> core::Duration;
    [[nodiscard]] auto capture_completeness() const noexcept -> FrequencyCaptureCompleteness;
    auto operator==(const FrequencyObservationEvidence&) const noexcept -> bool = default;
  private:
    FrequencyObservationEvidence(MonitoringSignalState, std::vector<FrequencyDomainEvidence>,
                                 core::Duration, FrequencyCaptureCompleteness) noexcept;
    MonitoringSignalState state_;
    std::vector<FrequencyDomainEvidence> domains_;
    core::Duration interval_;
    FrequencyCaptureCompleteness completeness_;
    friend auto classify_frequency(platform_linux::RawMonotonicTimestamp,
      platform_linux::RawMonotonicTimestamp, core::Duration, FrequencyCaptureCompleteness,
      std::span<const FrequencyDomainAcquisitions>)
      -> std::expected<FrequencyObservationEvidence, core::Error>;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

[[nodiscard]] auto classify_frequency(
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end, core::Duration sampling_interval,
    FrequencyCaptureCompleteness completeness,
    std::span<const FrequencyDomainAcquisitions> domains)
    -> std::expected<FrequencyObservationEvidence, core::Error>;

} // namespace epycbench::monitoring
