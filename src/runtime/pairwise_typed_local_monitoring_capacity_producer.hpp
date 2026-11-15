#pragma once

#include "runtime/pairwise_typed_monitoring_capacity_binding.hpp"
#include "platform_linux/authenticated_monitoring_capacity_authority.hpp"

#include <expected>
#include <optional>

namespace epycbench::runtime::detail {

struct PairwiseTypedLocalMonitoringCapacityFailure final {
    PairwiseEvidenceFailure primary;
    std::optional<PairwiseEvidenceFailure> cleanup;
};

struct PairwiseTypedLocalMonitoringCapacityProducerTestAccess;

class PairwiseTypedLocalMonitoringCapacityAcceptance final {
  public:
    PairwiseTypedLocalMonitoringCapacityAcceptance(
        const PairwiseTypedLocalMonitoringCapacityAcceptance&) = delete;
    PairwiseTypedLocalMonitoringCapacityAcceptance(
        PairwiseTypedLocalMonitoringCapacityAcceptance&& other) noexcept;
    auto operator=(const PairwiseTypedLocalMonitoringCapacityAcceptance&)
        -> PairwiseTypedLocalMonitoringCapacityAcceptance& = delete;
    auto operator=(PairwiseTypedLocalMonitoringCapacityAcceptance&&)
        -> PairwiseTypedLocalMonitoringCapacityAcceptance& = delete;
    ~PairwiseTypedLocalMonitoringCapacityAcceptance() noexcept;

  private:
    PairwiseTypedLocalMonitoringCapacityAcceptance(
        PairwiseMonitoringHostCapacityAdmission, ipc::RequestId, ipc::RequestId,
        AdmittedPlacementIdentity, AdmittedPlacementIdentity, void*,
        void (*)(void*) noexcept) noexcept;
    std::optional<PairwiseMonitoringHostCapacityAdmission> admission_;
    ipc::RequestId first_request_;
    ipc::RequestId second_request_;
    AdmittedPlacementIdentity first_identity_;
    AdmittedPlacementIdentity second_identity_;
    void* retirement_context_{};
    void (*retirement_observer_)(void*) noexcept{};
    bool active_{};
    void retire() noexcept;
    friend class PairwiseTypedLocalMonitoringCapacityProducer;
};

class PairwiseTypedLocalMonitoringCapacityProducer final {
  public:
    PairwiseTypedLocalMonitoringCapacityProducer() noexcept = default;
    PairwiseTypedLocalMonitoringCapacityProducer(
        const PairwiseTypedLocalMonitoringCapacityProducer&) = delete;
    PairwiseTypedLocalMonitoringCapacityProducer(
        PairwiseTypedLocalMonitoringCapacityProducer&&) noexcept = default;

    [[nodiscard]] auto prepare(const AdmittedPairwiseProductionInputs&) const noexcept
        -> std::expected<PairwiseTypedLocalMonitoringCapacityAcceptance,
                         PairwiseTypedLocalMonitoringCapacityFailure>;
    [[nodiscard]] static auto release(
        PairwiseTypedLocalMonitoringCapacityAcceptance&&) noexcept
        -> std::expected<PairwiseTypedMonitoringCapacityHandoff,
                         PairwiseTypedMonitoringCapacityBindingFailure>;

  private:
    struct Dependencies final {
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations first{};
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority::Operations second{};
        void* retirement_context{};
        void (*retirement_observer)(void*) noexcept{};
        bool deny_admission_allocation{};
    };
    explicit PairwiseTypedLocalMonitoringCapacityProducer(Dependencies dependencies) noexcept
        : dependencies_{dependencies} {}
    Dependencies dependencies_{};
    friend struct PairwiseTypedLocalMonitoringCapacityProducerTestAccess;
};

} // namespace epycbench::runtime::detail
