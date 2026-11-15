#pragma once

#include "epycbench/runtime/production_admission.hpp"

#include <expected>
#include <optional>

namespace epycbench::runtime::detail {

namespace test {
struct PairwiseV2AdmissionClassificationTestAccess;
}
struct PairwiseV2ContextualAdmissionAccess;

enum class PairwiseV2ContextualAdmissionBranch : unsigned char {
    placement_context_invariant,
    placement_topology_mismatch,
    placement_present_cpu_mismatch,
    placement_policy_mismatch,
    placement_identity_failure,
    placement_contained_exception,
    placement_contained_unknown_failure,
    pair_context_mismatch,
    pair_provenance_mismatch,
    pair_transport_mismatch,
    // A retained legacy-direct classification. Contextual admission cannot reach it:
    // each placement must first equal the same RunnerContext fingerprints, so their
    // topology requests are equal; distinct contexts fail pair_context_mismatch first.
    pair_topology_mismatch,
    pair_policy_mismatch,
    first_monitoring_policy_mismatch,
    second_monitoring_policy_mismatch,
    monitoring_policy_mismatch,
    service_placement_mismatch,
    service_placement_contained_exception,
    service_placement_contained_unknown_failure,
    statistics_policy_mismatch,
    statistics_contained_exception,
    statistics_contained_unknown_failure,
    allocation,
    production_contained_exception,
    production_contained_unknown_failure
};

class PairwiseV2LegacyClassifiedAdmissionFailure final {
  public:
    ~PairwiseV2LegacyClassifiedAdmissionFailure() noexcept;
    PairwiseV2LegacyClassifiedAdmissionFailure(
        const PairwiseV2LegacyClassifiedAdmissionFailure&) = delete;
    PairwiseV2LegacyClassifiedAdmissionFailure(
        PairwiseV2LegacyClassifiedAdmissionFailure&&) noexcept = default;
    auto operator=(const PairwiseV2LegacyClassifiedAdmissionFailure&)
        -> PairwiseV2LegacyClassifiedAdmissionFailure& = delete;
    auto operator=(PairwiseV2LegacyClassifiedAdmissionFailure&&)
        -> PairwiseV2LegacyClassifiedAdmissionFailure& = delete;

  private:
    PairwiseV2LegacyClassifiedAdmissionFailure(
        PairwiseV2ContextualAdmissionBranch branch, core::Error legacy) noexcept
        : branch_{branch}, legacy_{std::move(legacy)} {}

    PairwiseV2ContextualAdmissionBranch branch_{};
    core::Error legacy_;

    friend struct PairwiseV2AdmissionClassificationAccess;
    friend struct PairwiseV2ContextualAdmissionAccess;
    friend struct test::PairwiseV2AdmissionClassificationTestAccess;
    friend class ::epycbench::runtime::AdmittedPairwiseProductionRequest;
    friend class ::epycbench::runtime::AdmittedPairwiseProductionInputs;
    friend class ::epycbench::runtime::ServicePlacementPlan;
    friend class ::epycbench::runtime::StatisticsProductionPolicy;
    friend auto ::epycbench::runtime::admit_placement_request(
        ipc::RequestId, const ipc::PlacementRequest&, const RunnerContext&) noexcept
        -> std::expected<AdmittedPlacementRequest, core::Error>;
};

struct PairwiseV2AdmissionClassificationAccess final {
    [[nodiscard]] static auto admit_placement(
        ipc::RequestId, const ipc::PlacementRequest&, const RunnerContext&) noexcept
        -> std::expected<AdmittedPlacementRequest,
                         PairwiseV2LegacyClassifiedAdmissionFailure>;
    [[nodiscard]] static auto admit_placement_fallible(ipc::RequestId, const ipc::PlacementRequest&,
                                                       const RunnerContext&,
                                                       std::optional<core::Error>* = nullptr)
        -> std::expected<AdmittedPlacementRequest, PairwiseV2LegacyClassifiedAdmissionFailure>;
    [[nodiscard]] static auto admit_placement_source(ipc::RequestId, const ipc::PlacementRequest&,
                                                     const RunnerContext&,
                                                     std::optional<core::Error>* = nullptr)
        -> std::expected<AdmittedPlacementRequest, core::Error>;
    [[nodiscard]] static auto create_service_placement_fallible(const AdmittedPlacementRequest&,
                                                                bool)
        -> std::expected<ServicePlacementPlan, PairwiseV2LegacyClassifiedAdmissionFailure>;
    [[nodiscard]] static auto create_service_placement_source(const AdmittedPlacementRequest&, bool)
        -> std::expected<ServicePlacementPlan, core::Error>;
    [[nodiscard]] static auto admit_pair(
        AdmittedPlacementRequest, AdmittedPlacementRequest, std::uint64_t) noexcept
        -> std::expected<AdmittedPairwiseProductionRequest,
                         PairwiseV2LegacyClassifiedAdmissionFailure>;
    [[nodiscard]] static auto create_service_placement(
        const AdmittedPlacementRequest&, bool) noexcept
        -> std::expected<ServicePlacementPlan,
                         PairwiseV2LegacyClassifiedAdmissionFailure>;
    [[nodiscard]] static auto create_statistics_policy(
        const StatisticsComparisonPolicySource&,
        const ipc::PlacementMeasurementInputs&) noexcept
        -> std::expected<StatisticsProductionPolicy,
                         PairwiseV2LegacyClassifiedAdmissionFailure>;
    [[nodiscard]] static auto create_inputs(
        AdmittedPlacementRequest, AdmittedPlacementRequest, std::uint64_t,
        StatisticsComparisonPolicySource) noexcept
        -> std::expected<AdmittedPairwiseProductionInputs,
                         PairwiseV2LegacyClassifiedAdmissionFailure>;
};

} // namespace epycbench::runtime::detail
