#include "runtime/pairwise_monitoring_host_capacity_startup.hpp"
#include "runtime/allocation_free_json_retirement.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <limits>
#include <ranges>
#include <utility>

namespace epycbench::runtime::detail {
PairwiseHostCapacityRequestTranscript::~PairwiseHostCapacityRequestTranscript() {
    // These are exclusively owned non-const request subobjects. Retirement is
    // confined to this private owner's teardown, after every admitted borrow
    // has expired; no public request or wire behavior changes.
    retire_json(const_cast<nlohmann::ordered_json&>(first_.wire()));
    retire_json(const_cast<nlohmann::ordered_json&>(second_.wire()));
}

struct PairwiseHostCapacityRequestTranscriptConstruction final {
    [[nodiscard]] static auto make(
        ipc::RequestId first_id, ipc::RequestId second_id, std::string body,
        core::Sha256Digest body_sha256, ipc::PlacementRequest first,
        ipc::PlacementRequest second, std::uint64_t seed,
        StatisticsComparisonPolicySource statistics) noexcept
        -> PairwiseHostCapacityRequestTranscript {
        return PairwiseHostCapacityRequestTranscript{
            first_id, second_id, std::move(body), body_sha256, std::move(first),
            std::move(second), seed, std::move(statistics)};
    }
};

namespace {

using Failure = PairwiseHostCapacityStartupFailure;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

struct ParsedEnvelope final {
    std::string protocol;
    std::string type;
    ipc::RequestId request_id;
    std::uint64_t sequence{};
    std::string profile;
    OrderedJson payload;
};

template<class Tree>
[[nodiscard]] auto exact(const Tree& value,
                         std::initializer_list<std::string_view> names) noexcept -> bool {
    if (!value.is_object() || value.size() != names.size())
        return false;
    return std::ranges::all_of(names, [&](std::string_view name) { return value.contains(name); });
}

// These guards own no authority and retire only exclusively owned parser scratch.
template<class Tree> struct RetireTree {
    Tree* value;
    ~RetireTree() { if (value) retire_json(*value); }
};
struct RequestJsonBuilder final : nlohmann::json_sax<Json> {
    struct Frame { Json* value; std::string key; };
    Json root;
    std::vector<Frame> frames;
    ~RequestJsonBuilder() { retire_json(root); }
    auto destination() -> Json& {
        if (frames.empty()) return root;
        auto& frame=frames.back();
        if (frame.value->is_array()) { frame.value->push_back(nullptr); return frame.value->back(); }
        return (*frame.value)[frame.key];
    }
    template<class Value> auto scalar(Value&& value) -> bool {
        destination()=std::forward<Value>(value); return true;
    }
    auto null() -> bool override { return scalar(nullptr); }
    auto boolean(bool value) -> bool override { return scalar(value); }
    auto number_integer(number_integer_t value) -> bool override { return scalar(value); }
    auto number_unsigned(number_unsigned_t value) -> bool override { return scalar(value); }
    auto number_float(number_float_t,const string_t&) -> bool override { return false; }
    auto string(string_t& value) -> bool override { return scalar(std::move(value)); }
    auto binary(binary_t&) -> bool override { return false; }
    auto start(bool object) -> bool {
        auto& value=destination();value=object?Json::object():Json::array();
        frames.push_back({&value,{}});return true;
    }
    auto start_object(std::size_t) -> bool override { return start(true); }
    auto start_array(std::size_t) -> bool override { return start(false); }
    auto key(string_t& value) -> bool override {
        if (frames.back().value->contains(value)) return false;
        frames.back().key=std::move(value);return true;
    }
    auto end_object() -> bool override { frames.pop_back();return true; }
    auto end_array() -> bool override { frames.pop_back();return true; }
    auto parse_error(std::size_t,const std::string&,const nlohmann::detail::exception&) -> bool override { return false; }
};
[[nodiscard]] auto parse_json(std::string_view body) -> std::expected<Json, Failure> {
    if (body.starts_with("\xef\xbb\xbf") || body.find('\0') != std::string_view::npos)
        return std::unexpected{Failure::invalid_encoding};
    RequestJsonBuilder builder;
    if (!Json::sax_parse(body,&builder))
        return std::unexpected{Failure::invalid_encoding};
    auto canonical = core::canonicalize_json(builder.root);
    core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
    if (!canonical || *canonical != body)
        return std::unexpected{Failure::invalid_encoding};
    return std::move(builder.root);
}

[[nodiscard]] auto decimal(std::string_view text, bool positive = false)
    -> std::optional<std::uint64_t> {
    if (text.empty() || (text.size() > 1U && text.front() == '0'))
        return std::nullopt;
    std::uint64_t value{};
    const auto [position, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || position != text.data() + text.size() ||
        (positive && value == 0U))
        return std::nullopt;
    return value;
}

[[nodiscard]] auto digest(const Json& value) -> std::optional<core::Sha256Digest> {
    if (!value.is_string())
        return std::nullopt;
    auto parsed = core::Sha256Digest::parse(value.get_ref<const std::string&>());
    if (!parsed)
        return std::nullopt;
    return *parsed;
}

[[nodiscard]] auto request_id(const Json& value) -> std::optional<ipc::RequestId> {
    if (!value.is_string())
        return std::nullopt;
    auto parsed = ipc::RequestId::parse(value.get_ref<const std::string&>());
    if (!parsed)
        return std::nullopt;
    return *parsed;
}

[[nodiscard]] auto encode_envelope(std::string_view type, ipc::RequestId request,
                                   std::string_view profile, OrderedJson payload)
    -> std::expected<std::string, Failure> {
    Json envelope{{"payload", std::move(payload)},
                  {"payload_profile", profile},
                  {"protocol", pairwise_host_capacity_startup_protocol},
                  {"request_id", request.text()},
                  {"sequence", "0"},
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

[[nodiscard]] auto parse_envelope(std::string_view body) -> std::expected<ParsedEnvelope, Failure> {
    auto parsed = parse_json(body);
    RetireTree<Json> parsed_cleanup{parsed?&*parsed:nullptr};
    if (!parsed || !exact(*parsed, {"protocol", "type", "request_id", "sequence",
                                   "payload_profile", "payload"}))
        return std::unexpected{parsed ? Failure::invalid_envelope : parsed.error()};
    if (!(*parsed)["protocol"].is_string() || !(*parsed)["type"].is_string() ||
        !(*parsed)["sequence"].is_string() || !(*parsed)["payload_profile"].is_string() ||
        !(*parsed)["payload"].is_object())
        return std::unexpected{Failure::invalid_envelope};
    auto id = request_id((*parsed)["request_id"]);
    const auto sequence = decimal((*parsed)["sequence"].get_ref<const std::string&>());
    if (!id || !sequence)
        return std::unexpected{Failure::invalid_envelope};
    ParsedEnvelope result{(*parsed)["protocol"].get<std::string>(),
                          (*parsed)["type"].get<std::string>(), *id, *sequence,
                          (*parsed)["payload_profile"].get<std::string>(), nullptr};
    RetireTree<OrderedJson> payload_cleanup{&result.payload};
    result.payload=core::detail::clone_json_as<OrderedJson>((*parsed)["payload"]);
    return result;
}

[[nodiscard]] auto ratio_json(const scenario::ExactRatio& ratio) -> OrderedJson {
    return {{"denominator", std::to_string(ratio.denominator())},
            {"numerator", std::to_string(ratio.numerator())}};
}

[[nodiscard]] auto statistics_json(const StatisticsComparisonPolicySource& source) -> OrderedJson {
    OrderedJson local = nullptr;
    if (source.local_practical_effect())
        local = ratio_json(*source.local_practical_effect());
    return {{"bootstrap_samples", std::to_string(source.bootstrap_samples())},
            {"comparison_confidence", ratio_json(source.comparison_confidence())},
            {"experiment_kind", source.experiment_kind() == StatisticsExperimentKind::comparison
                                    ? "comparison"
                                    : "matrix-comparison"},
            {"global_practical_effect", ratio_json(source.global_practical_effect())},
            {"local_practical_effect", std::move(local)}};
}

[[nodiscard]] auto parse_ratio(const Json& value) -> std::optional<scenario::ExactRatio> {
    if (!exact(value, {"numerator", "denominator"}) || !value["numerator"].is_string() ||
        !value["denominator"].is_string())
        return std::nullopt;
    const auto numerator = decimal(value["numerator"].get_ref<const std::string&>());
    const auto denominator = decimal(value["denominator"].get_ref<const std::string&>(), true);
    if (!numerator || !denominator)
        return std::nullopt;
    auto parsed = scenario::ExactRatio::create(*numerator, *denominator);
    if (!parsed)
        return std::nullopt;
    return *parsed;
}

[[nodiscard]] auto parse_statistics(const Json& value)
    -> std::optional<StatisticsComparisonPolicySource> {
    if (!exact(value, {"experiment_kind", "comparison_confidence", "bootstrap_samples",
                       "global_practical_effect", "local_practical_effect"}) ||
        !value["experiment_kind"].is_string() || !value["bootstrap_samples"].is_string())
        return std::nullopt;
    const auto kind_text = value["experiment_kind"].get_ref<const std::string&>();
    if (kind_text != "comparison" && kind_text != "matrix-comparison")
        return std::nullopt;
    auto confidence = parse_ratio(value["comparison_confidence"]);
    auto global = parse_ratio(value["global_practical_effect"]);
    auto samples = decimal(value["bootstrap_samples"].get_ref<const std::string&>(), true);
    std::optional<scenario::ExactRatio> local;
    if (!value["local_practical_effect"].is_null()) {
        local = parse_ratio(value["local_practical_effect"]);
        if (!local)
            return std::nullopt;
    }
    if (!confidence || !global || !samples)
        return std::nullopt;
    auto source = StatisticsComparisonPolicySource::create(
        kind_text == "comparison" ? StatisticsExperimentKind::comparison
                                  : StatisticsExperimentKind::matrix_comparison,
        *confidence, *samples, *global, local);
    if (!source)
        return std::nullopt;
    return *source;
}

[[nodiscard]] auto placement_profile(const ipc::PlacementRequest& request) -> std::string_view {
    switch (request.transport_version()) {
    case ipc::PlacementRequestTransportVersion::v2:
        return "epycbench.placement-request/v2";
    case ipc::PlacementRequestTransportVersion::v3:
        return "epycbench.placement-request/v3";
    case ipc::PlacementRequestTransportVersion::v4:
        return "epycbench.placement-request/v4";
    }
    return {};
}

[[nodiscard]] auto parse_pairwise_request(std::string_view body)
    -> std::expected<PairwiseHostCapacityRequestTranscript, Failure> {
    auto envelope_json = parse_json(body);
    RetireTree<Json> envelope_cleanup{envelope_json?&*envelope_json:nullptr};
    if (!envelope_json ||
        !exact(*envelope_json, {"protocol", "type", "request_id", "sequence",
                                "payload_profile", "payload"}))
        return std::unexpected{envelope_json ? Failure::invalid_envelope
                                             : envelope_json.error()};
    const auto& envelope = *envelope_json;
    if (!envelope["protocol"].is_string() || !core::detail::json_string_equal(envelope["protocol"], pairwise_v2_protocol) ||
        !envelope["type"].is_string() || !core::detail::json_string_equal(envelope["type"], "pairwise_request") ||
        !envelope["payload_profile"].is_string() ||
        !core::detail::json_string_equal(envelope["payload_profile"], pairwise_request_profile) ||
        !envelope["sequence"].is_string() || !core::detail::json_string_equal(envelope["sequence"], "0") ||
        !envelope["payload"].is_object())
        return std::unexpected{Failure::invalid_envelope};
    auto outer_id = request_id(envelope["request_id"]);
    const auto& payload = envelope["payload"];
    if (!outer_id ||
        !exact(payload, {"first_request_id", "second_request_id", "first", "second",
                         "resolved_comparison_order_seed", "statistics_source"}))
        return std::unexpected{Failure::invalid_envelope};
    auto first_id = request_id(payload["first_request_id"]);
    auto second_id = request_id(payload["second_request_id"]);
    if (!first_id || !second_id || *outer_id != *first_id || *first_id == *second_id ||
        !payload["resolved_comparison_order_seed"].is_string())
        return std::unexpected{Failure::request_binding_mismatch};
    const auto seed = decimal(
        payload["resolved_comparison_order_seed"].get_ref<const std::string&>());
    if (!seed)
        return std::unexpected{Failure::invalid_argument};
    const auto parse_variant = [](const Json& value) -> std::optional<ipc::PlacementRequest> {
        if (!exact(value, {"payload_profile", "payload"}) ||
            !value["payload_profile"].is_string() ||
            !core::detail::json_string_equal(value["payload_profile"], "epycbench.placement-request/v3") ||
            !value["payload"].is_object())
            return std::nullopt;
        auto parsed = ipc::parse_placement_request(value["payload"]);
        RetireTree<OrderedJson> parsed_cleanup{parsed?&const_cast<OrderedJson&>(parsed->wire()):nullptr};
        if (!parsed || parsed->transport_version() != ipc::PlacementRequestTransportVersion::v3)
            return std::nullopt;
        return std::move(*parsed);
    };
    auto first = parse_variant(payload["first"]);
    RetireTree<OrderedJson> first_cleanup{first?&const_cast<OrderedJson&>(first->wire()):nullptr};
    auto second = parse_variant(payload["second"]);
    RetireTree<OrderedJson> second_cleanup{second?&const_cast<OrderedJson&>(second->wire()):nullptr};
    auto statistics = parse_statistics(payload["statistics_source"]);
    if (!first || !second || !statistics)
        return std::unexpected{Failure::invalid_argument};
    std::string retained_body{body};
    const auto body_digest=core::sha256(body);
    return PairwiseHostCapacityRequestTranscriptConstruction::make(
        *first_id, *second_id, std::move(retained_body), body_digest, std::move(*first),
        std::move(*second), *seed, std::move(*statistics));
}

[[nodiscard]] auto provenance_token(
    platform_linux::detail::LinuxMonitoringCapacityProvenanceKind kind) noexcept
    -> std::string_view {
    switch (kind) {
    case platform_linux::detail::LinuxMonitoringCapacityProvenanceKind::scenario:
        return "scenario";
    case platform_linux::detail::LinuxMonitoringCapacityProvenanceKind::host_qualification_plan:
        return "host_qualification_plan";
    }
    return {};
}

[[nodiscard]] auto rejection_token(PairwiseHostCapacityRejectionReason reason) noexcept
    -> std::string_view {
    switch (reason) {
    case PairwiseHostCapacityRejectionReason::runner_authority_unavailable:
        return "runner_authority_unavailable";
    case PairwiseHostCapacityRejectionReason::request_binding_mismatch:
        return "request_binding_mismatch";
    case PairwiseHostCapacityRejectionReason::lower_schema_or_algorithm_mismatch:
        return "lower_schema_or_algorithm_mismatch";
    case PairwiseHostCapacityRejectionReason::lower_authority_mismatch:
        return "lower_authority_mismatch";
    case PairwiseHostCapacityRejectionReason::provenance_mismatch:
        return "provenance_mismatch";
    case PairwiseHostCapacityRejectionReason::topology_mismatch:
        return "topology_mismatch";
    case PairwiseHostCapacityRejectionReason::present_cpu_mismatch:
        return "present_cpu_mismatch";
    case PairwiseHostCapacityRejectionReason::capacity_contract_identity_mismatch:
        return "capacity_contract_identity_mismatch";
    case PairwiseHostCapacityRejectionReason::finalized_error_profile_identity_mismatch:
        return "finalized_error_profile_identity_mismatch";
    case PairwiseHostCapacityRejectionReason::pairwise_policy_mismatch:
        return "pairwise_policy_mismatch";
    case PairwiseHostCapacityRejectionReason::allocation:
        return "allocation";
    case PairwiseHostCapacityRejectionReason::contained_exception:
        return "contained_exception";
    case PairwiseHostCapacityRejectionReason::contained_unknown_failure:
        return "contained_unknown_failure";
    }
    return {};
}

[[nodiscard]] auto parse_rejection(std::string_view token)
    -> std::optional<PairwiseHostCapacityRejectionReason> {
    constexpr std::array values{
        PairwiseHostCapacityRejectionReason::runner_authority_unavailable,
        PairwiseHostCapacityRejectionReason::request_binding_mismatch,
        PairwiseHostCapacityRejectionReason::lower_schema_or_algorithm_mismatch,
        PairwiseHostCapacityRejectionReason::lower_authority_mismatch,
        PairwiseHostCapacityRejectionReason::provenance_mismatch,
        PairwiseHostCapacityRejectionReason::topology_mismatch,
        PairwiseHostCapacityRejectionReason::present_cpu_mismatch,
        PairwiseHostCapacityRejectionReason::capacity_contract_identity_mismatch,
        PairwiseHostCapacityRejectionReason::finalized_error_profile_identity_mismatch,
        PairwiseHostCapacityRejectionReason::pairwise_policy_mismatch,
        PairwiseHostCapacityRejectionReason::allocation,
        PairwiseHostCapacityRejectionReason::contained_exception,
        PairwiseHostCapacityRejectionReason::contained_unknown_failure};
    const auto found = std::ranges::find_if(
        values, [&](auto value) { return rejection_token(value) == token; });
    return found == values.end() ? std::nullopt : std::optional{*found};
}

[[nodiscard]] auto interrupted(const core::Error& error) noexcept -> bool {
    return error.code.value() == "runtime.ipc.io_failure" && error.context.is_object() &&
           error.context.value("classification", "") == "interrupted";
}

[[nodiscard]] auto map_admission_rejection(
    PairwiseMonitoringHostCapacityAdmissionFailure failure) noexcept
    -> PairwiseHostCapacityRejectionReason {
    using Rejection = PairwiseHostCapacityRejectionReason;
    switch (failure) {
    case PairwiseMonitoringHostCapacityAdmissionFailure::capacity_contract_identity_mismatch:
        return Rejection::capacity_contract_identity_mismatch;
    case PairwiseMonitoringHostCapacityAdmissionFailure::finalized_error_profile_identity_mismatch:
        return Rejection::finalized_error_profile_identity_mismatch;
    case PairwiseMonitoringHostCapacityAdmissionFailure::allocation:
        return Rejection::allocation;
    case PairwiseMonitoringHostCapacityAdmissionFailure::contained_exception:
        return Rejection::contained_exception;
    case PairwiseMonitoringHostCapacityAdmissionFailure::contained_unknown_failure:
        return Rejection::contained_unknown_failure;
    case PairwiseMonitoringHostCapacityAdmissionFailure::controller_unbound:
    case PairwiseMonitoringHostCapacityAdmissionFailure::runner_unbound:
        return Rejection::runner_authority_unavailable;
    case PairwiseMonitoringHostCapacityAdmissionFailure::lower_authority_mismatch:
        return Rejection::lower_authority_mismatch;
    case PairwiseMonitoringHostCapacityAdmissionFailure::provenance_mismatch:
        return Rejection::provenance_mismatch;
    case PairwiseMonitoringHostCapacityAdmissionFailure::topology_mismatch:
        return Rejection::topology_mismatch;
    case PairwiseMonitoringHostCapacityAdmissionFailure::present_cpu_mismatch:
        return Rejection::present_cpu_mismatch;
    case PairwiseMonitoringHostCapacityAdmissionFailure::worker_order_invalid:
    case PairwiseMonitoringHostCapacityAdmissionFailure::worker_membership_invalid:
    case PairwiseMonitoringHostCapacityAdmissionFailure::pairwise_policy_mismatch:
    case PairwiseMonitoringHostCapacityAdmissionFailure::invalid_input:
        return Rejection::pairwise_policy_mismatch;
    }
    return Rejection::contained_unknown_failure;
}

[[nodiscard]] auto map_lower_mint_rejection(
    const platform_linux::detail::LinuxMonitoringCapacityFailure& failure) noexcept
    -> PairwiseHostCapacityRejectionReason {
    using Kind = platform_linux::detail::LinuxMonitoringCapacityFailureKind;
    switch (failure.primary) {
    case Kind::allocation:
        return PairwiseHostCapacityRejectionReason::allocation;
    case Kind::contained_exception:
        return PairwiseHostCapacityRejectionReason::contained_exception;
    case Kind::contained_unknown_failure:
        return PairwiseHostCapacityRejectionReason::contained_unknown_failure;
    default:
        return PairwiseHostCapacityRejectionReason::runner_authority_unavailable;
    }
}

} // namespace

ValidatedRemoteCapacityCommitment::ValidatedRemoteCapacityCommitment(
    ValidatedRemoteCapacityCommitment&& other) noexcept
    : first_{other.first_}, second_{other.second_}, request_{other.request_}, offer_{other.offer_},
      capacity_{other.capacity_}, profile_{other.profile_}, first_wire_{other.first_wire_},
      second_wire_{other.second_wire_}, seed_{other.seed_},
      statistics_{std::move(other.statistics_)}, binding_{std::move(other.binding_)},
      active_{std::exchange(other.active_, false)} {}

auto ValidatedRemoteCapacityCommitment::consume_for_projection(
    const AdmittedPairwiseProductionInputs& inputs) noexcept -> ProjectionConsumption {
    if (!std::exchange(active_, false))
        return ProjectionConsumption::mismatch;
    try {
        const auto first_wire = core::hash_canonical_json(
            inputs.pairwise_request().first().request().wire());
        const auto second_wire = core::hash_canonical_json(
            inputs.pairwise_request().second().request().wire());
        const auto matches = first_wire && second_wire && *first_wire == first_wire_ &&
                             *second_wire == second_wire_ &&
                             inputs.pairwise_request().first().request_id() == first_ &&
                             inputs.pairwise_request().second().request_id() == second_ &&
                             inputs.pairwise_request().resolved_comparison_order_seed() == seed_;
        return matches ? ProjectionConsumption::accepted : ProjectionConsumption::mismatch;
    } catch (const std::bad_alloc&) {
        return ProjectionConsumption::allocation;
    } catch (const std::exception&) {
        return ProjectionConsumption::contained_exception;
    } catch (...) {
        return ProjectionConsumption::contained_unknown_failure;
    }
}

PreparedPairwiseHostCapacityOffer::PreparedPairwiseHostCapacityOffer(
    PreparedPairwiseHostCapacityOffer&& other) noexcept
    : body_{std::move(other.body_)}, body_sha256_{other.body_sha256_}, first_{other.first_},
      second_{other.second_}, request_sha256_{other.request_sha256_},
      lower_identity_{other.lower_identity_}, capacity_identity_{other.capacity_identity_},
      profile_identity_{other.profile_identity_},
      active_{std::exchange(other.active_, false)} {}

HostCapacityAcceptedAckCapability::HostCapacityAcceptedAckCapability(
    HostCapacityAcceptedAckCapability&& other) noexcept
    : first_{other.first_}, second_{other.second_}, request_{other.request_}, offer_{other.offer_},
      lower_{other.lower_}, capacity_{other.capacity_}, profile_{other.profile_},
      active_{std::exchange(other.active_, false)} {}

HostCapacityRejectedAckCapability::HostCapacityRejectedAckCapability(
    HostCapacityRejectedAckCapability&& other) noexcept
    : first_{other.first_}, second_{other.second_}, request_{other.request_},
      offer_{other.offer_}, reason_{other.reason_},
      active_{std::exchange(other.active_, false)} {}

auto PairwiseMonitoringHostCapacityStartupAccess::prepare_request(
    ipc::RequestId first_id, ipc::RequestId second_id, const ipc::PlacementRequest& first,
    const ipc::PlacementRequest& second, std::uint64_t resolved_comparison_order_seed,
    const StatisticsComparisonPolicySource& statistics) noexcept
    -> std::expected<PairwiseHostCapacityRequestTranscript, Failure> {
    try {
        if (first_id == second_id || first.transport_version() != ipc::PlacementRequestTransportVersion::v3 ||
            second.transport_version() != ipc::PlacementRequestTransportVersion::v3)
            return std::unexpected{Failure::invalid_argument};
        OrderedJson payload{
            {"first_request_id", first_id.text()},
            {"second_request_id", second_id.text()},
            {"first", {{"payload_profile", placement_profile(first)}, {"payload", first.wire()}}},
            {"second", {{"payload_profile", placement_profile(second)}, {"payload", second.wire()}}},
            {"resolved_comparison_order_seed", std::to_string(resolved_comparison_order_seed)},
            {"statistics_source", statistics_json(statistics)}};
        Json envelope{{"payload", std::move(payload)},
                      {"payload_profile", pairwise_request_profile},
                      {"protocol", pairwise_v2_protocol},
                      {"request_id", first_id.text()},
                      {"sequence", "0"},
                      {"type", "pairwise_request"}};
        auto canonical = core::canonicalize_json(envelope);
        if (!canonical)
            return std::unexpected{Failure::invalid_encoding};
        if (canonical->size() > ipc::maximum_frame_body_size)
            return std::unexpected{Failure::frame_over_limit};
        return parse_pairwise_request(*canonical);
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::parse_request(
    std::string_view canonical_body) noexcept
    -> std::expected<PairwiseHostCapacityRequestTranscript, Failure> {
    try {
        return parse_pairwise_request(canonical_body);
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::prepare_offer(
    const PairwiseHostCapacityRequestTranscript& request,
    const LowerAuthority& controller) noexcept
    -> std::expected<PreparedPairwiseHostCapacityOffer, Failure> {
    try {
        if (!controller.authentically_bound() ||
            controller.provenance().kind() !=
                platform_linux::detail::LinuxMonitoringCapacityProvenanceKind::scenario)
            return std::unexpected{Failure::invalid_argument};
        const auto capacity_identity =
            PairwiseMonitoringHostCapacityAdmissionAccess::expected_capacity_identity();
        const auto profile_identity =
            PairwiseMonitoringHostCapacityAdmissionAccess::expected_profile_identity();
        OrderedJson cpus = OrderedJson::array();
        for (const auto cpu : controller.present_cpus())
            cpus.push_back(std::to_string(cpu.value()));
        OrderedJson payload{
            {"base_page_bytes", std::to_string(controller.base_page_bytes().value())},
            {"capacity_contract_sha256", capacity_identity.to_hex()},
            {"effective_topology_fingerprint", controller.effective_fingerprint().to_string()},
            {"finalized_error_profile_sha256", profile_identity.to_hex()},
            {"first_request_id", request.first_request_id().text()},
            {"kernel_maximum_cpu", std::to_string(controller.kernel_maximum_cpu())},
            {"lower_algorithm", platform_linux::detail::linux_monitoring_capacity_algorithm},
            {"lower_identity_sha256", controller.identity().to_hex()},
            {"lower_schema", platform_linux::detail::linux_monitoring_capacity_schema},
            {"pairwise_request_body_sha256", request.body_sha256().to_hex()},
            {"physical_topology_fingerprint", controller.physical_fingerprint().to_string()},
            {"present_cpus", std::move(cpus)},
            {"provenance",
             {{"authority_sha256", controller.provenance().authority_digest().to_hex()},
              {"kind", provenance_token(controller.provenance().kind())}}},
            {"second_request_id", request.second_request_id().text()}};
        auto body = encode_envelope("host_capacity_offer", request.first_request_id(),
                                    pairwise_host_capacity_offer_profile, std::move(payload));
        if (!body)
            return std::unexpected{body.error()};
        const auto body_digest = core::sha256(*body);
        return PreparedPairwiseHostCapacityOffer{
            std::move(*body), body_digest, request.first_request_id(), request.second_request_id(),
            request.body_sha256(), controller.identity(), capacity_identity, profile_identity};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::author_offer(
    PreparedPairwiseHostCapacityOffer offer) noexcept
    -> std::expected<AuthoredPairwiseHostCapacityOffer, Failure> {
    try {
        if (!std::exchange(offer.active_, false))
            return std::unexpected{Failure::invalid_argument};
        PairwiseHostCapacityOfferValidationState validation{
            offer.body_sha256_, offer.first_, offer.second_, offer.request_sha256_,
            offer.lower_identity_, offer.capacity_identity_, offer.profile_identity_};
        return AuthoredPairwiseHostCapacityOffer{std::move(offer.body_),
                                                 std::move(validation)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::parse_offer(
    std::string_view canonical_body) noexcept
    -> std::expected<TransportedLinuxMonitoringCapacityCommitment, Failure> {
    try {
        auto envelope = parse_envelope(canonical_body);
        RetireTree<OrderedJson> envelope_cleanup{envelope?&envelope->payload:nullptr};
        if (!envelope)
            return std::unexpected{envelope.error()};
        if (envelope->protocol != pairwise_host_capacity_startup_protocol ||
            envelope->type != "host_capacity_offer" || envelope->sequence != 0U ||
            envelope->profile != pairwise_host_capacity_offer_profile)
            return std::unexpected{Failure::unexpected_message};
        const auto& payload = envelope->payload;
        if (!exact(payload,
                   {"base_page_bytes", "capacity_contract_sha256",
                    "effective_topology_fingerprint", "finalized_error_profile_sha256",
                    "first_request_id", "kernel_maximum_cpu", "lower_algorithm",
                    "lower_identity_sha256", "lower_schema", "pairwise_request_body_sha256",
                    "physical_topology_fingerprint", "present_cpus", "provenance",
                    "second_request_id"}) ||
            !payload["base_page_bytes"].is_string() ||
            !payload["kernel_maximum_cpu"].is_string() || !payload["lower_schema"].is_string() ||
            !payload["lower_algorithm"].is_string() || !payload["present_cpus"].is_array() ||
            !exact(payload["provenance"], {"authority_sha256", "kind"}) ||
            !payload["provenance"]["kind"].is_string())
            return std::unexpected{Failure::invalid_argument};
        const auto lower_contract_matches =
            payload["lower_schema"] ==
                platform_linux::detail::linux_monitoring_capacity_schema &&
            payload["lower_algorithm"] ==
                platform_linux::detail::linux_monitoring_capacity_algorithm;
        const auto provenance_kind_text =
            payload["provenance"]["kind"].get_ref<const std::string&>();
        if (provenance_kind_text != "scenario" &&
            provenance_kind_text != "host_qualification_plan")
            return std::unexpected{Failure::invalid_argument};
        const auto provenance_kind =
            provenance_kind_text == "scenario"
                ? platform_linux::detail::LinuxMonitoringCapacityProvenanceKind::scenario
                : platform_linux::detail::LinuxMonitoringCapacityProvenanceKind::
                      host_qualification_plan;
        auto first = request_id(payload["first_request_id"]);
        auto second = request_id(payload["second_request_id"]);
        auto page = decimal(payload["base_page_bytes"].get_ref<const std::string&>(), true);
        auto maximum = decimal(payload["kernel_maximum_cpu"].get_ref<const std::string&>());
        auto physical = payload["physical_topology_fingerprint"].is_string()
                            ? topology::PhysicalTopologyFingerprint::parse(
                                  payload["physical_topology_fingerprint"]
                                      .get_ref<const std::string&>())
                            : std::expected<topology::PhysicalTopologyFingerprint, core::Error>{
                                  std::unexpected{core::Error{}}};
        auto effective = payload["effective_topology_fingerprint"].is_string()
                             ? topology::EffectiveTopologyFingerprint::parse(
                                   payload["effective_topology_fingerprint"]
                                       .get_ref<const std::string&>())
                             : std::expected<topology::EffectiveTopologyFingerprint, core::Error>{
                                   std::unexpected{core::Error{}}};
        auto provenance = digest(payload["provenance"]["authority_sha256"]);
        auto lower = digest(payload["lower_identity_sha256"]);
        auto capacity = digest(payload["capacity_contract_sha256"]);
        auto profile = digest(payload["finalized_error_profile_sha256"]);
        auto request = digest(payload["pairwise_request_body_sha256"]);
        if (!first || !second || *first != envelope->request_id || *first == *second || !page ||
            !maximum || *maximum > std::numeric_limits<std::uint32_t>::max() || !physical ||
            !effective || !provenance || !lower || !capacity || !profile || !request)
            return std::unexpected{Failure::invalid_argument};
        std::vector<core::LinuxCpuId> present;
        present.reserve(payload["present_cpus"].size());
        std::optional<std::uint32_t> prior;
        for (const auto& item : payload["present_cpus"]) {
            if (!item.is_string())
                return std::unexpected{Failure::invalid_argument};
            auto value = decimal(item.get_ref<const std::string&>());
            if (!value || *value > *maximum ||
                *value > std::numeric_limits<std::uint32_t>::max() ||
                (prior && *value <= *prior))
                return std::unexpected{Failure::invalid_argument};
            prior = static_cast<std::uint32_t>(*value);
            present.emplace_back(*prior);
        }
        if (present.empty())
            return std::unexpected{Failure::invalid_argument};
        return TransportedLinuxMonitoringCapacityCommitment{
            *first, *second, core::ByteCount::from_base_units(*page),
            static_cast<std::uint32_t>(*maximum), std::move(present), std::move(*physical),
            std::move(*effective),
            provenance_kind, *provenance, *lower, *capacity, *profile, *request,
            core::sha256(canonical_body), lower_contract_matches};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::mint_controller(
    const PairwiseHostCapacityRequestTranscript& request,
    const RunnerContext& context) noexcept
    -> std::expected<LowerAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    return PairwiseMonitoringHostCapacityAdmissionAccess::mint_request(
        context, request.first());
}

auto PairwiseMonitoringHostCapacityStartupAccess::mint_controller_with(
    const PairwiseHostCapacityRequestTranscript& request,
    const RunnerContext& context, LowerOperations operations) noexcept
    -> std::expected<LowerAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    return PairwiseMonitoringHostCapacityAdmissionAccess::mint_request_with(
        context, request.first(), operations);
}

auto PairwiseMonitoringHostCapacityStartupAccess::mint_runner(
    const TransportedLinuxMonitoringCapacityCommitment& commitment,
    const PairwiseHostCapacityRequestTranscript& request,
    const RunnerContext& context) noexcept
    -> std::expected<LowerAuthority, HostCapacityRejectedAckCapability> {
    auto result = PairwiseMonitoringHostCapacityAdmissionAccess::mint_request(
        context, request.first());
    if (!result)
        return std::unexpected{HostCapacityRejectedAckCapability{
            commitment.first_, commitment.second_, commitment.request_sha256_,
            commitment.offer_sha256_, map_lower_mint_rejection(result.error())}};
    return std::move(*result);
}

auto PairwiseMonitoringHostCapacityStartupAccess::mint_runner_with(
    const TransportedLinuxMonitoringCapacityCommitment& commitment,
    const PairwiseHostCapacityRequestTranscript& request,
    const RunnerContext& context, LowerOperations operations) noexcept
    -> std::expected<LowerAuthority, HostCapacityRejectedAckCapability> {
    auto result = PairwiseMonitoringHostCapacityAdmissionAccess::mint_request_with(
        context, request.first(), operations);
    if (!result)
        return std::unexpected{HostCapacityRejectedAckCapability{
            commitment.first_, commitment.second_, commitment.request_sha256_,
            commitment.offer_sha256_, map_lower_mint_rejection(result.error())}};
    return std::move(*result);
}

auto PairwiseMonitoringHostCapacityStartupAccess::gate_a(
    const TransportedLinuxMonitoringCapacityCommitment& commitment,
    const PairwiseHostCapacityRequestTranscript& request,
    const LowerAuthority& runner) noexcept
    -> std::expected<ValidatedRemoteCapacityCommitment,
                     HostCapacityRejectedAckCapability> {
    const auto reject = [&](PairwiseHostCapacityRejectionReason reason)
        -> std::expected<ValidatedRemoteCapacityCommitment,
                         HostCapacityRejectedAckCapability> {
        return std::unexpected{HostCapacityRejectedAckCapability{
            commitment.first_, commitment.second_, commitment.request_sha256_,
            commitment.offer_sha256_, reason}};
    };
    try {
        if (!runner.authentically_bound())
            return reject(PairwiseHostCapacityRejectionReason::runner_authority_unavailable);
        if (commitment.first_ != request.first_request_id() ||
            commitment.second_ != request.second_request_id() ||
            commitment.request_sha256_ != request.body_sha256())
            return reject(PairwiseHostCapacityRejectionReason::request_binding_mismatch);
        if (!commitment.lower_contract_matches_)
            return reject(
                PairwiseHostCapacityRejectionReason::lower_schema_or_algorithm_mismatch);
        if (commitment.provenance_kind_ != runner.provenance().kind() ||
            commitment.provenance_ != runner.provenance().authority_digest())
            return reject(PairwiseHostCapacityRejectionReason::provenance_mismatch);
        if (commitment.physical_ != runner.physical_fingerprint() ||
            commitment.effective_ != runner.effective_fingerprint())
            return reject(PairwiseHostCapacityRejectionReason::topology_mismatch);
        if (!std::ranges::equal(commitment.present_, runner.present_cpus()))
            return reject(PairwiseHostCapacityRejectionReason::present_cpu_mismatch);
        if (commitment.page_ != runner.base_page_bytes() ||
            commitment.kernel_maximum_ != runner.kernel_maximum_cpu() ||
            commitment.lower_identity_ != runner.identity())
            return reject(PairwiseHostCapacityRejectionReason::lower_authority_mismatch);
        auto first_wire = core::hash_canonical_json(request.first().wire());
        auto second_wire = core::hash_canonical_json(request.second().wire());
        if (!first_wire || !second_wire)
            return reject(PairwiseHostCapacityRejectionReason::contained_exception);
        return ValidatedRemoteCapacityCommitment{
            commitment.first_, commitment.second_, commitment.request_sha256_,
            commitment.offer_sha256_, commitment.capacity_identity_, commitment.profile_identity_,
            *first_wire, *second_wire, request.resolved_comparison_order_seed(),
            request.statistics_source(),
            PairwiseMonitoringHostCapacityAdmissionAccess::binding_seal(runner)};
    } catch (const std::bad_alloc&) {
        return reject(PairwiseHostCapacityRejectionReason::allocation);
    } catch (const std::exception&) {
        return reject(PairwiseHostCapacityRejectionReason::contained_exception);
    } catch (...) {
        return reject(PairwiseHostCapacityRejectionReason::contained_unknown_failure);
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::gate_b(
    const AdmittedPairwiseProductionInputs& inputs, ValidatedRemoteCapacityCommitment proof,
    LowerAuthority runner) noexcept
    -> std::expected<AdmittedPairwiseHostCapacityStartup,
                     HostCapacityRejectedAckCapability> {
    const auto first = proof.first_;
    const auto second = proof.second_;
    const auto request = proof.request_;
    const auto offer = proof.offer_;
    const auto capacity = proof.capacity_;
    const auto profile = proof.profile_;
    const auto consumption = proof.consume_for_projection(inputs);
    if (consumption != ValidatedRemoteCapacityCommitment::ProjectionConsumption::accepted) {
        const auto reason = [&] {
            using Consumption = ValidatedRemoteCapacityCommitment::ProjectionConsumption;
            switch (consumption) {
            case Consumption::allocation:
                return PairwiseHostCapacityRejectionReason::allocation;
            case Consumption::contained_exception:
                return PairwiseHostCapacityRejectionReason::contained_exception;
            case Consumption::contained_unknown_failure:
                return PairwiseHostCapacityRejectionReason::contained_unknown_failure;
            case Consumption::accepted:
            case Consumption::mismatch:
                return PairwiseHostCapacityRejectionReason::pairwise_policy_mismatch;
            }
            return PairwiseHostCapacityRejectionReason::contained_unknown_failure;
        }();
        return std::unexpected{HostCapacityRejectedAckCapability{
            first, second, request, offer, reason}};
    }
    if (!PairwiseMonitoringHostCapacityAdmissionAccess::statistics_match(
            inputs, proof.statistics_))
        return std::unexpected{HostCapacityRejectedAckCapability{
            first, second, request, offer,
            PairwiseHostCapacityRejectionReason::pairwise_policy_mismatch}};
    if (!runner.authentically_bound())
        return std::unexpected{HostCapacityRejectedAckCapability{
            first, second, request, offer,
            PairwiseHostCapacityRejectionReason::runner_authority_unavailable}};
    if (!PairwiseMonitoringHostCapacityAdmissionAccess::binding_matches(
            proof.binding_, runner))
        return std::unexpected{HostCapacityRejectedAckCapability{
            first, second, request, offer,
            PairwiseHostCapacityRejectionReason::lower_authority_mismatch}};
    auto projected = PairwiseMonitoringHostCapacityAdmissionAccess::
        project_after_lower_validation(inputs, std::move(runner), capacity, profile, {});
    if (!projected)
        return std::unexpected{HostCapacityRejectedAckCapability{
            first, second, request, offer, map_admission_rejection(projected.error())}};
    return AdmittedPairwiseHostCapacityStartup{std::move(*projected), first, second, request,
                                               offer, capacity, profile,
                                               inputs.pairwise_request().first().placement_identity(),
                                               inputs.pairwise_request().second().placement_identity()};
}

auto PairwiseMonitoringHostCapacityStartupAccess::issue_accepted_ack(
    AdmittedPairwiseHostCapacityStartup& admitted) noexcept
    -> std::expected<HostCapacityAcceptedAckCapability, Failure> {
    if (std::exchange(admitted.ack_issued_, true))
        return std::unexpected{Failure::acknowledgement_already_issued};
    return HostCapacityAcceptedAckCapability{
        admitted.first_, admitted.second_, admitted.request_, admitted.offer_,
        admitted.admission_.authority().identity(), admitted.capacity_, admitted.profile_};
}

auto PairwiseMonitoringHostCapacityStartupAccess::encode_accepted_ack(
    HostCapacityAcceptedAckCapability capability) noexcept
    -> std::expected<std::string, Failure> {
    if (!std::exchange(capability.active_, false))
        return std::unexpected{Failure::acknowledgement_already_issued};
    return encode_envelope(
        "host_capacity_accepted", capability.first_, pairwise_host_capacity_accepted_profile,
        {{"accepted", true},
         {"capacity_contract_sha256", capability.capacity_.to_hex()},
         {"finalized_error_profile_sha256", capability.profile_.to_hex()},
         {"first_request_id", capability.first_.text()},
         {"lower_identity_sha256", capability.lower_.to_hex()},
         {"offer_body_sha256", capability.offer_.to_hex()},
         {"pairwise_request_body_sha256", capability.request_.to_hex()},
         {"second_request_id", capability.second_.text()}});
}

auto PairwiseMonitoringHostCapacityStartupAccess::parse_accepted_ack(
    std::string_view body) noexcept -> std::expected<ParsedHostCapacityAcceptedAck, Failure> {
    try {
        auto envelope = parse_envelope(body);
        RetireTree<OrderedJson> envelope_cleanup{envelope?&envelope->payload:nullptr};
        if (!envelope)
            return std::unexpected{envelope.error()};
        if (envelope->protocol != pairwise_host_capacity_startup_protocol ||
            envelope->type != "host_capacity_accepted" || envelope->sequence != 0U ||
            envelope->profile != pairwise_host_capacity_accepted_profile)
            return std::unexpected{Failure::unexpected_message};
        const auto& payload = envelope->payload;
        if (!exact(payload, {"accepted", "capacity_contract_sha256",
                             "finalized_error_profile_sha256", "first_request_id",
                             "lower_identity_sha256", "offer_body_sha256",
                             "pairwise_request_body_sha256", "second_request_id"}) ||
            !payload["accepted"].is_boolean() || !payload["accepted"].get<bool>())
            return std::unexpected{Failure::invalid_argument};
        auto first = request_id(payload["first_request_id"]);
        auto second = request_id(payload["second_request_id"]);
        auto request = digest(payload["pairwise_request_body_sha256"]);
        auto offer = digest(payload["offer_body_sha256"]);
        auto lower = digest(payload["lower_identity_sha256"]);
        auto capacity = digest(payload["capacity_contract_sha256"]);
        auto profile = digest(payload["finalized_error_profile_sha256"]);
        if (!first || !second || *first != envelope->request_id || *first == *second || !request ||
            !offer || !lower || !capacity || !profile)
            return std::unexpected{Failure::invalid_argument};
        return ParsedHostCapacityAcceptedAck{*first, *second, *request, *offer, *lower, *capacity,
                                             *profile};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::validate_accepted_ack(
    const PairwiseHostCapacityOfferValidationState& offer, const LowerAuthority& controller,
    const ParsedHostCapacityAcceptedAck& ack) noexcept -> std::expected<void, Failure> {
    if (!controller.authentically_bound() || ack.first != offer.first_ ||
        ack.second != offer.second_ || ack.request != offer.request_sha256_ ||
        ack.offer != offer.body_sha256_ || ack.lower != controller.identity() ||
        ack.lower != offer.lower_identity_)
        return std::unexpected{Failure::lower_authority_mismatch};
    if (ack.capacity != offer.capacity_identity_)
        return std::unexpected{Failure::capacity_contract_identity_mismatch};
    if (ack.profile != offer.profile_identity_)
        return std::unexpected{Failure::finalized_error_profile_identity_mismatch};
    return {};
}

auto PairwiseMonitoringHostCapacityStartupAccess::encode_rejected_ack(
    HostCapacityRejectedAckCapability capability) noexcept
    -> std::expected<std::string, Failure> {
    try {
        if (!std::exchange(capability.active_, false))
            return std::unexpected{Failure::acknowledgement_already_issued};
        return encode_envelope(
            "host_capacity_rejected", capability.first_,
            pairwise_host_capacity_rejected_profile,
            {{"accepted", false},
             {"first_request_id", capability.first_.text()},
             {"offer_body_sha256", capability.offer_.to_hex()},
             {"pairwise_request_body_sha256", capability.request_.to_hex()},
             {"reason", rejection_token(capability.reason_)},
             {"second_request_id", capability.second_.text()}});
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::parse_rejected_ack(
    std::string_view body) noexcept -> std::expected<ParsedHostCapacityRejectedAck, Failure> {
    try {
        auto envelope = parse_envelope(body);
        RetireTree<OrderedJson> envelope_cleanup{envelope?&envelope->payload:nullptr};
        if (!envelope)
            return std::unexpected{envelope.error()};
        if (envelope->protocol != pairwise_host_capacity_startup_protocol ||
            envelope->type != "host_capacity_rejected" || envelope->sequence != 0U ||
            envelope->profile != pairwise_host_capacity_rejected_profile)
            return std::unexpected{Failure::unexpected_message};
        const auto& payload = envelope->payload;
        if (!exact(payload, {"accepted", "first_request_id", "offer_body_sha256",
                             "pairwise_request_body_sha256", "reason", "second_request_id"}) ||
            !payload["accepted"].is_boolean() || payload["accepted"].get<bool>() ||
            !payload["reason"].is_string())
            return std::unexpected{Failure::invalid_argument};
        auto first = request_id(payload["first_request_id"]);
        auto second = request_id(payload["second_request_id"]);
        auto request = digest(payload["pairwise_request_body_sha256"]);
        auto offer = digest(payload["offer_body_sha256"]);
        auto reason = parse_rejection(payload["reason"].get_ref<const std::string&>());
        if (!first || !second || *first != envelope->request_id || *first == *second || !request ||
            !offer || !reason)
            return std::unexpected{Failure::invalid_argument};
        return ParsedHostCapacityRejectedAck{*first, *second, *request, *offer, *reason};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::dispatch_initial_protocol(
    std::string_view body) noexcept -> std::expected<PairwiseInitialProtocol, Failure> {
    try {
        auto parsed = parse_json(body);
        RetireTree<Json> parsed_cleanup{parsed?&*parsed:nullptr};
        if (!parsed || !parsed->is_object() || !parsed->contains("protocol") ||
            !(*parsed)["protocol"].is_string())
            return std::unexpected{parsed ? Failure::invalid_envelope : parsed.error()};
        const auto& protocol = (*parsed)["protocol"].get_ref<const std::string&>();
        if (protocol == ipc::protocol_profile)
            return PairwiseInitialProtocol::legacy_v1;
        if (protocol == pairwise_host_capacity_startup_protocol)
            return PairwiseInitialProtocol::host_capacity_startup;
        return std::unexpected{Failure::unsupported_protocol};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::encode_frame(
    std::string_view body) noexcept -> std::expected<std::vector<std::byte>, Failure> {
    try {
        if (body.empty())
            return std::unexpected{Failure::frame_zero};
        if (body.size() > ipc::maximum_frame_body_size)
            return std::unexpected{Failure::frame_over_limit};
        const auto size = static_cast<std::uint32_t>(body.size());
        std::vector<std::byte> frame(4U + body.size());
        for (std::size_t index{}; index < 4U; ++index)
            frame[index] = std::byte{static_cast<unsigned char>(size >> (24U - 8U * index))};
        std::transform(body.begin(), body.end(), frame.begin() + 4,
                       [](char value) { return std::byte{static_cast<unsigned char>(value)}; });
        return frame;
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::read_frame(ipc::ByteReader& reader) noexcept
    -> std::expected<std::optional<std::string>, Failure> {
    try {
        const auto fill = [&](std::span<std::byte> output, bool clean_eof)
            -> std::expected<bool, Failure> {
            std::size_t offset{};
            while (offset < output.size()) {
                auto count = reader.read(output.subspan(offset));
                if (!count) {
                    if (interrupted(count.error()))
                        continue;
                    return std::unexpected{Failure::io_failure};
                }
                if (*count > output.size() - offset)
                    return std::unexpected{Failure::io_failure};
                if (*count == 0U) {
                    if (clean_eof && offset == 0U)
                        return false;
                    return std::unexpected{Failure::truncated_frame};
                }
                offset += *count;
            }
            return true;
        };
        std::array<std::byte, 4> header{};
        auto header_read = fill(header, true);
        if (!header_read)
            return std::unexpected{header_read.error()};
        if (!*header_read)
            return std::optional<std::string>{};
        const auto size = (std::to_integer<std::uint32_t>(header[0]) << 24U) |
                          (std::to_integer<std::uint32_t>(header[1]) << 16U) |
                          (std::to_integer<std::uint32_t>(header[2]) << 8U) |
                          std::to_integer<std::uint32_t>(header[3]);
        if (size == 0U)
            return std::unexpected{Failure::frame_zero};
        if (size > ipc::maximum_frame_body_size)
            return std::unexpected{Failure::frame_over_limit};
        std::vector<std::byte> bytes(size);
        auto body_read = fill(bytes, false);
        if (!body_read)
            return std::unexpected{body_read.error()};
        std::string body(size, '\0');
        std::transform(bytes.begin(), bytes.end(), body.begin(), [](std::byte value) {
            return static_cast<char>(std::to_integer<unsigned char>(value));
        });
        return std::optional{std::move(body)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Failure::allocation};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

auto PairwiseMonitoringHostCapacityStartupAccess::write_frame(
    ipc::ByteWriter& writer, std::string_view body) noexcept -> std::expected<void, Failure> {
    auto frame = encode_frame(body);
    if (!frame)
        return std::unexpected{frame.error()};
    std::size_t offset{};
    while (offset < frame->size()) {
        auto count = writer.write(std::span<const std::byte>{*frame}.subspan(offset));
        if (!count) {
            if (interrupted(count.error()))
                continue;
            return std::unexpected{Failure::io_failure};
        }
        if (*count == 0U || *count > frame->size() - offset)
            return std::unexpected{Failure::io_failure};
        offset += *count;
    }
    return {};
}

} // namespace epycbench::runtime::detail
