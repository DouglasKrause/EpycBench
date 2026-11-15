#pragma once

#include "runtime/pairwise_monitoring_host_capacity_admission.hpp"

namespace epycbench::runtime::detail {

struct PairwiseMonitoringHostCapacityAdmissionTestAccess final {
    using Operations =
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations;
    using ValidationPoint = PairwiseMonitoringHostCapacityAdmissionAccess::ValidationPoint;
    using ValidationOperations =
        PairwiseMonitoringHostCapacityAdmissionAccess::ValidationOperations;

    [[nodiscard]] static auto mint_single(const AdmittedPlacementRequest& input,
                                        Operations operations) noexcept {
        return PairwiseMonitoringHostCapacityAdmissionAccess::mint_single_with(input, operations);
    }

    [[nodiscard]] static auto mint(
        const AdmittedPairwiseProductionInputs& inputs, PairwiseVariant variant,
        Operations operations) noexcept {
        return PairwiseMonitoringHostCapacityAdmissionAccess::mint_with(
            inputs, variant, operations);
    }
    [[nodiscard]] static auto project(
        const AdmittedPairwiseProductionInputs& inputs,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority controller,
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority runner,
        const core::Sha256Digest& capacity_identity,
        const core::Sha256Digest& profile_identity,
        ValidationOperations operations) noexcept {
        return PairwiseMonitoringHostCapacityAdmissionAccess::project_with(
            inputs, std::move(controller), std::move(runner), capacity_identity,
            profile_identity, operations);
    }
};

} // namespace epycbench::runtime::detail
