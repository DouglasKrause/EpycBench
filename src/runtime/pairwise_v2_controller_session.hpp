#pragma once

#include "runtime/controller_lifecycle_access.hpp"
#include "runtime/pairwise_v2_runner_session.hpp"

#include <chrono>
#include <array>
#include <expected>
#include <optional>

namespace epycbench::runtime::detail {

enum class PairwiseV2ControllerProofKind : unsigned char {
    rejected,
    accepted_prepared_non_success,
    cancelled
};

enum class PairwiseV2ControllerSessionFailure : unsigned char {
    invalid_input,
    channel_creation,
    spawn,
    deadline,
    offer_write,
    request_write,
    cancel_write,
    startup_read,
    accepted_read,
    cancelled_read,
    endpoint_close,
    termination,
    reap,
    process_nonzero,
    process_signaled,
    unexpected_eof,
    trailing_event,
    contained_exception,
    contained_unknown_failure
};

struct PairwiseV2ControllerSessionError final {
    PairwiseV2ControllerSessionError(PairwiseV2ControllerSessionFailure value) noexcept
        : primary{value} {}
    PairwiseV2ControllerSessionFailure primary{};
    std::array<PairwiseV2ControllerSessionFailure, 8> secondary{};
    std::size_t secondary_count{};
    std::optional<platform_linux::ProcessExitStatus> process_status;
    bool clean_eof{};
    bool termination_sent{};
    bool kill_sent{};
};

struct PairwiseV2ControllerProof final {
    PairwiseV2ControllerProofKind kind{};
    std::optional<PairwiseHostCapacityRejectionReason> rejection;
    platform_linux::ProcessExitStatus process_status;
    bool termination_sent{};
    bool kill_sent{};
};

struct PairwiseV2ControllerSessionConfig final {
    std::chrono::milliseconds startup_timeout;
    std::chrono::milliseconds termination_grace;
};

struct PairwiseV2ControllerSessionAccess final {
    [[nodiscard]] static auto run(
        const platform_linux::ChildSpawnRequest& spawn_request,
        AuthoredPairwiseHostCapacityOffer offer,
        const PairwiseHostCapacityRequestTranscript& request,
        const PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority& controller,
        PairwiseV2ControllerSessionConfig config,
        ControllerDependencies dependencies,
        std::optional<PairwiseV2CancellationReason> cancellation = std::nullopt) noexcept
        -> std::expected<PairwiseV2ControllerProof,
                         PairwiseV2ControllerSessionError>;
};

} // namespace epycbench::runtime::detail
