#include "results/package_result.hpp"
#include "results/package_monitoring_wire.hpp"
#include "results/package_observation.hpp"
#include "results/package_stream.hpp"

#include "epycbench/kernel/package.h"
#include "epycbench/results/journal_records.hpp"
#include "epycbench/statistics/adaptive_stopping.hpp"
#include "epycbench/statistics/conclusion_classifier.hpp"
#include "epycbench/topology/serialization.hpp"
#include "measurement/package_control_json.hpp"
#include "scenario/package_scenario.hpp"
#include "scenario/package_mixed.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <csignal>
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace epycbench::results {
namespace {
using namespace package_observation;
using Json = nlohmann::json;
using Digest = core::Sha256Digest;
using U64 = std::uint64_t;
namespace control = measurement::package_control;
using package_monitoring_wire::Assessment;

auto fraction(const Json& ratio) -> double {
    return ratio.at("numerator").get<double>() / ratio.at("denominator").get<double>();
}

auto paired_effect(double baseline, double candidate) {
    return statistics::PairedLogEffect::create(std::log(candidate) - std::log(baseline));
}

auto median(std::vector<double> values) -> double {
    std::ranges::sort(values);
    const auto mid = values.size() / 2U;
    return values.size() % 2U ? values[mid] : std::midpoint(values[mid - 1U], values[mid]);
}

auto compare_rates(const std::vector<double>& baseline, const std::vector<double>& candidate,
                   U64 root_seed, Json& point, std::string& conclusion)
    -> std::expected<void, core::Error> {
    std::vector<statistics::PairedLogEffect> effects;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        auto effect = paired_effect(baseline[i], candidate[i]);
        if (!effect)
            return std::unexpected{effect.error()};
        effects.push_back(*effect);
    }
    auto mean = statistics::aggregate_log_effects(effects);
    if (!mean)
        return std::unexpected{mean.error()};
    point["oriented_ratio"] = mean->oriented_geometric_ratio().value();
    if (effects.size() >= 20U) {
        auto seed = core::derive_seed(root_seed, {core::SeedDomain::bootstrap, {}});
        if (!seed)
            return std::unexpected{seed.error()};
        auto policy = statistics::BootstrapPolicy::create(2000, 0.95, *seed);
        if (!policy)
            return std::unexpected{policy.error()};
        auto interval = statistics::bootstrap_log_effects(effects, *policy);
        if (!interval)
            return std::unexpected{interval.error()};
        const auto range = interval->oriented_geometric_ratio_interval();
        point["ratio_interval"] = {{"lower", range.lower().value()},
                                   {"upper", range.upper().value()}};
        auto threshold = statistics::PracticalEffectThreshold::create(0.01);
        if (!threshold)
            return std::unexpected{threshold.error()};
        const auto classification = statistics::classify_conclusion(
            mean->mean_log_effect(), interval->log_effect_interval(), *threshold);
        constexpr std::array labels{"candidate_faster", "baseline_faster",
                                    "detectable_below_practical_threshold", "inconclusive"};
        conclusion = labels.at(static_cast<std::size_t>(classification));
    }
    return {};
}

auto verified_stream(const Json& plan, const Json& block, U64 previous_end, bool supplied)
    -> std::optional<package_stream::MeasuredBlock> {
    if (block.at("runner_verified") != true || block.at("runner_exit") != 0 ||
        !block.at("error").is_null())
        return {};
    const auto& roles = plan.at("scenario").at("roles");
    const auto validate_rank = [&](std::size_t s, const Json& row, const Json& shape) {
        const auto& trial = row.at("trial");
        const auto& placement = row.at("placement");
        const auto& requirements = shape.at("regions");
        const auto& local = plan.at("placement").at(s).at("locality").at(0U);
        if (trial.is_null() || !verified(trial, 1U, false) ||
            placement.at("page_bytes") != local.at("base_page_bytes") ||
            trial.at("partition_begin") != shape.at("partition_begin") ||
            trial.at("partition_count") != shape.at("partition_count") ||
            trial.at("regions").size() != requirements.size())
            return false;
        for (std::size_t i = 0; i < requirements.size(); ++i)
            for (const char* field : {"name", "kind", "bytes", "alignment"})
                if (trial.at("regions").at(i).at(field) != requirements.at(i).at(field))
                    return false;
        if (!placed(placement, roles.at(s), 0U, requirements, local.at("memory_node"), false, supplied))
            return false;
        const auto& observed = placement.at("regions");
        for (const auto& region : requirements) {
            const auto receipt = std::ranges::find_if(observed,
                [&](const auto& item) { return item.at("name") == region.at("name"); });
            const bool released = !supplied && region.at("kind") == EPYCBENCH_PACKAGE_INPUT;
            if (receipt == observed.end() || receipt->at("released") != (released ? Json(true) : Json(nullptr)))
                return false;
        }
        return true;
    };
    auto measured = package_stream::measured_block(block.at("stream"), plan, validate_rank);
    if (measured && measured->release_ns < previous_end)
        return {};
    return measured;
}

auto region_observation_identity(const Json& regions) -> Json {
    Json inputs = Json::object(), references = Json::object();
    for (const auto& region : regions) {
        const auto& name = region.at("name").get_ref<const std::string&>();
        const auto& kind = region.at("kind");
        if (kind == EPYCBENCH_PACKAGE_INPUT || kind == EPYCBENCH_PACKAGE_SHARED_INPUT)
            inputs[name] = region.at("before");
        if (kind == EPYCBENCH_PACKAGE_OUTPUT || kind == EPYCBENCH_PACKAGE_SHARED)
            references[name] = region.at("reference");
    }
    return {{"inputs", std::move(inputs)}, {"references", std::move(references)}};
}

auto role_observation_identity(const Json& block, const Json& id) -> Json {
    auto ranks = Json::object();
    for (const auto& rank : block.at("ranks"))
        if (rank.at("role") == id) {
            const auto& trial = rank.at("trial");
            ranks[std::to_string(rank.at("rank").get<std::size_t>())] = {
                {"placement", rank.at("placement")}, {"work", trial.at("actual")},
                {"regions", region_observation_identity(trial.at("regions"))}};
        }
    Json collective_work, shared_regions;
    for (const auto& team : block.at("collectives"))
        if (team.at("role") == id) {
            collective_work = team.at("actual");
            shared_regions = region_observation_identity(team.at("regions"));
        }
    const auto& proofs = block.at("input_proofs");
    const auto proof = std::ranges::find_if(proofs, [&](const auto& row) { return row.at("role") == id; });
    return {{"ranks", std::move(ranks)}, {"collective_work", std::move(collective_work)},
            {"shared_regions", std::move(shared_regions)}, {"input", *proof}};
}

auto mixed_pair_eligibility(const Json& together, const Json& alone, const Json& id) -> std::string_view {
    const auto a = role_observation_identity(alone, id);
    const auto b = role_observation_identity(together, id);
    if (a.at("input") != b.at("input")) return "different_input";
    if (a.at("collective_work") != b.at("collective_work")) return "different_work";
    const auto& shared = a.at("shared_regions");
    const auto& other_shared = b.at("shared_regions");
    if (!shared.is_null()) {
        if (shared.at("inputs") != other_shared.at("inputs")) return "different_control_input";
        if (shared.at("references") != other_shared.at("references")) return "different_reference";
    }
    for (const auto& [rank, row] : a.at("ranks").items()) {
        const auto& other = b.at("ranks").at(rank);
        if (row.at("work") != other.at("work")) return "different_work";
        if (row.at("placement") != other.at("placement")) return "different_placement";
        if (row.at("regions").at("inputs") != other.at("regions").at("inputs")) return "different_input";
        if (row.at("regions").at("references") != other.at("regions").at("references")) return "different_reference";
    }
    return "eligible";
}

auto mixed_measurement(const std::optional<MeasuredRole>& measure) -> Json {
    if (!measure) return nullptr;
    return {{"begin_ns", std::to_string(measure->interval.begin)},
            {"end_ns", std::to_string(measure->interval.end)},
            {"work_units", std::to_string(measure->units)}};
}

struct MixedBlock {
    U64 end{};
    std::vector<MeasuredRole> measured;
    Json pairs;
    std::optional<int> failure_signal;
};

auto inspect_mixed_block(const Json& plan, const BoundRanks& bound, const std::vector<Json>& solo_plans,
                         const kernel::packages::Catalog& catalog, const Json& block,
                         U64 repetitions, std::size_t ordinal, U64 previous_end, std::string_view source)
    -> std::expected<MixedBlock, core::Error> {
    const auto& roles = plan.at("scenario").at("roles");
    const auto order = scenario::mixed_condition_order(roles.size(), ordinal);
    const auto& evidence = block.at("mixed");
    if (!evidence.is_object() || evidence.size() != 3U || evidence.at("condition_order") != order ||
        !unsigned_value(evidence.at("attempted")) || evidence.at("attempted") < 1U ||
        evidence.at("attempted") > order.size() || !evidence.at("alone").is_array() ||
        evidence.at("alone").size() > roles.size())
        return std::unexpected{invalid("Invalid mixed condition schedule")};
    const auto attempted = evidence.at("attempted").get<std::size_t>();
    const auto& alone = evidence.at("alone");
    std::vector<const Json*> solo_blocks(roles.size());
    std::vector<std::optional<MeasuredRole>> solo(roles.size());
    std::vector<bool> solo_monitoring(roles.size(), true);
    std::vector<MeasuredRole> together;
    MixedBlock observation{.end = previous_end, .measured = {}, .pairs = Json::array(), .failure_signal = {}};
    std::size_t solo_index{};
    bool together_attempted = false;
    bool together_monitoring = true;
    for (std::size_t i = 0; i < attempted; ++i) {
        const auto condition = order[i];
        const bool concurrent = condition == roles.size();
        const Json* raw = &block;
        if (concurrent) {
            together_attempted = true;
        } else {
            if (solo_index >= alone.size())
                return std::unexpected{invalid("Missing retained solo measurement")};
            const auto& attempt = alone.at(solo_index++);
            if (!attempt.is_object() || attempt.size() != 2U || attempt.at("role") != roles.at(condition).at("id") ||
                attempt.at("block").contains("mixed"))
                return std::unexpected{invalid("Unbound mixed solo measurement")};
            raw = &attempt.at("block");
            solo_blocks[condition] = raw;
        }
        if (!raw->at("error").is_null()) {
            if (i + 1U != attempted)
                return std::unexpected{invalid("Mixed execution continued past a failed condition")};
            observation.failure_signal = raw->at("error").at("context").value("signal", 0);
        }
        const auto measured = [&]() -> std::expected<std::vector<MeasuredRole>, core::Error> {
            if (concurrent)
                return inspect_bound_block(plan, bound, *raw, repetitions, ordinal,
                                           observation.end, false, source, true);
            const auto& solo_plan = solo_plans.at(condition);
            auto solo_bound = bind_ranks(solo_plan, catalog);
            if (!solo_bound) return std::unexpected{solo_bound.error()};
            return inspect_bound_block(solo_plan, *solo_bound, *raw, repetitions, ordinal,
                                       observation.end, false, source);
        }();
        if (!measured) return std::unexpected{measured.error()};
        const auto& execution = plan.at("scenario").at("execution");
        if (execution.contains("monitoring")) {
            const auto monitoring = package_monitoring_wire::inspect(execution, *raw, false,
                source == "supplied", nullptr, false);
            if (!monitoring) return std::unexpected{monitoring.error()};
            if (concurrent) together_monitoring = *monitoring == Assessment::qualified;
            else solo_monitoring[condition] = *monitoring == Assessment::qualified;
        }
        for (const auto& role : *measured) observation.end = std::max(observation.end, role.interval.end);
        if (concurrent)
            together = *measured;
        else if (!measured->empty())
            solo[condition] = measured->front();
    }
    if (solo_index != alone.size() || (attempted < order.size() && !observation.failure_signal) ||
        (!together_attempted && (!block.at("ranks").empty() || !block.at("runner_exit").is_null() ||
                                 block.at("runner_verified") != false || !block.at("error").is_null())))
        return std::unexpected{invalid("Mixed measurements do not match the attempted order")};
    const bool overlap = !together.empty() && block.at("overlap_begin_ns") < block.at("overlap_end_ns");
    bool fully_matched = overlap;
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const auto& id = roles[i].at("id");
        const auto concurrent = together.empty() ? std::optional<MeasuredRole>{} : together[i];
        std::string_view eligibility = "eligible";
        if (!together_attempted || !solo_blocks[i])
            eligibility = "condition_not_attempted";
        else if (!concurrent)
            eligibility = "unqualified_together";
        else if (!overlap)
            eligibility = "no_overlap";
        else if (!solo[i])
            eligibility = "unqualified_alone";
        else if (!together_monitoring)
            eligibility = "monitoring_unqualified_together";
        else if (!solo_monitoring[i])
            eligibility = "monitoring_unqualified_alone";
        else if (solo[i]->units != concurrent->units || solo[i]->units == 0)
            eligibility = "different_work";
        else
            eligibility = mixed_pair_eligibility(block, *solo_blocks[i], id);
        const bool matched = eligibility == "eligible";
        fully_matched &= matched;
        Json ratio, slowdown;
        if (matched && source == "native") {
            const auto alone_ns = static_cast<double>(solo[i]->interval.end - solo[i]->interval.begin);
            const auto together_ns = static_cast<double>(concurrent->interval.end - concurrent->interval.begin);
            ratio = alone_ns / together_ns;
            slowdown = together_ns / alone_ns;
        }
        observation.pairs.push_back({{"role", id},
            {"eligibility", matched && source == "supplied" ? "supplied_workers" : eligibility},
            {"alone", mixed_measurement(solo[i])}, {"together", mixed_measurement(concurrent)},
            {"together_to_alone_throughput", std::move(ratio)}, {"slowdown", std::move(slowdown)}});
    }
    if (fully_matched) observation.measured = std::move(together);
    return observation;
}
} // namespace

PackagePrecision::PackagePrecision(const Json& execution)
    : minimum_blocks_{execution.at("precision").at("minimum_blocks").get<std::size_t>()},
      minimum_valid_blocks_{execution.at("precision").at("minimum_valid_blocks").get<std::size_t>()},
      seed_{execution.value("order_seed", U64{})},
      confidence_{fraction(execution.at("precision").at("confidence"))},
      target_{fraction(execution.at("precision").at("relative_half_width"))} {}

auto PackagePrecision::observe(std::size_t ordinal, U64 repetitions,
                               std::optional<std::pair<U64, U64>> durations)
    -> std::expected<std::optional<Json>, core::Error> {
    if (durations) {
        const auto rate = [repetitions](U64 duration_ns) {
            return static_cast<double>(repetitions) / (static_cast<double>(duration_ns) / 1e9);
        };
        auto effect = paired_effect(rate(durations->first), rate(durations->second));
        if (!effect)
            return std::unexpected{effect.error()};
        effects_.push_back(*effect);
    }
    if (ordinal + 1U < minimum_blocks_ || !enough_eligible_blocks())
        return std::nullopt;
    auto seed = core::derive_seed(seed_, {core::SeedDomain::bootstrap, {}});
    if (!seed)
        return std::unexpected{seed.error()};
    auto policy = statistics::BootstrapPolicy::create(2000, confidence_, *seed);
    if (!policy)
        return std::unexpected{policy.error()};
    auto target = statistics::RelativeHalfWidthTarget::create(target_);
    if (!target)
        return std::unexpected{target.error()};
    auto mean = statistics::aggregate_log_effects(effects_);
    if (!mean)
        return std::unexpected{mean.error()};
    auto interval = statistics::bootstrap_log_effects(effects_, *policy);
    if (!interval)
        return std::unexpected{interval.error()};
    const auto estimate = mean->oriented_geometric_ratio();
    const auto range = interval->oriented_geometric_ratio_interval();
    auto evaluation = statistics::evaluate_adaptive_stopping(estimate, range, *target);
    if (!evaluation)
        return std::unexpected{evaluation.error()};
    attained_ = evaluation->attainment() == statistics::PrecisionTargetAttainment::target_met;
    return Json{{"valid_blocks", effects_.size()},
                {"point_ratio", estimate.value()},
                {"lower_ratio", range.lower().value()},
                {"upper_ratio", range.upper().value()},
                {"observed_relative_half_width", evaluation->observed_precision().value()},
                {"attainment", attained_ ? "target_met" : "target_not_met"}};
}

auto package_precision_stop_reason(bool calibrated, bool qualified, bool attained,
                                   const Json& document) -> std::string_view {
    if (attained)
        return "precision_target_reached";
    if (qualified)
        return "maximum_blocks_reached";
    const auto& last = calibrated ? document.at("blocks").back()
        : document.at("calibration").at("attempts").back().at("block");
    const auto& error = last.at("error");
    if (!error.is_null()) {
        const auto signal = error.at("context").value("signal", 0);
        if (signal == SIGINT || signal == SIGTERM)
            return "interrupted";
    }
    return calibrated ? "measurement_failed" : "calibration_failed";
}

auto qualified_package_intervals(const Json& plan, const kernel::packages::Catalog& catalog,
                                 const Json& block, U64 repetitions, std::size_t ordinal,
                                 U64 previous_end, bool warm, bool supplied, bool calibrating,
                                 bool* factual_monitoring_rejection)
    -> std::expected<std::vector<std::pair<U64, U64>>, core::Error> {
    try {
        const auto& exec = plan.at("scenario").at("execution");
        const auto monitoring = package_monitoring_wire::inspect(exec, block, calibrating, supplied);
        if (!monitoring) return std::unexpected{monitoring.error()};
        if (*monitoring == Assessment::acquisition_failure ||
            (*monitoring == Assessment::factual_rejection && !factual_monitoring_rejection))
            return std::vector<std::pair<U64, U64>>{};
        if (factual_monitoring_rejection)
            *factual_monitoring_rejection = *monitoring == Assessment::factual_rejection;
        if (exec.contains("pipeline_chunks")) {
            if (block.at("ordinal") != ordinal || !block.at("ranks").empty() ||
                !package_stream::retained_shape(block.at("stream"), plan, true))
                return std::unexpected{invalid("Malformed streaming block or chunk evidence")};
            const auto stream = verified_stream(plan, block, previous_end, supplied);
            if (!stream)
                return std::unexpected{invalid("Stream work, placement, transfer or clock sequence failed verification")};
            return std::vector{std::pair{stream->release_ns, stream->completion_ns}};
        }
        const auto bound = bind_ranks(plan, catalog);
        if (!bound)
            return std::unexpected{bound.error()};
        const auto verified =
            inspect_bound_block(plan, *bound, block, repetitions, ordinal, previous_end, warm,
                                supplied ? "supplied" : "native");
        if (!verified)
            return std::unexpected{verified.error()};
        std::vector<std::pair<U64, U64>> result;
        for (const auto& role : *verified)
            result.emplace_back(role.interval.begin, role.interval.end);
        return result;
    } catch (const std::exception& e) {
        return std::unexpected{invalid(std::string("Malformed observation: ") + e.what())};
    }
}

static bool package_schema(const Json& doc) {
    return doc.is_object() && doc.contains("schema") &&
        (doc["schema"] == package_result_schema || doc["schema"] == previous_package_result_schema);
}

bool is_package_result(std::string_view bytes) {
    return package_schema(Json::parse(bytes, nullptr, false));
}

auto validate_package_result(std::string_view bytes) -> std::expected<PackageResult, core::Error> {
    try {
        auto doc = Json::parse(bytes, nullptr, false);
        if (!package_schema(doc) ||
            !hex(doc.at("content_sha256")))
            return std::unexpected{invalid("Expected a sealed package Result")};
        if (doc.dump() != bytes)
            return std::unexpected{
                invalid("Package Result is not the canonical published document")};
        auto seal = doc.at("content_sha256");
        doc.erase("content_sha256");
        const auto digest = core::sha256(doc.dump());
        if (seal != digest.to_hex())
            return std::unexpected{invalid("Result content digest mismatch")};
        doc["content_sha256"] = std::move(seal);
        const auto run = doc.at("run_id").get<std::string>();
        const auto& plan = doc.at("plan");
        const bool pinned = doc.at("schema") == package_result_schema;
        if (doc.contains("controller_placement") != pinned || plan.contains("controller") != pinned)
            return std::unexpected{invalid("Unexpected or missing controller placement evidence")};
        if (pinned) {
            const auto& expected = plan.at("controller");
            const auto& cpu = expected.at("cpu");
            const Json receipt{{"core", expected.at("core")}, {"cpu", cpu}, {"affinity", Json::array({cpu})},
                               {"cpu_before", cpu}, {"cpu_after", cpu}};
            if (doc.at("controller_placement").dump() != receipt.dump())
                return std::unexpected{invalid("Observed controller affinity differs from its frozen CPU and core")};
        }
        const auto& source = plan.at("scenario");
        const auto& roles = source.at("roles");
        const auto& exec = source.at("execution");
        const auto& mode = source.at("mode");
        const bool mixed = mode == "mixed";
        const bool streaming = exec.contains("pipeline_chunks");
        const auto scope = doc.at("evidence_source");
        auto repetitions = exec.at("repetitions").get<U64>();
        const bool calibrating = exec.contains("calibration");
        const bool stopping = exec.contains("precision");
        if (calibrating != doc.contains("calibration"))
            return std::unexpected{invalid("Calibration evidence and request differ")};
        if (stopping != doc.contains("precision_stop_reason"))
            return std::unexpected{invalid("Precision evidence and request differ")};
        const auto attempts = calibrating ? doc.at("calibration").at("attempts").size() : 0U;
        const auto& blocks = doc.at("blocks");
        if (!journal::RunId::parse(run) || (scope != "native" && scope != "supplied") ||
            !hex(doc.at("build_sha256")) || !doc.at("authored_scenario").is_object() ||
            !roles.is_array() || roles.empty() || roles.size() > 64U || !blocks.is_array() ||
            (blocks.empty() && !calibrating) || blocks.size() > 4096U ||
            blocks.size() > exec.at("blocks").get<U64>() || repetitions == 0 ||
            plan.at("placement").size() != roles.size() ||
            plan.at("bindings").size() != roles.size() ||
            doc.at("scenario_digest") != "sha256:" + core::sha256(source.dump()).to_hex() ||
            doc.at("resolved_plan_digest") != "sha256:" + core::sha256(plan.dump()).to_hex())
            return std::unexpected{
                invalid("Unbound Run, Scenario, topology, or observation population")};
        const auto& build_bytes = doc.at("build_manifest").get_ref<const std::string&>();
        if (doc.at("build_sha256") != core::sha256(build_bytes).to_hex())
            return std::unexpected{invalid("Build manifest digest mismatch")};
        const auto build_manifest = Json::parse(build_bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 8) throw std::invalid_argument{"Build manifest nesting"};
            return true;
        });
        if (!build_manifest.is_object() || !build_manifest.contains("executable_sha256") ||
            !hex(build_manifest.at("executable_sha256")))
            return std::unexpected{invalid("Malformed executable identity in Build manifest")};
        const auto& journal = doc.at("journal");
        if (!journal.is_object() || journal.size() != 3U ||
            journal.at("file") != run + ".epycbench.journal.jsonseq" ||
            !unsigned_value(journal.at("records")) ||
            journal.at("records") != blocks.size() + attempts + 2U ||
            !hex(journal.at("last_digest")))
            return std::unexpected{
                invalid("Invalid journal filename, record count or terminal digest")};
        auto snapshot = topology::deserialize_topology(
            doc.at("topology_snapshot").get_ref<const std::string&>(),
            topology::TopologyJsonInputPolicy::require_canonical);
        if (!snapshot)
            return std::unexpected{snapshot.error()};
        auto catalog = kernel::packages::Catalog::from_bindings(plan.at("bindings"));
        if (!catalog)
            return std::unexpected{catalog.error()};
        auto reconstructed = scenario::replay_package(
            doc.at("authored_scenario"), *catalog,
            {snapshot->topology, snapshot->stable_ids, snapshot->fingerprints, &*catalog},
            plan);
        if (!reconstructed || doc.at("resolved_plan_digest") != "sha256:" + reconstructed->resolved_digest.to_hex())
            return std::unexpected{invalid("Saved Scenario, package definitions, placement and "
                                           "topology do not reproduce the plan")};
        auto bound = bind_ranks(plan, *catalog);
        if (!bound)
            return std::unexpected{bound.error()};
        std::vector<Json> solo_plans;
        auto mixed_blocks = Json::array();
        if (mixed)
            for (std::size_t i = 0; i < roles.size(); ++i)
                solo_plans.push_back(scenario::mixed_role_alone_plan(plan, i, *catalog));
        const auto rank_count = bound->rank_count;
        const auto& tasks = bound->tasks;
        std::vector<std::vector<double>> work_metrics(roles.size());
        std::vector<std::vector<double>> rates(roles.size());
        std::string execution_status = "completed";
        std::size_t good_blocks{}, completed_precision_blocks{};
        std::vector<double> cohort_rates;
        std::map<std::string, U64> input_frequencies;
        U64 previous_end{};
        std::optional<PackagePrecision> precision;
        Json precision_history = Json::array();
        Json monitoring_summary = Json::object();
        if (stopping)
            precision.emplace(exec);
        if (calibrating) {
            const auto& calibration = doc.at("calibration");
            const auto& records = calibration.at("attempts");
            const auto settings = control::calibration_settings(exec);
            std::vector<control::CalibrationProbe> history;
            if (!records.is_array() || records.empty() || records.size() > settings.maximum_probes)
                return std::unexpected{invalid("Invalid common repetition probe population")};
            auto decision = control::next_probe(settings, roles.size(), history);
            for (const auto& record : records) {
                if (!decision || decision->status != control::CalibrationStatus::next_probe ||
                    record.at("repetitions") != *decision->repetitions)
                    return std::unexpected{invalid(
                        "Calibration history diverges from the frozen common-probe policy")};
                const auto& block = record.at("block");
                if (block.contains("precision"))
                    return std::unexpected{invalid("Calibration cannot contribute to precision")};
                const auto monitoring = package_monitoring_wire::inspect(exec, block, true, scope == "supplied");
                if (!monitoring) return std::unexpected{monitoring.error()};
                auto measured =
                    inspect_bound_block(plan, *bound, block, *decision->repetitions, history.size(),
                                        previous_end, false, scope.get_ref<const std::string&>());
                if (!measured)
                    return std::unexpected{measured.error()};
                control::CalibrationProbe probe{*decision->repetitions, {}};
                if (*monitoring == Assessment::qualified) {
                    for (const auto& role : *measured) {
                        previous_end = std::max(previous_end, role.interval.end);
                        probe.role_durations_ns.push_back(role.interval.end - role.interval.begin);
                    }
                }
                if (!block.at("error").is_null()) {
                    const auto sig = block.at("error").at("context").value("signal", 0);
                    execution_status = (sig == SIGINT || sig == SIGTERM) ? "interrupted" : "failed";
                } else if (*monitoring == Assessment::acquisition_failure)
                    execution_status = "failed";
                history.push_back(std::move(probe));
                decision = control::next_probe(settings, roles.size(), history);
            }
            if (!decision || decision->status == control::CalibrationStatus::next_probe ||
                calibration.at("status") != control::disposition(decision->status))
                return std::unexpected{
                    invalid("Calibration disposition does not follow from the retired probes")};
            const bool selected = decision->status == control::CalibrationStatus::selected;
            if ((selected && calibration.at("selected_repetitions") != *decision->repetitions) ||
                (!selected && !calibration.at("selected_repetitions").is_null()) ||
                selected == blocks.empty())
                return std::unexpected{
                    invalid("Measured observations do not follow the common repetition selection")};
            if (selected)
                repetitions = *decision->repetitions;
        }
        Json dispositions = Json::array();
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            const auto& block = blocks.at(b);
            if (block.at("ordinal") != b || !block.at("ranks").is_array() ||
                block.at("ranks").size() > rank_count)
                return std::unexpected{invalid("Unexpected block ordinal or rank population")};
            if (mixed != block.contains("mixed") || (mixed && execution_status != "completed"))
                return std::unexpected{invalid("Unexpected mixed observation population")};
            if ((!precision && block.contains("precision")) ||
                (precision && (precision->attained() || completed_precision_blocks != b)))
                return std::unexpected{invalid("Block falls outside the precision observation prefix")};
            if (!block.at("error").is_null()) {
                const auto& detail = block.at("error").at("context");
                const auto signal = detail.value("signal", 0);
                execution_status = signal == SIGTERM || signal == SIGINT ? "interrupted" : "failed";
            }
            const auto monitoring = package_monitoring_wire::inspect(exec, block, false, scope == "supplied",
                exec.contains("monitoring") ? &monitoring_summary : nullptr);
            if (!monitoring) return std::unexpected{monitoring.error()};
            if (*monitoring == Assessment::acquisition_failure && execution_status == "completed")
                execution_status = "failed";
            dispositions.push_back({{"ordinal", b}, {"qualified", false},
                {"reason", !block.at("error").is_null() ? "runner_error" :
                    *monitoring == Assessment::acquisition_failure ? "monitoring_acquisition_failed" :
                    *monitoring != Assessment::qualified ? "monitoring_rejected" : "verification_or_placement_failed"}});
            if (streaming) {
                if (!block.at("ranks").empty())
                    return std::unexpected{invalid("Streaming ranks belong to chunks")};
                if (!block.at("error").is_null())
                    continue;
                const auto& stages = block.at("stream");
                if (!package_stream::retained_shape(stages, plan, doc.at("schema") == package_result_schema))
                    return std::unexpected{invalid("Malformed streaming chunk evidence")};
                const auto measured = verified_stream(plan, block, previous_end, scope == "supplied");
                if (!measured || *monitoring != Assessment::qualified)
                    continue;
                previous_end = measured->completion_ns;
                ++good_blocks;
                dispositions.back()["qualified"] = true;
                dispositions.back()["reason"] = scope == "supplied" ? Json("supplied_clocks_unscored") : Json(nullptr);
                ++input_frequencies[package_stream::input_identity(stages, *measured)];
                cohort_rates.push_back(
                    static_cast<double>(repetitions) * 1e9 /
                    static_cast<double>(measured->completion_ns - measured->release_ns));
                for (std::size_t s = 0; s < roles.size(); ++s) {
                    const auto seconds = static_cast<double>(measured->callback_ns[s]) / 1e9;
                    if (measured->work_units[s] == 0)
                        continue;
                    const auto units = static_cast<double>(measured->work_units[s]);
                    work_metrics[s].push_back(tasks[s]->work_metric == "seconds_per_unit"
                                                  ? seconds / units
                                                  : units / seconds);
                }
                continue;
            }
            const auto& evidence_source = scope.get_ref<const std::string&>();
            auto measured = [&]() -> std::expected<std::vector<MeasuredRole>, core::Error> {
                if (!mixed)
                    return inspect_bound_block(plan, *bound, block, repetitions, b, previous_end,
                                               exec.contains("warmup"), evidence_source);
                auto observed = inspect_mixed_block(plan, *bound, solo_plans, *catalog, block,
                                                    repetitions, b, previous_end, evidence_source);
                if (!observed) return std::unexpected{observed.error()};
                previous_end = observed->end;
                if (observed->failure_signal) {
                    const auto signal = *observed->failure_signal;
                    execution_status = signal == SIGTERM || signal == SIGINT ? "interrupted" : "failed";
                }
                mixed_blocks.push_back({{"block", b}, {"condition_order", block.at("mixed").at("condition_order")},
                    {"attempted", block.at("mixed").at("attempted")},
                    {"overlap_begin_ns", block.value("overlap_begin_ns", Json(nullptr))},
                    {"overlap_end_ns", block.value("overlap_end_ns", Json(nullptr))},
                    {"roles", std::move(observed->pairs)}});
                return std::move(observed->measured);
            }();
            if (!measured)
                return std::unexpected{measured.error()};
            const bool eligible = *monitoring == Assessment::qualified;
            const bool factual_rejection = precision && *monitoring == Assessment::factual_rejection;
            if (measured->empty() || (!eligible && !factual_rejection)) {
                if (block.contains("precision"))
                    return std::unexpected{invalid("Unqualified block carries a precision decision")};
                continue;
            }
            if (precision) {
                ++completed_precision_blocks;
                std::optional<std::pair<U64, U64>> durations;
                if (eligible) {
                    const auto& baseline = measured->at(0).interval;
                    const auto& candidate = measured->at(1).interval;
                    durations = std::pair{baseline.end - baseline.begin, candidate.end - candidate.begin};
                }
                const auto evaluation = precision->observe(b, repetitions, durations);
                if (!evaluation)
                    return std::unexpected{evaluation.error()};
                if (block.contains("precision") != evaluation->has_value() ||
                    (*evaluation && block.at("precision") != **evaluation))
                    return std::unexpected{invalid("Retained precision decision differs from replay")};
                if (*evaluation)
                    precision_history.push_back({{"block", b}, {"evaluation", **evaluation}});
            }
            Interval cohort;
            for (std::size_t i = 0; i < measured->size(); ++i) {
                const auto& [interval, units] = measured->at(i);
                cohort.include(interval.begin, interval.end);
                if (!eligible) continue;
                const auto seconds = static_cast<double>(interval.end - interval.begin) / 1e9;
                rates[i].push_back(static_cast<double>(repetitions) / seconds);
                if (units)
                    work_metrics[i].push_back(tasks[i]->work_metric == "seconds_per_unit"
                                                  ? seconds / static_cast<double>(units)
                                                  : static_cast<double>(units) / seconds);
            }
            previous_end = std::max(previous_end, cohort.end);
            if (!eligible) continue;
            ++good_blocks;
            dispositions.back()["qualified"] = true;
            dispositions.back()["reason"] = scope == "supplied" ? Json("supplied_clocks_unscored") : Json(nullptr);
            ++input_frequencies[block_input_identity(block, roles)];
            cohort_rates.push_back(static_cast<double>(repetitions) * 1e9 /
                                   static_cast<double>(cohort.end - cohort.begin));
        }
        const auto maximum = exec.at("blocks").get<std::size_t>();
        const bool complete = precision ? precision->attained() ||
            (completed_precision_blocks == maximum && precision->enough_eligible_blocks())
            : good_blocks == maximum;
        if (precision) {
            const bool ready = !calibrating || !doc.at("calibration").at("selected_repetitions").is_null();
            const bool qualified = ready && completed_precision_blocks == blocks.size();
            if ((qualified && !precision->attained() && completed_precision_blocks != maximum) ||
                doc.at("precision_stop_reason") !=
                    package_precision_stop_reason(ready, qualified, precision->attained(), doc))
                return std::unexpected{invalid("Retained precision stop reason differs from replay")};
        }
        std::string conclusion = "not_evaluated";
        Json scaling = Json::array();
        // Fixture clocks prove wiring and verification. Their durations cannot enter a performance
        // ranking.
        if (complete && scope == "native" && (mode == "comparison" || mode == "standalone")) {
            const auto last = roles.size() - 1U;
            const auto workers = [&](std::size_t i) { return roles.at(i).at("cpus").size(); };
            const auto cores = [&](std::size_t i) {
                const auto& list = plan.at("placement").at(i).at("cores");
                return std::set<Json>(list.begin(), list.end()).size();
            };
            Json point{{"experiment_ordinal", 0},
                       {"pair_id", "0"},
                       {"baseline_workers", workers(0)},
                       {"candidate_workers", workers(last)},
                       {"baseline_physical_cores", cores(0)},
                       {"candidate_physical_cores", cores(last)},
                       {"metric",
                        {{"family", "task_throughput"},
                         {"unit", "tasks_per_second"},
                         {"scope", plan.at("scope")}}},
                       {"baseline_median", median(rates[0])},
                       {"candidate_median", median(rates[last])},
                       {"oriented_ratio", nullptr},
                       {"ratio_interval", nullptr}};
            if (mode == "comparison") {
                auto calculated = compare_rates(rates[0], rates[last],
                                                exec.value("order_seed", U64{}), point, conclusion);
                if (!calculated)
                    return std::unexpected{calculated.error()};
            }
            point["conclusion"] = conclusion;
            scaling.push_back(std::move(point));
        }
        const auto validity = complete ? "valid" : "invalid";
        Json summary{{"run_id", run},
                     {"execution", execution_status},
                     {"validity", validity},
                     {"conclusion", conclusion},
                     {"verified_blocks", good_blocks},
                     {"block_dispositions", std::move(dispositions)},
                     {"eligible_blocks", scope == "native" ? good_blocks : 0U},
                     {"metric_scope", plan.at("scope")},
                     {"evidence_source", scope},
                     {"scaling", scaling}};
        // Supplied clocks can be inspected descriptively without contributing any eligible blocks.
        if (complete && (mode == "mixed" || mode == "pipeline"))
            summary["observed_group_rate"] = median(cohort_rates);
        if (complete && (mode == "standalone" || mode == "comparison")) {
            auto& observed = summary["observed_role_rates"] = Json::object();
            for (std::size_t i = 0; i < roles.size(); ++i)
                observed[roles.at(i).at("id").get<std::string>()] = median(rates[i]);
        }
        if (source.contains("case_id")) summary["case_id"] = source.at("case_id");
        if (pinned) summary["controller_placement"] = doc.at("controller_placement");
        if (mixed) {
            auto by_role = Json::array();
            for (std::size_t i = 0; i < roles.size(); ++i) {
                std::vector<double> ratios, slowdowns;
                for (const auto& group : mixed_blocks) {
                    const auto& pair = group.at("roles").at(i);
                    if (pair.at("eligibility") == "eligible") {
                        ratios.push_back(pair.at("together_to_alone_throughput").get<double>());
                        slowdowns.push_back(pair.at("slowdown").get<double>());
                    }
                }
                by_role.push_back({{"role", roles.at(i).at("id")}, {"eligible_pairs", ratios.size()},
                    {"median_together_to_alone_throughput", complete && !ratios.empty()
                        ? Json(median(ratios)) : Json(nullptr)},
                    {"median_slowdown", complete && !slowdowns.empty()
                        ? Json(median(slowdowns)) : Json(nullptr)}});
            }
            const auto status = scope == "supplied" ? "supplied_workers" : complete ? "available" : "incomplete";
            summary["mixed_slowdown"] = {{"status", status}, {"roles", std::move(by_role)}};
        }
        if (calibrating)
            summary["calibration"] = {
                {"status", doc.at("calibration").at("status")},
                {"selected_repetitions", doc.at("calibration").at("selected_repetitions")},
                {"probes", attempts}};
        if (precision)
            summary["precision"] = {{"stop_reason", doc.at("precision_stop_reason")},
                                    {"completed_blocks", completed_precision_blocks},
                                    {"qualified_blocks", good_blocks},
                                    {"evaluations", std::move(precision_history)}};
        if (exec.contains("monitoring"))
            summary["monitoring"] = std::move(monitoring_summary);
        summary["work_metrics"] = Json::array();
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            const auto role_scope = streaming ? "sum-of-stage-callback-time"
                : plan.contains("mapping") ? plan.at("scope").get<std::string>()
                : tasks[i]->collective_finalizer ? "collective-release-to-completion"
                : tasks[i]->collective ? "collective-worker-wall-time" : "role-enclosing-wall-time";
            summary["work_metrics"].push_back(
                {{"role", roles[i].at("id")},
                 {"work_unit", tasks[i]->work_unit},
                 {"metric", tasks[i]->work_metric},
                 {"scope", role_scope},
                 {"median", complete && scope == "native" && !work_metrics[i].empty()
                                ? Json(median(work_metrics[i]))
                                : Json(nullptr)}});
        }
        if (mode == "mixed" || mode == "pipeline")
            summary["whole_group"] = {{"unit", "composite_tasks_per_second"},
                                      {"scope", plan.at("scope")},
                                      {"median", complete && scope == "native"
                                                     ? Json(median(cohort_rates))
                                                     : Json(nullptr)}};
        std::optional<Digest> observed_inputs;
        if (complete && !input_frequencies.empty()) {
            // A fresh confirmation can repeat the same fixed input for more blocks.
            // Keep the relative frequencies when a task actually varies by block.
            U64 divisor{};
            for (const auto& [identity, count] : input_frequencies)
                divisor = std::gcd(divisor, count);
            Json distribution = Json::object();
            for (const auto& [identity, count] : input_frequencies)
                distribution[identity] = count / divisor;
            observed_inputs = core::sha256(distribution.dump());
            summary["observed_inputs_sha256"] = observed_inputs->to_hex();
        }
        const auto selected =
            calibrating && !doc.at("calibration").at("selected_repetitions").is_null()
                ? std::optional<U64>{repetitions}
                : std::nullopt;
        return PackageResult{run,
                             doc.at("scenario_digest"),
                             doc.at("resolved_plan_digest"),
                             plan.at("physical_topology"),
                             plan.at("effective_topology"),
                             execution_status,
                             validity,
                             conclusion,
                             digest,
                             *Digest::parse(doc.at("build_sha256").get<std::string>()),
                             scope == "native" ? good_blocks : 0U,
                             std::move(doc),
                             std::move(summary),
                             std::move(scaling),
                             selected,
                             observed_inputs,
                             std::move(mixed_blocks)};
    } catch (const std::exception& e) {
        return std::unexpected{invalid(std::string("Malformed package Result: ") + e.what())};
    }
}
} // namespace epycbench::results
