#pragma once
#include "monitoring/host_q2_context_switch_evidence.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2EvidenceTestAccess final {
  public:
    static void provenance(MonitoringQ2CollectorPlan& plan,
                           MonitoringQ2CollectorPlan::EvidenceProvenance value) {
        plan.provenance_.emplace(std::move(value));
    }
    static auto collection(MonitoringQ2CollectorPlan plan,
                           std::vector<MonitoringQ2ContextSwitchTrial> trials,
                           std::vector<MonitoringQ2ContextSwitchSummary> summaries,
                           MonitoringQ2ContextSwitchSummary global) {
        return MonitoringQ2ContextSwitchCollection{std::move(plan), std::move(trials),
                                                    std::move(summaries), std::move(global)};
    }
    static void build_attestation(MonitoringQ2ContextSwitchCollection& collection,
                                  std::string value) {
        collection.plan_.running_build_attestation_ = std::move(value);
    }
    static auto take_plan(MonitoringQ2ContextSwitchCollection&& collection) {
        return std::move(collection.plan_);
    }
    static void plan_value(MonitoringQ2CollectorPlan& plan, MonitoringQ2PlanValue value) {
        plan.plan_ = value;
    }
};
} // namespace epycbench::monitoring::qualification::detail
