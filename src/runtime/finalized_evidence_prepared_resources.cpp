#include "runtime/finalized_evidence_prepared_resources.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/runtime/placement_provenance.hpp"
#include "runtime/finalized_evidence_prepared_resources.generated.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <variant>

namespace epycbench::runtime::prepared_resources_v1 {
namespace {
using Json = nlohmann::json;
using GeneratedKind = detail::DefinitionKind;

auto error(PreparedResourceErrorCode code, std::uint32_t index = 0U,
           std::uint64_t limit = 0U, std::uint64_t observed = 0U) noexcept {
    return PreparedResourceError{code, index, limit, observed};
}
auto checked_add(std::uint64_t a, std::uint64_t b, std::uint32_t index)
    -> std::expected<std::uint64_t, PreparedResourceError> {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        return std::unexpected{error(PreparedResourceErrorCode::arithmetic_overflow, index)};
    return a + b;
}
auto checked_mul(std::uint64_t a, std::uint64_t b, std::uint32_t index)
    -> std::expected<std::uint64_t, PreparedResourceError> {
    if (a != 0U && b > std::numeric_limits<std::uint64_t>::max() / a)
        return std::unexpected{error(PreparedResourceErrorCode::arithmetic_overflow, index)};
    return a * b;
}

struct DecodedBounds final {
    std::uint64_t scenario_profile{}, scenario_digest{}, resolved_plan_digest{}, workload_id{},
        first_variant_id{}, second_variant_id{}, frequency_driver_ids{},
        frequency_governor_ids{}, stable_numa_node_ids{};
};
auto decoded_bounds(const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
                    std::uint64_t base_page) -> DecodedBounds {
    const auto& first = request.first();
    const auto& experiment = first.experiment();
    std::string scenario_profile, scenario_digest, plan_digest;
    if (const auto* scenario = std::get_if<ScenarioPlacementProvenance>(&first.provenance())) {
        scenario_profile = "epycbench.scenario/v0.1";
        scenario_digest = scenario->scenario_digest().text();
        plan_digest = scenario->resolved_scenario_plan_digest().text();
    } else {
        const auto& host = std::get<HostQualificationPlanPlacementProvenance>(first.provenance());
        scenario_profile = host.policy_identity().text();
        scenario_digest = host.environment_digest().text();
        plan_digest = host.host_qualification_plan_digest().text();
    }
    const auto experiment_id = experiment.id().to_string();
    std::uint64_t stable{};
    for (const auto* placement : {&request.first(), &request.second()})
        for (const auto& worker : placement->workers())
            for (const auto& node : worker.region().nodes())
                stable = std::max(stable, static_cast<std::uint64_t>(
                    node.stable_numa_node_id().value().size()));
    return {scenario_profile.size(), scenario_digest.size(), plan_digest.size(),
            experiment.workload_id().to_string().size(), experiment_id.size() + 6U,
            experiment_id.size() + 7U, base_page, base_page, stable};
}

struct Evaluation final {
    const wire_v1::CalculatedEvidenceCapacity& capacity;
    DecodedBounds decoded;
    std::uint64_t maximum_domain_cpu_count{}, maximum_total_pages{}, maximum_target_count{};
};
auto variable(std::string_view name, const Evaluation& e) -> std::optional<std::uint64_t> {
    const auto& p = e.capacity.prerequisite.parameters;
    if (name == "W") return p.workers.value;
    if (name == "B") return p.maximum_blocks.value;
    if (name == "T") return p.maximum_warmup_trials.value;
    if (name == "Bp") return p.base_page_bytes.value;
    if (name == "P") return p.present_cpu_ids.size();
    if (name == "Nd") return p.numa_domain_maxnode.value;
    if (name == "R") return p.monitoring_rounds.value;
    if (name == "Xp") return p.channels.page ? 1U : 0U;
    if (name == "Xc") return p.channels.context ? 1U : 0U;
    if (name == "Xt") return p.channels.temperature ? 1U : 0U;
    if (name == "Xf") return p.channels.frequency ? 1U : 0U;
    if (name == "domain_cpu_count") return e.maximum_domain_cpu_count;
    if (name == "owner_total_pages") return e.maximum_total_pages;
    if (name == "owner_target_count") return e.maximum_target_count;
    return std::nullopt;
}
auto reference(std::string_view name, const Evaluation& e) -> std::optional<std::uint64_t> {
    constexpr std::string_view prefix = "prepared_decoded_utf8_bytes.";
    if (!name.starts_with(prefix)) return variable(name, e);
    name.remove_prefix(prefix.size());
    if (name == "scenario_profile") return e.decoded.scenario_profile;
    if (name == "scenario_digest") return e.decoded.scenario_digest;
    if (name == "resolved_plan_digest") return e.decoded.resolved_plan_digest;
    if (name == "workload_id") return e.decoded.workload_id;
    if (name == "first_variant_id") return e.decoded.first_variant_id;
    if (name == "second_variant_id") return e.decoded.second_variant_id;
    if (name == "frequency_driver_ids") return e.decoded.frequency_driver_ids;
    if (name == "frequency_governor_ids") return e.decoded.frequency_governor_ids;
    if (name == "stable_numa_node_ids") return e.decoded.stable_numa_node_ids;
    return std::nullopt;
}
auto evaluate(std::uint32_t index, const Evaluation& context)
    -> std::expected<std::uint64_t, PreparedResourceError> {
    if (index >= detail::formula_nodes.size())
        return std::unexpected{error(PreparedResourceErrorCode::invalid_input, index)};
    const auto& node = detail::formula_nodes[index];
    if (node.op == detail::FormulaOp::constant) return node.value;
    if (node.op == detail::FormulaOp::variable || node.op == detail::FormulaOp::reference) {
        auto value = node.op == detail::FormulaOp::variable
            ? variable(node.name, context) : reference(node.name, context);
        if (!value) return std::unexpected{error(PreparedResourceErrorCode::invalid_input, index)};
        return *value;
    }
    if (node.arg_begin > detail::formula_args.size() ||
        node.arg_count > detail::formula_args.size() - node.arg_begin)
        return std::unexpected{error(PreparedResourceErrorCode::invalid_input, index)};
    std::vector<std::uint64_t> args;
    try { args.reserve(node.arg_count); }
    catch (...) { return std::unexpected{error(PreparedResourceErrorCode::allocation_failure, index)}; }
    for (std::uint32_t i{}; i < node.arg_count; ++i) {
        auto value = evaluate(detail::formula_args[node.arg_begin + i], context);
        if (!value) return value;
        args.push_back(*value);
    }
    if (args.empty()) return std::unexpected{error(PreparedResourceErrorCode::invalid_input, index)};
    if (node.op == detail::FormulaOp::add) {
        std::uint64_t result{};
        for (auto value : args) { auto next = checked_add(result, value, index); if (!next) return next; result = *next; }
        return result;
    }
    if (node.op == detail::FormulaOp::multiply) {
        std::uint64_t result{1U};
        for (auto value : args) { auto next = checked_mul(result, value, index); if (!next) return next; result = *next; }
        return result;
    }
    if (node.op == detail::FormulaOp::minimum) return *std::ranges::min_element(args);
    if (node.op == detail::FormulaOp::maximum) return *std::ranges::max_element(args);
    if (node.op == detail::FormulaOp::ceil_divide && args.size() == 2U && args[1] != 0U) {
        const auto quotient = args[0] / args[1];
        return quotient + (args[0] % args[1] == 0U ? 0U : 1U);
    }
    if (node.op == detail::FormulaOp::conditional && args.size() == 3U)
        return args[0] != 0U ? args[1] : args[2];
    return std::unexpected{error(PreparedResourceErrorCode::invalid_input, index)};
}

auto map_kind(GeneratedKind value) -> PreparedDefinitionKind {
    switch (value) {
    case GeneratedKind::scalar: return PreparedDefinitionKind::scalar;
    case GeneratedKind::object: return PreparedDefinitionKind::object;
    case GeneratedKind::array: return PreparedDefinitionKind::array;
    case GeneratedKind::option: return PreparedDefinitionKind::option;
    case GeneratedKind::sum: return PreparedDefinitionKind::sum;
    }
    return PreparedDefinitionKind::scalar;
}
auto kind_token(PreparedDefinitionKind value) -> std::string_view {
    switch (value) {
    case PreparedDefinitionKind::scalar: return "scalar";
    case PreparedDefinitionKind::object: return "object";
    case PreparedDefinitionKind::array: return "array";
    case PreparedDefinitionKind::option: return "option";
    case PreparedDefinitionKind::sum: return "sum";
    }
    return {};
}
auto decimal(std::uint64_t value) { return std::to_string(value); }
auto canonical(const Json& value) -> std::expected<std::string, PreparedResourceError> {
    auto result = core::canonicalize_json(value);
    if (!result) return std::unexpected{error(PreparedResourceErrorCode::domain)};
    return *result;
}
auto resource_json(const PreparedResourceValue& value) -> Json {
    Json definitions = Json::array();
    for (const auto& entry : value.definitions)
        definitions.push_back(Json{{"definition_id", entry.definition_id},
            {"kind", kind_token(entry.kind)}, {"owner_occurrences", decimal(entry.owner_occurrences)},
            {"local_array_cardinality", entry.local_array_cardinality
                ? Json{decimal(*entry.local_array_cardinality)} : Json{nullptr}},
            {"decoded_utf8_bytes", entry.decoded_utf8_bytes
                ? Json{decimal(*entry.decoded_utf8_bytes)} : Json{nullptr}}});
    const auto& s = value.stocks;
    Json result{{"profile", std::string{detail::profile}},
        {"wire_registry_sha256", value.wire_registry_sha256.to_hex()},
        {"source_manifest_sha256", value.source_manifest_sha256.to_hex()},
        {"capacity_offer_body_sha256", value.capacity_offer_body_sha256.to_hex()},
        {"comparison_bootstrap_replicates", decimal(value.comparison_bootstrap_replicates)},
        {"stopping_bootstrap_replicates", decimal(value.stopping_bootstrap_replicates)},
        {"definitions", std::move(definitions)}};
#define PUT(name) result[#name] = decimal(s.name)
    PUT(logical_bytes); PUT(logical_probe_bytes); PUT(parser_nodes); PUT(parser_depth);
    PUT(builder_slots); PUT(builder_initialized_bits); PUT(array_owners); PUT(array_elements);
    PUT(string_owners); PUT(decoded_string_bytes); PUT(results_raw_graph_slots);
    PUT(results_raw_graph_elements); PUT(results_raw_graph_string_bytes); PUT(results_first_metrics);
    PUT(results_second_metrics); PUT(results_first_observation_ids); PUT(results_second_observation_ids);
    PUT(results_block_ids); PUT(results_paired_effects); PUT(results_first_outlier_states);
    PUT(results_second_outlier_states); PUT(statistics_metric_scratch);
    PUT(statistics_double_scratch_primary); PUT(statistics_double_scratch_secondary);
    PUT(statistics_effect_resample); PUT(statistics_stopping_prefix); PUT(results_stopping_replays);
    PUT(results_mismatch_diagnostics); PUT(active_primary_entries); PUT(active_secondary_entries);
#undef PUT
    return result;
}
auto parse_canonical(std::string_view bytes) -> std::expected<Json, PreparedResourceError> {
    try {
        auto value = Json::parse(bytes.begin(), bytes.end());
        auto encoded = canonical(value);
        if (!encoded) return std::unexpected{encoded.error()};
        if (*encoded != bytes) return std::unexpected{error(PreparedResourceErrorCode::noncanonical)};
        return value;
    } catch (const std::bad_alloc&) { return std::unexpected{error(PreparedResourceErrorCode::allocation_failure)}; }
    catch (...) { return std::unexpected{error(PreparedResourceErrorCode::malformed)}; }
}
auto exact_keys(const Json& value, std::initializer_list<std::string_view> keys) -> bool {
    return value.is_object() && value.size() == keys.size() &&
        std::ranges::all_of(keys, [&](auto key) { return value.contains(key); });
}
auto derive_value(const wire_v1::detail::StartupCapacityValue& capacity_value,
                  core::Sha256Digest capacity_offer_sha,
                  const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
                  PreparedResourceLimits limits) noexcept
    -> std::expected<PreparedResourceValue, PreparedResourceError> {
    try {
        const auto& capacity = capacity_value.projected().capacity;
        const auto& parameters = capacity.prerequisite.parameters;
        Evaluation context{capacity, decoded_bounds(request, parameters.base_page_bytes.value)};
        for (const auto& domain : parameters.domain_cpu_ids)
            context.maximum_domain_cpu_count = std::max<std::uint64_t>(
                context.maximum_domain_cpu_count, domain.size());
        for (const auto& pages : {&parameters.first_total_pages, &parameters.second_total_pages})
            for (const auto value : *pages) context.maximum_total_pages = std::max(context.maximum_total_pages, value.value);
        for (const auto& targets : {&parameters.first_target_counts, &parameters.second_target_counts})
            for (const auto value : *targets) context.maximum_target_count = std::max(context.maximum_target_count, value.value);
        if (detail::definition_stocks.size() > limits.definition_container_max ||
            detail::definition_stocks.size() > limits.size_max)
            return std::unexpected{error(PreparedResourceErrorCode::size_unrepresentable)};
        PreparedResourceValue result{capacity_value.projected().registry_sha256,
            capacity_value.projected().source_manifest_sha256, capacity_offer_sha,
            request.statistics_source().bootstrap_samples(),
            request.statistics_source().bootstrap_samples(), {}, {}};
        result.definitions.reserve(detail::definition_stocks.size());
        for (std::size_t index{}; index < detail::definition_stocks.size(); ++index) {
            const auto& source = detail::definition_stocks[index];
            auto owners = evaluate(source.owner_formula, context); if (!owners) return std::unexpected{owners.error()};
            std::optional<std::uint64_t> array, decoded;
            if (source.local_array_formula) { auto v = evaluate(*source.local_array_formula, context); if (!v) return std::unexpected{v.error()}; array = *v; }
            if (source.decoded_utf8_formula) { auto v = evaluate(*source.decoded_utf8_formula, context); if (!v) return std::unexpected{v.error()}; decoded = *v; }
            if (*owners > limits.size_max || (array && *array > limits.size_max) ||
                (decoded && *decoded > limits.size_max))
                return std::unexpected{error(PreparedResourceErrorCode::size_unrepresentable,
                    static_cast<std::uint32_t>(index), limits.size_max,
                    std::max({*owners, array.value_or(0U), decoded.value_or(0U)}))};
            result.definitions.push_back({source.definition_id, map_kind(source.kind), *owners, array, decoded});
        }
        auto aggregate = [&](std::string_view name) -> std::expected<std::uint64_t, PreparedResourceError> {
            const auto it = std::ranges::find(detail::aggregate_stocks, name, &detail::AggregateStock::name);
            if (it == detail::aggregate_stocks.end()) return std::unexpected{error(PreparedResourceErrorCode::invalid_input)};
            return evaluate(it->formula, context);
        };
        auto bs=aggregate("builder_slots"), ao=aggregate("array_owners"), ae=aggregate("array_elements"),
             so=aggregate("string_owners"), db=aggregate("decoded_string_bytes"),
             depth=aggregate("cursor_depth");
        if (!bs||!ao||!ae||!so||!db||!depth) return std::unexpected{!bs?bs.error():!ao?ao.error():!ae?ae.error():!so?so.error():!db?db.error():depth.error()};
        const auto B = parameters.maximum_blocks.value;
        auto two_b = checked_mul(2U, B, 0U); if (!two_b) return std::unexpected{two_b.error()};
        auto bits_plus = checked_add(*bs, 7U, 0U); if (!bits_plus) return std::unexpected{bits_plus.error()};
        auto& s=result.stocks;
        s = {capacity.logical_shape.canonical_logical_bytes.value, 1U,
             *bs, *depth,
             *bs, *bits_plus/8U, *ao, *ae, *so, *db, *bs, *ae, *db,
             *two_b, *two_b, *two_b, *two_b, B, B, *two_b, *two_b, *two_b,
             std::max({*two_b, B, result.comparison_bootstrap_replicates,
                       result.stopping_bootstrap_replicates}),
             std::max(*two_b, B), B, B, B, B, 1U, 8U};
        const auto values = std::to_array<std::uint64_t>({
            s.logical_bytes,s.logical_probe_bytes,s.parser_nodes,s.parser_depth,s.builder_slots,
            s.builder_initialized_bits,s.array_owners,s.array_elements,s.string_owners,
            s.decoded_string_bytes,s.results_raw_graph_slots,s.results_raw_graph_elements,
            s.results_raw_graph_string_bytes,s.results_first_metrics,s.results_second_metrics,
            s.results_first_observation_ids,s.results_second_observation_ids,s.results_block_ids,
            s.results_paired_effects,s.results_first_outlier_states,s.results_second_outlier_states,
            s.statistics_metric_scratch,s.statistics_double_scratch_primary,
            s.statistics_double_scratch_secondary,s.statistics_effect_resample,
            s.statistics_stopping_prefix,s.results_stopping_replays,
            s.results_mismatch_diagnostics,s.active_primary_entries,s.active_secondary_entries});
        if (std::ranges::any_of(values, [&](auto v){ return v > limits.size_max; }))
            return std::unexpected{error(PreparedResourceErrorCode::size_unrepresentable)};
        return result;
    } catch (const std::bad_alloc&) { return std::unexpected{error(PreparedResourceErrorCode::allocation_failure)}; }
    catch (...) { return std::unexpected{error(PreparedResourceErrorCode::invalid_input)}; }
}
} // namespace

auto derive_prepared_resource(const wire_v1::detail::AuthoredCapacityOffer& offer,
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    PreparedResourceLimits limits) noexcept -> std::expected<PreparedResourceValue, PreparedResourceError> {
    if (offer.body_sha256 != core::sha256(offer.canonical_body) ||
        offer.capacity_body_sha256 != core::sha256(wire_v1::detail::encode_startup_capacity(offer.capacity).value_or("")) ||
        offer.capacity.projected().request_body_sha256 != request.body_sha256() ||
        offer.capacity.request_id() != request.first_request_id())
        return std::unexpected{error(PreparedResourceErrorCode::digest_mismatch)};
    return derive_value(offer.capacity, offer.body_sha256, request, limits);
}
auto encode_prepared_resource(const PreparedResourceValue& value) noexcept
    -> std::expected<std::string, PreparedResourceError> {
    try { return canonical(resource_json(value)); }
    catch (const std::bad_alloc&) { return std::unexpected{error(PreparedResourceErrorCode::allocation_failure)}; }
    catch (...) { return std::unexpected{error(PreparedResourceErrorCode::invalid_input)}; }
}

auto author_prepared_offer(const wire_v1::detail::AuthoredCapacityOffer& capacity,
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request) noexcept
    -> std::expected<AuthoredPreparedOffer, PreparedResourceError> {
    auto derived = derive_prepared_resource(capacity, request);
    if (!derived) return std::unexpected{derived.error()};
    auto resource = std::move(*derived);
    auto body = encode_prepared_resource(resource); if (!body) return std::unexpected{body.error()};
    const auto resource_sha=core::sha256(*body);
    Json value{{"profile",std::string{prepared_offer_profile}},
        {"session_id",std::string{capacity.capacity.session_id().text()}},
        {"request_id",capacity.capacity.request_id().text()},
        {"request_body_sha256",capacity.capacity.projected().request_body_sha256.to_hex()},
        {"capacity_offer_body_sha256",capacity.body_sha256.to_hex()},
        {"prepared_resource_sha256",resource_sha.to_hex()},
        {"prepared_resource",resource_json(resource)}};
    auto offer_body=canonical(value); if(!offer_body)return std::unexpected{offer_body.error()};
    const auto offer_sha = core::sha256(*offer_body);
    return AuthoredPreparedOffer{capacity.capacity.session_id(),capacity.capacity.request_id(),
        capacity.capacity.projected().request_body_sha256,capacity.body_sha256,std::move(resource),
        std::move(*body),resource_sha,std::move(*offer_body),offer_sha};
}

auto parse_prepared_offer(std::string_view bytes,
    const wire_v1::detail::StartupCapacityValue& runner_capacity,
    const runtime::detail::PairwiseHostCapacityRequestTranscript& request,
    core::Sha256Digest capacity_offer_sha) noexcept
    -> std::expected<ParsedPreparedOffer, PreparedResourceError> {
    if (runner_capacity.projected().request_body_sha256 != request.body_sha256() ||
        runner_capacity.request_id() != request.first_request_id())
        return std::unexpected{error(PreparedResourceErrorCode::digest_mismatch)};
    auto local=derive_value(runner_capacity,capacity_offer_sha,request,{}); if(!local)return std::unexpected{local.error()};
    auto expected_resource=encode_prepared_resource(*local); if(!expected_resource)return std::unexpected{expected_resource.error()};
    auto value=parse_canonical(bytes); if(!value)return std::unexpected{value.error()};
    try {
        if(!exact_keys(*value,{"profile","session_id","request_id","request_body_sha256",
            "capacity_offer_body_sha256","prepared_resource_sha256","prepared_resource"}) ||
            value->at("profile")!=prepared_offer_profile ||
            value->at("session_id")!=runner_capacity.session_id().text() ||
            value->at("request_id")!=runner_capacity.request_id().text() ||
            value->at("request_body_sha256")!=runner_capacity.projected().request_body_sha256.to_hex() ||
            value->at("capacity_offer_body_sha256")!=capacity_offer_sha.to_hex())
            return std::unexpected{error(PreparedResourceErrorCode::resource_mismatch)};
        auto embedded=canonical(value->at("prepared_resource")); if(!embedded)return std::unexpected{embedded.error()};
        if(*embedded!=*expected_resource || value->at("prepared_resource_sha256")!=core::sha256(*embedded).to_hex())
            return std::unexpected{error(PreparedResourceErrorCode::resource_mismatch)};
        return ParsedPreparedOffer{runner_capacity.session_id(), runner_capacity.request_id(),
            runner_capacity.projected().request_body_sha256, capacity_offer_sha,
            std::move(*local),std::string{bytes},core::sha256(bytes),core::sha256(*embedded)};
    } catch(const std::bad_alloc&){return std::unexpected{error(PreparedResourceErrorCode::allocation_failure)};}
    catch(...){return std::unexpected{error(PreparedResourceErrorCode::shape)};}
}

auto accept_prepared_offer(const ParsedPreparedOffer& parsed) noexcept
    -> std::expected<AcceptedPreparedAck, PreparedResourceError> {
    Json value{{"profile",std::string{prepared_ack_profile}},
        {"session_id",std::string{parsed.session_id.text()}},
        {"request_id",parsed.request_id.text()}, {"request_body_sha256",parsed.request_body_sha256.to_hex()},
        {"capacity_offer_body_sha256",parsed.capacity_offer_body_sha256.to_hex()},
        {"prepared_offer_body_sha256",parsed.body_sha256.to_hex()},
        {"prepared_resource_sha256",parsed.prepared_resource_sha256.to_hex()}, {"status","accepted"}};
    auto body=canonical(value); if(!body)return std::unexpected{body.error()};
    return AcceptedPreparedAck{*body,core::sha256(*body)};
}

auto parse_prepared_ack(std::string_view bytes,const AuthoredPreparedOffer& authored) noexcept
    -> std::expected<AcceptedPreparedAck, PreparedResourceError> {
    auto expected=accept_prepared_offer(ParsedPreparedOffer{authored.session_id,authored.request_id,
        authored.request_body_sha256,authored.capacity_offer_body_sha256,authored.resource,
        authored.canonical_body,authored.body_sha256,authored.prepared_resource_sha256});
    if(!expected)return std::unexpected{expected.error()};
    if(bytes!=expected->canonical_body)return std::unexpected{error(PreparedResourceErrorCode::resource_mismatch)};
    return *expected;
}

auto PreparedResourceOwner::create(PreparedResourceValue value) noexcept
    -> std::expected<PreparedResourceOwner, PreparedResourceError> {
    try { return PreparedResourceOwner{std::move(value)}; }
    catch (...) { return std::unexpected{error(PreparedResourceErrorCode::allocation_failure)}; }
}
auto PreparedResourceOwner::activate() noexcept -> std::expected<void,PreparedResourceError>{
    if(state_!=PreparedPipelineState::prepared)return std::unexpected{error(PreparedResourceErrorCode::invalid_state)};
    state_=PreparedPipelineState::active; return {};
}
auto PreparedResourceOwner::consume() noexcept -> std::expected<void,PreparedResourceError>{
    if(state_!=PreparedPipelineState::active)return std::unexpected{error(PreparedResourceErrorCode::invalid_state)};
    state_=PreparedPipelineState::consumed; return {};
}
auto PreparedResourceOwner::fail() noexcept -> std::expected<void,PreparedResourceError>{
    if(state_==PreparedPipelineState::consumed || state_==PreparedPipelineState::failed)
        return std::unexpected{error(PreparedResourceErrorCode::invalid_state)};
    state_=PreparedPipelineState::failed; return {};
}
} // namespace epycbench::runtime::prepared_resources_v1
