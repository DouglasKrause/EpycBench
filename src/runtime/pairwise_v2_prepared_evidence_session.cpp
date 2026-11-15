#include "runtime/pairwise_v2_prepared_evidence_session.hpp"
#include "runtime/pairwise_v2_prepared_resource_session.hpp"

#include <utility>

namespace epycbench::runtime::detail {

auto PairwiseV2PreparedEvidenceSessionAccess::adopt_runner(
    PairwiseV2EventWriterState&& state, PreparedRunnerEvidenceSession& role) noexcept
    -> std::expected<PairwiseV2PreparedEvidenceRunner, FinalizedEvidenceTransportFailure> {
    auto& sender = role.sender();
    if (state.phase_ != PairwiseV2EventWriterState::Phase::accepted) {
        sender.terminal_ = true;
        return std::unexpected{FinalizedEvidenceTransportFailure::one_use};
    }
    state.phase_ = PairwiseV2EventWriterState::Phase::invalid;
    if (sender.active_ || sender.terminal_ || sender.frame_.empty()) {
        sender.terminal_ = true;
        return std::unexpected{FinalizedEvidenceTransportFailure::one_use};
    }
    const FinalizedEvidenceTransportIdentity identity{state.identity_.first, state.identity_.second};
    if (identity != role.identity().pair || identity != sender.identity_) {
        sender.terminal_ = true;
        return std::unexpected{FinalizedEvidenceTransportFailure::identity_mismatch};
    }
    return PairwiseV2PreparedEvidenceRunner{std::move(sender)};
}

auto PairwiseV2PreparedEvidenceSessionAccess::adopt_controller(
    PairwiseV2EventReaderState&& state, PreparedControllerEvidenceSession& role) noexcept
    -> std::expected<PairwiseV2PreparedEvidenceController, FinalizedEvidenceTransportFailure> {
    auto& receiver = role.receiver();
    if (state.phase_ != PairwiseV2EventReaderState::Phase::accepted) {
        receiver.terminal_ = true;
        return std::unexpected{FinalizedEvidenceTransportFailure::one_use};
    }
    state.phase_ = PairwiseV2EventReaderState::Phase::invalid;
    if (receiver.active_ || receiver.terminal_ || receiver.frame_.empty() || receiver.logical_.empty()) {
        receiver.terminal_ = true;
        return std::unexpected{FinalizedEvidenceTransportFailure::one_use};
    }
    const FinalizedEvidenceTransportIdentity identity{state.identity_.first, state.identity_.second};
    if (identity != role.identity().pair || identity != receiver.identity_) {
        receiver.terminal_ = true;
        return std::unexpected{FinalizedEvidenceTransportFailure::identity_mismatch};
    }
    return PairwiseV2PreparedEvidenceController{std::move(receiver)};
}

auto PairwiseV2PreparedEvidenceSessionAccess::prepare_runner(
    PairwiseV2EventWriterState&& state, FinalizedEvidenceTransportPlan plan) noexcept
    -> std::expected<PairwiseV2PreparedEvidenceRunner, FinalizedEvidenceTransportFailure> {
    if (state.phase_ != PairwiseV2EventWriterState::Phase::accepted)
        return std::unexpected{FinalizedEvidenceTransportFailure::one_use};
    const auto identity =
        FinalizedEvidenceTransportIdentity{state.identity_.first, state.identity_.second};
    state.phase_ = PairwiseV2EventWriterState::Phase::invalid;
    auto sender = PreparedFinalizedEvidenceSender::prepare(plan, identity);
    if (!sender)
        return std::unexpected{sender.error()};
    return PairwiseV2PreparedEvidenceRunner{std::move(*sender)};
}

auto PairwiseV2PreparedEvidenceSessionAccess::prepare_controller(
    PairwiseV2EventReaderState&& state, FinalizedEvidenceTransportPlan plan) noexcept
    -> std::expected<PairwiseV2PreparedEvidenceController, FinalizedEvidenceTransportFailure> {
    if (state.phase_ != PairwiseV2EventReaderState::Phase::accepted)
        return std::unexpected{FinalizedEvidenceTransportFailure::one_use};
    const auto identity =
        FinalizedEvidenceTransportIdentity{state.identity_.first, state.identity_.second};
    state.phase_ = PairwiseV2EventReaderState::Phase::invalid;
    auto receiver = PreparedFinalizedEvidenceReceiver::prepare(plan, identity);
    if (!receiver)
        return std::unexpected{receiver.error()};
    return PairwiseV2PreparedEvidenceController{std::move(*receiver)};
}

auto PairwiseV2PreparedEvidenceRunner::activate(std::span<const char> canonical) noexcept
    -> std::expected<void, FinalizedEvidenceTransportFailure> {
    return sender_.activate(canonical);
}

auto PairwiseV2PreparedEvidenceRunner::write_step(ipc::ByteWriter& writer,
                                                  PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure> {
    return sender_.write_step(writer, signal);
}

auto PairwiseV2PreparedEvidenceRunner::write_step(PreparedByteWriter writer,
                                                  PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure> {
    return sender_.write_step(writer, signal);
}

auto PairwiseV2PreparedEvidenceController::activate() noexcept
    -> std::expected<void, FinalizedEvidenceTransportFailure> {
    return receiver_.activate();
}

auto PairwiseV2PreparedEvidenceController::read_step(ipc::ByteReader& reader,
                                                     PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure> {
    return receiver_.read_step(reader, signal);
}

auto PairwiseV2PreparedEvidenceController::read_step(PreparedByteReader reader,
                                                     PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure> {
    return receiver_.read_step(reader, signal);
}

auto PairwiseV2PreparedEvidenceController::finish() noexcept
    -> std::expected<std::span<const char>, FinalizedEvidenceTransportFailure> {
    return receiver_.finish();
}

} // namespace epycbench::runtime::detail
