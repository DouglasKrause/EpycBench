#include "epycbench/experiment/tradeoffs.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <numeric>
#include <tuple>
#include <vector>

namespace epycbench::experiment {
namespace {
using Json = nlohmann::json;
auto invalid(std::string_view reason) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::results,
        .code = core::StableErrorCode{"experiment.tradeoffs.invalid"},
        .message = "Cannot evaluate retained experiment tradeoffs",
        .operation = "experiment.tradeoffs", .os_error = {},
        .context = {{"reason", reason}}}};
}
auto positive(double value) noexcept -> bool { return std::isfinite(value) && value > 0.0; }
auto valid(const TradeoffMetrics& point) noexcept -> bool {
    return positive(point.tasks_per_second) && positive(point.seconds_per_task) &&
           (!point.workers || *point.workers > 0U) &&
           (!point.physical_cores || *point.physical_cores > 0U) &&
           (!point.workers || !point.physical_cores || *point.physical_cores <= *point.workers);
}
auto resource(const TradeoffMetrics& point, const TradeoffPolicy& policy) noexcept
    -> std::optional<std::size_t> {
    return policy.resource == ResourceObjective::workers ? point.workers : point.physical_cores;
}
auto nullable(const auto& value) -> Json { return value ? Json(*value) : Json(nullptr); }
auto median(std::vector<double> values) -> double {
    std::ranges::sort(values);
    const auto center = values.size() / 2U;
    return values.size() % 2U ? values[center] : std::midpoint(values[center - 1U], values[center]);
}
auto metrics_json(const TradeoffMetrics& point) -> Json {
    return {{"tasks_per_second", point.tasks_per_second},
            {"seconds_per_completed_task", point.seconds_per_task},
            {"tasks_per_second_per_assigned_core", point.physical_cores
                ? Json(point.tasks_per_second / static_cast<double>(*point.physical_cores)) : Json(nullptr)},
            {"workers", nullable(point.workers)}, {"physical_cores", nullable(point.physical_cores)}};
}

auto physical_core_throughput_steps(const Json& candidates, const TradeoffPolicy& policy) -> Json {
    if (policy.resource != ResourceObjective::physical_cores || policy.performance != scenario::TaskObjective::throughput)
        return nullptr;
    std::map<std::size_t, Json> levels;
    for (const auto& candidate : candidates) {
        if (!candidate.at("nondominated").get<bool>()) continue;
        const auto& metrics = candidate.at("metrics");
        const auto cores = metrics.at("physical_cores").get<std::size_t>();
        auto& point = levels[cores];
        // Feasible frontier candidates with the same cost also have equal throughput.
        if (point.is_null()) point = {{"physical_cores", cores}, {"tasks_per_second", metrics.at("tasks_per_second")},
                                      {"candidates", Json::array()}};
        point.at("candidates").push_back(candidate.at("candidate_sha256"));
    }
    auto steps = Json::array();
    if (levels.empty()) return steps;
    for (auto before = levels.begin(), after = std::next(before); after != levels.end(); ++before, ++after) {
        const auto added = after->first - before->first;
        const auto difference = after->second.at("tasks_per_second").get<double>() -
                                before->second.at("tasks_per_second").get<double>();
        steps.push_back({{"from", before->second}, {"to", after->second}, {"added_physical_cores", added},
            {"difference_tasks_per_second", difference},
            {"difference_tasks_per_second_per_added_core", difference / static_cast<double>(added)}});
    }
    return steps;
}
} // namespace

auto validate_tradeoff_policy(const TradeoffPolicy& policy) -> std::expected<void, core::Error> {
    if ((policy.maximum_workers && !*policy.maximum_workers) ||
        (policy.maximum_physical_cores && !*policy.maximum_physical_cores) ||
        (policy.minimum_tasks_per_second && !positive(*policy.minimum_tasks_per_second)) ||
        (policy.maximum_seconds_per_task && !positive(*policy.maximum_seconds_per_task)))
        return invalid("nonpositive_or_nonfinite_constraint");
    return {};
}

auto tradeoff_feasibility(const TradeoffMetrics& point, const TradeoffPolicy& policy) noexcept
    -> Feasibility {
    if (!valid(point)) return Feasibility::unavailable;
    if ((policy.maximum_workers && point.workers && *point.workers > *policy.maximum_workers) ||
        (policy.maximum_physical_cores && point.physical_cores &&
            *point.physical_cores > *policy.maximum_physical_cores) ||
        (policy.minimum_tasks_per_second && point.tasks_per_second < *policy.minimum_tasks_per_second) ||
        (policy.maximum_seconds_per_task && point.seconds_per_task > *policy.maximum_seconds_per_task))
        return Feasibility::infeasible;
    if (!resource(point, policy) || (policy.maximum_workers && !point.workers) ||
        (policy.maximum_physical_cores && !point.physical_cores))
        return Feasibility::unavailable;
    return Feasibility::feasible;
}

auto tradeoff_dominates(const TradeoffMetrics& first, const TradeoffMetrics& second,
                       const TradeoffPolicy& policy) noexcept -> bool {
    if (tradeoff_feasibility(first, policy) != Feasibility::feasible ||
        tradeoff_feasibility(second, policy) != Feasibility::feasible)
        return false;
    const auto first_resource = *resource(first, policy), second_resource = *resource(second, policy);
    const bool throughput = policy.performance == scenario::TaskObjective::throughput;
    const auto a = throughput ? first.tasks_per_second : first.seconds_per_task;
    const auto b = throughput ? second.tasks_per_second : second.seconds_per_task;
    const bool no_worse = throughput ? a >= b : a <= b;
    const bool better = throughput ? a > b : a < b;
    return no_worse && first_resource <= second_resource && (better || first_resource < second_resource);
}

auto tradeoff_frontiers(std::span<const TradeoffSample> samples, const TradeoffPolicy& policy)
    -> std::expected<Json, core::Error> {
    if (auto checked = validate_tradeoff_policy(policy); !checked) return std::unexpected{checked.error()};
    if (samples.size() > 4096U) return invalid("sample_bound");
    using Digest = core::Sha256Digest;
    using Scope = std::tuple<Digest, Digest, std::string, std::string>;
    using Members = std::map<Digest, std::vector<const TradeoffSample*>>;
    std::map<Scope, Members> groups;
    for (const auto& sample : samples) {
        if (sample.physical_topology.empty() || sample.effective_topology.empty() ||
            !valid({sample.tasks_per_second, 1.0 / sample.tasks_per_second,
                    sample.workers, sample.physical_cores}))
            return invalid("invalid_sample");
        groups[{sample.problem, sample.build, sample.physical_topology, sample.effective_topology}]
            [sample.candidate].push_back(&sample);
    }

    auto result = Json::array();
    for (const auto& [scope, candidates] : groups) {
        const auto& [problem, build, physical, effective] = scope;
        auto rows = Json::array();
        std::vector<TradeoffMetrics> values;
        values.reserve(candidates.size());
        for (const auto& [candidate, members] : candidates) {
            std::vector<double> throughput, time;
            auto workers = members.front()->workers, cores = members.front()->physical_cores;
            auto runs = Json::array();
            for (const auto* member : members) {
                throughput.push_back(member->tasks_per_second);
                time.push_back(1.0 / member->tasks_per_second);
                if (workers != member->workers) workers.reset();
                if (cores != member->physical_cores) cores.reset();
                auto sample = metrics_json({member->tasks_per_second, 1.0 / member->tasks_per_second,
                                            member->workers, member->physical_cores});
                sample["trial_ordinal"] = member->trial_ordinal;
                runs.push_back(std::move(sample));
            }
            values.push_back({median(std::move(throughput)), median(std::move(time)), workers, cores});
            const auto& point = values.back();
            const auto state = tradeoff_feasibility(point, policy);
            auto reasons = Json::array();
            if (policy.maximum_workers && point.workers && *point.workers > *policy.maximum_workers)
                reasons.push_back("maximum_workers_exceeded");
            if (policy.maximum_physical_cores && point.physical_cores &&
                *point.physical_cores > *policy.maximum_physical_cores)
                reasons.push_back("maximum_physical_cores_exceeded");
            if (policy.minimum_tasks_per_second && point.tasks_per_second < *policy.minimum_tasks_per_second)
                reasons.push_back("minimum_tasks_per_second_not_met");
            if (policy.maximum_seconds_per_task && point.seconds_per_task > *policy.maximum_seconds_per_task)
                reasons.push_back("maximum_seconds_per_task_exceeded");
            if ((!point.workers && (policy.resource == ResourceObjective::workers || policy.maximum_workers)) ||
                (!point.physical_cores && (policy.resource == ResourceObjective::physical_cores ||
                                         policy.maximum_physical_cores)))
                reasons.push_back("required_resource_count_unavailable_or_variable");
            rows.push_back({{"candidate_sha256", candidate.to_hex()}, {"run_samples", std::move(runs)},
                {"metrics", metrics_json(point)},
                {"feasibility", state == Feasibility::feasible ? "feasible" :
                                state == Feasibility::infeasible ? "infeasible" : "unavailable"},
                {"reasons", std::move(reasons)}, {"nondominated", false}});
        }
        auto frontier = Json::array();
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (tradeoff_feasibility(values[i], policy) != Feasibility::feasible) continue;
            bool dominated{};
            for (std::size_t j = 0; j < values.size() && !dominated; ++j)
                if (i != j) dominated = tradeoff_dominates(values[j], values[i], policy);
            rows.at(i)["nondominated"] = !dominated;
            if (!dominated) frontier.push_back(rows.at(i).at("candidate_sha256"));
        }
        result.push_back({{"problem_sha256", problem.to_hex()}, {"build_sha256", build.to_hex()},
            {"physical_topology_fingerprint", physical}, {"effective_topology_fingerprint", effective},
            {"physical_core_throughput_steps", physical_core_throughput_steps(rows, policy)}, {"candidates", std::move(rows)},
            {"nondominated_candidates", std::move(frontier)}});
    }
    return result;
}

} // namespace epycbench::experiment
