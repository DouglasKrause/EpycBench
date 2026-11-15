#include "runtime/pairwise_v2_active_evidence.hpp"
#include "runtime/pairwise_prepared_runner_execution.hpp"

namespace epycbench::runtime::detail {
namespace {
using Failure = PairwiseV2ActiveEvidenceFailure;
using Stage = PairwiseV2ActiveEvidenceStage;
auto fail(Stage stage) noexcept -> std::unexpected<Failure> {
    return std::unexpected{Failure{stage, std::nullopt, std::nullopt}};
}
auto fail(Stage stage, FinalizedEvidenceTransportFailure error) noexcept
    -> std::unexpected<Failure> {
    return std::unexpected{Failure{stage, error, std::nullopt}};
}
auto fail(Stage stage, prepared_codec_v1::ActiveCodecFailure error) noexcept
    -> std::unexpected<Failure> {
    return std::unexpected{Failure{stage, std::nullopt, error}};
}
} // namespace

PairwiseV2ActiveEvidenceRunner::PairwiseV2ActiveEvidenceRunner(
    PairwiseV2RunnerResources resources, PairwiseV2PreparedEvidenceRunner stream) noexcept
    : resources_{std::move(resources)}, stream_{std::move(stream)} {}
PairwiseV2ActiveEvidenceRunner::PairwiseV2ActiveEvidenceRunner(
    PairwiseV2ActiveEvidenceRunner&& other) noexcept
    : resources_{std::move(other.resources_)}, stream_{std::move(other.stream_)},
      pending_{std::move(other.pending_)}, retain_production_{std::exchange(other.retain_production_, false)},
      phase_{std::exchange(other.phase_, Phase::terminal)} {}

auto PairwiseV2ActiveEvidenceRunner::bind(PairwiseV2RunnerResources resources) noexcept
    -> std::expected<PairwiseV2ActiveEvidenceRunner, Failure> {
    auto stream = PairwiseV2PreparedEvidenceSessionAccess::adopt_runner(
        std::move(resources.events), resources.evidence);
    if (!stream)
        return fail(Stage::ownership, stream.error());
    const auto& identity = resources.evidence.identity();
    if (pairwise_v2_resource_identity_commitment(identity) != resources.authenticated_identity_ ||
        !resources.admission.matches_request(identity.pair.first, identity.pair.second,
                                              identity.request_body_sha256))
        return fail(Stage::ownership, FinalizedEvidenceTransportFailure::identity_mismatch);
    return PairwiseV2ActiveEvidenceRunner{std::move(resources), std::move(*stream)};
}

auto PairwiseV2ActiveEvidenceRunner::start(PairwiseV2FakeEvidenceBackend backend) noexcept
    -> std::expected<void, Failure> {
    auto produced = produce(backend);
    if (!produced) return produced;
    return encode();
}
auto PairwiseV2ActiveEvidenceRunner::produce(PairwiseV2FakeEvidenceBackend backend) noexcept
    -> std::expected<void, Failure> {
    if (phase_ != Phase::prepared)
        return fail(Stage::ownership);
    phase_ = Phase::terminal;
    try {
        if (!backend.produce) return fail(Stage::backend);
        auto evidence = backend.produce(backend.context, resources_.admission);
        if (!evidence) return fail(Stage::backend);
        pending_.emplace(std::move(*evidence));
        phase_ = Phase::produced;
        return {};
    } catch (...) { return fail(Stage::backend); }
}
auto PairwiseV2ActiveEvidenceRunner::produce(PreparedPairwiseRunnerExecution& producer) noexcept
    -> std::expected<void, Failure> {
    if (phase_ != Phase::prepared) return fail(Stage::ownership);
    phase_ = Phase::terminal;
    if (!producer.matches(resources_.admission.inputs())) return fail(Stage::ownership);
    auto evidence = producer.take_finalized_evidence();
    if (!evidence) return fail(Stage::backend);
    pending_.emplace(std::move(*evidence));
    retain_production_ = true;
    phase_ = Phase::produced;
    return {};
}
auto PairwiseV2ActiveEvidenceRunner::encode() noexcept -> std::expected<void, Failure> {
    if (std::exchange(phase_, Phase::terminal) != Phase::produced)
        return fail(Stage::ownership);
    auto& writer = resources_.evidence.writer();
    auto activated = writer.activate();
    if (!activated) return fail(Stage::canonical_encoding, activated.error());
    auto written = prepared_codec_v1::write_finalized_evidence(writer, *pending_);
    if (!written) return fail(Stage::canonical_encoding, written.error());
    auto canonical = writer.finish();
    if (!canonical) return fail(Stage::canonical_encoding, canonical.error());
    auto sending = stream_.activate(*canonical);
    if (!sending) return fail(Stage::transport, sending.error());
    if (!retain_production_) pending_.reset();
    phase_ = Phase::active;
    return {};
}

auto PairwiseV2ActiveEvidenceRunner::write_step(ipc::ByteWriter& output,
                                               PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, Failure> {
    if (phase_ != Phase::active)
        return fail(Stage::ownership);
    auto progress = stream_.write_step(output, signal);
    if (!progress || *progress == PreparedTransportProgress::complete)
        phase_ = Phase::terminal;
    if (!progress)
        return fail(Stage::transport, progress.error());
    return *progress;
}

auto PairwiseV2ActiveEvidenceRunner::write_step(PreparedByteWriter output,
                                               PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, Failure> {
    if (phase_ != Phase::active)
        return fail(Stage::ownership);
    auto progress = stream_.write_step(output, signal);
    if (!progress || *progress == PreparedTransportProgress::complete)
        phase_ = Phase::terminal;
    if (!progress)
        return fail(Stage::transport, progress.error());
    return *progress;
}

PairwiseV2ActiveEvidenceController::PairwiseV2ActiveEvidenceController(
    PairwiseV2ControllerResources resources, PairwiseV2PreparedEvidenceController stream) noexcept
    : resources_{std::move(resources)}, stream_{std::move(stream)} {}
PairwiseV2ActiveEvidenceController::PairwiseV2ActiveEvidenceController(
    PairwiseV2ActiveEvidenceController&& other) noexcept
    : resources_{std::move(other.resources_)}, stream_{std::move(other.stream_)},
      validated_bytes_{std::exchange(other.validated_bytes_, {})},
      pending_{std::move(other.pending_)}, phase_{std::exchange(other.phase_, Phase::terminal)} {}

auto PairwiseV2ActiveEvidenceController::bind(PairwiseV2ControllerResources resources) noexcept
    -> std::expected<PairwiseV2ActiveEvidenceController, Failure> {
    auto stream = PairwiseV2PreparedEvidenceSessionAccess::adopt_controller(
        std::move(resources.phase_a.event), resources.evidence);
    if (!stream)
        return fail(Stage::ownership, stream.error());
    if (pairwise_v2_resource_identity_commitment(resources.evidence.identity()) != resources.authenticated_identity_)
        return fail(Stage::ownership, FinalizedEvidenceTransportFailure::identity_mismatch);
    return PairwiseV2ActiveEvidenceController{std::move(resources), std::move(*stream)};
}

auto PairwiseV2ActiveEvidenceController::start() noexcept -> std::expected<void, Failure> {
    if (phase_ != Phase::prepared)
        return fail(Stage::ownership);
    phase_ = Phase::terminal;
    auto active = stream_.activate();
    if (!active)
        return fail(Stage::transport, active.error());
    phase_ = Phase::active;
    return {};
}

auto PairwiseV2ActiveEvidenceController::read_step(ipc::ByteReader& input,
                                                   PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, Failure> {
    if (phase_ != Phase::active)
        return fail(Stage::ownership);
    auto progress = stream_.read_step(input, signal);
    if (!progress)
        phase_ = Phase::terminal;
    else if (*progress == PreparedTransportProgress::complete)
        phase_ = Phase::eof;
    if (!progress)
        return fail(Stage::transport, progress.error());
    return *progress;
}

auto PairwiseV2ActiveEvidenceController::read_step(PreparedByteReader input,
                                                   PreparedTransportSignal signal) noexcept
    -> std::expected<PreparedTransportProgress, Failure> {
    if (phase_ != Phase::active)
        return fail(Stage::ownership);
    auto progress = stream_.read_step(input, signal);
    if (!progress)
        phase_ = Phase::terminal;
    else if (*progress == PreparedTransportProgress::complete)
        phase_ = Phase::eof;
    if (!progress)
        return fail(Stage::transport, progress.error());
    return *progress;
}

auto PairwiseV2ActiveEvidenceController::completion_received() const noexcept -> bool {
    return (phase_ == Phase::active || phase_ == Phase::eof) && stream_.completion_received();
}

auto PairwiseV2ActiveEvidenceController::finish() noexcept
    -> std::expected<results::PairwiseRunResult, Failure> {
    auto reconstructed = reconstruct();
    if (!reconstructed) return std::unexpected{reconstructed.error()};
    return project();
}
auto PairwiseV2ActiveEvidenceController::reconstruct() noexcept -> std::expected<void, Failure> {
    if (std::exchange(phase_, Phase::terminal) != Phase::eof)
        return fail(Stage::ownership);
    auto bytes = stream_.finish();
    if (!bytes) return fail(Stage::transport, bytes.error());
    validated_bytes_ = std::as_bytes(*bytes);
    // Validate typed invariants and exact canonical re-encoding before projection.
    auto evidence = resources_.evidence.reconstruction().build(*bytes, resources_.authenticated_cpus);
    if (!evidence) return fail(Stage::reconstruction, evidence.error());
    pending_.emplace(std::move(*evidence));
    phase_ = Phase::reconstructed;
    return {};
}
auto PairwiseV2ActiveEvidenceController::project() noexcept
    -> std::expected<results::PairwiseRunResult, Failure> {
    if (std::exchange(phase_, Phase::terminal) != Phase::reconstructed)
        return fail(Stage::ownership);
    auto result = resources_.evidence.projector().project_owned(std::move(*pending_));
    if (!result) return fail(Stage::results, result.error());
    pending_.reset();
    return std::move(*result);
}

} // namespace epycbench::runtime::detail
