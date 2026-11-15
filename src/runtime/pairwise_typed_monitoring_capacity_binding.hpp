#pragma once

#include "runtime/pairwise_monitoring_capacity.hpp"

#include <expected>
#include <optional>

namespace epycbench::runtime::detail {

struct PairwiseV2RunnerSessionAccess;
class PairwiseTypedLocalMonitoringCapacityProducer;

enum class PairwiseTypedMonitoringCapacityBindingFailureKind : unsigned char {
    foreign_inputs,
    already_consumed,
    runtime_failure,
};

// Closed, move-owning failure sum. State/identity rejections remain categorical;
// a lower capacity projection failure retains its sole exact typed value.
class PairwiseTypedMonitoringCapacityBindingFailure final {
  public:
    PairwiseTypedMonitoringCapacityBindingFailure(
        const PairwiseTypedMonitoringCapacityBindingFailure&) = delete;
    auto operator=(const PairwiseTypedMonitoringCapacityBindingFailure&)
        -> PairwiseTypedMonitoringCapacityBindingFailure& = delete;
    PairwiseTypedMonitoringCapacityBindingFailure(
        PairwiseTypedMonitoringCapacityBindingFailure&& other) noexcept
        : kind_{other.kind_}, runtime_failure_{std::move(other.runtime_failure_)},
          active_{std::exchange(other.active_, false)} {
        other.runtime_failure_.reset();
    }
    auto operator=(PairwiseTypedMonitoringCapacityBindingFailure&&)
        -> PairwiseTypedMonitoringCapacityBindingFailure& = delete;

    [[nodiscard]] auto kind() const noexcept
        -> PairwiseTypedMonitoringCapacityBindingFailureKind {
        require_active();
        return kind_;
    }
    [[nodiscard]] auto runtime_failure() const noexcept
        -> const PairwiseRuntimeFailure* {
        require_active();
        return runtime_failure_ ? &*runtime_failure_ : nullptr;
    }
    [[nodiscard]] auto release_runtime_failure() && noexcept
        -> PairwiseRuntimeFailure {
        require_active();
        if (!runtime_failure_ ||
            kind_ != PairwiseTypedMonitoringCapacityBindingFailureKind::runtime_failure)
            std::terminate();
        active_ = false;
        return std::move(*runtime_failure_);
    }

  private:
    explicit PairwiseTypedMonitoringCapacityBindingFailure(
        PairwiseTypedMonitoringCapacityBindingFailureKind kind) noexcept
        : kind_{kind} {
        if (kind == PairwiseTypedMonitoringCapacityBindingFailureKind::runtime_failure)
            std::terminate();
    }
    explicit PairwiseTypedMonitoringCapacityBindingFailure(
        PairwiseRuntimeFailure failure) noexcept
        : kind_{PairwiseTypedMonitoringCapacityBindingFailureKind::runtime_failure},
          runtime_failure_{std::move(failure)} {}
    void require_active() const noexcept {
        if (!active_)
            std::terminate();
    }

    PairwiseTypedMonitoringCapacityBindingFailureKind kind_;
    std::optional<PairwiseRuntimeFailure> runtime_failure_;
    bool active_{true};
    friend class PairwiseTypedLocalMonitoringCapacityProducer;
    friend class PairwiseTypedMonitoringCapacityBinding;
    friend class PairwiseTypedMonitoringPrepareContext;
};
// Inactive, source-private one-shot bridge between the accepted startup
// authority and the future typed adapter prepare context. It deliberately has
// no selected producer until the atomic activation landing supplies one.
class PairwiseTypedMonitoringCapacityHandoff final {
  public:
    PairwiseTypedMonitoringCapacityHandoff(
        const PairwiseTypedMonitoringCapacityHandoff&) = delete;
    PairwiseTypedMonitoringCapacityHandoff(
        PairwiseTypedMonitoringCapacityHandoff&& other) noexcept
        : admission_{std::move(other.admission_)},
          first_request_{other.first_request_}, second_request_{other.second_request_},
          first_identity_{std::move(other.first_identity_)},
          second_identity_{std::move(other.second_identity_)},
          retirement_context_{other.retirement_context_},
          retirement_observer_{other.retirement_observer_},
          active_{std::exchange(other.active_, false)} {}
    auto operator=(const PairwiseTypedMonitoringCapacityHandoff&)
        -> PairwiseTypedMonitoringCapacityHandoff& = delete;
    auto operator=(PairwiseTypedMonitoringCapacityHandoff&&)
        -> PairwiseTypedMonitoringCapacityHandoff& = delete;
    ~PairwiseTypedMonitoringCapacityHandoff() noexcept { retire(); }

  private:
    explicit PairwiseTypedMonitoringCapacityHandoff(
        PairwiseMonitoringHostCapacityAdmission admission,
        ipc::RequestId first, ipc::RequestId second,
        AdmittedPlacementIdentity first_identity,
        AdmittedPlacementIdentity second_identity,
        void* retirement_context,
        void (*retirement_observer)(void*) noexcept) noexcept
        : admission_{std::move(admission)}, first_request_{first},
          second_request_{second}, first_identity_{std::move(first_identity)},
          second_identity_{std::move(second_identity)},
          retirement_context_{retirement_context},
          retirement_observer_{retirement_observer}, active_{true} {}
    std::optional<PairwiseMonitoringHostCapacityAdmission> admission_;
    ipc::RequestId first_request_;
    ipc::RequestId second_request_;
    AdmittedPlacementIdentity first_identity_;
    AdmittedPlacementIdentity second_identity_;
    void* retirement_context_{};
    void (*retirement_observer_)(void*) noexcept{};
    bool active_{};
    void retire() noexcept {
        if (!std::exchange(active_, false))
            return;
        admission_.reset();
        if (retirement_observer_)
            retirement_observer_(retirement_context_);
    }
    friend struct PairwiseV2RunnerSessionAccess;
    friend class PairwiseTypedLocalMonitoringCapacityProducer;
    friend class PairwiseTypedMonitoringCapacityBinding;
};

class PairwiseTypedMonitoringCapacityBinding final {
  public:
    PairwiseTypedMonitoringCapacityBinding(
        const PairwiseTypedMonitoringCapacityBinding&) = delete;
    auto operator=(const PairwiseTypedMonitoringCapacityBinding&)
        -> PairwiseTypedMonitoringCapacityBinding& = delete;
    PairwiseTypedMonitoringCapacityBinding(
        PairwiseTypedMonitoringCapacityBinding&&) noexcept;
    auto operator=(PairwiseTypedMonitoringCapacityBinding&&)
        -> PairwiseTypedMonitoringCapacityBinding& = delete;
    ~PairwiseTypedMonitoringCapacityBinding() noexcept;

    [[nodiscard]] static auto create(
        PairwiseTypedMonitoringCapacityHandoff,
        const AdmittedPairwiseProductionInputs&) noexcept
        -> std::expected<PairwiseTypedMonitoringCapacityBinding,
                         PairwiseTypedMonitoringCapacityBindingFailure>;

    [[nodiscard]] auto consume(
        const AdmittedPairwiseProductionInputs&) && noexcept
        -> std::expected<PairwiseAdmittedMonitoringCapacity,
                         PairwiseTypedMonitoringCapacityBindingFailure>;

  private:
    PairwiseTypedMonitoringCapacityBinding(
        PairwiseMonitoringHostCapacityAdmission,
        ipc::RequestId, ipc::RequestId,
        AdmittedPlacementIdentity, AdmittedPlacementIdentity,
        void*, void (*)(void*) noexcept) noexcept;

    std::optional<PairwiseMonitoringHostCapacityAdmission> admission_;
    ipc::RequestId first_request_;
    ipc::RequestId second_request_;
    AdmittedPlacementIdentity first_identity_;
    AdmittedPlacementIdentity second_identity_;
    bool consumed_{};
    bool retirement_active_{true};
    void* retirement_context_{};
    void (*retirement_observer_)(void*) noexcept{};
    void retire() noexcept;
};

// Exact future prepare-context shape. Installation is construction-only and
// consumption is one-shot; there is intentionally no setter or reset path.
class PairwiseTypedMonitoringPrepareContext final {
  public:
    explicit PairwiseTypedMonitoringPrepareContext(
        PairwiseTypedMonitoringCapacityBinding binding) noexcept
        : binding_{std::move(binding)} {}
    PairwiseTypedMonitoringPrepareContext(
        const PairwiseTypedMonitoringPrepareContext&) = delete;
    PairwiseTypedMonitoringPrepareContext(
        PairwiseTypedMonitoringPrepareContext&&) noexcept = default;

    [[nodiscard]] auto prepare(
        const AdmittedPairwiseProductionInputs&) noexcept
        -> std::expected<PairwiseAdmittedMonitoringCapacity,
                         PairwiseTypedMonitoringCapacityBindingFailure>;

  private:
    std::optional<PairwiseTypedMonitoringCapacityBinding> binding_;
    bool attempted_{};
};

} // namespace epycbench::runtime::detail
