#include "runtime/pairwise_v2_phase_a_protocol.hpp"

#include "epycbench/core/canonical_json.hpp"

#include <charconv>
#include <exception>
#include <ranges>
#include <set>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

using Failure = PairwiseV2PhaseAFailure;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

struct Envelope final {
    std::string type;
    ipc::RequestId request_id;
    std::uint64_t sequence{};
    std::string profile;
    Json payload;
};

[[nodiscard]] auto map_startup_failure(
    PairwiseHostCapacityStartupFailure failure) noexcept -> Failure {
    switch (failure) {
    case PairwiseHostCapacityStartupFailure::invalid_encoding:
        return Failure::invalid_encoding;
    case PairwiseHostCapacityStartupFailure::unsupported_protocol:
        return Failure::unsupported_protocol;
    case PairwiseHostCapacityStartupFailure::unexpected_message:
        return Failure::unexpected_message;
    case PairwiseHostCapacityStartupFailure::sequence_mismatch:
        return Failure::sequence_mismatch;
    case PairwiseHostCapacityStartupFailure::frame_zero:
        return Failure::frame_zero;
    case PairwiseHostCapacityStartupFailure::frame_over_limit:
        return Failure::frame_over_limit;
    case PairwiseHostCapacityStartupFailure::truncated_frame:
        return Failure::truncated_frame;
    case PairwiseHostCapacityStartupFailure::io_failure:
        return Failure::io_failure;
    case PairwiseHostCapacityStartupFailure::allocation:
        return Failure::allocation;
    case PairwiseHostCapacityStartupFailure::contained_exception:
        return Failure::contained_exception;
    case PairwiseHostCapacityStartupFailure::contained_unknown_failure:
        return Failure::contained_unknown_failure;
    default:
        return Failure::invalid_envelope;
    }
}

[[nodiscard]] auto exact(const Json& value,
                         std::initializer_list<std::string_view> names) noexcept -> bool {
    if (!value.is_object() || value.size() != names.size())
        return false;
    return std::ranges::all_of(names,
                               [&](std::string_view name) { return value.contains(name); });
}

[[nodiscard]] auto has_float(const Json& value) noexcept -> bool {
    if (value.is_number_float())
        return true;
    if (value.is_structured())
        return std::ranges::any_of(value, has_float);
    return false;
}

[[nodiscard]] auto parse_json(std::string_view body) -> std::expected<Json, Failure> {
    if (body.empty())
        return std::unexpected{Failure::frame_zero};
    if (body.size() > ipc::maximum_frame_body_size)
        return std::unexpected{Failure::frame_over_limit};
    if (body.starts_with("\xef\xbb\xbf") || body.find('\0') != std::string_view::npos)
        return std::unexpected{Failure::invalid_encoding};
    bool duplicate{};
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::key &&
                 !keys.back().insert(value.get<std::string>()).second)
            duplicate = true;
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    };
    auto parsed = Json::parse(body, callback, false);
    if (parsed.is_discarded() || duplicate || has_float(parsed))
        return std::unexpected{Failure::invalid_encoding};
    auto canonical = core::canonicalize_json(parsed);
    if (!canonical || *canonical != body)
        return std::unexpected{Failure::invalid_encoding};
    return parsed;
}

[[nodiscard]] auto decimal(std::string_view text) -> std::optional<std::uint64_t> {
    if (text.empty() || (text.size() > 1U && text.front() == '0'))
        return std::nullopt;
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

[[nodiscard]] auto parse_envelope(std::string_view body) -> std::expected<Envelope, Failure> {
    auto parsed = parse_json(body);
    if (!parsed)
        return std::unexpected{parsed.error()};
    if (!exact(*parsed, {"payload", "payload_profile", "protocol", "request_id",
                         "sequence", "type"}) ||
        !(*parsed)["protocol"].is_string() || !(*parsed)["type"].is_string() ||
        !(*parsed)["request_id"].is_string() || !(*parsed)["sequence"].is_string() ||
        !(*parsed)["payload_profile"].is_string() || !(*parsed)["payload"].is_object())
        return std::unexpected{Failure::invalid_envelope};
    if ((*parsed)["protocol"].get_ref<const std::string&>() != pairwise_v2_protocol)
        return std::unexpected{Failure::unsupported_protocol};
    auto request = ipc::RequestId::parse(
        (*parsed)["request_id"].get_ref<const std::string&>());
    auto sequence = decimal((*parsed)["sequence"].get_ref<const std::string&>());
    if (!request || !sequence)
        return std::unexpected{Failure::invalid_envelope};
    return Envelope{(*parsed)["type"].get<std::string>(), *request, *sequence,
                    (*parsed)["payload_profile"].get<std::string>(),
                    std::move((*parsed)["payload"])};
}

[[nodiscard]] auto encode_envelope(std::string_view type, ipc::RequestId request,
                                   std::uint64_t sequence, std::string_view profile,
                                   OrderedJson payload)
    -> std::expected<std::string, Failure> {
    Json envelope{{"payload", std::move(payload)},
                  {"payload_profile", profile},
                  {"protocol", pairwise_v2_protocol},
                  {"request_id", request.text()},
                  {"sequence", std::to_string(sequence)},
                  {"type", type}};
    auto canonical = core::canonicalize_json(envelope);
    if (!canonical)
        return std::unexpected{Failure::invalid_encoding};
    if (canonical->empty())
        return std::unexpected{Failure::frame_zero};
    if (canonical->size() > ipc::maximum_frame_body_size)
        return std::unexpected{Failure::frame_over_limit};
    return *canonical;
}

[[nodiscard]] auto reason_token(PairwiseV2CancellationReason reason) noexcept
    -> std::string_view {
    switch (reason) {
    case PairwiseV2CancellationReason::user_interrupt:
        return "user_interrupt";
    case PairwiseV2CancellationReason::controller_shutdown:
        return "controller_shutdown";
    case PairwiseV2CancellationReason::controller_timeout:
        return "controller_timeout";
    }
    std::terminate();
}

[[nodiscard]] auto parse_reason(const Json& value)
    -> std::optional<PairwiseV2CancellationReason> {
    if (!value.is_string())
        return std::nullopt;
    const auto& token = value.get_ref<const std::string&>();
    if (token == "user_interrupt")
        return PairwiseV2CancellationReason::user_interrupt;
    if (token == "controller_shutdown")
        return PairwiseV2CancellationReason::controller_shutdown;
    if (token == "controller_timeout")
        return PairwiseV2CancellationReason::controller_timeout;
    return std::nullopt;
}

[[nodiscard]] auto phase_token(PairwiseV2CancellationPhase phase) noexcept
    -> std::string_view {
    switch (phase) {
    case PairwiseV2CancellationPhase::startup:
        return "startup";
    case PairwiseV2CancellationPhase::admission:
        return "admission";
    case PairwiseV2CancellationPhase::service:
        return "service";
    case PairwiseV2CancellationPhase::execution:
        return "execution";
    case PairwiseV2CancellationPhase::cleanup:
        return "cleanup";
    }
    std::terminate();
}

[[nodiscard]] auto parse_phase(const Json& value)
    -> std::optional<PairwiseV2CancellationPhase> {
    if (!value.is_string())
        return std::nullopt;
    const auto& token = value.get_ref<const std::string&>();
    if (token == "startup")
        return PairwiseV2CancellationPhase::startup;
    if (token == "admission")
        return PairwiseV2CancellationPhase::admission;
    if (token == "service")
        return PairwiseV2CancellationPhase::service;
    if (token == "execution")
        return PairwiseV2CancellationPhase::execution;
    if (token == "cleanup")
        return PairwiseV2CancellationPhase::cleanup;
    return std::nullopt;
}

[[nodiscard]] auto payload_identity(const Json& payload)
    -> std::optional<PairwiseV2PairIdentity> {
    if (!payload["first_request_id"].is_string() ||
        !payload["second_request_id"].is_string())
        return std::nullopt;
    auto first = ipc::RequestId::parse(
        payload["first_request_id"].get_ref<const std::string&>());
    auto second = ipc::RequestId::parse(
        payload["second_request_id"].get_ref<const std::string&>());
    if (!first || !second || *first == *second)
        return std::nullopt;
    return PairwiseV2PairIdentity{*first, *second};
}

[[nodiscard]] auto validate_envelope(const Envelope& envelope, std::string_view type,
                                     std::uint64_t sequence, std::string_view profile,
                                     const PairwiseV2PairIdentity* expected) noexcept
    -> std::expected<void, Failure> {
    if (envelope.type != type)
        return std::unexpected{Failure::unexpected_message};
    if (envelope.sequence != sequence)
        return std::unexpected{Failure::sequence_mismatch};
    if (envelope.profile != profile)
        return std::unexpected{Failure::unsupported_profile};
    if (expected && envelope.request_id != expected->first)
        return std::unexpected{Failure::identity_mismatch};
    return {};
}

template <class Callable, class Result = std::invoke_result_t<Callable>>
[[nodiscard]] auto contained(Callable&& callable) noexcept
    -> std::expected<Result, Failure> {
    try {
        return std::forward<Callable>(callable)();
    } catch (Failure failure) {
        return std::unexpected{failure};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

} // namespace

PairwiseV2RequestHandoff::PairwiseV2RequestHandoff(
    PairwiseV2RequestHandoff&& other) noexcept
    : identity_{other.identity_}, request_{std::move(other.request_)},
      commitment_{std::move(other.commitment_)},
      active_{std::exchange(other.active_, false)} {}

auto PairwiseV2RequestHandoff::consume() && noexcept
    -> std::expected<PairwiseV2GateAInputs, PairwiseV2PhaseAFailure> {
    if (!std::exchange(active_, false))
        return std::unexpected{PairwiseV2PhaseAFailure::invalid_state};
    return PairwiseV2GateAInputs{std::move(request_), std::move(commitment_)};
}

PairwiseV2ControlWriterState::PairwiseV2ControlWriterState(
    PairwiseV2ControlWriterState&& other) noexcept
    : phase_{std::exchange(other.phase_, Phase::invalid)}, identity_{other.identity_},
      validation_{std::move(other.validation_)},
      cancellation_reason_{std::move(other.cancellation_reason_)},
      acknowledged_{other.acknowledged_} {
    other.cancellation_reason_.reset();
    other.acknowledged_ = false;
}

PairwiseV2ControlReaderState::PairwiseV2ControlReaderState(
    PairwiseV2ControlReaderState&& other) noexcept
    : phase_{std::exchange(other.phase_, Phase::invalid)},
      identity_{std::move(other.identity_)}, commitment_{std::move(other.commitment_)} {
    other.identity_.reset();
    other.commitment_.reset();
}

PairwiseV2EventWriterState::PairwiseV2EventWriterState(
    PairwiseV2EventWriterState&& other) noexcept
    : phase_{std::exchange(other.phase_, Phase::invalid)}, identity_{other.identity_} {}

PairwiseV2EventReaderState::PairwiseV2EventReaderState(
    PairwiseV2EventReaderState&& other) noexcept
    : phase_{std::exchange(other.phase_, Phase::invalid)}, identity_{other.identity_},
      expected_cancellation_{std::move(other.expected_cancellation_)} {
    other.expected_cancellation_.reset();
}

PairwiseV2CancellationReceipt::PairwiseV2CancellationReceipt(
    PairwiseV2CancellationReceipt&& other) noexcept
    : identity_{other.identity_}, reason_{other.reason_},
      active_{std::exchange(other.active_, false)} {}

PairwiseV2ControlWriteCapability::PairwiseV2ControlWriteCapability(
    PairwiseV2ControlWriteCapability&& other) noexcept
    : body_{std::move(other.body_)}, state_{std::move(other.state_)}, kind_{other.kind_},
      active_{std::exchange(other.active_, false)} {}

PairwiseV2EventWriteCapability::PairwiseV2EventWriteCapability(
    PairwiseV2EventWriteCapability&& other) noexcept
    : body_{std::move(other.body_)}, state_{std::move(other.state_)}, kind_{other.kind_},
      active_{std::exchange(other.active_, false)} {}

auto PairwiseV2PhaseAProtocolAccess::write_startup_offer(
    ipc::ByteWriter& writer, AuthoredPairwiseHostCapacityOffer offer) noexcept
    -> std::expected<PairwiseV2ControlWriterState, Failure> {
    const PairwiseV2PairIdentity identity{offer.validation_.first_,
                                          offer.validation_.second_};
    auto written = PairwiseMonitoringHostCapacityStartupAccess::write_frame(
        writer, offer.body_);
    if (!written)
        return std::unexpected{map_startup_failure(written.error())};
    return PairwiseV2ControlWriterState{identity, std::move(offer.validation_)};
}

auto PairwiseV2PhaseAProtocolAccess::read_startup_offer(
    ipc::ByteReader& reader, stage_b_policy::Selection policy) noexcept
    -> std::expected<PairwiseV2ControlReaderState, Failure> {
    auto body = PairwiseMonitoringHostCapacityStartupAccess::read_frame(reader);
    if (!body)
        return std::unexpected{map_startup_failure(body.error())};
    if (!*body)
        return std::unexpected{Failure::truncated_frame};
    if (policy == stage_b_policy::Selection::stage_b_v1 &&
        !stage_b_policy::admit_bootstrap(**body))
        return std::unexpected{Failure::invalid_payload};
    auto commitment = PairwiseMonitoringHostCapacityStartupAccess::parse_offer(**body);
    if (!commitment)
        return std::unexpected{map_startup_failure(commitment.error())};
    return PairwiseV2ControlReaderState{std::move(*commitment)};
}

auto PairwiseV2PhaseAProtocolAccess::provisional_identity(
    const PairwiseV2ControlReaderState& state) noexcept -> std::optional<PairwiseV2PairIdentity> {
    if (state.phase_ != PairwiseV2ControlReaderState::Phase::start || !state.commitment_)
        return std::nullopt;
    return PairwiseV2PairIdentity{state.commitment_->first_request_id(),
                                 state.commitment_->second_request_id()};
}

auto PairwiseV2PhaseAProtocolAccess::write_startup_accepted_ack(
    ipc::ByteWriter& writer, HostCapacityAcceptedAckCapability capability) noexcept
    -> std::expected<PairwiseV2EventWriterState, Failure> {
    const PairwiseV2PairIdentity identity{capability.first_, capability.second_};
    auto body = PairwiseMonitoringHostCapacityStartupAccess::encode_accepted_ack(
        std::move(capability));
    if (!body)
        return std::unexpected{map_startup_failure(body.error())};
    auto written = PairwiseMonitoringHostCapacityStartupAccess::write_frame(writer, *body);
    if (!written)
        return std::unexpected{map_startup_failure(written.error())};
    return PairwiseV2EventWriterState{identity};
}

auto PairwiseV2PhaseAProtocolAccess::read_startup_accepted_ack(
    ipc::ByteReader& reader, PairwiseV2ControlWriterState control,
    const PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority& controller) noexcept
    -> std::expected<PairwiseV2ControllerAcceptedHandoff, Failure> {
    if (control.phase_ != PairwiseV2ControlWriterState::Phase::requested &&
        control.phase_ != PairwiseV2ControlWriterState::Phase::cancelled)
        return std::unexpected{Failure::invalid_state};
    if (control.phase_ == PairwiseV2ControlWriterState::Phase::cancelled &&
        !control.cancellation_reason_)
        return std::unexpected{Failure::invalid_state};
    auto body = PairwiseMonitoringHostCapacityStartupAccess::read_frame(reader);
    if (!body)
        return std::unexpected{map_startup_failure(body.error())};
    if (!*body)
        return std::unexpected{Failure::truncated_frame};
    auto ack = PairwiseMonitoringHostCapacityStartupAccess::parse_accepted_ack(**body);
    if (!ack)
        return std::unexpected{map_startup_failure(ack.error())};
    auto valid = PairwiseMonitoringHostCapacityStartupAccess::validate_accepted_ack(
        control.validation_, controller, *ack);
    if (!valid)
        return std::unexpected{map_startup_failure(valid.error())};
    const auto expected_cancellation = control.cancellation_reason_;
    control.acknowledged_ = true;
    control.phase_ = control.phase_ == PairwiseV2ControlWriterState::Phase::requested
                         ? PairwiseV2ControlWriterState::Phase::requested_acknowledged
                         : PairwiseV2ControlWriterState::Phase::cancelled_acknowledged;
    return PairwiseV2ControllerAcceptedHandoff{
        std::move(control),
        PairwiseV2EventReaderState{
            PairwiseV2PairIdentity{ack->first, ack->second}, expected_cancellation}};
}

auto PairwiseV2PhaseAProtocolAccess::read_startup_outcome(
    ipc::ByteReader& reader, PairwiseV2ControlWriterState control,
    const PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority& controller) noexcept
    -> std::expected<PairwiseV2ControllerStartupOutcome, Failure> {
    if (control.phase_ != PairwiseV2ControlWriterState::Phase::requested &&
        control.phase_ != PairwiseV2ControlWriterState::Phase::cancelled)
        return std::unexpected{Failure::invalid_state};
    if (control.phase_ == PairwiseV2ControlWriterState::Phase::cancelled &&
        !control.cancellation_reason_)
        return std::unexpected{Failure::invalid_state};
    auto body = PairwiseMonitoringHostCapacityStartupAccess::read_frame(reader);
    if (!body)
        return std::unexpected{map_startup_failure(body.error())};
    if (!*body)
        return std::unexpected{Failure::truncated_frame};
    if (auto accepted = PairwiseMonitoringHostCapacityStartupAccess::parse_accepted_ack(**body)) {
        auto valid = PairwiseMonitoringHostCapacityStartupAccess::validate_accepted_ack(
            control.validation_, controller, *accepted);
        if (!valid)
            return std::unexpected{map_startup_failure(valid.error())};
        const auto expected_cancellation = control.cancellation_reason_;
        control.acknowledged_ = true;
        control.phase_ = control.phase_ == PairwiseV2ControlWriterState::Phase::requested
                             ? PairwiseV2ControlWriterState::Phase::requested_acknowledged
                             : PairwiseV2ControlWriterState::Phase::cancelled_acknowledged;
        auto handoff = PairwiseV2ControllerAcceptedHandoff{
            std::move(control),
            PairwiseV2EventReaderState{
                PairwiseV2PairIdentity{accepted->first, accepted->second},
                expected_cancellation}};
        return PairwiseV2ControllerStartupOutcome{std::move(handoff)};
    }
    auto rejected = PairwiseMonitoringHostCapacityStartupAccess::parse_rejected_ack(**body);
    if (!rejected)
        return std::unexpected{map_startup_failure(rejected.error())};
    if (rejected->first != control.identity_.first ||
        rejected->second != control.identity_.second ||
        rejected->request != control.validation_.request_sha256_ ||
        rejected->offer != control.validation_.body_sha256_)
        return std::unexpected{Failure::identity_mismatch};
    return PairwiseV2ControllerStartupOutcome{
        std::in_place_type<PairwiseV2ControllerRejected>, control.identity_,
        rejected->reason};
}

auto PairwiseV2PhaseAProtocolAccess::prepare_request_write(
    PairwiseV2ControlWriterState state,
    const PairwiseHostCapacityRequestTranscript& request) noexcept
    -> std::expected<PairwiseV2ControlWriteCapability, Failure> {
    if (state.phase_ != PairwiseV2ControlWriterState::Phase::start)
        return std::unexpected{Failure::invalid_state};
    if (state.identity_ != PairwiseV2PairIdentity{request.first_request_id(),
                                                  request.second_request_id()} ||
        state.validation_.request_sha256_ != request.body_sha256())
        return std::unexpected{Failure::identity_mismatch};
    auto body = contained([&]() {
        return std::string{request.canonical_body()};
    });
    if (!body)
        return std::unexpected{body.error()};
    state.phase_ = PairwiseV2ControlWriterState::Phase::request_write_started;
    return PairwiseV2ControlWriteCapability{
        std::move(*body), std::move(state),
        PairwiseV2ControlWriteCapability::Kind::request};
}

auto PairwiseV2PhaseAProtocolAccess::commit_control_write(
    ipc::ByteWriter& writer, PairwiseV2ControlWriteCapability capability) noexcept
    -> std::expected<PairwiseV2ControlWriterState, Failure> {
    if (!std::exchange(capability.active_, false))
        return std::unexpected{Failure::invalid_state};
    auto& state = capability.state_;
    const auto expected = capability.kind_ == PairwiseV2ControlWriteCapability::Kind::request
                              ? PairwiseV2ControlWriterState::Phase::request_write_started
                              : PairwiseV2ControlWriterState::Phase::cancel_write_started;
    if (state.phase_ != expected)
        return std::unexpected{Failure::invalid_state};
    auto written = PairwiseMonitoringHostCapacityStartupAccess::write_frame(
        writer, capability.body_);
    if (!written)
        return std::unexpected{map_startup_failure(written.error())};
    if (capability.kind_ == PairwiseV2ControlWriteCapability::Kind::request)
        state.phase_ = PairwiseV2ControlWriterState::Phase::requested;
    else
        state.phase_ = state.acknowledged_
                           ? PairwiseV2ControlWriterState::Phase::cancelled_acknowledged
                           : PairwiseV2ControlWriterState::Phase::cancelled;
    return std::move(state);
}

auto PairwiseV2PhaseAProtocolAccess::read_request(PairwiseV2ControlReaderState& state,
                                                  std::string_view body) noexcept
    -> std::expected<PairwiseV2RequestHandoff, Failure> {
    if (state.phase_ != PairwiseV2ControlReaderState::Phase::start)
        return std::unexpected{Failure::invalid_state};
    state.phase_ = PairwiseV2ControlReaderState::Phase::invalid;
    auto request = PairwiseMonitoringHostCapacityStartupAccess::parse_request(body);
    if (!request)
        return std::unexpected{map_startup_failure(request.error())};
    const PairwiseV2PairIdentity identity{request->first_request_id(),
                                          request->second_request_id()};
    if (!state.commitment_ || state.commitment_->first_request_id() != identity.first ||
        state.commitment_->second_request_id() != identity.second ||
        state.commitment_->request_body_sha256() != request->body_sha256())
        return std::unexpected{Failure::identity_mismatch};
    state.identity_ = identity;
    state.phase_ = PairwiseV2ControlReaderState::Phase::requested;
    auto commitment = std::move(*state.commitment_);
    state.commitment_.reset();
    return PairwiseV2RequestHandoff{std::move(*request), std::move(commitment)};
}

auto PairwiseV2PhaseAProtocolAccess::validate_request_control_binding(
    const PairwiseV2RequestHandoff& request,
    const PairwiseV2ControlReaderState& control) noexcept
    -> std::expected<void, Failure> {
    if (!request.active_ ||
        control.phase_ != PairwiseV2ControlReaderState::Phase::requested ||
        !control.identity_ || *control.identity_ != request.identity_)
        return std::unexpected{Failure::identity_mismatch};
    return {};
}

auto PairwiseV2PhaseAProtocolAccess::prepare_cancel_write(
    PairwiseV2ControlWriterState state, PairwiseV2CancellationReason reason) noexcept
    -> std::expected<PairwiseV2ControlWriteCapability, Failure> {
    if (state.phase_ != PairwiseV2ControlWriterState::Phase::requested &&
        state.phase_ != PairwiseV2ControlWriterState::Phase::requested_acknowledged)
        return std::unexpected{Failure::invalid_state};
    auto result = contained([&]() {
        return encode_envelope(
            "cancel_pairwise", state.identity_.first, 1, pairwise_cancel_profile,
            OrderedJson{{"first_request_id", state.identity_.first.text()},
                        {"reason", reason_token(reason)},
                        {"second_request_id", state.identity_.second.text()}});
    });
    if (!result)
        return std::unexpected{result.error()};
    if (!*result)
        return std::unexpected{result->error()};
    state.cancellation_reason_ = reason;
    state.phase_ = PairwiseV2ControlWriterState::Phase::cancel_write_started;
    return PairwiseV2ControlWriteCapability{
        std::move(**result), std::move(state),
        PairwiseV2ControlWriteCapability::Kind::cancel};
}

auto PairwiseV2PhaseAProtocolAccess::read_cancel(PairwiseV2ControlReaderState& state,
                                                 std::string_view body) noexcept
    -> std::expected<PairwiseV2CancellationReceipt, Failure> {
    if (state.phase_ != PairwiseV2ControlReaderState::Phase::requested || !state.identity_)
        return std::unexpected{Failure::invalid_state};
    state.phase_ = PairwiseV2ControlReaderState::Phase::invalid;
    return contained([&]() -> PairwiseV2CancellationReceipt {
        auto envelope = parse_envelope(body);
        if (!envelope)
            throw envelope.error();
        auto valid = validate_envelope(*envelope, "cancel_pairwise", 1,
                                       pairwise_cancel_profile, &*state.identity_);
        if (!valid)
            throw valid.error();
        if (!exact(envelope->payload,
                   {"first_request_id", "reason", "second_request_id"}))
            throw Failure::invalid_payload;
        auto identity = payload_identity(envelope->payload);
        auto reason = parse_reason(envelope->payload["reason"]);
        if (!identity || *identity != *state.identity_)
            throw Failure::identity_mismatch;
        if (!reason)
            throw Failure::invalid_payload;
        state.phase_ = PairwiseV2ControlReaderState::Phase::cancelled;
        return PairwiseV2CancellationReceipt{*identity, *reason};
    }).transform_error([](Failure failure) { return failure; });
}

auto PairwiseV2PhaseAProtocolAccess::bind_committed_cancel(
    PairwiseV2EventReaderState& event,
    const PairwiseV2ControlWriterState& control) noexcept
    -> std::expected<void, Failure> {
    if (event.phase_ != PairwiseV2EventReaderState::Phase::accepted ||
        event.expected_cancellation_ ||
        control.phase_ != PairwiseV2ControlWriterState::Phase::cancelled_acknowledged ||
        !control.cancellation_reason_)
        return std::unexpected{Failure::invalid_state};
    if (event.identity_ != control.identity_)
        return std::unexpected{Failure::identity_mismatch};
    event.expected_cancellation_ = *control.cancellation_reason_;
    return {};
}

auto PairwiseV2PhaseAProtocolAccess::prepare_accepted_write(
    PairwiseV2EventWriterState state) noexcept
    -> std::expected<PairwiseV2EventWriteCapability, Failure> {
    if (state.phase_ != PairwiseV2EventWriterState::Phase::start)
        return std::unexpected{Failure::invalid_state};
    auto result = contained([&]() {
        return encode_envelope(
            "pairwise_accepted", state.identity_.first, 0, pairwise_accepted_profile,
            OrderedJson{{"accepted", true},
                        {"first_request_id", state.identity_.first.text()},
                        {"second_request_id", state.identity_.second.text()}});
    });
    if (!result)
        return std::unexpected{result.error()};
    if (!*result)
        return std::unexpected{result->error()};
    state.phase_ = PairwiseV2EventWriterState::Phase::accepted_write_started;
    return PairwiseV2EventWriteCapability{
        std::move(**result), std::move(state),
        PairwiseV2EventWriteCapability::Kind::accepted};
}

auto PairwiseV2PhaseAProtocolAccess::commit_event_write(
    ipc::ByteWriter& writer, PairwiseV2EventWriteCapability capability) noexcept
    -> std::expected<PairwiseV2EventWriterState, Failure> {
    if (!std::exchange(capability.active_, false))
        return std::unexpected{Failure::invalid_state};
    auto& state = capability.state_;
    const auto expected = capability.kind_ == PairwiseV2EventWriteCapability::Kind::accepted
                              ? PairwiseV2EventWriterState::Phase::accepted_write_started
                              : PairwiseV2EventWriterState::Phase::terminal_write_started;
    if (state.phase_ != expected)
        return std::unexpected{Failure::invalid_state};
    auto written = PairwiseMonitoringHostCapacityStartupAccess::write_frame(
        writer, capability.body_);
    if (!written)
        return std::unexpected{map_startup_failure(written.error())};
    state.phase_ = capability.kind_ == PairwiseV2EventWriteCapability::Kind::accepted
                       ? PairwiseV2EventWriterState::Phase::accepted
                       : PairwiseV2EventWriterState::Phase::terminal;
    return std::move(state);
}

auto PairwiseV2PhaseAProtocolAccess::read_accepted(PairwiseV2EventReaderState& state,
                                                   std::string_view body) noexcept
    -> std::expected<PairwiseV2Accepted, Failure> {
    if (state.phase_ != PairwiseV2EventReaderState::Phase::start)
        return std::unexpected{Failure::invalid_state};
    state.phase_ = PairwiseV2EventReaderState::Phase::invalid;
    return contained([&]() -> PairwiseV2Accepted {
        auto envelope = parse_envelope(body);
        if (!envelope)
            throw envelope.error();
        auto valid = validate_envelope(*envelope, "pairwise_accepted", 0,
                                       pairwise_accepted_profile, nullptr);
        if (!valid)
            throw valid.error();
        if (!exact(envelope->payload,
                   {"accepted", "first_request_id", "second_request_id"}) ||
            !envelope->payload["accepted"].is_boolean() ||
            !envelope->payload["accepted"].get<bool>())
            throw Failure::invalid_payload;
        auto identity = payload_identity(envelope->payload);
        if (!identity || envelope->request_id != identity->first ||
            *identity != state.identity_)
            throw Failure::identity_mismatch;
        state.phase_ = PairwiseV2EventReaderState::Phase::accepted;
        return PairwiseV2Accepted{*identity};
    }).transform_error([](Failure failure) { return failure; });
}

auto PairwiseV2PhaseAProtocolAccess::prepare_cancelled_write(
    PairwiseV2EventWriterState state, PairwiseV2CancellationReceipt cancellation,
    PairwiseV2CancellationPhase phase) noexcept
    -> std::expected<PairwiseV2EventWriteCapability, Failure> {
    if (state.phase_ != PairwiseV2EventWriterState::Phase::accepted ||
        !std::exchange(cancellation.active_, false))
        return std::unexpected{Failure::invalid_state};
    if (state.identity_ != cancellation.identity_)
        return std::unexpected{Failure::identity_mismatch};
    auto result = contained([&]() {
        return encode_envelope(
            "pairwise_cancelled", state.identity_.first, 1,
            pairwise_cancelled_profile,
            OrderedJson{{"first_request_id", state.identity_.first.text()},
                        {"phase", phase_token(phase)},
                        {"reason", reason_token(cancellation.reason_)},
                        {"second_request_id", state.identity_.second.text()}});
    });
    if (!result)
        return std::unexpected{result.error()};
    if (!*result)
        return std::unexpected{result->error()};
    state.phase_ = PairwiseV2EventWriterState::Phase::terminal_write_started;
    return PairwiseV2EventWriteCapability{
        std::move(**result), std::move(state),
        PairwiseV2EventWriteCapability::Kind::cancelled};
}

auto PairwiseV2PhaseAProtocolAccess::read_cancelled(PairwiseV2EventReaderState& state,
                                                    std::string_view body) noexcept
    -> std::expected<PairwiseV2Cancelled, Failure> {
    if (state.phase_ != PairwiseV2EventReaderState::Phase::accepted ||
        !state.expected_cancellation_)
        return std::unexpected{Failure::invalid_state};
    state.phase_ = PairwiseV2EventReaderState::Phase::invalid;
    return contained([&]() -> PairwiseV2Cancelled {
        auto envelope = parse_envelope(body);
        if (!envelope)
            throw envelope.error();
        auto valid = validate_envelope(*envelope, "pairwise_cancelled", 1,
                                       pairwise_cancelled_profile, &state.identity_);
        if (!valid)
            throw valid.error();
        if (!exact(envelope->payload,
                   {"first_request_id", "phase", "reason", "second_request_id"}))
            throw Failure::invalid_payload;
        auto identity = payload_identity(envelope->payload);
        auto reason = parse_reason(envelope->payload["reason"]);
        auto phase = parse_phase(envelope->payload["phase"]);
        if (!identity || *identity != state.identity_)
            throw Failure::identity_mismatch;
        if (!reason || !phase)
            throw Failure::invalid_payload;
        if (*reason != *state.expected_cancellation_)
            throw Failure::identity_mismatch;
        state.expected_cancellation_.reset();
        state.phase_ = PairwiseV2EventReaderState::Phase::terminal;
        return PairwiseV2Cancelled{*identity, *reason, *phase};
    }).transform_error([](Failure failure) { return failure; });
}

} // namespace epycbench::runtime::detail
