#pragma once

#include "epycbench/measurement/calibration.hpp"
#include "epycbench/measurement/empty_harness.hpp"
#include "epycbench/measurement/warmup.hpp"
#include "runtime/pairwise_typed_operation_source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace epycbench::runtime::detail {

class PairwiseMeasurementLeafFacade;
struct PairwisePreparedMeasurementExecutionTestAccess;

struct PairwiseTypedBandwidthCalibrationOperations final {
    void* context{};
    PairwiseTypedOperationSourceResult<measurement::EmptyHarnessTrial>
        (*run_empty)(void*) noexcept{};
    PairwiseTypedOperationSourceResult<measurement::MeasurementInterval>
        (*run_candidate)(void*, kernel::SequentialReadBandwidthPassCount) noexcept{};
};

struct PairwiseTypedLatencyCalibrationOperations final {
    void* context{};
    PairwiseTypedOperationSourceResult<measurement::EmptyHarnessTrial>
        (*run_empty)(void*) noexcept{};
    PairwiseTypedOperationSourceResult<measurement::MeasurementInterval>
        (*run_candidate)(void*, kernel::DependentLoadLatencyStepCount) noexcept{};
};

struct PairwiseTypedBandwidthWarmupOperations final {
    void* context{};
    PairwiseTypedOperationSourceResult<measurement::MeasurementInterval>
        (*run)(void*, kernel::SequentialReadBandwidthPassCount) noexcept{};
};

struct PairwiseTypedLatencyWarmupOperations final {
    void* context{};
    PairwiseTypedOperationSourceResult<measurement::MeasurementInterval>
        (*run)(void*, kernel::DependentLoadLatencyStepCount) noexcept{};
};

enum class PairwisePreparedMeasurementComponentState : std::uint8_t {
    prepared,
    consuming,
    succeeded,
    failed,
};

class PairwisePreparedBandwidthMeasurementExecution final {
  public:
    PairwisePreparedBandwidthMeasurementExecution(
        PairwisePreparedBandwidthMeasurementExecution&&) noexcept;
    PairwisePreparedBandwidthMeasurementExecution(
        const PairwisePreparedBandwidthMeasurementExecution&) = delete;
    auto operator=(PairwisePreparedBandwidthMeasurementExecution&&)
        -> PairwisePreparedBandwidthMeasurementExecution& = delete;
    auto operator=(const PairwisePreparedBandwidthMeasurementExecution&)
        -> PairwisePreparedBandwidthMeasurementExecution& = delete;
    ~PairwisePreparedBandwidthMeasurementExecution() noexcept = default;

  private:
    PairwisePreparedBandwidthMeasurementExecution(
        measurement::CalibrationDurationPolicy,
        measurement::WarmupPolicy,
        std::vector<measurement::EmptyHarnessTrial>&&,
        std::vector<measurement::SequentialReadBandwidthCalibrationTrial>&&,
        std::vector<measurement::WarmupTrial>&&, std::vector<std::uint64_t>&&) noexcept;
    void require_calibration(const measurement::CalibrationDurationPolicy&) const noexcept;
    void require_warmup(const measurement::WarmupPolicy&) const noexcept;

    measurement::CalibrationDurationPolicy calibration_policy_;
    measurement::WarmupPolicy warmup_policy_;
    std::vector<measurement::EmptyHarnessTrial> empty_trials_;
    std::vector<measurement::SequentialReadBandwidthCalibrationTrial> candidate_trials_;
    std::vector<measurement::WarmupTrial> warmup_trials_;
    std::vector<std::uint64_t> warmup_scratch_;
    PairwisePreparedMeasurementComponentState calibration_state_{
        PairwisePreparedMeasurementComponentState::prepared};
    PairwisePreparedMeasurementComponentState warmup_state_{
        PairwisePreparedMeasurementComponentState::prepared};
    bool moved_from_{};
    friend class PairwiseMeasurementLeafFacade;
    friend struct PairwisePreparedMeasurementExecutionTestAccess;
};

class PairwisePreparedLatencyMeasurementExecution final {
  public:
    PairwisePreparedLatencyMeasurementExecution(
        PairwisePreparedLatencyMeasurementExecution&&) noexcept;
    PairwisePreparedLatencyMeasurementExecution(
        const PairwisePreparedLatencyMeasurementExecution&) = delete;
    auto operator=(PairwisePreparedLatencyMeasurementExecution&&)
        -> PairwisePreparedLatencyMeasurementExecution& = delete;
    auto operator=(const PairwisePreparedLatencyMeasurementExecution&)
        -> PairwisePreparedLatencyMeasurementExecution& = delete;
    ~PairwisePreparedLatencyMeasurementExecution() noexcept = default;

  private:
    PairwisePreparedLatencyMeasurementExecution(
        measurement::CalibrationDurationPolicy,
        measurement::WarmupPolicy,
        std::vector<measurement::EmptyHarnessTrial>&&,
        std::vector<measurement::DependentLoadLatencyCalibrationTrial>&&,
        std::vector<measurement::WarmupTrial>&&, std::vector<std::uint64_t>&&) noexcept;
    void require_calibration(const measurement::CalibrationDurationPolicy&) const noexcept;
    void require_warmup(const measurement::WarmupPolicy&) const noexcept;

    measurement::CalibrationDurationPolicy calibration_policy_;
    measurement::WarmupPolicy warmup_policy_;
    std::vector<measurement::EmptyHarnessTrial> empty_trials_;
    std::vector<measurement::DependentLoadLatencyCalibrationTrial> candidate_trials_;
    std::vector<measurement::WarmupTrial> warmup_trials_;
    std::vector<std::uint64_t> warmup_scratch_;
    PairwisePreparedMeasurementComponentState calibration_state_{
        PairwisePreparedMeasurementComponentState::prepared};
    PairwisePreparedMeasurementComponentState warmup_state_{
        PairwisePreparedMeasurementComponentState::prepared};
    bool moved_from_{};
    friend class PairwiseMeasurementLeafFacade;
    friend struct PairwisePreparedMeasurementExecutionTestAccess;
};

struct PairwisePreparedMeasurementExecutionTestAccess final {
    static void install_allocation_observer(
        void*, void (*)(void*, std::size_t)) noexcept;
    static void install_max_sizes(std::size_t, std::size_t, std::size_t,
                                  std::size_t) noexcept;
    static void install_representation_limits(std::uint64_t,
                                               std::uint64_t) noexcept;
    static void clear_hooks() noexcept;
    template <class Prepared>
    [[nodiscard]] static auto empty_data(const Prepared& value) noexcept {
        return value.empty_trials_.data();
    }
    template <class Prepared>
    [[nodiscard]] static auto candidate_data(const Prepared& value) noexcept {
        return value.candidate_trials_.data();
    }
    template <class Prepared>
    [[nodiscard]] static auto warmup_data(const Prepared& value) noexcept {
        return value.warmup_trials_.data();
    }
    template <class Prepared>
    [[nodiscard]] static auto scratch_data(const Prepared& value) noexcept {
        return value.warmup_scratch_.data();
    }
    template <class Prepared>
    [[nodiscard]] static auto capacities(const Prepared& value) noexcept {
        return std::array{value.empty_trials_.capacity(),
                          value.candidate_trials_.capacity(),
                          value.warmup_trials_.capacity(),
                          value.warmup_scratch_.capacity()};
    }
    template <class Prepared>
    [[nodiscard]] static auto sizes(const Prepared& value) noexcept {
        return std::array{value.empty_trials_.size(), value.candidate_trials_.size(),
                          value.warmup_trials_.size(), value.warmup_scratch_.size()};
    }
    template <class Prepared>
    [[nodiscard]] static auto states(const Prepared& value) noexcept {
        return std::array{value.calibration_state_, value.warmup_state_};
    }
};

} // namespace epycbench::runtime::detail
