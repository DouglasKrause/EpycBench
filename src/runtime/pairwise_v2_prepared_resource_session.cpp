#include "runtime/pairwise_v2_prepared_resource_session.hpp"

#include "results/prepared_pairwise_run_result.hpp"
#include "runtime/finalized_evidence_prepared_direct_codec.hpp"

#include <limits>
#include <utility>
#include <variant>

namespace epycbench::runtime::detail {
namespace {

using Failure = PairwiseV2PreparedSessionFailure;
using ResourceError = prepared_resources_v1::PreparedResourceErrorCode;

auto map(ResourceError code, Failure mismatch) noexcept -> Failure {
    switch (code) {
    case ResourceError::allocation_failure:
        return Failure::allocation_failure;
    case ResourceError::arithmetic_overflow:
    case ResourceError::size_unrepresentable:
        return Failure::arithmetic_overflow;
    case ResourceError::digest_mismatch:
        return Failure::request_binding_mismatch;
    default:
        return mismatch;
    }
}

auto plan(const prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<FinalizedEvidenceTransportPlan, Failure> {
    const auto bytes = resource.stocks.logical_bytes;
    if (bytes == 0U)
        return std::unexpected{Failure::arithmetic_overflow};
    const auto quotient = bytes / finalized_evidence_raw_chunk_bytes;
    const auto remainder = bytes % finalized_evidence_raw_chunk_bytes;
    if (quotient == std::numeric_limits<std::uint64_t>::max() && remainder != 0U)
        return std::unexpected{Failure::arithmetic_overflow};
    return FinalizedEvidenceTransportPlan{bytes, quotient + (remainder != 0U ? 1U : 0U)};
}

auto capacity_ack_matches(const wire_v1::detail::AcceptedCapacityAck& ack,
                          const wire_v1::detail::AuthoredCapacityOffer& offer) noexcept
    -> std::expected<void, Failure> {
    auto decoded = wire_v1::detail::parse_accepted_capacity_ack(ack.canonical_body, offer);
    if (!decoded) {
        if (decoded.error().code == wire_v1::detail::StartupCodecErrorCode::allocation_failure)
            return std::unexpected{Failure::allocation_failure};
        return std::unexpected{Failure::capacity_ack_mismatch};
    }
    if (!(decoded->session_id == ack.session_id && decoded->request_id == ack.request_id &&
          decoded->request_body_sha256 == ack.request_body_sha256 &&
          decoded->offer_body_sha256 == ack.offer_body_sha256 &&
          decoded->capacity_body_sha256 == ack.capacity_body_sha256 &&
          decoded->body_sha256 == ack.body_sha256))
        return std::unexpected{Failure::capacity_ack_mismatch};
    return {};
}

auto capacity_offer_valid(const wire_v1::detail::AuthoredCapacityOffer& offer) noexcept
    -> std::expected<void, Failure> {
    auto expected = wire_v1::detail::author_capacity_offer(offer.capacity);
    if (!expected) {
        if (expected.error().code == wire_v1::detail::StartupCodecErrorCode::allocation_failure)
            return std::unexpected{Failure::allocation_failure};
        return std::unexpected{Failure::capacity_offer_mismatch};
    }
    if (!(expected->canonical_body == offer.canonical_body &&
          expected->body_sha256 == offer.body_sha256 &&
          expected->capacity_body_sha256 == offer.capacity_body_sha256))
        return std::unexpected{Failure::capacity_offer_mismatch};
    return {};
}

auto runner_capacity_ack_matches(const wire_v1::detail::AcceptedCapacityAck& ack,
                                 const wire_v1::detail::ParsedCapacityOffer& offer,
                                 const wire_v1::detail::StartupCapacityValue& local) noexcept
    -> std::expected<void, Failure> {
    auto expected = wire_v1::detail::accept_capacity_offer(offer, local);
    if (!expected) {
        if (expected.error().code == wire_v1::detail::StartupCodecErrorCode::allocation_failure)
            return std::unexpected{Failure::allocation_failure};
        return std::unexpected{Failure::capacity_ack_mismatch};
    }
    if (!(expected->canonical_body == ack.canonical_body &&
          expected->body_sha256 == ack.body_sha256 && expected->session_id == ack.session_id &&
          expected->request_id == ack.request_id &&
          expected->request_body_sha256 == ack.request_body_sha256 &&
          expected->offer_body_sha256 == ack.offer_body_sha256 &&
          expected->capacity_body_sha256 == ack.capacity_body_sha256))
        return std::unexpected{Failure::capacity_ack_mismatch};
    return {};
}

auto identity(const wire_v1::detail::StartupSessionId& session,
              FinalizedEvidenceTransportIdentity pair, core::Sha256Digest request_sha,
              core::Sha256Digest capacity_offer_sha, core::Sha256Digest capacity_body_sha,
              core::Sha256Digest capacity_ack_sha, core::Sha256Digest prepared_offer_sha,
              core::Sha256Digest prepared_resource_sha, core::Sha256Digest prepared_ack_sha,
              const prepared_resources_v1::PreparedResourceValue& resource)
    -> PreparedEvidenceSessionIdentity {
    return {session,
            pair,
            request_sha,
            capacity_offer_sha,
            capacity_body_sha,
            capacity_ack_sha,
            prepared_offer_sha,
            prepared_resource_sha,
            prepared_ack_sha,
            resource.wire_registry_sha256,
            resource.source_manifest_sha256};
}

} // namespace

struct PreparedEvidenceSessionBinding::Storage final {
    struct Runner final {
        prepared_codec_v1::PreparedCanonicalWriter writer;
        PreparedFinalizedEvidenceSender sender;
    };
    struct Controller final {
        results::detail::PreparedPairwiseRunResultProjector projector;
        prepared_codec_v1::PreparedFinalizedEvidenceReconstruction reconstruction;
        PreparedFinalizedEvidenceReceiver receiver;
    };

    Storage(prepared_resources_v1::PreparedResourceValue value, Runner role) noexcept
        : resource{std::move(value)}, prepared{std::move(role)} {}
    Storage(prepared_resources_v1::PreparedResourceValue value, Controller role) noexcept
        : resource{std::move(value)}, prepared{std::move(role)} {}

    prepared_resources_v1::PreparedResourceValue resource;
    std::variant<Runner, Controller> prepared;
};

PreparedEvidenceSessionBinding::PreparedEvidenceSessionBinding(
    PreparedEvidenceSessionIdentity identity, std::unique_ptr<Storage> storage,
    FinalizedEvidenceTransportPlan plan) noexcept
    : identity_{std::move(identity)}, storage_{std::move(storage)}, plan_{plan} {}
PreparedEvidenceSessionBinding::PreparedEvidenceSessionBinding(
    PreparedEvidenceSessionBinding&&) noexcept = default;
PreparedEvidenceSessionBinding::~PreparedEvidenceSessionBinding() noexcept = default;

auto PreparedEvidenceSessionBinding::resource() const noexcept
    -> const prepared_resources_v1::PreparedResourceValue* {
    return storage_ ? &storage_->resource : nullptr;
}

auto PreparedEvidenceSessionBinding::take_runner() noexcept
    -> std::expected<PreparedRunnerEvidenceSession, Failure> {
    if (!storage_)
        return std::unexpected{Failure::invalid_state};
    auto* runner = std::get_if<Storage::Runner>(&storage_->prepared);
    if (!runner)
        return std::unexpected{Failure::invalid_state};
    auto storage = std::move(storage_);
    return PreparedRunnerEvidenceSession{std::move(identity_), std::move(storage->resource),
                                         std::move(runner->writer), std::move(runner->sender)};
}

auto PreparedEvidenceSessionBinding::take_controller() noexcept
    -> std::expected<PreparedControllerEvidenceSession, Failure> {
    if (!storage_)
        return std::unexpected{Failure::invalid_state};
    auto* controller = std::get_if<Storage::Controller>(&storage_->prepared);
    if (!controller)
        return std::unexpected{Failure::invalid_state};
    auto storage = std::move(storage_);
    return PreparedControllerEvidenceSession{std::move(identity_), std::move(storage->resource),
                                             std::move(controller->projector),
                                             std::move(controller->reconstruction),
                                             std::move(controller->receiver)};
}

auto PairwiseV2PreparedControllerSession::prepare(
    const wire_v1::detail::AuthoredCapacityOffer& capacity_offer,
    const PairwiseHostCapacityRequestTranscript& request,
    stage_b_policy::Selection policy) noexcept
    -> std::expected<PairwiseV2PreparedControllerSession, Failure> {
    auto capacity_valid = capacity_offer_valid(capacity_offer);
    if (!capacity_valid)
        return std::unexpected{capacity_valid.error()};
    if (capacity_offer.capacity.request_id() != request.first_request_id() ||
        capacity_offer.capacity.projected().request_body_sha256 != request.body_sha256())
        return std::unexpected{Failure::request_binding_mismatch};
    auto offer = prepared_resources_v1::author_prepared_offer(capacity_offer, request);
    if (!offer)
        return std::unexpected{map(offer.error().code, Failure::prepared_offer_mismatch)};
    if (policy == stage_b_policy::Selection::stage_b_v1 &&
        (!stage_b_policy::admit(offer->resource) ||
         offer->canonical_body.size() > stage_b_policy::frame ||
         offer->canonical_resource_body.size() > stage_b_policy::frame))
        return std::unexpected{Failure::resource_policy};
    const FinalizedEvidenceTransportIdentity pair{request.first_request_id(),
                                                  request.second_request_id()};
    auto transport = plan(offer->resource);
    if (!transport)
        return std::unexpected{transport.error()};
    auto projector = results::detail::PreparedPairwiseRunResultProjector::prepare_owned(offer->resource);
    if (!projector)
        return std::unexpected{map(projector.error().code, Failure::prepared_offer_mismatch)};
    auto reconstruction =
        prepared_codec_v1::PreparedFinalizedEvidenceReconstruction::prepare(
            offer->resource, capacity_offer.capacity.projected().capacity, request);
    if (!reconstruction)
        return std::unexpected{map(reconstruction.error().code,
                                   Failure::prepared_offer_mismatch)};
    auto receiver = PreparedFinalizedEvidenceReceiver::prepare(*transport, pair);
    if (!receiver)
        return std::unexpected{receiver.error() == FinalizedEvidenceTransportFailure::allocation
                                   ? Failure::allocation_failure
                                   : Failure::prepared_offer_mismatch};
    try {
        auto storage = std::make_unique<PreparedEvidenceSessionBinding::Storage>(
            offer->resource, PreparedEvidenceSessionBinding::Storage::Controller{
                                 std::move(*projector), std::move(*reconstruction),
                                 std::move(*receiver)});
        auto bound_identity = identity(
            offer->session_id, pair, offer->request_body_sha256, offer->capacity_offer_body_sha256,
            capacity_offer.capacity_body_sha256, core::sha256(std::string_view{}),
            offer->body_sha256, offer->prepared_resource_sha256,
            core::sha256(std::string_view{}), offer->resource);
        PreparedEvidenceSessionBinding binding{std::move(bound_identity), std::move(storage),
                                               *transport};
        return PairwiseV2PreparedControllerSession{capacity_offer, std::move(*offer),
                                                   std::move(binding), pair};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation_failure};
    } catch (...) {
        return std::unexpected{Failure::prepared_offer_mismatch};
    }
}

auto PairwiseV2PreparedControllerSession::offer_body() const noexcept -> std::string_view {
    return completed_ ? std::string_view{} : std::string_view{offer_.canonical_body};
}

auto PairwiseV2PreparedControllerSession::retained_startup_body_sizes() const noexcept
    -> std::array<std::uint64_t, 3> {
    return {capacity_offer_.canonical_body.size(), offer_.canonical_resource_body.size(),
        offer_.canonical_body.size()};
}

auto PairwiseV2PreparedControllerSession::accept_ack(
    const wire_v1::detail::AcceptedCapacityAck& capacity_ack,
    std::string_view bytes) noexcept
    -> std::expected<PreparedEvidenceSessionBinding, Failure> {
    if (completed_)
        return std::unexpected{Failure::invalid_state};
    completed_ = true;
    auto capacity_bound = capacity_ack_matches(capacity_ack, capacity_offer_);
    if (!capacity_bound)
        return std::unexpected{capacity_bound.error()};
    auto accepted = prepared_resources_v1::parse_prepared_ack(bytes, offer_);
    if (!accepted)
        return std::unexpected{map(accepted.error().code, Failure::prepared_ack_mismatch)};
    binding_.identity_.prepared_ack_body_sha256 = accepted->body_sha256;
    binding_.identity_.capacity_ack_body_sha256 = capacity_ack.body_sha256;
    return std::move(binding_);
}

auto PairwiseV2PreparedRunnerSession::accept(
    std::string_view prepared_offer_body,
    const wire_v1::detail::StartupCapacityValue& runner_capacity,
    const wire_v1::detail::ParsedCapacityOffer& capacity_offer,
    const wire_v1::detail::AcceptedCapacityAck& capacity_ack,
    const PairwiseHostCapacityRequestTranscript& request,
    stage_b_policy::Selection policy) noexcept
    -> std::expected<PairwiseV2PreparedRunnerSession, Failure> {
    // Precedence is deliberate: the accepted capacity transcript is proven
    // before request or prepared-resource material is examined.
    auto capacity_bound =
        runner_capacity_ack_matches(capacity_ack, capacity_offer, runner_capacity);
    if (!capacity_bound)
        return std::unexpected{capacity_bound.error()};
    if (runner_capacity.request_id() != request.first_request_id() ||
        runner_capacity.projected().request_body_sha256 != request.body_sha256())
        return std::unexpected{Failure::request_binding_mismatch};
    auto offer = prepared_resources_v1::parse_prepared_offer(prepared_offer_body, runner_capacity,
                                                             request, capacity_offer.body_sha256);
    if (!offer)
        return std::unexpected{map(offer.error().code, Failure::prepared_offer_mismatch)};
    if (policy == stage_b_policy::Selection::stage_b_v1 && !stage_b_policy::admit(offer->resource))
        return std::unexpected{Failure::resource_policy};
    auto ack = prepared_resources_v1::accept_prepared_offer(*offer);
    if (!ack)
        return std::unexpected{map(ack.error().code, Failure::prepared_ack_mismatch)};
    auto owner = prepared_resources_v1::PreparedResourceOwner::create(offer->resource);
    if (!owner)
        return std::unexpected{map(owner.error().code, Failure::prepared_offer_mismatch)};
    const FinalizedEvidenceTransportIdentity pair{request.first_request_id(),
                                                  request.second_request_id()};
    auto transport = plan(offer->resource);
    if (!transport)
        return std::unexpected{transport.error()};
    auto writer = prepared_codec_v1::PreparedCanonicalWriter::prepare(std::move(*owner));
    if (!writer)
        return std::unexpected{map(writer.error().code, Failure::prepared_offer_mismatch)};
    auto sender = PreparedFinalizedEvidenceSender::prepare(*transport, pair);
    if (!sender)
        return std::unexpected{sender.error() == FinalizedEvidenceTransportFailure::allocation
                                   ? Failure::allocation_failure
                                   : Failure::prepared_offer_mismatch};
    try {
        auto storage = std::make_unique<PreparedEvidenceSessionBinding::Storage>(
            offer->resource, PreparedEvidenceSessionBinding::Storage::Runner{std::move(*writer),
                                                                             std::move(*sender)});
        auto bound_identity = identity(
            offer->session_id, pair, offer->request_body_sha256, offer->capacity_offer_body_sha256,
            capacity_offer.capacity_body_sha256, capacity_ack.body_sha256, offer->body_sha256,
            offer->prepared_resource_sha256, ack->body_sha256, offer->resource);
        PreparedEvidenceSessionBinding binding{std::move(bound_identity), std::move(storage),
                                               *transport};
        return PairwiseV2PreparedRunnerSession{std::move(*offer), std::move(*ack),
                                               std::move(binding), pair};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation_failure};
    } catch (...) {
        return std::unexpected{Failure::prepared_offer_mismatch};
    }
}

auto PairwiseV2PreparedRunnerSession::ack_body() const noexcept -> std::string_view {
    return completed_ ? std::string_view{} : std::string_view{ack_.canonical_body};
}

auto PairwiseV2PreparedRunnerSession::retained_startup_body_sizes() const noexcept
    -> std::array<std::uint64_t, 2> {
    return {offer_.canonical_body.size(), ack_.canonical_body.size()};
}

auto PairwiseV2PreparedRunnerSession::complete() noexcept
    -> std::expected<PreparedEvidenceSessionBinding, Failure> {
    if (completed_)
        return std::unexpected{Failure::invalid_state};
    completed_ = true;
    return std::move(binding_);
}

} // namespace epycbench::runtime::detail
