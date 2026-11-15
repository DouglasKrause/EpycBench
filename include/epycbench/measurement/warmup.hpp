#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/kernel/dependent_load_latency.hpp"
#include "epycbench/kernel/sequential_read_bandwidth.hpp"
#include "epycbench/measurement/observation.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace epycbench::measurement {

namespace detail {
struct MeasurementLeafAccess;
}

class WarmupStabilityThreshold {
  public:
    WarmupStabilityThreshold(const WarmupStabilityThreshold&) noexcept = default;
    WarmupStabilityThreshold(WarmupStabilityThreshold&&) noexcept = default;
    auto operator=(const WarmupStabilityThreshold&) -> WarmupStabilityThreshold& = delete;
    auto operator=(WarmupStabilityThreshold&&) -> WarmupStabilityThreshold& = delete;
    ~WarmupStabilityThreshold() noexcept = default;

    [[nodiscard]] static auto create(std::uint64_t numerator, std::uint64_t denominator)
        -> std::expected<WarmupStabilityThreshold, core::Error>;

    [[nodiscard]] auto numerator() const noexcept -> std::uint64_t;
    [[nodiscard]] auto denominator() const noexcept -> std::uint64_t;

  private:
    friend struct detail::MeasurementLeafAccess;
    WarmupStabilityThreshold(std::uint64_t numerator, std::uint64_t denominator) noexcept;
    const std::uint64_t numerator_;
    const std::uint64_t denominator_;
};

class WarmupPolicy {
  public:
    WarmupPolicy(const WarmupPolicy&) noexcept = default;
    WarmupPolicy(WarmupPolicy&&) noexcept = default;
    auto operator=(const WarmupPolicy&) -> WarmupPolicy& = delete;
    auto operator=(WarmupPolicy&&) -> WarmupPolicy& = delete;
    ~WarmupPolicy() noexcept = default;

    [[nodiscard]] static auto create(std::uint64_t minimum_trials, std::uint64_t maximum_trials,
                                     std::uint64_t stability_window,
                                     WarmupStabilityThreshold stability_threshold)
        -> std::expected<WarmupPolicy, core::Error>;

    [[nodiscard]] auto minimum_trials() const noexcept -> std::uint64_t;
    [[nodiscard]] auto maximum_trials() const noexcept -> std::uint64_t;
    [[nodiscard]] auto stability_window() const noexcept -> std::uint64_t;
    [[nodiscard]] auto stability_threshold() const noexcept -> WarmupStabilityThreshold;

  private:
    friend struct detail::MeasurementLeafAccess;
    WarmupPolicy(std::uint64_t minimum_trials, std::uint64_t maximum_trials,
                 std::uint64_t stability_window,
                 WarmupStabilityThreshold stability_threshold) noexcept;
    const std::uint64_t minimum_trials_;
    const std::uint64_t maximum_trials_;
    const std::uint64_t stability_window_;
    const WarmupStabilityThreshold stability_threshold_;
};

class WarmupTrial {
  public:
    WarmupTrial(const WarmupTrial&) noexcept = default;
    WarmupTrial(WarmupTrial&&) noexcept = default;
    auto operator=(const WarmupTrial&) -> WarmupTrial& = delete;
    auto operator=(WarmupTrial&&) -> WarmupTrial& = delete;
    ~WarmupTrial() noexcept = default;

    [[nodiscard]] static auto create(MeasurementInterval interval) noexcept -> WarmupTrial;
    [[nodiscard]] auto interval() const noexcept -> const MeasurementInterval&;

  private:
    explicit WarmupTrial(MeasurementInterval interval) noexcept;
    const MeasurementInterval interval_;
};

class WarmupStabilityEvaluation {
  public:
    WarmupStabilityEvaluation(const WarmupStabilityEvaluation&) noexcept = default;
    WarmupStabilityEvaluation(WarmupStabilityEvaluation&&) noexcept = default;
    auto operator=(const WarmupStabilityEvaluation&) -> WarmupStabilityEvaluation& = delete;
    auto operator=(WarmupStabilityEvaluation&&) -> WarmupStabilityEvaluation& = delete;
    ~WarmupStabilityEvaluation() noexcept = default;

    [[nodiscard]] static auto create(core::Duration median_duration,
                                     core::Duration maximum_absolute_deviation,
                                     bool stable) noexcept -> WarmupStabilityEvaluation;
    [[nodiscard]] auto median_duration() const noexcept -> core::Duration;
    [[nodiscard]] auto maximum_absolute_deviation() const noexcept -> core::Duration;
    [[nodiscard]] auto stable() const noexcept -> bool;

  private:
    WarmupStabilityEvaluation(core::Duration median_duration,
                              core::Duration maximum_absolute_deviation, bool stable) noexcept;
    const core::Duration median_duration_;
    const core::Duration maximum_absolute_deviation_;
    const bool stable_;
};

enum class WarmupDisposition {
    skipped,
    stabilized,
    maximum_trials_reached_without_stability,
};

namespace detail {
class WarmupAccess;
}

class SequentialReadBandwidthWarmupOutcome {
  public:
    SequentialReadBandwidthWarmupOutcome(const SequentialReadBandwidthWarmupOutcome&) = default;
    SequentialReadBandwidthWarmupOutcome(SequentialReadBandwidthWarmupOutcome&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthWarmupOutcome&)
        -> SequentialReadBandwidthWarmupOutcome& = delete;
    auto operator=(SequentialReadBandwidthWarmupOutcome&&)
        -> SequentialReadBandwidthWarmupOutcome& = delete;
    ~SequentialReadBandwidthWarmupOutcome() noexcept = default;

    [[nodiscard]] auto policy() const noexcept -> const WarmupPolicy&;
    [[nodiscard]] auto selected_work() const noexcept -> kernel::SequentialReadBandwidthPassCount;
    [[nodiscard]] auto trials() const noexcept -> std::span<const WarmupTrial>;
    [[nodiscard]] auto disposition() const noexcept -> WarmupDisposition;
    [[nodiscard]] auto final_stability_evaluation() const noexcept
        -> std::optional<WarmupStabilityEvaluation>;

  private:
    SequentialReadBandwidthWarmupOutcome(
        WarmupPolicy policy, kernel::SequentialReadBandwidthPassCount selected_work,
        std::vector<WarmupTrial> trials, WarmupDisposition disposition,
        std::optional<WarmupStabilityEvaluation> final_evaluation) noexcept;

    const WarmupPolicy policy_;
    const kernel::SequentialReadBandwidthPassCount selected_work_;
    std::vector<WarmupTrial> trials_;
    const WarmupDisposition disposition_;
    const std::optional<WarmupStabilityEvaluation> final_evaluation_;
    friend class detail::WarmupAccess;
};

class DependentLoadLatencyWarmupOutcome {
  public:
    DependentLoadLatencyWarmupOutcome(const DependentLoadLatencyWarmupOutcome&) = default;
    DependentLoadLatencyWarmupOutcome(DependentLoadLatencyWarmupOutcome&&) noexcept = default;
    auto operator=(const DependentLoadLatencyWarmupOutcome&)
        -> DependentLoadLatencyWarmupOutcome& = delete;
    auto operator=(DependentLoadLatencyWarmupOutcome&&)
        -> DependentLoadLatencyWarmupOutcome& = delete;
    ~DependentLoadLatencyWarmupOutcome() noexcept = default;

    [[nodiscard]] auto policy() const noexcept -> const WarmupPolicy&;
    [[nodiscard]] auto selected_work() const noexcept -> kernel::DependentLoadLatencyStepCount;
    [[nodiscard]] auto trials() const noexcept -> std::span<const WarmupTrial>;
    [[nodiscard]] auto disposition() const noexcept -> WarmupDisposition;
    [[nodiscard]] auto final_stability_evaluation() const noexcept
        -> std::optional<WarmupStabilityEvaluation>;

  private:
    DependentLoadLatencyWarmupOutcome(
        WarmupPolicy policy, kernel::DependentLoadLatencyStepCount selected_work,
        std::vector<WarmupTrial> trials, WarmupDisposition disposition,
        std::optional<WarmupStabilityEvaluation> final_evaluation) noexcept;

    const WarmupPolicy policy_;
    const kernel::DependentLoadLatencyStepCount selected_work_;
    std::vector<WarmupTrial> trials_;
    const WarmupDisposition disposition_;
    const std::optional<WarmupStabilityEvaluation> final_evaluation_;
    friend class detail::WarmupAccess;
};

using WarmupOutcome =
    std::variant<SequentialReadBandwidthWarmupOutcome, DependentLoadLatencyWarmupOutcome>;

namespace detail {

class WarmupAccess {
  public:
    [[nodiscard]] static auto
    make_outcome(WarmupPolicy policy, kernel::SequentialReadBandwidthPassCount selected_work,
                 std::vector<WarmupTrial> trials, WarmupDisposition disposition,
                 std::optional<WarmupStabilityEvaluation> final_evaluation) noexcept
        -> SequentialReadBandwidthWarmupOutcome;
    [[nodiscard]] static auto
    make_outcome(WarmupPolicy policy, kernel::DependentLoadLatencyStepCount selected_work,
                 std::vector<WarmupTrial> trials, WarmupDisposition disposition,
                 std::optional<WarmupStabilityEvaluation> final_evaluation) noexcept
        -> DependentLoadLatencyWarmupOutcome;
};

struct SequentialReadBandwidthWarmupOperations {
    void* context;
    std::expected<MeasurementInterval, core::Error> (*run)(
        void*, kernel::SequentialReadBandwidthPassCount) noexcept;
};

struct DependentLoadLatencyWarmupOperations {
    void* context;
    std::expected<MeasurementInterval, core::Error> (*run)(
        void*, kernel::DependentLoadLatencyStepCount) noexcept;
};

} // namespace detail

[[nodiscard]] auto run_sequential_read_bandwidth_warmup(
    WarmupPolicy policy, kernel::SequentialReadBandwidthPassCount selected_work,
    const detail::SequentialReadBandwidthWarmupOperations& operations)
    -> std::expected<SequentialReadBandwidthWarmupOutcome, core::Error>;

[[nodiscard]] auto
run_dependent_load_latency_warmup(WarmupPolicy policy,
                                  kernel::DependentLoadLatencyStepCount selected_work,
                                  const detail::DependentLoadLatencyWarmupOperations& operations)
    -> std::expected<DependentLoadLatencyWarmupOutcome, core::Error>;

} // namespace epycbench::measurement
