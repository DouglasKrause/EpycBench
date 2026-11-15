#pragma once
#include "epycbench/monitoring/host_q0b_v2_projection.hpp"
#include <optional>
namespace epycbench::monitoring::qualification::detail {
struct HostQ0bProjectionAccess final {
    static auto facts(const RunnerContextAcquisitionSession&,
                      const MonitoringHostQualificationPlan&,
                      const runtime::CalibrationOnlyProductionAdmission&, HostQ0bFamily,
                      runtime::PairwiseVariant, std::optional<core::Error>* = nullptr)
        -> std::expected<MonitoringHostAdmittedProductionFacts, core::Error>;
    static auto base(const HostEnvironmentAuthority&, const MonitoringHostQualificationPlan&,
                     const MonitoringHostAdmittedProductionFacts&,
                     const MonitoringHostAdmittedProductionFacts&,
                     std::optional<core::Error>* = nullptr)
        -> std::expected<MonitoringHostQ0bBaseEnvironment, core::Error>;
};
} // namespace epycbench::monitoring::qualification::detail
