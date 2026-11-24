#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include "epycbench/scenario/configuration_space.hpp"

#include <nlohmann/json_fwd.hpp>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace epycbench::experiment {

enum class ResourceObjective { workers, physical_cores };
enum class Feasibility { feasible, infeasible, unavailable };

struct TradeoffPolicy {
    scenario::TaskObjective performance{scenario::TaskObjective::throughput};
    ResourceObjective resource{ResourceObjective::workers};
    std::optional<std::size_t> maximum_workers;
    std::optional<std::size_t> maximum_physical_cores;
    std::optional<double> minimum_tasks_per_second;
    std::optional<double> maximum_seconds_per_task;
};

struct TradeoffMetrics {
    double tasks_per_second;
    double seconds_per_task;
    std::optional<std::size_t> workers;
    std::optional<std::size_t> physical_cores;
};

struct TradeoffSample {
    std::size_t trial_ordinal;
    core::Sha256Digest candidate;
    core::Sha256Digest problem;
    core::Sha256Digest build;
    std::string physical_topology;
    std::string effective_topology;
    double tasks_per_second;
    std::optional<std::size_t> workers;
    std::optional<std::size_t> physical_cores;
};

[[nodiscard]] auto validate_tradeoff_policy(const TradeoffPolicy&)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto tradeoff_feasibility(const TradeoffMetrics&, const TradeoffPolicy&) noexcept
    -> Feasibility;
// Descriptive point dominance, not a statistical conclusion. Equal points tie.
[[nodiscard]] auto tradeoff_dominates(const TradeoffMetrics&, const TradeoffMetrics&,
                                     const TradeoffPolicy&) noexcept -> bool;
[[nodiscard]] auto tradeoff_frontiers(std::span<const TradeoffSample>, const TradeoffPolicy&)
    -> std::expected<nlohmann::json, core::Error>;

} // namespace epycbench::experiment
