#pragma once

#include "epycbench/scenario/resource_resolution.hpp"
#include "epycbench/scenario/parser.hpp"
#include "epycbench/scenario/structural_validation.hpp"
#include "epycbench/scenario/package_plan.hpp"

#include <set>
#include <utility>

namespace epycbench::scenario {

enum class TaskObjective { throughput, time_per_task };
[[nodiscard]] auto objective_token(TaskObjective) noexcept -> std::string_view;
[[nodiscard]] auto objective_orientation(TaskObjective) noexcept -> std::string_view;

struct ConfigurationCandidate {
    std::size_t ordinal;
    nlohmann::ordered_json authored_scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
    std::optional<core::Sha256Digest> input_identity;
    std::optional<core::Sha256Digest> problem_identity;
    std::optional<core::Sha256Digest> candidate_identity;
    std::optional<PackagePlan> package{};
    [[nodiscard]] bool accepted() const noexcept { return package.has_value(); }
};

class ConfigurationSpace {
  public:
    [[nodiscard]] static auto parse(const ScenarioSource&, std::size_t maximum_candidates)
        -> std::expected<ConfigurationSpace, core::Error>;
    [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
    [[nodiscard]] auto objective() const noexcept -> TaskObjective { return objective_; }
    [[nodiscard]] auto axis_cardinalities() const -> std::vector<std::size_t>;
    [[nodiscard]] auto describe_axes() const -> nlohmann::ordered_json;
    [[nodiscard]] auto varies_task_parameters() const noexcept -> bool;
    [[nodiscard]] auto package_selections() const -> std::set<std::pair<std::string, std::string>>;
    [[nodiscard]] auto materialize(std::size_t ordinal) const
        -> std::expected<nlohmann::ordered_json, core::Error>;
    [[nodiscard]] auto resolve_controller(const ResourceResolutionContext&) const
        -> std::expected<PackageControllerPlacement, core::Error>;
    // Editable settings identified by the rejecting check, kept outside persisted diagnostics.
    [[nodiscard]] auto resolve(std::size_t ordinal, const ResourceResolutionContext&,
                               std::vector<std::string>* related_settings = nullptr) const
        -> std::expected<ConfigurationCandidate, core::Error>;

  private:
    struct Axis {
        nlohmann::ordered_json::json_pointer path;
        nlohmann::ordered_json choices;
        bool task_parameter;
    };
    ConfigurationSpace(TaskObjective objective, nlohmann::ordered_json scenario,
                       std::vector<Axis> axes, std::set<core::LinuxCpuId> occupied_cpus,
                       std::size_t size)
        : objective_{objective}, scenario_(std::move(scenario)), axes_{std::move(axes)},
          occupied_cpus_{std::move(occupied_cpus)}, size_{size} {}
    TaskObjective objective_;
    nlohmann::ordered_json scenario_;
    std::vector<Axis> axes_;
    std::set<core::LinuxCpuId> occupied_cpus_;
    std::size_t size_;
};

} // namespace epycbench::scenario
