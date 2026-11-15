#pragma once

#include "epycbench/monitoring/acquisition_driver.hpp"
#include "epycbench/runtime/service_placement.hpp"
#include "epycbench/runtime/statistics_production_policy.hpp"

#include <expected>

namespace epycbench::runtime {

namespace detail {
struct PairwiseMonitoringHostCapacityAdmissionAccess;
struct PairwiseV2ContextualAdmissionAccess;
struct PairwiseV2AdmissionClassificationAccess;
}

enum class PairwiseVariant { first, second };

class AdmittedPairwiseProductionInputs {
  public:
    [[nodiscard]] static auto create(AdmittedPlacementRequest first,
                                     AdmittedPlacementRequest second,
                                     std::uint64_t resolved_comparison_order_seed,
                                     StatisticsComparisonPolicySource statistics_source) noexcept
        -> std::expected<AdmittedPairwiseProductionInputs, core::Error>;
    [[nodiscard]] auto pairwise_request() const noexcept
        -> const AdmittedPairwiseProductionRequest& {
        return pair_;
    }
    [[nodiscard]] auto monitoring_policy() const noexcept
        -> const monitoring::MonitoringExecutionPolicy& {
        return policy_;
    }
    [[nodiscard]] auto service_placement() const noexcept -> const ServicePlacementPlan& {
        return service_placement_;
    }
    [[nodiscard]] auto statistics_source() const noexcept
        -> const StatisticsComparisonPolicySource& { return statistics_source_; }
    [[nodiscard]] auto statistics_policy() const noexcept
        -> const StatisticsProductionPolicy& {
        return statistics_policy_;
    }
    [[nodiscard]] auto measurement_monitor_required() const noexcept -> bool;

  private:
    AdmittedPairwiseProductionInputs(AdmittedPairwiseProductionRequest pair,
                                     monitoring::MonitoringExecutionPolicy policy,
                                     ServicePlacementPlan service_placement,
                                     StatisticsComparisonPolicySource statistics_source,
                                     StatisticsProductionPolicy statistics_policy)
        : pair_{std::move(pair)}, policy_{std::move(policy)},
          service_placement_{std::move(service_placement)},
          statistics_source_{std::move(statistics_source)},
          statistics_policy_{std::move(statistics_policy)} {}
    AdmittedPairwiseProductionRequest pair_;
    monitoring::MonitoringExecutionPolicy policy_;
    ServicePlacementPlan service_placement_;
    StatisticsComparisonPolicySource statistics_source_;
    StatisticsProductionPolicy statistics_policy_;
    friend struct detail::PairwiseMonitoringHostCapacityAdmissionAccess;
    friend struct detail::PairwiseV2ContextualAdmissionAccess;
    friend struct detail::PairwiseV2AdmissionClassificationAccess;
};

} // namespace epycbench::runtime
