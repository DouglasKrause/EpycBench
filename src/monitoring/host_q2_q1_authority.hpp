#pragma once
#include "monitoring/host_q1_plan_v2_authority_session.hpp"
#include <memory>
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2Q1Authority {
 public:
  MonitoringQ2Q1Authority()=default;
  MonitoringQ2Q1Authority(const MonitoringQ2Q1Authority&)=delete;
  virtual ~MonitoringQ2Q1Authority() noexcept=default;
  virtual auto bind(const RunnerContextAcquisitionSession&,const MonitoringHostQualificationPlan&,
    const runtime::AdmittedPlacementRequest&) noexcept
    -> std::expected<MonitoringQ1CollectorPlan,MonitoringQ1BoundedFailure> = 0;
  virtual auto retire() noexcept -> MonitoringQ1BoundedRetirement = 0;
  virtual void adopt(MonitoringQ1PlanV2AuthoritySession) noexcept=0;
};
[[nodiscard]] auto prepare_monitoring_q2_q1_authority() ->std::unique_ptr<MonitoringQ2Q1Authority>;
}
