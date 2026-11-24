#include "experiment/analysis.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <numeric>
#include <tuple>
#include <vector>

namespace epycbench::experiment {

auto comparison_distributions(std::span<const ComparisonPoint> points) -> nlohmann::json {
    using Digest = core::Sha256Digest;
    using Group = std::tuple<Digest, Digest, Digest>;
    std::map<Group, std::vector<const ComparisonPoint*>> groups;
    for (const auto& point : points)
        groups[{point.problem, point.candidate, point.build}].push_back(&point);

    auto result = nlohmann::json::array();
    for (const auto& [identity, members] : groups) {
        const auto& [problem, candidate, build] = identity;
        auto rows = nlohmann::json::array();
        std::vector<double> ratios;
        ratios.reserve(members.size());
        for (const auto* point : members) {
            rows.push_back({{"trial_ordinal", point->ordinal}, {"oriented_ratio", point->oriented_ratio}});
            ratios.push_back(point->oriented_ratio);
        }
        std::ranges::sort(ratios);
        const auto center = ratios.size() / 2U;
        const auto median = ratios.size() % 2U ? ratios[center] : std::midpoint(ratios[center - 1U], ratios[center]);
        result.push_back({{"problem_sha256", problem.to_hex()}, {"candidate_sha256", candidate.to_hex()},
            {"build_sha256", build.to_hex()}, {"sample_unit", "run"}, {"trials", std::move(rows)},
            {"minimum", ratios.front()}, {"median", median}, {"maximum", ratios.back()}});
    }
    return result;
}

} // namespace epycbench::experiment
