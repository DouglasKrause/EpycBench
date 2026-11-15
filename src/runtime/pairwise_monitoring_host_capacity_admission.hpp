#pragma once

#include "epycbench/runtime/production_admission.hpp"
#include "platform_linux/authenticated_monitoring_capacity_authority.hpp"

#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::runtime::detail {

struct PairwiseMonitoringHostCapacityAdmissionTestAccess;
struct PairwiseMonitoringHostCapacityStartupAccess;
class PairwiseTypedMonitoringCapacityBinding;
class PairwiseTypedLocalMonitoringCapacityProducer;

inline constexpr auto pairwise_monitoring_capacity_contract_identity =
    "epycbench.monitoring-evidence-capacity-control-tail/v1";
inline constexpr auto pairwise_finalized_error_profile_identity =
    "epycbench.pairwise-finalized-error-profile/215-failures-564-tuples/v1";

enum class PairwiseMonitoringHostCapacityAdmissionFailure : unsigned char {
    controller_unbound,
    runner_unbound,
    lower_authority_mismatch,
    provenance_mismatch,
    topology_mismatch,
    present_cpu_mismatch,
    worker_order_invalid,
    worker_membership_invalid,
    pairwise_policy_mismatch,
    capacity_contract_identity_mismatch,
    finalized_error_profile_identity_mismatch,
    invalid_input,
    allocation,
    contained_exception,
    contained_unknown_failure
};

class PairwiseMonitoringHostCapacityAdmission final {
  public:
    PairwiseMonitoringHostCapacityAdmission(const PairwiseMonitoringHostCapacityAdmission&) = delete;
    PairwiseMonitoringHostCapacityAdmission(PairwiseMonitoringHostCapacityAdmission&&) noexcept = default;
    auto operator=(const PairwiseMonitoringHostCapacityAdmission&)
        -> PairwiseMonitoringHostCapacityAdmission& = delete;
    auto operator=(PairwiseMonitoringHostCapacityAdmission&&)
        -> PairwiseMonitoringHostCapacityAdmission& = delete;

    [[nodiscard]] auto authority() const noexcept
        -> const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority& {
        return authority_;
    }
    [[nodiscard]] auto worker_count() const noexcept -> std::uint64_t { return workers_; }
    [[nodiscard]] auto maximum_blocks() const noexcept -> std::uint64_t { return blocks_; }
    [[nodiscard]] auto maximum_observation_nanoseconds() const noexcept -> std::uint64_t {
        return duration_;
    }
    [[nodiscard]] auto interval_nanoseconds() const noexcept -> std::optional<std::uint64_t> {
        return interval_;
    }
    [[nodiscard]] auto worker_cpus() const noexcept
        -> std::span<const core::LinuxCpuId> { return worker_cpus_; }
    [[nodiscard]] auto service_cpu() const noexcept
        -> std::optional<core::LinuxCpuId> { return service_cpu_; }
    [[nodiscard]] auto monitoring_policy() const noexcept
        -> const monitoring::MonitoringExecutionPolicy& { return policy_; }
    [[nodiscard]] auto capacity_contract_identity() const noexcept
        -> const core::Sha256Digest& { return capacity_identity_; }
    [[nodiscard]] auto finalized_error_profile_identity() const noexcept
        -> const core::Sha256Digest& { return profile_identity_; }

  private:
    PairwiseMonitoringHostCapacityAdmission(
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority authority,
        std::uint64_t workers, std::uint64_t blocks, std::uint64_t duration,
        std::optional<std::uint64_t> interval,
        std::vector<core::LinuxCpuId> worker_cpus,
        std::optional<core::LinuxCpuId> service_cpu,
        monitoring::MonitoringExecutionPolicy policy,
        core::Sha256Digest capacity, core::Sha256Digest profile) noexcept
        : authority_{std::move(authority)}, workers_{workers}, blocks_{blocks},
          duration_{duration}, interval_{interval}, worker_cpus_{std::move(worker_cpus)},
          service_cpu_{service_cpu}, policy_{std::move(policy)},
          capacity_identity_{capacity}, profile_identity_{profile} {}
    platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority authority_;
    std::uint64_t workers_{};
    std::uint64_t blocks_{};
    std::uint64_t duration_{};
    std::optional<std::uint64_t> interval_;
    std::vector<core::LinuxCpuId> worker_cpus_;
    std::optional<core::LinuxCpuId> service_cpu_;
    monitoring::MonitoringExecutionPolicy policy_;
    core::Sha256Digest capacity_identity_;
    core::Sha256Digest profile_identity_;
    friend struct PairwiseMonitoringHostCapacityAdmissionAccess;
};

struct PairwiseMonitoringHostCapacityAdmissionAccess final {
    // Additive host-qualification endpoint adapter. It cannot manufacture a
    // normal pair or accept raw capacity facts. Production minting is unselected.
    [[nodiscard]] static auto mint_single(const AdmittedPlacementRequest&) noexcept
        -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto project_single(
        const AdmittedPlacementRequest&, const monitoring::MonitoringExecutionPolicy&,
        const ServicePlacementPlan&,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority controller,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority runner,
        const core::Sha256Digest& capacity_identity,
        const core::Sha256Digest& profile_identity) noexcept
        -> std::expected<PairwiseMonitoringHostCapacityAdmission,
                         PairwiseMonitoringHostCapacityAdmissionFailure>;
    // Revalidate an already admitted single endpoint against its actual borrower;
    // no authority mint, kernel read or replacement producer is involved.
    [[nodiscard]] static auto matches_single(
        const PairwiseMonitoringHostCapacityAdmission&, const AdmittedPlacementRequest&,
        const monitoring::MonitoringExecutionPolicy&, const ServicePlacementPlan&) -> bool;
    [[nodiscard]] static auto mint(
        const AdmittedPairwiseProductionInputs&, PairwiseVariant) noexcept
        -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto project(
        const AdmittedPairwiseProductionInputs&,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority controller,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority runner,
        const core::Sha256Digest& capacity_identity,
        const core::Sha256Digest& profile_identity) noexcept
        -> std::expected<PairwiseMonitoringHostCapacityAdmission,
                         PairwiseMonitoringHostCapacityAdmissionFailure>;
    [[nodiscard]] static auto expected_capacity_identity() -> core::Sha256Digest;
    [[nodiscard]] static auto expected_profile_identity() -> core::Sha256Digest;
    [[nodiscard]] static auto provenance_matches(
        const PairwiseMonitoringHostCapacityAdmission&,
        const AdmittedPairwiseProductionInputs&) noexcept -> bool;
    [[nodiscard]] static auto provenance_for(const PlacementProvenance&)
        -> std::optional<platform_linux::detail::LinuxMonitoringCapacityProvenance>;

  private:
    [[nodiscard]] static auto mint_single_with(
        const AdmittedPlacementRequest&,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations) noexcept
        -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
    enum class ValidationPoint : unsigned char {
        worker_count,
        worker_pairing,
        worker_order,
        worker_membership,
        service_assignment,
        maximum_blocks,
        observation_duration,
        interval,
        normalized_policy
    };
    struct ValidationOperations final {
        void* context{};
        bool (*accept)(void*, ValidationPoint, bool) noexcept{};
    };
    [[nodiscard]] static auto mint_with(
        const AdmittedPairwiseProductionInputs&, PairwiseVariant,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations) noexcept
        -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto mint_request_with(
        const RunnerContext&, const ipc::PlacementRequest&,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations) noexcept
        -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto mint_request(
        const RunnerContext&, const ipc::PlacementRequest&) noexcept
        -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto binding_seal(
        const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority& authority)
        noexcept
        -> platform_linux::detail::LinuxMonitoringCapacityLocalBindingSeal;
    [[nodiscard]] static auto project_with(
        const AdmittedPairwiseProductionInputs&,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority controller,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority runner,
        const core::Sha256Digest& capacity_identity,
        const core::Sha256Digest& profile_identity, ValidationOperations) noexcept
        -> std::expected<PairwiseMonitoringHostCapacityAdmission,
                         PairwiseMonitoringHostCapacityAdmissionFailure>;
    [[nodiscard]] static auto binding_matches(
        const platform_linux::detail::LinuxMonitoringCapacityLocalBindingSeal&,
        const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority&) noexcept
        -> bool;
    [[nodiscard]] static auto statistics_match(
        const AdmittedPairwiseProductionInputs&,
        const StatisticsComparisonPolicySource&) noexcept -> bool;
    [[nodiscard]] static auto project_after_lower_validation(
        const AdmittedPairwiseProductionInputs&,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority authority,
        const core::Sha256Digest& capacity_identity,
        const core::Sha256Digest& profile_identity, ValidationOperations) noexcept
        -> std::expected<PairwiseMonitoringHostCapacityAdmission,
                         PairwiseMonitoringHostCapacityAdmissionFailure>;
    friend struct PairwiseMonitoringHostCapacityAdmissionTestAccess;
    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend class PairwiseTypedMonitoringCapacityBinding;
    friend class PairwiseTypedLocalMonitoringCapacityProducer;
};

} // namespace epycbench::runtime::detail
