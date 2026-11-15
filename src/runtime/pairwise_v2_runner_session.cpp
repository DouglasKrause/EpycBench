#include "runtime/pairwise_v2_runner_session.hpp"

namespace epycbench::runtime::detail {

auto PairwiseV2RunnerSessionAccess::observe_buffered_cancellation(
    PairwiseV2RequestHandoff request, PairwiseV2ControlReaderState control,
    std::optional<std::string_view> complete_cancel_body,
    PairwiseV2CancellationPhase first_observation_phase) noexcept
    -> std::expected<PairwiseV2BufferedCancellation,
                     PairwiseV2RunnerSessionFailure> {
    if (!PairwiseV2PhaseAProtocolAccess::validate_request_control_binding(
            request, control))
        return std::unexpected{
            PairwiseV2RunnerSessionFailure::cancellation_observation};
    std::optional<PairwiseV2CancellationReceipt> cancellation;
    if (complete_cancel_body) {
        auto receipt = PairwiseV2PhaseAProtocolAccess::read_cancel(
            control, *complete_cancel_body);
        if (!receipt)
            return std::unexpected{
                PairwiseV2RunnerSessionFailure::cancellation_observation};
        cancellation.emplace(std::move(*receipt));
    }
    return PairwiseV2BufferedCancellation{
        std::move(request), std::move(control), std::move(cancellation),
        first_observation_phase};
}

auto PairwiseV2RunnerSessionAccess::run(
    PairwiseV2BufferedCancellation input, RunnerContext context,
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations lower_operations,
    ipc::ByteWriter& events, PairwiseV2RunnerSessionOperations operations) noexcept
    -> std::expected<PairwiseV2RunnerSessionOutcome,
                     PairwiseV2RunnerSessionFailure> {
    const auto identity = input.request_.identity();
    auto gate_inputs = std::move(input.request_).consume();
    if (!gate_inputs)
        return std::unexpected{PairwiseV2RunnerSessionFailure::request_handoff};

    const auto reject = [&](HostCapacityRejectedAckCapability capability)
        -> std::expected<PairwiseV2RunnerSessionOutcome,
                         PairwiseV2RunnerSessionFailure> {
        auto body = PairwiseMonitoringHostCapacityStartupAccess::encode_rejected_ack(
            std::move(capability));
        if (!body)
            return std::unexpected{
                PairwiseV2RunnerSessionFailure::rejected_acknowledgement};
        auto parsed = PairwiseMonitoringHostCapacityStartupAccess::parse_rejected_ack(*body);
        if (!parsed)
            return std::unexpected{
                PairwiseV2RunnerSessionFailure::rejected_acknowledgement};
        if (!PairwiseMonitoringHostCapacityStartupAccess::write_frame(events, *body))
            return std::unexpected{
                PairwiseV2RunnerSessionFailure::rejected_acknowledgement};
        return PairwiseV2RunnerSessionOutcome{
            std::in_place_type<PairwiseV2RunnerRejected>, identity, parsed->reason};
    };

    auto runner = PairwiseMonitoringHostCapacityStartupAccess::mint_runner_with(
        gate_inputs->commitment, gate_inputs->request, context, lower_operations);
    if (!runner)
        return reject(std::move(runner.error()));
    auto proof = PairwiseMonitoringHostCapacityStartupAccess::gate_a(
        gate_inputs->commitment, gate_inputs->request, *runner);
    if (!proof)
        return reject(std::move(proof.error()));

    auto contextual = PairwiseV2ContextualAdmissionAccess::admit_after_gate_a(
        std::move(gate_inputs->request), std::move(context), std::move(*proof),
        std::move(*runner));
    if (!contextual)
        return reject(PairwiseV2ContextualAdmissionAccess::reject(
            std::move(contextual.error())));
    struct RetirementProbe final {
        PairwiseV2RunnerSessionOperations operations;
        bool active{};
        ~RetirementProbe() {
            if (active && operations.sidecar_retired)
                operations.sidecar_retired(operations.context);
        }
    } retirement_probe{operations};
    auto admitted = PairwiseV2ContextualAdmissionAccess::gate_b(
        std::move(*contextual));
    if (!admitted)
        return reject(std::move(admitted.error()));
    retirement_probe.active = true;

    auto ack = PairwiseMonitoringHostCapacityStartupAccess::issue_accepted_ack(*admitted);
    if (!ack)
        return std::unexpected{
            PairwiseV2RunnerSessionFailure::startup_acknowledgement};
    auto event_state = PairwiseV2PhaseAProtocolAccess::write_startup_accepted_ack(
        events, std::move(*ack));
    if (!event_state)
        return std::unexpected{
            PairwiseV2RunnerSessionFailure::startup_acknowledgement};
    auto accepted = PairwiseV2PhaseAProtocolAccess::prepare_accepted_write(
        std::move(*event_state));
    if (!accepted)
        return std::unexpected{PairwiseV2RunnerSessionFailure::accepted_event};
    auto accepted_state = PairwiseV2PhaseAProtocolAccess::commit_event_write(
        events, std::move(*accepted));
    if (!accepted_state)
        return std::unexpected{PairwiseV2RunnerSessionFailure::accepted_event};

    if (input.cancellation_) {
        const auto reason = input.cancellation_->reason();
        auto cancelled = PairwiseV2PhaseAProtocolAccess::prepare_cancelled_write(
            std::move(*accepted_state), std::move(*input.cancellation_), input.phase_);
        if (!cancelled)
            return std::unexpected{PairwiseV2RunnerSessionFailure::cancelled_event};
        auto terminal_state = PairwiseV2PhaseAProtocolAccess::commit_event_write(
            events, std::move(*cancelled));
        if (!terminal_state)
            return std::unexpected{PairwiseV2RunnerSessionFailure::cancelled_event};
        return PairwiseV2RunnerSessionOutcome{
            std::in_place_type<PairwiseV2RunnerCancelled>, identity, reason};
    }

    auto prepared = PairwiseV2PreparedRunnerHandoff{
        identity, std::move(*admitted), std::move(*accepted_state),
        std::move(input.control_), operations};
    retirement_probe.active = false;
    return PairwiseV2RunnerSessionOutcome{std::move(prepared)};
}

auto PairwiseV2RunnerSessionAccess::release_typed_monitoring_capacity(
    PairwiseV2PreparedRunnerHandoff&& prepared) noexcept
    -> std::expected<PairwiseTypedMonitoringCapacityHandoff,
                     PairwiseV2RunnerSessionFailure> {
    if (std::exchange(prepared.capacity_released_, true))
        return std::unexpected{PairwiseV2RunnerSessionFailure::capacity_handoff};
    prepared.retirement_active_ = false;
    return PairwiseTypedMonitoringCapacityHandoff{
        std::move(prepared.startup_.admission_), prepared.startup_.first_,
        prepared.startup_.second_, std::move(prepared.startup_.first_identity_),
        std::move(prepared.startup_.second_identity_), prepared.operations_.context,
        prepared.operations_.sidecar_retired};
}

} // namespace epycbench::runtime::detail
