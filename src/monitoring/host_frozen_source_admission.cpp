#include "monitoring/host_frozen_source_admission.hpp"
#include "monitoring/host_frozen_source_launch.hpp"
#include "monitoring/host_q4_cpufreq_evidence_json.hpp"
#include "monitoring/host_frozen_source_admission_test_access.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/current_revision_tracked_capture.hpp"
#include "epycbench/core/canonical_json.hpp"

#include <type_traits>
#include <tuple>
#include <algorithm>
#include <stdexcept>

namespace epycbench::monitoring::qualification::detail {
struct HostFrozenSourceAdmissionState final {
    std::optional<MonitoringQ4AuthoritySession> session;
    std::optional<MonitoringQ4CompositionOwner> retained_owner;
    std::optional<MonitoringQ1ValidatedPublicValues> values;
    std::optional<MonitoringQ4RetainedEvidence> evidence;
    std::optional<std::array<std::string, 7>> predecessor;
    std::optional<MonitoringQ4GovernedInputValue> governed;
    std::optional<MonitoringQ4CompletedSourceProvenance> source_provenance;
    std::optional<MonitoringQ4HistoricalGitValue> history;
    bool published{};
    const HostFrozenSourceLaunchBinding* source{};
};

HostFrozenSourceRetainedInput::HostFrozenSourceRetainedInput(
    const HostFrozenSourceLaunchBinding& binding, std::string_view packet,
    std::string_view q5, std::string_view manifest, std::string_view checksums,
    const std::filesystem::path& repository, std::uint64_t repository_device,
    std::uint64_t repository_inode, const std::filesystem::path& qualification_root,
    std::uint64_t qualification_device, std::uint64_t qualification_inode,
    const core::Sha256Digest& build, std::string_view build_bytes, void* owner,
    Recheck recheck) noexcept
    : binding_{&binding}, packet_{packet}, q5_{q5}, manifest_{manifest}, checksums_{checksums},
      repository_{&repository}, qualification_root_{&qualification_root},
      repository_device_{repository_device}, repository_inode_{repository_inode},
      qualification_device_{qualification_device}, qualification_inode_{qualification_inode},
      build_{&build}, build_bytes_{build_bytes}, owner_{owner}, recheck_{recheck} {}

struct HostFrozenSourceAdmission::RetainedPacket final {
    std::string_view root, qid, head, build, policy, original_build, q4;
    std::uint64_t device{}, inode{};
    std::string profile_sha;
    std::array<std::string_view, 6> commits{};
    std::array<std::string_view, std::tuple_size_v<MonitoringQ4GovernedArtifactSlots>> governed{};
    std::array<std::string_view, 7> predecessor{};
};

auto HostFrozenSourceAdmission::decode_retained_packet(std::string_view bytes)
    -> std::optional<RetainedPacket> {
    if (bytes.size() < 20U || bytes.size() > (64U << 20U)) return std::nullopt;
    std::size_t position{};
    const auto take = [&](std::size_t count) -> std::string_view {
        if (count > bytes.size() - position) throw std::invalid_argument{"packet truncated"};
        auto result = bytes.substr(position, count); position += count; return result;
    };
    const auto number = [&](std::size_t width) -> std::uint64_t {
        std::uint64_t result{};
        for (char byte : take(width))
            result = (result << 8U) | static_cast<unsigned char>(byte);
        return result;
    };
    const auto block = [&](std::size_t limit) -> std::string_view {
        const auto size = number(4U);
        if (size > limit) throw std::invalid_argument{"packet capacity"};
        return take(static_cast<std::size_t>(size));
    };
    const auto text = [&](std::size_t limit) -> std::string_view {
        const auto result = block(limit);
        if (result.empty() || result.find('\0') != std::string_view::npos)
            throw std::invalid_argument{"packet text"};
        return result;
    };
    const auto oid = [&]() -> std::string_view {
        const auto result = text(64U);
        if ((result.size() != 40U && result.size() != 64U) ||
            !std::ranges::all_of(result, [](char byte) {
                return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
            })) throw std::invalid_argument{"packet commit"};
        return result;
    };
    const auto hex = [&](std::string_view raw) {
        constexpr std::string_view alphabet{"0123456789abcdef"};
        std::string result; result.reserve(raw.size() * 2U);
        for (char raw_byte : raw) {
            const auto byte = static_cast<unsigned char>(raw_byte);
            result.push_back(alphabet[byte >> 4U]); result.push_back(alphabet[byte & 15U]);
        }
        return result;
    };
    try {
        if (take(8U) != "EBSINPUT" || number(2U) != 1U || number(2U) != 0U ||
            number(8U) != bytes.size()) return std::nullopt;
        RetainedPacket result;
        result.root = text(511U);
        if (result.root.front() != '/') return std::nullopt;
        result.device = number(8U); result.inode = number(8U);
        result.qid = text(128U); result.head = oid(); result.profile_sha = hex(take(32U));
        result.build = block(1U << 20U); result.policy = block(1U << 20U);
        result.original_build = text(1U << 20U);
        for (auto& commit : result.commits) commit = oid();
        if (result.commits[4] != result.head) return std::nullopt;
        static_assert(std::tuple_size_v<MonitoringQ4GovernedArtifactSlots> == 18U);
        for (std::size_t index{}; index < result.governed.size(); ++index) {
            const auto limit = index <= 2U ? (1U << 20U)
                : (index <= 10U || index == 17U ? (2U << 20U) : (1U << 16U));
            result.governed[index] = block(limit);
            if (core::sha256(result.governed[index]).to_hex() != hex(take(32U)))
                return std::nullopt;
        }
        for (std::size_t index{}; index < 5U; ++index)
            result.predecessor[index] = block(64U << 20U);
        result.q4 = block(64U << 20U);
        result.predecessor[5] = block(1U << 20U);
        result.predecessor[6] = block(1U << 20U);
        if (position != bytes.size()) return std::nullopt;
        return result;
    } catch (const std::invalid_argument&) { return std::nullopt; }
}

auto HostFrozenSourceAdmission::verify_retained_q4(
    const std::array<std::string, 7>& predecessor, std::string_view q4,
    const MonitoringQ4GovernedInputCandidate& candidate, const MonitoringQ4HistoricalGitValue& history,
    const MonitoringQ1ValidatedPublicValues& values, const MonitoringQ4GovernedPlanSchemaBinding& governed)
    -> std::expected<MonitoringQ4RetainedEvidence, core::Error> {
    auto prefix = MonitoringQ4CompleteQ3PredecessorAuthenticator::completed_prefix(
        predecessor, q4, candidate, values);
    if (!prefix) return std::unexpected{std::move(prefix.error().primary)};
    MonitoringQ4CompleteQ3PredecessorInput input{predecessor[0], predecessor[1], predecessor[2],
        predecessor[3], predecessor[4], std::move(prefix->manifest_checksums[0]),
        std::move(prefix->manifest_checksums[1])};
    auto prior = MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_campaign(
        std::move(input), candidate, history, values);
    if (!prior) return std::unexpected{std::move(prior.error().primary)};
    return MonitoringQ4RetainedEvidence::verify(*prior, history, governed, candidate, q4);
}

auto HostFrozenSourceAdmission::verify_published_q5(
    std::string_view q5_bytes, std::string_view manifest_bytes, std::string_view checksums,
    const std::array<std::string, 7>& predecessor, std::string_view q4, std::string_view qid,
    std::string_view profile, std::string_view head, const core::Sha256Digest& build,
    bool synthetic) -> bool {
    // Full Q5 measurement/chronology acceptance remains the genuine parent's
    // existing validator. This fresh collector checks the exact published byte
    // association, envelope, domain and producer identity again. The private
    // workflow fixture accepts only explicitly synthetic, authority-free cells.
    if (q5_bytes.empty() || q5_bytes.size() > (64U << 20U) ||
        manifest_bytes.empty() || manifest_bytes.size() > (1U << 20U) ||
        checksums.empty() || checksums.size() > (1U << 20U) ||
        !MonitoringQ4CompleteQ3PredecessorAuthenticator::retained_depth_valid(q5_bytes, 64U) ||
        !MonitoringQ4CompleteQ3PredecessorAuthenticator::retained_depth_valid(manifest_bytes, 3U))
        return false;
    using Json = nlohmann::json;
    const auto exact = [](const Json& value, std::initializer_list<std::string_view> keys) {
        return value.is_object() && value.size() == keys.size() &&
            std::ranges::all_of(keys, [&](std::string_view key) { return value.contains(key); });
    };
    auto phase = Json::parse(q5_bytes); core::detail::JsonCleanup phase_cleanup{phase};
    auto manifest = Json::parse(manifest_bytes); core::detail::JsonCleanup manifest_cleanup{manifest};
    const auto canonical = [](const Json& value, std::string_view bytes) {
        auto encoded = core::canonicalize_json(value);
        return encoded && *encoded + "\n" == bytes;
    };
    if (!canonical(phase, q5_bytes) || !canonical(manifest, manifest_bytes) ||
        !exact(phase, {"payload", "schema", "sha256"}) ||
        phase["schema"] != "epycbench.monitoring-host-frozen-evidence/v1") return false;
    const auto& payload = phase["payload"];
    auto body = core::canonicalize_json(payload);
    if (!body || phase["sha256"] != core::sha256(*body + "\n").to_hex() ||
        !exact(payload, {"cells", "comparisons", "domain", "phase", "qualification", "termination"}) ||
        payload["domain"] != (synthetic ? "synthetic" : "source") || payload["phase"] != "Q5" ||
        !payload["cells"].is_array() || payload["cells"].size() != 8U ||
        !payload["comparisons"].is_array() || !payload["comparisons"].empty() ||
        !exact(payload["termination"], {"kind", "consumed"}) ||
        payload["termination"]["kind"] != "completed" || payload["termination"]["consumed"] != "8")
        return false;
    if (synthetic) {
        if (!payload["qualification"].is_null()) return false;
    } else if (!exact(payload["qualification"], {"qualification_id", "profile_identity", "profile_sha256"}) ||
        payload["qualification"]["qualification_id"] != qid ||
        payload["qualification"]["profile_identity"] != "epycbench.monitoring-qualification.host-generic/v2" ||
        payload["qualification"]["profile_sha256"] != profile) return false;
    for (const auto& cell : payload["cells"]) {
        if (synthetic) {
            if (cell.at("payload").at("domain") != "synthetic" ||
                !cell.at("payload").at("authority").is_null() ||
                cell.at("payload").at("identity").at("phase") != "Q5") return false;
            continue;
        }
        const auto& authority = cell.at("payload").at("authority").at("payload");
        if (cell.at("payload").at("domain") != "source" ||
            cell.at("payload").at("identity").at("phase") != "Q5" ||
            authority.at("qualification_id") != qid || authority.at("profile").at("sha256") != profile ||
            authority.at("repository").at("commit") != head ||
            authority.at("repository").at("dirty") != false ||
            authority.at("build").at("identity") != "sha256:" + build.to_hex()) return false;
    }
    auto original = Json::parse(predecessor[5]); core::detail::JsonCleanup original_cleanup{original};
    if (!exact(manifest, {"artifacts", "environment_digest", "profile", "qualification_id", "schema", "tool"}) ||
        manifest["environment_digest"] != original.at("environment_digest") ||
        manifest["profile"] != original.at("profile") ||
        manifest["qualification_id"] != qid || manifest["schema"] != original.at("schema") ||
        manifest["tool"] != original.at("tool") ||
        !manifest["artifacts"].is_array() || manifest["artifacts"].size() != 7U) return false;
    constexpr std::array paths{"host-environment.json", "q0b/evidence.json", "q1/evidence.json",
        "q2/evidence.json", "q3/evidence.json", "q4/evidence.json", "q5/evidence.json"};
    std::string expected_checksums = core::sha256(manifest_bytes).to_hex() + "  manifest.json\n";
    for (std::size_t index{}; index < paths.size(); ++index) {
        const auto bytes = index < 5U ? std::string_view{predecessor[index]} : index == 5U ? q4 : q5_bytes;
        const auto digest = core::sha256(bytes).to_hex();
        const auto& artifact = manifest["artifacts"][index];
        if (!exact(artifact, {"path", "sha256"}) || artifact["path"] != paths[index] ||
            artifact["sha256"] != digest) return false;
        expected_checksums += digest + "  " + paths[index] + "\n";
    }
    return expected_checksums == checksums;
}

auto HostFrozenSourceAdmissionTestAccess::retained_packet_valid(std::string_view bytes) noexcept -> bool {
    try { return HostFrozenSourceAdmission::decode_retained_packet(bytes).has_value(); }
    catch (...) { return false; }
}

auto HostFrozenSourceAdmissionTestAccess::terminal_failure(MonitoringQ4AuthorityFailure failure)
    -> HostFrozenSourceAdmissionFailure {
    return HostFrozenSourceAdmissionFailure{std::move(failure), nullptr};
}

auto HostFrozenSourceAdmissionTestAccess::hidden_inputs(
    const HostFrozenSourceAdmissionFailure& failure) noexcept -> bool {
    return failure.retained_ && failure.retained_->values && failure.retained_->evidence &&
           failure.retained_->predecessor && failure.retained_->governed &&
           failure.retained_->source_provenance && failure.retained_->history &&
           !failure.retained_->published;
}

auto HostFrozenSourceAdmissionTestAccess::transfer(MonitoringQ4AuthoritySession session)
    -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
    auto state=std::make_unique<HostFrozenSourceAdmissionState>();
    state->session.emplace(std::move(session));
    return HostFrozenSourceAdmission::finish(std::move(state));
}

HostFrozenSourceAdmissionFailure::HostFrozenSourceAdmissionFailure(
    MonitoringQ4AuthorityFailure failure, std::unique_ptr<HostFrozenSourceAdmissionState> retained) noexcept
    : failure_{std::move(failure)}, retained_{std::move(retained)} {}
HostFrozenSourceAdmissionFailure::HostFrozenSourceAdmissionFailure(
    HostFrozenSourceAdmissionFailure&&) noexcept = default;
HostFrozenSourceAdmissionFailure::~HostFrozenSourceAdmissionFailure() noexcept {
    // These finite producer diagnostics remain intact until their last borrow.
    // Retire every engaged owning cell, including stock beyond a reported count.
    std::visit([](auto& detail) noexcept {
        using T = std::remove_cvref_t<decltype(detail)>;
        if constexpr (std::is_same_v<T, core::Error>) {
            core::detail::retire_json(detail.context);
        } else if constexpr (std::is_same_v<T, MonitoringQ1GitCommandFailure>) {
            core::detail::retire_json(detail.primary.context);
            for (auto& error : detail.cleanup) core::detail::retire_json(error.context);
        } else if constexpr (std::is_same_v<T, MonitoringQ4HistoricalGitFailure>) {
            core::detail::retire_json(detail.primary.context);
            for (auto& error : detail.cleanup)
                if (error) core::detail::retire_json(error->context);
        } else if constexpr (std::is_same_v<T, MonitoringQ4GovernedPlanSchemaBindingFailure>) {
            if (auto* error = std::get_if<core::Error>(&detail))
                core::detail::retire_json(error->context);
        }
    }, failure_.detail);
    if (failure_.projected) {
        if (failure_.projected->primary)
            core::detail::retire_json(failure_.projected->primary->context);
        for (auto& error : failure_.projected->cleanup)
            if (error) core::detail::retire_json(error->context);
    }
}
HostFrozenSourceAdmission::HostFrozenSourceAdmission(
    std::unique_ptr<HostFrozenSourceAdmissionState> state) noexcept : state_{std::move(state)} {}
HostFrozenSourceAdmission::HostFrozenSourceAdmission(HostFrozenSourceAdmission&&) noexcept = default;
HostFrozenSourceAdmission::~HostFrozenSourceAdmission() noexcept = default;

auto HostFrozenSourceAdmission::authenticate(const std::filesystem::path& root,
    std::string_view qid) noexcept
    -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
    return authenticate_impl(root, qid, nullptr);
}
auto HostFrozenSourceAdmission::authenticate_source(const std::filesystem::path& root,
    std::string_view qid, const HostFrozenSourceLaunchBinding& source) noexcept
    -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
    return authenticate_impl(root, qid, &source);
}
auto HostFrozenSourceAdmission::authenticate_impl(const std::filesystem::path& root,
    std::string_view qid, const HostFrozenSourceLaunchBinding* source,
    const MonitoringRunningBuildAuthority::Operations* controlled_build) noexcept
    -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
    std::unique_ptr<HostFrozenSourceAdmissionState> state;
    try {
        state = std::make_unique<HostFrozenSourceAdmissionState>();
        state->source = source;
        if (source && (!source->ready() || !source->guard())) {
            MonitoringQ4AuthorityFailure failure{MonitoringQ4AuthorityStage::contained,
                {MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::inactive}};
            return std::unexpected{HostFrozenSourceAdmissionFailure{std::move(failure), std::move(state)}};
        }
        auto session = controlled_build && (qid == "MON-007.host-v2.campaign-20261005-01" || qid == "MON-007.host-v2.campaign-20261018-01" || qid == "MON-007.host-v2.campaign-20261020-01" || qid == "MON-007.host-v2.campaign-20261024-01" || qid == "MON-007.host-v2.campaign-20261102-01")
            ? MonitoringQ4AuthoritySession::authenticate_impl(root, qid, true, controlled_build, source)
            : MonitoringQ4AuthoritySession::authenticate_completed(root, qid, source);
        if (!session) return std::unexpected{HostFrozenSourceAdmissionFailure{
            std::move(session.error()), std::move(state)}};
        state->session.emplace(std::move(*session));
        return finish(std::move(state));
    } catch (...) {
        MonitoringQ4AuthorityFailure failure{
            MonitoringQ4AuthorityStage::contained,
            {MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::allocation}};
        return std::unexpected{HostFrozenSourceAdmissionFailure{std::move(failure), std::move(state)}};
    }
}

auto HostFrozenSourceAdmission::authenticate_retained_source(
    const HostFrozenSourceRetainedInput& input) noexcept
    -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
    return authenticate_retained_impl(input, nullptr, false);
}
auto HostFrozenSourceAdmission::authenticate_retained_impl(
    const HostFrozenSourceRetainedInput& input,
    const MonitoringRunningBuildAuthority::Operations* controlled_build, bool synthetic) noexcept
    -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
    std::unique_ptr<HostFrozenSourceAdmissionState> state;
    const auto reject = [&](MonitoringQ4AuthorityFailure failure)
        -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
        if (state && state->retained_owner) {
            auto retired = state->retained_owner->rollback(failure.primary);
            for (std::size_t index{}; index < retired.cleanup.count; ++index) {
                if (failure.cleanup.count < failure.cleanup.facts.size())
                    failure.cleanup.facts[failure.cleanup.count++] = retired.cleanup.facts[index];
                else failure.cleanup.overflowed = true;
            }
            failure.cleanup.overflowed = failure.cleanup.overflowed || retired.cleanup.overflowed;
        }
        return std::unexpected{HostFrozenSourceAdmissionFailure{std::move(failure), std::move(state)}};
    };
    const auto fixed = [](MonitoringQ4AuthorityStage stage, MonitoringQ4CompositionSource source,
                          MonitoringQ4CompositionFactKind kind) {
        return MonitoringQ4AuthorityFailure{.stage = stage, .primary = {source, kind}};
    };
    try {
        const auto& binding = *input.binding_;
        const auto& grant = binding.grant_record();
        if ((synthetic && !controlled_build) || !binding.ready() || !binding.guard() ||
            grant.phase != platform_linux::host_work::SourceLaunchPhase::q6 ||
            grant.phase_generation != 2U || !input.owner_ || !input.recheck_ ||
            !input.repository_ || !input.qualification_root_ || !input.build_ ||
            !input.recheck_(input.owner_, *input.qualification_root_,
                input.qualification_device_, input.qualification_inode_))
            return reject(fixed(MonitoringQ4AuthorityStage::repository,
                MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::inactive));
        auto packet = decode_retained_packet(input.packet_);
        if (!packet || (packet->qid != "MON-007.host-v2.campaign-20261005-01" && packet->qid != "MON-007.host-v2.campaign-20261018-01" && packet->qid != "MON-007.host-v2.campaign-20261020-01" && packet->qid != "MON-007.host-v2.campaign-20261024-01" && packet->qid != "MON-007.host-v2.campaign-20261102-01") ||
            std::filesystem::path{packet->root} != *input.repository_ ||
            packet->device != input.repository_device_ || packet->inode != input.repository_inode_ ||
            packet->build != input.build_bytes_ ||
            *input.qualification_root_ != *input.repository_ / ".local/monitoring-qualification" / packet->qid)
            return reject(fixed(MonitoringQ4AuthorityStage::predecessor,
                MonitoringQ4CompositionSource::predecessor, MonitoringQ4CompositionFactKind::changed));
        state = std::make_unique<HostFrozenSourceAdmissionState>();
        state->source = &binding;
        auto owner = MonitoringQ4CompositionOwner::create();
        if (!owner) return reject(MonitoringQ4AuthorityFailure{
            MonitoringQ4AuthorityStage::contained, owner.error().primary, std::move(owner.error().cleanup)});
        state->retained_owner.emplace(std::move(*owner));
        auto& held = *state->retained_owner;
        auto repository = MonitoringQ1PlanV2AuthoritySession::authenticate_shared(
            *input.repository_, packet->qid, true, &binding);
        if (!repository) {
            auto failure = fixed(MonitoringQ4AuthorityStage::repository,
                MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::primary);
            failure.detail = std::move(repository.error()); return reject(std::move(failure));
        }
        held.adopt_repository(std::move(*repository));
        auto& repo = held.repository();
        const auto identity = repo.q4_root().identity();
        const auto& values = repo.q4_values();
        if (identity.device != packet->device || identity.inode != packet->inode ||
            repo.authenticated_repository_commit() != packet->head ||
            values.profile_sha256() != packet->profile_sha ||
            core::sha256(packet->policy).to_hex() != values.policy_sha256())
            return reject(fixed(MonitoringQ4AuthorityStage::repository,
                MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::changed));
        auto capture = MonitoringQ4CurrentRevisionTrackedCapture::run(
            repo.q4_executable(), repo.q4_root(), repo.q4_metadata());
        if (!capture) {
            auto failure = fixed(MonitoringQ4AuthorityStage::current_revision,
                MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::primary);
            failure.detail = std::move(capture.error()); return reject(std::move(failure));
        }
        auto governed = MonitoringQ4GovernedInputAuthority::authenticate(
            repo.q4_root().descriptor(), std::move(*capture));
        if (!governed) {
            auto failure = fixed(MonitoringQ4AuthorityStage::governed_input,
                MonitoringQ4CompositionSource::governed, MonitoringQ4CompositionFactKind::primary);
            failure.detail = governed.error(); return reject(std::move(failure));
        }
        auto candidate = governed->prepare_candidate();
        if (!candidate) {
            auto failure = fixed(MonitoringQ4AuthorityStage::governed_input,
                MonitoringQ4CompositionSource::governed, MonitoringQ4CompositionFactKind::primary);
            failure.detail = candidate.error(); return reject(std::move(failure));
        }
        held.adopt_governed(std::move(*governed), std::move(*candidate));
        for (std::size_t index{}; index < packet->governed.size(); ++index)
            if (!held.candidate().records_[index] ||
                held.candidate().records_[index]->bytes != packet->governed[index])
                return reject(fixed(MonitoringQ4AuthorityStage::governed_input,
                    MonitoringQ4CompositionSource::governed, MonitoringQ4CompositionFactKind::changed));
        const std::array<std::string_view, 4> old_commits{
            packet->commits[0], packet->commits[1], packet->commits[2], packet->commits[3]};
        auto history = MonitoringQ4HistoricalGitAuthority::authenticate(
            repo.q4_executable(), repo.q4_root(), repo.q4_metadata(), held.candidate(),
            nullptr, &old_commits, packet->commits[5]);
        if (!history) {
            auto failure = fixed(MonitoringQ4AuthorityStage::history,
                MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::primary);
            failure.detail = std::move(history.error()); return reject(std::move(failure));
        }
        auto semantic = MonitoringQ4GovernedPlanSchemaBinding::bind(
            held.candidate(), MonitoringQ4GovernedPlanSchemaBinding::candidate_binding(held.candidate()));
        if (!semantic) {
            auto failure = fixed(MonitoringQ4AuthorityStage::plan_schema,
                MonitoringQ4CompositionSource::governed, MonitoringQ4CompositionFactKind::primary);
            failure.detail = std::move(semantic.error()); return reject(std::move(failure));
        }
        std::array<std::string, 7> predecessor;
        for (std::size_t index{}; index < predecessor.size(); ++index)
            predecessor[index] = packet->predecessor[index];
        auto evidence = verify_retained_q4(predecessor, packet->q4, held.candidate(),
                                           *history, values, *semantic);
        if (!evidence) {
            auto failure = fixed(MonitoringQ4AuthorityStage::predecessor,
                MonitoringQ4CompositionSource::predecessor, MonitoringQ4CompositionFactKind::primary);
            failure.detail = std::move(evidence.error()); return reject(std::move(failure));
        }
        if (evidence->producer_build_identity() != packet->original_build ||
            !verify_published_q5(input.q5_, input.manifest_, input.checksums_, predecessor,
                packet->q4, values.qualification_id(), values.profile_sha256(), packet->head,
                *input.build_, synthetic))
            return reject(fixed(MonitoringQ4AuthorityStage::predecessor,
                MonitoringQ4CompositionSource::predecessor, MonitoringQ4CompositionFactKind::changed));
        auto build = controlled_build
            ? MonitoringRunningBuildAuthority::retain_for_composition(packet->head, *controlled_build)
            : MonitoringRunningBuildAuthority::retain(packet->head);
        if (!build) {
            auto failure = fixed(MonitoringQ4AuthorityStage::running_build,
                MonitoringQ4CompositionSource::build, MonitoringQ4CompositionFactKind::primary);
            failure.detail = std::move(build.error()); return reject(std::move(failure));
        }
        held.adopt_build(std::move(*build));
        if (held.build().identity() != *input.build_ || held.build().canonical() != packet->build ||
            !std::ranges::equal(grant.build, input.build_->bytes()))
            return reject(fixed(MonitoringQ4AuthorityStage::running_build,
                MonitoringQ4CompositionSource::build, MonitoringQ4CompositionFactKind::changed));
        const auto final_failure = [&](MonitoringQ4CompositionSource source,
            MonitoringQ4CompositionFactKind kind, HostFrozenRetainedRecheck checkpoint) {
            auto failure = fixed(MonitoringQ4AuthorityStage::final_recheck, source, kind);
            failure.primary.detail = static_cast<unsigned int>(checkpoint);
            return failure;
        };
        auto repo_recheck = repo.q3_recheck();
        if (!repo_recheck) {
            auto failure = final_failure(MonitoringQ4CompositionSource::repository,
                MonitoringQ4CompositionFactKind::primary, HostFrozenRetainedRecheck::repository);
            failure.detail = std::move(repo_recheck.error()); return reject(std::move(failure));
        }
        auto governed_recheck = held.governed().recheck();
        if (!governed_recheck) {
            auto failure = final_failure(MonitoringQ4CompositionSource::governed,
                MonitoringQ4CompositionFactKind::primary, HostFrozenRetainedRecheck::governed);
            failure.detail = MonitoringQ4GovernedInputFailure{MonitoringQ4GovernedInputFailureStage::recheck,
                governed_recheck.error().kind, governed_recheck.error().native_error,
                governed_recheck.error().retirement};
            return reject(std::move(failure));
        }
        auto build_recheck = held.build().recheck();
        if (!build_recheck) {
            auto failure = final_failure(MonitoringQ4CompositionSource::build,
                MonitoringQ4CompositionFactKind::primary, HostFrozenRetainedRecheck::build);
            failure.detail = std::move(build_recheck.error()); return reject(std::move(failure));
        }
        if (!*repo_recheck)
            return reject(final_failure(MonitoringQ4CompositionSource::repository,
                MonitoringQ4CompositionFactKind::changed, HostFrozenRetainedRecheck::repository));
        if (!*governed_recheck)
            return reject(final_failure(MonitoringQ4CompositionSource::governed,
                MonitoringQ4CompositionFactKind::changed, HostFrozenRetainedRecheck::governed));
        if (!*build_recheck)
            return reject(final_failure(MonitoringQ4CompositionSource::build,
                MonitoringQ4CompositionFactKind::changed, HostFrozenRetainedRecheck::build));
        if (!binding.guard())
            return reject(final_failure(MonitoringQ4CompositionSource::repository,
                MonitoringQ4CompositionFactKind::changed, HostFrozenRetainedRecheck::source_guard));
        if (!input.recheck_(input.owner_, *input.qualification_root_,
                input.qualification_device_, input.qualification_inode_))
            return reject(final_failure(MonitoringQ4CompositionSource::repository,
                MonitoringQ4CompositionFactKind::changed, HostFrozenRetainedRecheck::retained_root));
        state->source_provenance.emplace(MonitoringQ4CompletedSourceProvenance{
            repo.q4_root().accepted_path(), identity, repo.q4_executable().held_identity(),
            held.build().identity(), std::string{held.build().canonical()}});
        state->evidence.emplace(std::move(*evidence));
        state->predecessor.emplace(std::move(predecessor));
        state->history.emplace(std::move(*history));
        if (!repo.take_completed_values(state->values))
            return reject(fixed(MonitoringQ4AuthorityStage::contained,
                MonitoringQ4CompositionSource::repository, MonitoringQ4CompositionFactKind::inactive));
        auto retired = held.retire(&state->governed);
        if (retired.count != 0U || retired.overflowed) {
            auto failure = fixed(MonitoringQ4AuthorityStage::retirement,
                MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::primary);
            failure.cleanup = std::move(retired);
            // Already consumed retirement is not repeated by the rejection path.
            state->retained_owner.reset(); return reject(std::move(failure));
        }
        state->retained_owner.reset();
        if (!state->values || !state->evidence || !state->predecessor || !state->governed ||
            !state->history || !state->source_provenance)
            return reject(final_failure(MonitoringQ4CompositionSource::repository,
                MonitoringQ4CompositionFactKind::changed, HostFrozenRetainedRecheck::completed_state));
        if (!binding.guard())
            return reject(final_failure(MonitoringQ4CompositionSource::repository,
                MonitoringQ4CompositionFactKind::changed, HostFrozenRetainedRecheck::retired_guard));
        if (!input.recheck_(input.owner_, *input.qualification_root_,
                input.qualification_device_, input.qualification_inode_))
            return reject(final_failure(MonitoringQ4CompositionSource::repository,
                MonitoringQ4CompositionFactKind::changed, HostFrozenRetainedRecheck::retired_root));
        state->published = true;
        return HostFrozenSourceAdmission{std::move(state)};
    } catch (const std::bad_alloc&) {
        return reject(fixed(MonitoringQ4AuthorityStage::contained,
            MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::allocation));
    } catch (...) {
        return reject(fixed(MonitoringQ4AuthorityStage::contained,
            MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::contained));
    }
}

auto HostFrozenSourceAdmission::finish(std::unique_ptr<HostFrozenSourceAdmissionState> state) noexcept
    -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
    if (!state || !state->session || state->published) {
        MonitoringQ4AuthorityFailure failure{MonitoringQ4AuthorityStage::contained,
            {MonitoringQ4CompositionSource::contained, MonitoringQ4CompositionFactKind::inactive}};
        return std::unexpected{HostFrozenSourceAdmissionFailure{std::move(failure), std::move(state)}};
    }
    auto transferred = state->session->finalize_completed(
        state->values, state->evidence, state->predecessor, state->governed,
        &state->source_provenance, &state->history);
    if (!transferred) return std::unexpected{HostFrozenSourceAdmissionFailure{
        std::move(transferred.error()), std::move(state)}};
    state->session.reset(); // Actual native owners were already confirmed retired.
    state->published = true;
    return HostFrozenSourceAdmission{std::move(state)};
}

auto HostFrozenSourceAdmission::ready() const noexcept -> bool {
    return state_ && state_->published && state_->values && state_->evidence &&
           state_->predecessor && state_->governed && state_->source_provenance && state_->history &&
           (!state_->source || state_->source->ready());
}
auto HostFrozenSourceAdmission::values() const noexcept
    -> std::expected<const MonitoringQ1ValidatedPublicValues*, HostFrozenSourceAccessFailure> {
    if (!ready())
        return std::unexpected{HostFrozenSourceAccessFailure::inactive};
    return &*state_->values;
}
auto HostFrozenSourceAdmission::completed_q4_bytes() const noexcept
    -> std::expected<std::string_view, HostFrozenSourceAccessFailure> {
    if (!ready())
        return std::unexpected{HostFrozenSourceAccessFailure::inactive};
    return state_->evidence->bytes();
}
auto HostFrozenSourceAdmission::completed_predecessor_bytes(
    HostFrozenPredecessorArtifact artifact) const noexcept
    -> std::expected<std::string_view, HostFrozenSourceAccessFailure> {
    const auto index = static_cast<std::size_t>(artifact);
    if (!ready() || index >= state_->predecessor->size())
        return std::unexpected{HostFrozenSourceAccessFailure::inactive};
    return (*state_->predecessor)[index];
}
auto HostFrozenSourceAdmission::governed_inputs() const noexcept
    -> std::expected<const MonitoringQ4GovernedInputValue*, HostFrozenSourceAccessFailure> {
    if (!ready()) return std::unexpected{HostFrozenSourceAccessFailure::inactive};
    return &*state_->governed;
}
auto HostFrozenSourceAdmission::retained_source_provenance() const noexcept
    -> std::expected<const MonitoringQ4CompletedSourceProvenance*, HostFrozenSourceAccessFailure> {
    if (!ready()) return std::unexpected{HostFrozenSourceAccessFailure::inactive};
    return &*state_->source_provenance;
}
auto HostFrozenSourceAdmission::retained_history() const noexcept
    -> std::expected<const MonitoringQ4HistoricalGitValue*, HostFrozenSourceAccessFailure> {
    if (!ready()) return std::unexpected{HostFrozenSourceAccessFailure::inactive};
    return &*state_->history;
}
} // namespace epycbench::monitoring::qualification::detail
