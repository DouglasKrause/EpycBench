#pragma once

#include "runtime/pairwise_v2_contextual_admission.hpp"
#include "runtime/pairwise_v2_phase_a_protocol.hpp"
#include "runtime/pairwise_typed_monitoring_capacity_binding.hpp"

#include <expected>
#include <optional>
#include <variant>

namespace epycbench::runtime::detail {

enum class PairwiseV2RunnerSessionFailure : unsigned char {
    cancellation_observation,
    request_handoff,
    rejected_acknowledgement,
    startup_acknowledgement,
    accepted_event,
    cancelled_event,
    capacity_handoff
};

class PairwiseV2BufferedCancellation final {
  public:
    PairwiseV2BufferedCancellation(const PairwiseV2BufferedCancellation&) = delete;
    PairwiseV2BufferedCancellation(PairwiseV2BufferedCancellation&&) noexcept = default;
    auto operator=(const PairwiseV2BufferedCancellation&)
        -> PairwiseV2BufferedCancellation& = delete;
    auto operator=(PairwiseV2BufferedCancellation&&)
        -> PairwiseV2BufferedCancellation& = delete;

  private:
    PairwiseV2BufferedCancellation(
        PairwiseV2RequestHandoff request, PairwiseV2ControlReaderState control,
        std::optional<PairwiseV2CancellationReceipt> cancellation,
        PairwiseV2CancellationPhase phase) noexcept
        : request_{std::move(request)}, control_{std::move(control)},
          cancellation_{std::move(cancellation)}, phase_{phase} {}
    PairwiseV2RequestHandoff request_;
    PairwiseV2ControlReaderState control_;
    std::optional<PairwiseV2CancellationReceipt> cancellation_;
    PairwiseV2CancellationPhase phase_{};
    friend struct PairwiseV2RunnerSessionAccess;
};

struct PairwiseV2RunnerSessionOperations final {
    void* context{};
    void (*sidecar_retired)(void*) noexcept{};
};

class PairwiseV2PreparedRunnerHandoff final {
  public:
    PairwiseV2PreparedRunnerHandoff(const PairwiseV2PreparedRunnerHandoff&) = delete;
    PairwiseV2PreparedRunnerHandoff(PairwiseV2PreparedRunnerHandoff&& other) noexcept
        : identity_{other.identity_}, startup_{std::move(other.startup_)},
          events_{std::move(other.events_)}, control_{std::move(other.control_)},
          operations_{other.operations_},
          capacity_released_{std::exchange(other.capacity_released_, true)},
          retirement_active_{std::exchange(other.retirement_active_, false)} {}
    auto operator=(const PairwiseV2PreparedRunnerHandoff&)
        -> PairwiseV2PreparedRunnerHandoff& = delete;
    auto operator=(PairwiseV2PreparedRunnerHandoff&&)
        -> PairwiseV2PreparedRunnerHandoff& = delete;
    ~PairwiseV2PreparedRunnerHandoff() noexcept {
        if (retirement_active_ && operations_.sidecar_retired)
            operations_.sidecar_retired(operations_.context);
    }

    [[nodiscard]] auto identity() const noexcept -> const PairwiseV2PairIdentity& {
        return identity_;
    }

  private:
    PairwiseV2PreparedRunnerHandoff(
        PairwiseV2PairIdentity identity,
        AdmittedPairwiseHostCapacityStartup startup,
        PairwiseV2EventWriterState events,
        PairwiseV2ControlReaderState control,
        PairwiseV2RunnerSessionOperations operations) noexcept
        : identity_{identity}, startup_{std::move(startup)}, events_{std::move(events)},
          control_{std::move(control)}, operations_{operations},
          retirement_active_{true} {}

    PairwiseV2PairIdentity identity_;
    AdmittedPairwiseHostCapacityStartup startup_;
    PairwiseV2EventWriterState events_;
    PairwiseV2ControlReaderState control_;
    PairwiseV2RunnerSessionOperations operations_;
    bool capacity_released_{};
    bool retirement_active_{};
    friend struct PairwiseV2RunnerSessionAccess;
};

struct PairwiseV2RunnerCancelled final {
    PairwiseV2PairIdentity identity;
    PairwiseV2CancellationReason reason{};
};

struct PairwiseV2RunnerRejected final {
    PairwiseV2PairIdentity identity;
    PairwiseHostCapacityRejectionReason reason{};
};

using PairwiseV2RunnerSessionOutcome =
    std::variant<PairwiseV2PreparedRunnerHandoff,
                 PairwiseV2RunnerCancelled,
                 PairwiseV2RunnerRejected>;

struct PairwiseV2RunnerSessionAccess final {
    [[nodiscard]] static auto observe_buffered_cancellation(
        PairwiseV2RequestHandoff request, PairwiseV2ControlReaderState control,
        std::optional<std::string_view> complete_cancel_body,
        PairwiseV2CancellationPhase first_observation_phase) noexcept
        -> std::expected<PairwiseV2BufferedCancellation,
                         PairwiseV2RunnerSessionFailure>;
    [[nodiscard]] static auto run(
        PairwiseV2BufferedCancellation input, RunnerContext context,
        PairwiseMonitoringHostCapacityStartupAccess::LowerOperations lower_operations,
        ipc::ByteWriter& events,
        PairwiseV2RunnerSessionOperations operations = {}) noexcept
        -> std::expected<PairwiseV2RunnerSessionOutcome,
                         PairwiseV2RunnerSessionFailure>;
    [[nodiscard]] static auto release_typed_monitoring_capacity(
        PairwiseV2PreparedRunnerHandoff&&) noexcept
        -> std::expected<PairwiseTypedMonitoringCapacityHandoff,
                         PairwiseV2RunnerSessionFailure>;
};

} // namespace epycbench::runtime::detail
