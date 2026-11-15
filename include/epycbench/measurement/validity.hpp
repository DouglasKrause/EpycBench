#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/measurement/calibration.hpp"
#include "epycbench/measurement/observation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace epycbench::measurement {

namespace detail {
struct MeasurementLeafAccess;
}

class MaximumHarnessOverhead {
  public:
    MaximumHarnessOverhead(const MaximumHarnessOverhead&) noexcept = default;
    MaximumHarnessOverhead(MaximumHarnessOverhead&&) noexcept = default;
    auto operator=(const MaximumHarnessOverhead&) -> MaximumHarnessOverhead& = delete;
    auto operator=(MaximumHarnessOverhead&&) -> MaximumHarnessOverhead& = delete;
    ~MaximumHarnessOverhead() noexcept = default;

    [[nodiscard]] static auto create(std::uint64_t numerator, std::uint64_t denominator)
        -> std::expected<MaximumHarnessOverhead, core::Error>;
    [[nodiscard]] auto numerator() const noexcept -> std::uint64_t;
    [[nodiscard]] auto denominator() const noexcept -> std::uint64_t;

  private:
    friend struct detail::MeasurementLeafAccess;
    MaximumHarnessOverhead(std::uint64_t numerator, std::uint64_t denominator) noexcept;
    const std::uint64_t numerator_;
    const std::uint64_t denominator_;
};

class ObservationValidityPolicy {
  public:
    ObservationValidityPolicy(const ObservationValidityPolicy&) noexcept = default;
    ObservationValidityPolicy(ObservationValidityPolicy&&) noexcept = default;
    auto operator=(const ObservationValidityPolicy&) -> ObservationValidityPolicy& = delete;
    auto operator=(ObservationValidityPolicy&&) -> ObservationValidityPolicy& = delete;
    ~ObservationValidityPolicy() noexcept = default;

    [[nodiscard]] static auto create(MaximumHarnessOverhead maximum_harness_overhead) noexcept
        -> ObservationValidityPolicy;
    [[nodiscard]] auto maximum_harness_overhead() const noexcept -> MaximumHarnessOverhead;

  private:
    explicit ObservationValidityPolicy(MaximumHarnessOverhead maximum_harness_overhead) noexcept;
    const MaximumHarnessOverhead maximum_harness_overhead_;
};

class TimerOverheadFraction {
  public:
    TimerOverheadFraction(const TimerOverheadFraction&) noexcept = default;
    TimerOverheadFraction(TimerOverheadFraction&&) noexcept = default;
    auto operator=(const TimerOverheadFraction&) -> TimerOverheadFraction& = delete;
    auto operator=(TimerOverheadFraction&&) -> TimerOverheadFraction& = delete;
    ~TimerOverheadFraction() noexcept = default;

    [[nodiscard]] static auto create(std::uint64_t numerator_nanoseconds,
                                     std::uint64_t denominator_nanoseconds) noexcept
        -> TimerOverheadFraction;
    [[nodiscard]] auto numerator_nanoseconds() const noexcept -> std::uint64_t;
    [[nodiscard]] auto denominator_nanoseconds() const noexcept -> std::uint64_t;

  private:
    TimerOverheadFraction(std::uint64_t numerator_nanoseconds,
                          std::uint64_t denominator_nanoseconds) noexcept;
    const std::uint64_t numerator_nanoseconds_;
    const std::uint64_t denominator_nanoseconds_;
};

class TimerOverheadValidityEvidence {
  public:
    TimerOverheadValidityEvidence(const TimerOverheadValidityEvidence&) noexcept = default;
    TimerOverheadValidityEvidence(TimerOverheadValidityEvidence&&) noexcept = default;
    auto operator=(const TimerOverheadValidityEvidence&) -> TimerOverheadValidityEvidence& = delete;
    auto operator=(TimerOverheadValidityEvidence&&) -> TimerOverheadValidityEvidence& = delete;
    ~TimerOverheadValidityEvidence() noexcept = default;

    [[nodiscard]] static auto create(TimerOverheadEstimate estimate,
                                     MaximumHarnessOverhead threshold,
                                     std::optional<TimerOverheadFraction> fraction) noexcept
        -> TimerOverheadValidityEvidence;
    [[nodiscard]] auto estimate() const noexcept -> TimerOverheadEstimate;
    [[nodiscard]] auto threshold() const noexcept -> MaximumHarnessOverhead;
    [[nodiscard]] auto fraction() const noexcept -> std::optional<TimerOverheadFraction>;

  private:
    TimerOverheadValidityEvidence(TimerOverheadEstimate estimate, MaximumHarnessOverhead threshold,
                                  std::optional<TimerOverheadFraction> fraction) noexcept;
    const TimerOverheadEstimate estimate_;
    const MaximumHarnessOverhead threshold_;
    const std::optional<TimerOverheadFraction> fraction_;
};

enum class ObservationValidityDisposition { valid, invalid };

enum class ObservationValidityCheckOutcome {
    pass,
    fail,
    warning,
    unavailable,
    not_applicable,
};

enum class ObservationValidityCheckId {
    kernel_correctness,
    worker_interval_containment,
    timer_overhead,
    placement,
    migration,
    major_page_faults,
    throttling,
    context_switching,
    frequency_variation,
    thermal_variation,
};

class ObservationValidityCheck {
  public:
    [[nodiscard]] static auto create(ObservationValidityCheckId id,
                                     ObservationValidityCheckOutcome outcome) noexcept
        -> ObservationValidityCheck;
    [[nodiscard]] auto id() const noexcept -> ObservationValidityCheckId;
    [[nodiscard]] auto outcome() const noexcept -> ObservationValidityCheckOutcome;

  private:
    ObservationValidityCheck(ObservationValidityCheckId id,
                             ObservationValidityCheckOutcome outcome) noexcept;
    const ObservationValidityCheckId id_;
    const ObservationValidityCheckOutcome outcome_;
};

inline constexpr std::size_t observation_validity_check_count = 10U;
using ObservationValidityChecks =
    std::array<ObservationValidityCheck, observation_validity_check_count>;

enum class WorkerPlacementValidityState {
    qualified,
    not_qualified,
    unavailable,
    not_applicable,
};

class WorkerPlacementValidityEvidence {
  public:
    [[nodiscard]] static auto create(core::WorkerId worker_id,
                                     WorkerPlacementValidityState state) noexcept
        -> WorkerPlacementValidityEvidence;
    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto state() const noexcept -> WorkerPlacementValidityState;

  private:
    WorkerPlacementValidityEvidence(core::WorkerId worker_id,
                                    WorkerPlacementValidityState state) noexcept;
    const core::WorkerId worker_id_;
    const WorkerPlacementValidityState state_;
};

enum class WorkerMigrationValidityState {
    not_migrated,
    migrated,
    unavailable,
    not_applicable,
};

class WorkerMigrationValidityEvidence {
  public:
    [[nodiscard]] static auto create(core::WorkerId worker_id,
                                     WorkerMigrationValidityState state) noexcept
        -> WorkerMigrationValidityEvidence;
    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto state() const noexcept -> WorkerMigrationValidityState;

  private:
    WorkerMigrationValidityEvidence(core::WorkerId worker_id,
                                    WorkerMigrationValidityState state) noexcept;
    const core::WorkerId worker_id_;
    const WorkerMigrationValidityState state_;
};

enum class ObservationMonitoringSignalState {
    clear,
    detected,
    unavailable,
    not_applicable,
};

class ObservationMonitoringEvidence {
  public:
    [[nodiscard]] static auto create(ObservationMonitoringSignalState major_page_faults,
                                     ObservationMonitoringSignalState throttling,
                                     ObservationMonitoringSignalState context_switching,
                                     ObservationMonitoringSignalState frequency_variation,
                                     ObservationMonitoringSignalState thermal_variation) noexcept
        -> ObservationMonitoringEvidence;
    [[nodiscard]] auto major_page_faults() const noexcept -> ObservationMonitoringSignalState;
    [[nodiscard]] auto throttling() const noexcept -> ObservationMonitoringSignalState;
    [[nodiscard]] auto context_switching() const noexcept -> ObservationMonitoringSignalState;
    [[nodiscard]] auto frequency_variation() const noexcept -> ObservationMonitoringSignalState;
    [[nodiscard]] auto thermal_variation() const noexcept -> ObservationMonitoringSignalState;

  private:
    ObservationMonitoringEvidence(ObservationMonitoringSignalState major_page_faults,
                                  ObservationMonitoringSignalState throttling,
                                  ObservationMonitoringSignalState context_switching,
                                  ObservationMonitoringSignalState frequency_variation,
                                  ObservationMonitoringSignalState thermal_variation) noexcept;
    const ObservationMonitoringSignalState major_page_faults_;
    const ObservationMonitoringSignalState throttling_;
    const ObservationMonitoringSignalState context_switching_;
    const ObservationMonitoringSignalState frequency_variation_;
    const ObservationMonitoringSignalState thermal_variation_;
};

class ObservationValidityEvidence {
  public:
    ObservationValidityEvidence(const ObservationValidityEvidence&) noexcept = default;
    ObservationValidityEvidence(ObservationValidityEvidence&&) noexcept = default;
    auto operator=(const ObservationValidityEvidence&) -> ObservationValidityEvidence& = delete;
    auto operator=(ObservationValidityEvidence&&) -> ObservationValidityEvidence& = delete;
    ~ObservationValidityEvidence() noexcept = default;

    [[nodiscard]] static auto
    create(core::ObservationId observation_id, ObservationValidityDisposition disposition,
           ObservationValidityChecks checks, ObservationValidityPolicy policy,
           TimerOverheadValidityEvidence timer_overhead) noexcept -> ObservationValidityEvidence;
    [[nodiscard]] auto observation_id() const noexcept -> core::ObservationId;
    [[nodiscard]] auto disposition() const noexcept -> ObservationValidityDisposition;
    [[nodiscard]] auto checks() const noexcept -> std::span<const ObservationValidityCheck>;
    [[nodiscard]] auto policy() const noexcept -> const ObservationValidityPolicy&;
    [[nodiscard]] auto timer_overhead() const noexcept -> const TimerOverheadValidityEvidence&;

  private:
    ObservationValidityEvidence(core::ObservationId observation_id,
                                ObservationValidityDisposition disposition,
                                ObservationValidityChecks checks, ObservationValidityPolicy policy,
                                TimerOverheadValidityEvidence timer_overhead) noexcept;
    const core::ObservationId observation_id_;
    const ObservationValidityDisposition disposition_;
    const ObservationValidityChecks checks_;
    const ObservationValidityPolicy policy_;
    const TimerOverheadValidityEvidence timer_overhead_;
};

[[nodiscard]] auto evaluate_sequential_read_bandwidth_validity(
    const SequentialReadBandwidthObservation& observation, ObservationValidityPolicy policy,
    TimerOverheadEstimate estimate, std::span<const WorkerPlacementValidityEvidence> placement,
    std::span<const WorkerMigrationValidityEvidence> migration,
    ObservationMonitoringEvidence monitoring)
    -> std::expected<ObservationValidityEvidence, core::Error>;

[[nodiscard]] auto evaluate_dependent_load_latency_validity(
    const DependentLoadLatencyObservation& observation, ObservationValidityPolicy policy,
    TimerOverheadEstimate estimate, std::span<const WorkerPlacementValidityEvidence> placement,
    std::span<const WorkerMigrationValidityEvidence> migration,
    ObservationMonitoringEvidence monitoring)
    -> std::expected<ObservationValidityEvidence, core::Error>;

} // namespace epycbench::measurement
