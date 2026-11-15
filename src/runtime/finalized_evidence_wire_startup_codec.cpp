#include "runtime/finalized_evidence_wire_startup_codec.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "core/json_ownership.hpp"
#include "runtime/allocation_free_json_retirement.hpp"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>
#include <ranges>

namespace epycbench::runtime::wire_v1::detail {
namespace {
using Json = nlohmann::json;
struct RetireTree final {
    Json* value;
    ~RetireTree(){if(value)runtime::detail::retire_json(*value);}
};
// Preserve the former DOM parser's last-member behavior; byte equality below
// still rejects duplicate members as noncanonical. Every partial tree is owned.
struct CanonicalBuilder final : nlohmann::json_sax<Json> {
    struct Frame {Json* value;std::string key;};
    Json root;std::vector<Frame> frames;
    ~CanonicalBuilder(){runtime::detail::retire_json(root);}
    auto destination()->Json& {
        if(frames.empty())return root;
        auto& frame=frames.back();
        if(frame.value->is_array()){frame.value->push_back(nullptr);return frame.value->back();}
        auto& value=(*frame.value)[frame.key];runtime::detail::retire_json(value);return value;
    }
    template<class Value> auto scalar(Value&& value)->bool {destination()=std::forward<Value>(value);return true;}
    auto null()->bool override{return scalar(nullptr);}
    auto boolean(bool value)->bool override{return scalar(value);}
    auto number_integer(number_integer_t value)->bool override{return scalar(value);}
    auto number_unsigned(number_unsigned_t value)->bool override{return scalar(value);}
    auto number_float(number_float_t value,const string_t&)->bool override{return scalar(value);}
    auto string(string_t& value)->bool override{return scalar(std::move(value));}
    auto binary(binary_t& value)->bool override{return scalar(std::move(value));}
    auto start(bool object)->bool{auto& value=destination();value=object?Json::object():Json::array();frames.push_back({&value,{}});return true;}
    auto start_object(std::size_t)->bool override{return start(true);}
    auto start_array(std::size_t)->bool override{return start(false);}
    auto key(string_t& value)->bool override{frames.back().key=std::move(value);return true;}
    auto end_object()->bool override{frames.pop_back();return true;}
    auto end_array()->bool override{frames.pop_back();return true;}
    auto parse_error(std::size_t,const std::string&,const nlohmann::detail::exception&)->bool override{return false;}
};

struct Shape final { std::uint64_t nodes{}, depth{}; };
auto shape(const Json& value, std::uint64_t depth = 1U) -> Shape {
    Shape result{1U, depth};
    if (value.is_object()) for (const auto& item : value.items()) {
        const auto child = shape(item.value(), depth + 1U);
        result.nodes += child.nodes; result.depth = std::max(result.depth, child.depth);
    }
    if (value.is_array()) for (const auto& item : value) {
        const auto child = shape(item, depth + 1U);
        result.nodes += child.nodes; result.depth = std::max(result.depth, child.depth);
    }
    return result;
}
class ShapeSax final : public nlohmann::json_sax<Json> {
  public:
    ShapeSax(std::uint64_t nodes, std::uint64_t depth) : node_limit_{nodes}, depth_limit_{depth} {}
    auto null() -> bool override { return scalar(); } auto boolean(bool) -> bool override { return scalar(); }
    auto number_integer(number_integer_t) -> bool override { return scalar(); }
    auto number_unsigned(number_unsigned_t) -> bool override { return scalar(); }
    auto number_float(number_float_t, const string_t&) -> bool override { return false; }
    auto string(string_t&) -> bool override { return scalar(); } auto binary(binary_t&) -> bool override { return false; }
    auto key(string_t&) -> bool override { return true; }
    auto start_object(std::size_t) -> bool override { return start(); }
    auto start_array(std::size_t) -> bool override { return start(); }
    auto end_object() -> bool override { return end(); } auto end_array() -> bool override { return end(); }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) -> bool override { return false; }
  private:
    auto add() -> bool { if (nodes_ == node_limit_) return false; ++nodes_; return true; }
    auto scalar() -> bool { return add() && depth_ + 1U <= depth_limit_; }
    auto start() -> bool { if (!add() || depth_ == depth_limit_) return false; ++depth_; return true; }
    auto end() -> bool { if (depth_ == 0U) return false; --depth_; return true; }
    std::uint64_t node_limit_, depth_limit_, nodes_{}, depth_{};
};

auto failure(StartupCodecErrorCode code) { return StartupCodecError{code}; }
auto quoted(std::uint64_t value) { return std::to_string(value); }
auto digest(std::string_view value) -> std::optional<core::Sha256Digest> {
    auto result = core::Sha256Digest::parse(value);
    if (!result) return std::nullopt;
    return *result;
}
auto canonical(const Json& value) -> std::expected<std::string, StartupCodecError> {
    auto result = core::canonicalize_json(value);
    core::detail::ExpectedErrorCleanup cleanup{result};
    if (!result) return std::unexpected{failure(StartupCodecErrorCode::domain)};
    return *result;
}
auto parse_canonical(std::string_view bytes) -> std::expected<Json, StartupCodecError> {
    try {
        CanonicalBuilder builder;
        if(!Json::sax_parse(bytes.begin(),bytes.end(),&builder))
            return std::unexpected{failure(StartupCodecErrorCode::malformed)};
        auto encoded = canonical(builder.root);
        if (!encoded) return std::unexpected{encoded.error()};
        if (*encoded != bytes) return std::unexpected{failure(StartupCodecErrorCode::noncanonical)};
        return std::move(builder.root);
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)};
    } catch (...) {
        return std::unexpected{failure(StartupCodecErrorCode::malformed)};
    }
}
auto exact_keys(const Json& value, std::initializer_list<std::string_view> keys) -> bool {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::ranges::all_of(keys, [&](auto key) { return value.contains(key); });
}
auto decimal(const Json& value, bool positive = false) -> bool {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || (text.size() > 1U && text.front() == '0')) return false;
    std::uint64_t parsed{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return ec == std::errc{} && end == text.data() + text.size() && (!positive || parsed != 0U);
}
auto digest_field(const Json& value) -> bool {
    return value.is_string() && digest(value.get_ref<const std::string&>()).has_value();
}
auto decimals(const Json& value) -> bool {
    return value.is_array() && std::ranges::all_of(value, [](const auto& item) { return decimal(item); });
}
auto positive_decimals(const Json& value) -> bool {
    return value.is_array() &&
           std::ranges::all_of(value, [](const auto& item) { return decimal(item, true); });
}

#define EB_COUNT_FIELDS(X) \
 X(blocks) X(factual_observations) X(failed_observations) X(stopping_evaluations) \
 X(workers) X(placements) X(migrations) X(validity_checks) X(finalized_workers) \
 X(failed_observation_errors) X(operational_terminal_errors) X(raw_page_acquisitions) \
 X(raw_context_acquisitions) X(context_events) X(context_offsets) \
 X(raw_temperature_acquisitions) X(frequency_domains) X(frequency_acquisitions) \
 X(frequency_cpu_memberships) X(frequency_metadata) X(classified_page_workers) \
 X(classified_context_workers) X(classified_frequency_domains) \
 X(first_resident_page_counts) X(second_resident_page_counts)
#define EB_STOCK_FIELDS(X) \
 X(persistent_worker_scopes) X(page_sources) X(context_sources) X(context_coverage_slots) \
 X(temperature_sources) X(frequency_sources) X(frequency_metadata_slots) \
 X(persistent_frequency_cpu_ids) X(preflight_channel_dispositions) X(page_pair_scratch) \
 X(context_array_scratch) X(temperature_view_scratch) X(frequency_view_scratch) \
 X(frequency_result_scratch) X(whole_stop_cleanup) X(preparation_rollback_cleanup) \
 X(completion_abort_cleanup) X(cpufreq_paths) X(cpufreq_directory_entries) \
 X(cpufreq_operation_slots) X(hwmon_directory_entries) X(hwmon_operation_slots) \
 X(dynamic_frequency_raw_bytes) X(dynamic_frequency_escaped_bytes)
#define EB_DEST_FIELDS(X) \
 X(provenance) X(policies) X(calibration) X(warmup) X(blocks) X(stopping_evaluations) \
 X(terminal) X(finalization) X(monitoring_raw) X(monitoring_classified) X(errors) \
 X(memory_finalization)
#define EB_ESCAPED_FIELDS(X) \
 X(scenario_profile) X(scenario_digest) X(resolved_plan_digest) X(workload_id) \
 X(first_variant_id) X(second_variant_id) X(monitoring_implementation_ids) \
 X(frequency_driver_ids) X(frequency_governor_ids) X(stable_numa_node_ids)

auto counts(const ComponentMaxima& source) {
    Json value = Json::object();
#define PUT(name) value[#name] = quoted(source.name.value);
    EB_COUNT_FIELDS(PUT)
#undef PUT
    return value;
}
auto stocks(const MonitoringLowerStocks& source) {
    Json value = Json::object();
#define PUT(name) value[#name] = quoted(source.name.value);
    EB_STOCK_FIELDS(PUT)
#undef PUT
    return value;
}
auto destinations(const EvidenceDestinations& source) {
    Json value = Json::object();
#define PUT(name) value[#name] = quoted(source.name.value);
    EB_DEST_FIELDS(PUT)
#undef PUT
    return value;
}
auto escaped_bytes(const RequestKnownEscapedBytes& source) {
    Json value = Json::object();
#define PUT(name) value[#name] = quoted(source.name.value);
    EB_ESCAPED_FIELDS(PUT)
#undef PUT
    return value;
}
template <typename T> auto quantity_array(const std::vector<T>& source) {
    Json value = Json::array();
    for (const auto item : source) value.push_back(quoted(item.value));
    return value;
}

auto startup_json(const StartupCapacityValue& value) -> Json {
    const auto& projected = value.projected();
    const auto& capacity = projected.capacity;
    const auto& parameters = capacity.prerequisite.parameters;
    const auto& shape = capacity.logical_shape;
    const auto& plan = capacity.resource_plan;
    Json pages{{"family", value.family()}, {"worker_count", quoted(parameters.workers.value)},
               {"first_total_pages", quantity_array(parameters.first_total_pages)},
               {"second_total_pages", quantity_array(parameters.second_total_pages)},
               {"first_target_counts", quantity_array(parameters.first_target_counts)},
               {"second_target_counts", quantity_array(parameters.second_target_counts)}};
    Json maxima{{"canonical_logical_bytes", quoted(shape.canonical_logical_bytes.value)},
                {"typed_nodes", quoted(shape.typed_nodes.value)},
                {"parser_depth", quoted(shape.parser_depth.value)},
                {"component_maxima", counts(capacity.prerequisite.components)}};
    Json resource{{"controller_receive_bytes", quoted(plan.controller_receive_bytes.value)},
                  {"controller_overflow_probe_bytes", quoted(plan.controller_overflow_probe_bytes.value)},
                  {"controller_node_slots", quoted(plan.controller_node_slots.value)},
                  {"controller_decode_destinations", destinations(plan.controller_decode_destinations)},
                  {"runner_encode_bytes", quoted(plan.runner_encode_bytes.value)},
                  {"runner_overflow_probe_bytes", quoted(plan.runner_overflow_probe_bytes.value)},
                  {"runner_node_slots", quoted(plan.runner_node_slots.value)},
                  {"runner_evidence_destinations", destinations(plan.runner_evidence_destinations)}};
    return Json{{"capacity_profile", std::string{capacity_profile}},
                {"wire_profile", std::string{wire_profile}},
                {"wire_registry_sha256", projected.registry_sha256.to_hex()},
                {"finalized_error_profile_sha256", projected.finalized_error_profile_sha256.to_hex()},
                {"monitoring_capacity_profile_sha256", projected.monitoring_capacity_profile_sha256.to_hex()},
                {"session_id", std::string{value.session_id().text()}},
                {"request_id", value.request_id().text()},
                {"request_known_escaped_bytes", escaped_bytes(parameters.request_known_escaped_bytes)},
                {"numa_domain_maxnode", quoted(parameters.numa_domain_maxnode.value)},
                {"request_page_inputs", std::move(pages)},
                {"logical_wire_maxima", std::move(maxima)},
                {"monitoring_admission_stocks", stocks(capacity.prerequisite.monitoring_lower_stocks)},
                {"resource_plan", std::move(resource)}};
}

auto valid_named_decimals(const Json& value, std::initializer_list<std::string_view> names) {
    return exact_keys(value, names) &&
           std::ranges::all_of(names, [&](auto name) { return decimal(value.at(name)); });
}
auto valid_capacity(const Json& value) -> bool {
    if (!exact_keys(value, {"capacity_profile", "wire_profile", "wire_registry_sha256",
            "finalized_error_profile_sha256", "monitoring_capacity_profile_sha256", "session_id",
            "request_id", "request_known_escaped_bytes", "numa_domain_maxnode",
            "request_page_inputs", "logical_wire_maxima", "monitoring_admission_stocks",
            "resource_plan"}) || !core::detail::json_string_equal(value.at("capacity_profile"), capacity_profile) ||
        !core::detail::json_string_equal(value.at("wire_profile"), wire_profile) || !digest_field(value.at("wire_registry_sha256")) ||
        !digest_field(value.at("finalized_error_profile_sha256")) ||
        !digest_field(value.at("monitoring_capacity_profile_sha256")) ||
        !value.at("session_id").is_string() || !decimal(value.at("request_id"), true) ||
        !decimal(value.at("numa_domain_maxnode"), true)) return false;
    const auto& escaped = value.at("request_known_escaped_bytes");
    if (!valid_named_decimals(escaped, {"scenario_profile", "scenario_digest", "resolved_plan_digest",
            "workload_id", "first_variant_id", "second_variant_id", "monitoring_implementation_ids",
            "frequency_driver_ids", "frequency_governor_ids", "stable_numa_node_ids"})) return false;
    const auto& pages = value.at("request_page_inputs");
    if (!exact_keys(pages, {"family", "worker_count", "first_total_pages", "second_total_pages",
                            "first_target_counts", "second_target_counts"}) ||
        (!core::detail::json_string_equal(pages.at("family"), "bandwidth") && !core::detail::json_string_equal(pages.at("family"), "latency")) ||
        !decimal(pages.at("worker_count"), true) || !decimals(pages.at("first_total_pages")) ||
        !positive_decimals(pages.at("second_total_pages")) ||
        !positive_decimals(pages.at("first_total_pages")) ||
        !positive_decimals(pages.at("first_target_counts")) ||
        !positive_decimals(pages.at("second_target_counts"))) return false;
    const auto worker_count = pages.at("worker_count").get_ref<const std::string&>();
    std::uint64_t workers{}; std::from_chars(worker_count.data(), worker_count.data()+worker_count.size(), workers);
    if (pages.at("first_total_pages").size() != workers || pages.at("second_total_pages").size() != workers ||
        pages.at("first_target_counts").size() != workers || pages.at("second_target_counts").size() != workers)
        return false;
    const auto& maxima = value.at("logical_wire_maxima");
    if (!exact_keys(maxima, {"canonical_logical_bytes", "typed_nodes", "parser_depth", "component_maxima"}) ||
        !decimal(maxima.at("canonical_logical_bytes")) || !decimal(maxima.at("typed_nodes")) ||
        !decimal(maxima.at("parser_depth"), true)) return false;
    const auto& components = maxima.at("component_maxima");
#define NAME(name) #name,
    if (!valid_named_decimals(components, {EB_COUNT_FIELDS(NAME)})) return false;
    if (!valid_named_decimals(value.at("monitoring_admission_stocks"), {EB_STOCK_FIELDS(NAME)})) return false;
    const auto& resource = value.at("resource_plan");
    if (!exact_keys(resource, {"controller_receive_bytes", "controller_overflow_probe_bytes",
            "controller_node_slots", "controller_decode_destinations", "runner_encode_bytes",
            "runner_overflow_probe_bytes", "runner_node_slots", "runner_evidence_destinations"}) ||
        !decimal(resource.at("controller_receive_bytes")) ||
        !core::detail::json_string_equal(resource.at("controller_overflow_probe_bytes"), "1") ||
        !decimal(resource.at("controller_node_slots")) || !decimal(resource.at("runner_encode_bytes")) ||
        !core::detail::json_string_equal(resource.at("runner_overflow_probe_bytes"), "1") || !decimal(resource.at("runner_node_slots")) ||
        !valid_named_decimals(resource.at("controller_decode_destinations"), {EB_DEST_FIELDS(NAME)}) ||
        !valid_named_decimals(resource.at("runner_evidence_destinations"), {EB_DEST_FIELDS(NAME)})) return false;
#undef NAME
    auto session = StartupSessionId::parse(value.at("session_id").get_ref<const std::string&>());
    return session.has_value();
}

auto family(const runtime::detail::PairwiseHostCapacityRequestTranscript& request) -> std::optional<std::string> {
    const auto first = request.first().experiment().kernel_id();
    if (first != request.second().experiment().kernel_id()) return std::nullopt;
    if (first.family() == kernel::KernelFamily::bandwidth) return "bandwidth";
    if (first.family() == kernel::KernelFamily::latency) return "latency";
    return std::nullopt;
}
auto capacity_error() { return std::unexpected{failure(StartupCodecErrorCode::capacity_mismatch)}; }
} // namespace

auto StartupSessionId::parse(std::string_view text) noexcept
    -> std::expected<StartupSessionId, StartupCodecError> {
    try {
        if (text.size() != 64U || !std::ranges::all_of(text, [](char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            })) return std::unexpected{failure(StartupCodecErrorCode::domain)};
        return StartupSessionId{std::string{text}};
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)};
    } catch (...) { return std::unexpected{failure(StartupCodecErrorCode::invalid_argument)}; }
}

auto construct_controller_startup_capacity(
    StartupSessionId session, const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    const RunnerContext& context,
    const platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority& authority, LocalResourceLimits limits) noexcept
    -> std::expected<StartupCapacityValue, StartupCodecError> {
    auto projected = project_controller_startup_capacity(request, context, authority, limits);
    auto selected_family = family(request);
    if (!projected || !selected_family) return capacity_error();
    return StartupCapacityValue{std::move(session), request.first_request_id(),
                                std::move(*projected), std::move(*selected_family)};
}

auto construct_runner_startup_capacity(
    StartupSessionId session, const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    const AdmittedPairwiseProductionInputs& inputs,
    const runtime::detail::PairwiseMonitoringHostCapacityAdmission& admission, LocalResourceLimits limits) noexcept
    -> std::expected<StartupCapacityValue, StartupCodecError> {
    auto projected = project_runner_startup_capacity(request, inputs, admission, limits);
    auto selected_family = family(request);
    if (!projected || !selected_family) return capacity_error();
    return StartupCapacityValue{std::move(session), request.first_request_id(),
                                std::move(*projected), std::move(*selected_family)};
}

auto encode_startup_capacity(const StartupCapacityValue& value) noexcept
    -> std::expected<std::string, StartupCodecError> {
    try { return canonical(startup_json(value)); }
    catch (const std::bad_alloc&) { return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)}; }
    catch (...) { return std::unexpected{failure(StartupCodecErrorCode::invalid_argument)}; }
}

auto author_capacity_offer(StartupCapacityValue capacity_value) noexcept
    -> std::expected<AuthoredCapacityOffer, StartupCodecError> {
    try {
        auto capacity_body = encode_startup_capacity(capacity_value);
        if (!capacity_body) return std::unexpected{capacity_body.error()};
        const auto capacity_value_json = Json::parse(*capacity_body);
        Json offer{{"profile", std::string{capacity_offer_profile}},
                   {"session_id", std::string{capacity_value.session_id().text()}},
                   {"request_id", capacity_value.request_id().text()},
                   {"request_body_sha256", capacity_value.projected().request_body_sha256.to_hex()},
                   {"source_manifest_sha256", capacity_value.projected().source_manifest_sha256.to_hex()},
                   {"projection_context_profile_sha256",
                    capacity_value.projected().projection_context_profile_sha256.to_hex()},
                   {"startup_capacity", capacity_value_json}};
        auto body = canonical(offer);
        if (!body) return std::unexpected{body.error()};
        const auto body_sha = core::sha256(*body);
        const auto capacity_sha = core::sha256(*capacity_body);
        return AuthoredCapacityOffer{std::move(capacity_value), std::move(*body),
                                    body_sha, capacity_sha};
    } catch (const std::bad_alloc&) { return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)}; }
    catch (...) { return std::unexpected{failure(StartupCodecErrorCode::invalid_argument)}; }
}

auto parse_capacity_offer_facts(std::string_view bytes, CapacityFactsParseLimits limits) noexcept
    -> std::expected<ParsedCapacityOffer, StartupCodecError> {
    if (bytes.size() > limits.bytes || !limits.nodes || !limits.depth)
        return std::unexpected{failure(StartupCodecErrorCode::shape)};
    try {
        ShapeSax preflight{limits.nodes, limits.depth};
        if (!Json::sax_parse(bytes.begin(), bytes.end(), &preflight))
            return std::unexpected{failure(StartupCodecErrorCode::shape)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)};
    } catch (...) {
        return std::unexpected{failure(StartupCodecErrorCode::malformed)};
    }
    auto value = parse_canonical(bytes);
    RetireTree value_cleanup{value?&*value:nullptr};
    if (!value) return std::unexpected{value.error()};
    try {
        if (!exact_keys(*value, {"profile", "session_id", "request_id", "request_body_sha256",
                "source_manifest_sha256", "projection_context_profile_sha256", "startup_capacity"}) ||
            !core::detail::json_string_equal(value->at("profile"), capacity_offer_profile) ||
            !digest_field(value->at("request_body_sha256")) ||
            !digest_field(value->at("source_manifest_sha256")) ||
            !digest_field(value->at("projection_context_profile_sha256")) ||
            !valid_capacity(value->at("startup_capacity")))
            return std::unexpected{failure(StartupCodecErrorCode::shape)};
        const auto& capacity = value->at("startup_capacity");
        if (capacity.at("session_id") != value->at("session_id") ||
            capacity.at("request_id") != value->at("request_id"))
            return std::unexpected{failure(StartupCodecErrorCode::digest_mismatch)};
        auto session = StartupSessionId::parse(value->at("session_id").get_ref<const std::string&>());
        auto request = ipc::RequestId::parse(value->at("request_id").get_ref<const std::string&>());
        auto request_sha = digest(value->at("request_body_sha256").get_ref<const std::string&>());
        auto source_sha = digest(value->at("source_manifest_sha256").get_ref<const std::string&>());
        auto projection_sha = digest(value->at("projection_context_profile_sha256").get_ref<const std::string&>());
        auto capacity_body = canonical(capacity);
        if (!session || !request || !request_sha || !source_sha || !projection_sha || !capacity_body)
            return std::unexpected{failure(StartupCodecErrorCode::domain)};
        const auto capacity_sha = core::sha256(*capacity_body);
        return ParsedCapacityOffer{std::move(*session), *request, *request_sha, *source_sha,
            *projection_sha, std::move(*capacity_body), std::string{bytes}, core::sha256(bytes),
            capacity_sha};
    } catch (const std::bad_alloc&) { return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)}; }
    catch (...) { return std::unexpected{failure(StartupCodecErrorCode::shape)}; }
}

auto parse_capacity_offer(std::string_view bytes, const StartupCapacityValue& local) noexcept
    -> std::expected<ParsedCapacityOffer, StartupCodecError> {
    auto expected = author_capacity_offer(local);
    if (!expected) return std::unexpected{expected.error()};
    if (bytes.size() > expected->canonical_body.size())
        return std::unexpected{failure(StartupCodecErrorCode::shape)};
    try {
        auto expected_tree=parse_canonical(expected->canonical_body);
        RetireTree expected_cleanup{expected_tree?&*expected_tree:nullptr};
        if(!expected_tree)return std::unexpected{expected_tree.error()};
        const auto expected_shape = shape(*expected_tree);
        auto parsed = parse_capacity_offer_facts(bytes, {expected->canonical_body.size(),
            static_cast<std::size_t>(expected_shape.nodes), static_cast<std::size_t>(expected_shape.depth)});
        if (!parsed) return std::unexpected{parsed.error()};
        auto local_body = encode_startup_capacity(local);
        if (!local_body || parsed->canonical_capacity_body != *local_body ||
            parsed->session_id != local.session_id() || parsed->request_id != local.request_id() ||
            parsed->request_body_sha256 != local.projected().request_body_sha256 ||
            parsed->source_manifest_sha256 != local.projected().source_manifest_sha256 ||
            parsed->projection_context_profile_sha256 != local.projected().projection_context_profile_sha256)
            return std::unexpected{failure(StartupCodecErrorCode::capacity_mismatch)};
        return parsed;
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)};
    } catch (...) {
        return std::unexpected{failure(StartupCodecErrorCode::malformed)};
    }
}

auto accept_capacity_offer(const ParsedCapacityOffer& offer,
                           const StartupCapacityValue& local) noexcept
    -> std::expected<AcceptedCapacityAck, StartupCodecError> {
    try {
        auto local_body = encode_startup_capacity(local);
        if (!local_body) return std::unexpected{local_body.error()};
        auto locally_authored = author_capacity_offer(local);
        if (!locally_authored || offer.canonical_body != locally_authored->canonical_body ||
            offer.body_sha256 != locally_authored->body_sha256 ||
            offer.capacity_body_sha256 != locally_authored->capacity_body_sha256 ||
            offer.body_sha256 != core::sha256(offer.canonical_body) ||
            offer.capacity_body_sha256 != core::sha256(offer.canonical_capacity_body))
            return std::unexpected{failure(StartupCodecErrorCode::digest_mismatch)};
        if (offer.session_id != local.session_id() || offer.request_id != local.request_id() ||
            offer.request_body_sha256 != local.projected().request_body_sha256 ||
            offer.source_manifest_sha256 != local.projected().source_manifest_sha256 ||
            offer.projection_context_profile_sha256 != local.projected().projection_context_profile_sha256 ||
            offer.canonical_capacity_body != *local_body)
            return std::unexpected{failure(StartupCodecErrorCode::capacity_mismatch)};
        Json ack{{"profile", std::string{capacity_accepted_profile}},
                 {"session_id", std::string{local.session_id().text()}},
                 {"request_id", local.request_id().text()},
                 {"request_body_sha256", local.projected().request_body_sha256.to_hex()},
                 {"offer_body_sha256", offer.body_sha256.to_hex()},
                 {"capacity_body_sha256", offer.capacity_body_sha256.to_hex()},
                 {"status", "accepted"}};
        auto body = canonical(ack);
        if (!body) return std::unexpected{body.error()};
        const auto body_sha = core::sha256(*body);
        return AcceptedCapacityAck{local.session_id(), local.request_id(),
            local.projected().request_body_sha256, offer.body_sha256, offer.capacity_body_sha256,
            std::move(*body), body_sha};
    } catch (const std::bad_alloc&) { return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)}; }
    catch (...) { return std::unexpected{failure(StartupCodecErrorCode::invalid_argument)}; }
}

auto parse_accepted_capacity_ack(std::string_view bytes,
                                 const AuthoredCapacityOffer& offer) noexcept
    -> std::expected<AcceptedCapacityAck, StartupCodecError> {
    auto capacity_body = encode_startup_capacity(offer.capacity);
    auto locally_authored = author_capacity_offer(offer.capacity);
    if (!capacity_body || !locally_authored ||
        offer.canonical_body != locally_authored->canonical_body ||
        offer.body_sha256 != locally_authored->body_sha256 ||
        offer.capacity_body_sha256 != locally_authored->capacity_body_sha256 ||
        offer.body_sha256 != core::sha256(offer.canonical_body) ||
        offer.capacity_body_sha256 != core::sha256(*capacity_body))
        return std::unexpected{failure(StartupCodecErrorCode::digest_mismatch)};
    Json expected_value{{"profile", std::string{capacity_accepted_profile}},
                        {"session_id", std::string{offer.capacity.session_id().text()}},
                        {"request_id", offer.capacity.request_id().text()},
                        {"request_body_sha256", offer.capacity.projected().request_body_sha256.to_hex()},
                        {"offer_body_sha256", offer.body_sha256.to_hex()},
                        {"capacity_body_sha256", offer.capacity_body_sha256.to_hex()},
                        {"status", "accepted"}};
    auto expected_body = canonical(expected_value);
    if (!expected_body) return std::unexpected{expected_body.error()};
    if (bytes.size() > expected_body->size())
        return std::unexpected{failure(StartupCodecErrorCode::shape)};
    try {
        const auto expected_shape = shape(expected_value);
        ShapeSax preflight{expected_shape.nodes, expected_shape.depth};
        if (!Json::sax_parse(bytes.begin(), bytes.end(), &preflight))
            return std::unexpected{failure(StartupCodecErrorCode::shape)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)};
    } catch (...) {
        return std::unexpected{failure(StartupCodecErrorCode::malformed)};
    }
    auto value = parse_canonical(bytes);
    RetireTree value_cleanup{value?&*value:nullptr};
    if (!value) return std::unexpected{value.error()};
    try {
        if (!exact_keys(*value, {"profile", "session_id", "request_id", "request_body_sha256",
                "offer_body_sha256", "capacity_body_sha256", "status"}) ||
            !core::detail::json_string_equal(value->at("profile"), capacity_accepted_profile) || !core::detail::json_string_equal(value->at("status"), "accepted") ||
            !digest_field(value->at("request_body_sha256")) ||
            !digest_field(value->at("offer_body_sha256")) ||
            !digest_field(value->at("capacity_body_sha256")))
            return std::unexpected{failure(StartupCodecErrorCode::shape)};
        if (!core::detail::json_string_equal(value->at("session_id"), offer.capacity.session_id().text()) ||
            !core::detail::json_string_equal(value->at("request_id"), offer.capacity.request_id().text()) ||
            !core::detail::json_string_equal(value->at("request_body_sha256"), offer.capacity.projected().request_body_sha256.to_hex()) ||
            !core::detail::json_string_equal(value->at("offer_body_sha256"), offer.body_sha256.to_hex()) ||
            !core::detail::json_string_equal(value->at("capacity_body_sha256"), offer.capacity_body_sha256.to_hex()))
            return std::unexpected{failure(StartupCodecErrorCode::digest_mismatch)};
        return AcceptedCapacityAck{offer.capacity.session_id(), offer.capacity.request_id(),
            offer.capacity.projected().request_body_sha256, offer.body_sha256,
            offer.capacity_body_sha256, std::string{bytes}, core::sha256(bytes)};
    } catch (const std::bad_alloc&) { return std::unexpected{failure(StartupCodecErrorCode::allocation_failure)}; }
    catch (...) { return std::unexpected{failure(StartupCodecErrorCode::shape)}; }
}

} // namespace epycbench::runtime::wire_v1::detail
