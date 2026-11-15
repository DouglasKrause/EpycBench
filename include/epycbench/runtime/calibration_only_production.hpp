#pragma once

#include "epycbench/measurement/calibration.hpp"
#include "epycbench/monitoring/acquisition_driver.hpp"
#include "epycbench/runtime/service_placement.hpp"

#include <expected>
#include <memory>
#include <vector>

namespace epycbench::runtime {

namespace detail { struct CalibrationOnlyProductionAdmissionAccess; struct HostCalibrationOnlyProductionAdmissionAccess; }

struct CalibrationOnlyOperationFailure {
    core::Error primary;
    std::vector<core::Error> secondary;
};

template <typename T>
using CalibrationOnlyOperationResult = std::expected<T, CalibrationOnlyOperationFailure>;

class CalibrationOnlyProductionAdmission final {
  public:
    CalibrationOnlyProductionAdmission(const CalibrationOnlyProductionAdmission&) = delete;
    auto operator=(const CalibrationOnlyProductionAdmission&)
        -> CalibrationOnlyProductionAdmission& = delete;
    CalibrationOnlyProductionAdmission(CalibrationOnlyProductionAdmission&&) noexcept;
    auto operator=(CalibrationOnlyProductionAdmission&&)
        -> CalibrationOnlyProductionAdmission& = delete;
    ~CalibrationOnlyProductionAdmission();

    [[nodiscard]] auto first_request() const noexcept -> const ipc::PlacementRequest&;
    [[nodiscard]] auto second_request() const noexcept -> const ipc::PlacementRequest&;
    [[nodiscard]] auto first() const noexcept -> const AdmittedPlacementRequest&;
    [[nodiscard]] auto second() const noexcept -> const AdmittedPlacementRequest&;
    [[nodiscard]] auto resolved_comparison_order_seed() const noexcept -> std::uint64_t;
    [[nodiscard]] auto monitoring_policy() const noexcept
        -> const monitoring::MonitoringExecutionPolicy&;
    [[nodiscard]] auto service_placement() const noexcept -> const ServicePlacementPlan&;
    [[nodiscard]] auto calibration_policy() const noexcept
        -> const measurement::CalibrationDurationPolicy&;
    [[nodiscard]] auto has_value() const noexcept -> bool;

  private:
    struct State;
    explicit CalibrationOnlyProductionAdmission(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend struct detail::CalibrationOnlyProductionAdmissionAccess;
    friend struct detail::HostCalibrationOnlyProductionAdmissionAccess;
};

class CalibrationOnlyProductionRun {
  public:
    class Implementation;
    explicit CalibrationOnlyProductionRun(std::unique_ptr<Implementation>) noexcept;
    CalibrationOnlyProductionRun(CalibrationOnlyProductionRun&&) noexcept;
    CalibrationOnlyProductionRun(const CalibrationOnlyProductionRun&) = delete;
    auto operator=(CalibrationOnlyProductionRun&&) -> CalibrationOnlyProductionRun& = delete;
    auto operator=(const CalibrationOnlyProductionRun&) -> CalibrationOnlyProductionRun& = delete;
    ~CalibrationOnlyProductionRun() noexcept;

    [[nodiscard]] auto calibrate_bandwidth(const measurement::CalibrationDurationPolicy&) noexcept
        -> CalibrationOnlyOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome>;
    [[nodiscard]] auto calibrate_latency(const measurement::CalibrationDurationPolicy&) noexcept
        -> CalibrationOnlyOperationResult<measurement::DependentLoadLatencyCalibrationOutcome>;
    [[nodiscard]] auto finalize() && noexcept -> CalibrationOnlyOperationResult<void>;

  private:
    std::unique_ptr<Implementation> implementation_;
};

class CalibrationOnlyProductionRun::Implementation {
  public:
    virtual ~Implementation() noexcept = default;
    virtual auto calibrate_bandwidth(const measurement::CalibrationDurationPolicy&) noexcept
        -> CalibrationOnlyOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome> = 0;
    virtual auto calibrate_latency(const measurement::CalibrationDurationPolicy&) noexcept
        -> CalibrationOnlyOperationResult<measurement::DependentLoadLatencyCalibrationOutcome> = 0;
    virtual auto finalize() noexcept -> CalibrationOnlyOperationResult<void> = 0;
};

class CalibrationOnlyProductionOperations final {
  public:
    using PrepareFunction = CalibrationOnlyOperationResult<CalibrationOnlyProductionRun> (*)(
        void*, const CalibrationOnlyProductionAdmission&) noexcept;

    [[nodiscard]] static auto create(std::shared_ptr<void> context,
                                     PrepareFunction prepare) noexcept
        -> std::expected<CalibrationOnlyProductionOperations, core::Error>;
    [[nodiscard]] auto prepare_run(const CalibrationOnlyProductionAdmission&) const noexcept
        -> CalibrationOnlyOperationResult<CalibrationOnlyProductionRun>;

  private:
    CalibrationOnlyProductionOperations(std::shared_ptr<void> context,
                                        PrepareFunction prepare) noexcept;
    std::shared_ptr<void> context_;
    PrepareFunction prepare_;
};

} // namespace epycbench::runtime
