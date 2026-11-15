#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/kernel/descriptor.hpp"
#include "epycbench/measurement/calibration.hpp"
#include "epycbench/monitoring/host_input_template.hpp"
#include "epycbench/runtime/runner_context.hpp"

#include <expected>
#include <functional>
#include <memory>

namespace epycbench::monitoring::qualification {

enum class HostRevalidationDisposition {
    matched,
    physical_fingerprint_mismatch,
    effective_fingerprint_mismatch,
    worker_cpu_unavailable,
    worker_node_unavailable,
    service_core_incomplete
};

class detail_HostQualificationPlanAccess;

class MonitoringHostPrerequisiteCapabilities {
  public:
    MonitoringHostPrerequisiteCapabilities(const MonitoringHostPrerequisiteCapabilities&) = delete;
    MonitoringHostPrerequisiteCapabilities(MonitoringHostPrerequisiteCapabilities&&) noexcept =
        default;
    auto operator=(const MonitoringHostPrerequisiteCapabilities&)
        -> MonitoringHostPrerequisiteCapabilities& = delete;
    auto operator=(MonitoringHostPrerequisiteCapabilities&&)
        -> MonitoringHostPrerequisiteCapabilities& = delete;
    ~MonitoringHostPrerequisiteCapabilities() noexcept = default;

    [[nodiscard]] auto linux() const noexcept -> bool { return true; }
    [[nodiscard]] auto bandwidth_descriptor() const noexcept -> const kernel::KernelDescriptor& {
        return bandwidth_;
    }
    [[nodiscard]] auto latency_descriptor() const noexcept -> const kernel::KernelDescriptor& {
        return latency_;
    }
    [[nodiscard]] auto calibration_policy() const noexcept
        -> const measurement::CalibrationDurationPolicy& {
        return calibration_;
    }

  private:
    MonitoringHostPrerequisiteCapabilities(kernel::KernelDescriptor bandwidth,
                                           kernel::KernelDescriptor latency,
                                           measurement::CalibrationDurationPolicy calibration)
        : bandwidth_{std::move(bandwidth)}, latency_{std::move(latency)},
          calibration_{std::move(calibration)} {}

    const kernel::KernelDescriptor bandwidth_;
    const kernel::KernelDescriptor latency_;
    const measurement::CalibrationDurationPolicy calibration_;
    friend class detail_RunnerContextAcquisitionFactory;
};

class RunnerContextAcquisitionSession {
  public:
    RunnerContextAcquisitionSession(const RunnerContextAcquisitionSession&) = delete;
    RunnerContextAcquisitionSession(RunnerContextAcquisitionSession&&) noexcept;
    auto operator=(const RunnerContextAcquisitionSession&)
        -> RunnerContextAcquisitionSession& = delete;
    auto operator=(RunnerContextAcquisitionSession&&) -> RunnerContextAcquisitionSession& = delete;
    ~RunnerContextAcquisitionSession() noexcept;

    [[nodiscard]] auto input_template() const noexcept
        -> std::expected<std::reference_wrapper<const MonitoringHostInputTemplateAuthority>,
                         core::Error>;
    [[nodiscard]] auto runner_context() const noexcept
        -> std::expected<std::reference_wrapper<const runtime::RunnerContext>, core::Error>;
    [[nodiscard]] auto prerequisites() const noexcept
        -> std::expected<std::reference_wrapper<const MonitoringHostPrerequisiteCapabilities>,
                         core::Error>;
    [[nodiscard]] auto revalidate() const noexcept
        -> std::expected<HostRevalidationDisposition, core::Error>;

  private:
    class Impl;
    explicit RunnerContextAcquisitionSession(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> implementation_;
    friend class detail_RunnerContextAcquisitionFactory;
    friend class detail_RunnerContextAcquisitionAccess;
    friend class detail_HostQualificationPlanAccess;
};

[[nodiscard]] auto
acquire_runner_context_session(MonitoringHostInputTemplateAuthority input_template) noexcept
    -> std::expected<RunnerContextAcquisitionSession, core::Error>;

} // namespace epycbench::monitoring::qualification
