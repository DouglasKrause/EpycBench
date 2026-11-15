#include "runtime/pairwise_v2_composed_startup.hpp"

#include "runtime/pairwise_v2_resource_startup_protocol.hpp"

#include <array>
#include <limits>
#include <utility>

namespace epycbench::runtime::detail {
auto stage_b_progress_configuration(progress::Limits limits,PairwiseV2AuthoritySource source) noexcept
    -> progress::Configuration {
    return {limits,progress::Durability::process,
        source==PairwiseV2AuthoritySource::native?progress::Platform::native:progress::Platform::injected,
        source==PairwiseV2AuthoritySource::native?progress::Utc::acquired:progress::Utc::supplied};
}
auto pairwise_v2_resource_identity_commitment(const PreparedEvidenceSessionIdentity& identity) noexcept
    -> core::Sha256Digest {
    const std::array digests{core::sha256(identity.startup_session.text()),
        identity.request_body_sha256, identity.capacity_offer_body_sha256,
        identity.capacity_body_sha256, identity.capacity_ack_body_sha256,
        identity.prepared_offer_body_sha256, identity.prepared_resource_sha256,
        identity.prepared_ack_body_sha256, identity.wire_registry_sha256,
        identity.source_manifest_sha256};
    std::array<std::byte, 16U + 32U * digests.size()> bytes{};
    std::size_t cursor{};
    for (const auto id : {identity.pair.first.value(), identity.pair.second.value()})
        for (unsigned shift = 64U; shift != 0U; shift -= 8U)
            bytes[cursor++] = std::byte{static_cast<unsigned char>(id >> (shift - 8U))};
    for (const auto& digest : digests)
        for (const auto byte : digest.bytes()) bytes[cursor++] = byte;
    return core::sha256(bytes);
}

auto pairwise_v2_offered_resource_commitment(
    std::uint64_t first, std::uint64_t second,
    const std::array<core::Sha256Digest, 4>& digests) noexcept -> core::Sha256Digest {
    constexpr char domain[] = "epycbench.pairwise-progress/offered-resources/v1";
    std::array<std::byte, sizeof(domain) + 144U> bytes{};
    std::size_t offset{};
    for (const char c : domain) bytes[offset++] = std::byte{static_cast<unsigned char>(c)};
    for (const auto id : {first, second})
        for (unsigned shift = 64U; shift != 0U; shift -= 8U)
            bytes[offset++] = std::byte{static_cast<unsigned char>(id >> (shift - 8U))};
    for (const auto& digest : digests)
        for (const auto byte : digest.bytes()) bytes[offset++] = byte;
    return core::sha256(bytes);
}

namespace {
using Failure = PairwiseV2ComposedStartupFailure;
using Host = PairwiseMonitoringHostCapacityStartupAccess;
using PhaseA = PairwiseV2PhaseAProtocolAccess;
namespace wire = wire_v1::detail;

auto bind_progress(const progress::Configuration& configuration,
    const progress::Identity& identity, const core::Sha256Digest& resource) noexcept
    -> std::expected<progress::Offer, Failure> {
    progress::Offer offer{identity, resource.bytes(), configuration.limits,
        configuration.durability, configuration.platform, configuration.utc};
    auto bound = progress::bind_offer(offer);
    if (!bound) return std::unexpected{Failure::identity};
    return *bound;
}

auto read_progress(ipc::ByteReader& input) noexcept
    -> std::expected<progress::Frame, Failure> {
    auto body = Host::read_frame(input);
    if (!body || !*body || (**body).empty() || (**body).size() > 4096U)
        return std::unexpected{Failure::identity};
    progress::EncodedFrame encoded;
    encoded.size = (**body).size() + 4U;
    for (unsigned i = 0; i < 4U; ++i)
        encoded.storage[i] = std::byte{static_cast<unsigned char>(
            (**body).size() >> ((3U - i) * 8U))};
    for (std::size_t i = 0; i < (**body).size(); ++i)
        encoded.storage[i + 4U] = std::byte{static_cast<unsigned char>((**body)[i])};
    auto decoded = progress::decode(encoded.bytes());
    if (!decoded) return std::unexpected{Failure::identity};
    return *decoded;
}

class Buffer final : public ipc::ByteWriter {
  public:
    std::vector<std::byte> bytes;
    std::size_t limit{std::numeric_limits<std::size_t>::max()};
    auto write(std::span<const std::byte> input)
        -> std::expected<std::size_t, core::Error> override {
        if (bytes.size() > limit || input.size() > limit - bytes.size())
            return std::unexpected{core::Error{}};
        bytes.insert(bytes.end(), input.begin(), input.end());
        return input.size();
    }
};

auto write_buffer(ipc::ByteWriter& output, std::span<const std::byte> bytes) noexcept
    -> std::expected<void, Failure> {
    try {
        while (!bytes.empty()) {
            auto written = output.write(bytes);
            if (!written) {
                const auto& error = written.error();
                if (error.code.value() == "runtime.ipc.io_failure" &&
                    error.context.is_object() &&
                    error.context.value("classification", "") == "interrupted")
                    continue;
                return std::unexpected{Failure::channel_write};
            }
            if (*written == 0U || *written > bytes.size())
                return std::unexpected{Failure::channel_write};
            bytes = bytes.subspan(*written);
        }
        return {};
    } catch (...) {
        return std::unexpected{Failure::channel_write};
    }
}
} // namespace

struct PairwiseV2ComposedStartupController::Storage final {
    Host::LowerAuthority authority;
    wire::AuthoredCapacityOffer capacity_offer;
    PairwiseV2PreparedControllerSession prepared;
    PairwiseV2ResourceStartupController resources;
    PairwiseV2ControlWriterState control;
    std::vector<std::byte> frames;
    std::vector<core::LinuxCpuId> authenticated_cpus;
    std::optional<progress::Offer> progress_offer;
    enum class State : unsigned char { prepared, written, terminal } state{};
};

PairwiseV2ComposedStartupController::PairwiseV2ComposedStartupController(
    std::unique_ptr<Storage> storage) noexcept : storage_{std::move(storage)} {}
PairwiseV2ComposedStartupController::PairwiseV2ComposedStartupController(
    PairwiseV2ComposedStartupController&&) noexcept = default;
PairwiseV2ComposedStartupController::~PairwiseV2ComposedStartupController() noexcept = default;

auto PairwiseV2ComposedStartupController::prepare(
    wire::StartupSessionId session, const PairwiseHostCapacityRequestTranscript& request,
    const RunnerContext& context, Host::LowerOperations operations,
    std::optional<core::Sha256Digest>* captured_host_offer,
    std::optional<progress::Configuration> progress_profile,
    stage_b_policy::Selection policy, PairwiseV2AuthoritySource authority_source) noexcept
    -> std::expected<PairwiseV2ComposedStartupController, Failure> {
    try {
        if(policy==stage_b_policy::Selection::stage_b_v1&&progress_profile&&
           *progress_profile!=stage_b_progress_configuration(progress_profile->limits,authority_source))
            return std::unexpected{Failure::preparation};
        if (policy == stage_b_policy::Selection::stage_b_v1 &&
            (!stage_b_policy::admit(request) ||
             (progress_profile && !stage_b_policy::admit(progress_profile->limits))))
            return std::unexpected{Failure::preparation};
        const auto session_digest = core::sha256(session.text());
        const auto progress_identity = progress::identity(session.text(),
            request.first_request_id().value(), request.second_request_id().value());
        if (progress_profile && !progress_identity)
            return std::unexpected{Failure::identity};
        auto authority = authority_source == PairwiseV2AuthoritySource::native
            ? Host::mint_controller(request, context)
            : Host::mint_controller_with(request, context, operations);
        if (!authority)
            return std::unexpected{Failure::authority};
        auto host_prepared = Host::prepare_offer(request, *authority);
        if (!host_prepared)
            return std::unexpected{Failure::host_offer};
        auto host_offer = Host::author_offer(std::move(*host_prepared));
        if (!host_offer)
            return std::unexpected{Failure::host_offer};
        auto capacity = wire::construct_controller_startup_capacity(
            std::move(session), request, context, *authority, stage_b_policy::capacity_limits(policy));
        if (!capacity)
            return std::unexpected{Failure::capacity_offer};
        if (policy == stage_b_policy::Selection::stage_b_v1) {
            auto local = stage_b_policy::derive_progress(request, capacity->projected().capacity);
            if (!local || (progress_profile && progress_profile->limits != local->limits))
                return std::unexpected{Failure::capacity_offer};
            if (!progress_profile) progress_profile = stage_b_progress_configuration(local->limits,authority_source);
        }
        if (progress_profile && !progress::validate_component_ceilings(
                progress_profile->limits, capacity->projected().capacity))
            return std::unexpected{Failure::capacity_offer};
        auto capacity_offer = wire::author_capacity_offer(std::move(*capacity));
        if (!capacity_offer)
            return std::unexpected{Failure::capacity_offer};
        if (policy == stage_b_policy::Selection::stage_b_v1) {
            // Reserve copied capacity, prepared offer/resource and one temporary
            // canonical destination before entering prepared-session construction.
            const std::array retained{std::uint64_t{request.canonical_body().size()},
                std::uint64_t{host_offer->canonical_body().size()},
                std::uint64_t{capacity_offer->canonical_body.size()}, 4U * stage_b_policy::frame};
            if (!stage_b_policy::admit_startup_retention(retained))
                return std::unexpected{Failure::preparation};
        }
        auto prepared = PairwiseV2PreparedControllerSession::prepare(*capacity_offer, request, policy);
        if (!prepared)
            return std::unexpected{Failure::prepared_offer};

        Buffer frames;
        if (policy == stage_b_policy::Selection::stage_b_v1) {
            const auto prepared_bodies = prepared->retained_startup_body_sizes();
            // Three transient body/frame slots cover the host offer, resource
            // envelope/canonical copy and framed writer buffer, including prefixes.
            const std::array retained{std::uint64_t{request.canonical_body().size()},
                std::uint64_t{capacity_offer->canonical_body.size()},
                prepared_bodies[0], prepared_bodies[1], prepared_bodies[2],
                3U * (stage_b_policy::frame + 4U)};
            if (!stage_b_policy::admit_startup_retention(retained))
                return std::unexpected{Failure::preparation};
            std::uint64_t used{};
            for (auto size : retained) used += size;
            frames.limit = static_cast<std::size_t>(stage_b_policy::startup - used);
            frames.bytes.reserve(frames.limit);
        }
        if (captured_host_offer)
            *captured_host_offer = host_offer->validation().body_sha256();
        auto control = PhaseA::write_startup_offer(frames, std::move(*host_offer));
        if (!control)
            return std::unexpected{Failure::host_offer};
        PairwiseV2ResourceStartupController resources{request.first_request_id()};
        if (!resources.write_offers(frames, capacity_offer->canonical_body,
                                    prepared->offer_body()))
            return std::unexpected{Failure::resource_offers};
        auto request_write = PhaseA::prepare_request_write(std::move(*control), request);
        if (!request_write)
            return std::unexpected{Failure::request};
        const auto request_begin = frames.bytes.size();
        auto requested = PhaseA::commit_control_write(frames, std::move(*request_write));
        if (!requested)
            return std::unexpected{Failure::request};
        std::optional<progress::Offer> selected_progress;
        if (progress_profile) {
            const auto frame = std::span<const std::byte>{frames.bytes}.subspan(request_begin);
            if (frame.size() < 4U) return std::unexpected{Failure::request};
            std::uint32_t length{};
            for (const auto byte : frame.first(4U))
                length = (length << 8U) | std::to_integer<std::uint32_t>(byte);
            if (static_cast<std::size_t>(length) != frame.size() - 4U)
                return std::unexpected{Failure::request};
            const auto commitment = pairwise_v2_offered_resource_commitment(
                request.first_request_id().value(), request.second_request_id().value(),
                {session_digest, core::sha256(frame.subspan(4U)),
                 core::sha256(capacity_offer->canonical_body), core::sha256(prepared->offer_body())});
            auto offer = bind_progress(*progress_profile, *progress_identity, commitment);
            if (!offer) return std::unexpected{offer.error()};
            auto encoded = progress::encode({progress::Message::offer, *offer, {}});
            if (!encoded || !frames.write(encoded->bytes()))
                return std::unexpected{Failure::preparation};
            selected_progress = *offer;
        }
        std::vector<core::LinuxCpuId> authenticated_cpus{
            authority->present_cpus().begin(), authority->present_cpus().end()};
        return PairwiseV2ComposedStartupController{std::make_unique<Storage>(Storage{
            std::move(*authority), std::move(*capacity_offer), std::move(*prepared),
            std::move(resources), std::move(*requested), std::move(frames.bytes),
            std::move(authenticated_cpus), std::move(selected_progress), Storage::State::prepared})};
    } catch (...) {
        return std::unexpected{Failure::preparation};
    }
}

auto PairwiseV2ComposedStartupController::prepared_capacity_offer() const noexcept
    -> const wire::AuthoredCapacityOffer* {
    return storage_ && storage_->state == Storage::State::prepared
        ? &storage_->capacity_offer : nullptr;
}

auto PairwiseV2ComposedStartupController::progress_before_ack() const noexcept -> const progress::Offer* {
    return storage_ && storage_->state == Storage::State::prepared && storage_->progress_offer
        ? &*storage_->progress_offer : nullptr;
}

auto PairwiseV2ComposedStartupController::write_control(ipc::ByteWriter& output) noexcept
    -> std::expected<void, Failure> {
    if (!storage_ || storage_->state != Storage::State::prepared)
        return std::unexpected{Failure::invalid_state};
    storage_->state = Storage::State::terminal;
    auto written = write_buffer(output, storage_->frames);
    if (!written)
        return written;
    storage_->state = Storage::State::written;
    return {};
}

auto PairwiseV2ComposedStartupController::read_acknowledgements(ipc::ByteReader& input) noexcept
    -> std::expected<PairwiseV2ControllerResourceOutcome, Failure> {
    if (!storage_ || storage_->state != Storage::State::written)
        return std::unexpected{Failure::invalid_state};
    // Remove the owner before parsing: every path, including exceptions, is
    // terminal and cannot accept a replacement or fallback transcript.
    auto storage = std::move(storage_);
    try {
        auto startup = PhaseA::read_startup_outcome(
            input, std::move(storage->control), storage->authority);
        if (!startup)
            return std::unexpected{Failure::host_ack};
        if (auto* rejected = std::get_if<PairwiseV2ControllerRejected>(&*startup))
            return PairwiseV2ControllerResourceOutcome{*rejected};
        auto& phase_a = std::get<PairwiseV2ControllerAcceptedHandoff>(*startup);
        auto capacity_body = storage->resources.read_capacity_ack(input);
        if (!capacity_body)
            return std::unexpected{Failure::resource_acks};
        auto capacity_ack = wire::parse_accepted_capacity_ack(
            *capacity_body, storage->capacity_offer);
        if (!capacity_ack)
            return std::unexpected{Failure::capacity_ack};
        auto prepared_body = storage->resources.read_prepared_ack(input);
        if (!prepared_body)
            return std::unexpected{Failure::resource_acks};
        auto binding = storage->prepared.accept_ack(*capacity_ack, *prepared_body);
        if (!binding)
            return std::unexpected{Failure::prepared_ack};
        if (storage->progress_offer) {
            const auto echo = read_progress(input);
            if (!echo || echo->message != progress::Message::echo ||
                echo->offer.identity != storage->progress_offer->identity ||
                echo->offer.resource != storage->progress_offer->resource)
                return std::unexpected{Failure::identity};
        }
        auto accepted_body = Host::read_frame(input);
        if (!accepted_body || !*accepted_body ||
            !PhaseA::read_accepted(phase_a.event, **accepted_body))
            return std::unexpected{Failure::pairwise_accepted};
        auto role = binding->take_controller();
        if (!role)
            return std::unexpected{Failure::prepared_ack};
        return PairwiseV2ControllerResourceOutcome{PairwiseV2ControllerResources{
            std::move(phase_a), std::move(*role), std::move(storage->authenticated_cpus),
            std::move(storage->progress_offer)}};
    } catch (...) {
        return std::unexpected{Failure::preparation};
    }
}

struct PairwiseV2ComposedStartupRunner::Storage final {
    struct Ready final {
        PairwiseV2ControlReaderState control;
        PairwiseV2EventWriterState events;
        PairwiseV2AdmittedStartup admission;
        PreparedEvidenceSessionBinding binding;
        std::optional<progress::Offer> progress_offer;
        std::optional<PreparationGraphLimits> production_graph;
    };
    std::variant<Ready, PairwiseV2ControllerRejected> outcome;
    std::vector<std::byte> frames;
};

PairwiseV2ComposedStartupRunner::PairwiseV2ComposedStartupRunner(
    std::unique_ptr<Storage> storage) noexcept : storage_{std::move(storage)} {}
PairwiseV2ComposedStartupRunner::PairwiseV2ComposedStartupRunner(
    PairwiseV2ComposedStartupRunner&&) noexcept = default;
PairwiseV2ComposedStartupRunner::~PairwiseV2ComposedStartupRunner() noexcept = default;

auto PairwiseV2ComposedStartupRunner::reject(HostCapacityRejectedAckCapability capability) noexcept
    -> std::expected<PairwiseV2ComposedStartupRunner, Failure> {
    try {
        auto body = Host::encode_rejected_ack(std::move(capability));
        if (!body)
            return std::unexpected{Failure::host_ack};
        auto parsed = Host::parse_rejected_ack(*body);
        if (!parsed)
            return std::unexpected{Failure::host_ack};
        Buffer frames;
        if (!Host::write_frame(frames, *body))
            return std::unexpected{Failure::host_ack};
        return PairwiseV2ComposedStartupRunner{std::make_unique<Storage>(Storage{
            PairwiseV2ControllerRejected{{parsed->first, parsed->second}, parsed->reason},
            std::move(frames.bytes)})};
    } catch (...) {
        return std::unexpected{Failure::preparation};
    }
}

auto PairwiseV2ComposedStartupRunner::admit(
    ipc::ByteReader& input, wire::StartupSessionId session, ipc::RequestId first_request,
    RunnerContext context, Host::LowerOperations operations,
    std::optional<progress::Configuration> progress_profile,
    stage_b_policy::Selection policy, PairwiseV2AuthoritySource authority_source) noexcept
    -> std::expected<PairwiseV2ComposedStartupRunner, Failure> {
    return admit_impl(input, std::move(session), first_request, std::move(context),
                      operations, progress_profile, policy, authority_source);
}

auto PairwiseV2ComposedStartupRunner::admit_from_channel(
    ipc::ByteReader& input, RunnerContext context, Host::LowerOperations operations,
    std::optional<progress::Configuration> progress_profile, PairwiseV2AuthoritySource authority_source) noexcept
    -> std::expected<PairwiseV2ComposedStartupRunner, Failure> {
    return admit_impl(input, std::nullopt, std::nullopt, std::move(context), operations,
                      progress_profile, stage_b_policy::Selection::stage_b_v1, authority_source);
}

auto PairwiseV2ComposedStartupRunner::admit_impl(
    ipc::ByteReader& input, std::optional<wire::StartupSessionId> session,
    std::optional<ipc::RequestId> first_request, RunnerContext context,
    Host::LowerOperations operations, std::optional<progress::Configuration> progress_profile,
    stage_b_policy::Selection policy, PairwiseV2AuthoritySource authority_source) noexcept
    -> std::expected<PairwiseV2ComposedStartupRunner, Failure> {
    try {
        if(policy==stage_b_policy::Selection::stage_b_v1&&progress_profile&&
           *progress_profile!=stage_b_progress_configuration(progress_profile->limits,authority_source))
            return std::unexpected{Failure::preparation};
        auto control = PhaseA::read_startup_offer(input, policy);
        if (!control)
            return std::unexpected{Failure::host_offer};
        const auto provisional = PhaseA::provisional_identity(*control);
        if (!provisional || (first_request && *first_request != provisional->first))
            return std::unexpected{Failure::identity};
        if (!first_request) first_request = provisional->first;
        PairwiseV2ResourceStartupRunner resources{*first_request};
        auto offers = resources.read_offers(input, policy);
        if (!offers)
            return std::unexpected{Failure::resource_offers};
        if (!session) {
            auto facts = wire::parse_capacity_offer_facts(offers->capacity,
                {stage_b_policy::frame, stage_b_policy::boot_nodes, stage_b_policy::boot_depth});
            if (!facts || facts->request_id != *first_request)
                return std::unexpected{Failure::identity};
            session.emplace(std::move(facts->session_id));
        }
        const auto session_digest = core::sha256(session->text());
        const auto session_text = std::string{session->text()};
        auto request_body = Host::read_frame(input);
        if (!request_body || !*request_body)
            return std::unexpected{Failure::request};
        if (policy == stage_b_policy::Selection::stage_b_v1 &&
            !stage_b_policy::admit_bootstrap(**request_body))
            return std::unexpected{Failure::request};
        const auto request_digest = core::sha256(**request_body);
        const auto capacity_digest = core::sha256(offers->capacity);
        const auto prepared_digest = core::sha256(offers->prepared);
        auto request = PhaseA::read_request(*control, **request_body);
        if (!request)
            return std::unexpected{Failure::request};
        if (request->identity().first != *first_request)
            return std::unexpected{Failure::identity};
        auto inputs = std::move(*request).consume();
        if (!inputs)
            return std::unexpected{Failure::request};
        // The admitted transcript now owns the exact canonical request. Preserve
        // its digest and retire the raw frame before further canonical copies.
        request_body->reset();
        if (policy == stage_b_policy::Selection::stage_b_v1 &&
            (!stage_b_policy::admit(inputs->request.first()) ||
             !stage_b_policy::admit(inputs->request.second()) ||
             (progress_profile && !stage_b_policy::admit(progress_profile->limits))))
            return std::unexpected{Failure::preparation};
        auto authority = authority_source == PairwiseV2AuthoritySource::native
            ? Host::mint_runner(inputs->commitment, inputs->request, context)
            : Host::mint_runner_with(inputs->commitment, inputs->request, context, operations);
        if (!authority)
            return reject(std::move(authority.error()));
        auto proof = Host::gate_a(inputs->commitment, inputs->request, *authority);
        if (!proof)
            return reject(std::move(proof.error()));
        auto contextual = PairwiseV2ContextualAdmissionAccess::admit_after_gate_a(
            std::move(inputs->request), std::move(context), std::move(*proof),
            std::move(*authority));
        if (!contextual)
            return reject(PairwiseV2ContextualAdmissionAccess::reject(
                std::move(contextual.error())));
        auto admitted = PairwiseV2ContextualAdmissionAccess::gate_b_with_inputs(
            std::move(*contextual));
        if (!admitted)
            return reject(std::move(admitted.error()));
        const auto progress_identity = progress::identity(session_text,
            admitted->request().first_request_id().value(),
            admitted->request().second_request_id().value());
        if (progress_profile && !progress_identity)
            return std::unexpected{Failure::identity};
        auto capacity = wire::construct_runner_startup_capacity(
            std::move(*session), admitted->request(), admitted->inputs(),
            admitted->startup().admission(), stage_b_policy::capacity_limits(policy));
        if (!capacity)
            return std::unexpected{Failure::capacity_offer};
        std::optional<PreparationGraphLimits> production_graph;
        if (policy == stage_b_policy::Selection::stage_b_v1) {
            auto local = stage_b_policy::derive_progress(admitted->request(), capacity->projected().capacity);
            if (!local || (progress_profile && progress_profile->limits != local->limits))
                return std::unexpected{Failure::capacity_offer};
            production_graph = local->graph;
            if (!progress_profile) progress_profile = stage_b_progress_configuration(local->limits,authority_source);
        }
        if (progress_profile && !progress::validate_component_ceilings(
                progress_profile->limits, capacity->projected().capacity))
            return std::unexpected{Failure::capacity_offer};
        if (policy == stage_b_policy::Selection::stage_b_v1) {
            const std::array retained{std::uint64_t{offers->capacity.size()},
                std::uint64_t{offers->prepared.size()},
                std::uint64_t{admitted->request().canonical_body().size()},
                3U * stage_b_policy::frame};
            if (!stage_b_policy::admit_startup_retention(retained))
                return std::unexpected{Failure::preparation};
        }
        auto capacity_offer = wire::parse_capacity_offer(offers->capacity, *capacity);
        if (!capacity_offer)
            return std::unexpected{Failure::capacity_offer};
        auto capacity_ack = wire::accept_capacity_offer(*capacity_offer, *capacity);
        if (!capacity_ack)
            return std::unexpected{Failure::capacity_offer};
        if (policy == stage_b_policy::Selection::stage_b_v1) {
            const std::array retained{std::uint64_t{offers->capacity.size()},
                std::uint64_t{offers->prepared.size()},
                std::uint64_t{admitted->request().canonical_body().size()},
                std::uint64_t{capacity_offer->canonical_body.size()},
                std::uint64_t{capacity_offer->canonical_capacity_body.size()},
                std::uint64_t{capacity_ack->canonical_body.size()}, 3U * stage_b_policy::frame};
            if (!stage_b_policy::admit_startup_retention(retained))
                return std::unexpected{Failure::preparation};
        }
        auto prepared = PairwiseV2PreparedRunnerSession::accept(
            offers->prepared, *capacity, *capacity_offer, *capacity_ack, admitted->request(), policy);
        if (!prepared)
            return std::unexpected{Failure::prepared_offer};
        std::string{}.swap(offers->capacity);
        std::string{}.swap(offers->prepared);
        std::string{}.swap(capacity_offer->canonical_body);
        std::string{}.swap(capacity_offer->canonical_capacity_body);
        std::optional<progress::Offer> selected_progress;
        if (progress_profile) {
            const auto commitment = pairwise_v2_offered_resource_commitment(
                admitted->request().first_request_id().value(),
                admitted->request().second_request_id().value(),
                {session_digest, request_digest, capacity_digest, prepared_digest});
            const auto expected = bind_progress(*progress_profile, *progress_identity, commitment);
            const auto offer = read_progress(input);
            if (!expected || !offer || offer->message != progress::Message::offer ||
                offer->offer != *expected)
                return std::unexpected{Failure::identity};
            selected_progress = *expected;
        }
        Buffer frames;
        if (policy == stage_b_policy::Selection::stage_b_v1) {
            const auto prepared_bodies = prepared->retained_startup_body_sizes();
            const std::array retained{std::uint64_t{admitted->request().canonical_body().size()},
                std::uint64_t{capacity_ack->canonical_body.size()},
                prepared_bodies[0], prepared_bodies[1], 3U * (stage_b_policy::frame + 4U)};
            if (!stage_b_policy::admit_startup_retention(retained))
                return std::unexpected{Failure::preparation};
            std::uint64_t used{};
            for (auto size : retained) used += size;
            frames.limit = static_cast<std::size_t>(stage_b_policy::startup - used);
            frames.bytes.reserve(frames.limit);
        }
        auto host_ack = Host::issue_accepted_ack(admitted->startup());
        if (!host_ack)
            return std::unexpected{Failure::host_ack};
        auto events = PhaseA::write_startup_accepted_ack(frames, std::move(*host_ack));
        if (!events)
            return std::unexpected{Failure::host_ack};
        if (!resources.write_acks(frames, capacity_ack->canonical_body, prepared->ack_body()))
            return std::unexpected{Failure::resource_acks};
        auto binding = prepared->complete();
        if (!binding)
            return std::unexpected{Failure::prepared_offer};
        if (selected_progress) {
            const auto echo = progress::encode({progress::Message::echo, *selected_progress, {}});
            if (!echo || !frames.write(echo->bytes()))
                return std::unexpected{Failure::preparation};
        }
        auto accept_write = PhaseA::prepare_accepted_write(std::move(*events));
        if (!accept_write)
            return std::unexpected{Failure::pairwise_accepted};
        auto accepted = PhaseA::commit_event_write(frames, std::move(*accept_write));
        if (!accepted)
            return std::unexpected{Failure::pairwise_accepted};
        return PairwiseV2ComposedStartupRunner{std::make_unique<Storage>(Storage{
            Storage::Ready{std::move(*control), std::move(*accepted), std::move(*admitted),
                           std::move(*binding), std::move(selected_progress), production_graph}, std::move(frames.bytes)})};
    } catch (...) {
        return std::unexpected{Failure::preparation};
    }
}

auto PairwiseV2ComposedStartupRunner::production_graph_limits() const noexcept
    -> const PreparationGraphLimits* {
    if (!storage_) return nullptr;
    const auto* ready = std::get_if<Storage::Ready>(&storage_->outcome);
    return ready && ready->production_graph ? &*ready->production_graph : nullptr;
}

auto PairwiseV2ComposedStartupRunner::admitted_before_ack() const noexcept
    -> const PairwiseV2AdmittedStartup* {
    if (!storage_) return nullptr;
    const auto* ready = std::get_if<Storage::Ready>(&storage_->outcome);
    return ready ? &ready->admission : nullptr;
}

auto PairwiseV2ComposedStartupRunner::progress_before_ack() const noexcept -> const progress::Offer* {
    if (!storage_) return nullptr;
    const auto* ready = std::get_if<Storage::Ready>(&storage_->outcome);
    return ready && ready->progress_offer ? &*ready->progress_offer : nullptr;
}

auto PairwiseV2ComposedStartupRunner::write_acknowledgements(ipc::ByteWriter& output) noexcept
    -> std::expected<PairwiseV2RunnerResourceOutcome, Failure> {
    if (!storage_)
        return std::unexpected{Failure::invalid_state};
    auto storage = std::move(storage_);
    auto written = write_buffer(output, storage->frames);
    if (!written)
        return std::unexpected{written.error()};
    if (auto* rejected = std::get_if<PairwiseV2ControllerRejected>(&storage->outcome))
        return PairwiseV2RunnerResourceOutcome{*rejected};
    auto& ready = std::get<Storage::Ready>(storage->outcome);
    auto role = ready.binding.take_runner();
    if (!role)
        return std::unexpected{Failure::prepared_ack};
    return PairwiseV2RunnerResourceOutcome{PairwiseV2RunnerResources{
        std::move(ready.control), std::move(ready.events), std::move(ready.admission),
        std::move(*role), std::move(ready.progress_offer)}};
}

} // namespace epycbench::runtime::detail
