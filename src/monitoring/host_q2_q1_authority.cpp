#include "monitoring/host_q2_q1_authority.hpp"
namespace epycbench::monitoring::qualification::detail { namespace {
class RealQ1Authority final:public MonitoringQ2Q1Authority {
 public:
  RealQ1Authority()=default;
  void adopt(MonitoringQ1PlanV2AuthoritySession value) noexcept override{value_.emplace(std::move(value));}
  auto bind(const RunnerContextAcquisitionSession& session,const MonitoringHostQualificationPlan& plan,
    const runtime::AdmittedPlacementRequest& admitted) noexcept
    ->std::expected<MonitoringQ1CollectorPlan,MonitoringQ1BoundedFailure> override {
    if(!value_)std::terminate();
    return value_->bind_fresh_plan_bounded(session,plan,admitted);
  }
  auto retire() noexcept->MonitoringQ1BoundedRetirement override{if(!value_)return {};return value_->retire_before_binding();}
 private: std::optional<MonitoringQ1PlanV2AuthoritySession> value_;
};}
auto prepare_monitoring_q2_q1_authority()->std::unique_ptr<MonitoringQ2Q1Authority>{return std::make_unique<RealQ1Authority>();}
}
